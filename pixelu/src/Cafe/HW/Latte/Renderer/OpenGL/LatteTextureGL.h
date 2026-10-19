// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Common/GLInclude/GLInclude.h"

class LatteTextureGL : public LatteTexture
{
public:
	LatteTextureGL(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
		uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth);
	~LatteTextureGL();

	void AllocateOnHost() override;

	// creates a texture name for the GL target that corresponds to dim; with createForTargetType the name is
	// also initialised to that target (DSA create), otherwise it is only generated (needed for glTextureView)
	static void GenerateEmptyTextureFromGX2Dim(Latte::E_DIM dim, GLuint& texId, GLint& texTarget, bool createForTargetType);

	// how a GX2 format is stored on the GL side and how pixel data is supplied for uploads
	struct FormatInfoGL
	{
		sint32 glInternalFormat;
		sint32 glSuppliedFormat;
		sint32 glSuppliedFormatType;
		bool glIsCompressed;
		bool isUsingAlternativeFormat{}; // no bit exact GL equivalent

		void setFormat(sint32 internalFormat, sint32 suppliedFormat, sint32 suppliedFormatType)
		{
			glInternalFormat = internalFormat;
			glSuppliedFormat = suppliedFormat;
			glSuppliedFormatType = suppliedFormatType;
			glIsCompressed = false;
		}

		void setCompressed(sint32 internalFormat, sint32 suppliedFormat, sint32 suppliedFormatType)
		{
			setFormat(internalFormat, suppliedFormat, suppliedFormatType);
			glIsCompressed = true;
		}

		void markAsAlternativeFormat() { isUsingAlternativeFormat = true; }
	};

	static void GetOpenGLFormatInfo(bool isDepth, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, FormatInfoGL* formatInfoOut);

	GLuint glId_texture;
	GLint glTexTarget; // GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, ...
	GLint glInternalFormat = 0; // 0 while not set
	bool isAlternativeFormat{};

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;
};
