package nds

import "time"

// rtc is the Seiko real-time clock on the ARM7's RTC register (0x04000138),
// driven bit by bit. It tells the time from the computer's clock.
type rtc struct {
	now func() time.Time

	reg     uint16
	cs, sck bool
	haveCmd bool
	bits    int
	cmdByte uint8
	cmd     int
	reading bool
	data    []byte
	pos     int // bit position in data
	out     uint8
	status1 uint8
	status2 uint8
	free    uint8
	alarm   [2][3]uint8
	adjust  uint8
}

func bcd(v int) byte { return byte(v/10<<4 | v%10) }

func (r *rtc) read() uint16 {
	v := r.reg &^ 1
	if r.reg&0x10 == 0 { // data line is an input: the clock drives it
		v |= uint16(r.out)
	} else {
		v |= r.reg & 1
	}
	return v
}

func (r *rtc) write(v, mask uint16) {
	r.reg = r.reg&^mask | v&mask
	cs, sck := r.reg&4 != 0, r.reg&2 != 0
	if !cs {
		if r.cs && r.haveCmd && !r.reading {
			r.applyWrite()
		}
		r.cs, r.sck = false, sck
		r.haveCmd, r.bits, r.cmdByte, r.pos = false, 0, 0, 0
		return
	}
	rising := !r.sck && sck
	r.cs, r.sck = true, sck
	if !rising {
		return
	}
	bit := uint8(r.reg & 1)
	if !r.haveCmd {
		r.cmdByte = r.cmdByte<<1 | bit
		r.bits++
		if r.bits == 8 {
			r.command(r.cmdByte)
		}
		return
	}
	if r.reading {
		i := r.pos / 8
		if i < len(r.data) {
			r.out = r.data[i] >> (r.pos % 8) & 1
		} else {
			r.out = 0
		}
		r.pos++
		return
	}
	i := r.pos / 8
	for len(r.data) <= i {
		r.data = append(r.data, 0)
	}
	r.data[i] |= bit << (r.pos % 8)
	r.pos++
}

func reverse8(b uint8) uint8 {
	var r uint8
	for i := 0; i < 8; i++ {
		r = r<<1 | b>>i&1
	}
	return r
}

func (r *rtc) command(c uint8) {
	if c>>4 != 6 && c&0xf == 6 {
		c = reverse8(c)
	}
	r.haveCmd = true
	r.cmd = int(c >> 1 & 7)
	r.reading = c&1 != 0
	r.pos = 0
	r.data = nil
	if !r.reading {
		return
	}
	t := r.now()
	hour := func() byte {
		h := t.Hour()
		v := bcd(h)
		if r.status1&2 == 0 { // 12-hour mode
			v = bcd(h % 12)
		}
		if h >= 12 {
			v |= 0x40
		}
		return v
	}
	switch r.cmd {
	case 0:
		r.data = []byte{r.status1}
		r.status1 &^= 0xf0 // power and interrupt flags clear when read
	case 1:
		r.data = []byte{r.status2}
	case 2:
		r.data = []byte{bcd(t.Year() % 100), bcd(int(t.Month())), bcd(t.Day()), byte(t.Weekday()), hour(), bcd(t.Minute()), bcd(t.Second())}
	case 3:
		r.data = []byte{hour(), bcd(t.Minute()), bcd(t.Second())}
	case 4:
		r.data = r.alarm[0][:]
	case 5:
		r.data = r.alarm[1][:]
	case 6:
		r.data = []byte{r.adjust}
	case 7:
		r.data = []byte{r.free}
	}
}

func (r *rtc) applyWrite() {
	if len(r.data) == 0 {
		return
	}
	switch r.cmd {
	case 0:
		r.status1 = r.status1&0xf1 | r.data[0]&0x0e
	case 1:
		r.status2 = r.data[0]
	case 4:
		copy(r.alarm[0][:], r.data)
	case 5:
		copy(r.alarm[1][:], r.data)
	case 6:
		r.adjust = r.data[0]
	case 7:
		r.free = r.data[0]
	}
	// setting the time (2, 3) is ignored: the clock follows the computer's
}
