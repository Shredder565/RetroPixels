// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/OnScreenDisplayPane.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"

namespace
{
// A titled group box whose contents are laid out on a grid.
QGroupBox* MakeGroup(const QString& title, QGridLayout** grid)
{
  auto* const group = new QGroupBox(title);
  *grid = new QGridLayout();
  group->setLayout(*grid);
  return group;
}
}  // namespace

OnScreenDisplayPane::OnScreenDisplayPane(QWidget* parent) : QWidget(parent)
{
  auto* const column = new QVBoxLayout;
  column->addWidget(BuildGeneral());
  column->addWidget(BuildPerformance());
  column->addWidget(BuildMovie());
  column->addWidget(BuildNetPlay());
  column->addWidget(BuildDebug());
  column->addStretch();
  setLayout(column);

  // The detail options are greyed out while the movie window itself is off.
  const auto sync_details = [this] {
    const bool shown = m_movie_window->isChecked();
    for (ConfigBool* const detail : m_movie_details)
      detail->setEnabled(shown);
  };
  sync_details();
  connect(m_movie_window, &QCheckBox::toggled, this, sync_details);
}

ConfigBool* OnScreenDisplayPane::AddToggle(QGridLayout* grid, int row, int column,
                                           const QString& label, const Config::Info<bool>& setting,
                                           const QString& description)
{
  auto* const toggle = new ConfigBool(label, setting);
  toggle->SetDescription(description);
  grid->addWidget(toggle, row, column);
  return toggle;
}

QGroupBox* OnScreenDisplayPane::BuildGeneral()
{
  QGridLayout* grid;
  QGroupBox* const group = MakeGroup(tr("General"), &grid);

  AddToggle(grid, 0, 0, tr("Show Messages"), Config::MAIN_OSD_MESSAGES,
            tr("Shows on-screen display messages over the render window. These messages disappear "
               "after several seconds.<br><br><dolphin_emphasis>If unsure, leave this "
               "checked.</dolphin_emphasis>"));

  auto* const font_size = new ConfigInteger(12, 40, Config::MAIN_OSD_FONT_SIZE);
  font_size->SetTitle(tr("Font Size"));
  font_size->SetDescription(
      tr("Changes the font size of the On-Screen Display. Affects features such as "
         "performance statistics, frame counter, and netplay chat.<br><br>The font can be "
         "changed by placing a TTF font file into Dolphin's User/Load folder, and renaming it "
         "OSD_Font.ttf.<br><br><dolphin_emphasis>If unsure, leave this at "
         "13.</dolphin_emphasis>"));
  grid->addWidget(new QLabel(tr("Font Size:")), 1, 0);
  grid->addWidget(font_size, 1, 1);

  return group;
}

