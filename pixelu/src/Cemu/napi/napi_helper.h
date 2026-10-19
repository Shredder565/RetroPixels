// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "napi.h"
#include "curl/curl.h"
#include "pugixml.hpp"

typedef void CURL;

// Thin wrapper around a libcurl easy handle for the plain HTTP(S) services.
// The handle is reused: initate() resets per-request state, the received data
// buffer keeps accumulating until the helper is destroyed.
class CurlRequestHelper
{
public:
	// selects which console certificates are installed into the TLS context
	enum class SERVER_SSL_CONTEXT
	{
		ACT,    // account server
		ECS,
		IAS,
		CCS,
		IDBE,
		TAGAYA,
		OLIVE,  // Miiverse
		CUSTOM, // certificates given via AddCaCertId / AddClientCertId
	};

	CurlRequestHelper();
	~CurlRequestHelper();
	CurlRequestHelper(const CurlRequestHelper&) = delete;
	CurlRequestHelper& operator=(const CurlRequestHelper&) = delete;

	CURL* getCURL() { return m_curl; }

	void initate(NetworkService service, std::string url, SERVER_SSL_CONTEXT sslContext);
	void addHeaderField(const char* fieldName, std::string_view value);
	void addPostField(const char* fieldName, std::string_view value);
	void setWriteCallback(bool(*cbWriteCallback)(void* userData, const void* ptr, size_t len, bool isLast), void* userData);
	void setTimeout(sint32 timeoutSeconds); // 0 = no limit

	bool submitRequest(bool isPost = false);

	std::vector<uint8>& getReceivedData() { return m_received; }

	void setUseMultipartFormData(bool isUsingMultipartFormData) { m_multipart = isUsingMultipartFormData; }

	void ClearCaCertIds() { m_caCerts.clear(); }
	void ClearClientCertIds() { m_clientCerts.clear(); }
	void AddCaCertId(sint32 caCertId) { m_caCerts.push_back(caCertId); }
	void AddClientCertId(sint32 clientCertId) { m_clientCerts.push_back(clientCertId); }
	std::vector<sint32> GetCaCertIds() const { return m_caCerts; }
	std::vector<sint32> GetClientCertIds() const { return m_clientCerts; }

	sint32 GetHTTPStatusCode() const { return m_status; }

private:
	using StreamCallback = bool(*)(void* userData, const void* ptr, size_t len, bool isLast);

	static size_t OnBodyData(char* ptr, size_t size, size_t nmemb, void* userdata);

	CURL* m_curl{};
	curl_slist* m_headerList{};
	std::vector<uint8> m_received;
	std::vector<std::string> m_headerLines;
	std::vector<uint8> m_postBody;
	StreamCallback m_stream{};
	void* m_streamUser{};
	bool m_multipart{false};
	std::vector<sint32> m_caCerts;
	std::vector<sint32> m_clientCerts;
	sint32 m_status{0};
};

// Builds and posts a SOAP envelope for the BroadOn style services (NUS, IAS, ECS)
class CurlSOAPHelper
{
public:
	CurlSOAPHelper(NetworkService service);
	~CurlSOAPHelper();
	CurlSOAPHelper(const CurlSOAPHelper&) = delete;
	CurlSOAPHelper& operator=(const CurlSOAPHelper&) = delete;

	CURL* getCURL() { return m_curl; }

	void SOAP_initate(std::string_view serviceType, std::string url, std::string_view requestMethod, std::string_view requestVersion);
	void SOAP_addRequestField(const char* fieldName, std::string_view value);

	bool submitRequest();

	const std::vector<uint8>& getReceivedData() const { return m_received; }

private:
	std::string BuildEnvelope() const;
	static size_t OnBodyData(char* ptr, size_t size, size_t nmemb, void* userdata);

	CURL* m_curl{};
	curl_slist* m_headerList{};
	std::vector<uint8> m_received;
	std::string m_ns;      // service prefix, e.g. "ecs"
	std::string m_method;
	std::string m_version;
	std::string m_fields;  // serialized request fields
	std::string m_body;    // last posted envelope, must outlive the transfer
};

namespace NAPI
{
	// depth first search for the first element named `name` (case insensitive)
	bool _findXmlNode(pugi::xml_node& doc, pugi::xml_node& nodeOut, const char* name);
	// parses a SOAP reply, locates responseNodeName and evaluates its ErrorCode
	bool _parseResponseInit(const CurlSOAPHelper& soapHelper, const char* responseNodeName, pugi::xml_node& node, _NAPI_CommonResultSOAP& result, pugi::xml_document& doc, pugi::xml_node& responseNode);
};
