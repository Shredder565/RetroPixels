// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/panels/InputPanel.h"

#include <wx/gbsizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wupdlock.h>

#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/components/wxInputDraw.h"

namespace
{
	// ==== capturepick begin ====
	// A diagonal stick push reports two directions at once. While binding, a
	// direction only counts if it is pushed at least as far as the crossing
	// direction of the same stick (axis, rotation or trigger).
	struct StickDir
	{
		int stick;  // 0 axis, 1 rotation, 2 trigger
		bool vertical;
	};

	bool StickDirOf(uint32 id, StickDir& out)
	{
		switch (id)
		{
		case kAxisXP: case kAxisXN: out = {0, false}; return true;
		case kAxisYP: case kAxisYN: out = {0, true}; return true;
		case kRotationXP: case kRotationXN: out = {1, false}; return true;
		case kRotationYP: case kRotationYN: out = {1, true}; return true;
		case kTriggerXP: case kTriggerXN: out = {2, false}; return true;
		case kTriggerYP: case kTriggerYN: out = {2, true}; return true;
		default: return false;
		}
	}

	// the two button ids of the crossing direction of a stick
	void CrossingIds(const StickDir& dir, uint32& a, uint32& b)
	{
		static constexpr uint32 kHorizontal[3][2] = {{kAxisXP, kAxisXN}, {kRotationXP, kRotationXN}, {kTriggerXP, kTriggerXN}};
		static constexpr uint32 kVertical[3][2] = {{kAxisYP, kAxisYN}, {kRotationYP, kRotationYN}, {kTriggerYP, kTriggerYN}};
		const auto& pair = dir.vertical ? kHorizontal[dir.stick] : kVertical[dir.stick];
		a = pair[0];
		b = pair[1];
	}

	// sticks = { axis, rotation, trigger }
	template<typename TButtons, typename TVec>
	bool OvershadowedByCrossing(uint32 id, const TButtons& buttons, const TVec (&sticks)[3])
	{
		StickDir dir;
		if (!StickDirOf(id, dir))
			return false;
		uint32 crossA, crossB;
		CrossingIds(dir, crossA, crossB);
		if (!buttons.GetButtonState(crossA) && !buttons.GetButtonState(crossB))
			return false;
		const auto& v = sticks[dir.stick];
		const float own = std::abs(dir.vertical ? v.y : v.x);
		const float cross = std::abs(dir.vertical ? v.x : v.y);
		return cross > own;
	}
	// ==== capturepick end ====

	constexpr float kMinAnalogBindValue = 0.33f;

	// Shared by every panel: after a binding, input has to return to idle
	// before the next field accepts anything.
	bool s_awaitRelease = false;
}

InputPanel::InputPanel(wxWindow* parent)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER | wxWANTS_CHARS)
{
	Bind(wxEVT_LEFT_UP, &InputPanel::on_left_click, this);
}

template<typename TFunc>
void InputPanel::ForEachField(TFunc&& func)
{
	for (wxWindow* child : GetChildren())
	{
		if (auto* field = dynamic_cast<wxTextCtrl*>(child))
			func(field);
	}
}

wxTextCtrl* InputPanel::ArmedField()
{
	if (m_armedId == wxID_NONE)
		return nullptr;
	return dynamic_cast<wxTextCtrl*>(FindWindow(m_armedId));
}

void InputPanel::PaintField(wxTextCtrl* field, const wxColour& colour)
{
	field->SetBackgroundColour(colour);
	field->Refresh();
}

void InputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	// always poll so the controller state stays current
	const auto& state = controller->update_state();

	wxTextCtrl* field = ArmedField();
	if (!field)
		return;
	const uint64 mapping = reinterpret_cast<uint64>(field->GetClientData());

	const bool clear = std::exchange(m_clearRequested, false);
	if (clear || WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::ESCAPE))
	{
		emulated_controller->delete_mapping(mapping);
		field->SetBackgroundColour(kKeyColourNormalMode);
		m_savedColour[field->GetId()] = kKeyColourNormalMode;
		// first press empties the field, a second one leaves it
		if (field->IsEmpty())
			DisarmCurrent();
		else
			field->SetValue(wxEmptyString);
		return;
	}

	if (state.buttons.IsIdle())
	{
		s_awaitRelease = false;
		return;
	}
	if (s_awaitRelease)
		return;
	s_awaitRelease = true;

	const decltype(state.axis) sticks[3] = {state.axis, state.rotation, state.trigger};
	const bool analog = controller->has_axis();
	for (const uint32 id : state.buttons.GetButtonList())
	{
		if (analog)
		{
			if (OvershadowedByCrossing(id, state.buttons, sticks))
				continue;
			if (id >= kButtonAxisStart && controller->get_axis_value(id) < kMinAnalogBindValue)
			{
				// barely touched, keep waiting on this field
				cemuLog_logDebug(LogType::Force, "input binding ignored, axis value {} too small", controller->get_axis_value(id));
				s_awaitRelease = false;
				return;
			}
		}
		emulated_controller->set_mapping(mapping, controller, id);
		field->SetValue(controller->get_button_name(id));
		field->SetBackgroundColour(kKeyColourNormalMode);
		m_savedColour[field->GetId()] = kKeyColourNormalMode;
		break;
	}

	if (wxTextCtrl* next = get_next_sibling(field))
	{
		next->SetFocus();
	}
	else
	{
		DisarmCurrent();
		SetFocusIgnoringChildren();
	}
}

void InputPanel::reset_configuration()
{
	m_savedColour.clear();
	wxWindowUpdateLocker freeze(this);
	ForEachField([this](wxTextCtrl* field) {
		field->SetValue(wxEmptyString);
		PaintField(field, kKeyColourNormalMode);
	});
}

