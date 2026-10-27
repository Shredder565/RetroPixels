package ps3

// The machine: memory, the PPU, the kernel's threads and objects, and the
// system functions. It runs on one host goroutine: PPU threads take turns
// on the one interpreter, and time is virtual (it moves with the
// instructions run, and jumps ahead when every thread is waiting), so a run
// is the same every time.

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime/debug"
	"sort"
	"strings"
)

const (
	TBFreq   = 79800000  // the time base's rate
	virtIPS  = 800000000 // instructions per second of virtual time
	quantum  = 20000     // instructions a thread runs before the next takes a turn
	FPS      = 59.94
	tbPerVBL = uint64(TBFreq * 1001 / 60000)
)

type threadState int

const (
	thReady threadState = iota
	thBlocked
	thExited
	thStopped // created, not started
)

// Thread is a PPU thread.
type Thread struct {
	PPUState
	ID        uint32
	Name      string
	Prio      int32
	Stack     uint32
	StackSize uint32
	state     threadState
	wait      func() bool // blocked: true once the wait is over
	wakeAt    uint64      // blocked with a timeout: when (0 = none)
	timeout   func()      // runs at wakeAt if still blocked
	exitValue uint64
	joinable  bool
	joined    bool
	frames    []guestFrame // HLE code waiting for a guest function to return
	tls       uint32
	waitWhat  string
	lwWant    uint32 // the lwmutex it's waiting to (re)acquire
}

type guestFrame struct {
	saved PPUState
	done  func(ret uint64)
}

// Machine is one running game.
type Machine struct {
	Mem *Memory
	cpu *PPU

	threads []*Thread
	cur     *Thread
	rr      int
	nextID  uint32
	objects map[uint32]any

	hle         []hleEntry
	hleAddr     map[string]uint32 // system function name -> its OPD
	spursQueues map[uint32]uint32 // SPURS queue -> its entry size
	Missing     map[string]int    // system functions called that RetroPixels doesn't have
	sysNext     uint32            // RetroPixels' own area, bump allocated
	retTramp    uint32            // address of the "guest function returned" call
	exitTramp   uint32            // thread entry's return address

	tb      uint64 // virtual time base
	tbFrac  uint64
	nextVBL uint64
	frame   uint64

	// the game's files: the installed folder (USRDIR etc.)
	GameDir string
	SaveDir string
	klic    []byte
	mainELF []byte

	logw   io.Writer
	tty    string
	logN   int
	Exited bool
	Err    error

	proc                      procParam
	tlsImage, tlsFile, tlsMem uint32

	user   *allocator // sys_memory_allocate
	mmap   *allocator // sys_mmapper
	stacks *allocator

	prx []*prxModule

	fs             *fsState
	gcm            *gcmState
	sysu           *sysutilState
	pad            padState
	audio          *audioState
	mp3, mp3Stereo mp3Dec // the music decoder (mp3pic.go)
	spus           *spuState
	sd             *sdScratch
	gpu            *gpuRenderer // drawing on the host GPU (nil: software)
	spurs          *spursState
	misc           map[string]any

	// OnFrame is called at each vblank (the shell takes the picture then)
	OnFrame func()
	screen  *Screen
}

type procParam struct {
	sdk          uint32
	primaryPrio  int32
	primaryStack uint32
	mallocPage   uint32
}

// Options for a new machine.
type Options struct {
	GameDir string    // the installed game (with PARAM.SFO, USRDIR)
	SaveDir string    // where saves go
	Log     io.Writer // the game's and RetroPixels' messages (nil = none)
}

