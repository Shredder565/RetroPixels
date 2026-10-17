/* CPU test: the same program is built for the Wii U and for the host, and the
   two must print the same thing. */
#ifdef NATIVE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define OSReport printf
#define FMAF __builtin_fmaf
#define FMA __builtin_fma
#else
#include "wiiu.h"
static float FMAF(float a, float b, float c) { float r; __asm__("fmadds %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(c)); return r; }
static double FMA(double a, double b, double c) { double r; __asm__("fmadd %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(c)); return r; }
#endif

static uint32_t seed = 12345;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static uint32_t h = 0;
static int tracing;
static void mix_(uint32_t v, int line) {
#ifdef TRACE
	if (tracing) OSReport("%d %08x\n", line, v);
#endif
 h = (h ^ v) * 16777619u + 0x9e3779b9u; h = (h << 7) | (h >> 25); }
#define mix(v) mix_(v, __LINE__)
static void mix64(uint64_t v) { mix((uint32_t)v); mix((uint32_t)(v >> 32)); }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* values chosen to hit edge cases */
static const uint32_t edges[] = {0, 1, 2, 3, 7, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
	0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff, 0x12345678, 0xdeadbeef};
#define NE (sizeof edges / sizeof edges[0])

__attribute__((noinline)) static uint32_t idx(uint32_t i) { return i; }

static void ints(void) {
	for (int i = 0; i < 4000; i++) {
		uint32_t a = i < (int)(NE * NE) ? edges[i % NE] : rnd();
		uint32_t b = i < (int)(NE * NE) ? edges[i / NE % NE] : rnd();
		int32_t sa = (int32_t)a, sb = (int32_t)b;
		mix(a + b); mix(a - b); mix(a * b); mix(a & b); mix(a | b); mix(a ^ b); mix(~(a & b)); mix(~(a | b));
		mix(a & ~b); mix(a | ~b);
		mix((uint32_t)(((uint64_t)a * b) >> 32));
		mix((uint32_t)(((int64_t)sa * sb) >> 32));
		if (b) { mix(a / b); mix(a % b); }
		if (sb && !(sa == (int32_t)0x80000000 && sb == -1)) { mix((uint32_t)(sa / sb)); mix((uint32_t)(sa % sb)); }
		uint32_t s = idx(b & 31);
		mix(a << s); mix(a >> s); mix((uint32_t)(sa >> s));
		mix((a << s) | (a >> ((32 - s) & 31)));
		mix(a ? (uint32_t)__builtin_clz(a) : 32);
		mix(__builtin_bswap32(a));
		mix((uint32_t)(int32_t)(int16_t)a); mix((uint32_t)(int32_t)(int8_t)a);
		mix(sa < sb); mix(a < b); mix(sa == sb);
		mix((a >> 3) & 0x1f); mix((a & 0xff00ff00) | (b & 0x00ff00ff));
		uint64_t x = ((uint64_t)a << 32) | b, y = ((uint64_t)b << 32) | rnd();
		mix64(x + y); mix64(x - y); mix64(x * y); mix64(x >> (s + 1)); mix64(x << (s + 3)); mix64((uint64_t)((int64_t)x >> (s + 7)));
		if (y) { mix64(x / y); mix64(x % y); }
		if ((uint32_t)y) { mix64(x / (uint32_t)y); mix64((uint64_t)((int64_t)x / (int32_t)(uint32_t)y)); }
		mix((int64_t)x < (int64_t)y); mix(x < y);
	}
	OSReport("ints %08x\n", h);
}

static const float fedges[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.5f, -2.5f, 3.0f, 1e-30f, 1e30f, -7.25f,
	123456.78f, 0.1f, 1.0f / 3.0f, 16777216.0f, 2147483520.0f, -2147483648.0f, 4294967040.0f};
#define NF (sizeof fedges / sizeof fedges[0])

static void floats(void) {
	h = 0;
	tracing = 1;
	for (int i = 0; i < 3000; i++) {
		float a, b;
		if (i < (int)(NF * NF)) { a = fedges[i % NF]; b = fedges[i / NF % NF]; }
		else { a = (float)(int32_t)rnd() / (float)(rnd() | 1); b = (float)(rnd() & 0xffff) / 256.0f - 100.0f; }
		double da = (double)a * 1.0000001, db = (double)b / 3.0;
		mix(fbits(a + b)); mix(fbits(a - b)); mix(fbits(a * b));
		if (b != 0) mix(fbits(a / b));
		mix64(dbits(da + db)); mix64(dbits(da - db)); mix64(dbits(da * db));
		if (db != 0) mix64(dbits(da / db));
		mix(fbits(FMAF(a, b, 1.5f))); mix64(dbits(FMA(da, db, -2.0)));
		mix(fbits((float)da)); mix64(dbits((double)a));
		mix(fbits(__builtin_fabsf(a))); mix(fbits(-a)); mix64(dbits(__builtin_copysign(da, db)));
		mix(a < b); mix(a == b); mix(da >= db);
		if (a > -2147483648.0f && a < 2147483520.0f) mix((uint32_t)(int32_t)a);
		if (a >= 0 && a < 4294967040.0f) mix((uint32_t)a);
		if (da > -2147483648.0 && da < 2147483647.0) mix((uint32_t)(int32_t)da);
		int32_t ii = (int32_t)rnd();
		mix(fbits((float)ii)); mix64(dbits((double)ii)); mix64(dbits((double)(uint32_t)ii));
		mix(fbits((float)(uint32_t)ii));
		float m = a > b ? a : b;
		mix(fbits(m));
	}
	tracing = 0;
	OSReport("floats %08x\n", h);
}

struct S { int a; short b; char c; double d; float e[3]; };
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int op(int k, int v) {
	switch (k) {
	case 0: return v + 1; case 1: return v * 3; case 2: return v - 7; case 3: return v ^ 0x55;
	case 4: return v << 2; case 5: return v >> 1; case 6: return ~v; case 7: return -v;
	case 8: return v / 3; case 9: return v % 11;
	}
	return 0;
}
typedef int (*fn)(int);
static int f1(int x) { return x + 100; }
static int f2(int x) { return x * 2; }

static void misc(void) {
	h = 0;
	mix(fib(20));
	int v = 1;
	for (int i = 0; i < 1000; i++) { v = op(rnd() % 10, v); mix((uint32_t)v); }
	fn fs[2] = {f1, f2};
	for (int i = 0; i < 100; i++) mix((uint32_t)fs[i & 1](i));
	struct S s1 = {1, 2, 3, 4.5, {1, 2, 3}}, s2;
	memcpy(&s2, &s1, sizeof s1);
	s2.e[1] += 0.5f;
	mix(s2.a + s2.b + s2.c); mix64(dbits(s2.d)); mix(fbits(s2.e[1]));
	char str[64];
	for (int i = 0; i < 63; i++) str[i] = 'a' + i % 26;
	str[63] = 0;
	uint32_t len = 0;
	while (str[len]) len++;
	mix(len);
	unsigned char bytes[256];
	for (int i = 0; i < 256; i++) bytes[i] = (unsigned char)(i * 7);
	memmove(bytes + 3, bytes, 200);
	for (int i = 0; i < 256; i += 4) mix(bytes[i] | bytes[i + 1] << 8 | bytes[i + 2] << 16 | (uint32_t)bytes[i + 3] << 24);
	uint16_t hw[16];
	for (int i = 0; i < 16; i++) hw[i] = (uint16_t)(i * 4097);
	for (int i = 0; i < 16; i++) mix((int16_t)hw[i]);
	OSReport("misc %08x\n", h);
}

int main(int argc, char **argv) {
	ints();
	floats();
	misc();
	return 0;
}
