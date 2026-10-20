#!/usr/bin/env python3
"""Recount PixelU-Wii's source modules for docs/pixelu-wii-original.md.

Usage (from the repo root):
  python3 tools/pixelu-wii-original.py            rewrite the tables in the doc
  python3 tools/pixelu-wii-original.py --list W7  list the files of batch W7
                                                  (lines, kind, path)

Rewrites the parts of docs/pixelu-wii-original.md between the
"<!-- table:start -->" / "<!-- table:end -->" and "<!-- batches:start -->" /
"<!-- batches:end -->" markers. Everything else in the doc is left alone.

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
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL_SRC = "pixelu/wii/" + "Source"
SRC = os.path.join(ROOT, *REL_SRC.split("/"))
DOC = os.path.join(ROOT, "docs", "pixelu-wii-original.md")
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
# Directories split one level further in the module table, deepest match wins.
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

# The rewrite plan (docs/pixelu-wii-original.md, "Batch plan"). A file belongs
# to the first batch with a matching pattern. In a pattern "*" stays within one
# path component; a trailing "/*" takes the whole directory tree. W0 was the
# vendoring step itself.
C = "Core/Common/"
K = "Core/Core/"
V = "Core/VideoCommon/"
I = "Core/InputCommon/ControllerInterface/"
Q = "Core/DolphinQt/"
BATCHES = [
    ("W1", "Prune: code no Windows x64 build compiles (ARM64 JIT, Metal, Android, macOS, Linux backends)", [
        K + "PowerPC/JitArm64/*", K + "PowerPC/JitArmCommon/*", C + "Arm64Emitter*", C + "ArmCPUDetect*",
        C + "ArmFPURoundMode*", C + "ArmCommon*", C + "GenericCPUDetect*", C + "GenericFPURoundMode*",
        C + "MemArenaAndroid*", C + "MemArenaDarwin*", C + "MemArenaUnix*", C + "CommonFuncsObjC*", C + "UnixUtil*",
        C + "GL/GLX11Window*", C + "GL/GLInterface/AGL*", C + "GL/GLInterface/EGL*", C + "GL/GLInterface/GLX*",
        C + "GL/GLInterface/BGL*", V + "VertexLoaderARM64*", "Core/VideoBackends/Metal/*",
        I + "Android/*", I + "evdev/*", I + "Quartz/*", I + "Xlib/*", I + "Touch/*", I + "Pipes/*",
        K + "HW/WiimoteReal/IOAndroid*", K + "HW/WiimoteReal/IOLinux*", K + "HW/WiimoteReal/IOdarwin*",
        K + "HW/WiimoteReal/IOBluez*", K + "HW/EXI/BBA/TAPServer_Apple*", K + "HW/EXI/BBA/TAP_Apple*",
        K + "HW/EXI/BBA/TAP_Unix*", "Core/AudioCommon/AlsaSoundStream*", "Core/AudioCommon/PulseAudioStream*",
        "Core/AudioCommon/OpenSLESStream*", "Core/UICommon/X11Utils*"]),
    ("W2", "Common: types, bits, containers, strings, files, threads, config, logging", [
        C + "Align*", C + "Assert*", C + "BitField*", C + "BitSet*", C + "BitUtils*", C + "BlockingLoop*", C + "Buffer*",
        C + "ChunkFile*", C + "ColorUtil*", C + "Common.*", C + "CommonFuncs*", C + "CommonPaths*", C + "CommonTypes*",
        C + "Contains*", C + "DirectIOFile*", C + "EnumFormatter*", C + "EnumMap*", C + "Event*", C + "FileSearch*",
        C + "FileUtil*", C + "FixedSizeQueue*", C + "Flag*", C + "FloatUtils*", C + "FormatUtil*", C + "Functional*",
        C + "Hash*", C + "HookableEvent*", C + "Inline*", C + "IniFile*", C + "Intrinsics*", C + "IOFile*",
        C + "JsonUtil*", C + "Lazy*", C + "LinearDiskCache*", C + "MathUtil*", C + "Matrix*", C + "MsgHandler*",
        C + "Mutex*", C + "OneShotEvent*", C + "Projection*", C + "Random*", C + "RangeSet*", C + "RangeSizeSet*",
        C + "ScopeGuard*", C + "SettingsHandler*", C + "SmallVector*", C + "SpanUtils*", C + "SPSCQueue*",
        C + "StringUtil*", C + "Swap*", C + "Thread*", C + "Timer*", C + "TimeUtil*", C + "TransferableSharedMutex*",
        C + "TypeUtils*", C + "VariantUtil*", C + "Version*", C + "WaitableFlag*", C + "WorkQueueThread*",
        C + "Config/*", C + "Logging/*", C + "Profiler*", C + "FPURoundMode*", C + "CPUDetect*", C + "x64CPUDetect*",
        C + "x64FPURoundMode*", C + "WindowSystemInfo*", C + "Analytics*", C + "CompatPatches*"]),
    ("W3", "Common: platform, memory, network, crypto, file formats", [
        C + "MemArena*", C + "MemoryUtil*", C + "DynamicLibrary*", C + "Windows*", C + "LdrWatcher*", C + "HRWrap*",
        C + "Network*", C + "HttpRequest*", C + "UPnP*", C + "ENet*", C + "Traversal*", C + "SocketContext*",
        C + "QoSSession*", C + "SFMLHelper*", C + "PcapFile*", C + "Crypto/*", C + "FatFsUtil*", C + "SDCardUtil*",
        C + "NandPaths*", C + "Image*", C + "MinizipUtil*", C + "FilesystemWatcher*", C + "Debug/*", C + "SymbolDB*",
        C + "CWDemangler*"]),
    ("W4", "Common: x64 emitter, disassemblers, PowerPC assembler", [
        C + "x64*", C + "CodeBlock*", C + "JitRegister*", C + "HostDisassembler*", C + "GekkoDisassembler*",
        C + "Assembler/*"]),
    ("W5", "DiscIO: blob formats and compression", [
        "Core/DiscIO/*Blob*", "Core/DiscIO/WIACompression*", "Core/DiscIO/MultithreadedCompressor*",
        "Core/DiscIO/LaggedFibonacciGenerator*", "Core/DiscIO/WiiEncryptionCache*", "Core/DiscIO/DiscScrubber*"]),
    ("W6", "DiscIO: volumes, filesystems, verifier, Riivolution, NAND import", ["Core/DiscIO/*"]),
    ("W7", "PowerPC: CPU state, MMU, caches, analyst, tables, symbols, breakpoints, GDB stub",
        [K + "PowerPC/*.*", K + "PowerPC/SignatureDB/*"]),
    ("W8", "PowerPC: interpreter and cached interpreter",
        [K + "PowerPC/Interpreter/*", K + "PowerPC/CachedInterpreter/*"]),
    ("W9", "PowerPC: JIT common and Jit64", [K + "PowerPC/JitCommon/*", K + "PowerPC/Jit64Common/*", K + "PowerPC/Jit64/*"]),
    ("W10", "HW: memory map, MMIO, CPU/PI/MI/VI/AI/DSP interfaces, timers, IPC, CoreTiming", [
        K + "HW/AddressSpace*", K + "HW/AudioInterface*", K + "HW/CPU*", K + "HW/DSP.*", K + "HW/GPFifo*",
        K + "HW/HW.*", K + "HW/Memmap*", K + "HW/MemoryInterface*", K + "HW/MMIO*", K + "HW/ProcessorInterface*",
        K + "HW/Sram*", K + "HW/StreamADPCM*", K + "HW/SystemTimers*", K + "HW/VideoInterface*", K + "HW/WII_IPC*",
        K + "CoreTiming*", K + "HW/WiiSave*"]),
    ("W11", "HW: DVD and GameCube memory cards", [K + "HW/DVD/*", K + "HW/GCMemcard/*"]),
    ("W12", "HW: EXI devices (memory card slots, BBA, mic, modem, IPL)", [K + "HW/EXI/*"]),
    ("W13", "HW: SI, pads, keyboard, GBA link, HSP, Triforce, MagCard", [
        K + "HW/SI/*", K + "HW/GBA*", K + "HW/GCKeyboard*", K + "HW/GCPad*", K + "HW/HSP/*", K + "HW/Triforce/*",
        K + "HW/MagCard/*"]),
    ("W14", "HW: Wii Remote (common, emulated, real)",
        [K + "HW/WiimoteCommon/*", K + "HW/WiimoteEmu/*", K + "HW/WiimoteReal/*", K + "HW/Wiimote.*"]),
    ("W15", "DSP: core, interpreter, LLE glue", [K + "DSP/*.*", K + "DSP/Interpreter/*", K + "HW/DSPLLE/*", K + "DSPEmulator*"]),
    ("W16", "DSP: x64 JIT", [K + "DSP/Jit/*"]),
    ("W17", "DSP: HLE (AX, Zelda, GBA, CARD, ROM ucodes)", [K + "HW/DSPHLE/*"]),
    ("W18", "IOS: kernel, ES, FS, DI, SDIO, STM, WFS, crypto", [
        K + "IOS/*.*", K + "IOS/ES/*", K + "IOS/FS/*", K + "IOS/DI/*", K + "IOS/SDIO/*", K + "IOS/STM/*",
        K + "IOS/WFS/*", K + "IOS/Crypto/*"]),
    ("W19", "IOS: network (SO, KD/WC24, NCD, WD, SSL) and USB host devices", [
        K + "IOS/Network/*", K + "IOS/USB/*.*", K + "IOS/USB/OH0/*", K + "IOS/USB/USB_HID/*", K + "IOS/USB/USB_VEN/*"]),
    ("W20", "IOS: Bluetooth and emulated USB devices (Skylanders, Infinity, keyboard, mic)",
        [K + "IOS/USB/Bluetooth/*", K + "IOS/USB/Emulated/*"]),
    ("W21", "Core: boot, config, config loaders, HLE, debugger, FIFO player", [
        K + "Boot/*", K + "Config/*", K + "ConfigLoaders/*", K + "HLE/*", K + "Debugger/*", K + "FifoPlayer/*"]),
    ("W22", "Core: Core.cpp, System, state saves, movies, Wii root, title DB, hotkeys", [
        K + "Core.*", K + "System*", K + "State*", K + "Movie*", K + "MachineContext*", K + "BootManager*",
        K + "ConfigManager*", K + "SysConf*", K + "WiiRoot*", K + "WiiUtils*", K + "TitleDatabase*", K + "CommonTitles*",
        K + "HotkeyManager*", K + "FreeLookManager*", K + "MemTools*", K + "MemoryWatcher*", K + "Host*",
        K + "TimePlayed*", K + "CPUThreadConfigCallback*", K + "SyncIdentifier*"]),
    ("W23", "Core: cheats, patches, NetPlay, achievements, analytics, USB utils", [K + "*"]),
    ("W24", "VideoCommon: GX state and command processing (BP/CP/XF, FIFO, PE, CPU cull)", [
        V + "BP*", V + "CPMemory*", V + "XF*", V + "CommandProcessor*", V + "Fifo*", V + "OpcodeDecoding*",
        V + "PixelEngine*", V + "DataReader*", V + "VideoState*", V + "AsyncRequests*", V + "Statistics*",
        V + "BoundingBox*", V + "VideoEvents*", V + "CPUCull*", V + "LookUpTables*", V + "Constants*"]),
    ("W25", "VideoCommon: vertex loaders and vertex manager",
        [V + "VertexLoader*", V + "VertexManagerBase*", V + "IndexGenerator*", V + "NativeVertexFormat*"]),
    ("W26", "VideoCommon: texture cache and decoders", [V + "Texture*", V + "TMEM*", V + "HiresTextures*"]),
    ("W27", "VideoCommon: shader generators and shader managers", [
        V + "*ShaderGen*", V + "UberShader*", V + "ShaderGenCommon*", V + "Spirv*", V + "ShaderCompileUtils*",
        V + "ConstantManager*", V + "*ShaderManager*"]),
    ("W28", "VideoCommon: renderer abstraction, framebuffers, shader cache, present, post-processing", [
        V + "Abstract*", V + "FramebufferManager*", V + "ShaderCache*", V + "AsyncShaderCompiler*", V + "Present*",
        V + "PostProcessing*", V + "RenderState*", V + "PipelineUtils*", V + "EFBInterface*", V + "PerfQueryBase*",
        V + "VideoBackendBase*", V + "VideoConfig*", V + "DriverDetails*", V + "Widescreen*", V + "FreeLookCamera*",
        V + "GXPipelineTypes*", V + "VideoCommon.*"]),
    ("W29", "VideoCommon: OSD, frame dumping, performance, assets, graphics mods, resources", [V + "*"]),
    ("W30", "Video backends: Software and Null", ["Core/VideoBackends/Software/*", "Core/VideoBackends/Null/*"]),
    ("W31", "Video backends: OpenGL (with Common/GL)", ["Core/VideoBackends/OGL/*", C + "GL/*"]),
    ("W32", "Video backends: Vulkan", ["Core/VideoBackends/Vulkan/*"]),
    ("W33", "Video backends: D3D11, D3D12, D3DCommon",
        ["Core/VideoBackends/D3D/*", "Core/VideoBackends/D3D12/*", "Core/VideoBackends/D3DCommon/*"]),
    ("W34", "InputCommon: controller emulation, control references, dynamic input textures", [
        "Core/InputCommon/*.*", "Core/InputCommon/ControlReference/*", "Core/InputCommon/ControllerEmu/*",
        "Core/InputCommon/DynamicInputTextures/*"]),
    ("W35", "InputCommon: ControllerInterface (DInput, XInput, WGInput, SDL, DSU, Wii Remote, Steam Deck)", [I + "*"]),
    ("W36", "AudioCommon, UICommon, UpdaterCommon, DolphinTool, PCH",
        ["Core/AudioCommon/*", "Core/UICommon/*", "Core/UpdaterCommon/*", "Core/DolphinTool/*", "PCH/*"]),
    ("W37", "DolphinQt: main window, menus, host, settings, render widget, hotkeys, Qt utils", [
        Q + "MainWindow*", Q + "MenuBar*", Q + "Main.*", Q + "Host*", Q + "Settings.*", Q + "RenderWidget*",
        Q + "HotkeyScheduler*", Q + "Resources*", Q + "Translation*", Q + "ToolBar*", Q + "AboutDialog*",
        Q + "SearchBar*", Q + "resource*", Q + "pch_qt*", Q + "GameCount*", Q + "QtUtils/*"]),
    ("W38", "DolphinQt: game list and the other top-level dialogs (cheats, memcards, GBA, convert, NAND)",
        [Q + "GameList/*", Q + "*.*"]),
    ("W39", "DolphinQt: config dialogs (general, graphics, controls, tooltips, SDL hints)", [
        Q + "Config/*.*", Q + "Config/ConfigControls/*", Q + "Config/ControllerInterface/*", Q + "Config/Graphics/*",
        Q + "Config/SDLHints/*", Q + "Config/ToolTipControls/*"]),
    ("W40", "DolphinQt: controller mapping and settings panes", [Q + "Config/Mapping/*", Q + "Settings/*"]),
    ("W41", "DolphinQt: debugger", [Q + "Debugger/*"]),
    ("W42", "DolphinQt: NetPlay, TAS, FIFO, achievements, portals, emulated USB", [Q + "*"]),
]

_patterns = {}


def matches(rel, pattern):
    rx = _patterns.get(pattern)
    if rx is None:
        if pattern.endswith("/*"):
            body = re.escape(pattern[:-2]).replace(r"\*", "[^/]*") + "/.*"
        else:
            body = re.escape(pattern).replace(r"\*", "[^/]*")
        rx = _patterns[pattern] = re.compile(body + "$")
    return rx.match(rel) is not None


def batch_of(rel):
    for bid, _, patterns in BATCHES:
        if any(matches(rel, p) for p in patterns):
            return bid
    return None


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True).stdout


def vendor_commit():
    # The commit that added pixelu/wii/Source, found from history so it survives merges.
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


def scan():
    base = vendor_commit()
    if base:
        added = git_paths("log", "--diff-filter=A", "--name-only", "--format=", base + "..HEAD", "--", REL_SRC)
        changed = git_paths("diff", "--name-only", base, "--", REL_SRC)
    else:  # not committed yet
        added, changed = set(), set()
    files = []
    for dirpath, _, names in os.walk(SRC):
        for name in names:
            if not name.endswith(EXTS):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, SRC).replace(os.sep, "/")
            with open(full, "rb") as f:
                lines = f.read().count(b"\n")
            files.append((rel, lines, classify(rel, full, added, changed)))
    return files


def replace_between(doc, start_mark, end_mark, lines):
    start, end = doc.index(start_mark), doc.index(end_mark) + len(end_mark)
    return doc[:start] + "\n".join([start_mark] + lines + [end_mark]) + doc[end:]


def main():
    files = scan()
    if len(sys.argv) == 3 and sys.argv[1] == "--list":
        for rel, lines, kind in sorted(files):
            if batch_of(rel) == sys.argv[2] and kind != "third":
                print("%6d %-8s %s" % (lines, kind, rel))
        return 0

    modules = {}
    batches = {bid: dict.fromkeys(KINDS, 0) for bid, _, _ in BATCHES}
    unplanned = dict.fromkeys(KINDS, 0)
    for rel, lines, kind in files:
        modules.setdefault(module_of(rel), dict.fromkeys(KINDS, 0))[kind] += lines
        if kind != "third":
            bid = batch_of(rel)
            (batches[bid] if bid else unplanned)[kind] += lines

    totals = dict.fromkeys(KINDS, 0)
    rows = []
    for mod in sorted(modules, key=str.lower):
        c = modules[mod]
        for k in totals:
            totals[k] += c[k]
        rows.append("| %s | %d | %d | %s |" % (mod, sum(c.values()), c["original"], status_of(c)))

    grand = sum(totals.values())
    table = [
        "Totals (lines in %s, %s files):" % (REL_SRC, "/".join(e.lstrip(".") for e in EXTS)),
        "",
        "| Kind | Lines | Share |",
        "|---|---:|---:|",
    ]
    for label, key in (("Original (RetroPixels)", "original"), ("Dolphin, untouched", "dolphin"),
                       ("Dolphin, with PixelU changes", "changed"), ("PixelU-added files (no original mark)", "new"),
                       ("Third-party", "third")):
        table.append("| %s | %d | %.1f%% |" % (label, totals[key], 100 * totals[key] / grand))
    table.append("| **All** | **%d** | 100%% |" % grand)
    table += ["", "| Module | Lines | Original lines | Status |", "|---|---:|---:|---|"]
    table += rows

    plan = ["| Batch | Scope | Lines | Original lines | Status |", "|---|---|---:|---:|---|"]
    for bid, title, _ in BATCHES:
        c = batches[bid]
        n = sum(c.values())
        plan.append("| %s | %s | %d | %d | %s |" % (bid, title, n, c["original"], status_of(c) if n else "done (no files left)"))
    if sum(unplanned.values()):
        plan.append("| - | not in any batch | %d | %d | %s |" % (sum(unplanned.values()), unplanned["original"], status_of(unplanned)))

    with open(DOC, "r", encoding="utf-8") as f:
        doc = f.read()
    doc = replace_between(doc, "<!-- table:start -->", "<!-- table:end -->", table)
    doc = replace_between(doc, "<!-- batches:start -->", "<!-- batches:end -->", plan)
    with open(DOC, "w", encoding="utf-8", newline="\n") as f:
        f.write(doc)
    print("original %d / %d lines (%.2f%%)" % (totals["original"], grand, 100 * totals["original"] / grand))
    return 0


if __name__ == "__main__":
    sys.exit(main())
