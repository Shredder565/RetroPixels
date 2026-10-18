// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_ac is the auto-connect library: it brings the console's network
// connection up and tells games whether they are online. Under PixelU the
// host PC is the network, so the connection is always "up" and the address
// a game asks for is the host's own IPv4 address on its active adapter.
// Games get both the C++ API (nn::ac::*) and the C wrappers (AC*).

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Common/socket.h"
#include "nn_ac.h"

#if BOOST_OS_WINDOWS
#include <iphlpapi.h>
#elif BOOST_OS_LINUX || BOOST_OS_MACOS
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace
{
	// nn::ac::Status
	enum class ConnectStatus : uint32
	{
		Failed = 0xFFFFFFFF,
		Ok = 0,
	};
	static_assert(sizeof(betype<ConnectStatus>) == 4);

	constexpr nnResult AcSuccess()
	{
		return BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_AC, 0);
	}

	struct HostAddress
	{
		uint32 ip;   // host byte order
		uint32 mask; // host byte order
	};

	// What a typical home network looks like, used when the host's address can't be read.
	constexpr HostAddress kFallbackAddress{ 0xC0A80064 /* 192.168.0.100 */, 0xFFFFFF00 /* 255.255.255.0 */ };

#if BOOST_OS_WINDOWS
	// First IPv4 address of an adapter that is up and has a default gateway.
	std::optional<HostAddress> QueryHostAddress()
	{
		const ULONG flags = GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
		std::vector<uint8> storage(16 * 1024);
		ULONG size = (ULONG)storage.size();
		ULONG status = ERROR_BUFFER_OVERFLOW;
		for (int attempt = 0; attempt < 4 && status == ERROR_BUFFER_OVERFLOW; attempt++)
		{
			size = (ULONG)storage.size();
			status = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)storage.data(), &size);
			if (status == ERROR_BUFFER_OVERFLOW)
				storage.resize(std::max<size_t>(size, storage.size() * 2));
		}
		if (status != ERROR_SUCCESS)
			return std::nullopt;
		for (auto* adapter = (IP_ADAPTER_ADDRESSES*)storage.data(); adapter; adapter = adapter->Next)
		{
			if (adapter->OperStatus != IfOperStatusUp || !adapter->FirstGatewayAddress)
				continue;
			auto* unicast = adapter->FirstUnicastAddress;
			if (!unicast || !unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			ULONG mask = 0;
			if (ConvertLengthToIpv4Mask(unicast->OnLinkPrefixLength, &mask) != NO_ERROR)
				mask = 0;
			const auto* in = (const sockaddr_in*)unicast->Address.lpSockaddr;
			return HostAddress{ ntohl(in->sin_addr.s_addr), ntohl(mask) };
		}
		return std::nullopt;
	}
#elif BOOST_OS_LINUX || BOOST_OS_MACOS
	// First IPv4 address of a running, non-loopback, non-tunnel interface
	// (container bridges are skipped too).
	std::optional<HostAddress> QueryHostAddress()
	{
		ifaddrs* list = nullptr;
		if (getifaddrs(&list) != 0)
			return std::nullopt;
		std::optional<HostAddress> found;
		for (ifaddrs* it = list; it && !found; it = it->ifa_next)
		{
			if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || !it->ifa_netmask)
				continue;
			const unsigned int flags = it->ifa_flags;
			if ((flags & IFF_UP) == 0 || (flags & IFF_RUNNING) == 0)
				continue;
			if ((flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) != 0)
				continue;
			const std::string_view name = it->ifa_name ? it->ifa_name : "";
			if (name.starts_with("br-") || name.starts_with("docker"))
				continue;
			const auto* ip = (const sockaddr_in*)it->ifa_addr;
			const auto* mask = (const sockaddr_in*)it->ifa_netmask;
			found = HostAddress{ ntohl(ip->sin_addr.s_addr), ntohl(mask->sin_addr.s_addr) };
		}
		freeifaddrs(list);
		return found;
	}
#else
	std::optional<HostAddress> QueryHostAddress()
	{
		return std::nullopt;
	}
#endif

	HostAddress GetHostAddress()
	{
		if (auto address = QueryHostAddress())
			return *address;
		cemuLog_logDebug(LogType::Force, "nn_ac: no usable host network address, using {:08x}", kFallbackAddress.ip);
		return kFallbackAddress;
	}
}

namespace nn_ac
{
	nnResult Initialize()
	{
		return AcSuccess();
	}

	// Some games (Terraria) treat anything but a plain 0 as failure here.
	uint32 Connect()
	{
		return 0;
	}

	nnResult ConnectAsync()
	{
		return AcSuccess();
	}

	nnResult GetConnectResult(uint32be* result)
	{
		if (result)
			*result = AcSuccess();
		return AcSuccess();
	}

	nnResult GetLastErrorCode(uint32be* code)
	{
		if (code)
			*code = 0;
		return AcSuccess();
	}

	nnResult GetConnectStatus(betype<ConnectStatus>* status)
	{
		if (status)
			*status = ConnectStatus::Ok;
		return AcSuccess();
	}

