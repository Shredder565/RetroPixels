// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/USBDevicePicker.h"

#include <optional>

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Core/USBUtils.h"

namespace
{
// IDs are four hex digits.
constexpr int ID_DIGITS = 4;
constexpr int POLL_INTERVAL_MS = 1000;

QString HexId(u16 id)
{
  return QString::fromStdString(fmt::format("{:04x}", id));
}
}  // namespace

USBDevicePicker::USBDevicePicker(QWidget* parent, FilterFunctionType filter)
    : QDialog(parent), m_filter(std::move(filter))
{
  setLayout(BuildContents());
  adjustSize();
}

std::optional<USBUtils::DeviceInfo> USBDevicePicker::Run(QWidget* parent, const QString& title,
                                                         FilterFunctionType filter)
{
  USBDevicePicker dialog(parent, std::move(filter));
  dialog.setWindowTitle(title);
  if (dialog.exec() != QDialog::Accepted)
    return std::nullopt;
  return dialog.EnteredDevice();
}

QVBoxLayout* USBDevicePicker::BuildContents()
{
  auto* const buttons = new QDialogButtonBox();
  auto* const select = new QPushButton(tr("Select"));
  auto* const cancel = new QPushButton(tr("Cancel"));
  buttons->addButton(select, QDialogButtonBox::AcceptRole);
  buttons->addButton(cancel, QDialogButtonBox::RejectRole);
  connect(select, &QPushButton::clicked, this, &QDialog::accept);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  select->setDefault(true);

  auto* const column = new QVBoxLayout();

  auto* const manual_hint = new QLabel(tr("Enter USB device ID"));
  manual_hint->setAlignment(Qt::AlignCenter);
  column->addWidget(manual_hint);

  // Two equally stretching fields side by side.
  QSizePolicy field_policy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  field_policy.setHorizontalStretch(1);
  field_policy.setVerticalStretch(0);

  auto* const id_row = new QHBoxLayout();
  for (QLineEdit** field : {&m_vid_field, &m_pid_field})
  {
    *field = new QLineEdit();
    field_policy.setHeightForWidth((*field)->sizePolicy().hasHeightForWidth());
    (*field)->setSizePolicy(field_policy);
    id_row->addWidget(*field);
  }
  column->addLayout(id_row);

  auto* const list_hint = new QLabel(tr("or select a device"));
  list_hint->setAlignment(Qt::AlignCenter);
  column->addWidget(list_hint);

  m_device_list = new QListWidget();
  m_poll_timer = new QTimer(this);
  connect(m_device_list, &QListWidget::currentItemChanged, this,
          &USBDevicePicker::CopySelectionToFields);
  // Double-clicking a device acts like pressing Select.
  connect(m_device_list, &QListWidget::itemDoubleClicked, select, &QPushButton::clicked);
  connect(m_poll_timer, &QTimer::timeout, this, &USBDevicePicker::PollDevices);
  PollDevices();
  m_poll_timer->start(POLL_INTERVAL_MS);

  column->addWidget(m_device_list);
  column->addWidget(buttons);

  // i18n: VID means Vendor ID (in the context of a USB device)
  m_vid_field->setPlaceholderText(tr("Device VID"));
  // i18n: PID means Product ID (in the context of a USB device), not Process ID
  m_pid_field->setPlaceholderText(tr("Device PID"));

  auto* const hex_only =
      new QRegularExpressionValidator(QRegularExpression(QStringLiteral("^[0-9A-Fa-f]*$")), this);
  for (QLineEdit* const field : {m_vid_field, m_pid_field})
  {
    field->setValidator(hex_only);
    field->setMaxLength(ID_DIGITS);
  }

  return column;
}

void USBDevicePicker::PollDevices()
{
  std::vector<USBUtils::DeviceInfo> present = USBUtils::ListDevices(m_filter);
  if (present == m_listed)
    return;

  // Remember which device was highlighted so it stays highlighted after the rebuild.
  std::optional<USBUtils::DeviceInfo> highlighted;
  if (const QListWidgetItem* const item = m_device_list->currentItem())
    highlighted = item->data(Qt::UserRole).value<USBUtils::DeviceInfo>();

  {
    // Rebuilding must not touch the VID/PID fields the user may have edited.
    const QSignalBlocker quiet(m_device_list);
    m_device_list->clear();
    for (const USBUtils::DeviceInfo& device : present)
    {
      auto* const item =
          new QListWidgetItem(QString::fromStdString(device.ToDisplayString()), m_device_list);
      item->setData(Qt::UserRole, QVariant::fromValue(device));
      if (highlighted && *highlighted == device)
        m_device_list->setCurrentItem(item);
    }
  }

  m_listed = std::move(present);
}

void USBDevicePicker::CopySelectionToFields()
{
  const QListWidgetItem* const item = m_device_list->currentItem();
  if (!item)
    return;

  const auto device = item->data(Qt::UserRole).value<USBUtils::DeviceInfo>();
  m_vid_field->setText(HexId(device.vid));
  m_pid_field->setText(HexId(device.pid));
}

std::optional<USBUtils::DeviceInfo> USBDevicePicker::EnteredDevice() const
{
  const std::string vid_text = m_vid_field->text().toStdString();
  const std::string pid_text = m_pid_field->text().toStdString();
  if (vid_text.empty() || pid_text.empty())
    return std::nullopt;

  const auto parse = [](const std::string& hex) {
    return static_cast<u16>(std::stoul(hex, nullptr, 16));
  };
  return USBUtils::DeviceInfo{parse(vid_text), parse(pid_text)};
}
