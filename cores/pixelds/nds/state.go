package nds

import (
	"bytes"
	"compress/flate"
	"encoding/gob"
	"errors"
	"io"

	"retropixels/cores/pixelds/arm"
)

// Save states: everything that changes while a game runs, gob-encoded and
// compressed. The card's ROM isn't in it (the game is loaded again first).

type timerSnap struct {
	Reload, Ctrl, Base uint16
	Start, OverflowAt  int64
}

type dmaSnap struct{ SAD, DAD, CNT, Src, Dst, Left uint32 }

type fifoSnap struct {
	Data  [16]uint32
	N, At int
}

type engineSnap struct {
	Dispcnt                      uint32
	BGCnt, HOfs, VOfs            [4]uint16
	PA, PB, PC, PD               [2]int16
	RefX, RefY, CurX, CurY       [2]int32
	Win0H, Win1H, Win0V, Win1V   uint16
	WinIn, WinOut, Mosaic        uint16
	BldCnt, BldAlpha, BldY, MBri uint16
}

type chanSnap struct {
	Cnt, SAD          uint32
	Tmr, Pnt          uint16
	Length            uint32
	Pos               int
	Ticks             float64
	Sample            int32
	ADPCMVal, LoopVal int32
	ADPCMIdx, LoopIdx int
	LFSR              uint16
}

type snapshot struct {
	Now                            int64
	ARM9, ARM7                     arm.State
	MainRAM, SharedWR, ARM7WRAM    []byte
	Pal, OAM, ITCM, DTCM           []byte
	Banks                          [9][]byte
	VRAMCnt                        [9]uint8
	WRAMCnt                        uint8
	CP15Control, DTCMReg, ITCMReg  uint32
	CP15Regs                       map[uint32]uint32
	IME, IE, IF                    [2]uint32
	Timers                         [2][4]timerSnap
	DMA                            [2][4]dmaSnap
	DMAFill                        [4]uint32
	Sync9, Sync7, Cnt9, Cnt7       uint16
	To7, To9                       fifoSnap
	Last9, Last7                   uint32
	DivCnt, SqrtCnt                uint16
	Numer, Denom, Quot, Rem, SqrtP uint64
	SqrtR                          uint32
	Engines                        [2]engineSnap
	GPU3D                          []byte
	Chans                          [16]chanSnap
	SndCnt, SndBias                uint16
	CaptCnt                        [2]uint8
	CaptDAD, CaptLen               [2]uint32
	SndAt                          int64
	SndFrac                        float64
	CartCmd                        [8]uint8
	ROMCtrl                        uint32
	AuxSPICnt                      uint16
	AuxSPIData, CartCmdByte        uint8
	CartAddr                       uint32
	CartWords                      int
	CartDoneAt                     int64
	Backup                         []byte
	BackupAddrBytes, BackupPos     int
	BackupCmd                      uint8
	BackupAddr                     uint32
	BackupWEL                      bool
	SPICnt                         uint16
	SPIData                        uint8
	SPIPos, SPIDev                 int
	TouchCmd                       uint8
	TouchVal                       uint16
	PMIndex                        uint8
	PMRegs                         [8]uint8
	Firmware                       []byte
	FWCmd                          uint8
	FWAddr                         uint32
	FWWEL                          bool
	RTCReg                         uint16
	RTCFlags                       [4]bool // cs, sck, haveCmd, reading
	RTCBits, RTCCmd, RTCPos        int
	RTCCmdByte, RTCOut             uint8
	RTCData                        []byte
	RTCStatus                      [3]uint8 // status1, status2, free
	RTCAlarm                       [2][3]uint8
	RTCAdjust                      uint8
	Regs                           [12]uint16 // powcnt1, exmemcnt, rcnt, keycnt9, keycnt7, exmemcnt7, powcnt2, bios7prot, dispstat9, dispstat7, postflg, haltcnt
	Vcount                         int
	LineStart, NextLineEvent       int64
	HBlankDone, Capturing          bool
	DispCapCnt                     uint32
}

