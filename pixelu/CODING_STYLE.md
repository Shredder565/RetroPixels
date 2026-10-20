# PixelU code style

Match the code around you. The rules below cover new code.

- C++20, tabs for indentation, braces on their own line. Running
  `.clang-format` on the lines you change is fine. Do not reformat whole
  files.
- Use the integer types from `Common/precompiled.h` (`uint8`, `sint16`,
  `uint32`, `uint64`, ...). Use `uint32be` and the other `betype` wrappers
  for values that live in Wii U memory, which is big-endian.
- Members start with `m_`, statics with `s_`. Types and functions use
  PascalCase, variables camelCase.
- Put file-local helpers and constants in an anonymous namespace.
- Comments say what a piece of code is for, or which Wii U behaviour it
  copies. They do not describe Cemu.
- Write log output with `cemuLog_log(LogType::..., "...")` (fmt syntax).
  Wrap user-visible text in `_("...")` in wxWidgets code and in `_tr("...")`
  elsewhere, so it can be translated.
