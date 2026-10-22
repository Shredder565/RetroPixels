// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Sega 315-5881 decryption. The cipher's constants (S-box contents, which
// key bits feed which S-box inputs, the bit permutations and the
// decompression trees) are facts about the chip, recovered by Andreas Naive
// and others and published with MAME; they are restated here as data.
//
// Block cipher, per 16-bit word:
//   - counter (the RAM word address) -> permute -> network 1 (4 rounds,
//     keyed by the game key and the sub-key) -> 16-bit middle result;
//   - ciphertext -> permute -> network 2 (keyed by the game key, the
//     sub-key and the middle result) -> permute -> plaintext.
// Each round is L ^= F(R) or R ^= F(L) on 8-bit halves; F runs four S-boxes
// with 6 inputs and 2 outputs each, every input XORed with a key bit.
// Round subkeys are 24 bits (6 per S-box); "key schedule" entries name a
// key bit and the subkey bit (round * 24 + bit) it toggles.
//
// The plaintext stream is offset by 2 bits from the block output: each
// output word is the low 2 bits of this block and the high 14 of the last.

#include "Model3/Crypto.h"
#include "BlockFile.h"
#include "OSD/Logger.h"

#include <cstring>

namespace
{
  struct SBox
  {
    const char *table;  // 64 outputs as digits 0-3; x = unknown entry
    int inputs[6];      // which bit of the round input feeds each input (-1: key only)
    int outputs[2];     // where the two output bits go
  };

  // Network 1, rounds 1-4, four S-boxes each.
  const SBox kNet1[16] =
  {
    { "0322131232121231322021300323212023112211102330211111303210120313", {  3,  4,  5,  7, -1, -1 }, { 0, 4 } },
    { "2220330122323022110332020101231101331331232000220313032203031102", {  0,  1,  2,  5,  6,  7 }, { 1, 6 } },
    { "0130311112313023320211211310020113100323203300001233203210002233", {  0,  2,  5,  6,  7, -1 }, { 2, 3 } },
    { "3212123203223133023033211120220113300303021321000112010001332033", {  1,  2,  3,  4,  6,  7 }, { 5, 7 } },
    { "3312002221213113300303321132321323013201213122333122031221303013", {  0,  1,  3,  4,  5,  7 }, { 0, 4 } },
    { "2010032033121302020002313112303030200221023313101330013102032101", {  0,  1,  3,  4,  6, -1 }, { 1, 5 } },
    { "2223110133112220032330212230130032032010012233022131111203100232", {  1,  2,  5,  6,  7,  6 }, { 2, 7 } },
    { "0133313310202003121312322013033300021123223112020213113311302300", {  2,  3,  4,  5,  6,  7 }, { 3, 6 } },
    { "0010100320030102030020322132211200030110021031222031301221110231", {  1,  2,  3,  4,  5,  7 }, { 0, 5 } },
    { "1210311200232313203223111103200110013123002330100230120133312021", {  0,  2,  4,  5,  6,  7 }, { 1, 6 } },
    { "03021200110031100300233231200230xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", {  0,  2,  4,  6,  7, -1 }, { 2, 3 } },
    { "0010010233033230222032031003300022102032003133002112101103120203", {  0,  1,  2,  3,  6, -1 }, { 4, 7 } },
    { "0333332001202222110221323201232132231010020121231211221013232031", {  0,  1,  3,  4,  5,  6 }, { 0, 5 } },
    { "0300203111222131221322330310320130201021331220233230113302130223", {  0,  1,  2,  3,  5,  7 }, { 1, 7 } },
    { "0123333120232101221203201101313131001000012211331233323022133020", {  2,  3,  4,  5,  6,  7 }, { 2, 3 } },
    { "0211320310103211220310122233000012102122232301300130011010000201", {  0,  1,  2,  4,  6,  7 }, { 4, 6 } },
  };

