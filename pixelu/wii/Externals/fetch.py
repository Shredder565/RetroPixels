#!/usr/bin/env python3
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
"""Fetch PixelU-Wii's third-party sources from their own upstream projects.

sources.json (next to this script) lists every third-party library the build
uses: where it comes from (a git repository at a fixed commit, or a release
archive with a fixed SHA-256), where it goes under the third-party folder and
which of PixelU-Wii's patches (Externals/patches) are applied to it. Nothing
is taken from Dolphin's repositories.

    python fetch.py [--dest DIR] [--only NAME,...] [--jobs N] [--keep-git]
    python fetch.py --group ffmpeg    # the entries of one group (see below)
    python fetch.py --list            # the table, nothing fetched
    python fetch.py --check           # verify an existing third-party folder

Entries with a "group" are only fetched when that group is asked for: FFmpeg's
source is only needed to build FFmpeg (Externals/FFmpeg/build-ffmpeg.sh),
not by the CMake build, which uses the built libraries.

The default DIR is pixelu/wii/third_party, which is what the CMake build reads
(PIXELU_WII_THIRD_PARTY). A library whose folder already holds the same
version (its .pixelu-source stamp matches the manifest) is not fetched again.
Git commits are checked by git itself (a commit id names its whole tree);
archives are checked against their SHA-256 before anything is unpacked.
"""

import argparse
import concurrent.futures
import hashlib
import io
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "sources.json")
PATCHES = os.path.join(HERE, "patches")
DEFAULT_DEST = os.path.normpath(os.path.join(HERE, "..", "third_party"))
STAMP = ".pixelu-source"


class FetchError(Exception):
    pass


def load_manifest():
    with open(MANIFEST, encoding="utf-8") as f:
        data = json.load(f)
    entries = data["sources"]
    seen = set()
    for e in entries:
        for key in ("name", "dest", "license"):
            if key not in e:
                raise FetchError(f"sources.json: entry {e.get('name', '?')} has no '{key}'")
        if e["name"] in seen:
            raise FetchError(f"sources.json: {e['name']} is listed twice")
        seen.add(e["name"])
        kinds = [k for k in ("git", "archive") if k in e]
        if len(kinds) != 1:
            raise FetchError(f"sources.json: {e['name']} needs exactly one of 'git' or 'archive'")
        if "git" in e and len(e.get("commit", "")) != 40:
            raise FetchError(f"sources.json: {e['name']} needs a full 40-digit 'commit'")
        if "archive" in e and len(e.get("sha256", "")) != 64:
            raise FetchError(f"sources.json: {e['name']} needs a 'sha256'")
        for p in e.get("patches", []):
            if not os.path.isfile(os.path.join(PATCHES, p)):
                raise FetchError(f"sources.json: {e['name']}: patch {p} is missing")
    return entries


def stamp_of(e):
    """What is recorded next to a fetched library: everything that decides its content."""
    keys = ("name", "git", "commit", "paths", "submodules", "archive", "sha256", "strip", "patches", "write")
    s = {k: e[k] for k in keys if k in e}
    s["patch_sha256"] = {p: sha256_file(os.path.join(PATCHES, p)) for p in e.get("patches", [])}
    return s


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def rmtree(path):
    def make_writable(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if os.path.lexists(path):
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=make_writable)
        else:
            shutil.rmtree(path, onerror=make_writable)


def run_git(args, cwd, log):
    cmd = ["git", "-c", "advice.detachedHead=false", "-c", "core.autocrlf=false"] + args
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if p.returncode != 0:
        raise FetchError(f"git {' '.join(args)} failed:\n{p.stdout}{p.stderr}")
    return p.stdout.strip()


def fetch_git(e, out, log):
    os.makedirs(out)
    run_git(["init", "-q"], out, log)
    run_git(["remote", "add", "origin", e["git"]], out, log)
    fetch = ["fetch", "-q", "--depth", "1"]
    if e.get("paths"):
        # Only the listed folders: a partial clone with a sparse checkout.
        run_git(["config", "remote.origin.promisor", "true"], out, log)
        run_git(["config", "remote.origin.partialclonefilter", "blob:none"], out, log)
        run_git(["sparse-checkout", "set", "--no-cone"] + ["/" + p.strip("/") for p in e["paths"]], out, log)
        fetch.append("--filter=blob:none")
    try:
        run_git(fetch + ["origin", e["commit"]], out, log)
    except FetchError:
        # A server that does not hand out commits by id: fetch the named ref instead.
        if not e.get("ref"):
            raise
        log(f"{e['name']}: fetching {e['ref']} (the server refused the commit id)")
        run_git(["fetch", "-q", "origin", e["ref"]], out, log)
    run_git(["checkout", "-q", e["commit"]], out, log)
    head = run_git(["rev-parse", "HEAD"], out, log)
    if head != e["commit"]:
        raise FetchError(f"{e['name']}: got {head}, wanted {e['commit']}")
    if e.get("submodules"):
        run_git(["submodule", "update", "--init", "--recursive", "--depth", "1"], out, log)


