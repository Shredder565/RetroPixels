package wiiu

// Checks the shader interpreter against real R700 machine code: LLVM's R600
// back end (the one Mesa uses for these GPUs) compiles small shaders for the
// RV770, and the interpreter's results must match Go's. Skipped when llc
// isn't installed.

import (
	"debug/elf"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func compileR700(t *testing.T, ir string) []byte {
	t.Helper()
	llc, err := exec.LookPath("llc")
	if err != nil {
		t.Skip("llc isn't installed")
	}
	dir := t.TempDir()
	src := filepath.Join(dir, "s.ll")
	obj := filepath.Join(dir, "s.o")
	if err := os.WriteFile(src, []byte(ir), 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command(llc, "-mtriple=r600", "-mcpu=rv770", "-filetype=obj", "-o", obj, src).CombinedOutput()
	if err != nil {
		t.Fatalf("llc: %v\n%s\n%s", err, out, ir)
	}
	f, err := elf.Open(obj)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	sec := f.Section(".text")
	if sec == nil {
		t.Fatal("no .text")
	}
	b, err := sec.Data()
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func disasmR700(ir string) string {
	llc, _ := exec.LookPath("llc")
	cmd := exec.Command(llc, "-mtriple=r600", "-mcpu=rv770", "-o", "-", "-")
	cmd.Stdin = strings.NewReader(ir)
	out, _ := cmd.CombinedOutput()
	return string(out)
}

type shaderCase struct {
	name string
	body string // IR using %x, %y, %z, %w (floats of R1) and %a, %b (of R2); ends by setting %o0..%o3
	ref  func(x, y, z, w, a, b float32) [4]float32
	ints bool // outputs are integer bit patterns
}

func runVSLanes(t *testing.T, code []byte, in [nl][8]float32) [nl][4]uint32 {
	g := newTestGPU()
	g.mem.WriteBytes(tVS, code)
	// a fetch shader that just returns
	ret := &shaderAsm{}
	ret.CF(cfRETURN, 0, 0, 0, 0)
	putShader(g, tVS+0x1000, ret)
	e := &stageEnv{g: g, stage: stageVS, fsAddr: tVS + 0x1000}
	s := &shaderRun{env: e}
	var outs [nl]vtxOut
	for l := 0; l < nl; l++ {
		e.vtx[l] = &outs[l]
		s.valid[l] = true
		for c := 0; c < 4; c++ {
			s.gpr[1][c][l] = f2u(in[l][c])
			s.gpr[2][c][l] = f2u(in[l][4+c])
		}
	}
	s.run(tVS)
	if s.Err != "" {
		t.Fatal(s.Err)
	}
	var r [nl][4]uint32
	for l := 0; l < nl; l++ {
		for c := 0; c < 4; c++ {
			r[l][c] = f2u(outs[l].pos[c])
		}
	}
	return r
}

func shaderIR(body string) string {
	return `
declare void @llvm.r600.store.swizzle(<4 x float>, i32, i32)
declare float @llvm.floor.f32(float)
declare float @llvm.sqrt.f32(float)
declare float @llvm.fabs.f32(float)
declare float @llvm.sin.f32(float)
declare float @llvm.cos.f32(float)
declare float @llvm.maxnum.f32(float, float)
declare float @llvm.minnum.f32(float, float)
declare float @llvm.exp2.f32(float)
declare float @llvm.log2.f32(float)
declare float @llvm.trunc.f32(float)
declare float @llvm.ceil.f32(float)
define amdgpu_vs void @main(<4 x float> inreg %r0, <4 x float> inreg %r1, <4 x float> inreg %r2) {
entry:
  %x = extractelement <4 x float> %r1, i32 0
  %y = extractelement <4 x float> %r1, i32 1
  %z = extractelement <4 x float> %r1, i32 2
  %w = extractelement <4 x float> %r1, i32 3
  %a = extractelement <4 x float> %r2, i32 0
  %b = extractelement <4 x float> %r2, i32 1
` + body + `
  %v0 = insertelement <4 x float> undef, float %o0, i32 0
  %v1 = insertelement <4 x float> %v0, float %o1, i32 1
  %v2 = insertelement <4 x float> %v1, float %o2, i32 2
  %v3 = insertelement <4 x float> %v2, float %o3, i32 3
  call void @llvm.r600.store.swizzle(<4 x float> %v3, i32 60, i32 1)
  ret void
}
`
}

func f32eq(a, b float32) bool {
	if a == b {
		return true
	}
	if math.IsNaN(float64(a)) && math.IsNaN(float64(b)) {
		return true
	}
	d := math.Abs(float64(a - b))
	return d <= 1e-5*math.Max(1, math.Abs(float64(b)))
}

var shaderCases = []shaderCase{
	{name: "arith", body: `
  %m = fmul float %x, %y
  %o0 = fadd float %m, 3.5
  %o1 = call float @llvm.floor.f32(float %x)
  %o2 = fdiv float 1.0, %x
  %ay = call float @llvm.fabs.f32(float %y)
  %o3 = call float @llvm.sqrt.f32(float %ay)
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		return [4]float32{x*y + 3.5, float32(math.Floor(float64(x))), 1 / x, float32(math.Sqrt(math.Abs(float64(y))))}
	}},
	{name: "select", body: `
  %o0 = call float @llvm.maxnum.f32(float %x, float %y)
  %o1 = call float @llvm.minnum.f32(float %x, float %y)
  %c = fcmp ogt float %x, %y
  %y2 = fmul float %y, 2.0
  %o2 = select i1 %c, float %x, float %y2
  %e = fcmp oeq float %x, %z
  %o3 = select i1 %e, float 1.0, float 0.0
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		o := [4]float32{max(x, y), min(x, y), y * 2, 0}
		if x > y {
			o[2] = x
		}
		if x == z {
			o[3] = 1
		}
		return o
	}},
	{name: "ints", ints: true, body: `
  %xi = fptosi float %x to i32
  %yi = fptosi float %y to i32
  %p = mul i32 %xi, %yi
  %o0 = bitcast i32 %p to float
  %sh = shl i32 %xi, 3
  %an = and i32 %yi, 5
  %or = or i32 %sh, %an
  %o1 = bitcast i32 %or to float
  %sr = ashr i32 %yi, 1
  %xo = xor i32 %sr, %xi
  %o2 = bitcast i32 %xo to float
  %lt = icmp slt i32 %xi, %yi
  %sel = select i1 %lt, i32 %xi, i32 %yi
  %sum = add i32 %sel, 100
  %o3 = bitcast i32 %sum to float
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		xi, yi := int32(x), int32(y)
		bits := func(v int32) float32 { return math.Float32frombits(uint32(v)) }
		return [4]float32{bits(xi * yi), bits(xi<<3 | yi&5), bits(yi>>1 ^ xi), bits(min(xi, yi) + 100)}
	}},
	{name: "convert", body: `
  %xi = fptosi float %x to i32
  %o0 = sitofp i32 %xi to float
  %yu = fptoui float %w to i32
  %d = udiv i32 %yu, 3
  %o1 = uitofp i32 %d to float
  %o2 = call float @llvm.trunc.f32(float %y)
  %o3 = call float @llvm.ceil.f32(float %y)
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		return [4]float32{float32(int32(x)), float32(uint32(max(w, 0)) / 3), float32(math.Trunc(float64(y))), float32(math.Ceil(float64(y)))}
	}},
	{name: "branch", body: `
  %c = fcmp ogt float %x, 0.0
  br i1 %c, label %then, label %else
then:
  %t0 = fmul float %x, 2.0
  %t1 = fadd float %y, %z
  br label %done
else:
  %e0 = fsub float %y, 1.0
  %e1 = fmul float %z, %z
  br label %done
done:
  %o0 = phi float [ %t0, %then ], [ %e0, %else ]
  %o1 = phi float [ %t1, %then ], [ %e1, %else ]
  %o2 = fadd float %x, 0.0
  %o3 = fadd float %w, 1.0
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		if x > 0 {
			return [4]float32{x * 2, y + z, x, w + 1}
		}
		return [4]float32{y - 1, z * z, x, w + 1}
	}},
	{name: "loop", body: `
  %n = fptosi float %x to i32
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %i2, %body ]
  %s = phi float [ 0.0, %entry ], [ %s2, %body ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %s2 = fadd float %s, %y
  %i2 = add i32 %i, 1
  br label %head
exit:
  %o0 = fadd float %s, 0.0
  %o1 = sitofp i32 %i to float
  %o2 = fmul float %z, 1.0
  %o3 = fadd float %w, 0.0
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		n := int32(x)
		var s float32
		i := int32(0)
		for ; i < n; i++ {
			s += y
		}
		return [4]float32{s, float32(i), z, w}
	}},
	{name: "loopbreak", body: `
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %i2, %cont ]
  %s = phi float [ %y, %entry ], [ %s3, %cont ]
  %s2 = fmul float %s, 2.0
  %big = fcmp ogt float %s2, %z
  br i1 %big, label %exit, label %cont
cont:
  %i2 = add i32 %i, 1
  %s3 = fadd float %s2, 0.0
  %lim = icmp slt i32 %i2, 10
  br i1 %lim, label %head, label %exit
exit:
  %r = phi float [ %s2, %head ], [ %s3, %cont ]
  %ri = phi i32 [ %i, %head ], [ %i2, %cont ]
  %o0 = fadd float %r, 0.0
  %o1 = sitofp i32 %ri to float
  %o2 = fadd float %x, 0.0
  %o3 = fadd float %w, 0.0
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		s := y
		i := int32(0)
		for {
			s2 := s * 2
			if s2 > z {
				return [4]float32{s2, float32(i), x, w}
			}
			i++
			s = s2
			if i >= 10 {
				return [4]float32{s, float32(i), x, w}
			}
		}
	}},
	{name: "trig", body: `
  %o0 = call float @llvm.sin.f32(float %x)
  %o1 = call float @llvm.cos.f32(float %y)
  %o2 = call float @llvm.exp2.f32(float %z)
  %aw = call float @llvm.fabs.f32(float %w)
  %aw1 = fadd float %aw, 1.0
  %o3 = call float @llvm.log2.f32(float %aw1)
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		return [4]float32{float32(math.Sin(float64(x))), float32(math.Cos(float64(y))), float32(math.Exp2(float64(z))),
			float32(math.Log2(math.Abs(float64(w)) + 1))}
	}},
	{name: "dot", body: `
  %m0 = fmul float %x, %a
  %m1 = fmul float %y, %b
  %m2 = fmul float %z, %z
  %m3 = fmul float %w, %a
  %s0 = fadd float %m0, %m1
  %s1 = fadd float %s0, %m2
  %o0 = fadd float %s1, %m3
  %o1 = fsub float %a, %b
  %o2 = fneg float %x
  %q = fdiv float %y, %a
  %o3 = fadd float %q, 0.0
`, ref: func(x, y, z, w, a, b float32) [4]float32 {
		return [4]float32{x*a + y*b + z*z + w*a, a - b, -x, y / a}
	}},
}

func TestShaderLLVM(t *testing.T) {
	inputs := [nl][8]float32{
		{1.5, -2.25, 3, 7, 0.5, 2},
		{-3.75, 4.5, -1, 12, 1.25, -1},
		{0, 2, 0, 3, -2, 0.5},
		{6.25, 6.25, 100, -5, 4, 3},
	}
	for _, c := range shaderCases {
		t.Run(c.name, func(t *testing.T) {
			ir := shaderIR(c.body)
			code := compileR700(t, ir)
			got := runVSLanes(t, code, inputs)
			for l := 0; l < nl; l++ {
				in := inputs[l]
				want := c.ref(in[0], in[1], in[2], in[3], in[4], in[5])
				for k := 0; k < 4; k++ {
					ok := f32eq(u2f(got[l][k]), want[k])
					if c.ints {
						ok = got[l][k] == f2u(want[k])
					}
					if !ok {
						t.Errorf("lane %d out %d: got %v (%08x) want %v (%08x)", l, k, u2f(got[l][k]), got[l][k], want[k], f2u(want[k]))
					}
				}
			}
			if t.Failed() {
				fmt.Println(disasmR700(ir))
			}
		})
	}
}
