// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class GLContext;

namespace OGL
{
// GLSL dialect the shader header declares. Desktop versions first, then ES; code compares
// with < and >=, so the order matters.
enum GlslVersion
{
  Glsl130,
  Glsl140,
  Glsl150,
  Glsl330,
  Glsl400,  // GL 4.0 - 4.2
  Glsl430,  // GL 4.3 - 4.4
  Glsl450,  // GL 4.5 and newer
  GlslEs300,
  GlslEs310,
  GlslEs320,
};

// Which extension (if any) provides gl_PointSize in ES geometry shaders.
enum class EsPointSizeType
{
  PointSizeNone,
  PointSizeOes,
  PointSizeExt,
};

// Where texture buffers come from on ES.
enum class EsTexbufType
{
  TexbufNone,
  TexbufCore,
  TexbufOes,
  TexbufExt
};

// Framebuffer fetch extension in use, if any.
enum class EsFbFetchType
{
  FbFetchNone,
  FbFetchExt,
  FbFetchArm,
};

// How multisampled array textures are allocated.
enum class MultisampleTexStorageType
{
  TexStorageNone,
  TexStorageCore,
  TexStorageOes,
};

// Capabilities of the GL context that only this backend cares about (the shared ones live in
// g_backend_info). Filled by PopulateConfig.
struct VideoConfig
{
  bool bIsES;
  bool bSupportsGLPinnedMemory;
  bool bSupportsGLSync;
  bool bSupportsGLBaseVertex;
  bool bSupportsGLBufferStorage;
  bool bSupportsMSAA;
  GlslVersion eSupportedGLSLVersion;
  bool bSupportViewportFloat;
  bool bSupportsAEP;
  bool bSupportsDebug;
  bool bSupportsCopySubImage;
  EsPointSizeType SupportedESPointSize;
  EsTexbufType SupportedESTextureBuffer;
  bool bSupportsTextureStorage;
  MultisampleTexStorageType SupportedMultisampleTexStorage;
  bool bSupportsConservativeDepth;
  bool bSupportsImageLoadStore;
  bool bSupportsAniso;
  bool bSupportsBitfield;
  bool bSupportsTextureSubImage;
  EsFbFetchType SupportedFramebufferFetch;
  bool bSupportsKHRShaderSubgroup;  // basic, arithmetic and ballot together
  bool bSupportsExplicitLayoutInShader;

  const char* gl_vendor;
  const char* gl_renderer;
  const char* gl_version;
};

// Identifies the driver from the vendor/renderer/version strings for DriverDetails.
void InitDriverInfo();

// Reads the context's capabilities into g_ogl_config and g_backend_info. False (after telling
// the user) when the context lacks something the backend needs.
bool PopulateConfig(GLContext* main_gl_context);

extern VideoConfig g_ogl_config;

}  // namespace OGL
