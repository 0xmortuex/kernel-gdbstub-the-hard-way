# Break It: Target Descriptions

## Challenge 1: Swap Two Registers in the XML

Swap the `<reg>` lines for `ecx` and `edx`, leaving `reg_get` alone. Run the check, then
read registers with gdbstub-mcp.

**Hint:** The check still passes, because it only verifies names, pc/sp/fp and count. But
every client now shows ECX's value under the name EDX. The XML and the `g` order are a
contract with nothing enforcing it. A stub test that compares `p n` against the XML's
regnum for each register would catch it.

## Challenge 2: Add CR2

Page faults are the most common kernel crash, and CR2 holds the faulting address. Add
`<reg name="cr2" bitsize="32" type="data_ptr" regnum="16"/>` and make `p 10` return
`mov %cr2`.

**Hint:** Keep it *out* of `g`: the client reads registers beyond the `g` blob one at a
time with `p`, and Module 03 of the MCP course explains how gdbstub-mcp handles that.
`debug_registers(all=True)` should then show it. Make `P 10` return `E01`, because CR2
is effectively read-only state.

## Challenge 3: A Description Bigger Than PacketSize

Grow the XML past 4096 bytes (add comments) and connect with gdbstub-mcp. Does it still
work?

**Hint:** Yes. The client reads `qXfer` in chunks of its own choosing (0xfff bytes), and
the stub answers `m` until the last chunk. Now break the chunking by always answering
`l`, and watch the client get a truncated document and fail to parse it.

## Challenge 4: Forget the Escape

Add a comment containing `#` to the XML, then remove the escaping loop. What does the
client receive?

**Hint:** The `#` ends the packet early. The two bytes after it are taken as the
checksum, which fails, so the client NACKs, the stub resends the identical broken
packet, and they loop forever. Binary-safe framing isn't optional for `qXfer`.

## Challenge 5: Lie About the Architecture

Change `<architecture>` to `i386:x86-64`. What does gdbstub-mcp do differently?

**Hint:** It still finds the PC, because it looks up `eip` by name. But it decodes
memory as 64-bit code, so every `next:` line and every disassembly is wrong. `push ebp`
still decodes, but anything with an operand-size or REX-sensitive encoding doesn't.
Nothing errors. The architecture string steers decoding, and the register names steer
everything else. Both must tell the truth.

→ [Next: Module 09](../09-debugging-it-with-an-agent/README.md)
