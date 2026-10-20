// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/DumpCtrl.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "util/helpers/helpers.h"

#include "Cemu/ExpressionParser/ExpressionParser.h"

namespace
{
	constexpr sint32 kColAddress = 60;
	constexpr sint32 kColModuleOffset = 90;
	constexpr sint32 kAsciiColumnX = 450;
	constexpr uint32 kBytesPerLine = 0x10;

	enum
	{
		ID_WRITE_U8 = wxID_HIGHEST + 1,
		ID_WRITE_U16,
		ID_WRITE_U32,
		ID_WRITE_FLOAT,
		ID_WRITE_STRING
	};

	template <typename T>
	struct ValueTraits;
	template <>
	struct ValueTraits<uint8>
	{
		static constexpr const char* name = "byte";
		static uint8 Read(uint32 a) { return memory_readU8(a); }
		static void Write(uint32 a, uint8 v) { memory_writeU8(a, v); }
		static wxString Show(uint8 v) { return wxString::Format("0x%02x", v); }
	};
	template <>
	struct ValueTraits<uint16>
	{
		static constexpr const char* name = "int16";
		static uint16 Read(uint32 a) { return memory_readU16(a); }
		static void Write(uint32 a, uint16 v) { memory_writeU16(a, v); }
		static wxString Show(uint16 v) { return wxString::Format("0x%04x", v); }
	};
	template <>
	struct ValueTraits<uint32>
	{
		static constexpr const char* name = "int32";
		static uint32 Read(uint32 a) { return memory_readU32(a); }
		static void Write(uint32 a, uint32 v) { memory_writeU32(a, v); }
		static wxString Show(uint32 v) { return wxString::Format("0x%08x", v); }
	};
	template <>
	struct ValueTraits<float>
	{
		static constexpr const char* name = "float";
		static float Read(uint32 a) { return memory_readFloat(a); }
		static void Write(uint32 a, float v) { memory_writeFloat(a, v); }
		static wxString Show(float v) { return wxString::Format("%f", v); }
	};
}

DumpCtrl::DumpCtrl(wxWindow* parent, const wxWindowID& id, const wxPoint& pos, const wxSize& size, long style)
	: TextList(parent, id, pos, size, style)
{
	// start in the region that holds 0x10000000 (main data)
	if (MMURange* range = memory_getMMURangeByAddress(0x10000000))
	{
		m_regionBase = range->getBase();
		m_regionSize = range->getSize();
	}
	else
	{
		m_regionBase = 0x10000000;
		m_regionSize = 0x1000;
	}
	Init();
	Bind(wxEVT_MENU, &DumpCtrl::OnMenuSelected, this);
}

void DumpCtrl::Init()
{
	SetElementCount(m_regionSize / kBytesPerLine);
	Scroll(0, 0);
	RefreshControl();
}

uint32 DumpCtrl::LineToOffset(uint32 line)
{
	return m_regionBase + line * kBytesPerLine;
}

uint32 DumpCtrl::OffsetToLine(uint32 offset)
{
	return (offset - m_regionBase) / kBytesPerLine;
}

