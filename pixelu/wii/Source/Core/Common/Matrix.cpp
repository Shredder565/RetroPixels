// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Matrix.h"

#include <cmath>
#include <cstddef>

#include "Common/MathUtil.h"

namespace
{
// Row-major (Rows x Inner) * (Inner x Cols) product.
template <std::size_t Rows, std::size_t Inner, std::size_t Cols>
std::array<float, Rows * Cols> Product(const std::array<float, Rows * Inner>& lhs,
                                       const std::array<float, Inner * Cols>& rhs)
{
  std::array<float, Rows * Cols> out;
  for (std::size_t row = 0; row < Rows; ++row)
  {
    for (std::size_t col = 0; col < Cols; ++col)
    {
      float sum = 0.0f;
      for (std::size_t k = 0; k < Inner; ++k)
        sum += lhs[row * Inner + k] * rhs[k * Cols + col];
      out[row * Cols + col] = sum;
    }
  }
  return out;
}

// Determinant of the 2x2 matrix [[a, b], [c, d]].
constexpr float Det2(float a, float b, float c, float d)
{
  return a * d - b * c;
}
}  // namespace

namespace Common
{
// ---- Quaternion -------------------------------------------------------------------------------

Quaternion::Quaternion(float w, float x, float y, float z) : data(x, y, z, w)
{
}

Quaternion Quaternion::Identity()
{
  return Quaternion(1, 0, 0, 0);
}

Quaternion Quaternion::Rotate(float rad, const Vec3& axis)
{
  const float half = rad / 2;
  const float s = std::sin(half);
  return Quaternion(std::cos(half), axis.x * s, axis.y * s, axis.z * s);
}

Quaternion Quaternion::RotateX(float rad)
{
  return Rotate(rad, Vec3(1, 0, 0));
}

Quaternion Quaternion::RotateY(float rad)
{
  return Rotate(rad, Vec3(0, 1, 0));
}

Quaternion Quaternion::RotateZ(float rad)
{
  return Rotate(rad, Vec3(0, 0, 1));
}

Quaternion Quaternion::RotateXYZ(const Vec3& rads)
{
  const float angle = rads.Length();
  if (angle == 0)
    return Identity();
  return Rotate(angle, rads / angle);
}

float Quaternion::Norm() const
{
  return std::sqrt(data.Dot(data));
}

Quaternion Quaternion::Normalized() const
{
  Quaternion unit = *this;
  unit.data /= Norm();
  return unit;
}

Quaternion Quaternion::Conjugate() const
{
  return Quaternion(data.w, -data.x, -data.y, -data.z);
}

Quaternion Quaternion::Inverted() const
{
  // For a unit quaternion the inverse is the conjugate.
  return Normalized().Conjugate();
}

Quaternion& Quaternion::operator*=(const Quaternion& rhs)
{
  const Vec4 p = data;
  const Vec4& q = rhs.data;
  // (pw + pv)(qw + qv) = pw*qw - pv.qv + pw*qv + qw*pv + pv x qv
  data = Vec4{p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y,
              p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
              p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w,
              p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z};
  return *this;
}

Quaternion operator*(Quaternion lhs, const Quaternion& rhs)
{
  return lhs *= rhs;
}

Vec3 operator*(const Quaternion& lhs, const Vec3& rhs)
{
  // v' = q * (0, v) * q^-1, with q assumed to be unit length.
  const Quaternion rotated = lhs * Quaternion(0, rhs.x, rhs.y, rhs.z) * lhs.Conjugate();
  return {rotated.data.x, rotated.data.y, rotated.data.z};
}

Vec3 FromQuaternionToEuler(const Quaternion& q)
{
  const float x = q.data.x;
  const float y = q.data.y;
  const float z = q.data.z;
  const float w = q.data.w;

  Vec3 euler;

  // Roll
  euler.x = std::atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y));

  // Pitch, clamped to +-90 degrees where rounding pushes the sine out of range.
  const float sin_pitch = 2 * (w * y - z * x);
  euler.y = std::abs(sin_pitch) >= 1 ? std::copysign(MathUtil::PI / 2, sin_pitch) :
                                       std::asin(sin_pitch);

  // Yaw
  euler.z = std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));

  return euler;
}

// ---- Matrix33 ---------------------------------------------------------------------------------

Matrix33 Matrix33::Identity()
{
  return Scale(Vec3(1, 1, 1));
}

Matrix33 Matrix33::Scale(const Vec3& vec)
{
  Matrix33 m{};
  m.data[0] = vec.x;
  m.data[4] = vec.y;
  m.data[8] = vec.z;
  return m;
}

