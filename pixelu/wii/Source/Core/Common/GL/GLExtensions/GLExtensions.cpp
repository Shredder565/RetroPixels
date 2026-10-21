// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLExtensions/GLExtensions.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include "Common/GL/GLContext.h"
#include "Common/Logging/Log.h"

// Storage for every GL entry point declared by the API headers. All start out null;
// GLExtensions::Init fills in the ones the context provides.

// --- ARB_ES2_compatibility.h ---
PFNDOLCLEARDEPTHFPROC dolClearDepthf = nullptr;
PFNDOLDEPTHRANGEFPROC dolDepthRangef = nullptr;
PFNDOLGETSHADERPRECISIONFORMATPROC dolGetShaderPrecisionFormat = nullptr;
PFNDOLRELEASESHADERCOMPILERPROC dolReleaseShaderCompiler = nullptr;
PFNDOLSHADERBINARYPROC dolShaderBinary = nullptr;

// --- ARB_blend_func_extended.h ---
PFNDOLBINDFRAGDATALOCATIONINDEXEDPROC dolBindFragDataLocationIndexed = nullptr;
PFNDOLGETFRAGDATAINDEXPROC dolGetFragDataIndex = nullptr;

// --- ARB_buffer_storage.h ---
PFNDOLBUFFERSTORAGEPROC dolBufferStorage = nullptr;

// --- ARB_clip_control.h ---
PFNDOLCLIPCONTROLPROC dolClipControl = nullptr;

// --- ARB_compute_shader.h ---
PFNDOLDISPATCHCOMPUTEPROC dolDispatchCompute = nullptr;
PFNDOLDISPATCHCOMPUTEINDIRECTPROC dolDispatchComputeIndirect = nullptr;

// --- ARB_copy_image.h ---
PFNDOLCOPYIMAGESUBDATAPROC dolCopyImageSubData = nullptr;

// --- ARB_debug_output.h ---
PFNDOLDEBUGMESSAGECALLBACKARBPROC dolDebugMessageCallbackARB = nullptr;
PFNDOLDEBUGMESSAGECONTROLARBPROC dolDebugMessageControlARB = nullptr;
PFNDOLDEBUGMESSAGEINSERTARBPROC dolDebugMessageInsertARB = nullptr;
PFNDOLGETDEBUGMESSAGELOGARBPROC dolGetDebugMessageLogARB = nullptr;

// --- ARB_draw_elements_base_vertex.h ---
PFNDOLDRAWELEMENTSBASEVERTEXPROC dolDrawElementsBaseVertex = nullptr;
PFNDOLDRAWELEMENTSINSTANCEDBASEVERTEXPROC dolDrawElementsInstancedBaseVertex = nullptr;
PFNDOLDRAWRANGEELEMENTSBASEVERTEXPROC dolDrawRangeElementsBaseVertex = nullptr;
PFNDOLMULTIDRAWELEMENTSBASEVERTEXPROC dolMultiDrawElementsBaseVertex = nullptr;

// --- ARB_framebuffer_object.h ---
PFNDOLBINDFRAMEBUFFERPROC dolBindFramebuffer = nullptr;
PFNDOLBINDRENDERBUFFERPROC dolBindRenderbuffer = nullptr;
PFNDOLBLITFRAMEBUFFERPROC dolBlitFramebuffer = nullptr;
PFNDOLCHECKFRAMEBUFFERSTATUSPROC dolCheckFramebufferStatus = nullptr;
PFNDOLDELETEFRAMEBUFFERSPROC dolDeleteFramebuffers = nullptr;
PFNDOLDELETERENDERBUFFERSPROC dolDeleteRenderbuffers = nullptr;
PFNDOLFRAMEBUFFERRENDERBUFFERPROC dolFramebufferRenderbuffer = nullptr;
PFNDOLFRAMEBUFFERTEXTURE1DPROC dolFramebufferTexture1D = nullptr;
PFNDOLFRAMEBUFFERTEXTURE2DPROC dolFramebufferTexture2D = nullptr;
PFNDOLFRAMEBUFFERTEXTURE3DPROC dolFramebufferTexture3D = nullptr;
PFNDOLFRAMEBUFFERTEXTURELAYERPROC dolFramebufferTextureLayer = nullptr;
PFNDOLGENFRAMEBUFFERSPROC dolGenFramebuffers = nullptr;
PFNDOLGENRENDERBUFFERSPROC dolGenRenderbuffers = nullptr;
PFNDOLGENERATEMIPMAPPROC dolGenerateMipmap = nullptr;
PFNDOLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC dolGetFramebufferAttachmentParameteriv = nullptr;
PFNDOLGETRENDERBUFFERPARAMETERIVPROC dolGetRenderbufferParameteriv = nullptr;
PFNDOLISFRAMEBUFFERPROC dolIsFramebuffer = nullptr;
PFNDOLISRENDERBUFFERPROC dolIsRenderbuffer = nullptr;
PFNDOLRENDERBUFFERSTORAGEPROC dolRenderbufferStorage = nullptr;
PFNDOLRENDERBUFFERSTORAGEMULTISAMPLEPROC dolRenderbufferStorageMultisample = nullptr;

// --- ARB_get_program_binary.h ---
PFNDOLGETPROGRAMBINARYPROC dolGetProgramBinary = nullptr;
PFNDOLPROGRAMBINARYPROC dolProgramBinary = nullptr;
PFNDOLPROGRAMPARAMETERIPROC dolProgramParameteri = nullptr;

// --- ARB_map_buffer_range.h ---
PFNDOLFLUSHMAPPEDBUFFERRANGEPROC dolFlushMappedBufferRange = nullptr;
PFNDOLMAPBUFFERRANGEPROC dolMapBufferRange = nullptr;

// --- ARB_sample_shading.h ---
PFNDOLMINSAMPLESHADINGARBPROC dolMinSampleShading = nullptr;

// --- ARB_sampler_objects.h ---
PFNDOLBINDSAMPLERPROC dolBindSampler = nullptr;
PFNDOLDELETESAMPLERSPROC dolDeleteSamplers = nullptr;
PFNDOLGENSAMPLERSPROC dolGenSamplers = nullptr;
PFNDOLGETSAMPLERPARAMETERIIVPROC dolGetSamplerParameterIiv = nullptr;
PFNDOLGETSAMPLERPARAMETERIUIVPROC dolGetSamplerParameterIuiv = nullptr;
PFNDOLGETSAMPLERPARAMETERFVPROC dolGetSamplerParameterfv = nullptr;
PFNDOLGETSAMPLERPARAMETERIVPROC dolGetSamplerParameteriv = nullptr;
PFNDOLISSAMPLERPROC dolIsSampler = nullptr;
PFNDOLSAMPLERPARAMETERIIVPROC dolSamplerParameterIiv = nullptr;
PFNDOLSAMPLERPARAMETERIUIVPROC dolSamplerParameterIuiv = nullptr;
PFNDOLSAMPLERPARAMETERFPROC dolSamplerParameterf = nullptr;
PFNDOLSAMPLERPARAMETERFVPROC dolSamplerParameterfv = nullptr;
PFNDOLSAMPLERPARAMETERIPROC dolSamplerParameteri = nullptr;
PFNDOLSAMPLERPARAMETERIVPROC dolSamplerParameteriv = nullptr;

// --- ARB_shader_image_load_store.h ---
PFNDOLBINDIMAGETEXTUREPROC dolBindImageTexture = nullptr;
PFNDOLMEMORYBARRIERPROC dolMemoryBarrier = nullptr;

// --- ARB_shader_storage_buffer_object.h ---
PFNDOLSHADERSTORAGEBLOCKBINDINGPROC dolShaderStorageBlockBinding = nullptr;

// --- ARB_sync.h ---
PFNDOLCLIENTWAITSYNCPROC dolClientWaitSync = nullptr;
PFNDOLDELETESYNCPROC dolDeleteSync = nullptr;
PFNDOLFENCESYNCPROC dolFenceSync = nullptr;
PFNDOLGETINTEGER64VPROC dolGetInteger64v = nullptr;
PFNDOLGETSYNCIVPROC dolGetSynciv = nullptr;
PFNDOLISSYNCPROC dolIsSync = nullptr;
PFNDOLWAITSYNCPROC dolWaitSync = nullptr;

// --- ARB_texture_multisample.h ---
PFNDOLTEXIMAGE2DMULTISAMPLEPROC dolTexImage2DMultisample = nullptr;
PFNDOLTEXIMAGE3DMULTISAMPLEPROC dolTexImage3DMultisample = nullptr;
PFNDOLGETMULTISAMPLEFVPROC dolGetMultisamplefv = nullptr;
PFNDOLSAMPLEMASKIPROC dolSampleMaski = nullptr;

// --- ARB_texture_storage.h ---
PFNDOLTEXSTORAGE1DPROC dolTexStorage1D = nullptr;
PFNDOLTEXSTORAGE2DPROC dolTexStorage2D = nullptr;
PFNDOLTEXSTORAGE3DPROC dolTexStorage3D = nullptr;

// --- ARB_texture_storage_multisample.h ---
PFNDOLTEXSTORAGE2DMULTISAMPLEPROC dolTexStorage2DMultisample = nullptr;
PFNDOLTEXSTORAGE3DMULTISAMPLEPROC dolTexStorage3DMultisample = nullptr;

// --- ARB_uniform_buffer_object.h ---
PFNDOLBINDBUFFERBASEPROC dolBindBufferBase = nullptr;
PFNDOLBINDBUFFERRANGEPROC dolBindBufferRange = nullptr;
PFNDOLGETACTIVEUNIFORMBLOCKNAMEPROC dolGetActiveUniformBlockName = nullptr;
PFNDOLGETACTIVEUNIFORMBLOCKIVPROC dolGetActiveUniformBlockiv = nullptr;
PFNDOLGETACTIVEUNIFORMNAMEPROC dolGetActiveUniformName = nullptr;
PFNDOLGETACTIVEUNIFORMSIVPROC dolGetActiveUniformsiv = nullptr;
PFNDOLGETINTEGERI_VPROC dolGetIntegeri_v = nullptr;
PFNDOLGETUNIFORMBLOCKINDEXPROC dolGetUniformBlockIndex = nullptr;
PFNDOLGETUNIFORMINDICESPROC dolGetUniformIndices = nullptr;
PFNDOLUNIFORMBLOCKBINDINGPROC dolUniformBlockBinding = nullptr;

// --- ARB_vertex_array_object.h ---
PFNDOLBINDVERTEXARRAYPROC dolBindVertexArray = nullptr;
PFNDOLDELETEVERTEXARRAYSPROC dolDeleteVertexArrays = nullptr;
PFNDOLGENVERTEXARRAYSPROC dolGenVertexArrays = nullptr;
PFNDOLISVERTEXARRAYPROC dolIsVertexArray = nullptr;

// --- ARB_viewport_array.h ---
PFNDOLDEPTHRANGEARRAYVPROC dolDepthRangeArrayv = nullptr;
PFNDOLDEPTHRANGEINDEXEDPROC dolDepthRangeIndexed = nullptr;
PFNDOLGETDOUBLEI_VPROC dolGetDoublei_v = nullptr;
PFNDOLGETFLOATI_VPROC dolGetFloati_v = nullptr;
PFNDOLSCISSORARRAYVPROC dolScissorArrayv = nullptr;
PFNDOLSCISSORINDEXEDPROC dolScissorIndexed = nullptr;
PFNDOLSCISSORINDEXEDVPROC dolScissorIndexedv = nullptr;
PFNDOLVIEWPORTARRAYVPROC dolViewportArrayv = nullptr;
PFNDOLVIEWPORTINDEXEDFPROC dolViewportIndexedf = nullptr;
PFNDOLVIEWPORTINDEXEDFVPROC dolViewportIndexedfv = nullptr;

// --- KHR_debug.h ---
PFNDOLDEBUGMESSAGECALLBACKPROC dolDebugMessageCallback = nullptr;
PFNDOLDEBUGMESSAGECONTROLPROC dolDebugMessageControl = nullptr;
PFNDOLDEBUGMESSAGEINSERTPROC dolDebugMessageInsert = nullptr;
PFNDOLGETDEBUGMESSAGELOGPROC dolGetDebugMessageLog = nullptr;
PFNDOLGETOBJECTLABELPROC dolGetObjectLabel = nullptr;
PFNDOLGETOBJECTPTRLABELPROC dolGetObjectPtrLabel = nullptr;
PFNDOLOBJECTLABELPROC dolObjectLabel = nullptr;
PFNDOLOBJECTPTRLABELPROC dolObjectPtrLabel = nullptr;
PFNDOLPOPDEBUGGROUPPROC dolPopDebugGroup = nullptr;
PFNDOLPUSHDEBUGGROUPPROC dolPushDebugGroup = nullptr;

// --- NV_depth_buffer_float.h ---
PFNDOLDEPTHRANGEDNVPROC dolDepthRangedNV = nullptr;
PFNDOLCLEARDEPTHDNVPROC dolClearDepthdNV = nullptr;
PFNDOLDEPTHBOUNDSDNVPROC dolDepthBoundsdNV = nullptr;

// --- NV_occlusion_query_samples.h ---
PFNDOLGENOCCLUSIONQUERIESNVPROC dolGenOcclusionQueriesNV = nullptr;
PFNDOLDELETEOCCLUSIONQUERIESNVPROC dolDeleteOcclusionQueriesNV = nullptr;
PFNDOLISOCCLUSIONQUERYNVPROC dolIsOcclusionQueryNV = nullptr;
PFNDOLBEGINOCCLUSIONQUERYNVPROC dolBeginOcclusionQueryNV = nullptr;
PFNDOLENDOCCLUSIONQUERYNVPROC dolEndOcclusionQueryNV = nullptr;
PFNDOLGETOCCLUSIONQUERYIVNVPROC dolGetOcclusionQueryivNV = nullptr;
PFNDOLGETOCCLUSIONQUERYUIVNVPROC dolGetOcclusionQueryuivNV = nullptr;

// --- NV_primitive_restart.h ---
PFNDOLPRIMITIVERESTARTINDEXNVPROC dolPrimitiveRestartIndexNV = nullptr;
PFNDOLPRIMITIVERESTARTNVPROC dolPrimitiveRestartNV = nullptr;

