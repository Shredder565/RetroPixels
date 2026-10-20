// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "boost/algorithm/string.hpp"
#include "util/helpers/StringParser.h"
#include "util/helpers/StringHelpers.h"
#include "util/IniParser/IniParser.h"
#include "Cafe/CafeSystem.h"
#include "HW/Espresso/Debugger/Debugger.h"
#include "Cafe/HW/Latte/Core/LatteTiming.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include <cinttypes>

std::vector<GraphicPackPtr> GraphicPack2::s_graphic_packs;
std::vector<GraphicPackPtr> GraphicPack2::s_active_graphic_packs;
std::atomic_bool GraphicPack2::s_isReady;

namespace
{
	// rules.txt files with this version or older are no longer supported
	constexpr sint32 kOldestUnsupportedVersion = 2;

	bool IsTrueText(std::string_view text)
	{
		return boost::iequals(text, "true") || boost::iequals(text, "1");
	}

	std::optional<std::vector<uint8>> ReadWholeFile(const fs::path& path)
	{
		std::unique_ptr<FileStream> file(FileStream::openFile2(path));
		if (!file)
			return std::nullopt;
		std::vector<uint8> data;
		file->extract(data);
		return data;
	}

	// reads a shader text file (text mode, like the renderers expect); throws when it cannot be opened
	std::string ReadShaderText(const fs::path& path, const char* failMessage)
	{
		std::ifstream in(path);
		if (!in.is_open())
			throw std::runtime_error(failMessage);
		std::string text;
		in.seekg(0, std::ios::end);
		text.reserve(in.tellg());
		in.seekg(0, std::ios::beg);
		text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return text;
	}

	// evaluates an option if it is present; returns false when the option is absent
	template<typename T>
	bool EvalOption(const ExpressionParser& parser, IniParser& rules, const char* option, T& out)
	{
		const auto text = rules.FindOption(option);
		if (!text)
			return false;
		out = parser.Evaluate<T>(*text);
		return true;
	}

	// comma separated list of expressions; entries that do not parse are left out
	std::vector<sint32> EvalList(const ExpressionParser& parser, IniParser& rules, const char* option)
	{
		std::vector<sint32> values;
		const auto text = rules.FindOption(option);
		if (!text)
			return values;
		for (const std::string& part : Tokenize(*text, ','))
		{
			try
			{
				values.push_back(parser.Evaluate<sint32>(part));
			}
			catch (const std::invalid_argument&)
			{
			}
		}
		return values;
	}

	bool IsVariableChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}

	std::unique_ptr<RendererOutputShader> MakeOutputShader(const std::string& fragmentSource, bool upsideDown)
	{
		if (fragmentSource.empty())
			return nullptr;
		return std::make_unique<RendererOutputShader>(RendererOutputShader::GetOpenGlVertexSource(upsideDown), fragmentSource);
	}

	RendererOutputShader* CachedOutputShader(std::unique_ptr<RendererOutputShader>& cache, const std::string& fragmentSource, bool upsideDown)
	{
		if (!cache)
			cache = MakeOutputShader(fragmentSource, upsideDown);
		return cache.get();
	}

	std::optional<GraphicPack2::PresetVar> FindVariable(const GraphicPack2::Preset& preset, std::string_view name)
	{
		auto it = preset.variables.find(std::string(name));
		if (it == preset.variables.end())
			return std::nullopt;
		return it->second;
	}
}

// ---------------------------------------------------------------- pack list

void GraphicPack2::LoadGraphicPack(fs::path graphicPackPath)
{
	const fs::path rulesPath = graphicPackPath / "rules.txt";
	auto rulesData = ReadWholeFile(rulesPath);
	if (!rulesData)
		return;
	const std::string rulesName = _pathToUtf8(rulesPath);
	IniParser rules(*rulesData, rulesName);

	if (!rules.NextSection())
	{
		cemuLog_log(LogType::Force, "Graphic pack {}: rules.txt has no sections", rulesName);
		return;
	}
	if (!boost::iequals(rules.GetCurrentSectionName(), "Definition"))
	{
		cemuLog_log(LogType::Force, "Graphic pack {}: rules.txt must start with [Definition]", rulesName);
		return;
	}
	if (auto versionText = rules.FindOption("version"))
	{
		sint32 version = -1;
		const char* begin = versionText->data();
		if (std::from_chars(begin, begin + versionText->size(), version).ec != std::errc{})
		{
			cemuLog_log(LogType::Force, "Graphic pack {}: version is not a number", rulesName);
			return;
		}
		if (version > kOldestUnsupportedVersion)
		{
			LoadGraphicPack(rulesPath, rules);
			return;
		}
	}
	cemuLog_log(LogType::Force, "Graphic pack {}: this rules.txt version is no longer supported", rulesName);
}

