// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Orientation tracking for controllers that report raw gyro and accelerometer samples
// (SDL pads, DSU servers, ...). This is a Mahony style complementary filter: the gyro
// rate is integrated into a unit quaternion, and the direction of gravity seen by the
// accelerometer pulls the estimate back so that it does not drift away over time.
// A slowly averaged gyro offset (taken only while the pad is held still) is removed
// first, and tiny rates are ignored so a resting pad does not creep around its yaw.
//
// The angles it reports keep counting whole turns, the way the GamePad's own
// orientation values do.

#include <cmath>
#include "util/math/quaternion.h"

class MahonySensorFusion
{
public:
	MahonySensorFusion()
	{
		// resting pose: held in both hands, front tilted up by a quarter turn about x
		const float half = sqrtf(0.5);
		m_q = { half, half, 0.0f, 0.0f };
	}

	// gx/gy/gz: angular rate in radians per second, ax/ay/az: acceleration in g
	void updateIMU(float deltaTime, float gx, float gy, float gz, float ax, float ay, float az)
	{
		// a long stall (loading screen, window drag) would otherwise throw the estimate around
		const float dt = deltaTime > 0.2f ? 0.2f : deltaTime;

		trackGyroOffset(gx, gy, gz);
		float rate[3] = { gx - m_gyroOffset[0], gy - m_gyroOffset[1], gz - m_gyroOffset[2] };
		for (float& r : rate)
		{
			if (std::fabs(r) < kRateDeadband)
				r = 0.0f;
		}

		const bool haveAcc = std::fabs(ax) > kMinAcc || std::fabs(ay) > kMinAcc || std::fabs(az) > kMinAcc;
		if (haveAcc)
		{
			float down[3] = { ax, ay, az };
			const float len = std::sqrt((down[0] * down[0]) + (down[1] * down[1]) + (down[2] * down[2]));
			if (len != 0)
			{
				down[0] /= len;
				down[1] /= len;
				down[2] /= len;
			}
			// where the current estimate expects gravity (third column of its rotation), at half weight
			const Quat& q = m_q;
			float expect[3] = {
				2.0f * (q.x * q.z - q.w * q.y),
				2.0f * (q.y * q.z + q.w * q.x),
				2.0f * (q.w * q.w + q.z * q.z) - 1.0f };
			for (float& e : expect)
				e *= 0.5f;
			// the cross product of expected and measured gravity is the correction rate
			rate[0] -= expect[1] * down[2] - expect[2] * down[1];
			rate[1] -= expect[2] * down[0] - expect[0] * down[2];
			rate[2] -= expect[0] * down[1] - expect[1] * down[0];
		}

		const float step = 0.5f * dt;
		for (float& r : rate)
			r *= step;

		// q' = q + q * (0, rate)
		const Quat delta = multiply(m_q, Quat{ 0.0f, rate[0], rate[1], rate[2] });
		m_q.w += delta.w;
		m_q.x += delta.x;
		m_q.y += delta.y;
		m_q.z += delta.z;
		const float inv = 1.0f / sqrtf(m_q.w * m_q.w + m_q.x * m_q.x + m_q.y * m_q.y + m_q.z * m_q.z);
		m_q.w *= inv;
		m_q.x *= inv;
		m_q.y *= inv;
		m_q.z *= inv;

		refreshAngles();
	}

	float getRollRadians() { return withTurns(m_angle[kRoll], m_turns[kRoll]); }
	float getPitchRadians() { return withTurns(m_angle[kPitch], m_turns[kPitch]); }
	float getYawRadians() { return withTurns(m_angle[kYaw], m_turns[kYaw]); }

	// w, x, y, z
	void getQuaternion(float q[4]) const
	{
		q[0] = m_q.w;
		q[1] = m_q.x;
		q[2] = m_q.y;
		q[3] = m_q.z;
	}

	void getGyroBias(float gBias[3]) const
	{
		for (int i = 0; i < 3; ++i)
			gBias[i] = m_gyroOffset[i];
	}

private:
	struct Quat
	{
		float w, x, y, z;
	};

	static constexpr float kPi = 3.14159265f;
	static constexpr float kRateDeadband = 0.015f;
	static constexpr float kMinAcc = 0.000001f;
	// rates above this mean the pad is moving, so they say nothing about the offset.
	// It can't be much lower: some pads rest at almost 0.1 rad/s on one axis.
	static constexpr float kStillRate = 0.35f;
	static constexpr uint64 kOffsetSamples = 200;

	enum { kRoll, kPitch, kYaw };

	static Quat multiply(const Quat& a, const Quat& b)
	{
		return Quat{
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
	}

	static float withTurns(float angle, int turns)
	{
		return angle + (float)turns * 2.0f * kPi;
	}

	void trackGyroOffset(float gx, float gy, float gz)
	{
		if (std::fabs(gx) >= kStillRate || std::fabs(gy) >= kStillRate || std::fabs(gz) >= kStillRate)
			return;
		m_stillSum[0] += gx;
		m_stillSum[1] += gy;
		m_stillSum[2] += gz;
		++m_stillCount;
		if (m_stillCount < kOffsetSamples)
			return;
		for (int i = 0; i < 3; ++i)
			m_gyroOffset[i] = (float)(m_stillSum[i] / (double)m_stillCount);
	}

	// a jump of more than half a turn between two samples is a wrap of atan2/asin
	static int wrapCount(float before, float after)
	{
		if (after > before && after - before > kPi)
			return -1;
		if (after < before && before - after > kPi)
			return 1;
		return 0;
	}

	void refreshAngles()
	{
		const Quat& q = m_q;
		float angle[3];
		angle[kRoll] = std::atan2(2.0f * (q.z * q.w + q.x * q.y), 1.0f - 2.0f * (q.w * q.w + q.x * q.x));
		const float s = 2.0f * (q.z * q.x - q.y * q.w);
		if (std::abs(s) >= 1.0)
			angle[kPitch] = std::copysign(3.14159265359f / 2.0f, s);
		else
			angle[kPitch] = std::asin(s);
		angle[kYaw] = std::atan2(2.0f * (q.z * q.y + q.w * q.x), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
		for (int i = 0; i < 3; ++i)
		{
			m_turns[i] += wrapCount(m_angle[i], angle[i]);
			m_angle[i] = angle[i];
		}
	}

	Quat m_q{};
	float m_angle[3]{};
	int m_turns[3]{};
	float m_gyroOffset[3]{};
	double m_stillSum[3]{};
	uint64 m_stillCount{};
};
