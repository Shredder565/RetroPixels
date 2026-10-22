//go:build linux

package pixelu

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeFile(t *testing.T, path, body string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestCheckVulkanLinux(t *testing.T) {
	root := t.TempDir()
	lib := filepath.Join(root, "lib")
	data := filepath.Join(root, "share")
	env := map[string]string{"HOME": filepath.Join(root, "home"), "XDG_DATA_DIRS": data, "XDG_CONFIG_DIRS": filepath.Join(root, "etc")}
	getenv := func(k string) string { return env[k] }
	dirs := []string{lib}

	err := checkVulkan(getenv, dirs, nil)
	if err == nil || !strings.Contains(err.Error(), "libvulkan.so.1") {
		t.Fatalf("no loader: %v", err)
	}
	// the loader listed in ld.so.cache only
	if err := checkVulkan(getenv, dirs, []byte("x\x00libvulkan.so.1\x00y")); err == nil || !strings.Contains(err.Error(), "no Vulkan driver") {
		t.Fatalf("cache loader, no ICD: %v", err)
	}

	writeFile(t, filepath.Join(lib, "libvulkan.so.1"), "")
	if err := checkVulkan(getenv, dirs, nil); err == nil || !strings.Contains(err.Error(), "no Vulkan driver") {
		t.Fatalf("no ICD: %v", err)
	}

	icd := filepath.Join(data, "vulkan", "icd.d", "lvp_icd.json")
	writeFile(t, icd, `{"ICD": {"api_version": "1.3.0", "library_path": "libvulkan_lvp.so"}, "file_format_version": "1.0.1"}`)
	if err := checkVulkan(getenv, dirs, nil); err == nil || !strings.Contains(err.Error(), "missing libraries") {
		t.Fatalf("ICD without its library: %v", err)
	}
	// a bare name is found like the dynamic linker would: in the folders or the cache
	if err := checkVulkan(getenv, dirs, []byte("libvulkan_lvp.so\x00")); err != nil {
		t.Fatalf("library in cache: %v", err)
	}
	writeFile(t, filepath.Join(lib, "libvulkan_lvp.so"), "")
	if err := checkVulkan(getenv, dirs, nil); err != nil {
		t.Fatalf("working setup: %v", err)
	}

	// relative and absolute library paths
	os.Remove(icd)
	writeFile(t, filepath.Join(data, "vulkan", "icd.d", "rel.json"), `{"ICD": {"library_path": "../drv/libdrv.so"}}`)
	if err := checkVulkan(getenv, dirs, nil); err == nil {
		t.Fatal("relative path to a missing library accepted")
	}
	writeFile(t, filepath.Join(data, "vulkan", "drv", "libdrv.so"), "")
	if err := checkVulkan(getenv, dirs, nil); err != nil {
		t.Fatalf("relative path: %v", err)
	}

	// VK_ICD_FILENAMES replaces the folders
	abs := filepath.Join(root, "only.json")
	writeFile(t, abs, `{"ICD": {"library_path": "`+filepath.Join(root, "nope.so")+`"}}`)
	env["VK_ICD_FILENAMES"] = abs
	if err := checkVulkan(getenv, dirs, nil); err == nil {
		t.Fatal("VK_ICD_FILENAMES ignored")
	}
	writeFile(t, filepath.Join(root, "nope.so"), "")
	if err := checkVulkan(getenv, dirs, nil); err != nil {
		t.Fatalf("VK_ICD_FILENAMES: %v", err)
	}
}

func TestICDDirsOrder(t *testing.T) {
	got := icdDirs(func(k string) string { return map[string]string{"HOME": "/h"}[k] })
	want := []string{"/h/.config/vulkan/icd.d", "/etc/xdg/vulkan/icd.d", "/usr/local/etc/vulkan/icd.d", "/etc/vulkan/icd.d",
		"/h/.local/share/vulkan/icd.d", "/usr/local/share/vulkan/icd.d", "/usr/share/vulkan/icd.d"}
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("icdDirs = %v", got)
	}
}
