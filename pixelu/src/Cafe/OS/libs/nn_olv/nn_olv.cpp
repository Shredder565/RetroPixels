// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv.h"

#include "nn_olv_InitializeTypes.h"
#include "nn_olv_UploadCommunityTypes.h"
#include "nn_olv_DownloadCommunityTypes.h"
#include "nn_olv_UploadFavoriteTypes.h"
#include "nn_olv_PostTypes.h"
#include "nn_olv_OfflineDB.h"

#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"

namespace nn::olv
{
	namespace
	{
		// --- posting through the Miiverse applet ------------------------------------
		// There is no applet to hand the post to. The title is sent to the background
		// and brought back as if the user had closed the posting screen.

		SysAllocator<OSThread_t> s_returnThread;
		SysAllocator<uint8, 1024> s_returnThreadStack;
		SysAllocator<char, 32> s_returnThreadName;
		std::atomic<bool> s_returnPending{false};

		void ReturnFromPostApp()
		{
			coreinit::OSSleepTicks(ESPRESSO_TIMER_CLOCK * 2);
			coreinit::StartBackgroundForegroundTransition();
			s_returnPending = false;
		}

		sint32 UploadByPostApp(void* /*param*/)
		{
			if (s_returnPending.exchange(true))
				return kOlvSuccess; // a return to the title is already on its way
			OSThread_t* thread = s_returnThread.GetPtr();
			uint8* stack = s_returnThreadStack.GetPtr();
			const uint32 stackSize = s_returnThreadStack.GetByteSize();
			constexpr uint32 kAffinityCore1 = 1 << 1, kDetached = 1 << 3;
			coreinit::OSCreateThreadType(thread, makeCallableExport<&ReturnFromPostApp>(), 0, nullptr, stack + stackSize, (sint32)stackSize, 0,
				kAffinityCore1 | kDetached, OSThread_t::THREAD_TYPE::TYPE_APP);
			coreinit::OSResumeThread(thread);
			strcpy(s_returnThreadName.GetPtr(), "StubPostApp!");
			coreinit::OSSetThreadName(thread, s_returnThreadName.GetPtr());
			return kOlvSuccess;
		}

		sint32 PostAppResult()
		{
			return OlvStatus(301); // the user cancelled the post
		}

		sint32 PostAppResultWithData(void* /*uploadedData*/)
		{
			return PostAppResult();
		}

		// UploadedPostData::GetPostId; the id is left empty
		char* UploadedPostData_GetPostId(char* data)
		{
			data[4] = '\0';
			return data + 4;
		}

		// --- error codes ---------------------------------------------------------------

		// map an nn::Result to the 115xxxx error code shown to the user
		constexpr uint32 ErrorCodeFor(uint32 result)
		{
			const uint32 version = (result >> 27) & 3;
			const uint32 moduleMask = version == 3 ? 0x07F00000 : 0x1FF00000;
			const uint32 module = (result & moduleMask) >> 20;
			const bool failed = (result & 0x80000000) != 0;
			if (module == NN_RESULT_MODULE_NN_OLV && failed)
			{
				const uint32 detailMask = version == 3 ? 0x3FF : 0xFFFFF;
				return ((result & detailMask) >> 7) + 1150000;
			}
			// account (nn_act) errors and anything else get the generic code
			return 1159999;
		}
		static_assert(ErrorCodeFor(0xA119C600) == 1155004);

		uint32 GetErrorCode(uint32be* result)
		{
			return ErrorCodeFor(*result);
		}

		void ResetLibraryState()
		{
			g_olv = OliveState{};
			s_returnPending = false;
		}

		class OliveModule : public COSModule
		{
		public:
			std::string_view GetName() override
			{
				return "nn_olv";
			}

			void RPLMapped() override
			{
				RegisterInitializeExports();
				RegisterUploadCommunityExports();
				RegisterDownloadCommunityExports();
				RegisterUploadFavoriteExports();
				RegisterPostExports();

				cafeExportRegisterFunc(GetErrorCode, "nn_olv", "GetErrorCode__Q2_2nn3olvFRCQ2_2nn6Result", LogType::NN_OLV);

				cafeExportRegisterFunc(UploadByPostApp, "nn_olv", "UploadPostDataByPostApp__Q2_2nn3olvFPCQ3_2nn3olv28UploadPostDataByPostAppParam", LogType::NN_OLV);
				cafeExportRegisterFunc(UploadByPostApp, "nn_olv", "UploadCommentDataByPostApp__Q2_2nn3olvFPCQ3_2nn3olv31UploadCommentDataByPostAppParam", LogType::NN_OLV);
				cafeExportRegisterFunc(UploadByPostApp, "nn_olv", "UploadDirectMessageDataByPostApp__Q2_2nn3olvFPCQ3_2nn3olv37UploadDirectMessageDataByPostAppParam", LogType::NN_OLV);

				cafeExportRegisterFunc(PostAppResult, "nn_olv", "GetResultByPostApp__Q2_2nn3olvFv", LogType::NN_OLV);
				cafeExportRegisterFunc(PostAppResultWithData, "nn_olv", "GetResultWithUploadedPostDataByPostApp__Q2_2nn3olvFPQ3_2nn3olv16UploadedPostData", LogType::NN_OLV);
				cafeExportRegisterFunc(PostAppResultWithData, "nn_olv", "GetResultWithUploadedDirectMessageDataByPostApp__Q2_2nn3olvFPQ3_2nn3olv25UploadedDirectMessageData", LogType::NN_OLV);
				cafeExportRegisterFunc(PostAppResultWithData, "nn_olv", "GetResultWithUploadedCommentDataByPostApp__Q2_2nn3olvFPQ3_2nn3olv19UploadedCommentData", LogType::NN_OLV);

				cafeExportRegisterFunc(UploadedPostData_GetPostId, "nn_olv", "GetPostId__Q3_2nn3olv16UploadedPostDataCFv", LogType::NN_OLV);
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
			{
				if (reason == coreinit::RplEntryReason::Loaded)
					ResetLibraryState();
				else if (reason == coreinit::RplEntryReason::Unloaded)
					OfflineDB_Shutdown();
			}
		};

		OliveModule s_module;
	}

	COSModule* GetModule()
	{
		return &s_module;
	}
}
