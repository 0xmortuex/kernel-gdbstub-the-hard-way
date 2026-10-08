"""Module 10: the same stub in 64-bit long mode - wider registers in gdb's
amd64 order, an editable RSP, fault-safe reads that also catch non-canonical
addresses, and gdbstub-mcp's tools working unchanged against it."""
from gdbstub_mcp import server as S

from harness import ok, parse_args, rsp, symbols

args = parse_args()
syms = symbols(args.elf)

# ---- Raw RSP: the 64-bit 'g' packet -----------------------------------------
c = rsp(args.port)
assert c.stop_reason().startswith(b"S05"), "kernel should be stopped at its int3"
regs = c.read_registers()
# 17 x 8 (rax..r15, rip) + 7 x 4 (eflags, cs ss ds es fs gs) + 2 x 8 (fs/gs base)
assert len(regs) == 17 * 8 + 7 * 4 + 2 * 8 == 180, len(regs)
rip = int.from_bytes(regs[16 * 8:17 * 8], "little")
bp_fn, _ = syms.lookup("gdb_breakpoint")
assert syms.containing_function(rip) is not None
assert syms.containing_function(rip).name == "gdb_breakpoint", hex(rip)
ok(f"'g' is 180 bytes; rip = {rip:#x} = {syms.symbolize(rip)}")

eflags = int.from_bytes(regs[136:140], "little")
cs = int.from_bytes(regs[140:144], "little")
assert cs == 0x08, f"cs should be boot.s's 64-bit code selector, got {cs:#x}"
assert eflags & 0x2, "eflags bit 1 is always set"
ok(f"eflags = {eflags:#x}, cs = {cs:#x} (the L=1 descriptor from boot.s)")

# RSP is in the long-mode trap frame, so unlike Module 04 it can be written.
rsp_val = int.from_bytes(c.read_register(7), "little")
c.write_register(7, (rsp_val - 8).to_bytes(8, "little"))
assert int.from_bytes(c.read_register(7), "little") == rsp_val - 8
c.write_register(7, rsp_val.to_bytes(8, "little"))
ok(f"rsp is writable in long mode (wrote {rsp_val - 8:#x}, restored {rsp_val:#x})")

# Fault-safe memory: unmapped (#PF) and non-canonical (#GP) both answer E14.
for addr, why in ((0x40000000, "#PF: first byte above the 1 GiB identity map"),
                  (0x8000000000000000, "#GP: non-canonical address")):
    try:
        c.read_memory(addr, 4)
        raise AssertionError(f"read of {addr:#x} should fail")
    except Exception as e:
        assert "E14" in str(e), e
    ok(f"m{addr:x},4 -> E14 ({why}); the stub survived")
assert len(c.read_memory(bp_fn.address, 4)) == 4
# Close without detaching: the kernel stays stopped in serve(), waiting for
# the next packet - from whichever debugger connects next.
c.close()

# ---- gdbstub-mcp, unchanged, against the 64-bit stub --------------------------
out = S.debug_connect("hw64", port=args.port, elf=args.elf, timeout_s=20)
assert "i386:x86-64, 26 registers" in out and "SIGTRAP" in out, out
assert "gdb_breakpoint" in out, out
ok("debug_connect: " + " | ".join(out.splitlines()))
out = S.debug_break("hw64", "add")
assert "#1 sw breakpoint" in out and "kernel.c" in out, out
ok("debug_break add -> " + out)

out = S.debug_continue("hw64", 10)
assert "breakpoint #1 (add)" in out and "next: push rbp" in out, out
ok("debug_continue -> " + " | ".join(out.splitlines()))

out = S.debug_registers("hw64", "rip,rsp,rdi,rsi,eflags")
assert "<add>" in out and "rip" in out, out
ok("debug_registers:\n" + out)

out = S.debug_backtrace("hw64", mode="fp")
names = [ln.split("<")[1].split(">")[0].split("+")[0] for ln in out.splitlines() if "<" in ln]
assert names[:4] == ["add", "compute", "kmain", "long_start"], out
ok("debug_backtrace:\n" + out)

out = S.debug_disassemble("hw64", "add", count=3)
assert "push rbp" in out and "mov rbp, rsp" in out, out
ok("debug_disassemble add:\n" + out)

S.debug_delete("hw64", 1)
out = S.debug_continue("hw64", 0.5)
assert out.startswith("Still running"), out
out = S.debug_interrupt("hw64")
assert "SIGINT" in out, out
ok("Ctrl-C -> " + " | ".join(out.splitlines()))

out = S.debug_disconnect("hw64")
assert "the guest is running" in out, out
ok(out)
