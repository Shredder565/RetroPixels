/* RetroPixels' GX2 test: draws with the Wii U's graphics library (shaders,
   vertex buffers, uniforms, a texture, depth testing, blending, a display
   list and an indexed draw) into a 320x240 colour buffer, then shows it on
   the TV. gx2_test.go checks the picture. */
#include "wiiu.h"
#include "gx2.h"
#include "gx2shaders.h"

static GX2ColorBuffer cb;
static GX2DepthBuffer db;
static GX2Texture tex;
static GX2Sampler smp;
static GX2FetchShader fs;
static GX2VertexShader vs;
static GX2PixelShader psColor, psTex;
static GX2AttribStream attribs[2];

/* position xyz, then colour (or texture coordinate) rgba */
static float tri[] = {
	-0.95f, -0.8f, 0, 1, 0, 0, 1,
	-0.4f, -0.8f, 0, 0, 1, 0, 1,
	-0.675f, 0.8f, 0, 0, 0, 1, 1,
};
static float texQuad[] = {
	0.4f, 0.8f, 0, 0, 0, 0, 1,
	0.95f, 0.8f, 0, 1, 0, 0, 1,
	0.4f, -0.8f, 0, 0, 1, 0, 1,
	0.95f, -0.8f, 0, 1, 1, 0, 1,
};
static float green[] = {
	-0.3f, 0.2f, 0, 0, 1, 0, 1,
	0.2f, 0.2f, 0, 0, 1, 0, 1,
	-0.3f, -0.3f, 0, 0, 1, 0, 1,
	0.2f, -0.3f, 0, 0, 1, 0, 1,
};
static float red[] = {
	-0.2f, 0.3f, 0.5f, 1, 0, 0, 1,
	0.3f, 0.3f, 0.5f, 1, 0, 0, 1,
	-0.2f, -0.2f, 0.5f, 1, 0, 0, 1,
	0.3f, -0.2f, 0.5f, 1, 0, 0, 1,
};
static float band[] = {
	-1, 1, 0, 1, 1, 1, 0.5f,
	1, 1, 0, 1, 1, 1, 0.5f,
	-1, 0.85f, 0, 1, 1, 1, 0.5f,
	1, 0.85f, 0, 1, 1, 1, 0.5f,
};
static float cyan[] = {
	0.5f, -0.88f, 0, 0, 1, 1, 1,
	0.9f, -0.88f, 0, 0, 1, 1, 1,
	0.5f, -0.98f, 0, 0, 1, 1, 1,
	0.9f, -0.98f, 0, 0, 1, 1, 1,
};
static uint16_t cyanIdx[] = {0, 1, 2, 2, 1, 3};

static void *alloc(uint32_t size, uint32_t align) {
	void *p = MEMAllocFromDefaultHeapEx(size, align);
	memset(p, 0, size);
	return p;
}

static void setupShaders(void) {
	vs.regs[0] = 3;          /* SQ_PGM_RESOURCES_VS: GPRs */
	vs.regs[2] = 0;          /* SPI_VS_OUT_CONFIG: one parameter */
	vs.regs[3] = 1;          /* SPI_VS_OUT_ID count */
	vs.regs[4] = 0xffffff00; /* parameter 0 is semantic 0 */
	vs.regs[16] = 2;         /* SQ_VTX_SEMANTIC count */
	vs.regs[17] = 0;         /* R1: attribute location 0 */
	vs.regs[18] = 1;         /* R2: attribute location 1 */
	vs.program = (void *)vs_main;
	vs.size = sizeof vs_main;
	GX2PixelShader *ps[2] = {&psColor, &psTex};
	const uint32_t *code[2] = {ps_color, ps_tex};
	uint32_t sizes[2] = {sizeof ps_color, sizeof ps_tex};
	for (int i = 0; i < 2; i++) {
		ps[i]->regs[0] = 2;   /* SQ_PGM_RESOURCES_PS */
		ps[i]->regs[1] = 2;   /* SQ_PGM_EXPORTS_PS */
		ps[i]->regs[2] = 1;   /* SPI_PS_IN_CONTROL_0: one input */
		ps[i]->regs[4] = 1;   /* SPI_PS_INPUT_CNTL count */
		ps[i]->regs[5] = 0;   /* input 0 is semantic 0 */
		ps[i]->regs[37] = 15; /* CB_SHADER_MASK */
		ps[i]->regs[38] = 1;  /* CB_SHADER_CONTROL */
		ps[i]->program = (void *)code[i];
		ps[i]->size = sizes[i];
	}
	attribs[0] = (GX2AttribStream){0, 0, 0, 0x811, 0, 1, 0x00010205, 3}; /* xyz1 */
	attribs[1] = (GX2AttribStream){1, 0, 12, 0x813, 0, 1, 0x00010203, 3};
	uint32_t n = GX2CalcFetchShaderSizeEx(2, 0, 0);
	void *buf = alloc(n, 256);
	GX2InitFetchShaderEx(&fs, buf, 2, attribs, 0, 0);
}

