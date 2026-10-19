// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "config/LaunchSettings.h"

using SurfFmt = Latte::E_GX2SURFFMT;

namespace
{
	enum FormatFlags : uint8
	{
		kPlain = 0,
		kCompressed = 1 << 0, // block compressed, uploaded with glCompressedTex*
		kAlternative = 1 << 1, // the GL format is not a bit exact match
	};

	struct FormatMapping
	{
		SurfFmt format;
		GLenum internalFormat;
		GLint suppliedFormat;
		GLint suppliedType;
		uint8 flags;
	};

	// colour formats; BC4 and the S3TC fallback depend on runtime conditions and are handled in code
	constexpr FormatMapping kColorFormats[] =
	{
		// 8 bit per channel
		{ SurfFmt::R8_UNORM, GL_R8, GL_RED, GL_UNSIGNED_BYTE, kPlain },
		{ SurfFmt::R8_SNORM, GL_R8_SNORM, GL_RED, GL_BYTE, kPlain },
		{ SurfFmt::R8_UINT, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, kPlain },
		{ SurfFmt::R8_G8_UNORM, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, kPlain },
		{ SurfFmt::R8_G8_SNORM, GL_RG8_SNORM, GL_RG, GL_BYTE, kPlain },
		{ SurfFmt::R8_G8_B8_A8_UNORM, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, kPlain },
		{ SurfFmt::R8_G8_B8_A8_SRGB, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, kPlain },
		{ SurfFmt::R8_G8_B8_A8_SNORM, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, kPlain },
		{ SurfFmt::R8_G8_B8_A8_UINT, GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, kPlain },
		// 16 bit per channel
		{ SurfFmt::R16_UNORM, GL_R16, GL_RED, GL_UNSIGNED_SHORT, kPlain },
		{ SurfFmt::R16_SNORM, GL_R16_SNORM, GL_RED, GL_SHORT, kPlain },
		{ SurfFmt::R16_FLOAT, GL_R16F, GL_RED, GL_HALF_FLOAT, kPlain },
		{ SurfFmt::R16_UINT, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, kPlain },
		{ SurfFmt::R16_G16_UNORM, GL_RG16, GL_RG, GL_UNSIGNED_SHORT, kPlain },
		{ SurfFmt::R16_G16_FLOAT, GL_RG16F, GL_RG, GL_HALF_FLOAT, kPlain },
		{ SurfFmt::R16_G16_B16_A16_UNORM, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, kPlain },
		{ SurfFmt::R16_G16_B16_A16_SNORM, GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, kPlain },
		{ SurfFmt::R16_G16_B16_A16_FLOAT, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, kPlain },
		{ SurfFmt::R16_G16_B16_A16_UINT, GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, kPlain },
		// 32 bit per channel
		{ SurfFmt::R32_FLOAT, GL_R32F, GL_RED, GL_FLOAT, kPlain },
		{ SurfFmt::R32_UINT, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, kPlain },
		{ SurfFmt::R32_G32_FLOAT, GL_RG32F, GL_RG, GL_FLOAT, kPlain },
		{ SurfFmt::R32_G32_UINT, GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, kPlain },
		{ SurfFmt::R32_G32_B32_A32_FLOAT, GL_RGBA32F, GL_RGBA, GL_FLOAT, kPlain },
		{ SurfFmt::R32_G32_B32_A32_UINT, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, kPlain },
		{ SurfFmt::R32_G32_B32_A32_SINT, GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, kPlain },
		// packed
		{ SurfFmt::R4_G4_UNORM, GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, kAlternative },
		{ SurfFmt::R4_G4_B4_A4_UNORM, GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, kPlain },
		{ SurfFmt::R5_G6_B5_UNORM, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, kPlain },
		{ SurfFmt::R5_G5_B5_A1_UNORM, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, kPlain },
		{ SurfFmt::A1_B5_G5_R5_UNORM, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, kPlain },
		{ SurfFmt::R10_G10_B10_A2_UNORM, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, kPlain },
		{ SurfFmt::R10_G10_B10_A2_SRGB, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, kPlain }, // no sRGB variant in GL
		{ SurfFmt::A2_B10_G10_R10_UNORM, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_10_10_10_2, kPlain },
		{ SurfFmt::R10_G10_B10_A2_SNORM, GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, kAlternative }, // GL has no signed 10:10:10:2
		{ SurfFmt::R11_G11_B10_FLOAT, GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, kPlain },
		// depth-like colour formats
		{ SurfFmt::R24_X8_UNORM, GL_R32F, GL_RED, GL_FLOAT, kAlternative }, // stored as float
		{ SurfFmt::X24_G8_UINT, GL_RGBA8UI, GL_RGBA, GL_FLOAT, kAlternative }, // stencil in alpha, see the swizzle fix up
		// block compressed (no supplied format)
		{ SurfFmt::BC1_UNORM, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, -1, -1, kCompressed },
		{ SurfFmt::BC1_SRGB, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, -1, -1, kCompressed },
		{ SurfFmt::BC2_UNORM, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, -1, -1, kCompressed },
		{ SurfFmt::BC2_SRGB, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, -1, -1, kCompressed },
		{ SurfFmt::BC3_UNORM, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, -1, -1, kCompressed },
		{ SurfFmt::BC3_SRGB, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, -1, -1, kCompressed },
		{ SurfFmt::BC5_UNORM, GL_COMPRESSED_RG_RGTC2, -1, -1, kCompressed },
		{ SurfFmt::BC5_SNORM, GL_COMPRESSED_SIGNED_RG_RGTC2, -1, -1, kCompressed },
	};

