FreeSurround, kept in this folder (third-party code, GPL-2.0-or-later)

include/ and source/ are FreeSurround, Christian Kothe's DPL II style
stereo-to-surround decoder (2007-2010, GPL-2.0-or-later), with Mark
Borgerding's KISS FFT (BSD-3-Clause; licence text in the file headers).

There is no upstream repository to fetch this from. Kothe's decoder was
published as part of a foobar2000 plugin (foo_dsp_fsurround); the version
PixelU-Wii's AudioCommon/SurroundDecoder.cpp is written against
(DPL2FSDecoder, 5.1 and 7.1 only) is the cut-down port made for Dolphin in
2017 and kept in Dolphin's Externals/FreeSurround, where these ten files were
copied from unchanged (Dolphin 2609a, commit 409881c3). That port includes a
2023 fix in FreeSurroundDecoder.cpp and KissFFTR.cpp (memory from new[] was
freed with delete instead of delete[]).

So this is the one place where PixelU-Wii still carries a third-party library
as it was modified inside the Dolphin project; the licence (GPL-2.0-or-later)
is the authors' own, and PixelU-Wii's source is GPL-2.0-or-later.