  // Network 2, rounds 1-4.
  const SBox kNet2[16] =
  {
    { "3301010003001312033321011122232221331311001202211231213122013022", {  1,  3,  4,  5,  6,  7 }, { 0, 7 } },
    { "0130112320032131330010030332320132322131110332211202011010112030", {  0,  3,  5,  6,  5,  0 }, { 1, 2 } },
    { "0221012120123301311212133233210101020113203211132302302213011222", {  0,  2,  3,  4,  7, -1 }, { 3, 4 } },
    { "2313201200333331202123020103021023013032312031120300202122321231", {  1,  2,  5,  6, -1, -1 }, { 5, 6 } },
    { "2313103332332003230311231122010021012012031123120201301022303230", {  0,  1,  4,  5,  6,  7 }, { 0, 7 } },
    { "0220220323213233110030211332320112301030310212032312203230012333", {  0,  2,  3,  6,  7, -1 }, { 1, 5 } },
    { "1030012100100023220201302013230112221303011032332003121332102123", {  2,  3,  4,  6,  7,  2 }, { 2, 3 } },
    { "2313112331101023210022010222021031231302101001223231321120213310", {  1,  2,  3,  4,  5,  6 }, { 4, 6 } },
    { "0301300210132220333022030023032133030233111022113031202000321100", {  1,  4,  5,  6,  7,  5 }, { 0, 5 } },
    { "0301303132223032212203220021132323312012210000231203131232103002", {  0,  2,  3,  4,  6,  7 }, { 1, 7 } },
    { "22030310112323100032202313203313xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", {  1,  2,  4,  7,  2, -1 }, { 2, 4 } },
    { "0231311001302133202112330002023033332332301023320131012330203033", {  0,  1,  2,  3,  5,  7 }, { 3, 6 } },
    { "0110010233012302103320300210101303312030132033130233211221212011", {  0,  1,  2,  4,  7, -1 }, { 0, 5 } },
    { "2002302331112110021003100330101202020123211033333310300203202201", {  0,  1,  3,  5,  6, -1 }, { 1, 3 } },
    { "0112131100311120320112333030020332003231230320122202012231221111", {  0,  2,  3,  4,  5,  7 }, { 2, 7 } },
    { "0120330321330311323230003022322322312312030231003212121310233132", {  2,  3,  4,  5,  6,  7 }, { 4, 6 } },
  };

  struct KeyBit
  {
    int source;   // bit of the game key / sub-key
    int target;   // subkey bit, round * 24 + bit
  };

  const KeyBit kGameKeyNet1[] =
  {
    { 1, 29 }, { 1, 71 }, { 2, 4 }, { 2, 54 }, { 3, 8 }, { 4, 56 }, { 4, 73 }, { 5, 11 },
    { 6, 51 }, { 7, 92 }, { 8, 89 }, { 9, 9 }, { 9, 39 }, { 9, 58 }, { 10, 90 }, { 11, 6 },
    { 12, 64 }, { 13, 49 }, { 14, 44 }, { 15, 40 }, { 16, 69 }, { 17, 15 }, { 18, 23 }, { 18, 43 },
    { 19, 82 }, { 20, 81 }, { 21, 32 }, { 22, 5 }, { 23, 66 }, { 24, 13 }, { 24, 45 }, { 25, 12 },
    { 25, 35 }, { 26, 61 }, { 27, 10 }, { 27, 59 }, { 28, 25 }, { 29, 86 },
  };

  const KeyBit kGameKeyNet2[] =
  {
    { 0, 0 }, { 1, 3 }, { 2, 11 }, { 3, 20 }, { 4, 22 }, { 5, 23 }, { 6, 29 }, { 7, 38 },
    { 8, 39 }, { 9, 55 }, { 9, 86 }, { 9, 87 }, { 10, 50 }, { 11, 57 }, { 12, 59 }, { 13, 61 },
    { 14, 63 }, { 15, 67 }, { 16, 72 }, { 17, 83 }, { 18, 88 }, { 19, 94 }, { 20, 35 }, { 21, 17 },
    { 22, 6 }, { 23, 85 }, { 24, 16 }, { 25, 25 }, { 26, 92 }, { 27, 47 }, { 28, 28 }, { 29, 90 },
  };

