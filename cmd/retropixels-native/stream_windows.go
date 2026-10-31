package main

// Streaming to YouTube (Stream menu). ffmpeg grabs this window's game area on
// screen (RetroPixels' own picture, or a GameCube / Wii U / arcade game
// docked in or floating over it) with the PC's sound (internal/stream) and
// sends it to YouTube with the player's stream key, from YouTube Studio. The
// stream's live chat opens in a window of its own beside this one (YouTube's
// pop-out chat, in Microsoft Edge's app mode, signed in as in Edge). Moving
// or resizing the window restarts the stream for the new area.

import (
	"encoding/base64"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/ffmpeg"
	"retropixels/internal/stream"
)

// Menu commands 1940–1959 (and the settings window's controls).
const (
	cmdStreamStart    = 1940
	cmdStreamStop     = 1941
	cmdStreamChat     = 1942
	cmdStreamSettings = 1943
	cmdStreamStudio   = 1944
	cmdStreamFirst    = 1940
	cmdStreamLast     = 1949

	idStKey     = 1950
	idStChat    = 1951
	idStQuality = 1952
	idStAuto    = 1953
	idStSave    = 1954
	idStCancel  = 1955
	idStStudio  = 1956
	esPassword  = 0x0020
)

const studioURL = "https://www.youtube.com/live_dashboard" // opens YouTube Studio's Go live page

type streamState struct {
	run     *streamRun
	dlg     *streamDlg
	moveGen int // counts window moves and resizes while streaming
}

type streamRun struct {
	cmd     *exec.Cmd
	lb      *stream.Loopback
	pw      *io.PipeWriter
	out     *tailBuffer
	key     string
	region  stream.Region
	started time.Time
	exited  chan struct{}
	restart bool // started again for the window's new place once it has ended
	stopped bool // Stop streaming was asked
	noSound string
}

// tailBuffer keeps the end of what ffmpeg printed.
type tailBuffer struct {
	mu  sync.Mutex
	buf []byte
}

func (t *tailBuffer) Write(p []byte) (int, error) {
	t.mu.Lock()
	t.buf = append(t.buf, p...)
	if len(t.buf) > 4096 {
		t.buf = append([]byte(nil), t.buf[len(t.buf)-4096:]...)
	}
	t.mu.Unlock()
	return len(p), nil
}

func (t *tailBuffer) String() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return strings.TrimSpace(string(t.buf))
}

// ---------------- menu ----------------

func (a *app) addStreamMenu() {
	m := call(pCreatePopupMenu)
	appendMenu(m, mfString, cmdStreamStart, "&Start streaming to YouTube\tCtrl+F9")
	appendMenu(m, mfString, cmdStreamStop, "S&top streaming")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdStreamChat, "Open the live &chat")
	appendMenu(m, mfString, cmdStreamSettings, "Stream &settings (key, chat, quality)…")
	appendMenu(m, mfString, cmdStreamStudio, "Open YouTube Studio")
	appendMenu(a.menu, mfPopup, m, "&Stream")
}

func (a *app) streamUpdateMenus() {
	on := a.stream.run != nil
	enableItem(a.menu, cmdStreamStart, !on)
	enableItem(a.menu, cmdStreamStop, on)
}

func (a *app) streamCommand(id int) {
	switch id {
	case cmdStreamStart:
		a.streamStart(false)
	case cmdStreamStop:
		a.streamStop(false)
	case cmdStreamChat:
		a.streamOpenChat(false)
	case cmdStreamSettings:
		a.openStreamSettings()
	case cmdStreamStudio:
		shellOpen(studioURL)
	}
}

// streamToggle is Ctrl+F9.
func (a *app) streamToggle() {
	if a.stream.run != nil {
		a.streamStop(false)
	} else {
		a.streamStart(false)
	}
}

