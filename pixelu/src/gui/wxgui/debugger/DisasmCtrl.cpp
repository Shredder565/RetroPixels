// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/DisasmCtrl.h"

#include "wxHelper.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/OS/RPL/rpl_debug_symbols.h"
#include "Cemu/PPCAssembler/ppcAssembler.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "wxgui/debugger/DebuggerWindow2.h"
#include "util/helpers/helpers.h"

#include "Cemu/ExpressionParser/ExpressionParser.h"
#include "Cafe/HW/Espresso/Debugger/DebugSymbolStorage.h"

#include <wx/clipbrd.h>

wxDEFINE_EVENT(wxEVT_DISASMCTRL_NOTIFY_GOTO_ADDRESS, wxCommandEvent);

// ==== disasm-text begin ====
// Text of one disassembled instruction as coloured pieces (no wx types, so it can be tested on its own).
namespace disasmtext
{
	enum class Ink
	{
		Text,
		Muted,
		Gpr,
		Fpr,
		Spr,
		Cr,
		Imm,
		ImmOffset,
		CallTarget,
		Unknown,
		Todo,
	};

	struct Piece
	{
		std::string text;
		Ink ink;
	};

	// rotate-and-mask forms; their immediates are shown in decimal and they get a C-like hint
	inline bool IsRotateMaskForm(uint32 op)
	{
		switch (op)
		{
		case PPCASM_OP_RLWINM: case PPCASM_OP_RLWINM_:
		case PPCASM_OP_CLRLWI: case PPCASM_OP_CLRLWI_:
		case PPCASM_OP_CLRRWI: case PPCASM_OP_CLRRWI_:
		case PPCASM_OP_EXTLWI: case PPCASM_OP_EXTLWI_:
		case PPCASM_OP_EXTRWI: case PPCASM_OP_EXTRWI_:
		case PPCASM_OP_SLWI: case PPCASM_OP_SLWI_:
		case PPCASM_OP_SRWI: case PPCASM_OP_SRWI_:
		case PPCASM_OP_ROTRWI: case PPCASM_OP_ROTRWI_:
		case PPCASM_OP_ROTLWI: case PPCASM_OP_ROTLWI_:
			return true;
		default:
			return false;
		}
	}

	inline std::string Mnemonic(uint32 op)
	{
		std::string name = ppcAssembler_getInstructionName(op);
		for (char& c : name)
			c = (char)tolower((unsigned char)c);
		return name;
	}

	inline std::string ImmediateText(const PPCDisassemblerOperand& o, bool decimal)
	{
		if (o.isSignedImm)
		{
			sint32 v = o.immS32;
			if (o.immWidth == 16 && (v & 0x8000) != 0)
				v |= (sint32)0xFFFF0000; // sign-extend 16-bit fields
			if (decimal || (v > -10 && v < 10))
				return fmt::format("{}", v);
			if (v < 0)
				return fmt::format("-0x{:x}", 0u - (uint32)v);
			return fmt::format("0x{:x}", (uint32)v);
		}
		const uint32 u = o.immU32;
		if (decimal || u < 10)
			return fmt::format("{}", u);
		return fmt::format("0x{:x}", u);
	}

