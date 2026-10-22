// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// AES-128 helpers. Keys and IVs are 16 bytes. The CBC/CTR lengths are in
// bytes and are expected to be multiples of 16. output may equal input.

// Picks the AES-NI implementation when the CPU supports it.
void AES128_init();

extern void (*AES128_ECB_encrypt)(uint8* input, const uint8* key, uint8* output);
void AES128_ECB_decrypt(uint8* input, const uint8* key, uint8* output);

void AES128_CBC_encrypt(uint8* output, uint8* input, uint32 length, const uint8* key, const uint8* iv);
// iv may be null (all-zero IV)
extern void (*AES128_CBC_decrypt)(uint8* output, uint8* input, uint32 length, const uint8* key, const uint8* iv);
// decrypts the whole 16-byte blocks of input and stores the last cipher block in iv, for chunked decryption
void AES128_CBC_decrypt_updateIV(uint8* output, uint8* input, uint32 length, const uint8* key, uint8* iv);

// CTR mode, encrypts and decrypts in place; nonceIv is the 128-bit big-endian counter and is advanced
void AES128CTR_transform(uint8* data, sint32 length, uint8* key, uint8* nonceIv);
