// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/helpers/helpers.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"
#include "FST/fstUtil.h"

// Graphic packs replace single game files by registering host files under
// their virtual path. The whole table is mounted once at "/" with the
// highest priority, so it overrides every other device.
namespace
{
	struct Redirection
	{
		fs::path hostFile;
		sint32 priority;
	};

	FSAFileTree<Redirection> s_redirections;
	bool s_mounted = false;

	class RedirectDevice : public fscDeviceC
	{
	  public:
		FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
		{
			Redirection* redirection;
			if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) && s_redirections.getFile(path, redirection))
				return FSCVirtualFile_Host::OpenFile(redirection->hostFile, accessFlags, *fscStatus);
			FSCVirtualFile* folder;
			if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR) && s_redirections.getDirectory(path, folder))
				return folder;
			return nullptr;
		}
	};

	RedirectDevice& GetRedirectDevice()
	{
		static RedirectDevice s_device;
		return s_device;
	}
}

// an existing redirection of the same path is only replaced by a higher priority
void fscDeviceRedirect_add(std::string_view virtualSourcePath, size_t fileSize, const fs::path& targetFilePath, sint32 priority)
{
	Redirection* existing;
	if (s_redirections.getFile(virtualSourcePath, existing))
	{
		if (priority <= existing->priority)
			return;
		s_redirections.removeFile(virtualSourcePath);
		delete existing;
	}
	s_redirections.addFile(virtualSourcePath, fileSize, new Redirection{targetFilePath, priority});
}

void fscDeviceRedirect_map()
{
	if (s_mounted)
		return;
	fsc_mount("/", "/", &GetRedirectDevice(), nullptr, FSC_PRIORITY_REDIRECT);
	s_mounted = true;
}
