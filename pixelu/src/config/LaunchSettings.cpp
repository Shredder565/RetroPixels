// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "LaunchSettings.h"

#include "util/helpers/helpers.h"
#include "Cafe/Account/Account.h"

#include "Cafe/OS/libs/coreinit/coreinit.h"

#include "boost/program_options.hpp"

#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "util/crypto/aes128.h"

#include "Cafe/Filesystem/FST/FST.h"
#include "Cafe/TitleList/TitleId.h"
#include "util/helpers/StringHelpers.h"

void requireConsole(); // main.cpp

namespace po = boost::program_options;

namespace
{
	// Values of short options written as "-g=value" arrive with the '='
	// still attached, so a single leading '=' is removed.
	template<typename TString>
	TString withoutLeadingEquals(TString text)
	{
		if (!text.empty() && text.front() == '=')
			text.erase(0, 1);
		return text;
	}

	template<typename T>
	T argument(const po::variables_map& vm, const char* name)
	{
		return vm[name].as<T>();
	}

	// "--name" alone means true; "--name false" etc. is also accepted
	po::typed_value<bool>* boolSwitch()
	{
		return po::value<bool>()->implicit_value(true);
	}

	// --rp-parent: decimal or 0x-prefixed hexadecimal, non-zero, nothing else
	std::optional<uint64> parseWindowHandle(const std::string& text)
	{
		const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
		const std::string digits = hex ? text.substr(2) : text;
		size_t consumed = 0;
		const uint64 handle = std::stoull(digits, &consumed, hex ? 16 : 10); // throws on garbage
		if (handle == 0 || consumed != digits.size())
			return std::nullopt;
		return handle;
	}

	// "host path:guest path"; on Windows a drive letter colon at the start does not count
	size_t findMountSeparator(const std::wstring& mount)
	{
		size_t searchFrom = 0;
#if BOOST_OS_WINDOWS
		static const std::wregex drivePrefix(LR"(^(?:\\\\\?\\)?[A-Za-z]:)");
		std::wsmatch match;
		if (std::regex_search(mount, match, drivePrefix))
			searchFrom = (size_t)match.length();
#endif
		return mount.find(L':', searchFrom);
	}

	std::string versionText()
	{
#if EMULATOR_VERSION_PATCH == 0
		return fmt::format("{}.{}{}", EMULATOR_VERSION_MAJOR, EMULATOR_VERSION_MINOR, EMULATOR_VERSION_SUFFIX);
#else
		return fmt::format("{}.{}-{}{}", EMULATOR_VERSION_MAJOR, EMULATOR_VERSION_MINOR, EMULATOR_VERSION_PATCH, EMULATOR_VERSION_SUFFIX);
#endif
	}
}

std::optional<int> LaunchSettings::HandleCommandline(const wchar_t* lpCmdLine)
{
#if BOOST_OS_WINDOWS
	return HandleCommandline(po::split_winmain(lpCmdLine));
#else
	cemu_assert_unimplemented();
	return 1;
#endif
}

std::optional<int> LaunchSettings::HandleCommandline(int argc, wchar_t* argv[])
{
	return HandleCommandline(std::vector<std::wstring>(argv, argv + argc));
}

std::optional<int> LaunchSettings::HandleCommandline(int argc, char* argv[])
{
	std::vector<std::wstring> args;
	args.reserve(argc);
	for (int i = 0; i < argc; i++)
		args.push_back(boost::nowide::widen(argv[i]));
	return HandleCommandline(args);
}

