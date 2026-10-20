// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "util/tinyxml2/tinyxml2.h"
#include "Common/FileStream.h"
#include "config/ConfigValue.h"

#include <array>
#include <cassert>
#include <atomic>
#include <concepts>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Reading and writing of the XML settings files (settings.xml,
// network_services.xml, the debugger files).
//
// XMLConfigParser is a cursor on one element of a tinyxml2 document; the root
// cursor stands for the document itself. Values are stored as element text or
// as attributes. The C++ type of the default value picks the conversion:
//   bool, float, sint32, uint32, sint64   tinyxml2's text/attribute parsers
//   uint64                                stored and parsed as sint64
//   enums                                 their underlying integer type
//   const char*, std::string              the text as is (element text also
//                                         fills any type built from a const char*)
//   std::array<T, N>                      lowercase hex, two digits per entry
// A value of any other type cannot be read: the default comes back.
// These rules are part of the file format, existing files depend on them.
class XMLConfigParser
{
public:
	// cursor on the document itself
	XMLConfigParser(tinyxml2::XMLDocument* document)
		: m_document(document), m_current_element(nullptr), m_is_root(true) {}

private:
	XMLConfigParser(tinyxml2::XMLDocument* document, tinyxml2::XMLElement* element)
		: m_document(document), m_current_element(element), m_is_root(false) {}

	template<typename T>
	static constexpr bool kTextLike = std::is_same_v<T, const char*> || std::is_same_v<T, std::string>;

	// note: a cursor without an element (root, or a child that does not exist)
	// looks among the top level elements of the document
	tinyxml2::XMLElement* Child(const char* name) const
	{
		return m_current_element ? m_current_element->FirstChildElement(name) : m_document->FirstChildElement(name);
	}

	void Append(tinyxml2::XMLElement* element) const
	{
		if (m_current_element)
			m_current_element->InsertEndChild(element);
		else
			m_document->InsertEndChild(element);
	}

	static int HexDigit(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		throw std::invalid_argument("Invalid input string");
	}

public:
	// ---- element values ----

	template<typename T>
	T get(const char* name, T default_value = {})
	{
		if constexpr (std::is_enum_v<T>)
		{
			using Raw = std::underlying_type_t<T>;
			return static_cast<T>(get(name, static_cast<Raw>(default_value)));
		}
		else
		{
			const tinyxml2::XMLElement* element = Child(name);
			if (!element)
				return default_value;
			if constexpr (std::is_same_v<T, bool>)
				return element->BoolText(default_value);
			else if constexpr (std::is_same_v<T, float>)
				return element->FloatText(default_value);
			else if constexpr (std::is_same_v<T, sint32>)
				return element->IntText(default_value);
			else if constexpr (std::is_same_v<T, uint32>)
				return element->UnsignedText(default_value);
			else if constexpr (std::is_same_v<T, sint64>)
				return element->Int64Text(default_value);
			else if constexpr (std::is_same_v<T, uint64>)
				return (uint64)element->Int64Text((sint64)default_value);
			else if constexpr (kTextLike<T> || std::constructible_from<T, const char*>)
			{
				if (const char* text = element->GetText())
					return T(text);
				return default_value;
			}
			else
				return default_value;
		}
	}

	// a setting's declared value is the default, not its current one
	template<typename T>
	T get(const char* name, const ConfigValue<T>& default_value)
	{
		return get(name, default_value.GetInitValue());
	}

	template<typename T>
	T get(const char* name, const ConfigValueBounds<T>& default_value)
	{
		return get(name, default_value.GetInitValue());
	}

	// hex text into a byte array; the array is zeroed first and stays zero when
	// the element is missing. Text that is too short or not hex throws
	// std::invalid_argument, the entries before the bad digit are filled.
	template<typename TType, size_t TSize>
	std::array<TType, TSize>& get(const char* name, std::array<TType, TSize>& arr)
	{
		arr = {};
		const tinyxml2::XMLElement* element = Child(name);
		if (!element)
			return arr;
		const char* text = element->GetText();
		if (!text)
			return arr;
		assert(strlen(text) == TSize * 2);
		size_t pos = 0;
		for (size_t i = 0; i < TSize; i++)
		{
			// stop at the terminator: it is not a hex digit
			const int high = HexDigit(text[pos]);
			pos++;
			const int low = HexDigit(text[pos]);
			pos++;
			arr[i] = (TType)((high << 4) + low);
		}
		return arr;
	}

	// ---- attributes of this element ----

