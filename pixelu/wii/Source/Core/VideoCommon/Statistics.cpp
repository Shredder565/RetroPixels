// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Statistics.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

#include <imgui.h>

#include "Core/DolphinAnalytics.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/XFMemory.h"

Statistics g_stats;

namespace
{
Common::EventHook s_frame_start_hook;
Common::EventHook s_frame_end_hook;

// Set at the start of each frame; the first scissor recorded afterwards drops the old list.
bool s_scissors_stale = false;

void PlaceTopLeft()
{
  const float ui_scale = ImGui::GetIO().DisplayFramebufferScale.x;
  ImGui::SetNextWindowPos(ImVec2(10.0f * ui_scale, 10.0f * ui_scale), ImGuiCond_FirstUseEver);
}

// Begins a window; when it is collapsed or hidden, closes it again and returns false.
bool OpenWindow(const char* title, ImGuiWindowFlags flags)
{
  if (ImGui::Begin(title, nullptr, flags))
    return true;
  ImGui::End();
  return false;
}
}  // namespace

void Statistics::ResetFrame()
{
  this_frame = {};
  s_scissors_stale = true;
  // Keep the most recent configuration so the overlay is not empty between frames.
  if (scissors.size() > 1)
    scissors.erase(scissors.begin(), std::prev(scissors.end()));
}

void Statistics::SwapDL()
{
  ThisFrame& f = this_frame;
  std::swap(f.num_prims, f.num_dl_prims);
  std::swap(f.num_xf_loads, f.num_xf_loads_in_dl);
  std::swap(f.num_cp_loads, f.num_cp_loads_in_dl);
  std::swap(f.num_bp_loads, f.num_bp_loads_in_dl);
}

void Statistics::Display() const
{
  const float ui_scale = ImGui::GetIO().DisplayFramebufferScale.x;
  PlaceTopLeft();
  ImGui::SetNextWindowSizeConstraints(ImVec2(275.0f * ui_scale, 400.0f * ui_scale),
                                      ImGui::GetIO().DisplaySize);
  if (!OpenWindow("Statistics", ImGuiWindowFlags_NoNavInputs))
    return;

  ImGui::Columns(2, "Statistics", true);

  struct Row
  {
    const char* label;
    int value;
    const char* unit;
  };
  const auto show = [](const Row& row) {
    ImGui::TextUnformatted(row.label);
    ImGui::NextColumn();
    ImGui::Text("%d%s", row.value, row.unit);
    ImGui::NextColumn();
  };
  const ThisFrame& f = this_frame;

  // The software renderer is the only backend that fills in these counters.
  if (g_backend_info.api_type == APIType::Nothing)
  {
    const Row software_rows[] = {
        {"Objects", f.num_drawn_objects, ""},
        {"Vertices Loaded", f.num_vertices_loaded, ""},
        {"Triangles Input", f.num_triangles_in, ""},
        {"Triangles Rejected", f.num_triangles_rejected, ""},
        {"Triangles Culled", f.num_triangles_culled, ""},
        {"Triangles Clipped", f.num_triangles_clipped, ""},
        {"Triangles Drawn", f.num_triangles_drawn, ""},
        {"Rasterized Pix", f.rasterized_pixels, ""},
        {"TEV Pix In", f.tev_pixels_in, ""},
        {"TEV Pix Out", f.tev_pixels_out, ""},
    };
    for (const Row& row : software_rows)
      show(row);
  }

  const Row rows[] = {
      {"Textures created", num_textures_created, ""},
      {"Textures uploaded", num_textures_uploaded, ""},
      {"Textures alive", num_textures_alive, ""},
      {"pshaders created", num_pixel_shaders_created, ""},
      {"pshaders alive", num_pixel_shaders_alive, ""},
      {"vshaders created", num_vertex_shaders_created, ""},
      {"vshaders alive", num_vertex_shaders_alive, ""},
      {"shaders changes", f.num_shader_changes, ""},
      {"dlists called", f.num_dlists_called, ""},
      {"Primitive joins", f.num_primitive_joins, ""},
      {"Draw calls", f.num_draw_calls, ""},
      {"Primitives", f.num_prims, ""},
      {"Primitives (DL)", f.num_dl_prims, ""},
      {"XF loads", f.num_xf_loads, ""},
      {"XF loads (DL)", f.num_xf_loads_in_dl, ""},
      {"CP loads", f.num_cp_loads, ""},
      {"CP loads (DL)", f.num_cp_loads_in_dl, ""},
      {"BP loads", f.num_bp_loads, ""},
      {"BP loads (DL)", f.num_bp_loads_in_dl, ""},
      {"Vertex streamed", f.bytes_vertex_streamed / 1024, " kB"},
      {"Index streamed", f.bytes_index_streamed / 1024, " kB"},
      {"Uniform streamed", f.bytes_uniform_streamed / 1024, " kB"},
      {"Vertex Loaders", num_vertex_loaders, ""},
      {"EFB peeks:", f.num_efb_peeks, ""},
      {"EFB pokes:", f.num_efb_pokes, ""},
      {"Draw dones:", f.num_draw_done, ""},
  };
  for (const Row& row : rows)
    show(row);

  ImGui::TextUnformatted("Tokens:");
  ImGui::NextColumn();
  ImGui::Text("%d/%d", f.num_token, f.num_token_int);
  ImGui::NextColumn();

  ImGui::Columns(1);
  ImGui::End();
}

