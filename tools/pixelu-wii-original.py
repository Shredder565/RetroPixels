#!/usr/bin/env python3
"""Recount PixelU-Wii's source modules for docs/pixelu-wii-original.md.

Usage (from the repo root):  python3 tools/pixelu-wii-original.py

Rewrites the part of docs/pixelu-wii-original.md between the
"<!-- table:start -->" and "<!-- table:end -->" markers. Everything else in
the doc is left alone.

A file counts as:
  original       its first lines contain ORIGINAL_MARK
  third-party    it is in THIRD_PARTY (bundled code that is neither Dolphin's nor ours)
  PixelU new     added under pixelu/wii/Source after the vendoring commit, no mark
  PixelU change  a Dolphin file PixelU has edited: one of PATCHED (changed by the
                 old pixelu/wii/patches before vendoring) or edited since the
                 vendoring commit
  Dolphin        everything else
Lines are physical lines (like wc -l) of the EXTS files under pixelu/wii/Source.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL_SRC = "pixelu/wii/" + "Source"
SRC = os.path.join(ROOT, *REL_SRC.split("/"))
DOC = os.path.join(ROOT, "docs", "pixelu-wii-original.md")
# The commit that added pixelu/wii/Source (Dolphin 2609a, 409881c, with the
# PixelU patches). Found from git history so it survives merges.
ORIGINAL_MARK = "Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin."
EXTS = (".h", ".hpp", ".c", ".cpp", ".mm", ".m", ".inc", ".inl", ".S")
THIRD_PARTY = (
    # Khronos OpenGL API headers (MIT); GLExtensions.cpp/.h are Dolphin's loader
    "Core/Common/GL/GLExtensions/gl_",
    "Core/Common/GL/GLExtensions/AMD_",
    "Core/Common/GL/GLExtensions/ARB_",
    "Core/Common/GL/GLExtensions/EXT_",
    "Core/Common/GL/GLExtensions/HP_",
    "Core/Common/GL/GLExtensions/KHR_",
    "Core/Common/GL/GLExtensions/NV_",
    # OpenVPN TAP-Win32 driver interface header
    "Core/Core/HW/EXI/BBA/TAP_Win32.h",
)
# Dolphin files the old pixelu/wii/patches (0001, 0003, 0004) changed; they
# were vendored with those changes already in.
PATCHED = {
    "Core/Core/Core.cpp",
    "Core/DolphinQt/AboutDialog.cpp",
    "Core/DolphinQt/MainWindow.cpp",
    "Core/DolphinQt/RenderWidget.cpp",
    "Core/DolphinQt/RenderWidget.h",
}
# Directories split one level further in the table, deepest match wins.
SPLIT = (
    "Core/Common",
    "Core/Core",
    "Core/Core/DSP",
    "Core/Core/HW",
    "Core/Core/IOS",
    "Core/Core/IOS/USB",
    "Core/Core/PowerPC",
    "Core/DolphinQt",
    "Core/InputCommon",
    "Core/InputCommon/ControllerInterface",
    "Core/VideoBackends",
    "Core/VideoCommon",
)


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True).stdout


def vendor_commit():
    out = git("log", "--diff-filter=A", "--format=%H", "--", REL_SRC + "/CMakeLists.txt").split()
    return out[-1] if out else None


def git_paths(*args):
    prefix = REL_SRC + "/"
    return {p[len(prefix):] for p in git(*args).split() if p.startswith(prefix)}


def module_of(rel):
    d = rel.rsplit("/", 1)[0] if "/" in rel else ""
    best = None
    for s in SPLIT:
        if (d == s or d.startswith(s + "/")) and (best is None or len(s) > len(best)):
            best = s
    if best is None:
        p = d.split("/")
        if p[0] == "Core" and len(p) > 1:
            return "Core/" + p[1]
        return p[0] or "(top level)"
    if d == best:
        return best + " (top level)"
    return best + "/" + d[len(best) + 1:].split("/")[0]


def classify(rel, full, added, changed):
    if rel.startswith(THIRD_PARTY):
        return "third"
    with open(full, "r", encoding="utf-8", errors="replace") as f:
        head = "".join(f.readline() for _ in range(5))
    if ORIGINAL_MARK in head:
        return "original"
    if rel in added:
        return "new"
    if rel in changed or rel in PATCHED:
        return "changed"
    return "dolphin"


def status_of(c):
    total = sum(c.values())
    if total == 0:
        return "empty"
    if c["original"] == total:
        return "original"
    if c["third"] == total:
        return "third-party"
    if c["original"]:
        status = "mixed (%d%% original)" % round(100 * c["original"] / total)
    elif c["new"] or c["changed"]:
        status = "Dolphin + PixelU change"
    else:
        status = "Dolphin"
    if c["third"]:
        status += ", %d lines third-party" % c["third"]
    return status


KINDS = ("original", "dolphin", "changed", "new", "third")


def main():
    base = vendor_commit()
    if base:
        added = git_paths("log", "--diff-filter=A", "--name-only", "--format=", base + "..HEAD", "--", REL_SRC)
        changed = git_paths("diff", "--name-only", base, "--", REL_SRC)
    else:  # not committed yet
        added, changed = set(), set()
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
            c = modules.setdefault(module_of(rel), dict.fromkeys(KINDS, 0))
            c[kind] += lines

    totals = dict.fromkeys(KINDS, 0)
    rows = []
    for mod in sorted(modules, key=str.lower):
        c = modules[mod]
        for k in totals:
            totals[k] += c[k]
        rows.append("| %s | %d | %d | %s |" % (mod, sum(c.values()), c["original"], status_of(c)))

    grand = sum(totals.values())
    out = [
        "<!-- table:start -->",
        "Totals (lines in %s, %s files):" % (REL_SRC, "/".join(e.lstrip(".") for e in EXTS)),
        "",
        "| Kind | Lines | Share |",
        "|---|---:|---:|",
    ]
    for label, key in (("Original (RetroPixels)", "original"), ("Dolphin, untouched", "dolphin"),
                       ("Dolphin, with PixelU changes", "changed"), ("PixelU-added files (no original mark)", "new"),
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
