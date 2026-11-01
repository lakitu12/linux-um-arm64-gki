#!/usr/bin/env python3
"""Verdict for the umfb gate: check the sink file the guest booted with.

The kernel's umfb driver mirrors /dev/fb0 into this file; the gate's
init wrote a known pattern through the guest's mmap and through
pwrite().  The file must show the header the kernel published, a
published frame (seq > 0), the dirty bitmap, and the exact pixels --
byte for byte, because every viewer (umview on Linux, the console app
on Android) consumes precisely these bytes.

usage: umfb-check.py <sink-file>
exit:  0 = PASS, 1 = FAIL (reasons printed)
"""
import struct
import sys

MAGIC = 0x4D424655
C_BASE = 0xFF332211
C_SQ = 0xFFCCBBAA
C_PX = 0xFF998877
C_RED = 0xFF0000FF

CHECKS = [
    ((0, 0), C_BASE, "base TL"),
    ((319, 199), C_BASE, "base BR"),
    ((64, 64), C_SQ, "square TL"),
    ((127, 127), C_SQ, "square BR"),
    ((128, 128), C_BASE, "outside square"),
    ((0, 150), C_RED, "red strip TL"),
    ((31, 159), C_RED, "red strip BR"),
    ((32, 150), C_BASE, "outside strip"),
    ((10, 100), C_PX, "pwrite pixel"),
    ((9, 100), C_BASE, "pwrite neighbour"),
    ((200, 100), C_BASE, "plain"),
]

HDR = ("<15I", "magic", "version", "width", "height", "stride", "bpp",
       "format", "tile_shift", "tiles_x", "tiles_y", "tiles_total", "hz",
       "state", "seq", "full")


def main(path):
    data = open(path, "rb").read()
    fail = []

    fields = dict(zip(HDR[1:], struct.unpack(HDR[0], data[:60])))
    print("header: " + " ".join("%s=%s" % (k, fields[k]) for k in HDR[1:]))

    if fields["magic"] != MAGIC:
        fail.append("magic %08x" % fields["magic"])
    if fields["version"] != 1:
        fail.append("version %d" % fields["version"])
    if (fields["width"], fields["height"], fields["stride"],
            fields["bpp"], fields["format"], fields["tile_shift"]) != \
            (320, 200, 1280, 32, 1, 6):
        fail.append("geometry")
    if fields["tiles_total"] != fields["tiles_x"] * fields["tiles_y"] or \
            fields["tiles_total"] != 20:
        fail.append("tiles %d" % fields["tiles_total"])
    if fields["seq"] < 1:
        fail.append("seq %d (no frame published)" % fields["seq"])
    if fields["state"] != 1:
        fail.append("state %d" % fields["state"])

    stride = fields["stride"]
    pix = data[1024:]

    def px(x, y):
        return struct.unpack("<I", pix[y * stride + x * 4:
                                       y * stride + x * 4 + 4])[0]

    for (x, y), want, name in CHECKS:
        got = px(x, y)
        if got != want:
            fail.append("%s (%d,%d)=%08x want %08x" % (name, x, y, got, want))

    words = struct.unpack("<128I", data[64:64 + 512])
    nz = sum(bin(w).count("1") for w in words)
    print("dirty tiles: %d" % nz)
    if nz < 20:
        fail.append("dirty bitmap %d<20" % nz)

    if fail:
        print("UMFB_CHECK: FAIL: " + "; ".join(fail))
        return 1
    print("UMFB_CHECK: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))