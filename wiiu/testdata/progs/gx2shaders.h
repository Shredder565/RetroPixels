/* Made by gx2shaders_test.go: R700 shader code for gx2test.c, as the GPU
   reads it (little-endian words). */
static const uint32_t vs_main[] __attribute__((aligned(256))) = {
	0x00000000, 0x00008089, 0x04000000, 0x000004a0, 0x3ca00000, 0x88060094, 
	0x00400100, 0x88062094, 0x01002000, 0x00092200, 0x0104a080, 0x000d2220, 
};
static const uint32_t ps_color[] __attribute__((aligned(256))) = {
	0x00000000, 0x88062094, 
};
static const uint32_t ps_tex[] __attribute__((aligned(256))) = {
	0x04000000, 0x00008080, 0x06000000, 0x00000ca0, 0x00800000, 0x88062094, 
	0x00000000, 0x00000000, 0x10000000, 0x01100df0, 0x00008090, 0x00000000, 
	0x01002000, 0x10012000, 0x0104a000, 0x10012020, 0x01082001, 0x10012040, 
	0x010ca081, 0x10012060, 
};
