// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// How the board is set up in Supermodel.ini (with SimulateNet=0): Network=1,
// and PortIn, PortOut and AddressOut so that each machine sends to the next
// one's PortIn and the last sends back to the first.

#include "Supermodel.h"
#include "NetBoard.h"

#include <cstdio>
#include <cstring>

namespace
{
  int (*s_run68k)(int) = nullptr;
  void (*s_irq68k)(int) = nullptr;

  const int kCyclesPerFrame = 4000000 / 60;   // the 68000 runs at 4 MHz

  // Acknowledging any interrupt clears the 68000's interrupt request.
  int AckIrq(int)
  {
    M68KSetIRQ(0);
    return M68K_IRQ_AUTOVECTOR;
  }

  // Accesses the board's program shouldn't make stop the emulator with a
  // message box.
  void Complain(const char *what, UINT32 addr)
  {
    DebugLog("Net board 68K: %s at %06X\n", what, addr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Info", what, nullptr);
  }

  void CheckRam(UINT32 a)
  {
    if (a > 0xffff)
      Complain("Out of Range", a);
  }

  void CheckRegister(UINT32 a)
  {
    if ((a & 0xfff) > 0xff)
      Complain("Out of Range", a);
  }

  void CheckShared(UINT32 a)
  {
    if ((a & 0x3ffff) > 0xffff)
      Complain("Out of Range", a);
  }

  // The 68000's memory is kept as 16-bit words in host order.
  UINT16 Get16(const UINT8 *p)
  {
    UINT16 v;
    memcpy(&v, p, sizeof(v));
    return v;
  }

  void Put16(UINT8 *p, UINT16 v)
  {
    memcpy(p, &v, sizeof(v));
  }

  UINT32 Get32(const UINT8 *p)
  {
    return (UINT32(Get16(p)) << 16) | Get16(p + 2);
  }

  void Put32(UINT8 *p, UINT32 v)
  {
    Put16(p, UINT16(v >> 16));
    Put16(p + 2, UINT16(v));
  }

  unsigned Region(UINT32 a)
  {
    return (a >> 16) & 0xf;
  }

  // The link registers the board's program is known to use a byte at a time.
  bool KnownByteRegister(unsigned reg, bool write)
  {
    static const UINT8 reads[] = { 0x11, 0x19, 0x81, 0x83, 0x89, 0x8a };
    static const UINT8 writes[] = {
      0x01, 0x03, 0x05, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x29, 0x2f, 0x41, 0x43, 0x45, 0x47,
      0x51, 0x55, 0x57, 0x59, 0x5b, 0x5d, 0x5f, 0x81, 0x83, 0x85, 0x87, 0x88, 0x89, 0x8a, 0x8b
    };
    const UINT8 *list = write ? writes : reads;
    size_t n = write ? sizeof(writes) : sizeof(reads);
    return memchr(list, int(reg), n) != nullptr;
  }
}

void Net_SetCB(int (*Run68k)(int cycles), void (*Int68k)(int irq))
{
  s_run68k = Run68k;
  s_irq68k = Int68k;
}

CNetBoard::CNetBoard(const Util::Config::Node &config)
  : m_config(config)
{
}

CNetBoard::~CNetBoard(void)
{
}

// Nothing of the board goes into save states.
void CNetBoard::SaveState(CBlockFile *)
{
}

void CNetBoard::LoadState(CBlockFile *)
{
}

void CNetBoard::GetGame(const Game &game)
{
  m_game = game;
}

Result CNetBoard::Init(UINT8 *netRAMPtr, UINT8 *netBufferPtr)
{
  m_ram = netRAMPtr;
  m_shared = netBufferPtr;
  m_io = netBufferPtr + 0x10000;
  m_control = std::make_unique<UINT8[]>(0x100 + 2);   // zeroed; 2 spare for a word read at 0xff

  m_attached = m_game.netboard_present && m_config["Network"].ValueAs<bool>();

  M68KSetContext(&m_cpu);
  M68KInit();
  M68KAttachBus(this);
  M68KSetIRQCallback(AckIrq);
  M68KGetContext(&m_cpu);

  m_portIn = UINT16(m_config["PortIn"].ValueAs<unsigned>());
  m_portOut = UINT16(m_config["PortOut"].ValueAs<unsigned>());
  m_addressOut = m_config["AddressOut"].ValueAs<std::string>();
  m_send = std::make_unique<TCPSend>(m_addressOut, m_portOut);
  m_receive = std::make_unique<TCPReceive>(m_portIn);

  // Waits here until the next machine accepts.
  if (m_attached)
  {
    while (!m_send->Connect())
      printf("Connecting to %s:%i ..\n", m_addressOut.c_str(), m_portOut);
    printf("Successfully connected.\n");
  }
  return Result::OKAY;
}

void CNetBoard::Reset(void)
{
  m_bank = 0;
  m_rxOffset = 0;
  m_rxSize = 0;
  m_txOffset = 0;
  m_txSize = 0;

  M68KSetContext(&m_cpu);
  DebugLog("Net board reset (PC was %06X)\n", M68KGetPC());
  M68KReset();
  M68KGetContext(&m_cpu);
}

void CNetBoard::RunFrame(void)
{
  if (!IsRunning())
    return;

  // IRQ 5 is the board's timer; once a frame is too seldom for some games,
  // so the frame is run in four parts.
  M68KSetContext(&m_cpu);
  for (int part = 0; part < 4; part++)
  {
    M68KSetIRQ(5);
    M68KRun(kCyclesPerFrame);
  }
  M68KGetContext(&m_cpu);
}

M68KCtx *CNetBoard::GetM68K(void)
{
  return &m_cpu;
}

bool CNetBoard::IsAttached(void)
{
  return m_attached;
}

bool CNetBoard::IsRunning(void)
{
  return m_attached && m_io[0xc0] != 0;
}

// --- the 68000's view -------------------------------------------------------

UINT8 CNetBoard::Read8(UINT32 a)
{
  const unsigned reg = a & 0xff;
  switch (Region(a))
  {
  case 0x0:
    CheckRam(a);
    return m_ram[a ^ 1];
  case 0x4:
    CheckRegister(a);
    if (reg != 0x00)
      Complain("Unknown R8 CTRLRW", a);
    return m_control[reg];
  case 0x8:
    CheckShared(a);
    return m_shared[(a & 0xffff) ^ 1];
  case 0xc:
    CheckRegister(a);
    if (reg == 0x11 && m_game.name == "spikeofe")
      return 0x05;    // receive status this game waits for
    if (!KnownByteRegister(reg, false))
      Complain("Unknown R8 IOREG", a);
    return m_io[reg ^ 1];
  default:
    Complain("Unknown R8", a);
    return 0;
  }
}

UINT16 CNetBoard::Read16(UINT32 a)
{
  const unsigned reg = a & 0xff;
  switch (Region(a))
  {
  case 0x0:
    CheckRam(a);
    return Get16(m_ram + a);
  case 0x4:
    CheckRegister(a);
    if (reg != 0x00)
      Complain("Unknown R16 CTRLRW", a);
    return Get16(&m_control[reg]);
  case 0x8:
    CheckShared(a);
    return Get16(m_shared + (a & 0xffff));
  case 0xc:
    CheckRegister(a);
    if (reg != 0x88 && reg != 0x8a)
      Complain("Unknown R16 IOREG", a);
    return Get16(m_io + reg);
  default:
    Complain("Unknown R16", a);
    return 0;
  }
}

UINT32 CNetBoard::Read32(UINT32 a)
{
  switch (Region(a))
  {
  case 0x0:
    CheckRam(a);
    return Get32(m_ram + a);
  case 0x8:
    CheckShared(a);
    return Get32(m_shared + (a & 0xffff));
  default:
    Complain("Unknown R32", a);
    return 0;
  }
}

// What the 68000 can do through the control registers, byte or word wide.
void CNetBoard::ControlWrite(unsigned reg, bool wide)
{
  switch (reg)
  {
  case 0x40:      // acknowledge IRQ 5, which raises IRQ 4
    AckIrq(5);
    M68KSetIRQ(4);
    break;
  case 0xa0:      // acknowledge IRQ 2
    m_io[0] |= 0x01;
    AckIrq(2);
    break;
  case 0x80:      // LEDs
    break;
  case 0x00:      // shared RAM bank (noted, banks are not switched)
  case 0xc0:      // node id
  case 0xe0:      // receive complete
    if (wide)
      break;
    // fall through: only known as word writes
  default:
    Complain(wide ? "Unknown W16 CTRLRW" : "Unknown W8 CTRLRW", 0x40000 | reg);
    break;
  }
}

void CNetBoard::IoWrite8(unsigned reg, UINT8 d)
{
  const bool known = KnownByteRegister(reg, true);
  m_io[reg ^ 1] = d;

  switch (reg)
  {
  case 0x15:      // receive: 0x80 fetches the next message, 0x01 acknowledges IRQ 6
    if (d == 0x80)
    {
      const std::vector<char> &msg = m_receive->Receive();
      memcpy(m_shared + m_rxOffset, msg.data(), msg.size());
    }
    else if (d == 0x01)
      AckIrq(6);
    break;
  case 0x17:      // receive interrupt enable
    if (d == 0x8c)
    {
      M68KSetIRQ(6);
      M68KRun(6000);
    }
    break;
  case 0x1b:      // transmit: 0x80 sends
    if (d == 0x80)
      m_send->Send(m_shared + m_txOffset, m_txSize);
    break;
  case 0x1d:      // transmit interrupt enable
    if (d == 0x8c)
    {
      M68KSetIRQ(6);
      M68KRun(10000);
    }
    break;
  // offsets and sizes arrive a byte at a time, low byte first
  case 0x41: m_rxOffset = UINT16((m_rxOffset >> 8) | (d << 8)); break;
  case 0x43: m_rxSize = UINT16((m_rxSize >> 8) | (d << 8)); break;
  case 0x45: m_txOffset = UINT16((m_txOffset >> 8) | (d << 8)); break;
  case 0x47: m_txSize = UINT16((m_txSize >> 8) | (d << 8)); break;
  case 0x89:
    // Spikeout wants this copied into the shared RAM
    if (m_game.name == "spikeout" || m_game.name == "spikeofe")
      m_shared[4] = d;
    break;
  default:
    if (!known)
      Complain("Unknown W8 IOREG", 0xc0000 | reg);
    break;
  }
}

void CNetBoard::Write8(UINT32 a, UINT8 d)
{
  const unsigned reg = a & 0xff;
  switch (Region(a))
  {
  case 0x0:
    CheckRam(a);
    m_ram[a ^ 1] = d;
    break;
  case 0x4:
    CheckRegister(a);
    m_control[reg] = d;
    ControlWrite(reg, false);
    break;
  case 0x8:
    CheckShared(a);
    m_shared[(a & 0xffff) ^ 1] = d;
    break;
  case 0xc:
    CheckRegister(a);
    IoWrite8(reg, d);
    break;
  default:
    Complain("Unknown W8", a);
    break;
  }
}

void CNetBoard::Write16(UINT32 a, UINT16 d)
{
  const unsigned reg = a & 0xff;
  switch (Region(a))
  {
  case 0x0:
    CheckRam(a);
    Put16(m_ram + a, d);
    break;
  case 0x4:
    CheckRegister(a);
    Put16(&m_control[reg], d);
    if (reg == 0x00)
      m_bank = d;
    ControlWrite(reg, true);
    break;
  case 0x8:
    CheckShared(a);
    Put16(m_shared + (a & 0xffff), d);
    break;
  case 0xc:
    CheckRegister(a);
    if (reg == 0x88)
    {
      // status for the main board; the program then wants IRQ 4 served
      // and IRQ 2 raised
      Put16(m_io + reg, d);
      M68KSetIRQ(4);
      M68KRun(1000);
      M68KSetIRQ(2);
    }
    else if (reg == 0x8a)
      Put16(m_io + reg, d);
    else
      Complain("Unknown W16 IOREG", a);
    break;
  default:
    Complain("Unknown W16", a);
    break;
  }
}

void CNetBoard::Write32(UINT32 a, UINT32 d)
{
  switch (Region(a))
  {
  case 0x0:
    CheckRam(a);
    Put32(m_ram + a, d);
    break;
  case 0x8:
    CheckShared(a);
    Put32(m_shared + (a & 0xffff), d);
    break;
  default:
    Complain("Unknown W32", a);
    break;
  }
}

// --- the main board's view --------------------------------------------------

UINT8 CNetBoard::ReadCommRAM8(unsigned addr)
{
  return m_shared[addr];
}

UINT16 CNetBoard::ReadCommRAM16(unsigned addr)
{
  return Get16(m_shared + addr);
}

UINT32 CNetBoard::ReadCommRAM32(unsigned addr)
{
  UINT32 v;
  memcpy(&v, m_shared + addr, sizeof(v));
  return v;
}

void CNetBoard::WriteCommRAM8(unsigned addr, UINT8 data)
{
  m_shared[addr] = data;
}

void CNetBoard::WriteCommRAM16(unsigned addr, UINT16 data)
{
  Put16(m_shared + addr, data);
}

void CNetBoard::WriteCommRAM32(unsigned addr, UINT32 data)
{
  memcpy(m_shared + addr, &data, sizeof(data));
}

UINT16 CNetBoard::ReadIORegister(unsigned reg)
{
  if (!IsRunning())
    return 0;
  return Get16(m_io + reg);
}

void CNetBoard::WriteIORegister(unsigned reg, UINT16 data)
{
  // Register 0xc0 starts (non-zero) and stops the board; starting one that
  // is already going doesn't reset it.
  if (reg == 0xc0 && !(data != 0 && IsRunning()))
    Reset();
  Put16(m_io + reg, data);
}
