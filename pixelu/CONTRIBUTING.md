# Changing PixelU

PixelU is part of RetroPixels, so changes go through the RetroPixels
repository like any other change.

- Keep each change small and make sure it still builds on Windows: the
  `pixelu.yml` workflow has to pass.
- Write new code yourself. Do not paste code from Cemu, Dolphin or other
  emulators. The point of the rewrite is that PixelU no longer depends on
  them (see `docs/pixelu-original.md`). A new file starts with
  `// Original code for RetroPixels (PixelU). Not derived from Cemu.`
- Third-party libraries and data go in `dependencies/` (or stay where
  THIRD_PARTY.md lists them), keep their own licence, and are listed in
  THIRD_PARTY.md and in the `THIRD_PARTY` tuple of `tools/pixelu-original.py`.
- Do not rename the settings files, the folders PixelU creates (for example
  `usr/packages/title/cemu_*`) or the settings keys. Existing users' data
  depends on them.
- Never add Nintendo keys, firmware, system files or games.
