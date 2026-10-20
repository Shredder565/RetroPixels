// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui.h"
#include "AudioDebuggerWindow.h"

#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"

namespace
{
	struct Column
	{
		const char* title;
		int width;
	};

	// one row per voice; the voice index column comes first
	const Column kColumns[] = {
		{"idx", 40}, {"state", 48}, {"fmt", 52},
		{"base", 70}, {"current", 70}, {"loop", 70}, {"end", 70},
		{"vol", 46}, {"volD", 46}, {"src", 70},
		{"lpa0", 46}, {"lpb0", 46},
		{"bqb0", 46}, {"bqb1", 46}, {"bqb2", 46}, {"bqa1", 46}, {"bqa2", 46},
		{"deviceMix", 186},
	};
	constexpr int kColumnCount = (int)std::size(kColumns);

	enum ColumnIndex
	{
		ColIndex, ColState, ColFormat,
		ColBase, ColCurrent, ColLoop, ColEnd,
		ColVolume, ColVolumeDelta, ColSrc,
		ColLpfA0, ColLpfB0,
		ColBiquadB0, ColBiquadB1, ColBiquadB2, ColBiquadA1, ColBiquadA2,
		ColDeviceMix,
	};

	constexpr int kRefreshTimerId = 3;
	constexpr int kRefreshMs = 100;

	wxString Hex32(uint32 v) { return wxString::Format("%08x", v); }
	wxString Hex16(uint16 v) { return wxString::Format("%04x", (unsigned)v); }

	wxString FormatName(uint16 formatBE)
	{
		if (formatBE == _swapEndianU16(snd_core::AX_FORMAT_ADPCM))
			return "adpcm";
		if (formatBE == _swapEndianU16(snd_core::AX_FORMAT_PCM16))
			return "pcm16";
		if (formatBE == _swapEndianU16(snd_core::AX_FORMAT_PCM8))
			return "pcm8";
		return "ukn";
	}
}