void GraphicPack2::LoadAll()
{
	std::error_code ec;
	const fs::path root = ActiveSettings::GetUserDataPath("graphicPacks");
	fs::recursive_directory_iterator walker(root, ec);
	for (; walker != fs::recursive_directory_iterator(); walker.increment(ec))
	{
		if (ec)
			break;
		if (!walker->is_directory(ec))
			continue;
		if (fs::exists(walker->path() / "rules.txt", ec))
		{
			LoadGraphicPack(walker->path());
			walker.disable_recursion_pending(); // a pack folder never holds other packs
		}
	}
}

bool GraphicPack2::LoadGraphicPack(const fs::path& rulesPath, IniParser& rules)
{
	std::shared_ptr<GraphicPack2> pack;
	try
	{
		pack = std::make_shared<GraphicPack2>(rulesPath, rules);
	}
	catch (const std::exception&)
	{
		return false;
	}

	// saved settings are keyed by the absolute path (older configs) or by the path relative to the user folder
	const auto& saved = GetConfigHandle().data().graphic_pack_entries;
	auto entry = saved.find(pack->GetRulesPath().lexically_normal());
	if (entry == saved.end())
		entry = saved.find(_utf8ToPath(pack->GetNormalizedPathString()));
	if (entry != saved.end())
	{
		bool enable = true;
		for (const auto& [key, value] : entry->second)
		{
			if (boost::iequals(key, "_disabled"))
				enable = false;
			else
				pack->SetActivePreset(key, value, false);
		}
		pack->SetEnabled(enable);
	}

	pack->UpdatePresetVisibility();
	pack->ValidatePresetSelections();
	s_graphic_packs.push_back(std::move(pack));
	return true;
}

bool GraphicPack2::ActivateGraphicPack(const std::shared_ptr<GraphicPack2>& graphic_pack)
{
	if (!graphic_pack->Activate())
		return false;
	s_active_graphic_packs.push_back(graphic_pack);
	g_debuggerDispatcher.NotifyGraphicPacksModified();
	return true;
}

bool GraphicPack2::DeactivateGraphicPack(const std::shared_ptr<GraphicPack2>& graphic_pack)
{
	if (!graphic_pack->IsActivated())
		return false;
	const std::string key = graphic_pack->GetNormalizedPathString();
	auto it = std::find_if(s_active_graphic_packs.begin(), s_active_graphic_packs.end(),
		[&key](const GraphicPackPtr& p) { return p->GetNormalizedPathString() == key; });
	if (it == s_active_graphic_packs.end())
		return false;
	graphic_pack->Deactivate();
	s_active_graphic_packs.erase(it);
	g_debuggerDispatcher.NotifyGraphicPacksModified();
	return true;
}

void GraphicPack2::ActivateForCurrentTitle()
{
	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	for (const auto& pack : GetGraphicPacks())
	{
		if (!pack->IsEnabled() || !pack->ContainsTitleId(titleId))
			continue;
		if (!ActivateGraphicPack(pack))
			continue;
		std::string line = fmt::format("Graphic pack activated: {}", pack->GetVirtualPath());
		if (!pack->GetPresets().empty())
		{
			std::string names;
			for (const auto& preset : pack->GetPresets())
			{
				if (!preset->active)
					continue;
				if (!names.empty())
					names += ',';
				names += preset->name;
			}
			line += fmt::format(" (presets: {})", names);
		}
		cemuLog_log(LogType::Force, line);
	}
	s_isReady = true;
}

void GraphicPack2::Reset()
{
	s_isReady = false;
	s_active_graphic_packs.clear();
}

void GraphicPack2::ClearGraphicPacks()
{
	// the active list only references entries of the full list
	s_active_graphic_packs.clear();
	s_graphic_packs.clear();
}

void GraphicPack2::WaitUntilReady()
{
	constexpr auto kPollInterval = std::chrono::milliseconds(5);
	for (;;)
	{
		if (s_isReady)
			return;
		std::this_thread::sleep_for(kPollInterval);
	}
}

// ---------------------------------------------------------------- reading rules.txt

GraphicPack2::GraphicPack2(fs::path rulesPath, IniParser& rules)
	: m_rulesPath(std::move(rulesPath))
{
	ReadDefinition(rules);
	ReadLoadTimeSections(rules);
	CheckPresets();
}

