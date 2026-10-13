package n64

// RDP save state: every register, tile descriptor, TMEM and any command
// still waiting for its last words.

import (
	"bytes"
	"encoding/binary"
	"errors"
)

func (d *RDP) marshalState() []byte {
	var b bytes.Buffer
	binary.Write(&b, binary.LittleEndian, &d.rdpRegs)
	return b.Bytes()
}

func (d *RDP) unmarshalState(b []byte) error {
	if len(b) != binary.Size(&d.rdpRegs) {
		return errors.New("rdp: bad state size")
	}
	if err := binary.Read(bytes.NewReader(b), binary.LittleEndian, &d.rdpRegs); err != nil {
		return err
	}
	d.dirty = true
	return nil
}
