// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <string>
#include <string_view>
#include <system_error>

// Why an account cannot be used online
enum class OnlineAccountError
{
	kNone,
	kNoAccountId,
	kNoPasswordCached,
	kPasswordCacheEmpty,
	kNoPrincipalId,
};

// Result of checking the files online play needs
struct OnlineValidator
{
	enum class FileState
	{
		Missing,
		Corrupted,
		Ok,
	};

	bool valid_account = false;
	FileState otp = FileState::Missing;
	FileState seeprom = FileState::Missing;
	std::vector<std::wstring> missing_files;
	OnlineAccountError account_error = OnlineAccountError::kNone;

	// usable online only with the account data, both key files and nothing missing
	bool IsValid() const { return valid_account && missing_files.empty() && otp == FileState::Ok && seeprom == FileState::Ok; }

	explicit operator bool() const { return IsValid(); }
};

// A console user account, stored as mlc01/usr/save/system/act/<persistent id>/account.dat
class Account
{
  public:
	static constexpr uint32 kMinPersistendId = 0x80000001;

	// creates a new account with a random uuid and a default Mii
	Account(uint32 persistent_id, std::wstring_view mii_name);
	// loads an account from an account.dat file
	Account(std::wstring_view file_name);

	std::error_code Load(); // reads GetFileName() into this object
	std::error_code Save(); // writes this object to GetFileName()

	[[nodiscard]] std::wstring ToString() const { return fmt::format(L"{} ({:x})", GetMiiName(), GetPersistentId()); }

	[[nodiscard]] bool IsValidOnlineAccount() const;
	[[nodiscard]] OnlineAccountError GetOnlineAccountError() const;
	[[nodiscard]] fs::path GetFileName() const;

	[[nodiscard]] uint32 GetPersistentId() const { return m_persistentId; }
	[[nodiscard]] uint64 GetTransferableIdBase() const { return m_transferableIdBase; }
	[[nodiscard]] const std::array<uint8, 16>& GetUuid() const { return m_uuid; }
	[[nodiscard]] const std::array<uint8, 96>& GetMiiData() const { return m_miiData; }
	[[nodiscard]] std::wstring_view GetMiiName() const; // at most 10 characters
	[[nodiscard]] std::string_view GetAccountId() const { return m_accountId; }
	[[nodiscard]] uint16 GetBirthYear() const { return m_birthYear; }
	[[nodiscard]] uint8 GetBirthMonth() const { return m_birthMonth; }
	[[nodiscard]] uint8 GetBirthDay() const { return m_birthDay; }
	[[nodiscard]] uint8 GetGender() const { return m_gender; }
	[[nodiscard]] std::string_view GetEmail() const { return m_email; }
	[[nodiscard]] uint32 GetCountry() const { return m_country; }
	[[nodiscard]] uint32 GetSimpleAddressId() const { return m_simpleAddressId; }
	[[nodiscard]] std::string_view GetTimeZoneId() const { return m_timeZoneId; }
	[[nodiscard]] sint64 GetUtcOffset() const { return m_utcOffset; }
	[[nodiscard]] uint32 GetPrincipalId() const { return m_principalId; }
	[[nodiscard]] bool IsPasswordCacheEnabled() const { return m_passwordCacheEnabled != 0; }
	[[nodiscard]] const std::array<uint8, 32>& GetAccountPasswordCache() const { return m_passwordCache; }

	// value of a key from account.dat that has no dedicated field
	[[nodiscard]] std::string_view GetStorageValue(std::string_view key) const;

	void SetMiiName(std::wstring_view name);
	void SetBirthYear(uint16 birth_year) { m_birthYear = birth_year; }
	void SetBirthMonth(uint8 birth_month) { m_birthMonth = birth_month; }
	void SetBirthDay(uint8 birth_day) { m_birthDay = birth_day; }
	void SetGender(uint8 gender) { m_gender = gender; }
	void SetEmail(std::string_view email) { m_email = email; }
	void SetCountry(uint32 country) { m_country = country; }
	void SetTimeZoneId(std::string_view timezone_id) { m_timeZoneId = timezone_id; }
	void SetUtcOffset(sint64 utc_offset) { m_utcOffset = utc_offset; }

	// rescans the mlc for accounts; creates and saves a default account if there is none
	static const std::vector<Account>& RefreshAccounts();
	// writes persisid.dat with the highest persistent id in use
	static void UpdatePersisidDat();

	[[nodiscard]] static bool HasFreeAccountSlots();
	[[nodiscard]] static const std::vector<Account>& GetAccounts();
	[[nodiscard]] static const Account& GetAccount(uint32 persistent_id); // falls back to the first account
	[[nodiscard]] static const Account& GetCurrentAccount();
	[[nodiscard]] static uint32 GetNextPersistentId();
	[[nodiscard]] static fs::path GetFileName(uint32 persistent_id);
	[[nodiscard]] OnlineValidator ValidateOnlineFiles() const;

  private:
	explicit Account(uint32 persistent_id);

	[[nodiscard]] std::error_code CheckValid() const;
	void ParseFile(class FileStream* file);
	void ApplyField(std::string_view key, std::string_view value);
	void CreateDefaultMii();

	uint32 m_persistentId = 0;
	uint64 m_transferableIdBase = 0;
	std::array<uint8, 16> m_uuid{};
	std::array<uint8, 96> m_miiData{};
	std::array<wchar_t, 11> m_miiName{}; // NUL terminated
	std::string m_accountId;
	uint16 m_birthYear = 0;
	uint8 m_birthMonth = 0;
	uint8 m_birthDay = 0;
	uint8 m_gender = 0;
	std::string m_email;
	uint32 m_country = 0;
	uint32 m_simpleAddressId = 0;
	std::string m_timeZoneId;
	sint64 m_utcOffset = 0;
	uint32 m_principalId = 0;
	uint8 m_passwordCacheEnabled = 0;
	std::array<uint8, 32> m_passwordCache{};

	// keys of account.dat we do not interpret, written back unchanged on Save()
	std::unordered_map<std::string, std::string> m_otherFields;

	static std::vector<Account> s_accounts;
};
