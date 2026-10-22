// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef HAS_LIBMGBA

#include "DolphinQt/GBAWidget.h"

#include <algorithm>

#include <fmt/format.h>

#include <QAction>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>

#include "AudioCommon/AudioCommon.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/GameCubePane.h"

namespace
{
using CoreHandle = std::weak_ptr<HW::GBA::Core>;

// Volume up/down change the volume by this many percent.
constexpr int VOLUME_STEP = 10;

// Stops the core, then starts it again with another ROM (none for an empty path). If the new
// ROM doesn't start, the configured ROM is put back and started instead.
void SwapRom(const CoreHandle& core, std::string_view rom_path = {})
{
  Core::RunOnCPUThread(Core::System::GetInstance(), [core, rom = std::string(rom_path)] {
    const auto gba = core.lock();
    if (!gba)
      return;

    const auto& setting = Config::MAIN_GBA_ROM_PATHS[static_cast<size_t>(gba->GetCoreInfo().device_number)];
    gba->Stop();
    Config::SetCurrent(setting, rom);

    auto& timing = Core::System::GetInstance().GetCoreTiming();
    if (gba->Start(timing.GetTicks()))
      return;

    Config::SetCurrent(setting, Config::GetBase(setting));
    gba->Start(timing.GetTicks());
  });
}

void InsertCard(const CoreHandle& core, std::string_view card_path)
{
  Core::RunOnCPUThread(Core::System::GetInstance(), [core, card = std::string(card_path)] {
    if (const auto gba = core.lock())
      gba->EReaderQueueCard(card);
  });
}

// Runs `work` on the CPU thread with the core, if it still exists.
template <typename Action>
void WithCore(const CoreHandle& core, Action work)
{
  Core::RunOnCPUThread(Core::System::GetInstance(), [core, action = std::move(work)] {
    if (const auto gba = core.lock())
      action(*gba);
  });
}

QString SettingKey(const char* name, int pad)
{
  return QStringLiteral("gbawidget/%1%2").arg(QLatin1String(name)).arg(pad + 1);
}

QString PickFile(QWidget* parent, bool for_saving, const QString& filter)
{
  const QString caption = GBAWidget::tr("Select a File");
  const QString path =
      for_saving ? DolphinFileDialog::getSaveFileName(parent, caption, QString(), filter, nullptr,
                                                      QFileDialog::Options()) :
                   DolphinFileDialog::getOpenFileName(parent, caption, QString(), filter, nullptr,
                                                      QFileDialog::Options());
  return QDir::toNativeSeparators(path);
}

// Under NetPlay, the pad that this GBA is attached to, if that pad is one of ours.
std::optional<NetPlay::PadDetails> NetPlayPadOf(int device_number)
{
  if (!NetPlay::IsNetPlayRunning())
    return std::nullopt;
  const NetPlay::PadDetails pad = NetPlay::GetPadDetails(device_number);
  if (pad.local_pad >= 4)
    return std::nullopt;
  return pad;
}
}  // namespace

GBAWidget::GBAWidget(std::weak_ptr<HW::GBA::Core> core, const HW::GBA::CoreInfo& info,
                     const std::optional<NetPlay::PadDetails>& netplay_pad)
    : QWidget(nullptr, LoadWindowFlags(netplay_pad ? netplay_pad->local_pad : info.device_number)),
      m_core(std::move(core)), m_info(info), m_pad(info.device_number)
{
  // Under NetPlay the window may belong to another player, and may be hidden.
  bool shown = true;
  if (netplay_pad)
  {
    m_player_name = netplay_pad->player_name;
    m_pad_is_local = netplay_pad->is_local;
    m_pad = netplay_pad->local_pad;
    shown = !netplay_pad->hide_gba;
  }

  setWindowIcon(Resources::GetAppIcon());
  setAcceptDrops(true);
  resize(static_cast<int>(m_info.width), static_cast<int>(m_info.height));
  setVisible(shown);

  // A hidden GBA is also muted.
  SetVolume(100);
  if (!shown)
    ToggleMute();

  RestoreSettings();
  UpdateTitle();
}

