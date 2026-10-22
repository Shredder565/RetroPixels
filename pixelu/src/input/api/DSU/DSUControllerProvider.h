// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Client for a DSU ("cemuhook") motion server. One provider talks to one server
// address over UDP and tracks the server's 4 slots; a reader thread takes in slot info
// and data packets (and fuses the motion data right away), a writer thread sends our
// requests.

#include "input/motion/MotionHandler.h"
#include "input/api/DSU/DSUMessages.h"

#include "input/api/ControllerProvider.h"
#include "util/helpers/ConcurrentQueue.h"

#include <boost/asio.hpp>

#ifndef HAS_DSU
#define HAS_DSU 1
#endif

struct DSUProviderSettings : public ControllerProviderSettings
{
	std::string ip;
	uint16 port;

	DSUProviderSettings() : ip("127.0.0.1"), port(26760) {}

	DSUProviderSettings(std::string ip, uint16 port)
		: ip(std::move(ip)), port(port)
	{
	}

	bool operator==(const DSUProviderSettings& s) const { return ip == s.ip && port == s.port; }

	bool operator==(const ControllerProviderSettings& s) const override
	{
		const auto* other = dynamic_cast<const DSUProviderSettings*>(&s);
		return other && *this == *other;
	}
};

class DSUControllerProvider : public ControllerProvider<DSUProviderSettings>
{
	friend class DSUController;
public:
	constexpr static int kMaxClients = 8;

	// what we know about one slot
	struct ControllerState
	{
		std::chrono::steady_clock::time_point last_update{};
		uint64_t packet_index = 0; // counts every info/data packet taken in for this slot

		PortInfoData info{};
		DataResponseData data{};
		MotionSample motion_sample{};

		ControllerState& operator=(const PortInfo& port_info);
		ControllerState& operator=(const DataResponse& data_response);
	};

	// both throw when the UDP socket can't be set up
	DSUControllerProvider();
	DSUControllerProvider(const DSUProviderSettings& settings);
	~DSUControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::DSUClient;
	InputAPI::Type api() const override { return kAPIType; }

	// asks the server for its slots and waits up to 3 s for the answers
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	// (re)opens the socket for the configured address
	bool connect();

	bool is_connected(uint8_t index) const;
	ControllerState get_state(uint8_t index) const;
	ControllerState get_prev_state(uint8_t index) const;
	MotionSample get_motion_sample(uint8_t index) const;

	// which slots got a packet past the given packet indices within `timeout` ms
	std::array<bool, kMaxClients> wait_update(const std::array<uint8_t, kMaxClients>& indices, size_t timeout) const;
	bool wait_update(uint8_t index, uint32_t packet_index, size_t timeout) const;

	uint32_t get_packet_index(uint8_t index) const;

	void request_pad_info();              // all slots
	void request_pad_info(uint8_t index); // one slot
	void request_version();
	void request_pad_data();              // all slots
	void request_pad_data(uint8_t index); // one slot

private:
	void start();
	void reader_thread();
	void writer_thread();
	void handle_packet(const uint8_t* packet, size_t size);
	void integrate_motion(uint8_t index, const DataResponse& data_response);
	template<typename T>
	void send(const T& packet);

	uint16 m_server_version = 0;

	std::atomic_bool m_running = false;
	std::thread m_reader_thread, m_writer_thread;

	uint32 m_uid;
	boost::asio::io_context m_io_service;
	boost::asio::ip::udp::endpoint m_receiver_endpoint;
	boost::asio::ip::udp::socket m_socket;
	// our own socket's address, used to wake the reader on shutdown
	boost::asio::ip::udp::endpoint m_socketWakeupEndpoint;

	std::array<ControllerState, kMaxClients> m_state{};
	std::array<ControllerState, kMaxClients> m_prev_state{};
	mutable std::array<std::mutex, kMaxClients> m_mutex;
	mutable std::array<std::condition_variable, kMaxClients> m_wait_cond;

	// packets for the writer thread; an empty one only wakes it
	ConcurrentQueue<std::vector<uint8_t>> m_writerJobs;

	std::array<WiiUMotionHandler, kMaxClients> m_motion_handler;
	std::array<uint64, kMaxClients> m_last_motion_timestamp{};
};
