// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "GDBStub.h"
#include "GDBBreakpoints.h"
#include "Debugger.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/EspressoISA.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "util/ThreadPool/ThreadPool.h"
#include "util/helpers/helpers.h"
#include "Common/socket.h"

#include <charconv>

#if BOOST_OS_UNIX
#include <netinet/tcp.h>
#endif

std::unique_ptr<GDBServer> g_gdbstub;

namespace
{
	constexpr std::string_view kFeatures = "PacketSize=4096;qXfer:features:read+;qXfer:threads:read+;qXfer:libraries:read+;swbreak+;hwbreak+;vContSupported+";
	constexpr size_t kMaxPacketLength = 4096;
	constexpr std::string_view kOk = "OK";
	constexpr std::string_view kError = "E01";

	// GDB register numbers of the PowerPC target description
	constexpr uint32 kRegGprFirst = 0;
	constexpr uint32 kRegGprLast = 31;
	constexpr uint32 kRegPc = 64;
	constexpr uint32 kRegMsr = 65;
	constexpr uint32 kRegCr = 66;
	constexpr uint32 kRegLr = 67;
	constexpr uint32 kRegCtr = 68;
	constexpr uint32 kRegXer = 69;
	constexpr uint32 kRegFprFirst = 71;
	constexpr uint32 kRegFprLast = 102;
	constexpr uint32 kRegFpscr = 103;

	/* parsing */

	bool ParseHex(std::string_view text, uint64& value)
	{
		if (text.empty())
			return false;
		auto res = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		return res.ec == std::errc() && res.ptr == text.data() + text.size();
	}

	// thread selector: "-1" means all threads, otherwise a hex thread id (0 = any)
	bool ParseSelector(std::string_view text, sint64& selector)
	{
		if (text == "-1")
		{
			selector = -1;
			return true;
		}
		uint64 v;
		if (!ParseHex(text, v) || v > 0x7FFFFFFFFFFFFFFFull)
			return false;
		selector = (sint64)v;
		return true;
	}

	// splits "a<sep>b" into a and b; false if sep is missing
	bool SplitOnce(std::string_view text, char sep, std::string_view& left, std::string_view& right)
	{
		size_t p = text.find(sep);
		if (p == std::string_view::npos)
			return false;
		left = text.substr(0, p);
		right = text.substr(p + 1);
		return true;
	}

	/* guest threads */

	uint32 ThreadId(OSThread_t* thread)
	{
		return memory_getVirtualOffsetFromPointer(thread);
	}

	OSThread_t* ActiveThread(sint32 index)
	{
		return (OSThread_t*)memory_getPointerFromVirtualOffset(activeThread[index]);
	}

	// runs func for the selected threads while holding the scheduler lock
	template<typename TFunc>
	void ForSelectedThreads(sint64 selector, TFunc&& func)
	{
		__OSLockScheduler();
		cemu_assert_debug(activeThreadCount != 0);
		if (selector == 0)
		{
			func(coreinit::OSGetDefaultThread(1));
		}
		else
		{
			for (sint32 i = 0; i < activeThreadCount; i++)
			{
				OSThread_t* thread = ActiveThread(i);
				if (selector == -1)
					func(thread);
				else if (ThreadId(thread) == (uint64)selector)
				{
					func(thread);
					break;
				}
			}
		}
		__OSUnlockScheduler();
	}

	std::vector<OSThread_t*> SnapshotActiveThreads()
	{
		std::vector<OSThread_t*> threads;
		__OSLockScheduler();
		for (sint32 i = 0; i < activeThreadCount; i++)
			threads.push_back(ActiveThread(i));
		__OSUnlockScheduler();
		return threads;
	}

