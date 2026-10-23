// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DShader.cpp

#include "R3DShader.h"
#include "R3DShaderTriangles.h"
#include "R3DShaderQuads.h"
#include "R3DShaderCommon.h"

#include <cstdio>
#include <vector>

namespace New3D {

namespace
{
  GLuint Compile(GLenum type, const char *const *sources, GLsizei count)
  {
    GLuint id = glCreateShader(type);
    glShaderSource(id, count, sources, nullptr);
    glCompileShader(id);
    GLint ok = GL_FALSE;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE)
    {
      GLint length = 0;
      glGetShaderiv(id, GL_INFO_LOG_LENGTH, &length);
      if (length > 0)
      {
        std::vector<char> text(size_t(length) + 1, '\0');
        glGetShaderInfoLog(id, length, nullptr, text.data());
        printf("%s\n", text.data());
      }
    }
    return id;
  }

  // Polygon stencil bits: bit 7 is set where a polygon that does not return
  // line of sight was drawn; bits 0-6 count layered polygons.
  constexpr GLuint kLosBit = 0x80;
  constexpr GLuint kLayerBits = 0x7F;
}

R3DShader::R3DShader(const Util::Config::Node &config)
  : m_config(config)
{
  Forget();
}

GLint R3DShader::U(const char *name) const
{
  return glGetUniformLocation(m_program, name);
}

bool R3DShader::LoadShader()
{
  const bool quads = m_config["QuadRendering"].ValueAs<bool>();
  const char *vertexSrc = quads ? vertexShaderR3DQuads : vertexShaderR3D;
  const char *fragmentSrc[2] = { quads ? fragmentShaderR3DQuads : fragmentShaderR3D, fragmentShaderR3DCommon };

  m_program = glCreateProgram();
  m_vs = Compile(GL_VERTEX_SHADER, &vertexSrc, 1);
  m_fs = Compile(GL_FRAGMENT_SHADER, fragmentSrc, 2);
  if (quads)
  {
    m_gs = Compile(GL_GEOMETRY_SHADER, &geometryShaderR3DQuads, 1);
    glAttachShader(m_program, m_gs);
  }
  glAttachShader(m_program, m_vs);
  glAttachShader(m_program, m_fs);
  glLinkProgram(m_program);

  GLint linked = GL_FALSE;
  glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE)
  {
    GLint length = 0;
    glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> text(size_t(length) + 1, '\0');
    glGetProgramInfoLog(m_program, length, nullptr, text.data());
    printf("%s\n", text.data());
  }

  m_loc.sheet0 = U("uTexSheet[0]");
  m_loc.sheet1 = U("uTexSheet[1]");
  m_loc.texEnabled = U("uTexEnabled");
  m_loc.microEnabled = U("uMicroEnabled");
  m_loc.microMinLOD = U("uMicroMinLOD");
  m_loc.microID = U("uMicroID");
  m_loc.texRect = U("uTexRect");
  m_loc.texFormat = U("uTexFormat");
  m_loc.texInvert = U("uTexInvert");
  m_loc.texAlpha = U("uTexAlpha");
  m_loc.contour = U("uContour");
  m_loc.opaquePass = U("uOpaquePass");
  m_loc.texWrap = U("uTexWrap");
  m_loc.texPage = U("uTexPage");
  m_loc.outputLayer = U("uOutputLayer");
  m_loc.fogIntensity = U("uFogIntensity");
  m_loc.lit = U("uLit");
  m_loc.shininess = U("uShininess");
  m_loc.specular = U("uSpecular");
  m_loc.specularValue = U("uSpecularValue");
  m_loc.fixedShading = U("uFixedShading");
  m_loc.smoothShading = U("uSmoothShading");
  m_loc.translatorMap = U("uTranslatorMap");
  m_loc.modelScale = U("uModelScale");
  m_loc.nodeAlpha = U("uNodeAlpha");
  m_loc.modelMat = U("uModelMat");
  m_loc.fogDensity = U("uFogDensity");
  m_loc.fogStart = U("uFogStart");
  m_loc.fogColour = U("uFogColour");
  m_loc.fogAttenuation = U("uFogAttenuation");
  m_loc.fogAmbient = U("uFogAmbient");
  m_loc.lighting = U("uLighting");
  m_loc.sunClamp = U("uSunClamp");
  m_loc.intensityClamp = U("uIntensityClamp");
  m_loc.spotShape = U("uSpotShape");
  m_loc.spotRange = U("uSpotRange");
  m_loc.spotColour = U("uSpotColour");
  m_loc.spotFogColour = U("uSpotFogColour");
  m_loc.projMat = U("uProjMat");
  m_loc.hardwareStep = U("uHardwareStep");
  m_loc.cota = U("uCota");
  return true;
}

