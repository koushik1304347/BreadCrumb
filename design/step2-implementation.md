# Step 2: system calls and speculation lifecycle

This is a lifecycle-only implementation for the x86 `xv6-public` tree. It does
not provide speculative filesystem isolation. File creation, writes, unlink,
console output, and pipe traffic still have their ordinary xv6 effects.
`spectest` deliberately demonstrates that a file written during a speculation
remains visible after `xabort()` at this stage. No rollback or durability claim
is made.

## Interface and policy

`xbegin()`, `xcommit()`, and `xabort()` return 0 on success and -1 on failure.
`proc.spec == 0` means ordinary execution; a positive value is the ID of a
shared attempt. `SYS_add` remains syscall 22 and the existing `hello` syscall
remains 23; the new calls use 24, 25, and 26 respectively.

The owner is the process that called `xbegin()`. A child inherits the same ID,
including across further descendants and `exec()`. Nested begin, non-owner
completion, completion outside a speculation, and owner completion while any
other member or pinned reference remains all fail without changing the active
attempt. Explicit completion neither waits for nor kills peers.

The shared table has `NSPEC` (8) slots. IDs are positive monotonically assigned
integers; wrap skips zero and scans past identities still in the table. A slot
and its ID remain occupied until both its living-member count and reference
count reach zero, preventing reuse while a participant or pinned kernel user
can still refer to it.

## Context, references, and locking

The table and lifecycle helpers live in `proc.c`; the context shape is declared
in `proc.h`. Each context records its ID, owner PID, state, living members,
reference count, a resource lock, and a reserved resource pointer. At present,
references consist of one per process membership plus temporary pins. There
are no shadow resources yet.

`ptable.lock` protects process membership and all context lifecycle metadata.
It is the only lock used by this stage for membership transitions. This avoids
a nested process-table/context-metadata lock order. `resource_lock` is reserved
for Step 3 resource data. Code must never sleep while holding either spinlock.
Use `wakeup1()` while already holding `ptable.lock`; do not call `wakeup()` from
that critical section.

Step 3 can call `spec_pin(id)` to acquire an active context reference. The
returned pointer is valid only until the matching `spec_unpin(pointer)`, which
must run after releasing `resource_lock`. Pinning increments the context
reference while `ptable.lock` is held, then returns with that lock released.
Resource work can then take `resource_lock` without holding `ptable.lock`.
This establishes the rule that the two locks are never nested. Cancellation
rejects new pins but existing pinned work keeps the context alive while it
finishes. Exit and abort discard can use `spec_pin_cleanup(id)` to pin an
`ACTIVE` or `ABORTING` context; that API is reserved for cleanup and must not
admit new speculative work. Exit/abort cleanup should attach to the distinct
internal state-transition paths and clear the resource pointer before the
final reference permits slot reuse.

The current `spec_commit_context_locked()` and
`spec_abort_context_locked()` functions only change state while holding
`ptable.lock`; they do no resource work. Do not add disk I/O or other sleeping
operations to them. Before Step 5 adds commit/discard work that can sleep, split
completion into a synchronized state reservation, a pinned resource operation
outside `ptable.lock`, and a synchronized final detach. Cancellation racing
with an in-progress commit remains an open failure-semantics decision in the
design record.

## State and process lifecycle

The explicit paths are `ACTIVE -> COMMITTING -> freed` and
`ACTIVE -> ABORTING -> freed`. In this stage the commit/abort paths only change
lifecycle state and detach the sole owner; later resource publication or
discard belongs in their separate helpers before detachment. Owner death uses
`ABORTING`, never commit.

`allocproc()` initializes `spec` to zero. `fork()` copies ordinary process
state, then under `ptable.lock` validates that the parent's context is still
active, increments member/reference counts, sets the child's ID, and makes the
child runnable in one critical section. A cancelled context cannot admit new
children. Failed allocation/copy/admission releases any child resources before
making its process slot reusable and does not increment membership.

`exec()` leaves `proc.spec` unchanged. On exit, file descriptors and cwd are
cleaned up while membership remains attached. A non-owner then detaches; its
ordinary exit does not cancel the owner's context. Detachment occurs before
the process becomes a zombie, so an unreaped zombie no longer counts.

Owner cancellation begins in `kill()` under `ptable.lock` when the target is
the owner. It changes the context to `ABORTING`, marks all current members
killed, and makes sleeping members runnable with `wakeup1()` semantics. A
voluntary owner exit or a trap/fault-driven exit begins cancellation at the top
of `exit()`, before file cleanup. Trap-time faults set `killed`; `exit()` then
performs the synchronized transition. A concurrent fork either registers
before that transition and is included in cancellation, or sees `ABORTING` and
is rejected. As in xv6 generally, kill is cooperative: a process already in a
kernel operation runs until a safe return/exit point. Membership remains valid
through its cleanup, and the context is reclaimed only after members and pins
are gone.

## Verification

`spectest` uses pipes and observable lifecycle results rather than a new
production syscall to expose numeric IDs. Nested-begin rejection in an
inherited process, owner refusal while descendants live, and eventual success
after detachment demonstrate shared membership. It covers owner/non-owner
permissions, exec inheritance, unreaped zombies, a living grandchild, ordinary
member exit and kill, voluntary and killed owner cancellation with blocked
members, all eight contexts plus reuse, failed fork under process/resource
pressure, repeated begin/complete with process-slot reuse, and the ordinary
file-effects limitation.

Commands used in this checkout:

```sh
make
make fs.img
make qemu-nox QEMU=../build/qemu-system-x86_64 CPUS=1
# At the xv6 prompt: spectest; addtest; hello_test; mytest; usertests
make qemu-nox QEMU=../build/qemu-system-x86_64 CPUS=2
# At the xv6 prompt: spectest
```

Results: the kernel and user programs built successfully; `spectest` printed
`SPEC TESTS PASSED` with both one and two CPUs. `addtest`, `hello_test`, and
`mytest` ran successfully, preserving `add()`. The existing `usertests` suite
completed with `ALL TESTS PASSED` on one CPU.

Known limits: context capacity is eight; IDs are not exposed to user programs;
there is no filesystem isolation, rollback, console/pipe output buffering,
conflict handling, or crash-atomic commit. The failed-fork test exercises
resource/process pressure and verifies no leaked participant reference; it does
not claim to distinguish every possible allocator failure cause.
