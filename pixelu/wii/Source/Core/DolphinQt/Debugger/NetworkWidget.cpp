// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/NetworkWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include <bit>
#include <optional>
#include <string>

#include "Common/FileUtil.h"
#include "Common/Network.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/IOS/Network/SSL.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/System.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Settings.h"

namespace
{
// Order of the entries in the dump format box.
enum class DumpFormat : int
{
  None = 0,
  PCAP,
  BinarySSL,
  BinarySSLRead,
  BinarySSLWrite,
};

// Socket table columns.
constexpr int SOCKET_BLOCKING_COLUMN = 4;
constexpr int SOCKET_REDIRECTIONS_COLUMN = 6;

// Cell texts are translated in QTableWidget's context.
QTableWidgetItem* Cell(const QString& text = {})
{
  return new QTableWidgetItem(text);
}

std::optional<sockaddr_in> LocalAddress(s32 fd)
{
  sockaddr_in addr;
  socklen_t len = sizeof(sockaddr_in);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
    return std::nullopt;
  return addr;
}

std::optional<sockaddr_in> PeerAddress(s32 fd)
{
  sockaddr_in addr;
  socklen_t len = sizeof(sockaddr_in);
  if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
    return std::nullopt;
  return addr;
}

// "a.b.c.d:port", or empty if the address cannot be printed.
QString Endpoint(const sockaddr_in& addr)
{
  char text[16];
  if (!inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text)))
    return {};
  return QStringLiteral("%1:%2").arg(QString::fromLatin1(text)).arg(ntohs(addr.sin_port));
}

Common::IPv4Port AsIPv4Port(const sockaddr_in& addr)
{
  return Common::IPv4Port{std::bit_cast<Common::IPAddress>(addr.sin_addr), addr.sin_port};
}

// The numbers are the Windows values (AF_INET6 is 23 there).
QTableWidgetItem* DomainCell(s32 fd)
{
  if (fd < 0)
    return Cell();

  sockaddr sa;
  socklen_t len = sizeof(sa);
  if (getsockname(fd, &sa, &len) != 0)
    return Cell(QTableWidget::tr("Unknown"));

  if (sa.sa_family == 2)
    return Cell(QStringLiteral("AF_INET"));
  if (sa.sa_family == 23)
    return Cell(QStringLiteral("AF_INET6"));
  return Cell(QString::number(sa.sa_family));
}

QTableWidgetItem* TypeCell(s32 fd)
{
  if (fd < 0)
    return Cell();

  int kind;
  socklen_t len = sizeof(kind);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&kind), &len) != 0)
    return Cell(QTableWidget::tr("Unknown"));

  if (kind == 1)
    return Cell(QStringLiteral("SOCK_STREAM"));
  if (kind == 2)
    return Cell(QStringLiteral("SOCK_DGRAM"));
  return Cell(QString::number(kind));
}

QTableWidgetItem* StateCell(s32 fd)
{
  if (fd < 0)
    return Cell();

  if (PeerAddress(fd))
    return Cell(QTableWidget::tr("Connected"));

  int listening = 0;
  socklen_t len = sizeof(listening);
  const bool queried =
      getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&listening), &len) == 0;
  return Cell(queried && listening > 0 ? QTableWidget::tr("Listening") : QTableWidget::tr("Unbound"));
}

QTableWidgetItem* BlockingCell(const IOS::HLE::WiiSockMan& sockets, s32 wii_fd)
{
  if (sockets.GetHostSocket(wii_fd) < 0)
    return Cell();
  return Cell(sockets.IsSocketBlocking(wii_fd) ? QTableWidget::tr("Yes") : QTableWidget::tr("No"));
}

