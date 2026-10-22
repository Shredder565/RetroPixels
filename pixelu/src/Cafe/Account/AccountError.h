// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <system_error>

// Errors reported while loading, creating or saving an account (account.dat)
enum class AccountErrc
{
	NoError = 0,
	ParseError,
	InvalidPersistentId,
	InvalidUuid,
	InvalidMiiName,
	InvalidMiiData,
};

namespace std
{
	template<>
	struct is_error_code_enum<AccountErrc> : true_type
	{
	};
}

namespace detail
{
	class AccountErrorCategory final : public std::error_category
	{
	  public:
		const char* name() const noexcept override { return "AccountError"; }

		std::string message(int value) const override
		{
			switch (static_cast<AccountErrc>(value))
			{
			case AccountErrc::NoError: return "no error";
			case AccountErrc::ParseError: return "account file could not be parsed";
			case AccountErrc::InvalidPersistentId: return "invalid persistent id";
			case AccountErrc::InvalidUuid: return "invalid uuid";
			case AccountErrc::InvalidMiiName: return "invalid mii name";
			case AccountErrc::InvalidMiiData: return "invalid mii data";
			}
			return "unknown account error";
		}

		std::error_condition default_error_condition(int value) const noexcept override
		{
			if (static_cast<AccountErrc>(value) == AccountErrc::InvalidPersistentId)
				return std::make_error_condition(std::errc::invalid_argument);
			return std::error_condition(value, *this);
		}
	};
}

inline std::error_code make_error_code(AccountErrc e)
{
	static const detail::AccountErrorCategory s_category;
	return {static_cast<int>(e), s_category};
}
