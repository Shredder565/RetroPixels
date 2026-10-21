// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/FIFO/FIFOPlayerWindow.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/System.h"

#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"
#include "DolphinQt/FIFO/FIFOAnalyzer.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
const QString GEOMETRY_KEY = QStringLiteral("fifoplayerwindow/geometry");
}

FIFOPlayerWindow::FIFOPlayerWindow(FifoPlayer& fifo_player, FifoRecorder& fifo_recorder,
                                   QWidget* parent)
    : QWidget(parent), m_fifo_player(fifo_player), m_fifo_recorder(fifo_recorder)
{
  setWindowTitle(tr("FIFO Player"));
  setWindowIcon(Resources::GetAppIcon());

  BuildLayout();
  RestoreOptions();
  ConnectSignals();
  SetDescriptions();

  UpdateInfo();
  UpdateControls();

  // The player reports from the CPU thread; handle it on the UI thread.
  m_fifo_player.SetFileLoadedCallback(
      [this] { QueueOnObject(this, &FIFOPlayerWindow::OnFIFOLoaded); });
  m_fifo_player.SetFrameWrittenCallback([this] {
    QueueOnObject(this, [this] {
      UpdateInfo();
      UpdateControls();
    });
  });

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &FIFOPlayerWindow::OnEmulationStateChanged);

  installEventFilter(this);
}

FIFOPlayerWindow::~FIFOPlayerWindow()
{
  Settings::GetQSettings().setValue(GEOMETRY_KEY, saveGeometry());

  m_fifo_player.SetFileLoadedCallback({});
  m_fifo_player.SetFrameWrittenCallback({});
}

QGroupBox* FIFOPlayerWindow::BuildRangeGroup(const QString& title, QSpinBox*& from,
                                             QLabel*& from_label, QSpinBox*& to, QLabel*& to_label)
{
  auto* group = new QGroupBox(title);
  auto* row = new QHBoxLayout;

  from = new QSpinBox;
  from_label = new QLabel(tr("From:"));
  to = new QSpinBox;
  to_label = new QLabel(tr("To:"));

  row->addWidget(from_label);
  row->addWidget(from);
  row->addWidget(to_label);
  row->addWidget(to);
  group->setLayout(row);
  return group;
}

void FIFOPlayerWindow::BuildLayout()
{
  auto* page = new QVBoxLayout;

  // Summary of the loaded or recorded log, three lines tall.
  auto* info_group = new QGroupBox(tr("File Info"));
  auto* info_row = new QHBoxLayout;
  m_info_label = new QLabel;
  info_row->addWidget(m_info_label);
  info_group->setLayout(info_row);
  m_info_label->setFixedHeight(QFontMetrics(font()).lineSpacing() * 3);

  // Playback: which objects and frames to play, and how.
  QGroupBox* objects =
      BuildRangeGroup(tr("Object Range"), m_object_range_from, m_object_range_from_label,
                      m_object_range_to, m_object_range_to_label);
  QGroupBox* frames =
      BuildRangeGroup(tr("Frame Range"), m_frame_range_from, m_frame_range_from_label,
                      m_frame_range_to, m_frame_range_to_label);

  auto* playback_group = new QGroupBox(tr("Playback Options"));
  auto* playback_grid = new QGridLayout;
  m_early_memory_updates = new ToolTipCheckBox(tr("Early Memory Updates"));
  m_loop = new ToolTipCheckBox(tr("Loop"));
  playback_grid->addWidget(objects, 0, 0);
  playback_grid->addWidget(frames, 0, 1);
  playback_grid->addWidget(m_early_memory_updates, 1, 0);
  playback_grid->addWidget(m_loop, 1, 1);
  playback_group->setLayout(playback_grid);

  // Recording: how many frames to capture (one minute at most, three by default).
  auto* recording_group = new QGroupBox(tr("Recording Options"));
  auto* recording_row = new QHBoxLayout;
  m_frame_record_count = new QSpinBox;
  m_frame_record_count_label = new QLabel(tr("Frames to Record:"));
  m_frame_record_count->setMinimum(1);
  m_frame_record_count->setMaximum(3600);
  m_frame_record_count->setValue(3);
  recording_row->addWidget(m_frame_record_count_label);
  recording_row->addWidget(m_frame_record_count);
  recording_group->setLayout(recording_row);

  m_button_box = new QDialogButtonBox(QDialogButtonBox::Close);
  m_load = m_button_box->addButton(tr("Load..."), QDialogButtonBox::ActionRole);
  m_save = m_button_box->addButton(tr("Save..."), QDialogButtonBox::ActionRole);
  m_record = m_button_box->addButton(tr("Record"), QDialogButtonBox::ActionRole);
  m_stop = m_button_box->addButton(tr("Stop"), QDialogButtonBox::ActionRole);

  page->addWidget(info_group);
  page->addWidget(playback_group);
  page->addWidget(recording_group);
  page->addStretch();
  page->addWidget(m_button_box);

  m_main_widget = new QWidget(this);
  m_main_widget->setLayout(page);

  m_tab_widget = new QTabWidget(this);
  m_analyzer = new FIFOAnalyzer(m_fifo_player);
  m_tab_widget->addTab(m_main_widget, tr("Play / Record"));
  m_tab_widget->addTab(m_analyzer, tr("Analyze"));

  auto* outer = new QVBoxLayout;
  outer->addWidget(m_tab_widget);
  setLayout(outer);
}