QTableWidgetItem* NameCell(s32 fd)
{
  if (fd < 0)
    return Cell();

  const std::optional<sockaddr_in> local = LocalAddress(fd);
  if (!local)
    return Cell(QTableWidget::tr("Unknown"));
  const QString local_text = Endpoint(*local);
  if (local_text.isEmpty())
    return Cell(QTableWidget::tr("Unknown"));

  const std::optional<sockaddr_in> peer = PeerAddress(fd);
  const QString peer_text = peer ? Endpoint(*peer) : QString();
  if (peer_text.isEmpty())
    return Cell(local_text);
  return Cell(QStringLiteral("%1->%2").arg(local_text).arg(peer_text));
}

// Triforce IP redirection rules whose replacement matches this socket's local or peer address.
QTableWidgetItem* RedirectionsCell(s32 fd, const AMMediaboard::IPRedirections& rules)
{
  if (fd < 0 || rules.empty())
    return Cell();

  const std::optional<sockaddr_in> local = LocalAddress(fd);
  if (!local)
    return Cell();
  const Common::IPv4Port local_port = AsIPv4Port(*local);

  const std::optional<sockaddr_in> peer = PeerAddress(fd);
  const Common::IPv4Port peer_port = peer ? AsIPv4Port(*peer) : Common::IPv4Port{};

  QStringList local_hits;
  QStringList peer_hits;
  for (const auto& rule : rules)
  {
    if (rule.replacement.IsMatch(local_port))
      local_hits.append(QString::fromStdString(rule.ToString()));
    if (peer && rule.replacement.IsMatch(peer_port))
      peer_hits.append(QString::fromStdString(rule.ToString()));
  }

  const QString newline = QStringLiteral("\n");
  if (peer_hits.isEmpty())
    return local_hits.isEmpty() ? Cell() : Cell(local_hits.join(newline));
  return Cell(QStringLiteral("Sock rules:\n%1\n\nPeer rules:%2")
                  .arg(local_hits.join(newline))
                  .arg(peer_hits.join(newline)));
}

void FillSocketRow(QTableWidget* table, int row, s32 host_fd)
{
  table->setItem(row, 1, DomainCell(host_fd));
  table->setItem(row, 2, TypeCell(host_fd));
  table->setItem(row, 3, StateCell(host_fd));
}
}  // namespace

NetworkWidget::NetworkWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Network"));
  setObjectName(QStringLiteral("network"));

  auto& settings = Settings::Instance();
  setHidden(!settings.IsNetworkVisible() || !settings.IsDebugModeEnabled());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  BuildLayout();

  QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("networkwidget/geometry")).toByteArray());
  setFloating(stored.value(QStringLiteral("networkwidget/floating")).toBool());

  WireUp();

  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this, &NetworkWidget::Refresh);
  connect(&settings, &Settings::NetworkVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(&settings, &Settings::DebugModeToggled, this, [this](bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsNetworkVisible());
  });
}

NetworkWidget::~NetworkWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("networkwidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("networkwidget/floating"), isFloating());
}

void NetworkWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetNetworkVisible(false);
}

void NetworkWidget::showEvent(QShowEvent*)
{
  Refresh();
}

void NetworkWidget::BuildLayout()
{
  auto* body = new QWidget;
  auto* column = new QVBoxLayout;
  body->setLayout(column);
  column->addWidget(BuildSocketBox());
  column->addWidget(BuildSslBox());
  column->addWidget(BuildDumpBox());
  column->addWidget(BuildSecurityBox());
  column->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding));
  setWidget(body);

  Refresh();
}