// --- gl_1_1.h ---
PFNDOLCLEARINDEXPROC dolClearIndex = nullptr;
PFNDOLCLEARCOLORPROC dolClearColor = nullptr;
PFNDOLCLEARPROC dolClear = nullptr;
PFNDOLINDEXMASKPROC dolIndexMask = nullptr;
PFNDOLCOLORMASKPROC dolColorMask = nullptr;
PFNDOLALPHAFUNCPROC dolAlphaFunc = nullptr;
PFNDOLBLENDFUNCPROC dolBlendFunc = nullptr;
PFNDOLLOGICOPPROC dolLogicOp = nullptr;
PFNDOLCULLFACEPROC dolCullFace = nullptr;
PFNDOLFRONTFACEPROC dolFrontFace = nullptr;
PFNDOLPOINTSIZEPROC dolPointSize = nullptr;
PFNDOLLINEWIDTHPROC dolLineWidth = nullptr;
PFNDOLLINESTIPPLEPROC dolLineStipple = nullptr;
PFNDOLPOLYGONMODEPROC dolPolygonMode = nullptr;
PFNDOLPOLYGONOFFSETPROC dolPolygonOffset = nullptr;
PFNDOLPOLYGONSTIPPLEPROC dolPolygonStipple = nullptr;
PFNDOLGETPOLYGONSTIPPLEPROC dolGetPolygonStipple = nullptr;
PFNDOLEDGEFLAGPROC dolEdgeFlag = nullptr;
PFNDOLEDGEFLAGVPROC dolEdgeFlagv = nullptr;
PFNDOLSCISSORPROC dolScissor = nullptr;
PFNDOLCLIPPLANEPROC dolClipPlane = nullptr;
PFNDOLGETCLIPPLANEPROC dolGetClipPlane = nullptr;
PFNDOLDRAWBUFFERPROC dolDrawBuffer = nullptr;
PFNDOLREADBUFFERPROC dolReadBuffer = nullptr;
PFNDOLENABLEPROC dolEnable = nullptr;
PFNDOLDISABLEPROC dolDisable = nullptr;
PFNDOLISENABLEDPROC dolIsEnabled = nullptr;
PFNDOLENABLECLIENTSTATEPROC dolEnableClientState = nullptr;
PFNDOLDISABLECLIENTSTATEPROC dolDisableClientState = nullptr;
PFNDOLGETBOOLEANVPROC dolGetBooleanv = nullptr;
PFNDOLGETDOUBLEVPROC dolGetDoublev = nullptr;
PFNDOLGETFLOATVPROC dolGetFloatv = nullptr;
PFNDOLGETINTEGERVPROC dolGetIntegerv = nullptr;
PFNDOLPUSHATTRIBPROC dolPushAttrib = nullptr;
PFNDOLPOPATTRIBPROC dolPopAttrib = nullptr;
PFNDOLPUSHCLIENTATTRIBPROC dolPushClientAttrib = nullptr;
PFNDOLPOPCLIENTATTRIBPROC dolPopClientAttrib = nullptr;
PFNDOLRENDERMODEPROC dolRenderMode = nullptr;
PFNDOLGETERRORPROC dolGetError = nullptr;
PFNDOLGETSTRINGPROC dolGetString = nullptr;
PFNDOLFINISHPROC dolFinish = nullptr;
PFNDOLFLUSHPROC dolFlush = nullptr;
PFNDOLHINTPROC dolHint = nullptr;
PFNDOLCLEARDEPTHPROC dolClearDepth = nullptr;
PFNDOLDEPTHFUNCPROC dolDepthFunc = nullptr;
PFNDOLDEPTHMASKPROC dolDepthMask = nullptr;
PFNDOLDEPTHRANGEPROC dolDepthRange = nullptr;
PFNDOLCLEARACCUMPROC dolClearAccum = nullptr;
PFNDOLACCUMPROC dolAccum = nullptr;
PFNDOLMATRIXMODEPROC dolMatrixMode = nullptr;
PFNDOLORTHOPROC dolOrtho = nullptr;
PFNDOLFRUSTUMPROC dolFrustum = nullptr;
PFNDOLVIEWPORTPROC dolViewport = nullptr;
PFNDOLPUSHMATRIXPROC dolPushMatrix = nullptr;
PFNDOLPOPMATRIXPROC dolPopMatrix = nullptr;
PFNDOLLOADIDENTITYPROC dolLoadIdentity = nullptr;
PFNDOLLOADMATRIXDPROC dolLoadMatrixd = nullptr;
PFNDOLLOADMATRIXFPROC dolLoadMatrixf = nullptr;
PFNDOLMULTMATRIXDPROC dolMultMatrixd = nullptr;
PFNDOLMULTMATRIXFPROC dolMultMatrixf = nullptr;
PFNDOLROTATEDPROC dolRotated = nullptr;
PFNDOLROTATEFPROC dolRotatef = nullptr;
PFNDOLSCALEDPROC dolScaled = nullptr;
PFNDOLSCALEFPROC dolScalef = nullptr;
PFNDOLTRANSLATEDPROC dolTranslated = nullptr;
PFNDOLTRANSLATEFPROC dolTranslatef = nullptr;
PFNDOLISLISTPROC dolIsList = nullptr;
PFNDOLDELETELISTSPROC dolDeleteLists = nullptr;
PFNDOLGENLISTSPROC dolGenLists = nullptr;
PFNDOLNEWLISTPROC dolNewList = nullptr;
PFNDOLENDLISTPROC dolEndList = nullptr;
PFNDOLCALLLISTPROC dolCallList = nullptr;
PFNDOLCALLLISTSPROC dolCallLists = nullptr;
PFNDOLLISTBASEPROC dolListBase = nullptr;
PFNDOLBEGINPROC dolBegin = nullptr;
PFNDOLENDPROC dolEnd = nullptr;
PFNDOLVERTEX2DPROC dolVertex2d = nullptr;
PFNDOLVERTEX2FPROC dolVertex2f = nullptr;
PFNDOLVERTEX2IPROC dolVertex2i = nullptr;
PFNDOLVERTEX2SPROC dolVertex2s = nullptr;
PFNDOLVERTEX3DPROC dolVertex3d = nullptr;
PFNDOLVERTEX3FPROC dolVertex3f = nullptr;
PFNDOLVERTEX3IPROC dolVertex3i = nullptr;
PFNDOLVERTEX3SPROC dolVertex3s = nullptr;
PFNDOLVERTEX4DPROC dolVertex4d = nullptr;
PFNDOLVERTEX4FPROC dolVertex4f = nullptr;
PFNDOLVERTEX4IPROC dolVertex4i = nullptr;
PFNDOLVERTEX4SPROC dolVertex4s = nullptr;
PFNDOLVERTEX2DVPROC dolVertex2dv = nullptr;
PFNDOLVERTEX2FVPROC dolVertex2fv = nullptr;
PFNDOLVERTEX2IVPROC dolVertex2iv = nullptr;
PFNDOLVERTEX2SVPROC dolVertex2sv = nullptr;
PFNDOLVERTEX3DVPROC dolVertex3dv = nullptr;
PFNDOLVERTEX3FVPROC dolVertex3fv = nullptr;
PFNDOLVERTEX3IVPROC dolVertex3iv = nullptr;
PFNDOLVERTEX3SVPROC dolVertex3sv = nullptr;
PFNDOLVERTEX4DVPROC dolVertex4dv = nullptr;
PFNDOLVERTEX4FVPROC dolVertex4fv = nullptr;
PFNDOLVERTEX4IVPROC dolVertex4iv = nullptr;
PFNDOLVERTEX4SVPROC dolVertex4sv = nullptr;
PFNDOLNORMAL3BPROC dolNormal3b = nullptr;
PFNDOLNORMAL3DPROC dolNormal3d = nullptr;
PFNDOLNORMAL3FPROC dolNormal3f = nullptr;
PFNDOLNORMAL3IPROC dolNormal3i = nullptr;
PFNDOLNORMAL3SPROC dolNormal3s = nullptr;
PFNDOLNORMAL3BVPROC dolNormal3bv = nullptr;
PFNDOLNORMAL3DVPROC dolNormal3dv = nullptr;
PFNDOLNORMAL3FVPROC dolNormal3fv = nullptr;
PFNDOLNORMAL3IVPROC dolNormal3iv = nullptr;
PFNDOLNORMAL3SVPROC dolNormal3sv = nullptr;
PFNDOLINDEXDPROC dolIndexd = nullptr;
PFNDOLINDEXFPROC dolIndexf = nullptr;
PFNDOLINDEXIPROC dolIndexi = nullptr;
PFNDOLINDEXSPROC dolIndexs = nullptr;
PFNDOLINDEXUBPROC dolIndexub = nullptr;
PFNDOLINDEXDVPROC dolIndexdv = nullptr;
PFNDOLINDEXFVPROC dolIndexfv = nullptr;
PFNDOLINDEXIVPROC dolIndexiv = nullptr;
PFNDOLINDEXSVPROC dolIndexsv = nullptr;
PFNDOLINDEXUBVPROC dolIndexubv = nullptr;
PFNDOLCOLOR3BPROC dolColor3b = nullptr;
PFNDOLCOLOR3DPROC dolColor3d = nullptr;
PFNDOLCOLOR3FPROC dolColor3f = nullptr;
PFNDOLCOLOR3IPROC dolColor3i = nullptr;
PFNDOLCOLOR3SPROC dolColor3s = nullptr;
PFNDOLCOLOR3UBPROC dolColor3ub = nullptr;
PFNDOLCOLOR3UIPROC dolColor3ui = nullptr;
PFNDOLCOLOR3USPROC dolColor3us = nullptr;
PFNDOLCOLOR4BPROC dolColor4b = nullptr;
PFNDOLCOLOR4DPROC dolColor4d = nullptr;
PFNDOLCOLOR4FPROC dolColor4f = nullptr;
PFNDOLCOLOR4IPROC dolColor4i = nullptr;
PFNDOLCOLOR4SPROC dolColor4s = nullptr;
PFNDOLCOLOR4UBPROC dolColor4ub = nullptr;
PFNDOLCOLOR4UIPROC dolColor4ui = nullptr;
PFNDOLCOLOR4USPROC dolColor4us = nullptr;
PFNDOLCOLOR3BVPROC dolColor3bv = nullptr;
PFNDOLCOLOR3DVPROC dolColor3dv = nullptr;
PFNDOLCOLOR3FVPROC dolColor3fv = nullptr;
PFNDOLCOLOR3IVPROC dolColor3iv = nullptr;
PFNDOLCOLOR3SVPROC dolColor3sv = nullptr;
PFNDOLCOLOR3UBVPROC dolColor3ubv = nullptr;
PFNDOLCOLOR3UIVPROC dolColor3uiv = nullptr;
PFNDOLCOLOR3USVPROC dolColor3usv = nullptr;
PFNDOLCOLOR4BVPROC dolColor4bv = nullptr;
PFNDOLCOLOR4DVPROC dolColor4dv = nullptr;
PFNDOLCOLOR4FVPROC dolColor4fv = nullptr;
PFNDOLCOLOR4IVPROC dolColor4iv = nullptr;
PFNDOLCOLOR4SVPROC dolColor4sv = nullptr;
PFNDOLCOLOR4UBVPROC dolColor4ubv = nullptr;
PFNDOLCOLOR4UIVPROC dolColor4uiv = nullptr;
PFNDOLCOLOR4USVPROC dolColor4usv = nullptr;
PFNDOLTEXCOORD1DPROC dolTexCoord1d = nullptr;
PFNDOLTEXCOORD1FPROC dolTexCoord1f = nullptr;
PFNDOLTEXCOORD1IPROC dolTexCoord1i = nullptr;
PFNDOLTEXCOORD1SPROC dolTexCoord1s = nullptr;
PFNDOLTEXCOORD2DPROC dolTexCoord2d = nullptr;
PFNDOLTEXCOORD2FPROC dolTexCoord2f = nullptr;
PFNDOLTEXCOORD2IPROC dolTexCoord2i = nullptr;
PFNDOLTEXCOORD2SPROC dolTexCoord2s = nullptr;
PFNDOLTEXCOORD3DPROC dolTexCoord3d = nullptr;
PFNDOLTEXCOORD3FPROC dolTexCoord3f = nullptr;
PFNDOLTEXCOORD3IPROC dolTexCoord3i = nullptr;
PFNDOLTEXCOORD3SPROC dolTexCoord3s = nullptr;
PFNDOLTEXCOORD4DPROC dolTexCoord4d = nullptr;
PFNDOLTEXCOORD4FPROC dolTexCoord4f = nullptr;
PFNDOLTEXCOORD4IPROC dolTexCoord4i = nullptr;
PFNDOLTEXCOORD4SPROC dolTexCoord4s = nullptr;
PFNDOLTEXCOORD1DVPROC dolTexCoord1dv = nullptr;
PFNDOLTEXCOORD1FVPROC dolTexCoord1fv = nullptr;
PFNDOLTEXCOORD1IVPROC dolTexCoord1iv = nullptr;
PFNDOLTEXCOORD1SVPROC dolTexCoord1sv = nullptr;
PFNDOLTEXCOORD2DVPROC dolTexCoord2dv = nullptr;
PFNDOLTEXCOORD2FVPROC dolTexCoord2fv = nullptr;
PFNDOLTEXCOORD2IVPROC dolTexCoord2iv = nullptr;
PFNDOLTEXCOORD2SVPROC dolTexCoord2sv = nullptr;
PFNDOLTEXCOORD3DVPROC dolTexCoord3dv = nullptr;
PFNDOLTEXCOORD3FVPROC dolTexCoord3fv = nullptr;
PFNDOLTEXCOORD3IVPROC dolTexCoord3iv = nullptr;
PFNDOLTEXCOORD3SVPROC dolTexCoord3sv = nullptr;
PFNDOLTEXCOORD4DVPROC dolTexCoord4dv = nullptr;
PFNDOLTEXCOORD4FVPROC dolTexCoord4fv = nullptr;
PFNDOLTEXCOORD4IVPROC dolTexCoord4iv = nullptr;
PFNDOLTEXCOORD4SVPROC dolTexCoord4sv = nullptr;
PFNDOLRASTERPOS2DPROC dolRasterPos2d = nullptr;
PFNDOLRASTERPOS2FPROC dolRasterPos2f = nullptr;
PFNDOLRASTERPOS2IPROC dolRasterPos2i = nullptr;
PFNDOLRASTERPOS2SPROC dolRasterPos2s = nullptr;
PFNDOLRASTERPOS3DPROC dolRasterPos3d = nullptr;
PFNDOLRASTERPOS3FPROC dolRasterPos3f = nullptr;
PFNDOLRASTERPOS3IPROC dolRasterPos3i = nullptr;
PFNDOLRASTERPOS3SPROC dolRasterPos3s = nullptr;
PFNDOLRASTERPOS4DPROC dolRasterPos4d = nullptr;
PFNDOLRASTERPOS4FPROC dolRasterPos4f = nullptr;
PFNDOLRASTERPOS4IPROC dolRasterPos4i = nullptr;
PFNDOLRASTERPOS4SPROC dolRasterPos4s = nullptr;
PFNDOLRASTERPOS2DVPROC dolRasterPos2dv = nullptr;
PFNDOLRASTERPOS2FVPROC dolRasterPos2fv = nullptr;
PFNDOLRASTERPOS2IVPROC dolRasterPos2iv = nullptr;
PFNDOLRASTERPOS2SVPROC dolRasterPos2sv = nullptr;
PFNDOLRASTERPOS3DVPROC dolRasterPos3dv = nullptr;
PFNDOLRASTERPOS3FVPROC dolRasterPos3fv = nullptr;
PFNDOLRASTERPOS3IVPROC dolRasterPos3iv = nullptr;
PFNDOLRASTERPOS3SVPROC dolRasterPos3sv = nullptr;
PFNDOLRASTERPOS4DVPROC dolRasterPos4dv = nullptr;
PFNDOLRASTERPOS4FVPROC dolRasterPos4fv = nullptr;
PFNDOLRASTERPOS4IVPROC dolRasterPos4iv = nullptr;
PFNDOLRASTERPOS4SVPROC dolRasterPos4sv = nullptr;
PFNDOLRECTDPROC dolRectd = nullptr;
PFNDOLRECTFPROC dolRectf = nullptr;
PFNDOLRECTIPROC dolRecti = nullptr;
PFNDOLRECTSPROC dolRects = nullptr;
PFNDOLRECTDVPROC dolRectdv = nullptr;
PFNDOLRECTFVPROC dolRectfv = nullptr;
PFNDOLRECTIVPROC dolRectiv = nullptr;
PFNDOLRECTSVPROC dolRectsv = nullptr;
PFNDOLVERTEXPOINTERPROC dolVertexPointer = nullptr;
PFNDOLNORMALPOINTERPROC dolNormalPointer = nullptr;
PFNDOLCOLORPOINTERPROC dolColorPointer = nullptr;
PFNDOLINDEXPOINTERPROC dolIndexPointer = nullptr;
PFNDOLTEXCOORDPOINTERPROC dolTexCoordPointer = nullptr;
PFNDOLEDGEFLAGPOINTERPROC dolEdgeFlagPointer = nullptr;
PFNDOLGETPOINTERVPROC dolGetPointerv = nullptr;
PFNDOLARRAYELEMENTPROC dolArrayElement = nullptr;
PFNDOLDRAWARRAYSPROC dolDrawArrays = nullptr;
PFNDOLDRAWELEMENTSPROC dolDrawElements = nullptr;
PFNDOLINTERLEAVEDARRAYSPROC dolInterleavedArrays = nullptr;
PFNDOLSHADEMODELPROC dolShadeModel = nullptr;
PFNDOLLIGHTFPROC dolLightf = nullptr;
PFNDOLLIGHTIPROC dolLighti = nullptr;
PFNDOLLIGHTFVPROC dolLightfv = nullptr;
PFNDOLLIGHTIVPROC dolLightiv = nullptr;
PFNDOLGETLIGHTFVPROC dolGetLightfv = nullptr;
PFNDOLGETLIGHTIVPROC dolGetLightiv = nullptr;
PFNDOLLIGHTMODELFPROC dolLightModelf = nullptr;
PFNDOLLIGHTMODELIPROC dolLightModeli = nullptr;
PFNDOLLIGHTMODELFVPROC dolLightModelfv = nullptr;
PFNDOLLIGHTMODELIVPROC dolLightModeliv = nullptr;
PFNDOLMATERIALFPROC dolMaterialf = nullptr;
PFNDOLMATERIALIPROC dolMateriali = nullptr;
PFNDOLMATERIALFVPROC dolMaterialfv = nullptr;
PFNDOLMATERIALIVPROC dolMaterialiv = nullptr;
PFNDOLGETMATERIALFVPROC dolGetMaterialfv = nullptr;
PFNDOLGETMATERIALIVPROC dolGetMaterialiv = nullptr;
PFNDOLCOLORMATERIALPROC dolColorMaterial = nullptr;
PFNDOLPIXELZOOMPROC dolPixelZoom = nullptr;
PFNDOLPIXELSTOREFPROC dolPixelStoref = nullptr;
PFNDOLPIXELSTOREIPROC dolPixelStorei = nullptr;
PFNDOLPIXELTRANSFERFPROC dolPixelTransferf = nullptr;
PFNDOLPIXELTRANSFERIPROC dolPixelTransferi = nullptr;
PFNDOLPIXELMAPFVPROC dolPixelMapfv = nullptr;
PFNDOLPIXELMAPUIVPROC dolPixelMapuiv = nullptr;
PFNDOLPIXELMAPUSVPROC dolPixelMapusv = nullptr;
PFNDOLGETPIXELMAPFVPROC dolGetPixelMapfv = nullptr;
PFNDOLGETPIXELMAPUIVPROC dolGetPixelMapuiv = nullptr;
PFNDOLGETPIXELMAPUSVPROC dolGetPixelMapusv = nullptr;
PFNDOLBITMAPPROC dolBitmap = nullptr;
PFNDOLREADPIXELSPROC dolReadPixels = nullptr;
PFNDOLDRAWPIXELSPROC dolDrawPixels = nullptr;
PFNDOLCOPYPIXELSPROC dolCopyPixels = nullptr;
PFNDOLSTENCILFUNCPROC dolStencilFunc = nullptr;
PFNDOLSTENCILMASKPROC dolStencilMask = nullptr;
PFNDOLSTENCILOPPROC dolStencilOp = nullptr;
PFNDOLCLEARSTENCILPROC dolClearStencil = nullptr;
PFNDOLTEXGENDPROC dolTexGend = nullptr;
PFNDOLTEXGENFPROC dolTexGenf = nullptr;
PFNDOLTEXGENIPROC dolTexGeni = nullptr;
PFNDOLTEXGENDVPROC dolTexGendv = nullptr;
PFNDOLTEXGENFVPROC dolTexGenfv = nullptr;
PFNDOLTEXGENIVPROC dolTexGeniv = nullptr;
PFNDOLGETTEXGENDVPROC dolGetTexGendv = nullptr;
PFNDOLGETTEXGENFVPROC dolGetTexGenfv = nullptr;
PFNDOLGETTEXGENIVPROC dolGetTexGeniv = nullptr;
PFNDOLTEXENVFPROC dolTexEnvf = nullptr;
PFNDOLTEXENVIPROC dolTexEnvi = nullptr;
PFNDOLTEXENVFVPROC dolTexEnvfv = nullptr;
PFNDOLTEXENVIVPROC dolTexEnviv = nullptr;
PFNDOLGETTEXENVFVPROC dolGetTexEnvfv = nullptr;
PFNDOLGETTEXENVIVPROC dolGetTexEnviv = nullptr;
PFNDOLTEXPARAMETERFPROC dolTexParameterf = nullptr;
PFNDOLTEXPARAMETERIPROC dolTexParameteri = nullptr;
PFNDOLTEXPARAMETERFVPROC dolTexParameterfv = nullptr;
PFNDOLTEXPARAMETERIVPROC dolTexParameteriv = nullptr;
PFNDOLGETTEXPARAMETERFVPROC dolGetTexParameterfv = nullptr;
PFNDOLGETTEXPARAMETERIVPROC dolGetTexParameteriv = nullptr;
PFNDOLGETTEXLEVELPARAMETERFVPROC dolGetTexLevelParameterfv = nullptr;
PFNDOLGETTEXLEVELPARAMETERIVPROC dolGetTexLevelParameteriv = nullptr;
PFNDOLTEXIMAGE1DPROC dolTexImage1D = nullptr;
PFNDOLTEXIMAGE2DPROC dolTexImage2D = nullptr;
PFNDOLGETTEXIMAGEPROC dolGetTexImage = nullptr;
PFNDOLGENTEXTURESPROC dolGenTextures = nullptr;
PFNDOLDELETETEXTURESPROC dolDeleteTextures = nullptr;
PFNDOLBINDTEXTUREPROC dolBindTexture = nullptr;
PFNDOLPRIORITIZETEXTURESPROC dolPrioritizeTextures = nullptr;
PFNDOLARETEXTURESRESIDENTPROC dolAreTexturesResident = nullptr;
PFNDOLISTEXTUREPROC dolIsTexture = nullptr;
PFNDOLTEXSUBIMAGE1DPROC dolTexSubImage1D = nullptr;
PFNDOLTEXSUBIMAGE2DPROC dolTexSubImage2D = nullptr;
PFNDOLCOPYTEXIMAGE1DPROC dolCopyTexImage1D = nullptr;
PFNDOLCOPYTEXIMAGE2DPROC dolCopyTexImage2D = nullptr;
PFNDOLCOPYTEXSUBIMAGE1DPROC dolCopyTexSubImage1D = nullptr;
PFNDOLCOPYTEXSUBIMAGE2DPROC dolCopyTexSubImage2D = nullptr;
PFNDOLMAP1DPROC dolMap1d = nullptr;
PFNDOLMAP1FPROC dolMap1f = nullptr;
PFNDOLMAP2DPROC dolMap2d = nullptr;
PFNDOLMAP2FPROC dolMap2f = nullptr;
PFNDOLGETMAPDVPROC dolGetMapdv = nullptr;
PFNDOLGETMAPFVPROC dolGetMapfv = nullptr;
PFNDOLGETMAPIVPROC dolGetMapiv = nullptr;
PFNDOLEVALCOORD1DPROC dolEvalCoord1d = nullptr;
PFNDOLEVALCOORD1FPROC dolEvalCoord1f = nullptr;
PFNDOLEVALCOORD1DVPROC dolEvalCoord1dv = nullptr;
PFNDOLEVALCOORD1FVPROC dolEvalCoord1fv = nullptr;
PFNDOLEVALCOORD2DPROC dolEvalCoord2d = nullptr;
PFNDOLEVALCOORD2FPROC dolEvalCoord2f = nullptr;
PFNDOLEVALCOORD2DVPROC dolEvalCoord2dv = nullptr;
PFNDOLEVALCOORD2FVPROC dolEvalCoord2fv = nullptr;
PFNDOLMAPGRID1DPROC dolMapGrid1d = nullptr;
PFNDOLMAPGRID1FPROC dolMapGrid1f = nullptr;
PFNDOLMAPGRID2DPROC dolMapGrid2d = nullptr;
PFNDOLMAPGRID2FPROC dolMapGrid2f = nullptr;
PFNDOLEVALPOINT1PROC dolEvalPoint1 = nullptr;
PFNDOLEVALPOINT2PROC dolEvalPoint2 = nullptr;
PFNDOLEVALMESH1PROC dolEvalMesh1 = nullptr;
PFNDOLEVALMESH2PROC dolEvalMesh2 = nullptr;
PFNDOLFOGFPROC dolFogf = nullptr;
PFNDOLFOGIPROC dolFogi = nullptr;
PFNDOLFOGFVPROC dolFogfv = nullptr;
PFNDOLFOGIVPROC dolFogiv = nullptr;
PFNDOLFEEDBACKBUFFERPROC dolFeedbackBuffer = nullptr;
PFNDOLPASSTHROUGHPROC dolPassThrough = nullptr;
PFNDOLSELECTBUFFERPROC dolSelectBuffer = nullptr;
PFNDOLINITNAMESPROC dolInitNames = nullptr;
PFNDOLLOADNAMEPROC dolLoadName = nullptr;
PFNDOLPUSHNAMEPROC dolPushName = nullptr;
PFNDOLPOPNAMEPROC dolPopName = nullptr;

// --- gl_1_2.h ---
PFNDOLCOPYTEXSUBIMAGE3DPROC dolCopyTexSubImage3D = nullptr;
PFNDOLDRAWRANGEELEMENTSPROC dolDrawRangeElements = nullptr;
PFNDOLTEXIMAGE3DPROC dolTexImage3D = nullptr;
PFNDOLTEXSUBIMAGE3DPROC dolTexSubImage3D = nullptr;

