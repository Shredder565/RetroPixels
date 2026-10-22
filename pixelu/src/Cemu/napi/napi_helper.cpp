// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "napi.h"
#include "napi_helper.h"

#include "curl/curl.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "config/LaunchSettings.h"
#include "pugixml.hpp"
#include <charconv>

#include "openssl/bn.h"
#include "openssl/x509.h"
#include "openssl/ssl.h"

/*
 * TLS setup. libcurl calls one of these with the OpenSSL context before a
 * connection is made, so the console's own CA and client certificates can be
 * installed. Which ones depends on the server family.
 */
namespace
{
	void InstallCa(void* ctx, sint32 certId, bool reportAsCertError)
	{
		if (iosuCrypto_addCACertificate(ctx, certId))
			return;
		cemuLog_log(LogType::Force, "Invalid CA certificate ({})", certId);
		if (reportAsCertError)
			cemuLog_log(LogType::Force, "Certificate error");
	}

	void InstallClientCert(void* ctx, sint32 certId, const char* failMessage)
	{
		if (!iosuCrypto_addClientCertificate(ctx, certId))
			cemuLog_log(LogType::Force, std::string_view(failMessage));
	}

	enum class PeerCheck { Unchanged, Required, Off };

	void ApplyCommonTlsSettings(void* ctx, PeerCheck check)
	{
		SSL_CTX* sslCtx = (SSL_CTX*)ctx;
		SSL_CTX_set_mode(sslCtx, SSL_MODE_AUTO_RETRY);
		SSL_CTX_set_verify_depth(sslCtx, 2);
		if (check == PeerCheck::Required)
			SSL_CTX_set_verify(sslCtx, SSL_VERIFY_PEER, nullptr);
		else if (check == PeerCheck::Off)
			SSL_CTX_set_verify(sslCtx, SSL_VERIFY_NONE, nullptr);
	}

	// account server, tagaya
	CURLcode TlsSetupAccount(CURL*, void* ctx, void*)
	{
		InstallCa(ctx, 102, false);
		InstallCa(ctx, 105, false);
		InstallClientCert(ctx, 3, "Certificate error");
		ApplyCommonTlsSettings(ctx, PeerCheck::Required);
		return CURLE_OK;
	}

	// icon database, server certificate is not enforced
	CURLcode TlsSetupIconServer(CURL*, void* ctx, void*)
	{
		InstallCa(ctx, 105, false);
		ApplyCommonTlsSettings(ctx, PeerCheck::Unchanged);
		return CURLE_OK;
	}

	// SOAP services and content servers
	CURLcode TlsSetupShop(CURL*, void* ctx, void*)
	{
		InstallCa(ctx, 102, true);
		InstallClientCert(ctx, 1, "Certificate error");
		ApplyCommonTlsSettings(ctx, PeerCheck::Required);
		return CURLE_OK;
	}

	// Miiverse accepts a long list of CA groups
	CURLcode TlsSetupOlive(CURL*, void* ctx, void*)
	{
		InstallCa(ctx, 105, true);
		InstallClientCert(ctx, 7, "Olive client certificate error");
		static constexpr sint16 s_extraCaIds[] = { 100, 101, 102, 103, 104, 105 };
		for (sint16 id : s_extraCaIds)
			iosuCrypto_addCACertificate(ctx, id);
		for (sint32 id = 1001; id <= 1033; id++)
			iosuCrypto_addCACertificate(ctx, id);
		ApplyCommonTlsSettings(ctx, PeerCheck::Required);
		return CURLE_OK;
	}

	// certificates chosen by the caller (BOSS tasks)
	CURLcode TlsSetupCustom(CURL*, void* ctx, void* userParam)
	{
		const CurlRequestHelper* helper = (const CurlRequestHelper*)userParam;
		for (sint32 id : helper->GetCaCertIds())
			InstallCa(ctx, id, false);
		const std::vector<sint32> clientIds = helper->GetClientCertIds();
		for (sint32 id : clientIds)
		{
			if (!iosuCrypto_addClientCertificate(ctx, id))
				cemuLog_log(LogType::Force, "Invalid client certificate ({})", id);
		}
		ApplyCommonTlsSettings(ctx, clientIds.empty() ? PeerCheck::Off : PeerCheck::Required);
		return CURLE_OK;
	}

	using TlsSetupFn = CURLcode(*)(CURL*, void*, void*);

	void SetProxyFromConfig(CURL* curl)
	{
		const auto& proxy = GetConfig().proxy_server.GetValue();
		if (!proxy.empty())
			curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
	}
}

/* ---- CurlRequestHelper ---- */

CurlRequestHelper::CurlRequestHelper()
{
	m_curl = curl_easy_init();
	curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &CurlRequestHelper::OnBodyData);
	curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
	curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(m_curl, CURLOPT_MAXREDIRS, 2L);
	curl_easy_setopt(m_curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
	SetProxyFromConfig(m_curl);
}

CurlRequestHelper::~CurlRequestHelper()
{
	curl_easy_cleanup(m_curl);
	if (m_headerList)
		curl_slist_free_all(m_headerList);
}

