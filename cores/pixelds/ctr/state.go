package ctr

// Save states for the 3DS side: the whole machine (processor, memory,
// kernel, services, graphics and sound state) through cores/pixelds/snap.

import (
	"errors"
	"os"

	"retropixels/cores/pixelds/arm"
	"retropixels/cores/pixelds/snap"
)

func init() {
	// everything that can sit in an interface inside the machine
	snap.Register(
		(*memory)(nil), cp15{},
		(*thread)(nil), (*event)(nil), (*mutex)(nil), (*semaphore)(nil), (*timer)(nil), (*arbiter)(nil),
		(*memBlock)(nil), (*process)(nil), (*resourceLimit)(nil), (*session)(nil), (*port)(nil),
		(*fsFile)(nil), (*fsDir)(nil),
		(*aptService)(nil), (*cfgService)(nil), (*ptmService)(nil), (*dspService)(nil), (*fsService)(nil),
		(*fileService)(nil), (*dirService)(nil), (*gspService)(nil), (*hidService)(nil), (*roService)(nil),
		(*srvService)(nil), (*stubService)(nil),
	)
}

var _ arm.Bus = (*memory)(nil)

// SaveState captures the running machine.
func (m *Machine) SaveState() ([]byte, error) {
	if m.err != nil {
		return nil, errors.New("the game has stopped, so there is nothing to save")
	}
	return snap.Save(m, snap.Options{Extern: []any{m.romfs}})
}

// LoadState puts a captured machine into m, which must be newly loaded
// from the same game file (and not run yet). On an error m is unusable.
func (m *Machine) LoadState(b []byte) error {
	id, title := m.programID, m.title
	if err := snap.Load(b, m, snap.Options{Extern: []any{m.romfs}}); err != nil {
		return err
	}
	if m.programID != id || m.title != title {
		return errors.New("that save state is for another game")
	}
	m.afterLoad()
	return nil
}

// afterLoad rebuilds what a save state doesn't hold: the page table's
// views of memory, and the host files and folders the game had open.
func (m *Machine) afterLoad() {
	mem := m.mem
	for i, pa := range mem.phys {
		if pa != 0 {
			mem.pages[i] = mem.physSlice(pa, pageSize)
		} else {
			mem.pages[i] = nil
		}
	}
	for _, o := range m.kern.handles {
		s, ok := o.(*session)
		if !ok {
			continue
		}
		switch v := s.svc.(type) {
		case *fileService:
			if f := v.f; f != nil && f.f == nil && f.host != "" {
				f.f, _ = os.OpenFile(f.host, f.mode, 0o644)
			}
		case *dirService:
			if d := v.d; d != nil && d.host != "" {
				d.entries, _ = os.ReadDir(d.host)
			}
		}
	}
}