func (a *app) streamTitle() string {
	r := a.stream.run
	if r == nil {
		return ""
	}
	d := int(time.Since(r.started).Seconds())
	return fmt.Sprintf(" - ● LIVE %d:%02d:%02d", d/3600, d/60%60, d%60)
}

// ---------------- the stream key ----------------

var (
	modCrypt32             = syscall.NewLazyDLL("crypt32.dll")
	pCryptProtectData      = modCrypt32.NewProc("CryptProtectData")
	pCryptUnprotectData    = modCrypt32.NewProc("CryptUnprotectData")
	pLocalFree             = syscall.NewLazyDLL("kernel32.dll").NewProc("LocalFree")
	stClientToScreen       = user32.NewProc("ClientToScreen")
	stGetWindowTextLengthW = user32.NewProc("GetWindowTextLengthW")
	stGetWindowTextW       = user32.NewProc("GetWindowTextW")
	stGetSystemMetrics     = user32.NewProc("GetSystemMetrics")
)

type dataBlob struct {
	n    uint32
	data *byte
}

func blobOf(b []byte) dataBlob {
	if len(b) == 0 {
		return dataBlob{}
	}
	return dataBlob{n: uint32(len(b)), data: &b[0]}
}

// protect encrypts s for this Windows account (DPAPI), as base64.
func protect(s string) (string, error) {
	in := blobOf([]byte(s))
	var out dataBlob
	if r, _, err := pCryptProtectData.Call(uintptr(unsafe.Pointer(&in)), 0, 0, 0, 0, 0x1 /* CRYPTPROTECT_UI_FORBIDDEN */, uintptr(unsafe.Pointer(&out))); r == 0 {
		return "", err
	}
	defer pLocalFree.Call(uintptr(unsafe.Pointer(out.data)))
	return base64.StdEncoding.EncodeToString(unsafe.Slice(out.data, out.n)), nil
}

func unprotect(s string) (string, error) {
	b, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		return "", err
	}
	in := blobOf(b)
	var out dataBlob
	if r, _, err := pCryptUnprotectData.Call(uintptr(unsafe.Pointer(&in)), 0, 0, 0, 0, 0x1, uintptr(unsafe.Pointer(&out))); r == 0 {
		return "", err
	}
	defer pLocalFree.Call(uintptr(unsafe.Pointer(out.data)))
	return string(unsafe.Slice(out.data, out.n)), nil
}

func (a *app) streamKey() string {
	if a.settings.Stream.Key == "" {
		return ""
	}
	k, err := unprotect(a.settings.Stream.Key)
	if err != nil {
		return ""
	}
	return k
}

// ---------------- streaming ----------------

// streamRegion is the window's game area on screen, in physical pixels.
func (a *app) streamRegion() stream.Region {
	cr := clientRect(a.hwnd)
	pt := [2]int32{0, 0}
	call(stClientToScreen, a.hwnd, uintptr(unsafe.Pointer(&pt)))
	return stream.Region{X: int(pt[0]), Y: int(pt[1]), W: int(cr.Right), H: int(cr.Bottom)}
}