void FIFOPlayerWindow::RestoreOptions()
{
  restoreGeometry(Settings::GetQSettings().value(GEOMETRY_KEY).toByteArray());

  m_early_memory_updates->setChecked(Config::Get(Config::MAIN_FIFOPLAYER_EARLY_MEMORY_UPDATES));
  m_loop->setChecked(Config::Get(Config::MAIN_FIFOPLAYER_LOOP_REPLAY));
}

void FIFOPlayerWindow::ConnectSignals()
{
  connect(m_load, &QPushButton::clicked, this, &FIFOPlayerWindow::LoadRecording);
  connect(m_save, &QPushButton::clicked, this, &FIFOPlayerWindow::SaveRecording);
  connect(m_record, &QPushButton::clicked, this, &FIFOPlayerWindow::StartRecording);
  connect(m_stop, &QPushButton::clicked, this, &FIFOPlayerWindow::StopRecording);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &FIFOPlayerWindow::hide);

  for (ToolTipCheckBox* option : {m_early_memory_updates, m_loop})
    connect(option, &QCheckBox::toggled, this, &FIFOPlayerWindow::OnConfigChanged);

  for (QSpinBox* limit :
       {m_frame_range_from, m_frame_range_to, m_object_range_from, m_object_range_to})
  {
    connect(limit, &QSpinBox::valueChanged, this, &FIFOPlayerWindow::OnLimitsChanged);
  }
}