void NetworkWidget::WireUp()
{
  connect(m_dump_format, &QComboBox::currentIndexChanged, this, &NetworkWidget::DumpFormatPicked);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  using CheckValue = Qt::CheckState;
  const auto check_changed = &QCheckBox::checkStateChanged;
#else
  using CheckValue = int;
  const auto check_changed = &QCheckBox::stateChanged;
#endif

  // Each box writes its own setting as soon as it changes.
  const auto bind = [this, check_changed](QCheckBox* box, const Config::Info<bool>& setting) {
    connect(box, check_changed, this, [info = &setting](CheckValue value) {
      Config::SetBaseOrCurrent(*info, value == Qt::Checked);
    });
  };
  bind(m_dump_ssl_read, Config::MAIN_NETWORK_SSL_DUMP_READ);
  bind(m_dump_ssl_write, Config::MAIN_NETWORK_SSL_DUMP_WRITE);
  bind(m_dump_root_ca, Config::MAIN_NETWORK_SSL_DUMP_ROOT_CA);
  bind(m_dump_peer_cert, Config::MAIN_NETWORK_SSL_DUMP_PEER_CERT);
  bind(m_verify_certs, Config::MAIN_NETWORK_SSL_VERIFY_CERTIFICATES);
  bind(m_dump_bba, Config::MAIN_NETWORK_DUMP_BBA);

  connect(m_open_folder, &QPushButton::clicked, this, [] {
    const QString folder = QString::fromStdString(File::GetUserPath(D_DUMPSSL_IDX));
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
  });
}

void NetworkWidget::FillWiiTables(Core::System& system)
{
  // Wii sockets know whether they block; there are no redirections.
  m_sockets->showColumn(SOCKET_BLOCKING_COLUMN);
  m_sockets->hideColumn(SOCKET_REDIRECTIONS_COLUMN);

  auto* ios = system.GetIOS();
  if (!ios)
    return;
  const auto sockets = ios->GetSocketManager();
  if (!sockets)
    return;

  for (s32 wii_fd = 0; wii_fd < IOS::HLE::WII_SOCKET_FD_MAX; ++wii_fd)
  {
    m_sockets->insertRow(wii_fd);
    const s32 host_fd = sockets->GetHostSocket(wii_fd);
    m_sockets->setItem(wii_fd, 0, Cell(QString::number(wii_fd)));
    FillSocketRow(m_sockets, wii_fd, host_fd);
    m_sockets->setItem(wii_fd, 4, BlockingCell(*sockets, wii_fd));
    m_sockets->setItem(wii_fd, 5, NameCell(host_fd));
  }

  for (s32 id = 0; id < IOS::HLE::NET_SSL_MAXINSTANCES; ++id)
  {
    m_ssl_contexts->insertRow(id);
    s32 host_fd = -1;
    if (IOS::HLE::IsSSLIDValid(id))
    {
      const auto& context = IOS::HLE::NetSSLDevice::_SSL[id];
      host_fd = context.hostfd;
      m_ssl_contexts->setItem(id, 5, Cell(QString::fromStdString(context.hostname)));
    }
    m_ssl_contexts->setItem(id, 0, Cell(QString::number(id)));
    FillSocketRow(m_ssl_contexts, id, host_fd);
    m_ssl_contexts->setItem(id, 4, NameCell(host_fd));
  }
}

void NetworkWidget::FillTriforceTable()
{
  // The blocking state is not available here; the IP redirections are.
  m_sockets->hideColumn(SOCKET_BLOCKING_COLUMN);
  m_sockets->showColumn(SOCKET_REDIRECTIONS_COLUMN);

  const auto rules = AMMediaboard::GetIPRedirections();
  for (s32 fd = 0; fd != AMMediaboard::SOCKET_FD_MAX; ++fd)
  {
    m_sockets->insertRow(fd);
    const s32 host_fd = AMMediaboard::DebuggerGetSocket(static_cast<u32>(fd));
    m_sockets->setItem(fd, 0, Cell(QString::number(fd)));
    FillSocketRow(m_sockets, fd, host_fd);
    m_sockets->setItem(fd, 4, Cell(QTableWidget::tr("Unknown")));
    m_sockets->setItem(fd, 5, NameCell(host_fd));
    m_sockets->setItem(fd, 6, RedirectionsCell(host_fd, rules));
  }
}

