//go:build linux

package pixelu

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

var (
	vulkanOnce sync.Once
	vulkanErr  error
)

// CheckVulkan says whether Vulkan can work on this PC: the Vulkan loader
// (libvulkan.so.1) is installed and at least one Vulkan driver is
// registered with it (an ICD file in the loader's folders whose driver
// library is there). PixelU draws Wii U games with Vulkan; without it they
// can play with OpenGL instead. RetroPixels is built without cgo, so
// nothing is loaded: the files are only looked for. Checked once.
func CheckVulkan() error {
	vulkanOnce.Do(func() {
		cache, _ := os.ReadFile("/etc/ld.so.cache")
		vulkanErr = checkVulkan(os.Getenv, libDirs(os.Getenv), cache)
	})
	return vulkanErr
}

// libDirs are the folders the dynamic linker looks in, roughly: the
// environment's, then the usual system ones (Debian/Ubuntu multiarch,
// Fedora/openSUSE lib64, Arch lib, NixOS's driver folder).
func libDirs(getenv func(string) string) []string {
	var dirs []string
	for _, d := range filepath.SplitList(getenv("LD_LIBRARY_PATH")) {
		if d != "" {
			dirs = append(dirs, d)
		}
	}
	return append(dirs,
		"/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu",
		"/usr/lib64", "/lib64", "/usr/lib", "/lib",
		"/usr/local/lib", "/usr/local/lib64",
		"/run/opengl-driver/lib")
}

// findLib reports whether the dynamic linker would find a library by its
// bare name: in one of dirs, or listed in /etc/ld.so.cache.
func findLib(name string, dirs []string, cache []byte) bool {
	for _, d := range dirs {
		if _, err := os.Stat(filepath.Join(d, name)); err == nil {
			return true
		}
	}
	return len(cache) > 0 && bytes.Contains(cache, append([]byte(name), 0))
}

// icdDirs are the folders the Vulkan loader reads driver (ICD) files from,
// in its order.
func icdDirs(getenv func(string) string) []string {
	home := getenv("HOME")
	configHome := getenv("XDG_CONFIG_HOME")
	if configHome == "" && home != "" {
		configHome = filepath.Join(home, ".config")
	}
	configDirs := getenv("XDG_CONFIG_DIRS")
	if configDirs == "" {
		configDirs = "/etc/xdg"
	}
	dataHome := getenv("XDG_DATA_HOME")
	if dataHome == "" && home != "" {
		dataHome = filepath.Join(home, ".local", "share")
	}
	dataDirs := getenv("XDG_DATA_DIRS")
	if dataDirs == "" {
		dataDirs = "/usr/local/share:/usr/share"
	}
	var bases []string
	if configHome != "" {
		bases = append(bases, configHome)
	}
	bases = append(bases, filepath.SplitList(configDirs)...)
	bases = append(bases, "/usr/local/etc", "/etc")
	if dataHome != "" {
		bases = append(bases, dataHome)
	}
	bases = append(bases, filepath.SplitList(dataDirs)...)
	var dirs []string
	for _, b := range bases {
		if b != "" {
			dirs = append(dirs, filepath.Join(b, "vulkan", "icd.d"))
		}
	}
	return dirs
}

// icdFiles lists the ICD files the loader would read: the ones
// VK_DRIVER_FILES / VK_ICD_FILENAMES name (files or folders), else every
// .json in the standard folders.
func icdFiles(getenv func(string) string) []string {
	list := getenv("VK_DRIVER_FILES")
	if list == "" {
		list = getenv("VK_ICD_FILENAMES")
	}
	var out []string
	addDir := func(d string) {
		ents, err := os.ReadDir(d)
		if err != nil {
			return
		}
		for _, e := range ents {
			if !e.IsDir() && strings.HasSuffix(e.Name(), ".json") {
				out = append(out, filepath.Join(d, e.Name()))
			}
		}
	}
	if list != "" {
		for _, p := range filepath.SplitList(list) {
			if st, err := os.Stat(p); err == nil && st.IsDir() {
				addDir(p)
			} else if err == nil {
				out = append(out, p)
			}
		}
		return out
	}
	for _, d := range icdDirs(getenv) {
		addDir(d)
	}
	return out
}

// icdUsable reports whether an ICD file names a driver library that is
// there: an absolute path, a path relative to the file, or a bare name
// the dynamic linker finds.
func icdUsable(file string, dirs []string, cache []byte) bool {
	b, err := os.ReadFile(file)
	if err != nil {
		return false
	}
	var icd struct {
		ICD struct {
			LibraryPath string `json:"library_path"`
		} `json:"ICD"`
	}
	if json.Unmarshal(b, &icd) != nil || icd.ICD.LibraryPath == "" {
		return false
	}
	lib := icd.ICD.LibraryPath
	switch {
	case filepath.IsAbs(lib):
	case strings.Contains(lib, "/"):
		lib = filepath.Join(filepath.Dir(file), lib)
	default:
		return findLib(lib, dirs, cache)
	}
	_, err = os.Stat(lib)
	return err == nil
}

func checkVulkan(getenv func(string) string, dirs []string, cache []byte) error {
	if !findLib("libvulkan.so.1", dirs, cache) {
		return errors.New("Vulkan isn't installed: libvulkan.so.1 is missing (install your distribution's Vulkan loader, e.g. the libvulkan1 or vulkan-loader package)")
	}
	files := icdFiles(getenv)
	for _, f := range files {
		if icdUsable(f, dirs, cache) {
			return nil
		}
	}
	if len(files) == 0 {
		return errors.New("no Vulkan driver is installed (install mesa-vulkan-drivers or your distribution's vulkan-radeon/vulkan-intel package, or NVIDIA's driver)")
	}
	return errors.New("the Vulkan driver files on this PC point to missing libraries (reinstall the graphics driver's Vulkan package)")
}
