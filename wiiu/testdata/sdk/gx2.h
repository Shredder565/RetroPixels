/* The parts of gx2 (the Wii U's graphics library) RetroPixels' test programs
   use. Written for RetroPixels from the library's public interface; the
   structures have the sizes and field offsets programs rely on. */
#pragma once
#include "wiiu.h"

typedef struct GX2Surface {
	uint32_t dim, width, height, depth, mipLevels, format, aa, use;
	uint32_t imageSize;
	void *image;
	uint32_t mipmapSize;
	void *mipmaps;
	uint32_t tileMode, swizzle, alignment, pitch;
	uint32_t mipLevelOffset[13];
} GX2Surface; /* 0x74 */

typedef struct GX2ColorBuffer {
	GX2Surface surface;
	uint32_t viewMip, viewFirstSlice, viewNumSlices;
	void *aaBuffer;
	uint32_t aaSize;
	uint32_t regs[5];
} GX2ColorBuffer; /* 0x9c */

typedef struct GX2DepthBuffer {
	GX2Surface surface;
	uint32_t viewMip, viewFirstSlice, viewNumSlices;
	void *hiZPtr;
	uint32_t hiZSize;
	float depthClear;
	uint32_t stencilClear;
	uint32_t regs[7];
} GX2DepthBuffer; /* 0xac */

typedef struct GX2Texture {
	GX2Surface surface;
	uint32_t viewFirstMip, viewNumMips, viewFirstSlice, viewNumSlices;
	uint32_t compMap;
	uint32_t regs[5];
} GX2Texture; /* 0x9c */

typedef struct GX2Sampler { uint32_t regs[3]; } GX2Sampler;

typedef struct GX2AttribStream {
	uint32_t location, buffer, offset, format, type, aluDivisor, mask, endianSwap;
} GX2AttribStream; /* 0x20 */

typedef struct GX2FetchShader {
	uint32_t type;
	uint32_t regs[1];
	uint32_t size;
	void *program;
	uint32_t attribCount, numDivisors, divisors[2];
} GX2FetchShader; /* 0x20 */

typedef struct GX2LoopVar { uint32_t offset, value; } GX2LoopVar;

typedef struct GX2VertexShader {
	uint32_t regs[52];
	uint32_t size;
	void *program;
	uint32_t mode;
	uint32_t uniformBlockCount; void *uniformBlocks;
	uint32_t uniformVarCount; void *uniformVars;
	uint32_t initialValueCount; void *initialValues;
	uint32_t loopVarCount; GX2LoopVar *loopVars;
	uint32_t samplerVarCount; void *samplerVars;
	uint32_t attribVarCount; void *attribVars;
	uint32_t ringItemsize, hasStreamOut, streamOutStride[4];
	uint32_t gx2r[4];
} GX2VertexShader;

typedef struct GX2PixelShader {
	uint32_t regs[41];
	uint32_t size;
	void *program;
	uint32_t mode;
	uint32_t uniformBlockCount; void *uniformBlocks;
	uint32_t uniformVarCount; void *uniformVars;
	uint32_t initialValueCount; void *initialValues;
	uint32_t loopVarCount; GX2LoopVar *loopVars;
	uint32_t samplerVarCount; void *samplerVars;
	uint32_t gx2r[4];
} GX2PixelShader;

typedef struct GX2ContextState { uint8_t data[0xa100]; } __attribute__((aligned(256))) GX2ContextState;

void GX2Init(void *attribs);
void GX2Shutdown(void);
void GX2CalcTVSize(int mode, int format, int buffering, uint32_t *size, BOOL *scaleNeeded);
void GX2SetTVBuffer(void *buf, uint32_t size, int mode, int format, int buffering);
void GX2SetTVEnable(BOOL on);
void GX2CalcSurfaceSizeAndAlignment(GX2Surface *s);
void GX2InitColorBufferRegs(GX2ColorBuffer *cb);
void GX2InitDepthBufferRegs(GX2DepthBuffer *db);
void GX2InitTextureRegs(GX2Texture *t);
void GX2SetupContextStateEx(GX2ContextState *s, BOOL profiling);
void GX2SetContextState(GX2ContextState *s);
void GX2SetColorBuffer(GX2ColorBuffer *cb, int target);
void GX2SetDepthBuffer(GX2DepthBuffer *db);
void GX2ClearColor(GX2ColorBuffer *cb, float r, float g, float b, float a);
void GX2ClearDepthStencilEx(GX2DepthBuffer *db, float depth, uint8_t stencil, int flags);
void GX2SetViewport(float x, float y, float w, float h, float n, float f);
void GX2SetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
uint32_t GX2CalcFetchShaderSizeEx(uint32_t count, int type, int tess);
void GX2InitFetchShaderEx(GX2FetchShader *fs, void *buf, uint32_t count, const GX2AttribStream *attribs, int type, int tess);
void GX2SetFetchShader(const GX2FetchShader *fs);
void GX2SetVertexShader(const GX2VertexShader *vs);
void GX2SetPixelShader(const GX2PixelShader *ps);
void GX2SetAttribBuffer(uint32_t index, uint32_t size, uint32_t stride, const void *buf);
void GX2SetVertexUniformReg(uint32_t offset, uint32_t count, const void *values);
void GX2SetPixelUniformReg(uint32_t offset, uint32_t count, const void *values);
void GX2SetPixelTexture(const GX2Texture *t, uint32_t unit);
void GX2InitSampler(GX2Sampler *s, int clamp, int filter);
void GX2SetPixelSampler(const GX2Sampler *s, uint32_t unit);
void GX2DrawEx(int mode, uint32_t count, uint32_t first, uint32_t instances);
void GX2DrawIndexedEx(int mode, uint32_t count, int indexType, const void *indices, uint32_t baseVertex, uint32_t instances);
void GX2SetDepthOnlyControl(BOOL test, BOOL write, int func);
void GX2SetColorControl(int logicOp, uint8_t blendMask, BOOL multiWrite, BOOL colorWrite);
void GX2SetBlendControl(int target, int colorSrc, int colorDst, int colorComb, BOOL useAlpha, int alphaSrc, int alphaDst, int alphaComb);
void GX2SetCullOnlyControl(int frontFace, BOOL cullFront, BOOL cullBack);
void GX2BeginDisplayList(void *buf, uint32_t size);
uint32_t GX2EndDisplayList(void *buf);
void GX2CallDisplayList(const void *buf, uint32_t size);
void GX2Invalidate(int mode, void *buf, uint32_t size);
void GX2CopyColorBufferToScanBuffer(const GX2ColorBuffer *cb, int target);
void GX2SwapScanBuffers(void);
void GX2Flush(void);
void GX2WaitForVsync(void);
void GX2DrawDone(void);
