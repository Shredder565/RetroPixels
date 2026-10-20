// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// mbedtls needs its headers in this order.
// clang-format off
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
// clang-format on

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

namespace IOS::HLE
{
// Number of SSL contexts /dev/net/ssl can have at once.
constexpr int NET_SSL_MAXINSTANCES = 4;

// Results /dev/net/ssl writes to the first input vector.
enum ssl_err_t : s32
{
  SSL_OK = 0,
  SSL_ERR_FAILED = -1,
  SSL_ERR_RAGAIN = -2,
  SSL_ERR_WAGAIN = -3,
  SSL_ERR_SYSCALL = -5,
  // The read or write moved no data.
  SSL_ERR_ZERO = -6,
  // The socket is not connected.
  SSL_ERR_CAGAIN = -7,
  // No such SSL context.
  SSL_ERR_ID = -8,
  // Certificate checks that failed: common name, root CA, chain, date.
  SSL_ERR_VCOMMONNAME = -9,
  SSL_ERR_VROOTCA = -10,
  SSL_ERR_VCHAIN = -11,
  SSL_ERR_VDATE = -12,
  // The server certificate is unusable.
  SSL_ERR_SERVER_CERT = -13,
};

enum SSL_IOCTL
{
  IOCTLV_NET_SSL_NEW = 0x01,
  IOCTLV_NET_SSL_CONNECT = 0x02,
  IOCTLV_NET_SSL_DOHANDSHAKE = 0x03,
  IOCTLV_NET_SSL_READ = 0x04,
  IOCTLV_NET_SSL_WRITE = 0x05,
  IOCTLV_NET_SSL_SHUTDOWN = 0x06,
  IOCTLV_NET_SSL_SETCLIENTCERT = 0x07,
  IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT = 0x08,
  IOCTLV_NET_SSL_REMOVECLIENTCERT = 0x09,
  IOCTLV_NET_SSL_SETROOTCA = 0x0A,
  IOCTLV_NET_SSL_SETROOTCADEFAULT = 0x0B,
  IOCTLV_NET_SSL_DOHANDSHAKEEX = 0x0C,
  IOCTLV_NET_SSL_SETBUILTINROOTCA = 0x0D,
  IOCTLV_NET_SSL_SETBUILTINCLIENTCERT = 0x0E,
  IOCTLV_NET_SSL_DISABLEVERIFYOPTIONFORDEBUG = 0x0F,
  IOCTLV_NET_SSL_DEBUGGETVERSION = 0x14,
  IOCTLV_NET_SSL_DEBUGGETTIME = 0x15,
};

// One SSL context of the guest: the mbedtls state plus the Wii socket it runs over.
struct WII_SSL
{
  mbedtls_ssl_context ctx{};
  mbedtls_ssl_config config{};
  mbedtls_ssl_session session{};
  mbedtls_entropy_context entropy{};
  mbedtls_ctr_drbg_context ctr_drbg{};
  mbedtls_x509_crt cacert{};
  mbedtls_x509_crt clicert{};
  mbedtls_pk_context pk{};
  // The Wii socket, and the host socket behind it.
  int sockfd = -1;
  int hostfd = -1;
  std::string hostname;
  bool active = false;
};

// /dev/net/ssl. Context IDs given to the guest are the slot index plus one.
class NetSSLDevice : public EmulationDevice
{
public:
  NetSSLDevice(EmulationKernel& ios, const std::string& device_name);
  ~NetSSLDevice() override;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  // The ID of a free context, or 0 if all are in use.
  int GetSSLFreeID() const;

  static WII_SSL _SSL[NET_SSL_MAXINSTANCES];

private:
  // The setup error for missing certificate files is shown once per IOS instance.
  bool m_cert_error_shown = false;
};

constexpr bool IsSSLIDValid(int id)
{
  return id >= 0 && id < NET_SSL_MAXINSTANCES && NetSSLDevice::_SSL[id].active;
}
}  // namespace IOS::HLE