Matrix33 Matrix33::FromQuaternion(const Quaternion& q)
{
  const float x = q.data.x;
  const float y = q.data.y;
  const float z = q.data.z;
  const float w = q.data.w;

  Matrix33 m;
  m.data = {
      1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * z * w,     2 * x * z + 2 * y * w,
      2 * x * y + 2 * z * w,     1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * x * w,
      2 * x * z - 2 * y * w,     2 * y * z + 2 * x * w,     1 - 2 * x * x - 2 * y * y,
  };
  return m;
}

Matrix33 Matrix33::RotateX(float rad)
{
  const float s = std::sin(rad);
  const float c = std::cos(rad);
  Matrix33 m;
  m.data = {1, 0, 0, 0, c, -s, 0, s, c};
  return m;
}

Matrix33 Matrix33::RotateY(float rad)
{
  const float s = std::sin(rad);
  const float c = std::cos(rad);
  Matrix33 m;
  m.data = {c, 0, s, 0, 1, 0, -s, 0, c};
  return m;
}

Matrix33 Matrix33::RotateZ(float rad)
{
  const float s = std::sin(rad);
  const float c = std::cos(rad);
  Matrix33 m;
  m.data = {c, -s, 0, s, c, 0, 0, 0, 1};
  return m;
}

Matrix33 Matrix33::Rotate(float rad, const Vec3& axis)
{
  // Rodrigues' formula: R = c*I + (1-c)*a*a^T + s*[a]x
  const float s = std::sin(rad);
  const float c = std::cos(rad);
  const float t = 1 - c;
  const float x = axis.x;
  const float y = axis.y;
  const float z = axis.z;

  Matrix33 m;
  m.data = {
      x * x * t + c,     x * y * t - z * s, x * z * t + y * s,
      y * x * t + z * s, y * y * t + c,     y * z * t - x * s,
      z * x * t - y * s, z * y * t + x * s, z * z * t + c,
  };
  return m;
}

void Matrix33::Multiply(const Matrix33& a, const Matrix33& b, Matrix33* result)
{
  result->data = Product<3, 3, 3>(a.data, b.data);
}

void Matrix33::Multiply(const Matrix33& a, const Vec3& vec, Vec3* result)
{
  result->data = Product<3, 3, 1>(a.data, vec.data);
}

float Matrix33::Determinant() const
{
  const auto& m = data;
  // Expansion along the first row.
  return m[0] * Det2(m[4], m[5], m[7], m[8]) - m[1] * Det2(m[3], m[5], m[6], m[8]) +
         m[2] * Det2(m[3], m[4], m[6], m[7]);
}

Matrix33 Matrix33::Inverted() const
{
  // inverse = adjugate / determinant; the adjugate is the transposed cofactor matrix.
  const auto& m = data;
  const float inv_det = 1 / Determinant();

  Matrix33 inv;
  inv.data = {
      Det2(m[4], m[5], m[7], m[8]),  -Det2(m[1], m[2], m[7], m[8]), Det2(m[1], m[2], m[4], m[5]),
      -Det2(m[3], m[5], m[6], m[8]), Det2(m[0], m[2], m[6], m[8]),  -Det2(m[0], m[2], m[3], m[5]),
      Det2(m[3], m[4], m[6], m[7]),  -Det2(m[0], m[1], m[6], m[7]), Det2(m[0], m[1], m[3], m[4]),
  };
  for (float& value : inv.data)
    value *= inv_det;
  return inv;
}

Matrix33 Matrix33::Transposed() const
{
  Matrix33 t;
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
      t.data[col * 3 + row] = data[row * 3 + col];
  }
  return t;
}

// ---- Matrix44 ---------------------------------------------------------------------------------

Matrix44 Matrix44::Identity()
{
  Matrix44 m{};
  for (int i = 0; i < 4; ++i)
    m.data[i * 5] = 1.0f;
  return m;
}

Matrix44 Matrix44::FromMatrix33(const Matrix33& m33)
{
  Matrix44 m = Identity();
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
      m.data[row * 4 + col] = m33.data[row * 3 + col];
  }
  return m;
}

Matrix44 Matrix44::FromQuaternion(const Quaternion& q)
{
  return FromMatrix33(Matrix33::FromQuaternion(q));
}

Matrix44 Matrix44::FromArray(const std::array<float, 16>& arr)
{
  Matrix44 m;
  m.data = arr;
  return m;
}

Matrix44 Matrix44::Translate(const Vec3& vec)
{
  Matrix44 m = Identity();
  m.data[3] = vec.x;
  m.data[7] = vec.y;
  m.data[11] = vec.z;
  return m;
}

Matrix44 Matrix44::Shear(const float a, const float b)
{
  Matrix44 m = Identity();
  m.data[2] = a;
  m.data[6] = b;
  return m;
}

