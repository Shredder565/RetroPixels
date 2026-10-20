// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <type_traits>

// Small fixed-size vector, quaternion and matrix types (free look, emulated motion, graphics
// mods). Vectors expose their components both as named members and as a `data` array.

namespace Common
{
template <typename T>
union TVec3
{
  constexpr TVec3() = default;
  constexpr TVec3(T _x, T _y, T _z) : data{_x, _y, _z} {}

  template <typename OtherT>
  constexpr explicit TVec3(const TVec3<OtherT>& other)
      : TVec3(static_cast<T>(other.x), static_cast<T>(other.y), static_cast<T>(other.z))
  {
  }

  constexpr bool operator==(const TVec3& other) const { return data == other.data; }

  constexpr TVec3 Cross(const TVec3& rhs) const
  {
    return {y * rhs.z - rhs.y * z, z * rhs.x - rhs.z * x, x * rhs.y - rhs.x * y};
  }
  constexpr T Dot(const TVec3& rhs) const { return x * rhs.x + y * rhs.y + z * rhs.z; }
  constexpr T LengthSquared() const { return Dot(*this); }
  T Length() const { return std::sqrt(LengthSquared()); }
  TVec3 Normalized() const { return *this / Length(); }

  constexpr TVec3& operator+=(const TVec3& rhs) { return Apply(rhs, std::plus<>{}); }
  constexpr TVec3& operator-=(const TVec3& rhs) { return Apply(rhs, std::minus<>{}); }
  constexpr TVec3& operator*=(const TVec3& rhs) { return Apply(rhs, std::multiplies<>{}); }
  constexpr TVec3& operator/=(const TVec3& rhs) { return Apply(rhs, std::divides<>{}); }

  constexpr TVec3 operator-() const { return {-x, -y, -z}; }

  // Component-wise f(component), f(component, other component) or f(component, scalar).
  template <typename F>
  auto Map(F&& f) const -> TVec3<decltype(f(T{}))>
  {
    return {f(x), f(y), f(z)};
  }

  template <typename F, typename T2>
  auto Map(F&& f, const TVec3<T2>& t) const -> TVec3<decltype(f(T{}, t.x))>
  {
    return {f(x, t.x), f(y, t.y), f(z, t.z)};
  }

  template <typename F, typename T2>
  auto Map(F&& f, T2 scalar) const -> TVec3<decltype(f(T{}, scalar))>
  {
    return {f(x, scalar), f(y, scalar), f(z, scalar)};
  }

  std::array<T, 3> data = {};

  struct
  {
    T x;
    T y;
    T z;
  };

private:
  template <typename Op>
  constexpr TVec3& Apply(const TVec3& rhs, Op op)
  {
    x = op(x, rhs.x);
    y = op(y, rhs.y);
    z = op(z, rhs.z);
    return *this;
  }
};

template <typename T>
TVec3<bool> operator<(const TVec3<T>& lhs, const TVec3<T>& rhs)
{
  return lhs.Map(std::less<T>{}, rhs);
}

constexpr TVec3<bool> operator!(const TVec3<bool>& vec)
{
  return {!vec.x, !vec.y, !vec.z};
}

template <typename T>
auto operator+(const TVec3<T>& lhs, const TVec3<T>& rhs) -> TVec3<decltype(lhs.x + rhs.x)>
{
  return lhs.Map(std::plus<decltype(lhs.x + rhs.x)>{}, rhs);
}

template <typename T>
auto operator-(const TVec3<T>& lhs, const TVec3<T>& rhs) -> TVec3<decltype(lhs.x - rhs.x)>
{
  return lhs.Map(std::minus<decltype(lhs.x - rhs.x)>{}, rhs);
}

template <typename T1, typename T2>
auto operator*(const TVec3<T1>& lhs, const TVec3<T2>& rhs) -> TVec3<decltype(lhs.x * rhs.x)>
{
  return lhs.Map(std::multiplies<decltype(lhs.x * rhs.x)>{}, rhs);
}

template <typename T>
auto operator/(const TVec3<T>& lhs, const TVec3<T>& rhs) -> TVec3<decltype(lhs.x / rhs.x)>
{
  return lhs.Map(std::divides<decltype(lhs.x / rhs.x)>{}, rhs);
}

template <typename T1, typename T2>
auto operator*(const TVec3<T1>& lhs, T2 scalar) -> TVec3<decltype(lhs.x * scalar)>
{
  return lhs.Map(std::multiplies<decltype(lhs.x * scalar)>{}, scalar);
}

template <typename T1, typename T2>
auto operator/(const TVec3<T1>& lhs, T2 scalar) -> TVec3<decltype(lhs.x / scalar)>
{
  return lhs.Map(std::divides<decltype(lhs.x / scalar)>{}, scalar);
}

using Vec3 = TVec3<float>;
using DVec3 = TVec3<double>;

template <typename T>
union TVec4
{
  constexpr TVec4() = default;
  constexpr TVec4(TVec3<T> _vec, T _w) : data{_vec.x, _vec.y, _vec.z, _w} {}
  constexpr TVec4(T _x, T _y, T _z, T _w) : data{_x, _y, _z, _w} {}

