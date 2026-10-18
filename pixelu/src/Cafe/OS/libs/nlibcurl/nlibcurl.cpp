// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// nlibcurl: the console's build of libcurl. Every guest handle (easy, multi,
// share) is a zeroed block of guest memory obtained through the guest's
// allocator callbacks; all host state lives in host-side tables keyed by the
// guest address, so no host pointer is ever stored in guest memory.
//
// curl_easy_perform/curl_easy_pause run the host libcurl call on a helper host
// thread so the calling guest thread can keep servicing the guest callbacks
// (write, read, header, progress, share locks) while the transfer runs. The
// helper thread forwards each callback to the guest thread and waits for the
// answer. curl_multi_perform runs on the guest thread and calls back directly.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/common/OSUtil.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "nlibcurl.h"

#include "openssl/x509.h"
#include "openssl/ssl.h"

#define CURL_STRICTER
#include "curl/curl.h"

#include <atomic>
#include <bit>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/OS/libs/nsysnet/nsysnet.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/common/PPCConcurrentQueue.h"
#include "util/helpers/ConcurrentQueue.h"
#include "util/helpers/helpers.h"

#include "nlibcurlDebug.hpp"

namespace nlibcurl
{
	// ---- constants ----------------------------------------------------------

	// console-specific easy options (NSSL integration)
	constexpr uint32 OPT_NSSL_CONTEXT = 210;
	constexpr uint32 OPT_NSSL_VERIFY = 211;

	// verify flags for OPT_NSSL_VERIFY
	constexpr uint32 VERIFY_PEER = 1u << 0;
	constexpr uint32 VERIFY_HOSTNAME = 1u << 1;

	// sizes of the guest-side handle blocks
	constexpr uint32 GUEST_EASY_SIZE = 0x290;
	constexpr uint32 GUEST_MULTI_SIZE = 0x80;
	constexpr uint32 GUEST_SHARE_SIZE = 0x1C;

	// the console library hands at most 16 KiB to a read callback at once
	// (Super Mario Maker's level upload breaks with larger requests)
	constexpr size_t MAX_READ_ITEMS = 0x4000;

	// ---- guest structures ---------------------------------------------------

	struct GuestSlist
	{
		MEMPTR<char> text;
		MEMPTR<GuestSlist> next;
	};
	static_assert(sizeof(GuestSlist) == 8);

	struct GuestMultiMsg
	{
		uint32be msg;
		uint32be easy;   // guest easy handle
		uint32be result; // CURLcode
	};
	static_assert(sizeof(GuestMultiMsg) == 12);

	// ---- allocator callbacks -------------------------------------------------

	struct Allocators
	{
		MPTR malloc = 0;
		MPTR free = 0;
		MPTR realloc = 0;
		MPTR strdup = 0;
		MPTR calloc = 0;
	};

	sint32 s_initCount = 0;
	Allocators s_alloc;

	MPTR GuestAlloc(uint32 size)
	{
		return PPCCoreCallback(s_alloc.malloc, size);
	}

	MPTR GuestAllocZeroed(uint32 size)
	{
		MPTR p = PPCCoreCallback(s_alloc.calloc, (uint32)1, size);
		if (p)
			memset(memory_getPointerFromVirtualOffset(p), 0, size);
		return p;
	}

	void GuestFree(MPTR p)
	{
		if (p)
			PPCCoreCallback(s_alloc.free, p);
	}

	MPTR GuestStrdup(const char* s)
	{
		uint32 len = (uint32)strlen(s) + 1;
		MPTR p = GuestAlloc(len);
		if (p)
			memcpy(memory_getPointerFromVirtualOffset(p), s, len);
		return p;
	}

	// built-in allocators, used unless the game supplies its own through curl_global_init_mem
	namespace builtin
	{
		MPTR Malloc(uint32 size)
		{
			return PPCCoreCallback(gCoreinitData->MEMAllocFromDefaultHeap.GetMPTR(), size);
		}

		void Free(MPTR p)
		{
			PPCCoreCallback(gCoreinitData->MEMFreeToDefaultHeap.GetMPTR(), p);
		}

		MPTR Calloc(uint32 count, uint32 size)
		{
			uint32 total = count * size;
			MPTR p = PPCCoreCallback(gCoreinitData->MEMAllocFromDefaultHeap.GetMPTR(), total);
			if (p)
				memset(memory_getPointerFromVirtualOffset(p), 0, total);
			return p;
		}

		MPTR Strdup(MEMPTR<const char> s)
		{
			uint32 len = (uint32)strlen(s.GetPtr()) + 1;
			MEMPTR<char> p{ (char*)coreinit::default_MEMAllocFromDefaultHeap(len) };
			if (p)
				memcpy(p.GetPtr(), s.GetPtr(), len);
			return p.GetMPTR();
		}

		// the old block size is unknown, so `size` bytes are carried over
		MPTR Realloc(MPTR old, uint32 size)
		{
			MEMPTR<void> p{ coreinit::default_MEMAllocFromDefaultHeap(size) };
			if (old)
			{
				if (p)
					memcpy(p.GetPtr(), memory_getPointerFromVirtualOffset(old), size);
				coreinit::default_MEMFreeToDefaultHeap(memory_getPointerFromVirtualOffset(old));
			}
			return p.GetMPTR();
		}
	}