static void setupTexture(void) {
	tex.surface.dim = 1;
	tex.surface.width = 4;
	tex.surface.height = 4;
	tex.surface.depth = 1;
	tex.surface.mipLevels = 1;
	tex.surface.format = 0x1a;
	tex.surface.use = 1;
	tex.surface.tileMode = 1; /* linear */
	GX2CalcSurfaceSizeAndAlignment(&tex.surface);
	uint8_t *img = alloc(tex.surface.imageSize, tex.surface.alignment);
	tex.surface.image = img;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			uint8_t *p = img + (y * tex.surface.pitch + x) * 4;
			p[0] = 255;
			p[1] = ((x + y) & 1) ? 0 : 255; /* yellow and red squares */
			p[2] = 0;
			p[3] = 255;
		}
	tex.viewNumMips = 1;
	tex.viewNumSlices = 1;
	tex.compMap = 0x00010203;
	GX2InitTextureRegs(&tex);
	DCFlushRange(img, tex.surface.imageSize);
	GX2Invalidate(0x4, img, tex.surface.imageSize);
	GX2InitSampler(&smp, 2, 0); /* clamp, point */
}

static void draw(float *v, uint32_t bytes, int mode, uint32_t count) {
	GX2SetAttribBuffer(0, bytes, 28, v);
	GX2DrawEx(mode, count, 0, 1);
}

int main(int argc, char **argv) {
	ProcUIInit(0);
	GX2Init(0);
	uint32_t tvSize;
	BOOL scale;
	GX2CalcTVSize(3, 0x1a, 2, &tvSize, &scale);
	void *tvBuf = alloc(tvSize, 0x1000);
	GX2SetTVBuffer(tvBuf, tvSize, 3, 0x1a, 2);
	GX2SetTVEnable(1);

	GX2ContextState *ctx = alloc(sizeof(GX2ContextState), 256);
	GX2SetupContextStateEx(ctx, 0);
	GX2SetContextState(ctx);

	cb.surface.dim = 1;
	cb.surface.width = 320;
	cb.surface.height = 240;
	cb.surface.depth = 1;
	cb.surface.mipLevels = 1;
	cb.surface.format = 0x1a;
	cb.surface.use = 2;
	cb.viewNumSlices = 1;
	GX2CalcSurfaceSizeAndAlignment(&cb.surface);
	cb.surface.image = alloc(cb.surface.imageSize, cb.surface.alignment);
	GX2InitColorBufferRegs(&cb);

	db.surface.dim = 1;
	db.surface.width = 320;
	db.surface.height = 240;
	db.surface.depth = 1;
	db.surface.mipLevels = 1;
	db.surface.format = 0x011;
	db.surface.use = 4;
	db.viewNumSlices = 1;
	GX2CalcSurfaceSizeAndAlignment(&db.surface);
	db.surface.image = alloc(db.surface.imageSize, db.surface.alignment);
	GX2InitDepthBufferRegs(&db);

	setupShaders();
	setupTexture();
	void *dl = alloc(0x1000, 0x20);
	static const float ident[4] = {1, 1, 0, 0};
	static const float small[4] = {0.25f, 0.25f, 0.17f, -0.6f};
	static const float white[4] = {1, 1, 1, 1};

	for (int frame = 0; frame < 3; frame++) {
		GX2ClearColor(&cb, 0.1f, 0.2f, 0.4f, 1);
		GX2ClearDepthStencilEx(&db, 1, 0, 3);
		GX2SetContextState(ctx);
		GX2SetColorBuffer(&cb, 0);
		GX2SetDepthBuffer(&db);
		GX2SetViewport(0, 0, 320, 240, 0, 1);
		GX2SetScissor(0, 0, 320, 240);
		GX2SetFetchShader(&fs);
		GX2SetVertexShader(&vs);
		GX2SetPixelShader(&psColor);
		GX2SetVertexUniformReg(0, 4, ident);
		GX2SetDepthOnlyControl(0, 0, 7);
		GX2SetCullOnlyControl(0, 0, 0);

		draw(tri, sizeof tri, 4, 3);

		GX2SetPixelShader(&psTex);
		GX2SetPixelTexture(&tex, 0);
		GX2SetPixelSampler(&smp, 0);
		GX2SetPixelUniformReg(0, 4, white);
		draw(texQuad, sizeof texQuad, 6, 4);

		GX2SetPixelShader(&psColor);
		GX2SetDepthOnlyControl(1, 1, 1); /* less */
		draw(green, sizeof green, 6, 4);
		draw(red, sizeof red, 6, 4);

		GX2SetDepthOnlyControl(0, 0, 7);
		GX2SetColorControl(0xcc, 1, 0, 1);
		GX2SetBlendControl(0, 4, 5, 0, 0, 0, 0, 0);
		draw(band, sizeof band, 6, 4);
		GX2SetColorControl(0xcc, 0, 0, 1);

		GX2BeginDisplayList(dl, 0x1000);
		GX2SetVertexUniformReg(0, 4, small);
		draw(tri, sizeof tri, 4, 3);
		GX2SetVertexUniformReg(0, 4, ident);
		uint32_t n = GX2EndDisplayList(dl);
		GX2CallDisplayList(dl, n);

		GX2SetAttribBuffer(0, sizeof cyan, 28, cyan);
		GX2DrawIndexedEx(4, 6, 4, cyanIdx, 0, 1);

		GX2CopyColorBufferToScanBuffer(&cb, 1);
		GX2SwapScanBuffers();
		GX2Flush();
		GX2WaitForVsync();
	}
	OSReport("gx2 test done\n");
	GX2Shutdown();
	return 0;
}
