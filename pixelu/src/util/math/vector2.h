// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <cassert>
#include <cmath>
#include <algorithm>

// plain 2D vector; lengths and products are returned as float
template<typename T>
class Vector2
{
public:
	T x;
	T y;

	Vector2() : x{}, y{} {}
	Vector2(T x, T y) : x(x), y(y) {}

	template<typename U = T>
	Vector2(const Vector2<U>& other) : x((T)other.x), y((T)other.y) {}

	float Length() const { return (float)std::sqrt(x * x + y * y); }
	float Dot(const Vector2& o) const { return x * o.x + y * o.y; }
	// z component of the 3D cross product
	float Cross(const Vector2& o) const { return x * o.y - y * o.x; }
	// perpendicular vector (rotated by -90 degrees)
	Vector2 Ortho() const { return Vector2(y, -x); }

	// unit vector, or zero vector if the length is zero
	Vector2 Normalized() const
	{
		const float len = Length();
		if (len == 0)
			return Vector2();
		return Vector2((T)((float)x / len), (T)((float)y / len));
	}

	Vector2& Normalize()
	{
		const float len = Length();
		if (len != 0)
		{
			x = (T)((float)x / len);
			y = (T)((float)y / len);
		}
		return *this;
	}

	static Vector2 Max(const Vector2& a, const Vector2& b) { return Vector2(std::max(a.x, b.x), std::max(a.y, b.y)); }
	static Vector2 Min(const Vector2& a, const Vector2& b) { return Vector2(std::min(a.x, b.x), std::min(a.y, b.y)); }

	bool operator==(const Vector2& o) const { return x == o.x && y == o.y; }
	bool operator!=(const Vector2& o) const { return !operator==(o); }

	Vector2& operator+=(const Vector2& o) { x += o.x; y += o.y; return *this; }
	Vector2& operator-=(const Vector2& o) { x -= o.x; y -= o.y; return *this; }
	Vector2 operator+(const Vector2& o) const { return Vector2(x + o.x, y + o.y); }
	Vector2 operator-(const Vector2& o) const { return Vector2(x - o.x, y - o.y); }

	// scalar forms apply the same scalar to both components
	Vector2& operator+=(const T& s) { x += s; y += s; return *this; }
	Vector2& operator-=(const T& s) { x -= s; y -= s; return *this; }
	Vector2& operator*=(const T& s) { x *= s; y *= s; return *this; }
	Vector2& operator/=(const T& s) { assert(s != 0); x /= s; y /= s; return *this; }
	Vector2 operator+(const T& s) const { return Vector2(x + s, y + s); }
	Vector2 operator-(const T& s) const { return Vector2(x - s, y - s); }
	Vector2 operator*(const T& s) const { return Vector2(x * s, y * s); }
	Vector2 operator/(const T& s) const { assert(s != 0); return Vector2(x / s, y / s); }
};

using Vector2f = Vector2<float>;
using Vector2i = Vector2<int>;