void GraphicPack2::ReadDefinition(IniParser& rules)
{
	const auto versionText = rules.FindOption("version");
	if (!versionText)
		throw std::exception();
	m_version = StringHelpers::ToInt(*versionText, -1);
	if (m_version < 0)
	{
		cemuLog_log(LogType::Force, "Graphic pack {}: bad version", _pathToUtf8(m_rulesPath));
		throw std::exception();
	}

	if (auto renderer = rules.FindOption("rendererFilter"))
	{
		if (boost::iequals(*renderer, "vulkan"))
			m_renderer_api = RendererAPI::Vulkan;
		else if (boost::iequals(*renderer, "opengl"))
			m_renderer_api = RendererAPI::OpenGL;
		else if (boost::iequals(*renderer, "metal"))
			m_renderer_api = RendererAPI::Metal;
		else
			cemuLog_log(LogType::Force, "Graphic pack: rendererFilter '{}' is not known", *renderer);
	}

	if (auto defaultText = rules.FindOption("default"))
	{
		m_default_enabled = IsTrueText(*defaultText);
		m_enabled = m_default_enabled;
	}

	if (auto optimizationText = rules.FindOption("colorbufferOptimizationAware"))
		m_allowRendertargetSizeOptimization = IsTrueText(*optimizationText);

	if (auto vendor = rules.FindOption("vendorFilter"))
	{
		static constexpr std::pair<std::string_view, GfxVendor> kVendors[] = {
			{"amd", GfxVendor::AMD},
			{"intel", GfxVendor::Intel},
			{"mesa", GfxVendor::Mesa},
			{"nvidia", GfxVendor::Nvidia},
			{"apple", GfxVendor::Apple},
		};
		auto match = std::find_if(std::begin(kVendors), std::end(kVendors), [&](const auto& v) { return boost::iequals(*vendor, v.first); });
		if (match != std::end(kVendors))
			m_gfx_vendor = match->second;
		else
			cemuLog_log(LogType::Force, "Graphic pack: vendorFilter '{}' is not known", *vendor);
	}

	const auto pathText = rules.FindOption("path");
	if (!pathText)
	{
		const auto nameText = rules.FindOption("name");
		cemuLog_log(LogType::Force, "Graphic pack '{}': [Definition] has no path option", nameText ? *nameText : "?");
		throw std::exception();
	}
	m_virtualPath = *pathText;

	if (auto nameText = rules.FindOption("name"))
		m_name = *nameText;

	if (auto descriptionText = rules.FindOption("description"))
	{
		m_description = *descriptionText;
		for (char& c : m_description)
		{
			if (c == '|')
				c = '\n';
		}
	}

	m_title_ids = ParseTitleIds(rules, "titleIds");
	if (m_title_ids.empty() && !m_universal)
		throw std::exception();

	if (auto priorityText = rules.FindOption("fsPriority"))
		m_fs_priority = std::stoi(std::string(*priorityText));
}

std::vector<uint64> GraphicPack2::ParseTitleIds(IniParser& rules, const char* option_name)
{
	std::vector<uint64> ids;
	const auto text = rules.FindOption(option_name);
	if (!text)
		return ids;
	if (*text == "*")
	{
		m_universal = true;
		return ids;
	}
	for (std::string_view part : TokenizeView(*text, ','))
	{
		try
		{
			ids.push_back(ConvertString<uint64>(part, 16));
		}
		catch (const std::invalid_argument&)
		{
		}
	}
	return ids;
}

// options of the current section whose name starts with '$', optionally typed as "$name:int"
std::unordered_map<std::string, GraphicPack2::PresetVar> GraphicPack2::ParsePresetVars(IniParser& rules) const
{
	std::unordered_map<std::string, PresetVar> result;
	ExpressionParser evaluator; // later variables may use earlier ones
	for (const auto& [key, valueText] : rules.GetAllOptions())
	{
		if (key.empty() || key.front() != '$')
			continue;
		std::string varName(key);
		VarType type = kDouble;
		const size_t colon = varName.find(':');
		if (colon != std::string::npos)
		{
			std::string typeName = varName.substr(colon + 1);
			varName.resize(colon);
			trim(varName);
			trim(typeName);
			if (typeName == "int")
				type = kInt;
		}
		const double value = evaluator.Evaluate(valueText);
		result.try_emplace(varName, type, value);
		evaluator.AddConstant(varName, value);
	}
	return result;
}

void GraphicPack2::ReadLoadTimeSections(IniParser& rules)
{
	while (rules.NextSection())
	{
		const std::string_view section = rules.GetCurrentSectionName();
		if (boost::iequals(section, "Default"))
		{
			m_preset_vars = ParsePresetVars(rules);
		}
		else if (boost::iequals(section, "Preset"))
		{
			const auto presetName = rules.FindOption("name");
			if (!presetName)
			{
				cemuLog_log(LogType::Force, "Graphic pack {}: preset at line {} has no name and is ignored", GetNormalizedPathString(), rules.GetCurrentSectionLineNumber());
				continue;
			}
			const auto category = rules.FindOption("category");
			const auto condition = rules.FindOption("condition");
			const auto defaultText = rules.FindOption("default");
			try
			{
				auto vars = ParsePresetVars(rules);
				PresetPtr preset;
				if (!category)
					preset = std::make_shared<Preset>(*presetName, std::move(vars));
				else if (!condition)
					preset = std::make_shared<Preset>(*category, *presetName, std::move(vars));
				else
					preset = std::make_shared<Preset>(*category, *presetName, *condition, std::move(vars));
				if (defaultText)
					preset->is_default = StringHelpers::ToInt(*defaultText) != 0;
				m_presets.push_back(std::move(preset));
			}
			catch (const std::exception& ex)
			{
				cemuLog_log(LogType::Force, "Graphic pack {}: preset \"{}\" ignored: {}", GetNormalizedPathString(), *presetName, ex.what());
			}
		}
		else if (boost::iequals(section, "RAM"))
		{
			ReadRamSection(rules);
		}
		else if (boost::iequals(section, "Permissions"))
		{
			ReadPermissionSection(rules);
		}
	}
}