void NetworkWidget::Refresh()
{
  if (!isVisible())
    return;

  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) != Core::State::Paused)
  {
    m_sockets->setDisabled(true);
    m_ssl_contexts->setDisabled(true);
    return;
  }

  // Holding the CPU keeps the IOS instance from changing underneath us.
  const Core::CPUThreadGuard guard(system);
  m_sockets->setDisabled(false);
  m_sockets->setRowCount(0);
  m_ssl_contexts->setRowCount(0);
  if (system.IsTriforce())
  {
    FillTriforceTable();
  }
  else if (system.IsWii())
  {
    m_ssl_contexts->setDisabled(false);
    FillWiiTables(system);
  }
  m_sockets->resizeColumnsToContents();
  m_sockets->resizeRowsToContents();
  m_ssl_contexts->resizeColumnsToContents();

  const bool as_pcap = Config::Get(Config::MAIN_NETWORK_DUMP_AS_PCAP);
  const bool ssl_read = Config::Get(Config::MAIN_NETWORK_SSL_DUMP_READ);
  const bool ssl_write = Config::Get(Config::MAIN_NETWORK_SSL_DUMP_WRITE);

  m_dump_ssl_read->setChecked(ssl_read);
  m_dump_ssl_write->setChecked(ssl_write);
  m_dump_root_ca->setChecked(Config::Get(Config::MAIN_NETWORK_SSL_DUMP_ROOT_CA));
  m_dump_peer_cert->setChecked(Config::Get(Config::MAIN_NETWORK_SSL_DUMP_PEER_CERT));
  m_verify_certs->setChecked(Config::Get(Config::MAIN_NETWORK_SSL_VERIFY_CERTIFICATES));

  DumpFormat format = DumpFormat::None;
  if (as_pcap)
    format = DumpFormat::PCAP;
  else if (ssl_read && ssl_write)
    format = DumpFormat::BinarySSL;
  else if (ssl_read)
    format = DumpFormat::BinarySSLRead;
  else if (ssl_write)
    format = DumpFormat::BinarySSLWrite;
  m_dump_format->setCurrentIndex(static_cast<int>(format));
}

QGroupBox* NetworkWidget::BuildSocketBox()
{
  auto* box = new QGroupBox(tr("Socket table"));
  auto* grid = new QGridLayout;
  box->setLayout(grid);

  m_sockets = new QTableWidget();
  // i18n: FD = the socket's file descriptor number
  const QStringList headings{tr("FD"),       tr("Domain"), tr("Type"),        tr("State"),
                             tr("Blocking"), tr("Name"),   tr("Redirections")};
  m_sockets->setColumnCount(static_cast<int>(headings.size()));
  m_sockets->setHorizontalHeaderLabels(headings);
  m_sockets->setTabKeyNavigation(false);
  m_sockets->verticalHeader()->setVisible(false);
  m_sockets->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_sockets->setSelectionMode(QAbstractItemView::NoSelection);
  m_sockets->setWordWrap(false);

  grid->addWidget(m_sockets, 0, 0);
  grid->setSpacing(1);
  return box;
}

QGroupBox* NetworkWidget::BuildSslBox()
{
  auto* box = new QGroupBox(tr("SSL context"));
  auto* grid = new QGridLayout;
  box->setLayout(grid);

  m_ssl_contexts = new QTableWidget();
  const QStringList headings{tr("ID"),    tr("Domain"), tr("Type"),
                             tr("State"), tr("Name"),   tr("Hostname")};
  m_ssl_contexts->setColumnCount(static_cast<int>(headings.size()));
  m_ssl_contexts->setHorizontalHeaderLabels(headings);
  m_ssl_contexts->setTabKeyNavigation(false);
  m_ssl_contexts->verticalHeader()->setVisible(false);
  m_ssl_contexts->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_ssl_contexts->setSelectionMode(QAbstractItemView::NoSelection);
  m_ssl_contexts->setWordWrap(false);

  grid->addWidget(m_ssl_contexts, 0, 0);
  grid->setSpacing(1);
  return box;
}