	// ---- running host calls off the guest thread ---------------------------

	struct RelayItem
	{
		bool finished;
		uint32 value;                    // result, when finished
		std::function<uint32()>* job;    // guest-side work, otherwise
	};

	struct Relay
	{
		OSThread_t* guestThread;
		PPCConcurrentQueue<RelayItem> toGuest;
		ConcurrentQueue<uint32> toHelper;
	};

	// set on helper threads only
	thread_local Relay* t_relay = nullptr;

	// run guest code (callbacks) from whatever thread host libcurl calls us on
	uint32 OnGuestThread(std::function<uint32()> job)
	{
		Relay* relay = t_relay;
		if (!relay)
			return job(); // already on the guest thread
		relay->toGuest.push(RelayItem{ false, 0, &job }, relay->guestThread);
		return relay->toHelper.pop();
	}

	// run `op` on a helper host thread while the current guest thread services callbacks
	uint32 RunWithRelay(std::function<uint32()> op)
	{
		Relay relay;
		relay.guestThread = coreinit::OSGetCurrentThread();
		std::thread helper([&relay, &op]() {
			SetThreadName("nlibcurl");
			t_relay = &relay;
			uint32 r = op();
			t_relay = nullptr;
			relay.toGuest.push(RelayItem{ true, r, nullptr }, relay.guestThread);
		});
		uint32 result = 0;
		while (true)
		{
			RelayItem item = relay.toGuest.pop(relay.guestThread);
			if (item.finished)
			{
				result = item.value;
				break;
			}
			relay.toHelper.push((*item.job)());
		}
		helper.join();
		return result;
	}

	// ---- handle state -------------------------------------------------------

	struct Share;

	struct Easy
	{
		MPTR guest = 0;
		CURL* curl = nullptr;

		// guest callbacks and their user pointers
		MPTR writeFn = 0, writeData = 0;
		MPTR headerFn = 0, headerData = 0;
		MPTR readFn = 0, readData = 0;
		MPTR progressFn = 0, progressData = 0;
		MPTR sockoptFn = 0, sockoptData = 0;

		// NSSL
		uint32 nssl = 0;
		uint32 verifyFlags = 0;

		// request state applied lazily before a transfer
		bool usePost = false;
		MPTR headerList = 0;     // guest curl_slist
		sint64 inFileSize = 0;
		bool needsSync = true;
		curl_slist* hostHeaders = nullptr;

		// strings handed out by curl_easy_getinfo, by info id
		std::map<uint32, MPTR> infoStrings;

		RequestDump dump;
	};

	struct Multi
	{
		MPTR guest = 0;
		CURLM* multi = nullptr;
		std::vector<MPTR> members;  // guest easy handles, in add order
		MPTR msgBuffer = 0;         // GuestMultiMsg returned by curl_multi_info_read
	};

	struct Share
	{
		MPTR guest = 0;
		CURLSH* share = nullptr;
		MPTR lockFn = 0;
		MPTR unlockFn = 0;
		MPTR userData = 0;
		MPTR lastEasy = 0;          // passed as the handle argument of the lock callbacks
	};

	std::mutex s_tableLock;
	std::unordered_map<MPTR, std::unique_ptr<Easy>> s_easy;
	std::unordered_map<MPTR, std::unique_ptr<Multi>> s_multi;
	std::unordered_map<MPTR, std::unique_ptr<Share>> s_share;

	template<typename T>
	T* Find(std::unordered_map<MPTR, std::unique_ptr<T>>& table, MPTR handle)
	{
		std::unique_lock _l(s_tableLock);
		auto it = table.find(handle);
		return it == table.end() ? nullptr : it->second.get();
	}

	template<typename T>
	T* Insert(std::unordered_map<MPTR, std::unique_ptr<T>>& table, MPTR handle)
	{
		std::unique_lock _l(s_tableLock);
		auto& slot = table[handle];
		slot = std::make_unique<T>();
		slot->guest = handle;
		return slot.get();
	}

	template<typename T>
	std::unique_ptr<T> Remove(std::unordered_map<MPTR, std::unique_ptr<T>>& table, MPTR handle)
	{
		std::unique_lock _l(s_tableLock);
		auto it = table.find(handle);
		if (it == table.end())
			return nullptr;
		std::unique_ptr<T> r = std::move(it->second);
		table.erase(it);
		return r;
	}

	// ---- host libcurl callbacks --------------------------------------------

	uint32 CopyToGuestAndCall(MPTR fn, const char* data, size_t size, size_t count, MPTR user)
	{
		uint32 bytes = (uint32)(size * count);
		StackAllocator<uint8> buf(std::max<uint32>(bytes, 1));
		memcpy(buf.GetPointer(), data, bytes);
		return PPCCoreCallback(fn, buf.GetMPTR(), (uint32)size, (uint32)count, user);
	}

	size_t OnHeader(char* data, size_t size, size_t count, void* user)
	{
		Easy* e = (Easy*)user;
		e->dump.ResponseHeader(data, size * count);
		if (!e->headerFn)
			return size * count;
		return OnGuestThread([&]() { return CopyToGuestAndCall(e->headerFn, data, size, count, e->headerData); });
	}