void CurlRequestHelper::initate(NetworkService service, std::string url, SERVER_SSL_CONTEXT sslContext)
{
	m_headerLines.clear();
	m_postBody.clear();
	m_stream = nullptr;

	curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(m_curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_DEFAULT);
	curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 1L);
	if (IsNetworkServiceSSLDisabled(service))
	{
		curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 0L);
		return;
	}

	TlsSetupFn setup = nullptr;
	void* setupParam = nullptr;
	switch (sslContext)
	{
	case SERVER_SSL_CONTEXT::ACT:
	case SERVER_SSL_CONTEXT::TAGAYA:
		setup = TlsSetupAccount;
		break;
	case SERVER_SSL_CONTEXT::IDBE:
		setup = TlsSetupIconServer;
		break;
	case SERVER_SSL_CONTEXT::IAS:
	case SERVER_SSL_CONTEXT::ECS:
	case SERVER_SSL_CONTEXT::CCS:
		setup = TlsSetupShop;
		break;
	case SERVER_SSL_CONTEXT::OLIVE:
		setup = TlsSetupOlive;
		break;
	case SERVER_SSL_CONTEXT::CUSTOM:
		setup = TlsSetupCustom;
		setupParam = this;
		break;
	}
	if (!setup)
	{
		cemu_assert(false);
		return;
	}
	curl_easy_setopt(m_curl, CURLOPT_SSL_CTX_FUNCTION, setup);
	curl_easy_setopt(m_curl, CURLOPT_SSL_CTX_DATA, setupParam);
}

void CurlRequestHelper::addHeaderField(const char* fieldName, std::string_view value)
{
	std::string line(fieldName);
	line.append(": ");
	line.append(value);
	m_headerLines.push_back(std::move(line));
}

void CurlRequestHelper::addPostField(const char* fieldName, std::string_view value)
{
	auto append = [this](std::string_view s) { m_postBody.insert(m_postBody.end(), s.begin(), s.end()); };
	if (!m_postBody.empty())
		m_postBody.push_back('&');
	append(fieldName);
	m_postBody.push_back('=');
	append(value);
}

void CurlRequestHelper::setWriteCallback(bool(*cbWriteCallback)(void* userData, const void* ptr, size_t len, bool isLast), void* userData)
{
	m_stream = cbWriteCallback;
	m_streamUser = userData;
}

void CurlRequestHelper::setTimeout(sint32 timeoutSeconds)
{
	curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, (long)timeoutSeconds);
}

size_t CurlRequestHelper::OnBodyData(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	CurlRequestHelper* self = (CurlRequestHelper*)userdata;
	const size_t bytes = size * nmemb;
	if (self->m_stream)
		return self->m_stream(self->m_streamUser, ptr, bytes, false) ? bytes : 0;
	self->m_received.insert(self->m_received.end(), (const uint8*)ptr, (const uint8*)ptr + bytes);
	return bytes;
}

bool CurlRequestHelper::submitRequest(bool isPost)
{
	curl_slist* headers = nullptr;
	for (const std::string& line : m_headerLines)
		headers = curl_slist_append(headers, line.c_str());
	curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);
	if (m_headerList)
		curl_slist_free_all(m_headerList);
	m_headerList = headers;

	if (!isPost)
		curl_easy_setopt(m_curl, CURLOPT_POST, 0L);
	else if (!m_multipart)
	{
		curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
		curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, m_postBody.data());
		curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, (long)m_postBody.size());
	}

	CURLcode rc = curl_easy_perform(m_curl);
	if (rc != CURLE_OK)
	{
		cemuLog_log(LogType::Force, "CURL web request failed with error {}. Retrying...", (int)rc);
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (curl_easy_perform(m_curl) != CURLE_OK)
			return false;
	}

	long status = 0;
	curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &status);
	m_status = (sint32)status;
	if (status != 200)
	{
		cemuLog_log(LogType::Force, "HTTP request received response {} but expected 200", status);
		char* finalUrl = nullptr;
		curl_easy_getinfo(m_curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
		if (finalUrl)
			cemuLog_log(LogType::Force, "Request: {}", finalUrl);
		// 4xx/5xx fail the request, except 400 which login errors etc. come with
		const bool isErrorStatus = status >= 401 && status <= 599;
		if (isErrorStatus || m_received.empty())
			return false;
	}

	if (m_stream)
		m_stream(m_streamUser, nullptr, 0, true);
	return true;
}

/* ---- CurlSOAPHelper ---- */

CurlSOAPHelper::CurlSOAPHelper(NetworkService service)
{
	m_curl = curl_easy_init();
	curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &CurlSOAPHelper::OnBodyData);
	curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
	curl_easy_setopt(m_curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
	if (IsNetworkServiceSSLDisabled(service))
	{
		curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 0L);
	}
	else
	{
		curl_easy_setopt(m_curl, CURLOPT_SSL_CTX_FUNCTION, (TlsSetupFn)TlsSetupShop);
		curl_easy_setopt(m_curl, CURLOPT_SSL_CTX_DATA, (void*)nullptr);
		curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 1L);
	}
	SetProxyFromConfig(m_curl);
}