void Statistics::DisplayProj() const
{
  if (!OpenWindow("Projection Statistics", ImGuiWindowFlags_NoNavInputs))
    return;

  ImGui::TextUnformatted("Projection #: X for Raw 6=0 (X for Raw 6!=0)");
  ImGui::NewLine();

  // Matrix element each raw XF projection parameter ends up in.
  constexpr std::array<int, 6> raw_target = {0, 2, 5, 6, 10, 11};
  for (int i = 0; i < static_cast<int>(gproj.size()); ++i)
  {
    const auto element = static_cast<size_t>(i);
    const auto raw = std::ranges::find(raw_target, i);
    if (raw != raw_target.end())
    {
      const auto raw_index = raw - raw_target.begin();
      ImGui::Text("Projection %d: %f (%f) Raw %d: %f", i, gproj[element], g2proj[element],
                  static_cast<int>(raw_index), proj[static_cast<size_t>(raw_index)]);
    }
    else
    {
      ImGui::Text("Projection %d: %f (%f)", i, gproj[element], g2proj[element]);
    }
  }

  ImGui::NewLine();
  ImGui::Text("Avg Projection Viewport Ratio Persp(3D): %f", avg_persp_proj_viewport_ratio);
  ImGui::Text("Avg Projection Viewport Ratio Ortho(2D): %f", avg_ortho_proj_viewport_ratio);

  ImGui::End();
}

void Statistics::AddScissorRect()
{
  if (s_scissors_stale)
  {
    scissors.clear();
    s_scissors_stale = false;
  }

  BPFunctions::ScissorResult current = BPFunctions::ComputeScissorRects(
      bpmem.scissorTL, bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);

  const auto same = [&](const BPFunctions::ScissorResult& seen) {
    return seen.Matches(current, show_scissors, show_viewports);
  };
  // With duplicates allowed, only an immediate repeat is dropped.
  bool is_new = true;
  if (!scissors.empty())
    is_new = allow_duplicate_scissors ? !same(scissors.back()) : std::ranges::none_of(scissors, same);

  if (is_new)
    scissors.push_back(std::move(current));
}

namespace
{
// Draws the scissor overlay: a 3x3 grid of 1024-unit cells covering coordinates -1024..2048,
// with each recorded configuration in its own colour, plus optional tables of the values.
class ScissorOverlay
{
public:
  static constexpr int GRID_FIRST = -1024;
  static constexpr int GRID_LAST = GRID_FIRST + 3 * 1024;
  static constexpr int SCISSOR_COLUMNS = 8;
  static constexpr int VIEWPORT_COLUMNS = 5;