	constexpr FormatMapping kDepthFormats[] =
	{
		{ SurfFmt::D16_UNORM, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, kPlain },
		{ SurfFmt::D24_S8_UNORM, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, kPlain },
		{ SurfFmt::D24_S8_FLOAT, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, kAlternative },
		{ SurfFmt::D32_FLOAT, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, kPlain },
		{ SurfFmt::D32_S8_FLOAT, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, kPlain },
	};

	template<size_t N>
	const FormatMapping* FindMapping(const FormatMapping (&table)[N], SurfFmt format)
	{
		for (const FormatMapping& entry : table)
		{
			if (entry.format == format)
				return &entry;
		}
		return nullptr;
	}

	void ApplyMapping(const FormatMapping& entry, LatteTextureGL::FormatInfoGL* out)
	{
		if (entry.flags & kCompressed)
			out->setCompressed(entry.internalFormat, entry.suppliedFormat, entry.suppliedType);
		else
			out->setFormat(entry.internalFormat, entry.suppliedFormat, entry.suppliedType);
		out->isUsingAlternativeFormat = (entry.flags & kAlternative) != 0;
	}

	bool UseGLDebugNames()
	{
#ifdef CEMU_DEBUG_ASSERT
		return true;
#else
		return LaunchSettings::NSightModeEnabled();
#endif
	}
}

void LatteTextureGL::GetOpenGLFormatInfo(bool isDepth, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, FormatInfoGL* formatInfoOut)
{
	formatInfoOut->isUsingAlternativeFormat = false;
	if (isDepth)
	{
		if (const FormatMapping* entry = FindMapping(kDepthFormats, format))
		{
			ApplyMapping(*entry, formatInfoOut);
			return;
		}
		cemuLog_log(LogType::Force, "OpenGL: Unsupported texture depth format 0x{:04x}", (uint32)format);
		formatInfoOut->setFormat(GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT);
		formatInfoOut->markAsAlternativeFormat();
		return;
	}

	// without S3TC support BC1-3 are decoded on the CPU to RGBA (float data), sRGB is then handled by the sampler
	const Latte::E_HWFMT hwFormat = Latte::GetHWFormat(format);
	if (hwFormat == Latte::E_HWFMT::HWFMT_BC1 || hwFormat == Latte::E_HWFMT::HWFMT_BC2 || hwFormat == Latte::E_HWFMT::HWFMT_BC3)
	{
		const bool srgb = HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
		if (!OpenGLRenderer::GetInstance()->SupportsS3TC(srgb))
		{
			formatInfoOut->setFormat(srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, GL_FLOAT);
			formatInfoOut->markAsAlternativeFormat();
			return;
		}
	}

	// RGTC1 is limited to 2D textures, other dimensions use a decoded two channel float texture
	if (format == SurfFmt::BC4_UNORM || format == SurfFmt::BC4_SNORM)
	{
		if (dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_ARRAY)
		{
			formatInfoOut->setCompressed(format == SurfFmt::BC4_UNORM ? GL_COMPRESSED_RED_RGTC1 : GL_COMPRESSED_SIGNED_RED_RGTC1, -1, -1);
			return;
		}
		formatInfoOut->setFormat(GL_RG16F, GL_RG, GL_FLOAT);
		formatInfoOut->markAsAlternativeFormat();
		return;
	}

	if (format == SurfFmt::R32_X8_FLOAT)
	{
		// only exists as a depth format in GL (Cars 3)
		formatInfoOut->setFormat(GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV);
		cemu_assert_debug(false);
		return;
	}

	if (const FormatMapping* entry = FindMapping(kColorFormats, format))
	{
		ApplyMapping(*entry, formatInfoOut);
		return;
	}
	cemuLog_log(LogType::Force, "OpenGL: Unsupported texture format 0x{:04x}", (uint32)format);
	cemu_assert_unimplemented();
	formatInfoOut->setFormat(0, 0, 0);
}

