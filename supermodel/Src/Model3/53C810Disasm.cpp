// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// A small NCR 53C810 SCRIPTS disassembler for looking at what games feed
// the SCSI controller. It decodes the instruction groups Model 3 games use
// (Block Move, Memory Move, INT/INTFLY); anything else is reported as
// unknown. It is not part of the emulator build; define STANDALONE to build
// it as a command line tool:
//
//   scriptsd <file> [-s <hex start offset>] [-l <count>] [-o <hex origin>]
//
// The file is read as little endian 32-bit words, the order the 53C810
// fetches them in.

#include <cstdio>
#include <cstring>
#ifdef STANDALONE
#include <cstdlib>
#include <vector>
#endif
#include "Types.h"

namespace
{
  const char *const kPhaseName[8] =
  {
    "data_out", "data_in", "command", "status", "res4", "res5", "message_out", "message_in"
  };

  // Appends formatted text at the end of the buffer.
  template <typename... Args>
  void Append(char *buf, size_t size, const char *fmt, Args... args)
  {
    size_t used = std::strlen(buf);
    if (used < size)
      std::snprintf(buf + used, size - used, fmt, args...);
  }

  void Append(char *buf, size_t size, const char *text)
  {
    size_t used = std::strlen(buf);
    if (used < size)
      std::snprintf(buf + used, size - used, "%s", text);
  }
}

// Decodes the instruction whose words are op[0..2] at address addr into
// mnem (at least 64 bytes; empty if unknown). Returns the number of words
// the instruction takes (2 or 3), or 0 if it is not recognised.
int DisassembleSCRIPTS(UINT32 op[3], UINT32 addr, char *mnem)
{
  const size_t kSize = 64;
  (void)addr;
  mnem[0] = '\0';

  const UINT32 w = op[0];
  const unsigned group = w >> 30;
  const unsigned phase = (w >> 24) & 7;

  if (group == 0)
  {
    // Block Move: bit 28 table indirect, bit 29 indirect, bit 27 opcode.
    unsigned opc = (w >> 27) & 1;
    if (w & 0x10000000)
      Append(mnem, kSize, "move (%u) from %08X, when %s", opc, op[1], kPhaseName[phase]);
    else if (w & 0x20000000)
      Append(mnem, kSize, "move (%u) %X, ptr %08X, when %s", opc, w & 0x00FFFFFF, op[1], kPhaseName[phase]);
    else
      Append(mnem, kSize, "move (%u) %X, %08X, when %s", opc, w & 0x00FFFFFF, op[1], kPhaseName[phase]);
    return 2;
  }

  if ((w >> 29) == 0x6)
  {
    // Memory Move: count, source, destination; bit 24 = no flush.
    Append(mnem, kSize, "move memory %s%X, %08X, %08X", (w & 0x01000000) ? "no flush " : "", w & 0x00FFFFFF, op[1], op[2]);
    return 3;
  }

  if ((w >> 27) == 0x13)
  {
    // Transfer control, interrupt type: INT / INTFLY with its condition.
    bool onTheFly = (w & 0x00100000) != 0;
    bool useCarry = (w & 0x00200000) != 0;
    bool ifTrue = (w & 0x00080000) != 0;
    bool testData = (w & 0x00040000) != 0;
    bool testPhase = (w & 0x00020000) != 0;
    bool waitValid = (w & 0x00010000) != 0;

    Append(mnem, kSize, "%s %08X, %s%s ", onTheFly ? "intfly" : "int", op[1], waitValid ? "if" : "when", ifTrue ? "" : " not");
    if (useCarry)
      Append(mnem, kSize, "carry");
    else if (!testPhase && !testData)
      Append(mnem, kSize, "atn");
    else
    {
      if (testPhase)
        Append(mnem, kSize, kPhaseName[phase]);
      if (testPhase && testData)
        Append(mnem, kSize, " and ");
      if (testData)
        Append(mnem, kSize, "%02X mask %02X", w & 0xFF, (w >> 8) & 0xFF);
    }
    return 2;
  }

  return 0;
}

#ifdef STANDALONE

static void Usage(void)
{
  std::puts("scriptsd: NCR 53C810 SCRIPTS disassembler");
  std::puts("Usage:    scriptsd <file> [options]");
  std::puts("Options:  -?,-h       this help");
  std::puts("          -s <offset> start offset in the file (hexadecimal)");
  std::puts("          -l <num>    number of instructions (default 16)");
  std::puts("          -o <addr>   address of the start offset (hexadecimal)");
  std::exit(0);
}

static UINT32 WordAt(const std::vector<UINT8> &buf, size_t at)
{
  UINT32 v = 0;
  for (int i = 3; i >= 0; i--)
    v = (v << 8) | (at + i < buf.size() ? buf[at + i] : 0);
  return v;
}

int main(int argc, char **argv)
{
  const char *path = nullptr;
  unsigned long start = 0, origin = 0, count = 16;
  bool haveOrigin = false;

  if (argc <= 1)
    Usage();
  for (int i = 1; i < argc; i++)
  {
    const char *a = argv[i];
    if (!std::strcmp(a, "-h") || !std::strcmp(a, "-?"))
      Usage();
    else if (!std::strcmp(a, "-s") || !std::strcmp(a, "-l") || !std::strcmp(a, "-o"))
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "scriptsd: warning: no argument to %s\n", a);
        continue;
      }
      const char *v = argv[++i];
      if (a[1] == 's')
        start = std::strtoul(v, nullptr, 16);
      else if (a[1] == 'l')
        count = std::strtoul(v, nullptr, 10);
      else
      {
        origin = std::strtoul(v, nullptr, 16);
        haveOrigin = true;
      }
    }
    else
      path = a;
  }
  if (path == nullptr)
  {
    std::fprintf(stderr, "scriptsd: no input file specified\n");
    return 1;
  }

  std::FILE *f = std::fopen(path, "rb");
  if (f == nullptr)
  {
    std::fprintf(stderr, "scriptsd: failed to open file: %s\n", path);
    return 1;
  }
  std::vector<UINT8> buf;
  int c;
  while ((c = std::fgetc(f)) != EOF)
    buf.push_back(UINT8(c));
  std::fclose(f);

  if (!haveOrigin)
    origin = start;

  size_t at = start;
  UINT32 pc = UINT32(origin);
  for (unsigned long n = 0; n < count && at < buf.size(); n++)
  {
    UINT32 op[3] = { WordAt(buf, at), WordAt(buf, at + 4), WordAt(buf, at + 8) };
    char text[64];
    int words = DisassembleSCRIPTS(op, pc, text);
    if (words == 3)
      std::printf("%08X: %08X %08X %08X %s\n", pc, op[0], op[1], op[2], text);
    else if (words == 2)
      std::printf("%08X: %08X %08X          %s\n", pc, op[0], op[1], text);
    else
    {
      std::printf("%08X: %08X                   ?\n", pc, op[0]);
      words = 1;
    }
    at += size_t(words) * 4;
    pc += UINT32(words) * 4;
  }
  return 0;
}

#endif // STANDALONE
