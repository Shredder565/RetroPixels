// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/VerifyWidget.h"

#include <future>
#include <memory>
#include <optional>
#include <span>
#include <tuple>

#include <QByteArray>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeVerifier.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/Settings.h"

namespace
{
// Progress is counted in units of this many bytes so it fits in an int.
constexpr u64 BYTES_PER_STEP = 0x100;

void ShowHex(QLineEdit* field, std::span<const u8> digest)
{
  const QByteArray raw = QByteArray::fromRawData(reinterpret_cast<const char*>(digest.data()),
                                                 static_cast<int>(digest.size()));
  field->setText(QString::fromLatin1(raw.toHex()));
}
}  // namespace

VerifyWidget::VerifyWidget(std::shared_ptr<DiscIO::Volume> volume) : m_volume(std::move(volume))
{
  auto* const column = new QVBoxLayout;

  m_problems = new QTableWidget(0, 2, this);
  m_problems->setTabKeyNavigation(false);
  m_problems->setHorizontalHeaderLabels({tr("Problem"), tr("Severity")});
  QHeaderView* const top = m_problems->horizontalHeader();
  top->setSectionResizeMode(0, QHeaderView::Stretch);
  top->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  top->setHighlightSections(false);
  m_problems->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_problems->verticalHeader()->hide();

  m_summary = new QTextEdit(this);
  m_summary->setReadOnly(true);

  m_hash_form = new QFormLayout;
  std::tie(m_crc32_box, m_crc32_field) = AddHashRow(m_hash_form, tr("CRC32:"));
  std::tie(m_md5_box, m_md5_field) = AddHashRow(m_hash_form, tr("MD5:"));
  std::tie(m_sha1_box, m_sha1_field) = AddHashRow(m_hash_form, tr("SHA-1:"));

  const auto defaults = DiscIO::VolumeVerifier::GetDefaultHashesToCalculate();
  m_crc32_box->setChecked(defaults.crc32);
  m_md5_box->setChecked(defaults.md5);
  m_sha1_box->setChecked(defaults.sha1);

  // Only discs can be checked against Redump.
  m_redump_form = new QFormLayout;
  m_redump_box = nullptr;
  m_redump_field = nullptr;
  if (DiscIO::IsDisc(m_volume->GetVolumeType()))
  {
    std::tie(m_redump_box, m_redump_field) =
        AddHashRow(m_redump_form, tr("Redump.info Status:"));
    m_redump_box->setChecked(RedumpAllowed());
    RefreshRedumpBox();
  }

  // Let the hash fields take the full width (macOS doesn't by default).
  m_hash_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  m_redump_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  m_verify_button = new QPushButton(tr("Verify Integrity"), this);

  connect(m_verify_button, &QPushButton::clicked, this, &VerifyWidget::RunVerification);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(m_md5_box, &QCheckBox::checkStateChanged, this, &VerifyWidget::RefreshRedumpBox);
  connect(m_sha1_box, &QCheckBox::checkStateChanged, this, &VerifyWidget::RefreshRedumpBox);
#else
  connect(m_md5_box, &QCheckBox::stateChanged, this, &VerifyWidget::RefreshRedumpBox);
  connect(m_sha1_box, &QCheckBox::stateChanged, this, &VerifyWidget::RefreshRedumpBox);
#endif

  column->addWidget(m_problems);
  column->addWidget(m_summary);
  column->addLayout(m_hash_form);
  column->addLayout(m_redump_form);
  column->addWidget(m_verify_button);
  column->setStretchFactor(m_problems, 5);
  column->setStretchFactor(m_summary, 2);
  setLayout(column);

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &VerifyWidget::OnEmulationStateChanged);
  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

std::pair<QCheckBox*, QLineEdit*> VerifyWidget::AddHashRow(QFormLayout* form,
                                                           const QString& caption)
{
  auto* const field = new QLineEdit(this);
  field->setReadOnly(true);
  auto* const calculate = new QCheckBox(tr("Calculate"), this);

  auto* const row = new QHBoxLayout;
  row->addWidget(field);
  row->addWidget(calculate);
  form->addRow(caption, row);

  return {calculate, field};
}