void R3DShader::UnloadShader()
{
  glUseProgram(0);
  for (GLuint *s : { &m_vs, &m_gs, &m_fs })
  {
    if (*s)
      glDeleteShader(*s);
    *s = 0;
  }
  if (m_program)
    glDeleteProgram(m_program);
  m_program = 0;
}

GLint R3DShader::GetVertexAttribPos(const std::string &name)
{
  auto it = m_attrib.find(name);
  if (it == m_attrib.end())
    it = m_attrib.emplace(name, glGetAttribLocation(m_program, name.c_str())).first;
  return it->second;
}

void R3DShader::Forget()
{
  m_groupKnown = false;
  m_modelKnown = false;
  m_sent.rectValid = false;
  m_offsetX = m_offsetY = 0;
  m_pageFlip = 0;
}

void R3DShader::SetShader(bool enable)
{
  if (!enable)
  {
    glUseProgram(0);
    return;
  }
  glUseProgram(m_program);
  Forget();
  DiscardAlpha(false);
}

void R3DShader::DiscardAlpha(bool opaquePass)
{
  glUniform1i(m_loc.opaquePass, opaquePass);
}

void R3DShader::SetLayer(Layer layer)
{
  glUniform1i(m_loc.outputLayer, GLint(layer));
}

void R3DShader::SetViewportUniforms(const ViewportState *vp)
{
  glUniform1f(m_loc.fogDensity, vp->fogDensity);
  glUniform1f(m_loc.fogStart, vp->fogStart);
  glUniform3fv(m_loc.fogColour, 1, vp->fogColour);
  glUniform1f(m_loc.fogAttenuation, vp->fogAttenuation);
  glUniform1f(m_loc.fogAmbient, vp->fogAmbient);
  glUniform3fv(m_loc.lighting, 2, vp->lightingParams);
  glUniform1i(m_loc.sunClamp, vp->sunClamp);
  glUniform1i(m_loc.intensityClamp, vp->intensityClamp);
  glUniform4fv(m_loc.spotShape, 1, vp->spotEllipse);
  glUniform2fv(m_loc.spotRange, 1, vp->spotRange);
  glUniform3fv(m_loc.spotColour, 1, vp->spotColor);
  glUniform3fv(m_loc.spotFogColour, 1, vp->spotFogColor);
  glUniformMatrix4fv(m_loc.projMat, 1, GL_FALSE, vp->projectionMatrix);
  glUniform1i(m_loc.hardwareStep, vp->hardwareStep);
  glUniform1f(m_loc.cota, vp->cota);
}

void R3DShader::SetModelStates(const ModelInstance *model)
{
  if (!m_modelKnown || model->scale != m_sentScale)
  {
    glUniform1f(m_loc.modelScale, model->scale);
    m_sentScale = model->scale;
  }
  if (!m_modelKnown || model->alpha != m_sentAlpha)
  {
    glUniform1f(m_loc.nodeAlpha, model->alpha);
    m_sentAlpha = model->alpha;
  }
  m_modelKnown = true;

  m_offsetX = model->textureOffsetX;
  m_offsetY = model->textureOffsetY;
  m_pageFlip = model->page;
  m_sent.rectValid = false;   // the texture position depends on the model's offset

  glUniformMatrix4fv(m_loc.modelMat, 1, GL_FALSE, model->modelMat);
}

