// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// CBlockFile: the container used for save states (Saves/*.st) and NVRAM
// files (NVRAM/*.nv).
//
// A file is a plain sequence of blocks. Each block is
//
//   uint32  total size of the block in bytes, this header included
//   uint32  length of the name, counting its terminating zero
//   uint32  length of the comment, counting its terminating zero
//   char[]  name, zero terminated
//   char[]  comment, zero terminated
//   ...     data
//
// with the integers in the host's (little endian) byte order. The first
// block is the file header (its name says what the file is; it has no
// data). Readers look blocks up by name, so their order does not matter.

#ifndef INCLUDED_BLOCKFILE_H
#define INCLUDED_BLOCKFILE_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include "Types.h"

class CBlockFile
{
public:
  // Reading (after Load() and FindBlock()). Each returns the number of
  // bytes read. Reads simply continue through the file, past the end of
  // the current block if asked to.
  unsigned Read(void *data, uint32_t numBytes);
  unsigned Read(bool *value);   // one byte, nonzero = true
  unsigned Read(bool &value);

  template <typename T>
  unsigned Read(T &value)
  {
    return Read(static_cast<void *>(&value), static_cast<uint32_t>(sizeof(value)));
  }

  // Positions reading at the data of the first block with this name.
  Result FindBlock(const std::string &name);

  // Writing (after Create()). Data goes to the block last started with
  // NewBlock().
  void Write(bool value);       // one byte, 1 or 0
  void Write(const void *data, uint32_t numBytes);
  void Write(const std::string &str);   // its characters and a zero byte

  template <typename T>
  void Write(const T &value)
  {
    static_assert(std::is_trivially_copyable<T>::value, "Write() only supports POD / trivially copyable types");
    Write(static_cast<const void *>(&value), static_cast<uint32_t>(sizeof(value)));
  }

  // Ends the current block and starts a new one.
  void NewBlock(const std::string &name, const std::string &comment);

  // Creates (truncates) a file for writing and writes its header block.
  Result Create(const std::string &file, const std::string &headerName, const std::string &comment);

  // Opens a file for reading.
  Result Load(const std::string &file);

  // Finishes and closes the file. The destructor does the same.
  void Close(void);

  CBlockFile(void);
  ~CBlockFile(void);

private:
  enum class Mode
  {
    Closed,
    Reading,
    Writing
  };

  void StartBlock(const std::string &name, const std::string &comment);
  void FinishBlock(void);
  void Put(const void *data, size_t size);

  std::FILE *m_file = nullptr;
  Mode m_mode = Mode::Closed;
  long m_length = 0;          // file size when reading
  long m_blockStart = 0;      // offset of the current block's header
  uint32_t m_blockSize = 0;   // bytes in the current block so far (writing)
};

#endif // INCLUDED_BLOCKFILE_H
