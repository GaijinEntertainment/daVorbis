// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once
#include <stdint.h>
#include <immintrin.h>

// No vecmath here: a source file built for AVX must not instance inline functions that other files share.
namespace
{
struct V4
{
  typedef __m128 T;
  static constexpr uint32_t N = 4;

  // VEX code takes an unaligned load as an operand: the aligned and the `u` forms are the same here.
  static inline T ld(const float *p) { return _mm_loadu_ps(p); }
  static inline void st(float *p, T v) { _mm_storeu_ps(p, v); }
  static inline T ldu(const float *p) { return _mm_loadu_ps(p); }
  static inline void stu(float *p, T v) { _mm_storeu_ps(p, v); }
  static inline T add(T a, T b) { return _mm_add_ps(a, b); }
  static inline T sub(T a, T b) { return _mm_sub_ps(a, b); }
  static inline T mul(T a, T b) { return _mm_mul_ps(a, b); }
  static inline T madd(T a, T b, T c) { return _mm_add_ps(_mm_mul_ps(a, b), c); }
  static inline T msub(T a, T b, T c) { return _mm_sub_ps(_mm_mul_ps(a, b), c); }
  static inline T nmsub(T a, T b, T c) { return _mm_sub_ps(c, _mm_mul_ps(a, b)); }
  static inline T ld_even(const float *p) { return _mm_shuffle_ps(_mm_loadu_ps(p), _mm_loadu_ps(p + 4), 0x88); }
  static inline T ld_odd_rev(const float *p) { return _mm_shuffle_ps(_mm_loadu_ps(p + 4), _mm_loadu_ps(p), 0x77); }
  static inline T even(T a, T b) { return _mm_shuffle_ps(a, b, 0x88); }
  static inline T odd(T a, T b) { return _mm_shuffle_ps(a, b, 0xDD); }
  static inline void transpose(T &a, T &b, T &c, T &d)
  {
    const T ab0 = _mm_unpacklo_ps(a, b), ab1 = _mm_unpackhi_ps(a, b), cd0 = _mm_unpacklo_ps(c, d), cd1 = _mm_unpackhi_ps(c, d);
    a = _mm_movelh_ps(ab0, cd0);
    b = _mm_movehl_ps(cd0, ab0);
    c = _mm_movelh_ps(ab1, cd1);
    d = _mm_movehl_ps(cd1, ab1);
  }
  static inline void st4(float *out, const uint16_t *pos, T a, T b, T c, T d)
  {
    transpose(a, b, c, d);
    _mm_storeu_ps(out + pos[0], a);
    _mm_storeu_ps(out + pos[1], b);
    _mm_storeu_ps(out + pos[2], c);
    _mm_storeu_ps(out + pos[3], d);
  }
};

struct V8
{
  typedef __m256 T;
  static constexpr uint32_t N = 8;

