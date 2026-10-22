// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// ES (the "e-ticket services" module): title installation and removal, tickets and TMDs,
// content access, and title launching. ESCore holds the logic that kernel code (and WAD
// installation outside of emulation) can use directly; ESDevice is /dev/es on top of it.

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"

class PointerWrap;

namespace CoreTiming
{
class CoreTimingManager;
}
namespace DiscIO
{
enum class Platform;
}

namespace IOS::HLE
{
// The title ES currently considers running (set on launch or DI verify).
struct TitleContext
{
  void Clear();
  void DoState(PointerWrap& p);
  void Update(const ES::TMDReader& tmd_, const ES::TicketReader& ticket_,
              DiscIO::Platform platform);

  ES::TicketReader ticket;
  ES::TMDReader tmd;
  bool active = false;
  bool first_change = true;
};

class ESDevice;

class ESCore final
{
public:
  explicit ESCore(Kernel& ios);
  ESCore(const ESCore& other) = delete;
  ESCore(ESCore&& other) = delete;
  ESCore& operator=(const ESCore& other) = delete;
  ESCore& operator=(ESCore&& other) = delete;
  ~ESCore();

  // State of an import or export that is in progress on one ES handle.
  struct TitleImportExportContext
  {
    void DoState(PointerWrap& p);

    bool valid = false;
    ES::TMDReader tmd;
    IOSC::Handle key_handle = 0;
    struct ContentContext
    {
      bool valid = false;
      u32 id = 0;
      std::array<u8, 16> iv{};
      std::vector<u8> buffer;
    };
    ContentContext content;
  };

  // Per-handle state.
  struct Context
  {
    void DoState(PointerWrap& p);

    u16 gid = 0;
    u32 uid = 0;
    TitleImportExportContext title_import_export;
    bool active = false;
    // The IPC fd this context belongs to.
    s32 ipc_fd = -1;
  };

  ES::TMDReader FindImportTMD(u64 title_id, Ticks ticks = {}) const;
  ES::TMDReader FindInstalledTMD(u64 title_id, Ticks ticks = {}) const;
  ES::TicketReader FindSignedTicket(u64 title_id,
                                    std::optional<u8> desired_version = std::nullopt) const;

  // Title IDs that have a directory under /title (TMDs are not checked).
  std::vector<u64> GetInstalledTitles() const;
  // Title IDs that have a directory under /import (TMDs are not checked).
  std::vector<u64> GetTitleImports() const;
  // Title IDs that have a ticket under /ticket.
  std::vector<u64> GetTitlesWithTickets() const;

  enum class CheckContentHashes : bool
  {
    Yes = true,
    No = false,
  };

  std::vector<ES::Content>
  GetStoredContentsFromTMD(const ES::TMDReader& tmd,
                           CheckContentHashes check_content_hashes = CheckContentHashes::No) const;
  u32 GetSharedContentsCount() const;
  std::vector<std::array<u8, 20>> GetSharedContents() const;

  // Reading title contents
  s32 OpenContent(const ES::TMDReader& tmd, u16 content_index, u32 uid, Ticks ticks = {});
  s32 CloseContent(u32 cfd, u32 uid, Ticks ticks = {});
  s32 ReadContent(u32 cfd, u8* buffer, u32 size, u32 uid, Ticks ticks = {});
  s32 SeekContent(u32 cfd, u32 offset, SeekMode mode, u32 uid, Ticks ticks = {});

