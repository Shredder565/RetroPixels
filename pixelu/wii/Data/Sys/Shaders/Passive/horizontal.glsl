// Passive 3D for row-interleaved (line-polarised) displays.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Even window rows show the left eye (layer 0), odd rows the right eye (layer 1).

void main()
{
  int row = int(floor(GetCoordinates().y * GetWindowResolution().y));
  SetOutput(SampleLayer(row - (row / 2) * 2));
}
