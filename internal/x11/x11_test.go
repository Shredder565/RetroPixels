package x11

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func TestParseDisplay(t *testing.T) {
	tests := []struct {
		in   string
		want Display
	}{
		{":0", Display{Protocol: "unix", Socket: "/tmp/.X11-unix/X0"}},
		{":1.2", Display{Protocol: "unix", Socket: "/tmp/.X11-unix/X1", Number: 1, Screen: 2}},
		{"unix:3", Display{Protocol: "unix", Socket: "/tmp/.X11-unix/X3", Number: 3}},
		{"unix/:4", Display{Protocol: "unix", Socket: "/tmp/.X11-unix/X4", Number: 4}},
		{"localhost:10.0", Display{Protocol: "tcp", Host: "localhost", Number: 10}},
		{"tcp/box:2", Display{Protocol: "tcp", Host: "box", Number: 2}},
		{"tcp/:5", Display{Protocol: "tcp", Host: "localhost", Number: 5}},
		{"/private/tmp/launch-x/org.xquartz:0", Display{Protocol: "unix", Socket: "/private/tmp/launch-x/org.xquartz:0"}},
		{"/private/tmp/launch-x/org.xquartz:0.1", Display{Protocol: "unix", Socket: "/private/tmp/launch-x/org.xquartz:0", Screen: 1}},
	}
	for _, tt := range tests {
		got, err := ParseDisplay(tt.in)
		if err != nil || got != tt.want {
			t.Errorf("ParseDisplay(%q) = %+v, %v; want %+v", tt.in, got, err, tt.want)
		}
	}
	for _, bad := range []string{"0", ":x", ":1.y", "host:-1"} {
		if _, err := ParseDisplay(bad); err == nil {
			t.Errorf("ParseDisplay(%q) succeeded", bad)
		}
	}
	t.Setenv("DISPLAY", ":7")
	if d, err := ParseDisplay(""); err != nil || d.Number != 7 {
		t.Errorf("ParseDisplay from $DISPLAY = %+v, %v", d, err)
	}
	t.Setenv("DISPLAY", "")
	if _, err := ParseDisplay(""); err == nil {
		t.Error("empty $DISPLAY accepted")
	}
}

func TestAuthority(t *testing.T) {
	host, _ := os.Hostname()
	entries := []AuthEntry{
		{Family: FamilyLocal, Address: "otherhost", Number: "0", Name: "MIT-MAGIC-COOKIE-1", Data: []byte("wrong-host-cooki")},
		{Family: FamilyLocal, Address: host, Number: "1", Name: "MIT-MAGIC-COOKIE-1", Data: []byte("display-1-cookie")},
		{Family: FamilyLocal, Address: host, Number: "0", Name: "XDM-AUTHORIZATION-1", Data: []byte("xdm")},
		{Family: FamilyLocal, Address: host, Number: "0", Name: "MIT-MAGIC-COOKIE-1", Data: []byte("display-0-cookie")},
		{Family: FamilyInternet, Address: string([]byte{10, 0, 0, 5}), Number: "0", Name: "MIT-MAGIC-COOKIE-1", Data: []byte("ip-cookie")},
		{Family: FamilyWild, Address: "", Number: "", Name: "MIT-MAGIC-COOKIE-1", Data: []byte("wild")},
	}
	var buf bytes.Buffer
	if err := WriteAuthority(&buf, entries); err != nil {
		t.Fatal(err)
	}
	got, err := ReadAuthority(bytes.NewReader(buf.Bytes()))
	if err != nil || len(got) != len(entries) {
		t.Fatalf("ReadAuthority: %d entries, %v", len(got), err)
	}
	for i := range got {
		if got[i].Family != entries[i].Family || got[i].Address != entries[i].Address ||
			got[i].Number != entries[i].Number || got[i].Name != entries[i].Name || !bytes.Equal(got[i].Data, entries[i].Data) {
			t.Errorf("entry %d = %+v, want %+v", i, got[i], entries[i])
		}
	}
	if _, err := ReadAuthority(bytes.NewReader(buf.Bytes()[:buf.Len()-3])); err == nil {
		t.Error("truncated file accepted")
	}

	file := filepath.Join(t.TempDir(), "Xauthority")
	if err := os.WriteFile(file, buf.Bytes(), 0o600); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		display, want string
	}{
		{":0", "display-0-cookie"},
		{":1", "display-1-cookie"},
		{":9", "wild"},
		{"10.0.0.5:0", "ip-cookie"},
		{"10.0.0.6:0", "wild"},
	}
	for _, c := range cases {
		d, _ := ParseDisplay(c.display)
		if name, data := findAuth(d, file); name != "MIT-MAGIC-COOKIE-1" || string(data) != c.want {
			t.Errorf("findAuth(%s) = %q %q, want %q", c.display, name, data, c.want)
		}
	}
	d, _ := ParseDisplay(":0")
	if name, _ := findAuth(d, filepath.Join(t.TempDir(), "missing")); name != "" {
		t.Error("missing file gave a cookie")
	}
	t.Setenv("XAUTHORITY", file)
	if AuthorityFile() != file {
		t.Error("AuthorityFile ignores $XAUTHORITY")
	}
}

