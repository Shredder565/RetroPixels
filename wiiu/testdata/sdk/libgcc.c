/* the 64-bit arithmetic helpers a 32-bit PowerPC compiler calls */
typedef unsigned long long u64;
typedef long long s64;
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
	u64 q = 0, r = 0;
	for (int i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d) { r -= d; q |= 1ULL << i; }
	}
	if (rem) *rem = r;
	return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }
s64 __divdi3(s64 a, s64 b) {
	int neg = (a < 0) != (b < 0);
	u64 q = __udivdi3(a < 0 ? -(u64)a : a, b < 0 ? -(u64)b : b);
	return neg ? -(s64)q : (s64)q;
}
s64 __moddi3(s64 a, s64 b) {
	u64 r = __umoddi3(a < 0 ? -(u64)a : a, b < 0 ? -(u64)b : b);
	return a < 0 ? -(s64)r : (s64)r;
}
typedef unsigned int u32;
typedef union { u64 v; struct { u32 hi, lo; } w; } dw;
u64 __lshrdi3(u64 a, int b) {
	dw x = {a}, r;
	if (b == 0) return a;
	if (b >= 32) { r.w.hi = 0; r.w.lo = b >= 64 ? 0 : x.w.hi >> (b - 32); }
	else { r.w.hi = x.w.hi >> b; r.w.lo = (x.w.lo >> b) | (x.w.hi << (32 - b)); }
	return r.v;
}
s64 __ashrdi3(s64 a, int b) {
	dw x = {(u64)a}, r;
	if (b == 0) return a;
	if (b >= 32) { r.w.hi = (u32)((int)x.w.hi >> 31); r.w.lo = (u32)((int)x.w.hi >> (b >= 63 ? 31 : b - 32)); }
	else { r.w.hi = (u32)((int)x.w.hi >> b); r.w.lo = (x.w.lo >> b) | (x.w.hi << (32 - b)); }
	return (s64)r.v;
}
s64 __ashldi3(s64 a, int b) {
	dw x = {(u64)a}, r;
	if (b == 0) return a;
	if (b >= 32) { r.w.lo = 0; r.w.hi = b >= 64 ? 0 : x.w.lo << (b - 32); }
	else { r.w.lo = x.w.lo << b; r.w.hi = (x.w.hi << b) | (x.w.lo >> (32 - b)); }
	return (s64)r.v;
}
