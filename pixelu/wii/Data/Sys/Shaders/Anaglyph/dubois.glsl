// Red-cyan anaglyph 3D, the least-squares method of Eric Dubois.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Each output channel mixes both eyes' colours so that, seen through red (left) and cyan (right)
// filters, the picture keeps as much of its colour as the glasses allow. The coefficients are
// the red-cyan matrices Dubois published for typical LCD displays and glasses ("Conversion of a
// stereo pair to anaglyph with the least-squares projection method", E. Dubois, 2009 revision).
// Layer 0 is the left eye, layer 1 the right eye.

void main()
{
  float3 l = SampleLayer(0).rgb;
  float3 r = SampleLayer(1).rgb;

  float3 color;
  color.r = dot(float3(0.437, 0.449, 0.164), l) + dot(float3(-0.011, -0.032, -0.007), r);
  color.g = dot(float3(-0.062, -0.062, -0.024), l) + dot(float3(0.377, 0.761, 0.009), r);
  color.b = dot(float3(-0.048, -0.050, -0.017), l) + dot(float3(-0.026, -0.093, 1.234), r);

  SetOutput(float4(max(color, float3(0.0, 0.0, 0.0)), SampleLayer(0).a));
}
