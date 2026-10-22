// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/ShaderCompileUtils.h"

#include <utility>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

namespace VideoCommon
{
ShaderIncluder::ShaderIncluder(std::string user_path, std::string system_path)
    : m_root_user_path(std::move(user_path)), m_root_system_path(std::move(system_path))
{
}

std::vector<std::string> ShaderIncluder::GetIncludes() const
{
  std::vector<std::string> paths;
  paths.reserve(m_include_results.size());
  for (const auto& entry : m_include_results)
    paths.push_back(entry.first);
  return paths;
}

ShaderIncluder::IncludeResult*
ShaderIncluder::includeLocal(const char* header_name, const char* includer_name, std::size_t depth)
{
  return ProcessInclude(m_root_user_path, header_name, includer_name, depth);
}

ShaderIncluder::IncludeResult*
ShaderIncluder::includeSystem(const char* header_name, const char* includer_name, std::size_t depth)
{
  return ProcessInclude(m_root_system_path, header_name, includer_name, depth);
}

void ShaderIncluder::releaseInclude(IncludeResult* result)
{
  // glslang also hands back the null result of a failed local lookup before it tries the
  // system paths.
  if (result == nullptr)
    return;
  m_include_results.erase(result->headerName);
}

ShaderIncluder::IncludeResult* ShaderIncluder::ProcessInclude(const std::string& root,
                                                              const char* header_name,
                                                              const char* includer_name,
                                                              std::size_t depth)
{
  // Forget the directories of files deeper than the one doing this include. A top-level include
  // starts from the main source's directory, or from the root if it has none.
  m_dirs.resize(depth);
  if (depth == 1)
  {
    const std::string_view source_dir = GetDirectory(includer_name);
    m_dirs.emplace_back(source_dir == "." ? std::string_view{root} : source_dir);
  }

  // Innermost directory first.
  for (std::size_t n = m_dirs.size(); n-- > 0;)
  {
    std::string full_path = WithUnifiedPathSeparators(m_dirs[n] + '/' + header_name);
    std::string contents;
    if (!File::ReadFileToString(full_path, contents))
      continue;

    // Files included from this one are searched next to it first.
    m_dirs.emplace_back(GetDirectory(full_path));

    // Store the text before pointing the result at it, so the pointer stays valid (a moved
    // short string would leave it dangling).
    IncludeResultData& slot = m_include_results[full_path];
    slot.file_data = std::move(contents);
    slot.result = std::make_unique<IncludeResult>(full_path, slot.file_data.data(),
                                                  slot.file_data.size(), nullptr);
    return slot.result.get();
  }
  return nullptr;
}

std::string_view ShaderIncluder::GetDirectory(std::string_view path) const
{
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string_view{"."} : path.substr(0, slash);
}
}  // namespace VideoCommon