  // Installing and removing titles
  enum class TicketImportType
  {
    // Decrypt a console-specific title key with this console's key if the ticket needs it.
    PossiblyPersonalised,
    // Never treat the ticket as console-specific.
    Unpersonalised,
  };
  enum class VerifySignature
  {
    No,
    Yes,
  };
  ReturnCode ImportTicket(const std::vector<u8>& ticket_bytes, const std::vector<u8>& cert_chain,
                          TicketImportType type = TicketImportType::PossiblyPersonalised,
                          VerifySignature verify_signature = VerifySignature::Yes);
  ReturnCode ImportTmd(Context& context, const std::vector<u8>& tmd_bytes, u64 caller_title_id,
                       u32 caller_title_flags);
  ReturnCode ImportTitleInit(Context& context, const std::vector<u8>& tmd_bytes,
                             const std::vector<u8>& cert_chain,
                             VerifySignature verify_signature = VerifySignature::Yes);
  ReturnCode ImportContentBegin(Context& context, u64 title_id, u32 content_id);
  ReturnCode ImportContentData(Context& context, u32 content_fd, const u8* data, u32 data_size);
  ReturnCode ImportContentEnd(Context& context, u32 content_fd);
  ReturnCode ImportTitleDone(Context& context);
  ReturnCode ImportTitleCancel(Context& context);
  ReturnCode ExportTitleInit(Context& context, u64 title_id, u8* tmd, u32 tmd_size,
                             u64 caller_title_id, u32 caller_title_flags);
  ReturnCode ExportContentBegin(Context& context, u64 title_id, u32 content_id);
  ReturnCode ExportContentData(Context& context, u32 content_fd, u8* data, u32 data_size);
  ReturnCode ExportContentEnd(Context& context, u32 content_fd);
  ReturnCode ExportTitleDone(Context& context);
  ReturnCode DeleteTitle(u64 title_id);
  ReturnCode DeleteTitleContent(u64 title_id) const;
  ReturnCode DeleteTicket(const u8* ticket_view);
  ReturnCode DeleteSharedContent(const std::array<u8, 20>& sha1) const;
  ReturnCode DeleteContent(u64 title_id, u32 content_id) const;

  ReturnCode GetDeviceId(u32* device_id) const;
  ReturnCode GetTitleId(u64* device_id) const;

  ReturnCode VerifySign(const std::vector<u8>& hash, const std::vector<u8>& ecc_signature,
                        const std::vector<u8>& certs);

  // Tickets from views
  ReturnCode GetTicketFromView(const u8* ticket_view, u8* ticket, u32* ticket_size,
                               std::optional<u8> desired_version) const;

  ReturnCode SetUpStreamKey(u32 uid, const u8* ticket_view, const ES::TMDReader& tmd, u32* handle);

  bool CreateTitleDirectories(u64 title_id, u16 group_id) const;

  enum class VerifyContainerType
  {
    TMD,
    Ticket,
    Device,
  };
  enum class VerifyMode
  {
    // Whether certificates that were used get appended to /sys/cert.sys.
    DoNotUpdateCertStore,
    UpdateCertStore,
  };
  // Checks a signed blob against its issuer chain. When issuer_handle is given and the check
  // passes, it receives the IOSC object holding the issuer's key; the caller must delete it.
  ReturnCode VerifyContainer(VerifyContainerType type, VerifyMode mode,
                             const ES::SignedBlobReader& signed_blob,
                             const std::vector<u8>& cert_chain, u32* issuer_handle = nullptr);
  ReturnCode VerifyContainer(VerifyContainerType type, VerifyMode mode,
                             const ES::CertReader& certificate, const std::vector<u8>& cert_chain,
                             u32 certificate_iosc_handle);

private:
  // Prepares /import for a title.
  bool InitImport(const ES::TMDReader& tmd);
  // Moves an import from /import back into /title.
  bool FinishImport(const ES::TMDReader& tmd);
  // Writes the TMD of an import (atomically).
  bool WriteImportTMD(const ES::TMDReader& tmd);
  // Completes or discards imports left over from a previous run.
  void FinishStaleImport(u64 title_id);
  void FinishAllStaleImports();

  std::string GetContentPath(u64 title_id, const ES::Content& content, Ticks ticks = {}) const;

  bool IsActiveTitlePermittedByTicket(const u8* ticket_view) const;

  bool IsIssuerCorrect(VerifyContainerType type, const ES::CertReader& issuer_cert) const;
  ReturnCode ReadCertStore(std::vector<u8>* buffer) const;
  ReturnCode WriteNewCertToStore(const ES::CertReader& cert);

  ReturnCode CheckStreamKeyPermissions(u32 uid, const u8* ticket_view,
                                       const ES::TMDReader& tmd) const;