std::optional<int> LaunchSettings::HandleCommandline(const std::vector<std::wstring>& args)
{
	po::options_description launchOptions{"Launch options"};
	launchOptions.add_options()
		("help,h", "Show this list of options")
		("version,v", "Print the version number")
#if !BOOST_OS_WINDOWS
		("verbose", "Also write the log to stdout")
#endif

		("game,g", po::wvalue<std::wstring>(), "Game file or folder to start")
		("title-id,t", po::value<std::string>(), "Title ID (hex) of an installed title to start; --game takes precedence")
		("mlc,m", po::wvalue<std::wstring>(), "Use this MLC folder instead of the configured one")

		("fullscreen,f", boolSwitch(), "Start games in fullscreen")
		("ud,u", boolSwitch(), "Flip the output vertically")

		("account,a", po::value<std::string>(), "Persistent ID (hex) of the account to use")

		("cos-mounts", po::wvalue<std::vector<std::wstring>>()->composing(), "Extra mounts, each written as host path:guest path (for example /tmp:/vol/temporary/)")
		("forward-console-logging", "Print OSReport, OSConsoleWrite and similar guest output to stdout/stderr")
		("cos-argstr", po::value<std::string>(), "Argument string for the first RPX started ('file.rpx ...'); used once")

		("force-interpreter", boolSwitch(), "Use the single-core CPU interpreter only (no recompiler); slow, but gives exact memory accesses and stack traces")
		("force-multicore-interpreter", boolSwitch(), "Use the multi-core CPU interpreter only (no recompiler); for stack traces, a little faster than --force-interpreter")
		("enable-gdbstub", boolSwitch(), "Start the GDB stub so an external debugger can attach")
		("open-debugger", boolSwitch(), "Open the PPC debugger window at startup")

		// PixelU: options used by RetroPixels
		("rp-record", po::wvalue<std::wstring>(), "RetroPixels: record the game's TV picture and audio into this folder (needs --rp-ffmpeg)")
		("rp-ffmpeg", po::wvalue<std::wstring>(), "RetroPixels: full path of the ffmpeg executable used for recording")
		("rp-opengl", "RetroPixels: draw with OpenGL instead of Vulkan for this run (Vulkan doesn't work on this PC)")
		("rp-embed", "RetroPixels: a window that shows only the game, for docking into another window; no menus, game list or optional dialogs")
		("rp-parent", po::value<std::string>(), "RetroPixels: show the game as a child of this window (Windows HWND or X11 window id, decimal or 0x hex), filling it; implies --rp-embed");

	po::options_description hiddenOptions{"Hidden options"};
	hiddenOptions.add_options()
		("nsight", boolSwitch(), "Settings for debugging with NSight")
		("legacy", boolSwitch(), "Legacy Intel graphics mode")
		("ppcrec-lower-addr", po::value<std::string>(), "Debugging: lowest address the PPC recompiler may translate")
		("ppcrec-upper-addr", po::value<std::string>(), "Debugging: highest address the PPC recompiler may translate");

	po::options_description extractorOptions{"Extractor tool"};
	extractorOptions.add_options()
		("extract,e", po::wvalue<std::wstring>(), "WUD or WUX image to extract a file from")
		("path,p", po::value<std::string>(), "Path of the file inside the image (for example meta/meta.xml)")
		("output,o", po::wvalue<std::wstring>(), "Folder to write the extracted file to (default: print it)");

	po::options_description allOptions;
	allOptions.add(launchOptions).add(hiddenOptions).add(extractorOptions);
	po::options_description shownInHelp;
	shownInHelp.add(launchOptions).add(extractorOptions);

	try
	{
		namespace style = po::command_line_style;
		po::wcommand_line_parser parser{args};
		parser.allow_unregistered()
			.options(allOptions)
			.style(style::allow_long | style::allow_short | style::allow_dash_for_short | style::case_insensitive |
				   style::long_allow_next | style::short_allow_next | style::allow_long_disguise);
		po::variables_map vm;
		po::store(parser.run(), vm);
		po::notify(vm);
		auto has = [&vm](const char* name) { return vm.count(name) != 0; };

		if (has("help"))
		{
			requireConsole();
			std::cout << shownInHelp << std::endl;
			return 0;
		}
		if (has("version"))
		{
			requireConsole();
			std::cout << versionText() << std::endl;
			return 0;
		}

		if (has("verbose"))
			s_verbose = true;

		if (has("game"))
			s_load_game_file = withoutLeadingEquals(argument<std::wstring>(vm, "game"));
		if (has("title-id"))
		{
			try
			{
				const std::string text = withoutLeadingEquals(argument<std::string>(vm, "title-id"));
				TitleId titleId{};
				if (!TitleIdParser::ParseFromStr(text, titleId))
					std::cerr << "Invalid title id format";
				s_load_title_id = titleId; // also set (to 0) when invalid
			}
			catch (const std::invalid_argument&)
			{
				std::cerr << "Expected title_param ID as an unsigned 64-bit hexadecimal string\n";
			}
		}
		if (has("mlc"))
			s_mlc_path = withoutLeadingEquals(argument<std::wstring>(vm, "mlc"));

		// PixelU: recording needs both the output folder and ffmpeg
		if (has("rp-record") && has("rp-ffmpeg"))
		{
			s_rp_record_path = fs::path(withoutLeadingEquals(argument<std::wstring>(vm, "rp-record")));
			s_rp_ffmpeg_path = fs::path(withoutLeadingEquals(argument<std::wstring>(vm, "rp-ffmpeg")));
		}
		if (has("rp-embed"))
			s_rp_embed = true;
		if (has("rp-opengl"))
			s_rp_opengl = true;
		if (has("rp-parent"))
		{
			std::optional<uint64> handle;
			try
			{
				handle = parseWindowHandle(withoutLeadingEquals(argument<std::string>(vm, "rp-parent")));
			}
			catch (const std::exception&)
			{
			}
			if (handle)
				s_rp_parent_window = *handle;
			else
				std::cerr << "Invalid --rp-parent window handle\n";
			s_rp_embed = true; // embedding is implied even when the handle is invalid
		}

		if (has("account"))
		{
			const uint32 persistentId = ConvertString<uint32>(argument<std::string>(vm, "account"), 16);
			if (persistentId >= Account::kMinPersistendId)
				s_persistent_id = persistentId;
		}

		if (has("fullscreen"))
			s_fullscreen = argument<bool>(vm, "fullscreen");
		if (has("ud"))
			s_render_upside_down = argument<bool>(vm, "ud");
		if (has("nsight"))
			s_nsight_mode = argument<bool>(vm, "nsight");
		if (has("force-interpreter"))
			s_force_interpreter = argument<bool>(vm, "force-interpreter");
		if (has("force-multicore-interpreter"))
			s_force_multicore_interpreter = argument<bool>(vm, "force-multicore-interpreter");
		if (has("enable-gdbstub"))
			s_enable_gdbstub = argument<bool>(vm, "enable-gdbstub");
		if (has("open-debugger"))
			s_open_debugger = argument<bool>(vm, "open-debugger");

		if (has("forward-console-logging"))
		{
			requireConsole();
			s_forward_console_logging = true;
		}

		if (has("cos-argstr"))
		{
			const std::string argstr = argument<std::string>(vm, "cos-argstr");
			// the first argument has to be the RPX file name
			const std::string_view firstArgument = std::string_view(argstr).substr(0, argstr.find(' '));
			if (!firstArgument.ends_with(".rpx"))
			{
				std::cout << "--cos-argstr's first argument _must_ be a filename that ends in '.rpx'." << std::endl;
				return 1;
			}
			s_cos_argstr = argstr;
		}

		if (has("cos-mounts"))
		{
			for (const std::wstring& mount : argument<std::vector<std::wstring>>(vm, "cos-mounts"))
			{
				const size_t separator = findMountSeparator(mount);
				if (separator == std::wstring::npos)
				{
					std::cerr << "Argument for a mount expects to be in the format: `path on host:path in emulated system`, was not: `" << boost::nowide::narrow(mount) << "`\n";
					continue;
				}
				s_cos_mounts[mount.substr(0, separator)] = mount.substr(separator + 1);
			}
		}

		const std::wstring extractImage = has("extract") ? argument<std::wstring>(vm, "extract") : std::wstring();
		const std::string extractFile = has("path") ? argument<std::string>(vm, "path") : std::string();
		const std::wstring extractTo = has("output") ? argument<std::wstring>(vm, "output") : std::wstring();

		if (has("ppcrec-lower-addr"))
			ppcRec_limitLowerAddr = (uint32)StringHelpers::ToInt64(argument<std::string>(vm, "ppcrec-lower-addr"));
		if (has("ppcrec-upper-addr"))
			ppcRec_limitUpperAddr = (uint32)StringHelpers::ToInt64(argument<std::string>(vm, "ppcrec-upper-addr"));
		if (ppcRec_limitLowerAddr != 0 && ppcRec_limitUpperAddr != 0)
			cemuLog_log(LogType::Force, "PPCRec range limited to 0x{:08x}-0x{:08x}", ppcRec_limitLowerAddr, ppcRec_limitUpperAddr);

		if (!extractImage.empty())
		{
			ExtractorTool(extractImage, extractFile, extractTo);
			return 0;
		}
		return std::nullopt;
	}
	catch (const std::exception& ex)
	{
		std::cout << "Error while trying to parse command line parameter:\n" << ex.what() << std::endl;
		return 1;
	}
}