	// callLabel: symbol text for a bl/bla target if one is known
	inline void Operands(const PPCDisassembledInstruction& ins, uint32 opcode, uint32 address, const std::optional<std::string>& callLabel, std::vector<Piece>& out)
	{
		const bool decimal = IsRotateMaskForm(ins.ppcAsmCode);
		if (ins.ppcAsmCode == PPCASM_OP_UKN)
			out.push_back({fmt::format("{:02x} {:02x} {:02x} {:02x}", opcode >> 24, (opcode >> 16) & 0xFF, (opcode >> 8) & 0xFF, opcode & 0xFF), Ink::Unknown});

		bool needComma = false;
		for (sint32 i = 0; i < PPCASM_OPERAND_COUNT; i++)
		{
			if (((ins.operandMask >> i) & 1) == 0)
				continue;
			if (needComma)
				out.push_back({", ", Ink::Text});
			needComma = true;

			const PPCDisassemblerOperand& o = ins.operand[i];
			switch (o.type)
			{
			case PPCASM_OPERAND_TYPE_GPR: out.push_back({fmt::format("r{}", o.registerIndex), Ink::Gpr}); break;
			case PPCASM_OPERAND_TYPE_FPR: out.push_back({fmt::format("f{}", o.registerIndex), Ink::Fpr}); break;
			case PPCASM_OPERAND_TYPE_SPR: out.push_back({fmt::format("spr{}", o.registerIndex), Ink::Spr}); break;
			case PPCASM_OPERAND_TYPE_CR: out.push_back({fmt::format("cr{}", o.registerIndex), Ink::Cr}); break;
			case PPCASM_OPERAND_TYPE_IMM: out.push_back({ImmediateText(o, decimal), Ink::Imm}); break;
			case PPCASM_OPERAND_TYPE_PSQMODE: out.push_back({o.immS32 != 0 ? "single" : "paired", Ink::Imm}); break;
			case PPCASM_OPERAND_TYPE_CIMM:
			{
				const uint32 target = o.immU32;
				std::string label;
				if (callLabel)
				{
					label = *callLabel;
					if (label.size() >= 36)
						label = label.substr(0, 34) + "..";
				}
				else
					label = fmt::format("0x{:08x}", target);
				out.push_back({label, Ink::CallTarget});
				if (ins.ppcAsmCode != PPCASM_OP_BL)
					out.push_back({target <= address ? " \xE2\x86\x91" : " \xE2\x86\x93", Ink::Text}); // up / down arrow
				break;
			}
			case PPCASM_OPERAND_TYPE_MEM:
			{
				std::string disp;
				if (o.isSignedImm && o.immS32 >= 0)
					disp = fmt::format("+0x{:x}", (uint32)o.immS32);
				else
					disp = fmt::format("-0x{:x}", 0u - o.immU32);
				out.push_back({disp, Ink::ImmOffset});
				out.push_back({"(", Ink::Text});
				out.push_back({fmt::format("r{}", o.registerIndex), Ink::Gpr});
				out.push_back({")", Ink::Text});
				break;
			}
			default:
				out.push_back({"<TODO>", Ink::Todo});
				break;
			}
		}
	}

	// C-like explanation shown in the comment column when there is no user comment
	inline std::string Hint(const PPCDisassembledInstruction& ins, uint32 opcode)
	{
		const uint32 f21 = (opcode >> 21) & 0x1F, f16 = (opcode >> 16) & 0x1F, f11 = (opcode >> 11) & 0x1F;
		if (IsRotateMaskForm(ins.ppcAsmCode))
		{
			const uint32 mb = (opcode >> 6) & 0x1F, me = (opcode >> 1) & 0x1F;
			const uint32 mask = ppcAssembler_generateMaskRLW(mb, me);
			const uint32 sh = f11;
			if (sh == 0)
				return fmt::format("r{}=r{}&0x{:x}", f16, f21, mask);
			if ((0xFFFFFFFFu << ((uint32)ins.operand[2].immS32 & 31)) == mask)
				return fmt::format("r{}=r{}<<{}", f16, f21, sh);
			if ((0xFFFFFFFFu >> (32 - sh)) == mask)
				return fmt::format("r{}=r{}>>{}", f16, f21, 32 - sh);
			return fmt::format("r{}=(r{}<<<{})&0x{:x}", f16, f21, sh, mask);
		}
		if (ins.ppcAsmCode == PPCASM_OP_SUBF || ins.ppcAsmCode == PPCASM_OP_SUBF_)
			return fmt::format("r{}=r{}-r{}", f21, f11, f16);
		return {};
	}
}
// ==== disasm-text end ====

