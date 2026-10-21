// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/WiiUtils.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <pugixml.hpp>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"
#include "Core/IOS/Uids.h"
#include "Core/SysConf.h"
#include "Core/System.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeFileBlobReader.h"
#include "DiscIO/VolumeWad.h"

namespace WiiUtils
{
namespace
{
using IOS::HLE::ESCore;
namespace FS = IOS::HLE::FS;

constexpr FS::Modes ALL_RW{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};

// Some games (Mega Man 9, for one) only load DLC when the Wii Shop's log files
// exist, which a WAD install would not otherwise create. Creates any that are
// missing, filled with 32 zero bytes.
bool EnsureShopLogs(FS::FileSystem& fs)
{
  constexpr std::array<const char*, 2> LOGS{"/shared2/ec/shopsetu.log",
                                            "/shared2/succession/shop.log"};
  constexpr std::array<u8, 32> FILLER{};
  for (const std::string path : LOGS)
  {
    if (fs.CreateFullPath(IOS::SYSMENU_UID, IOS::SYSMENU_GID, path, 0, ALL_RW) !=
        FS::ResultCode::Success)
    {
      return false;
    }
    if (fs.OpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, path, FS::Mode::ReadWrite))
      continue;

    const auto created = fs.CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, path, ALL_RW);
    if (!created || !created->Write(FILLER.data(), FILLER.size()))
      return false;
  }
  return true;
}

// Feeds a WAD's ticket, TMD and contents through ES.
bool ImportWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad,
               ESCore::VerifySignature verify_signature)
{
  if (!wad.GetTicket().IsValid() || !wad.GetTMD().IsValid())
  {
    PanicAlertFmtT("WAD installation failed: The selected file is not a valid WAD.");
    return false;
  }

  const IOS::ES::TMDReader tmd = wad.GetTMD();
  ESCore& es = ios.GetESCore();
  const auto fs = ios.GetFS();

  // IOS checks the ticket's common key index, so use the corrected ticket.
  const IOS::ES::TicketReader ticket = wad.GetTicketWithFixedCommonKey();
  const auto& certs = wad.GetCertificateChain();

  ESCore::Context context;
  IOS::HLE::ReturnCode ret = es.ImportTicket(
      ticket.GetBytes(), certs, ESCore::TicketImportType::Unpersonalised, verify_signature);
  if (ret >= 0)
    ret = es.ImportTitleInit(context, tmd.GetBytes(), certs, verify_signature);
  if (ret < 0)
  {
    // Every failure except a check-value mismatch gets an alert.
    if (ret != IOS::HLE::IOSC_FAIL_CHECKVALUE)
    {
      PanicAlertFmtT("WAD installation failed: Could not initialise title import (error {0}).",
                     std::to_underlying(ret));
    }
    return false;
  }

  bool contents_ok = true;
  const u64 title_id = tmd.GetTitleId();
  for (const IOS::ES::Content& content : tmd.GetContents())
  {
    const std::vector<u8> data = wad.GetContent(content.index);
    const bool imported =
        es.ImportContentBegin(context, title_id, content.id) >= 0 &&
        es.ImportContentData(context, 0, data.data(), static_cast<u32>(data.size())) >= 0 &&
        es.ImportContentEnd(context, 0) >= 0;
    if (!imported)
    {
      PanicAlertFmtT("WAD installation failed: Could not import content {0:08x}.", content.id);
      contents_ok = false;
      break;
    }
  }

  const IOS::HLE::ReturnCode finish =
      contents_ok ? es.ImportTitleDone(context) : es.ImportTitleCancel(context);
  if (finish < 0)
  {
    PanicAlertFmtT("WAD installation failed: Could not finalise title import.");
    return false;
  }

  if (!EnsureShopLogs(*fs))
  {
    PanicAlertFmtT("WAD installation failed: Could not create Wii Shop log files.");
    return false;
  }
  return true;
}

std::shared_ptr<IOS::HLE::Device> FindBluetoothDevice()
{
  auto* const ios = Core::System::GetInstance().GetIOS();
  if (!ios)
    return nullptr;
  return ios->GetDeviceByName("/dev/usb/oh1/57e/305");
}
}  // namespace

