#!/usr/bin/env python3
"""Run guest tests on disposable x86 images and retain the raw serial log."""
import argparse
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, choices=(1, 2, 3, 4), default=1)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--qemu", default=str(ROOT.parent / "build/qemu-system-i386"))
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--suite", choices=("shadow", "lifecycle", "usertests"), default="shadow")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="xv6-spec-tests-") as tmp, args.log.open("wb") as log:
        folder = Path(tmp)
        for name in ("xv6.img", "fs.img"):
            shutil.copyfile(ROOT / name, folder / name)
        cmd = [args.qemu, "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-smp", str(args.cpus), "-m", "512",
               "-drive", f"file={folder/'xv6.img'},index=0,format=raw",
               "-drive", f"file={folder/'fs.img'},index=1,format=raw"]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        pending = b""

        def until(pattern, timeout=180):
            nonlocal pending
            deadline = time.monotonic() + timeout
            while True:
                match = re.search(pattern, pending)
                if match:
                    out, pending = pending[:match.start()], pending[match.end():]
                    return out.decode(errors="replace")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(f"Guest timeout; inspect {args.log}")
                if select.select([proc.stdout], [], [], remaining)[0]:
                    data = os.read(proc.stdout.fileno(), 4096)
                    if not data:
                        raise RuntimeError(f"QEMU stopped; inspect {args.log}")
                    log.write(data)
                    log.flush()
                    pending += data
                    if len(pending) > 4 * 1024 * 1024:
                        raise RuntimeError("Guest output exceeded 4 MiB")

        def run(command, expected):
            marker = "TESTDONE" + uuid.uuid4().hex[:8]
            for byte in f"{command}; echo {marker}\n".encode():
                proc.stdin.write(bytes([byte]))
                proc.stdin.flush()
                time.sleep(0.003)
            output = until(rb"(?:^|\n)" + marker.encode() + rb"\r?\n")
            until(rb"\$ ")
            print(output, flush=True)
            if expected not in output or "FAIL:" in output or "panic:" in output:
                raise RuntimeError(f"{command} failed; inspect {args.log}")

        try:
            until(rb"\$ ")
            if args.suite == "shadow":
                for _ in range(args.repeat):
                    run("shadowtest", "SHADOW TESTS PASSED")
                run("spectest", "SPEC TESTS PASSED")
            elif args.suite == "lifecycle":
                run("spectest", "SPEC TESTS PASSED")
                run("addtest", "add(3, 4) = 7")
                run("hello_test", "hello() returned 0")
                run("mytest", "Hey, I am trying xv6")
            else:
                run("usertests", "ALL TESTS PASSED")
            print(f"PASS: {args.suite}, {args.cpus} CPU(s); raw log: {args.log}")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stdin.close()
            proc.stdout.close()


if __name__ == "__main__":
    main()
