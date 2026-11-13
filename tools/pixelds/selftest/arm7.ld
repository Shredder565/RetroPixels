ENTRY(_start)
MEMORY { ram : ORIGIN = 0x03800000, LENGTH = 48K }
__sp_irq = 0x0380FF80 ;
__sp_svc = 0x0380FFC0 ;
__sp_usr = 0x0380FD80 ;
SECTIONS {
  .text : { *(.crt0) *(.text*) *(.rodata*) *(.data*) . = ALIGN(4); } > ram
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > ram
}
