// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Types shared by the guest nn_boss library (Cafe/OS/libs/nn_boss) and the /dev/boss service.
// All structs mirror guest memory layouts; offsets are checked below.
#pragma once
#include "Common/CafeString.h"

namespace nn::boss
{
	typedef uint32 BossResult;

	// guest vtable slot
	struct VTableEntry
	{
		uint16be offsetA{0};
		uint16be offsetB{0};
		MEMPTR<void> ptr;
	};
	static_assert(sizeof(VTableEntry) == 8);

	// ---- small value classes --------------------------------------------------------

	struct TitleId
	{
		uint64be u64{};

		static bool IsValid(TitleId* _thisptr);
		static TitleId* ctorDefault(TitleId* _thisptr);
		static TitleId* ctorFromTitleId(TitleId* _thisptr, uint64 titleId);
		static TitleId* ctorCopy(TitleId* _thisptr, TitleId* titleId);
		static bool operator_ne(TitleId* _thisptr, TitleId* titleId);
	};
	static_assert(sizeof(TitleId) == 8);

	struct TaskId
	{
		CafeString<8> id;

		TaskId() = default;
		TaskId(const char* taskId) { id.assign(taskId); }

		static TaskId* ctorDefault(TaskId* _thisptr);
		static TaskId* ctorFromString(TaskId* _thisptr, const char* taskId);

		// ordered by the id string so it can key a std::map
		std::strong_ordering operator<=>(const TaskId& other) const noexcept
		{
			return id <=> other.id;
		}
	};
	static_assert(sizeof(TaskId) == 8);

	struct Title
	{
		uint32be accountId{};
		TitleId titleId{}; // +0x08
		MEMPTR<void> vTablePtr{}; // +0x10

		struct VTable
		{
			VTableEntry rtti;
			VTableEntry dtor;
		};
		static inline SysAllocator<VTable> s_titleVTable;

		static Title* ctor(Title* _this);
		static void dtor(Title* _this, uint32 options);
		static void InitVTable();
	};
	static_assert(sizeof(Title) == 0x18);

	struct DirectoryName
	{
		CafeString<8> name2;

		static DirectoryName* ctor(DirectoryName* _thisptr);
		static const char* operator_const_char(DirectoryName* _thisptr);
	};
	static_assert(sizeof(DirectoryName) == 8);

	// nn::boss::Account in the guest; renamed here so it is not mixed up with Cafe/Account
	struct BossAccount
	{
		struct VTable
		{
			VTableEntry rtti;
			VTableEntry dtor;
		};
		static inline SysAllocator<VTable> s_VTable;

		uint32be accountId;
		MEMPTR<void> vTablePtr;

		static BossAccount* ctor(BossAccount* _this, uint32 accountId);
		static void dtor(BossAccount* _this, uint32 options);
		static void InitVTable();
	};
	static_assert(sizeof(BossAccount) == 8);

	// ---- task states ----------------------------------------------------------------

	// state argument of Task::Wait
	enum class TaskWaitState : uint32
	{
		Done = 1,
	};

	// value reported by Task::GetTurnState
	enum class TaskTurnState : uint32
	{
		Ukn = 0,
		Stopped = 1,
		Ready = 4,
		Running = 6,
		Done = 7,
		DoneSuccess = 16,
		DoneError = 17
	};

	// internal state of a registered task
	enum class TaskState : uint8
	{
		Initial = 0,
		Stopped = 1,
		Ready = 4, // queued for the task thread
		Running = 6,
		Done = 7,
	};

	enum class TaskType : uint16
	{
		NbdlTaskSetting = 2,
		RawDlTaskSetting_1 = 1,
		RawDlTaskSetting_3 = 3,
		RawDlTaskSetting_9 = 9,
		RawUlTaskSetting = 4,
		PlayLogUploadTaskSetting = 5,
		PlayReportSetting = 6,
		DataStoreDownloadSetting = 8,
		NbdlDataListTaskSetting = 10
	};

	// ---- task settings --------------------------------------------------------------