bool InstallWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad, InstallType install_type)
{
  const IOS::ES::TMDReader& tmd = wad.GetTMD();
  if (!tmd.IsValid())
    return false;

  // IPL.TID holds the title installed temporarily last time (big-endian), or
  // 0. The SysConf object writes it back when this function returns.
  SysConf sysconf{ios.GetFS()};
  SysConf::Entry* const temp_tid = sysconf.GetOrAddEntry("IPL.TID", SysConf::Entry::Type::LongLong);
  const u64 previous_temp_title = Common::swap64(temp_tid->GetData<u64>(0));
  const u64 title_id = tmd.GetTitleId();
  ESCore& es = ios.GetESCore();

  // Nothing to do if exactly these contents are already installed, except
  // that a permanent install of the temporary title makes it permanent.
  const auto stored = es.GetStoredContentsFromTMD(tmd, ESCore::CheckContentHashes::Yes);
  if (stored == tmd.GetContents())
  {
    if (install_type == InstallType::Permanent && previous_temp_title == title_id)
      temp_tid->SetData<u64>(0);
    return true;
  }

  // Replacing a different installed version needs the user's consent.
  const IOS::ES::TMDReader installed = es.FindInstalledTMD(title_id);
  const bool replaces_other_version =
      installed.IsValid() && installed.GetTitleVersion() != tmd.GetTitleVersion();
  if (replaces_other_version &&
      !AskYesNoFmtT("A different version of this title is already installed on the "
                    "NAND.\n\nInstalled version: {0}\nWAD version: {1}\n\nInstalling "
                    "this WAD will replace it irreversibly. Continue?",
                    installed.GetTitleVersion(), tmd.GetTitleVersion()))
  {
    return false;
  }

  // Make room by removing the previous temporary title.
  if (previous_temp_title != 0)
    es.DeleteTitleContent(previous_temp_title);

  // Fakesigned WADs are common, so signatures are not checked here.
  if (!ImportWAD(ios, wad, ESCore::VerifySignature::No))
    return false;

  // Same bookkeeping the System Menu uses for titles run from the SD card.
  const bool remember = install_type == InstallType::Temporary && !replaces_other_version;
  temp_tid->SetData<u64>(remember ? Common::swap64(title_id) : 0);
  return true;
}

bool InstallWAD(const std::string& wad_path)
{
  if (const std::unique_ptr<DiscIO::VolumeWAD> wad = DiscIO::CreateWAD(wad_path))
  {
    IOS::HLE::Kernel ios;
    return InstallWAD(ios, *wad, InstallType::Permanent);
  }
  return false;
}

bool UninstallTitle(u64 title_id)
{
  IOS::HLE::Kernel ios;
  return ios.GetESCore().DeleteTitleContent(title_id) == IOS::HLE::IPC_SUCCESS;
}

bool IsTitleInstalled(u64 title_id)
{
  IOS::HLE::Kernel ios;
  const auto files = ios.GetFS()->ReadDirectory(0, 0, Common::GetTitleContentPath(title_id));
  if (!files)
    return false;

  // A cheap approximation of what IOS would do: anything in the content
  // folder besides the TMD counts as an installed title.
  return std::ranges::find_if(*files, [](const std::string& name) {
           return name != "title.tmd";
         }) != files->end();
}

bool IsTMDImported(IOS::HLE::FS::FileSystem& fs, u64 title_id)
{
  const auto files = fs.ReadDirectory(0, 0, Common::GetTitleContentPath(title_id));
  if (!files)
    return false;
  return std::ranges::find(*files, std::string("title.tmd")) != files->end();
}

IOS::ES::TMDReader FindBackupTMD(IOS::HLE::FS::FileSystem& fs, u64 title_id)
{
  const auto store = fs.OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL,
                                 "/title/00000001/00000002/data/tmds.sys", FS::Mode::Read);
  if (!store)
    return {};

  // tmds.sys is a sequence of records: a 32-byte descriptor (title ID at 0,
  // TMD size at 8, both big-endian), then the TMD padded to 32 bytes.
  for (;;)
  {
    std::array<u8, 32> descriptor;
    if (!store->Read(descriptor.data(), descriptor.size()))
      return {};

    const u64 record_title = Common::swap64(descriptor.data());
    const u32 tmd_size = Common::swap32(descriptor.data() + 8);
    if (record_title != title_id)
    {
      if (!store->Seek(Common::AlignUp(tmd_size, 32), FS::SeekMode::Current))
        return {};
      continue;
    }

    std::vector<u8> tmd(tmd_size);
    if (!store->Read(tmd.data(), tmd_size))
      return {};
    return IOS::ES::TMDReader(std::move(tmd));
  }
}

bool EnsureTMDIsImported(IOS::HLE::FS::FileSystem& fs, IOS::HLE::ESCore& es, u64 title_id)
{
  if (IsTMDImported(fs, title_id))
    return true;

  const IOS::ES::TMDReader backup = FindBackupTMD(fs, title_id);
  if (!backup.IsValid())
    return false;

  // Import as the System Menu.
  ESCore::Context context;
  context.uid = IOS::SYSMENU_UID;
  context.gid = IOS::SYSMENU_GID;
  if (es.ImportTmd(context, backup.GetBytes(), Titles::SYSTEM_MENU, IOS::ES::TITLE_TYPE_DEFAULT) !=
      IOS::HLE::IPC_SUCCESS)
  {
    return false;
  }
  return es.ImportTitleDone(context) == IOS::HLE::IPC_SUCCESS;
}

