// Original code for RetroPixels (PixelU). Not derived from Cemu.
/* Embeds the Font Awesome TTF for the imgui overlay (Mach-O: C symbols get a
   leading underscore). */
	.section __DATA,__const
	.globl _g_fontawesome_data
	.globl _g_fontawesome_size
_g_fontawesome_data:
	.incbin "fontawesome-webfont.ttf"
Lfontawesome_end:
	.p2align 2
_g_fontawesome_size:
	.int Lfontawesome_end - _g_fontawesome_data
