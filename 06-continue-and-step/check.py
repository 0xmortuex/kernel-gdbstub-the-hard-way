"""Module 06: 's' single-steps with EFLAGS.TF and #DB; 'c' resumes, and a
compiled-in breakpoint brings the kernel back."""
from harness import le32, ok, parse_args, rsp, symbols

args = parse_args()
syms = symbols(args.elf)
c = rsp(args.port)

seen = [le32(c.read_registers(), 8)]
for _ in range(6):
    c.resume(step=True)
    reply = c.wait_stop(5)
    assert reply == b"S05", reply
    regs = c.read_registers()
    seen.append(le32(regs, 8))
    assert not le32(regs, 9) & 0x100, "TF must be clear again once the step is reported"
assert len(set(seen)) == len(seen), [hex(a) for a in seen]
ok("6 single steps: " + " -> ".join(syms.symbolize(a) for a in seen))

break_now = syms.lookup("break_now")[0].address
counter = syms.lookup("counter")[0].address
before = int.from_bytes(c.read_memory(counter, 4), "little")
c.write_memory(break_now, (1).to_bytes(4, "little"))
c.resume()
reply = c.wait_stop(10)
assert reply == b"S05", reply
eip = le32(c.read_registers(), 8)
assert syms.containing_function(eip).name == "gdb_breakpoint", syms.symbolize(eip)
after = int.from_bytes(c.read_memory(counter, 4), "little")
assert after != before
ok(f"'c' ran the kernel (counter {before} -> {after}) until the break_now int3 stopped it")

c.resume()
assert c.wait_stop(0.5) is None
ok("'c' again: no stop reply while the kernel runs")