namespace
{
	// column layout in pixels
	constexpr sint32 kColAddress = 60;
	constexpr sint32 kColModuleOffset = 90;
	constexpr sint32 kColInstruction = 300;
	constexpr sint32 kColOperands = 80;
	constexpr size_t kMaxLabelLength = 120;

	enum
	{
		ID_MENU_TOGGLE_BP = wxID_HIGHEST + 1,
		ID_MENU_TOGGLE_LOGPOINT,
		ID_MENU_RESTORE_CODE,
		ID_MENU_COPY_ADDRESS,
		ID_MENU_COPY_IMAGE_ADDRESS,
		ID_MENU_LAST
	};

	struct DisasmColours
	{
		wxColour text, muted, background;
		wxColour lineBpAndIp, lineIp, lineBp, lineLogpoint, lineGoto;
		wxColour gpr, fpr, spr, cr, imm, immOffset, callTarget, unknown, symbol;
		wxColour opcode, dataType, patchedOpcode, patchedData;
	};
	DisasmColours s_ink;

	wxColour Pick(const wxColour& light, const wxColour& dark)
	{
		return wxSystemSettings::SelectLightDark(light, dark);
	}

	void LoadColours()
	{
		s_ink.text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
		s_ink.muted = Pick(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT), wxColour(140, 142, 145));
		s_ink.background = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

		s_ink.lineBp = Pick(wxColour(0xFF, 0x80, 0x80), wxColour(0x84, 0x21, 0x21));
		s_ink.lineBpAndIp = Pick(wxColour(0xD2, 0x7E, 0xD2), wxColour(0x91, 0x44, 0xA4));
		s_ink.lineIp = Pick(wxColour(0xFF, 0xA0, 0x80), wxColour(0x87, 0x53, 0x1A));
		s_ink.lineLogpoint = Pick(wxColour(0xAB, 0xED, 0xEE), wxColour(0x25, 0x5D, 0x6D));
		s_ink.lineGoto = Pick(wxColour(0xE0, 0xE0, 0xE0), wxColour(0x40, 0x40, 0x40));

		s_ink.gpr = Pick(wxColour(0x66, 0x00, 0x00), wxColour(0xE0, 0x6C, 0x75));
		s_ink.fpr = Pick(wxColour(0x66, 0x66, 0x00), wxColour(0xD1, 0x9A, 0x66));
		s_ink.spr = Pick(wxColour(0x00, 0x66, 0x66), wxColour(0x56, 0xB6, 0xC2));
		s_ink.cr = s_ink.spr;
		s_ink.imm = Pick(wxColour(0x00, 0x66, 0x00), wxColour(0x9D, 0xDE, 0x6F));
		s_ink.immOffset = s_ink.imm;
		s_ink.callTarget = Pick(wxColour(0x00, 0x00, 0x88), wxColour(0x61, 0xAF, 0xEF));
		s_ink.unknown = Pick(wxColour(0xA0, 0xA0, 0xA0), wxColour(0x5C, 0x63, 0x70));
		s_ink.symbol = Pick(wxColour(0xA0, 0x00, 0x00), wxColour(0xE0, 0x6C, 0x75));

