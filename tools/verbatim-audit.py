#!/usr/bin/env python3
"""Find long runs of lines that rewritten files still share verbatim with the
upstream source they replaced.

Usage: python3 tools/verbatim-audit.py {wii|wiiu} [min_run]

A "run" is a block of consecutive lines identical (after whitespace
normalisation) between the upstream file and the rewritten one. Blank lines,
lone braces and trivial lines do not count towards a run's length, so short
getters and forward declarations don't flag. Runs of min_run (default 12)
counted lines or more are printed, worst first.
"""
import difflib, re, subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = {
    "wii": ("pixelu/wii/Source", "6da5148", "Original code for RetroPixels (PixelU-Wii)"),
    "wiiu": ("pixelu/src", "a798c44", "Original code for RetroPixels (PixelU)"),
}

def norm(l):
    return re.sub(r"\s+", " ", l).strip()

def trivial(l):
    return len(l) < 8 or l in ("{", "}", "};", "public:", "private:", "protected:", "break;", "return;", "default:") \
        or l.startswith(("#include", "#pragma", "//"))

def main():
    which = sys.argv[1]
    min_run = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    base_dir, base, mark = CFG[which]
    files = subprocess.run(["git", "-C", ROOT, "ls-files", base_dir], capture_output=True, text=True).stdout.split()
    hits = []
    for f in files:
        if not f.endswith((".cpp", ".h", ".hpp", ".c", ".inl")):
            continue
        try:
            new = open(os.path.join(ROOT, f), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if mark not in new[:400]:
            continue
        old = subprocess.run(["git", "-C", ROOT, "show", base + ":" + f], capture_output=True, text=True, errors="replace").stdout
        if not old:
            continue
        o = [norm(l) for l in old.split("\n")]
        n = [norm(l) for l in new.split("\n")]
        sm = difflib.SequenceMatcher(None, o, n, autojunk=False)
        for b in sm.get_matching_blocks():
            cnt = sum(1 for l in n[b.b:b.b + b.size] if not trivial(l))
            if cnt >= min_run:
                hits.append((cnt, f, b.b + 1, b.b + b.size))
    hits.sort(reverse=True)
    for cnt, f, s, e in hits:
        print("%4d  %s:%d-%d" % (cnt, f, s, e))
    print("%d runs of %d+ lines" % (len(hits), min_run), file=sys.stderr)

if __name__ == "__main__":
    main()