namespace
{
struct TitleInfo
{
  u64 id;
  u16 version;
};

// Base of both updaters: owns the IOS instance they install through.
class SystemUpdater
{
public:
  virtual ~SystemUpdater() = default;

protected:
  // The installed System Menu's region as a NUS region code, or "".
  std::string GetDeviceRegion();
  // The console's device ID in NUS form, or "".
  std::string GetDeviceId();

  IOS::HLE::Kernel m_ios;
};

std::string SystemUpdater::GetDeviceRegion()
{
  const IOS::ES::TMDReader menu = m_ios.GetESCore().FindInstalledTMD(Titles::SYSTEM_MENU);
  if (!menu.IsValid())
    return "";

  switch (menu.GetRegion())
  {
  case DiscIO::Region::NTSC_J:
    return "JPN";
  case DiscIO::Region::NTSC_U:
    return "USA";
  case DiscIO::Region::NTSC_K:
    return "KOR";
  case DiscIO::Region::PAL:
  case DiscIO::Region::Unknown:
  default:
    return "EUR";
  }
}

std::string SystemUpdater::GetDeviceId()
{
  u32 id;
  if (m_ios.GetESCore().GetDeviceId(&id) < 0)
    return "";
  // NUS expects the ID with bit 32 set, in decimal.
  return std::to_string(u64{1} << 32 | id);
}

// SOAP request for the system title list; DeviceId and RegionId are filled in.
constexpr const char* GET_SYSTEM_TITLES_REQUEST_PAYLOAD =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\"\n"
    "  xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"\n"
    "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
    "  <soapenv:Body>\n"
    "    <GetSystemUpdateRequest xmlns=\"urn:nus.wsapi.broadon.com\">\n"
    "      <Version>1.0</Version>\n"
    "      <MessageId>0</MessageId>\n"
    "      <DeviceId></DeviceId>\n"
    "      <RegionId></RegionId>\n"
    "    </GetSystemUpdateRequest>\n"
    "  </soapenv:Body>\n"
    "</soapenv:Envelope>\n";

// Updates from the network: the update server lists titles and versions, the
// CDN it names serves tickets, TMDs and contents.
class OnlineSystemUpdater final : public SystemUpdater
{
public:
  OnlineSystemUpdater(UpdateCallback update_callback, std::string region)
      : m_update_callback(std::move(update_callback)), m_requested_region(std::move(region))
  {
  }
  UpdateResult DoOnlineUpdate();

private:
  UpdateCallback m_update_callback;
  std::string m_requested_region;
  Common::HttpRequest m_http{std::chrono::minutes{3}};

  struct TitleList
  {
    std::string content_prefix_url;
    std::vector<TitleInfo> titles;
  };

  TitleList RequestTitleList();
  TitleList ParseTitleList(std::span<const u8> response) const;
  bool NeedsInstall(const TitleInfo& title);

  UpdateResult InstallTitle(const std::string& prefix_url, const TitleInfo& title,
                                   std::unordered_set<u64>* updated_titles);
  UpdateResult ImportContents(const std::string& prefix_url, const TitleInfo& title,
                              const IOS::ES::TMDReader& tmd, ESCore::Context& context);

  std::pair<IOS::ES::TMDReader, std::vector<u8>> FetchTMD(const std::string& prefix_url,
                                                             const TitleInfo& title);
  std::pair<std::vector<u8>, std::vector<u8>> FetchTicket(const std::string& prefix_url,
                                                             const TitleInfo& title);
  std::optional<std::vector<u8>> FetchContent(const std::string& prefix_url,
                                                 const TitleInfo& title, u32 cid);

};

OnlineSystemUpdater::TitleList
OnlineSystemUpdater::ParseTitleList(std::span<const u8> response) const
{
  pugi::xml_document doc;
  if (!doc.load_buffer(response.data(), response.size()))
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Could not parse response");
    return {};
  }

  // pugixml ignores XML namespaces, so the bare element name matches.
  const pugi::xml_node reply = doc.select_node("//GetSystemUpdateResponse").node();
  if (!reply)
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Could not find response node");
    return {};
  }

  if (const int error = reply.child("ErrorCode").text().as_int(); error != 0)
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Non-zero error code ({})", error);
    return {};
  }

  // The cached content URL is used (the uncached one the console uses is far
  // too slow), over plain HTTP since HTTPS would need a device certificate.
  TitleList parsed;
  parsed.content_prefix_url =
      ReplaceAll(reply.child("ContentPrefixURL").text().as_string(), "https://", "http://");
  if (parsed.content_prefix_url.empty())
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Empty content prefix URL");
    return {};
  }

  for (const pugi::xml_node& entry : reply.children("TitleVersion"))
  {
    TitleInfo& title = parsed.titles.emplace_back();
    title.id = std::stoull(entry.child("TitleId").text().as_string(), nullptr, 16);
    title.version = static_cast<u16>(entry.child("Version").text().as_uint());
  }
  return parsed;
}

