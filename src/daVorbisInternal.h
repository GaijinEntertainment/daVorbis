// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once
#include <daVorbis/daVorbis.h>

namespace dacodecs
{
namespace davorbis_detail
{
// The fast table of a book must be small: each code word reads a line of it at random, and the tables of all books
// of a packet must stay in the first-level cache.
inline constexpr uint32_t FAST_BITS = 10;
inline constexpr uint32_t FAST_MASK = (1u << FAST_BITS) - 1;
// Each table entry is value << LEN_BITS | length: a shift by the entry takes the length (x86 masks the count to 6 bits).
// A fast entry of length 0 holds the number of a sub table.
inline constexpr uint32_t LEN_BITS = 6;
inline constexpr uint32_t LEN_MASK = (1u << LEN_BITS) - 1;
inline constexpr uint32_t FAST_VALUE_MAX = (1u << (16 - LEN_BITS)) - 1;
// A sub table takes the SUB_BITS bits after the first FAST_BITS.
inline constexpr uint32_t SUB_BITS = 6;
inline constexpr uint32_t SUB_MASK = (1u << SUB_BITS) - 1;
inline constexpr uint32_t MAX_POSTS = 65;
// Partition class bytes on the stack of one residue decode.
inline constexpr uint32_t MAX_CLASS_SLOTS = 2048;
// Largest block whose FFT scratch is on the stack.
inline constexpr uint32_t STACK_BLOCK = 2048;
inline constexpr uint16_t POST_UNUSED = 0x8000, POST_VALUE = 0x7FFF;
inline constexpr uint32_t RECIP_BITS = 40;
// Bits that the window holds for sure after a refill.
inline constexpr uint32_t REFILL_BITS = 56;
// Floats between the work vectors of two channels. A CPU matches a load with an earlier store by the low 12 address
// bits: with no gap, the same index of two channels is a multiple of 4096 bytes apart and the loads wait.
inline constexpr uint32_t CHANNEL_GAP = 64;
// The float line of the floor multiply is exact up to this distance between posts.
inline constexpr uint32_t MAX_FLOOR_RANGE_BITS = 13;

// code: the code word from bit 31 down. entry: value << LEN_BITS | length.
struct LongCode
{
  uint32_t code, entry;
};

// A table value is the entry number of a scalar book. For a book with vectors it is the index of a vector
// (dim floats, used entries only); the vectors are right after the fast table (book_values): one pointer serves the two.
// A code word is in the fast table, else in a sub table, else in longCodes.
// burst: code words that fit in the bit window after one refill. kind: 0, 1, 2 for dim 2, 4, 8; 3 for the rest.
struct Book
{
  const uint16_t *fast;
  const uint32_t *sub;
  const LongCode *longCodes;
  uint32_t longCount, entries;
  uint16_t dim;
  uint8_t maxLen, burst, kind;
  bool scalar;
};

struct Floor1
{
  uint8_t partitions, multiplier, values;
  uint8_t partClass[31];
  uint8_t classDim[16], classSubBits[16], classMaster[16];
  int16_t subBook[16][8];
  uint16_t x[MAX_POSTS];
  uint8_t sorted[MAX_POSTS], lowNb[MAX_POSTS], highNb[MAX_POSTS];
  // 2^RECIP_BITS / (x[highNb] - x[lowNb]) + 1: the divide of the post prediction as a multiply.
  uint64_t recip[MAX_POSTS];
};

// bits: the most bits that one partition can take.
struct ResidueBook
{
  int32_t book;
  uint32_t bits;
};

struct Residue
{
  uint32_t begin, end, partSize;
  uint8_t type, classBook, passes;
  uint16_t classWords;
  const uint8_t *classDigits;
  const ResidueBook *books;
};

struct Mapping
{
  uint8_t submaps;
  uint16_t couplingSteps;
  const uint8_t *coupling;
  uint8_t mux[DAVORBIS_MAX_CHANNELS];
  uint8_t floor[16], residue[16];
};

struct Mode
{
  uint8_t blockFlag, mapping;
};

// Tables of one block size n; the FFT has n / 4 points.
// window: n values. groupPos[j]: where the group of t[j], j < n / 16, goes after the first pass.
// passTw: the twiddles of each FFT pass, in the order of use. postTw: re then im of exp(-i pi (8j + 1) / (4n)).
struct BlockTables
{
  uint32_t n;
  const float *window;
  const uint16_t *groupPos;
  const float *passTw, *postTw;
};

typedef uint32_t (*DecodePacket)(DaVorbisDecoder *dec, const uint8_t *packet, uint32_t bytes, DaVorbisStatus *status);
extern const DecodePacket DECODE_BASE, DECODE_AVX, DECODE_AVX2;
extern const float FLOOR1_INVERSE_DB[256];
DecodePacket isa_decode(DaVorbisIsa isa);
// The output pass has one build: it uses only the 4-lane layer of the base path.
void read_float(const DaVorbisDecoder *dec, float *out, uint32_t frames);
void read_int16(const DaVorbisDecoder *dec, int16_t *out, uint32_t frames);

static inline const float *book_values(const uint16_t *fast) { return reinterpret_cast<const float *>(fast + (1u << FAST_BITS)); }

static inline uint32_t bit_reverse(uint32_t x)
{
  x = (x >> 16) | (x << 16);
  x = ((x & 0xFF00FF00u) >> 8) | ((x & 0x00FF00FFu) << 8);
  x = ((x & 0xF0F0F0F0u) >> 4) | ((x & 0x0F0F0F0Fu) << 4);
  x = ((x & 0xCCCCCCCCu) >> 2) | ((x & 0x33333333u) << 2);
  return ((x & 0xAAAAAAAAu) >> 1) | ((x & 0x55555555u) << 1);
}
} // namespace davorbis_detail

struct DaVorbisSetup
{
  davorbis_detail::DecodePacket decode;
  const davorbis_detail::Book *books;
  const davorbis_detail::Floor1 *floors;
  const davorbis_detail::Residue *residues;
  const davorbis_detail::Mapping *mappings;
  davorbis_detail::Mode modes[64];
  davorbis_detail::BlockTables tables[2];
  uint32_t bookCount, floorCount, residueCount, mappingCount, modeCount, modeBits;
  uint32_t channels;
  bool anyChannels;
};

// work: the spectrum of a packet, then its ready frames: `stride` floats for each channel.
// tail: a quarter of the last block, not windowed: `quarter` floats for each channel.
// scratch: null if the buffer for the floor levels and the FFT fits the stack (blocks up to STACK_BLOCK).
struct DaVorbisDecoder
{
  const DaVorbisSetup *setup;
  float *work, *tail, *scratch;
  uint32_t channels, stride, quarter;
  uint32_t prevBlock, readPos, ready;
};
} // namespace dacodecs