  constexpr bool operator==(const TVec4& other) const { return data == other.data; }

  constexpr T Dot(const TVec4& rhs) const
  {
    return x * rhs.x + y * rhs.y + z * rhs.z + w * rhs.w;
  }

  constexpr TVec4& operator*=(const TVec4& rhs)
  {
    for (int i = 0; i < 4; ++i)
      data[i] *= rhs.data[i];
    return *this;
  }

  constexpr TVec4& operator/=(const TVec4& rhs)
  {
    for (int i = 0; i < 4; ++i)
      data[i] /= rhs.data[i];
    return *this;
  }

  constexpr TVec4& operator*=(T scalar) { return *this *= TVec4{scalar, scalar, scalar, scalar}; }
  constexpr TVec4& operator/=(T scalar) { return *this /= TVec4{scalar, scalar, scalar, scalar}; }

  std::array<T, 4> data = {};

  struct
  {
    T x;
    T y;
    T z;
    T w;
  };
};

// The scalar parameter is a non-deduced context so e.g. Vec4 * 2.0 works.
template <typename T>
constexpr TVec4<T> operator*(TVec4<T> lhs, std::common_type_t<T> scalar)
{
  return lhs *= scalar;
}

template <typename T>
constexpr TVec4<T> operator/(TVec4<T> lhs, std::common_type_t<T> scalar)
{
  return lhs /= scalar;
}

using Vec4 = TVec4<float>;
using DVec4 = TVec4<double>;

template <typename T>
union TVec2
{
  constexpr TVec2() = default;
  constexpr TVec2(T _x, T _y) : data{_x, _y} {}

  template <typename OtherT>
  constexpr explicit TVec2(const TVec2<OtherT>& other)
      : TVec2(static_cast<T>(other.x), static_cast<T>(other.y))
  {
  }

  constexpr bool operator==(const TVec2& other) const { return data == other.data; }

  // The z component of the 3D cross product.
  constexpr T Cross(const TVec2& rhs) const { return x * rhs.y - y * rhs.x; }
  constexpr T Dot(const TVec2& rhs) const { return x * rhs.x + y * rhs.y; }
  constexpr T LengthSquared() const { return Dot(*this); }
  T Length() const { return std::sqrt(LengthSquared()); }
  TVec2 Normalized() const { return *this / Length(); }

  constexpr TVec2& operator+=(const TVec2& rhs) { return Set(x + rhs.x, y + rhs.y); }
  constexpr TVec2& operator-=(const TVec2& rhs) { return Set(x - rhs.x, y - rhs.y); }
  constexpr TVec2& operator*=(const TVec2& rhs) { return Set(x * rhs.x, y * rhs.y); }
  constexpr TVec2& operator/=(const TVec2& rhs) { return Set(x / rhs.x, y / rhs.y); }
  constexpr TVec2& operator*=(T scalar) { return Set(x * scalar, y * scalar); }
  constexpr TVec2& operator/=(T scalar) { return Set(x / scalar, y / scalar); }

  constexpr TVec2 operator-() const { return {-x, -y}; }

  std::array<T, 2> data = {};

  struct
  {
    T x;
    T y;
  };

private:
  constexpr TVec2& Set(T new_x, T new_y)
  {
    x = new_x;
    y = new_y;
    return *this;
  }
};

template <typename T>
constexpr TVec2<bool> operator<(const TVec2<T>& lhs, const TVec2<T>& rhs)
{
  return {lhs.x < rhs.x, lhs.y < rhs.y};
}

constexpr TVec2<bool> operator!(const TVec2<bool>& vec)
{
  return {!vec.x, !vec.y};
}

template <typename T>
constexpr TVec2<T> operator+(TVec2<T> lhs, const TVec2<T>& rhs)
{
  return lhs += rhs;
}

template <typename T>
constexpr TVec2<T> operator-(TVec2<T> lhs, const TVec2<T>& rhs)
{
  return lhs -= rhs;
}

template <typename T>
constexpr TVec2<T> operator*(TVec2<T> lhs, const TVec2<T>& rhs)
{
  return lhs *= rhs;
}

template <typename T>
constexpr TVec2<T> operator/(TVec2<T> lhs, const TVec2<T>& rhs)
{
  return lhs /= rhs;
}

// Vector-scalar arithmetic is done in the promoted type (e.g. Vec2 * double gives DVec2).
template <typename T, typename T2>
constexpr auto operator*(TVec2<T> lhs, T2 scalar)
{
  using Result = decltype(lhs.x * scalar);
  return TVec2<Result>(lhs) *= static_cast<Result>(scalar);
}

template <typename T, typename T2>
constexpr auto operator/(TVec2<T> lhs, T2 scalar)
{
  using Result = decltype(lhs.x / scalar);
  return TVec2<Result>(lhs) /= static_cast<Result>(scalar);
}

using Vec2 = TVec2<float>;
using DVec2 = TVec2<double>;

class Matrix33;

// A rotation quaternion. `data` holds (x, y, z, w) with w the real part; note that the
// constructor takes w first.
class Quaternion
{
public:
  static Quaternion Identity();

