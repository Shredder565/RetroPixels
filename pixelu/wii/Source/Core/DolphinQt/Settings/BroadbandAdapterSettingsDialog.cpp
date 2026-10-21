// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/BroadbandAdapterSettingsDialog.h"

#include <regex>
#include <string>

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QString>
#include <QVBoxLayout>

#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

namespace
{
using Mode = BroadbandAdapterSettingsDialog::Type;

// The config entry each mode edits.
const Config::Info<std::string>& SettingFor(Mode mode)
{
  switch (mode)
  {
  case Mode::Ethernet:
    return Config::MAIN_BBA_MAC;
  case Mode::TapServer:
    return Config::MAIN_BBA_TAPSERVER_DESTINATION;
  case Mode::ModemTapServer:
    return Config::MAIN_MODEM_TAPSERVER_DESTINATION;
  case Mode::BuiltIn:
    return Config::MAIN_BBA_BUILTIN_DNS;
  case Mode::XLinkKai:
  default:
    return Config::MAIN_BBA_XLINK_IP;
  }
}

bool LooksLikeMac(const std::string& text)
{
  static const std::regex six_hex_pairs("([0-9A-Fa-f]{2}:){5}([0-9A-Fa-f]{2})");
  return std::regex_match(text, six_hex_pairs);
}
}  // namespace

// Texts shown for one mode.
struct BroadbandAdapterSettingsDialog::Wording
{
  QString title;
  QString prompt;
  QString example;
  QString help;
};

BroadbandAdapterSettingsDialog::BroadbandAdapterSettingsDialog(QWidget* parent, Type bba_type)
    : QDialog(parent), m_mode(bba_type)
{
  BuildLayout();
}

BroadbandAdapterSettingsDialog::Wording BroadbandAdapterSettingsDialog::DescribeMode() const
{
  Wording words;

  switch (m_mode)
  {
  case Type::Ethernet:
    // i18n: MAC stands for Media Access Control. A MAC address uniquely identifies a network
    // interface (physical) like a serial number. "MAC" should be kept in translations.
    words.title = tr("Broadband Adapter MAC Address");
    // i18n: MAC stands for Media Access Control. A MAC address uniquely identifies a network
    // interface (physical) like a serial number. "MAC" should be kept in translations.
    words.prompt = tr("Enter new Broadband Adapter MAC address:");
    words.example = QStringLiteral("aa:bb:cc:dd:ee:ff");
    words.help = tr("For setup instructions, <a href=\"https://wiki.dolphin-emu.org/"
                    "index.php?title=Broadband_Adapter\">refer to this page</a>.");
    break;

  case Type::TapServer:
  case Type::ModemTapServer:
    words.title = tr("BBA destination address");
#ifdef _WIN32
    words.prompt = tr("Destination (address:port):");
    words.example = QStringLiteral("");
    words.help =
        tr("Enter the IP address and port of the tapserver instance you want to connect to.");
#else
    words.prompt = tr("Destination (UNIX socket path or address:port):");
    words.example = m_mode == Type::ModemTapServer ? QStringLiteral(u"/tmp/dolphin-modem-tap") :
                                                     QStringLiteral(u"/tmp/dolphin-tap");
    words.help = tr("The default value \"%1\" will work with a local tapserver and newserv. You "
                    "can also enter a network location (address:port) to connect to a remote "
                    "tapserver.")
                     .arg(words.example);
#endif
    break;

  case Type::BuiltIn:
    words.title = tr("Broadband Adapter DNS setting");
    words.prompt = tr("Enter the DNS server to use:");
    words.example = QStringLiteral("8.8.8.8");
    words.help = tr("Use 8.8.8.8 for normal DNS, else enter your custom one");
    break;

  case Type::XLinkKai:
    words.title = tr("XLink Kai BBA Destination Address");
    words.prompt = tr("Enter IP address of device running the XLink Kai Client:");
    words.example = QStringLiteral("127.0.0.1");
    words.help = tr("For setup instructions, <a href=\"https://www.teamxlink.co.uk/wiki/"
                    "Dolphin\">refer to this page</a>.");
    break;
  }

  return words;
}

void BroadbandAdapterSettingsDialog::BuildLayout()
{
  const Wording words = DescribeMode();
  setWindowTitle(words.title);

  auto* const prompt = new QLabel(words.prompt);

  m_value_edit = new QLineEdit(QString::fromStdString(Config::Get(SettingFor(m_mode))));
  m_value_edit->setPlaceholderText(words.example);

  // The help text may carry a link that opens in the browser.
  auto* const help = new QLabel(words.help);
  help->setTextFormat(Qt::RichText);
  help->setWordWrap(true);
  help->setTextInteractionFlags(Qt::TextBrowserInteraction);
  help->setOpenExternalLinks(true);

  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &BroadbandAdapterSettingsDialog::Store);
  connect(buttons, &QDialogButtonBox::rejected, this, &BroadbandAdapterSettingsDialog::reject);

  auto* const column = new QVBoxLayout();
  column->addWidget(prompt);
  column->addWidget(m_value_edit);
  column->addWidget(help);
  column->addWidget(buttons);
  setLayout(column);
}

void BroadbandAdapterSettingsDialog::Store()
{
  const std::string value{StripWhitespace(m_value_edit->text().toStdString())};

  if (m_mode == Type::Ethernet && !LooksLikeMac(value))
  {
    // i18n: MAC stands for Media Access Control. A MAC address uniquely identifies a network
    // interface (physical) like a serial number. "MAC" should be kept in translations.
    ModalMessageBox::critical(this, tr("Broadband Adapter Error"),
                              tr("The entered MAC address is invalid."));
    return;
  }

  Config::SetBaseOrCurrent(SettingFor(m_mode), value);
  accept();
}
