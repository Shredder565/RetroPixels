// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef HAS_LIBMGBA

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QImage>
#include <QPoint>
#include <QWidget>

#include "Common/CommonTypes.h"
#include "Core/HW/GBACore.h"

class QCloseEvent;
class QContextMenuEvent;
class QDragEnterEvent;
class QDropEvent;
class QMouseEvent;
class QPaintEvent;

namespace NetPlay
{
struct PadDetails;
}  // namespace NetPlay

// The window of one emulated Game Boy Advance: shows its screen and offers the ROM, save,
// state, volume and window options in a context menu.
class GBAWidget : public QWidget
{
  Q_OBJECT
public:
  explicit GBAWidget(std::weak_ptr<HW::GBA::Core> core, const HW::GBA::CoreInfo& info,
                     const std::optional<NetPlay::PadDetails>& netplay_pad);
  ~GBAWidget() override;

  void GameChanged(const HW::GBA::CoreInfo& info);
  void SetVideoBuffer(std::span<const u32> video_buffer);

  void SetVolume(int volume);
  void VolumeDown();
  void VolumeUp();
  bool IsMuted();
  void ToggleMute();
  void ToggleDisconnect();

  void LoadROM();
  void UnloadROM();
  void PromptForEReaderCards();
  void ResetCore();
  void DoState(bool export_state);
  void ImportExportSave(bool export_save);
  void Resize(int scale);

  bool IsBorderless() const;
  void SetBorderless(bool enable);

  bool IsAlwaysOnTop() const;
  void SetAlwaysOnTop(bool enable);

private:
  void UpdateTitle();
  void ApplyVolume();

  static Qt::WindowFlags LoadWindowFlags(int device_number);
  void RestoreSettings();
  void StoreSettings();

  bool CanControlCore();
  bool CanResetCore();
  QRect ScreenRect() const;

  void closeEvent(QCloseEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

  std::weak_ptr<HW::GBA::Core> m_core;
  HW::GBA::CoreInfo m_info;
  QImage m_frame;
  QImage m_previous_frame;
  // The pad this window belongs to; under NetPlay this is the local pad number.
  int m_pad;
  bool m_pad_is_local = true;
  std::string m_player_name;
  int m_volume = 0;
  bool m_muted = false;
  bool m_disconnected = false;
  // Dragging a borderless window around.
  bool m_dragging = false;
  QPoint m_drag_offset;
  bool m_blend_frames = false;
};

// Lives on the GUI thread and owns the GBAWidget of one GBAHost.
class GBAWidgetController : public QObject
{
  Q_OBJECT
public:
  explicit GBAWidgetController() = default;
  ~GBAWidgetController() override;

  void Create(std::weak_ptr<HW::GBA::Core> core, const HW::GBA::CoreInfo& info);
  void GameChanged(const HW::GBA::CoreInfo& info);
  void FrameEnded(std::span<const u32> video_buffer);

private:
  GBAWidget* m_widget{};
};
#endif  // HAS_LIBMGBA