func (a *app) streamStart(again bool) {
	if a.stream.run != nil {
		return
	}
	key := a.streamKey()
	if err := stream.CheckKey(key); err != nil {
		messageBox(a.hwnd, err.Error()+".", "Stream to YouTube", mbIconInfo)
		a.openStreamSettings()
		return
	}
	if !a.recFFmpeg(true, true) {
		return
	}
	ff := ffmpeg.Find()
	if ff == "" {
		return
	}
	if ic := call(pIsIconic, a.hwnd); ic != 0 {
		messageBox(a.hwnd, "Restore the window first: the stream shows what's in it.", "Stream to YouTube", mbIconInfo)
		return
	}
	region := a.streamRegion()
	if region.W < 160 || region.H < 120 {
		messageBox(a.hwnd, "The window is too small to stream.", "Stream to YouTube", mbIconInfo)
		return
	}
	r := &streamRun{key: key, region: region, out: &tailBuffer{}, started: time.Now(), exited: make(chan struct{})}
	pr, pw := io.Pipe()
	var audio stream.Audio
	if lb, err := stream.StartLoopback(pw); err != nil {
		r.noSound = err.Error()
		pr.Close()
		pw.Close()
		pr, pw = nil, nil
	} else {
		r.lb, r.pw, audio = lb, pw, lb.Format
	}
	cmd := exec.Command(ff, stream.Args(region, audio, stream.QualityOf(a.settings.Stream.Quality), key)...)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	if pr != nil {
		cmd.Stdin = pr
	}
	cmd.Stdout, cmd.Stderr = r.out, r.out
	if err := cmd.Start(); err != nil {
		if r.lb != nil {
			pw.CloseWithError(err)
			r.lb.Close()
		}
		messageBox(a.hwnd, "Couldn't start ffmpeg: "+err.Error(), "Stream to YouTube", mbIconWarning)
		return
	}
	r.cmd = cmd
	a.stream.run = r
	go func() {
		err := cmd.Wait()
		if pr != nil {
			pr.CloseWithError(io.ErrClosedPipe) // the sound capture stops writing
		}
		if r.lb != nil {
			r.lb.Close()
		}
		msg := ""
		if err != nil {
			msg = stream.Redact(r.out.String(), r.key)
		}
		close(r.exited)
		a.event("stream-ended", msg)
	}()
	if again {
		a.showMessage("Streaming the window's new area")
	} else if r.noSound != "" {
		a.showMessage("Streaming to YouTube, without sound (" + r.noSound + ")")
	} else {
		a.showMessage("Streaming to YouTube: it shows in YouTube Studio in a few seconds")
	}
	if !again && !a.settings.Stream.NoChat && strings.TrimSpace(a.settings.Stream.Chat) != "" {
		a.streamOpenChat(true)
	}
	a.updateTitle()
}

// streamStop ends the stream: the sound input is closed so ffmpeg finishes
// the stream properly, and it's ended if it hasn't after 5 seconds.
func (a *app) streamStop(restart bool) {
	r := a.stream.run
	if r == nil || r.stopped {
		return
	}
	r.stopped, r.restart = true, restart
	go func() {
		if r.pw != nil {
			r.lb.Close()
			r.pw.Close()
		} else if r.cmd.Process != nil {
			r.cmd.Process.Kill()
		}
		select {
		case <-r.exited:
		case <-time.After(5 * time.Second):
			r.cmd.Process.Kill()
		}
	}()
	if !restart {
		a.showMessage("Stopping the stream…")
	}
}

// streamAreaChanged: the window moved or changed size; the stream restarts
// for the new area once it has stayed put for a moment.
func (a *app) streamAreaChanged() {
	if a.stream.run == nil {
		return
	}
	a.stream.moveGen++
	gen := a.stream.moveGen
	time.AfterFunc(1500*time.Millisecond, func() { a.event("stream-moved", fmt.Sprint(gen)) })
}

func (a *app) streamEvent(name, detail string) {
	switch name {
	case "stream-ended":
		r := a.stream.run
		if r == nil {
			return
		}
		a.stream.run = nil
		a.updateTitle()
		switch {
		case r.restart:
			a.streamStart(true)
		case r.stopped:
			a.showMessage("Stream stopped")
		default:
			why := detail
			if why == "" {
				why = "ffmpeg stopped."
			}
			if len(why) > 900 {
				why = "…" + why[len(why)-900:]
			}
			messageBox(a.hwnd, "The stream stopped:\n\n"+why+"\n\nCheck the stream key (Stream > Stream settings) and the internet connection.",
				"Stream to YouTube", mbIconWarning)
		}
	case "stream-moved":
		r := a.stream.run
		if r == nil || detail != fmt.Sprint(a.stream.moveGen) || r.stopped {
			return
		}
		if ic := call(pIsIconic, a.hwnd); ic != 0 || a.streamRegion() == r.region {
			return
		}
		a.streamStop(true)
	case "stream-chat":
		if detail != "" {
			a.openChatWindow(detail)
		}
	case "stream-message":
		a.showMessage(detail)
	}
}

