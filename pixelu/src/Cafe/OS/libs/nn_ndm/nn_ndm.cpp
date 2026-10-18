// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_ndm controls the console's background network daemons (SpotPass/BOSS,
// the download manager, ...). Games suspend them before heavy online play
// and resume them afterwards; system apps such as the Download Manager read
// their status. We keep a status per daemon, all "running" at start, and
// apply suspend/resume bitmasks to it. Results use the nn::ndm module code.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/nn_common.h"
#include "nn_ndm.h"

namespace nn::ndm
{
	namespace
	{
		constexpr nnResult kOk = BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NDM, 0);
		constexpr nnResult kInvalid = BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NDM, 0);

		// status values seen by callers
		constexpr uint32 kDaemonRunning = 1;
		constexpr uint32 kDaemonSuspended = 3;

		constexpr uint32 kDaemonCount = 3; // indexed by nn::ndm::Cafe::DaemonName

		std::array<uint32, kDaemonCount> g_daemonState{};
		uint32 g_users = 0;

		void ResetState()
		{
			g_daemonState.fill(kDaemonRunning);
			g_users = 0;
		}

		void SetDaemons(uint32 mask, uint32 newState)
		{
			for (uint32 index = 0; index < kDaemonCount; index++)
			{
				if ((mask >> index) & 1)
					g_daemonState[index] = newState;
			}
		}

		nnResult Initialize()
		{
			g_users++;
			return kOk;
		}

		nnResult Finalize()
		{
			if (g_users == 0)
				return kInvalid;
			// Note: the user count is deliberately not decreased here. Earlier
			// PixelU builds kept the library initialized after Finalize and
			// IsInitialized keeps answering the same way.
			return kOk;
		}

		uint32 IsInitialized()
		{
			return g_users > 0 ? 1 : 0;
		}

		nnResult GetDaemonStatus(uint32be* statusOut, uint32 daemon)
		{
			if (daemon >= kDaemonCount)
				return kInvalid;
			*statusOut = g_daemonState[daemon];
			return kOk;
		}

		nnResult SuspendDaemons(uint32 mask)
		{
			SetDaemons(mask, kDaemonSuspended);
			return kOk;
		}

		nnResult ResumeDaemons(uint32 mask)
		{
			SetDaemons(mask, kDaemonRunning);
			return kOk;
		}

		class NdmLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_ndm"; }

			void RPLMapped() override
			{
				ResetState();
				cafeExportRegisterFunc(Initialize, "nn_ndm", "Initialize__Q2_2nn3ndmFv", LogType::Placeholder);
				cafeExportRegisterFunc(Finalize, "nn_ndm", "Finalize__Q2_2nn3ndmFv", LogType::Placeholder);
				cafeExportRegisterFunc(IsInitialized, "nn_ndm", "IsInitialized__Q2_2nn3ndmFv", LogType::Placeholder);
				cafeExportRegisterFunc(GetDaemonStatus, "nn_ndm", "GetDaemonStatus__Q2_2nn3ndmFPQ4_2nn3ndm7IDaemon6StatusQ4_2nn3ndm4Cafe10DaemonName", LogType::Placeholder);
				cafeExportRegisterFunc(SuspendDaemons, "nn_ndm", "SuspendDaemons__Q2_2nn3ndmFUi", LogType::Placeholder);
				cafeExportRegisterFunc(ResumeDaemons, "nn_ndm", "ResumeDaemons__Q2_2nn3ndmFUi", LogType::Placeholder);
			}
		};

		NdmLibrary g_ndmLibrary;
	}

	COSModule* GetModule()
	{
		return &g_ndmLibrary;
	}
}
