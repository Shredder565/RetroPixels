// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/LaunchSettings.h"
#include <pugixml.hpp>

// coreinit formatted output (the printf family used by OSReport and __os_snprintf),
// the developer console, process/title queries and title relaunch.

namespace coreinit
{
	/* formatted output */

	namespace
	{
		// bounded output buffer: writes past the capacity are dropped
		class FormatSink
		{
		public:
			FormatSink(char* dst, sint32 capacity) : m_dst(dst), m_capacity(capacity) {}

			sint32 Room() const { return m_capacity - m_used; }
			sint32 Used() const { return m_used; }

			void Put(char c)
			{
				if (Room() > 0)
					m_dst[m_used++] = c;
			}

			void Repeat(char c, sint64 count)
			{
				sint32 n = (sint32)std::min<sint64>(count, Room());
				for (sint32 i = 0; i < n; i++)
					m_dst[m_used++] = c;
			}

			void Append(const char* src, sint32 count)
			{
				count = std::min(count, Room());
				if (count <= 0)
					return;
				memcpy(m_dst + m_used, src, count);
				m_used += count;
			}

		private:
			char* m_dst;
			sint32 m_capacity;
			sint32 m_used{0};
		};

		// a parsed conversion. 'width' is signed: negative means left-aligned in |width| columns
		struct FieldSpec
		{
			bool zeroFlag{false};
			bool altFlag{false};
			sint32 width{0};
			std::optional<sint32> precision;
		};

		enum class ArgKind
		{
			Signed64,
			Unsigned64,
			Signed32,
			Unsigned32,
			Hex64,
			Hex32,
			Pointer,
			String,
			Char,
			Double,
		};

		struct ConversionToken
		{
			const char* text;
			ArgKind kind;
			bool upper;
		};

		// recognised length modifier + conversion letter sequences. None is a prefix of another
		constexpr ConversionToken s_conversionTokens[] =
		{
			{"lld", ArgKind::Signed64, false},
			{"lli", ArgKind::Signed64, false},
			{"llu", ArgKind::Unsigned64, false},
			{"d", ArgKind::Signed32, false},
			{"i", ArgKind::Signed32, false},
			{"ld", ArgKind::Signed32, false},
			{"li", ArgKind::Signed32, false},
			{"u", ArgKind::Unsigned32, false},
			{"lu", ArgKind::Unsigned32, false},
			{"llx", ArgKind::Hex64, false},
			{"llX", ArgKind::Hex64, true},
			{"x", ArgKind::Hex32, false},
			{"X", ArgKind::Hex32, true},
			{"p", ArgKind::Pointer, false},
			{"s", ArgKind::String, false},
			{"c", ArgKind::Char, false},
			{"f", ArgKind::Double, false},
			{"g", ArgKind::Double, false},
			{"G", ArgKind::Double, false},
			{"lf", ArgKind::Double, false},
		};

		const ConversionToken* MatchConversion(const char* text)
		{
			for (const ConversionToken& token : s_conversionTokens)
			{
				size_t n = strlen(token.text);
				if (strncmp(text, token.text, n) == 0)
					return &token;
			}
			return nullptr;
		}

		uint32 TakeArg32(ppc_va_list* args)
		{
			return *(uint32be*)_ppc_va_arg(args, ppc_va_type::INT32);
		}

		uint64 TakeArg64(ppc_va_list* args)
		{
			return *(uint64be*)_ppc_va_arg(args, ppc_va_type::INT64);
		}

		double TakeArgDouble(ppc_va_list* args)
		{
			return *(betype<double>*)_ppc_va_arg(args, ppc_va_type::FLOAT_OR_DOUBLE);
		}

		// parses a run of decimal digits with 32-bit wrap-around
		sint32 ReadDecimal(const char*& text)
		{
			uint32 v = 0;
			while (*text >= '0' && *text <= '9')
			{
				v = v * 10 + (uint32)(*text - '0');
				text++;
			}
			return (sint32)v;
		}