// --- gl_1_3.h ---
PFNDOLACTIVETEXTUREARBPROC dolActiveTexture = nullptr;
PFNDOLCLIENTACTIVETEXTUREARBPROC dolClientActiveTexture = nullptr;
PFNDOLCOMPRESSEDTEXIMAGE1DPROC dolCompressedTexImage1D = nullptr;
PFNDOLCOMPRESSEDTEXIMAGE2DPROC dolCompressedTexImage2D = nullptr;
PFNDOLCOMPRESSEDTEXIMAGE3DPROC dolCompressedTexImage3D = nullptr;
PFNDOLCOMPRESSEDTEXSUBIMAGE1DPROC dolCompressedTexSubImage1D = nullptr;
PFNDOLCOMPRESSEDTEXSUBIMAGE2DPROC dolCompressedTexSubImage2D = nullptr;
PFNDOLCOMPRESSEDTEXSUBIMAGE3DPROC dolCompressedTexSubImage3D = nullptr;
PFNDOLGETCOMPRESSEDTEXIMAGEPROC dolGetCompressedTexImage = nullptr;
PFNDOLLOADTRANSPOSEMATRIXDARBPROC dolLoadTransposeMatrixd = nullptr;
PFNDOLLOADTRANSPOSEMATRIXFARBPROC dolLoadTransposeMatrixf = nullptr;
PFNDOLMULTTRANSPOSEMATRIXDARBPROC dolMultTransposeMatrixd = nullptr;
PFNDOLMULTTRANSPOSEMATRIXFARBPROC dolMultTransposeMatrixf = nullptr;
PFNDOLMULTITEXCOORD1DARBPROC dolMultiTexCoord1d = nullptr;
PFNDOLMULTITEXCOORD1DVARBPROC dolMultiTexCoord1dv = nullptr;
PFNDOLMULTITEXCOORD1FARBPROC dolMultiTexCoord1f = nullptr;
PFNDOLMULTITEXCOORD1FVARBPROC dolMultiTexCoord1fv = nullptr;
PFNDOLMULTITEXCOORD1IARBPROC dolMultiTexCoord1i = nullptr;
PFNDOLMULTITEXCOORD1IVARBPROC dolMultiTexCoord1iv = nullptr;
PFNDOLMULTITEXCOORD1SARBPROC dolMultiTexCoord1s = nullptr;
PFNDOLMULTITEXCOORD1SVARBPROC dolMultiTexCoord1sv = nullptr;
PFNDOLMULTITEXCOORD2DARBPROC dolMultiTexCoord2d = nullptr;
PFNDOLMULTITEXCOORD2DVARBPROC dolMultiTexCoord2dv = nullptr;
PFNDOLMULTITEXCOORD2FARBPROC dolMultiTexCoord2f = nullptr;
PFNDOLMULTITEXCOORD2FVARBPROC dolMultiTexCoord2fv = nullptr;
PFNDOLMULTITEXCOORD2IARBPROC dolMultiTexCoord2i = nullptr;
PFNDOLMULTITEXCOORD2IVARBPROC dolMultiTexCoord2iv = nullptr;
PFNDOLMULTITEXCOORD2SARBPROC dolMultiTexCoord2s = nullptr;
PFNDOLMULTITEXCOORD2SVARBPROC dolMultiTexCoord2sv = nullptr;
PFNDOLMULTITEXCOORD3DARBPROC dolMultiTexCoord3d = nullptr;
PFNDOLMULTITEXCOORD3DVARBPROC dolMultiTexCoord3dv = nullptr;
PFNDOLMULTITEXCOORD3FARBPROC dolMultiTexCoord3f = nullptr;
PFNDOLMULTITEXCOORD3FVARBPROC dolMultiTexCoord3fv = nullptr;
PFNDOLMULTITEXCOORD3IARBPROC dolMultiTexCoord3i = nullptr;
PFNDOLMULTITEXCOORD3IVARBPROC dolMultiTexCoord3iv = nullptr;
PFNDOLMULTITEXCOORD3SARBPROC dolMultiTexCoord3s = nullptr;
PFNDOLMULTITEXCOORD3SVARBPROC dolMultiTexCoord3sv = nullptr;
PFNDOLMULTITEXCOORD4DARBPROC dolMultiTexCoord4d = nullptr;
PFNDOLMULTITEXCOORD4DVARBPROC dolMultiTexCoord4dv = nullptr;
PFNDOLMULTITEXCOORD4FARBPROC dolMultiTexCoord4f = nullptr;
PFNDOLMULTITEXCOORD4FVARBPROC dolMultiTexCoord4fv = nullptr;
PFNDOLMULTITEXCOORD4IARBPROC dolMultiTexCoord4i = nullptr;
PFNDOLMULTITEXCOORD4IVARBPROC dolMultiTexCoord4iv = nullptr;
PFNDOLMULTITEXCOORD4SARBPROC dolMultiTexCoord4s = nullptr;
PFNDOLMULTITEXCOORD4SVARBPROC dolMultiTexCoord4sv = nullptr;
PFNDOLSAMPLECOVERAGEARBPROC dolSampleCoverage = nullptr;

// --- gl_1_4.h ---
PFNDOLBLENDCOLORPROC dolBlendColor = nullptr;
PFNDOLBLENDEQUATIONPROC dolBlendEquation = nullptr;
PFNDOLBLENDFUNCSEPARATEPROC dolBlendFuncSeparate = nullptr;
PFNDOLFOGCOORDPOINTERPROC dolFogCoordPointer = nullptr;
PFNDOLFOGCOORDDPROC dolFogCoordd = nullptr;
PFNDOLFOGCOORDDVPROC dolFogCoorddv = nullptr;
PFNDOLFOGCOORDFPROC dolFogCoordf = nullptr;
PFNDOLFOGCOORDFVPROC dolFogCoordfv = nullptr;
PFNDOLMULTIDRAWARRAYSPROC dolMultiDrawArrays = nullptr;
PFNDOLMULTIDRAWELEMENTSPROC dolMultiDrawElements = nullptr;
PFNDOLPOINTPARAMETERFPROC dolPointParameterf = nullptr;
PFNDOLPOINTPARAMETERFVPROC dolPointParameterfv = nullptr;
PFNDOLPOINTPARAMETERIPROC dolPointParameteri = nullptr;
PFNDOLPOINTPARAMETERIVPROC dolPointParameteriv = nullptr;
PFNDOLSECONDARYCOLOR3BPROC dolSecondaryColor3b = nullptr;
PFNDOLSECONDARYCOLOR3BVPROC dolSecondaryColor3bv = nullptr;
PFNDOLSECONDARYCOLOR3DPROC dolSecondaryColor3d = nullptr;
PFNDOLSECONDARYCOLOR3DVPROC dolSecondaryColor3dv = nullptr;
PFNDOLSECONDARYCOLOR3FPROC dolSecondaryColor3f = nullptr;
PFNDOLSECONDARYCOLOR3FVPROC dolSecondaryColor3fv = nullptr;
PFNDOLSECONDARYCOLOR3IPROC dolSecondaryColor3i = nullptr;
PFNDOLSECONDARYCOLOR3IVPROC dolSecondaryColor3iv = nullptr;
PFNDOLSECONDARYCOLOR3SPROC dolSecondaryColor3s = nullptr;
PFNDOLSECONDARYCOLOR3SVPROC dolSecondaryColor3sv = nullptr;
PFNDOLSECONDARYCOLOR3UBPROC dolSecondaryColor3ub = nullptr;
PFNDOLSECONDARYCOLOR3UBVPROC dolSecondaryColor3ubv = nullptr;
PFNDOLSECONDARYCOLOR3UIPROC dolSecondaryColor3ui = nullptr;
PFNDOLSECONDARYCOLOR3UIVPROC dolSecondaryColor3uiv = nullptr;
PFNDOLSECONDARYCOLOR3USPROC dolSecondaryColor3us = nullptr;
PFNDOLSECONDARYCOLOR3USVPROC dolSecondaryColor3usv = nullptr;
PFNDOLSECONDARYCOLORPOINTERPROC dolSecondaryColorPointer = nullptr;
PFNDOLWINDOWPOS2DPROC dolWindowPos2d = nullptr;
PFNDOLWINDOWPOS2DVPROC dolWindowPos2dv = nullptr;
PFNDOLWINDOWPOS2FPROC dolWindowPos2f = nullptr;
PFNDOLWINDOWPOS2FVPROC dolWindowPos2fv = nullptr;
PFNDOLWINDOWPOS2IPROC dolWindowPos2i = nullptr;
PFNDOLWINDOWPOS2IVPROC dolWindowPos2iv = nullptr;
PFNDOLWINDOWPOS2SPROC dolWindowPos2s = nullptr;
PFNDOLWINDOWPOS2SVPROC dolWindowPos2sv = nullptr;
PFNDOLWINDOWPOS3DPROC dolWindowPos3d = nullptr;
PFNDOLWINDOWPOS3DVPROC dolWindowPos3dv = nullptr;
PFNDOLWINDOWPOS3FPROC dolWindowPos3f = nullptr;
PFNDOLWINDOWPOS3FVPROC dolWindowPos3fv = nullptr;
PFNDOLWINDOWPOS3IPROC dolWindowPos3i = nullptr;
PFNDOLWINDOWPOS3IVPROC dolWindowPos3iv = nullptr;
PFNDOLWINDOWPOS3SPROC dolWindowPos3s = nullptr;
PFNDOLWINDOWPOS3SVPROC dolWindowPos3sv = nullptr;

// --- gl_1_5.h ---
PFNDOLBEGINQUERYPROC dolBeginQuery = nullptr;
PFNDOLBINDBUFFERPROC dolBindBuffer = nullptr;
PFNDOLBUFFERDATAPROC dolBufferData = nullptr;
PFNDOLBUFFERSUBDATAPROC dolBufferSubData = nullptr;
PFNDOLDELETEBUFFERSPROC dolDeleteBuffers = nullptr;
PFNDOLDELETEQUERIESPROC dolDeleteQueries = nullptr;
PFNDOLENDQUERYPROC dolEndQuery = nullptr;
PFNDOLGENBUFFERSPROC dolGenBuffers = nullptr;
PFNDOLGENQUERIESPROC dolGenQueries = nullptr;
PFNDOLGETBUFFERPARAMETERIVPROC dolGetBufferParameteriv = nullptr;
PFNDOLGETBUFFERPOINTERVPROC dolGetBufferPointerv = nullptr;
PFNDOLGETBUFFERSUBDATAPROC dolGetBufferSubData = nullptr;
PFNDOLGETQUERYOBJECTIVPROC dolGetQueryObjectiv = nullptr;
PFNDOLGETQUERYOBJECTUIVPROC dolGetQueryObjectuiv = nullptr;
PFNDOLGETQUERYIVPROC dolGetQueryiv = nullptr;
PFNDOLISBUFFERPROC dolIsBuffer = nullptr;
PFNDOLISQUERYPROC dolIsQuery = nullptr;
PFNDOLMAPBUFFERPROC dolMapBuffer = nullptr;
PFNDOLUNMAPBUFFERPROC dolUnmapBuffer = nullptr;

// --- gl_2_0.h ---
PFNDOLATTACHSHADERPROC dolAttachShader = nullptr;
PFNDOLBINDATTRIBLOCATIONPROC dolBindAttribLocation = nullptr;
PFNDOLBLENDEQUATIONSEPARATEPROC dolBlendEquationSeparate = nullptr;
PFNDOLCOMPILESHADERPROC dolCompileShader = nullptr;
PFNDOLCREATEPROGRAMPROC dolCreateProgram = nullptr;
PFNDOLCREATESHADERPROC dolCreateShader = nullptr;
PFNDOLDELETEPROGRAMPROC dolDeleteProgram = nullptr;
PFNDOLDELETESHADERPROC dolDeleteShader = nullptr;
PFNDOLDETACHSHADERPROC dolDetachShader = nullptr;
PFNDOLDISABLEVERTEXATTRIBARRAYPROC dolDisableVertexAttribArray = nullptr;
PFNDOLDRAWBUFFERSPROC dolDrawBuffers = nullptr;
PFNDOLENABLEVERTEXATTRIBARRAYPROC dolEnableVertexAttribArray = nullptr;
PFNDOLGETACTIVEATTRIBPROC dolGetActiveAttrib = nullptr;
PFNDOLGETACTIVEUNIFORMPROC dolGetActiveUniform = nullptr;
PFNDOLGETATTACHEDSHADERSPROC dolGetAttachedShaders = nullptr;
PFNDOLGETATTRIBLOCATIONPROC dolGetAttribLocation = nullptr;
PFNDOLGETPROGRAMINFOLOGPROC dolGetProgramInfoLog = nullptr;
PFNDOLGETPROGRAMIVPROC dolGetProgramiv = nullptr;
PFNDOLGETSHADERINFOLOGPROC dolGetShaderInfoLog = nullptr;
PFNDOLGETSHADERSOURCEPROC dolGetShaderSource = nullptr;
PFNDOLGETSHADERIVPROC dolGetShaderiv = nullptr;
PFNDOLGETUNIFORMLOCATIONPROC dolGetUniformLocation = nullptr;
PFNDOLGETUNIFORMFVPROC dolGetUniformfv = nullptr;
PFNDOLGETUNIFORMIVPROC dolGetUniformiv = nullptr;
PFNDOLGETVERTEXATTRIBPOINTERVPROC dolGetVertexAttribPointerv = nullptr;
PFNDOLGETVERTEXATTRIBDVPROC dolGetVertexAttribdv = nullptr;
PFNDOLGETVERTEXATTRIBFVPROC dolGetVertexAttribfv = nullptr;
PFNDOLGETVERTEXATTRIBIVPROC dolGetVertexAttribiv = nullptr;
PFNDOLISPROGRAMPROC dolIsProgram = nullptr;
PFNDOLISSHADERPROC dolIsShader = nullptr;
PFNDOLLINKPROGRAMPROC dolLinkProgram = nullptr;
PFNDOLSHADERSOURCEPROC dolShaderSource = nullptr;
PFNDOLSTENCILFUNCSEPARATEPROC dolStencilFuncSeparate = nullptr;
PFNDOLSTENCILMASKSEPARATEPROC dolStencilMaskSeparate = nullptr;
PFNDOLSTENCILOPSEPARATEPROC dolStencilOpSeparate = nullptr;
PFNDOLUNIFORM1FPROC dolUniform1f = nullptr;
PFNDOLUNIFORM1FVPROC dolUniform1fv = nullptr;
PFNDOLUNIFORM1IPROC dolUniform1i = nullptr;
PFNDOLUNIFORM1IVPROC dolUniform1iv = nullptr;
PFNDOLUNIFORM2FPROC dolUniform2f = nullptr;
PFNDOLUNIFORM2FVPROC dolUniform2fv = nullptr;
PFNDOLUNIFORM2IPROC dolUniform2i = nullptr;
PFNDOLUNIFORM2IVPROC dolUniform2iv = nullptr;
PFNDOLUNIFORM3FPROC dolUniform3f = nullptr;
PFNDOLUNIFORM3FVPROC dolUniform3fv = nullptr;
PFNDOLUNIFORM3IPROC dolUniform3i = nullptr;
PFNDOLUNIFORM3IVPROC dolUniform3iv = nullptr;
PFNDOLUNIFORM4FPROC dolUniform4f = nullptr;
PFNDOLUNIFORM4FVPROC dolUniform4fv = nullptr;
PFNDOLUNIFORM4IPROC dolUniform4i = nullptr;
PFNDOLUNIFORM4IVPROC dolUniform4iv = nullptr;
PFNDOLUNIFORMMATRIX2FVPROC dolUniformMatrix2fv = nullptr;
PFNDOLUNIFORMMATRIX3FVPROC dolUniformMatrix3fv = nullptr;
PFNDOLUNIFORMMATRIX4FVPROC dolUniformMatrix4fv = nullptr;
PFNDOLUSEPROGRAMPROC dolUseProgram = nullptr;
PFNDOLVALIDATEPROGRAMPROC dolValidateProgram = nullptr;
PFNDOLVERTEXATTRIB1DPROC dolVertexAttrib1d = nullptr;
PFNDOLVERTEXATTRIB1DVPROC dolVertexAttrib1dv = nullptr;
PFNDOLVERTEXATTRIB1FPROC dolVertexAttrib1f = nullptr;
PFNDOLVERTEXATTRIB1FVPROC dolVertexAttrib1fv = nullptr;
PFNDOLVERTEXATTRIB1SPROC dolVertexAttrib1s = nullptr;
PFNDOLVERTEXATTRIB1SVPROC dolVertexAttrib1sv = nullptr;
PFNDOLVERTEXATTRIB2DPROC dolVertexAttrib2d = nullptr;
PFNDOLVERTEXATTRIB2DVPROC dolVertexAttrib2dv = nullptr;
PFNDOLVERTEXATTRIB2FPROC dolVertexAttrib2f = nullptr;
PFNDOLVERTEXATTRIB2FVPROC dolVertexAttrib2fv = nullptr;
PFNDOLVERTEXATTRIB2SPROC dolVertexAttrib2s = nullptr;
PFNDOLVERTEXATTRIB2SVPROC dolVertexAttrib2sv = nullptr;
PFNDOLVERTEXATTRIB3DPROC dolVertexAttrib3d = nullptr;
PFNDOLVERTEXATTRIB3DVPROC dolVertexAttrib3dv = nullptr;
PFNDOLVERTEXATTRIB3FPROC dolVertexAttrib3f = nullptr;
PFNDOLVERTEXATTRIB3FVPROC dolVertexAttrib3fv = nullptr;
PFNDOLVERTEXATTRIB3SPROC dolVertexAttrib3s = nullptr;
PFNDOLVERTEXATTRIB3SVPROC dolVertexAttrib3sv = nullptr;
PFNDOLVERTEXATTRIB4NBVPROC dolVertexAttrib4Nbv = nullptr;
PFNDOLVERTEXATTRIB4NIVPROC dolVertexAttrib4Niv = nullptr;
PFNDOLVERTEXATTRIB4NSVPROC dolVertexAttrib4Nsv = nullptr;
PFNDOLVERTEXATTRIB4NUBPROC dolVertexAttrib4Nub = nullptr;
PFNDOLVERTEXATTRIB4NUBVPROC dolVertexAttrib4Nubv = nullptr;
PFNDOLVERTEXATTRIB4NUIVPROC dolVertexAttrib4Nuiv = nullptr;
PFNDOLVERTEXATTRIB4NUSVPROC dolVertexAttrib4Nusv = nullptr;
PFNDOLVERTEXATTRIB4BVPROC dolVertexAttrib4bv = nullptr;
PFNDOLVERTEXATTRIB4DPROC dolVertexAttrib4d = nullptr;
PFNDOLVERTEXATTRIB4DVPROC dolVertexAttrib4dv = nullptr;
PFNDOLVERTEXATTRIB4FPROC dolVertexAttrib4f = nullptr;
PFNDOLVERTEXATTRIB4FVPROC dolVertexAttrib4fv = nullptr;
PFNDOLVERTEXATTRIB4IVPROC dolVertexAttrib4iv = nullptr;
PFNDOLVERTEXATTRIB4SPROC dolVertexAttrib4s = nullptr;
PFNDOLVERTEXATTRIB4SVPROC dolVertexAttrib4sv = nullptr;
PFNDOLVERTEXATTRIB4UBVPROC dolVertexAttrib4ubv = nullptr;
PFNDOLVERTEXATTRIB4UIVPROC dolVertexAttrib4uiv = nullptr;
PFNDOLVERTEXATTRIB4USVPROC dolVertexAttrib4usv = nullptr;
PFNDOLVERTEXATTRIBPOINTERPROC dolVertexAttribPointer = nullptr;

// --- gl_2_1.h ---
PFNDOLUNIFORMMATRIX2X3FVPROC dolUniformMatrix2x3fv = nullptr;
PFNDOLUNIFORMMATRIX2X4FVPROC dolUniformMatrix2x4fv = nullptr;
PFNDOLUNIFORMMATRIX3X2FVPROC dolUniformMatrix3x2fv = nullptr;
PFNDOLUNIFORMMATRIX3X4FVPROC dolUniformMatrix3x4fv = nullptr;
PFNDOLUNIFORMMATRIX4X2FVPROC dolUniformMatrix4x2fv = nullptr;
PFNDOLUNIFORMMATRIX4X3FVPROC dolUniformMatrix4x3fv = nullptr;

