// AutoHDR: lets the brightest parts of an SDR game use an HDR display's extra range.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Needs HDR output (Graphics > Enhancements > HDR Post-Processing) on an HDR display; otherwise the
// picture passes through unchanged. Colours up to the knee keep their SDR brightness (relative to
// the paper white chosen in the colour correction settings). Above it, brightness is stretched
// along a curve that reaches the display's peak at SDR white, keeping each colour's hue.

/*
[configuration]

[OptionRangeFloat]
GUIName = Display peak brightness (nits)
OptionName = PEAK_NITS
MinValue = 100
MaxValue = 4000
StepAmount = 10
DefaultValue = 600

[OptionRangeFloat]
GUIName = Knee (share of SDR white left untouched)
OptionName = KNEE
MinValue = 0.0
MaxValue = 0.95
StepAmount = 0.01
DefaultValue = 0.5

[OptionRangeFloat]
GUIName = Curve steepness
OptionName = STEEPNESS
MinValue = 1.0
MaxValue = 6.0
StepAmount = 0.1
DefaultValue = 2.0

[/configuration]
*/

void main()
{
  float4 color = Sample();
  if (!OptionEnabled(hdr_output) || !OptionEnabled(linear_space_output))
  {
    SetOutput(color);
    return;
  }

  // Back to SDR units, where 1.0 is SDR white.
  float paper_white = hdr_paper_white_nits / hdr_sdr_white_nits;
  float3 sdr = color.rgb / paper_white;
  float y = dot(sdr, float3(0.2126, 0.7152, 0.0722));

  // Brightness gain at SDR white, so that it lands on the display's peak.
  float peak_gain = max(GetOption(PEAK_NITS) / hdr_paper_white_nits, 1.0);
  float knee = GetOption(KNEE);
  if (y > knee)
  {
    // 0 at the knee, 1 at SDR white (and beyond for values the game pushed past white).
    float t = (min(y, 1.0) - knee) / (1.0 - knee);
    float gain = 1.0 + (peak_gain - 1.0) * pow(t, GetOption(STEEPNESS));
    sdr *= gain;
  }

  SetOutput(float4(sdr * paper_white, color.a));
}
