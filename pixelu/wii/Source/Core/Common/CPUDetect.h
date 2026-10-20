// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

enum class CPUVendor
{
  Intel,
  AMD,
  ARM,
  Other,
};

// Host CPU features, filled in once at start-up (see the global cpu_info).
struct CPUInfo
{
  CPUVendor vendor = CPUVendor::Other;

  std::string cpu_id;      // "family:model:stepping" in hex
  std::string model_name;  // marketing name, e.g. from the CPUID brand string

  bool HTT = false;  // real simultaneous multithreading
  int num_cores = 0;  // logical processors

  bool bSSE3 = false;
  bool bSSSE3 = false;
  bool bSSE4_1 = false;
  bool bSSE4_2 = false;
  bool bLZCNT = false;
  bool bAVX = false;  // and enabled by the OS
  bool bBMI1 = false;
  bool bBMI2 = false;
  // BMI2 with fast PDEP/PEXT (false on AMD family 17h, where they take hundreds of cycles).
  bool bBMI2FastParallelBitOps = false;
  bool bFMA = false;
  bool bFMA4 = false;
  bool bAES = false;
  bool bMOVBE = false;
  // Some mode flushes denormal inputs and outputs to (signed) zero.
  bool bFlushToZero = false;
  bool bAtom = false;
  bool bCRC32 = false;
  bool bSHA1 = false;
  bool bSHA2 = false;

  // ARMv8 alternate floating-point behaviour (FEAT_AFP).
  bool bAFP = false;

  // Runs Detect().
  explicit CPUInfo();

  // "<model_name>,<cpu_id>,<FLAG>,<FLAG>,..." with the flags worth reporting that are present.
  // model_name and cpu_id may be empty and contain no commas.
  std::string Summarize();

private:
  void Detect();
};

extern CPUInfo cpu_info;