LatteTextureGL::LatteTextureGL(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle,
	Latte::E_HWTILEMODE tileMode, bool isDepth)
	: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth)
{
	GenerateEmptyTextureFromGX2Dim(dim, glId_texture, glTexTarget, true);
	// a format overwrite from a graphic pack replaces the storage format
	const SurfFmt storageFormat = overwriteInfo.hasFormatOverwrite ? (SurfFmt)overwriteInfo.format : format;
	FormatInfoGL info;
	GetOpenGLFormatInfo(isDepth, storageFormat, dim, &info);
	glInternalFormat = info.glInternalFormat;
	isAlternativeFormat = info.isUsingAlternativeFormat;
	if (UseGLDebugNames())
	{
		const std::string label = fmt::format("{:08x}_f{:04x}{}_p{:04x}_{}x{}", physAddress, (uint32)format, this->isDepth ? "_d" : "", pitch, width, height);
		glObjectLabel(GL_TEXTURE, glId_texture, -1, label.c_str());
	}
}

LatteTextureGL::~LatteTextureGL()
{
	glDeleteTextures(1, &glId_texture);
	catchOpenGLError();
}

void LatteTextureGL::GenerateEmptyTextureFromGX2Dim(Latte::E_DIM dim, GLuint& texId, GLint& texTarget, bool createForTargetType)
{
	switch (dim)
	{
	case Latte::E_DIM::DIM_1D: texTarget = GL_TEXTURE_1D; break;
	case Latte::E_DIM::DIM_2D: texTarget = GL_TEXTURE_2D; break;
	case Latte::E_DIM::DIM_2D_MSAA: texTarget = GL_TEXTURE_2D; break; // multisampling is not emulated
	case Latte::E_DIM::DIM_3D: texTarget = GL_TEXTURE_3D; break;
	case Latte::E_DIM::DIM_2D_ARRAY: texTarget = GL_TEXTURE_2D_ARRAY; break;
	case Latte::E_DIM::DIM_CUBEMAP: texTarget = GL_TEXTURE_CUBE_MAP_ARRAY; break; // cube maps are always arrays of faces
	default:
		cemu_assert_unimplemented();
		break;
	}
	if (createForTargetType)
		texId = glCreateTextureWrapper(texTarget);
	else
		glGenTextures(1, &texId);
}

LatteTextureView* LatteTextureGL::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
{
	return new LatteTextureViewGL(this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
}

void LatteTextureGL::AllocateOnHost()
{
	cemu_assert_debug(!isDataDefined);
	sint32 w = width, h = height, d = depth;
	if (overwriteInfo.hasResolutionOverwrite)
	{
		w = overwriteInfo.width;
		h = overwriteInfo.height;
		d = overwriteInfo.depth;
	}
	const sint32 levels = std::max(1, std::min(mipLevels, maxPossibleMipLevels));
	switch (dim)
	{
	case Latte::E_DIM::DIM_1D:
		cemu_assert_debug(h == 1 && d == 1);
		glTextureStorage1DWrapper(GL_TEXTURE_1D, glId_texture, levels, glInternalFormat, w);
		break;
	case Latte::E_DIM::DIM_2D:
	case Latte::E_DIM::DIM_2D_MSAA:
		cemu_assert_debug(d == 1);
		glTextureStorage2DWrapper(GL_TEXTURE_2D, glId_texture, levels, glInternalFormat, w, h);
		break;
	case Latte::E_DIM::DIM_2D_ARRAY:
	case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
		glTextureStorage3DWrapper(GL_TEXTURE_2D_ARRAY, glId_texture, levels, glInternalFormat, w, h, std::max(1, d));
		break;
	case Latte::E_DIM::DIM_3D:
		glTextureStorage3DWrapper(GL_TEXTURE_3D, glId_texture, levels, glInternalFormat, w, h, std::max(1, d));
		break;
	case Latte::E_DIM::DIM_CUBEMAP:
		glTextureStorage3DWrapper(GL_TEXTURE_CUBE_MAP_ARRAY, glId_texture, levels, glInternalFormat, w, h, d);
		break;
	default:
		cemu_assert_unimplemented();
		break;
	}
}
