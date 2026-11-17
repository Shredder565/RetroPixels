PixelU-Wii's patches to third-party sources

Externals/fetch.py applies these to the upstream sources named in
Externals/sources.json, right after fetching them. Each file explains itself
at the top; all are PixelU-Wii's own, written against the upstream release.

  fatfs-utf8-putc.patch         FatFs R0.14b: ChaN's published fix for the
                                UTF-8 lead-byte test in putc_bfd().
  fatfs-ffconf.patch            FatFs R0.14b: the four ffconf.h options
                                PixelU-Wii's code needs.

Not patched, but adapted in Externals/ instead:
  - FatFs: Externals/FatFs/include/ff.h keeps FatFs' TCHAR, _T and _TEXT apart
    from Windows' names without changing FatFs.
  - liblzma: Externals/liblzma/config.h (XZ Utils' configure answers) also
    includes <windows.h>/<intrin.h>, which tuklib_integer.h needs under MSVC.

Applying: fetch.py's own unified-diff reader (exact context, the file's line
ends kept), so no patch(1) or git apply is needed.
