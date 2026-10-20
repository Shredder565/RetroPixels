// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Assembler/AssemblerShared.h"

#include <algorithm>

#include <fmt/format.h>

namespace Common::GekkoAssembler
{
std::string AssemblerError::FormatError() const
{
  std::string_view source = error_line;
  if (!source.empty() && source.back() == '\n')
    source.remove_suffix(1);

  // The marker line starts two spaces in, like the source line, then lines up with `col`. The
  // marker is always at least one caret wide so zero-length errors stay visible.
  std::string marker(col, ' ');
  marker.append(std::max<size_t>(len, 1), '^');

  return fmt::format("Error on line {} col {}:\n  {}\n  {}\n{}", line + 1, col + 1, source, marker,
                     message);
}
}  // namespace Common::GekkoAssembler
