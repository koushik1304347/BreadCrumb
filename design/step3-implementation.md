# Step 3: dynamically allocated shadow-block infrastructure

This stage provides private **kernel block storage**, not filesystem isolation.
Ordinary `open/read/write/unlink`, the filesystem cache, console, and pipes still
operate normally. Step 4 must explicitly integrate the private view. Never
interpret lifecycle-only success as a real durable commit of private blocks.

## Storage and API

`shadow.c` implements a linked list of shadow entries attached to each existing
`spec_context.resources` pointer. Each entry identifies `(device, block number)`
and holds 512 private bytes. `NSHADOW` in `param.h` limits each context to eight
linked entries, including LOADING reservations. There is no fixed eight-pointer
array to replace when raising the cap. `NSPEC` separately limits live contexts.

Each entry gets one dynamically allocated 4096-byte `kalloc()` page, containing
both bookkeeping and data. Eight entries therefore occupy 32 KiB of allocated
pages, of which 4 KiB contains block data. This intentionally simple allocation
has internal fragmentation. Candidate pages can temporarily exist per in-flight
caller before insertion, so 32 KiB is the attached-page limit, not a strict peak
memory bound. A slab/packed-page allocator is a possible later optimization.
The compile-time size check prevents silently overflowing a page.

The entry states are implicit FREE (unlinked), LOADING, and READY. Allocate a
candidate page without the resource spinlock, then recheck the list and cap.
Only one candidate is attached as LOADING for a particular original block;
others are freed or reused. The reservation is made **before reading the source
block**. All readers/writers encountering it wait, then retry. No partially
initialized bytes are exposed. A linked entry is never evicted during the attempt.

Kernel API (`defs.h`):

| Function | Contract |
|---|---|
| `shadow_read(id, dev, block, offset, dst, length)` | Copy private bytes to a kernel buffer. Return 1 if found, 0 if absent, -1 on invalid input/cancellation. Does not allocate a read snapshot. |
| `shadow_write(id, dev, block, offset, src, length)` | Copy kernel bytes into the private view, first initializing from the original cache block if necessary. Return 1 on success, -1 on failure. |
| `shadow_count(context)` | Count linked entries, including LOADING. Caller owns a pin or exclusive lifecycle reference. |
| `shadow_discard(context)` | Detach and free all entries. Only exclusive completion/final reclamation may call this; no other resource user may remain. |

No unprotected entry/data pointers escape the module. READY copies of at most
one block are serialized by `resource_lock`. Bounds require a nonempty slice
within BSIZE, a non-null kernel buffer, ROOTDEV, and a block below this tree's
FSSIZE. This is not a general multi-device interface or a user-pointer API.

First-copy currently uses ordinary `bread()` because no filesystem interception
exists yet. It never calls `bwrite()` or `log_write()`. Step 4 must preserve an
explicit original-cache path to prevent recursive shadow lookup. Callers must
not hold a cache-buffer lock or any spinlock when entering shadow access.

## Locking, cancellation, and cleanup

`ptable.lock` protects context membership/lifecycle; each context's
`resource_lock` protects the shadow list and entry state. Pins keep contexts and
entries from being reclaimed, but do not grant perpetual permission after
cancellation. New normal pins reject cancelled contexts. Existing operations
can finish private, bounded work while cancellation races; they never publish
real writes. Owner cancellation marks all members killed. Initialization
rechecks admission after disk work and removes its reservation on cancellation.

Normal resource work does not acquire ptable.lock while holding resource_lock.
There is one deliberate **sleep-handoff exception**: `spec_wait()` follows xv6's
sleep protocol, acquiring ptable.lock before releasing resource_lock. It checks
context state/killed status and enters sleep atomically. On return it releases
ptable.lock before reacquiring resource_lock. This prevents both missed normal
wakeups and kill-before-sleep races. No ptable-lock holder takes resource_lock;
there is no reverse lock order. Disk I/O and allocation run outside these locks.
Normal READY/failed initialization wakes waiters after releasing resource_lock;
cancellation wakes killed sleepers through the existing process-table path.

Final detachment or unpin retains an exclusive reclaimer reference at zero
members. New cleanup pins also reject zero-member contexts. Reclamation drops
ptable.lock, discards shadow pages, then reacquires the lock to free the context.
An exiting process detaches after descriptor/cwd cleanup but before zombie
publication. Reaping is not required for membership release. Existing user edits
to the proc.c comment and hello() message were preserved.

Explicit completion reserves COMMITTING/ABORTING with an extra reference, then
does resource inspection/discard outside ptable.lock. **xcommit returns -1 when
shadows exist**, restoring the attempt for a later xabort (unless concurrently
cancelled). Publishing those blocks belongs to Step 5. Empty lifecycle commits
and explicit aborts remain supported. The default syscall ABI remains 24-26.

## Tests and repeatable commands

The test-only syscall is number 27 only in `SHADOW_TEST=1` builds. It accepts
bounded operations on fixed test block slots, not raw user pointers or arbitrary
disk addresses. Test controls inject allocation failure, hold a loader after
reservation, and count pages/waiters. There is no test syscall/guest test program
in the normal build. Always force a rebuild when changing this compile flag:

```sh
make -B SHADOW_TEST=1 xv6.img fs.img
python3 tests/run-spec-tests.py --cpus 1 --log /tmp/shadow-cpu1.txt
python3 tests/run-spec-tests.py --cpus 3 --repeat 5 --log /tmp/shadow-cpu3.txt

# Restore a normal build (without the test-only syscall):
make -B xv6.img fs.img
python3 tests/run-spec-tests.py --suite lifecycle --cpus 3 --log /tmp/lifecycle.txt
python3 tests/run-spec-tests.py --suite usertests --cpus 1 --log /tmp/usertests.txt
```

The Python harness boots temporary copies of both images; source images are not
mutated by guest execution. Set `--qemu /path/to/qemu-system-i386` if necessary.
The shadow suite also runs spectest. A guest command must print its success
marker; failures/timeouts exit nonzero and preserve the raw serial log.

Coverage includes first-copy preservation, private read/write, unchanged source
bytes, absent versus invalid lookup, reuse at capacity, ninth-block rejection,
allocation-failure recovery, shared-context access, separate-context isolation,
nonempty commit gating, retained child changes, LOADING reader/writer waits,
killed waiters, owner exit/kill with an in-flight loader and waiter, and 100
allocation/abort cycles with page-leak accounting. Existing lifecycle tests
continue to verify their Step-2-only ordinary file-effects limitation.

`spinlock.h` needed an include guard: baseline recompilation exposed duplicate
definitions after proc.h began including it. Test-build assembly also needs the
SHADOW_TEST preprocessor flag, or the conditional stub is absent at link time.
These issues were fixed; neither is a shadow isolation/security claim.

Raw verification logs are retained under `design/evidence/step3/`. Full results
and remaining integration limits are recorded in `step3-verification.md`.

## Later work

Step 4: route real filesystem access through private copies, including correct
cached inode views. Step 5: reserve/commit all blocks atomically and handle
cache visibility/durability; resolve cancellation during publication. Step 6:
extend isolation to allocation, sizes, directories, create/unlink and indirect
blocks. No multi-block syscall rollback/preflight or outside-writer conflict
detection is implemented here. Disk I/O errors still follow xv6's existing panic
behavior. Output buffers are separate later resources, and their integration
will require replacing/extending the current shadow-list resource ownership.
Raise NSHADOW only with compatible log/header/cache limits before enabling real
commits. The eight-block cap counts metadata as well as file data once integrated.