void DumpCtrl::OnDraw(wxDC& dc, sint32 start, sint32 count, const wxPoint& start_position)
{
	const wxColour textColour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	const wxColour greyColour = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
	// module offsets are relative to the module the first painted line belongs to
	RPLModule* codeModule = RPLLoader_FindModuleByCodeAddr(LineToOffset(start));
	RPLModule* dataModule = RPLLoader_FindModuleByDataAddr(LineToOffset(start));
	const sint32 byteStep = m_char_width * 3;

	wxPoint at = start_position;
	for (sint32 i = 0; i <= count; i++)
	{
		const uint32 lineAddr = LineToOffset(start + i);

		dc.SetTextForeground(textColour);
		dc.DrawText(wxString::Format("%08x", lineAddr), at);
		at.x += kColAddress;

		// the hex bytes are drawn in the same grey as the module offset
		dc.SetTextForeground(greyColour);
		if (codeModule)
			dc.DrawText(wxString::Format("+0x%-8x", lineAddr - codeModule->regionMappingBase_text.GetMPTR()), at);
		else if (dataModule)
			dc.DrawText(wxString::Format("+0x%-8x", lineAddr - dataModule->regionMappingBase_data), at);
		else
			dc.DrawText("???", at);
		at.x += kColModuleOffset;

		if (memory_isAddressRangeAccessible(lineAddr, kBytesPerLine))
		{
			std::array<uint8, kBytesPerLine> bytes;
			memory_readBytes(lineAddr, bytes);
			for (uint8 b : bytes)
			{
				wxPoint cell = at;
				WriteText(dc, wxString::Format("%02x ", b), cell);
				at.x += byteStep;
			}
			at.x = kAsciiColumnX;
			dc.SetTextForeground(textColour);
			for (uint8 b : bytes)
			{
				dc.DrawText(isprint(b) ? wxString::Format("%c ", b) : wxString("."), at);
				at.x += m_char_width;
			}
		}
		else
		{
			for (uint32 b = 0; b < kBytesPerLine; b++)
			{
				wxPoint cell = at;
				WriteText(dc, "?? ", cell);
				at.x += byteStep;
			}
		}

		// red bracket in front of the byte last jumped to
		if (m_gotoMarker >= lineAddr && m_gotoMarker < lineAddr + kBytesPerLine)
		{
			const sint32 markX = start_position.x + start_position.x + kColAddress + kColModuleOffset + (sint32)(m_gotoMarker - lineAddr) * byteStep - 2;
			const wxPoint top(markX, at.y);
			const wxPoint bottom(markX, at.y + m_line_height);
			dc.SetPen(*wxRED_PEN);
			dc.DrawLine(top, bottom);
			dc.DrawLine(top, wxPoint(top.x + 3, top.y));
			dc.DrawLine(bottom, wxPoint(bottom.x + 3, bottom.y));
		}
		NextLine(at, &start_position);
	}

	// separators after every four hex bytes
	const sint32 hexX = start_position.x + kColAddress + kColModuleOffset;
	sint32 x = hexX + m_char_width * 11 + m_char_width / 2;
	const sint32 y0 = start_position.y;
	const sint32 y1 = y0 + m_line_height * (count + 1);
	dc.SetPen(wxSystemSettings::GetColour(wxSYS_COLOUR_INACTIVECAPTIONTEXT));
	for (int group = 0; group < 3; group++, x += m_char_width * 12)
		dc.DrawLine(x, y0, x, y1);
}

uint32 DumpCtrl::PositionToAddress(const wxPoint& position, uint32 line)
{
	sint32 x = position.x - (kColAddress + kColModuleOffset);
	if (x <= 0 || x > kAsciiColumnX)
		return MPTR_NULL;
	const uint32 byteIndex = (uint32)(x / m_char_width) / 3;
	return LineToOffset(line) + byteIndex;
}

void DumpCtrl::RefreshRow(uint32 line)
{
	RefreshLine(line);
}

void DumpCtrl::OnMouseDClick(const wxPoint& position, uint32 line)
{
	const uint32 address = PositionToAddress(position, line);
	if (address == MPTR_NULL || !memory_isAddressRangeAccessible(address, 1))
		return;
	if (WriteNumericDialog<uint8>(address))
		RefreshRow(line);
}

void DumpCtrl::ShowRegionOf(uint32 address)
{
	MMURange* range = memory_getMMURangeByAddress(address);
	if (range->getBase() == m_regionBase && range->getSize() == m_regionSize)
		return;
	m_regionBase = range->getBase();
	m_regionSize = range->getSize();
	Init();
}

