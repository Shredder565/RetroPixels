// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/TitleList/TitleId.h"

enum class CosCapabilityBits : uint64;
enum class CosCapabilityGroup : uint32;
enum class CafeConsoleRegion;

// Lifetime of the emulated console: one-time system start, preparing and launching
// the foreground title, information about it, and shutting it down again.
namespace CafeSystem
{
	// callbacks into the frontend
	class SystemImplementation
	{
	public:
		virtual void CafeRecreateCanvas() = 0;
		virtual void CafePPCProcessExit() = 0; // the guest application ended
	};

	enum class PREPARE_STATUS_CODE
	{
		SUCCESS,
		INVALID_RPX,
		UNABLE_TO_MOUNT, // the title's files could not be mounted (missing, moved or unreadable)
	};

	void Initialize();
	void SetImplementation(SystemImplementation* impl);
	void Shutdown();

	PREPARE_STATUS_CODE PrepareForegroundTitle(TitleId titleId);
	PREPARE_STATUS_CODE PrepareForegroundTitleFromStandaloneRPX(const fs::path& path);
	void LaunchForegroundTitle();
	bool IsTitleRunning();

	// arguments handed over when one title launches another
	bool GetOverrideArgStr(std::vector<std::string>& args);
	void SetOverrideArgs(std::span<std::string> args);
	void UnsetOverrideArgs();

	TitleId GetForegroundTitleId();
	uint16 GetForegroundTitleVersion();
	uint32 GetForegroundTitleSDKVersion();
	CafeConsoleRegion GetForegroundTitleRegion();
	CafeConsoleRegion GetPlatformRegion();
	std::string GetForegroundTitleName();
	std::string GetForegroundTitleArgStr();
	uint32 GetForegroundTitleOlvAccesskey();
	CosCapabilityBits GetForegroundTitleCosCapabilities(CosCapabilityGroup group);
	std::optional<sint32> GetForegroundTitleReturnStatus(); // set once the application exited through coreinit

	void ShutdownTitle();

	std::string GetMlcStoragePath(TitleId titleId);
	void MlcStorageMountAllTitles();

	std::string GetInternalVirtualCodeFolder();

	// checksums of the main executable, with and without the update applied
	uint32 GetRPXHashBase();
	uint32 GetRPXHashUpdated();
	void RequestRecreateCanvas();
	void NotifyPPCProcessExit(sint32 status);

	void DumpCurrentRPX();
};

extern RPLModule* applicationRPX;

extern std::atomic_bool g_isGPUInitFinished;