	size_t OnWrite(char* data, size_t size, size_t count, void* user)
	{
		Easy* e = (Easy*)user;
		e->dump.ResponseBody(data, size * count);
		if (!e->writeFn)
			return size * count; // no guest consumer: discard
		return OnGuestThread([&]() { return CopyToGuestAndCall(e->writeFn, data, size, count, e->writeData); });
	}

	size_t OnRead(char* buffer, size_t size, size_t count, void* user)
	{
		Easy* e = (Easy*)user;
		if (!e->readFn)
			return 0; // nothing to upload
		count = std::min(count, MAX_READ_ITEMS);
		const uint32 capacity = (uint32)(size * count);
		return OnGuestThread([&]() -> uint32 {
			StackAllocator<uint8> buf(std::max<uint32>(capacity, 1));
			uint32 r = PPCCoreCallback(e->readFn, buf.GetMPTR(), (uint32)size, (uint32)count, e->readData);
			if (r <= capacity) // larger values are CURL_READFUNC_ABORT / CURL_READFUNC_PAUSE
				memcpy(buffer, buf.GetPointer(), r);
			return r;
		});
	}

	int OnProgress(void* user, double dlTotal, double dlNow, double ulTotal, double ulNow)
	{
		Easy* e = (Easy*)user;
		if (!e->progressFn)
			return 0;
		return (int)OnGuestThread([&]() { return PPCCoreCallback(e->progressFn, e->progressData, dlTotal, dlNow, ulTotal, ulNow); });
	}

	int OnSockopt(void* user, curl_socket_t s, curlsocktype purpose)
	{
		// the guest's sockopt callback expects console sockets; it is not invoked
		return CURL_SOCKOPT_OK;
	}

	int OnCloseSocket(void* user, curl_socket_t s)
	{
		nsysnet::ForgetHostSocket((SOCKET)s);
		closesocket(s);
		return 0;
	}

	void OnShareLock(CURL* handle, curl_lock_data data, curl_lock_access access, void* user)
	{
		Share* sh = (Share*)user;
		if (!sh->lockFn)
			return;
		OnGuestThread([&]() { return PPCCoreCallback(sh->lockFn, sh->lastEasy, (uint32)data, (uint32)access, sh->userData); });
	}

	void OnShareUnlock(CURL* handle, curl_lock_data data, void* user)
	{
		Share* sh = (Share*)user;
		if (!sh->unlockFn)
			return;
		OnGuestThread([&]() { return PPCCoreCallback(sh->unlockFn, sh->lastEasy, (uint32)data, sh->userData); });
	}

	int AcceptAnyCertificate(int preverified, X509_STORE_CTX* ctx)
	{
		if (!preverified)
		{
			int err = X509_STORE_CTX_get_error(ctx);
			if (err != 0)
				cemuLog_logDebug(LogType::nlibcurl, "nlibcurl: certificate verification error {} ignored", err);
			X509_STORE_CTX_set_error(ctx, X509_V_OK);
		}
		return 1;
	}

	// installs the certificates of the NSSL context into the host SSL context
	CURLcode OnSslContext(CURL* curl, void* sslCtx, void* user)
	{
		Easy* e = (Easy*)user;
		const nsysnet::NsslContext* ctx = nsysnet::GetNsslContext(e->nssl);
		if (!ctx)
			return CURLE_SSL_CERTPROBLEM;
		for (uint32 id : ctx->serverCertIds)
		{
			if (!iosuCrypto_addCACertificate(sslCtx, (sint32)id))
			{
				cemuLog_log(LogType::Force, "nlibcurl: failed to add CA certificate {}", id);
				return CURLE_SSL_CACERT;
			}
		}
		for (const auto& der : ctx->serverCertsExternal)
		{
			std::vector<uint8> copy(der);
			if (copy.empty() || !iosuCrypto_addCustomCACertificate(sslCtx, copy.data(), (sint32)copy.size()))
			{
				cemuLog_log(LogType::Force, "nlibcurl: failed to add a game-supplied CA certificate");
				return CURLE_SSL_CACERT;
			}
		}
		if (ctx->clientCertId != 0 && !iosuCrypto_addClientCertificate(sslCtx, (sint32)ctx->clientCertId))
		{
			cemuLog_log(LogType::Force, "nlibcurl: failed to add client certificate {}", ctx->clientCertId);
			return CURLE_SSL_CERTPROBLEM;
		}

		SSL_CTX* ssl = (SSL_CTX*)sslCtx;
		if ((e->verifyFlags & VERIFY_PEER) != 0)
		{
			SSL_CTX_set_cipher_list(ssl, "AES256-SHA"); // TLS_RSA_WITH_AES_256_CBC_SHA, as the console uses
			SSL_CTX_set_mode(ssl, SSL_MODE_AUTO_RETRY);
			SSL_CTX_set_verify_depth(ssl, 2);
			SSL_CTX_set_verify(ssl, SSL_VERIFY_PEER, AcceptAnyCertificate);
		}
		else
			SSL_CTX_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
		return CURLE_OK;
	}

	// ---- easy handle helpers ------------------------------------------------