QGroupBox* NetworkWidget::BuildDumpBox()
{
  auto* box = new QGroupBox(tr("Dump options"));
  auto* column = new QVBoxLayout;
  box->setLayout(column);

  m_dump_format = new QComboBox();
  m_dump_format->insertItem(static_cast<int>(DumpFormat::None), tr("None"));
  // i18n: PCAP is the packet capture file format
  m_dump_format->insertItem(static_cast<int>(DumpFormat::PCAP), tr("PCAP"));
  m_dump_format->insertItem(static_cast<int>(DumpFormat::BinarySSL), tr("Binary SSL"));
  m_dump_format->insertItem(static_cast<int>(DumpFormat::BinarySSLRead), tr("Binary SSL (read)"));
  m_dump_format->insertItem(static_cast<int>(DumpFormat::BinarySSLWrite),
                            tr("Binary SSL (write)"));
  m_dump_format->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  m_dump_ssl_read = new QCheckBox(tr("Dump decrypted SSL reads"));
  m_dump_ssl_write = new QCheckBox(tr("Dump decrypted SSL writes"));
  // i18n: CA = certificate authority
  m_dump_root_ca = new QCheckBox(tr("Dump root CA certificates"));
  m_dump_peer_cert = new QCheckBox(tr("Dump peer certificates"));
  m_dump_bba = new QCheckBox(tr("Dump GameCube BBA traffic"));
  m_open_folder = new QPushButton(tr("Open dump folder"));
  m_open_folder->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  auto* format_caption = new QLabel(tr("Network dump format:"));
  format_caption->setBuddy(m_dump_format);
  auto* format_row = new QHBoxLayout;
  format_row->addWidget(format_caption);
  // A gap two underscores wide between the caption and the box.
  const int gap = format_caption->fontMetrics().boundingRect(QStringLiteral("__")).width();
  format_row->addItem(new QSpacerItem(gap, 0));
  format_row->addWidget(m_dump_format);
  format_row->addStretch();
  column->addLayout(format_row);

  for (QWidget* control : {static_cast<QWidget*>(m_dump_ssl_read),
                           static_cast<QWidget*>(m_dump_ssl_write),
                           static_cast<QWidget*>(m_dump_root_ca),
                           static_cast<QWidget*>(m_dump_peer_cert),
                           static_cast<QWidget*>(m_dump_bba), static_cast<QWidget*>(m_open_folder)})
  {
    column->addWidget(control);
  }
  column->setSpacing(1);
  return box;
}

QGroupBox* NetworkWidget::BuildSecurityBox()
{
  auto* box = new QGroupBox(tr("Security options"));
  auto* column = new QVBoxLayout;
  box->setLayout(column);

  m_verify_certs = new QCheckBox(tr("Verify certificates"));
  column->addWidget(m_verify_certs);
  column->setSpacing(1);
  return box;
}

void NetworkWidget::DumpFormatPicked(int index)
{
  const auto format = static_cast<DumpFormat>(index);

  // The binary SSL formats choose which directions are dumped; PCAP leaves the boxes to the user.
  if (format != DumpFormat::PCAP)
  {
    const bool reads = format == DumpFormat::BinarySSL || format == DumpFormat::BinarySSLRead;
    const bool writes = format == DumpFormat::BinarySSL || format == DumpFormat::BinarySSLWrite;
    m_dump_ssl_read->setChecked(reads);
    m_dump_ssl_write->setChecked(writes);
    m_dump_bba->setChecked(false);
  }

  const bool pcap = format == DumpFormat::PCAP;
  m_dump_ssl_read->setEnabled(pcap);
  m_dump_ssl_write->setEnabled(pcap);
  m_dump_bba->setEnabled(pcap);
  Config::SetBaseOrCurrent(Config::MAIN_NETWORK_DUMP_AS_PCAP, pcap);
}
