"""The adapters' character sets, from the Ultimate Oldschool PC Font Pack
(int10h.org, CC BY-SA 4.0) .FON files, into components/ibmpc/font.c.

    python3 -I tools/fon2c.py <folder with the Bm437_IBM_*.FON files> > components/ibmpc/font.c

A .FON is a Windows NE executable with FONT resources (the FNT format);
glyphs are stored a byte column at a time. Here each glyph becomes its rows,
8 pixels a byte (bit 7 the left one); the MDA's 9th column is the hardware's
rule (the 8th repeated for C0-DF), checked against the file.
"""
import os
import struct
import sys

FONTS = [
    # (file, C name, height, first char, chars)
    ("Bm437_IBM_BIOS.FON", "pc_font_bios8", 8, 0, 128),     # the BIOS ROM's 8x8 (F000:FA6E)
    ("Bm437_IBM_CGA.FON", "pc_font_cga8", 8, 0, 256),       # the CGA's character ROM
    ("Bm437_IBM_MDA.FON", "pc_font_mda14", 14, 0, 256),     # the MDA's (9 x 14 cells)
    ("Bm437_IBM_EGA_8x14.FON", "pc_font_ega14", 14, 0, 256),
    ("Bm437_IBM_VGA_8x16.FON", "pc_font_vga16", 16, 0, 256),
]


def fnt_resources(data):
    ne = struct.unpack_from("<H", data, 0x3C)[0]
    assert data[ne:ne + 2] == b"NE", "not an NE file"
    rsrc = ne + struct.unpack_from("<H", data, ne + 0x24)[0]
    shift = struct.unpack_from("<H", data, rsrc)[0]
    p = rsrc + 2
    while True:
        type_id, count = struct.unpack_from("<HH", data, p)
        if type_id == 0:
            break
        p += 8
        for _ in range(count):
            off, length = struct.unpack_from("<HH", data, p)
            if type_id == 0x8008:   # RT_FONT
                yield data[off << shift:(off << shift) + (length << shift)]
            p += 12


def glyphs(fnt):
    version = struct.unpack_from("<H", fnt, 0)[0]
    height = struct.unpack_from("<H", fnt, 88)[0]
    first, last = fnt[95], fnt[96]
    table = 148 if version == 0x300 else 118
    entry = 6 if version == 0x300 else 4
    out = {}
    for i in range(last - first + 1):
        if version == 0x300:
            width, off = struct.unpack_from("<HI", fnt, table + i * entry)
        else:
            width, off = struct.unpack_from("<HH", fnt, table + i * entry)
        cols = (width + 7) // 8
        rows = []
        for y in range(height):
            bits = 0
            for col in range(cols):
                bits = bits << 8 | fnt[off + col * height + y]
            rows.append((bits, width))
        out[first + i] = rows
    return height, out


def main():
    folder = sys.argv[1]
    print("/*")
    print(" * The adapters' character sets: IBM's, as the Ultimate Oldschool PC Font")
    print(" * Pack (VileR, int10h.org) reproduces them, licensed CC BY-SA 4.0")
    print(" * (creativecommons.org/licenses/by-sa/4.0). Made by tools/fon2c.py.")
    print(" * Each glyph its rows top to bottom, bit 7 the leftmost pixel.")
    print(" */")
    print("#include <stdint.h>")
    print('#include "pc_internal.h"')
    for name, cname, height, first, count in FONTS:
        with open(os.path.join(folder, name), "rb") as f:
            data = f.read()
        fnt = next(fnt_resources(data))
        h, g = glyphs(fnt)
        assert h == height, (name, h)
        print()
        print("/* %s */" % name)
        print("const uint8_t %s[%d] = {" % (cname, count * height))
        for c in range(first, first + count):
            rows = g.get(c, [(0, 8)] * height)
            values = []
            for bits, width in rows:
                if width == 9:
                    ninth = (bits >> 7) & 1     # the second byte column's first pixel
                    bits >>= 8
                    expect = (bits & 1) if 0xC0 <= c <= 0xDF else 0
                    assert ninth == expect, "%s: char %02X's 9th column" % (name, c)
                values.append(bits & 0xFF)
            print("    " + ", ".join("0x%02X" % v for v in values) + ",   /* %02X */" % c)
        print("};")


main()
