// Original code for RetroPixels (PixelU). Not derived from Cemu.
/* Embeds the Font Awesome TTF for the imgui overlay (ELF targets).
   g_fontawesome_data: the file's bytes, g_fontawesome_size: their count (int). */
	.section .rodata,"a",%progbits
	.globl g_fontawesome_data
	.globl g_fontawesome_size
g_fontawesome_data:
	.incbin "fontawesome-webfont.ttf"
fontawesome_end:
	.balign 4
g_fontawesome_size:
	.int fontawesome_end - g_fontawesome_data

	.section .note.GNU-stack,"",%progbits
