// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/DSU/DSUControllerProvider.h"
#include "input/api/DSU/DSUController.h"

namespace
{
	// the server's slots we ask about
	constexpr std::array<uint8_t, 4> kServerSlots{ 0, 1, 2, 3 };
	constexpr uint8_t kNoSlot = 0xFF;

	constexpr float kDegreesToRadians = 0.0174533f;
	// a motion timestamp this far (10 s, in microseconds) in the past means the pad restarted
	constexpr uint64 kTimestampResetUs = 10'000'000;
}

DSUControllerProvider::DSUControllerProvider()
	: m_uid(rand()), m_socket(m_io_service)
{
	start();
}

DSUControllerProvider::DSUControllerProvider(const DSUProviderSettings& settings)
	: ControllerProvider<DSUProviderSettings>(settings), m_uid(rand()), m_socket(m_io_service)
{
	start();
}

void DSUControllerProvider::start()
{
	if (!connect())
		throw std::runtime_error("dsu client can't open the udp connection");

	m_running = true;
	m_reader_thread = std::thread(&DSUControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&DSUControllerProvider::writer_thread, this);
	request_version();
}

DSUControllerProvider::~DSUControllerProvider()
{
	if (!m_running)
		return;
	m_running = false;

	// the reader blocks in receive_from: send ourselves a byte to wake it
	if (m_socketWakeupEndpoint.port() != 0)
	{
		boost::asio::io_context context;
		boost::asio::ip::udp::socket waker(context);
		boost::system::error_code ec;
		waker.open(m_socketWakeupEndpoint.protocol(), ec);
		if (ec)
			cemuLog_log(LogType::Force, "DSUControllerProvider wakeup failed");
		else
		{
			const uint8_t byte = 0;
			waker.send_to(boost::asio::buffer(&byte, 1), m_socketWakeupEndpoint, 0, ec);
		}
	}
	m_reader_thread.join();

	m_writerJobs.push(std::vector<uint8_t>{}); // wakes the writer
	m_writer_thread.join();
}

std::vector<std::shared_ptr<ControllerBase>> DSUControllerProvider::get_controllers()
{
	std::array<uint8_t, kMaxClients> before;
	for (int i = 0; i < kMaxClients; ++i)
		before[i] = (uint8_t)get_packet_index((uint8_t)i);

	request_pad_info();
	const auto answered = wait_update(before, 3000);

	std::vector<ControllerPtr> result;
	for (int i = 0; i < kMaxClients; ++i)
	{
		if (answered[i] && is_connected((uint8_t)i))
			result.emplace_back(std::make_shared<DSUController>(i, m_settings));
	}
	return result;
}

bool DSUControllerProvider::connect()
{
	const auto& settings = get_settings();
	if (m_receiver_endpoint.address().to_string() == settings.ip && m_receiver_endpoint.port() == settings.port)
		return true;

	try
	{
		namespace ip = boost::asio::ip;
		ip::udp::resolver resolver(m_io_service);
		m_receiver_endpoint = *resolver.resolve(settings.ip, fmt::format("{}", settings.port)).cbegin();

		if (m_socket.is_open())
			m_socket.close();
		m_socket.open(ip::udp::v4());
		m_socket.bind(ip::udp::endpoint(ip::udp::v4(), 0));
		m_socketWakeupEndpoint = ip::udp::endpoint(ip::address_v4::loopback(), m_socket.local_endpoint().port());

		m_state = {};
		m_prev_state = {};
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "dsu client connect error: {}", ex.what());
		return false;
	}
}

bool DSUControllerProvider::is_connected(uint8_t index) const
{
	if (index >= kMaxClients)
		return false;
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index].info.state == DsState::Connected;
}

DSUControllerProvider::ControllerState DSUControllerProvider::get_state(uint8_t index) const
{
	if (index >= kMaxClients)
		return {};
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index];
}

DSUControllerProvider::ControllerState DSUControllerProvider::get_prev_state(uint8_t index) const
{
	if (index >= kMaxClients)
		return {};
	std::scoped_lock lock(m_mutex[index]);
	return m_prev_state[index];
}

MotionSample DSUControllerProvider::get_motion_sample(uint8_t index) const
{
	if (index >= kMaxClients)
		return {};
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index].motion_sample;
}

std::array<bool, DSUControllerProvider::kMaxClients> DSUControllerProvider::wait_update(const std::array<uint8_t, kMaxClients>& indices, size_t timeout) const
{
	std::array<bool, kMaxClients> updated{};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
	do
	{
		bool all = true;
		for (int i = 0; i < kMaxClients; ++i)
		{
			if (!updated[i])
			{
				std::scoped_lock lock(m_mutex[i]);
				updated[i] = indices[i] < m_state[i].packet_index;
			}
			all = all && updated[i];
		}
		if (all)
			break;
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);
	return updated;
}

bool DSUControllerProvider::wait_update(uint8_t index, uint32_t packet_index, size_t timeout) const
{
	if (index >= kMaxClients)
		return false;
	std::unique_lock lock(m_mutex[index]);
	return m_wait_cond[index].wait_for(lock, std::chrono::milliseconds(timeout),
		[this, index, packet_index] { return packet_index < m_state[index].packet_index; });
}

uint32_t DSUControllerProvider::get_packet_index(uint8_t index) const
{
	std::scoped_lock lock(m_mutex[index]);
	return (uint32_t)m_state[index].packet_index;
}

