"""Module 11: several CPUs (verify.py boots -smp 2 and -smp 4). Each CPU is a thread; the
CPU that stops parks the others with an NMI; Hg switches whose registers
you read; a breakpoint hit on an application processor says which one."""
import os
import re
import time

from harness import ok, parse_args, rsp, symbols

args = parse_args()
syms = symbols(args.elf)
c = rsp(args.port)
NCPU = int(os.environ.get("QEMU_SMP", "4"))
TIDS = list(range(1, NCPU + 1))


def thread_regs(tid: int) -> tuple[int, int]:
    """(rip, rdi) of thread `tid`, via Hg + g."""
    assert c.request(f"Hg{tid:x}") == b"OK"
    regs = c.read_registers()
    return int.from_bytes(regs[128:136], "little"), int.from_bytes(regs[40:48], "little")


def expect_error(packet: str) -> str:
    """The reply to a packet that must fail (RSPClient raises on Exx)."""
    try:
        reply = c.request(packet)
    except Exception as e:
        return str(e)
    raise AssertionError(f"{packet} should have failed, got {reply!r}")


def fn(addr: int) -> str:
    f = syms.containing_function(addr)
    return f.name if f else hex(addr)


def counters() -> list[int]:
    base = syms.lookup("ap_counter")[0].address
    raw = c.read_memory(base, 8 * NCPU)
    return [int.from_bytes(raw[8 * i:8 * i + 8], "little") for i in range(NCPU)]


# ---- Who's here ----------------------------------------------------------------
stop = c.stop_reason()
assert stop == b"T05thread:01;", stop
ok(f"'?' -> {stop.decode()}: the boot CPU (APIC ID 0 = thread 1) stopped at its int3")

threads = c.request("qfThreadInfo")
assert threads == ("m" + ",".join(f"{t:02x}" for t in TIDS)).encode(), threads
assert c.request("qsThreadInfo") == b"l"
assert c.request("qC") == b"QC01"
assert c.request(f"T{NCPU:02x}") == b"OK" and "E01" in expect_error(f"T{NCPU + 1:02x}")
ok(f"qfThreadInfo -> {threads.decode()} / qsThreadInfo -> l: {NCPU} CPUs, one thread each")

# ---- Hg changes whose registers you read ------------------------------------------
rip1, _ = thread_regs(1)
assert fn(rip1) == "gdb_breakpoint", fn(rip1)
for tid in TIDS[1:]:
    rip, _ = thread_regs(tid)
    assert fn(rip) in ("ap_main", "ap_work"), f"thread {tid} at {fn(rip)}"
    assert rip != rip1
    ok(f"Hg{tid:02x}: rip = {rip:#x} <{syms.symbolize(rip)}> (thread 1 is in gdb_breakpoint)")
assert "E22" in expect_error("Hg07")
ok("Hg07 -> E22: no such CPU")

# ---- The others really are stopped ------------------------------------------------
before = counters()
time.sleep(0.5)
after = counters()
assert before == after, (before, after)
ok(f"ap_counter unchanged over 0.5s while stopped: {after} - the APs are parked in their NMI handlers")

# ---- A breakpoint that only APs hit ---------------------------------------------
ap_work = syms.lookup("ap_work")[0].address
c.set_breakpoint(0, ap_work, 1)
c.resume()
reply = c.wait_stop(10)
assert reply is not None, "no AP hit ap_work within 10s"
m = re.fullmatch(rb"T05thread:([0-9a-f]{2});", reply)
assert m, reply
tid = int(m.group(1), 16)
assert tid in TIDS[1:], f"ap_work runs only on APs, but thread {tid} reported"
assert c.stop_reason() == reply
rip, rdi = thread_regs(tid)
assert rip == ap_work, hex(rip)
assert rdi == tid - 1, f"ap_work(cpu) should get its APIC ID {tid - 1}, rdi = {rdi}"
ok(f"breakpoint hit -> {reply.decode()}: thread {tid} at ap_work, rdi (cpu) = {rdi}")

# ---- Stepping one CPU leaves the others parked ------------------------------------
others = [i for i in range(NCPU) if i != tid - 1]
snap = counters()
c.resume(step=True)
reply = c.wait_stop(10)
assert reply == f"T05thread:{tid:02x};".encode(), reply
rip, _ = thread_regs(tid)
assert rip == ap_work + 1, f"one instruction (push rbp) past ap_work, got {rip:#x}"
assert [counters()[i] for i in others] == [snap[i] for i in others]
ok(f"'s' on thread {tid}: rip = ap_work+1, and every other CPU stayed parked")

# ---- Ctrl-C lands on the boot CPU (only it polls the UART) -------------------------
c.clear_breakpoint(0, ap_work, 1)
c.resume()
assert c.wait_stop(0.5) is None
reply = c.interrupt()
assert reply == b"T02thread:01;", reply
rip1, _ = thread_regs(1)
ok(f"Ctrl-C -> {reply.decode()}: thread 1 in {fn(rip1)}")
after_run = counters()
assert all(after_run[i] > snap[i] for i in others if i != 0), (snap, after_run)
ok(f"the APs ran while continued: ap_counter {snap} -> {after_run}")
c.close()
