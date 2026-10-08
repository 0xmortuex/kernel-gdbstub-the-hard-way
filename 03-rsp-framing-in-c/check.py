"""Module 03: the C side of RSP framing - checksums, acks, NACK + resend."""
from harness import connect_raw, ok, parse_args


def frame(payload: bytes) -> bytes:
    return b"$" + payload + b"#%02x" % (sum(payload) % 256)


def read_packet(s) -> bytes:
    buf = b""
    while not (b"#" in buf and len(buf) >= buf.index(b"#") + 3):
        buf += s.recv(1)
    return buf


args = parse_args()
s = connect_raw(args.port)

s.sendall(frame(b"?"))
assert s.recv(1) == b"+", "stub must ack a good packet"
reply = read_packet(s)
assert reply == b"$S05#b8", reply
s.sendall(b"+")
ok("'?' -> ack '+', reply $S05#b8 ('S'+'0'+'5' = 0x53+0x30+0x35 = 0xb8)")

s.sendall(b"$?#00")                      # wrong checksum
assert s.recv(1) == b"-", "stub must NACK a corrupted packet"
ok("corrupted checksum -> '-' (NACK): the debugger will resend")

s.sendall(b"++noise" + frame(b"qFoo"))   # stray acks/noise before '$' are ignored
assert s.recv(1) == b"+"
assert read_packet(s) == b"$#00"
s.sendall(b"+")
ok("unknown packet -> empty reply $#00 ('not supported'); noise before '$' ignored")

s.sendall(frame(b"?"))
assert s.recv(1) == b"+"
assert read_packet(s) == b"$S05#b8"
s.sendall(b"-")                          # pretend the reply arrived corrupted
assert read_packet(s) == b"$S05#b8"
s.sendall(b"+")
ok("our '-' made the stub resend its reply")
