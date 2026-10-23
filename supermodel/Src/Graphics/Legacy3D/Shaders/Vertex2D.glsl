// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Vertex2D.glsl
//
// A copy of the tile generator layer vertex shader in Graphics/Shaders2D.h.

#version 410 core

out vec2 vUV;

void main(void)
{
  vec2 corner = vec2(float((gl_VertexID & 2) >> 1), float(gl_VertexID & 1));
  vUV = vec2(corner.x, 1.0 - corner.y);
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
