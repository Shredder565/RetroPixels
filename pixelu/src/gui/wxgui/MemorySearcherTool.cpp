// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/MemorySearcherTool.h"

#include "config/ActiveSettings.h"
#include "wxgui/helpers/wxHelpers.h"
#include "Common/FileStream.h"
#include "util/IniParser/IniParser.h"
#include "util/helpers/StringHelpers.h"
#include "Cafe/CafeSystem.h"

#include <wx/listctrl.h>
#include <wx/dataview.h>
#include <wx/combobox.h>
#include <wx/gauge.h>
#include <wx/menu.h>
#include <wx/timer.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	// popup menu of the stored entry table
	enum
	{
		ID_ENTRY_ADD = wxID_HIGHEST + 1,
		ID_ENTRY_REMOVE,
	};

	// columns of the stored entry table
	enum
	{
		kColDescription = 0,
		kColAddress,
		kColType,
		kColValue,
		kColFreeze,
	};

	constexpr size_t kMaxListedResults = 5000; // more hits are counted but not listed
	constexpr int kRefreshMs = 250;

	// ---- pure helpers (MS_PURE_BEGIN)

	constexpr uint32 kProgressBytes = 0x10000; // one gauge step per 64 KiB scanned
	constexpr uint32 kFilterTickItems = 4096; // filter progress is reported in batches

	struct TypeName
	{
		const char* name;
		SearchDataType type;
	};
	// the first kSearchableTypes are offered for searching; strings only exist as stored entries
	constexpr TypeName kTypeNames[] = {
		{ "float", SearchDataType_Float },
		{ "double", SearchDataType_Double },
		{ "int8", SearchDataType_Int8 },
		{ "int16", SearchDataType_Int16 },
		{ "int32", SearchDataType_Int32 },
		{ "int64", SearchDataType_Int64 },
		{ "string", SearchDataType_String },
	};
	constexpr size_t kSearchableTypes = 6;

	// exact (case sensitive) name as shown in the table
	SearchDataType TypeFromName(std::string_view name)
	{
		for (const TypeName& entry : kTypeNames)
		{
			if (name == entry.name)
				return entry.type;
		}
		return SearchDataType_None;
	}

	// stored entry files may spell the type in any case
	bool IsKnownTypeName(std::string_view name)
	{
		for (const TypeName& entry : kTypeNames)
		{
			if (boost::iequals(name, std::string_view(entry.name)))
				return true;
		}
		return false;
	}

	// the whole text must be one value of T (no surrounding spaces); int8 is read as a number, not a character
	template<typename T>
	bool ParsesExactly(const std::string& text)
	{
		std::istringstream in(text);
		in >> std::noskipws;
		if constexpr (std::is_same_v<T, sint8>)
		{
			sint16 wide;
			in >> wide;
			return in && in.eof() && wide >= -128 && wide <= 127;
		}
		else
		{
			T value;
			in >> value;
			return in && in.eof();
		}
	}

	template<typename T>
	T ReadBigEndian(const uint8* at)
	{
		betype<T> raw;
		std::memcpy(&raw, at, sizeof(T));
		return raw.value();
	}

	template<typename T>
	bool MatchesAt(const uint8* at, T wanted)
	{
		return equals(wanted, ReadBigEndian<T>(at));
	}

	// visits every sizeof(T) aligned element of [data, data + size); onHit(offset) for matches,
	// onProgress() after every kProgressBytes. Stops early once keepGoing turns false.
	template<typename T, typename HitFn, typename ProgressFn>
	void ScanForValue(const uint8* data, uint32 size, T wanted, const std::atomic_bool& keepGoing, HitFn&& onHit, ProgressFn&& onProgress)
	{
		uint32 sinceProgress = 0;
		for (uint32 offset = 0; size >= sizeof(T) && offset <= size - sizeof(T); offset += sizeof(T))
		{
			if (!keepGoing)
				return;
			if (MatchesAt<T>(data + offset, wanted))
				onHit(offset);
			sinceProgress += sizeof(T);
			if (sinceProgress >= kProgressBytes)
			{
				sinceProgress -= kProgressBytes;
				onProgress();
			}
		}
	}

	// the stored table's address column: hex, "0x" optional
	std::optional<uint32> ParseEntryAddress(const std::string& text)
	{
		const char* begin = text.c_str();
		char* end = nullptr;
		errno = 0;
		const unsigned long long value = std::strtoull(begin, &end, 16);
		if (end == begin || errno == ERANGE)
			return std::nullopt;
		return (uint32)value;
	}

	// lines of one [Entry] section; the value is only kept for frozen entries
	std::vector<std::string> StoredEntryLines(const std::string& description, const std::string& address, const std::string& type, const std::string& value, bool frozen)
	{
		return {
			"[Entry]",
			"description=" + description,
			"address=" + address,
			"type=" + type,
			"value=" + (frozen ? value : std::string()),
			"",
		};
	}

	// ---- MS_PURE_END

	// calls fn with a value of the C++ type for a numeric search type; does nothing otherwise
	template<typename Fn>
	void WithNumericType(SearchDataType type, Fn&& fn)
	{
		switch (type)
		{
		case SearchDataType_Float: fn(float{}); break;
		case SearchDataType_Double: fn(double{}); break;
		case SearchDataType_Int8: fn(sint8{}); break;
		case SearchDataType_Int16: fn(sint16{}); break;
		case SearchDataType_Int32: fn(sint32{}); break;
		case SearchDataType_Int64: fn(sint64{}); break;
		default: break;
		}
	}

	// current guest value as table text; nothing for strings
	std::optional<std::string> ReadGuestValue(SearchDataType type, uint32 address)
	{
		std::optional<std::string> text;
		WithNumericType(type, [&](auto tag) {
			using T = decltype(tag);
			const T value = memory_read<T>(address);
			if constexpr (std::is_same_v<T, sint8>)
				text = fmt::format("{}", (int)value);
			else
				text = fmt::format("{}", value);
		});
		return text;
	}

	// writes the table text in value to guest memory; text that is no valid value is ignored
	void WriteGuestValue(SearchDataType type, uint32 address, const wxVariant& value)
	{
		double real;
		long integer;
		switch (type)
		{
		case SearchDataType_Float:
			if (value.Convert(&real))
				memory_writeFloat(address, (float)real);
			break;
		case SearchDataType_Double:
			if (value.Convert(&real))
				memory_writeDouble(address, real);
			break;
		case SearchDataType_Int8:
			if (value.Convert(&integer))
				memory_writeU8(address, (uint8)integer);
			break;
		case SearchDataType_Int16:
			if (value.Convert(&integer))
				memory_writeU16(address, (uint16)integer);
			break;
		case SearchDataType_Int32:
			if (value.Convert(&integer))
				memory_writeU32(address, (uint32)integer);
			break;
		case SearchDataType_Int64:
		{
			const std::string text = value.GetString().ToStdString();
			char* end = nullptr;
			errno = 0;
			const unsigned long long number = std::strtoull(text.c_str(), &end, 10);
			if (end != text.c_str() && errno != ERANGE)
				memory_writeU64(address, number);
			break;
		}
		case SearchDataType_String:
		{
			// zero terminated
			const std::string text = value.GetString().ToStdString();
			for (size_t i = 0; i < text.size(); ++i)
				memory_writeU8(address + (uint32)i, (uint8)text[i]);
			memory_writeU8(address + (uint32)text.size(), 0);
			break;
		}
		default:
			break;
		}
	}

	fs::path StoredEntriesFile()
	{
		return ActiveSettings::GetUserDataPath("memorySearcher/{:016x}.ini", CafeSystem::GetForegroundTitleId());
	}
}

