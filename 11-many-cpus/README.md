# Module 11: Many CPUs

## The Concept

Every earlier module leaned on one rule: *the stub owns the machine while it runs*. With
one CPU that's free. Interrupts are off, so nothing else can happen. With four CPUs, the
other three keep running while one of them talks to the debugger. They change memory
behind its back, hit the breakpoints it's managing, and print to the UART it's using.

So this module adds three things:
1. waking the other CPUs at all
2. **stopping the world** whenever one CPU stops
3. telling the debugger there's more than one CPU, using threads

`verify.py` runs it twice, under `-smp 2` and `-smp 4`.

## Waking the Other CPUs

At reset only the **bootstrap processor** (BSP) runs. The others, the **application
processors** (APs), wait for a message from the BSP's local APIC:

```
 BSP                                    every AP
 count_cpus(): MADT type-0 entries
 copy trampoline → 0x8000
 ICR ← INIT        (all but self)  ──►  reset, wait
 ICR ← STARTUP 08  (twice)         ──►  start at 0x08 × 0x1000 = 0x8000, in REAL mode
                                        16-bit → lgdt → protected mode
                                        → enter_long_mode (shared with the BSP)
                                        → ap_entry: lock xadd = my stack slot
 wait until cpus_started == count  ◄──  gdb_ap_start: lidt, online[me] = 1, ++started
                                        → ap_main(apic_id) forever
```

An AP starts in 16-bit real mode, like a CPU fresh out of reset, at an address it can
only express as a page number (vector 0x08 means 0x8000). So the first few dozen bytes it
runs must live below 1 MiB, and they must not jump anywhere using an address computed at
link time: `ap_trampoline` is copied, so every reference inside it is written as
`label - ap_trampoline + 0x8000`. Once in protected mode it jumps to the kernel's own
`enter_long_mode`, through an absolute `jmp *%eax`. That's the same code the BSP ran, so
both share one page table and one GDT.

How many APs should check in? The firmware's ACPI MADT lists one *Processor Local APIC*
entry per CPU, and `count_cpus()` counts the enabled ones. (The companion course
[ACPI — The Hard Way](https://github.com/0xmortuex/acpi-the-hard-way) builds the parser
properly.) A fixed timeout would be simpler and wrong: a slow, loaded CI runner can
easily start an AP later than any guess.

## Stopping the World

When a CPU traps, it takes `stub_lock` and sends an **NMI** to every other CPU.
The others park:

```c
if (f->vector == 2 && stub_owner != me) {   /* NMI from the stopping CPU: park */
    park(me, f);
    return;
}
```

Why NMI and not a normal interrupt vector? Because interrupts are disabled everywhere in
this kernel, and an NMI is the one interrupt that ignores IF. A parked CPU publishes
its trap frame in `cpu_frame[me]` and spins until `release_gen` changes. Then it
returns with `iretq`, carrying any register edits the debugger made.

The owner waits until `parked == online_count() - 1`, and only then talks to the
debugger. `check.py` proves the world really stopped: `ap_counter` doesn't move during
half a second of being stopped, and it races ahead (38,262 → 7,391,174) once continued.

## CPUs as Threads

RSP has no notion of CPUs, but it has threads, and every SMP stub maps one onto the
other. Here, thread *n* is the CPU with APIC ID *n − 1*:

| Packet | Answer | Meaning |
|---|---|---|
| `qfThreadInfo` / `qsThreadInfo` | `m01,02,03,04` / `l` | the thread list, in chunks; `l` = no more |
| `qC` | `QC01` | which thread stopped |
| `Hg03` | `OK` | `g G p P` now read and write thread 3's frame |
| `Hc…` | `OK` | accepted; `c`/`s` always act on the whole machine |
| `T03` | `OK` / `E01` | is thread 3 alive? |
| stop reply | `T05thread:03;` | instead of `S05`: the signal *and* the CPU |

`Hg` is just a pointer swap. `serve()` reads registers from `cpu_frame[sel_thread]`
instead of its own `f`. The MSR-held `fs_base`/`gs_base` are the one catch: they belong
to whichever CPU executes `rdmsr`, so each CPU captures its own when it stops.

## The Stale Breakpoint

The first version of this module failed its own check in a way every real SMP debugger
has had to solve. Three APs run the same loop, so they can hit the `ap_work` breakpoint
in the same instant. One wins the lock. The other two have **already executed the
`int3`**, and they wait in `gdb_trap` until the NMI parks them.

The debugger sees one hit, removes the breakpoint, and continues. Now a waiting CPU gets
the lock and reports a stop for a breakpoint that no longer exists, at `ap_work+1`.
Worse, if it simply resumed there, it would skip the `push rbp` the `int3` had replaced
and corrupt its stack.

The fix decides "was this one of our breakpoints?" *before* queueing for the lock, then
checks again after getting it:

```c
int hit_bp = f->vector == 3 && bp_find(f->rip - 1) >= 0;
if (hit_bp) f->rip -= 1;                  /* back onto the patched byte */
... take the lock, stop the others ...
if (hit_bp && bp_find(f->rip) < 0)
    goto leave;                           /* removed meanwhile: just run it */
```

A queued CPU parked by the NMI also shows the debugger its *pending* frame, where its
kernel code was, rather than the inside of the stub's spin loop.

## Stepping One CPU

The step-over dance from Module 07 lifts an `int3` for one instruction. If the other CPUs
were running during that instruction, they could sail through the unpatched breakpoint.
So a step keeps them parked and the lock held across the `#DB`:

```c
if (f->rflags & RFLAGS_TF) { others_held = 1; return; }
```

That means `s` runs exactly one instruction on exactly one CPU, and `check.py` checks
that the others' counters didn't move. gdb calls this "scheduler locking", and it's
what you want when stepping kernel code.

## What's Still Simplified

- One CPU at a time can own the stub. Two CPUs that fault in the same instant are
  reported one after the other.
- NMIs can't be masked. A CPU already inside an NMI handler can't be parked until it
  returns.
- `vCont`, which allows per-thread continue and step, isn't implemented. `c` resumes
  everyone and `s` steps only the stopped CPU.
- Only the BSP polls the UART, so Ctrl-C always stops thread 1.
