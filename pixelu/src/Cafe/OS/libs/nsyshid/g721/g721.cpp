// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "g721.h"

#include <cstdlib>

namespace
{
	// G.726 tables for 32 kbit/s, indexed by |I| (0-7)
	// quantizer decision levels, log2 domain with 7 fraction bits
	constexpr int kDecision[7] = {-124, 80, 178, 246, 300, 349, 400};
	// inverse quantizer output levels, same domain
	constexpr int kReconstruct[8] = {-2048, 4, 135, 213, 273, 323, 373, 425};
	// scale factor multipliers W(I), 4 fraction bits
	constexpr int kW[8] = {-12, 18, 41, 64, 112, 198, 355, 1122};
	// rate-of-change values F(I)
	constexpr int kF[8] = {0, 0, 0, 1, 1, 1, 3, 7};

	// the position past the highest set bit (0 for 0), at most 15
	int BitLength(int v)
	{
		int n = 0;
		while (n < 15 && v >= (1 << n))
			n++;
		return n;
	}
} // namespace

void G721Encoder::Reset()
{
	m_yl = 34816;
	m_yu = 544;
	m_dms = m_dml = m_ap = 0;
	m_a[0] = m_a[1] = 0;
	for (int& b : m_b)
		b = 0;
	m_pk[0] = m_pk[1] = false;
	for (Float11& f : m_dq)
		f = {false, 0, 32};
	for (Float11& f : m_sr)
		f = {false, 0, 32};
	m_tone = false;
}

G721Encoder::Float11 G721Encoder::ToFloat11(int magnitude, bool negative)
{
	if (magnitude == 0)
		return {negative, 0, 32};
	const int e = BitLength(magnitude);
	return {negative, e, (magnitude << 6) >> e};
}

// FMULT: a 16-bit predictor coefficient times a history value
int G721Encoder::FloatMultiply(int coefficient, const Float11& value)
{
	const int magnitude = coefficient > 0 ? coefficient : (-coefficient) & 0x1fff;
	const int shift = BitLength(magnitude) - 6;
	int mantissa = 32;
	if (magnitude != 0)
		mantissa = shift >= 0 ? magnitude >> shift : magnitude << -shift;
	const int exponent = shift + value.exponent - 13;
	const int productMantissa = (mantissa * value.mantissa + 48) >> 4;
	const int product = exponent >= 0 ? (productMantissa << exponent) & 0x7fff : productMantissa >> -exponent;
	return ((coefficient < 0) != value.negative) ? -product : product;
}

// MIX: the scale factor between the unlocked and locked ones
int G721Encoder::ScaleFactor() const
{
	if (m_ap >= 256)
		return m_yu;
	int y = m_yl >> 6;
	const int diff = m_yu - y;
	const int weight = m_ap >> 2;
	if (diff > 0)
		y += (diff * weight) >> 6;
	else if (diff < 0)
		y += (diff * weight + 63) >> 6;
	return y;
}

int G721Encoder::Encode(int16_t sample)
{
	const int sl = sample >> 2; // 14-bit input

	// the signal estimate from the zero and pole predictors
	int zeroSum = 0;
	for (int i = 0; i < 6; i++)
		zeroSum += FloatMultiply(m_b[i] >> 2, m_dq[i]);
	const int sez = zeroSum >> 1;
	const int se = (zeroSum + FloatMultiply(m_a[1] >> 2, m_sr[1]) + FloatMultiply(m_a[0] >> 2, m_sr[0])) >> 1;

	const int y = ScaleFactor();
	const int d = sl - se;

	// quantize log2|d| against the scale factor
	const int dMag = std::abs(d);
	const int e = BitLength(dMag >> 1);
	const int dl = (e << 7) + (((dMag << 7) >> e) & 127);
	const int dln = dl - (y >> 2);
	int level = 0;
	while (level < 7 && dln >= kDecision[level])
		level++;
	int code;
	if (d < 0)
		code = 15 - level;
	else
		code = level == 0 ? 15 : level;

	// the quantized difference the decoder will see, sign and magnitude
	const int mag = code & 8 ? 15 - code : code;
	const int dql = kReconstruct[mag] + (y >> 2);
	int dqMag = 0;
	if (dql >= 0)
		dqMag = ((128 + (dql & 127)) << 7) >> (14 - ((dql >> 7) & 15));
	const int dq = (code & 8) ? dqMag - 0x8000 : dqMag;

	const int sr = dq < 0 ? se - (dq & 0x3fff) : se + dq;
	Adapt(code, y, dq, sr, sr + sez - se);
	return code;
}