void VerifyWidget::OnEmulationStateChanged(Core::State state)
{
  // Verifying a Wii disc starts its own IOS instance, which can't coexist
  // with a running game.
  m_verify_button->setEnabled(state == Core::State::Uninitialized);
}

bool VerifyWidget::RedumpAllowed() const
{
  // CRC32 alone is too easy to forge to compare against Redump.
  return m_md5_box->isChecked() || m_sha1_box->isChecked();
}

void VerifyWidget::RefreshRedumpBox()
{
  if (m_redump_box != nullptr)
    m_redump_box->setEnabled(RedumpAllowed());
}

void VerifyWidget::RunVerification()
{
  const bool check_redump = RedumpAllowed() && m_redump_box && m_redump_box->isChecked();

  DiscIO::VolumeVerifier verifier(
      *m_volume, check_redump, {m_crc32_box->isChecked(), m_md5_box->isChecked(), m_sha1_box->isChecked()});

  ParallelProgressDialog progress(tr("Verifying"), tr("Cancel"), 0,
                                  static_cast<int>(verifier.GetTotalBytes() / BYTES_PER_STEP),
                                  this);
  progress.GetRaw()->setWindowTitle(tr("Verifying"));
  progress.GetRaw()->setMinimumDuration(500);
  progress.GetRaw()->setWindowModality(Qt::WindowModal);

  // The verifier runs on a worker; the dialog's event loop runs here until
  // the worker resets it. A cancelled run yields no result.
  using Outcome = std::optional<DiscIO::VolumeVerifier::Result>;
  std::future<Outcome> worker = std::async(std::launch::async, [&verifier, &progress]() -> Outcome {
    progress.SetValue(0);
    verifier.Start();
    for (;;)
    {
      const u64 done = verifier.GetBytesProcessed();
      if (done == verifier.GetTotalBytes())
        break;
      progress.SetValue(static_cast<int>(done / BYTES_PER_STEP));
      if (progress.WasCanceled())
        return std::nullopt;
      verifier.Process();
    }
    verifier.Finish();
    progress.Reset();
    return verifier.GetResult();
  });
  progress.GetRaw()->exec();

  const Outcome outcome = worker.get();
  if (!outcome)
    return;
  const DiscIO::VolumeVerifier::Result& result = *outcome;

  m_summary->setText(QString::fromStdString(result.summary_text));

  m_problems->setRowCount(static_cast<int>(result.problems.size()));
  for (int row = 0; row < m_problems->rowCount(); ++row)
  {
    const DiscIO::VolumeVerifier::Problem& problem = result.problems[static_cast<size_t>(row)];

    QString severity;
    using Severity = DiscIO::VolumeVerifier::Severity;
    if (problem.severity == Severity::Low)
      severity = tr("Low");
    else if (problem.severity == Severity::Medium)
      severity = tr("Medium");
    else if (problem.severity == Severity::High)
      severity = tr("High");

    PutProblemCell(row, 0, QString::fromStdString(problem.text));
    PutProblemCell(row, 1, severity);
  }

  ShowHex(m_crc32_field, result.hashes.crc32);
  ShowHex(m_md5_field, result.hashes.md5);
  ShowHex(m_sha1_field, result.hashes.sha1);

  if (m_redump_field != nullptr)
    m_redump_field->setText(QString::fromStdString(result.redump.message));
}

void VerifyWidget::PutProblemCell(int row, int column, const QString& text)
{
  // Labels rather than items, so long texts wrap and can be selected.
  auto* const cell = new QLabel(text);
  cell->setTextInteractionFlags(Qt::TextSelectableByMouse);
  cell->setWordWrap(true);
  cell->setMargin(4);
  m_problems->setCellWidget(row, column, cell);
}
