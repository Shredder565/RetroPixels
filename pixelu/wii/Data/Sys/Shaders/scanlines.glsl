// CRT-style scanlines: every other line of the game's picture is darkened.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// The lines follow the game's own lines (the source picture, so they scale with the internal
// resolution setting divided by "Lines per scanline"), with a soft cosine profile.

/*
[configuration]

[OptionRangeFloat]
GUIName = Darkness
OptionName = DARKNESS
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.35

[OptionRangeInteger]
GUIName = Lines per scanline
OptionName = LINES
MinValue = 1
MaxValue = 8
StepAmount = 1
DefaultValue = 1

[/configuration]
*/

void main()
{
  float4 color = Sample();
  float line = GetCoordinates().y * GetResolution().y / float(max(GetOption(LINES), 1));
  // 1 in the middle of a line, 0 between lines.
  float profile = 0.5 + 0.5 * cos(6.2831853 * (fract(line) - 0.5));
  float shade = 1.0 - GetOption(DARKNESS) * (1.0 - profile);
  SetOutput(float4(color.rgb * shade, color.a));
}
