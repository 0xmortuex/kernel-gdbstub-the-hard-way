"""Module 05: 'm'/'M' memory access, and a bad address answers E14 instead
of taking the stub down."""
from gdbstub_mcp.rsp import RSPError

from harness import ok, parse_args, rsp, symbols

args = parse_args()
syms = symbols(args.elf)
c = rsp(args.port)

kmain = syms.lookup("kmain")[0].address
live = c.read_memory(kmain, 32)
assert live == syms.code_bytes(kmain, 32), live.hex()
ok("'m' at kmain returns exactly the bytes in the ELF's .text")

big = c.read_memory(kmain, 0x1000)       # RSPClient asks for 0x800 per packet
assert big[:32] == live and len(big) == 0x1000
ok("4 KiB read as two 0x800-byte packets (4096 hex chars per reply)")

counter = syms.lookup("counter")[0].address
c.write_memory(counter, (0xDEADBEEF).to_bytes(4, "little"))
assert c.read_memory(counter, 4) == (0xDEADBEEF).to_bytes(4, "little")
ok("'M' wrote counter = 0xdeadbeef and 'm' reads it back")

try:
    c.read_memory(0x40000000, 4)
    raise AssertionError("reading unmapped memory must fail")
except RSPError as e:
    assert "E14" in str(e), e
ok("'m' at 0x40000000 (unmapped: paging maps only 0-4 MiB) -> E14, no crash")

assert c.stop_reason() == b"S05"
ok("the stub is still alive after the #PF it caught")

partial = c.request(f"m{0x3FFFF8:x},10")
assert len(partial) == 16, partial      # 8 bytes readable, then the mapping ends
ok("a read straddling the end of the mapping returns the 8 readable bytes")

try:
    c.write_memory(0x40000000, b"\x00")
    raise AssertionError("writing unmapped memory must fail")
except RSPError as e:
    assert "E14" in str(e), e
ok("'M' at an unmapped address -> E14 as well")