  const KeyBit kSubKeyNet1[] =
  {
    { 0, 52 }, { 1, 34 }, { 2, 17 }, { 3, 36 }, { 4, 84 }, { 4, 88 }, { 5, 57 }, { 6, 48 },
    { 6, 68 }, { 7, 76 }, { 8, 83 }, { 9, 30 }, { 10, 22 }, { 10, 41 }, { 11, 38 }, { 12, 55 },
    { 13, 74 }, { 14, 19 }, { 14, 80 }, { 15, 26 },
  };

  // Sub-key bit n and middle-result bit n toggle these network 2 subkey bits.
  const int kSubKeyNet2[16] = { 77, 34, 8, 42, 36, 27, 69, 66, 13, 9, 79, 31, 49, 7, 24, 64 };
  const int kMiddleNet2[16] = { 1, 10, 44, 68, 74, 78, 81, 95, 2, 4, 30, 40, 41, 51, 53, 58 };

  // Bit permutations: entry i is the source bit of output bit 15 - i.
  const int kCounterIn[16] = { 5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2 };
  const int kCipherIn[16] = { 14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10 };
  const int kPlainOut[16] = { 15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8 };

  // Decompression prefix trees, one pair (bit 0 / bit 1 branch) per
  // position class: 0 = first byte of a line, 1 = inside the line, 2-8 =
  // the last 7 bytes by position. A node below 0x80 is the next node; a
  // leaf is 1a0bbccc: a = 1 copy from the previous line at offset bb
  // (00: 0, 01: +1, 11: -1), a = 0 an 8-bit literal follows; ccc + 1 is the
  // byte count. FF: no output.
  const char *const kTrees[9][2] =
  {
    { "01100f05c413870acc81ce0c860e84c211c1c3cf15c8cdffffffffffffffffff",
      "c70203048006070809c90b0d828385c012c6c51416cacbffffffffffffffffff" },
    { "028005048110158209830b0c0ddc0fde1ccfc5dd86168718191adacac91eceff",
      "0117030a080607c2d9c4d8c80e84cb8511121314cd1bdbc7c0c11ddfc3c6ccff" },
    { "c680030b0507820815dcdd0cd9c2141085861816c5c4c8c9c0ccffffffffffff",
      "01021204810683c3090a84110d0e0f19cac113d8dadb17decdcbffffffffffff" },
    { "01800d0405158308d9100b0c840ec01412cb13cac8c2ffffffffffffffffffff",
      "c5020307810682cc090ac911c40f85d8dadbc3dcddc1ffffffffffffffffffff" },
    { "0180060c0581d88409dc0b0f0d0e10db11caffffffffffffffffffffffffffff",
      "c4020304cb0a0708d982c883c0c1dac2c9c3ffffffffffffffffffffffffffff" },
    { "0102060a830b07080982d80cd9daffffffffffffffffffffffffffffffffffff",
      "c38003040581cac8dbc9c0c10dc2ffffffffffffffffffffffffffffffffffff" },
    { "01020304810708d8dad9ffffffffffffffffffffffffffffffffffffffffffff",
      "c28005c9c80682c009c1ffffffffffffffffffffffffffffffffffffffffffff" },
    { "018004c8c0d9ffffffffffffffffffffffffffffffffffffffffffffffffffff",
      "c102038105d8ffffffffffffffffffffffffffffffffffffffffffffffffffff" },
    { "01d8ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
      "c080ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" },
  };

  int HexDigit(char c)
  {
    return (c <= '9') ? (c - '0') : (c - 'a' + 10);
  }

  uint8_t TreeNode(int tree, int branch, unsigned node)
  {
    const char *s = kTrees[tree][branch] + 2 * (node & 0x1F);
    return uint8_t((HexDigit(s[0]) << 4) | HexDigit(s[1]));
  }