void GraphicPack2::ReadRamSection(IniParser& rules)
{
	for (uint32 index = 0; index < 32; index++)
	{
		const std::string option = fmt::format("mapping{}", index);
		const auto text = rules.FindOption(option);
		if (!text)
			continue;
		if (m_version <= 5)
		{
			cemuLog_log(LogType::Force, "Graphic pack {}: [RAM] needs version 6 or newer", GetNormalizedPathString());
			throw std::exception();
		}
		StringTokenParser tokens(*text);
		uint32 first = 0, last = 0;
		const bool wellFormed = tokens.parseU32(first) && tokens.matchWordI("-") && tokens.parseU32(last) && tokens.isEndOfString();
		if (!wellFormed)
		{
			cemuLog_log(LogType::Force, "Graphic pack {}: {} is not of the form <start> - <end>", GetNormalizedPathString(), option);
			throw std::exception();
		}
		if (last <= first)
		{
			cemuLog_log(LogType::Force, "Graphic pack {}: {} ends at 0x{:08x}, not after its start 0x{:08x}", GetNormalizedPathString(), option, last, first);
			throw std::exception();
		}
		if (((first | last) & 0xFFF) != 0)
		{
			cemuLog_log(LogType::Force, "Graphic pack {}: {} must use 4KB aligned addresses", GetNormalizedPathString(), option);
			throw std::exception();
		}
		m_ramMappings.emplace_back(first, last);
	}
}

void GraphicPack2::ReadPermissionSection(IniParser& rules)
{
	static constexpr std::pair<std::string_view, CosCapabilityGroup> kGroups[] = {
		{"BSP", CosCapabilityGroup::BSP},
		{"DK", CosCapabilityGroup::DK},
		{"USB", CosCapabilityGroup::USB},
		{"UHS", CosCapabilityGroup::UHS},
		{"FS", CosCapabilityGroup::FS},
		{"MCP", CosCapabilityGroup::MCP},
		{"NIM", CosCapabilityGroup::NIM},
		{"ACT", CosCapabilityGroup::ACT},
		{"FPD", CosCapabilityGroup::FPD},
		{"BOSS", CosCapabilityGroup::BOSS},
		{"ACP", CosCapabilityGroup::ACP},
		{"PDM", CosCapabilityGroup::PDM},
		{"AC", CosCapabilityGroup::AC},
		{"NDM", CosCapabilityGroup::NDM},
		{"NSEC", CosCapabilityGroup::NSEC},
	};
	for (const auto& [groupName, group] : kGroups)
	{
		const auto text = rules.FindOption(groupName);
		if (!text)
			continue;
		cemuLog_log(LogType::Force, "Graphic pack {}: permission mask {} for {}", GetNormalizedPathString(), *text, groupName);
		m_permissions.emplace_back(group, ConvertString<uint64>(*text, 16));
	}
}

// checks the presets read from rules.txt and picks the initial selection of every category
void GraphicPack2::CheckPresets()
{
	// presets grouped by category, in file order
	std::unordered_map<std::string, std::vector<PresetPtr>> byCategory;
	for (const auto& preset : m_presets)
		byCategory[preset->category].push_back(preset);

	if (m_version >= 5)
	{
		// every preset variable must exist in [Default], whose type it takes
		bool undefinedFound = false;
		for (const auto& preset : m_presets)
		{
			for (auto& [varName, var] : preset->variables)
			{
				auto def = m_preset_vars.find(varName);
				if (def != m_preset_vars.end())
				{
					var.first = def->second.first;
					continue;
				}
				if (!undefinedFound)
					cemuLog_log(LogType::Force, "Graphic pack {}: preset variables missing from [Default]:", GetNormalizedPathString());
				undefinedFound = true;
				cemuLog_log(LogType::Force, "  preset {}: {}", preset->name, varName);
			}
		}
		if (undefinedFound)
			throw std::exception();

		// the preset marked default (or else the first one) starts out selected
		for (auto& [category, presets] : byCategory)
		{
			auto pick = std::find_if(presets.begin(), presets.end(), [](const PresetPtr& p) { return p->is_default; });
			(pick != presets.end() ? *pick : presets.front())->active = true;
		}
		return;
	}

	// before version 5 all presets of a category must define the same variables
	auto sameNames = [](const Preset& a, const Preset& b) {
		if (a.variables.size() != b.variables.size())
			return false;
		for (const auto& entry : a.variables)
		{
			if (!b.variables.contains(entry.first))
				return false;
		}
		return true;
	};
	for (auto& [category, presets] : byCategory)
	{
		bool defaultSeen = false;
		// the old format never treats the last preset of a category as default
		for (size_t i = 1; i < presets.size(); i++)
		{
			const PresetPtr& prev = presets[i - 1];
			if (!sameNames(*prev, *presets[i]))
			{
				cemuLog_log(LogType::Force, "Graphic pack {}: presets do not define the same variables", GetNormalizedPathString());
				throw std::exception();
			}
			if (prev->is_default)
			{
				if (defaultSeen)
					cemuLog_log(LogType::Force, "Graphic pack {}: category \"{}\" has several default presets", GetNormalizedPathString(), prev->name);
				prev->active = true;
				defaultSeen = true;
			}
		}
		if (!defaultSeen)
			presets.front()->active = true;
	}
}

