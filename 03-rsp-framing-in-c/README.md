# Module 03: RSP Framing in C

## The Concept

The Remote Serial Protocol wraps every message in the same frame:

```
 $ <payload> # <two hex digits: sum of payload bytes mod 256>

 $?#3f        '?' = 0x3f, so the checksum is 3f
 $S05#b8      'S'+'0'+'5' = 0x53+0x30+0x35 = 0xb8
```

The receiver checks the sum and answers with one byte: `+` (got it) or `-` (corrupted,
send again). That's the whole reliability layer. It's crude, but it's enough for a
serial line where the usual failure is a flipped bit, not a lost connection.

The client side of this was built in
[MCP Server — The Hard Way, Module 02](https://github.com/0xmortuex/mcp-server-the-hard-way).
Here it's the stub's turn, in C, with no allocator, no strings library, and a
byte-at-a-time UART.

## Receiving a Packet

```c
static int get_packet(void) {
    for (;;) {
        while (uart_getc() != '$')
            ;
        u8 sum = 0;
        int n = 0;
        int restart = 0;
        for (;;) {
            u8 c = uart_getc();
            if (c == '$') { restart = 1; break; }   /* resync: a new packet began */
            if (c == '#') break;
            if (n < (int)sizeof(inbuf) - 1) inbuf[n++] = (char)c;
            sum += c;
        }
        if (restart) continue;
        int hi = hexval(uart_getc());
        int lo = hexval(uart_getc());
        if (hi >= 0 && lo >= 0 && ((hi << 4) | lo) == sum) {
            uart_putc('+');
            inbuf[n] = 0;
            return n;
        }
        uart_putc('-');
    }
}
```

Each decision is about surviving a noisy line:

- **Skip everything before `$`.** The debugger's `+` acks, line noise, a stray Ctrl-C:
  none of it is a packet. Discarding until `$` is how the stub finds the start of the
  next frame.
- **A `$` inside a packet restarts it.** If the end of a packet was lost, the next `$`
  means a new packet began. Without this, two packets merge into garbage, and the
  checksum fails on both.
- **The sum includes every payload byte as sent.** `u8` arithmetic wraps at 256 for free.
- **Bad checksum: `-` and wait again.** The debugger resends the same packet. The stub
  doesn't need to remember anything.
- **`inbuf` is a fixed static array.** A stub can't call `malloc`: the allocator may be
  the thing that crashed. Oversized packets are truncated, and then fail the checksum,
  which is the right outcome.

## Sending a Packet

```c
static void put_packet(const char *data) {
    for (;;) {
        u8 sum = 0;
        uart_putc('$');
        for (const char *p = data; *p; p++) {
            uart_putc((u8)*p);
            sum += (u8)*p;
        }
        uart_putc('#');
        uart_putc((u8)hexchars[sum >> 4]);
        uart_putc((u8)hexchars[sum & 0xF]);
        for (;;) {
            u8 c = uart_getc();
            if (c == '+') return;
            if (c == '-') break;    /* resend */
        }
    }
}
```

The stub sends, then waits for the verdict. `-` means send the whole packet again. Other
bytes are ignored while waiting. There's no timeout. A stub with nothing else to do can
wait forever, and "the debugger went away" isn't something it can fix.

## Empty Means "Not Supported"

The protocol has dozens of packet types, and a stub implements only the ones it needs.
Any packet it doesn't understand gets an **empty reply**, `$#00`. Clients are built for
this: gdb sends `qSupported`, `vCont?` and friends speculatively, and an empty reply
means "fall back to something simpler". That's why a tiny stub can still talk to a full
debugger.

Module 03's server has no machine behind it yet. It answers `?` and refuses everything
else:

```c
void gdb_serve_demo(void) {
    for (;;) {
        get_packet();
        if (inbuf[0] == '?')
            put_packet("S05");
        else
            put_packet("");   /* empty reply = "I don't support that" */
    }
}
```

`S05` means "stopped by signal 5", which is SIGTRAP: the standard answer for a target
halted at a breakpoint. Module 04 makes it true.

## The Check

`check.py` speaks raw bytes over the socket, with no client library, so every byte is
visible:

```
ok  '?' -> ack '+', reply $S05#b8 ('S'+'0'+'5' = 0x53+0x30+0x35 = 0xb8)
ok  corrupted checksum -> '-' (NACK): the debugger will resend
ok  unknown packet -> empty reply $#00 ('not supported'); noise before '$' ignored
ok  our '-' made the stub resend its reply
```

The last line is the other direction of reliability. The test pretends the stub's reply
arrived corrupted by answering `-`, and the stub sends it again.

## What About No-Ack Mode?

Over TCP, acks are redundant. The transport already guarantees delivery, so clients
offer `QStartNoAckMode` to skip them. A stub over a serial line should **not** accept it:
the line is exactly where corruption happens. This stub never advertises it, and
gdbstub-mcp's client handles both modes. QEMU's own stub doesn't offer it either.

## Binary Data and Run-Length Encoding

Two parts of the framing aren't needed yet:

- **Escaping:** `}` followed by the byte XOR 0x20, for payloads that contain `#`, `$`,
  `}` or `*`. Module 08 needs it for `target.xml` replies.
- **Run-length encoding:** `X*n`. Stubs may use it to compress replies, but never have
  to, so this one doesn't.