	// the 0xC00 bytes of a TaskSetting that are passed to /dev/boss on registration
	struct TaskSettingCore
	{
		// 64-bit value stored as two words (the struct is not 8-byte aligned in guest arrays)
		struct LifeTime
		{
			uint32be high;
			uint32be low;
		};

		uint32be persistentId;
		uint32be ukn04;
		uint32be ukn08;
		uint32be ukn0C;
		uint32be ukn10;
		uint32be ukn14;
		uint32be ukn18;
		uint32be ukn1C;
		TaskId taskId;
		betype<TaskType> taskType;
		uint8be priority;
		uint8be mode;
		uint8be permission;
		uint16be intervalA;
		uint32be intervalB;
		uint32be unk34;
		LifeTime lifeTime;
		uint8be httpProtocol;
		uint8be internalClientCert;
		uint16be httpOption;
		uint8 ukn44[0x48 - 0x44];
		CafeString<256> url;
		CafeString<64> lastModifiedTime;
		uint8be internalCaCert[3];
		uint8 ukn18B;
		uint32be httpTimeout;
		CafeString<128> caCert[3];
		CafeString<128> clientCertName;
		CafeString<128> clientCertKey;
		struct HttpHeader
		{
			CafeString<32> name;
			CafeString<64> value;
		};
		HttpHeader httpHeaders[3];
		CafeString<96> httpQueryString;
		CafeString<512> serviceToken;
		uint8 ukn790[0x7C0 - 0x790];

		// fields from 0x7C0 on depend on the task type
		union
		{
			struct
			{
				uint32be optionValue;
				CafeString<32> largeHttpHeaderKey;
				CafeString<512> largeHttpHeaderValue;
			}rawUl;
			// same fields as rawUl; the report data itself is attached through a separate call
			struct
			{
				uint32be optionValue;
				CafeString<32> largeHttpHeaderKey;
				CafeString<512> largeHttpHeaderValue;
			}playReportSetting;
			struct
			{
				uint8be newArrival;
				uint8be led;
				uint8be ukn7C2[6];
				CafeString<8> bossDirectory;
				CafeString<32> fileName; // meaning not confirmed
			}rawDl;
			struct
			{
				CafeString<32> bossCode;
				CafeString<8> bossDirectory;
				uint32be ukn7E8;
				uint32be ukn7EC;
				uint32be directorySizeLimitHigh;
				uint32be directorySizeLimitLow;
				CafeString<32> fileName;
			}nbdl;
			struct
			{
				uint8 finalPadding[0xC00 - 0x7C0];
			}paddedBlock;
		};
	};
	static_assert(sizeof(TaskSettingCore) == 0xC00);

	struct TaskSetting : public TaskSettingCore
	{
		uint8 paddingC00[0x1000 - 0xC00];
		MEMPTR<void> vTablePtr;

		struct VTableTaskSetting
		{
			VTableEntry rtti;
			VTableEntry dtor;
			VTableEntry RegisterPreprocess;
			VTableEntry unk1;
		};
		static inline SysAllocator<VTableTaskSetting> s_VTable;

