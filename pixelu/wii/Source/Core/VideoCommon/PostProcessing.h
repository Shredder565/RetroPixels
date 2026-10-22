// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/Timer.h"
#include "VideoCommon/TextureConfig.h"

class AbstractPipeline;
class AbstractShader;
class AbstractTexture;
class AbstractFramebuffer;

namespace VideoCommon
{
class ShaderIncluder;

// The selected user post-processing shader: its GLSL, and the options declared in its
// [configuration] block together with the values the user set (kept in the main ini, in a
// "<shader>-options" section).
class PostProcessingConfiguration
{
public:
  // One option from a shader's [configuration] block.
  struct ConfigurationOption
  {
    enum class OptionType
    {
      Bool = 0,
      Float,
      Integer,
    };

    bool m_bool_value = false;

    // Up to four components each.
    std::vector<float> m_float_values;
    std::vector<s32> m_integer_values;

    std::vector<float> m_float_min_values;
    std::vector<s32> m_integer_min_values;

    std::vector<float> m_float_max_values;
    std::vector<s32> m_integer_max_values;

    std::vector<float> m_float_step_values;
    std::vector<s32> m_integer_step_values;

    OptionType m_type = OptionType::Bool;

    std::string m_gui_name;
    std::string m_option_name;
    // Only shown in the UI while this other (bool) option is on.
    std::string m_dependent_option;
    // Changed since the values were last uploaded.
    bool m_dirty = false;
  };

  using ConfigMap = std::map<std::string, ConfigurationOption>;

  PostProcessingConfiguration();
  virtual ~PostProcessingConfiguration();

  // Loads the named shader (for the current stereo mode) and its options; an empty name, or a
  // shader that cannot be read, selects the built-in pass-through shader.
  void LoadShader(const std::string& shader);
  void LoadDefaultShader();
  void SaveOptionsConfiguration();
  const std::string& GetShader() const { return m_current_shader; }
  const std::string& GetShaderCode() const { return m_current_shader_code; }
  ShaderIncluder* GetShaderIncluder() { return m_shader_includer.get(); }
  bool IsDirty() const { return m_any_options_dirty; }
  void SetDirty(bool dirty) { m_any_options_dirty = dirty; }
  bool HasOptions() const { return !m_options.empty(); }
  const ConfigMap& GetOptions() const { return m_options; }
  ConfigMap& GetOptions() { return m_options; }
  const ConfigurationOption& GetOption(const std::string& option) { return m_options[option]; }
  // Value setters for the options UI. The option must exist.
  void SetOptionf(const std::string& option, int index, float value);
  void SetOptioni(const std::string& option, int index, s32 value);
  void SetOptionb(const std::string& option, bool value);

private:
  bool m_any_options_dirty = false;
  std::unique_ptr<ShaderIncluder> m_shader_includer;
  std::string m_current_shader;
  std::string m_current_shader_code;
  ConfigMap m_options;

  void LoadOptions(const std::string& code);
  void LoadOptionsConfiguration();
};

// Draws the XFB to the backbuffer. Up to two passes: a built-in pass (colour correction, gamma
// and output resampling) and the user's shader. When both are needed the built-in pass renders
// into an RGBA16F intermediate texture first.
class PostProcessing
{
public:
  PostProcessing();
  virtual ~PostProcessing();

  // Names of the selectable shaders (normal, passive stereo, anaglyph stereo).
  static std::vector<std::string> GetShaderList();
  static std::vector<std::string> GetPassiveShaderList();
  static std::vector<std::string> GetAnaglyphShaderList();

  PostProcessingConfiguration* GetConfig() { return &m_config; }

  bool Initialize(AbstractTextureFormat format);

  void RecompileShader();
  void RecompilePipeline();

  // Draws src of src_tex into dst of the bound framebuffer. A negative src_layer draws every
  // layer to the matching target layer.
  void BlitFromTexture(const MathUtil::Rectangle<int>& dst, const MathUtil::Rectangle<int>& src,
                       const AbstractTexture* src_tex, int src_layer = -1);

  bool IsColorCorrectionActive() const;
  bool NeedsIntermediaryBuffer() const;

protected:
  std::string GetUniformBufferHeader(bool user_post_process) const;
  std::string GetHeader(bool user_post_process) const;
  std::string GetFooter() const;

  bool CompileVertexShader();
  bool CompilePixelShader();
  bool CompilePipeline();

  size_t CalculateUniformsSize(bool user_post_process) const;
  void FillUniformBuffer(const MathUtil::Rectangle<int>& src, const AbstractTexture* src_tex,
                         int src_layer, const MathUtil::Rectangle<int>& dst,
                         const MathUtil::Rectangle<int>& wnd, u8* buffer, bool user_post_process,
                         bool intermediary_buffer);

  // Source of the "time" uniform.
  Common::Timer m_timer;

  // Built-in pass.
  PostProcessingConfiguration::ConfigMap m_default_options;
  std::unique_ptr<AbstractShader> m_default_vertex_shader;
  std::unique_ptr<AbstractShader> m_default_pixel_shader;
  std::unique_ptr<AbstractPipeline> m_default_pipeline;
  std::unique_ptr<AbstractFramebuffer> m_intermediary_frame_buffer;
  std::unique_ptr<AbstractTexture> m_intermediary_color_texture;
  std::vector<u8> m_default_uniform_staging_buffer;
  // User pass.
  PostProcessingConfiguration m_config;
  std::unique_ptr<AbstractShader> m_vertex_shader;
  std::unique_ptr<AbstractShader> m_pixel_shader;
  std::unique_ptr<AbstractPipeline> m_pipeline;
  std::vector<u8> m_uniform_staging_buffer;

  AbstractTextureFormat m_framebuffer_format = AbstractTextureFormat::Undefined;
};
}  // namespace VideoCommon
