// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <cstdint>

// A G.721 (32 kbit/s ADPCM) encoder, written from the block descriptions in
// ITU-T Recommendation G.726. It turns 16-bit linear samples into 4-bit
// codes, which is what the Xbox 360 Skylanders portal's speaker plays.
class G721Encoder
{
  public:
	G721Encoder() { Reset(); }
	void Reset();
	// Encode takes one 16-bit sample and returns its 4-bit code.
	int Encode(int16_t sample);

  private:
	// a number in the codec's 11-bit floating format: sign, 4-bit exponent,
	// 6-bit mantissa (the history of DQ and SR)
	struct Float11
	{
		bool negative;
		int exponent;
		int mantissa;
	};
	static Float11 ToFloat11(int magnitude, bool negative);
	static int FloatMultiply(int coefficient, const Float11& value);
	int ScaleFactor() const;
	void Adapt(int code, int y, int dq, int sr, int dqsez);

	int32_t m_yl;	  // locked scale factor, 6 more fraction bits
	int m_yu;		  // unlocked scale factor
	int m_dms, m_dml; // short and long term averages of F(I)
	int m_ap;		  // speed control
	int m_a[2];		  // pole predictor coefficients
	int m_b[6];		  // zero predictor coefficients
	bool m_pk[2];	  // signs of the last two partial reconstructions
	Float11 m_dq[6];  // last six quantized differences
	Float11 m_sr[2];  // last two reconstructed samples
	bool m_tone;	  // tone detected (TD)
};