// ---------------- the live chat ----------------

// streamOpenChat opens the stream's live chat: straight away for a stream's
// link; for a channel, once its live stream is found (auto: just started,
// so YouTube is given a minute or two to show it).
func (a *app) streamOpenChat(auto bool) {
	t, err := stream.ParseTarget(a.settings.Stream.Chat)
	if err != nil {
		if !auto {
			messageBox(a.hwnd, "Say where the chat is first: your channel (@name) or your stream's link.", "Live chat", mbIconInfo)
			a.openStreamSettings()
		}
		return
	}
	if t.VideoID != "" {
		a.openChatWindow(stream.ChatPage(t.VideoID))
		return
	}
	if !auto {
		a.showMessage("Looking for your live stream's chat…")
	}
	go func() {
		tries := 1
		if auto {
			tries = 12 // every 10 s for 2 minutes: YouTube takes a little while to show a new stream
		}
		for i := 0; i < tries; i++ {
			if i > 0 {
				time.Sleep(10 * time.Second)
			}
			if id := findLiveVideo(t.Channel); id != "" {
				a.event("stream-chat", stream.ChatPage(id))
				return
			}
		}
		if auto {
			a.event("stream-message", "Your channel isn't showing as live yet: open the chat from the Stream menu once it is")
		} else {
			a.event("stream-message", "Your channel isn't live right now: its page shows the chat once it is")
			a.event("stream-chat", stream.LivePage(t.Channel))
		}
	}()
}

func findLiveVideo(channel string) string {
	c := &http.Client{Timeout: 15 * time.Second}
	req, err := http.NewRequest("GET", stream.LivePage(channel), nil)
	if err != nil {
		return ""
	}
	req.Header.Set("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36")
	req.Header.Set("Accept-Language", "en")
	req.AddCookie(&http.Cookie{Name: "CONSENT", Value: "YES+1"}) // past the cookie page in Europe
	resp, err := c.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	return stream.LiveVideoID(string(b))
}

// openChatWindow shows a page in a window of its own beside this one:
// Microsoft Edge's app mode (no tabs or address bar), else the browser.
func (a *app) openChatWindow(url string) {
	edge := ""
	for _, env := range []string{"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"} {
		if d := os.Getenv(env); d != "" {
			p := filepath.Join(d, "Microsoft", "Edge", "Application", "msedge.exe")
			if st, err := os.Stat(p); err == nil && !st.IsDir() {
				edge = p
				break
			}
		}
	}
	if edge == "" {
		shellOpen(url)
		return
	}
	// Edge takes its size and place in 96-DPI units; this window's rect is in pixels
	wr := windowRect(a.hwnd)
	dpi := a.dpi()
	if dpi <= 0 {
		dpi = 96
	}
	px := func(v int) int { return v * 96 / dpi }
	w, h := 420, px(int(wr.Bottom-wr.Top))
	if h < 400 {
		h = 720
	}
	x, y := px(int(wr.Right)), px(int(wr.Top))
	vx := px(int(int32(call(stGetSystemMetrics, 76 /* SM_XVIRTUALSCREEN */))))
	vw := px(int(call(stGetSystemMetrics, 78 /* SM_CXVIRTUALSCREEN */)))
	if x+w > vx+vw {
		x = px(int(wr.Left)) - w // no room on the right: on the left
		if x < vx {
			x = vx + vw - w
		}
	}
	cmd := exec.Command(edge, "--app="+url, fmt.Sprintf("--window-size=%d,%d", w, h), fmt.Sprintf("--window-position=%d,%d", x, y))
	if err := cmd.Start(); err != nil {
		shellOpen(url)
		return
	}
	go cmd.Wait()
}