  explicit ScissorOverlay(const Statistics& stats)
      : m_stats(stats), m_draw(ImGui::GetWindowDrawList()), m_origin(ImGui::GetCursorScreenPos()),
        m_grey(ImGui::GetColorU32(ImVec4(.5f, .5f, .5f, 1.f)))
  {
  }

  void ReserveCanvas() const
  {
    const int side = 1024 * 3 / m_stats.scissor_scale;
    ImGui::Dummy(ImVec2(static_cast<float>(side), static_cast<float>(side)));
  }

  void DrawGrid() const
  {
    for (int x = GRID_FIRST; x <= GRID_LAST; x += 1024)
      m_draw->AddLine(At(x, GRID_FIRST), At(x, GRID_LAST), m_grey);
    for (int y = GRID_FIRST; y <= GRID_LAST; y += 1024)
      m_draw->AddLine(At(GRID_FIRST, y), At(GRID_LAST, y), m_grey);
  }

  void DrawEntry(size_t index) const
  {
    const BPFunctions::ScissorResult& entry = m_stats.scissors[index];
    const ImU32 colour = ImGui::GetColorU32(Colour(index));

    // Every position the EFB can take given the (wrapping) scissor offset. Starting two cells
    // back catches offsets large enough to show two copies in the upper cells.
    const int off_x = static_cast<int>(entry.scissor_off.x << 1);
    const int off_y = static_cast<int>(entry.scissor_off.y << 1);
    for (int y = off_y - 2048; y < GRID_LAST; y += 1024)
    {
      for (int x = off_x - 2048; x < GRID_LAST; x += 1024)
        Outline(x, y, x + static_cast<int>(EFB_WIDTH), y + static_cast<int>(EFB_HEIGHT), colour,
                false);
    }
    // The unwrapped offset, so entries using the extra bit stand out.
    Cross(static_cast<int>(entry.scissor_off.x_full << 1),
          static_cast<int>(entry.scissor_off.y_full << 1), 4, colour);

    if (m_stats.show_scissors)
    {
      Outline(static_cast<int>(entry.scissor_tl.x), static_cast<int>(entry.scissor_tl.y),
              static_cast<int>(entry.scissor_br.x + 1), static_cast<int>(entry.scissor_br.y + 1),
              colour);
    }
    if (m_stats.show_viewports)
    {
      Outline(static_cast<int>(entry.viewport_left), static_cast<int>(entry.viewport_top),
              static_cast<int>(entry.viewport_right), static_cast<int>(entry.viewport_bottom),
              colour);
    }

    // The final rectangle is the one hardware backends use; it is drawn fainter here.
    const size_t count = entry.rectangles.size();
    for (size_t i = 0; i < count; ++i)
    {
      const auto& r = entry.rectangles[i];
      m_draw->AddRectFilled(At(r.rect.left + r.x_off, r.rect.top + r.y_off),
                            At(r.rect.right + r.x_off, r.rect.bottom + r.y_off),
                            WithAlpha(colour, i + 1 == count ? 0x40 : 0x80));
    }
  }

  static void ScissorHeader()
  {
    for (const char* name : {"#", "x0", "y0", "x1", "y1", "xOff", "yOff", "Affected"})
      ImGui::TableSetupColumn(name);
    ImGui::TableHeadersRow();
  }

