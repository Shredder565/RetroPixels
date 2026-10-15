# Makes testdata/archipelago/ for RetroPixels' Archipelago tests, using Archipelago's own patch
# classes (from a local clone of github.com/ArchipelagoMW/Archipelago) and only made-up data.
# Usage: python -I tools/ap-testdata.py testdata/archipelago   (AP_CLONE = the clone;
# needs the bsdiff4 package)
import sys, os, json, random, hashlib, zipfile, io
AP = os.environ.get("AP_CLONE", "/tmp/archipelago")
sys.path.insert(0, AP)
import bsdiff4
import importlib.util
# Load worlds/Files.py on its own (importing the worlds package would load every game).
_spec = importlib.util.spec_from_file_location("apfiles", AP + "/worlds/Files.py")
_files = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(_files)
APProcedurePatch, APTokenMixin, APTokenTypes, APDeltaPatch, APPatchExtension = (_files.APProcedurePatch, _files.APTokenMixin,
    _files.APTokenTypes, _files.APDeltaPatch, _files.APPatchExtension)

out = sys.argv[1]
rnd = random.Random(1234)

def blob(n):
    return bytes(rnd.getrandbits(8) for _ in range(n))

# 1. plain bsdiff4 vectors
old = blob(5000)
new = bytearray(old)
for _ in range(60):
    i = rnd.randrange(len(new)); new[i] = rnd.getrandbits(8)
new[1000:1000] = blob(300)          # insertion
del new[3000:3100]                  # deletion
new += blob(500)                    # extra tail
new = bytes(new)
open(os.path.join(out, "old.bin"), "wb").write(old)
open(os.path.join(out, "new.bin"), "wb").write(new)
open(os.path.join(out, "old_new.bsdiff4"), "wb").write(bsdiff4.diff(old, new))
open(os.path.join(out, "empty_new.bsdiff4"), "wb").write(bsdiff4.diff(b"", new[:777]))

# 2. a procedure patch: bsdiff4, tokens, calc_snes_crc (synthetic 64 KB "ROM")
base = blob(0x8000)
target = bytearray(base)
target[0x100:0x110] = b"RETROPIXELS TEST"
target = bytes(target)

class TestPatch(APProcedurePatch, APTokenMixin):
    game = "RetroPixels Test"
    patch_file_ending = ".aprptest"
    result_file_ending = ".sfc"
    hash = hashlib.md5(base).hexdigest()
    procedure = [("apply_bsdiff4", ["base.bsdiff4"]), ("apply_tokens", ["tokens.bin"]), ("calc_snes_crc", [])]
    @classmethod
    def get_source_data(cls):
        return base

p = TestPatch(path=os.path.join(out, "AP_test_P1_Tester.aprptest"), player=1, player_name="Tester", server="localhost:38281")
p.write_file("base.bsdiff4", bsdiff4.diff(base, target))
p.write_token(APTokenTypes.WRITE, 0x200, b"\x01\x02\x03\x04")
p.write_token(APTokenTypes.COPY, 0x300, (16, 0x100))
p.write_token(APTokenTypes.RLE, 0x400, (32, 0xAB))
p.write_token(APTokenTypes.AND_8, 0x500, 0x0F)
p.write_token(APTokenTypes.OR_8, 0x501, 0xF0)
p.write_token(APTokenTypes.XOR_8, 0x502, 0xFF)
p.write_token(APTokenTypes.WRITE, 0x8000, b"TAIL")   # past the end: appended
p.write_file("tokens.bin", p.get_token_binary())
p.write()
open(os.path.join(out, "procedure_base.bin"), "wb").write(base)
p2 = TestPatch(path=p.path); p2.read()
p2.patch(os.path.join(out, "procedure_result.bin"))

# 3. an old-style delta patch (no procedure in the manifest)
class OldDelta(APDeltaPatch):
    game = "RetroPixels Old Test"
    patch_file_ending = ".aprpold"
    result_file_ending = ".gb"
    hash = hashlib.md5(old).hexdigest()
    @classmethod
    def get_source_data(cls):
        return old
newfile = os.path.join(out, "_tmp_new.bin"); open(newfile, "wb").write(new)
d = OldDelta(path=os.path.join(out, "AP_old_P2_Other.aprpold"), player=2, player_name="Other", server="", patched_path=newfile)
d.write()
os.remove(newfile)
# strip "procedure" from the manifest, like patches made before procedures existed
zin = zipfile.ZipFile(d.path); files = {n: zin.read(n) for n in zin.namelist()}; zin.close()
m = json.loads(files["archipelago.json"]); m.pop("procedure", None); m.pop("base_checksum", None); files["archipelago.json"] = json.dumps(m).encode()
with zipfile.ZipFile(d.path, "w", zipfile.ZIP_DEFLATED) as z:
    for n, b in files.items(): z.writestr(n, b)

# 4. a patch with a game-specific step RetroPixels can't run
class Custom(APProcedurePatch):
    game = "RetroPixels Custom Test"
    patch_file_ending = ".aprpcustom"
    result_file_ending = ".gba"
    hash = None
    procedure = [("apply_bsdiff4", ["x.bsdiff4"]), ("shuffle_enemies", [])]
c = Custom(path=os.path.join(out, "AP_custom.aprpcustom"), player=1, player_name="X", server="")
c.write_file("x.bsdiff4", bsdiff4.diff(b"a", b"b"))
c.write()

# 5. Super Metroid's write_crc on an odd-sized ROM (3 MB + 64 KB isn't needed; 96 KB = 64 + 32)
sys.path.insert(0, AP)
odd = blob(0xC000)
open(os.path.join(out, "crc_odd.bin"), "wb").write(odd)
import ast
_src = open(AP + "/worlds/sm/Rom.py").read()
_fn = next(n for n in ast.walk(ast.parse(_src)) if isinstance(n, ast.FunctionDef) and n.name == "write_crc")
_fn.decorator_list = []
_ns = {"APProcedurePatch": object}
exec(compile(ast.Module(body=[_fn], type_ignores=[]), "sm_write_crc", "exec"), _ns)
open(os.path.join(out, "crc_odd_sm.bin"), "wb").write(_ns["write_crc"](None, odd))
print("ok", sorted(os.listdir(out)))

# Keep the expected results small: hashes and the four checksum bytes.
exp = {
    "procedure_result_md5": hashlib.md5(open(os.path.join(out, "procedure_result.bin"), "rb").read()).hexdigest(),
    "procedure_result_len": os.path.getsize(os.path.join(out, "procedure_result.bin")),
    "sm_crc_bytes": open(os.path.join(out, "crc_odd_sm.bin"), "rb").read()[0x7FDC:0x7FE0].hex(),
}
open(os.path.join(out, "expected.json"), "w").write(json.dumps(exp, indent=1))
os.remove(os.path.join(out, "procedure_result.bin")); os.remove(os.path.join(out, "crc_odd_sm.bin"))
