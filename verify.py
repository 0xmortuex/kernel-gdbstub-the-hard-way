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


def find_qemu(arch: str) -> str:
    found = shutil.which(f"qemu-system-{arch}")
    if found:
        return found
    win = rf"C:\Program Files\qemu\qemu-system-{arch}.exe"
    if os.path.isfile(win):
        return win
    sys.exit(f"qemu-system-{arch} not found - install QEMU first")


# Module 11 needs more than one CPU to have anything to show. It runs once
# per CPU count; its check reads the count from $QEMU_SMP.
SMP = {11: (2, 4)}


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port: int = s.getsockname()[1]
    s.close()
    return port


def run_module(module: str, smp: int = 1) -> tuple[bool, str]:
    stale = build.drift(module)
    if stale:
        return False, f"sources out of date with src/: {', '.join(stale)} (run: python build.py stage)"
    with tempfile.TemporaryDirectory() as tmp:
        elf = os.path.join(tmp, "kernel.elf")
        try:
            image = build.build(module, elf)
        except subprocess.CalledProcessError as e:
            return False, f"build failed: {e}"
        port = free_port()
        qemu = find_qemu("x86_64" if build.is_64bit(module) else "i386")
        vm = subprocess.Popen(
            [qemu, "-kernel", image, "-display", "none", "-no-reboot", "-m", "64",
             "-smp", str(smp), "-serial", f"tcp:127.0.0.1:{port},server=on,wait=on"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        try:
            proc = subprocess.run(
                [sys.executable, os.path.join(ROOT, module, "check.py"),
                 "--port", str(port), "--elf", elf],
                capture_output=True, text=True, timeout=180, cwd=ROOT,
                env={**os.environ, "PYTHONPATH": ROOT, "QEMU_SMP": str(smp)},
            )
            return proc.returncode == 0, proc.stdout + proc.stderr
        except subprocess.TimeoutExpired:
            return False, "check timed out after 180s"
        finally:
            vm.kill()
            vm.wait()


def main() -> int:
    verbose = "-v" in sys.argv
    wanted = [a for a in sys.argv[1:] if a != "-v"]
    mods = [m for m in build.modules()
            if os.path.isfile(os.path.join(ROOT, m, "check.py"))
            and (not wanted or any(m.startswith(w) for w in wanted))]
    if not mods:
        print("no modules matched")
        return 1
    failed = []
    runs = 0
    for m in mods:
        for smp in SMP.get(int(m[:2]), (1,)):
            runs += 1
            label = m if smp == 1 else f"{m} (-smp {smp})"
            start = time.monotonic()
            ok, output = run_module(m, smp)
            print(f"{'PASS' if ok else 'FAIL'}  {label}  ({time.monotonic() - start:.1f}s)")
            if not ok:
                failed.append(label)
            if not ok or verbose:
                print("    " + output.strip().replace("\n", "\n    "))
    plural = "s" if len(mods) != 1 else ""
    print(f"\n{runs - len(failed)}/{runs} runs verified ({len(mods)} module{plural}): "
          "real kernel, real QEMU, the kernel's own stub")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
