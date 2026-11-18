// Copyright (C) Gaijin Games KFT.  All rights reserved.
#include "daVorbisSimd4.h"
#include "daVorbisDecode.inl"

namespace dacodecs
{
namespace davorbis_detail
{
extern const DecodePacket DECODE_BASE = decode_packet;

// store(first value index, 4 values in output order) for 1, 2 or 4 channels; the caller does the frames past the returned count.
template <class Store>
static inline uint32_t interleave(const DaVorbisDecoder *dec, uint32_t frames, Store store)
{
  const float *src = dec->work + dec->readPos;
  const uint32_t channels = dec->channels, stride = dec->stride;
  uint32_t i = 0;
  for (; channels == 1 && i + 4 <= frames; i += 4)
    store(i, V4::ldu(src + i));
  for (; channels == 2 && i + 4 <= frames; i += 4)
  {
    const V4::T l = V4::ldu(src + i), r = V4::ldu(src + stride + i);
    store(2 * i, V4::zip_lo(l, r));
    store(2 * i + 4, V4::zip_hi(l, r));
  }
  for (; channels == 4 && i + 4 <= frames; i += 4)
  {
    V4::T a = V4::ldu(src + i), b = V4::ldu(src + stride + i), c = V4::ldu(src + 2 * stride + i), d = V4::ldu(src + 3 * stride + i);
    V4::transpose(a, b, c, d);
    store(4 * i, a);
    store(4 * i + 4, b);
    store(4 * i + 8, c);
    store(4 * i + 12, d);
  }
  return i;
}

void read_float(const DaVorbisDecoder *dec, float *out, uint32_t frames)
{
  for (uint32_t i = interleave(dec, frames, [&](uint32_t at, V4::T v) { V4::stu(out + at, v); }); i < frames; i++)
    for (uint32_t c = 0; c < dec->channels; c++)
      out[i * dec->channels + c] = dec->work[c * dec->stride + dec->readPos + i];
}

void read_int16(const DaVorbisDecoder *dec, int16_t *out, uint32_t frames)
{
  for (uint32_t i = interleave(dec, frames, [&](uint32_t at, V4::T v) { V4::st_i16(out + at, v); }); i < frames; i++)
    for (uint32_t c = 0; c < dec->channels; c++)
    {
      int16_t lanes[4];
      V4::st_i16(lanes, V4::splat(dec->work[c * dec->stride + dec->readPos + i]));
      out[i * dec->channels + c] = lanes[0];
    }
}
} // namespace davorbis_detail
} // namespace dacodecs
