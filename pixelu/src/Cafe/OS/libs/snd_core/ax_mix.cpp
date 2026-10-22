// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// The voice renderer: what the DSP does for each voice in a frame.
//
//   decode (ADPCM / PCM16 / PCM8, from guest memory)
//   -> resample to the renderer rate (linear interpolation, 16.16 step)
//   -> volume envelope -> biquad -> one-pole low pass
//   -> add into the main and aux buses of TV and GamePad (per-bus volume ramps)
//
// Samples are carried as floats scaled to 24 bits (16-bit value << 8). After
// all voices, the aux sends are handed to the aux effects, the effects'
// returns from the previous frame are added back, and the main buses are
// summed into the device mixes ax_ist.cpp picks up.
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "config/ActiveSettings.h"

void mic_updateOnAXFrame();

namespace snd_core
{
	uint16 __AXTVAuxReturnVolume[AX_AUX_BUS_COUNT];

	namespace
	{
		constexpr uint32 kOne = 0x10000;            // 1.0 in 16.16
		constexpr uint16 kUnityVolume = 0x8000;     // 1.0 in 1.15
		constexpr float kVolumeScale = 32768.0f;
		constexpr sint32 kMaxDecodeAhead = 4096;    // input samples one voice may consume per frame

		uint32 s_defaultMixerSelect = AX_MIXER_SELECT_BOTH;

		// bus lanes, [bus][channel][sample], channel stride = samples per frame
		float s_tvLanes[AX_MAX_NUM_BUS * AX_TV_CHANNEL_COUNT * AX_SAMPLES_MAX];
		float s_drcLanes[AX_MAX_NUM_BUS * AX_DRC_CHANNEL_COUNT * AX_SAMPLES_MAX];

		uint16 Be16(uint16 raw) { return _swapEndianU16(raw); }
		sint16 Be16s(uint16 raw) { return (sint16)_swapEndianU16(raw); }

		// float -> integer the way x86 converts (truncate to 32 bits, then narrow)
		sint16 ToS16Wrap(float f) { return (sint16)(sint32)f; }
		uint16 ToU16Wrap(float f) { return (uint16)(sint32)f; }

		sint32 AddWrap(sint32 a, sint32 b) { return (sint32)((uint32)a + (uint32)b); }

		// --- sample sources -----------------------------------------------------------

		// PCM8 / PCM16 straight from guest memory. A source that runs off its end
		// without a loop clears the voice's playback state.
		struct PcmSource
		{
			AXVPBInternal_t* pb;
			bool wide;   // PCM16
			uint32 pos;  // byte addresses
			uint32 end;
			uint32 loop;
			sint32 held = 0; // last PCM16 sample read in this frame
			bool justEnded = false;

			PcmSource(AXVPBInternal_t* block, bool pcm16) : pb(block), wide(pcm16)
			{
				const uint32 ext = (uint32)pb->addr.addrExt << 29;
				const uint32 unit = wide ? 2 : 1;
				pos = (pb->addr.Cur() * unit) | ext;
				end = (pb->addr.End() * unit) | ext;
				loop = (pb->addr.Loop() * unit) | ext;
			}

			bool Playing() const { return pb->playbackState != 0; }

			sint32 Next()
			{
				justEnded = false;
				if (!Playing())
					return wide ? held : 0; // PCM16 holds its last value, PCM8 falls silent
				sint32 s;
				if (wide)
					s = held = (sint16)_swapEndianU16(*(uint16*)(memory_base + pos));
				else
					s = (sint32)(sint8)memory_base[pos] * 256;
				if (pos == end)
				{
					if (pb->addr.loopFlag != 0)
						pos = loop;
					else
					{
						pb->playbackState = 0;
						justEnded = true;
					}
				}
				else
					pos += wide ? 2 : 1;
				return s;
			}

			void Store()
			{
				uint32 word = pos & 0x1FFFFFFF;
				if (wide)
					word >>= 1;
				pb->addr.SetCur(word);
			}
		};

		// 4-bit DSPADPCM: frames of 8 bytes, a predictor/scale header byte
		// followed by 14 nibbles. Positions are nibble offsets from the sample
		// buffer named in the voice handle, so the header occupies nibbles 0-1.
		struct AdpcmSource
		{
			AXVPBInternal_t* pb;
			const uint8* data;
			bool active;
			uint32 pos;
			uint32 startPos;
			uint32 end;
			uint32 loopPos;
			uint32 startWord;
			uint16 header;
			sint32 coef1 = 0;
			sint32 coef2 = 0;
			uint32 shift = 0;
			sint32 hist1;
			sint32 hist2;