	nnResult GetStatus(betype<ConnectStatus>* status)
	{
		return GetConnectStatus(status);
	}

	nnResult IsApplicationConnected(uint8be* connected)
	{
		if (connected)
			*connected = 1;
		return AcSuccess();
	}

	nnResult GetAssignedAddress(uint32be* address)
	{
		*address = GetHostAddress().ip;
		return AcSuccess();
	}

	nnResult GetAssignedSubnet(uint32be* mask)
	{
		*mask = GetHostAddress().mask;
		return AcSuccess();
	}

	uint32 IsSystemConnected(uint8* connected, uint32be* apType)
	{
		*apType = 0;
		*connected = 1;
		return 0;
	}

	// PixelU keeps no saved console network profiles.
	uint32 IsConfigExisting(uint32 configId, uint8* exists)
	{
		*exists = 0;
		return 0;
	}

	// C wrappers

	nnResult ACInitialize()
	{
		return Initialize();
	}

	bool ACIsSuccess(betype<nnResult>* result)
	{
		return NN_RESULT_IS_SUCCESS(*result);
	}

	bool ACIsFailure(betype<nnResult>* result)
	{
		return NN_RESULT_IS_FAILURE(*result);
	}

	nnResult ACGetConnectStatus(betype<ConnectStatus>* status)
	{
		return GetConnectStatus(status);
	}

	nnResult ACGetStatus(betype<ConnectStatus>* status)
	{
		return GetStatus(status);
	}

	nnResult ACConnectAsync()
	{
		return ConnectAsync();
	}

	nnResult ACIsApplicationConnected(uint32be* connected)
	{
		uint8be flag = 0;
		const nnResult result = IsApplicationConnected(&flag);
		*connected = (uint8)flag;
		return result;
	}

	nnResult ACGetAssignedAddress(uint32be* address)
	{
		return GetAssignedAddress(address);
	}

	void RegisterExports()
	{
		cafeExportRegisterFunc(Initialize, "nn_ac", "Initialize__Q2_2nn2acFv", LogType::Placeholder);
		cafeExportRegisterFunc(Connect, "nn_ac", "Connect__Q2_2nn2acFv", LogType::Placeholder);
		cafeExportRegisterFunc(ConnectAsync, "nn_ac", "ConnectAsync__Q2_2nn2acFv", LogType::Placeholder);
		cafeExportRegisterFunc(GetConnectResult, "nn_ac", "GetConnectResult__Q2_2nn2acFPQ2_2nn6Result", LogType::Placeholder);
		cafeExportRegisterFunc(GetLastErrorCode, "nn_ac", "GetLastErrorCode__Q2_2nn2acFPUi", LogType::Placeholder);
		cafeExportRegisterFunc(GetConnectStatus, "nn_ac", "GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status", LogType::Placeholder);
		cafeExportRegisterFunc(GetStatus, "nn_ac", "GetStatus__Q2_2nn2acFPQ3_2nn2ac6Status", LogType::Placeholder);
		cafeExportRegisterFunc(IsApplicationConnected, "nn_ac", "IsApplicationConnected__Q2_2nn2acFPb", LogType::Placeholder);
		cafeExportRegisterFunc(GetAssignedAddress, "nn_ac", "GetAssignedAddress__Q2_2nn2acFPUl", LogType::Placeholder);
		cafeExportRegisterFunc(GetAssignedSubnet, "nn_ac", "GetAssignedSubnet__Q2_2nn2acFPUl", LogType::Placeholder);
		cafeExportRegisterFunc(IsSystemConnected, "nn_ac", "IsSystemConnected__Q2_2nn2acFPbPQ3_2nn2ac6ApType", LogType::Placeholder);
		cafeExportRegisterFunc(IsConfigExisting, "nn_ac", "IsConfigExisting__Q2_2nn2acFQ3_2nn2ac11ConfigIdNumPb", LogType::Placeholder);

		cafeExportRegister("nn_ac", ACInitialize, LogType::Placeholder);
		cafeExportRegister("nn_ac", ACIsSuccess, LogType::Placeholder);
		cafeExportRegister("nn_ac", ACIsFailure, LogType::Placeholder);
		cafeExportRegister("nn_ac", ACGetConnectStatus, LogType::Placeholder);
		cafeExportRegister("nn_ac", ACGetStatus, LogType::Placeholder);
		cafeExportRegister("nn_ac", ACConnectAsync, LogType::Placeholder);
		cafeExportRegister("nn_ac", ACIsApplicationConnected, LogType::Placeholder);
		cafeExportRegister("nn_ac", ACGetAssignedAddress, LogType::Placeholder);
	}
}

void nnAc_load()
{
}

namespace nn::ac
{
	namespace
	{
		class AcLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_ac"; }

			void RPLMapped() override
			{
				nn_ac::RegisterExports();
			}
		};

		AcLibrary g_acLibrary;
	}

	COSModule* GetModule()
	{
		return &g_acLibrary;
	}
}
