#!/usr/bin/env python3
"""vg-sink-check.py <sink> -- the virtio-gpu gate's pixel verdict.

The probe (ci/vgprobe-init.c) paints a 64x64 block at the origin and a 40x40
block at (500,300) through /dev/fb0 and nothing else, so the sink must show
exactly that: red where painted, black where not, in the umfb layout the app's
viewer reads (1024-byte header, then stride*height BGRX).

Exits 0 only if the header is sane and both blocks are red at their corners.
"""
import struct
import sys

MAGIC = 0x4D46554D


def main(path):
    try:
        with open(path, 'rb') as f:
            raw = f.read()
    except OSError as e:
        print('  FAIL no sink (%s) -- the backend never published a frame' % e)
        return 1
    if len(raw) < 1024:
        print('  FAIL sink shorter than its header (%d bytes)' % len(raw))
        return 1
    magic, ver, w, h, stride, seq, state = struct.unpack_from('<IIIIIII', raw, 0)
    print('sink: magic=0x%08x v%d %dx%d stride=%d seq=%d state=%d bytes=%d'
          % (magic, ver, w, h, stride, seq, state, len(raw)))
    if magic != MAGIC:
        print('  FAIL sink magic')
        return 1
    if (w, h, stride) != (1024, 768, 4096):
        print('  FAIL sink geometry (want 1024x768 stride 4096)')
        return 1
    if len(raw) < 1024 + stride * h:
        print('  FAIL sink truncated')
        return 1
    px = raw[1024:]

    def at(x, y):
        o = y * stride + x * 4
        return px[o], px[o + 1], px[o + 2]      # B, G, R

    red = (0, 0, 255)
    bad = 0
    for (x, y) in ((0, 0), (63, 63), (32, 32), (500, 300), (539, 339), (520, 320)):
        got = at(x, y)
        ok = got == red
        print('  %s (%d,%d) BGR=%s' % ('ok  ' if ok else 'FAIL', x, y, got))
        bad += 0 if ok else 1
    # a point the probe never touched must still be black
    got = at(900, 700)
    ok = got == (0, 0, 0)
    print('  %s untouched (900,700) BGR=%s' % ('ok  ' if ok else 'FAIL', got))
    bad += 0 if ok else 1

    # Where did the red actually land?  The bbox turns a coordinate failure
    # into a diagnosis: a clipped rect, a dropped row, or nothing at all.
    minx = miny = 1 << 30
    maxx = maxy = -1
    count = 0
    for y in range(h):
        row = px[y * stride:(y + 1) * stride]
        if b'\x00\x00\xff' not in row:      # cheap reject
            continue
        for x in range(w):
            o = x * 4
            if row[o] == 0 and row[o + 1] == 0 and row[o + 2] == 255:
                count += 1
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if count:
        print('  red bbox: x %d..%d y %d..%d (%d px; probe painted 64x64@0,0 and 40x40@500,300)'
              % (minx, maxx, miny, maxy, count))
    else:
        print('  red bbox: none -- the frame carries no painted pixels')
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('usage: vg-sink-check.py <sink>')
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