  void ScissorRow(size_t index) const
  {
    const BPFunctions::ScissorResult& entry = m_stats.scissors[index];
    // Values are shown relative to the viewport corner (top and left are used crosswise, as the
    // overlay has always done).
    const auto relative = [](u32 value, float origin) {
      return static_cast<int>(static_cast<float>(value) - origin);
    };
    const int cells[] = {
        relative(entry.scissor_tl.x, entry.viewport_top),
        relative(entry.scissor_tl.y, entry.viewport_top),
        relative(entry.scissor_br.x, entry.viewport_left),
        relative(entry.scissor_br.y, entry.viewport_left),
        relative(entry.scissor_off.x << 1, entry.viewport_top),
        relative(entry.scissor_off.y << 1, entry.viewport_left),
    };

    IndexCell(index);
    for (const int value : cells)
    {
      ImGui::TableNextColumn();
      ImGui::Text("%d", value);
    }

    // "Affected": a thumbnail of the EFB with the generated rectangles, sized to fill the row
    // (two rows when the raw values are shown below).
    ImGui::TableNextColumn();
    float unit = ImGui::GetTextLineHeight() / EFB_HEIGHT;
    if (m_stats.show_raw_scissors)
      unit += ImGui::GetTextLineHeightWithSpacing() / EFB_HEIGHT;
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(EFB_WIDTH * unit, 1));
    const size_t count = entry.rectangles.size();
    for (size_t i = 0; i < count; ++i)
    {
      const auto& r = entry.rectangles[i];
      const ImU32 colour = WithAlpha(ImGui::GetColorU32(Colour(index)), i + 1 == count ? 0x80 : 0x40);
      const auto scaled = [unit](int v) { return static_cast<float>(v) * unit; };
      m_draw->AddRectFilled(ImVec2(corner.x + scaled(r.rect.left), corner.y + scaled(r.rect.top)),
                            ImVec2(corner.x + scaled(r.rect.right), corner.y + scaled(r.rect.bottom)),
                            colour);
    }
    m_draw->AddRect(corner, ImVec2(corner.x + EFB_WIDTH * unit, corner.y + EFB_HEIGHT * unit),
                    m_grey);
    ImGui::SameLine();
    ImGui::Text("%d", static_cast<int>(count));

    if (!m_stats.show_raw_scissors)
      return;

    ImGui::TableNextColumn();
    ImGui::TextColored(Colour(index), "Raw");
    const u32 raw[] = {
        entry.scissor_tl.x_full.Value(), entry.scissor_tl.y_full.Value(),
        entry.scissor_br.x_full.Value(), entry.scissor_br.y_full.Value(),
        entry.scissor_off.x_full.Value(), entry.scissor_off.y_full.Value(),
    };
    for (const u32 value : raw)
    {
      ImGui::TableNextColumn();
      ImGui::Text("%d", static_cast<int>(value));
    }
    ImGui::TableNextColumn();
  }

  // Placeholder row for an expected entry that was not recorded.
  void ScissorPlaceholder(size_t index) const
  {
    PlaceholderRow(index);
    if (m_stats.show_raw_scissors)
    {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(Colour(index), "Raw");
    }
  }

  static void ViewportHeader()
  {
    for (const char* name : {"#", "vx0", "vy0", "vx1", "vy1"})
      ImGui::TableSetupColumn(name);
    ImGui::TableHeadersRow();
  }

  void ViewportRow(size_t index) const
  {
    const BPFunctions::ScissorResult& entry = m_stats.scissors[index];
    IndexCell(index);
    for (const float value :
         {entry.viewport_left, entry.viewport_top, entry.viewport_right, entry.viewport_bottom})
    {
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", value);
    }
  }

  static void ViewportPlaceholder(size_t index) { PlaceholderRow(index); }

private:
  static const ImVec4& Colour(size_t index)
  {
    static const std::array<ImVec4, 6> palette = {
        ImVec4(1, 0, 0, 1), ImVec4(1, 1, 0, 1), ImVec4(0, 1, 0, 1),
        ImVec4(0, 1, 1, 1), ImVec4(0, 0, 1, 1), ImVec4(1, 0, 1, 1),
    };
    return palette[index % palette.size()];
  }

  static ImU32 WithAlpha(ImU32 colour, u32 alpha)
  {
    return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
  }

