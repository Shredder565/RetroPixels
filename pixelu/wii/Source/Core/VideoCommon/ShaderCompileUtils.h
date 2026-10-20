// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glslang/Public/ShaderLang.h>

namespace VideoCommon
{
// Resolves #include directives for glslang from files on disk. Quoted includes start from
// user_path, angle-bracket includes from system_path; nested includes are looked up relative to
// the including file first, then outwards through the files that included it.
class ShaderIncluder final : public glslang::TShader::Includer
{
public:
  ShaderIncluder(std::string user_path, std::string system_path);
  ~ShaderIncluder() override = default;

  // Full paths of the includes glslang currently holds.
  std::vector<std::string> GetIncludes() const;

private:
  IncludeResult* includeLocal(const char* header_name, const char* includer_name,
                              std::size_t depth) override;
  IncludeResult* includeSystem(const char* header_name, const char* includer_name,
                               std::size_t depth) override;
  void releaseInclude(IncludeResult* result) override;

  IncludeResult* ProcessInclude(const std::string& root, const char* header_name,
                                const char* includer_name, std::size_t depth);
  std::string_view GetDirectory(std::string_view path) const;

  std::string m_root_user_path;
  std::string m_root_system_path;
  // Search directories by include depth; the last entry belongs to the innermost file.
  std::vector<std::string> m_dirs;

  // A loaded file. The result points into file_data, so file_data must not move afterwards.
  struct IncludeResultData
  {
    std::unique_ptr<IncludeResult> result;
    std::string file_data;
  };
  std::map<std::string, IncludeResultData, std::less<>> m_include_results;
};

}  // namespace VideoCommon