// --- gl_3_0.h ---
PFNDOLBEGINCONDITIONALRENDERPROC dolBeginConditionalRender = nullptr;
PFNDOLBEGINTRANSFORMFEEDBACKPROC dolBeginTransformFeedback = nullptr;
PFNDOLBINDFRAGDATALOCATIONPROC dolBindFragDataLocation = nullptr;
PFNDOLCLAMPCOLORPROC dolClampColor = nullptr;
PFNDOLCLEARBUFFERFIPROC dolClearBufferfi = nullptr;
PFNDOLCLEARBUFFERFVPROC dolClearBufferfv = nullptr;
PFNDOLCLEARBUFFERIVPROC dolClearBufferiv = nullptr;
PFNDOLCLEARBUFFERUIVPROC dolClearBufferuiv = nullptr;
PFNDOLCOLORMASKIPROC dolColorMaski = nullptr;
PFNDOLDISABLEIPROC dolDisablei = nullptr;
PFNDOLENABLEIPROC dolEnablei = nullptr;
PFNDOLENDCONDITIONALRENDERPROC dolEndConditionalRender = nullptr;
PFNDOLENDTRANSFORMFEEDBACKPROC dolEndTransformFeedback = nullptr;
PFNDOLGETBOOLEANI_VPROC dolGetBooleani_v = nullptr;
PFNDOLGETFRAGDATALOCATIONPROC dolGetFragDataLocation = nullptr;
PFNDOLGETSTRINGIPROC dolGetStringi = nullptr;
PFNDOLGETTEXPARAMETERIIVPROC dolGetTexParameterIiv = nullptr;
PFNDOLGETTEXPARAMETERIUIVPROC dolGetTexParameterIuiv = nullptr;
PFNDOLGETTRANSFORMFEEDBACKVARYINGPROC dolGetTransformFeedbackVarying = nullptr;
PFNDOLGETUNIFORMUIVPROC dolGetUniformuiv = nullptr;
PFNDOLGETVERTEXATTRIBIIVPROC dolGetVertexAttribIiv = nullptr;
PFNDOLGETVERTEXATTRIBIUIVPROC dolGetVertexAttribIuiv = nullptr;
PFNDOLISENABLEDIPROC dolIsEnabledi = nullptr;
PFNDOLTEXPARAMETERIIVPROC dolTexParameterIiv = nullptr;
PFNDOLTEXPARAMETERIUIVPROC dolTexParameterIuiv = nullptr;
PFNDOLTRANSFORMFEEDBACKVARYINGSPROC dolTransformFeedbackVaryings = nullptr;
PFNDOLUNIFORM1UIPROC dolUniform1ui = nullptr;
PFNDOLUNIFORM1UIVPROC dolUniform1uiv = nullptr;
PFNDOLUNIFORM2UIPROC dolUniform2ui = nullptr;
PFNDOLUNIFORM2UIVPROC dolUniform2uiv = nullptr;
PFNDOLUNIFORM3UIPROC dolUniform3ui = nullptr;
PFNDOLUNIFORM3UIVPROC dolUniform3uiv = nullptr;
PFNDOLUNIFORM4UIPROC dolUniform4ui = nullptr;
PFNDOLUNIFORM4UIVPROC dolUniform4uiv = nullptr;
PFNDOLVERTEXATTRIBI1IPROC dolVertexAttribI1i = nullptr;
PFNDOLVERTEXATTRIBI1IVPROC dolVertexAttribI1iv = nullptr;
PFNDOLVERTEXATTRIBI1UIPROC dolVertexAttribI1ui = nullptr;
PFNDOLVERTEXATTRIBI1UIVPROC dolVertexAttribI1uiv = nullptr;
PFNDOLVERTEXATTRIBI2IPROC dolVertexAttribI2i = nullptr;
PFNDOLVERTEXATTRIBI2IVPROC dolVertexAttribI2iv = nullptr;
PFNDOLVERTEXATTRIBI2UIPROC dolVertexAttribI2ui = nullptr;
PFNDOLVERTEXATTRIBI2UIVPROC dolVertexAttribI2uiv = nullptr;
PFNDOLVERTEXATTRIBI3IPROC dolVertexAttribI3i = nullptr;
PFNDOLVERTEXATTRIBI3IVPROC dolVertexAttribI3iv = nullptr;
PFNDOLVERTEXATTRIBI3UIPROC dolVertexAttribI3ui = nullptr;
PFNDOLVERTEXATTRIBI3UIVPROC dolVertexAttribI3uiv = nullptr;
PFNDOLVERTEXATTRIBI4BVPROC dolVertexAttribI4bv = nullptr;
PFNDOLVERTEXATTRIBI4IPROC dolVertexAttribI4i = nullptr;
PFNDOLVERTEXATTRIBI4IVPROC dolVertexAttribI4iv = nullptr;
PFNDOLVERTEXATTRIBI4SVPROC dolVertexAttribI4sv = nullptr;
PFNDOLVERTEXATTRIBI4UBVPROC dolVertexAttribI4ubv = nullptr;
PFNDOLVERTEXATTRIBI4UIPROC dolVertexAttribI4ui = nullptr;
PFNDOLVERTEXATTRIBI4UIVPROC dolVertexAttribI4uiv = nullptr;
PFNDOLVERTEXATTRIBI4USVPROC dolVertexAttribI4usv = nullptr;
PFNDOLVERTEXATTRIBIPOINTERPROC dolVertexAttribIPointer = nullptr;

// --- gl_3_1.h ---
PFNDOLDRAWARRAYSINSTANCEDPROC dolDrawArraysInstanced = nullptr;
PFNDOLDRAWELEMENTSINSTANCEDPROC dolDrawElementsInstanced = nullptr;
PFNDOLPRIMITIVERESTARTINDEXPROC dolPrimitiveRestartIndex = nullptr;
PFNDOLTEXBUFFERPROC dolTexBuffer = nullptr;

// --- gl_3_2.h ---
PFNDOLFRAMEBUFFERTEXTUREPROC dolFramebufferTexture = nullptr;
PFNDOLGETBUFFERPARAMETERI64VPROC dolGetBufferParameteri64v = nullptr;
PFNDOLGETINTEGER64I_VPROC dolGetInteger64i_v = nullptr;

// --- gl_4_2.h ---
PFNDOLDRAWARRAYSINSTANCEDBASEINSTANCEPROC dolDrawArraysInstancedBaseInstance = nullptr;
PFNDOLDRAWELEMENTSINSTANCEDBASEINSTANCEPROC dolDrawElementsInstancedBaseInstance = nullptr;
PFNDOLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC dolDrawElementsInstancedBaseVertexBaseInstance = nullptr;
PFNDOLGETINTERNALFORMATIVPROC dolGetInternalformativ = nullptr;
PFNDOLGETACTIVEATOMICCOUNTERBUFFERIVPROC dolGetActiveAtomicCounterBufferiv = nullptr;
PFNDOLDRAWTRANSFORMFEEDBACKINSTANCEDPROC dolDrawTransformFeedbackInstanced = nullptr;
PFNDOLDRAWTRANSFORMFEEDBACKSTREAMINSTANCEDPROC dolDrawTransformFeedbackStreamInstanced = nullptr;

// --- gl_4_3.h ---
PFNDOLCLEARBUFFERDATAPROC dolClearBufferData = nullptr;
PFNDOLCLEARBUFFERSUBDATAPROC dolClearBufferSubData = nullptr;
PFNDOLFRAMEBUFFERPARAMETERIPROC dolFramebufferParameteri = nullptr;
PFNDOLGETFRAMEBUFFERPARAMETERIVPROC dolGetFramebufferParameteriv = nullptr;
PFNDOLGETINTERNALFORMATI64VPROC dolGetInternalformati64v = nullptr;
PFNDOLINVALIDATETEXSUBIMAGEPROC dolInvalidateTexSubImage = nullptr;
PFNDOLINVALIDATETEXIMAGEPROC dolInvalidateTexImage = nullptr;
PFNDOLINVALIDATEBUFFERSUBDATAPROC dolInvalidateBufferSubData = nullptr;
PFNDOLINVALIDATEBUFFERDATAPROC dolInvalidateBufferData = nullptr;
PFNDOLINVALIDATEFRAMEBUFFERPROC dolInvalidateFramebuffer = nullptr;
PFNDOLINVALIDATESUBFRAMEBUFFERPROC dolInvalidateSubFramebuffer = nullptr;
PFNDOLMULTIDRAWARRAYSINDIRECTPROC dolMultiDrawArraysIndirect = nullptr;
PFNDOLMULTIDRAWELEMENTSINDIRECTPROC dolMultiDrawElementsIndirect = nullptr;
PFNDOLGETPROGRAMINTERFACEIVPROC dolGetProgramInterfaceiv = nullptr;
PFNDOLGETPROGRAMRESOURCEINDEXPROC dolGetProgramResourceIndex = nullptr;
PFNDOLGETPROGRAMRESOURCENAMEPROC dolGetProgramResourceName = nullptr;
PFNDOLGETPROGRAMRESOURCEIVPROC dolGetProgramResourceiv = nullptr;
PFNDOLGETPROGRAMRESOURCELOCATIONPROC dolGetProgramResourceLocation = nullptr;
PFNDOLGETPROGRAMRESOURCELOCATIONINDEXPROC dolGetProgramResourceLocationIndex = nullptr;
PFNDOLTEXBUFFERRANGEPROC dolTexBufferRange = nullptr;
PFNDOLTEXTUREVIEWPROC dolTextureView = nullptr;
PFNDOLBINDVERTEXBUFFERPROC dolBindVertexBuffer = nullptr;
PFNDOLVERTEXATTRIBFORMATPROC dolVertexAttribFormat = nullptr;
PFNDOLVERTEXATTRIBIFORMATPROC dolVertexAttribIFormat = nullptr;
PFNDOLVERTEXATTRIBLFORMATPROC dolVertexAttribLFormat = nullptr;
PFNDOLVERTEXATTRIBBINDINGPROC dolVertexAttribBinding = nullptr;
PFNDOLVERTEXBINDINGDIVISORPROC dolVertexBindingDivisor = nullptr;

// --- gl_4_4.h ---
PFNDOLCLEARTEXIMAGEPROC dolClearTexImage = nullptr;
PFNDOLCLEARTEXSUBIMAGEPROC dolClearTexSubImage = nullptr;
PFNDOLBINDBUFFERSBASEPROC dolBindBuffersBase = nullptr;
PFNDOLBINDBUFFERSRANGEPROC dolBindBuffersRange = nullptr;
PFNDOLBINDTEXTURESPROC dolBindTextures = nullptr;
PFNDOLBINDSAMPLERSPROC dolBindSamplers = nullptr;
PFNDOLBINDIMAGETEXTURESPROC dolBindImageTextures = nullptr;
PFNDOLBINDVERTEXBUFFERSPROC dolBindVertexBuffers = nullptr;

// --- gl_4_5.h ---
PFNDOLCREATETRANSFORMFEEDBACKSPROC dolCreateTransformFeedbacks = nullptr;
PFNDOLTRANSFORMFEEDBACKBUFFERBASEPROC dolTransformFeedbackBufferBase = nullptr;
PFNDOLTRANSFORMFEEDBACKBUFFERRANGEPROC dolTransformFeedbackBufferRange = nullptr;
PFNDOLGETTRANSFORMFEEDBACKIVPROC dolGetTransformFeedbackiv = nullptr;
PFNDOLGETTRANSFORMFEEDBACKI_VPROC dolGetTransformFeedbacki_v = nullptr;
PFNDOLGETTRANSFORMFEEDBACKI64_VPROC dolGetTransformFeedbacki64_v = nullptr;
PFNDOLCREATEBUFFERSPROC dolCreateBuffers = nullptr;
PFNDOLNAMEDBUFFERSTORAGEPROC dolNamedBufferStorage = nullptr;
PFNDOLNAMEDBUFFERDATAPROC dolNamedBufferData = nullptr;
PFNDOLNAMEDBUFFERSUBDATAPROC dolNamedBufferSubData = nullptr;
PFNDOLCOPYNAMEDBUFFERSUBDATAPROC dolCopyNamedBufferSubData = nullptr;
PFNDOLCLEARNAMEDBUFFERDATAPROC dolClearNamedBufferData = nullptr;
PFNDOLCLEARNAMEDBUFFERSUBDATAPROC dolClearNamedBufferSubData = nullptr;
PFNDOLMAPNAMEDBUFFERPROC dolMapNamedBuffer = nullptr;
PFNDOLMAPNAMEDBUFFERRANGEPROC dolMapNamedBufferRange = nullptr;
PFNDOLUNMAPNAMEDBUFFERPROC dolUnmapNamedBuffer = nullptr;
PFNDOLFLUSHMAPPEDNAMEDBUFFERRANGEPROC dolFlushMappedNamedBufferRange = nullptr;
PFNDOLGETNAMEDBUFFERPARAMETERIVPROC dolGetNamedBufferParameteriv = nullptr;
PFNDOLGETNAMEDBUFFERPARAMETERI64VPROC dolGetNamedBufferParameteri64v = nullptr;
PFNDOLGETNAMEDBUFFERPOINTERVPROC dolGetNamedBufferPointerv = nullptr;
PFNDOLGETNAMEDBUFFERSUBDATAPROC dolGetNamedBufferSubData = nullptr;
PFNDOLCREATEFRAMEBUFFERSPROC dolCreateFramebuffers = nullptr;
PFNDOLNAMEDFRAMEBUFFERRENDERBUFFERPROC dolNamedFramebufferRenderbuffer = nullptr;
PFNDOLNAMEDFRAMEBUFFERPARAMETERIPROC dolNamedFramebufferParameteri = nullptr;
PFNDOLNAMEDFRAMEBUFFERTEXTUREPROC dolNamedFramebufferTexture = nullptr;
PFNDOLNAMEDFRAMEBUFFERTEXTURELAYERPROC dolNamedFramebufferTextureLayer = nullptr;
PFNDOLNAMEDFRAMEBUFFERDRAWBUFFERPROC dolNamedFramebufferDrawBuffer = nullptr;
PFNDOLNAMEDFRAMEBUFFERDRAWBUFFERSPROC dolNamedFramebufferDrawBuffers = nullptr;
PFNDOLNAMEDFRAMEBUFFERREADBUFFERPROC dolNamedFramebufferReadBuffer = nullptr;
PFNDOLINVALIDATENAMEDFRAMEBUFFERDATAPROC dolInvalidateNamedFramebufferData = nullptr;
PFNDOLINVALIDATENAMEDFRAMEBUFFERSUBDATAPROC dolInvalidateNamedFramebufferSubData = nullptr;
PFNDOLCLEARNAMEDFRAMEBUFFERIVPROC dolClearNamedFramebufferiv = nullptr;
PFNDOLCLEARNAMEDFRAMEBUFFERUIVPROC dolClearNamedFramebufferuiv = nullptr;
PFNDOLCLEARNAMEDFRAMEBUFFERFVPROC dolClearNamedFramebufferfv = nullptr;
PFNDOLCLEARNAMEDFRAMEBUFFERFIPROC dolClearNamedFramebufferfi = nullptr;
PFNDOLBLITNAMEDFRAMEBUFFERPROC dolBlitNamedFramebuffer = nullptr;
PFNDOLCHECKNAMEDFRAMEBUFFERSTATUSPROC dolCheckNamedFramebufferStatus = nullptr;
PFNDOLGETNAMEDFRAMEBUFFERPARAMETERIVPROC dolGetNamedFramebufferParameteriv = nullptr;
PFNDOLGETNAMEDFRAMEBUFFERATTACHMENTPARAMETERIVPROC dolGetNamedFramebufferAttachmentParameteriv = nullptr;
PFNDOLCREATERENDERBUFFERSPROC dolCreateRenderbuffers = nullptr;
PFNDOLNAMEDRENDERBUFFERSTORAGEPROC dolNamedRenderbufferStorage = nullptr;
PFNDOLNAMEDRENDERBUFFERSTORAGEMULTISAMPLEPROC dolNamedRenderbufferStorageMultisample = nullptr;
PFNDOLGETNAMEDRENDERBUFFERPARAMETERIVPROC dolGetNamedRenderbufferParameteriv = nullptr;
PFNDOLCREATETEXTURESPROC dolCreateTextures = nullptr;
PFNDOLTEXTUREBUFFERPROC dolTextureBuffer = nullptr;
PFNDOLTEXTUREBUFFERRANGEPROC dolTextureBufferRange = nullptr;
PFNDOLTEXTURESTORAGE1DPROC dolTextureStorage1D = nullptr;
PFNDOLTEXTURESTORAGE2DPROC dolTextureStorage2D = nullptr;
PFNDOLTEXTURESTORAGE3DPROC dolTextureStorage3D = nullptr;
PFNDOLTEXTURESTORAGE2DMULTISAMPLEPROC dolTextureStorage2DMultisample = nullptr;
PFNDOLTEXTURESTORAGE3DMULTISAMPLEPROC dolTextureStorage3DMultisample = nullptr;
PFNDOLTEXTURESUBIMAGE1DPROC dolTextureSubImage1D = nullptr;
PFNDOLTEXTURESUBIMAGE2DPROC dolTextureSubImage2D = nullptr;
PFNDOLTEXTURESUBIMAGE3DPROC dolTextureSubImage3D = nullptr;
PFNDOLCOMPRESSEDTEXTURESUBIMAGE1DPROC dolCompressedTextureSubImage1D = nullptr;
PFNDOLCOMPRESSEDTEXTURESUBIMAGE2DPROC dolCompressedTextureSubImage2D = nullptr;
PFNDOLCOMPRESSEDTEXTURESUBIMAGE3DPROC dolCompressedTextureSubImage3D = nullptr;
PFNDOLCOPYTEXTURESUBIMAGE1DPROC dolCopyTextureSubImage1D = nullptr;
PFNDOLCOPYTEXTURESUBIMAGE2DPROC dolCopyTextureSubImage2D = nullptr;
PFNDOLCOPYTEXTURESUBIMAGE3DPROC dolCopyTextureSubImage3D = nullptr;
PFNDOLTEXTUREPARAMETERFPROC dolTextureParameterf = nullptr;
PFNDOLTEXTUREPARAMETERFVPROC dolTextureParameterfv = nullptr;
PFNDOLTEXTUREPARAMETERIPROC dolTextureParameteri = nullptr;
PFNDOLTEXTUREPARAMETERIIVPROC dolTextureParameterIiv = nullptr;
PFNDOLTEXTUREPARAMETERIUIVPROC dolTextureParameterIuiv = nullptr;
PFNDOLTEXTUREPARAMETERIVPROC dolTextureParameteriv = nullptr;
PFNDOLGENERATETEXTUREMIPMAPPROC dolGenerateTextureMipmap = nullptr;
PFNDOLBINDTEXTUREUNITPROC dolBindTextureUnit = nullptr;
PFNDOLGETTEXTUREIMAGEPROC dolGetTextureImage = nullptr;
PFNDOLGETCOMPRESSEDTEXTUREIMAGEPROC dolGetCompressedTextureImage = nullptr;
PFNDOLGETTEXTURELEVELPARAMETERFVPROC dolGetTextureLevelParameterfv = nullptr;
PFNDOLGETTEXTURELEVELPARAMETERIVPROC dolGetTextureLevelParameteriv = nullptr;
PFNDOLGETTEXTUREPARAMETERFVPROC dolGetTextureParameterfv = nullptr;
PFNDOLGETTEXTUREPARAMETERIIVPROC dolGetTextureParameterIiv = nullptr;
PFNDOLGETTEXTUREPARAMETERIUIVPROC dolGetTextureParameterIuiv = nullptr;
PFNDOLGETTEXTUREPARAMETERIVPROC dolGetTextureParameteriv = nullptr;
PFNDOLCREATEVERTEXARRAYSPROC dolCreateVertexArrays = nullptr;
PFNDOLDISABLEVERTEXARRAYATTRIBPROC dolDisableVertexArrayAttrib = nullptr;
PFNDOLENABLEVERTEXARRAYATTRIBPROC dolEnableVertexArrayAttrib = nullptr;
PFNDOLVERTEXARRAYELEMENTBUFFERPROC dolVertexArrayElementBuffer = nullptr;
PFNDOLVERTEXARRAYVERTEXBUFFERPROC dolVertexArrayVertexBuffer = nullptr;
PFNDOLVERTEXARRAYVERTEXBUFFERSPROC dolVertexArrayVertexBuffers = nullptr;
PFNDOLVERTEXARRAYATTRIBBINDINGPROC dolVertexArrayAttribBinding = nullptr;
PFNDOLVERTEXARRAYATTRIBFORMATPROC dolVertexArrayAttribFormat = nullptr;
PFNDOLVERTEXARRAYATTRIBIFORMATPROC dolVertexArrayAttribIFormat = nullptr;
PFNDOLVERTEXARRAYATTRIBLFORMATPROC dolVertexArrayAttribLFormat = nullptr;
PFNDOLVERTEXARRAYBINDINGDIVISORPROC dolVertexArrayBindingDivisor = nullptr;
PFNDOLGETVERTEXARRAYIVPROC dolGetVertexArrayiv = nullptr;
PFNDOLGETVERTEXARRAYINDEXEDIVPROC dolGetVertexArrayIndexediv = nullptr;
PFNDOLGETVERTEXARRAYINDEXED64IVPROC dolGetVertexArrayIndexed64iv = nullptr;
PFNDOLCREATESAMPLERSPROC dolCreateSamplers = nullptr;
PFNDOLCREATEPROGRAMPIPELINESPROC dolCreateProgramPipelines = nullptr;
PFNDOLCREATEQUERIESPROC dolCreateQueries = nullptr;
PFNDOLGETQUERYBUFFEROBJECTI64VPROC dolGetQueryBufferObjecti64v = nullptr;
PFNDOLGETQUERYBUFFEROBJECTIVPROC dolGetQueryBufferObjectiv = nullptr;
PFNDOLGETQUERYBUFFEROBJECTUI64VPROC dolGetQueryBufferObjectui64v = nullptr;
PFNDOLGETQUERYBUFFEROBJECTUIVPROC dolGetQueryBufferObjectuiv = nullptr;
PFNDOLMEMORYBARRIERBYREGIONPROC dolMemoryBarrierByRegion = nullptr;
PFNDOLGETTEXTURESUBIMAGEPROC dolGetTextureSubImage = nullptr;
PFNDOLGETCOMPRESSEDTEXTURESUBIMAGEPROC dolGetCompressedTextureSubImage = nullptr;
PFNDOLGETGRAPHICSRESETSTATUSPROC dolGetGraphicsResetStatus = nullptr;
PFNDOLGETNCOMPRESSEDTEXIMAGEPROC dolGetnCompressedTexImage = nullptr;
PFNDOLGETNTEXIMAGEPROC dolGetnTexImage = nullptr;
PFNDOLGETNUNIFORMDVPROC dolGetnUniformdv = nullptr;
PFNDOLGETNUNIFORMFVPROC dolGetnUniformfv = nullptr;
PFNDOLGETNUNIFORMIVPROC dolGetnUniformiv = nullptr;
PFNDOLGETNUNIFORMUIVPROC dolGetnUniformuiv = nullptr;
PFNDOLREADNPIXELSPROC dolReadnPixels = nullptr;
PFNDOLGETNMAPDVPROC dolGetnMapdv = nullptr;
PFNDOLGETNMAPFVPROC dolGetnMapfv = nullptr;
PFNDOLGETNMAPIVPROC dolGetnMapiv = nullptr;
PFNDOLGETNPIXELMAPFVPROC dolGetnPixelMapfv = nullptr;
PFNDOLGETNPIXELMAPUIVPROC dolGetnPixelMapuiv = nullptr;
PFNDOLGETNPIXELMAPUSVPROC dolGetnPixelMapusv = nullptr;
PFNDOLGETNPOLYGONSTIPPLEPROC dolGetnPolygonStipple = nullptr;
PFNDOLGETNCOLORTABLEPROC dolGetnColorTable = nullptr;
PFNDOLGETNCONVOLUTIONFILTERPROC dolGetnConvolutionFilter = nullptr;
PFNDOLGETNSEPARABLEFILTERPROC dolGetnSeparableFilter = nullptr;
PFNDOLGETNHISTOGRAMPROC dolGetnHistogram = nullptr;
PFNDOLGETNMINMAXPROC dolGetnMinmax = nullptr;
PFNDOLTEXTUREBARRIERPROC dolTextureBarrier = nullptr;

