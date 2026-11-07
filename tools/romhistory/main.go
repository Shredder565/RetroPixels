// Command romhistory makes internal/romdb/history.bin: for each MAME version
// from 0.171 on, every arcade machine's ROM files and their CRCs, read from
// MAME's source code (the ROM_START ... ROM_END listings and the GAME lines
// of src/mame). It needs a git repository of MAME with the version tags
// (mame0171 ... ) fetched; a shallow fetch of each tag is enough:
//
//	git init mame && cd mame && git remote add origin https://github.com/mamedev/mame
//	for v in $(seq 171 289); do git fetch --depth 1 origin tag mame0$v --no-tags; done
//	go run ./tools/romhistory -repo ../mame -from 171 -to 289
//
// -check <listxml file> compares one version with MAME's own -listxml.
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/xml"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"retropixels/internal/romdb"
)

func main() {
	repo := flag.String("repo", "", "MAME git repository with the version tags fetched")
	from := flag.Int("from", 171, "first version (0.xxx)")
	to := flag.Int("to", 289, "last version")
	out := flag.String("out", "internal/romdb/history.bin", "output file")
	check := flag.String("check", "", "a -listxml file to compare with")
	checkVer := flag.String("checkver", "", "the version of the -listxml file (0.264)")
	flag.Parse()
	if *check != "" {
		compare(*out, *check, *checkVer)
		return
	}
	g := newGit(*repo)
	b := newBuilder()
	for v := *from; v <= *to; v++ {
		tag := fmt.Sprintf("mame0%d", v)
		files, err := g.tree(tag)
		if err != nil {
			log.Printf("%s: %v (skipped)", tag, err)
			continue
		}
		vi := len(b.versions)
		b.versions = append(b.versions, fmt.Sprintf("0.%d", v))
		b.addVersion(vi, files, g)
		log.Printf("%s: %d files, %d machines so far", tag, len(files), len(b.machines))
	}
	db := b.finish()
	f, err := os.Create(*out)
	if err != nil {
		log.Fatal(err)
	}
	if err := db.Encode(f); err != nil {
		log.Fatal(err)
	}
	f.Close()
	st, _ := os.Stat(*out)
	n := 0
	for _, m := range db.Machines {
		n += len(m.Roms)
	}
	log.Printf("wrote %s: %d versions, %d machines, %d rom rows, %d bytes", *out, len(db.Versions), len(db.Machines), n, st.Size())
}

// ---------------------------------------------------------------- git

type gitRepo struct {
	dir string
	in  io.WriteCloser
	out *bufio.Reader
}

func newGit(dir string) *gitRepo {
	cmd := exec.Command("git", "-C", dir, "cat-file", "--batch")
	in, _ := cmd.StdinPipe()
	o, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		log.Fatal(err)
	}
	return &gitRepo{dir: dir, in: in, out: bufio.NewReaderSize(o, 1<<20)}
}

type blob struct{ sha, path string }

func (g *gitRepo) tree(tag string) ([]blob, error) {
	out, err := exec.Command("git", "-C", g.dir, "ls-tree", "-r", tag, "--", "src/mame").Output()
	if err != nil {
		return nil, err
	}
	var l []blob
	for _, line := range strings.Split(string(out), "\n") {
		// "100644 blob <sha>\t<path>"
		tab := strings.IndexByte(line, '\t')
		if tab < 0 {
			continue
		}
		f := strings.Fields(line[:tab])
		p := line[tab+1:]
		if len(f) == 3 && f[1] == "blob" && (strings.HasSuffix(p, ".cpp") || strings.HasSuffix(p, ".h") || strings.HasSuffix(p, ".c") || strings.HasSuffix(p, ".hxx") || strings.HasSuffix(p, ".ipp")) {
			l = append(l, blob{f[2], p})
		}
	}
	return l, nil
}

func (g *gitRepo) read(sha string) []byte {
	fmt.Fprintln(g.in, sha)
	hdr, err := g.out.ReadString('\n')
	if err != nil {
		log.Fatal(err)
	}
	f := strings.Fields(hdr)
	if len(f) < 3 {
		log.Fatalf("cat-file: %q", hdr)
	}
	n, _ := strconv.Atoi(f[2])
	buf := make([]byte, n+1)
	if _, err := io.ReadFull(g.out, buf); err != nil {
		log.Fatal(err)
	}
	return buf[:n]
}