Matrix44 Matrix44::Perspective(float fov_y, float aspect_ratio, float z_near, float z_far)
{
  const float tan_half_fov = std::tan(fov_y / 2);
  const float depth = z_far - z_near;

  Matrix44 m{};
  m.data[0] = 1 / (aspect_ratio * tan_half_fov);
  m.data[5] = 1 / tan_half_fov;
  m.data[10] = -(z_far + z_near) / depth;
  m.data[11] = -(2 * z_far * z_near) / depth;
  m.data[14] = -1;
  return m;
}

void Matrix44::Multiply(const Matrix44& a, const Matrix44& b, Matrix44* result)
{
  result->data = Product<4, 4, 4>(a.data, b.data);
}

void Matrix44::Multiply(const Matrix44& a, const Vec4& vec, Vec4* result)
{
  result->data = Product<4, 4, 1>(a.data, vec.data);
}

Vec3 Matrix44::Transform(const Vec3& point, float w) const
{
  const std::array<float, 4> out = Product<4, 4, 1>(data, {point.x, point.y, point.z, w});
  return {out[0], out[1], out[2]};
}

namespace
{
// The 2x2 minors of the lower two rows (s) and upper two rows (c) of a 4x4 matrix; the Laplace
// expansion along those row pairs gives both the determinant and the adjugate. Computed in double
// for a little extra precision.
struct Minors44
{
  explicit Minors44(const std::array<float, 16>& m)
  {
    const auto e = [&m](int i) { return static_cast<double>(m[i]); };
    // Rows 0 and 1.
    c[0] = e(0) * e(5) - e(1) * e(4);
    c[1] = e(0) * e(6) - e(2) * e(4);
    c[2] = e(0) * e(7) - e(3) * e(4);
    c[3] = e(1) * e(6) - e(2) * e(5);
    c[4] = e(1) * e(7) - e(3) * e(5);
    c[5] = e(2) * e(7) - e(3) * e(6);
    // Rows 2 and 3.
    s[0] = e(8) * e(13) - e(9) * e(12);
    s[1] = e(8) * e(14) - e(10) * e(12);
    s[2] = e(8) * e(15) - e(11) * e(12);
    s[3] = e(9) * e(14) - e(10) * e(13);
    s[4] = e(9) * e(15) - e(11) * e(13);
    s[5] = e(10) * e(15) - e(11) * e(14);
  }

  double Determinant() const
  {
    return c[0] * s[5] - c[1] * s[4] + c[2] * s[3] + c[3] * s[2] - c[4] * s[1] + c[5] * s[0];
  }

  double c[6];
  double s[6];
};
}  // namespace

float Matrix44::Determinant() const
{
  return static_cast<float>(Minors44(data).Determinant());
}

Matrix44 Matrix44::Inverted() const
{
  const Minors44 k(data);
  const double inv_det = 1 / static_cast<double>(Determinant());
  const auto e = [this](int i) { return static_cast<double>(data[i]); };
  const double* c = k.c;
  const double* s = k.s;

  const double adj[16] = {
      e(5) * s[5] - e(6) * s[4] + e(7) * s[3],
      -e(1) * s[5] + e(2) * s[4] - e(3) * s[3],
      e(13) * c[5] - e(14) * c[4] + e(15) * c[3],
      -e(9) * c[5] + e(10) * c[4] - e(11) * c[3],

      -e(4) * s[5] + e(6) * s[2] - e(7) * s[1],
      e(0) * s[5] - e(2) * s[2] + e(3) * s[1],
      -e(12) * c[5] + e(14) * c[2] - e(15) * c[1],
      e(8) * c[5] - e(10) * c[2] + e(11) * c[1],

      e(4) * s[4] - e(5) * s[2] + e(7) * s[0],
      -e(0) * s[4] + e(1) * s[2] - e(3) * s[0],
      e(12) * c[4] - e(13) * c[2] + e(15) * c[0],
      -e(8) * c[4] + e(9) * c[2] - e(11) * c[0],

      -e(4) * s[3] + e(5) * s[1] - e(6) * s[0],
      e(0) * s[3] - e(1) * s[1] + e(2) * s[0],
      -e(12) * c[3] + e(13) * c[1] - e(14) * c[0],
      e(8) * c[3] - e(9) * c[1] + e(10) * c[0],
  };

  Matrix44 inv;
  for (int i = 0; i < 16; ++i)
    inv.data[i] = static_cast<float>(adj[i] * inv_det);
  return inv;
}

Matrix44 Matrix44::Transposed() const
{
  Matrix44 t;
  for (int row = 0; row < 4; ++row)
  {
    for (int col = 0; col < 4; ++col)
      t.data[col * 4 + row] = data[row * 4 + col];
  }
  return t;
}
}  // namespace Common
