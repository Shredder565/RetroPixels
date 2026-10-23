// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Plane.h
//
// A plane a*x + b*y + c*z + d = 0.

#ifndef INCLUDED_NEW3D_PLANE_H
#define INCLUDED_NEW3D_PLANE_H

#include <cmath>

struct Plane
{
  float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;

  void Normalise()
  {
    float scale = 1.0f / std::sqrt(a * a + b * b + c * c);
    a *= scale;
    b *= scale;
    c *= scale;
    d *= scale;
  }

  float DistanceToPoint(const float p[3]) const { return a * p[0] + b * p[1] + c * p[2] + d; }
  float DotProduct(const float p[3]) const { return a * p[0] + b * p[1] + c * p[2]; }
};

#endif  // INCLUDED_NEW3D_PLANE_H
