// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once
#include <vecmath/dag_vecMath.h>

namespace
{
// 4 lanes through vecmath: the path of each target that has no wider one.
struct V4
{
  typedef vec4f T;
  static constexpr uint32_t N = 4;

  // ld / st and each operation with no `u` form take 16-byte aligned memory: a legacy SSE operation takes an aligned
  // load as its operand, and an unaligned one costs an instruction more. ldu / stu take any address.
  static VECMATH_FINLINE T ld(const float *p) { return v_ld(p); }
  static VECMATH_FINLINE void st(float *p, T v) { v_st(p, v); }
  static VECMATH_FINLINE T ldu(const float *p) { return v_ldu(p); }
  static VECMATH_FINLINE void stu(float *p, T v) { v_stu(p, v); }
  static VECMATH_FINLINE T splat(float v) { return v_splats(v); }
  static VECMATH_FINLINE T iota() { return v_make_vec4f(0, 1, 2, 3); }
  // dst = trunc(v) + offset.
  static VECMATH_FINLINE void st_int(int32_t *dst, T v, int32_t offset) { v_stui(dst, v_addi(v_cvt_vec4i(v), v_splatsi(offset))); }
  static VECMATH_FINLINE void scale(float *v, const int32_t *i, const float *table)
  {
    v_st(v, v_mul(v_ld(v), v_make_vec4f(table[i[0]], table[i[1]], table[i[2]], table[i[3]])));
  }
  static VECMATH_FINLINE T add(T a, T b) { return v_add(a, b); }
  static VECMATH_FINLINE T sub(T a, T b) { return v_sub(a, b); }
  static VECMATH_FINLINE T mul(T a, T b) { return v_mul(a, b); }
  static VECMATH_FINLINE T madd(T a, T b, T c) { return v_madd(a, b, c); }
  static VECMATH_FINLINE T msub(T a, T b, T c) { return v_msub(a, b, c); }
  static VECMATH_FINLINE T nmsub(T a, T b, T c) { return v_nmsub(a, b, c); }
  static VECMATH_FINLINE T rev(T v) { return v_perm_yxwz(v_perm_zwxy(v)); }
  // p[0], p[2], p[4], p[6].
  static VECMATH_FINLINE T ld_even(const float *p) { return v_perm_xzac(v_ld(p), v_ld(p + 4)); }
  // p[7], p[5], p[3], p[1].
  static VECMATH_FINLINE T ld_odd_rev(const float *p) { return v_perm_yxwz(v_perm_ywbd(v_ld(p + 4), v_ld(p))); }
  static VECMATH_FINLINE T zip_lo(T a, T b) { return v_merge_hw(a, b); }
  static VECMATH_FINLINE T zip_hi(T a, T b) { return v_merge_lw(a, b); }
  static VECMATH_FINLINE T even(T a, T b) { return v_perm_xzac(a, b); }
  static VECMATH_FINLINE T odd(T a, T b) { return v_perm_ywbd(a, b); }
  static VECMATH_FINLINE void transpose(T &a, T &b, T &c, T &d) { v_mat44_transpose(a, b, c, d); }
  // out[pos[lane] + k] = lane of the k-th argument.
  static VECMATH_FINLINE void st4(float *out, const uint16_t *pos, T a, T b, T c, T d)
  {
    v_mat44_transpose(a, b, c, d);
    v_st(out + pos[0], a);
    v_st(out + pos[1], b);
    v_st(out + pos[2], c);
    v_st(out + pos[3], d);
  }
  static VECMATH_FINLINE void couple(float *magnitude, float *angle)
  {
    const T m = v_ld(magnitude), a = v_ld(angle), zero = v_zero();
    const T t = v_xor(a, v_andnot(v_cmp_gt(m, zero), v_cast_vec4f(v_splatsi(int(0x80000000)))));
    const T positive = v_cmp_gt(a, zero);
    v_st(magnitude, v_sel(v_add(m, t), m, positive));
    v_st(angle, v_sel(m, v_sub(m, t), positive));
  }
  static VECMATH_FINLINE void st_i16(int16_t *out, T v)
  {
    const T scaled = v_min(v_max(v_mul(v, v_splats(32768.f)), v_splats(-32768.f)), v_splats(32767.f));
    v_stui_half(out, v_packs(v_cvt_roundi_ieee(scaled)));
  }
};
typedef V4 VW;
} // namespace