namespace GLExtensions
{
namespace
{
// GL version as major * 100 + minor * 10, e.g. 430.
u32 s_version = 0;

// Feature names that are available: the context's extension strings plus the pseudo names
// "VERSION_GL", "VERSION_x_y" (desktop) and "VERSION_GLES_*" (ES), and, on desktop, the
// extensions that the core version includes.
std::unordered_set<std::string> s_features;

bool Has(const std::string& name)
{
  return s_features.contains(name);
}

bool Has(std::string_view name)
{
  return Has(std::string(name));
}

// Requirement strings are evaluated left to right, starting from "true": a plain token ANDs
// its availability in, "!token" ANDs its absence in, "|token" ORs its availability in. So
// "A !B |C" means (A and not B) or C.
bool RequirementMet(std::string_view requirement)
{
  bool met = true;
  std::size_t pos = 0;
  while (pos < requirement.size())
  {
    const std::size_t start = requirement.find_first_not_of(" \t\n\r\f\v", pos);
    if (start == std::string_view::npos)
      break;
    std::size_t end = requirement.find_first_of(" \t\n\r\f\v", start);
    if (end == std::string_view::npos)
      end = requirement.size();
    const std::string_view token = requirement.substr(start, end - start);
    pos = end;

    if (token.front() == '!')
      met = met && !Has(token.substr(1));
    else if (token.front() == '|')
      met = met || Has(token.substr(1));
    else
      met = met && Has(token);
  }
  return met;
}

void* Resolve(GLContext* context, const char* name, void** slot)
{
  *slot = context->GetFuncAddress(name);
  if (*slot == nullptr)
    ERROR_LOG_FMT(VIDEO, "Couldn't load function {}", name);
  return *slot;
}

// Extensions promoted to core in each desktop version. Drivers do not always list a core
// feature as an extension too, so they are added for the version the context reports.
// Extensions that changed their entry point names when they became core are left out.
constexpr const char* CORE_450[] = {
    "GL_ARB_ES3_1_compatibility", "GL_ARB_clip_control", "GL_ARB_conditional_render_inverted",
    "GL_ARB_cull_distance", "GL_ARB_derivative_control", "GL_ARB_direct_state_access",
    "GL_ARB_get_texture_sub_image", "GL_ARB_robustness", "GL_ARB_shader_texture_image_samples",
    "GL_ARB_texture_barrier", "VERSION_4_5",
};
constexpr const char* CORE_440[] = {
    "GL_ARB_buffer_storage", "GL_ARB_clear_texture", "GL_ARB_enhanced_layouts", "GL_ARB_multi_bind",
    "GL_ARB_query_buffer_object", "GL_ARB_texture_mirror_clamp_to_edge", "GL_ARB_texture_stencil8",
    "GL_ARB_vertex_type_10f_11f_11f_rev", "VERSION_4_4",
};
constexpr const char* CORE_430[] = {
    "GL_ARB_ES3_compatibility", "GL_ARB_arrays_of_arrays", "GL_ARB_clear_buffer_object",
    "GL_ARB_compute_shader", "GL_ARB_copy_image", "GL_ARB_explicit_uniform_location",
    "GL_ARB_fragment_layer_viewport", "GL_ARB_framebuffer_no_attachments",
    "GL_ARB_internalformat_query2", "GL_ARB_invalidate_subdata", "GL_ARB_multi_draw_indirect",
    "GL_ARB_program_interface_query", "GL_ARB_shader_image_size",
    "GL_ARB_shader_storage_buffer_object", "GL_ARB_stencil_texturing",
    "GL_ARB_texture_buffer_range", "GL_ARB_texture_query_levels",
    "GL_ARB_texture_storage_multisample", "GL_ARB_texture_view", "GL_ARB_vertex_attrib_binding",
    "VERSION_4_3",
};
constexpr const char* CORE_420[] = {
    "GL_ARB_base_instance", "GL_ARB_compressed_texture_pixel_storage", "GL_ARB_conservative_depth",
    "GL_ARB_internalformat_query", "GL_ARB_map_buffer_alignment", "GL_ARB_shader_atomic_counters",
    "GL_ARB_shader_image_load_store", "GL_ARB_shading_language_420pack",
    "GL_ARB_shading_language_packing", "GL_ARB_texture_compression_BPTC", "GL_ARB_texture_storage",
    "GL_ARB_transform_feedback_instanced", "VERSION_4_2",
};
constexpr const char* CORE_410[] = {
    "GL_ARB_ES2_compatibility", "GL_ARB_get_program_binary", "GL_ARB_separate_shader_objects",
    "GL_ARB_shader_precision", "GL_ARB_vertex_attrib_64_bit", "GL_ARB_viewport_array",
    "VERSION_4_1",
};
constexpr const char* CORE_400[] = {
    "GL_ARB_draw_indirect", "GL_ARB_gpu_shader5", "GL_ARB_gpu_shader_fp64", "GL_ARB_sample_shading",
    "GL_ARB_shader_subroutine", "GL_ARB_tessellation_shader", "GL_ARB_texture_buffer_object_rgb32",
    "GL_ARB_texture_cube_map_array", "GL_ARB_texture_gather", "GL_ARB_texture_query_lod",
    "GL_ARB_transform_feedback2", "GL_ARB_transform_feedback3", "VERSION_4_0",
};
constexpr const char* CORE_330[] = {
    "GL_ARB_shader_bit_encoding", "GL_ARB_blend_func_extended", "GL_ARB_explicit_attrib_location",
    "GL_ARB_occlusion_query2", "GL_ARB_sampler_objects", "GL_ARB_texture_swizzle",
    "GL_ARB_timer_query", "GL_ARB_instanced_arrays", "GL_ARB_texture_rgb10_a2ui",
    "GL_ARB_vertex_type_2_10_10_10_rev", "VERSION_3_3",
};
constexpr const char* CORE_320[] = {
    "GL_ARB_geometry_shader4", "GL_ARB_sync", "GL_ARB_vertex_array_bgra",
    "GL_ARB_draw_elements_base_vertex", "GL_ARB_seamless_cube_map", "GL_ARB_texture_multisample",
    "GL_ARB_fragment_coord_conventions", "GL_ARB_provoking_vertex", "GL_ARB_depth_clamp",
    "VERSION_3_2",
};
constexpr const char* CORE_310[] = {
    "GL_ARB_draw_instanced", "GL_ARB_copy_buffer", "GL_ARB_texture_buffer_object",
    "GL_ARB_texture_rectangle", "GL_ARB_uniform_buffer_object", "VERSION_3_1",
};
constexpr const char* CORE_300[] = {
    "GL_ARB_map_buffer_range", "GL_ARB_color_buffer_float", "GL_ARB_texture_float",
    "GL_ARB_half_float_pixel", "GL_ARB_framebuffer_object", "GL_ARB_texture_float",
    "GL_ARB_vertex_array_object", "GL_ARB_depth_buffer_float", "VERSION_3_0",
};

struct CoreLevel
{
  u32 version;
  std::span<const char* const> promoted;
};

// Newest first. A context of version V gets its own level and every older one.
constexpr CoreLevel CORE_LEVELS[] = {
    {450, CORE_450},
    {440, CORE_440},
    {430, CORE_430},
    {420, CORE_420},
    {410, CORE_410},
    {400, CORE_400},
    {330, CORE_330},
    {320, CORE_320},
    {310, CORE_310},
    {300, CORE_300},
};

// Versions below 3.0 that bring no promoted extensions.
constexpr u32 PRE_GL3_VERSIONS[] = {210, 200, 150, 140, 130, 121, 120, 110, 100};

void AddDesktopCoreFeatures()
{
  // A version that is neither one of the levels nor one of the old versions (for instance
  // 4.6) counts as having every level.
  std::size_t first_level = 0;
  for (u32 old_version : PRE_GL3_VERSIONS)
  {
    if (s_version == old_version)
      return;
  }
  for (std::size_t i = 0; i < std::size(CORE_LEVELS); ++i)
  {
    if (CORE_LEVELS[i].version == s_version)
    {
      first_level = i;
      break;
    }
  }
  for (std::size_t i = first_level; i < std::size(CORE_LEVELS); ++i)
  {
    for (const char* name : CORE_LEVELS[i].promoted)
      s_features.emplace(name);
  }
}

void AddESVersionFeatures()
{
  // 3.1 and 3.0 are recognised; anything else is treated as 3.2 or newer.
  if (s_version != 300 && s_version != 310)
    s_features.emplace("VERSION_GLES_3_2");
  if (s_version != 300)
    s_features.emplace("VERSION_GLES_3_1");
  s_features.emplace("VERSION_GLES_3");
  s_features.emplace("VERSION_GLES_2");
}

void AddWords(const char* text)
{
  if (text == nullptr)
    return;
  const std::string_view all(text);
  std::size_t pos = 0;
  while (true)
  {
    const std::size_t start = all.find_first_not_of(" \t\n\r\f\v", pos);
    if (start == std::string_view::npos)
      return;
    std::size_t end = all.find_first_of(" \t\n\r\f\v", start);
    if (end == std::string_view::npos)
      end = all.size();
    s_features.emplace(all.substr(start, end - start));
    pos = end;
  }
}

void BuildFeatureList(const GLContext* context)
{
  s_features.clear();

  if (context->IsGLES())
  {
    AddESVersionFeatures();
  }
  else
  {
    AddDesktopCoreFeatures();
    s_features.emplace("VERSION_GL");
  }

  if (s_version < 300)
  {
    // GL 2.x / ES 2.0: one space-separated string.
    AddWords(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)));
    return;
  }

  // GL 3.0+ / ES 3.0+: one string per index.
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; ++i)
  {
    const auto* name =
        reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
    if (name != nullptr)
      s_features.emplace(name);
  }
}

void ReadVersion()
{
  GLint major = 0;
  GLint minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);

  // GL_MAJOR_VERSION only exists from 3.0 on; an error means an older context.
  if (glGetError() != GL_NO_ERROR)
  {
    s_version = 210;
    return;
  }
  s_version = static_cast<u32>(major * 100 + minor * 10);
}

// Common requirement strings.
constexpr const char* DESKTOP = "VERSION_GL";
constexpr const char* DESKTOP_OR_ES2 = "VERSION_GL |VERSION_GLES_2";
constexpr const char* DESKTOP_OR_ES3 = "VERSION_GL |VERSION_GLES_3";
constexpr const char* DESKTOP_OR_ES32 = "VERSION_GL |VERSION_GLES_3_2";

struct EntryPoint
{
  void** slot;
  const char* name;
  const char* requirement;
};

// RP_GL(fn, req): look fn up by its own name. RP_GL_AS(fn, suffix, req): look up fn's name
// with a vendor suffix (e.g. "EXT") but store it in fn's slot. The table is walked in order,
// so where several rows fill the same slot, the last one whose requirement holds wins.
#define RP_GL(fn, req) {reinterpret_cast<void**>(&fn), #fn, req}
#define RP_GL_AS(fn, suffix, req) {reinterpret_cast<void**>(&fn), #fn suffix, req}

