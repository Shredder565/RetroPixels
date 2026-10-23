// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Vec.cpp

#include "Vec.h"

#include <cmath>

namespace New3D {
namespace V3 {

void Set(Vec3 v, float x, float y, float z)
{
  v[0] = x;
  v[1] = y;
  v[2] = z;
}

void Copy(const Vec3 from, Vec3 to)
{
  for (int i = 0; i < 3; i++)
    to[i] = from[i];
}

void Negate(Vec3 v)
{
  for (int i = 0; i < 3; i++)
    v[i] = -v[i];
}

float Dot(const Vec3 a, const Vec3 b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross(const Vec3 a, const Vec3 b, Vec3 out)
{
  float x = a[1] * b[2] - a[2] * b[1];
  float y = a[2] * b[0] - a[0] * b[2];
  float z = a[0] * b[1] - a[1] * b[0];
  Set(out, x, y, z);
}

float Length(const Vec3 v)
{
  return std::sqrt(Dot(v, v));
}

void Normalise(Vec3 v)
{
  float len = Length(v);
  if (len > 0.0f)
  {
    for (int i = 0; i < 3; i++)
      v[i] /= len;
  }
}

} // V3
} // New3D
