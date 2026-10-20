// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/x64ABI.h"

#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"

using namespace Gen;

// Frame built by ABI_PushRegistersAndAdjustStack, from high to low addresses:
//
//   return address / caller's frame      (RSP % 16 == rsp_alignment % 16 here)
//   pushed general purpose registers     (RBP first, then the others in ascending order)
//   padding to 16 bytes                  (only when XMM registers are saved)
//   saved XMM registers, 16 bytes each   (at RSP + xmm_offset)
//   needed_frame_size bytes for the caller
//   shadow space                         (32 bytes on Windows, at RSP)
//   padding so that the final RSP is 16-byte aligned
//
// "subtraction" is everything below the pushes.
XEmitter::FrameLayout XEmitter::CalculateFrame(BitSet32 mask, size_t rsp_alignment,
                                               size_t needed_frame_size)
{
#ifdef _WIN32
  constexpr size_t shadow = 0x20;
#else
  constexpr size_t shadow = 0;
#endif

  const size_t gpr_bytes = 8 * static_cast<size_t>((mask & ABI_ALL_GPRS).Count());
  const size_t xmm_count = static_cast<size_t>((mask & ABI_ALL_FPRS).Count());

  // The alignment (mod 16) of RSP after the pushes.
  const size_t misalign_after_pushes = rsp_alignment - gpr_bytes;

  // MOVAPS needs the XMM save area 16-byte aligned.
  const size_t xmm_pad = xmm_count != 0 ? (misalign_after_pushes & 0xF) : 0;
  const size_t xmm_area_end = xmm_pad + 16 * xmm_count;

  size_t subtraction = xmm_area_end + needed_frame_size + shadow;
  subtraction += (misalign_after_pushes - subtraction) & 0xF;

  return FrameLayout{shadow, subtraction, subtraction - xmm_area_end};
}

size_t XEmitter::ABI_PushRegistersAndAdjustStack(BitSet32 mask, size_t rsp_alignment,
                                                 size_t needed_frame_size)
{
  mask[RSP] = false;
  const FrameLayout frame = CalculateFrame(mask, rsp_alignment, needed_frame_size);

  // RBP goes first and is made a frame pointer, which debuggers and profilers like.
  if (mask[RBP])
  {
    PUSH(RBP);
    MOV(64, R(RBP), R(RSP));
  }
  for (int reg = 0; reg < 16; reg++)
  {
    if (reg != RBP && mask[reg])
      PUSH(static_cast<X64Reg>(reg));
  }

  if (frame.subtraction != 0)
    SUB(64, R(RSP), Imm32(static_cast<u32>(frame.subtraction)));

  size_t offset = frame.xmm_offset;
  for (int xmm = 0; xmm < 16; xmm++)
  {
    if (!mask[16 + xmm])
      continue;
    MOVAPD(MDisp(RSP, static_cast<int>(offset)), static_cast<X64Reg>(xmm));
    offset += 16;
  }

  return frame.shadow;
}

void XEmitter::ABI_PopRegistersAndAdjustStack(BitSet32 mask, size_t rsp_alignment,
                                              size_t needed_frame_size)
{
  mask[RSP] = false;
  const FrameLayout frame = CalculateFrame(mask, rsp_alignment, needed_frame_size);

  size_t offset = frame.xmm_offset;
  for (int xmm = 0; xmm < 16; xmm++)
  {
    if (!mask[16 + xmm])
      continue;
    MOVAPD(static_cast<X64Reg>(xmm), MDisp(RSP, static_cast<int>(offset)));
    offset += 16;
  }

  if (frame.subtraction != 0)
    ADD(64, R(RSP), Imm32(static_cast<u32>(frame.subtraction)));

  // Reverse order of the pushes; RBP last.
  for (int reg = 15; reg >= 0; reg--)
  {
    if (reg != RBP && mask[reg])
      POP(static_cast<X64Reg>(reg));
  }
  if (mask[RBP])
    POP(RBP);
}

void XEmitter::MOVTwo(int bits, X64Reg dst1, X64Reg src1, s32 offset, X64Reg dst2, X64Reg src2)
{
  // dst1 = src1 + offset
  const auto first = [&] {
    if (dst1 != src1)
    {
      if (offset != 0)
        LEA(bits, dst1, MDisp(src1, offset));
      else
        MOV(bits, R(dst1), R(src1));
    }
    else if (offset != 0)
    {
      ADD(bits, R(dst1), Imm32(static_cast<u32>(offset)));
    }
  };
  // dst2 = src2
  const auto second = [&] {
    if (dst2 != src2)
      MOV(bits, R(dst2), R(src2));
  };

  if (dst1 == src2 && dst2 == src1)
  {
    // A swap: neither move can go first.
    XCHG(bits, R(src1), R(src2));
    if (offset != 0)
      ADD(bits, R(dst1), Imm32(static_cast<u32>(offset)));
  }
  else if (src2 == dst1)
  {
    // Writing dst1 first would destroy src2.
    second();
    first();
  }
  else
  {
    first();
    second();
  }
}
