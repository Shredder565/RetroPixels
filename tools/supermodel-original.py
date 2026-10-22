#!/usr/bin/env python3
"""Recount RetroPixels' Supermodel source for docs/supermodel-original.md.

Usage (from the repo root):
  python3 tools/supermodel-original.py              rewrite the table in the doc
  python3 tools/supermodel-original.py --list DIR   list DIR's files (e.g. Model3) with their kind and lines

Rewrites the part of docs/supermodel-original.md between the
"<!-- table:start -->" and "<!-- table:end -->" markers.

A file counts as:
  original     its first lines contain ORIGINAL_MARK
  third-party  under THIRD_PARTY (bundled code that is neither Supermodel's nor ours)
  Supermodel   everything else (including files with RetroPixels' changes)
Lines are physical lines of .h/.hpp/.c/.cpp/.inc/.glsl files under supermodel/Src.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "supermodel", "Src")
DOC = os.path.join(ROOT, "docs", "supermodel-original.md")
ORIGINAL_MARK = "Original code for RetroPixels (Supermodel). Not derived from Supermodel."
EXTS = (".h", ".hpp", ".c", ".cpp", ".inc", ".glsl")
THIRD_PARTY = (
    "Pkgs/",  # GLEW and the Khronos GL headers, Dear ImGui, minizip, minimp3, TinyXML-2
    "CPU/68K/Musashi/",  # Karl Stenerud's Musashi 68000 core (MIT)
)


def files():
    for d, _, names in os.walk(SRC):
        for n in sorted(names):
            if n.endswith(EXTS):
                full = os.path.join(d, n)
                yield os.path.relpath(full, SRC).replace(os.sep, "/"), full


def module_of(rel):
    p = rel.split("/")
    if len(p) == 1:
        return "(top level)"
    if p[0] in ("Pkgs",):
        return p[0]
    if len(p) > 2 and p[0] in ("CPU", "Graphics", "OSD", "Model3", "Debugger", "Sound"):
        return p[0] + "/" + p[1]
    return p[0] + (" (top level)" if p[0] in ("CPU", "Graphics", "OSD", "Model3", "Debugger", "Sound") else "")


def classify(rel, full):
    if rel.startswith(THIRD_PARTY):
        return "third"
    with open(full, "r", encoding="utf-8", errors="replace") as f:
        head = "".join(f.readline() for _ in range(5))
    return "original" if ORIGINAL_MARK in head else "supermodel"


def lines(full):
    with open(full, "rb") as f:
        return f.read().count(b"\n")


def status_of(c):
    total = sum(c.values())
    if c["original"] == total:
        return "original"
    if c["third"] == total:
        return "third-party"
    s = "mixed (%d%% original)" % round(100 * c["original"] / total) if c["original"] else "Supermodel"
    if c["third"]:
        s += ", %d lines third-party" % c["third"]
    return s


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list":
        want = sys.argv[2].rstrip("/") + "/"
        for rel, full in files():
            if rel.startswith(want) or module_of(rel) == sys.argv[2]:
                print("%-11s %6d  %s" % (classify(rel, full), lines(full), rel))
        return
    totals = {"original": 0, "supermodel": 0, "third": 0}
    mods = {}
    for rel, full in files():
        k = classify(rel, full)
        n = lines(full)
        totals[k] += n
        mods.setdefault(module_of(rel), {"original": 0, "supermodel": 0, "third": 0})[k] += n
    all_ = sum(totals.values())
    pct = lambda n: "%.1f%%" % (100 * n / all_)
    out = [
        "Totals (lines in supermodel/Src, %s files):" % "/".join(e[1:] for e in EXTS),
        "",
        "| Kind | Lines | Share |",
        "|---|---:|---:|",
        "| Original (RetroPixels) | %d | %s |" % (totals["original"], pct(totals["original"])),
        "| Supermodel (with or without RetroPixels changes) | %d | %s |" % (totals["supermodel"], pct(totals["supermodel"])),
        "| Third-party | %d | %s |" % (totals["third"], pct(totals["third"])),
        "| **All** | **%d** | 100%% |" % all_,
        "",
        "| Module | Lines | Original lines | Status |",
        "|---|---:|---:|---|",
    ]
    for m in sorted(mods, key=str.lower):
        c = mods[m]
        out.append("| %s | %d | %d | %s |" % (m, sum(c.values()), c["original"], status_of(c)))
    doc = open(DOC, encoding="utf-8").read()
    a, b = doc.index("<!-- table:start -->"), doc.index("<!-- table:end -->")
    doc = doc[: a + len("<!-- table:start -->")] + "\n" + "\n".join(out) + "\n" + doc[b:]
    open(DOC, "w", encoding="utf-8").write(doc)
    own = totals["original"] + totals["supermodel"]
    print("original %d / %d lines of Supermodel's own code (%.2f%%), %d lines third-party"
          % (totals["original"], own, 100 * totals["original"] / own if own else 0, totals["third"]))


if __name__ == "__main__":
    main()