// New loads the installed game and readies its main thread.
func New(opts Options) (*Machine, error) {
	m := &Machine{
		Mem:     NewMemory(),
		objects: map[uint32]any{},
		hleAddr: map[string]uint32{},
		Missing: map[string]int{},
		nextID:  0x01000100,
		GameDir: opts.GameDir,
		SaveDir: opts.SaveDir,
		logw:    opts.Log,
		misc:    map[string]any{},
	}
	m.cpu = &PPU{Mem: m.Mem}
	m.cpu.jit = newJIT(m.Mem)
	m.cpu.HLE = m.hleCall
	m.cpu.Syscall = m.syscall
	m.cpu.TB = func() uint64 { return m.tb }
	m.Mem.Fault = func(addr uint32, write bool) {
		w := "read"
		if write {
			w = "write"
		}
		if m.Err == nil {
			m.Err = fmt.Errorf("bad memory %s at %08X (PC %08X, LR %08X)", w, addr, m.cpu.PC-4, uint32(m.cpu.LR))
			if os.Getenv("PS3_DEBUG") != "" {
				m.Logf("fault from:\n%s", debug.Stack())
			}
		}
		m.cpu.Stop()
	}
	m.Mem.Map(SysBase, SysSize)
	m.Mem.Map(StackBase, StackSize)
	m.Mem.Map(RSXBase, RSXSize)
	m.sysNext = SysBase
	m.user = newAllocator(UserBase, UserSize)
	m.mmap = newAllocator(MMapBase, MMapSize)
	m.stacks = newAllocator(StackBase, StackSize)
	m.Mem.Map(UserBase, UserSize)
	m.Mem.Map(MMapBase, MMapSize)
	m.initHLE()
	m.fs = newFS(m)
	m.retTramp = m.hleSpecial("(guest return)", m.guestReturned)
	m.exitTramp = m.hleSpecial("(thread end)", func(m *Machine, t *Thread) { m.threadExit(t, t.GPR[3]) })

	if err := m.klicensee(); err != nil {
		return nil, err
	}
	elf, err := m.readProgram("USRDIR/EBOOT.BIN")
	if err != nil {
		return nil, err
	}
	m.mainELF = elf
	entry, err := m.loadExec(elf)
	if err != nil {
		return nil, err
	}
	m.startMain(entry)
	if w := os.Getenv("PS3_WATCH"); w != "" {
		m.cpu.Watch = map[uint32]func(*PPU){}
		for _, a := range strings.Split(w, ",") {
			var v uint32
			fmt.Sscanf(a, "%x", &v)
			m.cpu.Watch[v] = func(c *PPU) {
				m.Logf("WATCH %08X: r3 %x r4 %x r5 %x r6 %x LR %08X", c.PC, c.GPR[3], c.GPR[4], c.GPR[5], c.GPR[6], uint32(c.LR))
			}
		}
	}
	if w := os.Getenv("PS3_MEMWATCH"); w != "" { // debugging: log writes to an address
		var a, size uint32 = 0, 4
		fmt.Sscanf(w, "%x:%x", &a, &size)
		m.Mem.Watch(a >> pageBits)
		m.Mem.Hook = func(addr uint32, n int) {
			if addr < a+size && a < addr+uint32(n) {
				name := ""
				if m.cur != nil {
					name = m.cur.Name
				}
				m.Logf("MEMWATCH write %d bytes at %08X by %s PC %08X LR %08X", n, addr, name, m.cpu.PC-4, uint32(m.cpu.LR))
			}
		}
	}
	m.nextVBL = tbPerVBL
	return m, nil
}

