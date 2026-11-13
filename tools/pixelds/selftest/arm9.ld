ENTRY(_start)
MEMORY { ram : ORIGIN = 0x02000000, LENGTH = 3M }
__sp_irq = 0x02FFFC00 - 0x400 ;
__sp_svc = 0x02FFFC00 - 0x200 ;
__sp_usr = 0x02FFFC00 - 0x800 ;
SECTIONS {
  .text : { *(.crt0) *(.text*) *(.rodata*) *(.data*) . = ALIGN(4); } > ram
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > ram
}
