// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// A controller provider is the connection to one host input API (SDL, XInput, a DSU
// server, ...). It lists the controllers it can see; the controllers themselves read
// their state through it. Providers that need configuration (the DSU server address)
// derive from ControllerProvider<TSettings>, and InputManager keeps one provider per
// distinct settings value.

#include "input/api/InputAPI.h"

class ControllerBase;

struct ControllerProviderSettings
{
	virtual ~ControllerProviderSettings() = default;
	virtual bool operator==(const ControllerProviderSettings&) const = 0;
};

class ControllerProviderBase
{
public:
	ControllerProviderBase() = default;
	virtual ~ControllerProviderBase() = default;

	virtual InputAPI::Type api() const = 0;
	std::string_view api_name() const { return to_string(api()); }

	virtual std::vector<std::shared_ptr<ControllerBase>> get_controllers() = 0;

	virtual bool has_settings() const { return false; }

	// providers without settings are interchangeable when they talk to the same API
	virtual bool operator==(const ControllerProviderBase& p) const { return api() == p.api(); }
	// does this provider use exactly these settings?
	virtual bool operator==(const ControllerProviderSettings& p) const { return false; }
};

using ControllerProviderPtr = std::shared_ptr<ControllerProviderBase>;

template <typename TSettings>
class ControllerProvider : public ControllerProviderBase
{
public:
	ControllerProvider() = default;
	ControllerProvider(const TSettings& settings) : m_settings(settings) {}

	bool has_settings() const override { return true; }
	const TSettings& get_settings() const { return m_settings; }

	bool operator==(const ControllerProviderBase& p) const override
	{
		if (api() != p.api() || !p.has_settings())
			return false;
		const auto* other = dynamic_cast<const ControllerProvider<TSettings>*>(&p);
		return other && m_settings == other->m_settings;
	}

	bool operator==(const ControllerProviderSettings& p) const override
	{
		const auto* settings = dynamic_cast<const TSettings*>(&p);
		return settings && m_settings == *settings;
	}

protected:
	TSettings m_settings{};
};