	template<typename T>
	T get_attribute(const char* name, T default_value = {})
	{
		if constexpr (std::is_enum_v<T>)
		{
			using Raw = std::underlying_type_t<T>;
			return static_cast<T>(get_attribute(name, static_cast<Raw>(default_value)));
		}
		else
		{
			const tinyxml2::XMLElement* element = m_current_element;
			if (!element)
				return default_value;
			if constexpr (std::is_same_v<T, bool>)
				return element->BoolAttribute(name, default_value);
			else if constexpr (std::is_same_v<T, float>)
				return element->FloatAttribute(name, default_value);
			else if constexpr (std::is_same_v<T, sint32>)
				return element->IntAttribute(name, default_value);
			else if constexpr (std::is_same_v<T, uint32>)
				return element->UnsignedAttribute(name, default_value);
			else if constexpr (std::is_same_v<T, sint64>)
				return element->Int64Attribute(name, default_value);
			else if constexpr (std::is_same_v<T, uint64>)
				return (uint64)element->Int64Attribute(name, (sint64)default_value);
			else if constexpr (kTextLike<T>)
			{
				if (const char* text = element->Attribute(name))
					return T(text);
				return default_value;
			}
			else
				return default_value;
		}
	}

	template<typename T>
	T get_attribute(const char* name, const ConfigValue<T>& default_value)
	{
		return get_attribute(name, default_value.GetInitValue());
	}

	template<typename T>
	T get_attribute(const char* name, const ConfigValueBounds<T>& default_value)
	{
		return get_attribute(name, default_value.GetInitValue());
	}

	// ---- the text of this element itself ----

	// unsupported types: the default
	template<typename T>
	T value(T default_value = T())
	{
		return default_value;
	}

	bool value(bool default_value) { return m_current_element ? m_current_element->BoolText(default_value) : default_value; }
	float value(float default_value) { return m_current_element ? m_current_element->FloatText(default_value) : default_value; }
	double value(double default_value) { return m_current_element ? m_current_element->DoubleText(default_value) : default_value; }
	uint32 value(uint32 default_value) { return m_current_element ? m_current_element->UnsignedText(default_value) : default_value; }
	sint32 value(sint32 default_value) { return m_current_element ? m_current_element->IntText(default_value) : default_value; }
	uint64 value(uint64 default_value) { return m_current_element ? (uint64)m_current_element->Int64Text(default_value) : default_value; }
	sint64 value(sint64 default_value) { return m_current_element ? m_current_element->Int64Text(default_value) : default_value; }

	const char* value(const char* default_value)
	{
		const char* text = m_current_element ? m_current_element->GetText() : nullptr;
		return text ? text : default_value;
	}

	// ---- writing: each call appends a new child element ----

	template<typename TType, size_t TSize>
	void set(const char* name, const std::array<TType, TSize>& value)
	{
		std::string hex;
		hex.reserve(TSize * 2);
		for (const TType& v : value)
			hex += fmt::format("{:02x}", v);
		tinyxml2::XMLElement* element = m_document->NewElement(name);
		element->SetText(hex.c_str());
		Append(element);
	}

	template<typename T>
	void set(const char* name, T value)
	{
		tinyxml2::XMLElement* element = m_document->NewElement(name);
		if constexpr (std::is_enum_v<T>)
			element->SetText(fmt::format("{}", static_cast<std::underlying_type_t<T>>(value)).c_str());
		else
			element->SetText(fmt::format("{}", value).c_str());
		Append(element);
	}

	template<typename T>
	void set(const char* name, const std::atomic<T>& value)
	{
		set(name, value.load());
	}

	template<typename T>
	void set(const char* name, const ConfigValue<T>& value)
	{
		set(name, value.GetValue());
	}

	// uint64 is written signed, matching how it is read back
	void set(const char* name, uint64 value)
	{
		set(name, (sint64)value);
	}

	// ---- navigation ----

	tinyxml2::XMLElement* GetCurrentElement() const { return m_current_element; }

	// first child with this name (the cursor is invalid when there is none)
	XMLConfigParser get(const char* name) const
	{
		return {m_document, Child(name)};
	}

	// next sibling of previous with this name
	XMLConfigParser get(const char* name, const XMLConfigParser& previous)
	{
		tinyxml2::XMLElement* next = previous.m_current_element ? previous.m_current_element->NextSiblingElement(name)
															   : previous.m_document->NextSiblingElement(name);
		return {m_document, next};
	}

	// new child element
	XMLConfigParser set(const char* name) const
	{
		tinyxml2::XMLElement* element = m_document->NewElement(name);
		Append(element);
		return {m_document, element};
	}

	template<typename T>
	XMLConfigParser& set_attribute(const char* name, const T& value)
	{
		cemu_assert_debug(m_current_element != nullptr);
		if (m_current_element)
			m_current_element->SetAttribute(name, value);
		return *this;
	}

