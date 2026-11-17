// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/JpegEncoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <thread>

namespace VideoCommon
{
namespace
{
// Position in the 8x8 block of the n-th coefficient in zig-zag order.
constexpr std::array<u8, 64> kZigZag = [] {
  std::array<u8, 64> order{};
  int x = 0, y = 0;
  for (int n = 0; n < 64; ++n)
  {
    order[n] = static_cast<u8>(y * 8 + x);
    if ((x + y) % 2 == 0)
    {
      if (x == 7)
        ++y;
      else if (y == 0)
        ++x;
      else
        --y, ++x;
    }
    else
    {
      if (y == 7)
        ++x;
      else if (x == 0)
        ++y;
      else
        ++y, --x;
    }
  }
  return order;
}();

// T.81 Annex K example quantization tables, in natural (row-major) order.
constexpr u8 kLumaQuant[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr u8 kChromaQuant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// T.81 Annex K example Huffman tables: the number of codes of each length 1-16, then the
// symbols in order of increasing code length.
struct HuffmanSpec
{
  u8 counts[16];
  std::vector<u8> symbols;
};
const HuffmanSpec kLumaDC = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kChromaDC = {{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
                               {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kLumaAC = {
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
     0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
     0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
     0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
     0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
     0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
     0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
     0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
     0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
     0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
     0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};
const HuffmanSpec kChromaAC = {
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
     0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
     0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
     0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
     0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
     0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
     0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
     0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
     0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
     0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
     0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};

// Code and length for each symbol, assigned as in T.81 Annex C: codes of each length count up
// from the previous length's last code, shifted left by one.
struct HuffmanCodes
{
  u16 code[256]{};
  u8 length[256]{};

  explicit HuffmanCodes(const HuffmanSpec& spec)
  {
    u32 next = 0;
    size_t symbol = 0;
    for (int len = 1; len <= 16; ++len)
    {
      for (int i = 0; i < spec.counts[len - 1]; ++i)
      {
        const u8 s = spec.symbols[symbol++];
        code[s] = static_cast<u16>(next++);
        length[s] = static_cast<u8>(len);
      }
      next <<= 1;
    }
  }
};
const HuffmanCodes kLumaDCCodes(kLumaDC), kChromaDCCodes(kChromaDC);
const HuffmanCodes kLumaACCodes(kLumaAC), kChromaACCodes(kChromaAC);

// cos((2x + 1) u pi / 16) with the C(u) normalisation of the DCT, indexed [u][x].
const std::array<std::array<float, 8>, 8> kCosine = [] {
  std::array<std::array<float, 8>, 8> c{};
  for (int u = 0; u < 8; ++u)
  {
    const double cu = u == 0 ? std::numbers::sqrt2 / 2 : 1.0;
    for (int x = 0; x < 8; ++x)
      c[u][x] = static_cast<float>(cu / 2 * std::cos((2 * x + 1) * u * std::numbers::pi / 16));
  }
  return c;
}();

// Forward 2-D DCT of a level-shifted 8x8 block, rows then columns.
void ForwardDCT(const float in[64], float out[64])
{
  float rows[64];
  for (int y = 0; y < 8; ++y)
  {
    for (int u = 0; u < 8; ++u)
    {
      float sum = 0;
      for (int x = 0; x < 8; ++x)
        sum += kCosine[u][x] * in[y * 8 + x];
      rows[y * 8 + u] = sum;
    }
  }
  for (int u = 0; u < 8; ++u)
  {
    for (int v = 0; v < 8; ++v)
    {
      float sum = 0;
      for (int y = 0; y < 8; ++y)
        sum += kCosine[v][y] * rows[y * 8 + u];
      out[v * 8 + u] = sum;
    }
  }
}

class BitWriter
{
public:
  explicit BitWriter(std::vector<u8>* out) : m_out(out) {}

  void Put(u32 bits, int count)
  {
    m_buffer = (m_buffer << count) | (bits & ((1u << count) - 1));
    m_count += count;
    while (m_count >= 8)
    {
      const u8 byte = static_cast<u8>(m_buffer >> (m_count - 8));
      m_out->push_back(byte);
      // A 0xFF in entropy-coded data is followed by a zero byte so it is not read as a marker.
      if (byte == 0xff)
        m_out->push_back(0);
      m_count -= 8;
    }
  }

  // Pads the last byte with one bits.
  void Flush()
  {
    if (m_count > 0)
      Put(0x7f, 8 - m_count);
  }

private:
  std::vector<u8>* m_out;
  u32 m_buffer = 0;
  int m_count = 0;
};

// Bits needed for the magnitude of v (its JPEG "category").
int Category(int v)
{
  v = std::abs(v);
  int bits = 0;
  while (v != 0)
  {
    ++bits;
    v >>= 1;
  }
  return bits;
}

// The extra bits after a category: v itself if positive, else v - 1 (ones' complement).
u32 ExtraBits(int v, int category)
{
  return static_cast<u32>(v < 0 ? v + (1 << category) - 1 : v);
}

void EncodeBlock(BitWriter& bits, const float samples[64], const float scale[64], int* dc_pred,
                 const HuffmanCodes& dc, const HuffmanCodes& ac)
{
  float coeffs[64];
  ForwardDCT(samples, coeffs);
  int q[64];
  for (int n = 0; n < 64; ++n)
  {
    const int pos = kZigZag[n];
    q[n] = static_cast<int>(std::lround(coeffs[pos] * scale[pos]));
  }

  const int diff = q[0] - *dc_pred;
  *dc_pred = q[0];
  const int dc_cat = Category(diff);
  bits.Put(dc.code[dc_cat], dc.length[dc_cat]);
  if (dc_cat != 0)
    bits.Put(ExtraBits(diff, dc_cat), dc_cat);

  int zeros = 0;
  for (int n = 1; n < 64; ++n)
  {
    if (q[n] == 0)
    {
      ++zeros;
      continue;
    }
    // A run of 16 zeros is the symbol 0xF0.
    while (zeros >= 16)
    {
      bits.Put(ac.code[0xf0], ac.length[0xf0]);
      zeros -= 16;
    }
    const int cat = std::min(Category(q[n]), 10);
    const int symbol = zeros << 4 | cat;
    bits.Put(ac.code[symbol], ac.length[symbol]);
    bits.Put(ExtraBits(std::clamp(q[n], -1023, 1023), cat), cat);
    zeros = 0;
  }
  // End of block, unless the last coefficient was non-zero.
  if (zeros > 0)
    bits.Put(ac.code[0], ac.length[0]);
}

void PutMarker(std::vector<u8>* out, u8 marker)
{
  out->push_back(0xff);
  out->push_back(marker);
}

void Put16(std::vector<u8>* out, u32 v)
{
  out->push_back(static_cast<u8>(v >> 8));
  out->push_back(static_cast<u8>(v));
}

void PutHuffmanTable(std::vector<u8>* out, u8 id, const HuffmanSpec& spec)
{
  out->push_back(id);
  out->insert(out->end(), spec.counts, spec.counts + 16);
  out->insert(out->end(), spec.symbols.begin(), spec.symbols.end());
}
}  // namespace

JpegEncoder::JpegEncoder(int quality)
{
  quality = std::clamp(quality, 1, 100);
  // The usual mapping of a 1-100 quality onto a percentage of the example tables.
  const int percent = quality < 50 ? 5000 / quality : 200 - quality * 2;
  for (int i = 0; i < 64; ++i)
  {
    m_luma_quant[i] = static_cast<u8>(std::clamp((kLumaQuant[i] * percent + 50) / 100, 1, 255));
    m_chroma_quant[i] =
        static_cast<u8>(std::clamp((kChromaQuant[i] * percent + 50) / 100, 1, 255));
    m_luma_scale[i] = 1.0f / m_luma_quant[i];
    m_chroma_scale[i] = 1.0f / m_chroma_quant[i];
  }
}

void JpegEncoder::EncodeRow(const u8* rgba, int width, int height, int stride, int row,
                            std::vector<u8>* out) const
{
  BitWriter bits(out);
  int y_pred = 0, cb_pred = 0, cr_pred = 0;
  const int mcus = (width + 15) / 16;

  for (int mcu = 0; mcu < mcus; ++mcu)
  {
    float luma[4][64];
    float cb[64] = {}, cr[64] = {};
    for (int py = 0; py < 16; ++py)
    {
      // Edge pixels repeat past the image's right and bottom edges.
      const int sy = std::min(row * 16 + py, height - 1);
      const u8* line = rgba + static_cast<size_t>(sy) * stride;
      for (int px = 0; px < 16; ++px)
      {
        const int sx = std::min(mcu * 16 + px, width - 1);
        const float r = line[sx * 4], g = line[sx * 4 + 1], b = line[sx * 4 + 2];
        // JFIF's full-range BT.601 conversion, level-shifted by 128.
        const float y = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
        luma[(py / 8) * 2 + px / 8][(py % 8) * 8 + px % 8] = y;
        const int c = (py / 2) * 8 + px / 2;
        cb[c] += 0.25f * (-0.168736f * r - 0.331264f * g + 0.5f * b);
        cr[c] += 0.25f * (0.5f * r - 0.418688f * g - 0.081312f * b);
      }
    }
    for (const auto& block : luma)
      EncodeBlock(bits, block, m_luma_scale, &y_pred, kLumaDCCodes, kLumaACCodes);
    EncodeBlock(bits, cb, m_chroma_scale, &cb_pred, kChromaDCCodes, kChromaACCodes);
    EncodeBlock(bits, cr, m_chroma_scale, &cr_pred, kChromaDCCodes, kChromaACCodes);
  }
  bits.Flush();
}

void JpegEncoder::Encode(const u8* rgba, int width, int height, int stride,
                         std::vector<u8>* out) const
{
  out->clear();
  const int rows = (height + 15) / 16;
  const int mcus = (width + 15) / 16;

  PutMarker(out, 0xd8);  // SOI

  PutMarker(out, 0xdb);  // DQT: both tables, 8-bit, in zig-zag order
  Put16(out, 2 + 2 * 65);
  out->push_back(0);
  for (int n = 0; n < 64; ++n)
    out->push_back(m_luma_quant[kZigZag[n]]);
  out->push_back(1);
  for (int n = 0; n < 64; ++n)
    out->push_back(m_chroma_quant[kZigZag[n]]);

  PutMarker(out, 0xc0);  // SOF0: 8-bit, three components, Y sampled 2x2
  Put16(out, 17);
  out->push_back(8);
  Put16(out, static_cast<u32>(height));
  Put16(out, static_cast<u32>(width));
  out->push_back(3);
  const u8 components[3][3] = {{1, 0x22, 0}, {2, 0x11, 1}, {3, 0x11, 1}};
  for (const auto& c : components)
    out->insert(out->end(), c, c + 3);

  PutMarker(out, 0xc4);  // DHT
  Put16(out, static_cast<u32>(2 + 4 * 17 + kLumaDC.symbols.size() + kLumaAC.symbols.size() +
                              kChromaDC.symbols.size() + kChromaAC.symbols.size()));
  PutHuffmanTable(out, 0x00, kLumaDC);
  PutHuffmanTable(out, 0x10, kLumaAC);
  PutHuffmanTable(out, 0x01, kChromaDC);
  PutHuffmanTable(out, 0x11, kChromaAC);

  PutMarker(out, 0xdd);  // DRI: a restart after each row of blocks
  Put16(out, 4);
  Put16(out, static_cast<u32>(mcus));

  PutMarker(out, 0xda);  // SOS
  Put16(out, 12);
  out->push_back(3);
  const u8 tables[3][2] = {{1, 0x00}, {2, 0x11}, {3, 0x11}};
  for (const auto& t : tables)
    out->insert(out->end(), t, t + 2);
  out->push_back(0);
  out->push_back(63);
  out->push_back(0);

  // Rows are independent after a restart, so they are encoded in parallel.
  std::vector<std::vector<u8>> encoded(rows);
  const int workers =
      std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, std::max(rows, 1));
  auto work = [&](int first) {
    for (int row = first; row < rows; row += workers)
      EncodeRow(rgba, width, height, stride, row, &encoded[row]);
  };
  std::vector<std::thread> threads;
  for (int w = 1; w < workers; ++w)
    threads.emplace_back(work, w);
  work(0);
  for (auto& t : threads)
    t.join();

  for (int row = 0; row < rows; ++row)
  {
    out->insert(out->end(), encoded[row].begin(), encoded[row].end());
    if (row + 1 < rows)
      PutMarker(out, static_cast<u8>(0xd0 + row % 8));  // RSTn
  }
  PutMarker(out, 0xd9);  // EOI
}
}  // namespace VideoCommon