void G721Encoder::Adapt(int code, int y, int dq, int sr, int dqsez)
{
	const int mag = code & 8 ? 15 - code : code;
	const bool pk0 = dqsez < 0;
	const int dqMag = dq & 0x7fff;

	// TRANS: a transition away from a tone resets the predictor
	const int ylInt = m_yl >> 15, ylFrac = (m_yl >> 10) & 31;
	const int threshold = ylInt > 9 ? 31 << 10 : (32 + ylFrac) << ylInt;
	const bool transition = m_tone && dqMag > ((threshold + (threshold >> 1)) >> 1);

	// scale factor adaptation
	m_yu = y + (((kW[mag] << 5) - y) >> 5);
	if (m_yu < 544)
		m_yu = 544;
	else if (m_yu > 5120)
		m_yu = 5120;
	m_yl += m_yu + ((-m_yl) >> 6);

	// predictor coefficients
	int a2 = 0;
	if (transition)
	{
		m_a[0] = m_a[1] = 0;
		for (int& b : m_b)
			b = 0;
	}
	else
	{
		const bool signsDiffer = pk0 != m_pk[0];
		a2 = m_a[1] - (m_a[1] >> 7);
		if (dqsez != 0)
		{
			const int fa1 = signsDiffer ? m_a[0] : -m_a[0];
			if (fa1 < -8191)
				a2 -= 256;
			else if (fa1 > 8191)
				a2 += 255;
			else
				a2 += fa1 >> 5;
			if (pk0 != m_pk[1])
			{
				if (a2 <= -12160)
					a2 = -12288;
				else if (a2 >= 12416)
					a2 = 12288;
				else
					a2 -= 128;
			}
			else
			{
				if (a2 <= -12416)
					a2 = -12288;
				else if (a2 >= 12160)
					a2 = 12288;
				else
					a2 += 128;
			}
		}
		m_a[1] = a2;

		m_a[0] -= m_a[0] >> 8;
		if (dqsez != 0)
			m_a[0] += signsDiffer ? -192 : 192;
		const int a1Limit = 15360 - a2;
		if (m_a[0] < -a1Limit)
			m_a[0] = -a1Limit;
		else if (m_a[0] > a1Limit)
			m_a[0] = a1Limit;

		for (int i = 0; i < 6; i++)
		{
			m_b[i] -= m_b[i] >> 8;
			if (dqMag != 0)
				m_b[i] += ((dq < 0) == m_dq[i].negative) ? 128 : -128;
		}
	}

	// history
	for (int i = 5; i > 0; i--)
		m_dq[i] = m_dq[i - 1];
	m_dq[0] = ToFloat11(dqMag, dq < 0);
	m_sr[1] = m_sr[0];
	if (sr <= -32768)
		m_sr[0] = {true, 0, 32};
	else
		m_sr[0] = ToFloat11(std::abs(sr), sr < 0);
	m_pk[1] = m_pk[0];
	m_pk[0] = pk0;

	// TONE
	m_tone = !transition && a2 < -11776;

	// adaptation speed control
	const int f = kF[mag] << 9;
	m_dms += (f - m_dms) >> 5;
	m_dml += ((f << 2) - m_dml) >> 7;
	if (transition)
		m_ap = 256;
	else if (y < 1536 || m_tone || std::abs((m_dms << 2) - m_dml) >= (m_dml >> 3))
		m_ap += (0x200 - m_ap) >> 4;
	else
		m_ap += (-m_ap) >> 4;
}
