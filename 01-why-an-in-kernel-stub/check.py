"""Module 01: the kernel can talk over COM1, and we can hear it over TCP."""
from harness import connect_raw, ok, parse_args, read_until

args = parse_args()
s = connect_raw(args.port)      # QEMU starts the CPU only once we're connected
read_until(s, b"hello from ring 0\n")
ok("kernel printed over COM1 -> QEMU -> TCP: 'hello from ring 0'")
