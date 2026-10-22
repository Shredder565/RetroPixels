package x11

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Display is a parsed X display name such as ":0", "unix:1.0",
// "localhost:10.0" or "tcp/host:0".
type Display struct {
	Protocol string // "unix" or "tcp"
	Host     string // empty for the local machine
	Socket   string // the Unix socket path for Protocol "unix"
	Number   int    // the display number
	Screen   int    // the screen number (".1"), 0 when not given
}

// ParseDisplay parses an X display name. An empty name means $DISPLAY.
func ParseDisplay(name string) (Display, error) {
	if name == "" {
		name = os.Getenv("DISPLAY")
	}
	if name == "" {
		return Display{}, errors.New("x11: no display ($DISPLAY is empty)")
	}
	var d Display
	rest := name
	if i := strings.Index(rest, "/"); i > 0 && !strings.HasPrefix(rest, "/") {
		switch p := rest[:i]; p {
		case "unix", "tcp", "inet", "inet6":
			if p != "unix" {
				p = "tcp"
			}
			d.Protocol = p
			rest = rest[i+1:]
		}
	}
	colon := strings.LastIndex(rest, ":")
	if colon < 0 {
		return Display{}, fmt.Errorf("x11: bad display name %q", name)
	}
	host, num := rest[:colon], rest[colon+1:]
	if dot := strings.Index(num, "."); dot >= 0 {
		s, err := strconv.Atoi(num[dot+1:])
		if err != nil || s < 0 {
			return Display{}, fmt.Errorf("x11: bad screen in display name %q", name)
		}
		d.Screen = s
		num = num[:dot]
	}
	n, err := strconv.Atoi(num)
	if err != nil || n < 0 {
		return Display{}, fmt.Errorf("x11: bad display number in %q", name)
	}
	d.Number = n
	switch {
	case strings.HasPrefix(host, "/"):
		// A socket path (XQuartz style "/path/org.xquartz:0"): the path is
		// the whole name up to the screen.
		d.Protocol = "unix"
		d.Socket = rest
		if dot := strings.LastIndex(rest, "."); dot > colon {
			d.Socket = rest[:dot]
		}
	case host == "" || host == "unix":
		if d.Protocol == "" {
			d.Protocol = "unix"
		}
		if d.Protocol == "tcp" {
			d.Host = "localhost"
		}
	default:
		if d.Protocol == "" {
			d.Protocol = "tcp"
		}
		d.Host = host
	}
	if d.Protocol == "unix" && d.Socket == "" {
		d.Socket = "/tmp/.X11-unix/X" + strconv.Itoa(d.Number)
	}
	return d, nil
}

// dial opens the transport for d: the socket path, then (on Linux) the
// abstract socket of the same name, which X servers also listen on.
func (d Display) dial(timeout time.Duration) (net.Conn, error) {
	if d.Protocol == "tcp" {
		return net.DialTimeout("tcp", net.JoinHostPort(d.Host, strconv.Itoa(6000+d.Number)), timeout)
	}
	c, err := net.DialTimeout("unix", d.Socket, timeout)
	if err == nil {
		return c, nil
	}
	if c2, err2 := net.DialTimeout("unix", "@"+d.Socket, timeout); err2 == nil {
		return c2, nil
	}
	return nil, err
}

// Xauthority families.
const (
	FamilyInternet  = 0
	FamilyInternet6 = 6
	FamilyLocal     = 256
	FamilyWild      = 65535
)

// AuthEntry is one entry of an Xauthority file.
type AuthEntry struct {
	Family  uint16
	Address string // host name for FamilyLocal, raw address bytes otherwise
	Number  string // display number, "" matches any
	Name    string // e.g. "MIT-MAGIC-COOKIE-1"
	Data    []byte
}

// AuthorityFile returns the Xauthority file path: $XAUTHORITY, else
// ~/.Xauthority.
func AuthorityFile() string {
	if f := os.Getenv("XAUTHORITY"); f != "" {
		return f
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return filepath.Join(home, ".Xauthority")
}

// ReadAuthority parses an Xauthority file's contents.
func ReadAuthority(r io.Reader) ([]AuthEntry, error) {
	var out []AuthEntry
	readField := func() ([]byte, error) {
		var n [2]byte
		if _, err := io.ReadFull(r, n[:]); err != nil {
			return nil, err
		}
		b := make([]byte, binary.BigEndian.Uint16(n[:]))
		if _, err := io.ReadFull(r, b); err != nil {
			return nil, io.ErrUnexpectedEOF
		}
		return b, nil
	}
	for {
		var fam [2]byte
		if _, err := io.ReadFull(r, fam[:]); err != nil {
			if err == io.EOF {
				return out, nil
			}
			return out, io.ErrUnexpectedEOF
		}
		var f [4][]byte
		for i := range f {
			b, err := readField()
			if err != nil {
				if err == io.EOF {
					err = io.ErrUnexpectedEOF
				}
				return out, err
			}
			f[i] = b
		}
		out = append(out, AuthEntry{
			Family:  binary.BigEndian.Uint16(fam[:]),
			Address: string(f[0]),
			Number:  string(f[1]),
			Name:    string(f[2]),
			Data:    f[3],
		})
	}
}

// WriteAuthority writes entries in the Xauthority file format.
func WriteAuthority(w io.Writer, entries []AuthEntry) error {
	var b []byte
	field := func(s []byte) {
		b = binary.BigEndian.AppendUint16(b, uint16(len(s)))
		b = append(b, s...)
	}
	for _, e := range entries {
		b = binary.BigEndian.AppendUint16(b, e.Family)
		field([]byte(e.Address))
		field([]byte(e.Number))
		field([]byte(e.Name))
		field(e.Data)
	}
	_, err := w.Write(b)
	return err
}

// findAuth picks the MIT-MAGIC-COOKIE-1 entry for d from the Xauthority
// file. It returns empty values when there is none, and the server then
// decides whether to let us in.
func findAuth(d Display, file string) (name string, data []byte) {
	if file == "" {
		return "", nil
	}
	f, err := os.Open(file)
	if err != nil {
		return "", nil
	}
	defer f.Close()
	entries, _ := ReadAuthority(f)
	host, _ := os.Hostname()
	local := d.Protocol == "unix" || d.Host == "localhost" || d.Host == "127.0.0.1" || d.Host == "::1"
	num := strconv.Itoa(d.Number)
	for _, e := range entries {
		if e.Name != "MIT-MAGIC-COOKIE-1" || (e.Number != "" && e.Number != num) {
			continue
		}
		switch {
		case e.Family == FamilyWild:
		case e.Family == FamilyLocal && local && e.Address == host:
		case e.Family == FamilyInternet && !local:
			ip := net.ParseIP(d.Host).To4()
			if ip == nil || string(ip) != e.Address {
				continue
			}
		case e.Family == FamilyInternet6 && !local:
			ip := net.ParseIP(d.Host).To16()
			if ip == nil || string(ip) != e.Address {
				continue
			}
		default:
			continue
		}
		return e.Name, e.Data
	}
	return "", nil
}
