// Copyright (C) Gaijin Games KFT.  All rights reserved.
// One source file for each instruction set includes this after its vector layer: V4 (4 lanes) and VW (the widest).
#include "daVorbisInternal.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define DAVORBIS_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define DAVORBIS_NOINLINE __declspec(noinline)
#else
#define DAVORBIS_NOINLINE
#endif
#if defined(__GNUC__) || defined(__clang__)
#define DAVORBIS_UNLIKELY(x) __builtin_expect(!!(x), false)
#else
#define DAVORBIS_UNLIKELY(x) (x)
#endif

namespace
{
using namespace dacodecs;
using namespace dacodecs::davorbis_detail;

// LSB first. `cnt` low bits of `acc` are valid; `p` is the next byte to load. The bits read never pass `end`.
struct Bits
{
  uint64_t acc;
  const uint8_t *p, *end;
  uint32_t cnt;
  bool stop;
};

// Leaves REFILL_BITS or more. Reads up to DAVORBIS_PACKET_PAD bytes after `end`.
static inline void refill(Bits &b)
{
  uint64_t next;
  memcpy(&next, b.p, 8);
  b.acc |= next << b.cnt;
  b.p += (63 - b.cnt) >> 3;
  b.cnt |= 56;
}

static inline int64_t bits_left(const Bits &b) { return int64_t(b.end - b.p) * 8 + b.cnt; }

static inline uint32_t get(Bits &b, uint32_t n)
{
  refill(b);
  if (bits_left(b) < int64_t(n))
  {
    b.stop = true;
    return 0;
  }
  const uint32_t v = uint32_t(b.acc) & ((1u << n) - 1);
  b.acc >>= n;
  b.cnt -= n;
  return v;
}

// Takes values, not the reader: the reader state must stay in registers. Returns 0 if no code matches.
static DAVORBIS_NOINLINE uint32_t long_code(uint64_t acc, const LongCode *codes, uint32_t count)
{
  const uint32_t window = bit_reverse(uint32_t(acc));
  uint32_t lo = 0, hi = count;
  while (hi - lo > 1)
  {
    const uint32_t mid = (lo + hi) / 2;
    (codes[mid].code <= window ? lo : hi) = mid;
  }
  return hi && !((codes[lo].code ^ window) >> (32 - (codes[lo].entry & LEN_MASK))) ? codes[lo].entry : 0;
}

// Returns the table value of the next code word; the window must hold it (bk.burst code words fit after a refill).
// checked: a code word that passes the packet end stops the reader.
static inline uint32_t take(Bits &b, const uint16_t *fast, const Book &bk, bool checked)
{
  uint32_t value = fast[uint32_t(b.acc) & FAST_MASK], len = value & LEN_MASK;
  value >>= LEN_BITS;
  if (DAVORBIS_UNLIKELY(!len))
  {
    uint32_t entry = bk.sub[(value << SUB_BITS) + (uint32_t(b.acc >> FAST_BITS) & SUB_MASK)];
    if (!entry)
      entry = long_code(b.acc, bk.longCodes, bk.longCount);
    b.stop |= !entry;
    len = entry & LEN_MASK;
    value = entry >> LEN_BITS;
  }
  if (checked && bits_left(b) < int64_t(len))
  {
    b.stop = true;
    return 0;
  }
  b.acc >>= len;
  b.cnt -= len;
  return value;
}

static inline uint32_t code(Bits &b, const Book &bk)
{
  refill(b);
  return take(b, bk.fast, bk, true);
}

// The refill is not in the chain of each code word: one serves a burst.
// b must be a local of the caller, so that the reader state stays in registers. dim: bk.dim, a constant where the caller knows it.
// The book fields are locals: the compiler must take each store of `add` as a change of the book.
template <class Add>
static inline void code_run(Bits &b, const Book &bk, uint32_t dim, uint32_t codes, bool checked, Add add)
{
  const uint16_t *const fast = bk.fast;
  const float *const values = book_values(fast);
  const uint32_t burst = bk.burst;
  for (uint32_t i = 0; i < codes;)
  {
    refill(b);
    // A burst takes REFILL_BITS at most: only in the last bits of the packet a code word can pass the end.
    // The two forms are two loops in the source: the fast one must not depend on a loop split by the compiler.
    if (checked && bits_left(b) < int64_t(REFILL_BITS))
    {
      const float *v = values + take(b, fast, bk, true) * dim;
      if (b.stop)
        return;
      add(v);
      i++;
      continue;
    }
    for (const uint32_t stop = i + burst < codes ? i + burst : codes; i < stop; i++)
    {
      const float *v = values + take(b, fast, bk, false) * dim;
      if (b.stop)
        return;
      add(v);
    }
  }
}

// y: the final value of each post, with POST_UNUSED for a post that the curve does not pass through.
static DAVORBIS_NOINLINE bool floor_decode(Bits &reader, const DaVorbisSetup &s, const Floor1 &f, uint16_t *y)
{
  static const uint16_t RANGE[4] = {256, 128, 86, 64};
  static const uint8_t RANGE_BITS[4] = {8, 7, 7, 6};
  if (!get(reader, 1))
    return false;
  Bits b = reader;
  uint32_t raw[MAX_POSTS], at = 2;
  raw[0] = get(b, RANGE_BITS[f.multiplier - 1]);
  raw[1] = get(b, RANGE_BITS[f.multiplier - 1]);
  for (uint32_t i = 0; i < f.partitions; i++)
  {
    const uint32_t cls = f.partClass[i], subBits = f.classSubBits[cls];
    uint32_t sub = subBits ? code(b, s.books[f.classMaster[cls]]) : 0;
    for (uint32_t j = 0; j < f.classDim[cls]; j++, sub >>= subBits)
    {
      const int32_t book = f.subBook[cls][sub & ((1u << subBits) - 1)];
      raw[at++] = book >= 0 ? code(b, s.books[book]) : 0;
    }
  }
  reader = b;
  if (b.stop)
    return false;
  const int32_t range = RANGE[f.multiplier - 1];
  y[0] = uint16_t(raw[0]);
  y[1] = uint16_t(raw[1]);
  for (uint32_t i = 2; i < f.values; i++)
  {
    const uint32_t lo = f.lowNb[i], hi = f.highNb[i];
    const int32_t y0 = y[lo] & POST_VALUE, dy = (y[hi] & POST_VALUE) - y0;
    const int32_t slope = int32_t((uint64_t(abs(dy)) * (f.x[i] - f.x[lo]) * f.recip[i]) >> RECIP_BITS);
    const int32_t predicted = dy < 0 ? y0 - slope : y0 + slope;
    // No branch on the values: they are as good as random, and each wrong guess costs more than this whole step.
    const int32_t value = int32_t(raw[i]), highRoom = range - predicted, lowRoom = predicted;
    const int32_t far = highRoom > lowRoom ? value - lowRoom : highRoom - value - 1, near = (value >> 1) ^ -(value & 1);
    const int32_t result = predicted + (value >= 2 * (highRoom < lowRoom ? highRoom : lowRoom) ? far : near);
    const uint16_t keep = value ? POST_VALUE : uint16_t(0xFFFF);
    y[i] = value ? uint16_t(result & POST_VALUE) : uint16_t(predicted | POST_UNUSED);
    y[lo] &= keep;
    y[hi] &= keep;
  }
  return true;
}

// Multiplies the first `limit` bins of the spectrum by the floor curve: the integer line of the specification between
// the used posts, each value through the dB table. The line value at step k of a segment is ly + trunc(k dy / adx),
// here trunc(k (dy / adx) + 0.5 / adx): the 0.5 keeps the float value away from an integer, so trunc is exact.
// levels: limit + VW::N values of scratch.
static DAVORBIS_NOINLINE void floor_apply(float *v, const Floor1 &f, const uint16_t *y, uint32_t limit, int32_t *levels)
{
  auto level = [&](uint32_t post) {
    const uint32_t l = uint32_t(y[post]) * f.multiplier;
    return int32_t(l < 255 ? l : 255);
  };
  uint32_t lx = 0;
  int32_t ly = level(0);
  for (uint32_t i = 1; i < f.values && lx < limit; i++)
  {
    const uint32_t post = f.sorted[i];
    if (y[post] & POST_UNUSED)
      continue;
    const uint32_t hx = f.x[post], stop = hx < limit ? hx : limit;
    const int32_t hy = level(post);
    const float inv = 1.f / float(hx - lx);
    const VW::T step = VW::splat(float(hy - ly) * inv), half = VW::splat(hy < ly ? -0.5f * inv : 0.5f * inv);
    VW::T k = VW::iota();
    for (uint32_t x = lx; x < stop; x += VW::N, k = VW::add(k, VW::splat(float(VW::N))))
      VW::st_int(levels + x, VW::madd(k, step, half), ly);
    lx = hx;
    ly = hy;
  }
  for (; lx < limit; lx++)
    levels[lx] = ly;
  for (uint32_t k = 0; k < limit; k += VW::N)
    VW::scale(v + k, levels + k, FLOOR1_INVERSE_DB);
}

// A partition shape that has no loop of its own in residue_decode: a type 0 residue, a type 2 residue of more than
// 2 channels, a vector size other than 2, 4, 8. Takes and returns the reader by value: a reference would keep the
// reader of the caller in memory.
static DAVORBIS_NOINLINE Bits partition_any(Bits b, const Book &bk, uint32_t type, float *const *vec, uint32_t stream, uint32_t count,
  uint32_t offset, uint32_t n, bool checked)
{
  const uint32_t dim = bk.dim, codes = n / dim;
  float *d = vec[stream] + offset;
  if (type == 2 && count > 1)
  {
    uint32_t c = offset % count, x = offset / count;
    code_run(b, bk, dim, codes, checked, [&](const float *v) {
      for (uint32_t k = 0; k < dim; k++)
      {
        vec[c][x] += v[k];
        x += ++c == count;
        c = c == count ? 0 : c;
      }
    });
  }
  else if (type == 0)
    code_run(b, bk, dim, codes, checked, [&](const float *v) {
      for (uint32_t k = 0; k < dim; k++)
        d[k * codes] += v[k];
      d++;
    });
  else
    code_run(b, bk, dim, codes, checked, [&](const float *v) {
      for (uint32_t k = 0; k < dim; k++)
        *d++ += v[k];
    });
  return b;
}

// decode[i]: the channel takes residue. The vectors are zero at the start.
// The reader is a local, and the loops of the usual partition shapes are in this function: no call for each partition.
static DAVORBIS_NOINLINE void residue_decode(Bits &reader, const DaVorbisSetup &s, const Residue &r, float *const *vec,
  const bool *decode, uint32_t count, uint32_t n2)
{
  const bool joint = r.type == 2;
  bool any = false;
  for (uint32_t i = 0; i < count; i++)
    any |= decode[i];
  if (!any)
    return;
  const uint32_t streams = joint ? 1 : count, size = joint ? n2 * count : n2;
  const uint32_t begin = r.begin < size ? r.begin : size, end = r.end < size ? r.end : size;
  const uint32_t parts = (end - begin) / r.partSize, stride = (parts + r.classWords - 1) / r.classWords * r.classWords;
  // pairs: a type 2 residue of 2 channels, where the values of a vector go to the two channels in turn.
  // The loops of the pairs keep one pointer, and load the two channels before they store: fewer operations.
  const bool pairs = joint && count == 2 && !((begin | r.partSize) & 1);
  const ptrdiff_t gap = pairs ? vec[1] - vec[0] : 0;
  const uint32_t layout = pairs ? 3 : r.type == 0 || (joint && count > 1) ? 6 : 0;
  const Book &classBook = s.books[r.classBook];
  uint8_t classes[MAX_CLASS_SLOTS];
  Bits b = reader;
  for (uint32_t pass = 0; pass < r.passes && !b.stop; pass++)
    for (uint32_t part = 0; part < parts && !b.stop;)
    {
      for (uint32_t j = 0; pass == 0 && j < streams; j++)
        if (joint || decode[j])
        {
          const uint32_t word = code(b, classBook);
          for (uint32_t i = 0; i < r.classWords; i++)
            classes[j * stride + part + i] = r.classDigits[word * r.classWords + i];
        }
      for (uint32_t i = 0; i < r.classWords && part < parts && !b.stop; i++, part++)
        for (uint32_t j = 0; j < streams && !b.stop; j++)
        {
          if (!joint && !decode[j])
            continue;
          const ResidueBook rb = r.books[classes[j * stride + part] * 8 + pass];
          if (rb.book < 0)
            continue;
          const Book &bk = s.books[rb.book];
          const uint32_t offset = begin + part * r.partSize, codes = r.partSize >> (bk.kind + 1);
          const bool checked = bits_left(b) < int64_t(rb.bits);
          float *a = pairs ? vec[0] + offset / 2 : vec[j] + offset;
          switch (bk.kind < 3 ? layout + bk.kind : 6)
          {
            case 0:
              code_run(b, bk, 2, codes, checked, [&](const float *v) {
                a[0] += v[0], a[1] += v[1];
                a += 2;
              });
              break;
            case 1:
              code_run(b, bk, 4, codes, checked, [&](const float *v) {
                V4::stu(a, V4::add(V4::ldu(a), V4::ldu(v)));
                a += 4;
              });
              break;
            case 2:
              code_run(b, bk, 8, codes, checked, [&](const float *v) {
                V4::stu(a, V4::add(V4::ldu(a), V4::ldu(v)));
                V4::stu(a + 4, V4::add(V4::ldu(a + 4), V4::ldu(v + 4)));
                a += 8;
              });
              break;
            case 3:
              code_run(b, bk, 2, codes, checked, [&](const float *v) {
                const float x = a[0] + v[0], y = a[gap] + v[1];
                a[0] = x, a[gap] = y;
                a++;
              });
              break;
            case 4:
              code_run(b, bk, 4, codes, checked, [&](const float *v) {
                const float x0 = a[0] + v[0], y0 = a[gap] + v[1], x1 = a[1] + v[2], y1 = a[gap + 1] + v[3];
                a[0] = x0, a[gap] = y0, a[1] = x1, a[gap + 1] = y1;
                a += 2;
              });
              break;
            case 5:
              code_run(b, bk, 8, codes, checked, [&](const float *v) {
                const V4::T lo = V4::ldu(v), hi = V4::ldu(v + 4);
                const V4::T x = V4::add(V4::ldu(a), V4::even(lo, hi)), y = V4::add(V4::ldu(a + gap), V4::odd(lo, hi));
                V4::stu(a, x);
                V4::stu(a + gap, y);
                a += 4;
              });
              break;
            default: b = partition_any(b, bk, r.type, vec, j, count, offset, r.partSize, checked);
          }
        }
    }
  reader = b;
}

// x: n2 spectrum values. Makes t[j] = (x[n2 - 1 - 2j] - i x[2j]) w[j] and does the first radix-4 pass on it.
// The 4 members of a group are t[j + q size / 4]: the lanes are groups of successive j, so the inputs are plain loads
// and only the store of each group goes to its digit-reverse place.
template <class V>
static DAVORBIS_NOINLINE void fft_first_pass(const float *x, const BlockTables &bt, float *re, float *im)
{
  typedef typename V::T T;
  const uint32_t size = bt.n / 4, quarter = size / 4;
  const float *wr = bt.postTw, *wi = wr + size;
  for (uint32_t j = 0; j < quarter; j += V::N)
  {
    T tr[4], ti[4];
    for (uint32_t q = 0; q < 4; q++)
    {
      const uint32_t at = j + q * quarter;
      const T c = V::ld_even(x + 2 * at), a = V::ld_odd_rev(x + 2 * size - 2 * at - 2 * V::N);
      const T r = V::ld(wr + at), i = V::ld(wi + at);
      tr[q] = V::madd(a, r, V::mul(c, i));
      ti[q] = V::msub(a, i, V::mul(c, r));
    }
    const T s0r = V::add(tr[0], tr[2]), s0i = V::add(ti[0], ti[2]), s1r = V::sub(tr[0], tr[2]), s1i = V::sub(ti[0], ti[2]);
    const T s2r = V::add(tr[1], tr[3]), s2i = V::add(ti[1], ti[3]), s3r = V::sub(tr[1], tr[3]), s3i = V::sub(ti[1], ti[3]);
    V::st4(re, bt.groupPos + j, V::add(s0r, s2r), V::add(s1r, s3i), V::sub(s0r, s2r), V::sub(s1r, s3i));
    V::st4(im, bt.groupPos + j, V::add(s0i, s2i), V::sub(s1i, s3r), V::sub(s0i, s2i), V::add(s1i, s3r));
  }
}

// Joins 4 sub-transforms of `stride` points into one, for each group of 4 * stride points.
// tw: re and im of the 3 twiddles of each point, 6 arrays of `stride` values.
template <class V>
static DAVORBIS_NOINLINE void fft_radix4(float *re, float *im, const float *tw, uint32_t size, uint32_t stride)
{
  typedef typename V::T T;
  for (uint32_t group = 0; group < size; group += 4 * stride)
    for (uint32_t k = 0; k < stride; k += V::N)
    {
      float *r = re + group + k, *i = im + group + k;
      const float *w = tw + k;
      const T ar = V::ld(r), ai = V::ld(i);
      T xr = V::ld(r + stride), xi = V::ld(i + stride);
      const T br = V::nmsub(xi, V::ld(w + stride), V::mul(xr, V::ld(w))), bi = V::madd(xr, V::ld(w + stride), V::mul(xi, V::ld(w)));
      xr = V::ld(r + 2 * stride), xi = V::ld(i + 2 * stride);
      const T cr = V::nmsub(xi, V::ld(w + 3 * stride), V::mul(xr, V::ld(w + 2 * stride)));
      const T ci = V::madd(xr, V::ld(w + 3 * stride), V::mul(xi, V::ld(w + 2 * stride)));
      xr = V::ld(r + 3 * stride), xi = V::ld(i + 3 * stride);
      const T dr = V::nmsub(xi, V::ld(w + 5 * stride), V::mul(xr, V::ld(w + 4 * stride)));
      const T di = V::madd(xr, V::ld(w + 5 * stride), V::mul(xi, V::ld(w + 4 * stride)));
      const T s0r = V::add(ar, cr), s0i = V::add(ai, ci), s1r = V::sub(ar, cr), s1i = V::sub(ai, ci);
      const T s2r = V::add(br, dr), s2i = V::add(bi, di), s3r = V::sub(br, dr), s3i = V::sub(bi, di);
      V::st(r, V::add(s0r, s2r));
      V::st(i, V::add(s0i, s2i));
      V::st(r + stride, V::add(s1r, s3i));
      V::st(i + stride, V::sub(s1i, s3r));
      V::st(r + 2 * stride, V::sub(s0r, s2r));
      V::st(i + 2 * stride, V::sub(s0i, s2i));
      V::st(r + 3 * stride, V::sub(s1r, s3i));
      V::st(i + 3 * stride, V::add(s1i, s3r));
    }
}

static DAVORBIS_NOINLINE void fft_passes(float *re, float *im, const BlockTables &bt)
{
  const uint32_t size = bt.n / 4;
  const float *tw = bt.passTw;
  fft_radix4<V4>(re, im, tw, size, 4);
  tw += 24;
  uint32_t stride = 16;
  for (; stride * 4 <= size; tw += 6 * stride, stride *= 4)
    fft_radix4<VW>(re, im, tw, size, stride);
  for (uint32_t k = 0; stride < size && k < stride; k += VW::N)
  {
    const VW::T xr = VW::ld(re + stride + k), xi = VW::ld(im + stride + k), ar = VW::ld(re + k), ai = VW::ld(im + k);
    const VW::T br = VW::nmsub(xi, VW::ld(tw + stride + k), VW::mul(xr, VW::ld(tw + k)));
    const VW::T bi = VW::madd(xr, VW::ld(tw + stride + k), VW::mul(xi, VW::ld(tw + k)));
    VW::st(re + k, VW::add(ar, br));
    VW::st(im + k, VW::add(ai, bi));
    VW::st(re + stride + k, VW::sub(ar, br));
    VW::st(im + stride + k, VW::sub(ai, bi));
  }
}

// The middle half of the IMDCT output from the FFT result T: with c[m] = T[m] w[m], the left quarter is
// L[2m] = Re c[m], L[2m + 1] = Im c[size - 1 - m], and the right quarter the same from m = size / 2 on, index wrapped.
// This writes out[2i] = Re c[first + i], out[2i + 1] = Im c[last - i], for i < pairs.
static DAVORBIS_NOINLINE void fft_post(const float *re, const float *im, const float *tw, uint32_t size, uint32_t first, uint32_t last,
  float *out, uint32_t pairs)
{
  const float *twi = tw + size;
  for (uint32_t i = 0; i < pairs; i += VW::N, out += 2 * VW::N)
  {
    const uint32_t a = first + i, q = last + 1 - VW::N - i;
    const VW::T even = VW::nmsub(VW::ld(im + a), VW::ld(twi + a), VW::mul(VW::ld(re + a), VW::ld(tw + a)));
    const VW::T odd = VW::rev(VW::madd(VW::ld(re + q), VW::ld(twi + q), VW::mul(VW::ld(im + q), VW::ld(tw + q))));
    VW::st(out, VW::zip_lo(even, odd));
    VW::st(out + VW::N, VW::zip_hi(even, odd));
  }
}

// The first h values of the left quarter with the window and the overlap-add, in one pass: L is not stored.
// tail: h values of the last block's right half, rising in time. win: the window of 4h points. Writes 2h frames:
// tail[j] and L[h - 1 - j] give out[j] and out[2h - 1 - j] (the mirror of L is its negative).
static DAVORBIS_NOINLINE void post_overlap(const float *re, const float *im, const float *tw, uint32_t size, const float *tail,
  const float *win, float *out, uint32_t h)
{
  const float *twi = tw + size;
  auto lap = [&](uint32_t j, VW::T l) {
    const uint32_t m = 2 * h - VW::N - j;
    const VW::T t = VW::ld(tail + j);
    VW::st(out + j, VW::nmsub(VW::rev(l), VW::ld(win + j), VW::mul(t, VW::ld(win + 2 * h + j))));
    VW::st(out + m, VW::madd(VW::rev(t), VW::ld(win + 2 * h + m), VW::mul(l, VW::ld(win + m))));
  };
  for (uint32_t j = 0; j < h; j += 2 * VW::N)
  {
    const uint32_t a = (h - j) / 2 - VW::N, q = size - VW::N - a;
    const VW::T even = VW::nmsub(VW::ld(im + a), VW::ld(twi + a), VW::mul(VW::ld(re + a), VW::ld(tw + a)));
    const VW::T odd = VW::rev(VW::madd(VW::ld(re + q), VW::ld(twi + q), VW::mul(VW::ld(im + q), VW::ld(tw + q))));
    lap(j, VW::zip_hi(even, odd));
    lap(j + VW::N, VW::zip_lo(even, odd));
  }
}

// work: the spectrum on entry, the ready frames on exit. prev: the tables of the last block, null after a restart.
static void transform(float *work, float *tail, const BlockTables &bt, const BlockTables *prev, float *scratch)
{
  const uint32_t size = bt.n / 4;
  float *re = scratch, *im = scratch + size;
  if (size / 4 < VW::N)
    fft_first_pass<V4>(work, bt, re, im);
  else
    fft_first_pass<VW>(work, bt, re, im);
  fft_passes(re, im, bt);
  if (prev)
  {
    // A longer block before: its flat part comes first. A longer block now: its flat part comes last.
    const uint32_t prevSize = prev->n / 4, h = prevSize < size ? prevSize : size;
    if (prevSize > h)
      memcpy(work, tail, (prevSize - h) * sizeof(float));
    post_overlap(re, im, bt.postTw, size, tail + prevSize - h, (prevSize < size ? prev : &bt)->window, work + prevSize - h, h);
    if (size > h)
      fft_post(re, im, bt.postTw, size, h / 2, size - 1 - h / 2, work + 2 * h, (size - h) / 2);
  }
  fft_post(re, im, bt.postTw, size, size / 2, size / 2 - 1, tail, size / 2);
}

static uint32_t decode_packet(DaVorbisDecoder *dec, const uint8_t *packet, uint32_t bytes, DaVorbisStatus *status)
{
  const DaVorbisSetup &s = *dec->setup;
  const uint32_t channels = dec->channels, stride = dec->stride;
  Bits b = {0, packet, packet + bytes, 0, false};
  const uint32_t type = get(b, 1), modeIndex = get(b, s.modeBits);
  *status = DaVorbisStatus::BAD_PACKET;
  if (b.stop || type || modeIndex >= s.modeCount)
    return 0;
  const Mode &mode = s.modes[modeIndex];
  const BlockTables &bt = s.tables[mode.blockFlag];
  const Mapping &map = s.mappings[mode.mapping];
  const uint32_t n2 = bt.n / 2;
  get(b, mode.blockFlag ? 2 : 0);
  if (b.stop)
    return 0;

  uint16_t posts[DAVORBIS_MAX_CHANNELS][MAX_POSTS];
  bool hasFloor[DAVORBIS_MAX_CHANNELS], hasResidue[DAVORBIS_MAX_CHANNELS];
  uint32_t limit[DAVORBIS_MAX_CHANNELS];
  for (uint32_t c = 0; c < channels; c++)
    hasResidue[c] = hasFloor[c] = floor_decode(b, s, s.floors[map.floor[map.mux[c]]], posts[c]);
  for (uint32_t i = 0; i < map.couplingSteps; i++)
    if (hasResidue[map.coupling[i * 2]] || hasResidue[map.coupling[i * 2 + 1]])
      hasResidue[map.coupling[i * 2]] = hasResidue[map.coupling[i * 2 + 1]] = true;

  for (uint32_t sub = 0; sub < map.submaps; sub++)
  {
    const Residue &r = s.residues[map.residue[sub]];
    float *vec[DAVORBIS_MAX_CHANNELS];
    bool decode[DAVORBIS_MAX_CHANNELS];
    uint32_t count = 0;
    for (uint32_t c = 0; c < channels; c++)
      if (map.mux[c] == sub)
      {
        vec[count] = dec->work + c * stride;
        decode[count++] = hasResidue[c];
        memset(dec->work + c * stride, 0, n2 * sizeof(float));
      }
    // count is 0 for a submap that no channel uses.
    const uint32_t end = r.type == 2 && count ? (r.end + count - 1) / count : r.end, bins = ((end < n2 ? end : n2) + 7) & ~7u;
    for (uint32_t c = 0; c < channels; c++)
      if (map.mux[c] == sub)
        limit[c] = bins;
    residue_decode(b, s, r, vec, decode, count, n2);
  }
  for (uint32_t i = map.couplingSteps; i-- > 0;)
  {
    const uint32_t m = map.coupling[i * 2], a = map.coupling[i * 2 + 1];
    // The two channels can be in submaps with other residue ends: the coupling gives each the bins of the other.
    const uint32_t bins = limit[m] = limit[a] = limit[m] > limit[a] ? limit[m] : limit[a];
    float *magnitude = dec->work + m * stride, *angle = dec->work + a * stride;
    for (uint32_t k = 0; k < bins; k += VW::N)
      VW::couple(magnitude + k, angle + k);
  }

  // One buffer: the floor levels of a channel are not necessary when its transform starts.
  alignas(32) float stack[STACK_BLOCK / 2 + 8];
  float *scratch = dec->scratch ? dec->scratch : stack;
  const BlockTables *prev = dec->prevBlock ? &s.tables[dec->prevBlock == s.tables[1].n] : nullptr;
  for (uint32_t c = 0; c < channels; c++)
  {
    float *work = dec->work + c * stride;
    if (hasFloor[c])
      floor_apply(work, s.floors[map.floor[map.mux[c]]], posts[c], limit[c], reinterpret_cast<int32_t *>(scratch));
    else // a type 2 residue gives values to a channel with no floor too
      memset(work, 0, n2 * sizeof(float));
    transform(work, dec->tail + c * dec->quarter, bt, prev, scratch);
  }
  dec->ready = dec->prevBlock ? (dec->prevBlock + bt.n) / 4 : 0;
  dec->prevBlock = bt.n;
  dec->readPos = 0;
  *status = DaVorbisStatus::OK;
  return dec->ready;
}
} // namespace