void InputPanel::reset_colours()
{
	m_savedColour.clear();
	wxWindowUpdateLocker freeze(this);
	ForEachField([this](wxTextCtrl* field) { PaintField(field, kKeyColourNormalMode); });
}

void InputPanel::load_controller(const EmulatedControllerPtr& controller)
{
	reset_configuration();
	if (!controller || controller->get_controllers().empty())
		return;

	wxWindowUpdateLocker freeze(this);
	ForEachField([&controller](wxTextCtrl* field) {
		const auto mapping = reinterpret_cast<sint64>(field->GetClientData());
		if (mapping > 0)
			field->ChangeValue(controller->get_mapping_name(mapping));
	});
}

void InputPanel::set_selected_controller(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	wxWindowUpdateLocker freeze(this);
	ForEachField([&](wxTextCtrl* field) {
		if (field->GetId() == m_armedId)
			return;
		const auto mapping = reinterpret_cast<sint64>(field->GetClientData());
		if (mapping <= 0)
			return;
		const auto source = emulated_controller->get_mapping_controller(mapping);
		if (!source)
			return;
		PaintField(field, *source == *controller ? kKeyColourNormalMode : kKeyColourActiveMode);
	});
}

void InputPanel::MakeMappingField(wxTextCtrl* field, uint64 mapping)
{
	field->SetClientData(reinterpret_cast<void*>(mapping));
	field->SetMinSize(wxSize(150, -1));
	field->SetEditable(false);
	field->SetBackgroundColour(kKeyColourNormalMode);
	field->Bind(wxEVT_SET_FOCUS, &InputPanel::Arm, this);
	field->Bind(wxEVT_KILL_FOCUS, &InputPanel::Disarm, this);
	field->Bind(wxEVT_RIGHT_DOWN, &InputPanel::OnFieldRightDown, this);
#if BOOST_OS_LINUX
	// swallow key presses so the arrow keys do not move between fields
	field->Bind(wxEVT_KEY_DOWN, [](wxKeyEvent&) {});
#endif
}

wxTextCtrl* InputPanel::AddMappingRow(wxGridBagSizer* sizer, int row, int column, uint64 mapping, const wxString& label)
{
	sizer->Add(new wxStaticText(this, wxID_ANY, label), wxGBPosition(row, column), wxDefaultSpan, wxALL | wxALIGN_CENTER_VERTICAL, 5);
	auto* field = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxTE_PROCESS_ENTER | wxTE_PROCESS_TAB);
	MakeMappingField(field, mapping);
	sizer->Add(field, wxGBPosition(row, column + 1), wxDefaultSpan, wxALL | wxEXPAND, 5);
	return field;
}

void InputPanel::AddHeading(wxGridBagSizer* sizer, int row, int column, const wxString& text)
{
	auto* heading = new wxStaticText(this, wxID_ANY, text);
	wxFont font = GetFont();
	font.MakeBold();
	heading->SetFont(font);
	sizer->Add(heading, wxGBPosition(row, column), wxGBSpan(1, 3), wxALL | wxEXPAND, 5);
}

void InputPanel::AddSeparator(wxGridBagSizer* sizer, int column, int flags)
{
	sizer->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVERTICAL), wxGBPosition(0, column), wxGBSpan(11, 1), flags, 5);
}

wxInputDraw* InputPanel::AddStickPreview(wxGridBagSizer* sizer, int row, int column)
{
	auto* preview = new wxInputDraw(this, wxID_ANY, wxDefaultPosition, {60, 60});
	sizer->Add(preview, wxGBPosition(row, column), wxGBSpan(2, 1), wxTOP | wxBOTTOM | wxEXPAND | wxALIGN_CENTER, 5);
	return preview;
}

void InputPanel::on_left_click(wxMouseEvent& event)
{
	// clicking the panel background disarms the current field
	if (m_armedId == wxID_NONE)
		return;
	wxWindow* armed = FindWindow(m_armedId);
	if (armed)
	{
		wxFocusEvent lost(wxEVT_KILL_FOCUS, m_armedId);
		lost.SetWindow(armed);
		armed->GetEventHandler()->ProcessEvent(lost);
	}
	SetFocusIgnoringChildren();
}

void InputPanel::Arm(wxFocusEvent& event)
{
	auto* field = dynamic_cast<wxTextCtrl*>(event.GetEventObject());
	if (field)
	{
		m_savedColour[field->GetId()] = field->GetBackgroundColour();
		field->SetBackgroundColour(kKeyColourEditMode);
#if BOOST_OS_WINDOWS
		field->HideNativeCaret();
#endif
		field->Refresh();
		m_armedId = field->GetId();
	}
	event.Skip();
}

void InputPanel::Disarm(wxFocusEvent& event)
{
	DisarmCurrent();
	event.Skip();
}

void InputPanel::OnFieldRightDown(wxMouseEvent& event)
{
	// handled on the next poll; focusing first arms the clicked field
	m_clearRequested = true;
	if (m_armedId != wxID_NONE)
		return;
	if (auto* field = dynamic_cast<wxTextCtrl*>(event.GetEventObject()))
		field->SetFocus();
}

bool InputPanel::DisarmCurrent()
{
	if (m_armedId == wxID_NONE)
		return false;
	if (wxTextCtrl* field = ArmedField())
	{
		const auto saved = m_savedColour.find(field->GetId());
		field->SetBackgroundColour(saved != m_savedColour.end() ? saved->second : kKeyColourNormalMode);
#if BOOST_OS_WINDOWS
		field->HideNativeCaret();
#endif
		field->Refresh();
	}
	m_armedId = wxID_NONE;
	return true;
}
