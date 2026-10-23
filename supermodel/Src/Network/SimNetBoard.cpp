// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Supermodel.h"
#include "SimNetBoard.h"
#include "OSD/Thread.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
  // Sent round the ring first: every machine must be running this build's
  // protocol.
  const uint64_t kLinkId = 0x5bf177da34872;
}

CSimNetBoard::CSimNetBoard(const Util::Config::Node &config)
  : m_config(config)
{
}

CSimNetBoard::~CSimNetBoard(void)
{
  m_stop = true;
  if (m_connector.joinable())
    m_connector.join();
}

// Nothing of the simulated board goes into save states.
void CSimNetBoard::SaveState(CBlockFile *)
{
}

void CSimNetBoard::LoadState(CBlockFile *)
{
}

bool CSimNetBoard::Is(const char *name) const
{
  return m_game.name == name || m_game.parent == name;
}

uint16_t CSimNetBoard::Ram(unsigned offset) const
{
  uint16_t v;
  memcpy(&v, m_ram + offset, sizeof(v));
  return v;
}

void CSimNetBoard::SetShared(unsigned offset, uint16_t value)
{
  memcpy(m_local + offset, &value, sizeof(value));
}

void CSimNetBoard::GetGame(const Game &game)
{
  m_game = game;
}

Result CSimNetBoard::Init(uint8_t *netRAMPtr, uint8_t *netBufferPtr)
{
  m_ram = netRAMPtr;
  m_buffer = netBufferPtr;
  m_local = m_buffer;
  m_external = m_buffer + 0x10000;

  m_attached = m_game.netboard_present && m_config["Network"].ValueAs<bool>();
  if (!m_attached)
    return Result::OKAY;

  static const char *const type1[] = { "daytona2", "harley", "scud", "srally2", "skichamp", "spikeout", "spikeofe" };
  static const char *const type2[] = { "lemans24", "von2", "dirtdvls" };
  auto any = [this](const char *const *names, size_t n)
  {
    return std::any_of(names, names + n, [this](const char *s) { return Is(s); });
  };
  if (any(type1, sizeof(type1) / sizeof(type1[0])))
    m_type = GameType::one;
  else if (any(type2, sizeof(type2) / sizeof(type2[0])))
    m_type = GameType::two;
  else
    return ErrorLog("Game not recognized or supported");

  m_state = State::start;
  m_portIn = uint16_t(m_config["PortIn"].ValueAs<unsigned>());
  m_portOut = uint16_t(m_config["PortOut"].ValueAs<unsigned>());
  m_addressOut = m_config["AddressOut"].ValueAs<std::string>();
  m_send = std::make_unique<TCPSend>(m_addressOut, m_portOut);
  m_receive = std::make_unique<TCPReceive>(m_portIn);
  return Result::OKAY;
}

void CSimNetBoard::Reset(void)
{
  // Tell the others the link is going down, so nobody waits for us.
  if (m_state == State::ready)
  {
    m_send->Send(nullptr, 0);
    m_receive->Receive();
  }
  m_running = false;
  m_state = State::start;
}

bool CSimNetBoard::IsAttached(void)
{
  return m_attached;
}

bool CSimNetBoard::IsRunning(void)
{
  return m_attached && m_running;
}

// Runs on its own thread until both directions of the ring are up.
void CSimNetBoard::Connect()
{
  if (m_linked)
    return;
  printf("Connecting to %s:%i ..\n", m_addressOut.c_str(), m_portOut);
  while (!m_send->Connect())
  {
    if (m_stop)
      return;
  }
  while (!m_receive->Connected())
  {
    if (m_stop)
      return;
    CThread::Sleep(1);
  }
  printf("Successfully connected.\n");
  m_linked = true;
}

// The next message from the previous machine, as much of it as fits in dst.
// False if the link broke.
bool CSimNetBoard::Take(void *dst, size_t bytes)
{
  const std::vector<char> &msg = m_receive->Receive();
  if (msg.empty())
    return false;
  memcpy(dst, msg.data(), std::min(msg.size(), bytes));
  return true;
}

// The id goes twice round the ring; a machine that receives a wrong one
// passes on 0, so on the second lap every machine learns whether all agree.
CSimNetBoard::Step CSimNetBoard::CheckBuild(bool master)
{
  uint64_t id = 0;
  if (master)
  {
    while (m_receive->CheckDataAvailable())
      m_receive->Receive();     // left over from before
    m_send->Send(&kLinkId, sizeof(kLinkId));
    if (!Take(&id, sizeof(id)))
      return Step::again;
    if (id != kLinkId)
      id = 0;
    m_send->Send(&id, sizeof(id));
    m_receive->Receive();
  }
  else
  {
    for (int lap = 0; lap < 2; lap++)
    {
      if (!Take(&id, sizeof(id)))
        return Step::again;
      if (id != kLinkId)
        id = 0;
      m_send->Send(&id, sizeof(id));
    }
  }
  return id == kLinkId ? Step::done : Step::failed;
}