	template<typename T>
	XMLConfigParser& set_attribute(const char* name, const ConfigValue<T>& value)
	{
		return set_attribute(name, value.GetValue());
	}

	template<typename T>
	XMLConfigParser& set_attribute(const char* name, const ConfigValueBounds<T>& value)
	{
		return set_attribute(name, value.GetValue());
	}

	XMLConfigParser& set_attribute(const char* name, const std::string& value)
	{
		return set_attribute(name, value.c_str());
	}

	bool valid() const
	{
		if (!m_document)
			return false;
		return m_is_root || m_current_element != nullptr;
	}

private:
	tinyxml2::XMLDocument* m_document;
	tinyxml2::XMLElement* m_current_element;
	bool m_is_root;
};

// save and load callbacks of a section that lives inside another file's root element
using ChildXMLConfigParser = std::pair<std::function<void(XMLConfigParser&)>, std::function<void(XMLConfigParser&)>>;

// T::Load/Save receive the document cursor and return the cursor child sections are attached to
template<typename T>
concept XMLConfigurable = requires(T t, XMLConfigParser& configParser) {
	{ t.Save(configParser) } -> std::same_as<XMLConfigParser>;
	{ t.Load(configParser) } -> std::same_as<XMLConfigParser>;
};

// An XML file bound to an object
template<XMLConfigurable T>
class XMLConfig
{
public:
	XMLConfig() = delete;
	XMLConfig(T& instance) : m_instance(instance) {}
	XMLConfig(T& instance, std::wstring_view filename) : m_instance(instance), m_filename(filename) {}
	XMLConfig(const XMLConfig&) = delete;
	virtual ~XMLConfig() = default;

	bool Load()
	{
		return !m_filename.empty() && Load(m_filename);
	}

	bool Load(const std::wstring& filename)
	{
		std::vector<uint8> content;
		{
			std::unique_ptr<FileStream> file(FileStream::openFile(filename.c_str()));
			if (!file)
			{
				cemuLog_logDebug(LogType::Force, "XMLConfig: cannot open \"{}\" (errno {})", boost::nowide::narrow(filename), errno);
				return false;
			}
			content.resize(file->GetSize());
			file->readData(content.data(), content.size());
		}

		tinyxml2::XMLDocument doc;
		const tinyxml2::XMLError result = doc.Parse((const char*)content.data(), content.size());
		if (result != tinyxml2::XML_SUCCESS)
		{
			cemuLog_logDebug(LogType::Force, "XMLConfig: parse error {} in \"{}\"", (int)result, boost::nowide::narrow(filename));
			return false;
		}

		XMLConfigParser root(&doc);
		XMLConfigParser sectionParent = m_instance.Load(root);
		for (const ChildXMLConfigParser& child : m_childConfigParsers)
			child.second(sectionParent);
		return true;
	}

	bool Save()
	{
		return !m_filename.empty() && Save(m_filename);
	}

	// writes to a temporary file next to the target, then renames it over the target
	bool Save(const std::wstring& filename)
	{
		const std::wstring tempName = fmt::format(L"{}_{}.tmp", filename, rand() % 1000);
		std::error_code ec;
		fs::create_directories(fs::path(filename).parent_path(), ec);
		if (ec)
		{
			cemuLog_log(LogType::Force, "XMLConfig: cannot create the folder for the settings file: {}", ec.message());
			return false;
		}

#if BOOST_OS_WINDOWS
		FILE* out = _wfopen(tempName.c_str(), L"wb");
#else
		FILE* out = fopen(boost::nowide::narrow(tempName).c_str(), "wb");
#endif
		if (!out)
		{
			cemuLog_logDebug(LogType::Force, "XMLConfig: cannot write \"{}\" (errno {})", boost::nowide::narrow(filename), errno);
			return false;
		}

		tinyxml2::XMLDocument doc;
		doc.InsertFirstChild(doc.NewDeclaration());
		XMLConfigParser root(&doc);
		XMLConfigParser sectionParent = m_instance.Save(root);
		for (const ChildXMLConfigParser& child : m_childConfigParsers)
			child.first(sectionParent);

		const tinyxml2::XMLError result = doc.SaveFile(out);
		if (result != tinyxml2::XML_SUCCESS)
			cemuLog_logDebug(LogType::Force, "XMLConfig: write error {} for \"{}\"", (int)result, boost::nowide::narrow(filename));
		fflush(out);
		fclose(out);

		fs::rename(tempName, filename, ec);
		if (ec)
		{
			cemuLog_log(LogType::Force, "XMLConfig: cannot replace the settings file: {}", ec.message());
			fs::remove(tempName, ec);
		}
		// note: success means the document was written, a failed rename only gets logged
		return result == tinyxml2::XML_SUCCESS;
	}