		s_ink.opcode = Pick(wxColour(0xFF, 0x00, 0x40), wxColour(0xFF, 0x70, 0x7B));
		s_ink.patchedOpcode = Pick(wxColour(0xFF, 0x20, 0x20), wxColour(0xCC, 0x52, 0xF5));
		s_ink.dataType = Pick(wxColour(0xFF, 0x20, 0x20), wxColour(0xCE, 0x91, 0x78));
		s_ink.patchedData = Pick(wxColour(0xFF, 0x20, 0x20), wxColour(0xF4, 0x43, 0x36));
	}

	const wxColour& InkColour(disasmtext::Ink ink)
	{
		using disasmtext::Ink;
		static const wxColour todoGrey(0x44, 0x44, 0x44);
		switch (ink)
		{
		case Ink::Muted: return s_ink.muted;
		case Ink::Gpr: return s_ink.gpr;
		case Ink::Fpr: return s_ink.fpr;
		case Ink::Spr: return s_ink.spr;
		case Ink::Cr: return s_ink.cr;
		case Ink::Imm: return s_ink.imm;
		case Ink::ImmOffset: return s_ink.immOffset;
		case Ink::CallTarget: return s_ink.callTarget;
		case Ink::Unknown: return s_ink.unknown;
		case Ink::Todo: return todoGrey;
		case Ink::Text:
		default: return s_ink.text;
		}
	}

	// small filled arrow in front of the instruction the CPU is stopped at
	void DrawIpMarker(wxDC& dc, const wxPoint& at, sint32 lineHeight)
	{
		const sint32 h = std::max(6, std::min(11, lineHeight - 4));
		const sint32 w = 14;
		const wxPoint shape[] = {
			wxPoint(0, h / 2 - 2), wxPoint(w - h / 2 - 1, h / 2 - 2), wxPoint(w - h / 2 - 1, 0), wxPoint(w - 1, h / 2),
			wxPoint(w - h / 2 - 1, h - 1), wxPoint(w - h / 2 - 1, h - h / 2 + 1), wxPoint(0, h - h / 2 + 1),
		};
		const wxPen oldPen = dc.GetPen();
		const wxBrush oldBrush = dc.GetBrush();
		dc.SetPen(wxPen(s_ink.text));
		dc.SetBrush(wxBrush(s_ink.text));
		dc.DrawPolygon(WXSIZEOF(shape), shape, at.x, at.y);
		dc.SetPen(oldPen);
		dc.SetBrush(oldBrush);
	}

	// address as it appears in the module file (text section based at 0x02000000), 0 if unknown
	uint32 ImageAddress(MPTR address)
	{
		RPLModule* mod = RPLLoader_FindModuleByCodeAddr(address);
		if (mod == nullptr)
			return 0;
		const MPTR textBase = mod->regionMappingBase_text.GetMPTR();
		if (address < textBase || address >= textBase + mod->regionSize_text)
			return 0;
		return 0x02000000 + (address - textBase);
	}

	// symbol text for a bl/bla target; the library prefix is left out inside the caller's own module
	std::optional<std::string> CallTargetLabel(const PPCDisassembledInstruction& ins, MPTR address)
	{
		if (ins.ppcAsmCode != PPCASM_OP_BL && ins.ppcAsmCode != PPCASM_OP_BLA)
			return std::nullopt;
		uint32 target = 0;
		for (sint32 i = 0; i < PPCASM_OPERAND_COUNT; i++)
		{
			if (((ins.operandMask >> i) & 1) && ins.operand[i].type == PPCASM_OPERAND_TYPE_CIMM)
			{
				target = ins.operand[i].immU32;
				break;
			}
		}
		RPLStoredSymbol* sym = rplSymbolStorage_getByAddress(target);
		if (sym == nullptr)
			return std::nullopt;
		RPLModule* own = RPLLoader_FindModuleByCodeAddr(address);
		if (own != nullptr)
		{
			const MPTR textBase = own->regionMappingBase_text.GetMPTR();
			if (target >= textBase && target < textBase + own->regionSize_text)
				return std::string((const char*)sym->symbolName);
		}
		return fmt::format("{}.{}", (const char*)sym->libName, (const char*)sym->symbolName);
	}

	void PutOnClipboard(const std::string& text)
	{
		wxClipboard* cb = wxClipboard::Get();
		if (!cb->Open())
			return;
		cb->SetData(new wxTextDataObject(text));
		cb->Close();
	}
}

DisasmCtrl::DisasmCtrl(wxWindow* parent, const wxWindowID& id, const wxPoint& pos, const wxSize& size, long style)
	: TextList(parent, id, pos, size, style)
{
	Init();

	LoadColours();
	Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& evt) {
		evt.Skip();
		CallAfter([this] {
			LoadColours();
			Refresh();
		});
	});

	auto* tipLayout = new wxBoxSizer(wxVERTICAL);
	tipLayout->Add(new wxStaticText(m_tooltip_window, wxID_ANY, wxEmptyString), 0, wxALL, 5);
	m_tooltip_window->SetSizer(tipLayout);

	Bind(wxEVT_MENU, &DisasmCtrl::OnMenuCommand, this, ID_MENU_TOGGLE_BP, ID_MENU_LAST);
}