void R3DShader::ApplyStencil(Stencil mode)
{
  if (mode == Stencil::layered)
  {
    // A layered polygon is drawn only where no layered polygon has been
    // drawn yet at this pixel, and counts itself in.
    glStencilFunc(GL_EQUAL, 0, kLayerBits);
    glStencilOp(GL_KEEP, GL_INCR, GL_INCR);
    glStencilMask(kLayerBits);
  }
  else
  {
    glStencilFunc(GL_ALWAYS, m_sent.noLosReturn ? kLosBit : 0, kLosBit);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(kLosBit);
  }
}

void R3DShader::SetMeshUniforms(const DrawGroup *g)
{
  if (g == nullptr)
    return;

  const bool all = !m_groupKnown;
  Sent &s = m_sent;

  auto sendBool = [&](GLint loc, bool value, bool &sent) {
    if (all || value != sent) { glUniform1i(loc, value); sent = value; }
  };
  auto sendInt = [&](GLint loc, int value, int &sent) {
    if (all || value != sent) { glUniform1i(loc, value); sent = value; }
  };
  auto sendFloat = [&](GLint loc, float value, float &sent) {
    if (all || value != sent) { glUniform1f(loc, value); sent = value; }
  };

  if (all)
  {
    glUniform1i(m_loc.sheet0, 0);
    glUniform1i(m_loc.sheet1, 1);
  }

  sendBool(m_loc.texEnabled, g->textured, s.textured);
  sendBool(m_loc.microEnabled, g->microTexture, s.micro);
  sendInt(m_loc.texPage, g->page ^ m_pageFlip, s.page);
  sendFloat(m_loc.microMinLOD, g->microTextureMinLOD, s.microMinLOD);
  sendInt(m_loc.microID, g->microTextureID, s.microID);

  const int rect[4] = { g->x + m_offsetX, g->y + m_offsetY, g->width, g->height };
  if (all || !s.rectValid || rect[0] != s.rect[0] || rect[1] != s.rect[1] || rect[2] != s.rect[2] || rect[3] != s.rect[3])
  {
    glUniform4i(m_loc.texRect, rect[0], rect[1], rect[2], rect[3]);
    for (int i = 0; i < 4; i++)
      s.rect[i] = rect[i];
    s.rectValid = true;
  }

  sendInt(m_loc.texFormat, g->format, s.format);
  sendBool(m_loc.texInvert, g->inverted, s.invert);
  sendBool(m_loc.contour, g->alphaTest, s.contour);
  sendBool(m_loc.texAlpha, g->textureAlpha, s.texAlpha);
  sendFloat(m_loc.fogIntensity, g->fogIntensity, s.fogIntensity);
  sendBool(m_loc.lit, g->lighting, s.lit);
  sendFloat(m_loc.shininess, g->shininess, s.shininess);
  sendBool(m_loc.specular, g->specular, s.specular);
  sendFloat(m_loc.specularValue, g->specularValue, s.specularValue);
  sendBool(m_loc.fixedShading, g->fixedShading, s.fixedShading);
  sendBool(m_loc.smoothShading, g->smoothShading, s.smoothShading);
  sendBool(m_loc.translatorMap, g->translatorMap, s.translatorMap);

  if (all || g->wrapModeU != s.wrapU || g->wrapModeV != s.wrapV)
  {
    s.wrapU = g->wrapModeU;
    s.wrapV = g->wrapModeV;
    const GLint wrap[2] = { s.wrapU, s.wrapV };
    glUniform2iv(m_loc.texWrap, 1, wrap);
  }

  // Stencil. A change of the layered flag picks the mode for it; a change
  // of only the line-of-sight flag switches to line-of-sight mode, even
  // when the group is layered.
  const bool losChanged = all || g->noLosReturn != s.noLosReturn;
  const bool layerChanged = all || g->layered != s.layered;
  s.noLosReturn = g->noLosReturn;
  s.layered = g->layered;
  if (layerChanged)
    ApplyStencil(s.layered ? Stencil::layered : Stencil::lineOfSight);
  else if (losChanged)
    ApplyStencil(Stencil::lineOfSight);

  m_groupKnown = true;
}

} // New3D