bool OnlineSystemUpdater::NeedsInstall(const TitleInfo& title)
{
  // Up to date means: installed, at least the listed version, all contents present.
  const ESCore& es = m_ios.GetESCore();
  const IOS::ES::TMDReader installed = es.FindInstalledTMD(title.id);
  if (!installed.IsValid() || installed.GetTitleVersion() < title.version)
    return true;
  return es.GetStoredContentsFromTMD(installed).size() !=
         static_cast<size_t>(installed.GetNumContents());
}

OnlineSystemUpdater::TitleList OnlineSystemUpdater::RequestTitleList()
{
  pugi::xml_document doc;
  const bool parsed = static_cast<bool>(doc.load_string(GET_SYSTEM_TITLES_REQUEST_PAYLOAD));
  ASSERT(parsed);

  // The server only checks that the device ID is well-formed.
  const std::string device_id = GetDeviceId();
  const bool id_set = doc.select_node("//DeviceId").node().text().set(device_id.c_str());
  ASSERT(id_set);

  const std::string region = !m_requested_region.empty() ? m_requested_region : GetDeviceRegion();
  const bool region_set = doc.select_node("//RegionId").node().text().set(region.c_str());
  ASSERT(region_set);

  std::ostringstream body;
  doc.save(body);

  // Nintendo's own update server for the Wii is gone, but its CDN still works.
  // Unless the user configured a server, use the Dolphin-hosted replacement
  // endpoint, which lists the titles with CDN URLs.
  std::string server = Config::Get(Config::MAIN_WII_NUS_SHOP_URL);
  if (server.empty())
    server = "https://fakenus.dolphin-emu.org";

  const Common::HttpRequest::Response reply =
      m_http.Post(fmt::format("{}/nus/services/NetUpdateSOAP", server), body.str(),
                  {
                      {"SOAPAction", "urn:nus.wsapi.broadon.com/GetSystemUpdate"},
                      {"User-Agent", "wii libnup/1.0"},
                      {"Content-Type", "text/xml; charset=utf-8"},
                  });
  return reply ? ParseTitleList(*reply) : TitleList{};
}

UpdateResult OnlineSystemUpdater::DoOnlineUpdate()
{
  const TitleList listing = RequestTitleList();
  if (listing.titles.empty())
    return UpdateResult::ServerFailed;

  // Titles go in the server's order (boot2, System Menu, IOSes, channels);
  // any IOS a title needs is installed just before it.
  const size_t total = listing.titles.size();
  std::unordered_set<u64> updated;
  for (size_t index = 0; index < total; ++index)
  {
    const TitleInfo& title = listing.titles[index];
    if (!m_update_callback(index, total, title.id))
      return UpdateResult::Cancelled;

    const UpdateResult result = InstallTitle(listing.content_prefix_url, title, &updated);
    if (result != UpdateResult::Succeeded)
    {
      ERROR_LOG_FMT(CORE, "Failed to update {:016x} -- aborting update", title.id);
      return result;
    }

    m_update_callback(index + 1, total, title.id);
  }

  if (!updated.empty())
  {
    NOTICE_LOG_FMT(CORE, "Update finished - {} updates installed", updated.size());
    return UpdateResult::Succeeded;
  }
  NOTICE_LOG_FMT(CORE, "Update finished - Already up-to-date");
  return UpdateResult::AlreadyUpToDate;
}

// Downloads and imports the contents of `tmd` that are not on the NAND yet.
UpdateResult OnlineSystemUpdater::ImportContents(const std::string& prefix_url,
                                                 const TitleInfo& title,
                                                 const IOS::ES::TMDReader& tmd,
                                                 ESCore::Context& context)
{
  ESCore& es = m_ios.GetESCore();
  const std::vector<IOS::ES::Content> present = es.GetStoredContentsFromTMD(tmd);
  for (const IOS::ES::Content& content : tmd.GetContents())
  {
    if (Common::Contains(present, content.id, &IOS::ES::Content::id))
      continue;

    if (const IOS::HLE::ReturnCode ret = es.ImportContentBegin(context, title.id, content.id);
        ret < 0)
    {
      ERROR_LOG_FMT(CORE, "Failed to initialise import for content {:08x}: error {}", content.id,
                    std::to_underlying(ret));
      return UpdateResult::ImportFailed;
    }

    const std::optional<std::vector<u8>> data = FetchContent(prefix_url, title, content.id);
    if (!data)
    {
      ERROR_LOG_FMT(CORE, "Failed to download content {:08x}", content.id);
      return UpdateResult::DownloadFailed;
    }

    const bool written =
        es.ImportContentData(context, 0, data->data(), static_cast<u32>(data->size())) >= 0 &&
        es.ImportContentEnd(context, 0) >= 0;
    if (!written)
    {
      ERROR_LOG_FMT(CORE, "Failed to import content {:08x}", content.id);
      return UpdateResult::ImportFailed;
    }
  }
  return UpdateResult::Succeeded;
}

