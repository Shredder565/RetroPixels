package wiiu

import "encoding/binary"

// dxVSRun is one vertex shader test run (the Direct3D side is in
// latte_dxbc_windows_test.go).
type dxVSRun struct {
	mem   map[uint32]uint32 // program memory (GPU words)
	vs    uint32
	fs    uint32
	nGPR  int        // R1..RnGPR come from in
	in    [][]uint32 // per vertex, 4*nGPR words
	nOut  int        // outputs: position, then parameters 0..nOut-2
	sys   [dxSysSize * 4]uint32
	cfile [256 * 4]uint32
}

func (r *dxVSRun) read32(a uint32) uint32 { return r.mem[a] }

func (r *dxVSRun) load(addr uint32, code []byte) {
	for i := 0; i+3 < len(code); i += 4 {
		r.mem[addr+uint32(i)] = binary.LittleEndian.Uint32(code[i:])
	}
}

func newVSRun() *dxVSRun { return &dxVSRun{mem: map[uint32]uint32{}, vs: tVS, fs: tVS + 0x10000} }

func dxSimpleRunAny(vs *shaderAsm, nGPR, nOut int) *dxVSRun {
	r := newVSRun()
	r.load(r.vs, vs.Bytes())
	ret := &shaderAsm{}
	ret.CF(cfRETURN, 0, 0, 0, 0)
	r.load(r.fs, ret.Bytes())
	r.nGPR, r.nOut = nGPR, nOut
	return r
}