	void WaitUntilNotRunning(const std::vector<OSThread_t*>& threads)
	{
		for (OSThread_t* thread : threads)
		{
			while (coreinit::OSIsThreadRunning(thread))
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	// suspends every guest thread and waits until none of them executes anymore
	void SuspendAllAndWait()
	{
		std::vector<OSThread_t*> suspended;
		__OSLockScheduler();
		cemu_assert_debug(activeThreadCount != 0);
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			OSThread_t* thread = ActiveThread(i);
			coreinit::__OSSuspendThreadNolock(thread);
			suspended.push_back(thread);
		}
		__OSUnlockScheduler();
		WaitUntilNotRunning(suspended);
	}

	void ResumeSelectedThreads(sint64 selector)
	{
		__OSLockScheduler();
		cemu_assert_debug(activeThreadCount != 0);
		if (selector == -1)
		{
			// all suspensions done by the stub are undone at once
			for (sint32 i = 0; i < activeThreadCount; i++)
				coreinit::__OSResumeThreadInternal(ActiveThread(i), 4);
		}
		else if (selector == 0)
		{
			coreinit::__OSResumeThreadInternal(coreinit::OSGetDefaultThread(1), 1);
		}
		else
		{
			for (sint32 i = 0; i < activeThreadCount; i++)
			{
				OSThread_t* thread = ActiveThread(i);
				if (ThreadId(thread) == (uint64)selector)
				{
					coreinit::__OSResumeThreadInternal(thread, 1);
					break;
				}
			}
		}
		__OSUnlockScheduler();
	}

	// Called on the thread that hit a trap: suspends all other threads and
	// then the calling thread itself (which blocks here until it is resumed).
	void SuspendAllWithTrappedLast(uint32 trappedThreadId)
	{
		__OSLockScheduler();
		cemu_assert_debug(activeThreadCount != 0);
		OSThread_t* trapped = nullptr;
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			OSThread_t* thread = ActiveThread(i);
			if (ThreadId(thread) == trappedThreadId)
				trapped = thread;
			else
				coreinit::__OSSuspendThreadNolock(thread);
		}
		coreinit::__OSSuspendThreadNolock(trapped);
		__OSUnlockScheduler();
	}

	// runs on a helper thread: answers a parked continue request once every
	// guest thread came to a halt
	void ReplyWhenStopped(std::unique_ptr<GDBServer::Request> request, std::string stopReply)
	{
		WaitUntilNotRunning(SnapshotActiveThreads());
		request->Reply(stopReply);
	}

	/* code flow */

	// addresses that may execute right after the instruction at ip
	std::vector<MPTR> PossibleSuccessors(MPTR ip, uint32 lr, uint32 ctr)
	{
		using namespace Espresso;
		const uint32 instr = memory_readU32(ip);
		const PrimaryOpcode primary = GetPrimaryOpcode(instr);
		if (primary == PrimaryOpcode::B)
		{
			uint32 target;
			bool absolute, link;
			decodeOp_B(instr, target, absolute, link);
			return { absolute ? target : ip + target };
		}
		if (primary == PrimaryOpcode::BC)
		{
			uint32 target, bi;
			BOField bo{};
			bool absolute, link;
			decodeOp_BC(instr, target, bo, bi, absolute, link);
			return { ip + 4, absolute ? target : ip + target };
		}
		if (primary == PrimaryOpcode::GROUP_19)
		{
			const Opcode19 op = GetGroup19Opcode(instr);
			if (op == Opcode19::BCLR)
				return { ip + 4, lr };
			if (op == Opcode19::BCCTR)
				return { ip + 4, ctr };
		}
		return { ip + 4 };
	}

	/* documents for qXfer */

	std::string BuildTargetDescription()
	{
		std::string xml;
		xml += "<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target version=\"1.0\">\n";
		xml += "<architecture>powerpc:common</architecture>\n";
		xml += "<feature name=\"org.gnu.gdb.power.core\">\n";
		for (uint32 i = 0; i < 32; i++)
			xml += fmt::format("<reg name=\"r{}\" bitsize=\"32\" type=\"uint32\"/>\n", i);
		xml += fmt::format("<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"{}\"/>\n", kRegPc);
		xml += "<reg name=\"msr\" bitsize=\"32\" type=\"uint32\"/>\n";
		xml += "<reg name=\"cr\" bitsize=\"32\" type=\"uint32\"/>\n";
		xml += "<reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/>\n";
		xml += "<reg name=\"ctr\" bitsize=\"32\" type=\"uint32\"/>\n";
		xml += "<reg name=\"xer\" bitsize=\"32\" type=\"uint32\"/>\n";
		xml += "</feature>\n<feature name=\"org.gnu.gdb.power.fpu\">\n";
		for (uint32 i = 0; i < 32; i++)
		{
			if (i == 0)
				xml += fmt::format("<reg name=\"f0\" bitsize=\"64\" type=\"ieee_double\" regnum=\"{}\"/>\n", kRegFprFirst);
			else
				xml += fmt::format("<reg name=\"f{}\" bitsize=\"64\" type=\"ieee_double\"/>\n", i);
		}
		xml += "<reg name=\"fpscr\" bitsize=\"32\" group=\"float\"/>\n";
		xml += "</feature>\n</target>";
		return xml;
	}

	const std::string& TargetDescription()
	{
		static const std::string s_xml = BuildTargetDescription();
		return s_xml;
	}

