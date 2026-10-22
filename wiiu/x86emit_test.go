package wiiu

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"
)

// TestX86Encodings checks the machine code writer against llvm-mc's
// disassembler.
func TestX86Encodings(t *testing.T) {
	if _, err := exec.LookPath("llvm-mc"); err != nil {
		t.Skip("llvm-mc is needed")
	}
	type tc struct {
		emit func(a *x86)
		want string
	}
	cases := []tc{
		{func(a *x86) { a.movLoad(rAX, rBX, 0x10) }, "movl 16(%rbx), %eax"},
		{func(a *x86) { a.movLoad(r8, rBX, 0x200) }, "movl 512(%rbx), %r8d"},
		{func(a *x86) { a.movStore(rBX, 0x7c, rCX) }, "movl %ecx, 124(%rbx)"},
		{func(a *x86) { a.movStore(rBX, 4, r10) }, "movl %r10d, 4(%rbx)"},
		{func(a *x86) { a.movStoreImm(rBX, 8, 0x12345678) }, "movl $305419896, 8(%rbx)"},
		{func(a *x86) { a.movRI(rDX, 5) }, "movl $5, %edx"},
		{func(a *x86) { a.movRI(r11, 5) }, "movl $5, %r11d"},
		{func(a *x86) { a.movRR(rAX, r11) }, "movl %r11d, %eax"},
		{func(a *x86) { a.movRR(r8, rAX) }, "movl %eax, %r8d"},
		{func(a *x86) { a.movLoad64Idx(rDX, r12, rAX, 8) }, "movq (%r12,%rax,8), %rdx"},
		{func(a *x86) { a.movLoad64Idx(rDX, r13, rCX, 8) }, "movq (%r13,%rcx,8), %rdx"},
		{func(a *x86) { a.movLoad64Idx(rDX, rDX, rCX, 2) }, "movq (%rdx,%rcx,2), %rdx"},
		{func(a *x86) { a.testRR64(rDX, rDX) }, "testq %rdx, %rdx"},
		{func(a *x86) { a.testRR64(r9, r9) }, "testq %r9, %r9"},
		{func(a *x86) { a.load32Idx(rAX, rDX, rAX) }, "movl (%rdx,%rax), %eax"},
		{func(a *x86) { a.store32Idx(rDX, rAX, r8) }, "movl %r8d, (%rdx,%rax)"},
		{func(a *x86) { a.load8Idx(rAX, rDX, rAX) }, "movzbl (%rdx,%rax), %eax"},
		{func(a *x86) { a.load16Idx(rAX, rDX, rAX) }, "movzwl (%rdx,%rax), %eax"},
		{func(a *x86) { a.store16Idx(rDX, rAX, r8) }, "movw %r8w, (%rdx,%rax)"},
		{func(a *x86) { a.store8Idx(rDX, rAX, r8) }, "movb %r8b, (%rdx,%rax)"},
		{func(a *x86) { a.aluRR(aluAdd, rAX, rCX) }, "addl %ecx, %eax"},
		{func(a *x86) { a.aluRR(aluXor, r8, rDX) }, "xorl %edx, %r8d"},
		{func(a *x86) { a.aluRM(aluSub, rAX, rBX, 0x14) }, "subl 20(%rbx), %eax"},
		{func(a *x86) { a.aluRM(aluAdc, rAX, rBX, 0x14) }, "adcl 20(%rbx), %eax"},
		{func(a *x86) { a.aluRI(aluAnd, rAX, 0xffff) }, "andl $65535, %eax"},
		{func(a *x86) { a.aluRI(aluCmp, rCX, 32) }, "cmpl $32, %ecx"},
		{func(a *x86) { a.aluRI(aluAdd, r11, 0xfffffffc) }, "addl $-4, %r11d"},
		{func(a *x86) { a.aluMI(aluSub, rBX, 0x28c, 1) }, "subl $1, 652(%rbx)"},
		{func(a *x86) { a.aluRI64(aluAdd, r9, 5) }, "addq $5, %r9"},
		{func(a *x86) { a.testMI(rBX, 0x280, 0x20000000) }, "testl $536870912, 640(%rbx)"},
		{func(a *x86) { a.unary(2, rAX) }, "notl %eax"},
		{func(a *x86) { a.unary(3, rAX) }, "negl %eax"},
		{func(a *x86) { a.unary(5, rCX) }, "imull %ecx"},
		{func(a *x86) { a.unary(6, rCX) }, "divl %ecx"},
		{func(a *x86) { a.imulRRI(rAX, rAX, 0x1234) }, "imull $4660, %eax, %eax"},
		{func(a *x86) { a.opRM(false, []byte{0x0f, 0xaf}, rAX, rBX, 0x18) }, "imull 24(%rbx), %eax"},
		{func(a *x86) { a.shiftRI(shRol, rAX, 3) }, "roll $3, %eax"},
		{func(a *x86) { a.shiftRI(shShr, rCX, 16) }, "shrl $16, %ecx"},
		{func(a *x86) { a.shiftRI(shSar, r8, 1) }, "sarl %r8d"},
		{func(a *x86) { a.shiftRCL(shShl, rAX) }, "shll %cl, %eax"},
		{func(a *x86) { a.bswap(rAX) }, "bswapl %eax"},
		{func(a *x86) { a.bswap(r8) }, "bswapl %r8d"},
		{func(a *x86) { a.rol16by8(rAX) }, "rolw $8, %ax"},
		{func(a *x86) { a.rol16by8(r8) }, "rolw $8, %r8w"},
		{func(a *x86) { a.movzx8(rCX, rCX) }, "movzbl %cl, %ecx"},
		{func(a *x86) { a.movsx16(rAX, rAX) }, "movswl %ax, %eax"},
		{func(a *x86) { a.movsx8(rAX, rAX) }, "movsbl %al, %eax"},
		{func(a *x86) { a.setcc(ccL, rCX) }, "setl %cl"},
		{func(a *x86) { a.setcc(ccE, rAX) }, "sete %al"},
		{func(a *x86) { a.cmovRR(ccAE, rAX, rDX) }, "cmovael %edx, %eax"},
		{func(a *x86) { a.bsr(rAX, rCX) }, "bsrl %ecx, %eax"},
		{func(a *x86) { a.jmpReg(rDX) }, "jmpq *%rdx"},
		{func(a *x86) { a.opRMI(true, []byte{0x8d}, rDX, rDX, rAX, 1, 0) }, "leaq (%rdx,%rax), %rdx"},
		{func(a *x86) { a.opRM(true, []byte{0x89}, rAX, rDX, 8) }, "movq %rax, 8(%rdx)"},
		{func(a *x86) { a.movLoad(rAX, rDX, 0) }, "movl (%rdx), %eax"},
		{func(a *x86) { a.movLoad(rAX, r13, 0) }, "movl (%r13), %eax"},
		{func(a *x86) { a.movLoad(rAX, r12, 4) }, "movl 4(%r12), %eax"},
		{func(a *x86) { a.movsdLoad(x1, rBX, 0x88) }, "movsd 136(%rbx), %xmm1"},
		{func(a *x86) { a.movsdStore(rBX, 0x180, x0) }, "movsd %xmm0, 384(%rbx)"},
		{func(a *x86) { a.sd(sseAdd, x0, x3) }, "addsd %xmm3, %xmm0"},
		{func(a *x86) { a.sd(sseSqrt, x0, x1) }, "sqrtsd %xmm1, %xmm0"},
		{func(a *x86) { a.cvtsd2ss(x2) }, "cvtsd2ss %xmm2, %xmm2"},
		{func(a *x86) { a.cvtss2sd(x2) }, "cvtss2sd %xmm2, %xmm2"},
		{func(a *x86) { a.movapd(x0, x5) }, "movapd %xmm5, %xmm0"},
		{func(a *x86) { a.xorpd(x0, x7) }, "xorpd %xmm7, %xmm0"},
		{func(a *x86) { a.ucomisd(x0, x1) }, "ucomisd %xmm1, %xmm0"},
		{func(a *x86) { a.cvttsd2si(rCX, x0) }, "cvttsd2si %xmm0, %ecx"},
		{func(a *x86) { a.movqToX(x1, rAX) }, "movq %rax, %xmm1"},
		{func(a *x86) { a.movqFromX(rAX, x6) }, "movq %xmm6, %rax"},
		{func(a *x86) { a.movdToX(x0, rAX) }, "movd %eax, %xmm0"},
		{func(a *x86) { a.movdFromX(r8, x0) }, "movd %xmm0, %r8d"},
		{func(a *x86) { a.vfmadd231sd(x0, x1, x2) }, "vfmadd231sd %xmm2, %xmm1, %xmm0"},
		{func(a *x86) { a.vfmadd231sd(x3, x2, x6) }, "vfmadd231sd %xmm6, %xmm2, %xmm3"},
		{func(a *x86) { a.bswap64(rAX) }, "bswapq %rax"},
		{func(a *x86) { a.bswap64(r8) }, "bswapq %r8"},
		{func(a *x86) { a.movRI64(rAX, 1<<63) }, "movabsq $-9223372036854775808, %rax"},
		{func(a *x86) { a.aluRI64(aluAnd, rAX, -0x8000000) }, "andq $-134217728, %rax"},
		{func(a *x86) { a.shiftRI64(shShr, rCX, 52) }, "shrq $52, %rcx"},
		{func(a *x86) { a.load64Idx(rAX, rDX, rAX) }, "movq (%rdx,%rax), %rax"},
		{func(a *x86) { a.store64Idx(rDX, rAX, r8) }, "movq %r8, (%rdx,%rax)"},
		{func(a *x86) { a.movStore64(rBX, 0x80, rAX) }, "movq %rax, 128(%rbx)"},
	}
	var in strings.Builder
	for _, c := range cases {
		a := &x86{}
		c.emit(a)
		for _, v := range a.b {
			fmt.Fprintf(&in, "0x%02x ", v)
		}
		in.WriteString("\n")
	}
	cmd := exec.Command("llvm-mc", "--disassemble", "-triple=x86_64")
	cmd.Stdin = strings.NewReader(in.String())
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	var got []string
	for _, l := range strings.Split(string(out), "\n") {
		if i := strings.Index(l, "#"); i >= 0 {
			l = l[:i]
		}
		l = strings.TrimSpace(l)
		if l == "" || strings.HasPrefix(l, ".") {
			continue
		}
		got = append(got, strings.Join(strings.Fields(l), " "))
	}
	if len(got) != len(cases) {
		t.Fatalf("got %d instructions for %d cases:\n%s", len(got), len(cases), out)
	}
	for i, c := range cases {
		if got[i] != c.want {
			t.Errorf("case %d: got %q, want %q", i, got[i], c.want)
		}
	}
}
