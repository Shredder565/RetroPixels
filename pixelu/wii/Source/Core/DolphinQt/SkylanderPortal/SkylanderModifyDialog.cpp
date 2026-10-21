// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/SkylanderPortal/SkylanderModifyDialog.h"

#include <array>
#include <climits>
#include <string>

#include <QBoxLayout>
#include <QCheckBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QValidator>

#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/System.h"

#include "DolphinQt/QtUtils/QtUtils.h"

using FigureType = IOS::HLE::USB::Type;

namespace
{
IOS::HLE::USB::Skylander* SlotOnPortal(u8 slot)
{
  return Core::System::GetInstance().GetSkylanderPortal().GetSkylander(slot);
}

// The nickname is a null-terminated UTF-16 string.
QString NicknameOf(const IOS::HLE::USB::SkylanderData& values)
{
  return QString::fromUtf16(reinterpret_cast<const char16_t*>(values.nickname.data()));
}

QDateTime ToDateTime(const IOS::HLE::USB::SkylanderDateTime& stamp)
{
  return QDateTime(QDate(stamp.year, stamp.month, stamp.day), QTime(stamp.hour, stamp.minute));
}

IOS::HLE::USB::SkylanderDateTime FromEditor(const QDateTimeEdit* editor)
{
  const QDate date = editor->date();
  const QTime time = editor->time();
  return {.minute = static_cast<u8>(time.minute()),
          .hour = static_cast<u8>(time.hour()),
          .day = static_cast<u8>(date.day()),
          .month = static_cast<u8>(date.month()),
          .year = static_cast<u16>(date.year())};
}

void AddRow(QVBoxLayout* layout, QWidget* caption, QWidget* editor)
{
  auto* row = new QHBoxLayout();
  row->addWidget(caption);
  row->addWidget(editor);
  layout->addLayout(row);
}
}  // namespace

SkylanderModifyDialog::SkylanderModifyDialog(QWidget* parent, u8 slot)
    : QDialog(parent), m_portal_slot(slot)
{
  auto* layout = new QVBoxLayout;

  m_figure = SlotOnPortal(slot)->figure.get();
  m_values = m_figure->GetData();

  auto* title_row = new QHBoxLayout;
  // i18n: %1 is a name
  title_row->addWidget(new QLabel(tr("Modifying Skylander: %1").arg(DisplayName())));
  layout->addLayout(title_row);

  m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  bool editable = false;
  switch (m_values.normalized_type)
  {
  case FigureType::Skylander:
    PopulateSkylanderOptions(layout);
    editable = true;
    break;
  case FigureType::Trophy:
    editable = PopulateTrophyOptions(layout);
    break;
  case FigureType::Item:
    QMessageBox::warning(
        this, tr("No data to modify!"),
        tr("The type of this Skylander does not have any data that can be modified!"),
        QMessageBox::Ok);
    break;
  case FigureType::Unknown:
    QMessageBox::warning(this, tr("Unknown Skylander type!"),
                         tr("The type of this Skylander is unknown!"), QMessageBox::Ok);
    break;
  default:
    QMessageBox::warning(
        this, tr("Unable to modify Skylander!"),
        tr("The type of this Skylander is unknown, or can't be modified at this time!"),
        QMessageBox::Ok);
    QMessageBox::warning(this, tr("Can't be modified yet!"),
                         tr("This Skylander type can't be modified yet!"), QMessageBox::Ok);
    break;
  }

  layout->addWidget(m_buttons);
  setLayout(layout);

  if (editable)
  {
    show();
    raise();
  }
}

QString SkylanderModifyDialog::DisplayName() const
{
  // A character's own nickname wins; otherwise the name it has in the figure list.
  if (m_values.normalized_type == FigureType::Skylander &&
      m_values.skylander_data.nickname[0] != 0x00)
  {
    return QStringLiteral("\"%1\"").arg(NicknameOf(m_values.skylander_data));
  }

  const auto known =
      IOS::HLE::USB::list_skylanders.find(std::make_pair(m_values.figure_id, m_values.variant_id));
  if (known != IOS::HLE::USB::list_skylanders.end())
    return QString::fromStdString(known->second.name);

  // Not expected: every figure the portal accepts should be in the list.
  // i18n: "Var" is short for "variant"
  return tr("Unknown (Id:%1 Var:%2)").arg(m_values.figure_id).arg(m_values.variant_id);
}

