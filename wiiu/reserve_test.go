package wiiu

import "testing"

// Core A takes a reservation, core B changes the word, then A's stwcx. must
// fail: otherwise a lock-free queue shared by two cores loses B's update.
func TestReservationAcrossCores(t *testing.T) {
	mem := NewMemory()
	const code, data = 0x02000000, 0x10000000
	mem.Write32(code, 0x7c602028)   // lwarx r3, 0, r4
	mem.Write32(code+4, 0x7ca0212d) // stwcx. r5, 0, r4
	mem.Write32(data, 1)
	a := &CPU{Mem: mem, Context: &Context{PC: code}}
	b := &CPU{Mem: mem, Context: &Context{PC: code}}
	a.GPR[4], a.GPR[5] = data, 2
	b.GPR[4], b.GPR[5] = data, 3
	a.Step() // A: lwarx
	b.Step() // B: lwarx
	b.Step() // B: stwcx. (succeeds, word = 3)
	a.Step() // A: stwcx. must fail
	if v := mem.Read32(data); v != 3 {
		t.Fatalf("word = %d, want 3 (core B's store)", v)
	}
	if a.CR>>28&2 != 0 {
		t.Fatal("core A's stwcx. succeeded after core B changed the word")
	}
	if b.CR>>28&2 == 0 {
		t.Fatal("core B's stwcx. failed")
	}
}