UpdateResult OnlineSystemUpdater::InstallTitle(const std::string& prefix_url,
                                                      const TitleInfo& title,
                                                      std::unordered_set<u64>* updated_titles)
{
  // boot2 updates are not supported; treat them as done.
  if (title.id == Titles::BOOT2)
    return UpdateResult::Succeeded;
  if (!NeedsInstall(title) || updated_titles->contains(title.id))
    return UpdateResult::Succeeded;

  NOTICE_LOG_FMT(CORE, "Updating title {:016x}", title.id);

  const auto [ticket, ticket_certs] = FetchTicket(prefix_url, title);
  if (ticket.empty() || ticket_certs.empty())
  {
    ERROR_LOG_FMT(CORE, "Failed to download ticket and certs");
    return UpdateResult::DownloadFailed;
  }

  ESCore& es = m_ios.GetESCore();
  if (const IOS::HLE::ReturnCode ret = es.ImportTicket(ticket, ticket_certs); ret < 0)
  {
    ERROR_LOG_FMT(CORE, "Failed to import ticket: error {}", std::to_underlying(ret));
    return UpdateResult::ImportFailed;
  }

  const auto [tmd, tmd_certs] = FetchTMD(prefix_url, title);
  if (!tmd.IsValid())
  {
    ERROR_LOG_FMT(CORE, "Failed to download TMD");
    return UpdateResult::DownloadFailed;
  }

  // A missing system title (the IOS this title runs on) goes in first.
  const u64 ios_id = tmd.GetIOSId();
  const bool needs_ios = ios_id != 0 && IOS::ES::IsTitleType(ios_id, IOS::ES::TitleType::System) &&
                         !es.FindInstalledTMD(ios_id).IsValid();
  if (needs_ios)
  {
    WARN_LOG_FMT(CORE, "Importing required system title {:016x} first", ios_id);
    const UpdateResult ios_result = InstallTitle(prefix_url, {ios_id, 0}, updated_titles);
    if (ios_result != UpdateResult::Succeeded)
    {
      ERROR_LOG_FMT(CORE, "Failed to import required system title {:016x}", ios_id);
      return ios_result;
    }
  }

  ESCore::Context context;
  if (const IOS::HLE::ReturnCode ret = es.ImportTitleInit(context, tmd.GetBytes(), tmd_certs);
      ret < 0)
  {
    ERROR_LOG_FMT(CORE, "Failed to initialise title import: error {}", std::to_underlying(ret));
    return UpdateResult::ImportFailed;
  }

  const UpdateResult contents_result = ImportContents(prefix_url, title, tmd, context);
  const bool contents_ok = contents_result == UpdateResult::Succeeded;

  const IOS::HLE::ReturnCode finish =
      contents_ok ? es.ImportTitleDone(context) : es.ImportTitleCancel(context);
  if (finish < 0)
  {
    ERROR_LOG_FMT(CORE, "Failed to finalise title import: error {}", std::to_underlying(finish));
    return UpdateResult::ImportFailed;
  }
  if (!contents_ok)
    return contents_result;

  updated_titles->insert(title.id);
  return UpdateResult::Succeeded;
}

std::pair<IOS::ES::TMDReader, std::vector<u8>>
OnlineSystemUpdater::FetchTMD(const std::string& prefix_url, const TitleInfo& title)
{
  // Version 0 stands for "latest".
  std::string url = fmt::format("{}/{:016x}/tmd", prefix_url, title.id);
  if (title.version != 0)
    url += fmt::format(".{}", title.version);

  const Common::HttpRequest::Response response = m_http.Get(url);
  if (!response)
    return {};

  // The download is the TMD followed by its certificate chain; it must hold
  // more than the TMD.
  const std::vector<u8>& bytes = *response;
  if (bytes.size() <= sizeof(IOS::ES::TMDHeader))
    return {};
  const u16 content_count =
      Common::swap16(bytes.data() + offsetof(IOS::ES::TMDHeader, num_contents));
  const size_t tmd_size = sizeof(IOS::ES::TMDHeader) + sizeof(IOS::ES::Content) * content_count;
  if (bytes.size() <= tmd_size)
    return {};

  const auto split = bytes.begin() + static_cast<std::ptrdiff_t>(tmd_size);
  return {IOS::ES::TMDReader(std::vector<u8>(bytes.begin(), split)),
          std::vector<u8>(split, bytes.end())};
}