void SkylanderModifyDialog::PopulateSkylanderOptions(QVBoxLayout* layout)
{
  const IOS::HLE::USB::SkylanderData& current = m_values.skylander_data;

  auto* toy_code_caption = new QLabel(tr("Toy code:"));
  auto* toy_code = new QLineEdit(QString::fromUtf8(current.toy_code));
  toy_code->setDisabled(true);

  auto* money_caption = new QLabel(tr("Money:"));
  auto* money = new QLineEdit(QStringLiteral("%1").arg(current.money));

  auto* hero_caption = new QLabel(tr("Hero level:"));
  auto* hero_level = new QLineEdit(QStringLiteral("%1").arg(current.hero_level));

  auto* nickname_caption = new QLabel(tr("Nickname:"));
  auto* nickname = new QLineEdit(NicknameOf(current));

  // i18n: The total amount of time the Skylander has been used for
  auto* playtime_caption = new QLabel(tr("Playtime:"));
  auto* playtime = new QLineEdit(QStringLiteral("%1").arg(current.playtime));

  // i18n: A timestamp for when the Skylander was most recently reset
  auto* reset_caption = new QLabel(tr("Last reset:"));
  auto* last_reset = new QDateTimeEdit(ToDateTime(current.last_reset));

  // i18n: A timestamp for when the Skylander was most recently used
  auto* placed_caption = new QLabel(tr("Last placed:"));
  auto* last_placed = new QDateTimeEdit(ToDateTime(current.last_placed));

  money->setValidator(new QIntValidator(0, 65000, this));
  hero_level->setValidator(new QIntValidator(0, 100, this));
  nickname->setValidator(
      new QRegularExpressionValidator(QRegularExpression(QStringLiteral("^\\p{L}{0,15}$")), this));
  playtime->setValidator(new QIntValidator(0, INT_MAX, this));

  QtUtils::ShowFourDigitYear(last_reset);
  QtUtils::ShowFourDigitYear(last_placed);

  toy_code->setToolTip(tr("The toy code for this figure. Only available for real figures."));
  money->setToolTip(tr("The amount of money this Skylander has. Between 0 and 65000"));
  hero_level->setToolTip(tr("The hero level of this Skylander. Only seen in Skylanders: Spyro's "
                            "Adventures. Between 0 and 100"));
  nickname->setToolTip(tr("The nickname for this Skylander. Limited to 15 characters"));
  playtime->setToolTip(tr("The total time this figure has been used inside a game in seconds"));
  last_reset->setToolTip(tr("The last time the figure has been reset. If the figure has never "
                            "been reset, the first time the figure was placed on a portal"));
  last_placed->setToolTip(tr("The last time the figure has been placed on a portal"));

  AddRow(layout, toy_code_caption, toy_code);
  AddRow(layout, money_caption, money);
  AddRow(layout, hero_caption, hero_level);
  AddRow(layout, nickname_caption, nickname);
  AddRow(layout, playtime_caption, playtime);
  AddRow(layout, reset_caption, last_reset);
  AddRow(layout, placed_caption, last_placed);

  connect(m_buttons, &QDialogButtonBox::accepted, this,
          [this, money, hero_level, nickname, playtime, last_reset, last_placed] {
            // The first field that doesn't validate is reported and nothing is saved.
            struct Check
            {
              bool ok;
              const char* title;
              const char* text;
            };
            const Check checks[] = {
                {money->hasAcceptableInput(), QT_TR_NOOP("Incorrect money value!"),
                 QT_TR_NOOP("Make sure that the money value is between 0 and 65000!")},
                {hero_level->hasAcceptableInput(), QT_TR_NOOP("Incorrect hero level value!"),
                 QT_TR_NOOP("Make sure that the hero level value is between 0 and 100!")},
                {nickname->hasAcceptableInput(), QT_TR_NOOP("Incorrect nickname!"),
                 QT_TR_NOOP("Make sure that the nickname is between 0 and 15 characters long!")},
                {playtime->hasAcceptableInput(), QT_TR_NOOP("Incorrect playtime value!"),
                 QT_TR_NOOP("Make sure that the playtime value is valid!")},
                {last_reset->hasAcceptableInput(), QT_TR_NOOP("Incorrect last reset time!"),
                 QT_TR_NOOP("Make sure that the last reset datetime value is valid!")},
                {last_placed->hasAcceptableInput(), QT_TR_NOOP("Incorrect last placed time!"),
                 QT_TR_NOOP("Make sure that the last placed datetime value is valid!")},
            };
            for (const Check& check : checks)
            {
              if (!check.ok)
              {
                QMessageBox::warning(this, tr(check.title), tr(check.text), QMessageBox::Ok);
                return;
              }
            }

            m_saved = true;

            // Everything not edited here (the toy code included) is written back as zero.
            IOS::HLE::USB::SkylanderData edited{};
            edited.money = money->text().toUShort();
            edited.hero_level = hero_level->text().toUShort();
            edited.playtime = playtime->text().toUInt();
            edited.last_reset = FromEditor(last_reset);
            edited.last_placed = FromEditor(last_placed);
            const std::u16string name = nickname->text().toStdU16String();
            name.copy(reinterpret_cast<char16_t*>(edited.nickname.data()), name.length());
            m_values.skylander_data = edited;

            if (!m_figure->FileIsOpen())
            {
              QMessageBox::warning(this, tr("Could not save your changes!"),
                                   tr("The file associated to this file was closed! Did you clear "
                                      "the slot before saving?"),
                                   QMessageBox::Ok);
            }
            else
            {
              m_figure->SetData(&m_values);
            }

            accept();
          });
}

