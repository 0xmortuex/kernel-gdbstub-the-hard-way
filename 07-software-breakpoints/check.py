"""Module 07: Z0/z0 patch int3 into code; hits rewind EIP; continuing from a
breakpoint steps over it; 'm' hides the int3."""
from harness import le32, ok, parse_args, rsp, symbols

args = parse_args()
syms = symbols(args.elf)
c = rsp(args.port)
add = syms.lookup("add")[0].address
original = syms.code_bytes(add, 4)

c.set_breakpoint(0, add, 1)
assert c.read_memory(add, 4) == original
ok(f"Z0 at add ({add:#x}); 'm' still shows the original bytes {original.hex()} (int3 hidden)")


def run_to_stop() -> bytes:
    c.resume()
    reply = c.wait_stop(10)
    assert reply is not None, "kernel never stopped"
    return c.read_registers()


def arg_b(regs: bytes) -> int:
    # At add's first instruction: [esp] = return address, [esp+4] = a, [esp+8] = b
    esp = le32(regs, 4)
    return int.from_bytes(c.read_memory(esp + 8, 4), "little")


regs = run_to_stop()
assert le32(regs, 8) == add, syms.symbolize(le32(regs, 8))
b1 = arg_b(regs)
ok(f"hit: eip rewound to exactly add (not add+1); b = {b1}")

regs = run_to_stop()                     # continuing FROM the breakpoint
assert le32(regs, 8) == add
b2 = arg_b(regs)
assert b2 == (b1 + 1) % 5, (b1, b2)
ok(f"continued from the breakpoint and hit it on the next call: b = {b2}")

c.resume(step=True)
assert c.wait_stop(5) == b"S05"
eip = le32(c.read_registers(), 8)
assert eip != add and syms.containing_function(eip).name == "add"
ok(f"'s' from the breakpoint executed add's first instruction -> {syms.symbolize(eip)}")

regs = run_to_stop()
assert le32(regs, 8) == add
ok("the breakpoint was re-armed after the step (hit again)")

c.clear_breakpoint(0, add, 1)
break_now = syms.lookup("break_now")[0].address
c.write_memory(break_now, (1).to_bytes(4, "little"))
regs = run_to_stop()
fn = syms.containing_function(le32(regs, 8))
assert fn.name == "gdb_breakpoint", fn
assert int.from_bytes(c.read_memory(break_now, 4), "little") == 0
ok("z0 removed it; break_now=1 brought us back via the compiled-in breakpoint")
