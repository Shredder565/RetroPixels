// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Vec.h
//
// Small helpers for 3-component float vectors stored as plain arrays.

#ifndef INCLUDED_NEW3D_VEC_H
#define INCLUDED_NEW3D_VEC_H

namespace New3D {
namespace V3 {

typedef float Vec3[3];

void Set(Vec3 v, float x, float y, float z);
void Copy(const Vec3 from, Vec3 to);
void Negate(Vec3 v);
float Dot(const Vec3 a, const Vec3 b);
void Cross(const Vec3 a, const Vec3 b, Vec3 out);
float Length(const Vec3 v);
void Normalise(Vec3 v);

} // V3
} // New3D

#endif  // INCLUDED_NEW3D_VEC_H