void DumpCtrl::GoToAddressDialog()
{
	wxTextEntryDialog ask(this, _("Enter a target address."), _("GoTo address"), wxEmptyString);
	if (ask.ShowModal() != wxID_OK)
		return;

	std::string input = ask.GetValue().ToStdString();
	std::transform(input.begin(), input.end(), input.begin(), tolower);

	ExpressionParser parser;
	debugger_addParserSymbols(parser);
	// a bare number is read as hex ("1234" means 0x1234); otherwise the text is an expression
	for (const std::string& candidate : {"0x" + input, input})
	{
		if (!parser.IsConstantExpression(candidate))
			continue;
		const uint32 target = (uint32)parser.Evaluate(candidate);
		m_gotoMarker = target;
		CenterOffset(target);
		return;
	}
	try
	{
		(void)parser.Evaluate(input); // reports what is wrong with the expression
	}
	catch (const std::exception& ex)
	{
		wxMessageBox(ex.what(), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
	}
}

void DumpCtrl::CenterOffset(uint32 offset)
{
	if (!memory_isAddressRangeAccessible(offset, 1))
		return;
	ShowRegionOf(offset);
	const sint32 line = (sint32)OffsetToLine(offset);
	if (line < 0 || line >= (sint32)m_element_count)
		return;
	DoScroll(0, std::max(0, line - ((sint32)m_elements_visible / 2)));
	RefreshControl();
}

void DumpCtrl::OnKeyPressed(sint32 key_code, const wxPoint& /*position*/)
{
	if (key_code == 'G' && IsKeyDown(WXK_CONTROL))
		GoToAddressDialog();
}

wxSize DumpCtrl::DoGetBestSize() const
{
	return TextList::DoGetBestSize();
}

void DumpCtrl::OnContextMenu(const wxPoint& position, uint32 line)
{
	const uint32 address = PositionToAddress(position, line);
	if (!memory_isAddressRangeAccessible(address, 1))
		return;
	m_editAddress = address;
	m_editLine = line;

	wxMenu menu;
	menu.Append(ID_WRITE_U8, _("Write Byte"));
	menu.Append(ID_WRITE_U16, _("Write Int16"));
	menu.Append(ID_WRITE_U32, _("Write Int32"));
	menu.Append(ID_WRITE_FLOAT, _("Write Float"));
	menu.Append(ID_WRITE_STRING, _("Write String"));
	PopupMenu(&menu);
}

void DumpCtrl::OnMenuSelected(wxCommandEvent& event)
{
	bool written = false;
	switch (event.GetId())
	{
	case ID_WRITE_U8: written = WriteNumericDialog<uint8>(m_editAddress); break;
	case ID_WRITE_U16: written = WriteNumericDialog<uint16>(m_editAddress); break;
	case ID_WRITE_U32: written = WriteNumericDialog<uint32>(m_editAddress); break;
	case ID_WRITE_FLOAT: written = WriteNumericDialog<float>(m_editAddress); break;
	case ID_WRITE_STRING: written = WriteString(m_editAddress); break;
	default: break;
	}
	if (written)
		RefreshRow(m_editLine);
}

template <typename T>
bool DumpCtrl::WriteNumericDialog(uint32 address)
{
	using Traits = ValueTraits<T>;
	const T current = Traits::Read(address);
	wxTextEntryDialog ask(this, _("Enter a new value."), wxString::Format(_("Write %s at address 0x%08x"), Traits::name, address), Traits::Show(current));
	if (ask.ShowModal() != wxID_OK)
		return false;
	Traits::Write(address, ConvertString<T>(ask.GetValue().ToStdString()));
	return true;
}

bool DumpCtrl::WriteString(uint32 address)
{
	wxTextEntryDialog ask(this, _("Enter string"), wxString::Format(_("Write string at address 0x%08x"), address), "");
	if (ask.ShowModal() != wxID_OK)
		return false;
	const std::string text = ask.GetValue().ToStdString();
	if (text.empty())
		return false;
	for (size_t i = 0; i < text.size(); i++)
		memory_writeU8(address + (uint32)i, (uint8)text[i]);
	memory_writeU8(address + (uint32)text.size(), 0); // terminator
	return true;
}