QGroupBox* OnScreenDisplayPane::BuildPerformance()
{
  QGridLayout* grid;
  QGroupBox* const group = MakeGroup(tr("Performance Statistics"), &grid);

  AddToggle(grid, 0, 0, tr("Show FPS"), Config::GFX_SHOW_FPS,
            tr("Shows the number of distinct frames rendered per second as a measure of visual "
               "smoothness.<br><br><dolphin_emphasis>If unsure, leave this "
               "unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 0, 1, tr("Show Frame Times"), Config::GFX_SHOW_FTIMES,
            tr("Shows the average time in ms between each distinct rendered frame alongside the "
               "standard deviation.<br><br><dolphin_emphasis>If unsure, leave this "
               "unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 1, 0, tr("Show VPS"), Config::GFX_SHOW_VPS,
            tr("Shows the number of frames rendered per second as a measure of emulation "
               "speed.<br><br><dolphin_emphasis>If unsure, leave this "
               "unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 1, 1, tr("Show VBlank Times"), Config::GFX_SHOW_VTIMES,
            tr("Shows the average time in ms between each rendered frame alongside the standard "
               "deviation.<br><br><dolphin_emphasis>If unsure, leave this "
               "unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 2, 0, tr("Show % Speed"), Config::GFX_SHOW_SPEED,
            tr("Shows the % speed of emulation compared to full speed.<br><br><dolphin_emphasis>If "
               "unsure, leave this unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 2, 1, tr("Show Performance Graphs"), Config::GFX_SHOW_GRAPHS,
            tr("Shows frametime graph along with statistics as a representation of emulation "
               "performance.<br><br><dolphin_emphasis>If unsure, leave this "
               "unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 3, 0, tr("Show Speed Colors"), Config::GFX_SHOW_SPEED_COLORS,
            tr("Changes the color of the FPS counter depending on emulation "
               "speed.<br><br><dolphin_emphasis>If unsure, leave this "
               "checked.</dolphin_emphasis>"));

  auto* const sample_window = new ConfigInteger(0, 10000, Config::GFX_PERF_SAMP_WINDOW, 100);
  sample_window->SetTitle(tr("Performance Sample Window (ms)"));
  sample_window->SetDescription(
      tr("The amount of time the FPS and VPS counters will sample over.<br><br>The higher the "
         "value, the more stable the FPS/VPS counter will be, but the slower it will be to "
         "update.<br><br><dolphin_emphasis>If unsure, leave this at "
         "1000ms.</dolphin_emphasis>"));
  grid->addWidget(new QLabel(tr("Performance Sample Window (ms):")), 4, 0);
  grid->addWidget(sample_window, 4, 1);

  return group;
}

QGroupBox* OnScreenDisplayPane::BuildMovie()
{
  QGridLayout* grid;
  QGroupBox* const group = MakeGroup(tr("Movie Window"), &grid);

  m_movie_window = AddToggle(
      grid, 0, 0, tr("Show Movie Window"), Config::MAIN_MOVIE_SHOW_OSD,
      tr("Shows a window that can be filled with information related to movie recordings. The "
         "other options in this group determine what appears in the window. "
         "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>"));

  m_movie_details = {
      AddToggle(grid, 1, 0, tr("Show Rerecord Counter"), Config::MAIN_MOVIE_SHOW_RERECORD,
                tr("Shows how many times the input recording has been overwritten by using "
                   "savestates.<br><br><dolphin_emphasis>If unsure, leave this "
                   "unchecked.</dolphin_emphasis>")),
      AddToggle(grid, 1, 1, tr("Show Lag Counter"), Config::MAIN_SHOW_LAG,
                tr("Shows how many frames have occurred without controller inputs being "
                   "checked. Resets to 1 when inputs are processed. "
                   "<br><br><dolphin_emphasis>If unsure, leave this "
                   "unchecked.</dolphin_emphasis>")),
      AddToggle(grid, 2, 0, tr("Show Frame Counter"), Config::MAIN_SHOW_FRAME_COUNT,
                tr("Shows how many frames have passed. <br><br><dolphin_emphasis>If unsure, "
                   "leave this unchecked.</dolphin_emphasis>")),
      AddToggle(grid, 2, 1, tr("Show Input Display"), Config::MAIN_MOVIE_SHOW_INPUT_DISPLAY,
                tr("Shows the controls currently being input.<br><br><dolphin_emphasis>If "
                   "unsure, leave this unchecked.</dolphin_emphasis>")),
      AddToggle(grid, 3, 0, tr("Show System Clock"), Config::MAIN_MOVIE_SHOW_RTC,
                tr("Shows current system time.<br><br><dolphin_emphasis>If unsure, leave this "
                   "unchecked.</dolphin_emphasis>")),
  };

  return group;
}

QGroupBox* OnScreenDisplayPane::BuildNetPlay()
{
  QGridLayout* grid;
  QGroupBox* const group = MakeGroup(tr("Netplay"), &grid);

  AddToggle(grid, 0, 0, tr("Show NetPlay Ping"), Config::GFX_SHOW_NETPLAY_PING,
            tr("Shows the player's maximum ping while playing on NetPlay.<br><br>"
               "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 0, 1, tr("Show NetPlay Chat"), Config::GFX_SHOW_NETPLAY_MESSAGES,
            tr("Shows chat messages, buffer changes, and desync alerts while playing "
               "NetPlay.<br><br><dolphin_emphasis>If unsure, leave this "
               "unchecked.</dolphin_emphasis>"));

  return group;
}

QGroupBox* OnScreenDisplayPane::BuildDebug()
{
  QGridLayout* grid;
  QGroupBox* const group = MakeGroup(tr("Debug"), &grid);

  AddToggle(grid, 0, 0, tr("Show Statistics"), Config::GFX_OVERLAY_STATS,
            tr("Shows various rendering statistics.<br><br><dolphin_emphasis>If unsure, leave "
               "this unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 0, 1, tr("Show Projection Statistics"), Config::GFX_OVERLAY_PROJ_STATS,
            tr("Shows various projection statistics.<br><br><dolphin_emphasis>If unsure, leave "
               "this unchecked.</dolphin_emphasis>"));
  AddToggle(grid, 1, 0, tr("Show XFB Resolution"), Config::GFX_SHOW_INTERNAL_RESOLUTION,
            tr("Shows the size of the emulated external frame buffer (XFB) in pixels, as a "
               "product of width and height.<br><br><dolphin_emphasis>If unsure, leave this "
               "unchecked.</dolphin_emphasis>"));

  return group;
}
