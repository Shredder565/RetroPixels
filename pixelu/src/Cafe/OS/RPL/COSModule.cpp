// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// The table of Cafe OS libraries that have a C++ implementation.
#include "COSModule.h"

#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/zlib125/zlib125.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/dmae/dmae.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_user/snd_user.h"
#include "Cafe/OS/libs/mic/mic.h"
#include "Cafe/OS/libs/erreula/erreula.h"
#include "Cafe/OS/libs/nn_act/nn_act.h"
#include "Cafe/OS/libs/nn_acp/nn_acp.h"
#include "Cafe/OS/libs/nn_ac/nn_ac.h"
#include "Cafe/OS/libs/nn_ec/nn_ec.h"
#include "Cafe/OS/libs/nn_boss/nn_boss.h"
#include "Cafe/OS/libs/nn_nfp/nn_nfp.h"
#include "Cafe/OS/libs/nn_uds/nn_uds.h"
#include "Cafe/OS/libs/nn_nim/nn_nim.h"
#include "Cafe/OS/libs/nn_ndm/nn_ndm.h"
#include "Cafe/OS/libs/nn_spm/nn_spm.h"
#include "Cafe/OS/libs/nn_save/nn_save.h"
#include "Cafe/OS/libs/nsysnet/nsysnet.h"
#include "Cafe/OS/libs/nn_fp/nn_fp.h"
#include "Cafe/OS/libs/nn_olv/nn_olv.h"
#include "Cafe/OS/libs/nn_idbe/nn_idbe.h"
#include "Cafe/OS/libs/nlibnss/nlibnss.h"
#include "Cafe/OS/libs/nlibcurl/nlibcurl.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "Cafe/OS/libs/nsyshid/nsyshid.h"
#include "Cafe/OS/libs/nsyskbd/nsyskbd.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "Cafe/OS/libs/camera/camera.h"
#include "Cafe/OS/libs/proc_ui/proc_ui.h"
#include "Cafe/OS/libs/avm/avm.h"
#include "Cafe/OS/libs/drmapp/drmapp.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/nn_cmpt/nn_cmpt.h"
#include "Cafe/OS/libs/nn_ccr/nn_ccr.h"
#include "Cafe/OS/libs/nn_temp/nn_temp.h"
#include "Cafe/OS/libs/nn_aoc/nn_aoc.h"
#include "Cafe/OS/libs/nn_pdm/nn_pdm.h"
#include "Cafe/OS/libs/h264_avc/h264dec.h"
#include "Cafe/OS/libs/ntag/ntag.h"
#include "Cafe/OS/libs/nfc/nfc.h"

std::span<COSModule*> GetCOSModules()
{
	// modules are looked up by name, first match wins
	static std::vector<COSModule*> s_modules{
		// kernel, compression, graphics, DMA
		coreinit::GetModule(), zlib::GetModule(), GX2::GetModule(), dmae::GetModule(),
		// controllers, sound and microphone
		padscore::GetModule(), vpad::GetModule(),
		snd_core::GetModuleSndCore1(), snd_core::GetModuleSndCore2(),
		snd_user::GetModuleSndUser1(), snd_user::GetModuleSndUser2(),
		mic::GetModule(),
		// nn services
		nn::erreula::GetModule(), nn::act::GetModule(), nn::acp::GetModule(), nn::ac::GetModule(),
		nn::ec::GetModule(), nn::boss::GetModule(), nn::nfp::GetModule(), nn::uds::GetModule(),
		nn::nim::GetModule(), nn::ndm::GetModule(), nn::spm::GetModule(), nn::save::GetModule(),
		nsysnet::GetModule(), nn::fp::GetModule(), nn::olv::GetModule(), nn::idbe::GetModule(),
		nlibnss::GetModule(), nlibcurl::GetModule(),
		// system applets, HID and keyboards, camera, process UI
		sysapp::GetModule(), nsyshid::GetModule(), nsyskbd::GetModule(), swkbd::GetModule(),
		camera::GetModule(), proc_ui::GetModule(),
		// AV, DRM, TV control, compatibility, temp/AOC/play stats
		avm::GetModule(), drmapp::GetModule(), TCL::GetModule(), nn::cmpt::GetModule(),
		nn::ccr::GetModule(), nn::temp::GetModule(), nn::aoc::GetModule(), nn::pdm::GetModule(),
		// video decoding and NFC
		H264::GetModule(), ntag::GetModule(), nfc::GetModule(),
	};
	return s_modules;
}
