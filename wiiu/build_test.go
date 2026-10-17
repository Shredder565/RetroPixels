package wiiu

// Building RetroPixels' own Wii U test programs with clang and ld.lld (both
// target PowerPC out of the box), from the C sources in testdata/progs and
// the tiny SDK in testdata/sdk. Tests that need them skip when the tools
// aren't installed.

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

func haveTools() bool {
	for _, t := range []string{"clang", "ld.lld"} {
		if _, err := exec.LookPath(t); err != nil {
			return false
		}
	}
	return true
}

var ppcFlags = []string{"--target=powerpc-unknown-eabi", "-mcpu=750", "-O2", "-ffreestanding", "-nostdlib",
	"-fno-pic", "-fwrapv", "-fno-stack-protector", "-ffp-contract=off", "-fno-exceptions", "-fno-asynchronous-unwind-tables",
	"-fno-unwind-tables", "-Wall", "-Wno-unused-function"}

// buildRPX compiles C files (in testdata/progs) into an RPX.
func buildRPX(t testing.TB, compress bool, files ...string) []byte {
	t.Helper()
	if !haveTools() {
		t.Skip("clang and ld.lld are needed to build the test programs")
	}
	dir := t.TempDir()
	sdk, _ := filepath.Abs("testdata/sdk")
	progs, _ := filepath.Abs("testdata/progs")

	// import stubs
	imports, err := os.ReadFile(filepath.Join(sdk, "imports.txt"))
	if err != nil {
		t.Fatal(err)
	}
	libs := map[string][]string{}
	var order []string
	for _, line := range strings.Split(string(imports), "\n") {
		f := strings.Fields(line)
		if len(f) < 2 {
			continue
		}
		if _, ok := libs[f[0]]; !ok {
			order = append(order, f[0])
		}
		libs[f[0]] = append(libs[f[0]], f[1:]...)
	}
	var asm, script strings.Builder
	script.WriteString("ENTRY(_start)\nSECTIONS {\n  . = 0x01FF0000;\n")
	sort.Strings(order)
	for _, lib := range order {
		var funcs, data []string
		for _, n := range libs[lib] {
			if strings.HasPrefix(n, "data:") {
				data = append(data, strings.TrimPrefix(n, "data:"))
			} else {
				funcs = append(funcs, n)
			}
		}
		for _, kind := range []string{"f", "d"} {
			list := funcs
			flags := "ax"
			if kind == "d" {
				list, flags = data, "a"
			}
			if len(list) == 0 {
				continue
			}
			sec := "." + kind + "import_" + lib
			fmt.Fprintf(&asm, ".section %s, \"%s\", @progbits\n.align 3\n.long 1\n.long 0\n.asciz \"%s\"\n.align 3\n", sec, flags, lib)
			for _, n := range list {
				typ := "@function"
				if kind == "d" {
					typ = "@object"
				}
				fmt.Fprintf(&asm, ".global %s\n.type %s, %s\n%s:\n.long 0\n.long 0\n", n, n, typ, n)
			}
			fmt.Fprintf(&script, "  %s : { KEEP(*(%s)) }\n", sec, sec)
		}
	}
	script.WriteString(`  . = 0x02000000;
  .text : { *(.text .text.*) }
  . = 0x10000000;
  .rodata : { *(.rodata .rodata.* .sdata2 .sdata2.* .sbss2 .sbss2.*) }
  .data : { *(.data .data.* .sdata .sdata.*) }
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
`)
	write := func(name, s string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	stubs := write("stubs.s", asm.String())
	ld := write("link.ld", script.String())

	var objs []string
	compile := func(src string) {
		obj := filepath.Join(dir, strings.TrimSuffix(filepath.Base(src), filepath.Ext(src))+fmt.Sprint(len(objs))+".o")
		args := append(append([]string{}, ppcFlags...), "-I", sdk, "-c", src, "-o", obj)
		if out, err := exec.Command("clang", args...).CombinedOutput(); err != nil {
			t.Fatalf("clang %s: %v\n%s", src, err, out)
		}
		objs = append(objs, obj)
	}
	compile(stubs)
	compile(filepath.Join(sdk, "crt0.c"))
	compile(filepath.Join(sdk, "libgcc.c"))
	for _, f := range files {
		compile(filepath.Join(progs, f))
	}
	elf := filepath.Join(dir, "prog.elf")
	args := append([]string{"-q", "-T", ld, "-o", elf, "--no-check-sections", "-static"}, objs...)
	if out, err := exec.Command("ld.lld", args...).CombinedOutput(); err != nil {
		t.Fatalf("ld.lld: %v\n%s", err, out)
	}
	b, err := os.ReadFile(elf)
	if err != nil {
		t.Fatal(err)
	}
	rpx, err := ELFToRPX(b, 0x20000, compress)
	if err != nil {
		t.Fatal(err)
	}
	if dump := os.Getenv("WIIU_DUMP"); dump != "" {
		os.WriteFile(filepath.Join(dump, strings.TrimSuffix(files[0], ".c")+".rpx"), rpx, 0o644)
		os.WriteFile(filepath.Join(dump, strings.TrimSuffix(files[0], ".c")+".elf"), b, 0o644)
	}
	return rpx
}

// runRPX runs a program until it exits (or frames run out) and returns its log.
func runRPX(t testing.TB, rpx []byte, frames int) (*Machine, string) {
	t.Helper()
	m, err := New(rpx, "test.rpx", nil)
	if err != nil {
		t.Fatal(err)
	}
	var log strings.Builder
	m.Log = func(s string) { log.WriteString(s + "\n") }
	for i := 0; i < frames && !m.Exited && m.Err == nil; i++ {
		m.RunFrame()
	}
	if m.Err != nil {
		t.Fatalf("%v\nlog:\n%s\nthreads:\n%s", m.Err, log.String(), m.ThreadsSummary())
	}
	return m, log.String()
}
