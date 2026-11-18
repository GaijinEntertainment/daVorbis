// Copyright (C) Gaijin Games KFT.  All rights reserved.
#include "daVorbisInternal.h"
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define DAVORBIS_XSAVE_TARGET
static void cpuid_leaf(uint32_t leaf, uint32_t *r) { __cpuidex(reinterpret_cast<int *>(r), int(leaf), 0); }
#else
#include <cpuid.h>
#define DAVORBIS_XSAVE_TARGET __attribute__((target("xsave")))
static void cpuid_leaf(uint32_t leaf, uint32_t *r) { __cpuid_count(leaf, 0, r[0], r[1], r[2], r[3]); }
#endif

namespace dacodecs
{
namespace davorbis_detail
{
// A console build targets one known CPU.
#if defined(__AVX2__)
static constexpr DaVorbisIsa TARGET_ISA = DaVorbisIsa::AVX2;
#elif defined(__AVX__)
static constexpr DaVorbisIsa TARGET_ISA = DaVorbisIsa::AVX;
#else
static constexpr DaVorbisIsa TARGET_ISA = DaVorbisIsa::BASE;
#endif

static DAVORBIS_XSAVE_TARGET DaVorbisIsa cpu_isa()
{
  constexpr uint32_t FMA = 1u << 12, OSXSAVE = 1u << 27, AVX = 1u << 28, SSE_AND_AVX_STATE = 6;
  constexpr uint32_t BMI1_AVX2_BMI2 = (1u << 3) | (1u << 5) | (1u << 8), LZCNT = 1u << 5;
  uint32_t leaf1[4], leaf7[4], ext1[4];
  cpuid_leaf(1, leaf1);
  // XGETBV faults unless OSXSAVE is set.
  if ((leaf1[2] & (OSXSAVE | AVX)) != (OSXSAVE | AVX) || (_xgetbv(0) & SSE_AND_AVX_STATE) != SSE_AND_AVX_STATE)
    return DaVorbisIsa::BASE;
  cpuid_leaf(7, leaf7);
  cpuid_leaf(0x80000001, ext1);
  // AVX2 file also uses FMA, BMI, LZCNT.
  const bool avx2 = (leaf7[1] & BMI1_AVX2_BMI2) == BMI1_AVX2_BMI2 && (leaf1[2] & FMA) && (ext1[2] & LZCNT);
  return avx2 ? DaVorbisIsa::AVX2 : DaVorbisIsa::AVX;
}

// AUTO until the static init of this file: a setup before it takes the base decode.
static const DaVorbisIsa AUTO_ISA = TARGET_ISA != DaVorbisIsa::BASE ? TARGET_ISA : cpu_isa();

DecodePacket isa_decode(DaVorbisIsa isa)
{
  if (isa == DaVorbisIsa::AUTO)
    isa = AUTO_ISA;
  return isa == DaVorbisIsa::AVX2 ? DECODE_AVX2 : isa == DaVorbisIsa::AVX ? DECODE_AVX : DECODE_BASE;
}
} // namespace davorbis_detail
} // namespace dacodecs