		void EmitInteger(FormatSink& out, bool negative, uint64 magnitude, bool hex, bool upper, const FieldSpec& spec)
		{
			if (out.Room() <= 0)
				return;
			// digits, least significant first
			char digits[24];
			sint32 digitCount = 0;
			uint64 rest = magnitude;
			const char* hexAlphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
			do
			{
				if (hex)
				{
					digits[digitCount++] = hexAlphabet[rest & 0xF];
					rest >>= 4;
				}
				else
				{
					digits[digitCount++] = (char)('0' + rest % 10);
					rest /= 10;
				}
			} while (rest != 0);
			// an explicit precision of zero prints nothing for the value zero
			if (spec.precision && *spec.precision == 0 && magnitude == 0)
				digitCount = 0;
			sint32 leadingZeroes = 0;
			if (spec.precision && *spec.precision > digitCount)
				leadingZeroes = *spec.precision - digitCount;
			const bool wantsPrefix = hex && spec.altFlag && magnitude != 0;
			// the field length includes "0x" only if two characters were available when the field started
			sint64 fieldLength = (sint64)digitCount + leadingZeroes + (negative ? 1 : 0) + ((wantsPrefix && out.Room() >= 2) ? 2 : 0);
			// the zero flag pads between the sign/prefix and the digits, and only without a precision
			const bool zeroFill = spec.width > 0 && spec.zeroFlag && !spec.precision;

			if (spec.width > 0 && !zeroFill)
				out.Repeat(' ', (sint64)spec.width - fieldLength);
			if (negative)
				out.Put('-');
			bool prefixWritten = false;
			if (wantsPrefix && out.Room() >= 2)
			{
				out.Put('0');
				out.Put(upper ? 'X' : 'x');
				prefixWritten = true;
			}
			if (zeroFill)
				out.Repeat('0', (sint64)spec.width - digitCount - (negative ? 1 : 0) - (prefixWritten ? 2 : 0));
			else
				out.Repeat('0', leadingZeroes);
			for (sint32 i = digitCount - 1; i >= 0 && out.Room() > 0; i--)
				out.Put(digits[i]);
			if (spec.width < 0)
				out.Repeat(' ', -(sint64)spec.width - fieldLength);
		}

		void EmitString(FormatSink& out, MPTR address, FieldSpec spec)
		{
			if (out.Room() <= 0)
				return;
			const char* text;
			if (address != MPTR_NULL)
				text = (const char*)memory_getPointerFromVirtualOffset(address);
			else
			{
				text = "(null)";
				if (spec.precision && *spec.precision > 6)
					spec.precision = 6;
			}
			sint32 length = 0;
			if (spec.precision)
			{
				sint32 limit = *spec.precision;
				while (length < limit && text[length] != '\0')
					length++;
				if (limit < 0)
				{
					cemuLog_log(LogType::APIErrors, "ppc_vprintf: negative string precision");
					length = 0;
				}
			}
			else
				length = (sint32)strlen(text);
			if (spec.width > 0)
				out.Repeat(' ', (sint64)spec.width - length);
			sint32 shown = std::min(length, out.Room());
			out.Append(text, shown);
			if (spec.width < 0)
				out.Repeat(' ', -(sint64)spec.width - shown);
		}

		void EmitChar(FormatSink& out, char c, const FieldSpec& spec)
		{
			if (out.Room() <= 0)
				return;
			if (spec.width > 0)
				out.Repeat(' ', (sint64)spec.width - 1);
			out.Put(c);
			if (spec.width < 0)
				out.Repeat(' ', -(sint64)spec.width - 1);
		}

		// floating point goes through the host printf with the guest's own flags. A '*' width or
		// precision is replaced by the value already taken from the argument list
		void EmitDouble(FormatSink& out, const char* specStart, const char* specEnd, const FieldSpec& spec, std::optional<sint32> starWidth, std::optional<sint32> starPrecision, double value)
		{
			std::string hostFormat;
			for (const char* p = specStart; p < specEnd; p++)
			{
				if (*p == '*')
				{
					bool isPrecision = p > specStart && p[-1] == '.';
					sint32 v = (isPrecision ? starPrecision : starWidth).value_or(0);
					if (isPrecision && v < 0)
						hostFormat.pop_back(); // a negative precision counts as none: drop the '.'
					else
						hostFormat.append(fmt::format("{}", v));
					continue;
				}
				hostFormat.push_back(*p);
			}
			if (hostFormat.size() > 63)
				hostFormat.resize(63);
			char rendered[4096];
			int n = snprintf(rendered, sizeof(rendered), hostFormat.c_str(), value);
			if (n < 0)
				return;
			n = std::min<int>(n, (int)sizeof(rendered) - 1);
			out.Append(rendered, n);
		}
	}

