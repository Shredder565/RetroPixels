// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/GCMemcardCreateNewDialog.h"

#include <utility>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>

#include "Common/FileUtil.h"
#include "Common/Timer.h"

#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/Sram.h"

#include "DolphinQt/QtUtils/DolphinFileDialog.h"

namespace
{
// Index of the 128 Mbit entry, the default size.
constexpr int DEFAULT_SIZE_INDEX = 5;
}  // namespace

GCMemcardCreateNewDialog::GCMemcardCreateNewDialog(QWidget* parent) : QDialog(parent)
{
  // Card sizes in megabits; the block counts are what a formatted card of that size offers.
  m_size = new QComboBox();
  const std::pair<QString, int> sizes[] = {
      {tr("4 Mbit (59 blocks)"), 4},     {tr("8 Mbit (123 blocks)"), 8},
      {tr("16 Mbit (251 blocks)"), 16},  {tr("32 Mbit (507 blocks)"), 32},
      {tr("64 Mbit (1019 blocks)"), 64}, {tr("128 Mbit (2043 blocks)"), 128},
  };
  for (const auto& [label, megabits] : sizes)
    m_size->addItem(label, megabits);
  m_size->setCurrentIndex(DEFAULT_SIZE_INDEX);

  m_western = new QRadioButton(tr("Western (Windows-1252)"));
  // i18n: The translation of this string should be consistent with the translation of the
  // string "Western (Windows-1252)". Because of this, you may want to parse "Japanese" as
  // "a character encoding which is from Japan / used in Japan" rather than "the Japanese language".
  m_shift_jis = new QRadioButton(tr("Japanese (Shift-JIS)"));
  m_western->setChecked(true);

  auto* const size_label = new QLabel(tr("Card Size"));
  // i18n: Character encoding
  auto* const encoding_label = new QLabel(tr("Encoding"));

  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Create..."));

  auto* const grid = new QGridLayout();
  grid->addWidget(size_label, 0, 0);
  grid->addWidget(m_size, 0, 1);
  grid->addWidget(encoding_label, 1, 0);
  grid->addWidget(m_western, 1, 1);
  grid->addWidget(m_shift_jis, 2, 1);
  grid->addWidget(buttons, 3, 0, 1, 2, Qt::AlignRight);
  setLayout(grid);

  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  // The dialog only closes once the card has really been written.
  connect(buttons, &QDialogButtonBox::accepted, [this] {
    if (SaveNewCard())
      accept();
  });

  setWindowTitle(tr("Create New Memory Card"));
}

GCMemcardCreateNewDialog::~GCMemcardCreateNewDialog() = default;

bool GCMemcardCreateNewDialog::SaveNewCard()
{
  const auto megabits = static_cast<u16>(m_size->currentData().toInt());
  const bool shift_jis = m_shift_jis->isChecked();

  const QString chosen = DolphinFileDialog::getSaveFileName(
      this, tr("Create New Memory Card"), QString::fromStdString(File::GetUserPath(D_GCUSER_IDX)),
      tr("GameCube Memory Cards (*.raw *.gcp)") + QStringLiteral(";;") + tr("All Files (*)"));
  if (chosen.isEmpty())
    return false;

  // A blank card as a freshly formatted one from a console with default SRAM: no flash ID,
  // no RTC bias, English, formatted now.
  const CardFlashId flash_id{};
  const u64 now = Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH;

  const std::string path = chosen.toStdString();
  auto card = Memcard::GCMemcard::Create(path, flash_id, megabits, shift_jis, 0, 0, now);
  if (!card || !card->Save())
    return false;

  m_path = path;
  return true;
}

std::string GCMemcardCreateNewDialog::GetMemoryCardPath() const
{
  return m_path;
}