GBAWidget::~GBAWidget()
{
  StoreSettings();
}

void GBAWidget::GameChanged(const HW::GBA::CoreInfo& info)
{
  m_info = info;
  m_previous_frame = QImage();
  UpdateTitle();
  update();
}

void GBAWidget::SetVideoBuffer(std::span<const u32> video_buffer)
{
  m_previous_frame = std::move(m_frame);

  // The core hands out 0xAABBGGRR pixels; Qt wants 0xAARRGGBB. A buffer of the wrong size (the
  // core is switching games) shows as black.
  if (video_buffer.size() != static_cast<size_t>(m_info.width * m_info.height))
  {
    m_frame = QImage();
  }
  else
  {
    const QImage raw(reinterpret_cast<const uchar*>(video_buffer.data()),
                     static_cast<int>(m_info.width), static_cast<int>(m_info.height),
                     QImage::Format_ARGB32);
    m_frame = raw.convertToFormat(QImage::Format_RGB32).rgbSwapped();
  }
  update();
}

void GBAWidget::SetVolume(int volume)
{
  m_muted = false;
  m_volume = std::clamp(volume, 0, 100);
  ApplyVolume();
}

void GBAWidget::VolumeDown()
{
  SetVolume(m_volume - VOLUME_STEP);
}

void GBAWidget::VolumeUp()
{
  SetVolume(m_volume + VOLUME_STEP);
}

bool GBAWidget::IsMuted()
{
  return m_muted;
}

void GBAWidget::ToggleMute()
{
  m_muted = !m_muted;
  ApplyVolume();
}

void GBAWidget::ToggleDisconnect()
{
  if (!CanControlCore())
    return;

  m_disconnected = !m_disconnected;
  WithCore(m_core, [disconnected = m_disconnected](HW::GBA::Core& gba) {
    gba.SetForceDisconnect(disconnected);
  });
}

void GBAWidget::LoadROM()
{
  if (!CanControlCore())
    return;

  const std::string rom = GameCubePane::GetOpenGBARom("");
  if (!rom.empty())
    SwapRom(m_core, rom);
}

void GBAWidget::UnloadROM()
{
  if (CanControlCore() && m_info.has_rom)
    SwapRom(m_core);
}

void GBAWidget::PromptForEReaderCards()
{
  const QStringList cards = DolphinFileDialog::getOpenFileNames(
      this, tr("Select e-Reader Cards"), QString(), tr("e-Reader Cards (*.raw);;All Files (*)"),
      nullptr, QFileDialog::Options());

  for (const QString& card : cards)
    InsertCard(m_core, QDir::toNativeSeparators(card).toStdString());
}

void GBAWidget::ResetCore()
{
  if (CanResetCore())
    Pad::SetGBAReset(m_pad, true);
}

void GBAWidget::DoState(bool export_state)
{
  // Exporting is harmless, so it is allowed even while the core is locked (movie, NetPlay).
  if (!export_state && !CanControlCore())
    return;

  const QString path = PickFile(this, export_state,
                                tr("mGBA Save States (*.ss0 *.ss1 *.ss2 *.ss3 *.ss4 "
                                   "*.ss5 *.ss6 *.ss7 *.ss8 *.ss9);;"
                                   "All Files (*)"));
  if (path.isEmpty())
    return;

  WithCore(m_core, [export_state, file = path.toStdString()](HW::GBA::Core& gba) {
    if (export_state)
      gba.ExportState(file);
    else
      gba.ImportState(file);
  });
}

void GBAWidget::ImportExportSave(bool export_save)
{
  if (!export_save && !CanControlCore())
    return;
  if (!m_info.has_rom)
    return;

  const QString path = PickFile(this, export_save,
                                tr("Save Game Files (*.sav);;"
                                   "All Files (*)"));
  if (path.isEmpty())
    return;

  WithCore(m_core, [export_save, file = path.toStdString()](HW::GBA::Core& gba) {
    if (export_save)
      gba.ExportSave(file);
    else
      gba.ImportSave(file);
  });
}

