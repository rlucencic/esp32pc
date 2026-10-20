"""A bootable FreeDOS floppy (1.44 MB FAT12) with files, for testing programs:

  tools/fatmk.py [--2880] [--no-shell] OUT.img FD13BOOT.img FILE... ['NAME=text\\n...']

--2880 makes a 2.88 MB floppy (for bigger programs), --no-shell leaves
COMMAND.COM out (a CONFIG.SYS with SHELL= the program then).

FreeDOS's boot sector, KERNEL.SYS and COMMAND.COM come from its floppy
edition's FD13BOOT.img; then the files (into the root, contiguous), and
NAME=text ones written as given (\\n: CR LF), e.g. AUTOEXEC.BAT or a
CONFIG.SYS with SHELL=A:\\GAME.EXE to leave FreeCOM out of memory.
"""
import struct, sys, os
args = sys.argv[1:]
big = '--2880' in args
shell = '--no-shell' not in args
args = [a for a in args if a not in ('--2880', '--no-shell')]
out, boot_img, files = args[0], args[1], args[2:]
src = open(boot_img, 'rb').read()
bps, spc, res, nfat, nroot, tot, media, spf = struct.unpack_from('<HBHBHHBH', src, 11)
def src_entries(off, n):
    for i in range(n):
        e = src[off + i*32: off + i*32 + 32]
        if e[0] == 0: break
        if e[0] == 0xE5 or e[11] == 0x0F: continue
        name = e[:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else '')
        yield name, e[11], struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]
root = (res + nfat * spf) * bps
data = root + nroot * 32
def nxt(c):
    v = struct.unpack_from('<H', src, res * bps + c * 3 // 2)[0]
    return (v >> 4) if c & 1 else (v & 0xFFF)
def chain(c, size):
    b = b''
    while c < 0xFF8 and len(b) < size:
        o = data + (c - 2) * spc * bps; b += src[o:o + spc * bps]; c = nxt(c)
    return b[:size]
def find(path):
    ents = list(src_entries(root, nroot))
    for p in path.split('\\'):
        n, a, c, s = [x for x in ents if x[0] == p][0]
        if a & 0x10:
            blob = chain(c, 1 << 20)
            ents = []
            for i in range(len(blob) // 32):
                e = blob[i*32:i*32+32]
                if e[0] == 0: break
                if e[0] == 0xE5 or e[11] == 0x0F: continue
                nm = e[:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else '')
                ents.append((nm, e[11], struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]))
    return chain(c, s)
items = [('KERNEL.SYS', find('KERNEL.SYS'))]
if shell:
    items.append(('COMMAND.COM', find('FREEDOS\\BIN\\COMMAND.COM')))
for f in files:
    if '=' in f:
        name, text = f.split('=', 1); items.append((name.upper(), text.replace('\\n', '\r\n').encode()))
    else:
        items.append((os.path.basename(f).upper(), open(f, 'rb').read()))
img = bytearray(2949120 if big else 1474560)
img[:512] = src[:512]
if big:
    # the 2.88 MB geometry: 2 sectors a cluster, 240 root entries, 9 a FAT, 36 a track
    spc, nroot, tot, spf = 2, 240, 5760, 9
    struct.pack_into('<HBHBHHBH', img, 11, bps, spc, res, nfat, nroot, tot, media, spf)
    struct.pack_into('<H', img, 24, 36)
    root = (res + nfat * spf) * bps
    data = root + nroot * 32
fat = bytearray(spf * bps)
fat[0:3] = bytes([media, 0xFF, 0xFF])
def setfat(c, v):
    o = c * 3 // 2
    w = struct.unpack_from('<H', fat, o)[0]
    w = (w & 0x000F) | (v << 4) if c & 1 else (w & 0xF000) | v
    struct.pack_into('<H', fat, o, w)
cl = 2
total_clusters = (tot - data // bps) // spc
for i, (name, blob) in enumerate(items):
    base, _, ext = name.partition('.')
    n = (len(blob) + bps * spc - 1) // (bps * spc)
    first = cl if n else 0
    for k in range(n):
        setfat(cl + k, cl + k + 1 if k < n - 1 else 0xFFF)
        o = data + (cl + k - 2) * spc * bps
        img[o:o + spc * bps] = blob[k * spc * bps:(k + 1) * spc * bps].ljust(spc * bps, b'\0')
    cl += n
    assert cl < total_clusters, 'disk full'
    e = base.ljust(8)[:8].encode() + ext.ljust(3)[:3].encode() + bytes([0x20]) + bytes(14) + struct.pack('<HI', first, len(blob))
    img[root + i * 32: root + i * 32 + 32] = e
for k in range(nfat):
    img[res * bps + k * spf * bps: res * bps + (k + 1) * spf * bps] = fat
open(out, 'wb').write(img)
print(len(items), 'files,', (cl - 2) * spc * bps // 1024, 'KB used')
