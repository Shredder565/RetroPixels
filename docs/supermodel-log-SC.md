# Supermodel rewrite: batch SC (graphics)

Batch SC covers the Real3D GPU front end, the tile generator and every file
under `supermodel/Src/Graphics/`:

- `Model3/Real3D.cpp/.h`, `Model3/TileGen.cpp/.h`, `Model3/TileGenBuffer.h`
- `Graphics/` top level: `FBO`, `IRender3D.h`, `Render2D`, `Shader`, `Shaders2D.h`, `SuperAA`
- `Graphics/New3D/` (renderer, shaders, maths, buffers, texture bank, scroll fog)
- `Graphics/Legacy3D/` (renderer, models, texture refs, `Shaders3D.h`, `Shaders/`)

Every file was read only for its interface (class and method names other
files call, config keys, the save-state blocks, the Real3D memory map,
registers, display list and polygon formats, the uniform and attribute names
user shader files bind to). Then it was written again from scratch. Each file
starts with the RetroPixels original-code header and has no Supermodel licence
text. The `#ifdef SUPERMODEL_DEBUGGER` code and the debugger hooks are gone.
File names are unchanged.

## Line counts

Physical lines, before (Supermodel, as brought in by S0) and after.

| File | Before | After |
|---|---:|---:|
| Graphics/FBO.cpp | 73 | 63 |
| Graphics/FBO.h | 27 | 32 |
| Graphics/IRender3D.h | 31 | 41 |
| Graphics/Legacy3D/Error.cpp | 68 | 39 |
| Graphics/Legacy3D/Legacy3D.cpp | 1400 | 728 |
| Graphics/Legacy3D/Legacy3D.h | 543 | 254 |
| Graphics/Legacy3D/Models.cpp | 1288 | 630 |
| Graphics/Legacy3D/Shaders/DIR.txt | 1 | 7 |
| Graphics/Legacy3D/Shaders/Fragment.glsl | 194 | 85 |
| Graphics/Legacy3D/Shaders/Fragment2D.glsl | 42 | 80 |
| Graphics/Legacy3D/Shaders/Fragment_Flat.glsl | 142 | 79 |
| Graphics/Legacy3D/Shaders/Fragment_MultiSheet.glsl | 239 | 99 |
| Graphics/Legacy3D/Shaders/Fragment_NoSpotlight.glsl | 196 | 80 |
| Graphics/Legacy3D/Shaders/Vertex.glsl | 209 | 74 |
| Graphics/Legacy3D/Shaders/Vertex2D.glsl | 33 | 16 |
| Graphics/Legacy3D/Shaders3D.h | 694 | 203 |
| Graphics/Legacy3D/TextureRefs.cpp | 335 | 66 |
| Graphics/Legacy3D/TextureRefs.h | 170 | 47 |
| Graphics/New3D/GLSLShader.cpp | 150 | 134 |
| Graphics/New3D/GLSLShader.h | 60 | 55 |
| Graphics/New3D/Mat4.cpp | 339 | 204 |
| Graphics/New3D/Mat4.h | 51 | 54 |
| Graphics/New3D/Model.cpp | 75 | 53 |
| Graphics/New3D/Model.h | 277 | 202 |
| Graphics/New3D/New3D.cpp | 1750 | 1291 |
| Graphics/New3D/New3D.h | 314 | 159 |
| Graphics/New3D/Plane.h | 27 | 29 |
| Graphics/New3D/PolyHeader.cpp | 414 | 77 |
| Graphics/New3D/PolyHeader.h | 165 | 130 |
| Graphics/New3D/R3DData.h | 19 | 29 |
| Graphics/New3D/R3DFloat.cpp | 41 | 33 |
| Graphics/New3D/R3DFloat.h | 17 | 23 |
| Graphics/New3D/R3DFrameBuffers.cpp | 334 | 215 |
| Graphics/New3D/R3DFrameBuffers.h | 55 | 59 |
| Graphics/New3D/R3DScrollFog.cpp | 173 | 86 |
| Graphics/New3D/R3DScrollFog.h | 57 | 40 |
| Graphics/New3D/R3DShader.cpp | 447 | 315 |
| Graphics/New3D/R3DShader.h | 137 | 91 |
| Graphics/New3D/R3DShaderCommon.h | 374 | 331 |
| Graphics/New3D/R3DShaderQuads.h | 510 | 304 |
| Graphics/New3D/R3DShaderTriangles.h | 260 | 84 |
| Graphics/New3D/TextureBank.cpp | 71 | 74 |
| Graphics/New3D/TextureBank.h | 33 | 40 |
| Graphics/New3D/VBO.cpp | 80 | 63 |
| Graphics/New3D/VBO.h | 27 | 34 |
| Graphics/New3D/Vec.cpp | 207 | 60 |
| Graphics/New3D/Vec.h | 41 | 26 |
| Graphics/Render2D.cpp | 480 | 166 |
| Graphics/Render2D.h | 215 | 71 |
| Graphics/Shader.cpp | 188 | 102 |
| Graphics/Shader.h | 75 | 28 |
| Graphics/Shaders2D.h | 136 | 105 |
| Graphics/SuperAA.cpp | 229 | 159 |
| Graphics/SuperAA.h | 32 | 42 |
| Model3/Real3D.cpp | 1243 | 791 |
| Model3/Real3D.h | 560 | 165 |
| Model3/TileGen.cpp | 631 | 380 |
| Model3/TileGen.h | 335 | 88 |
| Model3/TileGenBuffer.h | 17 | 27 |
| total | 16331 | 9042 |

