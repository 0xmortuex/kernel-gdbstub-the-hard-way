"""Helpers shared by every module's check.py.

verify.py boots each module's kernel with COM1 wired to a TCP socket:

    qemu-system-i386 -kernel kernel.elf -serial tcp:127.0.0.1:PORT,server=on,wait=on

`wait=on` makes QEMU hold the CPU until a client connects, so nothing the
kernel prints can be lost - and the client below is that first connection.
QEMU's own gdbstub (-s) is never used: everything goes through the kernel.
"""

from __future__ import annotations

import argparse
import socket
import time

from gdbstub_mcp.rsp import RSPClient
from gdbstub_mcp.symbols import SymbolTable


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True, help="TCP port of COM1")
    ap.add_argument("--elf", required=True, help="the module's kernel.elf (symbols)")
    return ap.parse_args()


def connect_raw(port: int, timeout: float = 20.0) -> socket.socket:
    """Connect to COM1, retrying while QEMU starts up."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            s.settimeout(5)
            return s
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.1)


def read_until(sock: socket.socket, needle: bytes, timeout: float = 10.0) -> bytes:
    data = b""
    deadline = time.monotonic() + timeout
    while needle not in data:
        if time.monotonic() > deadline:
            raise AssertionError(f"timed out waiting for {needle!r}; got {data!r}")
        chunk = sock.recv(4096)
        if not chunk:
            raise AssertionError(f"connection closed before {needle!r}; got {data!r}")
        data += chunk
    return data


def rsp(port: int) -> RSPClient:
    """gdbstub-mcp's RSP client, pointed at the kernel's serial port."""
    return RSPClient("127.0.0.1", port, connect_timeout=20, read_timeout=10)


def symbols(elf: str) -> SymbolTable:
    return SymbolTable(elf)


def le32(blob: bytes, index: int) -> int:
    """Register `index` (0=eax ... 8=eip) out of a 'g' reply."""
    return int.from_bytes(blob[4 * index:4 * index + 4], "little")


def ok(msg: str) -> None:
    print(f"  ok  {msg}")