  // Unknown S-box entries behave as if both output bits were set.
  unsigned SBoxValue(const SBox &box, unsigned index)
  {
    char c = box.table[index & 0x3F];
    return (c == 'x') ? 3 : unsigned(c - '0');
  }

  unsigned Bit(uint32_t v, int n)
  {
    return (v >> n) & 1;
  }

  uint16_t Permute(uint16_t v, const int (&from)[16])
  {
    uint16_t r = 0;
    for (int i = 0; i < 16; i++)
      r = uint16_t(r | (Bit(v, from[i]) << (15 - i)));
    return r;
  }

  void Toggle(uint32_t (&subkeys)[4], int target)
  {
    subkeys[target / 24] ^= 1u << (target % 24);
  }

  // One round function: 8-bit input, four S-boxes, 24-bit subkey.
  unsigned Round(unsigned input, const SBox *boxes, uint32_t subkey)
  {
    unsigned out = 0;
    for (int b = 0; b < 4; b++)
    {
      const SBox &box = boxes[b];
      unsigned index = 0;
      for (int k = 0; k < 6; k++)
      {
        if (box.inputs[k] >= 0)
          index |= Bit(input, box.inputs[k]) << k;
      }
      unsigned v = SBoxValue(box, index ^ ((subkey >> (6 * b)) & 0x3F));
      out |= (v & 1) << box.outputs[0];
      out |= ((v >> 1) & 1) << box.outputs[1];
    }
    return out;
  }

  // Four-round Feistel network on a 16-bit value (high byte = right half).
  uint16_t Network(uint16_t v, const SBox *boxes, const uint32_t (&subkeys)[4])
  {
    unsigned hi = v >> 8;
    unsigned lo = v & 0xFF;
    lo ^= Round(hi, boxes + 0, subkeys[0]);
    hi ^= Round(lo, boxes + 4, subkeys[1]);
    lo ^= Round(hi, boxes + 8, subkeys[2]);
    hi ^= Round(lo, boxes + 12, subkeys[3]);
    return uint16_t((hi << 8) | lo);
  }
}

CCrypto::CCrypto()
{
}

void CCrypto::SaveState(CBlockFile *SaveState)
{
  if (m_key == 0)
    return;   // no security board
  SaveState->NewBlock("Sega 315-5881", __FILE__);
  SaveState->Write(m_out, kOutBytes);
  SaveState->Write(m_lines[m_curLine], kLineBytes);
  SaveState->Write(m_lines[m_curLine ^ 1], kLineBytes);
  SaveState->Write(&m_address, sizeof(m_address));
  SaveState->Write(&m_subKey, sizeof(m_subKey));
  SaveState->Write(&m_ready, sizeof(m_ready));
  SaveState->Write(&m_lastPlain, sizeof(m_lastPlain));
  SaveState->Write(&m_header, sizeof(m_header));
  SaveState->Write(&m_outPos, sizeof(m_outPos));
  SaveState->Write(&m_linePos, sizeof(m_linePos));
  SaveState->Write(&m_lineSize, sizeof(m_lineSize));
}

void CCrypto::LoadState(CBlockFile *SaveState)
{
  if (m_key == 0)
    return;
  if (SaveState->FindBlock("Sega 315-5881") != Result::OKAY)
  {
    ErrorLog("Unable to load security board encryption device state. Save state file is corrupt.");
    return;
  }
  SaveState->Read(m_out, kOutBytes);
  SaveState->Read(m_lines[m_curLine], kLineBytes);
  SaveState->Read(m_lines[m_curLine ^ 1], kLineBytes);
  SaveState->Read(&m_address, sizeof(m_address));
  SaveState->Read(&m_subKey, sizeof(m_subKey));
  SaveState->Read(&m_ready, sizeof(m_ready));
  SaveState->Read(&m_lastPlain, sizeof(m_lastPlain));
  SaveState->Read(&m_header, sizeof(m_header));
  SaveState->Read(&m_outPos, sizeof(m_outPos));
  SaveState->Read(&m_linePos, sizeof(m_linePos));
  SaveState->Read(&m_lineSize, sizeof(m_lineSize));
  ScheduleSequenceKey();
}