template<typename T>
void DSUControllerProvider::send(const T& packet)
{
	const auto* bytes = reinterpret_cast<const uint8_t*>(&packet);
	m_writerJobs.push(std::vector<uint8_t>(bytes, bytes + packet.header.GetSize()));
}

void DSUControllerProvider::request_version()
{
	send(DSUMessage::Version(m_uid));
}

void DSUControllerProvider::request_pad_info()
{
	send(DSUMessage::PortList(m_uid, (uint32_t)kServerSlots.size(), kServerSlots));
}

void DSUControllerProvider::request_pad_info(uint8_t index)
{
	if (index >= kMaxClients)
		return;
	send(DSUMessage::PortList(m_uid, 1, { index, 0, 0, 0 }));
}

void DSUControllerProvider::request_pad_data()
{
	send(DSUMessage::Data(m_uid));
}

void DSUControllerProvider::request_pad_data(uint8_t index)
{
	if (index >= kMaxClients)
		return;
	send(DSUMessage::Data(m_uid, index));
}

void DSUControllerProvider::reader_thread()
{
	SetThreadName("DSU-reader");
	bool first_error = true;
	while (m_running.load(std::memory_order_relaxed))
	{
		std::array<uint8_t, sizeof(DataResponse)> packet{};
		boost::asio::ip::udp::endpoint sender;
		boost::system::error_code ec;
		const size_t size = m_socket.receive_from(boost::asio::buffer(packet), sender, 0, ec);
		if (!m_running.load(std::memory_order_relaxed))
			break;
		if (ec)
		{
			// nobody listening at the server address (the first receive often fails at once)
			if (first_error)
				first_error = false;
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				std::this_thread::yield();
			}
			continue;
		}
		handle_packet(packet.data(), size);
	}
}

void DSUControllerProvider::handle_packet(const uint8_t* bytes, size_t size)
{
	if (size < sizeof(MessageHeader))
		return;

	// the buffer is always sizeof(DataResponse) long, so every packet type can be read from it
	uint8_t slot = kNoSlot;
	const auto& header = *reinterpret_cast<const MessageHeader*>(bytes);
	switch (header.GetMessageType())
	{
	case MessageType::Version:
	{
		const auto& version = *reinterpret_cast<const VersionResponse*>(bytes);
		if (!version.IsValid())
			return;
		m_server_version = version.GetVersion();
		break;
	}
	case MessageType::Information:
	{
		const auto& info = *reinterpret_cast<const PortInfo*>(bytes);
		if (!info.IsValid())
			return;
		slot = info.GetIndex();
		cemu_assert(slot < kMaxClients);
		std::scoped_lock lock(m_mutex[slot]);
		m_prev_state[slot] = m_state[slot];
		m_state[slot] = info;
		m_wait_cond[slot].notify_all();
		break;
	}
	case MessageType::Data:
	{
		const auto& data = *reinterpret_cast<const DataResponse*>(bytes);
		if (!data.IsValid())
			return;
		slot = data.GetIndex();
		cemu_assert(slot < kMaxClients);
		std::scoped_lock lock(m_mutex[slot]);
		m_prev_state[slot] = m_state[slot];
		m_state[slot] = data;
		m_wait_cond[slot].notify_all();
		// fused here so no sample is skipped
		integrate_motion(slot, data);
		break;
	}
	default:
		break;
	}

	// keep the subscription to this slot alive
	if (slot != kNoSlot)
		request_pad_data(slot);
}

void DSUControllerProvider::writer_thread()
{
	SetThreadName("DSU-writer");
	while (m_running.load(std::memory_order_relaxed))
	{
		const std::vector<uint8_t> packet = m_writerJobs.pop();
		if (!m_running.load(std::memory_order_relaxed))
			return;
		if (packet.empty())
			continue;
		try
		{
			m_socket.send_to(boost::asio::buffer(packet), m_receiver_endpoint);
		}
		catch (const std::exception&)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}
}

void DSUControllerProvider::integrate_motion(uint8_t index, const DataResponse& data_response)
{
	uint64& last = m_last_motion_timestamp[index];
	const uint64 timestamp = data_response.GetMotionTimestamp();
	if (timestamp <= last)
	{
		if (last - timestamp >= kTimestampResetUs)
			last = 0;
		return;
	}

	const uint64 elapsed_us = timestamp - last;
	last = timestamp;
	const float dt = (float)((double)elapsed_us / 1000000.0);
	const Vector3f& acc = data_response.GetAcceleration();
	const Vector3f& gyro = data_response.GetGyro();
	WiiUMotionHandler& handler = m_motion_handler[index];
	handler.processMotionSample(dt, gyro.x * kDegreesToRadians, gyro.y * kDegreesToRadians, gyro.z * kDegreesToRadians, acc.x, -acc.y, -acc.z);
	m_state[index].motion_sample = handler.getMotionSample();
}

DSUControllerProvider::ControllerState& DSUControllerProvider::ControllerState::operator=(const PortInfo& port_info)
{
	info = port_info.GetInfo();
	last_update = std::chrono::steady_clock::now();
	++packet_index;
	return *this;
}

DSUControllerProvider::ControllerState& DSUControllerProvider::ControllerState::operator=(const DataResponse& data_response)
{
	info = data_response.GetInfo();
	last_update = std::chrono::steady_clock::now();
	++packet_index;
	data = data_response.GetData();
	return *this;
}
