// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/CommandLineParse.h"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <OptionParser.h>

#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Config/MainSettings.h"

namespace CommandLineParse
{
namespace
{
// One command-line option. Null fields are left at optparse's defaults.
struct OptionSpec
{
  const char* short_flag;
  const char* long_flag;
  const char* action;
  const char* metavar;
  const char* type;
  const char* help;
};

constexpr OptionSpec COMMON_OPTIONS[] = {
    {"-u", "--user", "store", nullptr, nullptr, "User folder path"},
    {"-m", "--movie", "store", nullptr, nullptr, "Play a movie file"},
    {"-e", "--exec", "append", "<file>", "string", "Load the specified file"},
    {"-n", "--nand_title", "store", "<16-character ASCII title ID>", "string",
     "Launch a NAND title"},
    {"-C", "--config", "append", "<System>.<Section>.<Key>=<Value>", "string",
     "Set a configuration option"},
    {"-s", "--save_state", "store", "<file>", "string", "Load the initial save state"},
};

constexpr OptionSpec GUI_OPTIONS[] = {
    {"-d", "--debugger", "store_true", nullptr, nullptr,
     "Show the debugger pane and additional View menu options"},
    {"-l", "--logger", "store_true", nullptr, nullptr, "Open the logger"},
    {"-b", "--batch", "store_true", nullptr, nullptr,
     "Run Dolphin without the user interface (Requires --exec or --nand-title)"},
    {"-c", "--confirm", "store_true", nullptr, nullptr, "Set Confirm on Stop"},
};

void Register(optparse::OptionParser& parser, const OptionSpec& spec)
{
  optparse::Option& option = parser.add_option(spec.short_flag, spec.long_flag);
  // The action goes first: setting a type afterwards also fixes the argument count.
  option.action(spec.action);
  if (spec.metavar != nullptr)
    option.metavar(spec.metavar);
  if (spec.type != nullptr)
    option.type(spec.type);
  option.help(spec.help);
}

// Takes `text` up to the next `delimiter` (or its end), dropping the delimiter.
std::string TakeUntil(std::string_view* text, char delimiter)
{
  const std::size_t stop = text->find(delimiter);
  std::string piece(text->substr(0, stop));
  text->remove_prefix(stop == std::string_view::npos ? text->size() : stop + 1);
  return piece;
}

// The CommandLine config layer: the dedicated options first, then each --config override.
class ArgumentLayer final : public Config::ConfigLayerLoader
{
public:
  ArgumentLayer(const std::list<std::string>& overrides, const std::string& video_backend,
                const std::string& audio_emulation, bool batch, bool debugger)
      : ConfigLayerLoader(Config::LayerType::CommandLine)
  {
    if (!video_backend.empty())
      Add(Config::MAIN_GFX_BACKEND.GetLocation(), video_backend);
    if (!audio_emulation.empty())
      Add(Config::MAIN_DSP_HLE.GetLocation(), ValueToString(audio_emulation == "HLE"));

    // Batch mode has no main window, so the game must not render into it.
    if (batch)
      Add(Config::MAIN_RENDER_TO_MAIN.GetLocation(), ValueToString(false));
    if (debugger)
      Add(Config::MAIN_ENABLE_DEBUGGING.GetLocation(), ValueToString(true));

    // "<System>.<Section>.<Key>=<Value>"; overrides naming an unknown system are ignored. The key
    // may contain dots, and the value ends at a second '=' if there is one.
    for (const std::string& override_text : overrides)
    {
      std::string_view rest = override_text;
      const std::string system_name = TakeUntil(&rest, '.');
      std::string section = TakeUntil(&rest, '.');
      std::string key = TakeUntil(&rest, '=');
      std::string value = TakeUntil(&rest, '=');

      if (const std::optional<Config::System> system = Config::GetSystemFromName(system_name))
        Add(Config::Location{*system, std::move(section), std::move(key)}, std::move(value));
    }
  }

  void Load(Config::Layer* layer) override
  {
    for (const auto& [location, value] : m_settings)
      layer->Set(location, value);
  }

  // Command-line settings are never written back.
  void Save(Config::Layer*) override {}

private:
  void Add(Config::Location location, std::string value)
  {
    m_settings.emplace_back(std::move(location), std::move(value));
  }

  std::vector<std::pair<Config::Location, std::string>> m_settings;
};

void InstallConfigLayer(const optparse::Values& values)
{
  std::list<std::string> overrides;
  if (values.is_set_by_user("config"))
    overrides = values.all("config");

  const std::string video_backend = static_cast<const char*>(values.get("video_backend"));
  const std::string audio_emulation = static_cast<const char*>(values.get("audio_emulation"));
  const bool batch = static_cast<bool>(values.get("batch"));
  const bool debugger = static_cast<bool>(values.get("debugger"));

  Config::AddLayer(
      std::make_unique<ArgumentLayer>(overrides, video_backend, audio_emulation, batch, debugger));
}
}  // namespace

std::unique_ptr<optparse::OptionParser> CreateParser(ParserOptions options)
{
  auto parser = std::make_unique<optparse::OptionParser>();
  parser->usage("usage: %prog [options]... [FILE]...").version(Common::GetScmRevStr());

  for (const OptionSpec& spec : COMMON_OPTIONS)
    Register(*parser, spec);

  if (options == ParserOptions::IncludeGUIOptions)
  {
    for (const OptionSpec& spec : GUI_OPTIONS)
      Register(*parser, spec);
  }

  parser->set_defaults("video_backend", "");
  parser->set_defaults("audio_emulation", "");
  parser->add_option("-v", "--video_backend").action("store").help("Specify a video backend");
  parser->add_option("-a", "--audio_emulation")
      .choices({"HLE", "LLE"})
      .help("Choose audio emulation from [HLE|LLE]");

  return parser;
}

optparse::Values& ParseArguments(optparse::OptionParser* parser, int argc, char** argv)
{
  optparse::Values& values = parser->parse_args(argc, argv);
  InstallConfigLayer(values);
  return values;
}

optparse::Values& ParseArguments(optparse::OptionParser* parser,
                                 const std::vector<std::string>& arguments)
{
  optparse::Values& values = parser->parse_args(arguments);
  InstallConfigLayer(values);
  return values;
}
}  // namespace CommandLineParse