std::pair<std::vector<u8>, std::vector<u8>>
OnlineSystemUpdater::FetchTicket(const std::string& prefix_url, const TitleInfo& title)
{
  const Common::HttpRequest::Response response =
      m_http.Get(fmt::format("{}/{:016x}/cetk", prefix_url, title.id));
  if (!response)
    return {};

  // The ticket, then its certificate chain.
  const std::vector<u8>& bytes = *response;
  constexpr size_t TICKET_SIZE = sizeof(IOS::ES::Ticket);
  if (bytes.size() <= TICKET_SIZE)
    return {};

  const auto split = bytes.begin() + static_cast<std::ptrdiff_t>(TICKET_SIZE);
  return {std::vector<u8>(bytes.begin(), split), std::vector<u8>(split, bytes.end())};
}

std::optional<std::vector<u8>> OnlineSystemUpdater::FetchContent(const std::string& prefix_url,
                                                                    const TitleInfo& title, u32 cid)
{
  return m_http.Get(fmt::format("{}/{:016x}/{:08x}", prefix_url, title.id, cid));
}

// Updates from a disc's update partition, driven by its __update.inf manifest.
class DiscSystemUpdater final : public SystemUpdater
{
public:
  DiscSystemUpdater(UpdateCallback update_callback, const std::string& image_path)
      : m_update_callback{std::move(update_callback)}, m_volume{DiscIO::CreateDisc(image_path)}
  {
  }
  UpdateResult DoDiscUpdate();

private:
  UpdateCallback m_update_callback;
  std::unique_ptr<DiscIO::VolumeDisc> m_volume;
  DiscIO::Partition m_partition;
  // The update partition, once found.
  // Manifest layout: a 32-byte header, then 512-byte big-endian entries.
#pragma pack(push, 1)
  struct ManifestHeader
  {
    char timestamp[0x10];  // "YYYY/MM/DD"
    // Newer manifests keep an entry count somewhere in here, but not at a
    // fixed place, so the count comes from the file size instead.
    u32 padding[4];
  };
  static_assert(sizeof(ManifestHeader) == 32, "Wrong size");

  struct Entry
  {
    u32 type, attribute, unknown1, unknown2;
    char path[0x40];
    u64 title_id;
    u16 title_version, unused1[3];
    char name[0x40], info[0x40];
    u8 unused2[0x120];
  };
  static_assert(sizeof(Entry) == 512, "Wrong size");
#pragma pack(pop)

  UpdateResult InstallFromManifest(std::string_view manifest_name);
  UpdateResult InstallEntry(u32 type, std::bitset<32> attrs, const TitleInfo& title,
                            std::string_view path);

};

UpdateResult DiscSystemUpdater::DoDiscUpdate()
{
  if (!m_volume)
    return UpdateResult::DiscReadFailed;

  // An update changes the console's region, so a disc for another region than
  // the installed System Menu could brick it.
  const IOS::ES::TMDReader menu = m_ios.GetESCore().FindInstalledTMD(Titles::SYSTEM_MENU);
  if (menu.IsValid() && m_volume->GetRegion() != menu.GetRegion())
    return UpdateResult::RegionMismatch;

  const std::vector<DiscIO::Partition> partitions = m_volume->GetPartitions();
  const auto is_update = [this](const DiscIO::Partition& partition) {
    return m_volume->GetPartitionType(partition) == DiscIO::PARTITION_UPDATE;
  };
  const auto found = std::ranges::find_if(partitions, is_update);
  if (found == partitions.end())
  {
    ERROR_LOG_FMT(CORE, "Could not find any update partition");
    return UpdateResult::MissingUpdatePartition;
  }

  m_partition = *found;
  return InstallFromManifest("__update.inf");
}