		static TaskSetting* ctor(TaskSetting* _thisptr);
		static void dtor(TaskSetting* _this, uint32 options);
		static bool IsPrivileged(TaskSetting* _thisptr);
		static void InitializeSetting(TaskSetting* _thisptr);
		static void InitVTable();
	};
	static_assert(offsetof(TaskSetting, taskId) == 0x20);
	static_assert(offsetof(TaskSetting, taskType) == 0x28);
	static_assert(offsetof(TaskSetting, priority) == 0x2A);
	static_assert(offsetof(TaskSetting, mode) == 0x2B);
	static_assert(offsetof(TaskSetting, permission) == 0x2C);
	static_assert(offsetof(TaskSetting, intervalA) == 0x2E);
	static_assert(offsetof(TaskSetting, intervalB) == 0x30);
	static_assert(offsetof(TaskSetting, unk34) == 0x34);
	static_assert(offsetof(TaskSetting, lifeTime) == 0x38);
	static_assert(offsetof(TaskSetting, httpProtocol) == 0x40);
	static_assert(offsetof(TaskSetting, internalClientCert) == 0x41);
	static_assert(offsetof(TaskSetting, httpOption) == 0x42);
	static_assert(offsetof(TaskSetting, url) == 0x48);
	static_assert(offsetof(TaskSetting, lastModifiedTime) == 0x148);
	static_assert(offsetof(TaskSetting, internalCaCert) == 0x188);
	static_assert(offsetof(TaskSetting, httpTimeout) == 0x18C);
	static_assert(offsetof(TaskSetting, caCert) == 0x190);
	static_assert(offsetof(TaskSetting, clientCertName) == 0x310);
	static_assert(offsetof(TaskSetting, clientCertKey) == 0x390);
	static_assert(offsetof(TaskSetting, httpHeaders) == 0x410);
	static_assert(offsetof(TaskSetting, httpQueryString) == 0x530);
	static_assert(offsetof(TaskSetting, serviceToken) == 0x590);
	static_assert(offsetof(TaskSetting, rawUl.optionValue) == 0x7C0);
	static_assert(offsetof(TaskSetting, rawUl.largeHttpHeaderKey) == 0x7C4);
	static_assert(offsetof(TaskSetting, rawUl.largeHttpHeaderValue) == 0x7E4);
	static_assert(offsetof(TaskSetting, rawDl.newArrival) == 0x7C0);
	static_assert(offsetof(TaskSetting, rawDl.led) == 0x7C1);
	static_assert(offsetof(TaskSetting, rawDl.bossDirectory) == 0x7C8);
	static_assert(offsetof(TaskSetting, rawDl.fileName) == 0x7D0);
	static_assert(offsetof(TaskSetting, nbdl.bossCode) == 0x7C0);
	static_assert(offsetof(TaskSetting, nbdl.bossDirectory) == 0x7E0);
	static_assert(offsetof(TaskSetting, nbdl.directorySizeLimitHigh) == 0x7F0);
	static_assert(offsetof(TaskSetting, nbdl.fileName) == 0x7F8);
	static_assert(offsetof(TaskSetting, vTablePtr) == 0x1000);
	static_assert(sizeof(TaskSetting) == 0x1004);

	struct NetTaskSetting : TaskSetting
	{
		struct VTableNetTaskSetting : public VTableTaskSetting
		{ };
		static inline SysAllocator<VTableNetTaskSetting> s_VTable;

		static NetTaskSetting* ctor(NetTaskSetting* _thisptr);
		static BossResult AddCaCert(NetTaskSetting* _thisptr, const char* name);
		static BossResult SetServiceToken(NetTaskSetting* _thisptr, const uint8* serviceToken);
		static BossResult AddInternalCaCert(NetTaskSetting* _thisptr, char certId);
		static void SetInternalClientCert(NetTaskSetting* _thisptr, char certId);
		static void InitVTable();
	};
	static_assert(sizeof(NetTaskSetting) == 0x1004);

	struct NbdlTaskSetting : NetTaskSetting
	{
		struct VTableNbdlTaskSetting : public VTableNetTaskSetting
		{
			VTableEntry rttiNetTaskSetting;
		};
		static_assert(sizeof(VTableNbdlTaskSetting) == 8*5);
		static inline SysAllocator<VTableNbdlTaskSetting> s_VTable;

		static NbdlTaskSetting* ctor(NbdlTaskSetting* _thisptr);
		static BossResult Initialize(NbdlTaskSetting* _thisptr, const char* bossCode, uint64 directorySizeLimit, const char* bossDirectory);
		static BossResult SetFileName(NbdlTaskSetting* _thisptr, const char* fileName);
		static void InitVTable();
	};
	static_assert(sizeof(NbdlTaskSetting) == 0x1004);

	struct RawUlTaskSetting : NetTaskSetting
	{
		uint32be ukRaw1; // +0x1004
		uint32be ukRaw2;
		uint32be ukRaw3;
		uint8 rawSpace[0x200]; // +0x1010