	sint32 ppc_vprintf(const char* formatStr, char* strOut, sint32 maxLength, ppc_va_list* vargs)
	{
		if (maxLength <= 0)
			return 0;
		FormatSink out(strOut, maxLength);
		const char* f = formatStr;
		while (*f != '\0')
		{
			if (*f != '%')
			{
				if (out.Room() <= 0)
					break;
				out.Put(*f++);
				continue;
			}
			const char* specStart = f;
			f++;
			if (*f == '%')
			{
				if (out.Room() <= 0)
					break;
				out.Put('%');
				f++;
				continue;
			}
			// flags are only recognised in this order; '+' and ' ' are accepted but have no effect on integers
			FieldSpec spec;
			bool leftAlign = false;
			if (*f == '-') { leftAlign = true; f++; }
			if (*f == '+') f++;
			if (*f == ' ') f++;
			if (*f == '#') { spec.altFlag = true; f++; }
			if (*f == '0') { spec.zeroFlag = true; f++; }
			// width
			std::optional<sint32> starWidth;
			if (*f == '*')
			{
				f++;
				sint32 w = (sint32)TakeArg32(vargs);
				starWidth = w;
				if (leftAlign)
					w = (sint32)(0u - (uint32)(w < 0 ? -(sint64)w : (sint64)w));
				spec.width = w;
			}
			else
			{
				sint32 w = ReadDecimal(f);
				spec.width = leftAlign ? (sint32)(0u - (uint32)w) : w;
			}
			// precision
			std::optional<sint32> starPrecision;
			if (*f == '.')
			{
				f++;
				if (*f == '*')
				{
					f++;
					sint32 p = (sint32)TakeArg32(vargs);
					starPrecision = p;
					if (p >= 0)
						spec.precision = p;
				}
				else
					spec.precision = ReadDecimal(f);
			}
			const ConversionToken* token = MatchConversion(f);
			if (!token)
			{
				cemu_assert_debug(false); // conversion not supported, the rest of the format is dropped
				break;
			}
			f += strlen(token->text);
			switch (token->kind)
			{
			case ArgKind::Signed64:
			{
				uint64 raw = TakeArg64(vargs);
				bool negative = (sint64)raw < 0;
				EmitInteger(out, negative, negative ? (0ull - raw) : raw, false, false, spec);
				break;
			}
			case ArgKind::Unsigned64:
				EmitInteger(out, false, TakeArg64(vargs), false, false, spec);
				break;
			case ArgKind::Signed32:
			{
				sint32 v = (sint32)TakeArg32(vargs);
				EmitInteger(out, v < 0, (uint64)(v < 0 ? -(sint64)v : (sint64)v), false, false, spec);
				break;
			}
			case ArgKind::Unsigned32:
				EmitInteger(out, false, TakeArg32(vargs), false, false, spec);
				break;
			case ArgKind::Hex64:
				EmitInteger(out, false, TakeArg64(vargs), true, token->upper, spec);
				break;
			case ArgKind::Hex32:
				EmitInteger(out, false, TakeArg32(vargs), true, token->upper, spec);
				break;
			case ArgKind::Pointer:
			{
				// pointers print as 0x followed by at least eight lowercase hex digits
				uint32 v = TakeArg32(vargs);
				FieldSpec ptrSpec = spec;
				ptrSpec.altFlag = true;
				if (!ptrSpec.precision)
					ptrSpec.precision = 8;
				EmitInteger(out, false, v, true, false, ptrSpec);
				break;
			}
			case ArgKind::String:
				EmitString(out, TakeArg32(vargs), spec);
				break;
			case ArgKind::Char:
				EmitChar(out, (char)TakeArg32(vargs), spec);
				break;
			case ArgKind::Double:
			{
				double v = TakeArgDouble(vargs);
				EmitDouble(out, specStart, f, spec, starWidth, starPrecision, v);
				break;
			}
			}
		}
		// the result is always terminated; a full buffer loses its last character to the terminator
		sint32 length = std::min(out.Used(), maxLength - 1);
		strOut[length] = '\0';
		return length;
	}

	sint32 __os_snprintf(char* outputStr, sint32 maxLength, const char* formatStr)
	{
		ppc_define_va_list(3, 0);
		return ppc_vprintf(formatStr, outputStr, maxLength, &vargs);
	}

	/* developer console */

	namespace
	{
		// collects console output into lines; a line goes to the log on '\n' or when it is full
		class ConsoleLineCollector
		{
		public:
			explicit ConsoleLineCollector(std::string_view tag) : m_tag(tag) {}

