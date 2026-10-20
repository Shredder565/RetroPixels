// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// One DirectInput device, identified by its instance GUID. Profiles also store the
// product GUID, so the same model plugged into another port is found again.

#include "input/api/DirectInput/DirectInputControllerProvider.h"
#include "input/api/Controller.h"
#include <wrl/client.h>

class DirectInputController : public Controller<DirectInputControllerProvider>
{
public:
	DirectInputController(const GUID& guid);
	DirectInputController(const GUID& guid, std::string_view display_name);
	~DirectInputController() override;

	std::string_view api_name() const override { return to_string(InputAPI::DirectInput); } // "DirectInput"
	InputAPI::Type api() const override { return InputAPI::DirectInput; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	bool connect() override;
	bool is_connected() override;

	// a constant force effect exists (only with exclusive access)
	bool has_rumble() override;

	void start_rumble() override;
	void stop_rumble() override;

	std::string get_button_name(uint64 button) const override;

	const GUID& get_guid() const { return m_guid; }
	const GUID& get_product_guid() const { return m_product_guid; }

protected:
	ControllerState raw_state() override;

private:
	void create_rumble_effect();
	void read_axis_ranges();

	GUID m_guid;
	GUID m_product_guid{};

	std::shared_mutex m_mutex;
	Microsoft::WRL::ComPtr<IDirectInputDevice8W> m_device;
	Microsoft::WRL::ComPtr<IDirectInputEffect> m_effect;

	// range of X, Y, Z, RX, RY, RZ as the device reports them
	std::array<LONG, 6> m_min_axis{};
	std::array<LONG, 6> m_max_axis{};
};