// ---------------------------------------------------------------- presets

template<typename TType>
void GraphicPack2::FillPresetConstants(TExpressionParser<TType>& parser) const
{
	const auto active = GetActivePresets();
	// visible selections overwrite each other in order
	for (const auto& preset : active)
	{
		if (!preset->visible)
			continue;
		for (const auto& [name, var] : preset->variables)
			parser.AddConstant(name, (TType)var.second);
	}
	// hidden selections and then [Default] only fill names still missing
	for (const auto& preset : active)
	{
		if (preset->visible)
			continue;
		for (const auto& [name, var] : preset->variables)
			parser.TryAddConstant(name, (TType)var.second);
	}
	for (const auto& [name, var] : m_preset_vars)
		parser.TryAddConstant(name, (TType)var.second);
}

bool GraphicPack2::HasActivePreset() const
{
	for (const auto& preset : m_presets)
	{
		if (preset->active)
			return true;
	}
	return false;
}

std::string GraphicPack2::GetActivePreset(std::string_view category) const
{
	for (const auto& preset : m_presets)
	{
		if (preset->active && preset->category == category)
			return preset->name;
	}
	return {};
}

std::vector<GraphicPack2::PresetPtr> GraphicPack2::GetActivePresets() const
{
	std::vector<PresetPtr> active;
	for (const auto& preset : m_presets)
	{
		if (preset->active)
			active.push_back(preset);
	}
	return active;
}

void GraphicPack2::UpdatePresetVisibility()
{
	// presets are updated one after the other; a condition sees the flags already updated
	for (auto& preset : m_presets)
		preset->visible = m_version < 5 || IsPresetVisible(preset);
}

void GraphicPack2::ValidatePresetSelections()
{
	if (m_version < 5)
		return;
	// Every category with at least one visible preset needs exactly one selected visible preset.
	// Categories with nothing visible keep their selection.
	std::vector<std::string> order;
	auto categories = GetCategorizedPresets(order);
	bool changed = false;
	for (auto& [category, presets] : categories)
	{
		PresetPtr selected;
		PresetPtr fallback; // the last visible default, or else the first visible preset
		bool anyVisible = false;
		for (const auto& preset : presets)
		{
			if (preset->visible)
			{
				anyVisible = true;
				if (!fallback || preset->is_default)
					fallback = preset;
			}
			if (!preset->active)
				continue;
			if (selected)
			{
				preset->active = false;
				changed = true;
			}
			else
				selected = preset;
		}
		if (!anyVisible)
			continue;
		if (!selected)
		{
			fallback->active = true;
			continue;
		}
		if (!selected->visible)
		{
			selected->active = false;
			fallback->active = true;
			changed = true;
		}
	}
	if (changed)
		UpdatePresetVisibility();
}

bool GraphicPack2::SetActivePreset(std::string_view category, std::string_view name, bool update_visibility)
{
	for (auto& preset : m_presets)
	{
		if (preset->category == category)
			preset->active = false;
	}
	if (name.empty())
		return true;

	bool found = false;
	for (auto& preset : m_presets)
	{
		if (preset->category == category && preset->name == name)
		{
			preset->active = true;
			found = true;
			break;
		}
	}
	if (update_visibility)
	{
		UpdatePresetVisibility();
		ValidatePresetSelections();
	}
	return found;
}

bool GraphicPack2::SetActivePreset(std::string_view name)
{
	return SetActivePreset("", name);
}

bool GraphicPack2::IsPresetVisible(const PresetPtr& preset) const
{
	if (preset->condition.empty())
		return true;
	try
	{
		TExpressionParser<int> evaluator;
		FillPresetConstants(evaluator);
		return evaluator.Evaluate(preset->condition) != 0;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Graphic pack: preset condition failed: {}", ex.what());
		return false;
	}
}

std::optional<GraphicPack2::PresetVar> GraphicPack2::GetPresetVariable(const std::vector<PresetPtr>& presets, std::string_view var_name) const
{
	if (m_version < 5)
	{
		for (const auto& preset : presets)
		{
			if (auto v = FindVariable(*preset, var_name))
				return v;
		}
		return std::nullopt;
	}
	// visible presets first, then hidden ones, then [Default]
	for (bool wantVisible : {true, false})
	{
		for (const auto& preset : presets)
		{
			if (preset->visible != wantVisible)
				continue;
			if (auto v = FindVariable(*preset, var_name))
				return v;
		}
	}
	auto def = m_preset_vars.find(std::string(var_name));
	if (def != m_preset_vars.end())
		return def->second;
	return std::nullopt;
}

