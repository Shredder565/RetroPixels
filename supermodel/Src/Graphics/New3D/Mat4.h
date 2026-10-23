// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Mat4.h
//
// A 4x4 column-major float matrix with a small save/restore stack, in the
// style of the classic OpenGL matrix calls. Every operation post-multiplies
// the current matrix (current = current * op).

#ifndef INCLUDED_NEW3D_MAT4_H
#define INCLUDED_NEW3D_MAT4_H

#include <array>
#include <vector>

namespace New3D {

class Mat4
{
public:
  Mat4();

  void LoadIdentity();
  void Translate(float x, float y, float z);
  void Rotate(float angleDegrees, float x, float y, float z);
  void Scale(float x, float y, float z);
  void Frustum(float left, float right, float bottom, float top, float nearVal, float farVal);
  void FrustumRZ(float left, float right, float bottom, float top, float nearVal);   // reversed Z, far plane at infinity
  void Perspective(float fovyDegrees, float aspect, float zNear, float zFar);
  void Ortho(float left, float right, float bottom, float top, float nearVal, float farVal);
  void MultMatrix(const float *m);
  void LoadMatrix(const float *m);
  void LoadTransposeMatrix(const float *m);
  void MultTransposeMatrix(const float *m);

  // The stack keeps at most 129 saved matrices; further pushes are ignored.
  void PushMatrix();
  void PopMatrix();
  void Release();     // empties the stack

  operator float *() { return currentMatrix; }
  operator const float *() const { return currentMatrix; }

  float currentMatrix[16];

private:
  static void Multiply(const float a[16], const float b[16], float out[16]);
  void Apply(const float m[16]);

  std::vector<std::array<float, 16>> m_saved;
};

} // New3D

#endif  // INCLUDED_NEW3D_MAT4_H