wxDEFINE_EVENT(wxEVT_MEMSEARCH_DONE, wxCommandEvent);

MemorySearcherTool::MemorySearcherTool(wxFrame* parent)
	: wxFrame(parent, wxID_ANY, _("Memory Searcher"), wxDefaultPosition, wxSize(600, 540), wxDEFAULT_FRAME_STYLE | wxTAB_TRAVERSAL)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);
	wxTopLevelWindowBase::SetMinSize(wxSize(600, 540));

	// search row: type, value, Search/Clear, Filter
	wxString typeChoices[kSearchableTypes];
	for (size_t i = 0; i < kSearchableTypes; ++i)
		typeChoices[i] = kTypeNames[i].name;
	m_cbDataType = new wxComboBox(this, wxID_ANY, typeChoices[0], wxDefaultPosition, wxDefaultSize, (int)kSearchableTypes, typeChoices, wxCB_READONLY);
	m_textValue = new wxTextCtrl(this, wxID_ANY);
	m_buttonStart = new wxButton(this, wxID_ANY, _("Search"));
	m_buttonStart->Bind(wxEVT_BUTTON, &MemorySearcherTool::OnSearch, this);
	m_buttonFilter = new wxButton(this, wxID_ANY, _("Filter"));
	m_buttonFilter->Bind(wxEVT_BUTTON, &MemorySearcherTool::OnFilter, this);
	m_buttonFilter->Disable(); // needs results first

	auto* searchRow = new wxFlexGridSizer(0, 4, 0, 0);
	searchRow->AddGrowableCol(1);
	searchRow->Add(m_cbDataType, 0, wxALL, 5);
	searchRow->Add(m_textValue, 0, wxALL | wxEXPAND, 5);
	searchRow->Add(m_buttonStart, 0, wxALL, 5);
	searchRow->Add(m_buttonFilter, 0, wxALL, 5);

	m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL);
	m_gauge->SetValue(0);
	m_gauge->Enable(false);

	// hits: address + live value; double click stores them
	m_textEntryTable = new wxStaticText(this, wxID_ANY, _("Results"));
	m_listResults = new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SORT_ASCENDING);
	m_listResults->Bind(wxEVT_LEFT_DCLICK, &MemorySearcherTool::OnResultListClick, this);
	m_listResults->AppendColumn(_("Address"), wxLIST_FORMAT_LEFT, 100);
	m_listResults->AppendColumn(_("Value"), wxLIST_FORMAT_LEFT, 250);

	// stored entries: description and value are editable, frozen values are rewritten every refresh
	auto* storedLabel = new wxStaticText(this, wxID_ANY, _("Stored Entries"));
	m_listEntryTable = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(420, 200), wxDV_HORIZ_RULES);
	m_listEntryTable->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, &MemorySearcherTool::OnEntryListRightClick, this);
	m_listEntryTable->Bind(wxEVT_DATAVIEW_ITEM_EDITING_DONE, &MemorySearcherTool::OnItemEdited, this);
	m_listEntryTable->AppendTextColumn(_("Description"), wxDATAVIEW_CELL_EDITABLE, 150, wxALIGN_LEFT, wxDATAVIEW_COL_SORTABLE);
	m_listEntryTable->AppendTextColumn(_("Address"), wxDATAVIEW_CELL_INERT, 100, wxALIGN_LEFT, wxDATAVIEW_COL_SORTABLE);
	m_listEntryTable->AppendTextColumn(_("Type"));
	m_listEntryTable->AppendTextColumn(_("Value"), wxDATAVIEW_CELL_EDITABLE);
	m_listEntryTable->AppendToggleColumn(_("Freeze"), wxDATAVIEW_CELL_ACTIVATABLE, 50, wxALIGN_LEFT, 0);

	auto* lists = new wxFlexGridSizer(0, 1, 0, 0);
	lists->AddGrowableCol(0);
	lists->AddGrowableRow(3);
	lists->AddGrowableRow(5);
	lists->Add(m_gauge, 0, wxALL | wxEXPAND, 5);
	lists->Add(0, 10, 1, wxEXPAND, 5);
	lists->Add(m_textEntryTable, 0, wxALL, 5);
	lists->Add(m_listResults, 1, wxALL | wxEXPAND, 5);
	lists->Add(storedLabel, 0, wxALL, 5);
	lists->Add(m_listEntryTable, 1, wxALL | wxEXPAND, 5);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(searchRow, 0, wxEXPAND, 5);
	layout->Add(lists, 1, wxEXPAND, 5);

	Load();

	Bind(wxEVT_CLOSE_WINDOW, &MemorySearcherTool::OnClose, this);
	Bind(wxEVT_MEMSEARCH_DONE, &MemorySearcherTool::OnSearchFinished, this);
	Bind(wxEVT_SET_GAUGE_VALUE, &MemorySearcherTool::OnUpdateGauge, this);

	m_refresh_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &MemorySearcherTool::OnTimerTick, this, m_refresh_timer->GetId());
	m_refresh_timer->Start(kRefreshMs);

	SetSizer(layout);
	wxWindowBase::Layout();
	Centre(wxBOTH);
}