void GraphicPack2::AddConstantsForCurrentPreset(ExpressionParser& ep)
{
	if (m_version >= 5)
	{
		FillPresetConstants(ep);
		return;
	}
	for (const auto& preset : GetActivePresets())
	{
		for (const auto& [name, var] : preset->variables)
			ep.AddConstant(name, var.second);
	}
}

bool GraphicPack2::ResolvePresetConstant(const std::string& varname, double& value) const
{
	auto var = GetPresetVariable(GetActivePresets(), varname);
	if (!var)
		return false;
	value = var->second;
	return true;
}

std::unordered_map<std::string, std::vector<GraphicPack2::PresetPtr>> GraphicPack2::GetCategorizedPresets(std::vector<std::string>& order) const
{
	order.clear();
	std::unordered_map<std::string, std::vector<PresetPtr>> result;
	for (const auto& preset : m_presets)
	{
		auto [it, isNew] = result.try_emplace(preset->category);
		it->second.push_back(preset);
		if (isNew)
			order.push_back(preset->category);
	}
	return result;
}

// ---------------------------------------------------------------- misc queries

bool GraphicPack2::RequiresRestart(bool changeEnableState, bool changePreset)
{
	// texture rules are only picked up when textures are created
	return !m_texture_rules.empty();
}

bool GraphicPack2::Reload()
{
	Deactivate();
	return Activate();
}

std::string GraphicPack2::GetNormalizedPathString() const
{
	return _pathToUtf8(MakeRelativePath(ActiveSettings::GetUserDataPath(), GetRulesPath()).lexically_normal());
}

bool GraphicPack2::ContainsTitleId(uint64_t title_id) const
{
	return m_universal || std::find(m_title_ids.begin(), m_title_ids.end(), title_id) != m_title_ids.end();
}

std::vector<std::pair<MPTR, MPTR>> GraphicPack2::GetActiveRAMMappings()
{
	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	std::vector<std::pair<MPTR, MPTR>> ranges;
	for (const auto& pack : GetGraphicPacks())
	{
		if (pack->IsEnabled() && pack->ContainsTitleId(titleId))
			ranges.insert(ranges.end(), pack->m_ramMappings.begin(), pack->m_ramMappings.end());
	}
	std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	return ranges;
}

// ---------------------------------------------------------------- shaders

void GraphicPack2::ApplyShaderPresets(std::string& shader_source) const
{
	const auto active = GetActivePresets();
	size_t pos = 0;
	while ((pos = shader_source.find('$', pos)) != std::string::npos)
	{
		size_t end = pos + 1;
		while (end < shader_source.size() && IsVariableChar(shader_source[end]))
			end++;
		if (end == pos + 1)
		{
			pos++; // a lone '$'
			continue;
		}
		if (active.empty())
			throw std::runtime_error("shader uses a preset variable but no preset is selected");
		auto var = GetPresetVariable(active, std::string_view(shader_source).substr(pos, end - pos));
		if (!var)
			throw std::runtime_error("shader uses an unknown preset variable");
		const std::string text = var->first == kInt ? fmt::format("{}", (int)var->second) : fmt::format("{:f}", var->second);
		shader_source.replace(pos, end - pos, text);
		pos += text.size();
	}
}

GraphicPack2::CustomShader GraphicPack2::LoadShader(const fs::path& path, uint64 shader_base_hash, uint64 shader_aux_hash, GP_SHADER_TYPE shader_type, bool isMetalShader) const
{
	CustomShader shader;
	shader.source = ReadShaderText(path, "shader file cannot be opened");
	ApplyShaderPresets(shader.source);
	shader.shader_base_hash = shader_base_hash;
	shader.shader_aux_hash = shader_aux_hash;
	shader.type = shader_type;
	shader.isPreVulkanShader = m_version <= 3;
	shader.isMetalShader = isMetalShader;
	return shader;
}

void GraphicPack2::LoadShaders()
{
	const fs::path folder = m_rulesPath.parent_path();
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(folder, ec))
	{
		if (!entry.is_regular_file(ec))
			continue;
		const fs::path& file = entry.path();
		try
		{
			// replacement shaders are named <base hash>_<aux hash>_<ps|vs|gs>[_msl]...
			const std::wstring name = file.filename().wstring();
			uint64 baseHash = 0, auxHash = 0;
			wchar_t kind[256]{};
			if (name.size() < 256 && swscanf(name.c_str(), L"%" SCNx64 "_%" SCNx64 "_%ls", &baseHash, &auxHash, kind) == 3)
			{
				const bool metal = kind[2] == '_' && kind[3] == 'm' && kind[4] == 's' && kind[5] == 'l';
				std::optional<GP_SHADER_TYPE> type;
				if (kind[0] == 'p' && kind[1] == 's')
					type = GP_SHADER_TYPE::PIXEL;
				else if (kind[0] == 'v' && kind[1] == 's')
					type = GP_SHADER_TYPE::VERTEX;
				else if (kind[0] == 'g' && kind[1] == 's')
					type = GP_SHADER_TYPE::GEOMETRY;
				if (type)
					m_custom_shaders.push_back(LoadShader(file, baseHash, auxHash, *type, metal));
				continue;
			}
			std::string* target = nullptr;
			if (name == L"output.glsl")
				target = &m_output_shader_source;
			else if (name == L"upscaling.glsl")
				target = &m_upscaling_shader_source;
			else if (name == L"downscaling.glsl")
				target = &m_downscaling_shader_source;
			if (!target)
				continue;
			*target = ReadShaderText(file, "graphic pack shader cannot be opened");
			ApplyShaderPresets(*target); // on failure the text stays as read
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "Graphic pack {}: shader {} not loaded: {}", GetNormalizedPathString(), _pathToUtf8(file.filename()), ex.what());
		}
	}
}

