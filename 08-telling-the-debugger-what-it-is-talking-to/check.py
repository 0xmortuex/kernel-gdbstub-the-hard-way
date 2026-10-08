"""Module 08: qSupported advertises qXfer:features:read, and target.xml
describes the 16 registers well enough for a generic client."""
from gdbstub_mcp.regs import describe_flags, expand_includes, parse_target_xml
from gdbstub_mcp.rsp import RSPError

from harness import le32, ok, parse_args, rsp

args = parse_args()
c = rsp(args.port)
assert c.features.get("qXfer:features:read") == "+", c.features
ok(f"qSupported -> {c.features}")

first = c.request("qXfer:features:read:target.xml:0,20")
assert first[:1] == b"m" and len(first) == 0x21, first
ok(f"32-byte chunk at offset 0 -> 'm' (more follows): {first[1:].decode()!r}")

xml = c.read_xfer("features", "target.xml")
layout = parse_target_xml(expand_includes(xml, lambda a: c.read_xfer("features", a)))
names = [r.name for r in layout.registers]
assert names[:9] == ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip"], names
assert len(names) == 16 and layout.architecture == "i386"
assert layout.pc().name == "eip" and layout.sp().name == "esp" and layout.fp().name == "ebp"
ok(f"target.xml ({len(xml)} chars) -> 16 registers, pc=eip sp=esp fp=ebp")

blob = c.read_registers()
eflags = layout.decode(layout.by_name("eflags"), layout.split_g(blob)["eflags"])
assert eflags == le32(blob, 9)
flags = describe_flags(layout.by_name("eflags"), eflags)
ok(f"eflags = {eflags:#x} [{flags}], decoded from the XML's flag fields")

try:
    c.request("qXfer:features:read:other.xml:0,20")
    raise AssertionError("unknown annex must be an error")
except RSPError as e:
    assert "E00" in str(e), e
ok("unknown annex -> E00")