			explicit AdpcmSource(AXVPBInternal_t* block) : pb(block)
			{
				const AXVPB& handle = __AXVPBArrayPtr[(uint32)pb->index];
				active = pb->playbackState != 0;
				data = memory_getPointerFromVirtualOffset(_swapEndianU32(handle.offsets.samples));
				const uint32 loopOffset = _swapEndianU32(handle.offsets.loopOffset);
				end = _swapEndianU32(handle.offsets.endOffset);
				// an end offset on a frame header can never be reached (a title using
				// 0x40000 jumped from 0x3FFFF straight to 0x40002); move it onto the first sample
				if ((end & 0xF) <= 1)
					end += 2;
				startWord = pb->addr.Cur();
				pos = startPos = loopOffset + (startWord - pb->addr.Loop());
				loopPos = loopOffset;
				SetHeader(Be16(pb->adpcm.scale));
				hist1 = Be16s(pb->adpcm.yn1);
				hist2 = Be16s(pb->adpcm.yn2);
			}

			void SetHeader(uint16 ps)
			{
				header = ps;
				shift = ps & 0xF;
				const uint32 pair = (ps >> 4) & 7;
				coef1 = Be16s(pb->adpcm.a[pair * 2 + 0]);
				coef2 = Be16s(pb->adpcm.a[pair * 2 + 1]);
			}

			sint32 Next()
			{
				if (!active || pb->playbackState == 0)
					return 0;
				if ((pos & 0xF) == 0)
				{
					SetHeader(data[pos >> 1]);
					pos += 2;
				}
				const uint8 byte = data[pos >> 1];
				const sint32 nibble = (pos & 1) ? (sint32)(sint8)(uint8)(byte << 4) >> 4 : (sint32)(sint8)byte >> 4;
				const sint64 predicted = ((sint64)nibble << (11 + shift)) + (sint64)hist1 * coef1 + (sint64)hist2 * coef2;
				const sint32 s = (sint32)std::clamp<sint64>((predicted + 0x400) >> 11, -32768, 32767);
				hist2 = hist1;
				hist1 = s;
				if (pos == end)
				{
					if (pb->addr.loopFlag != 0)
					{
						SetHeader(Be16(pb->adpcmLoop.loopScale));
						pos = loopPos;
					}
					else
						pb->playbackState = 0;
				}
				else
					pos++;
				return s;
			}

			void Store()
			{
				if (!active)
					return;
				pb->adpcm.scale = Be16(header);
				pb->adpcm.yn1 = Be16((uint16)(sint16)hist1);
				pb->adpcm.yn2 = Be16((uint16)(sint16)hist2);
				pb->addr.SetCur(startWord + (pos - startPos));
			}
		};

		// --- resampling -----------------------------------------------------------------

		// The last four input samples, oldest first. The DSP block keeps them in
		// one of two arrangements: ADPCM and PCM8 store the newest sample first
		// and interpolate between the two newest; PCM16 stores them oldest first
		// and interpolates between the two oldest (three samples later).
		struct History
		{
			sint32 s[4]; // s[3] newest
			bool lagged;

			History(const AXPBSRC_t& src, bool pcm16) : lagged(pcm16)
			{
				sint32 stored[4];
				for (sint32 i = 0; i < 4; i++)
					stored[i] = Be16s(src.historySamples[i]);
				if (lagged)
				{
					for (sint32 i = 0; i < 4; i++)
						s[i] = stored[i];
				}
				else
				{
					s[3] = stored[0];
					s[0] = stored[1];
					s[1] = stored[2];
					s[2] = stored[3];
				}
			}

			void Push(sint32 v)
			{
				s[0] = s[1];
				s[1] = s[2];
				s[2] = s[3];
				s[3] = (sint16)v;
			}

			sint32 From() const { return lagged ? s[0] : s[2]; }
			sint32 To() const { return lagged ? s[1] : s[3]; }

			void Store(AXPBSRC_t& src) const
			{
				const sint32 order[4] = { lagged ? s[0] : s[3], lagged ? s[1] : s[0], lagged ? s[2] : s[1], lagged ? s[3] : s[2] };
				for (sint32 i = 0; i < 4; i++)
					src.historySamples[i] = Be16((uint16)(sint16)order[i]);
			}
		};