void GBAWidget::Resize(int scale)
{
  showNormal();
  const auto factor = static_cast<u32>(scale);
  resize(static_cast<int>(m_info.width * factor), static_cast<int>(m_info.height * factor));
}

bool GBAWidget::IsBorderless() const
{
  return windowFlags().testFlag(Qt::FramelessWindowHint) ||
         windowState().testFlag(Qt::WindowFullScreen);
}

void GBAWidget::SetBorderless(bool enable)
{
  const Qt::WindowStates state = windowState();

  // Maximized and full screen windows switch between those two states; normal windows toggle
  // their frame.
  if (state.testFlag(Qt::WindowFullScreen))
  {
    if (!enable)
      setWindowState((state ^ Qt::WindowFullScreen) | Qt::WindowMaximized);
    return;
  }
  if (state.testFlag(Qt::WindowMaximized))
  {
    if (enable)
      setWindowState((state ^ Qt::WindowMaximized) | Qt::WindowFullScreen);
    return;
  }
  if (windowFlags().testFlag(Qt::FramelessWindowHint) == enable)
    return;

  // Changing window flags recreates the window, so keep where it was.
  const QRect where = geometry();
  setWindowFlag(Qt::FramelessWindowHint, enable);
  setGeometry(where);
  show();
}

bool GBAWidget::IsAlwaysOnTop() const
{
  return windowFlags().testFlag(Qt::WindowStaysOnTopHint);
}

void GBAWidget::SetAlwaysOnTop(bool enable)
{
  if (IsAlwaysOnTop() == enable)
    return;
  setWindowFlag(Qt::WindowStaysOnTopHint, enable);
  show();
}

void GBAWidget::UpdateTitle()
{
  // "GBA<n>[ <player>][ | <game>] | Volume <v>%" (or "| Muted").
  std::string title = fmt::format("GBA{}", m_info.device_number + 1);
  if (!m_player_name.empty())
    title += " " + m_player_name;
  if (!m_info.game_title.empty())
    title += " | " + m_info.game_title;
  title += m_muted ? std::string(" | Muted") : fmt::format(" | Volume {}%", m_volume);

  setWindowTitle(QString::fromStdString(title));
}

void GBAWidget::ApplyVolume()
{
  // The mixer takes volumes from 0 to 256.
  const auto level = static_cast<u32>(m_muted ? 0 : m_volume * 256 / 100);
  auto& system = Core::System::GetInstance();
  system.GetSoundStream()->GetMixer()->SetGBAVolume(static_cast<size_t>(m_info.device_number),
                                                    level, level);
  UpdateTitle();
}

Qt::WindowFlags GBAWidget::LoadWindowFlags(int device_number)
{
  const QSettings& settings = Settings::GetQSettings();
  const QString key = SettingKey("flags", device_number);
  if (!settings.contains(key))
    return {};
  return Qt::WindowFlags{settings.value(key).toInt()};
}

void GBAWidget::RestoreSettings()
{
  const QSettings& settings = Settings::GetQSettings();

  const QString geometry_key = SettingKey("geometry", m_pad);
  if (settings.contains(geometry_key))
    restoreGeometry(settings.value(geometry_key).toByteArray());

  const QString blending_key = SettingKey("interframeblending", m_pad);
  if (settings.contains(blending_key))
    m_blend_frames = settings.value(blending_key).toBool();
}

void GBAWidget::StoreSettings()
{
  QSettings& settings = Settings::GetQSettings();
  settings.setValue(SettingKey("flags", m_pad), static_cast<int>(windowFlags()));
  settings.setValue(SettingKey("geometry", m_pad), saveGeometry());
  settings.setValue(SettingKey("interframeblending", m_pad), m_blend_frames);
}

bool GBAWidget::CanControlCore()
{
  // Movies and NetPlay need every GBA to stay exactly as it is.
  return !Core::System::GetInstance().GetMovie().IsMovieActive() && !NetPlay::IsNetPlayRunning();
}

bool GBAWidget::CanResetCore()
{
  return m_pad_is_local;
}