UpdateResult DiscSystemUpdater::InstallFromManifest(std::string_view manifest_name)
{
  const DiscIO::FileSystem* const disc_fs = m_volume->GetFileSystem(m_partition);
  if (!disc_fs)
  {
    ERROR_LOG_FMT(CORE, "Could not read the update partition file system");
    return UpdateResult::DiscReadFailed;
  }

  const std::unique_ptr<DiscIO::FileInfo> manifest = disc_fs->FindFileInfo(manifest_name);
  const auto entries_bytes = [&] { return manifest->GetSize() - sizeof(ManifestHeader); };
  if (!manifest || entries_bytes() % sizeof(Entry) != 0)
  {
    ERROR_LOG_FMT(CORE, "Invalid or missing update manifest");
    return UpdateResult::DiscReadFailed;
  }

  const u32 count = static_cast<u32>(entries_bytes() / sizeof(Entry));
  if (count > 200)
    return UpdateResult::DiscReadFailed;

  std::vector<u8> raw(sizeof(Entry));
  size_t installed = 0;
  for (u32 index = 0; index < count; ++index)
  {
    const u64 offset = sizeof(ManifestHeader) + sizeof(Entry) * index;
    const u64 read = DiscIO::ReadFile(*m_volume, m_partition, manifest.get(), raw.data(),
                                      raw.size(), offset);
    if (read != raw.size())
    {
      ERROR_LOG_FMT(CORE, "Failed to read update information from update manifest");
      return UpdateResult::DiscReadFailed;
    }

    const u8* const fields = raw.data();
    const u32 type = Common::swap32(fields + offsetof(Entry, type));
    const std::bitset<32> attrs = Common::swap32(fields + offsetof(Entry, attribute));
    const TitleInfo title{Common::swap64(fields + offsetof(Entry, title_id)),
                          Common::swap16(fields + offsetof(Entry, title_version))};
    const char* const path_chars = reinterpret_cast<const char*>(fields + offsetof(Entry, path));
    const std::string_view path{path_chars, strnlen(path_chars, sizeof(Entry::path))};

    if (!m_update_callback(index, count, title.id))
      return UpdateResult::Cancelled;

    const UpdateResult result = InstallEntry(type, attrs, title, path);
    if (result == UpdateResult::Succeeded)
    {
      ++installed;
    }
    else if (result != UpdateResult::AlreadyUpToDate)
    {
      ERROR_LOG_FMT(CORE, "Failed to update {:016x} -- aborting update", title.id);
      return result;
    }
  }
  return installed != 0 ? UpdateResult::Succeeded : UpdateResult::AlreadyUpToDate;
}

UpdateResult DiscSystemUpdater::InstallEntry(u32 type, std::bitset<32> attrs,
                                             const TitleInfo& title, std::string_view path)
{
  // Only these entry types are installed; others (boot2 among them) are skipped.
  constexpr std::array<u32, 4> HANDLED_TYPES{2, 3, 6, 7};
  if (!Common::Contains(HANDLED_TYPES, type))
    return UpdateResult::AlreadyUpToDate;

  const IOS::ES::TMDReader installed = m_ios.GetESCore().FindInstalledTMD(title.id);
  const IOS::ES::TicketReader ticket = m_ios.GetESCore().FindSignedTicket(title.id);

  // Bit 16 marks an optional title: having its ticket is enough, installed or not.
  if (attrs.test(16) && ticket.IsValid())
    return UpdateResult::AlreadyUpToDate;

  // Otherwise skip only if a new enough version is installed. (Unlike the
  // online updater, contents are not checked.)
  if (installed.IsValid() && installed.GetTitleVersion() >= title.version)
    return UpdateResult::AlreadyUpToDate;

  auto blob = DiscIO::VolumeFileBlobReader::Create(*m_volume, m_partition, path);
  if (!blob)
  {
    ERROR_LOG_FMT(CORE, "Could not find {}", path);
    return UpdateResult::DiscReadFailed;
  }
  const DiscIO::VolumeWAD wad{std::move(blob)};
  if (!ImportWAD(m_ios, wad, ESCore::VerifySignature::Yes))
    return UpdateResult::ImportFailed;
  return UpdateResult::Succeeded;
}

// Records a problem: fixed right away when repairing, otherwise flagged.
void Flag(NANDCheckResult& result, bool repair, const std::function<void()>& fix)
{
  if (repair)
    fix();
  else
    result.bad = true;
}