  struct OpenedContent
  {
    bool m_opened = false;
    u64 m_fd = 0;
    u64 m_title_id = 0;
    ES::Content m_content{};
    u32 m_uid = 0;
  };

  Kernel& m_ios;

  using ContentTable = std::array<OpenedContent, 16>;
  ContentTable m_content_table;

  TitleContext m_title_context{};

  friend class ESDevice;
};

class ESDevice final : public EmulationDevice
{
public:
  ESDevice(EmulationKernel& ios, ESCore& core, const std::string& device_name);
  ESDevice(const ESDevice& other) = delete;
  ESDevice(ESDevice&& other) = delete;
  ESDevice& operator=(const ESDevice& other) = delete;
  ESDevice& operator=(ESDevice&& other) = delete;
  ~ESDevice() override;

  static void InitializeEmulationState(CoreTiming::CoreTimingManager& core_timing);
  static void FinalizeEmulationState();

  ReturnCode DIVerify(const ES::TMDReader& tmd, const ES::TicketReader& ticket);
  bool LaunchTitle(u64 title_id, HangPPC hang_ppc = HangPPC::No);

  void DoState(PointerWrap& p) override;

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> Close(u32 fd) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  ESCore& GetCore() const { return m_core; }

private:
  // ioctlv numbers (WiiBrew, "/dev/es").
  enum
  {
    IOCTL_ES_ADDTICKET = 0x01,
    IOCTL_ES_ADDTITLESTART = 0x02,
    IOCTL_ES_ADDCONTENTSTART = 0x03,
    IOCTL_ES_ADDCONTENTDATA = 0x04,
    IOCTL_ES_ADDCONTENTFINISH = 0x05,
    IOCTL_ES_ADDTITLEFINISH = 0x06,
    IOCTL_ES_GETDEVICEID = 0x07,
    IOCTL_ES_LAUNCH = 0x08,
    IOCTL_ES_OPEN_ACTIVE_TITLE_CONTENT = 0x09,
    IOCTL_ES_READCONTENT = 0x0A,
    IOCTL_ES_CLOSECONTENT = 0x0B,
    IOCTL_ES_GETOWNEDTITLECNT = 0x0C,
    IOCTL_ES_GETOWNEDTITLES = 0x0D,
    IOCTL_ES_GETTITLECNT = 0x0E,
    IOCTL_ES_GETTITLES = 0x0F,
    IOCTL_ES_GETTITLECONTENTSCNT = 0x10,
    IOCTL_ES_GETTITLECONTENTS = 0x11,
    IOCTL_ES_GETVIEWCNT = 0x12,
    IOCTL_ES_GETVIEWS = 0x13,
    IOCTL_ES_GETTMDVIEWCNT = 0x14,
    IOCTL_ES_GETTMDVIEWS = 0x15,
    IOCTL_ES_GETCONSUMPTION = 0x16,
    IOCTL_ES_DELETETITLE = 0x17,
    IOCTL_ES_DELETETICKET = 0x18,
    IOCTL_ES_DIGETTMDVIEWSIZE = 0x19,
    IOCTL_ES_DIGETTMDVIEW = 0x1A,
    IOCTL_ES_DIGETTICKETVIEW = 0x1B,
    IOCTL_ES_DIVERIFY = 0x1C,
    IOCTL_ES_GETTITLEDIR = 0x1D,
    IOCTL_ES_GETDEVICECERT = 0x1E,
    IOCTL_ES_IMPORTBOOT = 0x1F,
    IOCTL_ES_GETTITLEID = 0x20,
    IOCTL_ES_SETUID = 0x21,
    IOCTL_ES_DELETETITLECONTENT = 0x22,
    IOCTL_ES_SEEKCONTENT = 0x23,
    IOCTL_ES_OPENCONTENT = 0x24,
    IOCTL_ES_LAUNCHBC = 0x25,
    IOCTL_ES_EXPORTTITLEINIT = 0x26,
    IOCTL_ES_EXPORTCONTENTBEGIN = 0x27,
    IOCTL_ES_EXPORTCONTENTDATA = 0x28,
    IOCTL_ES_EXPORTCONTENTEND = 0x29,
    IOCTL_ES_EXPORTTITLEDONE = 0x2A,
    IOCTL_ES_ADDTMD = 0x2B,
    IOCTL_ES_ENCRYPT = 0x2C,
    IOCTL_ES_DECRYPT = 0x2D,
    IOCTL_ES_GETBOOT2VERSION = 0x2E,
    IOCTL_ES_ADDTITLECANCEL = 0x2F,
    IOCTL_ES_SIGN = 0x30,
    IOCTL_ES_VERIFYSIGN = 0x31,
    IOCTL_ES_GETSTOREDCONTENTCNT = 0x32,
    IOCTL_ES_GETSTOREDCONTENTS = 0x33,
    IOCTL_ES_GETSTOREDTMDSIZE = 0x34,
    IOCTL_ES_GETSTOREDTMD = 0x35,
    IOCTL_ES_GETSHAREDCONTENTCNT = 0x36,
    IOCTL_ES_GETSHAREDCONTENTS = 0x37,
    IOCTL_ES_DELETESHAREDCONTENT = 0x38,
    IOCTL_ES_DIGETTMDSIZE = 0x39,
    IOCTL_ES_DIGETTMD = 0x3A,
    IOCTL_ES_DIVERIFY_WITH_VIEW = 0x3B,
    IOCTL_ES_SET_UP_STREAM_KEY = 0x3C,
    IOCTL_ES_DELETE_STREAM_KEY = 0x3D,
    IOCTL_ES_DELETE_CONTENT = 0x3E,
    IOCTL_ES_INVALID_3F = 0x3F,
    IOCTL_ES_GET_V0_TICKET_FROM_VIEW = 0x40,
    IOCTL_ES_UNKNOWN_41 = 0x41,
    IOCTL_ES_UNKNOWN_42 = 0x42,
    IOCTL_ES_GET_TICKET_SIZE_FROM_VIEW = 0x43,
    IOCTL_ES_GET_TICKET_FROM_VIEW = 0x44,
    IOCTL_ES_CHECKKOREAREGION = 0x45,
  };

