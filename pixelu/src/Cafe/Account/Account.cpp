// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Account.h"
#include "AccountError.h"
#include "util/helpers/helpers.h"
#include "util/helpers/StringHelpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Common/FileStream.h"

#include <random>

// account.dat is a text file: a header line ("AccountInstance_20120705")
// followed by Key=Value lines. Numbers are hexadecimal, binary blobs are hex
// strings and the Mii name is 11 UTF-16 code units as 4-digit hex each.

std::vector<Account> Account::s_accounts;

namespace
{
	constexpr size_t kMaxAccounts = 12;
	constexpr std::string_view kAccountFileHeader = "AccountInstance_20120705";
	constexpr std::string_view kPersisIdHeader = "PersistentIdManager_20120607";
	constexpr std::string_view kPersisIdHeadKey = "PersistentIdHead=";

	fs::path PersisIdFilePath()
	{
		return ActiveSettings::GetMlcPath("usr/save/system/act/persisid.dat");
	}

	// Hex number with optional leading whitespace; anything unparsable is 0.
	// (Whitespace-only text is handled here because ConvertString would read
	// past the end of such a view.)
	template<typename T>
	T ParseHex(std::string_view text)
	{
		const size_t start = text.find_first_not_of("\t\n\v\f\r ");
		if (start == std::string_view::npos)
			return 0;
		return ConvertString<T>(text.substr(start), 16);
	}

	// Decodes a string of 2*N hex digits into N bytes. Invalid digit pairs decode to 0.
	template<size_t N>
	void DecodeHexBytes(std::string_view hex, std::array<uint8, N>& out)
	{
		for (size_t i = 0; i < N; i++)
			out[i] = ParseHex<uint8>(hex.substr(i * 2, 2));
	}

	template<size_t N>
	std::string EncodeHexBytes(const std::array<uint8, N>& bytes)
	{
		std::string s;
		s.reserve(N * 2);
		for (uint8 b : bytes)
			s.append(fmt::format("{:02x}", b));
		return s;
	}

	// CRC-16/CCITT over the Mii data. A valid Mii block checksums to zero
	// (its last two bytes hold the CRC of the preceding bytes).
	uint16 MiiChecksum(const uint8* data, size_t size)
	{
		uint16 crc = 0;
		for (size_t i = 0; i < size; i++)
		{
			for (int bit = 0; bit < 8; bit++)
				crc = (crc & 0x8000) ? (uint16)((crc << 1) ^ 0x1021) : (uint16)(crc << 1);
			crc ^= data[i];
		}
		return crc;
	}

	void PutLE16(uint8* p, uint16 v)
	{
		p[0] = (uint8)v;
		p[1] = (uint8)(v >> 8);
	}

	void PutBE32(uint8* p, uint32 v)
	{
		p[0] = (uint8)(v >> 24);
		p[1] = (uint8)(v >> 16);
		p[2] = (uint8)(v >> 8);
		p[3] = (uint8)v;
	}
}

Account::Account(uint32 persistent_id)
	: m_persistentId(persistent_id)
{
}

Account::Account(uint32 persistent_id, std::wstring_view mii_name)
	: m_persistentId(persistent_id)
{
	if (mii_name.empty())
		throw std::system_error(AccountErrc::InvalidMiiName);

	static std::mt19937 s_rng{std::random_device{}()};
	std::uniform_int_distribution<uint32> byteDist(0, 255);
	for (uint8& b : m_uuid)
		b = (uint8)byteDist(s_rng);

	// transferable id base: fixed upper half, lower half from the last four uuid bytes
	m_transferableIdBase = (0x2000004ull << 32) | ((uint64)m_uuid[12] << 24) | ((uint64)m_uuid[13] << 16) | ((uint64)m_uuid[14] << 8) | (uint64)m_uuid[15];

	SetMiiName(mii_name);
	CreateDefaultMii();

	if (std::error_code ec = CheckValid())
		throw std::system_error(ec);
}