AudioDebuggerWindow::AudioDebuggerWindow(wxFrame& parent)
	: wxFrame(&parent, wxID_ANY, _("AX voice viewer"), wxDefaultPosition, wxSize(1126, 580), wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER)
{
	auto* panel = new wxPanel(this);
	auto* layout = new wxBoxSizer(wxVERTICAL);

	voiceListbox = new wxListCtrl(panel, wxID_ANY, wxPoint(0, 0), wxSize(1126, 570), wxLC_REPORT);
	voiceListbox->SetFont(wxFont(8, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Courier New"));
	for (int c = 0; c < kColumnCount; ++c)
	{
		wxListItem column;
		column.SetId(c);
		column.SetText(kColumns[c].title);
		column.SetWidth(kColumns[c].width);
		voiceListbox->InsertColumn(c, column);
	}
	layout->Add(voiceListbox, 1, wxEXPAND | wxBOTTOM, 0);
	voiceListbox->Bind(wxEVT_RIGHT_DOWN, &AudioDebuggerWindow::OnVoiceListRightClick, this);
	panel->SetSizer(layout);

	// rows are listed from the highest voice index down
	for (sint32 row = 0; row < snd_core::AX_MAX_VOICES; ++row)
	{
		wxListItem item;
		item.SetId(row);
		item.SetText(wxString::Format("%d", snd_core::AX_MAX_VOICES - 1 - row));
		voiceListbox->InsertItem(item);
	}
	RefreshVoiceList();

	Bind(wxEVT_TIMER, &AudioDebuggerWindow::OnRefreshTimer, this, kRefreshTimerId);
	Bind(wxEVT_CLOSE_WINDOW, &AudioDebuggerWindow::OnClose, this);
	refreshTimer = new wxTimer(this, kRefreshTimerId);
	refreshTimer->Start(kRefreshMs);
}

void AudioDebuggerWindow::OnRefreshTimer(wxTimerEvent&)
{
	RefreshVoiceList();
}

void AudioDebuggerWindow::OnCloseButton(wxCommandEvent&)
{
	Close();
}

void AudioDebuggerWindow::OnRefreshButton(wxCommandEvent&)
{
	RefreshVoiceList();
}

void AudioDebuggerWindow::OnClose(wxCloseEvent&)
{
	Close();
}

void AudioDebuggerWindow::RefreshVoiceList_sndgeneric()
{
	using namespace snd_core;
	if (__AXVPBInternalVoiceArray == nullptr || __AXVPBArrayPtr == nullptr)
		return;

	// snapshot of the public voice blocks (the game keeps changing them)
	AXVPB voices[AX_MAX_VOICES];
	memcpy(voices, __AXVPBArrayPtr, sizeof(voices));

	voiceListbox->Freeze();
	for (sint32 row = 0; row < AX_MAX_VOICES; ++row)
	{
		const sint32 voice = AX_MAX_VOICES - 1 - row;
		const AXVPBInternal_t& state = __AXVPBInternalVoiceArray[voice];
		auto set = [&](int column, const wxString& text) { voiceListbox->SetItem(row, column, text); };

		const bool playing = _swapEndianU16(state.playbackState) != 0;
		if (!playing)
		{
			for (int c = 0; c < kColumnCount; ++c)
				set(c, "");
			continue;
		}
		set(ColIndex, wxString::Format("%d", (sint32)voices[voice].index));
		set(ColState, "on");
		set(ColFormat, FormatName(voices[voice].offsets.format));

		AXPBOFFSET_t offsets;
		AXGetVoiceOffsets(voices + voice, &offsets);
		set(ColBase, Hex32(_swapEndianU32(offsets.samples)));
		set(ColCurrent, Hex32(_swapEndianU32(offsets.currentOffset)));
		set(ColLoop, offsets.loopFlag ? Hex32(_swapEndianU32(offsets.loopOffset)) : wxString());
		set(ColEnd, Hex32(_swapEndianU32(offsets.endOffset)));

		set(ColVolume, Hex16((uint16)state.veVolume));
		set(ColVolumeDelta, Hex16((uint16)state.veDelta));
		set(ColSrc, Hex16(_swapEndianU16(state.src.ratioHigh)) + Hex16(_swapEndianU16(state.src.ratioLow)));

		const bool lpf = state.lpf.on != 0;
		set(ColLpfA0, lpf ? Hex16(_swapEndianU16(state.lpf.a0)) : wxString());
		set(ColLpfB0, lpf ? Hex16(_swapEndianU16(state.lpf.b0)) : wxString());

		const bool biquad = state.biquad.on != 0;
		set(ColBiquadB0, biquad ? Hex16(_swapEndianU16(state.biquad.b0)) : wxString());
		set(ColBiquadB1, biquad ? Hex16(_swapEndianU16(state.biquad.b1)) : wxString());
		set(ColBiquadB2, biquad ? Hex16(_swapEndianU16(state.biquad.b2)) : wxString());
		set(ColBiquadA1, biquad ? Hex16(_swapEndianU16(state.biquad.a1)) : wxString());
		set(ColBiquadA2, biquad ? Hex16(_swapEndianU16(state.biquad.a2)) : wxString());

		// TV mix: one hex digit (volume rounded up to 1/16 steps) per channel and bus
		wxString mix;
		for (uint32 channel = 0; channel < AX_TV_CHANNEL_COUNT; ++channel)
		{
			for (uint32 bus = 0; bus < AX_MAX_NUM_BUS; ++bus)
			{
				const uint32 volume = state.deviceMixTV[channel * 4 + bus].vol;
				mix += wxString::Format("%x", (volume + 0x0FFF) >> 12);
			}
		}
		set(ColDeviceMix, mix);
	}
	voiceListbox->Thaw();
}

void AudioDebuggerWindow::RefreshVoiceList()
{
	RefreshVoiceList_sndgeneric();
}

void AudioDebuggerWindow::OnVoiceListPopupClick(wxCommandEvent&)
{
}

void AudioDebuggerWindow::OnVoiceListRightClick(wxMouseEvent&)
{
}

void AudioDebuggerWindow::Close()
{
	refreshTimer->Stop();
	Destroy();
}
