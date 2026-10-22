package wiiu

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestHello(t *testing.T) {
	for _, z := range []bool{false, true} {
		m, log := runRPX(t, buildRPX(t, z, "hello.c"), 10)
		want := "hello from test.rpx: -42 42 beef wii u 3.250 1234567890123"
		if !strings.Contains(log, want) || !strings.Contains(log, "snprintf: 00007|ab  |Z (12)") {
			t.Fatalf("log:\n%s", log)
		}
		if !m.Exited || m.ExitCode != 3 {
			t.Fatalf("exited %v code %d", m.Exited, m.ExitCode)
		}
	}
}

// TestCPU runs the same C program on the emulated Espresso and natively; the
// results must match.
func TestCPU(t *testing.T) {
	rpx := buildRPX(t, false, "cputest.c")
	native := filepath.Join(t.TempDir(), "cputest")
	out, err := exec.Command("clang", "-O2", "-DNATIVE", "-fwrapv", "-ffp-contract=off", "-o", native,
		"testdata/progs/cputest.c", "-lm").CombinedOutput()
	if err != nil {
		t.Skipf("can't build the native version: %v\n%s", err, out)
	}
	want, err := exec.Command(native).Output()
	if err != nil {
		t.Fatal(err)
	}
	_, log := runRPX(t, rpx, 600)
	log = strings.Replace(log, "the program ended (exit code 0)\n", "", 1)
	if strings.TrimSpace(log) != strings.TrimSpace(string(want)) {
		t.Fatalf("emulated:\n%s\nnative:\n%s", log, want)
	}
}

func TestThreads(t *testing.T) {
	_, log := runRPX(t, buildRPX(t, false, "threads.c"), 600)
	for _, want := range []string{"counter 3000 results 10 21 32", "queue sum 210", "slept 100 ms, alarms ok", "heap 1 1 1", "reuse 1"} {
		if !strings.Contains(log, want) {
			t.Fatalf("missing %q in log:\n%s", want, log)
		}
	}
}

// TestSync: threads waiting on each other the way Cafe OS has them do.
func TestSync(t *testing.T) {
	rpx := buildModule(t, buildOpts{imports: []string{
		"coreinit OSSuspendThread OSIsThreadSuspended OSIsThreadTerminated OSDetachThread OSSetThreadDeallocator",
		"coreinit OSSetThreadCleanupCallback OSCreateThreadType OSGetThreadPriority OSExitThread OSDisableInterrupts",
		"coreinit OSRestoreInterrupts OSInitSemaphore OSWaitSemaphore OSSignalSemaphore OSGetSemaphoreCount OSResetEvent",
		"coreinit OSSignalEventAll OSWaitEventWithTimeout OSJamMessage OSPeekMessage OSInitSpinLock OSAcquireSpinLock",
		"coreinit OSReleaseSpinLock OSInitThreadQueue OSSleepThread OSWakeupThread OSSetAlarmEx OSInitAlarmQueue",
		"coreinit OSGetAlarmFromQueue OSGetSystemInfo OSInitRendezvous OSWaitRendezvous"}},
		"synctest.c")
	m, log := runRPX(t, rpx, 300)
	for _, want := range []string{
		"suspend 1 0 1 2 0 1 1 8 7 0",
		"join 1 1 0 1 5 1 1 6 0 2",
		"events 2 1 3 0 1 0 1",
		"sem 0 1 3 0",
		"mutex 20",
		"mq 1234 4 0 1 1 9 2 0",
		"prio 0 3 0 IA 31",
		"spin 1",
		"queue 7 run 1 1 5",
		"alarm 1 0 1 1 0 0",
		"rendezvous 0 2",
		"sysinfo 248625000 1243125000 5 1",
		"sync done",
	} {
		if !strings.Contains(log, want) {
			t.Errorf("want %q in:\n%s\nthreads:\n%s", want, log, m.ThreadsSummary())
		}
	}
}

func TestOSScreen(t *testing.T) {
	m, err := New(buildRPX(t, false, "screen.c"), "screen.rpx", nil)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 30; i++ {
		m.RunFrame()
	}
	if m.Err != nil {
		t.Fatal(m.Err)
	}
	tv := m.screen.TV
	if tv[0] != 0x102040 {
		t.Fatalf("background %06x", tv[0])
	}
	if tv[300*TVWidth+350] != 0xff0000 {
		t.Fatalf("line %06x", tv[300*TVWidth+350])
	}
	text := 0
	for y := 0; y < 24*3; y++ {
		for x := 0; x < 400; x++ {
			if tv[y*TVWidth+x] == 0xffffff {
				text++
			}
		}
	}
	if text < 500 {
		t.Fatalf("only %d text pixels", text)
	}
	// press A twice: the program counts them and ends
	for i := 0; i < 4 && !m.Exited; i++ {
		b := uint32(0)
		if i%2 == 0 {
			b = BtnA
		}
		m.SetGamePad(PadState{Buttons: b})
		for j := 0; j < 3; j++ {
			m.RunFrame()
		}
	}
	if !m.Exited {
		t.Fatalf("still running")
	}
	if os.Getenv("WIIU_DUMP") != "" {
		savePNG(t, os.Getenv("WIIU_DUMP")+"/screen.png", m.screen.TV, TVWidth, TVHeight)
	}
}

