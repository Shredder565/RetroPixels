// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Typed views of 32-bit hardware registers. A register value is kept in host
// order; MMU::RegisterMMIO_32 converts between raw values and these types.

namespace HWREG
{
	class HWREG
	{
	  public:
		uint32 getRawValue() const { return v; }
		uint32 getRawValueBE() const { return _swapEndianU32(v); }
		void setFromRaw(uint32 regValue) { v = regValue; }

	  protected:
		template<uint32 TFirstBit, uint32 TBitCount>
		static constexpr uint32 FieldMask()
		{
			static_assert(TBitCount >= 1 && TFirstBit + TBitCount <= 32);
			return (TBitCount == 32 ? 0xFFFFFFFFu : ((1u << TBitCount) - 1u)) << TFirstBit;
		}

		template<uint32 TFirstBit, uint32 TBitCount>
		uint32 ReadField() const
		{
			return (v & FieldMask<TFirstBit, TBitCount>()) >> TFirstBit;
		}

		template<uint32 TFirstBit, uint32 TBitCount>
		void WriteField(uint32 fieldValue)
		{
			constexpr uint32 mask = FieldMask<TFirstBit, TBitCount>();
			cemu_assert_debug(((fieldValue << TFirstBit) & ~mask) == 0);
			v = (v & ~mask) | ((fieldValue << TFirstBit) & mask);
		}

		uint32 v{};
	};

	// ACR block (0x0D000000 interface). Titles that run in Wii U mode reach the
	// legacy video interface through this select/data register pair.

	struct ACR_VI_DATA : HWREG // +0x21C
	{
		uint32 Data() const { return v; }
		void SetData(uint32 value) { v = value; }
	};

	struct ACR_VI_ADDR : HWREG // +0x224
	{
		uint32 Index() const { return v; }
		void SetIndex(uint32 value) { v = value; }
	};

	struct ACR_VI_CTRL : HWREG // +0x228
	{
		bool Owned() const { return ReadField<0, 1>() != 0; }
		void SetOwned(bool owned) { WriteField<0, 1>(owned ? 1 : 0); }
	};

	// Serial interface (GameCube style joybus ports, 4 channels)

	struct SICOUTBUF : HWREG // +0x6400 + channel * 0xC
	{
		uint32 Output1() const { return ReadField<0, 8>(); }
		uint32 Output0() const { return ReadField<8, 8>(); }
		uint32 Command() const { return ReadField<16, 8>(); }
	};

	struct SICINBUFH : HWREG // +0x6404 + channel * 0xC
	{
		uint32 Input3() const { return ReadField<0, 8>(); }
		uint32 Input2() const { return ReadField<8, 8>(); }
		uint32 Input1() const { return ReadField<16, 8>(); }
		uint32 Input0() const { return ReadField<24, 6>(); }
		uint32 ErrorLatch() const { return ReadField<30, 1>(); }
		uint32 ErrorStatus() const { return ReadField<31, 1>(); }
	};

	struct SICINBUFL : HWREG // +0x6408 + channel * 0xC
	{
		uint32 Input4() const { return ReadField<0, 8>(); }
		uint32 Input5() const { return ReadField<8, 8>(); }
		uint32 Input6() const { return ReadField<16, 8>(); }
		uint32 Input7() const { return ReadField<24, 8>(); }
	};

	struct SIPOLL : HWREG // +0x6430
	{
		uint32 CopyOnVBlank() const { return ReadField<0, 4>(); }
		uint32 EnableMask() const { return ReadField<4, 4>(); } // bit 3 = channel 0 ... bit 0 = channel 3
		uint32 LinesY() const { return ReadField<8, 8>(); }
		uint32 LinesX() const { return ReadField<16, 10>(); }

		bool ChannelPolled(uint32 channel) const { return ((EnableMask() >> (3 - channel)) & 1) != 0; }
	};

	struct SICOMCSR : HWREG // +0x6434
	{
		bool TransferStart() const { return ReadField<0, 1>() != 0; }
		uint32 Channel() const { return ReadField<1, 2>(); }
		uint32 InLength() const { return ReadField<8, 7>(); }
		uint32 OutLength() const { return ReadField<16, 7>(); }
		bool TransferCompleteIrq() const { return ReadField<31, 1>() != 0; }
		void SetTransferCompleteIrq(bool set) { WriteField<31, 1>(set ? 1 : 0); }
	};

	struct SISR : HWREG // +0x6438, one byte per channel (channel 0 in the top byte)
	{
		// per-channel status bits, relative to the channel's byte
		enum : uint32
		{
			UNDERRUN = 0,
			OVERRUN = 1,
			COLLISION = 2,
			NO_RESPONSE = 3,
			WRITE_STATUS = 4,
			READ_STATUS = 5,
		};

		bool ChannelFlag(uint32 channel, uint32 flag) const { return ((v >> (ChannelShift(channel) + flag)) & 1) != 0; }
		void SetChannelFlag(uint32 channel, uint32 flag) { v |= (1u << (ChannelShift(channel) + flag)); }
		bool WriteBuffers() const { return ReadField<31, 1>() != 0; }

	  private:
		static uint32 ChannelShift(uint32 channel) { return (3 - channel) * 8; }
	};
}