// The master sends index 0 round; each machine adds one and passes it on, so
// what comes back to the master is the number of other machines, which then
// goes round once more for everybody. Type 2 games count two things (all
// machines, and those that can be played on) in two bytes; type 1 only one.
CSimNetBoard::Step CSimNetBoard::CountMachines(bool master, size_t bytes, uint8_t *index, uint8_t *count, bool playable)
{
  if (master)
  {
    m_send->Send(index, int(bytes));
    if (!Take(count, bytes))
      return Step::again;
    m_send->Send(count, int(bytes));
    m_receive->Receive();
    return Step::done;
  }
  if (!Take(index, bytes))
    return Step::again;
  index[0]++;
  if (bytes > 1 && playable)
    index[1]++;
  m_send->Send(index, int(bytes));
  if (!Take(count, bytes))
    return Step::again;
  m_send->Send(count, int(bytes));
  return Step::done;
}

// No other machine answered. That is only right when this machine is linked
// to itself; otherwise two machines are probably both set to master.
bool CSimNetBoard::AloneByMistake(unsigned others) const
{
  return others == 0 && (m_portIn != m_portOut || m_addressOut != "127.0.0.1");
}

void CSimNetBoard::LinkType1()
{
  m_reg88++;    // these games want this to move every frame
  if (!m_linked)
    return;

  const bool master = Ram(0x400) == 0;
  Step step = CheckBuild(master);
  if (step == Step::again)
    return;
  if (step == Step::failed)
  {
    ErrorLog("unable to verify connection. Make sure all machines are using same build!");
    m_state = State::error;
    return;
  }

  uint8_t index = 0, others = 0;
  if (CountMachines(master, 1, &index, &others, true) == Step::again)
    return;
  if (AloneByMistake(others))
  {
    ErrorLog("no slave machines detected. Make sure only one machine is set to master!");
    m_state = State::error;
    return;
  }

  m_machines = uint8_t(others + 1);
  m_reg88 = 0;
  m_reg8a = uint16_t(0x2021 + others * 0x20 + index);

  SetShared(0x0, Ram(0x400));
  SetShared(0x2, others);
  SetShared(0x4, index);
  m_frame = 0;
  SetShared(0x6, 0);

  m_slice = Ram(0x404);
  const int top = Ram(0x402);
  SetShared(0x8, FLIPENDIAN16(uint16_t(0x100 + m_slice)));
  SetShared(0xa, FLIPENDIAN16(uint16_t(top - m_slice - 1)));
  SetShared(0xc, FLIPENDIAN16(0x100));
  SetShared(0xe, FLIPENDIAN16(uint16_t(top - m_slice + 0x200)));
  m_state = State::ready;
}

void CSimNetBoard::LinkType2()
{
  if (!m_linked)
    return;

  // 0 master, below 0x8000 a playable slave, otherwise a relay/satellite
  const uint16_t role = Ram(0x200);
  const bool master = role == 0;
  const bool relay = role >= 0x8000;

  Step step = CheckBuild(master);
  if (step == Step::again)
    return;
  if (step == Step::failed)
  {
    ErrorLog("unable to verify connection. Make sure all machines are using same build!");
    m_state = State::error;
    return;
  }

  // byte 0: all machines, byte 1: playable ones
  uint8_t index[2] = { 0, 0 }, others[2] = { 0, 0 };
  if (CountMachines(master, 2, index, others, !relay) == Step::again)
    return;
  const bool dirt = Is("dirtdvls");
  if (relay && !dirt)
    index[1] |= 0x80;

  if (AloneByMistake(others[0]))
  {
    ErrorLog("no slave machines detected. Make sure only one machine is set to master!");
    if (dirt)
      m_reg8a = 0x8085;
    m_state = State::error;
    return;
  }

  m_machines = uint8_t(others[0] + 1);
  m_reg88 = 5;
  if (dirt)
    m_reg8a = uint16_t((others[1] << 4) | index[1] | 0x7400);
  else
    m_reg8a = uint16_t((others[1] << 8) | index[1]);

  SetShared(0x0, role);
  SetShared(0x2, uint16_t((others[1] << 8) | others[0]));
  SetShared(0x4, uint16_t((index[1] << 8) | index[0]));
  m_frame = 0;
  SetShared(0x6, 0);

  m_slice = Ram(0x204);
  const int top = Ram(0x206);
  SetShared(0x8, FLIPENDIAN16(uint16_t(0x100 + m_slice)));
  SetShared(0xa, FLIPENDIAN16(uint16_t(top)));
  SetShared(0xc, FLIPENDIAN16(0x100));
  SetShared(0xe, FLIPENDIAN16(uint16_t(top + 0x80)));
  m_state = State::ready;
}