MemorySearcherTool::~MemorySearcherTool()
{
	m_running = false; // tells the worker to stop
	m_refresh_timer->Stop();
	if (m_worker.joinable())
		m_worker.join();
}

void MemorySearcherTool::OnClose(wxCloseEvent& event)
{
	event.Skip();
	Save();
}

void MemorySearcherTool::OnTimerTick(wxTimerEvent& event)
{
	RefreshResultList();
	RefreshStashList();
}

void MemorySearcherTool::OnUpdateGauge(wxSetGaugeValue& event)
{
	wxGauge* gauge = event.GetGauge();
	if (event.GetRange() != 0)
	{
		// a new task: range and start value
		gauge->SetRange(event.GetRange());
		gauge->SetValue(event.GetValue());
	}
	else
		gauge->SetValue(gauge->GetValue() + event.GetValue()); // progress step
}

void MemorySearcherTool::SetSearchDataType()
{
	m_searchDataType = TypeFromName(m_cbDataType->GetStringSelection().utf8_string());
}

bool MemorySearcherTool::VerifySearchValue() const
{
	const std::string text = m_textValue->GetValue().ToStdString();
	switch (m_searchDataType)
	{
	case SearchDataType_String: return true;
	case SearchDataType_Float: return ParsesExactly<float>(text);
	case SearchDataType_Double: return ParsesExactly<double>(text);
	case SearchDataType_Int8: return ParsesExactly<sint8>(text);
	case SearchDataType_Int16: return ParsesExactly<sint16>(text);
	case SearchDataType_Int32: return ParsesExactly<sint32>(text);
	case SearchDataType_Int64: return ParsesExactly<sint64>(text);
	default: return false;
	}
}