void DisasmCtrl::Init()
{
	SelectCodeRegion(mmuRange_TEXT_AREA.getBase());
}

void DisasmCtrl::SelectCodeRegion(uint32 newAddress)
{
	// the view always covers the whole MMU range the address lies in
	if (MMURange* range = memory_getMMURangeByAddress(newAddress))
	{
		m_regionBegin = range->getBase();
		m_regionEnd = range->getEnd();
	}
	else
	{
		m_regionBegin = 0;
		m_regionEnd = 0;
	}
	const sint32 bytes = (sint32)(m_regionEnd - m_regionBegin);
	if (SetElementCount(bytes / 4))
	{
		Scroll(0, 0);
		RefreshControl();
	}
}

void DisasmCtrl::DrawInstructionLine(wxDC& dc, const wxPoint& linePos, MPTR address, RPLModule* viewModule)
{
	wxPoint at = linePos;
	const bool patched = debugger_hasPatch(address);

	// with an enabled execute breakpoint in place, memory holds the trap: show the original instruction
	const DebuggerBreakpoint* bp = debugger_getFirstBP(address);
	while (bp != nullptr && !(bp->isExecuteBP() && bp->enabled))
		bp = bp->next;
	const uint32 opcode = bp ? bp->originalOpcodeValue : memory_readU32(address);

	bool isCurrentIp = false;
	if (PPCInterpreter_t* cpu = debugger_lockDebugSession())
	{
		isCurrentIp = cpu->instructionPointer == address;
		debugger_unlockDebugSession(cpu);
	}

	const wxColour* lineColour = &s_ink.background;
	if (isCurrentIp)
		lineColour = bp ? &s_ink.lineBpAndIp : &s_ink.lineIp;
	else if (bp)
		lineColour = bp->bpType == DEBUGGER_BP_T_NORMAL ? &s_ink.lineBp : &s_ink.lineLogpoint;
	else if (address == m_gotoTarget)
		lineColour = &s_ink.lineGoto;
	DrawLineBackground(dc, at, *lineColour);

	dc.SetTextForeground(s_ink.text);
	dc.DrawText(wxString::Format("%08x", address), at);
	at.x += kColAddress;

	dc.SetTextForeground(s_ink.muted);
	dc.DrawText(viewModule ? wxString::Format("+0x%-8x", address - viewModule->regionMappingBase_text.GetMPTR()) : wxString("???"), at);
	at.x += kColModuleOffset;

	if (isCurrentIp)
		DrawIpMarker(dc, wxPoint(at.x - 24, at.y + 2), m_line_height);

	// addresses marked as data by debug symbols show their value instead of an instruction
	const auto dataType = DebugSymbolStorage::GetDataType(address);
	if (dataType == DEBUG_SYMBOL_TYPE::FLOAT || dataType == DEBUG_SYMBOL_TYPE::U32)
	{
		const bool isFloat = dataType == DEBUG_SYMBOL_TYPE::FLOAT;
		dc.SetTextForeground(patched ? s_ink.patchedData : s_ink.dataType);
		dc.DrawText(isFloat ? ".float" : ".uint", at);
		at.x += kColOperands;
		dc.SetTextForeground(patched ? s_ink.patchedData : s_ink.imm);
		dc.DrawText(isFloat ? fmt::format("{}", memory_readFloat(address)) : fmt::format("{}", memory_readU32(address)), at);
		return;
	}

	PPCDisassembledInstruction ins;
	ppcAssembler_disassemble(address, opcode, &ins);

	const sint32 instructionX = at.x;
	dc.SetTextForeground(patched ? s_ink.patchedOpcode : s_ink.opcode);
	dc.DrawText(wxString::Format("%-12s", disasmtext::Mnemonic(ins.ppcAsmCode)), at);
	at.x += kColOperands;

	std::vector<disasmtext::Piece> pieces;
	disasmtext::Operands(ins, opcode, address, CallTargetLabel(ins, address), pieces);
	for (const disasmtext::Piece& p : pieces)
		WriteText(dc, wxString::FromUTF8(p.text), at, InkColour(p.ink));

	// comment column: the user's comment, else a hint for some instructions
	at.x = instructionX + kColInstruction;
	const auto* note = static_cast<rplDebugSymbolComment*>(rplDebugSymbol_getForAddress(address));
	if (note != nullptr && note->type == RplDebugSymbolComment)
	{
		WriteText(dc, note->comment, at, s_ink.text);
		return;
	}
	const std::string hint = disasmtext::Hint(ins, opcode);
	if (!hint.empty())
		WriteText(dc, hint, at, s_ink.muted);
}