  static inline T ld(const float *p) { return _mm256_loadu_ps(p); }
  static inline void st(float *p, T v) { _mm256_storeu_ps(p, v); }
  static inline T splat(float v) { return _mm256_set1_ps(v); }
  static inline T iota() { return _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7); }
  static inline void st_int(int32_t *dst, T v, int32_t offset)
  {
    const __m256i i = _mm256_cvttps_epi32(v);
    const __m128i o = _mm_set1_epi32(offset);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm_add_epi32(_mm256_castsi256_si128(i), o));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 4), _mm_add_epi32(_mm256_extractf128_si256(i, 1), o));
  }
  static inline void scale(float *v, const int32_t *i, const float *t)
  {
    const T m = _mm256_setr_ps(t[i[0]], t[i[1]], t[i[2]], t[i[3]], t[i[4]], t[i[5]], t[i[6]], t[i[7]]);
    _mm256_storeu_ps(v, _mm256_mul_ps(_mm256_loadu_ps(v), m));
  }
  static inline T add(T a, T b) { return _mm256_add_ps(a, b); }
  static inline T sub(T a, T b) { return _mm256_sub_ps(a, b); }
  static inline T mul(T a, T b) { return _mm256_mul_ps(a, b); }
  static inline T madd(T a, T b, T c) { return _mm256_add_ps(_mm256_mul_ps(a, b), c); }
  static inline T msub(T a, T b, T c) { return _mm256_sub_ps(_mm256_mul_ps(a, b), c); }
  static inline T nmsub(T a, T b, T c) { return _mm256_sub_ps(c, _mm256_mul_ps(a, b)); }
  static inline T rev(T v)
  {
    const T r = _mm256_permute_ps(v, 0x1B);
    return _mm256_permute2f128_ps(r, r, 0x01);
  }
  // Two 4-float loads as the low and the high half.
  static inline T ld2(const float *lo, const float *hi)
  {
    return _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(lo)), _mm_loadu_ps(hi), 1);
  }
  static inline T ld_even(const float *p) { return _mm256_shuffle_ps(ld2(p, p + 8), ld2(p + 4, p + 12), 0x88); }
  static inline T ld_odd_rev(const float *p) { return _mm256_shuffle_ps(ld2(p + 12, p + 4), ld2(p + 8, p), 0x77); }
  static inline T zip_lo(T a, T b) { return _mm256_permute2f128_ps(_mm256_unpacklo_ps(a, b), _mm256_unpackhi_ps(a, b), 0x20); }
  static inline T zip_hi(T a, T b) { return _mm256_permute2f128_ps(_mm256_unpacklo_ps(a, b), _mm256_unpackhi_ps(a, b), 0x31); }
  static inline void st4(float *out, const uint16_t *pos, T a, T b, T c, T d)
  {
    const T ab0 = _mm256_unpacklo_ps(a, b), ab1 = _mm256_unpackhi_ps(a, b);
    const T cd0 = _mm256_unpacklo_ps(c, d), cd1 = _mm256_unpackhi_ps(c, d);
    const T g04 = _mm256_shuffle_ps(ab0, cd0, 0x44), g15 = _mm256_shuffle_ps(ab0, cd0, 0xEE);
    const T g26 = _mm256_shuffle_ps(ab1, cd1, 0x44), g37 = _mm256_shuffle_ps(ab1, cd1, 0xEE);
    _mm_storeu_ps(out + pos[0], _mm256_castps256_ps128(g04));
    _mm_storeu_ps(out + pos[1], _mm256_castps256_ps128(g15));
    _mm_storeu_ps(out + pos[2], _mm256_castps256_ps128(g26));
    _mm_storeu_ps(out + pos[3], _mm256_castps256_ps128(g37));
    _mm_storeu_ps(out + pos[4], _mm256_extractf128_ps(g04, 1));
    _mm_storeu_ps(out + pos[5], _mm256_extractf128_ps(g15, 1));
    _mm_storeu_ps(out + pos[6], _mm256_extractf128_ps(g26, 1));
    _mm_storeu_ps(out + pos[7], _mm256_extractf128_ps(g37, 1));
  }
  static inline void couple(float *magnitude, float *angle)
  {
    const T m = _mm256_loadu_ps(magnitude), a = _mm256_loadu_ps(angle), zero = _mm256_setzero_ps();
    const T t = _mm256_xor_ps(a, _mm256_andnot_ps(_mm256_cmp_ps(m, zero, _CMP_GT_OQ), _mm256_set1_ps(-0.f)));
    const T positive = _mm256_cmp_ps(a, zero, _CMP_GT_OQ);
    _mm256_storeu_ps(magnitude, _mm256_blendv_ps(_mm256_add_ps(m, t), m, positive));
    _mm256_storeu_ps(angle, _mm256_blendv_ps(m, _mm256_sub_ps(m, t), positive));
  }
};
typedef V8 VW;
} // namespace
