// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DShader.h
//
// The shader program that draws Real3D polygons, and the uniform state for
// viewports, models and draw groups. Uniforms are only re-sent when their
// value changes within a pass; stencil state for layered polygons and the
// line-of-sight bit is set here too.

#ifndef INCLUDED_NEW3D_R3DSHADER_H
#define INCLUDED_NEW3D_R3DSHADER_H

#include <GL/glew.h>

#include <map>
#include <string>

#include "Util/NewConfig.h"
#include "Model.h"

namespace New3D {

class R3DShader
{
public:
  explicit R3DShader(const Util::Config::Node &config);

  bool LoadShader();
  void UnloadShader();

  void SetShader(bool enable);          // enabling also forgets all cached uniform values
  void SetViewportUniforms(const ViewportState *vp);
  void SetModelStates(const ModelInstance *model);
  void SetMeshUniforms(const DrawGroup *group);
  void DiscardAlpha(bool opaquePass);   // texture-alpha polygons: opaque pass keeps solid texels only
  void SetLayer(Layer layer);

  GLint GetVertexAttribPos(const std::string &name);

private:
  enum class Stencil { lineOfSight, layered };
  void ApplyStencil(Stencil mode);
  void Forget();
  GLint U(const char *name) const;

  const Util::Config::Node &m_config;

  GLuint m_program = 0;
  GLuint m_vs = 0;
  GLuint m_gs = 0;
  GLuint m_fs = 0;

  std::map<std::string, GLint> m_uniform;
  std::map<std::string, GLint> m_attrib;

  // Uniform locations used for every draw.
  struct
  {
    GLint sheet0, sheet1, texEnabled, microEnabled, microMinLOD, microID, texRect, texFormat;
    GLint texInvert, texAlpha, contour, opaquePass, texWrap, texPage, outputLayer;
    GLint fogIntensity, lit, shininess, specular, specularValue, fixedShading, smoothShading, translatorMap;
    GLint modelScale, nodeAlpha, modelMat;
    GLint fogDensity, fogStart, fogColour, fogAttenuation, fogAmbient, lighting, sunClamp, intensityClamp;
    GLint spotShape, spotRange, spotColour, spotFogColour, projMat, hardwareStep, cota;
  } m_loc;

  // Values last sent in this pass (valid when m_known is set).
  struct Sent
  {
    bool textured, micro, invert, texAlpha, contour, lit, specular, fixedShading, smoothShading, translatorMap;
    bool layered, noLosReturn;
    int page, microID, format, wrapU, wrapV;
    int rect[4];
    bool rectValid;
    float microMinLOD, fogIntensity, shininess, specularValue;
  } m_sent;
  bool m_groupKnown = false;

  bool m_modelKnown = false;
  float m_sentScale = 1.0f;
  float m_sentAlpha = 1.0f;

  // Current model's texture offset and page flip.
  int m_offsetX = 0;
  int m_offsetY = 0;
  int m_pageFlip = 0;
};

} // New3D

#endif  // INCLUDED_NEW3D_R3DSHADER_H