	[[nodiscard]] const std::wstring& GetFilename() const { return m_filename; }
	void SetFilename(const std::wstring& filename) { m_filename = filename; }

	std::unique_lock<std::mutex> Lock() { return std::unique_lock(m_mutex); }

	// sections stored by other objects inside this file, saved and loaded after the main object
	void AddChildConfig(ChildXMLConfigParser childConfigParser)
	{
		m_childConfigParsers.emplace_back(std::move(childConfigParser));
	}

private:
	std::vector<ChildXMLConfigParser> m_childConfigParsers;
	T& m_instance;
	std::wstring m_filename;
	std::mutex m_mutex;
};

// a callable that returns the XMLConfig a child section belongs to
template<typename T>
concept XMLConfigProvider = requires(T t, ChildXMLConfigParser& configParser) {
	{ t().Save() } -> std::same_as<bool>;
	{ t().Load() } -> std::same_as<bool>;
	{ t().Lock() } -> std::same_as<std::unique_lock<std::mutex>>;
	{ t().AddChildConfig(configParser) } -> std::same_as<void>;
};

// Settings object stored as a section of another config file. L and S read and
// write the section from the cursor the parent's object returns.
template<typename T, void (T::*L)(XMLConfigParser&), void (T::*S)(XMLConfigParser&)>
class XMLChildConfig
{
public:
	XMLChildConfig(XMLConfigProvider auto getParentConfig)
	{
		m_parent.lock = [getParentConfig]() { return getParentConfig().Lock(); };
		m_parent.save = [getParentConfig]() { return getParentConfig().Save(); };
		m_parent.load = [getParentConfig]() { return getParentConfig().Load(); };
		getParentConfig().AddChildConfig(ChildXMLConfigParser(
			[this](XMLConfigParser& parser) { (m_data.*S)(parser); },
			[this](XMLConfigParser& parser) { (m_data.*L)(parser); }));
	}

	virtual ~XMLChildConfig() = default;

	// saving or loading always covers the whole parent file
	bool Save() { return m_parent.save(); }
	bool Load() { return m_parent.load(); }
	std::unique_lock<std::mutex> Lock() { return m_parent.lock(); }

	T& Data() { return m_data; }

private:
	T m_data;
	struct
	{
		std::function<std::unique_lock<std::mutex>()> lock;
		std::function<bool()> save;
		std::function<bool()> load;
	} m_parent;
};

// An XML file that owns its object. T either has XMLConfigurable Load/Save or
// void Load/Save (XMLStandaloneConfigurable), see the specialisations below.
template<typename T>
struct XMLDataConfig {};

template<XMLConfigurable T>
class XMLDataConfig<T> : public XMLConfig<T>
{
public:
	XMLDataConfig() : XMLConfig<T>(m_data), m_data() {}
	XMLDataConfig(std::wstring_view filename) : XMLConfig<T>(m_data, filename), m_data() {}
	XMLDataConfig(std::wstring_view filename, T init_data) : XMLConfig<T>(m_data, filename), m_data(std::move(init_data)) {}
	XMLDataConfig(const XMLDataConfig&) = delete;

	T& data() { return m_data; }

private:
	T m_data;
};

template<typename T>
concept XMLStandaloneConfigurable = requires(T t, XMLConfigParser& configParser) {
	{ t.Save(configParser) } -> std::same_as<void>;
	{ t.Load(configParser) } -> std::same_as<void>;
};

// adapts void Load/Save to the XMLConfigurable form (no child sections)
template<XMLStandaloneConfigurable T>
struct XMLConfigWrapper
{
	T data;

	XMLConfigParser Load(XMLConfigParser& configParser) { return (data.Load(configParser), configParser); }
	XMLConfigParser Save(XMLConfigParser& configParser) { return (data.Save(configParser), configParser); }
};

template<XMLStandaloneConfigurable T>
class XMLDataConfig<T> : public XMLConfig<XMLConfigWrapper<T>>
{
	using Base = XMLConfig<XMLConfigWrapper<T>>;

public:
	XMLDataConfig() : Base(m_wrapper), m_wrapper() {}
	XMLDataConfig(std::wstring_view filename) : Base(m_wrapper, filename), m_wrapper() {}
	XMLDataConfig(std::wstring_view filename, T init_data) : Base(m_wrapper, filename), m_wrapper{.data = std::move(init_data)} {}
	XMLDataConfig(const XMLDataConfig&) = delete;

	T& data() { return m_wrapper.data; }

private:
	XMLConfigWrapper<T> m_wrapper;
};