CurlSOAPHelper::~CurlSOAPHelper()
{
	curl_easy_cleanup(m_curl);
	if (m_headerList)
		curl_slist_free_all(m_headerList);
}

size_t CurlSOAPHelper::OnBodyData(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	CurlSOAPHelper* self = (CurlSOAPHelper*)userdata;
	const size_t bytes = size * nmemb;
	self->m_received.insert(self->m_received.end(), (const uint8*)ptr, (const uint8*)ptr + bytes);
	return bytes;
}

void CurlSOAPHelper::SOAP_initate(std::string_view serviceType, std::string url, std::string_view requestMethod, std::string_view requestVersion)
{
	curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
	m_ns.assign(serviceType);
	m_method.assign(requestMethod);
	m_version.assign(requestVersion);
	m_fields.clear();
}

void CurlSOAPHelper::SOAP_addRequestField(const char* fieldName, std::string_view value)
{
	fmt::format_to(std::back_inserter(m_fields), "<{0}:{1}>{2}</{0}:{1}>", m_ns, fieldName, value);
}

std::string CurlSOAPHelper::BuildEnvelope() const
{
	// The MessageId is echoed back by the server. Real consoles put the device id
	// and uptime in it; any pair of numbers is accepted.
	static const uint64 s_sessionTag = 1 + (uint64)HighResolutionTimer::now().getTick() / 7;
	const uint64 sequence = (uint64)HighResolutionTimer::now().getTick();

	std::string xml;
	xml.reserve(1024 + m_fields.size());
	xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\"\n"
		" xmlns:SOAP-ENC=\"http://schemas.xmlsoap.org/soap/encoding/\"\n"
		" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
		" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"\n";
	auto out = std::back_inserter(xml);
	fmt::format_to(out, " xmlns:{0}=\"urn:{0}.wsapi.broadon.com\">\n", m_ns);
	xml += "<SOAP-ENV:Body>\n";
	fmt::format_to(out, "<{0}:{1} xsi:type=\"{0}:{1}RequestType\">\n", m_ns, m_method);
	fmt::format_to(out, "<{0}:Version>{1}</{0}:Version>\n", m_ns, m_version);
	fmt::format_to(out, "<{0}:MessageId>EC-{1}-{2}</{0}:MessageId>", m_ns, s_sessionTag, sequence);
	xml += m_fields;
	fmt::format_to(out, "</{}:{}>\n", m_ns, m_method);
	xml += "</SOAP-ENV:Body>\n"
		"</SOAP-ENV:Envelope>\n";
	return xml;
}

bool CurlSOAPHelper::submitRequest()
{
	m_body = BuildEnvelope();
	curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
	curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, m_body.c_str());
	curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, (long)m_body.size());

	const std::string soapAction = fmt::format("SOAPAction: urn:{}.wsapi.broadon.com/{}", m_ns, m_method);
	curl_slist* headers = nullptr;
	for (const char* line : { "Content-Type: text/xml; charset=utf-8", "Accept-Charset: UTF-8", soapAction.c_str(), "Accept: */*" })
		headers = curl_slist_append(headers, line);
	curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);
	if (m_headerList)
		curl_slist_free_all(m_headerList);
	m_headerList = headers;
	curl_easy_setopt(m_curl, CURLOPT_USERAGENT, "EVL NUP 040800 Sep 18 2012 20:20:02");

	return curl_easy_perform(m_curl) == CURLE_OK;
}

/* ---- XML helpers shared by the SOAP services ---- */

namespace NAPI
{
	bool _findXmlNode(pugi::xml_node& doc, pugi::xml_node& nodeOut, const char* name)
	{
		for (pugi::xml_node child = doc.first_child(); child; child = child.next_sibling())
		{
			if (boost::iequals(child.name(), name))
			{
				nodeOut = child;
				return true;
			}
			if (_findXmlNode(child, nodeOut, name))
				return true;
		}
		return false;
	}

	bool _parseResponseInit(const CurlSOAPHelper& soapHelper, const char* responseNodeName, pugi::xml_node& node, _NAPI_CommonResultSOAP& result, pugi::xml_document& doc, pugi::xml_node& responseNode)
	{
		result.apiError = NAPI_RESULT::XML_ERROR;
		const std::vector<uint8>& reply = soapHelper.getReceivedData();
		if (!doc.load_buffer(reply.data(), reply.size()))
		{
			cemuLog_log(LogType::Force, "Failed to parse GetRegistrationInfo() response");
			return false;
		}
		if (!_findXmlNode(doc, node, responseNodeName))
			return false;

		std::string_view codeText = node.child_value("ErrorCode");
		int code = 0;
		auto [end, ec] = std::from_chars(codeText.data(), codeText.data() + codeText.size(), code);
		if (ec != std::errc())
			return false;
		if (code != 0)
		{
			result.apiError = NAPI_RESULT::SERVICE_ERROR;
			result.serviceError = (EC_ERROR_CODE)code;
			return false;
		}
		result.apiError = NAPI_RESULT::SUCCESS;
		return true;
	}
};