bool GraphicPack2::HasShaders() const
{
	return !m_custom_shaders.empty() || !m_output_shader_source.empty() || !m_upscaling_shader_source.empty() || !m_downscaling_shader_source.empty();
}

const std::string* GraphicPack2::FindCustomShaderSource(uint64 shaderBaseHash, uint64 shaderAuxHash, GP_SHADER_TYPE type, bool isVulkanRenderer, bool isMetalRenderer)
{
	for (const auto& pack : GetActiveGraphicPacks())
	{
		// only the first shader of a pack with these hashes counts
		auto it = std::find_if(pack->m_custom_shaders.begin(), pack->m_custom_shaders.end(), [&](const CustomShader& s) {
			return s.shader_base_hash == shaderBaseHash && s.shader_aux_hash == shaderAuxHash && s.type == type;
		});
		if (it == pack->m_custom_shaders.end())
			continue;
		if (isVulkanRenderer && it->isPreVulkanShader)
			continue;
		if (it->isMetalShader != isMetalRenderer)
			continue;
		return &it->source;
	}
	return nullptr;
}

RendererOutputShader* GraphicPack2::GetOuputShader(bool render_upside_down)
{
	return CachedOutputShader(render_upside_down ? m_output_shader_ud : m_output_shader, m_output_shader_source, render_upside_down);
}

RendererOutputShader* GraphicPack2::GetUpscalingShader(bool render_upside_down)
{
	return CachedOutputShader(render_upside_down ? m_upscaling_shader_ud : m_upscaling_shader, m_upscaling_shader_source, render_upside_down);
}

RendererOutputShader* GraphicPack2::GetDownscalingShader(bool render_upside_down)
{
	return CachedOutputShader(render_upside_down ? m_downscaling_shader_ud : m_downscaling_shader, m_downscaling_shader_source, render_upside_down);
}

// ---------------------------------------------------------------- activation

void GraphicPack2::RedirectFolder(const fs::path& hostFolder, const std::string& virtualBase)
{
	std::error_code ec;
	fs::recursive_directory_iterator walker(hostFolder, ec);
	for (; !ec && walker != fs::recursive_directory_iterator(); walker.increment(ec))
	{
		if (!walker->is_regular_file(ec))
			continue;
		const fs::path relative = fs::relative(walker->path(), hostFolder, ec);
		if (ec)
		{
			ec.clear();
			continue;
		}
		const uint64 size = walker->file_size(ec);
		if (ec)
		{
			ec.clear();
			continue;
		}
		const fs::path virtualPath = fs::path(virtualBase) / relative;
		fscDeviceRedirect_add(virtualPath.generic_string(), size, walker->path().generic_string(), m_fs_priority);
	}
}

void GraphicPack2::LoadReplacedFiles()
{
	if (m_patchedFilesLoaded)
		return;
	m_patchedFilesLoaded = true;

	const fs::path folder = m_rulesPath.parent_path();
	const uint64 aocTitleId = (CafeSystem::GetForegroundTitleId() & 0xFFFFFFFFull) | 0x0005000c00000000ull;
	// <pack>/content, <pack>/aoc and <pack>/code replace files of the title
	const std::pair<const char*, std::string> sources[] = {
		{"content", "vol/content/"},
		{"aoc", fmt::format("/vol/aoc{:016x}/", aocTitleId)},
		{"code", CafeSystem::GetInternalVirtualCodeFolder()},
	};
	for (const auto& [subFolder, virtualBase] : sources)
	{
		std::error_code ec;
		const fs::path hostFolder = folder / subFolder;
		if (!fs::exists(hostFolder, ec))
			continue;
		fscDeviceRedirect_map();
		RedirectFolder(hostFolder, virtualBase);
	}
}