void DisasmCtrl::DrawSymbolLine(wxDC& dc, const wxPoint& linePos, RPLStoredSymbol* symbol)
{
	wxString label = wxString::Format("%s:", (const char*)symbol->symbolName);
	if (label.Length() > kMaxLabelLength)
		label = label.Left(kMaxLabelLength - 3) + "..:";
	wxPoint at = linePos;
	WriteText(dc, label, at, s_ink.symbol);
}

void DisasmCtrl::OnDraw(wxDC& dc, sint32 start, sint32 count, const wxPoint& start_position)
{
	RPLModule* viewModule = RPLLoader_FindModuleByCodeAddr(GetViewBaseAddress());
	if (m_regionBegin == m_regionEnd)
		return;

	// Lines are laid out from the top of the view, because symbol labels take a
	// line of their own and shift every following address. Lines above the dirty
	// area are only laid out, not painted.
	const sint32 firstDirty = start - GetViewStart().y;
	cemu_assert_debug(firstDirty >= 0);
	const sint32 linesToLayout = std::min(firstDirty + count, (sint32)m_elements_visible);
	if (m_visibleAddresses.size() != m_elements_visible)
		m_visibleAddresses.resize(m_elements_visible);

	wxPoint at(0, 0);
	uint32 address = GetViewBaseAddress();
	sint32 line = 0;
	while (line < linesToLayout)
	{
		const uint32 current = address;
		address += 4;
		if (current < m_regionBegin || current >= m_regionEnd || !memory_isAddressRangeAccessible(current, 4))
		{
			m_visibleAddresses[line++] = std::nullopt;
			NextLine(at, &start_position);
			continue;
		}
		if (RPLStoredSymbol* sym = rplSymbolStorage_getByAddress(current))
		{
			if (line >= firstDirty)
				DrawSymbolLine(dc, at, sym);
			m_visibleAddresses[line++] = current;
			if (line >= linesToLayout)
				break;
			NextLine(at, &start_position);
		}
		m_visibleAddresses[line] = current;
		if (line >= firstDirty)
			DrawInstructionLine(dc, at, current, viewModule);
		NextLine(at, &start_position);
		line++;
	}

	// column separators: offsets | instructions | comments
	dc.SetPen(*wxLIGHT_GREY_PEN);
	const sint32 x1 = kColAddress + kColModuleOffset - 5;
	const sint32 x2 = x1 + kColInstruction;
	const sint32 yEnd = start_position.y + (count + 1) * m_line_height;
	dc.DrawLine(x1, start_position.y, x1, yEnd);
	dc.DrawLine(x2, start_position.y, x2, yEnd);
}

void DisasmCtrl::NotifyBreakpointsChanged()
{
	wxCommandEvent evt(wxEVT_BREAKPOINT_CHANGE);
	wxPostEvent(GetParent(), evt);
}