void GBAWidget::closeEvent(QCloseEvent* event)
{
  // The window lives as long as its GBA; it can't be closed by the user.
  event->ignore();
}

void GBAWidget::contextMenuEvent(QContextMenuEvent* event)
{
  auto* const menu = new QMenu(this);
  connect(menu, &QMenu::triggered, menu, &QMenu::deleteLater);

  const bool can_control = CanControlCore();
  const bool has_rom = m_info.has_rom;

  const auto add = [](QMenu* to, const QString& text, bool enabled = true) {
    auto* const action = new QAction(text, to);
    action->setEnabled(enabled);
    to->addAction(action);
    return action;
  };
  const auto add_toggle = [&add](QMenu* to, const QString& text, bool checked) {
    QAction* const action = add(to, text);
    action->setCheckable(true);
    action->setChecked(checked);
    return action;
  };

  QAction* const connected =
      add(menu, m_disconnected ? tr("Dis&connected") : tr("&Connected"), can_control);
  connected->setCheckable(true);
  connected->setChecked(!m_disconnected);
  connect(connected, &QAction::triggered, this, &GBAWidget::ToggleDisconnect);

  menu->addSeparator();
  connect(add(menu, tr("L&oad ROM..."), can_control), &QAction::triggered, this,
          &GBAWidget::LoadROM);
  connect(add(menu, tr("&Unload ROM"), can_control && has_rom), &QAction::triggered, this,
          &GBAWidget::UnloadROM);
  connect(add(menu, tr("&Scan e-Reader Card(s)..."), can_control && m_info.has_ereader),
          &QAction::triggered, this, &GBAWidget::PromptForEReaderCards);
  connect(add(menu, tr("&Reset"), CanResetCore()), &QAction::triggered, this,
          &GBAWidget::ResetCore);

  menu->addSeparator();
  // i18n: Noun (i.e. the data saved by the game)
  auto* const save_menu = new QMenu(tr("Save Game"), menu);
  connect(add(save_menu, tr("&Import Save Game..."), can_control && has_rom), &QAction::triggered,
          this, [this] { ImportExportSave(false); });
  connect(add(save_menu, tr("&Export Save Game..."), has_rom), &QAction::triggered, this,
          [this] { ImportExportSave(true); });
  menu->addMenu(save_menu);

  auto* const state_menu = new QMenu(tr("Save State"), menu);
  connect(add(state_menu, tr("&Import State..."), can_control), &QAction::triggered, this,
          [this] { DoState(false); });
  connect(add(state_menu, tr("&Export State...")), &QAction::triggered, this,
          [this] { DoState(true); });
  menu->addMenu(state_menu);

  menu->addSeparator();
  connect(add_toggle(menu, tr("&Mute"), m_muted), &QAction::triggered, this,
          &GBAWidget::ToggleMute);

  menu->addSeparator();
  auto* const options_menu = new QMenu(tr("Options"), menu);
  auto* const size_menu = new QMenu(tr("Window Size"), options_menu);
  const QString size_labels[] = {tr("&1x"), tr("&2x"), tr("&3x"), tr("&4x")};
  for (int scale = 1; scale <= 4; ++scale)
  {
    connect(add(size_menu, size_labels[scale - 1]), &QAction::triggered, this,
            [this, scale] { Resize(scale); });
  }
  options_menu->addMenu(size_menu);
  options_menu->addSeparator();
  connect(add_toggle(options_menu, tr("&Borderless Window"), IsBorderless()), &QAction::triggered,
          this, [this] { SetBorderless(!IsBorderless()); });
  connect(add_toggle(options_menu, tr("Always on &Top"), IsAlwaysOnTop()), &QAction::triggered,
          this, [this] { SetAlwaysOnTop(!IsAlwaysOnTop()); });
  connect(add_toggle(options_menu, tr("&Interframe Blending"), m_blend_frames),
          &QAction::triggered, this, [this] { m_blend_frames = !m_blend_frames; });
  menu->addMenu(options_menu);

  menu->move(event->globalPos());
  menu->show();
}

