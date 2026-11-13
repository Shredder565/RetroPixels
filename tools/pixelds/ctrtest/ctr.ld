ENTRY(_start)
SECTIONS
{
	. = 0x00100000;
	.text : { *(.text.start) . = 0x100; *(.results) . = 0x200; *(.text*) *(.rodata*) *(.data*) }
	. = ALIGN(4);
	__bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(4);
	__bss_end = .;
	/DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
