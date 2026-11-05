#!/bin/sh
# Builds pixelds-selftest.nds with arm-none-eabi-gcc (no DS SDK needed).
set -e
cd "$(dirname "$0")"
CF="-O2 -marm -ffreestanding -nostdlib -fno-builtin -Wall"
arm-none-eabi-gcc -mcpu=arm946e-s $CF -T arm9.ld crt0.s arm9.c -o arm9.elf -lgcc
arm-none-eabi-gcc -mcpu=arm7tdmi $CF -T arm7.ld crt0.s arm7.c -o arm7.elf -lgcc
arm-none-eabi-objcopy -O binary arm9.elf arm9.bin
arm-none-eabi-objcopy -O binary arm7.elf arm7.bin
python3 -I mknds.py arm9.bin arm7.bin ${1:-pixelds-selftest.nds}
rm -f arm9.elf arm7.elf arm9.bin arm7.bin
