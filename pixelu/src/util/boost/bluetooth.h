// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Bluetooth RFCOMM protocol type for boost::asio sockets (Windows only).
// Not used by the current build.
#if BOOST_OS_WINDOWS

#include <boost/asio.hpp>
#include <ws2bth.h>

namespace boost
{
	namespace asio
	{
		template<typename Protocol>
		class device_endpoint
		{
		public:
			typedef Protocol protocol_type;
			typedef boost::asio::detail::socket_addr_type data_type;

			struct device_t
			{
				device_t(long long device_addr) : addr(device_addr) {}
				long long addr;
			};

			device_endpoint() : m_sockaddr{} {}

			device_endpoint(device_t device) : m_sockaddr{}
			{
				m_sockaddr.addressFamily = AF_BTH;
				m_sockaddr.btAddr = (BTH_ADDR)device.addr;
				m_sockaddr.serviceClassId = RFCOMM_PROTOCOL_UUID;
				m_sockaddr.port = BT_PORT_ANY;
			}

			protocol_type protocol() const { return protocol_type(); }
			data_type* data() { return reinterpret_cast<data_type*>(&m_sockaddr); }
			const data_type* data() const { return reinterpret_cast<const data_type*>(&m_sockaddr); }
			size_t size() const { return sizeof(SOCKADDR_BTH); }
			size_t capacity() const { return sizeof(SOCKADDR_BTH); }

		private:
			SOCKADDR_BTH m_sockaddr;
		};

		class bluetooth
		{
		public:
			using endpoint = device_endpoint<bluetooth>;
			using socket = basic_stream_socket<bluetooth>;
			using acceptor = basic_socket_acceptor<bluetooth>;
			using iostream = basic_socket_iostream<bluetooth>;

			bluetooth() = default;

			int type() const { return SOCK_STREAM; }
			int protocol() const { return BTHPROTO_RFCOMM; }
			int family() const { return AF_BTH; }
		};
	}
}

#endif