	void ApplyDefaults(Easy* e)
	{
		e->writeFn = e->writeData = 0;
		e->headerFn = e->headerData = 0;
		e->readFn = e->readData = 0;
		e->progressFn = e->progressData = 0;
		e->sockoptFn = e->sockoptData = 0;
		e->nssl = 0;
		e->verifyFlags = 0;
		e->usePost = false;
		e->headerList = 0;
		e->inFileSize = 0;
		e->needsSync = true;

		curl_easy_setopt(e->curl, CURLOPT_HEADERFUNCTION, OnHeader);
		curl_easy_setopt(e->curl, CURLOPT_HEADERDATA, e);
		curl_easy_setopt(e->curl, CURLOPT_WRITEFUNCTION, OnWrite);
		curl_easy_setopt(e->curl, CURLOPT_WRITEDATA, e);
		curl_easy_setopt(e->curl, CURLOPT_READFUNCTION, OnRead);
		curl_easy_setopt(e->curl, CURLOPT_READDATA, e);
		curl_easy_setopt(e->curl, CURLOPT_CLOSESOCKETFUNCTION, OnCloseSocket);
		curl_easy_setopt(e->curl, CURLOPT_CLOSESOCKETDATA, nullptr);
	}

	// push the state that is kept on our side to the host handle
	void SyncRequest(Easy* e)
	{
		if (!e->needsSync)
			return;
		e->needsSync = false;

		if (e->usePost)
		{
			curl_easy_setopt(e->curl, CURLOPT_POST, 1L);
			curl_easy_setopt(e->curl, CURLOPT_NOBODY, 0L);
		}
		else
			curl_easy_setopt(e->curl, CURLOPT_HTTPGET, 1L);

		// headers. A hand-written Content-Length on a POST is turned into
		// CURLOPT_INFILESIZE_LARGE, since current libcurl does not allow setting it
		// manually (NEX DataStore in Super Mario Maker sends one)
		curl_slist* list = nullptr;
		uint64 headerLength = 0;
		static constexpr char kContentLength[] = "Content-Length:";
		for (MEMPTR<GuestSlist> it{ e->headerList }; it; it = it->next)
		{
			const char* line = it->text.GetPtr();
			if (!line)
				continue;
			if (e->usePost && strncmp(line, kContentLength, sizeof(kContentLength) - 1) == 0)
			{
				headerLength = strtoull(line + sizeof(kContentLength) - 1, nullptr, 10);
				continue;
			}
			e->dump.LogOption("CURLOPT_HTTPHEADER", line);
			list = curl_slist_append(list, line);
		}
		curl_easy_setopt(e->curl, CURLOPT_HTTPHEADER, list);
		if (e->hostHeaders)
			curl_slist_free_all(e->hostHeaders);
		e->hostHeaders = list;

		curl_off_t size = 0;
		if (e->inFileSize != 0)
			size = (curl_off_t)e->inFileSize;
		else if (e->usePost)
			size = (curl_off_t)headerLength;
		curl_easy_setopt(e->curl, CURLOPT_INFILESIZE_LARGE, size);
	}

	void SetInfoString(Easy* e, uint32 info, const char* value, MEMPTR<uint32be> out)
	{
		MPTR& slot = e->infoStrings[info];
		GuestFree(slot);
		slot = value ? GuestStrdup(value) : 0;
		*out = slot;
	}

	// ---- global ------------------------------------------------------------

	uint32 GlobalInit(uint32 flags)
	{
		if (s_initCount++ != 0)
			return CURLE_OK;
		s_alloc.malloc = makeCallableExport<&builtin::Malloc>();
		s_alloc.free = makeCallableExport<&builtin::Free>();
		s_alloc.calloc = makeCallableExport<&builtin::Calloc>();
		s_alloc.strdup = makeCallableExport<&builtin::Strdup>();
		s_alloc.realloc = makeCallableExport<&builtin::Realloc>();
		return ::curl_global_init(flags);
	}

	uint32 GlobalInitMem(uint32 flags, MPTR fnMalloc, MPTR fnFree, MPTR fnRealloc, MPTR fnStrdup, MPTR fnCalloc)
	{
		if (!fnMalloc || !fnFree || !fnRealloc || !fnStrdup || !fnCalloc)
			return CURLE_FAILED_INIT;
		if (s_initCount != 0)
			return CURLE_OK;
		uint32 r = GlobalInit(flags);
		if (r == CURLE_OK)
			s_alloc = Allocators{ fnMalloc, fnFree, fnRealloc, fnStrdup, fnCalloc };
		return r;
	}

	MPTR SlistAppend(MPTR list, MEMPTR<const char> text)
	{
		MPTR copy = PPCCoreCallback(s_alloc.strdup, text.GetMPTR());
		if (!copy)
			return 0;
		MEMPTR<GuestSlist> node{ GuestAlloc(sizeof(GuestSlist)) };
		if (!node)
		{
			GuestFree(copy);
			return list;
		}
		node->text = MEMPTR<char>(copy);
		node->next = nullptr;
		if (!list)
			return node.GetMPTR();
		MEMPTR<GuestSlist> tail{ list };
		while (tail->next)
			tail = tail->next;
		tail->next = node;
		return list;
	}