void MemorySearcherTool::RunOnWorker(std::function<ListType_t()> job)
{
	if (m_worker.joinable())
		m_worker.join();
	m_search_running = true;
	m_worker = std::thread([this, job = std::move(job)]() {
		ListType_t result = job();
		if (!m_running)
			return; // the window is going away
		{
			std::lock_guard lock(m_mutex);
			m_searchBuffer.swap(result);
		}
		wxQueueEvent(this, new wxCommandEvent(wxEVT_MEMSEARCH_DONE));
	});
}

void MemorySearcherTool::OnSearch(wxCommandEvent&)
{
	// with results shown the button is "Clear"
	if (m_clear_state)
	{
		Reset();
		return;
	}
	if (m_search_running || m_textValue->IsEmpty())
		return;

	SetSearchDataType();
	if (!VerifySearchValue())
	{
		wxMessageBox(_("Your entered value is not valid for the selected datatype."), _("Error"), wxICON_ERROR);
		return;
	}

	m_buttonStart->Disable();
	m_buttonFilter->Disable();
	m_cbDataType->Disable();
	m_buttonStart->SetLabelText(_("Clear"));
	m_clear_state = true;

	const SearchDataType type = m_searchDataType;
	const std::string text = m_textValue->GetValue().ToStdString();
	RunOnWorker([this, type, text]() -> ListType_t {
		struct Block
		{
			const uint8* data;
			uint32 size;
		};
		std::vector<Block> blocks;
		int steps = 0;
		for (MMURange* range : memory_getMMURanges())
		{
			if (!m_running)
				return {};
			if (!range->isMapped())
				continue;
			blocks.push_back({ range->getPtr(), range->getSize() });
			steps += (int)(range->getSize() / kProgressBytes);
		}
		wxQueueEvent(this, new wxSetGaugeValue(0, steps, m_gauge));

		// one scanner per mapped range
		ListType_t found;
		WithNumericType(type, [&](auto tag) {
			using T = decltype(tag);
			const T wanted = ConvertString<T>(text);
			std::vector<std::future<ListType_t>> scans;
			for (const Block& block : blocks)
			{
				scans.emplace_back(std::async(std::launch::async, [this, block, wanted]() {
					ListType_t hits;
					ScanForValue<T>(block.data, block.size, wanted, m_running,
						[&](uint32 offset) { hits.emplace_back((void*)(block.data + offset)); },
						[this]() { wxQueueEvent(this, new wxSetGaugeValue(1, m_gauge)); });
					return hits;
				}));
			}
			for (auto& scan : scans)
			{
				const ListType_t hits = scan.get();
				found.insert(found.end(), hits.cbegin(), hits.cend());
			}
		});
		return found;
	});
}