			void Feed(const char* data, sint32 length)
			{
				for (sint32 i = 0; i < length; i++)
				{
					char c = data[i];
					if (c == '\r')
						continue;
					if (c == '\n')
					{
						Flush();
						continue;
					}
					m_line[m_lineLength++] = c;
					if (m_lineLength == m_line.size())
						Flush();
				}
			}

		private:
			void Flush()
			{
				cemuLog_log(LogType::CoreinitLogging, "[{0}] {1}", m_tag, std::string_view(m_line.data(), m_lineLength));
				m_lineLength = 0;
			}

			std::string_view m_tag;
			std::array<char, 270> m_line;
			size_t m_lineLength{0};
		};

		std::mutex s_consoleMutex;
		ConsoleLineCollector s_osConsole("OSConsole");
		bool s_echoConsoleToStdout = false;

		void ConsoleOutput(const char* data, sint32 length)
		{
			std::lock_guard<std::mutex> lock(s_consoleMutex);
			if (s_echoConsoleToStdout)
			{
				fwrite(data, 1, length, stdout);
				fflush(stdout);
			}
			s_osConsole.Feed(data, length);
		}

		constexpr sint32 kReportBufferSize = 1024;

		// formats with a host-side prefix in front of the guest's text
		void ReportWithPrefix(const std::string& prefix, const char* format, ppc_va_list* vargs)
		{
			char buffer[kReportBufferSize];
			sint32 prefixLength = (sint32)std::min<size_t>(prefix.size(), kReportBufferSize - 1);
			memcpy(buffer, prefix.data(), prefixLength);
			sint32 bodyLength = ppc_vprintf(format, buffer + prefixLength, kReportBufferSize - prefixLength, vargs);
			ConsoleOutput(buffer, prefixLength + bodyLength);
		}
	}

	void COSVReport(COSReportModule module, COSReportLevel level, const char* format, ppc_va_list* vargs)
	{
		ReportWithPrefix("", format, vargs);
	}

	void OSReport(const char* format)
	{
		ppc_define_va_list(1, 0);
		COSVReport(COSReportModule::coreinit, COSReportLevel::Info, format, &vargs);
	}

	void OSVReport(const char* format, ppc_va_list* vargs)
	{
		COSVReport(COSReportModule::coreinit, COSReportLevel::Info, format, vargs);
	}

	void COSWarn(int moduleId, const char* format)
	{
		ppc_define_va_list(2, 0);
		ReportWithPrefix(fmt::format("[COSWarn-{}] ", moduleId), format, &vargs);
	}

	void OSLogPrintf(int ukn1, int ukn2, int ukn3, const char* format)
	{
		ppc_define_va_list(4, 0);
		ReportWithPrefix(fmt::format("[OSLogPrintf-{}-{}-{}] ", ukn1, ukn2, ukn3), format, &vargs);
	}

	void OSConsoleWrite(const char* strPtr, sint32 length)
	{
		if (length >= 0)
			ConsoleOutput(strPtr, length);
	}

	void OSFatal(const char* msg)
	{
		{
			ppc_define_va_list(1, 0);
			COSVReport(COSReportModule::coreinit, COSReportLevel::Error, msg, &vargs);
		}
		// show the message on both screens with OSScreen and never return
		OSScreenInit();
		uint32 tvBytes = OSScreenGetBufferSizeEx(OSSCREEN_TV);
		OSScreenSetBufferEx(OSSCREEN_TV, MEMORY_MEM1_AREA_ADDR);
		OSScreenSetBufferEx(OSSCREEN_DRC, MEMORY_MEM1_AREA_ADDR + tvBytes);
		OSScreenEnableEx(OSSCREEN_TV, 1);
		OSScreenEnableEx(OSSCREEN_DRC, 1);
		const uint32 screens[2] = { OSSCREEN_TV, OSSCREEN_DRC };
		for (;;)
		{
			for (uint32 screen : screens)
			{
				OSScreenClearBufferEx(screen, 0);
				OSScreenPutFontEx(screen, 0, 0, msg);
				OSScreenFlipBuffersEx(screen);
			}
			OSSleepTicks(ESPRESSO_TIMER_CLOCK / 30);
		}
	}

	/* HOME button menu */

	namespace
	{
		bool s_homeMenuAllowed = true;
	}

	bool OSIsHomeButtonMenuEnabled()
	{
		return s_homeMenuAllowed;
	}