	std::string BuildThreadList()
	{
		std::map<uint32, std::string> sorted;
		ForSelectedThreads(-1, [&](OSThread_t* thread) {
			std::string entry = fmt::format("<thread id=\"{:x}\" core=\"{}\"", ThreadId(thread), thread->context.upir.value());
			if (!thread->threadName.IsNull())
				entry += fmt::format(" name=\"{}\"", GDBServer::Request::EscapeXml(thread->threadName.GetPtr()));
			entry += "></thread>";
			sorted.emplace(ThreadId(thread), std::move(entry));
		});
		std::string xml = "<?xml version=\"1.0\"?><threads>";
		for (auto& [id, entry] : sorted)
			xml += entry;
		xml += "</threads>";
		return xml;
	}

	std::string BuildLibraryList()
	{
		std::string xml = "<?xml version=\"1.0\"?><library-list>";
		for (RPLModule* module : RPLLoader_GetModuleList())
		{
			xml += fmt::format("<library name=\"{}\"><segment address=\"{:#x}\"/></library>",
							   GDBServer::Request::EscapeXml(module->moduleName), module->regionMappingBase_text.GetMPTR());
		}
		xml += "</library-list>";
		return xml;
	}

	// qXfer reply: 'l' marks the last chunk, 'm' means more data follows
	void ReplyChunk(GDBServer::Request& req, const std::string& document, uint64 offset, uint64 length)
	{
		if (offset >= document.size())
		{
			req.Reply("l");
			return;
		}
		std::string chunk = document.substr((size_t)offset, (size_t)length);
		req.Reply(chunk.size() == length ? "m" : "l");
		req.Reply(chunk);
	}

	void ShutdownSocket(SOCKET s)
	{
		if (s == INVALID_SOCKET)
			return;
#if BOOST_OS_WINDOWS
		shutdown(s, SD_BOTH);
#else
		shutdown(s, SHUT_RDWR);
#endif
	}
}

/* Request */

GDBServer::Request::Request(const GDBServer* server, std::string packet)
	: m_server(server), m_packet(std::move(packet))
{
	send(m_server->m_client_socket, "+", 1, 0);
}

GDBServer::Request::~Request()
{
	std::string body = EscapeBinary(m_reply);
	std::string framed = fmt::format("${}#{:02x}", body, Checksum(body));
	send(m_server->m_client_socket, framed.data(), (int)framed.size(), 0);
}

uint8 GDBServer::Request::Checksum(std::string_view data)
{
	uint32 sum = 0;
	for (char c : data)
		sum += (uint8)c;
	return (uint8)sum;
}

std::string GDBServer::Request::EscapeBinary(std::string_view data)
{
	std::string out;
	out.reserve(data.size());
	for (char c : data)
	{
		switch (c)
		{
		case '#':
		case '$':
		case '}':
		case '*':
			out += '}';
			out += (char)(c ^ 0x20);
			break;
		default:
			out += c;
		}
	}
	return out;
}

std::string GDBServer::Request::EscapeXml(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	for (char c : text)
	{
		switch (c)
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		case '\'': out += "&apos;"; break;
		default: out += c;
		}
	}
	return out;
}

/* server lifetime */

GDBServer::GDBServer(uint16 port)
	: m_port(port)
{
#if BOOST_OS_WINDOWS
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

GDBServer::~GDBServer()
{
	m_stopRequested = true;
	// unblock recv() and accept() on the server thread
	ShutdownSocket(m_client_socket);
#if BOOST_OS_WINDOWS
	if (m_server_socket != INVALID_SOCKET)
	{
		closesocket(m_server_socket);
		m_server_socket = INVALID_SOCKET;
	}
#else
	ShutdownSocket(m_server_socket);
#endif
	if (m_thread.joinable())
		m_thread.join();
	if (m_client_socket != INVALID_SOCKET)
		closesocket(m_client_socket);
	if (m_server_socket != INVALID_SOCKET)
		closesocket(m_server_socket);
	m_client_socket = INVALID_SOCKET;
	m_server_socket = INVALID_SOCKET;
#if BOOST_OS_WINDOWS
	WSACleanup();
#endif
}

bool GDBServer::Initialize()
{
	cemuLog_createLogFile(false);

	SOCKET s = socket(PF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
		return false;

	auto fail = [&]() {
		closesocket(s);
		return false;
	};

	int one = 1;
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one)) == SOCKET_ERROR)
		return fail();
	if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one)) == SOCKET_ERROR)
		return fail();

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(m_port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (const sockaddr*)&address, sizeof(address)) == SOCKET_ERROR)
		return fail();
	if (listen(s, 1) == SOCKET_ERROR)
		return fail();

	m_server_socket = s;
	m_thread = std::thread([this]() { ServerLoop(); });
	return true;
}

