// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/Wiimote/l2cap/L2CapWiimote.h"

#include <bluetooth/l2cap.h>
#include <fcntl.h>
#include <unistd.h>

namespace
{
	// Bluetooth HID channels
	constexpr uint16 kControlPsm = 0x11;
	constexpr uint16 kInterruptPsm = 0x13;

	// HID transaction headers on the interrupt channel
	constexpr uint8 kOutputHeader = 0xA2; // DATA | output
	constexpr uint8 kInputHeader = 0xA1;  // DATA | input
	constexpr size_t kMaxFrame = 23;      // header + report id + 21 bytes

	struct AddressLess
	{
		bool operator()(const bdaddr_t& a, const bdaddr_t& b) const { return bacmp(&a, &b) < 0; }
	};

	// known addresses -> a socket is open to it
	std::mutex g_candidatesMutex;
	std::map<bdaddr_t, bool, AddressLess> g_candidates;

	std::string FormatAddress(const bdaddr_t& addr)
	{
		const uint8_t* b = addr.b; // stored little endian
		return fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", b[5], b[4], b[3], b[2], b[1], b[0]);
	}

	// opens one L2CAP channel to the remote, non-blocking once connected; -1 on failure
	int OpenChannel(const bdaddr_t& addr, uint16 psm, const char* what)
	{
		const int fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
		if (fd < 0)
		{
			cemuLog_logDebug(LogType::Force, "Failed to open {} socket: {}", what, strerror(errno));
			return -1;
		}

		sockaddr_l2 target{};
		target.l2_family = AF_BLUETOOTH;
		target.l2_psm = htobs(psm);
		target.l2_bdaddr = addr;

		// a freshly woken remote often refuses the first attempt
		const auto* raw = reinterpret_cast<const sockaddr*>(&target);
		bool connected = connect(fd, raw, sizeof(target)) == 0 || connect(fd, raw, sizeof(target)) == 0;
		if (connected)
			connected = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0;

		if (!connected)
		{
			cemuLog_logDebug(LogType::Force, "Failed to connect {} socket to '{}': {}", what, FormatAddress(addr), strerror(errno));
			close(fd);
			return -1;
		}
		return fd;
	}

	void SetInUse(const bdaddr_t& addr, bool in_use)
	{
		std::scoped_lock lock(g_candidatesMutex);
		g_candidates[addr] = in_use;
	}
}

L2CapWiimote::L2CapWiimote(int controlFd, int dataFd, bdaddr_t addr)
	: m_controlFd(controlFd), m_dataFd(dataFd), m_addr(addr)
{
}

L2CapWiimote::~L2CapWiimote()
{
	close(m_dataFd);
	close(m_controlFd);
	cemuLog_logDebug(LogType::Force, "Wiimote at {} disconnected", FormatAddress(m_addr));
	SetInUse(m_addr, false); // may be connected again later
}

void L2CapWiimote::AddCandidateAddress(bdaddr_t addr)
{
	std::scoped_lock lock(g_candidatesMutex);
	g_candidates.try_emplace(addr, false);
}

std::vector<WiimoteDevicePtr> L2CapWiimote::get_devices()
{
	std::vector<bdaddr_t> free_addresses;
	{
		std::scoped_lock lock(g_candidatesMutex);
		for (const auto& entry : g_candidates)
		{
			if (!entry.second)
				free_addresses.push_back(entry.first);
		}
	}

	std::vector<WiimoteDevicePtr> opened;
	for (const bdaddr_t& addr : free_addresses)
	{
		// the interrupt channel can only be opened while the control channel is
		const int control = OpenChannel(addr, kControlPsm, "control");
		if (control < 0)
			continue;
		const int data = OpenChannel(addr, kInterruptPsm, "data");
		if (data < 0)
		{
			close(control);
			continue;
		}
		opened.emplace_back(std::make_shared<L2CapWiimote>(control, data, addr));
		SetInUse(addr, true);
	}
	return opened;
}

bool L2CapWiimote::write_data(const std::vector<uint8>& data)
{
	cemu_assert_debug(data.size() < kMaxFrame);
	uint8 frame[kMaxFrame];
	const size_t length = std::min(data.size(), kMaxFrame - 1);
	frame[0] = kOutputHeader;
	std::memcpy(frame + 1, data.data(), length);
	return send(m_dataFd, frame, length + 1, 0) == (ssize_t)(length + 1);
}

std::optional<std::vector<uint8>> L2CapWiimote::read_data()
{
	uint8 frame[kMaxFrame];
	const ssize_t received = recv(m_dataFd, frame, sizeof(frame), 0);
	if (received < 0 && errno == EWOULDBLOCK)
		return std::vector<uint8>{}; // nothing waiting
	if (received < 2 || frame[0] != kInputHeader)
		return std::nullopt;
	return std::vector<uint8>(frame + 1, frame + received);
}

bool L2CapWiimote::operator==(const WiimoteDevice& o) const
{
	const auto* other = dynamic_cast<const L2CapWiimote*>(&o);
	return other && bacmp(&other->m_addr, &m_addr) == 0;
}