void CCrypto::Init(uint32_t encryptionKey, std::function<uint16_t(uint32_t)> ReadRAMCallback)
{
  m_read = ReadRAMCallback;
  m_key = encryptionKey;

  // The game key's part of the key schedule never changes.
  std::memset(m_gameKeyNet1, 0, sizeof(m_gameKeyNet1));
  std::memset(m_gameKeyNet2, 0, sizeof(m_gameKeyNet2));
  for (const KeyBit &k : kGameKeyNet1)
  {
    if (Bit(m_key, k.source))
      Toggle(m_gameKeyNet1, k.target);
  }
  for (const KeyBit &k : kGameKeyNet2)
  {
    if (Bit(m_key, k.source))
      Toggle(m_gameKeyNet2, k.target);
  }
  ScheduleSequenceKey();
}

void CCrypto::ScheduleSequenceKey()
{
  std::memcpy(m_seqKeyNet1, m_gameKeyNet1, sizeof(m_seqKeyNet1));
  std::memcpy(m_seqKeyNet2, m_gameKeyNet2, sizeof(m_seqKeyNet2));
  for (const KeyBit &k : kSubKeyNet1)
  {
    if (Bit(m_subKey, k.source))
      Toggle(m_seqKeyNet1, k.target);
  }
  for (int n = 0; n < 16; n++)
  {
    if (Bit(m_subKey, n))
      Toggle(m_seqKeyNet2, kSubKeyNet2[n]);
  }
}

void CCrypto::Reset()
{
  std::memset(m_out, 0, sizeof(m_out));
  std::memset(m_lines, 0, sizeof(m_lines));
  m_address = 0;
  m_subKey = 0;
  m_lastPlain = 0;
  m_header = 0;
  m_ready = false;
  m_outPos = 0;
  m_linePos = 0;
  m_lineSize = 0;
  m_bitInByte = 0;
  ScheduleSequenceKey();
}

uint16_t CCrypto::DecryptBlock(uint16_t counter, uint16_t cipher) const
{
  uint16_t middle = Network(Permute(counter, kCounterIn), kNet1, m_seqKeyNet1);

  uint32_t net2[4];
  std::memcpy(net2, m_seqKeyNet2, sizeof(net2));
  for (int n = 0; n < 16; n++)
  {
    if (Bit(middle, n))
      Toggle(net2, kMiddleNet2[n]);
  }

  return Permute(Network(Permute(cipher, kCipherIn), kNet2, net2), kPlainOut);
}

uint16_t CCrypto::NextWord()
{
  uint16_t cipher = m_read(m_address);
  uint16_t block = DecryptBlock(uint16_t(m_address), cipher);
  uint16_t plain = uint16_t((m_lastPlain & 0xFFFC) | (block & 0x0003));
  m_lastPlain = block;
  m_address++;
  return plain;
}

void CCrypto::StartStream()
{
  m_blockPos = 0;
  m_streamDone = false;
  m_outPos = kOutBytes;

  // The header's first 2 bits may still be in the bit reader's word.
  if (m_bitInWord != 15)
    m_header = uint32_t(m_compWord & 0x0003) << 16;
  else
  {
    m_lastPlain = 0;
    m_header = uint32_t(NextWord()) << 16;
  }
  m_header |= NextWord();

  // Size: (bits 16-8 + 1) * (bits 7-0 + 1); for a compressed stream the
  // first factor is the line length.
  m_blockLines = int(m_header & 0xFF) + 1;
  int lineBytes = int((m_header >> 8) & 0x1FF) + 1;
  m_blockSize = m_blockLines * lineBytes;

  if (m_header & kCompressed)
  {
    m_lineSize = lineBytes;
    m_linePos = lineBytes;
    m_bitInByte = 7;
    m_bitInWord = 15;
  }
  m_ready = true;
}

