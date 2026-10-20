#!/usr/bin/env python3
"""Recount PixelU's source modules for docs/pixelu-original.md.

Usage (from the repo root):  python3 tools/pixelu-original.py

Rewrites the part of docs/pixelu-original.md between the
"<!-- table:start -->" and "<!-- table:end -->" markers. Everything else in
the doc is left alone.

A file counts as:
  original     its first lines contain ORIGINAL_MARK
  third-party  it is in THIRD_PARTY (bundled code that is neither Cemu's nor ours)
  PixelU new   added in PixelU after the fork commit (still MPL-2.0 headed)
  PixelU change  a Cemu file PixelU has edited since the fork commit
  Cemu         everything else
Lines are physical lines (like wc -l) of .h/.hpp/.c/.cpp/.mm/.S files under pixelu/src.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "pixelu", "src")
DOC = os.path.join(ROOT, "docs", "pixelu-original.md")
FORK_COMMIT = "a798c44"  # "Add PixelU: Wii U emulator forked from Cemu (MPL-2.0)"
ORIGINAL_MARK = "Original code for RetroPixels (PixelU). Not derived from Cemu."
EXTS = (".h", ".hpp", ".c", ".cpp", ".mm", ".S")
THIRD_PARTY = (
    "util/tinyxml2/",
    "util/containers/robin_hood.h",
    "util/containers/flat_hash_map.hpp",
    "util/crypto/md5.",
    "audio/xaudio2_7/",
    "Common/unix/date.h",
    "Common/unix/fast_float.h",
    "Common/GLInclude/egl.h",
    "Common/GLInclude/glext.h",
    "Common/GLInclude/glxext.h",
    "Common/GLInclude/khrplatform.h",
    "Common/GLInclude/wglext.h",
    "Cafe/OS/libs/nsyshid/g721/",
    # font data, not code: Lato Medium (SIL OFL 1.1), the 'More Perfect DOS VGA'
    # bitmap font, and the generated Font Awesome 5 icon table (zlib licence)
    "resource/CafeDefaultFont.cpp",
    "Cafe/OS/libs/coreinit/coreinit_OSScreen_font.h",
    "resource/IconsFontAwesome5.h",
)


def git_paths(*args):
    out = subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True).stdout
    prefix = "pixelu/src/"
    return {p[len(prefix):] for p in out.split() if p.startswith(prefix)}


def module_of(rel):
    p = rel.split("/")
    n = len(p)
    if n == 1:
        return "(top level: main.cpp, mainLLE.cpp)"
    if p[0] == "Cafe":
        if n == 2:
            return "Cafe (CafeSystem, GamePatch)"
        if p[1] == "OS" and p[2] == "libs":
            return "Cafe/OS/libs/" + p[3] if n > 4 else "Cafe/OS/libs (shared headers)"
        if p[1] == "OS":
            return "Cafe/OS/" + p[2]
        if p[1] == "HW" and p[2] == "Latte" and n > 4:
            if p[3] == "Renderer" and n > 5:
                return "Cafe/HW/Latte/Renderer/" + p[4]
            return "Cafe/HW/Latte/" + p[3]
        if p[1] == "HW" and p[2] == "Espresso":
            return "Cafe/HW/Espresso/" + p[3] if n > 4 else "Cafe/HW/Espresso (core)"
        if p[1] in ("HW", "IOSU"):
            return "Cafe/%s/%s" % (p[1], p[2]) if n > 3 else "Cafe/%s (shared headers)" % p[1]
        return "Cafe/" + p[1]
    if p[0] in ("Cemu", "util", "input") and n > 2:
        return p[0] + "/" + p[1]
    if p[0] == "gui" and n > 3 and p[1] == "wxgui":
        return "gui/wxgui/" + p[2]
    if p[0] == "gui" and n > 2:
        return "gui/" + p[1] + (" (top level)" if p[1] == "wxgui" else "")
    if p[0] in ("input", "util"):
        return p[0] + " (top level)"
    return p[0]


def classify(rel, full, added, changed):
    if rel.startswith(THIRD_PARTY):
        return "third"
    with open(full, "r", encoding="utf-8", errors="replace") as f:
        head = "".join(f.readline() for _ in range(5))
    if ORIGINAL_MARK in head:
        return "original"
    if rel in added:
        return "new"
    if rel in changed:
        return "changed"
    return "cemu"


def status_of(c):
    total = sum(c.values())
    if c["original"] == total:
        return "original"
    if c["third"] == total:
        return "third-party"
    if c["original"]:
        status = "mixed (%d%% original)" % round(100 * c["original"] / total)
    elif c["new"] or c["changed"]:
        status = "Cemu + PixelU change"
    else:
        status = "Cemu"
    if c["third"]:
        status += ", %d lines third-party" % c["third"]
    return status


def main():
    added = git_paths("log", "--diff-filter=A", "--name-only", "--format=", FORK_COMMIT + "..HEAD", "--", "pixelu/src")
    changed = git_paths("diff", "--name-only", FORK_COMMIT, "--", "pixelu/src")
    modules = {}
    for dirpath, _, files in os.walk(SRC):
        for name in files:
            if not name.endswith(EXTS):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, SRC).replace(os.sep, "/")
            with open(full, "rb") as f:
                lines = f.read().count(b"\n")
            kind = classify(rel, full, added, changed)
            c = modules.setdefault(module_of(rel), dict.fromkeys(("original", "cemu", "changed", "new", "third"), 0))
            c[kind] += lines

    totals = dict.fromkeys(("original", "cemu", "changed", "new", "third"), 0)
    rows = []
    for mod in sorted(modules, key=str.lower):
        c = modules[mod]
        for k in totals:
            totals[k] += c[k]
        rows.append("| %s | %d | %d | %s |" % (mod, sum(c.values()), c["original"], status_of(c)))

    grand = sum(totals.values())
    out = [
        "<!-- table:start -->",
        "Totals (lines in pixelu/src, %s files):" % "/".join(e.lstrip(".") for e in EXTS),
        "",
        "| Kind | Lines | Share |",
        "|---|---:|---:|",
    ]
    for label, key in (("Original (RetroPixels)", "original"), ("Cemu, untouched", "cemu"),
                       ("Cemu, with PixelU changes", "changed"), ("PixelU-added files (MPL-2.0 header)", "new"),
                       ("Third-party", "third")):
        out.append("| %s | %d | %.1f%% |" % (label, totals[key], 100 * totals[key] / grand))
    out.append("| **All** | **%d** | 100%% |" % grand)
    out += ["", "| Module | Lines | Original lines | Status |", "|---|---:|---:|---|"]
    out += rows
    out.append("<!-- table:end -->")

    with open(DOC, "r", encoding="utf-8") as f:
        doc = f.read()
    start, end = doc.index("<!-- table:start -->"), doc.index("<!-- table:end -->") + len("<!-- table:end -->")
    with open(DOC, "w", encoding="utf-8", newline="\n") as f:
        f.write(doc[:start] + "\n".join(out) + doc[end:])
    print("original %d / %d lines (%.2f%%)" % (totals["original"], grand, 100 * totals["original"] / grand))


if __name__ == "__main__":
    sys.exit(main())