bool GraphicPack2::ReadActivationSections()
{
	auto rulesData = ReadWholeFile(m_rulesPath);
	if (!rulesData)
		return false;
	IniParser rules(*rulesData, GetNormalizedPathString());
	try
	{
		ExpressionParser parser;
		AddConstantsForCurrentPreset(parser);
		while (rules.NextSection())
		{
			const std::string_view section = rules.GetCurrentSectionName();
			if (boost::iequals(section, "TextureRedefine"))
			{
				TextureRule rule{};
				auto& filter = rule.filter_settings;
				auto& overwrite = rule.overwrite_settings;
				EvalOption(parser, rules, "width", filter.width);
				EvalOption(parser, rules, "height", filter.height);
				EvalOption(parser, rules, "depth", filter.depth);
				bool inMem1 = false;
				if (EvalOption(parser, rules, "inMEM1", inMem1))
					filter.inMEM1 = inMem1 ? TextureRule::FILTER_SETTINGS::MEM1_FILTER::INSIDE : TextureRule::FILTER_SETTINGS::MEM1_FILTER::OUTSIDE;
				filter.format_whitelist = EvalList(parser, rules, "formats");
				filter.format_blacklist = EvalList(parser, rules, "formatsExcluded");
				filter.tilemode_whitelist = EvalList(parser, rules, "tilemodes");
				filter.tilemode_blacklist = EvalList(parser, rules, "tilemodesExcluded");

				EvalOption(parser, rules, "overwriteWidth", overwrite.width);
				EvalOption(parser, rules, "overwriteHeight", overwrite.height);
				EvalOption(parser, rules, "overwriteDepth", overwrite.depth);
				EvalOption(parser, rules, "overwriteFormat", overwrite.format);
				float bias;
				if (EvalOption(parser, rules, "overwriteLodBias", bias))
					overwrite.lod_bias = (sint32)(bias * 64.0f);
				if (EvalOption(parser, rules, "overwriteRelativeLodBias", bias))
					overwrite.relative_lod_bias = (sint32)(bias * 64.0f);
				sint32 anisotropy;
				if (EvalOption(parser, rules, "overwriteAnisotropy", anisotropy))
				{
					// stored as log2 of the sample count
					sint32 log2 = -1;
					for (sint32 i = 0; i <= 4; i++)
					{
						if (anisotropy == (1 << i))
							log2 = i;
					}
					if (log2 >= 0)
						overwrite.anistropic_value = log2;
					else
						cemuLog_log(LogType::Force, "Graphic pack {}: overwriteAnisotropy {} is not one of 1, 2, 4, 8, 16", GetNormalizedPathString(), anisotropy);
				}
				m_texture_rules.push_back(std::move(rule));
			}
			else if (boost::iequals(section, "Control"))
			{
				EvalOption(parser, rules, "vsyncFrequency", m_vsync_frequency);
			}
			else if (boost::iequals(section, "OutputShader"))
			{
				const auto up = rules.FindOption("upscaleMagFilter");
				if (up && boost::iequals(*up, "NearestNeighbor"))
					m_output_settings.upscale_filter = LatteTextureView::MagFilter::kNearestNeighbor;
				const auto down = rules.FindOption("downscaleMinFilter");
				if (down && boost::iequals(*down, "NearestNeighbor"))
					m_output_settings.downscale_filter = LatteTextureView::MagFilter::kNearestNeighbor;
			}
		}
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Graphic pack {}: {}", GetNormalizedPathString(), ex.what());
		return false;
	}
	return true;
}

bool GraphicPack2::Activate()
{
	if (m_activated)
		return true;
	if (m_renderer_api && *m_renderer_api != g_renderer->GetType())
		return false;
	if (m_gfx_vendor && *m_gfx_vendor != g_renderer->GetVendor())
		return false;
	if (!ReadActivationSections())
		return false;

	LoadShaders();
	LoadPatchFiles();
	EnablePatches();
	LoadReplacedFiles();

	if (HasCustomVSyncFrequency())
	{
		const sint32 wanted = GetCustomVSyncFrequency();
		sint32 current = 0;
		if (!LatteTiming_getCustomVsyncFrequency(current))
		{
			cemuLog_log(LogType::Force, "Graphic pack {}: vsync frequency set to {}", GetVirtualPath(), wanted);
			LatteTiming_setCustomVsyncFrequency(wanted);
		}
		else if (current != wanted)
			cemuLog_log(LogType::Force, "Graphic pack {}: vsync frequency {} conflicts with another pack", GetVirtualPath(), wanted);
	}
	m_activated = true;
	return true;
}

bool GraphicPack2::Deactivate()
{
	if (!m_activated)
		return false;
	UnloadPatches();
	m_activated = false;

	m_custom_shaders.clear();
	m_texture_rules.clear();
	for (auto* shader : {&m_output_shader, &m_upscaling_shader, &m_downscaling_shader, &m_output_shader_ud, &m_upscaling_shader_ud, &m_downscaling_shader_ud})
		shader->reset();
	m_output_shader_source.clear();
	m_upscaling_shader_source.clear();
	m_downscaling_shader_source.clear();

	if (HasCustomVSyncFrequency())
	{
		m_vsync_frequency = -1;
		LatteTiming_disableCustomVsyncFrequency();
	}
	return true;
}