  static Quaternion RotateX(float rad);
  static Quaternion RotateY(float rad);
  static Quaternion RotateZ(float rad);

  // One rotation about the axis `rads` points along, by |rads| radians. This is what a gyroscope
  // reading (angular velocity about each axis times time) corresponds to.
  static Quaternion RotateXYZ(const Vec3& rads);

  // Rotation by `rad` about a unit-length axis.
  static Quaternion Rotate(float rad, const Vec3& axis);

  Quaternion() = default;
  Quaternion(float w, float x, float y, float z);

  float Norm() const;
  Quaternion Normalized() const;
  Quaternion Conjugate() const;
  Quaternion Inverted() const;

  // Hamilton product: applying the result rotates by rhs first, then by *this.
  Quaternion& operator*=(const Quaternion& rhs);

  Vec4 data;
};

Quaternion operator*(Quaternion lhs, const Quaternion& rhs);
// Rotates a vector.
Vec3 operator*(const Quaternion& lhs, const Vec3& rhs);

// Roll (x), pitch (y) and yaw (z) in radians.
Vec3 FromQuaternionToEuler(const Quaternion& q);

// 3x3 matrix, row-major: data[row * 3 + column]. Multiplies column vectors from the left.
class Matrix33
{
public:
  static Matrix33 Identity();
  static Matrix33 FromQuaternion(const Quaternion&);

  static Matrix33 RotateX(float rad);
  static Matrix33 RotateY(float rad);
  static Matrix33 RotateZ(float rad);

  // Rotation by `rad` about a unit-length axis.
  static Matrix33 Rotate(float rad, const Vec3& axis);

  static Matrix33 Scale(const Vec3& vec);

  // *result = a * b (result may alias an input).
  static void Multiply(const Matrix33& a, const Matrix33& b, Matrix33* result);
  static void Multiply(const Matrix33& a, const Vec3& vec, Vec3* result);

  float Determinant() const;
  Matrix33 Inverted() const;
  Matrix33 Transposed() const;

  Matrix33& operator*=(const Matrix33& rhs)
  {
    Multiply(*this, rhs, this);
    return *this;
  }

  std::array<float, 9> data;
};

inline Matrix33 operator*(Matrix33 lhs, const Matrix33& rhs)
{
  return lhs *= rhs;
}

inline Vec3 operator*(const Matrix33& lhs, Vec3 rhs)
{
  Matrix33::Multiply(lhs, rhs, &rhs);
  return rhs;
}

// 4x4 matrix, row-major: data[row * 4 + column]. Multiplies column vectors from the left.
class Matrix44
{
public:
  static Matrix44 Identity();
  static Matrix44 FromMatrix33(const Matrix33& m33);
  static Matrix44 FromQuaternion(const Quaternion& q);
  static Matrix44 FromArray(const std::array<float, 16>& arr);

  static Matrix44 Translate(const Vec3& vec);
  // x' = x + a*z, y' = y + b*z.
  static Matrix44 Shear(const float a, const float b = 0);
  // OpenGL-style right-handed projection mapping z to [-1, 1].
  static Matrix44 Perspective(float fov_y, float aspect_ratio, float z_near, float z_far);

  // *result = a * b (result may alias an input).
  static void Multiply(const Matrix44& a, const Matrix44& b, Matrix44* result);
  static void Multiply(const Matrix44& a, const Vec4& vec, Vec4* result);

  // (*this * Vec4(point, w)).xyz
  Vec3 Transform(const Vec3& point, float w) const;

  float Determinant() const;
  Matrix44 Inverted() const;
  Matrix44 Transposed() const;

  Matrix44& operator*=(const Matrix44& rhs)
  {
    Multiply(*this, rhs, this);
    return *this;
  }

  std::array<float, 16> data;
};

inline Matrix44 operator*(Matrix44 lhs, const Matrix44& rhs)
{
  return lhs *= rhs;
}

inline Vec4 operator*(const Matrix44& lhs, Vec4 rhs)
{
  Matrix44::Multiply(lhs, rhs, &rhs);
  return rhs;
}
}  // namespace Common