	bool OSEnableHomeButtonMenu(bool enable)
	{
		s_homeMenuAllowed = enable;
		return true;
	}

	/* process and title information */

	namespace
	{
		constexpr uint32 kGameProcessId = 15;
		uint64 s_runningTitleId = 0;
		uint32 s_runningSdkVersion = 0;
	}

	uint32 OSGetPFID()
	{
		return kGameProcessId;
	}

	uint32 OSGetUPID()
	{
		return kGameProcessId;
	}

	uint64 OSGetTitleID()
	{
		return s_runningTitleId;
	}

	uint32 __OSGetProcessSDKVersion()
	{
		return s_runningSdkVersion;
	}

	/* relaunching and exiting */

	namespace
	{
		constexpr uint32 kLaunchFailed = 0x80000000;

		// The calling guest thread is destroyed when the title shuts down, so shutdown and relaunch run
		// on a detached host thread while the caller suspends itself.
		void RunDetachedAndSuspendCaller(std::function<void()> job)
		{
			std::thread(std::move(job)).detach();
			OSSuspendThread(OSGetCurrentThread());
		}

		void RelaunchTitle(uint64 titleId, std::vector<std::string> args)
		{
			// arguments are handed to the next launch directly instead of going through the kernel and MCP
			CafeSystem::SetOverrideArgs(args);
			RunDetachedAndSuspendCaller([titleId]()
			{
				CafeSystem::ShutdownTitle();
				CafeSystem::PrepareForegroundTitle(titleId);
				CafeSystem::RequestRecreateCanvas();
				CafeSystem::LaunchForegroundTitle();
			});
		}

		enum class AppXmlResult
		{
			Found,
			CannotOpen,
			Malformed,
		};

		// reads <title_id> from a title's code/app.xml (0 if the element is missing or not a number)
		AppXmlResult ReadTitleIdFromAppXml(const std::string& titlePath, uint64& titleId)
		{
			std::string xmlPath = titlePath + "/code/app.xml";
			sint32 fscStatus;
			FSCVirtualFile* file = fsc_open(xmlPath.c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &fscStatus);
			if (!file)
				return AppXmlResult::CannotOpen;
			std::vector<uint8> xml(fsc_getFileSize(file));
			fsc_readFile(file, xml.data(), (uint32)xml.size());
			fsc_close(file);
			pugi::xml_document doc;
			if (!doc.load_buffer_inplace(xml.data(), xml.size()))
				return AppXmlResult::Malformed;
			const char* text = doc.child("app").child("title_id").child_value();
			titleId = strtoull(text, nullptr, 16);
			return AppXmlResult::Found;
		}
	}

	uint32 OSLaunchTitleByPathl(const char* path, uint32 pathLength, uint32 argc)
	{
		cemu_assert_debug(argc == 0); // launch arguments are not passed on
		if (pathLength >= 1024 - 32)
		{
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: path too long");
			return kLaunchFailed;
		}
		uint64 titleId = 0;
		switch (ReadTitleIdFromAppXml(std::string(path, strnlen(path, pathLength)), titleId))
		{
		case AppXmlResult::CannotOpen:
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: cannot open the title's app.xml");
			return kLaunchFailed;
		case AppXmlResult::Malformed:
			return 0; // nothing is launched, but this is not reported as an error
		case AppXmlResult::Found:
			break;
		}
		if (titleId == 0)
		{
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: no title id found");
			return kLaunchFailed;
		}
		RelaunchTitle(titleId, {});
		return 0;
	}

	uint32 OSRestartGame(uint32 argc, MEMPTR<char>* argv)
	{
		std::vector<std::string> args;
		for (uint32 i = 0; i < argc; i++)
			args.emplace_back(argv[i].GetPtr());
		RelaunchTitle(CafeSystem::GetForegroundTitleId(), std::move(args));
		return 0;
	}

	void __PPCExit(sint32 status)
	{
		RunDetachedAndSuspendCaller([status]()
		{
			CafeSystem::ShutdownTitle();
			CafeSystem::NotifyPPCProcessExit(status);
		});
	}

	void coreinit_exit(sint32 status)
	{
		cemuLog_log(LogType::Force, "The title terminated the process by calling coreinit.exit({})", status);
		DebugLogStackTrace(OSGetCurrentThread(), OSGetStackPointer());
		// run the guest's atexit handlers and stdio teardown before the process goes away
		if (gCoreinitData->__atexit_cleanup)
			PPCCoreCallback(gCoreinitData->__atexit_cleanup, status);
		if (gCoreinitData->__stdio_cleanup)
			PPCCoreCallback(gCoreinitData->__stdio_cleanup);
		__PPCExit(status);
	}

