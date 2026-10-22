// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The security board's Sega 315-5881 protection chip. Games load an
// encrypted (and optionally compressed) stream into security board RAM,
// set a sub-key and read the plaintext back word by word through register
// F01A001C. The cipher is the one documented by the MAME team's analysis
// of the chip (also used on NAOMI M2 cartridges): a 16-bit block cipher
// built from two 4-round Feistel networks, run in counter mode over the
// RAM word address. Streams start with an 18-bit header (compressed flag,
// then the size as two counts) and compressed streams are coded line by
// line with fixed prefix-code trees.

#ifndef INCLUDED_CRYPTO_H
#define INCLUDED_CRYPTO_H

#include <cstdint>
#include <functional>
#include <memory>

class CBlockFile;

class CCrypto
{
public:
  CCrypto();

  // Save state block "Sega 315-5881" (only for games with a security
  // board, i.e. a nonzero key).
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // The game's key (0 = no security board) and how to read a 16-bit word
  // of security board RAM by word address.
  void Init(uint32_t encryptionKey, std::function<uint16_t(uint32_t)> ReadRAMCallback);
  void Reset();

  // Next plaintext word. *base is set to the two bytes it came from.
  // Returns FFFF when there is no security board.
  uint16_t Decrypt(uint8_t **base);

  // Stream start address (word address, in two halves) and sub-key.
  void SetAddressLow(uint16_t data);
  void SetAddressHigh(uint16_t data);
  void SetSubKey(uint16_t data);

  std::function<uint16_t(uint32_t)> m_read;

private:
  static constexpr int kOutBytes = 2;
  static constexpr int kLineBytes = 512;
  static constexpr uint32_t kCompressed = 0x20000;   // header flag

  uint16_t DecryptBlock(uint16_t counter, uint16_t cipher) const;
  void ScheduleSequenceKey();
  uint16_t NextWord();
  int NextBit();
  void StartStream();
  void FillOutput();
  void FillLine();

  uint32_t m_key = 0;
  uint32_t m_gameKeyNet1[4] = {}, m_gameKeyNet2[4] = {};  // key schedule from the game key
  uint32_t m_seqKeyNet1[4] = {}, m_seqKeyNet2[4] = {};    // ... with the sub-key added

  // Saved state
  uint8_t m_out[kOutBytes] = {};
  uint8_t m_lines[2][kLineBytes] = {};  // current and previous line
  int m_curLine = 0;
  uint32_t m_address = 0;
  uint16_t m_subKey = 0;
  bool m_ready = false;
  uint16_t m_lastPlain = 0;     // previous block output (2-bit shift)
  uint32_t m_header = 0;
  int32_t m_outPos = 0;
  int32_t m_linePos = 0;
  int32_t m_lineSize = 0;

  // Not saved
  int m_bitInByte = 0;          // next bit of the current byte, 7..0
  int m_bitInWord = 15;         // bits taken from m_compWord, 15 = need a new word
  uint16_t m_compWord = 0;
  uint8_t m_compBytes[2] = {};
  int m_blockPos = 0;
  int m_blockSize = 0;
  int m_blockLines = 0;
  bool m_streamDone = false;
};

#endif // INCLUDED_CRYPTO_H