void MemorySearcherTool::OnFilter(wxCommandEvent& event)
{
	m_buttonStart->Disable();
	m_buttonFilter->Disable();

	// keeps the hits that now hold the (possibly changed) value, same type as the search
	const SearchDataType type = m_searchDataType;
	const std::string text = m_textValue->GetValue().ToStdString();
	RunOnWorker([this, type, text]() -> ListType_t {
		const uint32 count = (uint32)m_searchBuffer.size();
		wxQueueEvent(this, new wxSetGaugeValue(0, (int)count, m_gauge));

		ListType_t kept;
		WithNumericType(type, [&](auto tag) {
			using T = decltype(tag);
			const T wanted = ConvertString<T>(text);
			kept.reserve(count);
			uint32 unreported = 0;
			for (const MEMPTR<void>& address : m_searchBuffer)
			{
				if (!m_running)
					break;
				if (MatchesAt<T>((const uint8*)address.GetPtr(), wanted))
					kept.emplace_back(address);
				if (++unreported == kFilterTickItems)
				{
					wxQueueEvent(this, new wxSetGaugeValue((int)unreported, m_gauge));
					unreported = 0;
				}
			}
			if (unreported != 0)
				wxQueueEvent(this, new wxSetGaugeValue((int)unreported, m_gauge));
		});
		return kept;
	});

	m_gauge->SetValue(0);
}

void MemorySearcherTool::OnSearchFinished(wxCommandEvent&)
{
	FillResultList();
	m_search_running = false;
	m_buttonStart->Enable();
	m_buttonFilter->Enable();
}

void MemorySearcherTool::Reset()
{
	m_clear_state = false;
	m_searchBuffer.clear();
	m_listResults->DeleteAllItems();
	m_textEntryTable->SetLabelText(_("Results"));
	m_textValue->SetValue("");
	// a new search may use any data type; filtering needs results first
	m_cbDataType->Enable();
	m_buttonFilter->Disable();
	m_buttonStart->SetLabelText(_("Search"));
}

void MemorySearcherTool::FillResultList()
{
	m_textEntryTable->SetLabelText(formatWxString(_("Results ({0})"), m_searchBuffer.size()));
	m_listResults->DeleteAllItems();
	if (m_searchBuffer.empty() || m_searchBuffer.size() > kMaxListedResults)
		return;

	// the value column starts with the searched text; the refresh timer replaces it with live values
	const wxString searched = m_textValue->GetValue();
	for (const MEMPTR<void>& address : m_searchBuffer)
	{
		const long row = m_listResults->InsertItem(0, fmt::format("{:#08x}", address.GetMPTR()));
		m_listResults->SetItemData(row, address.GetMPTR());
		m_listResults->SetItem(row, 1, searched);
	}
}

void MemorySearcherTool::RefreshResultList()
{
	std::lock_guard lock(m_mutex);
	if (m_searchBuffer.empty() || m_searchBuffer.size() > kMaxListedResults)
		return;
	const int rows = m_listResults->GetItemCount();
	for (int row = 0; row < rows; ++row)
	{
		const auto value = ReadGuestValue(m_searchDataType, (uint32)m_listResults->GetItemData(row));
		if (value)
			m_listResults->SetItem(row, 1, *value);
	}
}

void MemorySearcherTool::RefreshStashList()
{
	const int rows = m_listEntryTable->GetItemCount();
	for (int row = 0; row < rows; ++row)
	{
		const auto address = ParseEntryAddress(m_listEntryTable->GetTextValue(row, kColAddress).ToStdString());
		if (!address)
			continue;
		const SearchDataType type = TypeFromName(m_listEntryTable->GetTextValue(row, kColType).utf8_string());

		if (m_listEntryTable->GetToggleValue(row, kColFreeze))
		{
			// frozen: the table value is written back instead of read
			wxVariant value;
			m_listEntryTable->GetValue(value, row, kColValue);
			WriteGuestValue(type, *address, value);
		}
		else if (const auto current = ReadGuestValue(type, *address))
			m_listEntryTable->SetValue(wxVariant(wxString(*current)), row, kColValue);
	}
}