const EntryPoint ENTRY_POINTS[] = {
    // gl_1_1.h
    RP_GL(glClearIndex, DESKTOP),
    RP_GL(glIndexMask, DESKTOP),
    RP_GL(glAlphaFunc, DESKTOP),
    RP_GL(glLogicOp, DESKTOP),
    RP_GL(glPointSize, DESKTOP),
    RP_GL(glLineStipple, DESKTOP),
    RP_GL(glPolygonMode, DESKTOP),
    RP_GL(glPolygonStipple, DESKTOP),
    RP_GL(glGetPolygonStipple, DESKTOP),
    RP_GL(glEdgeFlag, DESKTOP),
    RP_GL(glEdgeFlagv, DESKTOP),
    RP_GL(glClipPlane, DESKTOP),
    RP_GL(glGetClipPlane, DESKTOP),
    RP_GL(glDrawBuffer, DESKTOP),
    RP_GL(glEnableClientState, DESKTOP),
    RP_GL(glDisableClientState, DESKTOP),
    RP_GL(glGetDoublev, DESKTOP),
    RP_GL(glPushAttrib, DESKTOP),
    RP_GL(glPopAttrib, DESKTOP),
    RP_GL(glPushClientAttrib, DESKTOP),
    RP_GL(glPopClientAttrib, DESKTOP),
    RP_GL(glRenderMode, DESKTOP),
    RP_GL(glClearDepth, DESKTOP),
    RP_GL(glDepthRange, DESKTOP),
    RP_GL(glClearAccum, DESKTOP),
    RP_GL(glAccum, DESKTOP),
    RP_GL(glMatrixMode, DESKTOP),
    RP_GL(glOrtho, DESKTOP),
    RP_GL(glFrustum, DESKTOP),
    RP_GL(glPushMatrix, DESKTOP),
    RP_GL(glPopMatrix, DESKTOP),
    RP_GL(glLoadIdentity, DESKTOP),
    RP_GL(glLoadMatrixd, DESKTOP),
    RP_GL(glLoadMatrixf, DESKTOP),
    RP_GL(glMultMatrixd, DESKTOP),
    RP_GL(glMultMatrixf, DESKTOP),
    RP_GL(glRotated, DESKTOP),
    RP_GL(glRotatef, DESKTOP),
    RP_GL(glScaled, DESKTOP),
    RP_GL(glScalef, DESKTOP),
    RP_GL(glTranslated, DESKTOP),
    RP_GL(glTranslatef, DESKTOP),
    RP_GL(glIsList, DESKTOP),
    RP_GL(glDeleteLists, DESKTOP),
    RP_GL(glGenLists, DESKTOP),
    RP_GL(glNewList, DESKTOP),
    RP_GL(glEndList, DESKTOP),
    RP_GL(glCallList, DESKTOP),
    RP_GL(glCallLists, DESKTOP),
    RP_GL(glListBase, DESKTOP),
    RP_GL(glBegin, DESKTOP),
    RP_GL(glEnd, DESKTOP),
    RP_GL(glVertex2d, DESKTOP),
    RP_GL(glVertex2f, DESKTOP),
    RP_GL(glVertex2i, DESKTOP),
    RP_GL(glVertex2s, DESKTOP),
    RP_GL(glVertex3d, DESKTOP),
    RP_GL(glVertex3f, DESKTOP),
    RP_GL(glVertex3i, DESKTOP),
    RP_GL(glVertex3s, DESKTOP),
    RP_GL(glVertex4d, DESKTOP),
    RP_GL(glVertex4f, DESKTOP),
    RP_GL(glVertex4i, DESKTOP),
    RP_GL(glVertex4s, DESKTOP),
    RP_GL(glVertex2dv, DESKTOP),
    RP_GL(glVertex2fv, DESKTOP),
    RP_GL(glVertex2iv, DESKTOP),
    RP_GL(glVertex2sv, DESKTOP),
    RP_GL(glVertex3dv, DESKTOP),
    RP_GL(glVertex3fv, DESKTOP),
    RP_GL(glVertex3iv, DESKTOP),
    RP_GL(glVertex3sv, DESKTOP),
    RP_GL(glVertex4dv, DESKTOP),
    RP_GL(glVertex4fv, DESKTOP),
    RP_GL(glVertex4iv, DESKTOP),
    RP_GL(glVertex4sv, DESKTOP),
    RP_GL(glNormal3b, DESKTOP),
    RP_GL(glNormal3d, DESKTOP),
    RP_GL(glNormal3f, DESKTOP),
    RP_GL(glNormal3i, DESKTOP),
    RP_GL(glNormal3s, DESKTOP),
    RP_GL(glNormal3bv, DESKTOP),
    RP_GL(glNormal3dv, DESKTOP),
    RP_GL(glNormal3fv, DESKTOP),
    RP_GL(glNormal3iv, DESKTOP),
    RP_GL(glNormal3sv, DESKTOP),
    RP_GL(glIndexd, DESKTOP),
    RP_GL(glIndexf, DESKTOP),
    RP_GL(glIndexi, DESKTOP),
    RP_GL(glIndexs, DESKTOP),
    RP_GL(glIndexub, DESKTOP),
    RP_GL(glIndexdv, DESKTOP),
    RP_GL(glIndexfv, DESKTOP),
    RP_GL(glIndexiv, DESKTOP),
    RP_GL(glIndexsv, DESKTOP),
    RP_GL(glIndexubv, DESKTOP),
    RP_GL(glColor3b, DESKTOP),
    RP_GL(glColor3d, DESKTOP),
    RP_GL(glColor3f, DESKTOP),
    RP_GL(glColor3i, DESKTOP),
    RP_GL(glColor3s, DESKTOP),
    RP_GL(glColor3ub, DESKTOP),
    RP_GL(glColor3ui, DESKTOP),
    RP_GL(glColor3us, DESKTOP),
    RP_GL(glColor4b, DESKTOP),
    RP_GL(glColor4d, DESKTOP),
    RP_GL(glColor4f, DESKTOP),
    RP_GL(glColor4i, DESKTOP),
    RP_GL(glColor4s, DESKTOP),
    RP_GL(glColor4ub, DESKTOP),
    RP_GL(glColor4ui, DESKTOP),
    RP_GL(glColor4us, DESKTOP),
    RP_GL(glColor3bv, DESKTOP),
    RP_GL(glColor3dv, DESKTOP),
    RP_GL(glColor3fv, DESKTOP),
    RP_GL(glColor3iv, DESKTOP),
    RP_GL(glColor3sv, DESKTOP),
    RP_GL(glColor3ubv, DESKTOP),
    RP_GL(glColor3uiv, DESKTOP),
    RP_GL(glColor3usv, DESKTOP),
    RP_GL(glColor4bv, DESKTOP),
    RP_GL(glColor4dv, DESKTOP),
    RP_GL(glColor4fv, DESKTOP),
    RP_GL(glColor4iv, DESKTOP),
    RP_GL(glColor4sv, DESKTOP),
    RP_GL(glColor4ubv, DESKTOP),
    RP_GL(glColor4uiv, DESKTOP),
    RP_GL(glColor4usv, DESKTOP),
    RP_GL(glTexCoord1d, DESKTOP),
    RP_GL(glTexCoord1f, DESKTOP),
    RP_GL(glTexCoord1i, DESKTOP),
    RP_GL(glTexCoord1s, DESKTOP),
    RP_GL(glTexCoord2d, DESKTOP),
    RP_GL(glTexCoord2f, DESKTOP),
    RP_GL(glTexCoord2i, DESKTOP),
    RP_GL(glTexCoord2s, DESKTOP),
    RP_GL(glTexCoord3d, DESKTOP),
    RP_GL(glTexCoord3f, DESKTOP),
    RP_GL(glTexCoord3i, DESKTOP),
    RP_GL(glTexCoord3s, DESKTOP),
    RP_GL(glTexCoord4d, DESKTOP),
    RP_GL(glTexCoord4f, DESKTOP),
    RP_GL(glTexCoord4i, DESKTOP),
    RP_GL(glTexCoord4s, DESKTOP),
    RP_GL(glTexCoord1dv, DESKTOP),
    RP_GL(glTexCoord1fv, DESKTOP),
    RP_GL(glTexCoord1iv, DESKTOP),
    RP_GL(glTexCoord1sv, DESKTOP),
    RP_GL(glTexCoord2dv, DESKTOP),
    RP_GL(glTexCoord2fv, DESKTOP),
    RP_GL(glTexCoord2iv, DESKTOP),
    RP_GL(glTexCoord2sv, DESKTOP),
    RP_GL(glTexCoord3dv, DESKTOP),
    RP_GL(glTexCoord3fv, DESKTOP),
    RP_GL(glTexCoord3iv, DESKTOP),
    RP_GL(glTexCoord3sv, DESKTOP),
    RP_GL(glTexCoord4dv, DESKTOP),
    RP_GL(glTexCoord4fv, DESKTOP),
    RP_GL(glTexCoord4iv, DESKTOP),
    RP_GL(glTexCoord4sv, DESKTOP),
    RP_GL(glRasterPos2d, DESKTOP),
    RP_GL(glRasterPos2f, DESKTOP),
    RP_GL(glRasterPos2i, DESKTOP),
    RP_GL(glRasterPos2s, DESKTOP),
    RP_GL(glRasterPos3d, DESKTOP),
    RP_GL(glRasterPos3f, DESKTOP),
    RP_GL(glRasterPos3i, DESKTOP),
    RP_GL(glRasterPos3s, DESKTOP),
    RP_GL(glRasterPos4d, DESKTOP),
    RP_GL(glRasterPos4f, DESKTOP),
    RP_GL(glRasterPos4i, DESKTOP),
    RP_GL(glRasterPos4s, DESKTOP),
    RP_GL(glRasterPos2dv, DESKTOP),
    RP_GL(glRasterPos2fv, DESKTOP),
    RP_GL(glRasterPos2iv, DESKTOP),
    RP_GL(glRasterPos2sv, DESKTOP),
    RP_GL(glRasterPos3dv, DESKTOP),
    RP_GL(glRasterPos3fv, DESKTOP),
    RP_GL(glRasterPos3iv, DESKTOP),
    RP_GL(glRasterPos3sv, DESKTOP),
    RP_GL(glRasterPos4dv, DESKTOP),
    RP_GL(glRasterPos4fv, DESKTOP),
    RP_GL(glRasterPos4iv, DESKTOP),
    RP_GL(glRasterPos4sv, DESKTOP),
    RP_GL(glRectd, DESKTOP),
    RP_GL(glRectf, DESKTOP),
    RP_GL(glRecti, DESKTOP),
    RP_GL(glRects, DESKTOP),
    RP_GL(glRectdv, DESKTOP),
    RP_GL(glRectfv, DESKTOP),
    RP_GL(glRectiv, DESKTOP),
    RP_GL(glRectsv, DESKTOP),
    RP_GL(glVertexPointer, DESKTOP),
    RP_GL(glNormalPointer, DESKTOP),
    RP_GL(glColorPointer, DESKTOP),
    RP_GL(glIndexPointer, DESKTOP),
    RP_GL(glTexCoordPointer, DESKTOP),
    RP_GL(glEdgeFlagPointer, DESKTOP),
    RP_GL(glArrayElement, DESKTOP),
    RP_GL(glInterleavedArrays, DESKTOP),
    RP_GL(glShadeModel, DESKTOP),
    RP_GL(glLightf, DESKTOP),
    RP_GL(glLighti, DESKTOP),
    RP_GL(glLightfv, DESKTOP),
    RP_GL(glLightiv, DESKTOP),
    RP_GL(glGetLightfv, DESKTOP),
    RP_GL(glGetLightiv, DESKTOP),
    RP_GL(glLightModelf, DESKTOP),
    RP_GL(glLightModeli, DESKTOP),
    RP_GL(glLightModelfv, DESKTOP),
    RP_GL(glLightModeliv, DESKTOP),
    RP_GL(glMaterialf, DESKTOP),
    RP_GL(glMateriali, DESKTOP),
    RP_GL(glMaterialfv, DESKTOP),
    RP_GL(glMaterialiv, DESKTOP),
    RP_GL(glGetMaterialfv, DESKTOP),
    RP_GL(glGetMaterialiv, DESKTOP),
    RP_GL(glColorMaterial, DESKTOP),
    RP_GL(glPixelZoom, DESKTOP),
    RP_GL(glPixelStoref, DESKTOP),
    RP_GL(glPixelTransferf, DESKTOP),
    RP_GL(glPixelTransferi, DESKTOP),
    RP_GL(glPixelMapfv, DESKTOP),
    RP_GL(glPixelMapuiv, DESKTOP),
    RP_GL(glPixelMapusv, DESKTOP),
    RP_GL(glGetPixelMapfv, DESKTOP),
    RP_GL(glGetPixelMapuiv, DESKTOP),
    RP_GL(glGetPixelMapusv, DESKTOP),
    RP_GL(glBitmap, DESKTOP),
    RP_GL(glDrawPixels, DESKTOP),
    RP_GL(glCopyPixels, DESKTOP),
    RP_GL(glTexGend, DESKTOP),
    RP_GL(glTexGenf, DESKTOP),
    RP_GL(glTexGeni, DESKTOP),
    RP_GL(glTexGendv, DESKTOP),
    RP_GL(glTexGenfv, DESKTOP),
    RP_GL(glTexGeniv, DESKTOP),
    RP_GL(glGetTexGendv, DESKTOP),
    RP_GL(glGetTexGenfv, DESKTOP),
    RP_GL(glGetTexGeniv, DESKTOP),
    RP_GL(glTexEnvf, DESKTOP),
    RP_GL(glTexEnvi, DESKTOP),
    RP_GL(glTexEnvfv, DESKTOP),
    RP_GL(glTexEnviv, DESKTOP),
    RP_GL(glGetTexEnvfv, DESKTOP),
    RP_GL(glGetTexEnviv, DESKTOP),
    RP_GL(glGetTexLevelParameterfv, DESKTOP),
    RP_GL(glGetTexLevelParameteriv, DESKTOP),
    RP_GL(glTexImage1D, DESKTOP),
    RP_GL(glGetTexImage, DESKTOP),
    RP_GL(glPrioritizeTextures, DESKTOP),
    RP_GL(glAreTexturesResident, DESKTOP),
    RP_GL(glTexSubImage1D, DESKTOP),
    RP_GL(glCopyTexImage1D, DESKTOP),
    RP_GL(glCopyTexSubImage1D, DESKTOP),
    RP_GL(glMap1d, DESKTOP),
    RP_GL(glMap1f, DESKTOP),
    RP_GL(glMap2d, DESKTOP),
    RP_GL(glMap2f, DESKTOP),
    RP_GL(glGetMapdv, DESKTOP),
    RP_GL(glGetMapfv, DESKTOP),
    RP_GL(glGetMapiv, DESKTOP),
    RP_GL(glEvalCoord1d, DESKTOP),
    RP_GL(glEvalCoord1f, DESKTOP),
    RP_GL(glEvalCoord1dv, DESKTOP),
    RP_GL(glEvalCoord1fv, DESKTOP),
    RP_GL(glEvalCoord2d, DESKTOP),
    RP_GL(glEvalCoord2f, DESKTOP),
    RP_GL(glEvalCoord2dv, DESKTOP),
    RP_GL(glEvalCoord2fv, DESKTOP),
    RP_GL(glMapGrid1d, DESKTOP),
    RP_GL(glMapGrid1f, DESKTOP),
    RP_GL(glMapGrid2d, DESKTOP),
    RP_GL(glMapGrid2f, DESKTOP),
    RP_GL(glEvalPoint1, DESKTOP),
    RP_GL(glEvalPoint2, DESKTOP),
    RP_GL(glEvalMesh1, DESKTOP),
    RP_GL(glEvalMesh2, DESKTOP),
    RP_GL(glFogf, DESKTOP),
    RP_GL(glFogi, DESKTOP),
    RP_GL(glFogfv, DESKTOP),
    RP_GL(glFogiv, DESKTOP),
    RP_GL(glFeedbackBuffer, DESKTOP),
    RP_GL(glPassThrough, DESKTOP),
    RP_GL(glSelectBuffer, DESKTOP),
    RP_GL(glInitNames, DESKTOP),
    RP_GL(glLoadName, DESKTOP),
    RP_GL(glPushName, DESKTOP),
    RP_GL(glPopName, DESKTOP),
    RP_GL(glTexImage2D, DESKTOP_OR_ES2),
    RP_GL(glClearColor, DESKTOP_OR_ES2),
    RP_GL(glClear, DESKTOP_OR_ES2),
    RP_GL(glColorMask, DESKTOP_OR_ES2),
    RP_GL(glBlendFunc, DESKTOP_OR_ES2),
    RP_GL(glCullFace, DESKTOP_OR_ES2),
    RP_GL(glFrontFace, DESKTOP_OR_ES2),
    RP_GL(glLineWidth, DESKTOP_OR_ES2),
    RP_GL(glPolygonOffset, DESKTOP_OR_ES2),
    RP_GL(glScissor, DESKTOP_OR_ES2),
    RP_GL(glEnable, DESKTOP_OR_ES2),
    RP_GL(glDisable, DESKTOP_OR_ES2),
    RP_GL(glIsEnabled, DESKTOP_OR_ES2),
    RP_GL(glGetBooleanv, DESKTOP_OR_ES2),
    RP_GL(glGetFloatv, DESKTOP_OR_ES2),
    RP_GL(glFinish, DESKTOP_OR_ES2),
    RP_GL(glFlush, DESKTOP_OR_ES2),
    RP_GL(glHint, DESKTOP_OR_ES2),
    RP_GL(glDepthFunc, DESKTOP_OR_ES2),
    RP_GL(glDepthMask, DESKTOP_OR_ES2),
    RP_GL(glViewport, DESKTOP_OR_ES2),
    RP_GL(glDrawArrays, DESKTOP_OR_ES2),
    RP_GL(glDrawElements, DESKTOP_OR_ES2),
    RP_GL(glPixelStorei, DESKTOP_OR_ES2),
    RP_GL(glReadPixels, DESKTOP_OR_ES2),
    RP_GL(glStencilFunc, DESKTOP_OR_ES2),
    RP_GL(glStencilMask, DESKTOP_OR_ES2),
    RP_GL(glStencilOp, DESKTOP_OR_ES2),
    RP_GL(glClearStencil, DESKTOP_OR_ES2),
    RP_GL(glTexParameterf, DESKTOP_OR_ES2),
    RP_GL(glTexParameteri, DESKTOP_OR_ES2),
    RP_GL(glTexParameterfv, DESKTOP_OR_ES2),
    RP_GL(glTexParameteriv, DESKTOP_OR_ES2),
    RP_GL(glGetTexParameterfv, DESKTOP_OR_ES2),
    RP_GL(glGetTexParameteriv, DESKTOP_OR_ES2),
    RP_GL(glGenTextures, DESKTOP_OR_ES2),
    RP_GL(glDeleteTextures, DESKTOP_OR_ES2),
    RP_GL(glBindTexture, DESKTOP_OR_ES2),
    RP_GL(glIsTexture, DESKTOP_OR_ES2),
    RP_GL(glTexSubImage2D, DESKTOP_OR_ES2),
    RP_GL(glCopyTexImage2D, DESKTOP_OR_ES2),
    RP_GL(glCopyTexSubImage2D, DESKTOP_OR_ES2),
    RP_GL(glReadBuffer, DESKTOP_OR_ES3),
    RP_GL(glGetPointerv, DESKTOP_OR_ES32),
    // gl_1_2.h
    RP_GL(glCopyTexSubImage3D, DESKTOP_OR_ES3),
    RP_GL(glDrawRangeElements, DESKTOP_OR_ES3),
    RP_GL(glTexImage3D, DESKTOP_OR_ES3),
    RP_GL(glTexSubImage3D, DESKTOP_OR_ES3),
    // gl_1_3.h
    RP_GL(glClientActiveTexture, DESKTOP),
    RP_GL(glCompressedTexImage1D, DESKTOP),
    RP_GL(glCompressedTexSubImage1D, DESKTOP),
    RP_GL(glGetCompressedTexImage, DESKTOP),
    RP_GL(glLoadTransposeMatrixd, DESKTOP),
    RP_GL(glLoadTransposeMatrixf, DESKTOP),
    RP_GL(glMultTransposeMatrixd, DESKTOP),
    RP_GL(glMultTransposeMatrixf, DESKTOP),
    RP_GL(glMultiTexCoord1d, DESKTOP),
    RP_GL(glMultiTexCoord1dv, DESKTOP),
    RP_GL(glMultiTexCoord1f, DESKTOP),
    RP_GL(glMultiTexCoord1fv, DESKTOP),
    RP_GL(glMultiTexCoord1i, DESKTOP),
    RP_GL(glMultiTexCoord1iv, DESKTOP),
    RP_GL(glMultiTexCoord1s, DESKTOP),
    RP_GL(glMultiTexCoord1sv, DESKTOP),
    RP_GL(glMultiTexCoord2d, DESKTOP),
    RP_GL(glMultiTexCoord2dv, DESKTOP),
    RP_GL(glMultiTexCoord2f, DESKTOP),
    RP_GL(glMultiTexCoord2fv, DESKTOP),
    RP_GL(glMultiTexCoord2i, DESKTOP),
    RP_GL(glMultiTexCoord2iv, DESKTOP),
    RP_GL(glMultiTexCoord2s, DESKTOP),
    RP_GL(glMultiTexCoord2sv, DESKTOP),
    RP_GL(glMultiTexCoord3d, DESKTOP),
    RP_GL(glMultiTexCoord3dv, DESKTOP),
    RP_GL(glMultiTexCoord3f, DESKTOP),
    RP_GL(glMultiTexCoord3fv, DESKTOP),
    RP_GL(glMultiTexCoord3i, DESKTOP),
    RP_GL(glMultiTexCoord3iv, DESKTOP),
    RP_GL(glMultiTexCoord3s, DESKTOP),
    RP_GL(glMultiTexCoord3sv, DESKTOP),
    RP_GL(glMultiTexCoord4d, DESKTOP),
    RP_GL(glMultiTexCoord4dv, DESKTOP),
    RP_GL(glMultiTexCoord4f, DESKTOP),
    RP_GL(glMultiTexCoord4fv, DESKTOP),
    RP_GL(glMultiTexCoord4i, DESKTOP),
    RP_GL(glMultiTexCoord4iv, DESKTOP),
    RP_GL(glMultiTexCoord4s, DESKTOP),
    RP_GL(glMultiTexCoord4sv, DESKTOP),
    RP_GL(glSampleCoverage, DESKTOP_OR_ES2),
    RP_GL(glActiveTexture, DESKTOP_OR_ES2),
    RP_GL(glCompressedTexImage2D, DESKTOP_OR_ES2),
    RP_GL(glCompressedTexSubImage2D, DESKTOP_OR_ES2),
    RP_GL(glCompressedTexImage3D, DESKTOP_OR_ES3),
    RP_GL(glCompressedTexSubImage3D, DESKTOP_OR_ES3),
    // gl_1_4.h
    RP_GL(glFogCoordPointer, DESKTOP),
    RP_GL(glFogCoordd, DESKTOP),
    RP_GL(glFogCoorddv, DESKTOP),
    RP_GL(glFogCoordf, DESKTOP),
    RP_GL(glFogCoordfv, DESKTOP),
    RP_GL(glMultiDrawArrays, DESKTOP),
    RP_GL(glMultiDrawElements, DESKTOP),
    RP_GL(glPointParameterf, DESKTOP),
    RP_GL(glPointParameterfv, DESKTOP),
    RP_GL(glPointParameteri, DESKTOP),
    RP_GL(glPointParameteriv, DESKTOP),
    RP_GL(glSecondaryColor3b, DESKTOP),
    RP_GL(glSecondaryColor3bv, DESKTOP),
    RP_GL(glSecondaryColor3d, DESKTOP),
    RP_GL(glSecondaryColor3dv, DESKTOP),
    RP_GL(glSecondaryColor3f, DESKTOP),
    RP_GL(glSecondaryColor3fv, DESKTOP),
    RP_GL(glSecondaryColor3i, DESKTOP),
    RP_GL(glSecondaryColor3iv, DESKTOP),
    RP_GL(glSecondaryColor3s, DESKTOP),
    RP_GL(glSecondaryColor3sv, DESKTOP),
    RP_GL(glSecondaryColor3ub, DESKTOP),
    RP_GL(glSecondaryColor3ubv, DESKTOP),
    RP_GL(glSecondaryColor3ui, DESKTOP),
    RP_GL(glSecondaryColor3uiv, DESKTOP),
    RP_GL(glSecondaryColor3us, DESKTOP),
    RP_GL(glSecondaryColor3usv, DESKTOP),
    RP_GL(glSecondaryColorPointer, DESKTOP),
    RP_GL(glWindowPos2d, DESKTOP),
    RP_GL(glWindowPos2dv, DESKTOP),
    RP_GL(glWindowPos2f, DESKTOP),
    RP_GL(glWindowPos2fv, DESKTOP),
    RP_GL(glWindowPos2i, DESKTOP),
    RP_GL(glWindowPos2iv, DESKTOP),
    RP_GL(glWindowPos2s, DESKTOP),
    RP_GL(glWindowPos2sv, DESKTOP),
    RP_GL(glWindowPos3d, DESKTOP),
    RP_GL(glWindowPos3dv, DESKTOP),
    RP_GL(glWindowPos3f, DESKTOP),
    RP_GL(glWindowPos3fv, DESKTOP),
    RP_GL(glWindowPos3i, DESKTOP),
    RP_GL(glWindowPos3iv, DESKTOP),
    RP_GL(glWindowPos3s, DESKTOP),
    RP_GL(glWindowPos3sv, DESKTOP),
    RP_GL(glBlendColor, DESKTOP_OR_ES2),
    RP_GL(glBlendEquation, DESKTOP_OR_ES2),
    RP_GL(glBlendFuncSeparate, DESKTOP_OR_ES2),
    // gl_1_5.h
    RP_GL(glGetBufferSubData, DESKTOP),
    RP_GL(glGetQueryObjectiv, DESKTOP),
    RP_GL(glMapBuffer, DESKTOP),
    RP_GL(glBindBuffer, DESKTOP_OR_ES2),
    RP_GL(glBufferData, DESKTOP_OR_ES2),
    RP_GL(glBufferSubData, DESKTOP_OR_ES2),
    RP_GL(glDeleteBuffers, DESKTOP_OR_ES2),
    RP_GL(glGenBuffers, DESKTOP_OR_ES2),
    RP_GL(glGetBufferParameteriv, DESKTOP_OR_ES2),
    RP_GL(glIsBuffer, DESKTOP_OR_ES2),
    RP_GL(glBeginQuery, DESKTOP_OR_ES3),
    RP_GL(glDeleteQueries, DESKTOP_OR_ES3),
    RP_GL(glEndQuery, DESKTOP_OR_ES3),
    RP_GL(glGenQueries, DESKTOP_OR_ES3),
    RP_GL(glIsQuery, DESKTOP_OR_ES3),
    RP_GL(glGetQueryiv, DESKTOP_OR_ES3),
    RP_GL(glGetQueryObjectuiv, DESKTOP_OR_ES3),
    RP_GL(glGetBufferPointerv, DESKTOP_OR_ES3),
    RP_GL(glUnmapBuffer, DESKTOP_OR_ES3),
    // gl_2_0.h
    RP_GL(glGetVertexAttribdv, DESKTOP),
    RP_GL(glVertexAttrib1d, DESKTOP),
    RP_GL(glVertexAttrib1dv, DESKTOP),
    RP_GL(glVertexAttrib1s, DESKTOP),
    RP_GL(glVertexAttrib1sv, DESKTOP),
    RP_GL(glVertexAttrib2d, DESKTOP),
    RP_GL(glVertexAttrib2dv, DESKTOP),
    RP_GL(glVertexAttrib2s, DESKTOP),
    RP_GL(glVertexAttrib2sv, DESKTOP),
    RP_GL(glVertexAttrib3d, DESKTOP),
    RP_GL(glVertexAttrib3dv, DESKTOP),
    RP_GL(glVertexAttrib3s, DESKTOP),
    RP_GL(glVertexAttrib3sv, DESKTOP),
    RP_GL(glVertexAttrib4Nbv, DESKTOP),
    RP_GL(glVertexAttrib4Niv, DESKTOP),
    RP_GL(glVertexAttrib4Nsv, DESKTOP),
    RP_GL(glVertexAttrib4Nub, DESKTOP),
    RP_GL(glVertexAttrib4Nubv, DESKTOP),
    RP_GL(glVertexAttrib4Nuiv, DESKTOP),
    RP_GL(glVertexAttrib4Nusv, DESKTOP),
    RP_GL(glVertexAttrib4bv, DESKTOP),
    RP_GL(glVertexAttrib4d, DESKTOP),
    RP_GL(glVertexAttrib4dv, DESKTOP),
    RP_GL(glVertexAttrib4iv, DESKTOP),
    RP_GL(glVertexAttrib4s, DESKTOP),
    RP_GL(glVertexAttrib4sv, DESKTOP),
    RP_GL(glVertexAttrib4ubv, DESKTOP),
    RP_GL(glVertexAttrib4uiv, DESKTOP),
    RP_GL(glVertexAttrib4usv, DESKTOP),
    RP_GL(glAttachShader, DESKTOP_OR_ES2),
    RP_GL(glBindAttribLocation, DESKTOP_OR_ES2),
    RP_GL(glBlendEquationSeparate, DESKTOP_OR_ES2),
    RP_GL(glCompileShader, DESKTOP_OR_ES2),
    RP_GL(glCreateProgram, DESKTOP_OR_ES2),
    RP_GL(glCreateShader, DESKTOP_OR_ES2),
    RP_GL(glDeleteProgram, DESKTOP_OR_ES2),
    RP_GL(glDeleteShader, DESKTOP_OR_ES2),
    RP_GL(glDetachShader, DESKTOP_OR_ES2),
    RP_GL(glDisableVertexAttribArray, DESKTOP_OR_ES2),
    RP_GL(glEnableVertexAttribArray, DESKTOP_OR_ES2),
    RP_GL(glGetActiveAttrib, DESKTOP_OR_ES2),
    RP_GL(glGetActiveUniform, DESKTOP_OR_ES2),
    RP_GL(glGetAttachedShaders, DESKTOP_OR_ES2),
    RP_GL(glGetAttribLocation, DESKTOP_OR_ES2),
    RP_GL(glGetProgramInfoLog, DESKTOP_OR_ES2),
    RP_GL(glGetProgramiv, DESKTOP_OR_ES2),
    RP_GL(glGetShaderInfoLog, DESKTOP_OR_ES2),
    RP_GL(glGetShaderSource, DESKTOP_OR_ES2),
    RP_GL(glGetShaderiv, DESKTOP_OR_ES2),
    RP_GL(glGetUniformLocation, DESKTOP_OR_ES2),
    RP_GL(glGetUniformfv, DESKTOP_OR_ES2),
    RP_GL(glGetUniformiv, DESKTOP_OR_ES2),
    RP_GL(glGetVertexAttribPointerv, DESKTOP_OR_ES2),
    RP_GL(glGetVertexAttribfv, DESKTOP_OR_ES2),
    RP_GL(glGetVertexAttribiv, DESKTOP_OR_ES2),
    RP_GL(glIsProgram, DESKTOP_OR_ES2),
    RP_GL(glIsShader, DESKTOP_OR_ES2),
    RP_GL(glLinkProgram, DESKTOP_OR_ES2),
    RP_GL(glShaderSource, DESKTOP_OR_ES2),
    RP_GL(glStencilFuncSeparate, DESKTOP_OR_ES2),
    RP_GL(glStencilMaskSeparate, DESKTOP_OR_ES2),
    RP_GL(glStencilOpSeparate, DESKTOP_OR_ES2),
    RP_GL(glUniform1f, DESKTOP_OR_ES2),
    RP_GL(glUniform1fv, DESKTOP_OR_ES2),
    RP_GL(glUniform1i, DESKTOP_OR_ES2),
    RP_GL(glUniform1iv, DESKTOP_OR_ES2),
    RP_GL(glUniform2f, DESKTOP_OR_ES2),
    RP_GL(glUniform2fv, DESKTOP_OR_ES2),
    RP_GL(glUniform2i, DESKTOP_OR_ES2),
    RP_GL(glUniform2iv, DESKTOP_OR_ES2),
    RP_GL(glUniform3f, DESKTOP_OR_ES2),
    RP_GL(glUniform3fv, DESKTOP_OR_ES2),
    RP_GL(glUniform3i, DESKTOP_OR_ES2),
    RP_GL(glUniform3iv, DESKTOP_OR_ES2),
    RP_GL(glUniform4f, DESKTOP_OR_ES2),
    RP_GL(glUniform4fv, DESKTOP_OR_ES2),
    RP_GL(glUniform4i, DESKTOP_OR_ES2),
    RP_GL(glUniform4iv, DESKTOP_OR_ES2),
    RP_GL(glUniformMatrix2fv, DESKTOP_OR_ES2),
    RP_GL(glUniformMatrix3fv, DESKTOP_OR_ES2),
    RP_GL(glUniformMatrix4fv, DESKTOP_OR_ES2),
    RP_GL(glUseProgram, DESKTOP_OR_ES2),
    RP_GL(glValidateProgram, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib1f, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib1fv, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib2f, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib2fv, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib3f, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib3fv, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib4f, DESKTOP_OR_ES2),
    RP_GL(glVertexAttrib4fv, DESKTOP_OR_ES2),
    RP_GL(glVertexAttribPointer, DESKTOP_OR_ES2),
    RP_GL(glDrawBuffers, DESKTOP_OR_ES3),
    // gl_2_1.h
    RP_GL(glUniformMatrix2x3fv, DESKTOP),
    RP_GL(glUniformMatrix2x4fv, DESKTOP),
    RP_GL(glUniformMatrix3x2fv, DESKTOP),
    RP_GL(glUniformMatrix3x4fv, DESKTOP),
    RP_GL(glUniformMatrix4x2fv, DESKTOP),
    RP_GL(glUniformMatrix4x3fv, DESKTOP),
    // gl_3_0.h
    RP_GL(glBeginConditionalRender, "VERSION_3_0"),
    RP_GL(glBindFragDataLocation, "VERSION_3_0"),
    RP_GL(glClampColor, "VERSION_3_0"),
    RP_GL(glEndConditionalRender, "VERSION_3_0"),
    RP_GL(glGetBooleani_v, "VERSION_3_0"),
    RP_GL(glVertexAttribI1i, "VERSION_3_0"),
    RP_GL(glVertexAttribI1iv, "VERSION_3_0"),
    RP_GL(glVertexAttribI1ui, "VERSION_3_0"),
    RP_GL(glVertexAttribI1uiv, "VERSION_3_0"),
    RP_GL(glVertexAttribI2i, "VERSION_3_0"),
    RP_GL(glVertexAttribI2iv, "VERSION_3_0"),
    RP_GL(glVertexAttribI2ui, "VERSION_3_0"),
    RP_GL(glVertexAttribI2uiv, "VERSION_3_0"),
    RP_GL(glVertexAttribI3i, "VERSION_3_0"),
    RP_GL(glVertexAttribI3iv, "VERSION_3_0"),
    RP_GL(glVertexAttribI3ui, "VERSION_3_0"),
    RP_GL(glVertexAttribI3uiv, "VERSION_3_0"),
    RP_GL(glVertexAttribI4bv, "VERSION_3_0"),
    RP_GL(glVertexAttribI4sv, "VERSION_3_0"),
    RP_GL(glVertexAttribI4ubv, "VERSION_3_0"),
    RP_GL(glVertexAttribI4usv, "VERSION_3_0"),
    RP_GL(glBeginTransformFeedback, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glClearBufferfi, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glClearBufferfv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glClearBufferiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glClearBufferuiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glEndTransformFeedback, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glGetFragDataLocation, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glGetStringi, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glGetTransformFeedbackVarying, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glGetUniformuiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glGetVertexAttribIiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glGetVertexAttribIuiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glTransformFeedbackVaryings, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform1ui, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform1uiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform2ui, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform2uiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform3ui, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform3uiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform4ui, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glUniform4uiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glVertexAttribI4i, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glVertexAttribI4iv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glVertexAttribI4ui, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glVertexAttribI4uiv, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glVertexAttribIPointer, "VERSION_3_0 |VERSION_GLES_3"),
    RP_GL(glColorMaski, "VERSION_3_0 |VERSION_GLES_3_2"),
    RP_GL(glDisablei, "VERSION_3_0 |VERSION_GLES_3_2"),
    RP_GL(glEnablei, "VERSION_3_0 |VERSION_GLES_3_2"),
    RP_GL(glGetTexParameterIiv, "VERSION_3_0 |VERSION_GLES_3_2"),
    RP_GL(glGetTexParameterIuiv, "VERSION_3_0 |VERSION_GLES_3_2"),
    RP_GL(glIsEnabledi, "VERSION_3_0 |VERSION_GLES_3_2"),
    RP_GL(glTexParameterIiv, "VERSION_3_0 |VERSION_GLES_3_2"),
    RP_GL(glTexParameterIuiv, "VERSION_3_0 |VERSION_GLES_3_2"),
    // gl_3_1.h
    RP_GL(glPrimitiveRestartIndex, "VERSION_3_1"),
    RP_GL(glDrawArraysInstanced, "VERSION_3_1 |VERSION_GLES_3"),
    RP_GL(glDrawElementsInstanced, "VERSION_3_1 |VERSION_GLES_3"),
    RP_GL(glTexBuffer, "VERSION_3_1 |VERSION_GLES_3_2"),
    // gl_3_2.h
    RP_GL(glGetBufferParameteri64v, "VERSION_3_2 |VERSION_GLES_3"),
    RP_GL(glGetInteger64i_v, "VERSION_3_2 |VERSION_GLES_3"),
    RP_GL(glFramebufferTexture, "VERSION_3_2 |VERSION_GLES_3_2"),
    // gl_4_2.h
    RP_GL(glDrawArraysInstancedBaseInstance, "VERSION_4_2"),
    RP_GL(glDrawElementsInstancedBaseInstance, "VERSION_4_2"),
    RP_GL(glDrawElementsInstancedBaseVertexBaseInstance, "VERSION_4_2"),
    RP_GL(glGetInternalformativ, "VERSION_4_2 |VERSION_GLES_3"),
    RP_GL(glGetActiveAtomicCounterBufferiv, "VERSION_4_2"),
    // ARB_shader_image_load_store.h
    RP_GL(glBindImageTexture, "VERSION_4_2"),
    RP_GL(glMemoryBarrier, "VERSION_4_2"),
    // ARB_texture_storage.h
    RP_GL(glTexStorage1D, "VERSION_4_2"),
    RP_GL(glTexStorage2D, "VERSION_4_2"),
    RP_GL(glTexStorage3D, "VERSION_4_2"),
    // gl_4_2.h
    RP_GL(glDrawTransformFeedbackInstanced, "VERSION_4_2"),
    RP_GL(glDrawTransformFeedbackStreamInstanced, "VERSION_4_2"),
    // gl_4_3.h
    RP_GL(glClearBufferData, "VERSION_4_3"),
    RP_GL(glClearBufferSubData, "VERSION_4_3"),
    // ARB_compute_shader.h
    RP_GL(glDispatchCompute, "VERSION_4_3"),
    RP_GL(glDispatchComputeIndirect, "VERSION_4_3"),
    // ARB_copy_image.h
    RP_GL(glCopyImageSubData, "VERSION_4_3"),
    // gl_4_3.h
    RP_GL(glFramebufferParameteri, "VERSION_4_3"),
    RP_GL(glGetFramebufferParameteriv, "VERSION_4_3"),
    RP_GL(glGetInternalformati64v, "VERSION_4_3"),
    RP_GL(glInvalidateTexSubImage, "VERSION_4_3"),
    RP_GL(glInvalidateTexImage, "VERSION_4_3"),
    RP_GL(glInvalidateBufferSubData, "VERSION_4_3"),
    RP_GL(glInvalidateBufferData, "VERSION_4_3"),
    RP_GL(glInvalidateFramebuffer, "VERSION_4_3"),
    RP_GL(glInvalidateSubFramebuffer, "VERSION_4_3"),
    RP_GL(glMultiDrawArraysIndirect, "VERSION_4_3"),
    RP_GL(glMultiDrawElementsIndirect, "VERSION_4_3"),
    RP_GL(glGetProgramInterfaceiv, "VERSION_4_3"),
    RP_GL(glGetProgramResourceIndex, "VERSION_4_3"),
    RP_GL(glGetProgramResourceName, "VERSION_4_3"),
    RP_GL(glGetProgramResourceiv, "VERSION_4_3"),
    RP_GL(glGetProgramResourceLocation, "VERSION_4_3"),
    RP_GL(glGetProgramResourceLocationIndex, "VERSION_4_3"),
    // ARB_shader_storage_buffer_object.h
    RP_GL(glShaderStorageBlockBinding, "VERSION_4_3"),
    // gl_4_3.h
    RP_GL(glTexBufferRange, "VERSION_4_3"),
    // ARB_texture_storage_multisample.h
    RP_GL(glTexStorage2DMultisample, "VERSION_4_3"),
    RP_GL(glTexStorage3DMultisample, "VERSION_4_3"),
    // gl_4_3.h
    RP_GL(glTextureView, "VERSION_4_3"),
    RP_GL(glBindVertexBuffer, "VERSION_4_3"),
    RP_GL(glVertexAttribFormat, "VERSION_4_3"),
    RP_GL(glVertexAttribIFormat, "VERSION_4_3"),
    RP_GL(glVertexAttribLFormat, "VERSION_4_3"),
    RP_GL(glVertexAttribBinding, "VERSION_4_3"),
    RP_GL(glVertexBindingDivisor, "VERSION_4_3"),
    // KHR_debug.h
    RP_GL(glDebugMessageControl, "VERSION_4_3"),
    RP_GL(glDebugMessageInsert, "VERSION_4_3"),
    RP_GL(glDebugMessageCallback, "VERSION_4_3"),
    RP_GL(glGetDebugMessageLog, "VERSION_4_3"),
    RP_GL(glPushDebugGroup, "VERSION_4_3"),
    RP_GL(glPopDebugGroup, "VERSION_4_3"),
    RP_GL(glObjectLabel, "VERSION_4_3"),
    RP_GL(glGetObjectLabel, "VERSION_4_3"),
    RP_GL(glObjectPtrLabel, "VERSION_4_3"),
    RP_GL(glGetObjectPtrLabel, "VERSION_4_3"),
    // ARB_buffer_storage.h
    RP_GL(glBufferStorage, "VERSION_4_4"),
    // gl_4_4.h
    RP_GL(glClearTexImage, "VERSION_4_4"),
    RP_GL(glClearTexSubImage, "VERSION_4_4"),
    RP_GL(glBindBuffersBase, "VERSION_4_4"),
    RP_GL(glBindBuffersRange, "VERSION_4_4"),
    RP_GL(glBindTextures, "VERSION_4_4"),
    RP_GL(glBindSamplers, "VERSION_4_4"),
    RP_GL(glBindImageTextures, "VERSION_4_4"),
    RP_GL(glBindVertexBuffers, "VERSION_4_4"),
    // ARB_clip_control.h
    RP_GL(glClipControl, "VERSION_4_5"),
    // gl_4_5.h
    RP_GL(glCreateTransformFeedbacks, "VERSION_4_5"),
    RP_GL(glTransformFeedbackBufferBase, "VERSION_4_5"),
    RP_GL(glTransformFeedbackBufferRange, "VERSION_4_5"),
    RP_GL(glGetTransformFeedbackiv, "VERSION_4_5"),
    RP_GL(glGetTransformFeedbacki_v, "VERSION_4_5"),
    RP_GL(glGetTransformFeedbacki64_v, "VERSION_4_5"),
    RP_GL(glCreateBuffers, "VERSION_4_5"),
    RP_GL(glNamedBufferStorage, "VERSION_4_5"),
    RP_GL(glNamedBufferData, "VERSION_4_5"),
    RP_GL(glNamedBufferSubData, "VERSION_4_5"),
    RP_GL(glCopyNamedBufferSubData, "VERSION_4_5"),
    RP_GL(glClearNamedBufferData, "VERSION_4_5"),
    RP_GL(glClearNamedBufferSubData, "VERSION_4_5"),
    RP_GL(glMapNamedBuffer, "VERSION_4_5"),
    RP_GL(glMapNamedBufferRange, "VERSION_4_5"),
    RP_GL(glUnmapNamedBuffer, "VERSION_4_5"),
    RP_GL(glFlushMappedNamedBufferRange, "VERSION_4_5"),
    RP_GL(glGetNamedBufferParameteriv, "VERSION_4_5"),
    RP_GL(glGetNamedBufferParameteri64v, "VERSION_4_5"),
    RP_GL(glGetNamedBufferPointerv, "VERSION_4_5"),
    RP_GL(glGetNamedBufferSubData, "VERSION_4_5"),
    RP_GL(glCreateFramebuffers, "VERSION_4_5"),
    RP_GL(glNamedFramebufferRenderbuffer, "VERSION_4_5"),
    RP_GL(glNamedFramebufferParameteri, "VERSION_4_5"),
    RP_GL(glNamedFramebufferTexture, "VERSION_4_5"),
    RP_GL(glNamedFramebufferTextureLayer, "VERSION_4_5"),
    RP_GL(glNamedFramebufferDrawBuffer, "VERSION_4_5"),
    RP_GL(glNamedFramebufferDrawBuffers, "VERSION_4_5"),
    RP_GL(glNamedFramebufferReadBuffer, "VERSION_4_5"),
    RP_GL(glInvalidateNamedFramebufferData, "VERSION_4_5"),
    RP_GL(glInvalidateNamedFramebufferSubData, "VERSION_4_5"),
    RP_GL(glClearNamedFramebufferiv, "VERSION_4_5"),
    RP_GL(glClearNamedFramebufferuiv, "VERSION_4_5"),
    RP_GL(glClearNamedFramebufferfv, "VERSION_4_5"),
    RP_GL(glClearNamedFramebufferfi, "VERSION_4_5"),
    RP_GL(glBlitNamedFramebuffer, "VERSION_4_5"),
    RP_GL(glCheckNamedFramebufferStatus, "VERSION_4_5"),
    RP_GL(glGetNamedFramebufferParameteriv, "VERSION_4_5"),
    RP_GL(glGetNamedFramebufferAttachmentParameteriv, "VERSION_4_5"),
    RP_GL(glCreateRenderbuffers, "VERSION_4_5"),
    RP_GL(glNamedRenderbufferStorage, "VERSION_4_5"),
    RP_GL(glNamedRenderbufferStorageMultisample, "VERSION_4_5"),
    RP_GL(glGetNamedRenderbufferParameteriv, "VERSION_4_5"),
    RP_GL(glCreateTextures, "VERSION_4_5"),
    RP_GL(glTextureBuffer, "VERSION_4_5"),
    RP_GL(glTextureBufferRange, "VERSION_4_5"),
    RP_GL(glTextureStorage1D, "VERSION_4_5"),
    RP_GL(glTextureStorage2D, "VERSION_4_5"),
    RP_GL(glTextureStorage3D, "VERSION_4_5"),
    RP_GL(glTextureStorage2DMultisample, "VERSION_4_5"),
    RP_GL(glTextureStorage3DMultisample, "VERSION_4_5"),
    RP_GL(glTextureSubImage1D, "VERSION_4_5"),
    RP_GL(glTextureSubImage2D, "VERSION_4_5"),
    RP_GL(glTextureSubImage3D, "VERSION_4_5"),
    RP_GL(glCompressedTextureSubImage1D, "VERSION_4_5"),
    RP_GL(glCompressedTextureSubImage2D, "VERSION_4_5"),
    RP_GL(glCompressedTextureSubImage3D, "VERSION_4_5"),
    RP_GL(glCopyTextureSubImage1D, "VERSION_4_5"),
    RP_GL(glCopyTextureSubImage2D, "VERSION_4_5"),
    RP_GL(glCopyTextureSubImage3D, "VERSION_4_5"),
    RP_GL(glTextureParameterf, "VERSION_4_5"),
    RP_GL(glTextureParameterfv, "VERSION_4_5"),
    RP_GL(glTextureParameteri, "VERSION_4_5"),
    RP_GL(glTextureParameterIiv, "VERSION_4_5"),
    RP_GL(glTextureParameterIuiv, "VERSION_4_5"),
    RP_GL(glTextureParameteriv, "VERSION_4_5"),
    RP_GL(glGenerateTextureMipmap, "VERSION_4_5"),
    RP_GL(glBindTextureUnit, "VERSION_4_5"),
    RP_GL(glGetTextureImage, "VERSION_4_5"),
    RP_GL(glGetCompressedTextureImage, "VERSION_4_5"),
    RP_GL(glGetTextureLevelParameterfv, "VERSION_4_5"),
    RP_GL(glGetTextureLevelParameteriv, "VERSION_4_5"),
    RP_GL(glGetTextureParameterfv, "VERSION_4_5"),
    RP_GL(glGetTextureParameterIiv, "VERSION_4_5"),
    RP_GL(glGetTextureParameterIuiv, "VERSION_4_5"),
    RP_GL(glGetTextureParameteriv, "VERSION_4_5"),
    RP_GL(glCreateVertexArrays, "VERSION_4_5"),
    RP_GL(glDisableVertexArrayAttrib, "VERSION_4_5"),
    RP_GL(glEnableVertexArrayAttrib, "VERSION_4_5"),
    RP_GL(glVertexArrayElementBuffer, "VERSION_4_5"),
    RP_GL(glVertexArrayVertexBuffer, "VERSION_4_5"),
    RP_GL(glVertexArrayVertexBuffers, "VERSION_4_5"),
    RP_GL(glVertexArrayAttribBinding, "VERSION_4_5"),
    RP_GL(glVertexArrayAttribFormat, "VERSION_4_5"),
    RP_GL(glVertexArrayAttribIFormat, "VERSION_4_5"),
    RP_GL(glVertexArrayAttribLFormat, "VERSION_4_5"),
    RP_GL(glVertexArrayBindingDivisor, "VERSION_4_5"),
    RP_GL(glGetVertexArrayiv, "VERSION_4_5"),
    RP_GL(glGetVertexArrayIndexediv, "VERSION_4_5"),
    RP_GL(glGetVertexArrayIndexed64iv, "VERSION_4_5"),
    RP_GL(glCreateSamplers, "VERSION_4_5"),
    RP_GL(glCreateProgramPipelines, "VERSION_4_5"),
    RP_GL(glCreateQueries, "VERSION_4_5"),
    RP_GL(glGetQueryBufferObjecti64v, "VERSION_4_5"),
    RP_GL(glGetQueryBufferObjectiv, "VERSION_4_5"),
    RP_GL(glGetQueryBufferObjectui64v, "VERSION_4_5"),
    RP_GL(glGetQueryBufferObjectuiv, "VERSION_4_5"),
    RP_GL(glMemoryBarrierByRegion, "VERSION_4_5"),
    RP_GL(glGetTextureSubImage, "VERSION_4_5"),
    RP_GL(glGetCompressedTextureSubImage, "VERSION_4_5"),
    RP_GL(glGetGraphicsResetStatus, "VERSION_4_5"),
    RP_GL(glReadnPixels, "VERSION_4_5"),
    RP_GL(glTextureBarrier, "VERSION_4_5"),
    // The robust glGetn* queries are left out on purpose: some AMD drivers do not export them,
    // and loading them would make a 4.5 context fail to initialise.
    // ARB_uniform_buffer_object.h
    RP_GL(glGetActiveUniformName, "GL_ARB_uniform_buffer_object"),
    RP_GL(glBindBufferBase, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glBindBufferRange, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glGetActiveUniformBlockName, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glGetActiveUniformBlockiv, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glGetActiveUniformsiv, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glGetIntegeri_v, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glGetUniformBlockIndex, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glGetUniformIndices, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    RP_GL(glUniformBlockBinding, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    // ARB_sampler_objects.h
    RP_GL(glBindSampler, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glDeleteSamplers, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glGenSamplers, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glGetSamplerParameterfv, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glGetSamplerParameteriv, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glIsSampler, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glSamplerParameterf, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glSamplerParameterfv, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glSamplerParameteri, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glSamplerParameteriv, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    RP_GL(glGetSamplerParameterIiv, "GL_ARB_sampler_objects |VERSION_GLES_3_2"),
    RP_GL(glGetSamplerParameterIuiv, "GL_ARB_sampler_objects |VERSION_GLES_3_2"),
    RP_GL(glSamplerParameterIiv, "GL_ARB_sampler_objects |VERSION_GLES_3_2"),
    RP_GL(glSamplerParameterIuiv, "GL_ARB_sampler_objects |VERSION_GLES_3_2"),
    // ARB_map_buffer_range.h
    RP_GL(glFlushMappedBufferRange, "GL_ARB_map_buffer_range |VERSION_GLES_3"),
    RP_GL(glMapBufferRange, "GL_ARB_map_buffer_range |VERSION_GLES_3"),
    // ARB_vertex_array_object.h
    RP_GL(glBindVertexArray, "GL_ARB_vertex_array_object |VERSION_GLES_3"),
    RP_GL(glDeleteVertexArrays, "GL_ARB_vertex_array_object |VERSION_GLES_3"),
    RP_GL(glGenVertexArrays, "GL_ARB_vertex_array_object |VERSION_GLES_3"),
    RP_GL(glIsVertexArray, "GL_ARB_vertex_array_object |VERSION_GLES_3"),
    RP_GL_AS(glBindVertexArray, "APPLE", "GL_APPLE_vertex_array_object !GL_ARB_vertex_array_object"),
    RP_GL_AS(glDeleteVertexArrays, "APPLE", "GL_APPLE_vertex_array_object !GL_ARB_vertex_array_object"),
    RP_GL_AS(glGenVertexArrays, "APPLE", "GL_APPLE_vertex_array_object !GL_ARB_vertex_array_object"),
    RP_GL_AS(glIsVertexArray, "APPLE", "GL_APPLE_vertex_array_object !GL_ARB_vertex_array_object"),
    // ARB_framebuffer_object.h
    RP_GL(glFramebufferTexture1D, "GL_ARB_framebuffer_object"),
    RP_GL(glFramebufferTexture3D, "GL_ARB_framebuffer_object"),
    RP_GL(glBindFramebuffer, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glBindRenderbuffer, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glBlitFramebuffer, "GL_ARB_framebuffer_object |VERSION_GLES_3"),
    RP_GL(glCheckFramebufferStatus, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glDeleteFramebuffers, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glDeleteRenderbuffers, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glFramebufferRenderbuffer, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glFramebufferTexture2D, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glFramebufferTextureLayer, "GL_ARB_framebuffer_object |VERSION_GLES_3"),
    RP_GL(glGenFramebuffers, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glGenRenderbuffers, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glGenerateMipmap, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glGetFramebufferAttachmentParameteriv, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glGetRenderbufferParameteriv, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glIsFramebuffer, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glIsRenderbuffer, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glRenderbufferStorage, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    RP_GL(glRenderbufferStorageMultisample, "GL_ARB_framebuffer_object |VERSION_GLES_3"),
    // ARB_get_program_binary.h
    RP_GL(glGetProgramBinary, "GL_ARB_get_program_binary |VERSION_GLES_3"),
    RP_GL(glProgramBinary, "GL_ARB_get_program_binary |VERSION_GLES_3"),
    RP_GL(glProgramParameteri, "GL_ARB_get_program_binary |VERSION_GLES_3"),
    // ARB_sync.h
    RP_GL(glClientWaitSync, "GL_ARB_sync |VERSION_GLES_3"),
    RP_GL(glDeleteSync, "GL_ARB_sync |VERSION_GLES_3"),
    RP_GL(glFenceSync, "GL_ARB_sync |VERSION_GLES_3"),
    RP_GL(glGetInteger64v, "GL_ARB_sync |VERSION_GLES_3"),
    RP_GL(glGetSynciv, "GL_ARB_sync |VERSION_GLES_3"),
    RP_GL(glIsSync, "GL_ARB_sync |VERSION_GLES_3"),
    RP_GL(glWaitSync, "GL_ARB_sync |VERSION_GLES_3"),
    // ARB_texture_multisample.h
    RP_GL(glTexImage2DMultisample, "GL_ARB_texture_multisample"),
    RP_GL(glTexImage3DMultisample, "GL_ARB_texture_multisample"),
    RP_GL(glGetMultisamplefv, "GL_ARB_texture_multisample"),
    RP_GL(glSampleMaski, "GL_ARB_texture_multisample"),
    // ARB_texture_storage.h
    RP_GL(glTexStorage1D, "GL_ARB_texture_storage !VERSION_4_2"),
    RP_GL(glTexStorage2D, "GL_ARB_texture_storage !VERSION_4_2 |VERSION_GLES_3"),
    RP_GL(glTexStorage3D, "GL_ARB_texture_storage !VERSION_4_2 |VERSION_GLES_3"),
    // ARB_texture_storage_multisample.h
    RP_GL(glTexStorage2DMultisample, "GL_ARB_texture_storage_multisample !VERSION_4_3 |VERSION_GLES_3_1"),
    RP_GL(glTexStorage3DMultisample, "GL_ARB_texture_storage_multisample !VERSION_4_3 |VERSION_GLES_3_2"),
    RP_GL_AS(glTexStorage3DMultisample, "OES", "GL_OES_texture_storage_multisample_2d_array !VERSION_GLES_3_2"),
    // ARB_ES2_compatibility.h
    RP_GL(glClearDepthf, "GL_ARB_ES2_compatibility |VERSION_GLES_2"),
    RP_GL(glDepthRangef, "GL_ARB_ES2_compatibility |VERSION_GLES_2"),
    RP_GL(glGetShaderPrecisionFormat, "GL_ARB_ES2_compatibility |VERSION_GLES_2"),
    RP_GL(glReleaseShaderCompiler, "GL_ARB_ES2_compatibility |VERSION_GLES_2"),
    RP_GL(glShaderBinary, "GL_ARB_ES2_compatibility |VERSION_GLES_2"),
    // NV_primitive_restart.h
    RP_GL(glPrimitiveRestartIndexNV, "GL_NV_primitive_restart"),
    RP_GL(glPrimitiveRestartNV, "GL_NV_primitive_restart"),
    // ARB_blend_func_extended.h
    RP_GL(glBindFragDataLocationIndexed, "GL_ARB_blend_func_extended"),
    RP_GL(glGetFragDataIndex, "GL_ARB_blend_func_extended"),
    // ARB_viewport_array.h
    RP_GL(glDepthRangeArrayv, "GL_ARB_viewport_array"),
    RP_GL(glDepthRangeIndexed, "GL_ARB_viewport_array"),
    RP_GL(glGetDoublei_v, "GL_ARB_viewport_array"),
    RP_GL(glGetFloati_v, "GL_ARB_viewport_array"),
    RP_GL(glScissorArrayv, "GL_ARB_viewport_array"),
    RP_GL(glScissorIndexed, "GL_ARB_viewport_array"),
    RP_GL(glScissorIndexedv, "GL_ARB_viewport_array"),
    RP_GL(glViewportArrayv, "GL_ARB_viewport_array"),
    RP_GL(glViewportIndexedf, "GL_ARB_viewport_array"),
    RP_GL(glViewportIndexedfv, "GL_ARB_viewport_array"),
    // ARB_draw_elements_base_vertex.h
    RP_GL(glDrawElementsBaseVertex, "GL_ARB_draw_elements_base_vertex |VERSION_GLES_3_2"),
    RP_GL(glDrawElementsInstancedBaseVertex, "GL_ARB_draw_elements_base_vertex |VERSION_GLES_3_2"),
    RP_GL(glDrawRangeElementsBaseVertex, "GL_ARB_draw_elements_base_vertex |VERSION_GLES_3_2"),
    RP_GL(glMultiDrawElementsBaseVertex, "GL_ARB_draw_elements_base_vertex"),
    RP_GL_AS(glDrawElementsBaseVertex, "OES", "GL_OES_draw_elements_base_vertex !VERSION_GLES_3_2"),
    RP_GL_AS(glDrawElementsInstancedBaseVertex, "OES", "GL_OES_draw_elements_base_vertex VERSION_GLES_3 !VERSION_GLES_3_2"),
    RP_GL_AS(glDrawRangeElementsBaseVertex, "OES", "GL_OES_draw_elements_base_vertex VERSION_GLES_3 !VERSION_GLES_3_2"),
    RP_GL_AS(glMultiDrawElementsBaseVertex, "EXT", "GL_OES_draw_elements_base_vertex GL_EXT_multi_draw_arrays"),
    RP_GL_AS(glDrawElementsBaseVertex, "EXT", "GL_EXT_draw_elements_base_vertex !GL_OES_draw_elements_base_vertex !VERSION_GLES_3_2"),
    RP_GL_AS(glDrawElementsInstancedBaseVertex, "EXT", "GL_EXT_draw_elements_base_vertex VERSION_GLES_3 !GL_OES_draw_elements_base_vertex !VERSION_GLES_3_2"),
    RP_GL_AS(glDrawRangeElementsBaseVertex, "EXT", "GL_EXT_draw_elements_base_vertex VERSION_GLES_3 !GL_OES_draw_elements_base_vertex !VERSION_GLES_3_2"),
    RP_GL_AS(glMultiDrawElementsBaseVertex, "EXT", "GL_EXT_draw_elements_base_vertex GL_EXT_multi_draw_arrays !GL_OES_draw_elements_base_vertex !VERSION_GLES_3_2"),
    // ARB_sample_shading.h
    RP_GL_AS(glMinSampleShading, "ARB", "GL_ARB_sample_shading"),
    RP_GL_AS(glMinSampleShading, "OES", "GL_OES_sample_shading !VERSION_GLES_3_2"),
    RP_GL(glMinSampleShading, "VERSION_GLES_3_2"),
    // ARB_debug_output.h
    RP_GL(glDebugMessageCallbackARB, "GL_ARB_debug_output"),
    RP_GL(glDebugMessageControlARB, "GL_ARB_debug_output"),
    RP_GL(glDebugMessageInsertARB, "GL_ARB_debug_output"),
    RP_GL(glGetDebugMessageLogARB, "GL_ARB_debug_output"),
    // KHR_debug.h
    RP_GL_AS(glDebugMessageCallback, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glDebugMessageControl, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glDebugMessageInsert, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glGetDebugMessageLog, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glGetObjectLabel, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glGetObjectPtrLabel, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glObjectLabel, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glObjectPtrLabel, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glPopDebugGroup, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL_AS(glPushDebugGroup, "KHR", "GL_KHR_debug VERSION_GLES_3"),
    RP_GL(glDebugMessageCallback, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glDebugMessageControl, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glDebugMessageInsert, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glGetDebugMessageLog, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glGetObjectLabel, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glGetObjectPtrLabel, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glObjectLabel, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glObjectPtrLabel, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glPopDebugGroup, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    RP_GL(glPushDebugGroup, "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    // ARB_buffer_storage.h
    RP_GL(glBufferStorage, "GL_ARB_buffer_storage !VERSION_4_4"),
    // gl_4_5.h
    RP_GL_AS(glNamedBufferStorage, "EXT", "GL_ARB_buffer_storage GL_EXT_direct_state_access !VERSION_4_5"),
    // ARB_buffer_storage.h
    RP_GL_AS(glBufferStorage, "EXT", "GL_EXT_buffer_storage !GL_ARB_buffer_storage !VERSION_4_4"),
    // gl_3_2.h
    RP_GL_AS(glFramebufferTexture, "EXT", "GL_EXT_geometry_shader !VERSION_3_2"),
    // NV_occlusion_query_samples.h
    RP_GL(glGenOcclusionQueriesNV, "GL_NV_occlusion_query_samples"),
    RP_GL(glDeleteOcclusionQueriesNV, "GL_NV_occlusion_query_samples"),
    RP_GL(glIsOcclusionQueryNV, "GL_NV_occlusion_query_samples"),
    RP_GL(glBeginOcclusionQueryNV, "GL_NV_occlusion_query_samples"),
    RP_GL(glEndOcclusionQueryNV, "GL_NV_occlusion_query_samples"),
    RP_GL(glGetOcclusionQueryivNV, "GL_NV_occlusion_query_samples"),
    RP_GL(glGetOcclusionQueryuivNV, "GL_NV_occlusion_query_samples"),
    // ARB_clip_control.h
    RP_GL(glClipControl, "GL_ARB_clip_control !VERSION_4_5"),
    // ARB_copy_image.h
    RP_GL(glCopyImageSubData, "GL_ARB_copy_image !VERSION_4_3 |VERSION_GLES_3_2"),
    RP_GL_AS(glCopyImageSubData, "NV", "GL_NV_copy_image !GL_ARB_copy_image !VERSION_GLES_3_2"),
    RP_GL_AS(glCopyImageSubData, "OES", "GL_OES_copy_image !VERSION_GLES_3_2"),
    RP_GL_AS(glCopyImageSubData, "EXT", "GL_EXT_copy_image !GL_OES_copy_image !VERSION_GLES_3_2"),
    // gl_3_1.h
    RP_GL_AS(glTexBuffer, "OES", "GL_OES_texture_buffer !VERSION_GLES_3_2"),
    RP_GL_AS(glTexBuffer, "EXT", "GL_EXT_texture_buffer !GL_OES_texture_buffer !VERSION_GLES_3_2"),
    // ARB_blend_func_extended.h
    RP_GL_AS(glBindFragDataLocationIndexed, "EXT", "GL_EXT_blend_func_extended"),
    RP_GL_AS(glGetFragDataIndex, "EXT", "GL_EXT_blend_func_extended"),
    // ARB_shader_storage_buffer_object.h
    RP_GL(glShaderStorageBlockBinding, "ARB_shader_storage_buffer_object !VERSION_4_3"),
    // NV_depth_buffer_float.h
    RP_GL(glDepthRangedNV, "GL_NV_depth_buffer_float"),
    RP_GL(glClearDepthdNV, "GL_NV_depth_buffer_float"),
    RP_GL(glDepthBoundsdNV, "GL_NV_depth_buffer_float"),
    // ARB_shader_image_load_store.h
    RP_GL(glBindImageTexture, "GL_ARB_shader_image_load_store !VERSION_4_2 |VERSION_GLES_3_1"),
    RP_GL(glMemoryBarrier, "GL_ARB_shader_image_load_store !VERSION_4_2 |VERSION_GLES_3_1"),
    // ARB_compute_shader.h
    RP_GL(glDispatchCompute, "GL_ARB_compute_shader !VERSION_4_3 |VERSION_GLES_3_1"),
    RP_GL(glDispatchComputeIndirect, "GL_ARB_compute_shader !VERSION_4_3 |VERSION_GLES_3_1"),
    // gl_4_5.h
    RP_GL(glGetTextureSubImage, "GL_ARB_get_texture_sub_image !VERSION_4_5"),
};

#undef RP_GL
#undef RP_GL_AS

bool ResolveEntryPoints(GLContext* context)
{
  // Every row is tried, so that each missing function is logged.
  bool all_found = true;
  for (const EntryPoint& entry : ENTRY_POINTS)
  {
    if (!RequirementMet(entry.requirement))
      continue;
    if (Resolve(context, entry.name, entry.slot) == nullptr)
      all_found = false;
  }
  return all_found;
}
}  // namespace

u32 Version()
{
  return s_version;
}

bool Supports(const std::string& name)
{
  return Has(name);
}

bool Init(GLContext* context)
{
  // Bootstrap: these three are needed to read the version and the extension list.
  if (Resolve(context, "glGetIntegerv", reinterpret_cast<void**>(&glGetIntegerv)) == nullptr ||
      Resolve(context, "glGetString", reinterpret_cast<void**>(&glGetString)) == nullptr ||
      Resolve(context, "glGetError", reinterpret_cast<void**>(&glGetError)) == nullptr)
  {
    return false;
  }

  ReadVersion();

  // From GL 3.0 / ES 3.0 on the extension list is read with glGetStringi.
  if (s_version > 210 &&
      Resolve(context, "glGetStringi", reinterpret_cast<void**>(&glGetStringi)) == nullptr)
  {
    return false;
  }

  BuildFeatureList(context);
  return ResolveEntryPoints(context);
}
}  // namespace GLExtensions
