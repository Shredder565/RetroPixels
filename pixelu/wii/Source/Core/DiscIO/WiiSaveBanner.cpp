// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/WiiSaveBanner.h"

#include <iterator>
#include <string>
#include <vector>

#include "Common/ColorUtil.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"

namespace DiscIO
{
namespace
{
constexpr u32 ICON_SIZE_BYTES = 48 * 48 * 2;
constexpr u32 BANNER_SIZE_BYTES = WiiSaveBanner::BANNER_WIDTH * WiiSaveBanner::BANNER_HEIGHT * 2;
}  // namespace

WiiSaveBanner::WiiSaveBanner(u64 title_id)
{
  // A usable file has the header, the banner and at least one icon.
  constexpr u64 minimum_size = sizeof(Header) + BANNER_SIZE_BYTES + ICON_SIZE_BYTES;

  File::IOFile file;
  for (const Common::FromWhichRoot root :
       {Common::FromWhichRoot::Configured, Common::FromWhichRoot::Banners})
  {
    m_path = Common::GetTitleDataPath(title_id, root) + "/banner.bin";
    file = File::IOFile(m_path, "rb");
    if (file)
      break;
  }

  if (!file.ReadArray(&m_header, 1))
  {
    m_header = {};
    m_valid = false;
    return;
  }
  m_valid = file.GetSize() >= minimum_size;
}

std::string WiiSaveBanner::GetName() const
{
  return UTF16BEToUTF8(m_header.name, std::size(m_header.name));
}

std::string WiiSaveBanner::GetDescription() const
{
  return UTF16BEToUTF8(m_header.description, std::size(m_header.description));
}

std::vector<u32> WiiSaveBanner::GetBanner(u32* width, u32* height) const
{
  *width = 0;
  *height = 0;

  File::IOFile file(m_path, "rb");
  std::vector<u16> pixels(BANNER_WIDTH * BANNER_HEIGHT);
  if (!file.Seek(sizeof(Header), File::SeekOrigin::Begin) ||
      !file.ReadArray(pixels.data(), pixels.size()))
  {
    return {};
  }

  std::vector<u32> image(BANNER_WIDTH * BANNER_HEIGHT);
  Common::Decode5A3Image(image.data(), pixels.data(), BANNER_WIDTH, BANNER_HEIGHT);
  *width = BANNER_WIDTH;
  *height = BANNER_HEIGHT;
  return image;
}

}  // namespace DiscIO
