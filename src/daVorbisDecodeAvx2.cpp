// Copyright (C) Gaijin Games KFT.  All rights reserved.
#include "daVorbisSimd8.h"
#include "daVorbisDecode.inl"

namespace dacodecs
{
namespace davorbis_detail
{
extern const DecodePacket DECODE_AVX2 = decode_packet;
}
} // namespace dacodecs
