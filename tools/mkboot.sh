#!/bin/sh
# tools/mkboot.sh IN.S OUT.img [KB]: a boot floppy (1.44 MB unless KB) from
# a 16-bit program in GNU assembler syntax (AT&T, through the C
# preprocessor: components/ibmpc/host/tests/boot.h has the loader). Needs
# LLVM's clang and llvm-objcopy (Homebrew's llvm: set LLVM to its bin
# folder if it isn't /opt/homebrew/opt/llvm/bin).
set -e
L=${LLVM:-/opt/homebrew/opt/llvm/bin}
T=$(mktemp -d)
"$L/clang" -target i386-none-elf -I"$(dirname "$1")" -c "$1" -o "$T/p.o"
"$L/llvm-objcopy" -O binary -j .text "$T/p.o" "$T/p.bin"
python3 -I -c "
import sys
b = open(sys.argv[1], 'rb').read()
size = int(sys.argv[3]) * 1024
assert len(b) <= size, len(b)
open(sys.argv[2], 'wb').write(b + bytes(size - len(b)))" "$T/p.bin" "$2" "${3:-1440}"
rm -r "$T"
