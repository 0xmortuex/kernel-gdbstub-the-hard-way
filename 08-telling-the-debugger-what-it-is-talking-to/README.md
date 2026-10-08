# Module 08: Telling the Debugger What It's Talking To

## The Concept

The `g` packet is a blob of hex. Nothing in it says which bytes are EAX and which are
EIP, or how many registers there are. Until now, the check scripts *knew*: register 8
is EIP, because the code says so. A generic client doesn't know. It has to guess the
layout from the architecture, which only works if you happen to match the layout it
guesses.

gdb solved this with **target descriptions**: an XML document the stub serves on
request, naming every register, its size, its type, and the order it appears in `g`.
[MCP Server — The Hard Way, Module 03](https://github.com/0xmortuex/mcp-server-the-hard-way)
built the client that reads QEMU's description of 50 registers. This module serves your
own description of 16.

## Advertising It: `qSupported`

The very first thing a client sends is `qSupported`, listing what *it* supports. The
stub answers with what *it* supports:

```c
if (starts_with(inbuf, "qSupported")) {
    put_packet("PacketSize=1000;qXfer:features:read+");
    break;
}
```

- **`PacketSize=1000`** (hex: 4096 bytes) is the largest packet the stub can receive.
  A careful client keeps writes under it.
- **`qXfer:features:read+`** means "you can read my target description".

Before this module, the stub answered just `PacketSize=1000`. A client that sees no
`qXfer:features:read+` won't ask for `target.xml`. gdb falls back to its built-in i386
layout, and gdbstub-mcp refuses to guess and reports an error. That's why Module 09
can't use the full agent tools without this one.

## The Document

```xml
<target><architecture>i386</architecture>
<feature name="org.gnu.gdb.i386.core">
  <flags id="i386_eflags" size="4">
    <field name="CF" start="0" end="0"/> ... <field name="IF" start="9" end="9"/> ...
  </flags>
  <reg name="eax" bitsize="32" type="int32" regnum="0"/>
  <reg name="ecx" bitsize="32" type="int32"/>
  ...
  <reg name="esp" bitsize="32" type="data_ptr"/>
  <reg name="ebp" bitsize="32" type="data_ptr"/>
  ...
  <reg name="eip" bitsize="32" type="code_ptr"/>
  <reg name="eflags" bitsize="32" type="i386_eflags"/>
  <reg name="cs" bitsize="32" type="int32"/> ... <reg name="gs" .../>
</feature></target>
```

Every field earns its place:

- **`<architecture>i386`** tells the client which disassembler and calling convention
  to use. gdbstub-mcp turns on its x86-specific backtrace handling from this string.
- **Order is everything.** Registers are numbered in document order, starting from
  `regnum="0"`. That number is the index in `g` and the `n` in `p n`/`P n=`. The XML must
  match `reg_get()`'s switch exactly.
- **`code_ptr` / `data_ptr`** tell the client which registers hold addresses worth
  symbolizing. That's how the client knows EIP is the program counter without a lookup
  table.
- **The flags type** lets a client print `eflags = 0x93 [CF AF SF]` instead of a bare
  number.
- **The feature name `org.gnu.gdb.i386.core`** is the name gdb expects for this register
  set. Use the standard names, and gdb treats your registers exactly like a real i386.

## Serving It: `qXfer`

The document is 1240 bytes, but the client chooses the chunk size:

```
qXfer:features:read:target.xml:0,fff     -> l<entire document>
qXfer:features:read:target.xml:0,20      -> m<first 32 bytes>      ('m' = more follows)
qXfer:features:read:target.xml:20,20     -> m<next 32 bytes>
...                                      -> l<last bytes>          ('l' = last)
```

```c
u32 off = parse_hex(&p);
...
u32 len = parse_hex(&p);
...
*o++ = (off + len >= total) ? 'l' : 'm';
for (; i < total && i < off + len; i++) {
    char c = target_xml[i];
    if (c == '#' || c == '$' || c == '}' || c == '*') {
        *o++ = '}';
        c = (char)(c ^ 0x20);
    }
    *o++ = c;
}
```

The reply is *binary data*, so the four bytes that mean something to the framing get
escaped as `}` followed by the byte XOR 0x20. This document happens to contain none of
them. The stub escapes anyway, because the day someone adds `#` to a comment, an
unescaped reply would end the packet early and fail its checksum.

An unknown annex (any file other than `target.xml`) gets `E00`. A description can
`xi:include` other files, as QEMU's does with `i386-32bit.xml`. This one is
self-contained, so it never needs to serve a second file.

## The Check

```
ok  qSupported -> {'PacketSize': '1000', 'qXfer:features:read': '+'}
ok  32-byte chunk at offset 0 -> 'm' (more follows): '<?xml version="1.0"?><!DOCTYPE t'
ok  target.xml (1240 chars) -> 16 registers, pc=eip sp=esp fp=ebp
ok  eflags = 0x2 [], decoded from the XML's flag fields
ok  unknown annex -> E00
```

The third line is the important one. The check uses **gdbstub-mcp's own parser**: the
code that would read QEMU's 50-register description works out, from your XML alone,
which register is the program counter, which is the stack pointer, and which is the
frame pointer. No knowledge of this stub is involved. That's the definition of
"describes itself".

EFLAGS is `0x2` here: no flags set except the always-one bit 1, which has no name, so
the bracket list is empty. At a breakpoint in Module 09 you'll see `[CF AF SF]`.

## Fewer Registers Than QEMU

QEMU describes 50 registers: control registers, segment bases, x87 and SSE. This stub
describes 16, because the trap frame only saves 16. Describing a register you can't
read is worse than leaving it out: the client would ask for it with `p` and get an
error mid-session. Describe exactly what you can deliver. Adding CR0/CR2/CR3 is a
read-only `mov %crN` away; see break-it.