/* connection handling */

void GDBServer::ServerLoop()
{
	SetThreadName("GDBServer");
	while (!m_stopRequested)
	{
		if (!m_client_connected)
		{
			cemuLog_logDebug(LogType::Force, "[GDBStub] Listening for a debugger on port {}", m_port);
			sockaddr_in peer{};
			socklen_t peerSize = sizeof(peer);
			SOCKET client = accept(m_server_socket, (sockaddr*)&peer, &peerSize);
			if (client == INVALID_SOCKET)
			{
				if (!m_stopRequested)
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			m_client_socket = client;
			m_client_connected = true;
			continue;
		}

		char c;
		if (!ReadByte(c))
		{
			// the debugger went away; wait for the next one
			cemuLog_logDebug(LogType::Force, "[GDBStub] Debugger disconnected");
			m_resumeRequest.reset();
			m_client_connected = false;
			if (!m_stopRequested)
			{
				closesocket(m_client_socket);
				m_client_socket = INVALID_SOCKET;
			}
			continue;
		}
		if (c == '$')
			ReceivePacket();
		else if (c == '\x03')
			ReceiveInterrupt();
		// '+' / '-' acknowledgements and stray bytes need no handling
	}
}

bool GDBServer::ReadByte(char& c)
{
	return recv(m_client_socket, &c, 1, 0) == 1;
}

void GDBServer::ReceivePacket()
{
	std::string packet;
	uint32 sum = 0;
	char c;
	while (true)
	{
		if (!ReadByte(c))
			return;
		if (c == '#')
			break;
		packet += c;
		sum += (uint8)c;
		if (packet.size() == kMaxPacketLength + 1)
			cemuLog_logDebug(LogType::Force, "[GDBStub] Packet exceeds the advertised size");
	}
	char checksumText[2];
	if (!ReadByte(checksumText[0]) || !ReadByte(checksumText[1]))
		return;
	uint64 expected;
	if (!ParseHex(std::string_view(checksumText, 2), expected) || expected != (sum & 0xFF))
		cemuLog_logDebug(LogType::Force, "[GDBStub] Checksum mismatch for packet {}", packet);
	Dispatch(packet);
}

void GDBServer::ReceiveInterrupt()
{
	cemuLog_logDebug(LogType::Force, "[GDBStub] Interrupt requested by debugger");
	SuspendAllAndWait();
	std::string stopReply = fmt::format("T05thread:{:08X};", ThreadId(coreinit::OSGetDefaultThread(1)));
	if (m_resumeRequest)
	{
		m_resumeRequest->Reply(stopReply);
		m_resumeRequest.reset();
		return;
	}
	std::string framed = fmt::format("+${}#{:02x}", stopReply, Request::Checksum(stopReply));
	send(m_client_socket, framed.data(), (int)framed.size(), 0);
}

void GDBServer::Dispatch(const std::string& packet)
{
	RequestPtr req = std::make_unique<Request>(this, packet);
	switch (req->Kind())
	{
	case 'q':
	case 'Q': OnQuery(req); break;
	case 'v': OnVCont(req); break;
	case 'c': OnContinue(req); break;
	case 'T': OnThreadAlive(req); break;
	case 'H': OnSetThread(req); break;
	case '?': OnStopReason(req); break;
	case 'p': OnReadRegister(req); break;
	case 'P': OnWriteRegister(req); break;
	case 'g': OnReadAllRegisters(req); break;
	case 'G': OnWriteAllRegisters(req); break;
	case 'm': OnReadMemory(req); break;
	case 'M': OnWriteMemory(req); break;
	case 'Z': OnInsertBreakpoint(req); break;
	case 'z': OnRemoveBreakpoint(req); break;
	default: break; // unsupported: the empty reply tells the client so
	}
}

/* queries */

void GDBServer::OnQuery(RequestPtr& req)
{
	const std::string& p = req->Packet();
	auto isCommand = [&](std::string_view name) {
		return p == name || (p.size() > name.size() && p.compare(0, name.size(), name) == 0 && p[name.size()] == ':');
	};

	if (isCommand("qSupported"))
		req->Reply(kFeatures);
	else if (isCommand("qAttached"))
		req->Reply("1");
	else if (p == "qC")
		req->Reply(fmt::format("QC{}", m_continueThread));
	else if (p == "qfThreadInfo")
	{
		std::string ids;
		ForSelectedThreads(-1, [&](OSThread_t* thread) {
			if (!ids.empty())
				ids += ',';
			ids += fmt::format("{:08X}", ThreadId(thread));
		});
		req->Reply("m" + ids);
	}
	else if (p == "qsThreadInfo")
		req->Reply("l");
	else if (p.compare(0, 6, "qXfer:") == 0)
		OnTransfer(req, std::string_view(p).substr(6));
}

// <object>:read:<annex>:<offset>,<length>
void GDBServer::OnTransfer(RequestPtr& req, std::string_view args)
{
	std::string_view object, rest, annex, range, offsetText, lengthText;
	if (!SplitOnce(args, ':', object, rest) || rest.substr(0, 5) != "read:")
		return;
	if (!SplitOnce(rest.substr(5), ':', annex, range) || !SplitOnce(range, ',', offsetText, lengthText))
		return;
	uint64 offset, length;
	if (!ParseHex(offsetText, offset) || !ParseHex(lengthText, length))
		return;

	if (object == "features")
	{
		if (annex == "target.xml")
			ReplyChunk(*req, TargetDescription(), offset, length);
		else
			cemuLog_logDebug(LogType::Force, "[GDBStub] Unknown feature document {}", annex);
	}
	else if (object == "threads")
		ReplyChunk(*req, BuildThreadList(), offset, length);
	else if (object == "libraries")
		ReplyChunk(*req, BuildLibraryList(), offset, length);
}

/* execution control */

void GDBServer::OnVCont(RequestPtr& req)
{
	const std::string& p = req->Packet();
	if (p == "vCont?")
	{
		req->Reply("vCont;c;C;s;S");
		return;
	}
	if (p.size() <= 6 || p.compare(0, 6, "vCont;") != 0)
		return;

	const std::string actions = p.substr(6);
	m_resumeRequest = std::move(req);

	bool resumedAny = false;
	std::string_view remaining = actions;
	while (!remaining.empty())
	{
		size_t end = remaining.find(';');
		std::string_view action = remaining.substr(0, end);
		remaining = (end == std::string_view::npos) ? std::string_view() : remaining.substr(end + 1);
		if (action.empty())
			continue;

		std::string_view kind = action;
		sint64 selector = -1;
		size_t colon = action.find(':');
		if (colon != std::string_view::npos)
		{
			kind = action.substr(0, colon);
			if (!ParseSelector(action.substr(colon + 1), selector))
				continue;
		}
		if (kind.empty())
			continue;

		if (kind[0] == 'c' || kind[0] == 'C')
		{
			if (kind[0] == 'c' && kind.size() != 1)
				continue;
			ResumeSelectedThreads(selector);
			resumedAny = true;
		}
		else if (kind[0] == 's' || kind[0] == 'S')
		{
			if (kind[0] == 's' && kind.size() != 1)
				continue;
			ForSelectedThreads(selector, [&](OSThread_t* thread) {
				PlantStepTraps(thread->context.srr0, _swapEndianU32(thread->context.lr), thread->context.ctr, "swbreak:;");
			});
		}
	}

	if (!resumedAny)
	{
		ResumeSelectedThreads(-1);
		cemuLog_logDebug(LogType::Force, "[GDBStub] Resumed all threads for stepping");
	}
}

void GDBServer::OnContinue(RequestPtr& req)
{
	m_resumeRequest = std::move(req);
	ResumeSelectedThreads(m_continueThread);
}

void GDBServer::OnThreadAlive(RequestPtr& req)
{
	sint64 selector;
	if (!ParseSelector(req->Body(), selector))
		return;
	bool found = false;
	ForSelectedThreads(selector, [&](OSThread_t*) { found = true; });
	req->Reply(found ? kOk : kError);
}

void GDBServer::OnSetThread(RequestPtr& req)
{
	std::string_view body = req->Body();
	if (body.empty() || (body[0] != 'c' && body[0] != 'g'))
		return;
	sint64 selector;
	if (!ParseSelector(body.substr(1), selector))
		return;
	if (selector >= 0)
	{
		bool found = false;
		ForSelectedThreads(selector, [&](OSThread_t*) { found = true; });
		if (!found)
		{
			req->Reply(kError);
			return;
		}
	}
	(body[0] == 'c' ? m_continueThread : m_registerThread) = selector;
	req->Reply(kOk);
}

void GDBServer::OnStopReason(RequestPtr& req)
{
	ForSelectedThreads(0, [&](OSThread_t* thread) {
		req->Reply(fmt::format("T05thread:{:08X};", ThreadId(thread)));
	});
}

/* registers */

void GDBServer::OnReadRegister(RequestPtr& req)
{
	uint64 reg;
	if (!ParseHex(req->Body(), reg))
		return;
	ForSelectedThreads(m_registerThread, [&](OSThread_t* thread) {
		OSContext_t& ctx = thread->context;
		if (reg <= kRegGprLast)
			req->Reply(fmt::format("{:08X}", _swapEndianU32(ctx.gpr[reg])));
		else if (reg >= kRegFprFirst && reg <= kRegFprLast)
			req->Reply(fmt::format("{:016X}", ctx.fp_ps0[reg - kRegFprFirst].value()));
		else if (reg == kRegFpscr)
			req->Reply(fmt::format("{:08X}", ctx.fpscr.fpscr.value()));
		else if (reg == kRegPc)
			req->Reply(fmt::format("{:08X}", ctx.srr0));
		else if (reg == kRegMsr)
			req->Reply("xxxxxxxx");
		else if (reg == kRegCr)
			req->Reply(fmt::format("{:08X}", ctx.cr));
		else if (reg == kRegLr)
			req->Reply(fmt::format("{:08X}", _swapEndianU32(ctx.lr)));
		else if (reg == kRegCtr)
			req->Reply(fmt::format("{:08X}", ctx.ctr));
		else if (reg == kRegXer)
			req->Reply(fmt::format("{:08X}", ctx.xer));
	});
}

void GDBServer::OnWriteRegister(RequestPtr& req)
{
	std::string_view regText, valueText;
	uint64 reg, value;
	if (!SplitOnce(req->Body(), '=', regText, valueText) || !ParseHex(regText, reg) || !ParseHex(valueText, value))
		return;
	ForSelectedThreads(m_registerThread, [&](OSThread_t* thread) {
		OSContext_t& ctx = thread->context;
		const uint32 value32 = (uint32)value;
		bool ok = true;
		if (reg <= kRegGprLast)
			ctx.gpr[reg] = _swapEndianU32(value32);
		else if (reg >= kRegFprFirst && reg <= kRegFprLast)
			ctx.fp_ps0[reg - kRegFprFirst] = uint64be{ value };
		else if (reg == kRegFpscr)
			ctx.fpscr.fpscr = uint32be{ value32 };
		else if (reg == kRegPc)
			ctx.srr0 = value32;
		else if (reg == kRegCr)
			ctx.cr = value32;
		else if (reg == kRegLr)
			ctx.lr = _swapEndianU32(value32);
		else if (reg == kRegCtr)
			ctx.ctr = value32;
		else if (reg == kRegXer)
			ctx.xer = value32;
		else
			ok = false; // MSR and unknown registers are read-only
		req->Reply(ok ? kOk : kError);
	});
}

void GDBServer::OnReadAllRegisters(RequestPtr& req)
{
	ForSelectedThreads(m_registerThread, [&](OSThread_t* thread) {
		for (uint32 gpr : thread->context.gpr)
			req->Reply(fmt::format("{:08X}", _swapEndianU32(gpr)));
	});
}

void GDBServer::OnWriteAllRegisters(RequestPtr& req)
{
	std::string_view data = req->Body();
	const size_t count = std::min<size_t>(32, data.size() / 8);
	uint32 values[32];
	for (size_t i = 0; i < count; i++)
	{
		uint64 v;
		if (!ParseHex(data.substr(i * 8, 8), v))
		{
			req->Reply(kError);
			return;
		}
		values[i] = (uint32)v;
	}
	ForSelectedThreads(m_registerThread, [&](OSThread_t* thread) {
		for (size_t i = 0; i < count; i++)
			thread->context.gpr[i] = _swapEndianU32(values[i]);
	});
	req->Reply(kOk);
}

/* memory */

template<typename TFunc>
void GDBServer::ForEachTrapInRange(MPTR address, uint32 length, TFunc&& func)
{
	const uint64 end = (uint64)address + length;
	auto it = m_codeTraps.lower_bound(address >= 3 ? address - 3 : 0);
	for (; it != m_codeTraps.end() && it->first < end; ++it)
	{
		if ((uint64)it->first + 4 > address)
			func(it->first, it->second);
	}
}

void GDBServer::OnReadMemory(RequestPtr& req)
{
	std::string_view addrText, lengthText;
	uint64 address, length;
	if (!SplitOnce(req->Body(), ',', addrText, lengthText) || !ParseHex(addrText, address) || !ParseHex(lengthText, length))
		return;
	if (address > 0xFFFFFFFF || length > 0xFFFFFFFF || !memory_isAddressRangeAccessible((MPTR)address, (uint32)length))
	{
		req->Reply(kError);
		return;
	}

	std::vector<uint8> bytes(memory_getPointerFromVirtualOffset((MPTR)address), memory_getPointerFromVirtualOffset((MPTR)address) + length);
	// show the client the instructions hidden behind our traps
	ForEachTrapInRange((MPTR)address, (uint32)length, [&](MPTR trapAddress, CodeTrap& trap) {
		const uint32 word = trap.ClientVisibleWord();
		for (uint32 b = 0; b < 4; b++)
		{
			const uint64 byteAddress = (uint64)trapAddress + b;
			if (byteAddress >= address && byteAddress < address + length)
				bytes[byteAddress - address] = (uint8)(word >> (24 - b * 8));
		}
	});

	std::string hex;
	hex.reserve(bytes.size() * 2);
	for (uint8 v : bytes)
		hex += fmt::format("{:02X}", v);
	req->Reply(hex);
}

void GDBServer::OnWriteMemory(RequestPtr& req)
{
	std::string_view header, data, addrText, lengthText;
	uint64 address, length;
	if (!SplitOnce(req->Body(), ':', header, data) || !SplitOnce(header, ',', addrText, lengthText) ||
		!ParseHex(addrText, address) || !ParseHex(lengthText, length))
		return;
	if (address > 0xFFFFFFFF || length > 0xFFFFFFFF || data.size() < length * 2 ||
		!memory_isAddressRangeAccessible((MPTR)address, (uint32)length))
	{
		req->Reply(kError);
		return;
	}

	std::vector<uint8> bytes((size_t)length);
	for (size_t i = 0; i < bytes.size(); i++)
	{
		uint64 v;
		if (!ParseHex(data.substr(i * 2, 2), v))
		{
			req->Reply(kError);
			return;
		}
		bytes[i] = (uint8)v;
	}

	uint8* dst = memory_getPointerFromVirtualOffset((MPTR)address);
	std::vector<bool> handledByTrap(bytes.size(), false);
	// bytes under an armed trap belong to the saved instruction, not to memory
	ForEachTrapInRange((MPTR)address, (uint32)length, [&](MPTR trapAddress, CodeTrap& trap) {
		if (trap.IsDisarmed())
			return;
		uint32 saved = trap.SavedInstruction();
		for (uint32 b = 0; b < 4; b++)
		{
			const uint64 byteAddress = (uint64)trapAddress + b;
			if (byteAddress < address || byteAddress >= address + length)
				continue;
			const uint32 shift = 24 - b * 8;
			saved = (saved & ~(0xFFu << shift)) | ((uint32)bytes[byteAddress - address] << shift);
			handledByTrap[byteAddress - address] = true;
		}
		trap.SetSavedInstruction(saved);
	});
	for (size_t i = 0; i < bytes.size(); i++)
	{
		if (!handledByTrap[i])
			dst[i] = bytes[i];
	}
	if (length)
		PPCRecompiler_invalidateRange((MPTR)address, (MPTR)(address + length));
	req->Reply(kOk);
}

/* breakpoints */

void GDBServer::OnInsertBreakpoint(RequestPtr& req)
{
	std::string_view body = req->Body();
	std::string_view typeText, rest, addrText, kindText;
	uint64 type, address, kind;
	if (!SplitOnce(body, ',', typeText, rest) || !SplitOnce(rest, ',', addrText, kindText) ||
		!ParseHex(typeText, type) || !ParseHex(addrText, address) || !ParseHex(kindText, kind) || type > 4)
		return;
	const MPTR addr = (MPTR)address;

	if (type <= 1)
	{
		m_codeTraps.erase(addr);
		m_codeTraps.try_emplace(addr, addr, BreakpointType::BP_PERSISTENT, type == 0, type == 0 ? "swbreak:;" : "hwbreak:;");
	}
	else
	{
		if (m_watchpoint)
		{
			req->Reply(kError);
			return;
		}
		m_watchpoint = std::make_unique<Watchpoint>(addr, (AccessPointType)type);
	}
	req->Reply(kOk);
}

void GDBServer::OnRemoveBreakpoint(RequestPtr& req)
{
	std::string_view body = req->Body();
	std::string_view typeText, rest, addrText, kindText;
	uint64 type, address, kind;
	if (!SplitOnce(body, ',', typeText, rest) || !SplitOnce(rest, ',', addrText, kindText) ||
		!ParseHex(typeText, type) || !ParseHex(addrText, address) || !ParseHex(kindText, kind) || type > 4)
		return;
	const MPTR addr = (MPTR)address;

	if (type <= 1)
	{
		auto it = m_codeTraps.find(addr);
		if (it == m_codeTraps.end() || !it->second.StopsThreads())
		{
			req->Reply(kError);
			return;
		}
		m_codeTraps.erase(it);
	}
	else
	{
		if (!m_watchpoint || m_watchpoint->GetAddress() != addr)
		{
			req->Reply(kError);
			return;
		}
		m_watchpoint.reset();
	}
	req->Reply(kOk);
}

void GDBServer::PlantStepTraps(uint32 ip, uint32 lr, uint32 ctr, const std::string& reason)
{
	for (MPTR next : PossibleSuccessors(ip, lr, ctr))
	{
		auto it = m_codeTraps.find(next);
		if (it != m_codeTraps.end() && !it->second.StopsThreads() && !it->second.DroppedOnAnyTrap())
		{
			// a restore point: any trap re-arms breakpoints, so a step point can take its place
			m_codeTraps.erase(it);
			it = m_codeTraps.end();
		}
		if (it == m_codeTraps.end())
		{
			m_codeTraps.try_emplace(next, next, BreakpointType::BP_STEP_POINT, false, reason);
			continue;
		}
		if (it->second.IsDisarmed() && next != ip)
			it->second.Rearm();
		it->second.RequestStopOnce();
	}
}

/* events from the emulated CPUs */

void GDBServer::HandleTrapInstruction(PPCInterpreter_t* hCPU)
{
	const MPTR ip = hCPU->instructionPointer;

	// the thread that stepped over a disarmed breakpoint has moved on, so
	// every breakpoint can take effect again
	for (auto& [address, trap] : m_codeTraps)
	{
		if (address != ip)
			trap.Rearm();
	}

	auto it = m_codeTraps.find(ip);
	if (it == m_codeTraps.end())
	{
		cemu_assert_suspicious();
		return;
	}

	CodeTrap& trap = it->second;
	const uint32 threadId = ThreadId(coreinit::OSGetCurrentThread());
	const std::string stopReply = fmt::format("T05thread:{:08X};core:{:02X};{}", threadId, PPCInterpreter_getCoreIndex(hCPU), trap.StopReason());
	const bool stop = trap.StopsThreads() || trap.ConsumeStopRequest();

	if (trap.StaysAfterHit())
	{
		// let the original instruction execute; restore points right after it re-arm the breakpoint
		for (MPTR next : PossibleSuccessors(ip, hCPU->spr.LR, hCPU->spr.CTR))
		{
			if (next != ip && !m_codeTraps.contains(next))
				m_codeTraps.try_emplace(next, next, BreakpointType::BP_RESTORE_POINT, false, "");
		}
		trap.Disarm();
	}
	else
	{
		m_codeTraps.erase(it);
	}

	std::erase_if(m_codeTraps, [](const auto& entry) { return entry.second.DroppedOnAnyTrap(); });

	if (!stop)
		return;

	cemuLog_logDebug(LogType::Force, "[GDBStub] Breakpoint hit at {:08x}", ip);
	if (m_resumeRequest)
		ThreadPool::FireAndForget(&ReplyWhenStopped, std::move(m_resumeRequest), stopReply);
	SuspendAllWithTrappedLast(threadId);
	cemuLog_logDebug(LogType::Force, "[GDBStub] Continuing after breakpoint");
}

void GDBServer::HandleAccessException(uint64 dr6)
{
	if (!m_watchpoint)
		return;
	// DR6.B2 is the write-only slot, DR6.B3 the read/write slot
	const bool hitWrite = (dr6 >> 2) & 1;
	const bool hitAccess = (dr6 >> 3) & 1;
	const MPTR address = m_watchpoint->GetAddress();

	std::string stopReason;
	switch (m_watchpoint->GetType())
	{
	case AccessPointType::BP_WRITE:
		if (hitWrite)
			stopReason = fmt::format("watch:{:08X};", address);
		break;
	case AccessPointType::BP_READ:
		if (hitAccess && !hitWrite)
			stopReason = fmt::format("rwatch:{:08X};", address);
		break;
	case AccessPointType::BP_BOTH:
		if (hitAccess)
			stopReason = fmt::format("awatch:{:08X};", address);
		break;
	}
	if (stopReason.empty())
		return;

	cemuLog_logDebug(LogType::Force, "[GDBStub] Watchpoint triggered: {}", stopReason);
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	PlantStepTraps(hCPU->instructionPointer, hCPU->spr.LR, hCPU->spr.CTR, stopReason);
}

void GDBServer::HandleEntryStop(uint32 entryAddress)
{
	m_codeTraps.try_emplace(entryAddress, entryAddress, BreakpointType::BP_SINGLE, false, "");
	m_entry_point = entryAddress;
}
