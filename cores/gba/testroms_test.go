package gba

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
)

// The test ROMs: original programs in testdata/roms (C and a little
// assembly), built here with clang and the LLVM tools when they are installed.
// Between them they cover every save type, every GPIO cart kind, the clock,
// the BIOS calls, the video modes, sound, timers, DMA and a game-like demo.

type romSpec struct {
	name, src string
	defs      []string
	code      string // game code (header 0xAC), picks the GPIO hardware
}

var romSpecs = []romSpec{
	{"save_sram", "save.c", []string{`-DTAG="SRAM_V113"`}, ""},
	{"save_sramf", "save.c", []string{`-DTAG="SRAM_F_V100"`}, ""},
	{"save_flash64", "save.c", []string{"-DFLASH=64", `-DTAG="FLASH_V126"`}, ""},
	{"save_flash512", "save.c", []string{"-DFLASH=64", `-DTAG="FLASH512_V131"`}, ""},
	{"save_flash128", "save.c", []string{"-DFLASH=128", `-DTAG="FLASH1M_V103"`}, ""},
	{"save_ee512", "save.c", []string{"-DEEPROM=512", `-DTAG="EEPROM_V124"`}, ""},
	{"save_ee8k", "save.c", []string{"-DEEPROM=8192", `-DTAG="EEPROM_V124"`}, ""},
	{"save_none", "save.c", []string{`-DTAG="NOTHING"`}, ""},
	{"gpio_plain", "gpio.c", nil, ""},
	{"gpio_rtc", "gpio.c", []string{`-DTAG="SIIRTC_V001"`}, ""},
	{"gpio_rtcflash", "gpio.c", []string{`-DTAG="FLASH1M_V103 SIIRTC_V001"`}, ""},
	{"gpio_solar", "gpio.c", nil, "U3IE"},
	{"gpio_solarflash", "gpio.c", []string{`-DTAG="FLASH1M_V103 SIIRTC_V001"`}, "U32E"},
	{"gpio_gyro", "gpio.c", nil, "RZWE"},
	{"gpio_rumble", "gpio.c", nil, "V49E"},
	{"gpio_tilt", "gpio.c", nil, "KYGE"},
	{"gpio_koro", "gpio.c", nil, "KHPJ"},
	{"bios", "bios.c", nil, ""},
	{"video", "video.c", nil, ""},
	{"snd", "snd.c", nil, ""},
	{"sys", "sys.c", nil, ""},
	{"demo", "demo.c", nil, ""},
}

var (
	romOnce  sync.Once
	romDir   string
	romErr   error
	romBuilt = map[string]string{}
)

// testROMs builds the test ROMs once (skipping the test when the tools aren't
// installed) and returns name -> path.
func testROMs(t testing.TB) map[string]string {
	t.Helper()
	romOnce.Do(func() { romDir, romErr = buildTestROMs() })
	if romErr != nil {
		t.Skip(romErr)
	}
	return romBuilt
}

// haveTestROMs: whether the test ROMs could be built.
func haveTestROMs() bool {
	romOnce.Do(func() { romDir, romErr = buildTestROMs() })
	return romErr == nil
}

func testROM(t testing.TB, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(testROMs(t)[name])
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func buildTestROMs() (string, error) {
	tools := map[string]string{}
	for _, n := range []string{"clang", "llvm-mc", "ld.lld", "llvm-objcopy"} {
		p, err := exec.LookPath(n)
		if err != nil {
			return "", fmt.Errorf("can't build the test ROMs: %s not installed", n)
		}
		tools[n] = p
	}
	src, err := filepath.Abs("testdata/roms")
	if err != nil {
		return "", err
	}
	dir, err := os.MkdirTemp("", "gba-testroms")
	if err != nil {
		return "", err
	}
	run := func(name string, args ...string) error {
		cmd := exec.Command(tools[name], args...)
		cmd.Dir = src
		if out, err := cmd.CombinedOutput(); err != nil {
			return fmt.Errorf("%s %v: %v\n%s", name, args, err, out)
		}
		return nil
	}
	cflags := []string{"--target=armv4t-none-eabi", "-mcpu=arm7tdmi", "-mthumb", "-O2", "-ffreestanding",
		"-fno-builtin", "-nostdlib", "-fno-pic", "-mno-unaligned-access", "-Wno-unused-function"}
	o := func(n string) string { return filepath.Join(dir, n) }
	if err := run("llvm-mc", "-triple=armv4t-none-eabi", "-filetype=obj", "crt0.s", "-o", o("crt0.o")); err != nil {
		return "", err
	}
	if err := run("clang", append(cflags, "-c", "rt.c", "-o", o("rt.o"))...); err != nil {
		return "", err
	}
	for _, s := range romSpecs {
		args := append(append(append([]string{}, cflags...), s.defs...), "-c", s.src, "-o", o(s.name+".o"))
		if err := run("clang", args...); err != nil {
			return "", err
		}
		if err := run("ld.lld", "-T", "link.ld", o("crt0.o"), o(s.name+".o"), o("rt.o"), "-o", o(s.name+".elf")); err != nil {
			return "", err
		}
		rom := o(s.name + ".gba")
		if err := run("llvm-objcopy", "-O", "binary", o(s.name+".elf"), rom); err != nil {
			return "", err
		}
		if s.code != "" {
			b, err := os.ReadFile(rom)
			if err != nil {
				return "", err
			}
			copy(b[0xac:0xb0], s.code)
			if err := os.WriteFile(rom, b, 0o644); err != nil {
				return "", err
			}
		}
		romBuilt[s.name] = rom
	}
	return dir, nil
}

func TestMain(m *testing.M) {
	code := m.Run()
	if romDir != "" {
		os.RemoveAll(romDir)
	}
	os.Exit(code)
}