// ---------------------------------------------------------------- parsing

type game struct {
	name, parent, desc string
	biosRoot           bool
}

type parsed struct {
	defines   map[string]string
	romstarts map[string]string
	aliases   [][2]string // #define rom_a rom_b
	games     []game
}

// stripComments removes // and /* */ comments (keeping strings) and joins
// lines continued with a backslash.
func stripComments(src []byte) string {
	var b strings.Builder
	b.Grow(len(src))
	for i := 0; i < len(src); i++ {
		c := src[i]
		switch {
		case c == '"':
			j := i + 1
			for j < len(src) && src[j] != '"' && src[j] != '\n' {
				if src[j] == '\\' {
					j++
				}
				j++
			}
			if j >= len(src) {
				j = len(src) - 1
			}
			b.Write(src[i : j+1])
			i = j
		case c == '\'':
			j := i + 1
			for j < len(src) && src[j] != '\'' && src[j] != '\n' {
				if src[j] == '\\' {
					j++
				}
				j++
			}
			if j >= len(src) {
				j = len(src) - 1
			}
			b.Write(src[i : j+1])
			i = j
		case c == '/' && i+1 < len(src) && src[i+1] == '/':
			for i < len(src) && src[i] != '\n' {
				if src[i] == '\\' && i+1 < len(src) && src[i+1] == '\n' {
					break
				}
				i++
			}
			i--
		case c == '/' && i+1 < len(src) && src[i+1] == '*':
			i += 2
			for i+1 < len(src) && !(src[i] == '*' && src[i+1] == '/') {
				i++
			}
			i++
			b.WriteByte(' ')
		case c == '\\' && i+1 < len(src) && (src[i+1] == '\n' || src[i+1] == '\r'):
			// line continuation
			i++
			if src[i] == '\r' && i+1 < len(src) && src[i+1] == '\n' {
				i++
			}
			b.WriteByte(' ')
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

var (
	reDefine   = regexp.MustCompile(`(?m)^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)(\([^)]*\))?(.*)$`)
	reRomStart = regexp.MustCompile(`\bROM_START\s*\(\s*([a-z0-9_]+)\s*\)`)
	reRomEnd   = regexp.MustCompile(`\bROM_END\w*`)
	reAlias    = regexp.MustCompile(`(?m)^[ \t]*#[ \t]*define[ \t]+rom_([a-z0-9_]+)[ \t]+rom_([a-z0-9_]+)[ \t]*$`)
	reGame     = regexp.MustCompile(`(?m)^\s*GAMEL?\s*\(`)
	reIdent    = regexp.MustCompile(`[A-Za-z_][A-Za-z0-9_]{2,}`)
	reCRC      = regexp.MustCompile(`CRC\s*\(\s*([0-9a-fA-F]{8})\s*\)`)
	reQuoted   = regexp.MustCompile(`"([^"]*)"`)
	reSize     = regexp.MustCompile(`([0-9a-fA-FxX]+)\s*,\s*$`)
	reLoadBios = regexp.MustCompile(`LOAD\w*_BIOS\s*\(\s*(\d+)`)
	reRomBios  = regexp.MustCompile(`ROM_BIOS\s*\(\s*(\d+)\s*\)`)
	reSysBios  = regexp.MustCompile(`ROM_SYSTEM_BIOS\s*\(\s*(\d+)\s*,\s*"([^"]*)"`)
	reDefBios  = regexp.MustCompile(`ROM_DEFAULT_BIOS\s*\(\s*"([^"]*)"`)
	reDisk     = regexp.MustCompile(`\bDISK_IMAGE\w*\s*\(`)
)

func parse(src []byte) *parsed {
	s := stripComments(src)
	p := &parsed{defines: map[string]string{}, romstarts: map[string]string{}}
	for _, m := range reDefine.FindAllStringSubmatch(s, -1) {
		body := m[3]
		if strings.Contains(body, "CRC") || strings.Contains(body, "ROM_") || strings.Contains(body, "DISK_IMAGE") {
			p.defines[m[1]] = body
		}
	}
	for _, loc := range reRomStart.FindAllStringSubmatchIndex(s, -1) {
		name := s[loc[2]:loc[3]]
		rest := s[loc[1]:]
		e := reRomEnd.FindStringIndex(rest)
		if e == nil {
			continue
		}
		// a ROM_END_xxx macro may add files of its own
		p.romstarts[name] = rest[:e[1]]
	}
	for _, m := range reAlias.FindAllStringSubmatch(s, -1) {
		if _, ok := p.romstarts[m[1]]; !ok {
			p.aliases = append(p.aliases, [2]string{m[1], m[2]})
		}
	}
	for _, loc := range reGame.FindAllStringIndex(s, -1) {
		args, ok := callArgs(s, loc[1])
		if !ok || len(args) < 10 {
			continue
		}
		g := game{name: strings.TrimSpace(args[1]), parent: strings.TrimSpace(args[2])}
		if g.parent == "0" {
			g.parent = ""
		}
		if q := reQuoted.FindStringSubmatch(args[9]); q != nil {
			g.desc = q[1]
		}
		if len(args) > 10 && strings.Contains(args[10], "MACHINE_IS_BIOS_ROOT") {
			g.biosRoot = true
		}
		if isName(g.name) && (g.parent == "" || isName(g.parent)) {
			p.games = append(p.games, g)
		}
	}
	return p
}

func isName(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_') {
			return false
		}
	}
	return true
}

// callArgs splits the arguments of a call whose "(" is just before start.
func callArgs(s string, start int) ([]string, bool) {
	depth := 1
	var args []string
	cur := start
	for i := start; i < len(s); i++ {
		switch s[i] {
		case '"':
			j := i + 1
			for j < len(s) && s[j] != '"' {
				if s[j] == '\\' {
					j++
				}
				j++
			}
			i = j
		case '(':
			depth++
		case ')':
			depth--
			if depth == 0 {
				args = append(args, s[cur:i])
				return args, true
			}
		case ',':
			if depth == 1 {
				args = append(args, s[cur:i])
				cur = i + 1
			}
		}
		if i-start > 4000 {
			return nil, false
		}
	}
	return nil, false
}

// expand replaces each use of a ROM macro with its body (parameters aren't
// substituted: a parameter is never a CRC literal, and the call's own
// arguments, which follow, still give the CRCs passed in).
func expand(text string, defs func(string) (string, bool), depth int) string {
	if depth > 6 {
		return text
	}
	return reIdent.ReplaceAllStringFunc(text, func(id string) string {
		if body, ok := defs(id); ok {
			return " " + expand(body, defs, depth+1) + " " + id
		}
		return id
	})
}

type romRow struct {
	name string
	crc  uint32
	size uint32
	bios int
	bad  bool
}

type romList struct {
	roms        []romRow
	bios        []string
	defaultBios string
	chd         bool
}

func extract(text string) romList {
	var l romList
	for _, m := range reSysBios.FindAllStringSubmatch(text, -1) {
		n, _ := strconv.Atoi(m[1])
		for len(l.bios) <= n {
			l.bios = append(l.bios, "")
		}
		l.bios[n] = m[2]
	}
	if m := reDefBios.FindStringSubmatch(text); m != nil {
		l.defaultBios = m[1]
	}
	l.chd = reDisk.MatchString(text)
	locs := reCRC.FindAllStringSubmatchIndex(text, -1)
	prevEnd := 0
	seen := map[[2]uint32]bool{}
	for k, loc := range locs {
		before := text[prevEnd:loc[0]]
		after := text[loc[1]:]
		if k+1 < len(locs) {
			after = text[loc[1]:locs[k+1][0]]
		}
		// the name: the last quoted string before the CRC
		qs := reQuoted.FindAllStringSubmatch(before, -1)
		if qs == nil {
			prevEnd = loc[1]
			continue
		}
		name := qs[len(qs)-1][1]
		c, _ := strconv.ParseUint(text[loc[2]:loc[3]], 16, 32)
		r := romRow{name: strings.ToLower(name), crc: uint32(c), bios: -1}
		if sm := reSize.FindStringSubmatch(before); sm != nil {
			if v, err := strconv.ParseUint(strings.TrimPrefix(strings.ToLower(sm[1]), "0x"), 16, 32); err == nil {
				r.size = uint32(v)
			}
		}
		if bm := reLoadBios.FindAllStringSubmatch(before, -1); bm != nil {
			r.bios, _ = strconv.Atoi(bm[len(bm)-1][1])
		}
		// ROMX_LOAD(..., CRC(...) SHA1(...), ROM_BIOS(n)): up to the next name
		tail := after
		if q := strings.IndexByte(tail, '"'); q >= 0 {
			tail = tail[:q]
		}
		if bm := reRomBios.FindStringSubmatch(tail); bm != nil {
			r.bios, _ = strconv.Atoi(bm[1])
		}
		r.bad = strings.Contains(tail, "BAD_DUMP")
		key := [2]uint32{r.crc, uint32(r.bios + 1)}
		if !seen[key] && r.crc != 0 {
			seen[key] = true
			l.roms = append(l.roms, r)
		}
		prevEnd = loc[1]
	}
	return l
}

// ---------------------------------------------------------------- building

type mrec struct {
	desc, parent string
	biosRoot     bool
	from, to     int
	bios         []string
	defaultBios  string
	chd          bool
	roms         map[romRow][]int // versions
}

type builder struct {
	versions []string
	machines map[string]*mrec
	cache    map[string]*parsed
	exCache  map[[20]byte]romList
}

func newBuilder() *builder {
	return &builder{machines: map[string]*mrec{}, cache: map[string]*parsed{}, exCache: map[[20]byte]romList{}}
}

func (b *builder) addVersion(vi int, files []blob, g *gitRepo) {
	var all []*parsed
	globalDefs := map[string]string{}
	globalRS := map[string]string{}
	type owner struct{ p *parsed }
	for _, f := range files {
		p := b.cache[f.sha]
		if p == nil {
			p = parse(g.read(f.sha))
			b.cache[f.sha] = p
		}
		all = append(all, p)
		for k, v := range p.defines {
			if _, ok := globalDefs[k]; !ok {
				globalDefs[k] = v
			}
		}
		for k, v := range p.romstarts {
			if _, ok := globalRS[k]; !ok {
				globalRS[k] = v
			}
		}
	}
	for _, p := range all {
		for _, a := range p.aliases {
			if t, ok := p.romstarts[a[1]]; ok {
				p.romstarts[a[0]] = t
			} else if t, ok := globalRS[a[1]]; ok {
				p.romstarts[a[0]] = t
			}
		}
	}
	for _, p := range all {
		defs := func(id string) (string, bool) {
			if v, ok := p.defines[id]; ok {
				return v, true
			}
			v, ok := globalDefs[id]
			return v, ok
		}
		for _, gm := range p.games {
			text, ok := p.romstarts[gm.name]
			if !ok {
				text, ok = globalRS[gm.name]
			}
			if !ok {
				continue
			}
			full := expand(text, defs, 0)
			key := sha1.Sum([]byte(full))
			rl, ok := b.exCache[key]
			if !ok {
				rl = extract(full)
				b.exCache[key] = rl
			}
			m := b.machines[gm.name]
			if m == nil {
				m = &mrec{from: vi, roms: map[romRow][]int{}}
				b.machines[gm.name] = m
			}
			if m.to == vi && len(m.roms) > 0 && m.from != vi {
				continue // listed twice in one version
			}
			m.to = vi
			m.desc, m.parent, m.biosRoot = gm.desc, gm.parent, gm.biosRoot
			m.bios, m.defaultBios, m.chd = rl.bios, rl.defaultBios, rl.chd
			for _, r := range rl.roms {
				vs := m.roms[r]
				if len(vs) == 0 || vs[len(vs)-1] != vi {
					m.roms[r] = append(vs, vi)
				}
			}
		}
	}
}

func (b *builder) finish() *romdb.DB {
	var ms []*romdb.Machine
	for name, r := range b.machines {
		m := &romdb.Machine{Name: name, Desc: r.desc, Parent: r.parent, BiosRoot: r.biosRoot, From: r.from, To: r.to,
			Bios: r.bios, DefaultBios: r.defaultBios, CHD: r.chd}
		for row, vs := range r.roms {
			// contiguous ranges
			start := vs[0]
			for i := 1; i <= len(vs); i++ {
				if i == len(vs) || vs[i] != vs[i-1]+1 {
					m.Roms = append(m.Roms, romdb.Rom{Name: row.name, CRC: row.crc, Size: row.size, Bios: row.bios, Bad: row.bad, From: start, To: vs[i-1]})
					if i < len(vs) {
						start = vs[i]
					}
				}
			}
		}
		sort.Slice(m.Roms, func(a, c int) bool {
			x, y := m.Roms[a], m.Roms[c]
			if x.Name != y.Name {
				return x.Name < y.Name
			}
			return x.From < y.From
		})
		ms = append(ms, m)
	}
	return romdb.New(b.versions, ms)
}

// ---------------------------------------------------------------- checking

type xmlMame struct {
	Machines []struct {
		Name     string `xml:"name,attr"`
		IsBios   string `xml:"isbios,attr"`
		IsDevice string `xml:"isdevice,attr"`
		RomOf    string `xml:"romof,attr"`
		Roms     []struct {
			Name   string `xml:"name,attr"`
			CRC    string `xml:"crc,attr"`
			Merge  string `xml:"merge,attr"`
			Bios   string `xml:"bios,attr"`
			Status string `xml:"status,attr"`
		} `xml:"rom"`
	} `xml:"machine"`
}

func compare(dbPath, listxml, ver string) {
	data, err := os.ReadFile(dbPath)
	if err != nil {
		log.Fatal(err)
	}
	db, err := romdb.Decode(data)
	if err != nil {
		log.Fatal(err)
	}
	vi := db.Version(ver)
	if vi < 0 {
		log.Fatalf("version %s isn't in the history", ver)
	}
	f, err := os.Open(listxml)
	if err != nil {
		log.Fatal(err)
	}
	var x xmlMame
	if err := xml.NewDecoder(f).Decode(&x); err != nil {
		log.Fatal(err)
	}
	total, exact, missingMachine, diff := 0, 0, 0, 0
	shown := 0
	for _, xm := range x.Machines {
		if xm.IsDevice == "yes" {
			continue
		}
		want := map[uint32]bool{}
		for _, r := range xm.Roms {
			if r.CRC == "" || r.Status == "nodump" {
				continue
			}
			c, _ := strconv.ParseUint(r.CRC, 16, 32)
			want[uint32(c)] = true
		}
		if len(want) == 0 {
			continue
		}
		total++
		m := db.Machine(xm.Name)
		if m == nil || !m.In(vi) {
			missingMachine++
			if shown < 30 {
				fmt.Println("missing machine:", xm.Name)
				shown++
			}
			continue
		}
		have := map[uint32]bool{}
		for _, r := range RomsWithBios(db, m, vi) {
			have[r] = true
		}
		var lack, extra []string
		for c := range want {
			if !have[c] {
				lack = append(lack, fmt.Sprintf("%08x", c))
			}
		}
		for c := range have {
			if !want[c] {
				extra = append(extra, fmt.Sprintf("%08x", c))
			}
		}
		if len(lack) == 0 && len(extra) == 0 {
			exact++
		} else {
			diff++
			if shown < 60 {
				fmt.Printf("%s: lacks %v extra %v\n", xm.Name, lack, extra)
				shown++
			}
		}
	}
	fmt.Printf("machines with ROMs: %d, exact %d, different %d, not in history %d\n", total, exact, diff, missingMachine)
}

// RomsWithBios: every CRC MAME's -listxml lists for the machine: its own
// ROMs and, all the way up, the ROMs of a BIOS it runs on.
func RomsWithBios(db *romdb.DB, m *romdb.Machine, v int) []uint32 {
	var out []uint32
	seen := map[string]bool{}
	for m != nil && !seen[m.Name] {
		seen[m.Name] = true
		for _, r := range m.Roms {
			if r.In(v) {
				out = append(out, r.CRC)
			}
		}
		p := db.Machine(m.Parent)
		if p == nil {
			break
		}
		// a clone's own list already has its parent's files; only a BIOS
		// parent adds files
		if !p.BiosRoot {
			// but the parent's BIOS still counts
			for p != nil && !p.BiosRoot && !seen[p.Name] {
				seen[p.Name] = true
				p = db.Machine(p.Parent)
			}
			if p == nil {
				break
			}
		}
		m = p
	}
	return out
}
