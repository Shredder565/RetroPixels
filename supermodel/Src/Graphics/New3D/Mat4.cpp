// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Mat4.cpp

#include "Mat4.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace New3D {

namespace
{
  constexpr double kPi = 3.14159265358979323846;

  // Element (row, col) of a column-major matrix.
  inline int At(int row, int col) { return col * 4 + row; }

  void Identity(float m[16])
  {
    for (int i = 0; i < 16; i++)
      m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  }
}

Mat4::Mat4()
{
  LoadIdentity();
}

void Mat4::LoadIdentity()
{
  Identity(currentMatrix);
}

// out = a * b. out may be the same array as a: each output row only depends
// on the same row of a, which is read before it is written.
void Mat4::Multiply(const float a[16], const float b[16], float out[16])
{
  for (int row = 0; row < 4; row++)
  {
    const float r0 = a[At(row, 0)];
    const float r1 = a[At(row, 1)];
    const float r2 = a[At(row, 2)];
    const float r3 = a[At(row, 3)];
    for (int col = 0; col < 4; col++)
      out[At(row, col)] = r0 * b[At(0, col)] + r1 * b[At(1, col)] + r2 * b[At(2, col)] + r3 * b[At(3, col)];
  }
}

void Mat4::Apply(const float m[16])
{
  Multiply(currentMatrix, m, currentMatrix);
}

void Mat4::Translate(float x, float y, float z)
{
  float m[16];
  Identity(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
  Apply(m);
}

void Mat4::Scale(float x, float y, float z)
{
  float m[16];
  Identity(m);
  m[0] = x;
  m[5] = y;
  m[10] = z;
  Apply(m);
}

void Mat4::Rotate(float angleDegrees, float x, float y, float z)
{
  const float inv = 1.0f / std::sqrt(x * x + y * y + z * z);
  x *= inv;
  y *= inv;
  z *= inv;

  const float rad = angleDegrees * float(kPi / 180.0);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float t = 1.0f - c;

  float m[16];
  Identity(m);
  m[At(0, 0)] = (x * x) * t + c;
  m[At(1, 0)] = (y * x) * t + (z * s);
  m[At(2, 0)] = (x * z) * t - (y * s);
  m[At(0, 1)] = (x * y) * t - (z * s);
  m[At(1, 1)] = (y * y) * t + c;
  m[At(2, 1)] = (y * z) * t + (x * s);
  m[At(0, 2)] = (x * z) * t + (y * s);
  m[At(1, 2)] = (y * z) * t - (x * s);
  m[At(2, 2)] = (z * z) * t + c;
  Apply(m);
}

void Mat4::Frustum(float left, float right, float bottom, float top, float nearVal, float farVal)
{
  float m[16] = {};
  m[0] = 2.0f / (right - left);
  m[5] = 2.0f / (top - bottom);
  m[8] = (right + left) / (right - left);
  m[9] = (top + bottom) / (top - bottom);
  m[10] = -(farVal + nearVal) / (farVal - nearVal);
  m[11] = -1.0f;
  m[14] = -(2.0f * farVal * nearVal) / (farVal - nearVal);
  Apply(m);
}

// Depth comes out as near / -z: 1 at the near plane, falling towards 0 at
// infinity, for use with a "greater" depth test.
void Mat4::FrustumRZ(float left, float right, float bottom, float top, float nearVal)
{
  float m[16] = {};
  m[0] = 2.0f / (right - left);
  m[5] = 2.0f / (top - bottom);
  m[8] = (right + left) / (right - left);
  m[9] = (top + bottom) / (top - bottom);
  m[11] = -1.0f;
  m[14] = nearVal;
  Apply(m);
}

void Mat4::Perspective(float fovyDegrees, float aspect, float zNear, float zFar)
{
  const float halfHeight = std::tan(fovyDegrees * float(kPi / 360.0));
  const float halfWidth = halfHeight * aspect;
  Frustum(-halfWidth, halfWidth, -halfHeight, halfHeight, zNear, zFar);
}

void Mat4::Ortho(float left, float right, float bottom, float top, float nearVal, float farVal)
{
  float m[16];
  Identity(m);
  m[0] = 2.0f / (right - left);
  m[5] = 2.0f / (top - bottom);
  m[10] = -2.0f / (farVal - nearVal);
  m[12] = -(right + left) / (right - left);
  m[13] = -(top + bottom) / (top - bottom);
  m[14] = -(farVal + nearVal) / (farVal - nearVal);
  Apply(m);
}

void Mat4::MultMatrix(const float *m)
{
  if (m)
    Apply(m);
}

void Mat4::LoadMatrix(const float *m)
{
  if (m)
    std::memcpy(currentMatrix, m, sizeof(currentMatrix));
}

void Mat4::LoadTransposeMatrix(const float *m)
{
  if (!m)
    return;
  for (int row = 0; row < 4; row++)
    for (int col = 0; col < 4; col++)
      currentMatrix[At(row, col)] = m[At(col, row)];
}

void Mat4::MultTransposeMatrix(const float *m)
{
  if (!m)
    return;
  float t[16];
  for (int row = 0; row < 4; row++)
    for (int col = 0; col < 4; col++)
      t[At(row, col)] = m[At(col, row)];
  Apply(t);
}

void Mat4::PushMatrix()
{
  if (m_saved.size() > 128)
    return;
  std::array<float, 16> copy;
  std::memcpy(copy.data(), currentMatrix, sizeof(currentMatrix));
  m_saved.push_back(copy);
}

void Mat4::PopMatrix()
{
  if (m_saved.empty())
    return;
  std::memcpy(currentMatrix, m_saved.back().data(), sizeof(currentMatrix));
  m_saved.pop_back();
}

void Mat4::Release()
{
  m_saved.clear();
}

} // New3D