// One frame of linked play: each machine's slice goes once round the ring
// (we get our own back too), then the banks swap.
void CSimNetBoard::Exchange()
{
  m_frame++;
  SetShared(0x6, FLIPENDIAN16(m_frame));

  for (int i = 0; i < m_machines; i++)
  {
    m_send->Send(m_local + 0x100 + i * m_slice, m_slice);
    const std::vector<char> &msg = m_receive->Receive();
    if (msg.empty())
    {
      m_send->Send(nullptr, 0);   // pass the bad news on
      m_state = State::error;
      if (m_type == GameType::one)
        m_reg8a = 0x40;           // the main board shows "link broken"
      break;
    }
    memcpy(m_local + 0x100 + (i + 1) * m_slice, msg.data(), msg.size());
  }

  m_bank = !m_bank;
  m_local = m_buffer + (m_bank ? 0x10000 : 0);
  m_external = m_buffer + (m_bank ? 0 : 0x10000);
}

void CSimNetBoard::RunFrame(void)
{
  if (!IsRunning())
    return;

  switch (m_state)
  {
  case State::start:
    if (!m_linked && !m_connector.joinable())
      m_connector = std::thread(&CSimNetBoard::Connect, this);
    m_reg88 = 0;
    m_reg8a = Is("dirtdvls") ? 0x4004 : 0xe000;
    m_state = State::init;
    break;

  case State::init:
    // 128 KB from the bank in use (when that is the upper bank this runs on
    // into the board RAM that follows it in memory, as it always has).
    memset(m_local, 0, 0x20000);
    if (m_type == GameType::one)
    {
      // Each time the main board writes the register, acknowledge as IRQ 2
      // would; 0xf000 means it is done.
      if (m_reg88 & 0x8000)
      {
        m_reg00 |= 0x01;
        if (m_reg88 == 0xf000)
        {
          m_reg8a = 0;
          SetShared(0x72, FLIPENDIAN16(0x1));
          m_state = State::testing;
        }
        m_reg88 = 0;
      }
    }
    else
    {
      // type 2 games set themselves up
      m_reg8a = 0;
      m_state = State::testing;
      m_frame = 0;
    }
    break;

  case State::testing:
    if (m_type == GameType::one)
      LinkType1();
    else
      LinkType2();
    break;

  case State::ready:
    Exchange();
    break;

  case State::error:
    break;
  }
}

uint8_t CSimNetBoard::ReadCommRAM8(unsigned addr)
{
  return m_external[addr];
}

uint16_t CSimNetBoard::ReadCommRAM16(unsigned addr)
{
  uint16_t v;
  memcpy(&v, m_external + addr, sizeof(v));
  return v;
}

uint32_t CSimNetBoard::ReadCommRAM32(unsigned addr)
{
  uint32_t v;
  memcpy(&v, m_external + addr, sizeof(v));
  return v;
}

void CSimNetBoard::WriteCommRAM8(unsigned addr, uint8_t data)
{
  m_external[addr] = data;
}

void CSimNetBoard::WriteCommRAM16(unsigned addr, uint16_t data)
{
  memcpy(m_external + addr, &data, sizeof(data));
}

void CSimNetBoard::WriteCommRAM32(unsigned addr, uint32_t data)
{
  memcpy(m_external + addr, &data, sizeof(data));
}

uint16_t CSimNetBoard::ReadIORegister(unsigned reg)
{
  if (!IsRunning())
    return 0;
  switch (reg)
  {
  case 0x00: return m_reg00;
  case 0x88: return m_reg88;
  case 0x8a: return m_reg8a;
  default:
    ErrorLog("read from unknown IO register 0x%02x", reg);
    return 0;
  }
}

void CSimNetBoard::WriteIORegister(unsigned reg, uint16_t data)
{
  switch (reg)
  {
  case 0x00: m_reg00 = data; break;
  case 0x88: m_reg88 = data; break;
  case 0x8a: m_reg8a = data; break;
  case 0xc0:
    // 0 stops the board (and resets it), anything else starts it
    if (data == 0)
      Reset();
    m_running = data != 0;
    break;
  default:
    ErrorLog("write to unknown IO register 0x%02x", reg);
    break;
  }
}