// The demo that comes in the download: it draws the sticks and the held
// buttons, and ends on Plus + Minus. With WIIU_DEMO set, the test writes the
// .rpx there (how testdata/demo.rpx is made).
func TestDemo(t *testing.T) {
	rpx := buildRPX(t, true, "demo.c")
	if out := os.Getenv("WIIU_DEMO"); out != "" {
		if err := os.WriteFile(out, rpx, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	m, err := New(rpx, "demo.rpx", nil)
	if err != nil {
		t.Fatal(err)
	}
	var log strings.Builder
	m.Log = func(s string) { log.WriteString(s + "\n") }
	m.SetGamePad(PadState{Buttons: BtnA | BtnL, LX: 1, LY: 1})
	for i := 0; i < 20; i++ {
		m.RunFrame()
	}
	if m.Err != nil || m.Exited {
		t.Fatalf("%v %v\n%s", m.Err, m.Exited, log.String())
	}
	if dump := os.Getenv("WIIU_DUMP"); dump != "" {
		savePNG(t, filepath.Join(dump, "demo-tv.png"), m.screen.TV, TVWidth, TVHeight)
		savePNG(t, filepath.Join(dump, "demo-gamepad.png"), m.screen.DRC, DRCWidth, DRCHeight)
	}
	// the left stick's dot is at the top right of its box
	if m.screen.TV[(170+6)*TVWidth+40+114] != 0x40e060 {
		t.Error("left stick")
	}
	m.SetGamePad(PadState{Buttons: BtnPlus | BtnMinus})
	for i := 0; i < 10 && !m.Exited; i++ {
		m.RunFrame()
	}
	if !m.Exited || !strings.Contains(log.String(), "demo ended") {
		t.Fatalf("didn't end:\n%s", log.String())
	}
}

// A program whose code is too far from the libraries for a direct branch.
func TestFarCalls(t *testing.T) {
	m, log := runRPX(t, buildRPX(t, true, "farcall.c"), 10)
	if !strings.Contains(log, "far call works") || !m.Exited || m.ExitCode != 5 {
		t.Fatalf("exited %v code %d err %v log:\n%s", m.Exited, m.ExitCode, m.Err, log)
	}
}

func TestVirtualMemoryAndDMA(t *testing.T) {
	rpx := buildModule(t, buildOpts{imports: []string{
		"coreinit OSGetMapVirtAddrRange OSGetAvailPhysAddrRange OSAllocVirtAddr OSMapMemory OSQueryVirtAddr OSEffectiveToPhysical",
		"dmae DMAECopyMem DMAEFillMem DMAEWaitDone DMAESemaphore"}}, "vmtest.c")
	_, log := runRPX(t, rpx, 10)
	for _, want := range []string{"vm: range a0000000+20000000 va a0000000 map 1 seen 12345678 back cafe query 1",
		"dmae: 44332211 88776655 abcd abcd", "vm: phys 1 map 1 1 shared beef", "dmae: sem 1"} {
		if !strings.Contains(log, want) {
			t.Errorf("want %q in:\n%s", want, log)
		}
	}
}

func TestCoroutinesAndLockedCache(t *testing.T) {
	rpx := buildModule(t, buildOpts{imports: []string{
		"coreinit OSInitCoroutine OSSwitchCoroutine LCAlloc LCDealloc LCGetAllocatableSize LCEnableDMA LCLoadDMABlocks LCStoreDMABlocks"}},
		"cotest.c")
	_, log := runRPX(t, rpx, 10)
	for _, want := range []string{"co: steps 33 local 7", "co: entry 1 1 r31 1", "lc: 4000 3000 4000 100 16"} {
		if !strings.Contains(log, want) {
			t.Errorf("want %q in:\n%s", want, log)
		}
	}
}

// The heaps, with the values Cafe OS's (and Cemu's) heap code gives.
func TestHeaps(t *testing.T) {
	rpx := buildModule(t, buildOpts{exports: []string{"__preinit_user"}, imports: []string{
		"coreinit MEMCreateExpHeapEx MEMResizeForMBlockExpHeap MEMGetAllocatableSizeForExpHeapEx MEMGetTotalFreeSizeForExpHeap",
		"coreinit MEMGetSizeForMBlockExpHeap MEMVisitAllocatedForExpHeap MEMFindContainHeap MEMFindParentHeap",
		"coreinit MEMCreateFrmHeapEx MEMRecordStateForFrmHeap MEMFreeByStateToFrmHeap MEMGetAllocatableSizeForFrmHeapEx",
		"coreinit MEMCreateUnitHeapEx MEMAllocFromUnitHeap MEMFreeToUnitHeap MEMCountFreeBlockForUnitHeap MEMCalcHeapSizeForUnitHeap",
		"coreinit MEMInitBlockHeap MEMAllocFromBlockHeapEx MEMFreeToBlockHeap MEMGetTrackingLeftInBlockHeap",
		"coreinit MEMGetTotalFreeSizeForBlockHeap MEMGetAllocatableSizeForBlockHeapEx",
		"coreinit OSGetMemBound OSGetForegroundBucket OSGetForegroundBucketFreeArea"}}, "memtest.c")
	_, log := runRPX(t, rpx, 10)
	for _, want := range []string{
		"exp: ff98 ff98 ff80 | 68 ff00 fd70 180 20 | 80 c0 fd64 fd38 | 3 1e0 1 | fdb0 ff98",
		"nested: 1 1 1",
		"frm: fb4 f00 | 4c 1 80 fc0 e40 | 1 fa4 0 fb4",
		"unit: 14 60 a0 12 60 3e4",
		"block: 1 0 f00 2 e00 e00 | 3 f00",
		"bounds: f4000000 2000000 50000000 e0000000 4000000 e0000000 2800000",
		"base: f4000000 e0000000 1 1ffffb4 27fffb4",
		"preinit: 1 f4000000 e0000000 1 0",
	} {
		if !strings.Contains(log, want) {
			t.Errorf("want %q in:\n%s", want, log)
		}
	}
}