// Builds a placeholder Mii (FFL data, 16-bit words stored little endian) that
// carries the account's name and a valid checksum.
void Account::CreateDefaultMii()
{
	uint8* mii = m_miiData.data();
	for (size_t offset = 0; offset < m_miiData.size(); offset += 2)
		PutLE16(mii + offset, 0x0001);
	PutLE16(mii + 0x02, 0x1001);
	PutLE16(mii + 0x08, 0x0003); // Mii id, low part
	PutLE16(mii + 0x0A, 0x3334);
	PutLE16(mii + 0x3A, 0x0601);

	// name: 10 UTF-16LE code units at +0x1A
	constexpr size_t kNameOffset = 0x1A;
	constexpr size_t kNameChars = 10;
	std::memset(mii + kNameOffset, 0, kNameChars * 2);
	std::wstring_view name = GetMiiName();
	for (size_t i = 0; i < name.size() && i < kNameChars; i++)
		PutLE16(mii + kNameOffset + i * 2, (uint16)name[i]);

	// search the 32-bit value at +4 (big endian) that makes the block checksum to zero
	for (uint32 salt = 0; MiiChecksum(mii, m_miiData.size()) != 0; salt++)
		PutBE32(mii + 4, salt);
}

Account::Account(std::wstring_view file_name)
{
	const fs::path path(file_name);
	if (!fs::exists(path))
		throw std::runtime_error("account file not found");
	std::unique_ptr<FileStream> file(FileStream::openFile2(path));
	if (!file)
		throw std::runtime_error("account file could not be opened");
	ParseFile(file.get());
	if (std::error_code ec = CheckValid())
		throw std::system_error(ec);
}

std::error_code Account::CheckValid() const
{
	if (m_persistentId < kMinPersistendId)
		return AccountErrc::InvalidPersistentId;
	if (m_miiName[0] == L'\0')
		return AccountErrc::InvalidMiiName;
	if (std::all_of(m_miiData.begin(), m_miiData.end(), [](uint8 b) { return b == 0; }))
		return AccountErrc::InvalidMiiData;
	return AccountErrc::NoError;
}

std::error_code Account::Load()
{
	// the folder name decides the persistent id, not the PersistentId key
	const uint32 folderId = m_persistentId;
	try
	{
		std::unique_ptr<FileStream> file(FileStream::openFile2(GetFileName()));
		if (!file)
			throw std::runtime_error("account file could not be opened");
		ParseFile(file.get());
		m_persistentId = folderId;
		return CheckValid();
	}
	catch (const std::system_error& e)
	{
		return e.code();
	}
	catch (const std::exception& e)
	{
		cemuLog_log(LogType::Force, "Account: loading account {:08x} failed: {}", folderId, e.what());
		return AccountErrc::ParseError;
	}
}

void Account::ParseFile(FileStream* file)
{
	std::vector<uint8> content(file->GetSize());
	if (file->readData(content.data(), (uint32)content.size()) != content.size())
		throw std::system_error(AccountErrc::ParseError);
	for (std::string_view line : StringHelpers::StringLineIterator(content))
	{
		const size_t sep = line.find('=');
		if (sep == std::string_view::npos)
			continue; // header line or garbage
		ApplyField(line.substr(0, sep), line.substr(sep + 1));
	}
}

void Account::ApplyField(std::string_view key, std::string_view value)
{
	auto hex = [&]<typename T>(T& field) { field = ParseHex<T>(value); };

	if (key == "PersistentId")
		hex(m_persistentId);
	else if (key == "TransferableIdBase")
		hex(m_transferableIdBase);
	else if (key == "Uuid")
	{
		if (value.size() != m_uuid.size() * 2)
			throw std::system_error(AccountErrc::InvalidUuid);
		DecodeHexBytes(value, m_uuid);
	}
	else if (key == "MiiData")
	{
		if (value.size() != m_miiData.size() * 2)
			throw std::system_error(AccountErrc::InvalidMiiData);
		DecodeHexBytes(value, m_miiData);
	}
	else if (key == "MiiName")
	{
		if (value.size() != m_miiName.size() * 4)
			throw std::system_error(AccountErrc::InvalidMiiName);
		for (size_t i = 0; i < m_miiName.size(); i++)
			m_miiName[i] = (wchar_t)ParseHex<uint16>(value.substr(i * 4, 4));
	}
	else if (key == "AccountId")
		m_accountId = value;
	else if (key == "BirthYear")
		hex(m_birthYear);
	else if (key == "BirthMonth")
		hex(m_birthMonth);
	else if (key == "BirthDay")
		hex(m_birthDay);
	else if (key == "Gender")
		hex(m_gender);
	else if (key == "EmailAddress")
		m_email = value;
	else if (key == "Country")
		hex(m_country);
	else if (key == "SimpleAddressId")
		hex(m_simpleAddressId);
	else if (key == "TimeZoneId")
		m_timeZoneId = value;
	else if (key == "UtcOffset")
		m_utcOffset = (sint64)ParseHex<uint64>(value);
	else if (key == "PrincipalId")
		hex(m_principalId);
	else if (key == "IsPasswordCacheEnabled")
		hex(m_passwordCacheEnabled);
	else if (key == "AccountPasswordCache")
	{
		// The value has no length check of its own, but a value too short to
		// hold the 32 bytes (fewer than 62 digits) makes the whole file invalid.
		if (value.size() < m_passwordCache.size() * 2 - 2)
			throw std::out_of_range("AccountPasswordCache is too short");
		DecodeHexBytes(value, m_passwordCache);
	}
	else
		m_otherFields[std::string(key)] = value;
}