void CCrypto::FillOutput()
{
  for (int i = 0; i < kOutBytes; i += 2)
  {
    uint16_t w = NextWord();
    m_out[i] = uint8_t(w);
    m_out[i + 1] = uint8_t(w >> 8);
    m_blockPos += 2;
    if (!(m_header & kCompressed) && m_blockPos == m_blockSize)
      StartStream();    // the next stream's header follows directly
  }
  m_outPos = 0;
}

int CCrypto::NextBit()
{
  // Bits come MSB first from the bytes of each new word, high byte first.
  if (m_bitInWord == 15)
  {
    m_bitInWord = 0;
    m_compWord = NextWord();
    m_compBytes[0] = uint8_t(m_compWord);
    m_compBytes[1] = uint8_t(m_compWord >> 8);
    m_outPos = 0;
  }
  else
    m_bitInWord++;

  int bit = (m_compBytes[(m_outPos & 1) ^ 1] >> m_bitInByte) & 1;
  if (--m_bitInByte < 0)
  {
    m_bitInByte = 7;
    m_outPos++;
  }
  return bit;
}

void CCrypto::FillLine()
{
  // The new line replaces the older one; the last line stays readable for
  // copies.
  m_curLine ^= 1;
  uint8_t *line = m_lines[m_curLine];
  const uint8_t *prev = m_lines[m_curLine ^ 1];
  const int size = m_lineSize;
  m_linePos = 0;

  int i = 0;
  while (i < size)
  {
    int tree = (i == 0) ? 0 : (i < size - 7) ? 1 : (i & 7) + 1;
    unsigned node = 0;
    while ((node & 0x80) == 0)
      node = TreeNode(tree, NextBit(), node);
    if (node == 0xFF)
      continue;

    int count = int(node & 7) + 1;
    if (node & 0x40)
    {
      static const int kShift[4] = { 0, 1, 0, -1 };
      int shift = kShift[(node >> 3) & 3];
      for (int n = 0; n < count && i < size; n++, i++)
      {
        int from = (i + shift) % size;
        if (from < 0)
          from += size;
        line[(i ^ 1) & (kLineBytes - 1)] = prev[(from ^ 1) & (kLineBytes - 1)];
      }
    }
    else
    {
      unsigned byte = 0;
      for (int b = 0; b < 8; b++)
        byte = (byte << 1) | unsigned(NextBit());
      for (int n = 0; n < count && i < size; n++, i++)
        line[(i ^ 1) & (kLineBytes - 1)] = uint8_t(byte);
    }
  }

  if (++m_blockPos == m_blockLines)
    m_streamDone = true;
}

uint16_t CCrypto::Decrypt(uint8_t **base)
{
  if (m_key == 0)
    return 0xFFFF;
  if (!m_ready)
    StartStream();

  uint8_t *p;
  if (m_header & kCompressed)
  {
    if (m_linePos >= m_lineSize)
    {
      if (m_streamDone)
        StartStream();
      FillLine();
    }
    p = m_lines[m_curLine] + (m_linePos & (kLineBytes - 2));
    m_linePos += 2;
  }
  else
  {
    if (m_outPos >= kOutBytes)
      FillOutput();
    p = m_out;
    m_outPos += 2;
  }
  *base = p;
  return uint16_t((p[0] << 8) | p[1]);
}

void CCrypto::SetAddressLow(uint16_t data)
{
  m_address = (m_address & 0xFFFF0000) | data;
  m_ready = false;
}

void CCrypto::SetAddressHigh(uint16_t data)
{
  m_address = (m_address & 0x0000FFFF) | (uint32_t(data) << 16);
  m_ready = false;
  m_bitInByte = 7;
  m_bitInWord = 15;
}

void CCrypto::SetSubKey(uint16_t data)
{
  m_subKey = data;
  m_ready = false;
  ScheduleSequenceKey();
}
