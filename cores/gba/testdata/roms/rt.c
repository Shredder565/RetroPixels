// Division helpers the compiler calls (original code).
typedef unsigned int u32; typedef unsigned long long u64;
static u32 udiv(u32 n, u32 d, u32 *r) {
  u32 q = 0, rem = 0;
  if (!d) { *r = n; return 0xffffffff; }
  for (int i = 31; i >= 0; i--) { rem = (rem << 1) | ((n >> i) & 1); if (rem >= d) { rem -= d; q |= 1u << i; } }
  *r = rem; return q;
}
u32 __aeabi_uidiv(u32 n, u32 d) { u32 r; return udiv(n, d, &r); }
u64 __aeabi_uidivmod(u32 n, u32 d) { u32 r; u32 q = udiv(n, d, &r); return q | (u64)r << 32; }
int __aeabi_idiv(int n, int d) { u32 r; u32 q = udiv(n < 0 ? -n : n, d < 0 ? -d : d, &r); return (n < 0) != (d < 0) ? -(int)q : (int)q; }
u64 __aeabi_idivmod(int n, int d) { u32 r; u32 q = udiv(n < 0 ? -n : n, d < 0 ? -d : d, &r); int qq = (n < 0) != (d < 0) ? -(int)q : (int)q; int rr = n < 0 ? -(int)r : (int)r; return (u32)qq | (u64)(u32)rr << 32; }
void *memset(void *p, int v, unsigned n) { unsigned char *b = p; while (n--) *b++ = v; return p; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