void MemorySearcherTool::OnResultListClick(wxMouseEvent& event)
{
	// stores every selected hit (description, address, type, value, freeze)
	for (long row = m_listResults->GetFirstSelected(); row != wxNOT_FOUND; row = m_listResults->GetNextSelected(row))
	{
		wxVector<wxVariant> entry;
		entry.push_back(wxString());
		entry.push_back(wxString::Format("0x%08x", (uint32)m_listResults->GetItemData(row)));
		entry.push_back(m_cbDataType->GetValue());
		entry.push_back(m_listResults->GetItemText(row, 1));
		entry.push_back(false);
		m_listEntryTable->AppendItem(entry);
	}
}

void MemorySearcherTool::OnEntryListRightClick(wxDataViewEvent& event)
{
	wxMenu menu;
	menu.Append(ID_ENTRY_ADD, _("&Add new entry"))->Enable(false);
	menu.Append(ID_ENTRY_REMOVE, _("&Remove entry"));
	menu.Bind(wxEVT_MENU, &MemorySearcherTool::OnPopupClick, this);
	PopupMenu(&menu);
}

void MemorySearcherTool::OnPopupClick(wxCommandEvent& event)
{
	if (event.GetId() != ID_ENTRY_REMOVE)
		return;
	const int row = m_listEntryTable->GetSelectedRow();
	if (row != wxNOT_FOUND)
		m_listEntryTable->DeleteItem(row);
}

void MemorySearcherTool::OnItemEdited(wxDataViewEvent& event)
{
	// a typed value goes to guest memory right away; descriptions need nothing
	if (event.GetColumn() != kColValue || event.IsEditCancelled())
		return;
	int row = m_listEntryTable->ItemToRow(event.GetItem());
	if (row == wxNOT_FOUND)
		row = m_listEntryTable->GetSelectedRow();
	if (row == wxNOT_FOUND)
		return;
	const auto address = ParseEntryAddress(m_listEntryTable->GetTextValue(row, kColAddress).ToStdString());
	if (!address)
		return;
	WriteGuestValue(TypeFromName(m_listEntryTable->GetTextValue(row, kColType).utf8_string()), *address, event.GetValue());
}

bool MemorySearcherTool::IsAddressValid(uint32 addr)
{
	for (MMURange* range : memory_getMMURanges())
	{
		if (!range->isMapped())
			continue;
		const uint32 start = MEMPTR<uint8>(range->getPtr()).GetMPTR();
		if (addr - start < range->getSize())
			return true;
	}
	return false;
}

void MemorySearcherTool::Load()
{
	const fs::path file = StoredEntriesFile();
	auto contents = FileStream::LoadIntoMemory(file);
	if (!contents)
		return;

	IniParser ini(*contents, _pathToUtf8(file));
	while (ini.NextSection())
	{
		const auto description = ini.FindOption("description");
		const auto address = ini.FindOption("address");
		const auto type = ini.FindOption("type");
		const auto value = ini.FindOption("value");
		if (!description || !address || !type || !value)
			continue;
		if (!IsAddressValid((uint32)StringHelpers::ToInt64(*address)) || !IsKnownTypeName(*type))
			continue;

		// texts are stored in the local encoding (as written by Save)
		wxVector<wxVariant> entry;
		entry.push_back(wxString(std::string(*description).c_str()));
		entry.push_back(wxString(std::string(*address).c_str()));
		entry.push_back(wxString(std::string(*type).c_str()));
		entry.push_back(wxString(std::string(*value).c_str()));
		entry.push_back(!value->empty()); // only frozen entries keep a value
		m_listEntryTable->AppendItem(entry);
	}
}

void MemorySearcherTool::Save()
{
	const fs::path file = StoredEntriesFile();
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	std::unique_ptr<FileStream> out(FileStream::createFile2(file));
	if (!out)
		return;
	const int rows = m_listEntryTable->GetItemCount();
	for (int row = 0; row < rows; ++row)
	{
		const auto lines = StoredEntryLines(
			m_listEntryTable->GetTextValue(row, kColDescription).ToStdString(),
			m_listEntryTable->GetTextValue(row, kColAddress).ToStdString(),
			m_listEntryTable->GetTextValue(row, kColType).ToStdString(),
			m_listEntryTable->GetTextValue(row, kColValue).ToStdString(),
			m_listEntryTable->GetToggleValue(row, kColFreeze));
		for (const std::string& line : lines)
			out->writeLine(line.c_str());
	}
}
