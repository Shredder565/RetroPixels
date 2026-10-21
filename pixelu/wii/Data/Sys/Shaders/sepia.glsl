// Old photograph: brightness kept, colour replaced by a warm brown tint.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

/*
[configuration]

[OptionRangeFloat]
GUIName = Amount
OptionName = AMOUNT
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Tint
OptionName = TINT
MinValue = 0.0, 0.0, 0.0
MaxValue = 1.5, 1.5, 1.5
StepAmount = 0.01, 0.01, 0.01
DefaultValue = 1.12, 0.96, 0.74

[/configuration]
*/

void main()
{
  float4 color = Sample();
  float y = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
  float3 toned = clamp(y * GetOption(TINT), 0.0, 1.0);
  SetOutput(float4(mix(color.rgb, toned, GetOption(AMOUNT)), color.a));
}
