// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "nexTypes.h"
#include <mutex>

// NEX remote procedure calls on top of a PRUDP connection.

const int NEX_PROTOCOL_AUTHENTICATION = 0xA;
const int NEX_PROTOCOL_SECURE = 0xB;
const int NEX_PROTOCOL_FRIENDS_WIIU = 0x66;

class nexService;

// answer to one of our calls
typedef struct
{
	nexService* nex;
	bool isSuccessful;
	uint32 errorCode;
	uint32 callId;
	uint32 methodId;
	uint8  protocolId;
	nexPacketBuffer data; // result data, only for successful calls
	void* custom;
}nexServiceResponse_t;

// call made by the server
typedef struct
{
	nexService* nex;
	uint32 callId;
	uint32 methodId;
	uint8  protocolId;
	nexPacketBuffer data;
	void* custom;
}nexServiceRequest_t;

class prudpClient;

class nexService
{
public:
	static const int STATE_CONNECTING = 0;
	static const int STATE_CONNECTED = 1;
	static const int STATE_DISCONNECTED = 2;

	// error codes reported for calls that never got an answer
	static const unsigned int ERR_TIMEOUT = (0xFFFFFFFF);
	static const unsigned int ERR_NO_CONNECTION = (0xFFFFFFFE);

	// takes ownership of an already connected client
	nexService(prudpClient* con);
	// opens an unencrypted connection
	nexService(uint32 ip, uint16 port, const char* accessKey);

	const uint8 PROTOCOL_BIT_REQUEST = 0x80;

	// Calls are queued and sent from update(). The handler is skipped for error
	// answers unless callHandlerIfError is set; timeouts always reach it.
	void callMethod(uint8 protocolId, uint32 methodId, nexPacketBuffer* parameter, void(*nexServiceResponse)(nexService* nex, nexServiceResponse_t* serviceResponse), void* custom, bool callHandlerIfError = false);
	void callMethod(uint8 protocolId, uint32 methodId, nexPacketBuffer* parameter, std::function<void(nexServiceResponse_t*)> cb, bool callHandlerIfError);
	void registerProtocolRequestHandler(uint8 protocol, void(*processRequest)(nexServiceRequest_t* request), void* custom);
	void sendRequestResponse(nexServiceRequest_t* request, uint32 errorCode, uint8* data, sint32 length);
	void update();
	prudpClient* getPRUDPConnection();
	sint32 getState();
	// hands the service to the nex thread, which then calls update()
	void registerForAsyncProcessing();
	// deletes the service now, or on the nex thread once it is owned by it
	void destroy();
	bool isMarkedForDestruction();

private:
	struct PendingCall
	{
		uint8 protocolId;
		uint32 methodId;
		std::vector<uint8> params;
		bool handlerWantsErrors;
		void(*legacyHandler)(nexService* nex, nexServiceResponse_t* serviceResponse);
		void* legacyCustom;
		std::function<void(nexServiceResponse_t*)> handler;
	};

	struct SentCall
	{
		PendingCall call;
		uint32 callId;
		uint32 sentAt;
	};

	struct ServerHandler
	{
		uint8 protocolId;
		void(*handler)(nexServiceRequest_t* request);
		void* custom;
	};

	nexService();
	~nexService();
	void Enqueue(PendingCall&& call);
	void SendPending(PendingCall& call);
	void Deliver(PendingCall& call, nexServiceResponse_t* response);
	void FailCall(PendingCall& call, uint32 errorCode);
	void PollHandshake();
	void PollConnected();
	void HandleIncoming(sint32 length);
	void ExpireCalls();

private:
	uint8 m_state{STATE_CONNECTING};
	prudpClient* m_connection{nullptr};
	bool m_ownedByNexThread{false};
	bool m_destroyRequested{false};
	std::vector<SentCall> m_sent;
	std::vector<ServerHandler> m_handlers;
	std::vector<uint8> m_receiveBuffer;
	std::mutex m_queueMutex;
	std::vector<PendingCall> m_queue;
};

nexService* nex_establishSecureConnection(uint32 authServerIp, uint16 authServerPort, const char* accessKey, uint32 pid, const char* nexPassword, const char* nexToken);