// ---------------- the settings window ----------------

var (
	streamDlgClassDone bool
	streamDlgCallback  = syscall.NewCallback(streamDlgProc)
)

type streamDlg struct {
	a                        *app
	hwnd                     uintptr
	key, chat, quality, auto uintptr
}

func (a *app) streamDlgHwnd() uintptr {
	if a.stream.dlg != nil {
		return a.stream.dlg.hwnd
	}
	return 0
}

// streamDialogMessage lets Tab, Enter and Esc work in the settings window.
func (a *app) streamDialogMessage(m *msg) bool {
	h := a.streamDlgHwnd()
	if h != 0 && (m.Hwnd == h || call(pGetAncestor, m.Hwnd, 2 /* GA_ROOT */) == h) {
		return call(pIsDialogMessageW, h, uintptr(unsafe.Pointer(m))) != 0
	}
	return false
}

func (a *app) openStreamSettings() {
	if d := a.stream.dlg; d != nil {
		call(pSetForegroundWindow, d.hwnd)
		return
	}
	cls := u16("RetroPixelsNativeStream")
	if !streamDlgClassDone {
		wc := wndClassEx{WndProc: streamDlgCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow), Background: 16, ClassName: cls}
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		streamDlgClassDone = true
	}
	d := &streamDlg{a: a}
	a.stream.dlg = d
	dpi := a.dpi()
	w, h := 560*dpi/96, 400*dpi/96
	wr := windowRect(a.hwnd)
	x := max(0, int(wr.Left)+(int(wr.Right-wr.Left)-w)/2)
	y := max(0, int(wr.Top)+(int(wr.Bottom-wr.Top)-h)/2)
	d.hwnd = call(pCreateWindowExW, wsExDlgModalFrame|wsExControlParent, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("Stream to YouTube"))),
		wsPopup|wsCaption|wsSysMenu|wsVisible|wsClipChildren, uintptr(x), uintptr(y), uintptr(w), uintptr(h), a.hwnd, 0, a.hinst, 0)
	d.build()
}

func (d *streamDlg) scale(v int) uintptr { return uintptr(v * d.a.dpi() / 96) }

func (d *streamDlg) child(class, text string, style uint32, exStyle uintptr, x, y, w, h, id int) uintptr {
	c := call(pCreateWindowExW, exStyle, uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), d.scale(x), d.scale(y), d.scale(w), d.scale(h), d.hwnd, uintptr(id), d.a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, d.a.font, 1)
	return c
}

