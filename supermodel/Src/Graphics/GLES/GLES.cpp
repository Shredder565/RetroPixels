// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The shader side of the OpenGL ES builds (see GL/glew.h): the renderers'
// programs are desktop GLSL 4.10 / 4.50 written to the subset that GLSL ES
// 3.20 shares with it, so only the first line changes. It becomes
// "#version 320 es", followed by highp defaults for every type the programs
// use (ES fragment shaders have no default float precision, and the integer
// texture samplers have none in any stage).

#include <GL/glew.h>

#include <cstring>
#include <string>

#undef glShaderSource

namespace GLES
{
  namespace
  {
    const char kHeader[] =
      "#version 320 es\n"
      "precision highp float;\n"
      "precision highp int;\n"
      "precision highp sampler2D;\n"
      "precision highp usampler2D;\n"
      "precision highp isampler2D;\n";

    // Where the line holding the #version directive starts and ends (the end
    // is past its newline), if there is one before any code.
    bool FindVersion(const std::string &src, size_t *start, size_t *end)
    {
      size_t at = 0;
      while (at < src.size())
      {
        size_t lineEnd = src.find('\n', at);
        if (lineEnd == std::string::npos)
          lineEnd = src.size();
        const size_t text = src.find_first_not_of(" \t\r", at);
        if (text != std::string::npos && text < lineEnd)
        {
          if (src.compare(text, 8, "#version") != 0)
            return false;   // code (or another directive) first: no version line
          *start = at;
          *end = lineEnd < src.size() ? lineEnd + 1 : lineEnd;
          return true;
        }
        at = lineEnd + 1;
      }
      return false;
    }
  }

  std::string Translate(const std::string &src)
  {
    size_t start = 0, end = 0;
    if (FindVersion(src, &start, &end))
      return kHeader + src.substr(end);
    return kHeader + src;
  }

  void ShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
  {
    std::string all;
    for (GLsizei i = 0; i < count; i++)
    {
      if (!strings[i])
        continue;
      if (lengths && lengths[i] >= 0)
        all.append(strings[i], size_t(lengths[i]));
      else
        all.append(strings[i]);
    }
    const std::string es = Translate(all);
    const GLchar *text = es.c_str();
    glShaderSource(shader, 1, &text, nullptr);
  }
}
