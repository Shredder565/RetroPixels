// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Turns a stream of raw motion samples from a host controller (gyro in rad/s,
// acceleration in g) into a MotionSample with the values the Wii U APIs expect:
// orientation in turns, de-biased gyro rate, acceleration and its change.

#include "Mahony.h"
#include "MotionSample.h"

class WiiUMotionHandler
{
public:
	void processMotionSample(float deltaTime, float gx, float gy, float gz, float accx, float accy, float accz)
	{
		m_rate[0] = gx;
		m_rate[1] = gy;
		m_rate[2] = gz;
		for (int i = 0; i < 3; ++i)
			m_lastAcc[i] = m_acc[i];
		m_acc[0] = accx;
		m_acc[1] = accy;
		m_acc[2] = accz;

		m_fusion.updateIMU(deltaTime, gx, gy, gz, accx, accy, accz);

		// the GamePad counts orientation in whole turns; yaw and pitch run the other way round
		// and are centred on half a turn
		m_turns[0] = toTurns(-m_fusion.getYawRadians()) - 0.50f;
		m_turns[1] = toTurns(-m_fusion.getPitchRadians()) - 0.50f;
		m_turns[2] = toTurns(m_fusion.getRollRadians());
	}

	MotionSample getMotionSample()
	{
		float q[4];
		m_fusion.getQuaternion(q);
		float offset[3];
		m_fusion.getGyroBias(offset);
		float rate[3];
		for (int i = 0; i < 3; ++i)
			rate[i] = m_rate[i] - offset[i];
		return MotionSample(m_acc, MotionSample::calculateAccAcceleration(m_lastAcc, m_acc), rate, m_turns, q);
	}

private:
	static float toTurns(float radians)
	{
		return radians / (2.0f * 3.14159265f);
	}

	MahonySensorFusion m_fusion;
	float m_rate[3]{};
	float m_acc[3]{};
	float m_lastAcc[3]{};
	float m_turns[3]{};
};