func (d *streamDlg) build() {
	s := d.a.settings.Stream
	y := 14
	d.child("STATIC", "Streams this window's game, with the PC's sound, straight to YouTube (no OBS needed). "+
		"Moving or resizing the window restarts the stream for the new area.", ssNoPrefix, 0, 16, y, 516, 40, 0)
	y += 50
	keyLabel := "Stream key (YouTube Studio > Go live > Stream > Stream key):"
	if s.Key != "" {
		keyLabel = "Stream key (saved; type a new one to change it):"
	}
	d.child("STATIC", keyLabel, ssNoPrefix, 0, 16, y, 516, 20, 0)
	y += 22
	d.key = d.child("EDIT", "", esAutoHScroll|esPassword|wsTabStop, 0x200, 16, y, 400, 24, idStKey)
	d.child("BUTTON", "YouTube Studio", bsPushButton|wsTabStop, 0, 424, y-1, 108, 26, idStStudio)
	y += 36
	d.child("STATIC", "Live chat: your channel (@name) or your stream's link:", ssNoPrefix, 0, 16, y, 516, 20, 0)
	y += 22
	d.chat = d.child("EDIT", s.Chat, esAutoHScroll|wsTabStop, 0x200, 16, y, 516, 24, idStChat)
	y += 32
	d.auto = d.child("BUTTON", "Open the chat when the stream starts", bsAutoCheck|wsTabStop, 0, 16, y, 400, 22, idStAuto)
	if !s.NoChat {
		call(pSendMessageW, d.auto, bmSetCheck, 1, 0)
	}
	y += 34
	d.child("STATIC", "Quality:", ssNoPrefix, 0, 16, y+4, 70, 20, 0)
	d.quality = d.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, 90, y, 260, 200, idStQuality)
	cur := stream.QualityOf(s.Quality).ID
	for i, q := range stream.Qualities {
		call(pSendMessageW, d.quality, cbAddString, 0, uintptr(unsafe.Pointer(u16(q.Label))))
		if q.ID == cur {
			call(pSendMessageW, d.quality, cbSetCurSel, uintptr(i), 0)
		}
	}
	y += 34
	d.child("STATIC", "The key is kept encrypted for your Windows account. Recording (F9) works while streaming.", ssNoPrefix, 0, 16, y, 516, 20, 0)
	y += 32
	d.child("BUTTON", "Save", bsDefPushButton|wsTabStop, 0, 332, y, 96, 28, idStSave)
	d.child("BUTTON", "Cancel", bsPushButton|wsTabStop, 0, 436, y, 96, 28, idStCancel)
	wr, cr := windowRect(d.hwnd), clientRect(d.hwnd)
	extraH := int(wr.Bottom-wr.Top) - int(cr.Bottom)
	call(pSetWindowPos, d.hwnd, 0, 0, 0, uintptr(wr.Right-wr.Left), uintptr(extraH)+d.scale(y+44), swpNoMove|swpNoZOrder|swpNoActivate)
	call(pSetFocus, d.key)
}

func editText(h uintptr) string {
	n := call(stGetWindowTextLengthW, h)
	buf := make([]uint16, n+1)
	call(stGetWindowTextW, h, uintptr(unsafe.Pointer(&buf[0])), n+1)
	return strings.TrimSpace(syscall.UTF16ToString(buf))
}

func (d *streamDlg) save() bool {
	s := &d.a.settings.Stream
	if k := editText(d.key); k != "" {
		if err := stream.CheckKey(k); err != nil {
			messageBox(d.hwnd, err.Error()+".", "Stream to YouTube", mbIconInfo)
			return false
		}
		enc, err := protect(k)
		if err != nil {
			messageBox(d.hwnd, "Couldn't save the key: "+err.Error(), "Stream to YouTube", mbIconWarning)
			return false
		}
		s.Key = enc
	}
	chat := editText(d.chat)
	if chat != "" {
		if _, err := stream.ParseTarget(chat); err != nil {
			messageBox(d.hwnd, "Live chat: "+err.Error()+".", "Stream to YouTube", mbIconInfo)
			return false
		}
	}
	s.Chat = chat
	s.NoChat = call(pSendMessageW, d.auto, bmGetCheck, 0, 0) != 1
	if k := int(int32(call(pSendMessageW, d.quality, cbGetCurSel, 0, 0))); k >= 0 && k < len(stream.Qualities) {
		s.Quality = stream.Qualities[k].ID
	}
	saveSettings(d.a.settings)
	if d.a.stream.run != nil {
		d.a.showMessage("Saved: a new key or quality applies from the next stream")
	} else {
		d.a.showMessage("Stream settings saved")
	}
	return true
}

func (d *streamDlg) close() {
	a := d.a
	call(pDestroyWindow, d.hwnd)
	a.stream.dlg = nil
	call(pSetForegroundWindow, a.hwnd)
}

func streamDlgProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.stream.dlg == nil || a.stream.dlg.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	d := a.stream.dlg
	switch uint32(m) {
	case wmCommand:
		switch loword(wp) & 0xffff {
		case idStSave, idOK:
			if d.save() {
				d.close()
			}
		case idStCancel, 2: // IDCANCEL (Esc)
			d.close()
		case idStStudio:
			shellOpen(studioURL)
		}
		return 0
	case wmClose:
		d.close()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}