	void SlistFreeAll(MPTR list)
	{
		// lists may still be referenced by easy handles that sync lazily, so they are kept
		cemuLog_logDebug(LogType::nlibcurl, "curl_slist_free_all(0x{:08x}) ignored", list);
	}

	MPTR EasyStrerror(uint32 code)
	{
		static std::mutex lock;
		static std::unordered_map<uint32, MPTR> cache;
		std::unique_lock _l(lock);
		auto it = cache.find(code);
		if (it != cache.end())
			return it->second;
		const char* text = curl_easy_strerror((CURLcode)code);
		if (!text)
			return 0;
		uint32 len = (uint32)strlen(text) + 1;
		MPTR p = coreinit_allocFromSysArea(len, 4);
		memcpy(memory_getPointerFromVirtualOffset(p), text, len);
		cache.emplace(code, p);
		return p;
	}

	// ---- easy ---------------------------------------------------------------

	MPTR EasyInit()
	{
		if (s_initCount == 0 && GlobalInit(CURL_GLOBAL_DEFAULT) != CURLE_OK)
			return 0;
		MPTR guest = GuestAllocZeroed(GUEST_EASY_SIZE);
		if (!guest)
			return 0;
		Easy* e = Insert(s_easy, guest);
		e->curl = curl_easy_init();
		ApplyDefaults(e);
		return guest;
	}

	void EasyCleanup(MPTR handle)
	{
		std::unique_ptr<Easy> e = Remove(s_easy, handle);
		if (!e)
			return;
		e->dump.Close();
		curl_easy_cleanup(e->curl);
		if (e->hostHeaders)
			curl_slist_free_all(e->hostHeaders);
		for (auto& it : e->infoStrings)
			GuestFree(it.second);
		GuestFree(handle);
	}

	void EasyReset(MPTR handle)
	{
		Easy* e = Find(s_easy, handle);
		if (!e)
			return;
		curl_easy_reset(e->curl);
		ApplyDefaults(e);
	}