bool SkylanderModifyDialog::PopulateTrophyOptions(QVBoxLayout* layout)
{
  constexpr u16 KAOS_TROPHY = 3503;
  constexpr u16 SEA_TROPHY = 3502;

  if (m_values.figure_id == KAOS_TROPHY)
  {
    QMessageBox::warning(this, tr("Can't edit villains for this trophy!"),
                         tr("Kaos is the only villain for this trophy and is always unlocked. No "
                            "need to edit anything!"),
                         QMessageBox::Ok);
    return false;
  }

  // Each of the four villains is one bit of the unlock mask; which bits depends on the trophy.
  constexpr std::array<int, 4> SEA_BITS = {0, 1, 2, 4};
  constexpr std::array<int, 4> OTHER_BITS = {0, 2, 3, 4};
  const std::array<int, 4> bits = m_values.figure_id == SEA_TROPHY ? SEA_BITS : OTHER_BITS;

  std::array<QCheckBox*, 4> captured;
  for (int villain = 0; villain < 4; ++villain)
  {
    const int bit = bits[static_cast<size_t>(villain)];
    auto* box = new QCheckBox();
    box->setChecked(((m_values.trophy_data.unlocked_villains >> bit) & 1) != 0);
    captured[static_cast<size_t>(villain)] = box;
    // i18n: "Captured" is a participle here. This string is used when listing villains, not when
    // a villain was just captured
    AddRow(layout, new QLabel(tr("Captured villain %1:").arg(villain + 1)), box);
  }

  connect(m_buttons, &QDialogButtonBox::accepted, this, [this, bits, captured] {
    u8 mask = 0;
    for (size_t villain = 0; villain < captured.size(); ++villain)
    {
      if (captured[villain]->isChecked())
        mask = static_cast<u8>(mask | (1 << bits[villain]));
    }
    m_values.trophy_data.unlocked_villains = mask;

    m_figure->SetData(&m_values);
    m_saved = true;
    accept();
  });

  return true;
}

void SkylanderModifyDialog::accept()
{
  if (!m_saved)
    return;

  // Lift the figure off and put it back so the game reads the new values.
  auto* on_portal = SlotOnPortal(m_portal_slot);
  on_portal->queued_status.push(IOS::HLE::USB::Skylander::REMOVED);
  on_portal->queued_status.push(IOS::HLE::USB::Skylander::ADDED);
  on_portal->queued_status.push(IOS::HLE::USB::Skylander::READY);
  QDialog::accept();
}