		template<typename TSource>
		void ResampleLinear(AXVPBInternal_t* pb, TSource& source, bool pcm16, float* out, sint32 count)
		{
			uint32 frac = Be16(pb->src.currentFrac);
			const uint32 step = pb->src.Ratio();
			History h(pb->src, pcm16);
			for (sint32 i = 0; i < count; i++)
			{
				frac += step;
				while (frac >= kOne)
				{
					h.Push(source.Next());
					frac -= kOne;
				}
				const sint32 a = (h.From() * (sint32)(kOne - frac)) >> 7;
				const sint32 b = (h.To() * (sint32)frac) >> 7;
				out[i] = (float)((a + b) >> 1);
			}
			pb->src.currentFrac = Be16((uint16)frac);
			h.Store(pb->src);
		}

		void RenderAdpcm(AXVPBInternal_t* pb, bool resample, float* out, sint32 count)
		{
			if (resample)
			{
				const uint32 needed = (Be16(pb->src.currentFrac) + pb->src.Ratio() * (uint32)count) >> 16;
				if ((sint32)needed >= kMaxDecodeAhead)
				{
					cemuLog_log(LogType::Force, "AX: voice {} resample ratio {:08x} is too high, muted", (uint32)pb->index, pb->src.Ratio());
					std::fill(out, out + count, 0.0f);
					return;
				}
			}
			AdpcmSource source(pb);
			if (resample)
				ResampleLinear(pb, source, false, out, count);
			else
			{
				for (sint32 i = 0; i < count; i++)
					out[i] = (float)(source.Next() * 256);
			}
			source.Store();
		}

		void RenderPcm(AXVPBInternal_t* pb, bool pcm16, bool resample, float* out, sint32 count)
		{
			PcmSource source(pb, pcm16);
			if (resample)
			{
				ResampleLinear(pb, source, pcm16, out, count);
			}
			else
			{
				if (!source.Playing())
				{
					std::fill(out, out + count, 0.0f);
					return;
				}
				for (sint32 i = 0; i < count; i++)
				{
					if (!source.Playing())
					{
						out[i] = 0.0f;
						continue;
					}
					const sint32 s = source.Next();
					// the sample that ends a non-looping voice is not played
					out[i] = source.justEnded ? 0.0f : (float)(s * 256);
				}
			}
			source.Store();
		}

		void RenderSamples(AXVPBInternal_t* pb, float* out, sint32 count)
		{
			const uint16 mode = pb->srcMode;
			const uint16 format = pb->addr.format;
			if (mode != AX_FILTER_MODE_TAP && mode != AX_FILTER_MODE_LINEAR && mode != AX_FILTER_MODE_NONE)
			{
				std::fill(out, out + count, 0.0f);
				return;
			}
			// the tap (polyphase) filters are approximated by linear interpolation
			const bool resample = mode != AX_FILTER_MODE_NONE;
			switch (format)
			{
			case AX_FORMAT_ADPCM: RenderAdpcm(pb, resample, out, count); break;
			case AX_FORMAT_PCM16: RenderPcm(pb, true, resample, out, count); break;
			case AX_FORMAT_PCM8: RenderPcm(pb, false, resample, out, count); break;
			default: std::fill(out, out + count, 0.0f); break;
			}
		}

		// --- per-voice processing -------------------------------------------------------

		// sndcore2 limits enveloped samples to the 24-bit range when the volume can exceed 1.0
		float Limit24(float x)
		{
			return std::min(8388352.0f, std::max(-8388608.0f, x));
		}

		void ApplyEnvelope(AXVPBInternal_t* pb, float* x, sint32 count)
		{
			const uint16 volume = pb->veVolume;
			const sint16 delta = (sint16)(uint16)pb->veDelta;
			if (volume == kUnityVolume && delta == 0)
				return;
			const uint16 endVolume = (uint16)((sint32)volume + delta * count);
			const bool limit = sndGeneric.isSoundCore2 && !(volume <= kUnityVolume && endVolume <= kUnityVolume);
			float gain = (float)volume / kVolumeScale;
			if (delta == 0)
			{
				for (sint32 i = 0; i < count; i++)
					x[i] = limit ? Limit24(x[i] * gain) : x[i] * gain;
				return;
			}
			const float step = (float)delta / kVolumeScale;
			for (sint32 i = 0; i < count; i++)
			{
				gain += step;
				x[i] = limit ? Limit24(x[i] * gain) : x[i] * gain;
			}
			pb->veVolume = endVolume;
		}

