"""Module 02: an initialised 16550, read and written by polling."""
from harness import connect_raw, ok, parse_args, read_until

args = parse_args()
s = connect_raw(args.port)
read_until(s, b"echo ready\n")
ok("banner arrived after uart_init()")
for msg in (b"ping", b"0123456789" * 20):   # 200 bytes: more than the 16-byte FIFO
    s.sendall(msg)
    read_until(s, msg)
    ok(f"{len(msg)} bytes echoed back byte-for-byte")
