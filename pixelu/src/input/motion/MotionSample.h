// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// One motion reading of a host controller, kept in a neutral form and handed out
// in the shape the Wii U libraries want (VPAD for the GamePad, KPAD for Wii remotes).
//
// acc:          acceleration in g
// accelerationChange: length of the change in acc since the previous reading
// gyro:         angular rate in rad/s
// orientation:  orientation in turns (1.0 = one full revolution)
// quaternion:   current attitude as w, x, y, z

#include <cmath>
#include "util/math/vector3.h"
#include "util/math/quaternion.h"

class MotionSample
{
public:
	MotionSample() = default;

	MotionSample(const float acc[3], float accelerationChange, const float gyro[3], const float orientation[3], const float quaternion[4])
	{
		for (int i = 0; i < 3; ++i)
		{
			m_acc[i] = acc[i];
			m_gyro[i] = gyro[i];
			m_turns[i] = orientation[i];
		}
		for (int i = 0; i < 4; ++i)
			m_attitude[i] = quaternion[i];
		m_accChange = accelerationChange;
		m_accLength = sqrtf(m_acc[0] * m_acc[0] + m_acc[1] * m_acc[1] + m_acc[2] * m_acc[2]);
	}

	void getVPADOrientation(float orientation[3]) const
	{
		for (int i = 0; i < 3; ++i)
			orientation[i] = m_turns[i];
	}

	// angular rate in turns per second; x and y point the other way on the GamePad
	void getVPADGyroChange(float gyro[3]) const
	{
		gyro[0] = toTurns(-m_gyro[0]);
		gyro[1] = toTurns(-m_gyro[1]);
		gyro[2] = toTurns(m_gyro[2]);
	}

	void getVPADAccelerometer(float acc[3]) const
	{
		acc[0] = -m_acc[0];
		acc[1] = -m_acc[1];
		acc[2] = m_acc[2];
	}

	float getVPADAccMagnitude() const { return m_accLength; }

	// probably a little smaller than what the real VPAD library reports
	float getVPADAccAcceleration() const { return m_accChange; }

	// The GamePad's attitude matrix does not follow one handedness. Its rows are the
	// x axis of the current attitude, of the attitude turned a quarter turn about z,
	// and of the attitude turned a quarter turn about y.
	void getVPADAttitudeMatrix(float mtx[9]) const
	{
		const Quaternionf attitude(m_attitude[0], m_attitude[1], m_attitude[2], m_attitude[3]);
		const Quaternionf rowY = attitude * Quaternionf::FromAngleAxis(1.5708f * 1.0f, 0.0f, 0.0f, 1.0f);
		const Quaternionf rowZ = attitude * Quaternionf::FromAngleAxis(1.5708f * 1.0f, 0.0f, 1.0f, 0.0f);
		writeAxisX(mtx + 0, attitude);
		writeAxisX(mtx + 3, rowY);
		writeAxisX(mtx + 6, rowZ);
	}

	static float calculateAccAcceleration(const float prevAcc[3], const float currentAcc[3])
	{
		const float dx = currentAcc[0] - prevAcc[0];
		const float dy = currentAcc[1] - prevAcc[1];
		const float dz = currentAcc[2] - prevAcc[2];
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}

	void getAccelerometer(float acc[3]) const
	{
		for (int i = 0; i < 3; ++i)
			acc[i] = m_acc[i];
	}

	void getGyrometer(float gyro[3]) const
	{
		for (int i = 0; i < 3; ++i)
			gyro[i] = m_gyro[i];
	}

private:
	static float toTurns(float radians)
	{
		return radians / (2.0f * 3.14159265f);
	}

	// first column of the rotation matrix of q, stored as x, z, y
	static void writeAxisX(float out[3], const Quaternionf& q)
	{
		out[0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
		out[2] = 2.0f * (q.x * q.y + q.z * q.w);
		out[1] = 2.0f * (q.x * q.z - q.y * q.w);
	}

	float m_acc[3]{};
	float m_accChange{};
	float m_gyro[3]{};
	float m_turns[3]{};
	float m_attitude[4]{};
	float m_accLength{};
};