	uint32 EasySetopt(MPTR handle, uint32 option, uint32 arg, uint32 argLow)
	{
		Easy* e = Find(s_easy, handle);
		if (!e)
			return CURLE_BAD_FUNCTION_ARGUMENT;
		// the variadic argument is either one word or, for *_LARGE options, a register pair
		const uint64 arg64 = ((uint64)arg << 32) | argLow;
		const char* str = MEMPTR<char>(arg).GetPtr();
		CURL* c = e->curl;
		e->needsSync = true;

		CURLcode r = CURLE_OK;
		switch (option)
		{
		case CURLOPT_POST:
			e->usePost = arg != 0;
			break;
		case CURLOPT_HTTPGET:
			if (arg != 0)
				e->usePost = false;
			break;
		case CURLOPT_INFILESIZE:
			e->inFileSize = (sint64)(sint32)arg;
			break;
		case CURLOPT_INFILESIZE_LARGE:
			e->inFileSize = (sint64)arg64;
			break;
		case CURLOPT_HTTPHEADER:
			e->headerList = arg;
			break;

		case CURLOPT_NOSIGNAL:
		case CURLOPT_FOLLOWLOCATION:
		case CURLOPT_BUFFERSIZE:
		case CURLOPT_TIMEOUT:
		case CURLOPT_CONNECTTIMEOUT:
		case CURLOPT_CONNECTTIMEOUT_MS:
		case CURLOPT_NOPROGRESS:
		case CURLOPT_LOW_SPEED_LIMIT:
		case CURLOPT_LOW_SPEED_TIME:
			r = curl_easy_setopt(c, (CURLoption)option, (long)arg);
			break;

		case CURLOPT_URL:
			e->dump.LogOption("CURLOPT_URL", str);
			r = curl_easy_setopt(c, (CURLoption)option, str);
			break;
		case CURLOPT_POSTFIELDS:
			// not copied by libcurl: the data stays in guest memory
			e->dump.LogOption("CURLOPT_POSTFIELDS", str);
			r = curl_easy_setopt(c, (CURLoption)option, str);
			break;
		case CURLOPT_PROXY:
		case CURLOPT_USERAGENT:
			r = curl_easy_setopt(c, (CURLoption)option, str);
			break;

		case CURLOPT_POSTFIELDSIZE_LARGE:
		case CURLOPT_MAX_SEND_SPEED_LARGE:
		case CURLOPT_MAX_RECV_SPEED_LARGE:
			r = curl_easy_setopt(c, (CURLoption)option, (curl_off_t)arg64);
			break;

		case OPT_NSSL_VERIFY:
			e->verifyFlags = arg;
			curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, (arg & VERIFY_PEER) ? 1L : 0L);
			curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, (arg & VERIFY_HOSTNAME) ? 2L : 0L);
			break;
		case OPT_NSSL_CONTEXT:
		{
			const nsysnet::NsslContext* ctx = nsysnet::GetNsslContext(arg);
			if (!ctx)
			{
				cemuLog_log(LogType::Force, "nlibcurl: unknown NSSL context {}", arg);
				break;
			}
			e->nssl = arg;
			r = curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, OnSslContext);
			curl_easy_setopt(c, CURLOPT_SSL_CTX_DATA, e);
			// NSSL version 2 is SSLv3; everything else negotiates up to TLS 1.2
			curl_easy_setopt(c, CURLOPT_SSLVERSION, ctx->sslVersion == 2 ? (long)CURL_SSLVERSION_SSLv3 : (long)CURL_SSLVERSION_TLSv1_2);
			break;
		}

		case CURLOPT_SHARE:
		{
			CURLSH* hostShare = nullptr;
			if (Share* sh = Find(s_share, arg))
			{
				sh->lastEasy = handle;
				hostShare = sh->share;
			}
			r = curl_easy_setopt(c, CURLOPT_SHARE, hostShare);
			break;
		}

		case CURLOPT_WRITEFUNCTION:
			e->dump.LogOption("CURLOPT_WRITEFUNCTION", arg);
			e->writeFn = arg;
			break;
		case CURLOPT_WRITEDATA:
			e->dump.LogOption("CURLOPT_WRITEDATA", arg);
			e->writeData = arg;
			break;
		case CURLOPT_HEADERFUNCTION:
			e->dump.LogOption("CURLOPT_HEADERFUNCTION", arg);
			e->headerFn = arg;
			break;
		case CURLOPT_HEADERDATA:
			e->dump.LogOption("CURLOPT_HEADERDATA", arg);
			e->headerData = arg;
			break;
		case CURLOPT_READFUNCTION:
			e->dump.LogOption("CURLOPT_READFUNCTION", arg);
			e->readFn = arg;
			break;
		case CURLOPT_READDATA:
			e->dump.LogOption("CURLOPT_READDATA", arg);
			e->readData = arg;
			break;
		case CURLOPT_PROGRESSFUNCTION:
			e->dump.LogOption("CURLOPT_PROGRESSFUNCTION", arg);
			e->progressFn = arg;
			r = curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, OnProgress);
			curl_easy_setopt(c, CURLOPT_PROGRESSDATA, e);
			break;
		case CURLOPT_PROGRESSDATA:
			e->dump.LogOption("CURLOPT_PROGRESSDATA", arg);
			e->progressData = arg;
			break;
		case CURLOPT_SOCKOPTFUNCTION:
			e->sockoptFn = arg;
			r = curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, OnSockopt);
			curl_easy_setopt(c, CURLOPT_SOCKOPTDATA, e);
			break;
		case CURLOPT_SOCKOPTDATA:
			e->sockoptData = arg;
			break;

		default:
			cemuLog_logDebug(LogType::nlibcurl, "curl_easy_setopt(0x{:08x}, {}, 0x{:08x}): option not supported", handle, option, arg);
			break;
		}
		return r;
	}

	uint32 EasyPerform(MPTR handle)
	{
		Easy* e = Find(s_easy, handle);
		if (!e)
			return CURLE_BAD_FUNCTION_ARGUMENT;
		e->dump.BeginIfIdle();
		e->dump.Submit();
		SyncRequest(e);
		CURL* c = e->curl;
		return RunWithRelay([c]() { return (uint32)curl_easy_perform(c); });
	}

	uint32 EasyPause(MPTR handle, sint32 mask)
	{
		Easy* e = Find(s_easy, handle);
		if (!e)
			return CURLE_BAD_FUNCTION_ARGUMENT;
		CURL* c = e->curl;
		return RunWithRelay([c, mask]() { return (uint32)curl_easy_pause(c, mask); });
	}

	uint32 EasyGetinfo(MPTR handle, uint32 info, MPTR out)
	{
		Easy* e = Find(s_easy, handle);
		if (!e || !out)
			return CURLE_BAD_FUNCTION_ARGUMENT;
		CURLcode r;
		switch (info & CURLINFO_TYPEMASK)
		{
		case CURLINFO_STRING:
		{
			char* value = nullptr;
			r = curl_easy_getinfo(e->curl, (CURLINFO)info, &value);
			SetInfoString(e, info, r == CURLE_OK ? value : nullptr, MEMPTR<uint32be>(out));
			break;
		}
		case CURLINFO_LONG:
		{
			long value = 0;
			r = curl_easy_getinfo(e->curl, (CURLINFO)info, &value);
			*MEMPTR<uint32be>(out) = (uint32)value;
			break;
		}
		case CURLINFO_DOUBLE:
		{
			double value = 0.0;
			r = curl_easy_getinfo(e->curl, (CURLINFO)info, &value);
			*MEMPTR<uint64be>(out) = std::bit_cast<uint64>(value);
			break;
		}
		default:
			cemuLog_log(LogType::Force, "nlibcurl: curl_easy_getinfo 0x{:x} not supported", info);
			r = CURLE_UNKNOWN_OPTION;
			break;
		}
		return r;
	}

	// ---- multi ---------------------------------------------------------------

	MPTR MultiInit()
	{
		MPTR guest = GuestAllocZeroed(GUEST_MULTI_SIZE);
		if (!guest)
			return 0;
		Multi* m = Insert(s_multi, guest);
		m->multi = curl_multi_init();
		return guest;
	}

	uint32 MultiCleanup(MPTR handle)
	{
		std::unique_ptr<Multi> m = Remove(s_multi, handle);
		if (!m)
			return CURLM_BAD_HANDLE;
		uint32 r = curl_multi_cleanup(m->multi);
		GuestFree(m->msgBuffer);
		GuestFree(handle);
		return r;
	}

	uint32 MultiAddHandle(MPTR multiHandle, MPTR easyHandle)
	{
		Multi* m = Find(s_multi, multiHandle);
		if (!m)
			return CURLM_BAD_HANDLE;
		Easy* e = Find(s_easy, easyHandle);
		if (!e)
			return CURLM_BAD_EASY_HANDLE;
		e->dump.BeginIfIdle();
		e->dump.Submit();
		CURLMcode r = curl_multi_add_handle(m->multi, e->curl);
		if (r == CURLM_OK)
			m->members.push_back(easyHandle);
		return r;
	}

	uint32 MultiRemoveHandle(MPTR multiHandle, MPTR easyHandle)
	{
		Multi* m = Find(s_multi, multiHandle);
		if (!m)
			return CURLM_BAD_HANDLE;
		Easy* e = Find(s_easy, easyHandle);
		if (!e)
			return CURLM_BAD_EASY_HANDLE;
		CURLMcode r = curl_multi_remove_handle(m->multi, e->curl);
		if (r == CURLM_OK)
			m->members.erase(std::remove(m->members.begin(), m->members.end(), easyHandle), m->members.end());
		return r;
	}

	uint32 MultiPerform(MPTR handle, MEMPTR<uint32be> running)
	{
		Multi* m = Find(s_multi, handle);
		if (!m)
			return CURLM_BAD_HANDLE;
		for (MPTR member : m->members)
		{
			if (Easy* e = Find(s_easy, member))
				SyncRequest(e);
		}
		int count = 0;
		CURLMcode r = curl_multi_perform(m->multi, &count);
		if (running)
			*running = (uint32)count;
		return r;
	}

	uint32 MultiSetopt(MPTR handle, uint32 option, uint32 arg)
	{
		Multi* m = Find(s_multi, handle);
		if (!m)
			return CURLM_BAD_HANDLE;
		if (option == CURLMOPT_MAXCONNECTS)
			return curl_multi_setopt(m->multi, CURLMOPT_MAXCONNECTS, (long)arg);
		cemuLog_logDebug(LogType::nlibcurl, "curl_multi_setopt(0x{:08x}, {}, 0x{:08x}): option not supported", handle, option, arg);
		return CURLM_OK;
	}

	uint32 MultiTimeout(MPTR handle, MEMPTR<uint32be> timeout)
	{
		Multi* m = Find(s_multi, handle);
		if (!m || !timeout)
			return CURLM_BAD_HANDLE;
		long ms = -1;
		CURLMcode r = curl_multi_timeout(m->multi, &ms);
		*timeout = (uint32)(sint32)ms;
		return r;
	}

	// the host sockets libcurl wants polled, as console socket handles
	uint32 MultiFdset(MPTR handle, MEMPTR<nsysnet::GuestFdSet> readSet, MEMPTR<nsysnet::GuestFdSet> writeSet, MEMPTR<nsysnet::GuestFdSet> exceptSet, MEMPTR<sint32be> maxFd)
	{
		Multi* m = Find(s_multi, handle);
		if (!m)
			return CURLM_BAD_HANDLE;
		fd_set hostRead, hostWrite, hostExcept;
		FD_ZERO(&hostRead);
		FD_ZERO(&hostWrite);
		FD_ZERO(&hostExcept);
		int hostMax = -1;
		CURLMcode r = curl_multi_fdset(m->multi, &hostRead, &hostWrite, &hostExcept, &hostMax);

		nsysnet::GuestFdSet* guestSets[3] = { readSet.GetPtr(), writeSet.GetPtr(), exceptSet.GetPtr() };
		for (auto* gs : guestSets)
			if (gs)
				nsysnet::FdSetClear(gs);

		sint32 guestMax = -1;
		auto add = [&](SOCKET s, nsysnet::GuestFdSet* gs) {
			if (!gs)
				return;
			sint32 h = nsysnet::GuestHandleOfHostSocket(s);
			if (h < 0)
				h = nsysnet::AdoptHostSocket(s);
			if (h < 0)
				return;
			nsysnet::FdSetAdd(gs, h);
			guestMax = std::max(guestMax, h);
		};
		fd_set* hostSets[3] = { &hostRead, &hostWrite, &hostExcept };
		for (int i = 0; i < 3; i++)
		{
#if BOOST_OS_WINDOWS
			for (u_int k = 0; k < hostSets[i]->fd_count; k++)
				add(hostSets[i]->fd_array[k], guestSets[i]);
#else
			for (int s = 0; s <= hostMax; s++)
				if (FD_ISSET(s, hostSets[i]))
					add(s, guestSets[i]);
#endif
		}
		if (maxFd)
			*maxFd = guestMax;
		return r;
	}

	MPTR MultiInfoRead(MPTR handle, MEMPTR<sint32be> remaining)
	{
		Multi* m = Find(s_multi, handle);
		if (!m)
			return 0;
		int left = 0;
		CURLMsg* msg = curl_multi_info_read(m->multi, &left);
		if (remaining)
			*remaining = left;
		if (!msg)
			return 0;
		if (!m->msgBuffer)
			m->msgBuffer = GuestAlloc(sizeof(GuestMultiMsg));
		if (!m->msgBuffer)
			return 0;
		MPTR easy = 0;
		for (MPTR member : m->members)
		{
			Easy* e = Find(s_easy, member);
			if (e && e->curl == msg->easy_handle)
			{
				easy = member;
				break;
			}
		}
		GuestMultiMsg* out = (GuestMultiMsg*)memory_getPointerFromVirtualOffset(m->msgBuffer);
		out->msg = (uint32)msg->msg;
		out->easy = easy;
		out->result = (uint32)msg->data.result;
		return m->msgBuffer;
	}

	// ---- share ---------------------------------------------------------------

	MPTR ShareInit()
	{
		MPTR guest = GuestAllocZeroed(GUEST_SHARE_SIZE);
		if (!guest)
			return 0;
		Share* sh = Insert(s_share, guest);
		sh->share = curl_share_init();
		curl_share_setopt(sh->share, CURLSHOPT_USERDATA, sh);
		return guest;
	}

	uint32 ShareSetopt(MPTR handle, uint32 option, uint32 arg)
	{
		Share* sh = Find(s_share, handle);
		if (!sh)
			return CURLSHE_INVALID;
		switch (option)
		{
		case CURLSHOPT_USERDATA:
			sh->userData = arg;
			return CURLSHE_OK;
		case CURLSHOPT_LOCKFUNC:
			sh->lockFn = arg;
			return curl_share_setopt(sh->share, CURLSHOPT_LOCKFUNC, OnShareLock);
		case CURLSHOPT_UNLOCKFUNC:
			sh->unlockFn = arg;
			return curl_share_setopt(sh->share, CURLSHOPT_UNLOCKFUNC, OnShareUnlock);
		case CURLSHOPT_SHARE:
		case CURLSHOPT_UNSHARE:
			return curl_share_setopt(sh->share, (CURLSHoption)option, (int)arg);
		default:
			cemuLog_log(LogType::Force, "nlibcurl: curl_share_setopt option {} not supported", option);
			return CURLSHE_OK;
		}
	}

	uint32 ShareCleanup(MPTR handle)
	{
		std::unique_ptr<Share> sh = Remove(s_share, handle);
		if (!sh)
			return CURLSHE_OK;
		curl_share_cleanup(sh->share);
		GuestFree(handle);
		return CURLSHE_OK;
	}

	// ---- module ----------------------------------------------------------------

	class NlibcurlModule : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "nlibcurl";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(GlobalInitMem, "nlibcurl", "curl_global_init_mem", LogType::nlibcurl);
			cafeExportRegisterFunc(GlobalInit, "nlibcurl", "curl_global_init", LogType::nlibcurl);

			cafeExportRegisterFunc(SlistAppend, "nlibcurl", "curl_slist_append", LogType::nlibcurl);
			cafeExportRegisterFunc(SlistFreeAll, "nlibcurl", "curl_slist_free_all", LogType::nlibcurl);
			cafeExportRegisterFunc(EasyStrerror, "nlibcurl", "curl_easy_strerror", LogType::nlibcurl);

			cafeExportRegisterFunc(ShareInit, "nlibcurl", "curl_share_init", LogType::nlibcurl);
			cafeExportRegisterFunc(ShareSetopt, "nlibcurl", "curl_share_setopt", LogType::nlibcurl);
			cafeExportRegisterFunc(ShareCleanup, "nlibcurl", "curl_share_cleanup", LogType::nlibcurl);

			cafeExportRegisterFunc(MultiInit, "nlibcurl", "curl_multi_init", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiAddHandle, "nlibcurl", "curl_multi_add_handle", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiPerform, "nlibcurl", "curl_multi_perform", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiInfoRead, "nlibcurl", "curl_multi_info_read", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiRemoveHandle, "nlibcurl", "curl_multi_remove_handle", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiSetopt, "nlibcurl", "curl_multi_setopt", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiFdset, "nlibcurl", "curl_multi_fdset", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiCleanup, "nlibcurl", "curl_multi_cleanup", LogType::nlibcurl);
			cafeExportRegisterFunc(MultiTimeout, "nlibcurl", "curl_multi_timeout", LogType::nlibcurl);

			cafeExportRegisterFunc(EasyInit, "nlibcurl", "mw_curl_easy_init", LogType::nlibcurl);
			cafeExportRegisterFunc(EasyInit, "nlibcurl", "curl_easy_init", LogType::nlibcurl);
			cafeExportRegisterFunc(EasyReset, "nlibcurl", "curl_easy_reset", LogType::nlibcurl);
			cafeExportRegisterFunc(EasySetopt, "nlibcurl", "curl_easy_setopt", LogType::nlibcurl);
			cafeExportRegisterFunc(EasyGetinfo, "nlibcurl", "curl_easy_getinfo", LogType::nlibcurl);
			cafeExportRegisterFunc(EasyPerform, "nlibcurl", "curl_easy_perform", LogType::nlibcurl);
			cafeExportRegisterFunc(EasyCleanup, "nlibcurl", "curl_easy_cleanup", LogType::nlibcurl);
			cafeExportRegisterFunc(EasyPause, "nlibcurl", "curl_easy_pause", LogType::nlibcurl);
		}
	};

	NlibcurlModule s_module;

	COSModule* GetModule()
	{
		return &s_module;
	}
}
