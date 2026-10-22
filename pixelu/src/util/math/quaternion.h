// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <tuple>
#include "util/helpers/helpers.h"
#include "util/math/vector3.h"

#define DEG2RAD(__d__) ((__d__ * M_PI) / 180.0f)
#define RAD2DEG(__r__) ((__r__ * 180.0f) / M_PI)

// quaternion w + xi + yj + zk
template<typename T>
class Quaternion
{
public:
	T w;
	T x;
	T y;
	T z;

	Quaternion() : w(), x(), y(), z() {}
	Quaternion(const T& w, const T& x, const T& y, const T& z) : w(w), x(x), y(y), z(z) {}
	// from Euler angles in degrees: pitch around x, yaw around y, roll around z
	Quaternion(float pitchDeg, float yawDeg, float rollDeg);

	void Assign(float nw, float nx, float ny, float nz)
	{
		w = nw;
		x = nx;
		y = ny;
		z = nz;
	}

	// w = cos(angle/2), axis is used as given and then rescaled by NormalizeXYZ
	static Quaternion FromAngleAxis(float inAngle, float inX, float inY, float inZ)
	{
		Quaternion q(std::cos(inAngle * 0.5f), inX, inY, inZ);
		q.NormalizeXYZ();
		return q;
	}

	// matrix rows, scaled by 1/|q|^2 so that non-unit quaternions work too
	std::tuple<Vector3<T>, Vector3<T>, Vector3<T>> GetRotationMatrix() { return buildMatrix(false); }
	std::tuple<Vector3<T>, Vector3<T>, Vector3<T>> GetTransposedRotationMatrix() { return buildMatrix(true); }

	// rescales x, y, z so that the quaternion has unit length; w is kept
	void NormalizeXYZ()
	{
		const T scale = std::sqrt((T)1.0 - w * w) / std::sqrt(x * x + y * y + z * z);
		x *= scale;
		y *= scale;
		z *= scale;
	}

	void NormalizeXYZW()
	{
		const T inv = 1.0f / std::sqrt(w * w + x * x + y * y + z * z);
		w *= inv;
		x *= inv;
		y *= inv;
		z *= inv;
	}

	// rotated unit axes (unit quaternions only)
	Vector3<T> GetVectorX() const
	{
		return Vector3<T>(2.0f * (w * w + x * x) - 1.0f, 2.0f * (x * y - w * z), 2.0f * (x * z + w * y));
	}

	Vector3<T> GetVectorY() const
	{
		return Vector3<T>(2.0f * (x * y + w * z), 2.0f * (w * w + y * y) - 1.0f, 2.0f * (y * z - w * x));
	}

	Vector3<T> GetVectorZ() const
	{
		return Vector3<T>(2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 2.0f * (w * w + z * z) - 1.0f);
	}

	// Hamilton product
	Quaternion& operator*=(const Quaternion<T>& r)
	{
		const T nw = w * r.w - x * r.x - y * r.y - z * r.z;
		const T nx = w * r.x + x * r.w + y * r.z - z * r.y;
		const T ny = w * r.y - x * r.z + y * r.w + z * r.x;
		const T nz = w * r.z + x * r.y - y * r.x + z * r.w;
		Assign(nw, nx, ny, nz);
		return *this;
	}

	Quaternion& operator+=(const Quaternion<T>& r)
	{
		w += r.w;
		x += r.x;
		y += r.y;
		z += r.z;
		return *this;
	}

	friend Quaternion operator*(Quaternion<T> lhs, const Quaternion<T>& rhs)
	{
		lhs *= rhs;
		return lhs;
	}

private:
	std::tuple<Vector3<T>, Vector3<T>, Vector3<T>> buildMatrix(bool transposed) const
	{
		const float ww = w * w, xx = x * x, yy = y * y, zz = z * z;
		const float inv = 1.0f / (xx + yy + zz + ww);
		// off-diagonal terms 2 * (a + b) / |q|^2; the product is done in double precision
		auto off = [inv](float a, float b) { return (T)(2.0 * (a + b) * inv); };
		const float xy = x * y, zw = z * w, xz = x * z, yw = y * w, yz = y * z, xw = x * w;
		Vector3<T> r0, r1, r2;
		r0.x = (xx - yy - zz + ww) * inv;
		r1.y = (-xx + yy - zz + ww) * inv;
		r2.z = (-xx - yy + zz + ww) * inv;
		const T xyPlus = off(xy, zw), xyMinus = off(xy, -zw);
		const T xzPlus = off(xz, yw), xzMinus = off(xz, -yw);
		const T yzPlus = off(yz, xw), yzMinus = off(yz, -xw);
		if (!transposed)
		{
			r1.x = xyPlus;  r0.y = xyMinus;
			r0.z = xzPlus;  r2.x = xzMinus;
			r2.y = yzPlus;  r1.z = yzMinus;
		}
		else
		{
			r0.y = xyPlus;  r1.x = xyMinus;
			r2.x = xzPlus;  r0.z = xzMinus;
			r1.z = yzPlus;  r2.y = yzMinus;
		}
		return std::make_tuple(r0, r1, r2);
	}
};

template<typename T>
Quaternion<T>::Quaternion(float pitchDeg, float yawDeg, float rollDeg)
{
	const float hp = 0.5f * (float)DEG2RAD(pitchDeg);
	const float hy = 0.5f * (float)DEG2RAD(yawDeg);
	const float hr = 0.5f * (float)DEG2RAD(rollDeg);
	// sine and cosine are evaluated in double precision, then rounded to float
	auto dcos = [](float a) { return (float)std::cos((double)a); };
	auto dsin = [](float a) { return (float)std::sin((double)a); };
	const float cp = dcos(hp), sp = dsin(hp);
	const float cy = dcos(hy), sy = dsin(hy);
	const float cr = dcos(hr), sr = dsin(hr);
	w = (cy * cp) * cr + (sy * sp) * sr;
	x = (cy * sp) * cr + (sy * cp) * sr;
	y = (sy * cp) * cr - (cy * sp) * sr;
	z = (cy * cp) * sr - (sy * sp) * cr;
}

using Quaternionf = Quaternion<float>;