  // At most three ES handles can be open at once.
  using Context = ESCore::Context;
  using ContextArray = std::array<Context, 3>;

  // Installing and removing titles
  IPCReply ImportTicket(const IOCtlVRequest& request);
  IPCReply ImportTmd(Context& context, const IOCtlVRequest& request);
  IPCReply ImportTitleInit(Context& context, const IOCtlVRequest& request);
  IPCReply ImportContentBegin(Context& context, const IOCtlVRequest& request);
  IPCReply ImportContentData(Context& context, const IOCtlVRequest& request);
  IPCReply ImportContentEnd(Context& context, const IOCtlVRequest& request);
  IPCReply ImportTitleDone(Context& context, const IOCtlVRequest& request);
  IPCReply ImportTitleCancel(Context& context, const IOCtlVRequest& request);
  IPCReply ExportTitleInit(Context& context, const IOCtlVRequest& request);
  IPCReply ExportContentBegin(Context& context, const IOCtlVRequest& request);
  IPCReply ExportContentData(Context& context, const IOCtlVRequest& request);
  IPCReply ExportContentEnd(Context& context, const IOCtlVRequest& request);
  IPCReply ExportTitleDone(Context& context, const IOCtlVRequest& request);
  IPCReply DeleteTitle(const IOCtlVRequest& request);
  IPCReply DeleteTitleContent(const IOCtlVRequest& request);
  IPCReply DeleteTicket(const IOCtlVRequest& request);
  IPCReply DeleteSharedContent(const IOCtlVRequest& request);
  IPCReply DeleteContent(const IOCtlVRequest& request);

  // Console identity and crypto
  IPCReply GetDeviceId(const IOCtlVRequest& request);
  IPCReply GetDeviceCertificate(const IOCtlVRequest& request);
  IPCReply CheckKoreaRegion(const IOCtlVRequest& request);
  IPCReply Sign(const IOCtlVRequest& request);
  IPCReply VerifySign(const IOCtlVRequest& request);
  IPCReply Encrypt(u32 uid, const IOCtlVRequest& request);
  IPCReply Decrypt(u32 uid, const IOCtlVRequest& request);

