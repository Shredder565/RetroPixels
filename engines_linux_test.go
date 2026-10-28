package main

import (
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"retropixels/internal/pixelu"
)

// On Linux the engines are named without .exe, the "choose a program"
// dialog looks for that name (and all files), and a chosen program must be
// marked as executable.
func TestEngineNamesLinux(t *testing.T) {
	if exeFile("RetroPixels") != "RetroPixels" || pixelu.ExeName != "PixelU" {
		t.Fatalf("names: %q %q", exeFile("RetroPixels"), pixelu.ExeName)
	}
	if f := programFilter("PixelU", pixelu.ExeName); f != "PixelU (PixelU)|PixelU" {
		t.Errorf("filter %q", f)
	}
	got := pickArgs("zenity", "Choose PixelU", programFilter("PixelU", pixelu.ExeName), "")
	want := []string{"--file-selection", "--title=Choose PixelU", "--file-filter=PixelU (PixelU) | PixelU", "--file-filter=All files | *"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("zenity args %q", got)
	}
	dir := t.TempDir()
	prog, data := filepath.Join(dir, "PixelU"), filepath.Join(dir, "notes.txt")
	os.WriteFile(prog, []byte("#!/bin/sh\n"), 0o755)
	os.WriteFile(data, []byte("x"), 0o644)
	if err := checkProgram(prog); err != nil {
		t.Errorf("program refused: %v", err)
	}
	if err := checkProgram(data); err == nil || !strings.Contains(err.Error(), "executable") {
		t.Errorf("data file: %v", err)
	}
	if err := checkProgram(dir); err == nil {
		t.Error("a folder taken for a program")
	}
	if err := checkProgram(filepath.Join(dir, "gone")); err == nil {
		t.Error("a missing file taken for a program")
	}
	if !strings.Contains(errNoWiiEngine.Error(), "next to RetroPixels (it must have wii/PixelU-Wii in it)") {
		t.Errorf("message: %v", errNoWiiEngine)
	}
}

// The page is told which system the app runs on, and words itself for it.
func TestPageKnowsTheSystem(t *testing.T) {
	rec := httptest.NewRecorder()
	req := httptest.NewRequest("GET", "/", nil)
	req.Host = fmt.Sprintf("127.0.0.1:%d", port)
	newMux().ServeHTTP(rec, req)
	body := rec.Body.String()
	if rec.Code != 200 || !strings.Contains(body, `os: "linux" };`) {
		t.Fatalf("page %d: no os in %q", rec.Code, body[:min(len(body), 200)])
	}
	for _, s := range []string{`const OS = (DESKTOP && DESKTOP.os) || "windows";`, `data-exe="RetroPixels"`, `data-path="code/*.rpx"`, "data-nonwin="} {
		if !strings.Contains(body, s) {
			t.Errorf("page has no %s", s)
		}
	}
}
