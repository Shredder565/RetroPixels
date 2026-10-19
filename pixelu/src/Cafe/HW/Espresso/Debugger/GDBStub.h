// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Common/precompiled.h"
#include "Common/socket.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

#include <numeric>

// GDB remote serial protocol server. A single client can connect on the
// loopback interface; guest threads are exposed as GDB threads (thread id =
// guest address of the OSThread) and software breakpoints are implemented
// with trap instructions that the interpreter forwards to
// HandleTrapInstruction.
class GDBServer
{
public:
	explicit GDBServer(uint16 port);
	~GDBServer();

	// opens the listening socket and starts the server thread
	bool Initialize();
	bool IsConnected()
	{
		return m_client_connected;
	}

	// stop at the title's entry point once it is reached
	void HandleEntryStop(uint32 entryAddress);
	// a guest thread executed one of our trap instructions
	void HandleTrapInstruction(PPCInterpreter_t* hCPU);
	// a host debug register used for a watchpoint fired
	void HandleAccessException(uint64 dr6);

	class CodeTrap;
	class Watchpoint;

	// One received packet. The '+' acknowledgement is sent when the request
	// is created and the reply ($...#xx) when it is destroyed, so a request
	// that resumes execution can be parked until the target stops again.
	class Request
	{
	public:
		Request(const GDBServer* server, std::string packet);
		~Request();
		Request(const Request&) = delete;
		Request& operator=(const Request&) = delete;

		const std::string& Packet() const { return m_packet; }
		char Kind() const { return m_packet.empty() ? '\0' : m_packet[0]; }
		// packet text after the command character
		std::string_view Body() const { return std::string_view(m_packet).substr(m_packet.empty() ? 0 : 1); }

		void Reply(std::string_view text) { m_reply += text; }

		static uint8 Checksum(std::string_view data);
		static std::string EscapeBinary(std::string_view data);
		static std::string EscapeXml(std::string_view text);

	private:
		const GDBServer* m_server;
		std::string m_packet;
		std::string m_reply;
	};

private:
	using RequestPtr = std::unique_ptr<Request>;

	void ServerLoop();
	bool ReadByte(char& c);
	void ReceivePacket();
	void ReceiveInterrupt();
	void Dispatch(const std::string& packet);

	void OnQuery(RequestPtr& req);
	void OnTransfer(RequestPtr& req, std::string_view args);
	void OnVCont(RequestPtr& req);
	void OnContinue(RequestPtr& req);
	void OnThreadAlive(RequestPtr& req);
	void OnSetThread(RequestPtr& req);
	void OnStopReason(RequestPtr& req);
	void OnReadRegister(RequestPtr& req);
	void OnWriteRegister(RequestPtr& req);
	void OnReadAllRegisters(RequestPtr& req);
	void OnWriteAllRegisters(RequestPtr& req);
	void OnReadMemory(RequestPtr& req);
	void OnWriteMemory(RequestPtr& req);
	void OnInsertBreakpoint(RequestPtr& req);
	void OnRemoveBreakpoint(RequestPtr& req);

	// adds traps that stop once the given thread state has executed one instruction
	void PlantStepTraps(uint32 ip, uint32 lr, uint32 ctr, const std::string& reason);
	// code traps overlapping [address, address+length)
	template<typename TFunc>
	void ForEachTrapInRange(MPTR address, uint32 length, TFunc&& func);

	const uint16 m_port;
	std::thread m_thread;
	std::atomic_bool m_stopRequested{ false };
	std::atomic_bool m_client_connected{ false };
	SOCKET m_server_socket = INVALID_SOCKET;
	SOCKET m_client_socket = INVALID_SOCKET;

	// thread selection from Hg / Hc (-1 all, 0 any, otherwise a thread id)
	sint64 m_registerThread = 0;
	sint64 m_continueThread = 0;

	MPTR m_entry_point{};
	RequestPtr m_resumeRequest; // reply to send when the target stops

	std::map<MPTR, CodeTrap> m_codeTraps;
	std::unique_ptr<Watchpoint> m_watchpoint;
};

extern std::unique_ptr<GDBServer> g_gdbstub;
