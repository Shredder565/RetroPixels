// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nlibnss exposes the console's device certificate and signing helpers to
// network code. The emulated console has no device certificate to give out,
// so exports report fixed sizes and do not write any certificate data:
//   NSSExportDeviceCertChain(cert, &certLen, chain, &chainLen, ...)
//       -> reports 0x100 bytes for both buffers, returns 0
//   NSSSignatureGetSignatureLength()
//       -> 0x1C (size of a signature blob)

#include "Cafe/OS/common/OSCommon.h"
#include "nlibnss.h"

namespace nlibnss
{
	namespace
	{
		constexpr uint32 kReportedCertSize = 0x100;
		constexpr sint32 kSignatureLength = 0x1C;

		sint32 ExportDeviceCertChain(uint8* certOut, uint32be* certSize, uint8* chainOut, uint32be* chainSize, uint32 arg4, uint32 arg5)
		{
			cemuLog_logDebug(LogType::Force, "nlibnss: NSSExportDeviceCertChain is not available, no certificate returned");
			if (certSize)
				*certSize = kReportedCertSize;
			if (chainSize)
				*chainSize = kReportedCertSize;
			return 0;
		}

		sint32 SignatureGetSignatureLength()
		{
			return kSignatureLength;
		}

		class NssLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nlibnss"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(SignatureGetSignatureLength, "nlibnss", "NSSSignatureGetSignatureLength", LogType::Placeholder);
				cafeExportRegisterFunc(ExportDeviceCertChain, "nlibnss", "NSSExportDeviceCertChain", LogType::Placeholder);
			}
		};

		NssLibrary g_nssLibrary;
	}

	COSModule* GetModule()
	{
		return &g_nssLibrary;
	}
}