  static void IndexCell(size_t index)
  {
    ImGui::TableNextColumn();
    ImGui::TextColored(Colour(index), "%zu", index + 1);
  }

  static void PlaceholderRow(size_t index)
  {
    ImGui::TableNextRow();
    IndexCell(index);
  }

  // Screen position of grid coordinate (x, y), nudged by (dx, dy) pixels.
  ImVec2 At(int x, int y, int dx = 0, int dy = 0) const
  {
    const auto scale = static_cast<float>(m_stats.scissor_scale);
    const int px = static_cast<int>(static_cast<float>(x - GRID_FIRST) / scale);
    const int py = static_cast<int>(static_cast<float>(y - GRID_FIRST) / scale);
    return ImVec2(m_origin.x + static_cast<float>(px) + static_cast<float>(dx),
                  m_origin.y + static_cast<float>(py) + static_cast<float>(dy));
  }

  void Cross(int x, int y, int arm, ImU32 colour) const
  {
    // The far ends get one extra pixel; otherwise ImGui draws the arms visibly short there.
    m_draw->AddLine(At(x, y, -arm, -arm), At(x, y, arm + 1, arm + 1), colour);
    m_draw->AddLine(At(x, y, -arm, arm), At(x, y, arm + 1, -arm - 1), colour);
  }

  // Outlines a rectangle clipped to the grid. When clipping leaves nothing, the two corners are
  // marked instead (if requested).
  void Outline(int x0, int y0, int x1, int y1, ImU32 colour, bool mark_corners = true) const
  {
    x0 = std::clamp(x0, GRID_FIRST, GRID_LAST);
    y0 = std::clamp(y0, GRID_FIRST, GRID_LAST);
    x1 = std::clamp(x1, GRID_FIRST, GRID_LAST);
    y1 = std::clamp(y1, GRID_FIRST, GRID_LAST);
    if (x0 < x1 && y0 < y1)
    {
      m_draw->AddRect(At(x0, y0), At(x1, y1), colour);
      return;
    }
    if (!mark_corners)
      return;
    m_draw->AddLine(At(x0, y0), At(x0, y0, 8, 0), colour);
    m_draw->AddLine(At(x0, y0), At(x0, y0, 0, 8), colour);
    m_draw->AddLine(At(x1, y1), At(x1, y1, -8, 0), colour);
    m_draw->AddLine(At(x1, y1), At(x1, y1, 0, -8), colour);
  }

  const Statistics& m_stats;
  ImDrawList* m_draw;
  ImVec2 m_origin;
  ImU32 m_grey;
};

void ScissorOptions(Statistics& stats)
{
  if (!ImGui::TreeNode("Options"))
    return;
  ImGui::Checkbox("Allow Duplicates", &stats.allow_duplicate_scissors);
  ImGui::Checkbox("Show Scissors", &stats.show_scissors);
  ImGui::BeginDisabled(!stats.show_scissors);
  ImGui::Checkbox("Show Raw Values", &stats.show_raw_scissors);
  ImGui::EndDisabled();
  ImGui::Checkbox("Show Viewports", &stats.show_viewports);
  ImGui::Checkbox("Show Text", &stats.show_text);
  ImGui::DragInt("Scale", &stats.scissor_scale, .2f, 1, 16);
  ImGui::DragInt("Expected Scissor Count", &stats.scissor_expected_count, .2f, 0, 16);
  ImGui::TreePop();
}

