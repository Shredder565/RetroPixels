// Red-cyan anaglyph 3D, the simple way: red from the left eye, green and blue from the right.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Brighter than the Dubois shader but with more ghosting and colour rivalry. The "Monochrome"
// option turns each eye to grey first, which removes the rivalry at the cost of colour.

/*
[configuration]

[OptionBool]
GUIName = Monochrome
OptionName = MONOCHROME
DefaultValue = false

[/configuration]
*/

void main()
{
  float3 l = SampleLayer(0).rgb;
  float3 r = SampleLayer(1).rgb;

  if (OptionEnabled(MONOCHROME))
  {
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    l = float3(dot(l, luma), dot(l, luma), dot(l, luma));
    r = float3(dot(r, luma), dot(r, luma), dot(r, luma));
  }

  SetOutput(float4(l.r, r.g, r.b, SampleLayer(0).a));
}