void FIFOPlayerWindow::SetDescriptions()
{
  static const char EARLY_UPDATES_HELP[] = QT_TR_NOOP(
      "If enabled, then all memory updates happen at once before the first frame.<br><br>"
      "Causes issues with many fifologs, but can be useful for testing.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char LOOP_HELP[] =
      QT_TR_NOOP("If unchecked, then playback of the fifolog stops after the final frame.<br><br>"
                 "This is generally only useful when a frame-dumping option is enabled.<br><br>"
                 "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");

  m_early_memory_updates->SetDescription(tr(EARLY_UPDATES_HELP));
  m_loop->SetDescription(tr(LOOP_HELP));
}

void FIFOPlayerWindow::OnEmulationStateChanged(Core::State state)
{
  // Pausing and resuming make no difference here, so Paused counts as Running; that also keeps
  // a resume from looking like a fresh start.
  const Core::State effective = state == Core::State::Paused ? Core::State::Running : state;
  if (effective == m_emu_state)
    return;

  UpdateControls();

  if (effective == Core::State::Running)
    OnEmulationStarted();
  else if (effective == Core::State::Uninitialized)
    OnEmulationStopped();

  m_emu_state = effective;
}

void FIFOPlayerWindow::LoadRecording()
{
  const QString path = DolphinFileDialog::getOpenFileName(this, tr("Open FIFO Log"), QString(),
                                                          tr("Dolphin FIFO Log (*.dff)"));
  if (!path.isEmpty())
    emit LoadFIFORequested(path);
}

void FIFOPlayerWindow::SaveRecording()
{
  const QString path = DolphinFileDialog::getSaveFileName(this, tr("Save FIFO Log"), QString(),
                                                          tr("Dolphin FIFO Log (*.dff)"));
  if (path.isEmpty())
    return;

  if (!m_fifo_recorder.GetRecordedFile()->Save(path.toStdString()))
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to save FIFO log."));
}

void FIFOPlayerWindow::StartRecording()
{
  // The recorder finishes on the video thread; the window catches up on the UI thread.
  m_fifo_recorder.StartRecording(m_frame_record_count->value(),
                                 [this] { QueueOnObject(this, [this] { OnRecordingDone(); }); });

  UpdateControls();
  UpdateInfo();
}

void FIFOPlayerWindow::StopRecording()
{
  m_fifo_recorder.StopRecording();

  UpdateControls();
  UpdateInfo();
}

void FIFOPlayerWindow::OnEmulationStarted()
{
  // A log loaded before the start gets its ranges and analysis now.
  if (m_fifo_player.GetFile())
    OnFIFOLoaded();
}

void FIFOPlayerWindow::OnEmulationStopped()
{
  if (m_fifo_recorder.IsRecording())
    StopRecording();

  // Nothing is left to analyze once emulation stops.
  m_tab_widget->setCurrentWidget(m_main_widget);
  m_analyzer->Update();
}

void FIFOPlayerWindow::OnRecordingDone()
{
  UpdateInfo();
  UpdateControls();
}

void FIFOPlayerWindow::UpdateInfo()
{
  if (m_fifo_player.IsPlaying())
  {
    const FifoDataFile* file = m_fifo_player.GetFile();
    m_info_label->setText(tr("%1 frame(s)\n%2 object(s)\nCurrent Frame: %3")
                              .arg(QString::number(file->GetFrameCount()),
                                   QString::number(m_fifo_player.GetCurrentFrameObjectCount()),
                                   QString::number(m_fifo_player.GetCurrentFrameNum())));
    return;
  }

  if (m_fifo_recorder.IsRecordingDone())
  {
    // Totals over the whole recording.
    const FifoDataFile* file = m_fifo_recorder.GetRecordedFile();
    size_t command_bytes = 0;
    size_t memory_bytes = 0;
    for (u32 frame = 0; frame < file->GetFrameCount(); ++frame)
    {
      const FifoFrameInfo& info = file->GetFrame(frame);
      command_bytes += info.fifoData.size();
      for (const MemoryUpdate& update : info.memoryUpdates)
        memory_bytes += update.data.size();
    }

    m_info_label->setText(tr("%1 FIFO bytes\n%2 memory bytes\n%3 frames")
                              .arg(QString::number(command_bytes), QString::number(memory_bytes),
                                   QString::number(file->GetFrameCount())));
    return;
  }

  const bool recording =
      Core::IsRunning(Core::System::GetInstance()) && m_fifo_recorder.IsRecording();
  m_info_label->setText(recording ? tr("Recording...") : tr("No file loaded / recorded."));
}

void FIFOPlayerWindow::OnFIFOLoaded()
{
  const FifoDataFile* file = m_fifo_player.GetFile();

  const u32 object_count = m_fifo_player.GetMaxObjectCount();
  const u32 frame_count = file->GetFrameCount();
  const int last_frame = static_cast<int>(frame_count - 1);
  const int last_object = static_cast<int>(object_count - 1);

  // Start out playing everything.
  m_frame_range_to->setMaximum(last_frame);
  m_object_range_to->setMaximum(last_object);
  m_frame_range_from->setValue(0);
  m_object_range_from->setValue(0);
  m_frame_range_to->setValue(last_frame);
  m_object_range_to->setValue(last_object);

  UpdateInfo();
  UpdateLimits();
  UpdateControls();

  m_analyzer->Update();
}

void FIFOPlayerWindow::OnConfigChanged()
{
  Config::SetBase(Config::MAIN_FIFOPLAYER_EARLY_MEMORY_UPDATES,
                  m_early_memory_updates->isChecked());
  Config::SetBase(Config::MAIN_FIFOPLAYER_LOOP_REPLAY, m_loop->isChecked());
}

void FIFOPlayerWindow::OnLimitsChanged()
{
  m_fifo_player.SetFrameRangeStart(static_cast<u32>(m_frame_range_from->value()));
  m_fifo_player.SetFrameRangeEnd(static_cast<u32>(m_frame_range_to->value()));
  m_fifo_player.SetObjectRangeStart(static_cast<u32>(m_object_range_from->value()));
  m_fifo_player.SetObjectRangeEnd(static_cast<u32>(m_object_range_to->value()));
  UpdateLimits();
}

void FIFOPlayerWindow::UpdateLimits()
{
  // Neither end of a range may pass the other.
  m_frame_range_from->setMaximum(m_frame_range_to->value());
  m_frame_range_to->setMinimum(m_frame_range_from->value());
  m_object_range_from->setMaximum(m_object_range_to->value());
  m_object_range_to->setMinimum(m_object_range_from->value());
}

void FIFOPlayerWindow::UpdateControls()
{
  Core::System& system = Core::System::GetInstance();
  const bool stopped = Core::IsUninitialized(system);
  const bool running = Core::IsRunning(system);
  const bool recording = m_fifo_recorder.IsRecording();
  const bool playing = m_fifo_player.IsPlaying();

  // The playback ranges only mean something while a log plays.
  for (QWidget* range_part :
       {static_cast<QWidget*>(m_frame_range_from), static_cast<QWidget*>(m_frame_range_from_label),
        static_cast<QWidget*>(m_frame_range_to), static_cast<QWidget*>(m_frame_range_to_label),
        static_cast<QWidget*>(m_object_range_from),
        static_cast<QWidget*>(m_object_range_from_label), static_cast<QWidget*>(m_object_range_to),
        static_cast<QWidget*>(m_object_range_to_label)})
  {
    range_part->setEnabled(playing);
  }

  const bool can_pick_frame_count = !playing && !recording;
  m_frame_record_count_label->setEnabled(can_pick_frame_count);
  m_frame_record_count->setEnabled(can_pick_frame_count);

  m_load->setEnabled(stopped);
  m_record->setEnabled(running && !playing);

  // Record and Stop share a spot: Stop shows while a recording runs.
  m_stop->setVisible(running && recording);
  m_record->setVisible(!m_stop->isVisible());

  m_save->setEnabled(m_fifo_recorder.IsRecordingDone());
}

bool FIFOPlayerWindow::eventFilter(QObject*, QEvent* event)
{
  // Escape hides the window; the key is never swallowed.
  if (event->type() == QEvent::KeyPress &&
      static_cast<QKeyEvent*>(event)->matches(QKeySequence::Cancel))
  {
    hide();
  }
  return false;
}
