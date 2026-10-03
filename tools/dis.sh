#!/bin/sh
# usage: tools/dis.sh <start rva hex> <end rva hex>   (disassembles backup/text.bin, the exe's first .text section)
PATH="/c/msys64/ucrt64/bin:$PATH"
objdump -D -b binary -mi386:x86-64 -M intel --adjust-vma=0x140001000 --start-address=$((0x140000000 + 0x$1)) --stop-address=$((0x140000000 + 0x$2)) backup/text.bin | tail -n +8 | sed -E 's/^ *140*//; s/\t[0-9a-f ]+\t/\t/'
