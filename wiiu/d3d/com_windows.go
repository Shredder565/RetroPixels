//go:build windows

// Package d3d is the parts of Direct3D 11 the Wii U renderer uses, called
// through COM from Go (no cgo): devices, textures, buffers, views, shaders
// compiled from HLSL, state objects and draws.
package d3d

import (
	"fmt"
	"syscall"
	"unsafe"
)

var (
	d3d11dll         = syscall.NewLazyDLL("d3d11.dll")
	procCreateDevice = d3d11dll.NewProc("D3D11CreateDevice")
	compilerDLL      = syscall.NewLazyDLL("d3dcompiler_47.dll")
	procCompile      = compilerDLL.NewProc("D3DCompile")
)

// Unknown is a COM object: a pointer to its table of methods.
type Unknown struct{ p uintptr }

func (u Unknown) Ptr() uintptr { return u.p }
func (u Unknown) Nil() bool    { return u.p == 0 }

// call calls method i of the object.
func (u Unknown) call(i int, args ...uintptr) uintptr {
	vt := *(*uintptr)(ptr(u.p))
	fn := *(*uintptr)(ptr(vt + uintptr(i)*8))
	r, _, _ := syscall.SyscallN(fn, append([]uintptr{u.p}, args...)...)
	return r
}

// Release lets the object go.
func (u *Unknown) Release() {
	if u.p != 0 {
		u.call(2)
		u.p = 0
	}
}

// ptr turns an address from COM into a pointer (memory outside Go's heap).
func ptr(a uintptr) unsafe.Pointer { return *(*unsafe.Pointer)(unsafe.Pointer(&a)) }

func hr(r uintptr, what string) error {
	if int32(r) < 0 {
		return fmt.Errorf("%s failed (%08X)", what, uint32(r))
	}
	return nil
}

func f32(v float32) uintptr { return uintptr(*(*uint32)(unsafe.Pointer(&v))) }

// Blob is an ID3DBlob.
type Blob struct{ Unknown }

func (b Blob) Bytes() []byte {
	p := b.call(3)
	n := b.call(4)
	if p == 0 || n == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(ptr(p)), int(n))
}

// Compile compiles HLSL source for a target ("vs_5_0", "ps_5_0").
func Compile(src, entry, target string) ([]byte, error) {
	if err := procCompile.Find(); err != nil {
		return nil, fmt.Errorf("the HLSL compiler (d3dcompiler_47.dll) isn't installed: %v", err)
	}
	s := []byte(src)
	e := append([]byte(entry), 0)
	t := append([]byte(target), 0)
	var code, errs Blob
	const optimize3 = 1 << 15
	r, _, _ := procCompile.Call(uintptr(unsafe.Pointer(&s[0])), uintptr(len(s)), 0, 0, 0,
		uintptr(unsafe.Pointer(&e[0])), uintptr(unsafe.Pointer(&t[0])), optimize3, 0,
		uintptr(unsafe.Pointer(&code.p)), uintptr(unsafe.Pointer(&errs.p)))
	defer errs.Release()
	if int32(r) < 0 {
		msg := ""
		if !errs.Nil() {
			msg = string(errs.Bytes())
		}
		return nil, fmt.Errorf("HLSL compile failed (%08X): %s", uint32(r), msg)
	}
	out := append([]byte(nil), code.Bytes()...)
	code.Release()
	return out, nil
}