		// y = b0 x + b1 x[-1] + b2 x[-2] + a1 y[-1] + a2 y[-2], on 16-bit scaled samples,
		// coefficients 2.14 (a1/a2 are stored with the sign the sum needs)
		void ApplyBiquad(AXVPBInternal_t* pb, float* x, sint32 count)
		{
			AXPBBIQUAD_t& f = pb->biquad;
			if (f.on == AX_BIQUAD_OFF)
				return;
			const float b0 = (float)Be16s(f.b0) / 16384.0f;
			const float b1 = (float)Be16s(f.b1) / 16384.0f;
			const float b2 = (float)Be16s(f.b2) / 16384.0f;
			const float a1 = (float)Be16s(f.a1) / 16384.0f;
			const float a2 = (float)Be16s(f.a2) / 16384.0f;
			float x1 = (float)Be16s((uint16)f.xn1);
			float x2 = (float)Be16s((uint16)f.xn2);
			float y1 = (float)Be16s((uint16)f.yn1);
			float y2 = (float)Be16s((uint16)f.yn2);
			for (sint32 i = 0; i < count; i++)
			{
				const float in = x[i] / 256.0f;
				const float y = b0 * in + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2;
				x[i] = y * 256.0f;
				x2 = x1;
				x1 = in;
				y2 = y1;
				y1 = std::max(-32768.0f, std::min(32767.0f, y)); // feedback saturates at 16 bits
			}
			f.xn1 = (sint16)Be16((uint16)ToS16Wrap(x1));
			f.xn2 = (sint16)Be16((uint16)ToS16Wrap(x2));
			f.yn1 = (sint16)Be16((uint16)ToS16Wrap(y1));
			f.yn2 = (sint16)Be16((uint16)ToS16Wrap(y2));
		}

		// y = a0 x - b0 y[-1], coefficients 0.15, state kept on the 16-bit scale
		void ApplyLowPass(AXVPBInternal_t* pb, float* x, sint32 count)
		{
			AXPBLPF_t& f = pb->lpf;
			if (f.on == AX_LPF_OFF)
				return;
			const float a0 = (float)Be16s((uint16)f.a0) / 32767.0f;
			const float b0 = (float)Be16s((uint16)f.b0) / 32767.0f;
			float y = (float)Be16s((uint16)f.yn1) * 256.0f / 32767.0f;
			for (sint32 i = 0; i < count; i++)
			{
				y = a0 * x[i] - b0 * y;
				x[i] = y;
			}
			f.yn1 = (sint16)Be16((uint16)ToS16Wrap(y / 256.0f * 32767.0f));
		}

		// add a voice into one bus lane with its volume (ramping by 'delta' per
		// sample); the volume reached is written back for the next frame
		void AddToLane(const float* in, float* lane, sint32 count, AXCHMIX_DEPR& mix)
		{
			const sint16 delta = Be16s((uint16)mix.delta);
			float volume = (float)Be16(mix.vol) / kVolumeScale;
			if (delta != 0)
			{
				const float step = (float)delta / kVolumeScale;
				for (sint32 i = 0; i < count; i++)
				{
					volume += step;
					lane[i] += in[i] * volume;
				}
			}
			else
			{
				for (sint32 i = 0; i < count; i++)
					lane[i] += in[i] * volume;
			}
			mix.vol = Be16(ToU16Wrap(volume * kVolumeScale));
		}

		float* TvLane(sint32 bus, sint32 channel, sint32 stride)
		{
			return s_tvLanes + (bus * AX_TV_CHANNEL_COUNT + channel) * stride;
		}

		float* DrcLane(sint32 bus, sint32 channel, sint32 stride)
		{
			return s_drcLanes + (bus * AX_DRC_CHANNEL_COUNT + channel) * stride;
		}

		uint32 MixBits(const uint16be& busMask, sint32 channel)
		{
			return ((uint32)(uint16)busMask >> (channel * 2)) & 3;
		}

