PixelU-Wii's patches to third-party sources

Externals/fetch.py applies these to the upstream sources named in
Externals/sources.json, right after fetching them. Each file explains itself
at the top; all are PixelU-Wii's own, written against the upstream release.

  bochs-disasm-adc-imm32.patch  Bochs disassembler: "adc r/m64, imm32" reads a
                                sign-extended 32-bit immediate (one table entry).
  fatfs-utf8-putc.patch         FatFs R0.14b: ChaN's published fix for the
                                UTF-8 lead-byte test in putc_bfd().
  fatfs-ffconf.patch            FatFs R0.14b: the four ffconf.h options
                                PixelU-Wii's code needs.

Not patched, but adapted in Externals/ instead:
  - FatFs: Externals/FatFs/include/ff.h keeps FatFs' TCHAR, _T and _TEXT apart
    from Windows' names without changing FatFs.
  - liblzma: Externals/liblzma/config.h (XZ Utils' configure answers) also
    includes <windows.h>/<intrin.h>, which tuklib_integer.h needs under MSVC.
  - Bochs disassembler: Externals/Bochs_disasm/include/config.h replaces the
    config.h Bochs' configure script writes.
  - miniLZO: Externals/LZO/include/lzo/lzo1x.h points <lzo/lzo1x.h> at minilzo.h.

Applying: fetch.py's own unified-diff reader (exact context, the file's line
ends kept), so no patch(1) or git apply is needed.