	/* foreground/background */

	void OSReleaseForeground()
	{
		cemuLog_logDebug(LogType::Force, "OSReleaseForeground is not implemented");
	}

	namespace
	{
		// a requested background+foreground round trip, delivered one message at a time
		bool s_pendingReleaseForeground = false;
		bool s_pendingAcquireForeground = false;
	}

	void StartBackgroundForegroundTransition()
	{
		s_pendingReleaseForeground = true;
		s_pendingAcquireForeground = true;
	}

	// hook at the start of OSReceiveMessage on the system message queue: injects pending transition messages
	void UpdateSystemMessageQueue()
	{
		if (!OSIsInterruptEnabled())
			return;
		cemu_assert_debug(!__OSHasSchedulerLock());
		OSMessage msg{};
		bool* pending;
		if (s_pendingReleaseForeground)
		{
			msg.data0 = stdx::to_underlying(SysMessageId::MsgReleaseForeground);
			msg.data1 = 0; // 0 = moving to the background, 1 = system shutdown
			pending = &s_pendingReleaseForeground;
		}
		else if (s_pendingAcquireForeground)
		{
			msg.data0 = stdx::to_underlying(SysMessageId::MsgAcquireForeground);
			msg.data1 = 1;
			msg.data2 = 1;
			pending = &s_pendingAcquireForeground;
		}
		else
			return;
		if (OSSendMessage(OSGetSystemMessageQueue(), &msg, 0))
			*pending = false;
	}

	// hook for messages OSReceiveMessage takes from the system message queue
	void HandleReceivedSystemMessage(OSMessage* msg)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		cemuLog_log(LogType::Force, "Receiving message: {:08x}", (uint32)msg->data0);
	}

	/* drivers */

	uint32 OSDriver_Register(uint32 moduleHandle, sint32 priority, OSDriverInterface* driverCallbacks, sint32 driverId, uint32be* outUkn1, uint32be* outUkn2, uint32be* outUkn3)
	{
		cemuLog_logDebug(LogType::Force, "OSDriver_Register: not implemented");
		return 0;
	}

	uint32 OSDriver_Deregister(uint32 moduleHandle, sint32 driverId)
	{
		cemuLog_logDebug(LogType::Force, "OSDriver_Deregister: not implemented");
		return 0;
	}

	void miscInit()
	{
		s_runningTitleId = CafeSystem::GetForegroundTitleId();
		s_runningSdkVersion = CafeSystem::GetForegroundTitleSDKVersion();
		s_echoConsoleToStdout = LaunchSettings::ForwardConsoleLogging();
		s_pendingReleaseForeground = false;
		s_pendingAcquireForeground = false;

		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::Placeholder)
		EXPORT_FN(__os_snprintf);

		EXPORT_FN(COSVReport); EXPORT_FN(COSWarn); EXPORT_FN(OSReport);
		EXPORT_FN(OSVReport); EXPORT_FN(OSLogPrintf); EXPORT_FN(OSConsoleWrite);
		EXPORT_FN(OSFatal);

		EXPORT_FN(OSGetPFID); EXPORT_FN(OSGetUPID); EXPORT_FN(OSGetTitleID);
		EXPORT_FN(__OSGetProcessSDKVersion);
#undef EXPORT_FN

		// the HOME menu starts out allowed; some titles (Disney Infinity 2.0) query erreula state
		// through it before setting erreula up and crash if it is off
		s_homeMenuAllowed = true;
		cafeExportRegister("coreinit", OSIsHomeButtonMenuEnabled, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSEnableHomeButtonMenu, LogType::CoreinitThread);

		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::Placeholder)
		EXPORT_FN(OSLaunchTitleByPathl); EXPORT_FN(OSRestartGame);

		EXPORT_FN(OSReleaseForeground);

		EXPORT_FN(OSDriver_Register); EXPORT_FN(OSDriver_Deregister);
#undef EXPORT_FN

		cafeExportRegisterFunc(coreinit_exit, "coreinit", "exit", LogType::CoreinitThread);
		cafeExportRegister("coreinit", __PPCExit, LogType::CoreinitThread);
	}
}
