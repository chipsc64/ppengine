#!/usr/bin/env python3
"""Write ppengine.info, the Workbench icon of the game.

    python3 tools/make_icon.py [--out build/ppengine.info]

A classic AmigaOS icon (DiskObject, WBTOOL) with a 4-colour planar image in
the standard Workbench colours (0 grey, 1 black, 2 white, 3 blue), a stack of
128 KB (the game ran from a Shell with 128 KB) and the tool type "(LOWRES)":
remove the brackets in the icon's Information window to play in 320x240.

DiskObject layout (big-endian): magic $E310, version 1, a 44-byte Gadget
(width, height, flags, the image pointer as a non-zero flag), type, default
tool, tool types, position, drawer data, tool window, stack size; then the
Image structure and its bitplanes; then the tool types, each as a length and
a NUL-terminated string.
"""

import argparse
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STACK_SIZE = 131072
TOOL_TYPES = ["(LOWRES)"]

# A racing car seen from behind. . = grey (0), # = black (1), w = white (2),
# b = blue (3).
PICTURE = """
..............................................
..................wwwwwwwwww..................
...........wwwwwwwwwwwwwwwwwwwwwwww...........
...........w##################w###w...........
...........wwwwwwwwwwwwwwwwwwwwwwww...........
....................bbbbbb....................
.................bbbbbbbbbbbb.................
...######.......bbbbwwwwwwbbbb.......######...
..########....bbbbbbwwwwwwbbbbbb....########..
..########..bbbbbbbbbbbbbbbbbbbbbb..########..
..########bbbbbbbbbb######bbbbbbbbbb########..
..########bbbbbbbbbb#ww#w#bbbbbbbbbb########..
..########bbbbbbbbbb######bbbbbbbbbb########..
..########..bbbbbbbbbbbbbbbbbbbbbb..########..
..########....wwwwwwwwwwwwwwwwww....########..
...######......ww############ww......######...
.................w##########w.................
..............................................
"""


def image_rows():
    rows = [r for r in PICTURE.strip("\n").split("\n")]
    width = max(len(r) for r in rows)
    colour = {".": 0, "#": 1, "w": 2, "b": 3}
    return [[colour[c] for c in r.ljust(width, ".")] for r in rows], width, len(rows)


def planes(pixels, width, height, depth):
    words = (width + 15) // 16
    out = bytearray()
    for plane in range(depth):
        for y in range(height):
            row = 0
            for x in range(words * 16):
                bit = 1 if x < width and (pixels[y][x] >> plane) & 1 else 0
                row = (row << 1) | bit
            out += row.to_bytes(words * 2, "big")
    return bytes(out)


def build():
    pixels, width, height = image_rows()
    depth = 2
    gadget = struct.pack(">IhhHHHHHIIIIIHI",
                         0, 0, 0, width, height,
                         0x0004,          # GFLG_GADGIMAGE, complement when selected
                         0x0003,          # GACT_RELVERIFY | GACT_IMMEDIATE
                         0x0001,          # GTYP_BOOLGADGET
                         1, 0, 0, 0, 0,   # GadgetRender present, no SelectRender
                         0,
                         1)               # UserData: WB_DISKREVISION
    disk_object = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIIIIII",
        3, 0,                             # WBTOOL
        0,                                # no default tool
        1 if TOOL_TYPES else 0,           # tool types follow
        0x80000000, 0x80000000,           # NO_ICON_POSITION
        0, 0,                             # no drawer data, no tool window
        STACK_SIZE)
    assert len(disk_object) == 78
    image = struct.pack(">hhHHHIBBI", 0, 0, width, height, depth, 1, (1 << depth) - 1, 0, 0)
    assert len(image) == 20
    data = disk_object + image + planes(pixels, width, height, depth)
    if TOOL_TYPES:
        data += struct.pack(">I", (len(TOOL_TYPES) + 1) * 4)
        for text in TOOL_TYPES:
            raw = text.encode("latin-1") + b"\0"
            data += struct.pack(">I", len(raw)) + raw
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=os.path.join(ROOT, "build", "ppengine.info"))
    args = parser.parse_args()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(build())
    print("icon    %s" % os.path.basename(args.out))


if __name__ == "__main__":
    main()