func (m *Machine) SaveState() ([]byte, error) {
	s := &snapshot{Now: m.now, ARM9: m.arm9.SaveState(), ARM7: m.arm7.SaveState(),
		MainRAM: m.mainRAM, SharedWR: m.sharedWR, ARM7WRAM: m.arm7WRAM, Pal: m.pal, OAM: m.oam,
		ITCM: m.cp15.itcm, DTCM: m.cp15.dtcm, VRAMCnt: m.vram.cnt, WRAMCnt: m.wramcnt,
		CP15Control: m.cp15.control, DTCMReg: m.cp15.dtcmReg, ITCMReg: m.cp15.itcmReg, CP15Regs: m.cp15.regs,
		IME: [2]uint32{m.irq9.ime, m.irq7.ime}, IE: [2]uint32{m.irq9.ie, m.irq7.ie}, IF: [2]uint32{m.irq9.ifl, m.irq7.ifl},
		DMAFill: m.dmaFill, Sync9: m.ipc.sync9, Sync7: m.ipc.sync7, Cnt9: m.ipc.cnt9, Cnt7: m.ipc.cnt7,
		To7: fifoSnap{m.ipc.to7.data, m.ipc.to7.n, m.ipc.to7.at}, To9: fifoSnap{m.ipc.to9.data, m.ipc.to9.n, m.ipc.to9.at},
		Last9: m.ipc.last9, Last7: m.ipc.last7,
		DivCnt: m.math.divcnt, SqrtCnt: m.math.sqrtcnt, Numer: m.math.numer, Denom: m.math.denom, Quot: m.math.quot,
		Rem: m.math.rem, SqrtP: m.math.sqrtParam, SqrtR: m.math.sqrtResult,
		GPU3D:  m.gpu3d.saveState(),
		SndCnt: m.spu.cnt, SndBias: m.spu.bias, SndAt: m.spu.at, SndFrac: m.spu.frac,
	}
	for i := range m.vram.banks {
		s.Banks[i] = m.vram.banks[i]
	}
	for i := 0; i < 4; i++ {
		for k, t := range [2]*timer{&m.timers9[i], &m.timers7[i]} {
			s.Timers[k][i] = timerSnap{t.reload, t.ctrl, t.base, t.start, t.overflowAt}
		}
		for k, d := range [2]*dma{&m.dma9[i], &m.dma7[i]} {
			s.DMA[k][i] = dmaSnap{d.sad, d.dad, d.cnt, d.src, d.dst, d.left}
		}
	}
	for k, e := range [2]*engine2D{m.gpuA, m.gpuB} {
		s.Engines[k] = engineSnap{e.dispcnt, e.bgcnt, e.hofs, e.vofs, e.pa, e.pb, e.pc, e.pd, e.refX, e.refY, e.curX, e.curY,
			e.win0h, e.win1h, e.win0v, e.win1v, e.winin, e.winout, e.mosaic, e.bldcnt, e.bldalpha, e.bldy, e.masterBright}
	}
	for i := range m.spu.ch {
		c := &m.spu.ch[i]
		s.Chans[i] = chanSnap{c.cnt, c.sad, c.tmr, c.pnt, c.length, c.pos, c.ticks, c.sample, c.adpcmVal, c.loopVal, c.adpcmIdx, c.loopIdx, c.lfsr}
	}
	for i := 0; i < 2; i++ {
		s.CaptCnt[i], s.CaptDAD[i], s.CaptLen[i] = m.spu.capt[i].cnt, m.spu.capt[i].dad, m.spu.capt[i].len
	}
	c := m.cart
	s.CartCmd, s.ROMCtrl, s.AuxSPICnt, s.AuxSPIData, s.CartCmdByte = c.cmd, c.romctrl, c.auxspicnt, c.spiData, c.cmdByte
	s.CartAddr, s.CartWords, s.CartDoneAt = c.addr, c.wordsLeft, c.doneAt
	b := c.backup
	s.Backup, s.BackupAddrBytes, s.BackupPos, s.BackupCmd, s.BackupAddr, s.BackupWEL = b.mem, b.addrBytes, b.pos, b.cmd, b.addr, b.wel
	p := m.spi
	s.SPICnt, s.SPIData, s.SPIPos, s.SPIDev = p.cnt, p.data, p.pos, p.dev
	s.TouchCmd, s.TouchVal, s.PMIndex, s.PMRegs = p.touch.cmd, p.touch.val, p.pm.index, p.pm.regs
	s.Firmware, s.FWCmd, s.FWAddr, s.FWWEL = p.fw.image, p.fw.cmd, p.fw.addr, p.fw.wel
	r := &m.rtc
	s.RTCReg, s.RTCFlags = r.reg, [4]bool{r.cs, r.sck, r.haveCmd, r.reading}
	s.RTCBits, s.RTCCmd, s.RTCPos, s.RTCCmdByte, s.RTCOut, s.RTCData = r.bits, r.cmd, r.pos, r.cmdByte, r.out, r.data
	s.RTCStatus, s.RTCAlarm, s.RTCAdjust = [3]uint8{r.status1, r.status2, r.free}, r.alarm, r.adjust
	s.Regs = [12]uint16{m.powcnt1, m.exmemcnt, m.rcnt, m.keycnt9, m.keycnt7, m.exmemcnt7, m.powcnt2, m.bios7prot,
		m.dispstat9, m.dispstat7, uint16(m.postflg9) | uint16(m.postflg7)<<8, uint16(m.haltcnt)}
	s.Vcount, s.LineStart, s.NextLineEvent, s.HBlankDone, s.Capturing, s.DispCapCnt =
		m.vcount, m.lineStart, m.nextLineEvent, m.hblankDone, m.capturing, m.dispCapCnt

	var buf bytes.Buffer
	zw, _ := flate.NewWriter(&buf, flate.BestSpeed)
	if err := gob.NewEncoder(zw).Encode(s); err != nil {
		return nil, err
	}
	zw.Close()
	return buf.Bytes(), nil
}

