#!/usr/bin/env python3
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
"""Checks a packaged Sys folder (PixelU\\wii\\Sys) file by file.

  python3 pixelu/wii/Data/check_sys_package.py PACKAGE_SYS [THIRD_PARTY_SYS]

Every file in PACKAGE_SYS must be one of:
  - a file of PixelU-Wii's own pixelu/wii/Data/Sys, with the same bytes;
  - a file matched by the third-party list in pixelu/wii/CMake/PixelUAssembleSys.cmake (the
    files still taken from the Dolphin checkout), with the same bytes as THIRD_PARTY_SYS's copy
    when that folder is given.
Anything else (for example one of the Dolphin data files that no longer ship: its themes,
shaders, graphics mods, totaldb.dsy, WiiConnect24 files or fonts) is an error, and so is any of
PixelU-Wii's own files that is missing. The check also makes sure that every theme icon the
program asks for (Resources::GetThemeIcon in Source/Core/DolphinQt) is in the bundled theme,
and that the shaders the code asks for by name are there.
Exit status 0 when everything is in order.
"""

import fnmatch
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WII = os.path.dirname(HERE)
OWN_SYS = os.path.join(HERE, "Sys")
ASSEMBLE = os.path.join(WII, "CMake", "PixelUAssembleSys.cmake")
QT_SOURCE = os.path.join(WII, "Source", "Core", "DolphinQt")
COMMON_PATHS = os.path.join(WII, "Source", "Core", "Common", "CommonPaths.h")

# Shaders the code loads by name (VideoCommon/PostProcessing.cpp, DolphinQt/HotkeyScheduler.cpp,
# DolphinQt/Config/Graphics/EnhancementsWidget.cpp).
NAMED_SHADERS = ["Shaders/default_pre_post_process.glsl", "Shaders/Anaglyph/dubois.glsl",
                 "Shaders/Passive/horizontal.glsl"]

# Files the program reads from Sys, wherever they come from.
REQUIRED = NAMED_SHADERS + [
    "GC/font_western.bin", "GC/font_japanese.bin", "GC/dsp_rom.bin", "GC/dsp_coef.bin",
    "Wii/shared2/wc24/nwc24msg.cfg", "Wii/shared2/wc24/nwc24dl.bin", "codehandler.bin",
    "Resources/OSD_Font.ttf", "ApprovedInis.json", "wiitdb-en.txt", "triforcetdb-en.txt",
]


def files_under(root):
    out = set()
    for base, _, names in os.walk(root):
        for name in names:
            out.add(os.path.relpath(os.path.join(base, name), root).replace(os.sep, "/"))
    return out


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.digest()


def third_party_patterns():
    text = open(ASSEMBLE, encoding="utf-8").read()
    block = re.search(r"set\(from_third_party(.*?)\n\)", text, re.S).group(1)
    return re.findall(r'^\s*"([^"]+)"', block, re.M)


def matches_third_party(rel, patterns):
    for pattern in patterns:
        if fnmatch.fnmatchcase(rel, pattern) or rel.startswith(pattern.rstrip("/") + "/"):
            return True
    return False


def theme_icon_names():
    names = set()
    for base, _, files in os.walk(QT_SOURCE):
        for name in files:
            if not name.endswith(".cpp"):
                continue
            text = open(os.path.join(base, name), encoding="utf-8", errors="replace").read()
            for call in re.findall(r"GetThemeIcon\(([^;]*?)\)", text):
                names.update(re.findall(r'"([a-z0-9_]+)"', call))
            for block in re.findall(r"DEBUG_ICONS\[\]\s*=\s*\{(.*?)\}", text, re.S):
                names.update(re.findall(r'"([a-z0-9_]+)"', block))
    return names


def default_theme():
    text = open(COMMON_PATHS, encoding="utf-8").read()
    return re.search(r'#define DEFAULT_THEME_DIR "([^"]+)"', text).group(1)


def main():
    args = sys.argv[1:]
    if not args or len(args) > 2:
        print(__doc__)
        return 2
    package = args[0]
    third_party = args[1] if len(args) == 2 else None
    errors = []

    own = files_under(OWN_SYS)
    have = files_under(package)
    patterns = third_party_patterns()

    from_own = from_third = 0
    for rel in sorted(have):
        path = os.path.join(package, rel)
        if rel in own:
            from_own += 1
            if digest(path) != digest(os.path.join(OWN_SYS, rel)):
                errors.append("%s is not PixelU-Wii's own (pixelu/wii/Data/Sys/%s)" % (rel, rel))
        elif matches_third_party(rel, patterns):
            from_third += 1
            if third_party is not None:
                source = os.path.join(third_party, rel)
                if not os.path.isfile(source) or digest(path) != digest(source):
                    errors.append("%s differs from the third-party copy" % rel)
        else:
            errors.append("%s should not be shipped (not PixelU-Wii's own, not on the "
                          "third-party list)" % rel)

    for rel in sorted(own - have):
        errors.append("%s is missing (PixelU-Wii's own file)" % rel)
    for rel in REQUIRED:
        if rel not in have:
            errors.append("%s is missing (the program reads it)" % rel)
    if not any(r.startswith("GameSettings/") for r in have):
        errors.append("GameSettings is missing")

    theme = default_theme()
    for name in sorted(theme_icon_names()):
        if "Themes/%s/%s.png" % (theme, name) not in have:
            errors.append("Themes/%s/%s.png is missing (the program loads it)" % (theme, name))
    for rel in sorted(have):
        if rel.startswith("Themes/") and not rel.startswith("Themes/%s/" % theme):
            errors.append("%s: only the %s theme is shipped" % (rel, theme))

    for e in errors:
        print("error:", e)
    print("Sys: %d files (%d PixelU-Wii's own, %d third-party), %d problems"
          % (len(have), from_own, from_third, len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