void GBAWidget::mouseDoubleClickEvent(QMouseEvent*)
{
  SetBorderless(!IsBorderless());
}

void GBAWidget::mousePressEvent(QMouseEvent* event)
{
  // Borderless windows are moved by dragging them with the left button.
  if (event->button() != Qt::MouseButton::LeftButton ||
      !windowFlags().testFlag(Qt::FramelessWindowHint))
  {
    return;
  }
  m_dragging = true;
  m_drag_offset = event->pos();
}

void GBAWidget::mouseReleaseEvent(QMouseEvent* event)
{
  if (event->button() == Qt::MouseButton::LeftButton)
    m_dragging = false;
}

void GBAWidget::mouseMoveEvent(QMouseEvent* event)
{
  if (!m_dragging)
    return;

  // pos() includes the frame while geometry() doesn't; subtract the difference.
  const QPoint frame_offset = geometry().topLeft() - pos();
  move(event->globalPosition().toPoint() - m_drag_offset - frame_offset);
}

QRect GBAWidget::ScreenRect() const
{
  const int screen_width = static_cast<int>(m_info.width);
  const int screen_height = static_cast<int>(m_info.height);
  if (size() == QSize(screen_width, screen_height))
    return QRect(0, 0, screen_width, screen_height);

  // Scale to fit while keeping the aspect ratio, centred, with black bars on the other axis.
  const bool wider_than_window =
      static_cast<float>(m_info.width) / static_cast<float>(m_info.height) >
      static_cast<float>(width()) / static_cast<float>(height());
  if (wider_than_window)
  {
    const int scaled_height =
        static_cast<int>(static_cast<u32>(width()) * m_info.height / m_info.width);
    return QRect(0, (height() - scaled_height) / 2, width(), scaled_height);
  }
  const int scaled_width =
      static_cast<int>(static_cast<u32>(height()) * m_info.width / m_info.height);
  return QRect((width() - scaled_width) / 2, 0, scaled_width, height());
}

void GBAWidget::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.fillRect(QRect(QPoint(), size()), Qt::black);

  const QRect source(0, 0, static_cast<int>(m_info.width), static_cast<int>(m_info.height));
  const QRect target = ScreenRect();

  // Interframe blending: the previous frame at full strength, the new one at half on top.
  if (m_blend_frames && m_previous_frame.size() == source.size())
  {
    painter.drawImage(target, m_previous_frame, source);
    painter.setOpacity(0.5);
  }
  if (m_frame.size() == source.size())
    painter.drawImage(target, m_frame, source);
}

void GBAWidget::dragEnterEvent(QDragEnterEvent* event)
{
  if (CanControlCore() && event->mimeData()->hasUrls())
    event->acceptProposedAction();
}

void GBAWidget::dropEvent(QDropEvent* event)
{
  if (!CanControlCore())
    return;

  // A dropped .raw file is an e-Reader card; anything else is loaded as a ROM.
  for (const QUrl& url : event->mimeData()->urls())
  {
    const QFileInfo file(url.toLocalFile());
    const QString path = file.filePath();

    if (!file.isFile())
      continue;
    if (!file.exists() || !file.isReadable())
    {
      ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
      continue;
    }

    if (file.suffix() == QStringLiteral("raw"))
      InsertCard(m_core, path.toStdString());
    else
      SwapRom(m_core, path.toStdString());
  }
}

GBAWidgetController::~GBAWidgetController()
{
  m_widget->deleteLater();
}

void GBAWidgetController::Create(std::weak_ptr<HW::GBA::Core> core, const HW::GBA::CoreInfo& info)
{
  m_widget = new GBAWidget(std::move(core), info, NetPlayPadOf(info.device_number));
}

void GBAWidgetController::GameChanged(const HW::GBA::CoreInfo& new_info)
{
  m_widget->GameChanged(new_info);
}

void GBAWidgetController::FrameEnded(std::span<const u32> frame)
{
  m_widget->SetVideoBuffer(frame);
}
#endif  // HAS_LIBMGBA
