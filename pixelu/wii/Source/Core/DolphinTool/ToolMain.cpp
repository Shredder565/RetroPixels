// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/ostream.h>

#include "DolphinTool/ConvertCommand.h"
#include "DolphinTool/ExtractCommand.h"
#include "DolphinTool/HeaderCommand.h"
#include "DolphinTool/VerifyCommand.h"

#ifdef _WIN32
#include <windows.h>

#include "Common/StringUtil.h"
#endif

namespace
{
using CommandFn = int (*)(const std::vector<std::string>&);

struct Command
{
  std::string_view name;
  CommandFn run;
};

constexpr Command COMMANDS[] = {
    {"convert", DolphinTool::ConvertCommand},
    {"verify", DolphinTool::VerifyCommand},
    {"header", DolphinTool::HeaderCommand},
    {"extract", DolphinTool::Extract},
};

int Usage()
{
  fmt::print(std::cerr, "usage: dolphin-tool COMMAND -h\n"
                        "\n"
                        "commands supported: [convert, verify, header, extract]\n");
  return EXIT_FAILURE;
}

// argv[1] picks the command, which gets the arguments after it.
int Dispatch(int argc, char** argv)
{
  if (argc < 2)
    return Usage();

  const std::string_view requested = argv[1];
  for (const Command& command : COMMANDS)
  {
    if (command.name == requested)
      return command.run(std::vector<std::string>(argv + 2, argv + argc));
  }
  return Usage();
}
}  // namespace

#ifdef _WIN32
// The wide command line is converted to UTF-8 here instead of using the narrow argv, which would
// be in the ANSI code page.
int wmain(int, wchar_t*[], wchar_t*[])
{
  std::vector<std::string> arguments = Common::CommandLineToUtf8Argv(GetCommandLineW());
  std::vector<char*> pointers;
  pointers.reserve(arguments.size());
  for (std::string& argument : arguments)
    pointers.push_back(argument.data());
  return Dispatch(static_cast<int>(pointers.size()), pointers.data());
}
#else
int main(int argc, char* argv[])
{
  return Dispatch(argc, argv);
}
#endif