def download(urls, sha256, log, name):
    errors = []
    for url in urls:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "PixelU-Wii fetch.py"})
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
        except Exception as ex:  # try the next mirror
            errors.append(f"{url}: {ex}")
            continue
        got = hashlib.sha256(data).hexdigest()
        if got != sha256:
            errors.append(f"{url}: SHA-256 is {got}, sources.json says {sha256}")
            continue
        log(f"{name}: {url} ({len(data)} bytes, SHA-256 OK)")
        return data
    raise FetchError(f"{name}: no usable download:\n  " + "\n  ".join(errors))


def safe_member(name, strip):
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if any(p == ".." for p in parts) or name.startswith(("/", "\\")) or ":" in name:
        raise FetchError(f"unsafe path in archive: {name}")
    if len(parts) <= strip:
        return None
    return "/".join(parts[strip:])


def fetch_archive(e, out, log):
    data = download(e["archive"], e["sha256"], log, e["name"])
    strip = e.get("strip", 0)
    os.makedirs(out)
    first = e["archive"][0].lower()
    if first.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for info in z.infolist():
                rel = safe_member(info.filename, strip)
                if rel is None or info.is_dir():
                    continue
                target = os.path.join(out, rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(z.read(info))
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as t:
            for m in t.getmembers():
                rel = safe_member(m.name, strip)
                if rel is None or not m.isfile():
                    continue
                target = os.path.join(out, rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(t.extractfile(m).read())


# --- unified diffs -----------------------------------------------------------
# Patches are applied here rather than with patch(1) or git apply, so the same
# thing happens on every machine and files with CRLF line ends (FatFs ships
# them) are patched without touching their line ends.

def parse_patch(text):
    files = []
    cur = None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("--- "):
            nxt = lines[i + 1] if i + 1 < len(lines) else ""
            if not nxt.startswith("+++ "):
                raise FetchError("patch: '---' without '+++'")
            path = nxt[4:].split("\t")[0].strip()
            if path.startswith("b/"):
                path = path[2:]
            cur = {"path": path, "hunks": []}
            files.append(cur)
            i += 2
            continue
        if line.startswith("@@"):
            if cur is None:
                raise FetchError("patch: hunk before any file header")
            head = line.split("@@")[1].split()
            old_start = int(head[0][1:].split(",")[0])
            hunk = {"old_start": old_start, "lines": []}
            cur["hunks"].append(hunk)
            i += 1
            while i < len(lines) and lines[i][:1] in (" ", "+", "-", "\\", ""):
                l = lines[i]
                if l.startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith("+++ "):
                    break
                if l == "":
                    l = " "  # an empty context line whose space was trimmed
                if not l.startswith("\\"):
                    hunk["lines"].append(l)
                i += 1
            continue
        i += 1
    return files


def apply_patch(patch_path, root):
    with open(patch_path, encoding="utf-8") as f:
        files = parse_patch(f.read())
    if not files:
        raise FetchError(f"{patch_path}: no changes in it")
    for pf in files:
        target = os.path.join(root, pf["path"])
        with open(target, "rb") as f:
            raw = f.read()
        eol = b"\r\n" if b"\r\n" in raw else b"\n"
        text = raw.decode("utf-8", errors="surrogateescape")
        had_final_eol = text.endswith(("\n",))
        old = text.replace("\r\n", "\n").split("\n")
        if had_final_eol:
            old = old[:-1]
        new = []
        pos = 0
        for h in pf["hunks"]:
            want = [l[1:] for l in h["lines"] if l[0] in " -"]
            repl = [l[1:] for l in h["lines"] if l[0] in " +"]
            start = h["old_start"] - 1 if want else h["old_start"]
            # Exact place first, then anywhere after the previous hunk.
            candidates = [start] + [k for k in range(pos, len(old) - len(want) + 1) if k != start]
            at = next((k for k in candidates if k >= pos and old[k:k + len(want)] == want), None)
            if at is None:
                raise FetchError(f"{os.path.basename(patch_path)}: hunk at line {h['old_start']} "
                                 f"of {pf['path']} does not match")
            new.extend(old[pos:at])
            new.extend(repl)
            pos = at + len(want)
        new.extend(old[pos:])
        out = "\n".join(new) + ("\n" if had_final_eol else "")
        data = out.encode("utf-8", errors="surrogateescape")
        if eol == b"\r\n":
            data = data.replace(b"\n", b"\r\n")
        with open(target, "wb") as f:
            f.write(data)


# --- main --------------------------------------------------------------------

def fetch_one(e, dest, keep_git, log):
    out = os.path.join(dest, e["dest"])
    stamp_path = os.path.join(out, STAMP)
    want = stamp_of(e)
    if os.path.isfile(stamp_path):
        with open(stamp_path, encoding="utf-8") as f:
            if json.load(f) == want:
                log(f"{e['name']}: up to date in {e['dest']}")
                return
    rmtree(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix=".fetch-", dir=os.path.dirname(out))
    work = os.path.join(tmp, "src")
    try:
        if "git" in e:
            fetch_git(e, work, log)
            if not keep_git:
                rmtree(os.path.join(work, ".git"))
        else:
            fetch_archive(e, work, log)
        for p in e.get("patches", []):
            apply_patch(os.path.join(PATCHES, p), work)
            log(f"{e['name']}: applied patches/{p}")
        # Small files the fetched tree needs, given in sources.json (e.g. the
        # version FFmpeg would otherwise read from its .git folder).
        for rel, text in e.get("write", {}).items():
            with open(os.path.join(work, rel), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        with open(os.path.join(work, STAMP), "w", encoding="utf-8") as f:
            json.dump(want, f, indent=1, sort_keys=True)
        os.replace(work, out)
    finally:
        rmtree(tmp)
    what = e["commit"][:12] if "git" in e else e["sha256"][:12]
    log(f"{e['name']}: {e.get('version', '')} {what} -> {e['dest']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=os.environ.get("PIXELU_WII_THIRD_PARTY", DEFAULT_DEST))
    ap.add_argument("--only", help="comma-separated names from sources.json")
    ap.add_argument("--group", help="fetch the entries of this group instead of the ungrouped ones")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--keep-git", action="store_true", help="keep each repository's .git folder")
    ap.add_argument("--list", action="store_true", help="print the table and stop")
    ap.add_argument("--check", action="store_true", help="only check that DEST matches sources.json")
    args = ap.parse_args()

    entries = load_manifest()
    if args.only:
        names = set(args.only.split(","))
        unknown = names - {e["name"] for e in entries}
        if unknown:
            raise FetchError(f"not in sources.json: {', '.join(sorted(unknown))}")
        entries = [e for e in entries if e["name"] in names]
    elif not args.list:
        entries = [e for e in entries if e.get("group") == args.group]

    if args.list:
        for e in entries:
            where = e.get("git") or e["archive"][0]
            pin = e.get("commit") or "sha256:" + e["sha256"]
            print(f"{e['name']:<22} {e.get('version', ''):<16} {e['license']:<28} {where} @ {pin}")
        return 0

    dest = os.path.abspath(args.dest)
    if args.check:
        bad = []
        for e in entries:
            p = os.path.join(dest, e["dest"], STAMP)
            ok = os.path.isfile(p) and json.load(open(p, encoding="utf-8")) == stamp_of(e)
            if not ok:
                bad.append(e["name"])
        if bad:
            print("not fetched or out of date: " + ", ".join(bad))
            return 1
        print(f"all {len(entries)} third-party sources match sources.json")
        return 0

    os.makedirs(dest, exist_ok=True)
    def log(msg):
        print(msg, flush=True)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        jobs = {pool.submit(fetch_one, e, dest, args.keep_git, log): e for e in entries}
        for job in concurrent.futures.as_completed(jobs):
            try:
                job.result()
            except Exception as ex:
                failed.append(jobs[job]["name"])
                print(f"ERROR {jobs[job]['name']}: {ex}", file=sys.stderr, flush=True)
    if failed:
        print(f"{len(failed)} failed: {', '.join(sorted(failed))}", file=sys.stderr)
        return 1
    print(f"{len(entries)} third-party sources in {dest}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FetchError as ex:
        print(f"ERROR: {ex}", file=sys.stderr)
        sys.exit(1)
