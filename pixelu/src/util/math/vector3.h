// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <cassert>
#include <cmath>

// plain 3D vector; vector operators work component-wise
template<typename T>
class Vector3
{
public:
	T x;
	T y;
	T z;

	Vector3() : x{}, y{}, z{} {}
	Vector3(T x, T y, T z) : x(x), y(y), z(z) {}

	template<typename U = T>
	Vector3(const Vector3<U>& other) : x((T)other.x), y((T)other.y), z((T)other.z) {}

	float Length() const { return std::sqrt(x * x + y * y + z * z); }
	float Dot(const Vector3& o) const { return x * o.x + y * o.y + z * o.z; }
	Vector3 Cross(const Vector3& o) const { return Vector3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x); }

	// unit vector, or zero vector if the length is zero
	Vector3 Normalized() const
	{
		const float len = Length();
		if (len == 0)
			return Vector3();
		return *this / len;
	}

	Vector3& Normalize()
	{
		const float len = Length();
		if (len != 0)
			*this /= len;
		return *this;
	}

	void Scale(const float s)
	{
		x *= s;
		y *= s;
		z *= s;
	}

	// rotation around the given axis by theta radians (right-handed)
	Vector3& RotateX(float theta);
	Vector3& RotateY(float theta);
	Vector3& RotateZ(float theta);

	bool operator==(const Vector3& o) const { return x == o.x && y == o.y && z == o.z; }
	bool operator!=(const Vector3& o) const { return !operator==(o); }

	template<typename U = T>
	Vector3& operator+=(const Vector3<U>& o) { x += (T)o.x; y += (T)o.y; z += (T)o.z; return *this; }
	template<typename U = T>
	Vector3& operator-=(const Vector3<U>& o) { x -= (T)o.x; y -= (T)o.y; z -= (T)o.z; return *this; }
	template<typename U = T>
	Vector3& operator*=(const Vector3<U>& o) { x *= (T)o.x; y *= (T)o.y; z *= (T)o.z; return *this; }
	template<typename U = T>
	Vector3& operator/=(const Vector3<U>& o)
	{
		assert(o.x != 0 && o.y != 0 && o.z != 0);
		x /= (T)o.x; y /= (T)o.y; z /= (T)o.z;
		return *this;
	}

	template<typename U = T>
	Vector3 operator+(const Vector3<U>& o) const { return Vector3(x + (T)o.x, y + (T)o.y, z + (T)o.z); }
	template<typename U = T>
	Vector3 operator-(const Vector3<U>& o) const { return Vector3(x - (T)o.x, y - (T)o.y, z - (T)o.z); }
	template<typename U = T>
	Vector3 operator*(const Vector3<U>& o) const { return Vector3(x * (T)o.x, y * (T)o.y, z * (T)o.z); }
	template<typename U = T>
	Vector3 operator/(const Vector3<U>& o) const
	{
		assert(o.x != 0 && o.y != 0 && o.z != 0);
		return Vector3(x / (T)o.x, y / (T)o.y, z / (T)o.z);
	}

	Vector3& operator+=(T s) { x += s; y += s; z += s; return *this; }
	Vector3& operator-=(T s) { x -= s; y -= s; z -= s; return *this; }
	Vector3& operator*=(T s) { x *= s; y *= s; z *= s; return *this; }
	Vector3& operator/=(T s) { assert(s != 0); x /= s; y /= s; z /= s; return *this; }

	Vector3 operator+(T s) const { return Vector3(x + s, y + s, z + s); }
	Vector3 operator-(T s) const { return Vector3(x - s, y - s, z - s); }
	Vector3 operator*(T s) const { return Vector3(x * s, y * s, z * s); }
	Vector3 operator/(T s) const { assert(s != 0); return Vector3(x / s, y / s, z / s); }

	bool IsZero() const { return x == 0 && y == 0 && z == 0; }
	bool HasZero() const { return x == 0 || y == 0 || z == 0; }
};

template<typename T>
Vector3<T>& Vector3<T>::RotateX(float theta)
{
	const float s = std::sin(theta), c = std::cos(theta);
	const T ny = (T)(y * c - z * s);
	const T nz = (T)(y * s + z * c);
	y = ny;
	z = nz;
	return *this;
}

template<typename T>
Vector3<T>& Vector3<T>::RotateY(float theta)
{
	const float s = std::sin(theta), c = std::cos(theta);
	const T nx = (T)(x * c + z * s);
	const T nz = (T)(z * c - x * s);
	x = nx;
	z = nz;
	return *this;
}

template<typename T>
Vector3<T>& Vector3<T>::RotateZ(float theta)
{
	const float s = std::sin(theta), c = std::cos(theta);
	const T nx = (T)(x * c - y * s);
	const T ny = (T)(x * s + y * c);
	x = nx;
	y = ny;
	return *this;
}

using Vector3f = Vector3<float>;
using Vector3i = Vector3<int>;
