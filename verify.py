"""Build and boot every module's kernel, and check its stub over the wire.

For each NN-module/:
  1. its sources must match what src/ generates for that stage (no drift);
  2. its kernel is built with zig cc;
  3. QEMU boots it with COM1 on a TCP socket - QEMU's own gdbstub is NOT
     enabled, so every byte of debugging goes through the kernel's stub;
  4. NN-module/check.py talks to it and asserts what the README claims.

    python verify.py          # all modules
    python verify.py 05 07    # just these
    python verify.py -v 09    # also show what each check printed
"""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import build

ROOT = os.path.dirname(os.path.abspath(__file__))


def find_qemu() -> str:
    found = shutil.which("qemu-system-i386")
    if found:
        return found
    win = r"C:\Program Files\qemu\qemu-system-i386.exe"
    if os.path.isfile(win):
        return win
    sys.exit("qemu-system-i386 not found - install QEMU first")


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port: int = s.getsockname()[1]
    s.close()
    return port


def run_module(qemu: str, module: str) -> tuple[bool, str]:
    stale = build.drift(module)
    if stale:
        return False, f"sources out of date with src/: {', '.join(stale)} (run: python build.py stage)"
    with tempfile.TemporaryDirectory() as tmp:
        elf = os.path.join(tmp, "kernel.elf")
        try:
            build.build(module, elf)
        except subprocess.CalledProcessError as e:
            return False, f"build failed: {e}"
        port = free_port()
        vm = subprocess.Popen(
            [qemu, "-kernel", elf, "-display", "none", "-no-reboot", "-m", "64",
             "-serial", f"tcp:127.0.0.1:{port},server=on,wait=on"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        try:
            proc = subprocess.run(
                [sys.executable, os.path.join(ROOT, module, "check.py"),
                 "--port", str(port), "--elf", elf],
                capture_output=True, text=True, timeout=180, cwd=ROOT,
                env={**os.environ, "PYTHONPATH": ROOT},
            )
            return proc.returncode == 0, proc.stdout + proc.stderr
        except subprocess.TimeoutExpired:
            return False, "check timed out after 180s"
        finally:
            vm.kill()
            vm.wait()


def main() -> int:
    qemu = find_qemu()
    verbose = "-v" in sys.argv
    wanted = [a for a in sys.argv[1:] if a != "-v"]
    mods = [m for m in build.modules()
            if os.path.isfile(os.path.join(ROOT, m, "check.py"))
            and (not wanted or any(m.startswith(w) for w in wanted))]
    if not mods:
        print("no modules matched")
        return 1
    failed = []
    for m in mods:
        start = time.monotonic()
        ok, output = run_module(qemu, m)
        print(f"{'PASS' if ok else 'FAIL'}  {m}  ({time.monotonic() - start:.1f}s)")
        if not ok:
            failed.append(m)
        if not ok or verbose:
            print("    " + output.strip().replace("\n", "\n    "))
    print(f"\n{len(mods) - len(failed)}/{len(mods)} modules verified: "
          "real kernel, real QEMU, the kernel's own stub")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