void DisasmCtrl::OnKeyPressed(sint32 key_code, const wxPoint& position)
{
	if (key_code == WXK_F9)
	{
		if (const auto address = LinePixelPosToAddress(position.y))
		{
			debugger_toggleExecuteBreakpoint(*address);
			NotifyBreakpointsChanged();
		}
		return;
	}
	if (key_code == 'G' && IsKeyDown(WXK_CONTROL))
	{
		GoToAddressDialog();
		return;
	}

	// run / step keys
	if (PPCInterpreter_t* cpu = debugger_lockDebugSession())
	{
		if (key_code == WXK_F5)
			debugger_stepCommand(DebuggerStepCommand::Run);
		else if (key_code == WXK_F10)
			debugger_stepCommand(DebuggerStepCommand::StepOver);
		else if (key_code == WXK_F11)
			debugger_stepCommand(DebuggerStepCommand::StepInto);
		debugger_unlockDebugSession(cpu);
	}
	else if (key_code == WXK_F5)
	{
		debugger_requestBreak();
	}
}

void DisasmCtrl::OnMouseDClick(const wxPoint& position, uint32 line)
{
	const auto hit = LinePixelPosToAddress(position.y - GetViewStart().y * m_line_height);
	if (!hit)
		return;
	const MPTR address = *hit;

	if (position.x <= kColAddress + kColModuleOffset)
	{
		debugger_toggleExecuteBreakpoint(address);
		RefreshLine(line);
		NotifyBreakpointsChanged();
		return;
	}

	if (position.x <= kColAddress + kColModuleOffset + kColInstruction)
	{
		// replace the instruction with one typed in by the user
		wxTextEntryDialog ask(this, _("Enter a new instruction."), wxString::Format(_("Overwrite instruction at address %08x"), address), wxEmptyString);
		if (ask.ShowModal() != wxID_OK)
			return;
		PPCAssemblerInOut ctx = {0};
		ctx.virtualAddress = address;
		if (ppcAssembler_assembleSingleInstruction(ask.GetValue().c_str(), &ctx))
		{
			debugger_createPatch(address, {ctx.outputData.data(), ctx.outputData.size()});
			RefreshLine(line);
		}
		return;
	}

	// comment column
	wxString current;
	const auto* note = static_cast<rplDebugSymbolComment*>(rplDebugSymbol_getForAddress(address));
	if (note != nullptr && note->type == RplDebugSymbolComment)
		current = note->comment;
	wxTextEntryDialog ask(this, _("Enter a new comment."), wxString::Format(_("Create comment at address %08x"), address), current);
	if (ask.ShowModal() == wxID_OK)
	{
		rplDebugSymbol_createComment(address, ask.GetValue().wc_str());
		RefreshLine(line);
	}
}

void DisasmCtrl::OnContextMenu(const wxPoint& position, uint32 /*line*/)
{
	const auto hit = LinePixelPosToAddress(position.y - GetViewStart().y * m_line_height);
	if (!hit)
		return;
	m_menuAddress = *hit;

	wxMenu menu;
	menu.Append(ID_MENU_TOGGLE_BP, _("Toggle breakpoint"));
	menu.Append(ID_MENU_TOGGLE_LOGPOINT, _("Toggle logging point"));
	if (debugger_hasPatch(m_menuAddress))
		menu.Append(ID_MENU_RESTORE_CODE, _("Restore original instructions"));
	menu.AppendSeparator();
	menu.Append(ID_MENU_COPY_ADDRESS, _("Copy address"));
	const uint32 imageAddress = ImageAddress(m_menuAddress);
	if (imageAddress != 0 && imageAddress != m_menuAddress)
		menu.Append(ID_MENU_COPY_IMAGE_ADDRESS, _("Copy virtual address (for IDA/Ghidra)"));
	PopupMenu(&menu);
}