std::error_code Account::Save()
{
	fs::path folder = ActiveSettings::GetMlcPath(fmt::format(L"usr/save/system/act/{:08x}", m_persistentId));
	if (!fs::exists(folder))
	{
		std::error_code ec;
		fs::create_directories(folder, ec);
		if (ec)
			return ec;
	}

	std::string text;
	auto line = [&text](std::string_view s) {
		text.append(s);
		text.push_back('\n');
	};
	line(kAccountFileHeader);
	line(fmt::format("PersistentId={:08x}", m_persistentId));
	line(fmt::format("TransferableIdBase={:x}", m_transferableIdBase));
	line("Uuid=" + EncodeHexBytes(m_uuid));
	line("MiiData=" + EncodeHexBytes(m_miiData));
	std::string name = "MiiName=";
	for (wchar_t c : m_miiName)
		name.append(fmt::format("{:04x}", (uint16)c));
	line(name);
	line(fmt::format("AccountId={}", m_accountId));
	line(fmt::format("BirthYear={:x}", m_birthYear));
	line(fmt::format("BirthMonth={:x}", m_birthMonth));
	line(fmt::format("BirthDay={:x}", m_birthDay));
	line(fmt::format("Gender={:x}", m_gender));
	line(fmt::format("EmailAddress={}", m_email));
	line(fmt::format("Country={:x}", m_country));
	line(fmt::format("SimpleAddressId={:x}", m_simpleAddressId));
	line(fmt::format("PrincipalId={:x}", m_principalId));
	line(fmt::format("IsPasswordCacheEnabled={:x}", m_passwordCacheEnabled));
	line("AccountPasswordCache=" + EncodeHexBytes(m_passwordCache));
	// TimeZoneId and UtcOffset are not written (as before); other keys are kept
	for (const auto& [key, value] : m_otherFields)
		line(fmt::format("{}={}", key, value));

	try
	{
		// text mode: line ends are native (CRLF on Windows)
		std::ofstream out;
		out.exceptions(std::ios::badbit);
		out.open(folder / "account.dat");
		out << text;
		out.flush();
		out.close();
	}
	catch (const std::system_error& e)
	{
		return e.code();
	}
	return CheckValid();
}

OnlineAccountError Account::GetOnlineAccountError() const
{
	if (m_accountId.empty())
		return OnlineAccountError::kNoAccountId;
	if (!IsPasswordCacheEnabled())
		return OnlineAccountError::kNoPasswordCached;
	if (std::all_of(m_passwordCache.begin(), m_passwordCache.end(), [](uint8 b) { return b == 0; }))
		return OnlineAccountError::kPasswordCacheEmpty;
	if (m_principalId == 0)
		return OnlineAccountError::kNoPrincipalId;
	return OnlineAccountError::kNone;
}

bool Account::IsValidOnlineAccount() const
{
	return GetOnlineAccountError() == OnlineAccountError::kNone;
}

fs::path Account::GetFileName() const
{
	return GetFileName(m_persistentId);
}

fs::path Account::GetFileName(uint32 persistent_id)
{
	if (persistent_id < kMinPersistendId)
		throw std::invalid_argument(fmt::format("invalid persistent id {:#x}", persistent_id));
	return ActiveSettings::GetMlcPath(fmt::format("usr/save/system/act/{:08x}/account.dat", persistent_id));
}

std::wstring_view Account::GetMiiName() const
{
	size_t length = 0;
	while (length < m_miiName.size() - 1 && m_miiName[length] != L'\0')
		length++;
	return {m_miiName.data(), length};
}

