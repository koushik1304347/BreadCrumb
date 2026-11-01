# Step 3 verification — 1 November 2026

Implementation branch: `shadow-block-infrastructure`. Results below were run
locally against built x86 xv6 images, using the existing QEMU i386 binary.
The runner copied the images before each boot, preserving the source images.

| Configuration / check | Result | Evidence |
|---|---|---|
| Normal kernel and filesystem build | PASS | `make -B xv6.img fs.img` |
| Test kernel and filesystem build | PASS | `make -B SHADOW_TEST=1 xv6.img fs.img` |
| Shadow suite, 1 CPU, followed by lifecycle suite | PASS | `evidence/step3/shadow-cpu1.txt` |
| Shadow suite, 3 CPUs, five consecutive runs, followed by lifecycle suite | PASS | `evidence/step3/shadow-cpu3-repeat5.txt` |
| Normal kernel, 3 CPUs: spectest, addtest, hello_test, mytest | PASS | `evidence/step3/normal-lifecycle-cpu3.txt` |
| Normal kernel, 1 CPU: full usertests | ALL TESTS PASSED | `evidence/step3/usertests-cpu1.txt` |
| Existing Python agent tests | 15 tests passed | `python3 -m unittest discover -s agent -v` |
| Normal kernel excludes test-only syscall/handler | PASS | `nm kernel`: no `sys_shadowtest` or `shadow_test` symbol |
| Source whitespace check | PASS | `git diff --check -- . ':!design/speculation-decisions.csv'` |

The CSV already had added CRLF-terminated decision rows before implementation;
Git flags those carriage returns as trailing whitespace. Its content and the
spreadsheet were not changed during Step 3. Existing local proc.c comment and
sysproc.c hello-message edits were retained.

The expected `usertests` uio test deliberately triggers a user protection fault;
the log continues through `uio test done` and `ALL TESTS PASSED`. `zombie!` lines
are printed by the existing init process while reaping adopted descendants.
They are not a claim of speculative console buffering, which remains unimplemented.

## Failures found and corrected during development

1. Baseline recompilation failed because spinlock.h could be included twice after
   proc.h started including it. Added an include guard.
2. The first instrumented build linked without the conditional user assembly
   stub: Makefile passed SHADOW_TEST to C compilation but not assembly. Added it
   to ASFLAGS as well, then rebuilt and passed all instrumented tests.

These are development records, not a substitute for the handout's required
future GDB session showing a real runtime bug.

## Coverage boundaries

The tests call the shadow API through a compile-time test-only interface and
verify no original block modifications. Ordinary filesystem reads/writes still
bypass shadows. No durable shadow commit, cached-inode isolation, outside-writer
conflict handling, multi-block syscall rollback, console/pipe buffering, or crash
atomicity of speculation has been implemented or claimed. Capacity can be raised
in the linked storage design, but real larger commits still require coordinated
log/cache/header changes.

The final source images were rebuilt in **normal mode**, without SHADOW_TEST.
Use the forced test build in step3-implementation.md to run shadowtest again.
