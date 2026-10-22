// Sharpen: an unsharp mask over the four direct neighbours of each pixel.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

/*
[configuration]

[OptionRangeFloat]
GUIName = Strength
OptionName = STRENGTH
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.05
DefaultValue = 0.5

[OptionRangeFloat]
GUIName = Limit (largest change per pixel)
OptionName = LIMIT
MinValue = 0.01
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.2

[/configuration]
*/

void main()
{
  float4 center = Sample();
  float2 step = GetInvResolution();
  float2 uv = GetCoordinates();
  float3 around = SampleLocation(uv + float2(step.x, 0.0)).rgb + SampleLocation(uv - float2(step.x, 0.0)).rgb +
                  SampleLocation(uv + float2(0.0, step.y)).rgb + SampleLocation(uv - float2(0.0, step.y)).rgb;
  float3 detail = center.rgb - around * 0.25;
  float limit = GetOption(LIMIT);
  float3 change = clamp(detail * GetOption(STRENGTH), -limit, limit);
  SetOutput(float4(max(center.rgb + change, float3(0.0, 0.0, 0.0)), center.a));
}