void DisasmCtrl::OnMenuCommand(wxCommandEvent& event)
{
	switch (event.GetId())
	{
	case ID_MENU_TOGGLE_BP:
		debugger_toggleExecuteBreakpoint(m_menuAddress);
		NotifyBreakpointsChanged();
		break;
	case ID_MENU_TOGGLE_LOGPOINT:
		debugger_toggleLoggingBreakpoint(m_menuAddress);
		NotifyBreakpointsChanged();
		break;
	case ID_MENU_RESTORE_CODE:
		debugger_removePatch(m_menuAddress);
		NotifyBreakpointsChanged(); // also repaints the disassembly
		break;
	case ID_MENU_COPY_ADDRESS:
		PutOnClipboard(fmt::format("{:#10x}", m_menuAddress));
		break;
	case ID_MENU_COPY_IMAGE_ADDRESS:
		PutOnClipboard(fmt::format("{:#10x}", ImageAddress(m_menuAddress)));
		break;
	default:
		UNREACHABLE;
	}
}

bool DisasmCtrl::OnShowTooltip(const wxPoint& /*position*/, uint32 /*line*/)
{
	return false;
}

void DisasmCtrl::ScrollWindow(int dx, int dy, const wxRect* /*prect*/)
{
	// label lines shift addresses, so every scroll repaints the whole view
	RefreshControl(nullptr);
	TextList::ScrollWindow(dx, dy, nullptr);
}

wxSize DisasmCtrl::DoGetBestSize() const
{
	return TextList::DoGetBestSize();
}

void DisasmCtrl::OnUpdateView()
{
	RefreshControl();
}

uint32 DisasmCtrl::GetViewBaseAddress() const
{
	const sint32 topLine = GetViewStart().y;
	if (topLine < 0)
		return MPTR_NULL;
	return m_regionBegin + topLine * 4;
}

std::optional<MPTR> DisasmCtrl::LinePixelPosToAddress(sint32 posY)
{
	if (posY < 0)
		return std::nullopt;
	const size_t row = (size_t)(posY / m_line_height);
	if (row >= m_visibleAddresses.size())
		return std::nullopt;
	return m_visibleAddresses[row];
}

uint32 DisasmCtrl::AddressToScrollPos(uint32 offset) const
{
	return (offset - m_regionBegin) / 4;
}

void DisasmCtrl::CenterOffset(uint32 offset)
{
	if (offset < m_regionBegin || offset >= m_regionEnd)
		SelectCodeRegion(offset);

	const sint32 line = (sint32)AddressToScrollPos(offset);
	if (line < 0 || line >= (sint32)m_element_count)
		return;

	if (m_highlightLine != -1)
		RefreshLine(m_highlightLine);
	DoScroll(0, std::max(0, line - (sint32)(m_elements_visible / 2)));
	m_highlightLine = line;
	RefreshLine(m_highlightLine);
}

void DisasmCtrl::JumpTo(uint32 address)
{
	m_gotoTarget = address;
	CenterOffset(address);
	wxCommandEvent evt(wxEVT_DISASMCTRL_NOTIFY_GOTO_ADDRESS);
	evt.SetExtraLong(static_cast<long>(address));
	wxPostEvent(GetParent(), evt);
}

void DisasmCtrl::GoToAddressDialog()
{
	wxTextEntryDialog ask(this, _("Enter a target address."), _("GoTo address"), wxEmptyString);
	if (ask.ShowModal() != wxID_OK)
		return;

	std::string input = ask.GetValue().ToStdString();
	std::transform(input.begin(), input.end(), input.begin(), tolower);
	input.erase(0, input.find_first_not_of(' '));

	ExpressionParser parser;
	debugger_addParserSymbols(parser);
	// a bare number is read as hex ("1234" means 0x1234); otherwise the text is an expression
	const std::string asHex = "0x" + input;
	if (parser.IsConstantExpression(asHex))
	{
		JumpTo((uint32)parser.Evaluate(asHex));
		return;
	}
	if (parser.IsConstantExpression(input))
	{
		JumpTo((uint32)parser.Evaluate(input));
		return;
	}
	// not constant: evaluating reports what is wrong with it
	try
	{
		(void)parser.Evaluate(input);
	}
	catch (const std::exception& ex)
	{
		wxMessageBox(ex.what(), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
	}
}