std::string_view Account::GetStorageValue(std::string_view key) const
{
	auto it = m_otherFields.find(std::string(key));
	if (it == m_otherFields.end())
		return {};
	return it->second;
}

void Account::SetMiiName(std::wstring_view name)
{
	m_miiName.fill(L'\0');
	const size_t count = std::min(name.size(), m_miiName.size() - 1);
	std::copy_n(name.begin(), count, m_miiName.begin());
}

const std::vector<Account>& Account::RefreshAccounts()
{
	std::vector<Account> found;
	const fs::path actFolder = ActiveSettings::GetMlcPath("usr/save/system/act");
	if (fs::exists(actFolder))
	{
		for (const fs::directory_entry& entry : fs::directory_iterator(actFolder))
		{
			if (!fs::is_directory(entry))
				continue;
			const std::string folderName = entry.path().filename().string();
			if (folderName.size() != 8)
				continue;
			const uint32 id = ParseHex<uint32>(folderName);
			if (id < kMinPersistendId)
				continue;
			Account account(id);
			if (!account.Load())
				found.push_back(account);
		}
	}
	if (found.empty())
	{
		// there is always at least one account
		found.emplace_back(kMinPersistendId, L"default");
		found.front().Save();
	}
	s_accounts = std::move(found);
	UpdatePersisidDat();
	return s_accounts;
}

void Account::UpdatePersisidDat()
{
	const uint32 head = std::max(kMinPersistendId, GetNextPersistentId() - 1);
	std::ofstream out(PersisIdFilePath()); // text mode
	if (!out.is_open())
	{
		cemuLog_log(LogType::Force, "Account: could not write persisid.dat");
		return;
	}
	out << kPersisIdHeader << '\n' << kPersisIdHeadKey << fmt::format("{:x}", head) << "\n\n";
	out.flush();
}

bool Account::HasFreeAccountSlots()
{
	return s_accounts.size() < kMaxAccounts;
}

const std::vector<Account>& Account::GetAccounts()
{
	if (s_accounts.empty())
		return RefreshAccounts();
	return s_accounts;
}

const Account& Account::GetAccount(uint32 persistent_id)
{
	const std::vector<Account>& accounts = GetAccounts();
	for (const Account& account : accounts)
	{
		if (account.GetPersistentId() == persistent_id)
			return account;
	}
	return accounts.front();
}

const Account& Account::GetCurrentAccount()
{
	return GetAccount(ActiveSettings::GetPersistentId());
}

uint32 Account::GetNextPersistentId()
{
	// persisid.dat stores the last id handed out
	uint32 lastId = kMinPersistendId;
	const fs::path persisId = PersisIdFilePath();
	if (fs::exists(persisId))
	{
		std::ifstream in(persisId);
		std::string line;
		while (in.is_open() && std::getline(in, line))
		{
			if (line.starts_with(kPersisIdHeadKey))
			{
				lastId = ParseHex<uint32>(std::string_view(line).substr(kPersisIdHeadKey.size()));
				break;
			}
		}
	}
	uint32 next = lastId + 1;
	for (const Account& account : s_accounts)
		next = std::max(next, account.GetPersistentId() + 1);
	return next;
}

OnlineValidator Account::ValidateOnlineFiles() const
{
	OnlineValidator result{};

	auto checkFile = [](const fs::path& path, uintmax_t expectedSize) {
		if (!fs::exists(path))
			return OnlineValidator::FileState::Missing;
		return fs::file_size(path) == expectedSize ? OnlineValidator::FileState::Ok : OnlineValidator::FileState::Corrupted;
	};
	result.otp = checkFile(ActiveSettings::GetUserDataPath("otp.bin"), 1024);
	result.seeprom = checkFile(ActiveSettings::GetUserDataPath("seeprom.bin"), 512);

	// certificates and keys of the system title 0005001b-10054000
	auto requireContent = [&result](const std::vector<const wchar_t*>& names) {
		for (const wchar_t* name : names)
		{
			fs::path path = ActiveSettings::GetMlcPath(L"sys/title/0005001b/10054000/content/{}", name);
			if (!fs::exists(path) || !fs::is_regular_file(path))
				result.missing_files.emplace_back(path.generic_wstring());
		}
	};
	requireContent(iosuCrypt_getCertificateKeys());
	requireContent(iosuCrypt_getCertificateNames());

	result.valid_account = IsValidOnlineAccount();
	result.account_error = GetOnlineAccountError();
	return result;
}