NANDCheckResult ScanNAND(IOS::HLE::Kernel& ios, bool repair)
{
  NANDCheckResult result;
  const ESCore& es = ios.GetESCore();

  // A sys/replace file means the NAND was used by very old builds and is
  // probably damaged.
  const std::string replace_file =
      Common::RootUserPath(Common::FromWhichRoot::Configured) + "/sys/replace";
  if (File::Exists(replace_file))
  {
    ERROR_LOG_FMT(CORE,
                  "CheckNAND: NAND was used with old versions, so it is likely to be damaged");
    Flag(result, repair, [&] { File::Delete(replace_file); });
  }

  // An empty Mii database is left behind by an old bug.
  const std::string mii_db = Common::GetMiiDatabasePath(Common::FromWhichRoot::Configured);
  if (const File::FileInfo info(mii_db); info.Exists() && info.GetSize() == 0)
  {
    ERROR_LOG_FMT(CORE, "CheckNAND: RFL_DB.dat exists but is empty");
    Flag(result, repair, [&] { File::Delete(mii_db); });
  }

  for (const u64 title_id : es.GetInstalledTitles())
  {
    const std::string title_dir = Common::GetTitlePath(title_id, Common::FromWhichRoot::Configured);
    const std::string content_dir = title_dir + "/content";
    const std::string data_dir = title_dir + "/data";
    const auto remove_title = [&] {
      result.titles_to_remove.insert(title_id);
      Flag(result, repair, [&] { File::DeleteDirRecursively(title_dir); });
    };

    for (const std::string& dir : {content_dir, data_dir})
    {
      if (!File::IsDirectory(dir))
      {
        ERROR_LOG_FMT(CORE, "CheckNAND: Missing dir {} for title {:016x}", dir, title_id);
        Flag(result, repair, [&] { File::CreateDir(dir); });
      }
    }

    // Titles that are not disc titles need a ticket.
    if (!IOS::ES::IsDiscTitle(title_id) && !es.FindSignedTicket(title_id).IsValid())
    {
      ERROR_LOG_FMT(CORE, "CheckNAND: Missing ticket for title {:016x}", title_id);
      remove_title();
    }

    const IOS::ES::TMDReader tmd = es.FindInstalledTMD(title_id);
    if (!tmd.IsValid())
    {
      // Without a TMD only an empty content folder is harmless.
      if (!File::ScanDirectoryTree(content_dir, false).children.empty())
      {
        ERROR_LOG_FMT(CORE, "CheckNAND: Missing TMD for title {:016x}", title_id);
        remove_title();
      }
      else
      {
        WARN_LOG_FMT(CORE, "CheckNAND: Missing TMD for title {:016x}", title_id);
      }
      continue;
    }

    // A title with any of its own (non-shared) contents installed must have
    // all of them, unless it is a data title.
    const std::vector<IOS::ES::Content> present = es.GetStoredContentsFromTMD(tmd);
    const bool has_own_content =
        std::ranges::find_if(present, [](const IOS::ES::Content& c) { return !c.IsShared(); }) !=
        present.end();
    const bool is_data_title = (tmd.GetTitleFlags() & IOS::ES::TitleFlags::TITLE_TYPE_DATA) != 0;
    if (has_own_content && !is_data_title && present != tmd.GetContents())
    {
      ERROR_LOG_FMT(CORE, "CheckNAND: Missing contents for title {:016x}", title_id);
      remove_title();
    }
  }

  // Usage figures. The System Menu counts only some folders as user space when
  // it shows free blocks, so report those apart from everything else.
  const auto fs = ios.GetFS();
  const auto whole_nand = fs->GetExtendedDirectoryStats("/");
  constexpr std::array<const char*, 10> USER_FOLDERS{
      "/meta",           "/ticket",         "/title/00010000", "/title/00010001",
      "/title/00010003", "/title/00010004", "/title/00010005", "/title/00010006",
      "/title/00010007", "/shared2/title"};
  for (const std::string folder : USER_FOLDERS)
  {
    if (const auto stats = fs->GetExtendedDirectoryStats(folder))
    {
      result.used_clusters_user += stats->used_clusters;
      result.used_inodes_user += stats->used_inodes;
    }
  }
  if (whole_nand)
  {
    result.used_clusters_system = whole_nand->used_clusters - result.used_clusters_user;
    result.used_inodes_system = whole_nand->used_inodes - result.used_inodes_user;
  }
  return result;
}
}  // namespace

UpdateResult DoOnlineUpdate(UpdateCallback update_callback, const std::string& region)
{
  OnlineSystemUpdater updater{std::move(update_callback), region};
  return updater.DoOnlineUpdate();
}

UpdateResult DoDiscUpdate(UpdateCallback update_callback, const std::string& image_path)
{
  DiscSystemUpdater updater{std::move(update_callback), image_path};
  return updater.DoDiscUpdate();
}

NANDCheckResult CheckNAND(IOS::HLE::Kernel& ios)
{
  return ScanNAND(ios, false);
}

bool RepairNAND(IOS::HLE::Kernel& ios)
{
  return !ScanNAND(ios, true).bad;
}

std::shared_ptr<IOS::HLE::BluetoothEmuDevice> GetBluetoothEmuDevice()
{
  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    return nullptr;
  return std::static_pointer_cast<IOS::HLE::BluetoothEmuDevice>(FindBluetoothDevice());
}

std::shared_ptr<IOS::HLE::BluetoothRealDevice> GetBluetoothRealDevice()
{
  if (!Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    return nullptr;
  return std::static_pointer_cast<IOS::HLE::BluetoothRealDevice>(FindBluetoothDevice());
}
}  // namespace WiiUtils
