"""Generate each module's sources from src/ and build a module's kernel.

src/ holds ONE copy of the kernel, with stage markers:

    //@if 5        (or #@if 5 in assembly)   kept from Module 05 on
    //@if 3..3                               kept only in Module 03
    //@endif

Each module directory gets a clean copy at its stage, with no markers, so
what you read in NN-module/ is exactly what that module builds.

    python build.py stage                 regenerate every module's sources
    python build.py build 05-memory ELF   build one module's kernel to ELF

Building needs `pip install ziglang` (a self-contained clang, no cross
toolchain to install).
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
SOURCES = ("boot.s", "stub.c", "kernel.c", "linker.ld")
_IF = re.compile(r"^\s*(?://|#)@if (\d+)(?:\.\.(\d+))?\s*$")
_ENDIF = re.compile(r"^\s*(?://|#)@endif\s*$")


def modules() -> list[str]:
    return sorted(d for d in os.listdir(ROOT) if re.match(r"^\d\d-", d)
                  and os.path.isdir(os.path.join(ROOT, d)))


def stage_text(text: str, stage: int) -> str:
    """Keep the lines whose enclosing @if ranges all include `stage`."""
    out: list[str] = []
    stack: list[bool] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        m = _IF.match(line)
        if m:
            lo = int(m.group(1))
            hi = int(m.group(2)) if m.group(2) else 99
            stack.append(lo <= stage <= hi)
            continue
        if _ENDIF.match(line):
            if not stack:
                raise SystemExit(f"unbalanced @endif at line {lineno}")
            stack.pop()
            continue
        if all(stack):
            out.append(line)
    if stack:
        raise SystemExit("unterminated @if")
    # Collapse the blank-line runs that removed blocks leave behind.
    result = re.sub(r"\n{3,}", "\n\n", "\n".join(out).strip("\n") + "\n")
    return result


def staged(module: str) -> dict[str, str]:
    stage = int(module[:2])
    files = {}
    for name in SOURCES:
        with open(os.path.join(SRC, name), encoding="utf-8") as fh:
            files[name] = stage_text(fh.read(), stage)
    return files


def write_stage(module: str) -> None:
    for name, text in staged(module).items():
        with open(os.path.join(ROOT, module, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)


def drift(module: str) -> list[str]:
    """Files in the module dir that don't match what src/ generates."""
    bad = []
    for name, text in staged(module).items():
        path = os.path.join(ROOT, module, name)
        try:
            with open(path, encoding="utf-8") as fh:
                current = fh.read().replace("\r\n", "\n")
        except FileNotFoundError:
            bad.append(name)
            continue
        if current != text:
            bad.append(name)
    return bad


def build(module: str, out_elf: str) -> None:
    mdir = os.path.join(ROOT, module)
    zig = [sys.executable, "-m", "ziglang", "cc", "-target", "x86-freestanding-none"]
    cflags = ["-ffreestanding", "-fno-pie", "-fno-stack-protector", "-O0", "-g",
              "-fno-omit-frame-pointer", "-fno-sanitize=undefined",
              "-mno-sse", "-mno-sse2", "-mno-mmx", "-Wall", "-Werror"]
    with tempfile.TemporaryDirectory() as tmp:
        objs = []
        for src, extra in (("boot.s", []), ("stub.c", cflags), ("kernel.c", cflags)):
            obj = os.path.join(tmp, src + ".o")
            subprocess.run([*zig, *extra, "-c", os.path.join(mdir, src), "-o", obj], check=True)
            objs.append(obj)
        subprocess.run([*zig, "-nostdlib", "-static", "-no-pie",
                        "-Wl,-T," + os.path.join(mdir, "linker.ld"), "-Wl,--build-id=none",
                        "-o", out_elf, *objs], check=True)


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "stage":
        for m in modules():
            write_stage(m)
            print(f"staged {m}")
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "build":
        build(sys.argv[2].rstrip("/\\"), sys.argv[3])
        print(f"built {sys.argv[3]}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
