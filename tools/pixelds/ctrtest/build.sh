#!/bin/sh
# Builds cores/pixelds/ctr/testdata/ctrtest.3dsx with Ubuntu's gcc-arm-none-eabi.
set -e
cd "$(dirname "$0")"
F="-mcpu=mpcore -mfloat-abi=hard -mfpu=vfp -marm -O2 -ffreestanding -nostdlib -fno-builtin"
arm-none-eabi-gcc $F -c crt0.s -o crt0.o
arm-none-eabi-gcc $F -c main.c -o main.o
arm-none-eabi-gcc $F -c fsq.c -o fsq.o
arm-none-eabi-gcc $F -T ctr.ld crt0.o main.o fsq.o -lgcc -o ctrtest.elf
arm-none-eabi-objcopy -O binary -j .text ctrtest.elf ctrtest.bin
BSS=$(arm-none-eabi-size -A ctrtest.elf | awk '$1==".bss"{print $2}')
mkdir -p ../../../cores/pixelds/ctr/testdata
python3 mk3dsx.py ctrtest.bin "$BSS" ../../../cores/pixelds/ctr/testdata/ctrtest.3dsx
rm -f *.o ctrtest.elf ctrtest.bin