func (m *Machine) LoadState(data []byte) error {
	raw, err := io.ReadAll(flate.NewReader(bytes.NewReader(data)))
	if err != nil {
		return errors.New("the save state is damaged")
	}
	s := &snapshot{}
	if err := gob.NewDecoder(bytes.NewReader(raw)).Decode(s); err != nil {
		return errors.New("the save state is damaged")
	}
	if len(s.MainRAM) != len(m.mainRAM) {
		return errors.New("the save state is damaged")
	}
	m.now = s.Now
	m.arm9.LoadState(s.ARM9)
	m.arm7.LoadState(s.ARM7)
	copy(m.mainRAM, s.MainRAM)
	copy(m.sharedWR, s.SharedWR)
	copy(m.arm7WRAM, s.ARM7WRAM)
	copy(m.pal, s.Pal)
	copy(m.oam, s.OAM)
	copy(m.cp15.itcm, s.ITCM)
	copy(m.cp15.dtcm, s.DTCM)
	for i := range m.vram.banks {
		copy(m.vram.banks[i], s.Banks[i])
	}
	m.vram.cnt, m.wramcnt, m.vram.wramcnt = s.VRAMCnt, s.WRAMCnt, s.WRAMCnt
	m.vram.remap()
	m.cp15.control, m.cp15.dtcmReg, m.cp15.itcmReg = s.CP15Control, s.DTCMReg, s.ITCMReg
	m.cp15.regs = s.CP15Regs
	if m.cp15.regs == nil {
		m.cp15.regs = map[uint32]uint32{}
	}
	m.cp15.update()
	m.irq9.ime, m.irq7.ime, m.irq9.ie, m.irq7.ie, m.irq9.ifl, m.irq7.ifl = s.IME[0], s.IME[1], s.IE[0], s.IE[1], s.IF[0], s.IF[1]
	for i := 0; i < 4; i++ {
		for k, t := range [2]*timer{&m.timers9[i], &m.timers7[i]} {
			ts := s.Timers[k][i]
			t.reload, t.ctrl, t.base, t.start, t.overflowAt = ts.Reload, ts.Ctrl, ts.Base, ts.Start, ts.OverflowAt
		}
		for k, d := range [2]*dma{&m.dma9[i], &m.dma7[i]} {
			ds := s.DMA[k][i]
			d.sad, d.dad, d.cnt, d.src, d.dst, d.left = ds.SAD, ds.DAD, ds.CNT, ds.Src, ds.Dst, ds.Left
		}
	}
	m.dmaFill = s.DMAFill
	m.ipc.sync9, m.ipc.sync7, m.ipc.cnt9, m.ipc.cnt7 = s.Sync9, s.Sync7, s.Cnt9, s.Cnt7
	m.ipc.to7 = fifo{s.To7.Data, s.To7.N, s.To7.At}
	m.ipc.to9 = fifo{s.To9.Data, s.To9.N, s.To9.At}
	m.ipc.last9, m.ipc.last7 = s.Last9, s.Last7
	m.math = mathUnit{s.DivCnt, s.Numer, s.Denom, s.Quot, s.Rem, s.SqrtCnt, s.SqrtP, s.SqrtR}
	for k, e := range [2]*engine2D{m.gpuA, m.gpuB} {
		es := s.Engines[k]
		e.dispcnt, e.bgcnt, e.hofs, e.vofs = es.Dispcnt, es.BGCnt, es.HOfs, es.VOfs
		e.pa, e.pb, e.pc, e.pd = es.PA, es.PB, es.PC, es.PD
		e.refX, e.refY, e.curX, e.curY = es.RefX, es.RefY, es.CurX, es.CurY
		e.win0h, e.win1h, e.win0v, e.win1v, e.winin, e.winout = es.Win0H, es.Win1H, es.Win0V, es.Win1V, es.WinIn, es.WinOut
		e.mosaic, e.bldcnt, e.bldalpha, e.bldy, e.masterBright = es.Mosaic, es.BldCnt, es.BldAlpha, es.BldY, es.MBri
	}
	if err := m.gpu3d.loadState(s.GPU3D); err != nil {
		return err
	}
	for i := range m.spu.ch {
		c, cs := &m.spu.ch[i], s.Chans[i]
		c.cnt, c.sad, c.tmr, c.pnt, c.length, c.pos, c.ticks, c.sample = cs.Cnt, cs.SAD, cs.Tmr, cs.Pnt, cs.Length, cs.Pos, cs.Ticks, cs.Sample
		c.adpcmVal, c.loopVal, c.adpcmIdx, c.loopIdx, c.lfsr = cs.ADPCMVal, cs.LoopVal, cs.ADPCMIdx, cs.LoopIdx, cs.LFSR
	}
	for i := 0; i < 2; i++ {
		m.spu.capt[i].cnt, m.spu.capt[i].dad, m.spu.capt[i].len = s.CaptCnt[i], s.CaptDAD[i], s.CaptLen[i]
	}
	m.spu.cnt, m.spu.bias, m.spu.at, m.spu.frac = s.SndCnt, s.SndBias, s.SndAt, s.SndFrac
	m.spu.out = nil
	c := m.cart
	c.cmd, c.romctrl, c.auxspicnt, c.spiData, c.cmdByte = s.CartCmd, s.ROMCtrl, s.AuxSPICnt, s.AuxSPIData, s.CartCmdByte
	c.addr, c.wordsLeft, c.doneAt = s.CartAddr, s.CartWords, s.CartDoneAt
	b := c.backup
	if s.Backup != nil {
		b.mem = append([]byte(nil), s.Backup...)
		b.dirty = true
	}
	b.addrBytes, b.pos, b.cmd, b.addr, b.wel = s.BackupAddrBytes, s.BackupPos, s.BackupCmd, s.BackupAddr, s.BackupWEL
	p := m.spi
	p.cnt, p.data, p.pos, p.dev = s.SPICnt, s.SPIData, s.SPIPos, s.SPIDev
	p.touch.cmd, p.touch.val, p.pm.index, p.pm.regs = s.TouchCmd, s.TouchVal, s.PMIndex, s.PMRegs
	if len(s.Firmware) == len(p.fw.image) {
		copy(p.fw.image, s.Firmware)
	}
	p.fw.cmd, p.fw.addr, p.fw.wel = s.FWCmd, s.FWAddr, s.FWWEL
	r := &m.rtc
	r.reg = s.RTCReg
	r.cs, r.sck, r.haveCmd, r.reading = s.RTCFlags[0], s.RTCFlags[1], s.RTCFlags[2], s.RTCFlags[3]
	r.bits, r.cmd, r.pos, r.cmdByte, r.out, r.data = s.RTCBits, s.RTCCmd, s.RTCPos, s.RTCCmdByte, s.RTCOut, s.RTCData
	r.status1, r.status2, r.free, r.alarm, r.adjust = s.RTCStatus[0], s.RTCStatus[1], s.RTCStatus[2], s.RTCAlarm, s.RTCAdjust
	g := s.Regs
	m.powcnt1, m.exmemcnt, m.rcnt, m.keycnt9, m.keycnt7, m.exmemcnt7, m.powcnt2, m.bios7prot = g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]
	m.dispstat9, m.dispstat7, m.postflg9, m.postflg7, m.haltcnt = g[8], g[9], uint8(g[10]), uint8(g[10]>>8), uint8(g[11])
	m.vcount, m.lineStart, m.nextLineEvent, m.hblankDone, m.capturing, m.dispCapCnt =
		s.Vcount, s.LineStart, s.NextLineEvent, s.HBlankDone, s.Capturing, s.DispCapCnt
	m.irq9.update()
	m.irq7.update()
	m.arm9.Halted, m.arm7.Halted = s.ARM9.Halted, s.ARM7.Halted
	return nil
}