		struct VTableRawUlTaskSetting : public VTableNetTaskSetting
		{
			VTableEntry rttiNetTaskSetting;
		};
		static_assert(sizeof(VTableRawUlTaskSetting) == 8*5);
		static inline SysAllocator<VTableRawUlTaskSetting> s_VTable;

		static RawUlTaskSetting* ctor(RawUlTaskSetting* _thisptr);
		static void dtor(RawUlTaskSetting* _this, uint32 options);
		static void InitVTable();
	};
	static_assert(offsetof(RawUlTaskSetting, ukRaw1) == 0x1004);
	static_assert(offsetof(RawUlTaskSetting, rawSpace) == 0x1010);
	static_assert(sizeof(RawUlTaskSetting) == 0x1210);

	struct RawDlTaskSetting : NetTaskSetting
	{
		struct VTableRawDlTaskSetting : public VTableNetTaskSetting
		{
			VTableEntry rttiNetTaskSetting;
		};
		static_assert(sizeof(VTableRawDlTaskSetting) == 8*5);
		static inline SysAllocator<VTableRawDlTaskSetting> s_VTable;

		static RawDlTaskSetting* ctor(RawDlTaskSetting* _thisptr);
		static BossResult Initialize(RawDlTaskSetting* _thisptr, const char* url, bool newArrival, bool led, const char* fileName, const char* bossDirectory);
		static void InitVTable();
	};
	static_assert(sizeof(RawDlTaskSetting) == 0x1004);

	struct PlayReportSetting : RawUlTaskSetting
	{
		MEMPTR<uint8> ukn1210_ptr; // +0x1210
		uint32be ukn1214_size;
		uint32be ukPlay3;
		uint32be ukPlay4;

		struct VTablePlayReportSetting : public VTableRawUlTaskSetting
		{};
		static_assert(sizeof(VTablePlayReportSetting) == 8*5);
		static inline SysAllocator<VTablePlayReportSetting> s_VTable;

		static PlayReportSetting* ctor(PlayReportSetting* _this);
		static void dtor(PlayReportSetting* _this, uint32 options);
		static void Initialize(PlayReportSetting* _this, uint8* ptr, uint32 size);
		static bool Set(PlayReportSetting* _this, const char* keyname, uint32 value);
		static void InitVTable();
	};
	static_assert(offsetof(PlayReportSetting, ukn1210_ptr) == 0x1210);
	static_assert(sizeof(PlayReportSetting) == 0x1220);

	// ---- storage --------------------------------------------------------------------

	enum class StorageKind : uint32
	{
		StorageNbdl = 0,
		StorageRawDl = 1,
	};

	struct DataName
	{
		CafeString<32> name;

		static DataName* ctor(DataName* _this);
		static const char* operator_const_char(DataName* _this);

		const char* c_str() const
		{
			return name.c_str();
		}
	};
	static_assert(sizeof(DataName) == 32);

	// ---- /dev/boss command ids (service 0) --------------------------------------------
	// Service 1 has variants of most commands that take an extra title id.
	enum class BossCommandId : uint32
	{
		TaskIsRegistered = 0x69,
		TaskRegisterA = 0x6A,
		TaskRegisterForImmediateRunA = 0x6B,
		TaskUnregister = 0x6D,
		TaskStartScheduling = 0x77,
		TaskRun = 0x78,
		TaskStopScheduling = 0x79,
		TaskWaitA = 0x7A,
		TaskGetHttpStatusCodeA = 0x7C,
		TaskGetTurnState = 0x7E,
		TaskGetContentLength = 0x82,
		TaskGetProcessedLength = 0x83,

		StorageExist = 0x87,
		StorageGetDataList = 0xB0,

		NsDataExist = 0x90,
		NsDataRead = 0x93,
		NsDataGetSize = 0x96,
		NsDataDeleteFile = 0xA7,
		NsDataDeleteFileWithHistory = 0xA8,
		NsFinalize = 0xB3,

		TitleSetNewArrivalFlag = 0x9E,

		// not identified
		UknA7 = 0xA5,
		DeleteDataRelated = 0xA6,
	};
}
