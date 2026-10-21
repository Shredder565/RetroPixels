// Black and white: each pixel becomes its BT.709 luma.
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

[/configuration]
*/

void main()
{
  float4 color = Sample();
  float y = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
  SetOutput(float4(mix(color.rgb, float3(y, y, y), GetOption(AMOUNT)), color.a));
}
