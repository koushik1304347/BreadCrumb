# Rule-based agent for x86 xv6

This course experiment is the first step toward **Externalization barriers for
speculative processes**. It uses this repository's x86 xv6, Python 3's standard
library, and QEMU. There is no LLM, API key, Python package installation, or kernel
modification.

For the optional Gemini API planner, follow [LLM_SETUP.md](LLM_SETUP.md).
The rule-based commands below remain available without a key or network access.

From the `xv6-public` directory:

```sh
make
python3 agent/agent.py list
python3 agent/agent.py read report.txt
python3 agent/agent.py summarize report.txt
python3 agent/agent.py
```

Interactive mode supports `list`, `read FILE`, `summarize FILE`, and `quit`.
Summarization extracts the first sentence; it does not interpret instructions.
Each invocation boots a new guest with fixtures under `/demo`. Files are guest
files, not files shared with the host. The dummy repository is a directory of
text files; Git does not run inside xv6.

The launcher finds `../build/qemu-system-i386`, then an installed QEMU on PATH.
Override this when moving the project to another machine:

```sh
python3 agent/agent.py --qemu /path/to/qemu-system-i386 list
```

Build images before launching; the agent never rebuilds them automatically. Do
not build or modify source images while the agent is copying them. Each run
copies both `fs.img` and `xv6.img` to a temporary directory, launches QEMU with
one CPU, no display, no monitor, and serial stdio, then removes the copies on
exit. Guest changes do not persist to your source images.

## Demonstrations

These explicitly selected scenarios authorize effects only in the disposable
guest. Interactive requests expose read-only tools.

```sh
python3 agent/agent.py --log agent/logs/baseline.jsonl demo baseline
python3 agent/agent.py --log agent/logs/injection.jsonl demo injection
```

`demo baseline` reads the original important file, creates and reads a tentative
draft, deletes the important file, and prints `APPROVED`. It then abandons the
attempt and verifies that the deleted file is still missing. File operations
and console output have already escaped the attempted work. This is a baseline
failure demonstration, not an implementation of speculative execution or abort.

`demo injection` reads an inbox containing a malicious instruction. An explicitly
scripted vulnerable planner proposes deletion after that read. The dispatcher
allows the valid demo action and xv6 executes it. This demonstrates a semantic
mistake that schema validation alone cannot prevent. Normal read/summarize never
follows file instructions. No real model or general prompt-injection detection
is being simulated.

The JSONL log records proposed/validated actions, serial chunks (including boot
and kernel output), fixed guest commands, observations, and verification results.
The default `agent/logs/latest.jsonl` is overwritten each invocation; use `--log`
with distinct filenames to preserve evidence. Empty output alone is never proof
that a mutation succeeded: the demos inspect the resulting file state.

Run the validator boundary tests with:

```sh
python3 -m unittest discover -s agent -v
```

Both demo commands above are also end-to-end checks against the actual guest;
they exit with an error if their expected file state is not observed.

## Bridge and limitations

The pipeline is request -> deterministic planner -> structured action validator
-> fixed guest command -> xv6 utility -> system call -> observation. Tools allow
only a single filename under `/demo`, maximum 14 characters, and reject shell
syntax. The bridge does not run a host shell. It paces serial input because the
teaching UART can lose bytes when flooded. Commands use a random completion
marker followed by the shell prompt, with a timeout and a 1 MiB response limit.
This bridge assumes the supplied fixtures contain ordinary text; arbitrary guest
programs could spoof framing. It is a teaching harness, not a hostile-guest RPC
protocol. xv6 shell utilities do not expose reliable exit codes here, so their
diagnostics are returned directly. A timeout stops the session rather than
retrying an operation whose outcome may be uncertain.

This tree's `>` does not truncate existing files and `>>` does not implement
normal append semantics. Demos create the draft on a fresh image. Fixture setup
uses fixed text and paths; the fixtures are created afresh each boot.

Later kernel tests need a guest utility that calls `xbegin()` and forks children
within the same speculation, then commits or aborts. Separate shell commands
cannot establish that shared process-tree context. Add outside-process readers,
console buffering tests, pipes inside/outside the speculation, conflicts, crash
tests, and shadow-capacity tests as the kernel mechanism becomes available.
Python validation is not the kernel externalization barrier.

## Version control

Keep the xv6 sources, license, `agent/`, documentation, and selected small test
fixtures in your own xv6 Git repository. QEMU is an external dependency; document
its version/path rather than checking in its full source tree. Generated images,
binaries, and routine logs should remain ignored. Preserve selected raw evidence
separately when preparing the project submission. This experiment is not intended
for upstream QEMU contribution.
