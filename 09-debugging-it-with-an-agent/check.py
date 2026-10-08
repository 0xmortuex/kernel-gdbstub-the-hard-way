"""Module 09: gdbstub-mcp's agent-facing tools, end to end, against the
stub inside the kernel - including Ctrl-C over the serial line."""
from gdbstub_mcp import server as S

from harness import ok, parse_args

args = parse_args()

out = S.debug_connect("hw", port=args.port, elf=args.elf, timeout_s=20)
assert "i386, 16 registers" in out and "SIGTRAP" in out and "gdb_breakpoint" in out, out
ok("debug_connect: " + " | ".join(out.splitlines()))

out = S.debug_break("hw", "add")
assert "#1 sw breakpoint" in out and "kernel.c" in out, out
ok("debug_break add -> " + out)

out = S.debug_continue("hw", 10)
assert "breakpoint #1 (add)" in out and "next: push ebp" in out, out
ok("debug_continue -> " + " | ".join(out.splitlines()))

out = S.debug_registers("hw", "eip,esp,eflags")
assert "<add>" in out, out
ok("debug_registers:\n" + out)

out = S.debug_backtrace("hw", mode="fp")
names = [ln.split("<")[1].split(">")[0].split("+")[0] for ln in out.splitlines() if "<" in ln]
assert names[:4] == ["add", "compute", "kmain", "_start"], out
ok("debug_backtrace:\n" + out)

out = S.debug_memory("hw", "counter", 4, "words")
ok("debug_memory counter -> " + out)

S.debug_delete("hw", 1)
out = S.debug_continue("hw", 0.5)
assert out.startswith("Still running"), out
out = S.debug_interrupt("hw")
assert "SIGINT" in out, out
ok("Ctrl-C over the serial line -> " + " | ".join(out.splitlines()))

out = S.debug_disassemble("hw", "add", count=3)
assert "push ebp" in out, out
ok("debug_disassemble add:\n" + out)

out = S.debug_disconnect("hw")
assert "the guest is running" in out, out
ok(out)
