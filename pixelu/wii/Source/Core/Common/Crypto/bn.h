// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

// Small modular arithmetic on n-byte big-endian unsigned numbers, as used by the ECC code.
// The modulus N is expected to have its top bit clear relative to n bytes of storage; inputs
// should be below N (results for larger inputs are well defined but not reduced).

// memcmp-style comparison of a and b.
int bn_compare(const u8* a, const u8* b, size_t n);
// a -= N (modulo 2^(8n)).
void bn_sub_modulus(u8* a, const u8* N, size_t n);
// d = a + b (mod N). d may alias a or b.
void bn_add(u8* d, const u8* a, const u8* b, const u8* N, size_t n);
// d = a * b (mod N).
void bn_mul(u8* d, const u8* a, const u8* b, const u8* N, size_t n);
// d = a^-1 (mod N), computed as a^(N-2); N must be prime.
void bn_inv(u8* d, const u8* a, const u8* N, size_t n);
// d = a^e (mod N), with e an en-byte big-endian exponent.
void bn_exp(u8* d, const u8* a, const u8* N, size_t n, const u8* e, size_t en);
