// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_boss is the client side of BOSS, the console's background download
// service (SpotPass). Games build C++ objects in their own memory - task
// settings, tasks, storages, data files - and the library turns calls on them
// into ioctlv requests to /dev/boss, which our IOSU module (boss_service.cpp)
// serves. The object layouts and the request formats are shared with that
// module through Cafe/IOSU/nn/boss/boss_common.h.
//
// Every request starts with the task or storage key: the account's
// persistent id followed by the task id, or by the storage directory name and
// kind (plus the data file name for NsData).

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/OS/libs/nn_client_service.h"
#include "Cafe/OS/libs/nn_act/nn_act.h"
#include "Cafe/OS/libs/nn_ac/nn_ac.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Common/CafeString.h"
#include "Cafe/IOSU/nn/boss/boss_common.h"
#include "nn_boss.h"

namespace nn::boss
{
	namespace
	{
		using ServiceCall = IPCServiceClient::IPCServiceCall;

		constexpr BossResult kOk = BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_BOSS, 0x80);
		constexpr BossResult kInvalidParam = BUILD_NN_RESULT(NN_RESULT_LEVEL_LVL6, NN_RESULT_MODULE_NN_BOSS, 0x3780);
		constexpr BossResult kDataListFailed = BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_BOSS, 0x20300);
		constexpr BossResult kNoFreeSlot = 0xA0220D00;
		constexpr BossResult kNotConnected = 0xA0223A00;

		IPCServiceClient g_boss;
		SysAllocator<uint8, 0x10000, 64> g_ipcMemory;
		SysAllocator<coreinit::OSMutex> g_initLock;
		sint32 g_initCount = 0;
		bool g_initialized = false;

		// objects created by a constructor called with this == nullptr live on
		// the default heap behind a small header
		constexpr uint32 kHeapTag = 0x424F5353; // "BOSS"

		template<typename T>
		T* HeapNew()
		{
			auto* block = (uint32be*)coreinit::_weak_MEMAllocFromDefaultHeapEx(sizeof(T) + 8, 8);
			block[0] = kHeapTag;
			block[1] = (uint32)sizeof(T);
			return (T*)(block + 2);
		}

		void HeapDelete(void* obj)
		{
			if (!obj)
				return;
			auto* block = (uint32be*)obj - 2;
			if (block[0] != kHeapTag)
			{
				cemuLog_log(LogType::Force, "nn_boss: object at {:08x} was not allocated by nn_boss", MEMPTR<void>(obj).GetMPTR());
				return;
			}
			block[0] = 0;
			coreinit::_weak_MEMFreeToDefaultHeap(block);
		}

		template<typename T>
		T* ThisOrNew(T* obj)
		{
			return obj ? obj : HeapNew<T>();
		}

		// PPC-callable destructor for a vtable slot
		template<auto fn>
		MEMPTR<void> DtorEntry()
		{
			return MEMPTR<void>(makeCallableExport<fn>());
		}

		ServiceCall BeginCall(BossCommandId cmd)
		{
			return g_boss.Begin(0, stdx::to_underlying(cmd));
		}
	}

	// ---- library -------------------------------------------------------------

	BossResult Initialize()
	{
		coreinit::OSLockMutex(g_initLock.GetPtr());
		if (g_initCount++ == 0)
		{
			g_boss.Initialize("/dev/boss", g_ipcMemory.GetPtr(), g_ipcMemory.GetByteSize());
			g_initialized = true;
		}
		coreinit::OSUnlockMutex(g_initLock.GetPtr());
		return 0;
	}

	uint32 IsInitialized()
	{
		return g_initialized ? 1 : 0;
	}

	void Finalize()
	{
		coreinit::OSLockMutex(g_initLock.GetPtr());
		if (g_initCount <= 0)
			cemuLog_log(LogType::Force, "nn_boss: Finalize without Initialize");
		else if (g_initCount == 1)
		{
			g_boss.Shutdown();
			g_initialized = false;
		}
		g_initCount--;
		coreinit::OSUnlockMutex(g_initLock.GetPtr());
	}

	uint32 GetBossState()
	{
		return 7; // service running
	}

	// ---- small value types (declared in boss_common.h) ----------------------

	bool TitleId::IsValid(TitleId* _thisptr)
	{
		return _thisptr->u64 != 0;
	}

	TitleId* TitleId::ctorDefault(TitleId* _thisptr)
	{
		return ctorFromTitleId(_thisptr, 0);
	}

	TitleId* TitleId::ctorFromTitleId(TitleId* _thisptr, uint64 titleId)
	{
		_thisptr = ThisOrNew(_thisptr);
		_thisptr->u64 = titleId;
		return _thisptr;
	}

	TitleId* TitleId::ctorCopy(TitleId* _thisptr, TitleId* titleId)
	{
		return ctorFromTitleId(_thisptr, titleId->u64);
	}

	bool TitleId::operator_ne(TitleId* _thisptr, TitleId* titleId)
	{
		return _thisptr->u64 != titleId->u64;
	}

	TaskId* TaskId::ctorDefault(TaskId* _thisptr)
	{
		_thisptr = ThisOrNew(_thisptr);
		_thisptr->id.data[0] = '\0';
		return _thisptr;
	}

	TaskId* TaskId::ctorFromString(TaskId* _thisptr, const char* taskId)
	{
		_thisptr = ThisOrNew(_thisptr);
		_thisptr->id.assign(taskId);
		return _thisptr;
	}

	DirectoryName* DirectoryName::ctor(DirectoryName* _thisptr)
	{
		_thisptr = ThisOrNew(_thisptr);
		_thisptr->name2.ClearAllBytes();
		return _thisptr;
	}

	const char* DirectoryName::operator_const_char(DirectoryName* _thisptr)
	{
		return _thisptr->name2.c_str();
	}

	DataName* DataName::ctor(DataName* _this)
	{
		_this = ThisOrNew(_this);
		_this->name.ClearAllBytes();
		return _this;
	}

	const char* DataName::operator_const_char(DataName* _this)
	{
		return _this->name.c_str();
	}

	// ---- Title / Account ----------------------------------------------------

	Title* Title::ctor(Title* _this)
	{
		_this = ThisOrNew(_this);
		*_this = {};
		_this->vTablePtr = s_titleVTable.GetPtr();
		return _this;
	}

	void Title::dtor(Title* _this, uint32 options)
	{
		if (_this && (options & 1))
			HeapDelete(_this);
	}

	void Title::InitVTable()
	{
		s_titleVTable->rtti.ptr = nullptr;
		s_titleVTable->dtor.ptr = DtorEntry<&Title::dtor>();
	}

	BossAccount* BossAccount::ctor(BossAccount* _this, uint32 accountId)
	{
		_this = ThisOrNew(_this);
		_this->accountId = accountId;
		_this->vTablePtr = s_VTable.GetPtr();
		return _this;
	}

	void BossAccount::dtor(BossAccount* _this, uint32 options)
	{
		if (_this && (options & 1))
			HeapDelete(_this);
	}

	void BossAccount::InitVTable()
	{
		s_VTable->rtti.ptr = nullptr;
		s_VTable->dtor.ptr = DtorEntry<&BossAccount::dtor>();
	}

	// ---- task settings ------------------------------------------------------

	TaskSetting* TaskSetting::ctor(TaskSetting* _thisptr)
	{
		_thisptr = ThisOrNew(_thisptr);
		_thisptr->vTablePtr = s_VTable.GetPtr();
		InitializeSetting(_thisptr);
		return _thisptr;
	}

	void TaskSetting::dtor(TaskSetting* _this, uint32 options)
	{
		if (_this && (options & 1))
			HeapDelete(_this);
	}

	bool TaskSetting::IsPrivileged(TaskSetting* _thisptr)
	{
		switch ((TaskType)_thisptr->taskType)
		{
		case TaskType::RawDlTaskSetting_1:
		case TaskType::RawDlTaskSetting_9:
		case TaskType::PlayLogUploadTaskSetting:
			return true;
		default:
			return false;
		}
	}

	void TaskSetting::InitializeSetting(TaskSetting* _thisptr)
	{
		// clears the whole setting block (not the vtable pointer behind it)
		memset((void*)_thisptr, 0, 0x1000);
		_thisptr->priority = 125;
		_thisptr->intervalB = 8 * 60 * 60;            // every 8 hours
		_thisptr->lifeTime.high = 0;
		_thisptr->lifeTime.low = 90 * 24 * 60 * 60;   // 90 days
	}

	void TaskSetting::InitVTable()
	{
		s_VTable->rtti.ptr = nullptr;
		s_VTable->dtor.ptr = DtorEntry<&TaskSetting::dtor>();
		s_VTable->RegisterPreprocess.ptr = nullptr;
		s_VTable->unk1.ptr = nullptr;
	}

	NetTaskSetting* NetTaskSetting::ctor(NetTaskSetting* _thisptr)
	{
		_thisptr = ThisOrNew(_thisptr);
		TaskSetting::ctor(_thisptr);
		_thisptr->vTablePtr = s_VTable.GetPtr();
		_thisptr->httpTimeout = 120;
		return _thisptr;
	}

	BossResult NetTaskSetting::AddCaCert(NetTaskSetting* _thisptr, const char* name)
	{
		if (!name || strnlen(name, 0x80) >= 0x80)
			return kInvalidParam;
		for (auto& cert : _thisptr->caCert)
		{
			if (cert.empty())
			{
				cert.assign(name);
				return kOk;
			}
		}
		return kNoFreeSlot;
	}

	BossResult NetTaskSetting::SetServiceToken(NetTaskSetting* _thisptr, const uint8* serviceToken)
	{
		memcpy(_thisptr->serviceToken.data, serviceToken, _thisptr->serviceToken.Size());
		return kOk;
	}

	BossResult NetTaskSetting::AddInternalCaCert(NetTaskSetting* _thisptr, char certId)
	{
		for (auto& slot : _thisptr->internalCaCert)
		{
			if (slot == 0)
			{
				slot = (uint8)certId;
				return kOk;
			}
		}
		return kNoFreeSlot; // three at most
	}

	void NetTaskSetting::SetInternalClientCert(NetTaskSetting* _thisptr, char certId)
	{
		_thisptr->internalClientCert = (uint8)certId;
	}

	void NetTaskSetting::InitVTable()
	{
		s_VTable->rtti.ptr = nullptr;
		s_VTable->dtor.ptr = DtorEntry<&NetTaskSetting::dtor>();
		s_VTable->RegisterPreprocess.ptr = nullptr;
		s_VTable->unk1.ptr = nullptr;
	}

	NbdlTaskSetting* NbdlTaskSetting::ctor(NbdlTaskSetting* _thisptr)
	{
		_thisptr = ThisOrNew(_thisptr);
		NetTaskSetting::ctor(_thisptr);
		_thisptr->vTablePtr = s_VTable.GetPtr();
		return _thisptr;
	}

	BossResult NbdlTaskSetting::Initialize(NbdlTaskSetting* _thisptr, const char* bossCode, uint64 directorySizeLimit, const char* bossDirectory)
	{
		auto& nbdl = _thisptr->nbdl;
		if (!bossCode || !nbdl.bossCode.CanHoldString(bossCode))
			return kInvalidParam;
		if (bossDirectory && !nbdl.bossDirectory.CanHoldString(bossDirectory)) // optional
			return kInvalidParam;
		nbdl.bossCode.assign(bossCode);
		if (bossDirectory)
			nbdl.bossDirectory.assign(bossDirectory);
		nbdl.directorySizeLimitHigh = (uint32)(directorySizeLimit >> 32);
		nbdl.directorySizeLimitLow = (uint32)directorySizeLimit;
		_thisptr->taskType = TaskType::NbdlTaskSetting;
		return kOk;
	}

	BossResult NbdlTaskSetting::SetFileName(NbdlTaskSetting* _thisptr, const char* fileName)
	{
		if (!fileName || !_thisptr->nbdl.fileName.CanHoldString(fileName))
			return kInvalidParam;
		_thisptr->nbdl.fileName.assign(fileName);
		return kOk;
	}

	void NbdlTaskSetting::InitVTable()
	{
		s_VTable->rtti.ptr = nullptr;
		s_VTable->dtor.ptr = DtorEntry<&NbdlTaskSetting::dtor>();
		s_VTable->RegisterPreprocess.ptr = nullptr;
		s_VTable->unk1.ptr = nullptr;
		s_VTable->rttiNetTaskSetting.ptr = nullptr;
	}

	RawUlTaskSetting* RawUlTaskSetting::ctor(RawUlTaskSetting* _thisptr)
	{
		_thisptr = ThisOrNew(_thisptr);
		NetTaskSetting::ctor(_thisptr);
		_thisptr->vTablePtr = s_VTable.GetPtr();
		_thisptr->ukRaw1 = 0;
		_thisptr->ukRaw2 = 0;
		_thisptr->ukRaw3 = 0;
		memset(_thisptr->rawSpace, 0, sizeof(_thisptr->rawSpace));
		return _thisptr;
	}

	void RawUlTaskSetting::dtor(RawUlTaskSetting* _this, uint32 options)
	{
		if (_this && (options & 1))
			HeapDelete(_this);
	}

	void RawUlTaskSetting::InitVTable()
	{
		s_VTable->rtti.ptr = nullptr;
		s_VTable->dtor.ptr = DtorEntry<&RawUlTaskSetting::dtor>();
		s_VTable->RegisterPreprocess.ptr = nullptr;
		s_VTable->unk1.ptr = nullptr;
		s_VTable->rttiNetTaskSetting.ptr = nullptr;
	}

	RawDlTaskSetting* RawDlTaskSetting::ctor(RawDlTaskSetting* _thisptr)
	{
		_thisptr = ThisOrNew(_thisptr);
		NetTaskSetting::ctor(_thisptr);
		_thisptr->vTablePtr = s_VTable.GetPtr();
		return _thisptr;
	}

	BossResult RawDlTaskSetting::Initialize(RawDlTaskSetting* _thisptr, const char* url, bool newArrival, bool led, const char* fileName, const char* bossDirectory)
	{
		auto& rawDl = _thisptr->rawDl;
		if (!url || !_thisptr->url.CanHoldString(url))
			return kInvalidParam;
		if (fileName && !rawDl.fileName.CanHoldString(fileName)) // optional
			return kInvalidParam;
		if (!bossDirectory || !rawDl.bossDirectory.CanHoldString(bossDirectory))
			return kInvalidParam;
		_thisptr->url.assign(url);
		rawDl.fileName.assign(fileName ? fileName : "rawcontent.dat");
		rawDl.bossDirectory.assign(bossDirectory);
		rawDl.newArrival = newArrival ? 1 : 0;
		rawDl.led = led ? 1 : 0;
		_thisptr->taskType = TaskType::RawDlTaskSetting_3;
		return kOk;
	}

	void RawDlTaskSetting::InitVTable()
	{
		s_VTable->rtti.ptr = nullptr;
		s_VTable->dtor.ptr = DtorEntry<&RawDlTaskSetting::dtor>();
		s_VTable->RegisterPreprocess.ptr = nullptr;
		s_VTable->unk1.ptr = nullptr;
		s_VTable->rttiNetTaskSetting.ptr = nullptr;
	}

	PlayReportSetting* PlayReportSetting::ctor(PlayReportSetting* _this)
	{
		_this = ThisOrNew(_this);
		RawUlTaskSetting::ctor(_this);
		_this->vTablePtr = s_VTable.GetPtr();
		_this->ukn1210_ptr = nullptr;
		_this->ukn1214_size = 0;
		_this->ukPlay3 = 0;
		_this->ukPlay4 = 0;
		return _this;
	}

	void PlayReportSetting::dtor(PlayReportSetting* _this, uint32 options)
	{
		// the report buffer belongs to the game
		if (_this && (options & 1))
			HeapDelete(_this);
	}

	void PlayReportSetting::Initialize(PlayReportSetting* _this, uint8* ptr, uint32 size)
	{
		if (!ptr || size == 0 || size > 0x19000)
		{
			cemuLog_logDebug(LogType::Force, "nn_boss: PlayReportSetting::Initialize with an invalid buffer");
			return;
		}
		ptr[0] = 0; // empty report
		_this->taskType = TaskType::PlayReportSetting;
		_this->mode |= 3;
		_this->permission |= 0xA;
		_this->rawUl.optionValue |= 2;
		_this->ukn1210_ptr = ptr;
		_this->ukn1214_size = size;
		_this->ukPlay3 = 0;
		_this->ukPlay4 = 0;
		AddInternalCaCert(_this, 102);
		SetInternalClientCert(_this, 1);
	}

	bool PlayReportSetting::Set(PlayReportSetting* _this, const char* keyname, uint32 value)
	{
		return true; // play reports are never sent
	}

	void PlayReportSetting::InitVTable()
	{
		s_VTable->rtti.ptr = nullptr;
		s_VTable->dtor.ptr = DtorEntry<&PlayReportSetting::dtor>();
		s_VTable->RegisterPreprocess.ptr = nullptr;
		s_VTable->unk1.ptr = nullptr;
		s_VTable->rttiNetTaskSetting.ptr = nullptr;
	}

	// ---- tasks --------------------------------------------------------------

	namespace task
	{
		struct VTable
		{
			VTableEntry rtti;
			VTableEntry dtor;
			VTableEntry rttiBase; // only in the derived task classes
		};

		struct Task
		{
			/* +0x00 */ uint32be persistentId;
			/* +0x04 */ uint32be unk04;
			/* +0x08 */ TaskId taskId;
			/* +0x10 */ TitleId titleId;
			/* +0x18 */ MEMPTR<VTable> vTablePtr;
			/* +0x1C */ uint32be pad;
		};
		static_assert(sizeof(Task) == 0x20);
		using PrivilegedTask = Task; // same layout, other vtable
		using AlmightyTask = Task;

		SysAllocator<VTable> s_taskVTable;
		SysAllocator<VTable> s_privilegedVTable;
		SysAllocator<VTable> s_almightyVTable;

		void WriteKey(ServiceCall& call, const Task* t)
		{
			call.WriteParam<uint32be>(t->persistentId);
			call.WriteParam<TaskId>(t->taskId);
		}

		Task* Construct(Task* t, VTable* vtable)
		{
			t = ThisOrNew(t);
			t->persistentId = 0;
			t->vTablePtr = vtable;
			TaskId::ctorDefault(&t->taskId);
			TitleId::ctorFromTitleId(&t->titleId, 0);
			return t;
		}

		BossResult InitWithPersistentId(Task* t, const char* taskId, uint32 persistentId)
		{
			if (!taskId || !t->taskId.id.CanHoldString(taskId))
				return kInvalidParam;
			t->persistentId = persistentId;
			t->taskId.id.assign(taskId);
			return kOk;
		}

		BossResult InitWithSlot(Task* t, uint8 slot, const char* taskId)
		{
			return InitWithPersistentId(t, taskId, slot ? act::GetPersistentIdEx(slot) : 0);
		}

		BossResult InitCommon(Task* t, const char* taskId)
		{
			return InitWithPersistentId(t, taskId, 0);
		}

		Task* CtorPersistentId(Task* t, const char* taskId, uint32 persistentId)
		{
			t = Construct(t, s_taskVTable.GetPtr());
			InitWithPersistentId(t, taskId, persistentId);
			return t;
		}

		Task* CtorSlot(Task* t, uint8 slot, const char* taskId)
		{
			t = Construct(t, s_taskVTable.GetPtr());
			InitWithSlot(t, slot, taskId);
			return t;
		}

		Task* CtorId(Task* t, const char* taskId)
		{
			t = Construct(t, s_taskVTable.GetPtr());
			InitCommon(t, taskId);
			return t;
		}

		Task* CtorEmpty(Task* t)
		{
			t = Construct(t, s_taskVTable.GetPtr());
			memset(&t->taskId, 0, sizeof(TaskId));
			return t;
		}

		void Dtor(Task* t, uint32 options)
		{
			if (t && (options & 1))
				HeapDelete(t);
		}

		PrivilegedTask* CtorPrivileged(PrivilegedTask* t)
		{
			t = CtorEmpty(t);
			t->vTablePtr = s_privilegedVTable.GetPtr();
			return t;
		}

		AlmightyTask* CtorAlmighty(AlmightyTask* t)
		{
			t = CtorPrivileged(t);
			t->vTablePtr = s_almightyVTable.GetPtr();
			return t;
		}

		uint32 InitAlmighty(AlmightyTask* t, TitleId* titleId, const char* taskId, uint32 accountId)
		{
			if (!t)
				return kInvalidParam;
			t->persistentId = accountId;
			t->titleId.u64 = titleId->u64;
			t->taskId.id.assign(taskId);
			return kOk;
		}

		BossResult Run(Task* t, bool foreground)
		{
			uint8be connected = 0;
			nn_ac::IsApplicationConnected(&connected);
			if (foreground && !connected)
				return kNotConnected;
			ServiceCall call = BeginCall(BossCommandId::TaskRun);
			WriteKey(call, t);
			call.WriteParam<uint8>(foreground ? 1 : 0);
			return call.Submit();
		}

		BossResult StartScheduling(Task* t, uint8 runNow)
		{
			ServiceCall call = BeginCall(BossCommandId::TaskStartScheduling);
			WriteKey(call, t);
			call.WriteParam<uint8be>(runNow);
			return call.Submit();
		}

		nnResult StopScheduling(Task* t)
		{
			ServiceCall call = BeginCall(BossCommandId::TaskStopScheduling);
			WriteKey(call, t);
			return call.Submit();
		}

		bool IsRegistered(Task* t)
		{
			ServiceCall call = BeginCall(BossCommandId::TaskIsRegistered);
			WriteKey(call, t);
			if (NN_RESULT_IS_FAILURE(call.Submit()))
				return false;
			return call.ReadResponse<uint8be>() != 0;
		}

		bool Wait(Task* t, uint32 timeout, TaskWaitState waitState)
		{
			ServiceCall call = BeginCall(BossCommandId::TaskWaitA);
			WriteKey(call, t);
			call.WriteParam<betype<TaskWaitState>>(waitState);
			call.WriteParam<uint32be>(timeout);
			if (NN_RESULT_IS_FAILURE(call.Submit()))
				return false;
			return call.ReadResponse<uint8be>() != 0; // false on timeout
		}

		BossResult SendSetting(Task* t, TaskSetting* setting, BossCommandId cmd)
		{
			ServiceCall call = BeginCall(cmd);
			WriteKey(call, t);
			call.WriteParamBuffer(setting, sizeof(TaskSettingCore));
			return call.Submit();
		}

		BossResult Register(Task* t, TaskSetting* setting)
		{
			if (!setting)
			{
				cemuLog_logDebug(LogType::Force, "nn_boss: Task::Register without a setting");
				return 0;
			}
			return SendSetting(t, setting, BossCommandId::TaskRegisterA);
		}

		BossResult RegisterForImmediateRun(Task* t, TaskSetting* setting)
		{
			return SendSetting(t, setting, BossCommandId::TaskRegisterForImmediateRunA);
		}

		BossResult Unregister(Task* t)
		{
			ServiceCall call = BeginCall(BossCommandId::TaskUnregister);
			WriteKey(call, t);
			return call.Submit();
		}

		// queries that answer with the execution count followed by a value
		template<typename TValue>
		TValue QueryCounted(Task* t, BossCommandId cmd, uint32be* executionCount, TValue onError)
		{
			ServiceCall call = BeginCall(cmd);
			WriteKey(call, t);
			if (NN_RESULT_IS_FAILURE(call.Submit()))
				return onError;
			const uint32 count = call.ReadResponse<uint32be>();
			if (executionCount)
				*executionCount = count;
			return call.ReadResponse<betype<TValue>>();
		}

		TaskTurnState GetTurnState(Task* t, uint32be* executionCount)
		{
			return QueryCounted<TaskTurnState>(t, BossCommandId::TaskGetTurnState, executionCount, TaskTurnState::Ukn);
		}

		uint64 GetContentLength(Task* t, uint32be* executionCount)
		{
			return QueryCounted<uint64>(t, BossCommandId::TaskGetContentLength, executionCount, 0);
		}

		uint64 GetProcessedLength(Task* t, uint32be* executionCount)
		{
			return QueryCounted<uint64>(t, BossCommandId::TaskGetProcessedLength, executionCount, 0);
		}

		uint32 GetHttpStatusCode(Task* t, uint32be* executionCount)
		{
			return QueryCounted<uint32>(t, BossCommandId::TaskGetHttpStatusCodeA, executionCount, 0);
		}

		void InitVTables()
		{
			for (VTable* vt : {s_taskVTable.GetPtr(), s_privilegedVTable.GetPtr(), s_almightyVTable.GetPtr()})
			{
				vt->rtti.ptr = nullptr;
				vt->dtor.ptr = DtorEntry<&Dtor>();
				vt->rttiBase.ptr = nullptr;
			}
		}
	}

	// ---- storages and data files --------------------------------------------

	namespace storage
	{
		struct VTable
		{
			VTableEntry rtti;
			VTableEntry dtor;
			VTableEntry rttiBase; // AlmightyStorage only
		};

		struct Storage
		{
			/* +0x00 */ uint32be persistentId;
			/* +0x04 */ uint32be kind; // StorageKind
			/* +0x08 */ uint8 unk08[3];
			/* +0x0B */ DirectoryName directory;
			/* +0x13 */ uint8 unk13[5];
			/* +0x18 */ TitleId titleId;
			/* +0x20 */ MEMPTR<VTable> vTablePtr;
			/* +0x24 */ uint32be unk24;
		};
		static_assert(sizeof(Storage) == 0x28);
		static_assert(offsetof(Storage, directory) == 0x0B);
		static_assert(offsetof(Storage, titleId) == 0x18);
		using AlmightyStorage = Storage;

		struct NsData
		{
			/* +0x00 */ DataName name;
			/* +0x20 */ Storage storage;
			/* +0x48 */ uint64be position;
			/* +0x50 */ MEMPTR<VTable> vTablePtr;
			/* +0x54 */ uint32 unk54;
		};
		static_assert(sizeof(NsData) == 0x58);

		SysAllocator<VTable> s_storageVTable;
		SysAllocator<VTable> s_almightyVTable;
		SysAllocator<VTable> s_nsDataVTable;

		void WriteKey(ServiceCall& call, const Storage* s)
		{
			call.WriteParam<uint32be>(s->persistentId);
			call.WriteParam<DirectoryName>(s->directory);
			call.WriteParam<betype<StorageKind>>((StorageKind)(uint32)s->kind);
		}

		void WriteKey(ServiceCall& call, const NsData* d)
		{
			WriteKey(call, &d->storage);
			call.WriteParam<DataName>(d->name);
		}

		Storage* Ctor(Storage* s)
		{
			s = ThisOrNew(s);
			s->vTablePtr = s_storageVTable.GetPtr();
			s->titleId.u64 = 0;
			return s;
		}

		void Finalize(Storage* s)
		{
			memset((void*)s, 0, sizeof(Storage));
		}

		void Dtor(Storage* s, uint32 options)
		{
			if (!s)
				return;
			Finalize(s);
			if (options & 1)
				HeapDelete(s);
		}

		BossResult InitWithAccount(Storage* s, const char* directory, uint32 accountId, StorageKind kind)
		{
			if (!directory)
				return kInvalidParam;
			s->kind = (uint32)kind;
			s->titleId.u64 = 0;
			s->directory.name2.assign(directory);
			s->persistentId = accountId;
			return kOk;
		}

		BossResult InitCommon(Storage* s, const char* directory, StorageKind kind)
		{
			return InitWithAccount(s, directory, 0, kind);
		}

		bool Exist(Storage* s)
		{
			ServiceCall call = BeginCall(BossCommandId::StorageExist);
			WriteKey(call, s);
			if (NN_RESULT_IS_FAILURE(call.Submit()))
				return false;
			return call.ReadResponse<uint8be>() != 0;
		}

		nnResult GetDataList(Storage* s, DataName* list, sint32 maxEntries, uint32be* countOut, uint32 startIndex)
		{
			ServiceCall call = BeginCall(BossCommandId::StorageGetDataList);
			WriteKey(call, s);
			call.WriteResponseBuffer(list, sizeof(DataName) * maxEntries);
			call.WriteParam<uint32be>(startIndex);
			const nnResult r = call.Submit();
			const uint32 count = call.ReadResponse<uint32be>();
			const bool listed = call.ReadResponse<uint8be>() != 0;
			if (!listed)
				return kDataListFailed;
			if (countOut)
				*countOut = count;
			return r;
		}

		AlmightyStorage* CtorAlmighty(AlmightyStorage* s)
		{
			s = Ctor(s);
			s->vTablePtr = s_almightyVTable.GetPtr();
			return s;
		}

		uint32 InitAlmighty(AlmightyStorage* s, TitleId* titleId, const char* directory, uint32 accountId, StorageKind kind)
		{
			if (!s)
				return kInvalidParam;
			s->persistentId = accountId;
			s->kind = (uint32)kind;
			s->titleId.u64 = titleId->u64;
			s->directory.name2.assign(directory);
			return kOk;
		}

		// NsData: one file in a storage

		NsData* NsDataCtor(NsData* d)
		{
			d = ThisOrNew(d);
			d->vTablePtr = s_nsDataVTable.GetPtr();
			DataName::ctor(&d->name);
			Ctor(&d->storage);
			d->position = 0;
			return d;
		}

		void NsDataDtor(NsData* d, uint32 options)
		{
			if (!d)
				return;
			Dtor(&d->storage, 0);
			if (options & 1)
				HeapDelete(d);
		}

		BossResult NsDataInit(NsData* d, Storage* s, const char* dataName)
		{
			// RawDl storages hold a single file, which is the default name
			const bool rawDl = s->kind == (uint32)StorageKind::StorageRawDl;
			if (!dataName && !rawDl)
				return kInvalidParam;
			d->storage = *s;
			d->name.name.assign(dataName ? dataName : "rawcontent.dat");
			d->position = 0;
			return kOk;
		}

		BossResult DeleteRealFile(NsData* d)
		{
			ServiceCall call = BeginCall(BossCommandId::NsDataDeleteFile);
			WriteKey(call, d);
			return call.Submit();
		}

		BossResult DeleteRealFileWithHistory(NsData* d)
		{
			ServiceCall call = BeginCall(BossCommandId::NsDataDeleteFileWithHistory);
			WriteKey(call, d);
			return call.Submit();
		}

		uint32 NsDataExist(NsData* d)
		{
			ServiceCall call = BeginCall(BossCommandId::NsDataExist);
			WriteKey(call, d);
			if (NN_RESULT_IS_FAILURE(call.Submit()))
				return 0;
			return call.ReadResponse<uint8be>();
		}

		uint64 GetSize(NsData* d)
		{
			ServiceCall call = BeginCall(BossCommandId::NsDataGetSize);
			WriteKey(call, d);
			if (NN_RESULT_IS_FAILURE(call.Submit()))
				return 0;
			return call.ReadResponse<uint64be>();
		}

		uint64 GetCreatedTime(NsData* d)
		{
			return 0; // not tracked
		}

		sint32 ReadWithSizeOut(NsData* d, uint64be* sizeOut, uint8* buffer, sint32 length)
		{
			ServiceCall call = BeginCall(BossCommandId::NsDataRead);
			WriteKey(call, d);
			call.WriteResponseBuffer(buffer, length);
			call.WriteParam<uint64be>(d->position);
			const nnResult r = call.Submit();
			if (NN_RESULT_IS_FAILURE(r))
				return r;
			const uint64 got = call.ReadResponse<uint64be>();
			cemu_assert(got <= (uint64)(uint32)length);
			d->position += got;
			if (sizeOut)
				*sizeOut = got;
			return r;
		}

		sint32 Read(NsData* d, uint8* buffer, sint32 length)
		{
			uint64be ignored = 0;
			return ReadWithSizeOut(d, &ignored, buffer, length);
		}

		enum class SeekOrigin : uint32
		{
			Begin = 0,
			Current = 1,
			End = 2,
		};

		BossResult Seek(NsData* d, sint64 offset, SeekOrigin origin)
		{
			switch (origin)
			{
			case SeekOrigin::Begin: d->position = (uint64)offset; break;
			case SeekOrigin::Current: d->position = d->position + (uint64)offset; break;
			case SeekOrigin::End: d->position = GetSize(d) + (uint64)offset; break;
			default:
				cemuLog_log(LogType::Force, "nn_boss: NsData::Seek with unknown origin {}", (uint32)origin);
				break;
			}
			return kOk;
		}

		void InitVTables()
		{
			for (VTable* vt : {s_storageVTable.GetPtr(), s_almightyVTable.GetPtr()})
			{
				vt->rtti.ptr = nullptr;
				vt->dtor.ptr = DtorEntry<&Dtor>();
				vt->rttiBase.ptr = nullptr;
			}
			s_nsDataVTable->rtti.ptr = nullptr;
			s_nsDataVTable->dtor.ptr = DtorEntry<&NsDataDtor>();
			s_nsDataVTable->rttiBase.ptr = nullptr;
		}
	}

	class : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "nn_boss";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(GetBossState, "nn_boss", "GetBossState__Q2_2nn4bossFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(Initialize, "nn_boss", "Initialize__Q2_2nn4bossFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(IsInitialized, "nn_boss", "IsInitialized__Q2_2nn4bossFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(Finalize, "nn_boss", "Finalize__Q2_2nn4bossFv", LogType::NN_BOSS);

			cafeExportRegisterFunc(TaskId::ctorDefault, "nn_boss", "__ct__Q3_2nn4boss6TaskIDFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(TaskId::ctorFromString, "nn_boss", "__ct__Q3_2nn4boss6TaskIDFPCc", LogType::NN_BOSS);

			task::InitVTables();
			cafeExportRegisterFunc(task::CtorSlot, "nn_boss", "__ct__Q3_2nn4boss4TaskFUcPCc", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::CtorPersistentId, "nn_boss", "__ct__Q3_2nn4boss4TaskFPCcUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::CtorId, "nn_boss", "__ct__Q3_2nn4boss4TaskFPCc", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::CtorEmpty, "nn_boss", "__ct__Q3_2nn4boss4TaskFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::Dtor, "nn_boss", "__dt__Q3_2nn4boss4TaskFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::InitWithPersistentId, "nn_boss", "Initialize__Q3_2nn4boss4TaskFPCcUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::InitWithSlot, "nn_boss", "Initialize__Q3_2nn4boss4TaskFUcPCc", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::InitCommon, "nn_boss", "Initialize__Q3_2nn4boss4TaskFPCc", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::Run, "nn_boss", "Run__Q3_2nn4boss4TaskFb", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::Wait, "nn_boss", "Wait__Q3_2nn4boss4TaskFUiQ3_2nn4boss13TaskWaitState", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::GetTurnState, "nn_boss", "GetTurnState__Q3_2nn4boss4TaskCFPUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::GetHttpStatusCode, "nn_boss", "GetHttpStatusCode__Q3_2nn4boss4TaskCFPUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::GetContentLength, "nn_boss", "GetContentLength__Q3_2nn4boss4TaskCFPUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::GetProcessedLength, "nn_boss", "GetProcessedLength__Q3_2nn4boss4TaskCFPUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::Register, "nn_boss", "Register__Q3_2nn4boss4TaskFRQ3_2nn4boss11TaskSetting", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::Unregister, "nn_boss", "Unregister__Q3_2nn4boss4TaskFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::IsRegistered, "nn_boss", "IsRegistered__Q3_2nn4boss4TaskCFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::RegisterForImmediateRun, "nn_boss", "RegisterForImmediateRun__Q3_2nn4boss4TaskFRCQ3_2nn4boss11TaskSetting", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::StartScheduling, "nn_boss", "StartScheduling__Q3_2nn4boss4TaskFb", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::StopScheduling, "nn_boss", "StopScheduling__Q3_2nn4boss4TaskFv", LogType::NN_BOSS);

			cafeExportRegisterFunc(task::CtorAlmighty, "nn_boss", "__ct__Q3_2nn4boss12AlmightyTaskFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::InitAlmighty, "nn_boss", "Initialize__Q3_2nn4boss12AlmightyTaskFQ3_2nn4boss7TitleIDPCcUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(task::Dtor, "nn_boss", "__dt__Q3_2nn4boss12AlmightyTaskFv", LogType::NN_BOSS);

			TaskSetting::InitVTable();
			cafeExportRegisterFunc(TaskSetting::ctor, "nn_boss", "__ct__Q3_2nn4boss11TaskSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(TaskSetting::dtor, "nn_boss", "__dt__Q3_2nn4boss11TaskSettingFv", LogType::NN_BOSS);
			// earlier PixelU builds answered this export with the privileged-type check; kept as is
			cafeExportRegisterFunc(TaskSetting::IsPrivileged, "nn_boss", "Initialize__Q3_2nn4boss11TaskSettingFPCcUi", LogType::NN_BOSS);

			NetTaskSetting::InitVTable();
			cafeExportRegisterFunc(NetTaskSetting::ctor, "nn_boss", "__ct__Q3_2nn4boss14NetTaskSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(NetTaskSetting::dtor, "nn_boss", "__dt__Q3_2nn4boss14NetTaskSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(NetTaskSetting::SetServiceToken, "nn_boss", "SetServiceToken__Q3_2nn4boss14NetTaskSettingFPCUc", LogType::NN_BOSS);
			cafeExportRegisterFunc(NetTaskSetting::AddInternalCaCert, "nn_boss", "AddInternalCaCert__Q3_2nn4boss14NetTaskSettingFSc", LogType::NN_BOSS);
			cafeExportRegisterFunc(NetTaskSetting::SetInternalClientCert, "nn_boss", "SetInternalClientCert__Q3_2nn4boss14NetTaskSettingFSc", LogType::NN_BOSS);

			NbdlTaskSetting::InitVTable();
			cafeExportRegisterFunc(NbdlTaskSetting::ctor, "nn_boss", "__ct__Q3_2nn4boss15NbdlTaskSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(NbdlTaskSetting::dtor, "nn_boss", "__dt__Q3_2nn4boss15NbdlTaskSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(NbdlTaskSetting::Initialize, "nn_boss", "Initialize__Q3_2nn4boss15NbdlTaskSettingFPCcLT1", LogType::NN_BOSS);
			cafeExportRegisterFunc(NbdlTaskSetting::SetFileName, "nn_boss", "SetFileName__Q3_2nn4boss15NbdlTaskSettingFPCc", LogType::NN_BOSS);

			RawUlTaskSetting::InitVTable();

			PlayReportSetting::InitVTable();
			cafeExportRegisterFunc(PlayReportSetting::ctor, "nn_boss", "__ct__Q3_2nn4boss17PlayReportSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(PlayReportSetting::dtor, "nn_boss", "__dt__Q3_2nn4boss17PlayReportSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(PlayReportSetting::Initialize, "nn_boss", "Initialize__Q3_2nn4boss17PlayReportSettingFPvUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(PlayReportSetting::Set, "nn_boss", "Set__Q3_2nn4boss17PlayReportSettingFPCcUi", LogType::NN_BOSS);

			RawDlTaskSetting::InitVTable();
			cafeExportRegisterFunc(RawDlTaskSetting::ctor, "nn_boss", "__ct__Q3_2nn4boss16RawDlTaskSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(RawDlTaskSetting::dtor, "nn_boss", "__dt__Q3_2nn4boss16RawDlTaskSettingFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(RawDlTaskSetting::Initialize, "nn_boss", "Initialize__Q3_2nn4boss16RawDlTaskSettingFPCcbT2N21", LogType::NN_BOSS);

			Title::InitVTable();
			cafeExportRegisterFunc(Title::ctor, "nn_boss", "__ct__Q3_2nn4boss5TitleFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(Title::dtor, "nn_boss", "__dt__Q3_2nn4boss5TitleFv", LogType::NN_BOSS);

			cafeExportRegisterFunc(TitleId::ctorDefault, "nn_boss", "__ct__Q3_2nn4boss7TitleIDFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(TitleId::ctorFromTitleId, "nn_boss", "__ct__Q3_2nn4boss7TitleIDFUL", LogType::NN_BOSS);
			cafeExportRegisterFunc(TitleId::ctorCopy, "nn_boss", "__ct__Q3_2nn4boss7TitleIDFRCQ3_2nn4boss7TitleID", LogType::NN_BOSS);
			cafeExportRegisterFunc(TitleId::operator_ne, "nn_boss", "__ne__Q3_2nn4boss7TitleIDCFRCQ3_2nn4boss7TitleID", LogType::NN_BOSS);

			cafeExportRegisterFunc(DataName::ctor, "nn_boss", "__ct__Q3_2nn4boss8DataNameFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(DataName::operator_const_char, "nn_boss", "__opPCc__Q3_2nn4boss8DataNameCFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(DirectoryName::ctor, "nn_boss", "__ct__Q3_2nn4boss13DirectoryNameFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(DirectoryName::operator_const_char, "nn_boss", "__opPCc__Q3_2nn4boss13DirectoryNameCFv", LogType::NN_BOSS);

			BossAccount::InitVTable();
			cafeExportRegisterFunc(BossAccount::ctor, "nn_boss", "__ct__Q3_2nn4boss7AccountFUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(BossAccount::dtor, "nn_boss", "__dt__Q3_2nn4boss7AccountFv", LogType::NN_BOSS);

			storage::InitVTables();
			cafeExportRegisterFunc(storage::Ctor, "nn_boss", "__ct__Q3_2nn4boss7StorageFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::Dtor, "nn_boss", "__dt__Q3_2nn4boss7StorageFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::Finalize, "nn_boss", "Finalize__Q3_2nn4boss7StorageFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::Exist, "nn_boss", "Exist__Q3_2nn4boss7StorageCFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::GetDataList, "nn_boss", "GetDataList__Q3_2nn4boss7StorageCFPQ3_2nn4boss8DataNameUiPUiT2", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::InitWithAccount, "nn_boss", "Initialize__Q3_2nn4boss7StorageFPCcUiQ3_2nn4boss11StorageKind", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::InitCommon, "nn_boss", "Initialize__Q3_2nn4boss7StorageFPCcQ3_2nn4boss11StorageKind", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::CtorAlmighty, "nn_boss", "__ct__Q3_2nn4boss15AlmightyStorageFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::InitAlmighty, "nn_boss", "Initialize__Q3_2nn4boss15AlmightyStorageFQ3_2nn4boss7TitleIDPCcUiQ3_2nn4boss11StorageKind", LogType::NN_BOSS);

			cafeExportRegisterFunc(storage::NsDataCtor, "nn_boss", "__ct__Q3_2nn4boss6NsDataFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::NsDataDtor, "nn_boss", "__dt__Q3_2nn4boss6NsDataFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::NsDataInit, "nn_boss", "Initialize__Q3_2nn4boss6NsDataFRCQ3_2nn4boss7StoragePCc", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::DeleteRealFile, "nn_boss", "DeleteRealFile__Q3_2nn4boss6NsDataFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::DeleteRealFileWithHistory, "nn_boss", "DeleteRealFileWithHistory__Q3_2nn4boss6NsDataFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::NsDataExist, "nn_boss", "Exist__Q3_2nn4boss6NsDataCFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::GetSize, "nn_boss", "GetSize__Q3_2nn4boss6NsDataCFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::GetCreatedTime, "nn_boss", "GetCreatedTime__Q3_2nn4boss6NsDataCFv", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::Read, "nn_boss", "Read__Q3_2nn4boss6NsDataFPvUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::ReadWithSizeOut, "nn_boss", "Read__Q3_2nn4boss6NsDataFPLPvUi", LogType::NN_BOSS);
			cafeExportRegisterFunc(storage::Seek, "nn_boss", "Seek__Q3_2nn4boss6NsDataFLQ3_2nn4boss12PositionBase", LogType::NN_BOSS);
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				coreinit::OSInitMutexEx(g_initLock.GetPtr(), nullptr);
				g_initCount = 0;
				g_initialized = false;
			}
		}
	} s_bossModule;

	COSModule* GetModule()
	{
		return &s_bossModule;
	}
}
