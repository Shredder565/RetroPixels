// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// PixelU game recorder for RetroPixels (--rp-record <dumpDir> --rp-ffmpeg <ffmpeg>)
//
// Video: the TV scan buffer (the game's own picture at its render resolution, without any overlay)
// is read back by the renderer and piped as raw RGBA into ffmpeg, which writes
// <dumpDir>/Frames/pixelu_framedump<N>.mkv (libx264, constant 60 fps). A new file is started whenever the picture size changes.
// Audio: the TV mix as it is handed to the host audio device (before host volume) is written to
// <dumpDir>/Audio/pixelu_dspdump.wav (48 kHz, signed 16-bit, stereo).
// Both streams are kept on the same wall clock: video frames are duplicated or dropped and silence is inserted into the audio when needed.

class LatteTextureView;

namespace LatteRecorder
{
	enum class ColorConversion : uint8
	{
		None,
		SRGBToLinear, // stored values are sRGB encoded but the output shader shows them linear
		LinearToSRGB, // stored values are linear but the output shader applies sRGB encoding
	};

	// true if both --rp-record and --rp-ffmpeg were given
	bool IsEnabled();

	// start or stop a recording session (game launch / game shutdown / app exit). Stop is safe to call at any time
	void Start();
	void Stop();
	bool IsRecording();

	// called on the GPU thread whenever the game copies a color buffer to the TV scan buffer
	void OnTVScanBufferCopy(LatteTextureView* texView);

	// used by the renderer backends to hand over a finished readback (tightly packed 8-bit RGBA, width*height*4 bytes)
	std::vector<uint8> AcquireFrameBuffer(size_t size);
	void SubmitVideoFrame(std::vector<uint8>&& rgba, uint32 width, uint32 height, sint64 frameSlot, ColorConversion colorConversion);
	void NotifyFrameDropped(const char* reason);

	// called with each block of TV audio samples as it is fed to the host audio device
	// samples are interleaved big-endian s16 with channelCount channels (1, 2 or 6)
	void SubmitTVAudioBE(const sint16* samples, sint32 sampleCountPerChannel, sint32 channelCount);
}