func TestFullSeq(t *testing.T) {
	tests := []struct {
		sent uint64
		s16  uint16
		want uint64
	}{
		{5, 5, 5},
		{5, 3, 3},
		{0x10002, 0x0002, 0x10002},
		{0x10002, 0xfffe, 0xfffe},
		{0x2ffff, 0xffff, 0x2ffff},
		{0x30000, 0xffff, 0x2ffff},
	}
	for _, tt := range tests {
		if got := fullSeq(tt.sent, tt.s16); got != tt.want {
			t.Errorf("fullSeq(%#x, %#x) = %#x, want %#x", tt.sent, tt.s16, got, tt.want)
		}
	}
}

func TestParseEvents(t *testing.T) {
	raw := ClientMessageEventBytes(0x400001, 300, [5]uint32{1, 2, 3, 4, 5})
	raw[0] |= 0x80
	e, ok := parseEvent(raw[:]).(*ClientMessageEvent)
	if !ok || !e.Synthetic || e.Code() != ClientMessage || e.Format != 32 || e.Window != 0x400001 || e.Type != 300 ||
		e.Data32() != [5]uint32{1, 2, 3, 4, 5} {
		t.Fatalf("ClientMessage = %+v", e)
	}

	b := make([]byte, 32)
	b[0] = ConfigureNotify
	le := binary.LittleEndian
	le.PutUint32(b[4:], 1)
	le.PutUint32(b[8:], 2)
	le.PutUint32(b[12:], 3)
	le.PutUint16(b[16:], uint16(0xfff6)) // -10
	le.PutUint16(b[18:], 20)
	le.PutUint16(b[20:], 640)
	le.PutUint16(b[22:], 480)
	cn, ok := parseEvent(b).(*ConfigureNotifyEvent)
	if !ok || cn.Synthetic || cn.Event != 1 || cn.Window != 2 || cn.Above != 3 || cn.X != -10 || cn.Y != 20 ||
		cn.Width != 640 || cn.Height != 480 {
		t.Fatalf("ConfigureNotify = %+v", cn)
	}

	b = make([]byte, 32)
	b[0] = 99
	if r, ok := parseEvent(b).(*RawEvent); !ok || r.Code() != 99 {
		t.Fatalf("unknown event = %#v", r)
	}

	b = make([]byte, 32)
	b[1] = BadWindow
	le.PutUint32(b[4:], 0x1234)
	b[10] = opMapWindow
	err := parseError(b)
	if err.Error() != "x11: BadWindow in MapWindow (value 0x1234)" || !IsError(err, BadWindow) || IsError(err, BadMatch) {
		t.Errorf("error = %v", err)
	}
}

func TestRequestEncoding(t *testing.T) {
	a := &WindowAttributes{Mask: CWEventMask | CWBackPixel | CWOverrideRedirect, BackPixel: 9, OverrideRedirect: true, EventMask: 0x20000}
	r := newRequest(opChangeWindowAttributes, 0)
	a.encode(r)
	want := []uint32{CWEventMask | CWBackPixel | CWOverrideRedirect, 9, 1, 0x20000}
	for i, w := range want {
		if got := binary.LittleEndian.Uint32(r.b[4+4*i:]); got != w {
			t.Errorf("value %d = %#x, want %#x", i, got, w)
		}
	}
	if FormatWindow(0x400001) != "0x400001" {
		t.Error(FormatWindow(0x400001))
	}
}