## Fixes

Behaviour is unchanged wherever the old code was well defined. The rewrite
differs from the old code only where the old code read or wrote memory it
didn't own:

- **Real3D texture uploads past the end of memory.** A texture whose mip chain
  runs off the end of texture RAM made the old `StoreTexture` write past the end
  of the Real3D memory pool, which is a heap overflow. AddressSanitizer catches
  it on harness seed 28, Step 1.0. In the harness the stray writes landed in
  the VROM buffer. The new code keeps writing on into the FIFO area, as the
  hardware's flat memory would, and stops at the end of Real3D memory.
- **New3D and Legacy3D polygon state.** Both old model decoders kept the current
  polygon (and Legacy3D its "previous vertices") in uninitialised stack
  variables. When a polygon shares vertices with one the decoder never stored,
  the old output depends on stack garbage. The new decoders start from zeroed
  state. The harnesses compare the new code with a copy of the old code patched
  the same way (`oldz`) and, for New3D, also accept any of three old runs when
  the old build disagrees with itself.
- `PolyHeader.h` had a UTF-8 byte-order mark before the header line. It was
  removed.

## Checks

Old and new sources were compiled side by side into the same harness and fed
the same generated inputs. All GL harnesses ran on Mesa llvmpipe
(single-threaded, `LP_NUM_THREADS=0`) under Xvfb.

- **Real3D**: 320 runs (40 seeds, multithreading off and on, Steps 1.0, 1.5,
  2.0 and 2.1). Each run has 3000 random operations: culling and polygon RAM
  writes (including the update lists), texture FIFO and texture port uploads,
  VROM uploads, DMA, registers, PCI config, JTAG mode words, flushes,
  ping-pong flips, VBlanks, whole frames and resets. The harness compares
  every value read back, hashes the memory the renderer sees at each sync and
  frame, and compares the saved `Real3D` block. Then each build loads the
  other's save state and runs 300 more operations. 318 runs are identical.
  The other 2 are seed 28, Step 1.0 (both thread modes): the old build's heap
  overflow (see Fixes), confirmed with AddressSanitizer. The new build is
  clean under ASan.
- **TileGen**: 200 seeds of 3000 random operations: 8/16/32-bit VRAM and
  palette writes and reads, register writes and reads (layer control,
  scroll, IRQ enable/acknowledge), line draws, full redraws, VBlanks with
  snapshot syncs, and resets. The harness compares every value read, the
  IRQ state and the saved `Tile Generator` block. Then each build loads the other's
  state and runs 200 more operations. All 200 are identical.
- **Render2D, SuperAA, FBO, Shader**: 768 runs (2 seeds: supersampling 1, 2 and
  3; CRT colour modes 0-5; upscale modes 0-3; 496x384, 744x576 and 992x768;
  wide background off and on). The harness renders the layers and the final
  composite and compares them pixel for pixel. All 768 are identical,
  including bicubic upscaling.
- **New3D**: 320 runs (40 random scene databases, Steps 1.0 and 2.1,
  triangle and quad rendering, normal and wide screen, 3 frames each with
  the scene changing between frames). The harness compares pixels,
  line-of-sight values and the GL error state against the old code with its
  polygon state zero-initialised (`oldz`). 291 runs are identical. In 25 the
  old build disagrees with itself across three runs (uninitialised data,
  mostly line-of-sight samples off the frame buffer), and the new output
  equals one of its runs or differs only in those samples. In the remaining
  4 neither build is stable on llvmpipe: ten runs of each case flip, about one
  run in ten, to the same alternate image in both builds (identical
  14416-byte difference). Rerun, all 4 match.
- **Legacy3D**: 127 runs (seeds 1-20; Steps 1.0, 1.5, 2.0 and 2.1; normal,
  wide and high resolution; one or several viewports; pointer-list roots of
  up to 3 trees; single and multi texture-map mode with `MaxTexMaps` and
  `MaxTexMapExtent`; 2-3 frames). The harness compares
  pixels and the GL error state. 126 are identical. Seed 17 with 3 root trees
  differs from the plain old build and equals `oldz` (old polygon state
  zero-initialised), so the difference is the old uninitialised read.

The MinGW-w64 cross-build (`sh supermodel/build.sh windows`) succeeds. It
gives no warnings in batch files: the only warnings are in `GameLoader.cpp`,
`OSD/SDL/Gui.cpp` and `OSD/Windows/FileSystemPath.cpp` (other batches). A native
`g++ -Wall -Wextra -fsyntax-only` of every batch source shows warnings only in
shared headers outside the batch (`CPU/Bus.h`, `Util/GenericValue.h`).

## Legacy3D shader files

`Graphics/Legacy3D/Shaders/*.glsl` are not built or shipped. They are reference
copies, and users can load such files with `-vert-shader` / `-frag-shader`
(the `VertexShader` / `FragmentShader` settings). They were regenerated from
the rewritten built-in programs:

- `Vertex`, `Fragment` and `Fragment_MultiSheet` copy the programs in `Shaders3D.h`.
- `Fragment_NoSpotlight` is the single-map program without the spotlight.
- `Fragment_Flat` is the single-map program with no lighting.
- `Vertex2D` and `Fragment2D` copy the `Shaders2D.h` layer shaders, with
  `UPSCALEMODE` 0.

All seven compile with `glslangValidator`. `DIR.txt` now describes the folder.

## Left in the batch

Nothing. Every file in the batch is rewritten.
