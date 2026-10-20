#!/bin/sh
# tools/bench.sh OUT.img: the speed benchmark (tools/bench.s) as a 1.44 MB
# boot floppy. Needs LLVM's clang and llvm-objcopy (Homebrew's llvm: set
# LLVM to its bin folder if it isn't /opt/homebrew/opt/llvm/bin).
set -e
L=${LLVM:-/opt/homebrew/opt/llvm/bin}
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
"$L/clang" -target i386-none-elf -c "$D/bench.s" -o "$T/bench.o"
"$L/llvm-objcopy" -O binary -j .text "$T/bench.o" "$T/bench.bin"
python3 -I -c "
import sys
b = open(sys.argv[1], 'rb').read()
assert len(b) == 512, len(b)
open(sys.argv[2], 'wb').write(b + bytes(1474560 - 512))" "$T/bench.bin" "${1:-bench.img}"
rm -r "$T"