  // Other
  IPCReply SetUID(u32 uid, const IOCtlVRequest& request);
  IPCReply GetTitleDirectory(const IOCtlVRequest& request);
  IPCReply GetTitleId(const IOCtlVRequest& request);
  IPCReply GetConsumption(const IOCtlVRequest& request);
  std::optional<IPCReply> Launch(const IOCtlVRequest& request);
  std::optional<IPCReply> LaunchBC(const IOCtlVRequest& request);
  IPCReply DIVerify(const IOCtlVRequest& request);
  IPCReply SetUpStreamKey(const Context& context, const IOCtlVRequest& request);
  IPCReply DeleteStreamKey(const IOCtlVRequest& request);

  // Reading title contents
  IPCReply OpenActiveTitleContent(u32 uid, const IOCtlVRequest& request);
  IPCReply OpenContent(u32 uid, const IOCtlVRequest& request);
  IPCReply ReadContent(u32 uid, const IOCtlVRequest& request);
  IPCReply CloseContent(u32 uid, const IOCtlVRequest& request);
  IPCReply SeekContent(u32 uid, const IOCtlVRequest& request);

  // Title listings
  IPCReply GetTitleCount(std::span<const u64> titles, const IOCtlVRequest& request);
  IPCReply GetTitles(std::span<const u64> titles, const IOCtlVRequest& request);
  IPCReply GetOwnedTitleCount(const IOCtlVRequest& request);
  IPCReply GetOwnedTitles(const IOCtlVRequest& request);
  IPCReply GetTitleCount(const IOCtlVRequest& request);
  IPCReply GetTitles(const IOCtlVRequest& request);
  IPCReply GetBoot2Version(const IOCtlVRequest& request);
  IPCReply GetStoredContentsCount(const ES::TMDReader& tmd, const IOCtlVRequest& request);
  IPCReply GetStoredContents(const ES::TMDReader& tmd, const IOCtlVRequest& request);
  IPCReply GetStoredContentsCount(const IOCtlVRequest& request);
  IPCReply GetStoredContents(const IOCtlVRequest& request);
  IPCReply GetTMDStoredContentsCount(const IOCtlVRequest& request);
  IPCReply GetTMDStoredContents(const IOCtlVRequest& request);
  IPCReply GetStoredTMDSize(const IOCtlVRequest& request);
  IPCReply GetStoredTMD(const IOCtlVRequest& request);
  IPCReply GetSharedContentsCount(const IOCtlVRequest& request) const;
  IPCReply GetSharedContents(const IOCtlVRequest& request) const;

  // Ticket and TMD views
  IPCReply GetTicketViewCount(const IOCtlVRequest& request);
  IPCReply GetTicketViews(const IOCtlVRequest& request);
  IPCReply GetV0TicketFromView(const IOCtlVRequest& request);
  IPCReply GetTicketSizeFromView(const IOCtlVRequest& request);
  IPCReply GetTicketFromView(const IOCtlVRequest& request);
  IPCReply GetTMDViewSize(const IOCtlVRequest& request);
  IPCReply GetTMDViews(const IOCtlVRequest& request);
  IPCReply DIGetTicketView(const IOCtlVRequest& request);
  IPCReply DIGetTMDViewSize(const IOCtlVRequest& request);
  IPCReply DIGetTMDView(const IOCtlVRequest& request);
  IPCReply DIGetTMDSize(const IOCtlVRequest& request);
  IPCReply DIGetTMD(const IOCtlVRequest& request);

  ContextArray::iterator FindActiveContext(s32 fd);
  ContextArray::iterator FindInactiveContext();

  bool LaunchIOS(u64 ios_title_id, HangPPC hang_ppc);
  bool LaunchPPCTitle(u64 title_id);

  void FinishInit();

  s32 WriteSystemFile(const std::string& path, std::span<const u8> data, Ticks ticks = {});
  s32 WriteLaunchFile(const ES::TMDReader& tmd, Ticks ticks = {});
  bool BootstrapPPC();

  ESCore& m_core;
  ContextArray m_contexts;
  std::string m_pending_ppc_boot_content_path;
};
}  // namespace IOS::HLE
