# Break It: Framing

## Challenge 1: Remove the Resync

Delete the `if (c == '$') { restart = 1; break; }` line. Send `$?#3` (cut short), then
`$?#3f`. What does the stub do?

**Hint:** Without resync, the second packet's bytes become part of the first:
`?#3$?#3f` up to the first `#`, then garbage checksum digits. The stub NACKs, the
debugger resends, and the stub probably recovers on the next try. Now do the same when
the truncated packet is a long `M` write. How many round trips does recovery cost?

## Challenge 2: Overflow `inbuf`

Send a packet longer than `inbuf`. What happens with the truncation, and what would
happen without the `n < sizeof(inbuf) - 1` check?

**Hint:** With the check, the payload is cut but the checksum still covers every byte
received, so it fails and the debugger gets `-` forever for that packet. Without the
check, you overwrite whatever follows `inbuf` in `.bss`, from inside the debugger, with
bytes an attacker on the serial line chooses. Stubs are attack surface.

## Challenge 3: A Debugger That Never Acks

Have the client send `?` and then never answer the reply with `+`. Where does the stub
sit?

**Hint:** In `put_packet`'s inner loop, forever. For a stub that's acceptable: it only
runs when the machine is stopped. But it's why a stub must never be entered from a
context that has to make progress, like a watchdog or a timer interrupt, unless you're
prepared for the machine to freeze until the debugger reconnects.

## Challenge 4: Uppercase Hex

Send `$?#3F` with an uppercase checksum. Does the stub accept it? Should it?

**Hint:** `hexval` accepts both cases, and the protocol allows either. The stub's
*replies* use lowercase. A stub that only accepts lowercase works with gdb and fails
with a client that happens to send uppercase, and that's the kind of incompatibility you
only discover in the field.

→ [Next: Module 04](../04-trapping-and-registers/README.md)
