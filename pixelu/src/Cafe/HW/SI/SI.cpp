// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Common/HwReg.h"
#include "si.h"

// Serial interface (GameCube controller ports). No controller is ever attached,
// so every channel that software polls reports "no response". Transfers are not
// emulated.

namespace HW_SI
{
	namespace
	{
		constexpr uint32 kChannels = 4;

		struct ChannelState
		{
			HWREG::SICOUTBUF outputRegister; // as written by software
			uint8 latchedCommand{};          // copied on SISR.WR
			uint8 latchedOutput[2]{};
			bool noResponse{};
		};

		ChannelState s_channel[kChannels];
		HWREG::SIPOLL s_poll;
		HWREG::SICOMCSR s_comcsr;

		uint32 ChannelFromAddress(PAddr addr)
		{
			return ((addr & 0xFF) / 0xC) % kChannels;
		}

		// Polling happens continuously on hardware. We evaluate it lazily whenever
		// the status register is read: an enabled channel without a device always
		// ends up flagging "no response".
		void UpdatePolling()
		{
			for (uint32 ch = 0; ch < kChannels; ch++)
			{
				if (s_poll.ChannelPolled(ch))
					s_channel[ch].noResponse = true;
			}
		}

		HWREG::SICOUTBUF ReadOutBuf(PAddr)
		{
			cemu_assert_debug(false); // write-only register
			return {};
		}

		void WriteOutBuf(PAddr addr, HWREG::SICOUTBUF value)
		{
			s_channel[ChannelFromAddress(addr)].outputRegister = value;
		}

		HWREG::SIPOLL ReadPoll(PAddr)
		{
			cemu_assert_debug(false); // not expected to be read back
			return s_poll;
		}

		void WritePoll(PAddr, HWREG::SIPOLL value)
		{
			s_poll = value;
		}

		HWREG::SICOMCSR ReadComCsr(PAddr)
		{
			return s_comcsr;
		}

		void WriteComCsr(PAddr, HWREG::SICOMCSR value)
		{
			// The stored control value never changes because no transfer is ever
			// started; writing 1 to the transfer-complete bit acknowledges it.
			if (value.TransferCompleteIrq())
				s_comcsr.SetTransferCompleteIrq(false);
			if (value.TransferStart())
				cemu_assert_debug(false); // joybus transfers are not emulated
		}

		HWREG::SISR ReadStatus(PAddr)
		{
			UpdatePolling();
			HWREG::SISR status;
			for (uint32 ch = 0; ch < kChannels; ch++)
			{
				if (s_channel[ch].noResponse)
					status.SetChannelFlag(ch, HWREG::SISR::NO_RESPONSE);
			}
			return status;
		}

		void WriteStatus(PAddr, HWREG::SISR value)
		{
			// writing 1 to an error bit clears it
			for (uint32 ch = 0; ch < kChannels; ch++)
			{
				if (value.ChannelFlag(ch, HWREG::SISR::NO_RESPONSE))
					s_channel[ch].noResponse = false;
			}
			// WR latches the output buffers of all channels
			if (value.WriteBuffers())
			{
				for (auto& ch : s_channel)
				{
					ch.latchedCommand = (uint8)ch.outputRegister.Command();
					ch.latchedOutput[0] = (uint8)ch.outputRegister.Output0();
					ch.latchedOutput[1] = (uint8)ch.outputRegister.Output1();
				}
			}
		}
	}

	void Initialize()
	{
		using MMU::MMIOInterface;
		for (uint32 ch = 0; ch < kChannels; ch++)
			MMU::RegisterMMIO_32<HWREG::SICOUTBUF, ReadOutBuf, WriteOutBuf>(MMIOInterface::INTERFACE_0D000000, 0x6400 + ch * 0xC);
		MMU::RegisterMMIO_32<HWREG::SIPOLL, ReadPoll, WritePoll>(MMIOInterface::INTERFACE_0D000000, 0x6430);
		MMU::RegisterMMIO_32<HWREG::SICOMCSR, ReadComCsr, WriteComCsr>(MMIOInterface::INTERFACE_0D000000, 0x6434);
		MMU::RegisterMMIO_32<HWREG::SISR, ReadStatus, WriteStatus>(MMIOInterface::INTERFACE_0D000000, 0x6438);
	}
}