// readProgram reads a program from the game folder and decrypts it.
func (m *Machine) readProgram(rel string) ([]byte, error) {
	path := filepath.Join(m.GameDir, filepath.FromSlash(rel))
	if b, err := os.ReadFile(path + ".elf"); err == nil {
		return b, nil
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	elf, err := DecryptSELF(b, m.klic)
	if err != nil && m.klic != nil {
		// (the main program can have its own key, not the game's)
		if e2, err2 := DecryptSELF(b, nil); err2 == nil {
			return e2, nil
		}
	}
	return elf, err
}

// klicensee finds the game's key (for its SPRX and EDAT files).
func (m *Machine) klicensee() error {
	if b, err := os.ReadFile(filepath.Join(m.GameDir, "klicensee.bin")); err == nil && len(b) == 16 {
		m.klic = b
		return nil
	}
	// a game installed elsewhere (another emulator's folder): find the key
	// as Install does, without writing into that folder
	if k, err := FindGameKey(m.GameDir); err == nil {
		m.klic = k
	}
	return nil
}

// Logf writes to the log.
func (m *Machine) Logf(format string, args ...any) {
	if m.logw == nil {
		return
	}
	m.logN++
	if m.logN > 200000 {
		return
	}
	fmt.Fprintf(m.logw, "[%9.4f] ", float64(m.tb)/TBFreq)
	if m.cur != nil {
		fmt.Fprintf(m.logw, "{%s} ", m.cur.Name)
	}
	fmt.Fprintf(m.logw, format, args...)
	if !strings.HasSuffix(format, "\n") {
		fmt.Fprintln(m.logw)
	}
}

// sysAlloc allocates from RetroPixels' own area (never freed).
func (m *Machine) sysAlloc(size, align uint32) uint32 {
	if align < 8 {
		align = 8
	}
	a := (m.sysNext + align - 1) &^ (align - 1)
	m.sysNext = a + size
	if m.sysNext > SysBase+SysSize {
		panic("ps3: out of system memory")
	}
	return a
}

func (m *Machine) newID() uint32 {
	m.nextID++
	return m.nextID
}

// ---- threads

func (m *Machine) newThread(name string, entryOPD uint32, arg uint64, prio int32, stackSize uint32) *Thread {
	if stackSize < 0x4000 {
		stackSize = 0x4000
	}
	stackSize = (stackSize + 0xfff) &^ 0xfff
	stack := m.stacks.alloc(stackSize, 0x10000)
	if stack == 0 {
		return nil
	}
	m.Mem.Fill(stack, 0, stackSize)
	t := &Thread{ID: m.newID(), Name: name, Prio: prio, Stack: stack, StackSize: stackSize, state: thStopped, joinable: true}
	t.GPR[1] = uint64(stack + stackSize - 0x70)
	t.PC = m.Mem.Read32(entryOPD)
	t.GPR[2] = uint64(m.Mem.Read32(entryOPD + 4))
	t.GPR[3] = arg
	t.LR = uint64(m.exitTramp)
	t.VSCR = 0x10000
	t.GPR[13] = uint64(m.allocTLS())
	t.tls = uint32(t.GPR[13])
	m.threads = append(m.threads, t)
	m.objects[t.ID] = t
	return t
}

// allocTLS makes a thread's TLS block from the program's TLS image; it
// returns the r13 value (block + 0x7030).
func (m *Machine) allocTLS() uint32 {
	if m.tlsMem == 0 {
		return 0
	}
	size := 0x30 + m.tlsMem
	a := m.sysAlloc(size, 0x80)
	m.Mem.Fill(a, 0, size)
	buf := make([]byte, m.tlsFile)
	m.Mem.ReadBytes(m.tlsImage, buf)
	m.Mem.WriteBytes(a+0x30, buf)
	return a + 0x30 + 0x7000
}

func (m *Machine) startMain(entry uint32) {
	stack := m.proc.primaryStack
	if stack == 0 {
		stack = 0x100000
	}
	prio := m.proc.primaryPrio
	if prio == 0 {
		prio = 1000
	}
	t := m.newThread("main", entry, 0, prio, stack)
	// argv: the program's path
	argv := m.sysAlloc(16, 16)
	path := m.sysAlloc(64, 16)
	m.Mem.WriteString(path, "/dev_hdd0/game/"+m.titleID()+"/USRDIR/EBOOT.BIN")
	m.Mem.Write32(argv, path)
	m.Mem.Write32(argv+4, 0)
	envp := m.sysAlloc(8, 8)
	t.GPR[3] = 1
	t.GPR[4] = uint64(argv)
	t.GPR[5] = uint64(envp)
	t.GPR[6] = 0
	t.GPR[7] = uint64(t.ID)
	t.GPR[8] = uint64(m.tlsImage)
	t.GPR[9] = uint64(m.tlsFile)
	t.GPR[10] = uint64(m.tlsMem)
	t.GPR[11] = uint64(entry)
	t.GPR[12] = uint64(m.proc.mallocPage)
	t.GPR[13] = 0
	t.state = thReady
}

func (m *Machine) threadExit(t *Thread, v uint64) {
	t.exitValue = v
	t.state = thExited
	m.Logf("thread %s (%x) ended (%x)", t.Name, t.ID, v)
	if t.Name == "main" {
		// the main thread returning ends the process
	}
}

// block makes t wait until done() reports true (done sets its results).
func (m *Machine) block(t *Thread, what string, done func() bool) {
	if done() {
		return
	}
	t.state = thBlocked
	t.wait = done
	t.waitWhat = what
	t.wakeAt = 0
	t.timeout = nil
	if t == m.cur {
		m.cpu.Stop()
	}
}

// blockTimeout is block with a timeout (in time base ticks; 0 = none).
func (m *Machine) blockTimeout(t *Thread, what string, ticks uint64, done func() bool, timedOut func()) {
	m.block(t, what, done)
	if t.state == thBlocked && ticks != 0 {
		t.wakeAt = m.tb + ticks
		t.timeout = timedOut
	}
}

func usToTB(us uint64) uint64 { return us * TBFreq / 1000000 }

// ---- guest calls from HLE code

// callGuest makes thread t run the guest function at opd with args, then
// continue where it was (t.PC); done gets the function's r3.
func (m *Machine) callGuest(t *Thread, opd uint32, args []uint64, done func(ret uint64)) {
	t.frames = append(t.frames, guestFrame{saved: t.PPUState, done: done})
	for i, a := range args {
		t.GPR[3+i] = a
	}
	t.GPR[1] = (t.GPR[1] - 0x200) &^ 15
	t.PC = m.Mem.Read32(opd)
	t.GPR[2] = uint64(m.Mem.Read32(opd + 4))
	t.LR = uint64(m.retTramp)
}

func (m *Machine) guestReturned(_ *Machine, t *Thread) {
	n := len(t.frames)
	if n == 0 {
		m.fail("a guest function returned with nothing waiting for it")
		return
	}
	f := t.frames[n-1]
	t.frames = t.frames[:n-1]
	ret := t.GPR[3]
	t.PPUState = f.saved
	if f.done != nil {
		f.done(ret)
	}
}

func (m *Machine) fail(format string, args ...any) {
	if m.Err == nil {
		m.Err = fmt.Errorf(format, args...)
		m.Logf("STOP: %v", m.Err)
	}
	m.cpu.Stop()
}

// ---- running

// RunFrame runs until the next vblank.
func (m *Machine) RunFrame() error {
	if m.Err != nil {
		return m.Err
	}
	target := m.nextVBL
	m.runUntil(target)
	if m.Err != nil {
		return m.Err
	}
	if m.tb >= m.nextVBL {
		m.nextVBL += tbPerVBL
		m.frame++
		m.vblank()
		if m.OnFrame != nil {
			m.OnFrame()
		}
	}
	return m.Err
}

func (m *Machine) runUntil(target uint64) {
	for m.tb < target && m.Err == nil && !m.Exited {
		t := m.pick()
		if t == nil {
			// no PPU thread can run: let the SPUs work, else jump to the
			// next timeout (or the target)
			if !m.spuIdle() {
				if ran := m.runSPUs(quantum); ran > 0 {
					m.advance(uint64(ran))
					m.rsxRun()
					continue
				}
			}
			next := target
			for _, th := range m.threads {
				if th.state == thBlocked && th.wakeAt != 0 && th.wakeAt < next {
					next = th.wakeAt
				}
			}
			if next > m.tb {
				m.tb = next
			}
			m.checkTimeouts()
			continue
		}
		m.cur = t
		m.cpu.PPUState = &t.PPUState
		left := (target - m.tb) * (virtIPS / TBFreq)
		n := quantum
		if left < uint64(n) && left > 0 {
			n = int(left)
		}
		before := m.cpu.Ran
		m.cpu.Run(n)
		ran := m.cpu.Ran - before
		m.advance(ran)
		if m.cpu.Err != nil && m.Err == nil {
			m.Err = m.cpu.Err
			m.Logf("STOP: %v", m.Err)
			m.dumpThread(t)
		}
		m.cur = nil
		m.runSPUs(ran)
		m.rsxRun()
	}
}

func (m *Machine) advance(instrs uint64) {
	m.tbFrac += instrs * TBFreq
	m.tb += m.tbFrac / virtIPS
	m.tbFrac %= virtIPS
	m.checkTimeouts()
}

func (m *Machine) checkTimeouts() {
	for _, t := range m.threads {
		if t.state == thBlocked && t.wakeAt != 0 && m.tb >= t.wakeAt {
			if t.wait != nil && t.wait() {
				t.state = thReady
				t.wait = nil
				continue
			}
			t.state = thReady
			t.wait = nil
			t.wakeAt = 0
			if t.timeout != nil {
				t.timeout()
			}
		}
	}
}

// pick chooses the next thread to run (round robin over those that can).
func (m *Machine) pick() *Thread {
	n := len(m.threads)
	for k := 0; k < n; k++ {
		m.rr = (m.rr + 1) % n
		t := m.threads[m.rr]
		switch t.state {
		case thReady:
			return t
		case thBlocked:
			if t.wait != nil && t.wait() {
				t.state = thReady
				t.wait = nil
				t.wakeAt = 0
				return t
			}
		}
	}
	return nil
}

func (m *Machine) dumpThread(t *Thread) {
	m.Logf("thread %s: PC %08X LR %08X CTR %08X CR %08X", t.Name, t.PC, uint32(t.LR), uint32(t.CTR), t.CR)
	for i := 0; i < 32; i += 4 {
		m.Logf("  r%-2d %016X %016X %016X %016X", i, t.GPR[i], t.GPR[i+1], t.GPR[i+2], t.GPR[i+3])
	}
}

// Threads describes every thread (for the log).
func (m *Machine) Threads() string {
	var b strings.Builder
	ts := append([]*Thread(nil), m.threads...)
	sort.Slice(ts, func(i, j int) bool { return ts[i].ID < ts[j].ID })
	for _, t := range ts {
		st := [...]string{"ready", "waiting", "ended", "not started"}[t.state]
		fmt.Fprintf(&b, "%s (%x): %s %s PC %08X LR %08X\n", t.Name, t.ID, st, t.waitWhat, t.PC, uint32(t.LR))
	}
	return b.String()
}

// Frame is the number of vblanks so far.
func (m *Machine) Frame() uint64 { return m.frame }

var errExit = errors.New("the game ended")

func readFileCase(p string) ([]byte, error) { return os.ReadFile(findCase(p)) }

// decryptWithGameKey decrypts a module with the game's klicensee, finding
// the key in the game's program the first time.
func (m *Machine) decryptWithGameKey(b []byte) ([]byte, error) {
	if !IsSELF(b) {
		return b, nil
	}
	if out, err := DecryptSELF(b, m.klic); err == nil {
		return out, nil
	}
	if m.klic == nil && m.mainELF != nil {
		// find the NPD block in the module's control info and match it
		for i := 0; i+0x80 <= len(b) && i < 0x2000; i++ {
			if string(b[i:i+4]) == "NPD\x00" {
				if k, ok := FindKlicensee(m.mainELF, b[i:i+0x80]); ok {
					m.klic = k
				}
				break
			}
		}
	}
	return DecryptSELF(b, m.klic)
}