// Previous/next buttons and the caption. The index is only clamped when stepping forward.
void ScissorNavigation(Statistics& stats)
{
  const size_t total = stats.scissors.size();

  ImGui::BeginDisabled(stats.current_scissor == 0);
  if (ImGui::ArrowButton("##left", ImGuiDir_Left))
    --stats.current_scissor;
  ImGui::EndDisabled();
  ImGui::SameLine();

  ImGui::BeginDisabled(stats.current_scissor >= total);
  if (ImGui::ArrowButton("##right", ImGuiDir_Right))
    stats.current_scissor = std::min(stats.current_scissor + 1, total);
  ImGui::EndDisabled();
  ImGui::SameLine();

  const size_t shown = stats.current_scissor;
  if (shown == 0)
    ImGui::Text("Displaying all %zu rectangle(s)", total);
  else if (shown <= total)
    ImGui::Text("Displaying rectangle %zu / %zu", shown, total);
  else
    ImGui::Text("Displaying rectangle %zu / %zu (OoB)", shown, total);
}
}  // namespace

void Statistics::DisplayScissor()
{
  PlaceTopLeft();
  if (!OpenWindow("Scissor Rectangles", ImGuiWindowFlags_AlwaysAutoResize))
    return;

  ScissorOptions(*this);
  ScissorNavigation(*this);

  const ScissorOverlay overlay(*this);
  overlay.ReserveCanvas();
  overlay.DrawGrid();

  const size_t total = scissors.size();
  const size_t expected = static_cast<size_t>(scissor_expected_count);

  if (current_scissor == 0)
  {
    for (size_t i = 0; i < total; ++i)
      overlay.DrawEntry(i);

    if (show_text && show_scissors && ImGui::BeginTable("Scissors", ScissorOverlay::SCISSOR_COLUMNS))
    {
      ScissorOverlay::ScissorHeader();
      for (size_t i = 0; i < total; ++i)
        overlay.ScissorRow(i);
      for (size_t i = total; i < expected; ++i)
        overlay.ScissorPlaceholder(i);
      ImGui::EndTable();
    }
    if (show_text && show_viewports &&
        ImGui::BeginTable("Viewports", ScissorOverlay::VIEWPORT_COLUMNS))
    {
      ScissorOverlay::ViewportHeader();
      for (size_t i = 0; i < total; ++i)
        overlay.ViewportRow(i);
      for (size_t i = total; i < expected; ++i)
        ScissorOverlay::ViewportPlaceholder(i);
      ImGui::EndTable();
    }
  }
  else if (current_scissor <= total)
  {
    // The selection can point past the end when this frame recorded fewer entries; that case
    // is handled below.
    const size_t index = current_scissor - 1;
    overlay.DrawEntry(index);

    // In single-entry mode the viewport table goes with the scissor table.
    if (show_text && show_scissors)
    {
      if (ImGui::BeginTable("Scissors", ScissorOverlay::SCISSOR_COLUMNS))
      {
        ScissorOverlay::ScissorHeader();
        overlay.ScissorRow(index);
        ImGui::EndTable();
      }
      if (ImGui::BeginTable("Viewports", ScissorOverlay::VIEWPORT_COLUMNS))
      {
        ScissorOverlay::ViewportHeader();
        overlay.ViewportRow(index);
        ImGui::EndTable();
      }
    }
  }
  else if (show_text)
  {
    if (show_scissors)
      ImGui::Text("Scissor %zu: Does not exist", current_scissor);
    if (show_viewports)
      ImGui::Text("Viewport %zu: Does not exist", current_scissor);
  }

  ImGui::End();
}

void Statistics::Init()
{
  s_frame_start_hook = GetVideoEvents().before_frame_event.Register([] { g_stats.ResetFrame(); });

  s_frame_end_hook = GetVideoEvents().after_frame_event.Register([](const Core::System& system) {
    const Statistics::ThisFrame& f = g_stats.this_frame;
    DolphinAnalytics::Instance().ReportPerformanceInfo({
        .speed_ratio = system.GetSystemTimers().GetEstimatedEmulationPerformance(),
        .num_prims = f.num_prims + f.num_dl_prims,
        .num_draw_calls = f.num_draw_calls,
    });
  });
}

void Statistics::Shutdown()
{
  s_frame_start_hook.reset();
  s_frame_end_hook.reset();
}