		void MixVoice(AXVPBInternal_t* pb, const float* samples, sint32 count)
		{
			for (sint32 bus = 0; bus < AX_MAX_NUM_BUS; bus++)
			{
				for (sint32 ch = 0; ch < AX_TV_CHANNEL_COUNT; ch++)
				{
					uint16& now = pb->tvVolumeNow[bus * AX_TV_CHANNEL_COUNT + ch];
					if (MixBits(pb->mixMaskTV[bus], ch) == 0)
					{
						now = 0;
						continue;
					}
					AXCHMIX_DEPR& mix = pb->deviceMixTV[ch * AX_MAX_NUM_BUS + bus];
					AddToLane(samples, TvLane(bus, ch, count), count, mix);
					now = mix.vol;
				}
			}
			// first GamePad only; the second GamePad and the remote speakers are not rendered
			for (sint32 bus = 0; bus < AX_MAX_NUM_BUS; bus++)
			{
				for (sint32 ch = 0; ch < AX_DRC_CHANNEL_COUNT; ch++)
				{
					if (MixBits(pb->mixMaskDRC[bus], ch) == 0)
						continue;
					AddToLane(samples, DrcLane(bus, ch, count), count, pb->deviceMixDRC[ch * AX_MAX_NUM_BUS + bus]);
				}
			}
		}

		void RenderVoices(AXVPBInternal_t* first, sint32 count)
		{
			float buffer[AX_SAMPLES_MAX];
			for (AXVPBInternal_t* pb = first; pb; pb = pb->nextToProcess.GetPtr())
			{
				RenderSamples(pb, buffer, count);
				ApplyEnvelope(pb, buffer, count);
				ApplyBiquad(pb, buffer, count);
				ApplyLowPass(pb, buffer, count);
				MixVoice(pb, buffer, count);
			}
		}

		// --- buses ------------------------------------------------------------------------

		// aux sends go to the effects at 16-bit scale
		void SendAux(sint32 count)
		{
			const sint32 n = count * AX_TV_CHANNEL_COUNT;
			for (sint32 aux = 0; aux < AX_AUX_BUS_COUNT; aux++)
			{
				sint32be* send = AXAux_GetInputBuffer(AX_DEV_TV, 0, aux);
				if (!send)
					continue;
				const float* lanes = TvLane(1 + aux, 0, count);
				for (sint32 i = 0; i < n; i++)
					send[i] = (sint32)lanes[i] >> 8;
			}
		}

		void AddMainToDevice(const float* lanes, sint32be* out, sint32 samples)
		{
			constexpr float kMasterGain = (float)kUnityVolume / kVolumeScale;
			for (sint32 i = 0; i < samples; i++)
				out[i] = AddWrap(out[i], (sint32)(lanes[i] * kMasterGain));
		}

		void FinishTV(sint32 count)
		{
			const sint32 n = count * AX_TV_CHANNEL_COUNT;
			float* main = TvLane(0, 0, count);
			if (ActiveSettings::AudioOutputOnlyAux())
				std::fill(main, main + n, 0.0f); // debug option: hear only what the effects return
			for (sint32 aux = 0; aux < AX_AUX_BUS_COUNT; aux++)
			{
				const sint32be* ret = AXAux_GetOutputBuffer(AX_DEV_TV, 0, aux);
				if (!ret)
					continue;
				const float gain = (float)__AXTVAuxReturnVolume[aux] / kVolumeScale;
				for (sint32 i = 0; i < n; i++)
					main[i] += (float)((sint32)ret[i] << 8) * gain;
			}
			AddMainToDevice(main, AXIst_TVMix(), n);
		}

		void FinishDRC(sint32 count)
		{
			AddMainToDevice(DrcLane(0, 0, count), AXIst_DRCMix(), count * AX_DRC_CHANNEL_COUNT);
		}
	}

	void AXSetDefaultMixerSelect(uint32 mixerSelect)
	{
		s_defaultMixerSelect = mixerSelect;
	}

	uint32 AXGetDefaultMixerSelect()
	{
		return s_defaultMixerSelect;
	}

	void AXMix_Init()
	{
		// everything is rendered on the host, so voices default to the PPC mixer
		AXSetDefaultMixerSelect(AX_MIXER_SELECT_PPC);
	}

	void AXMix_process(AXVPBInternal_t* firstDspBlock)
	{
		const sint32 count = AXGetInputSamplesPerFrame();
		std::fill(std::begin(s_tvLanes), std::end(s_tvLanes), 0.0f);
		std::fill(std::begin(s_drcLanes), std::end(s_drcLanes), 0.0f);

		RenderVoices(firstDspBlock, count);
		AXAux_Process(); // effects process last frame's sends
		AXIst_HandleFrameCallbacks();
		SendAux(count);
		FinishTV(count);
		FinishDRC(count);
		AXAux_incrementBufferIndex();
		mic_updateOnAXFrame();
	}
}
