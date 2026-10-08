"""Module 04: int3 traps into the stub; '?', 'g', 'G', 'p', 'P' see and
change the interrupted CPU's registers."""
from gdbstub_mcp.rsp import RSPError

from harness import le32, ok, parse_args, rsp, symbols

args = parse_args()
syms = symbols(args.elf)
c = rsp(args.port)
ok(f"handshake: qSupported -> {c.features}")

assert c.stop_reason() == b"S05"
ok("'?' -> S05 (SIGTRAP: stopped at the boot-time int3)")

regs = c.read_registers()
assert len(regs) == 16 * 4, len(regs)
eip, esp, eflags = le32(regs, 8), le32(regs, 4), le32(regs, 9)
fn = syms.containing_function(eip)
assert fn is not None and fn.name == "gdb_breakpoint", syms.symbolize(eip)
ok(f"'g' -> 16 registers; eip = {syms.symbolize(eip)} (just after the int3)")
stack_top = syms.lookup("stack_top")[0].address
assert stack_top - 16384 < esp < stack_top, hex(esp)
ok(f"esp = {esp:#x} is inside the kernel stack (rebuilt from the frame address)")
assert eflags & 0x2, "bit 1 of EFLAGS always reads 1"

new = bytearray(regs)
new[0:4] = (0x12345678).to_bytes(4, "little")       # eax
assert c.request(b"G" + new.hex().encode()) == b"OK"
assert le32(c.read_registers(), 0) == 0x12345678
ok("'G' wrote eax=0x12345678 into the trap frame; 'g' reads it back")

assert int.from_bytes(c.read_register(8), "little") == eip
c.write_register(3, (0xCAFE).to_bytes(4, "little"))  # ebx
assert le32(c.read_registers(), 3) == 0xCAFE
ok("'p 8' returns eip; 'P 3=...' sets ebx")
try:
    c.write_register(4, (0).to_bytes(4, "little"))
    raise AssertionError("writing esp must be refused")
except RSPError as e:
    assert "E01" in str(e), e
ok("'P' on esp is refused with E01 (the frame lives on that stack)")