bool LaunchSettings::ExtractorTool(std::wstring_view wud_path, std::string_view output_path, std::wstring_view log_path)
{
	if (output_path.empty())
	{
		requireConsole();
		puts("Cannot extract files because no source path was specified (-p)\n");
		return false;
	}

	AES128_init();
	const fs::path imagePath(wud_path);
	std::vector<uint8> fileData;
	bool fileFound = false;
	{
		std::unique_ptr<FSTVolume> volume(FSTVolume::OpenFromDiscImage(imagePath));
		if (!volume)
		{
			requireConsole();
			puts(fmt::format("Unable to open \"{}\"\n", imagePath.generic_string()).c_str());
			return false;
		}
		fileData = volume->ExtractFile(output_path, &fileFound);
	}
	if (!fileFound)
	{
		requireConsole();
		puts(fmt::format("Unable to read file \"{}\"\n", output_path).c_str());
		return false;
	}

	if (log_path.empty())
	{
		// no output folder: print the file
		requireConsole();
		printf("%.*s", (int)fileData.size(), fileData.data());
		fflush(stdout);
		return true;
	}
	try
	{
		const fs::path target = fs::path(std::wstring(log_path)) / boost::nowide::widen(std::string(output_path));
		fs::create_directories(fs::path(target).remove_filename());
		std::ofstream out(target, std::ios::out | std::ios::binary);
		out.write((const char*)fileData.data(), (std::streamsize)fileData.size());
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't write file: {}", ex.what());
		puts(fmt::format("can't write file: {}\n", ex.what()).c_str());
	}
	return true;
}
