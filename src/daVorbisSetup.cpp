// Copyright (C) Gaijin Games KFT.  All rights reserved.
#include "daVorbisInternal.h"
#include <daVorbisConfig.h>
#include <math.h>
#include <string.h>

namespace dacodecs
{
using namespace davorbis_detail;

namespace
{
constexpr double PI = 3.14159265358979323846;

uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }
uint32_t align32(uint64_t v) { return uint32_t((v + 31) & ~31ull); }

uint32_t ilog(uint32_t v)
{
  uint32_t r = 0;
  for (; v; v >>= 1)
    r++;
  return r;
}

struct Reader
{
  const uint8_t *data;
  uint64_t pos, bits;
  bool bad;

  uint32_t peek(uint64_t at, uint32_t n) const
  {
    uint64_t w = 0;
    for (uint32_t i = 0, count = (uint32_t(at & 7) + n + 7) >> 3; i < count; i++)
      w |= uint64_t(data[(at >> 3) + i]) << (8 * i);
    return uint32_t((w >> (at & 7)) & ((1ull << n) - 1));
  }
  void skip(uint64_t n)
  {
    bad |= pos + n > bits;
    pos += bad ? 0 : n;
  }
  uint32_t get(uint32_t n)
  {
    skip(n);
    return bad ? 0 : peek(pos - n, n);
  }
};

// take() gives null with a null mem (a count only) or past cap, and so does each later take(): a writer tests its last take.
// A small struct with a null take goes to a local.
struct Arena
{
  uint8_t *mem;
  uint64_t used, cap;

  template <class T>
  T *take(uint64_t count)
  {
    used = (used + 31) & ~31ull;
    T *p = mem && used + count * sizeof(T) <= cap ? reinterpret_cast<T *>(mem + used) : nullptr;
    used += count * sizeof(T);
    return p;
  }
};

// The code word lengths of a book, entry by entry; 0 = the entry is not used.
struct LengthScan
{
  Reader r;
  uint32_t entries, done = 0, run = 0, len = 0, cur = 0;
  bool ordered, sparse = false;

  LengthScan(const Reader &at, uint32_t entries_, bool ordered_) : r(at), entries(entries_), ordered(ordered_)
  {
    if (ordered)
      cur = r.get(5) + 1;
    else
      sparse = r.get(1);
  }
  uint32_t next()
  {
    if (!ordered)
      return (!sparse || r.get(1)) ? r.get(5) + 1 : 0;
    while (!run && !r.bad)
    {
      run = r.get(ilog(entries - done));
      len = cur++;
      r.bad |= run > entries - done || len > 32;
      done += run;
    }
    if (r.bad)
      return 0;
    run--;
    return len;
  }
};

float unpack_float(uint32_t x)
{
  const double mantissa = double(x & 0x1FFFFF) * ((x & 0x80000000u) ? -1.0 : 1.0);
  return float(ldexp(mantissa, int((x >> 21) & 0x3FF) - 788));
}

uint32_t lookup1_values(uint32_t entries, uint32_t dim)
{
  if (dim == 1)
    return entries;
  for (uint32_t r = 1;; r++)
  {
    uint64_t p = 1;
    for (uint32_t i = 0; i < dim && p <= entries; i++)
      p *= r + 1;
    if (p > entries)
      return r;
  }
}

// Calls fn(entry, length, code) for each used entry of a book, in entry order. code: the code word from bit 31 down,
// by the tree rule of the specification (section 3.2.1). Returns false if the lengths give no valid tree: the tree must
// be full, but a book with one used entry of length 1 is valid (errata 20150226), and so is a book with no used entry.
template <class Fn>
bool for_each_code(const Reader &at, uint32_t entries, bool ordered, Fn fn)
{
  // freeNode[d]: the one free tree node of depth d. The deepest one is the lowest code.
  uint32_t freeNode[33] = {}, used = 0;
  uint64_t freeMask = 1;
  LengthScan scan(at, entries, ordered);
  for (uint32_t e = 0; e < entries; e++)
  {
    const uint32_t len = scan.next();
    if (!len)
      continue;
    used++;
    if (!(freeMask & ((2ull << len) - 1)))
      return false;
    uint32_t depth = len;
    while (!((freeMask >> depth) & 1))
      depth--;
    const uint32_t code = freeNode[depth];
    freeMask &= ~(1ull << depth);
    for (uint32_t d = depth + 1; d <= len; d++)
    {
      freeNode[d] = code | (1u << (32 - d));
      freeMask |= 1ull << d;
    }
    fn(e, len, code);
  }
  return used == 0 || freeMask == (used == 1 ? 2 : 0);
}

DaVorbisStatus parse_book(Reader &r, Arena &a, Book &bk, uint32_t &entriesLeft)
{
  if (r.get(24) != 0x564342)
    return DaVorbisStatus::BAD_SETUP;
  const uint32_t dim = r.get(16), entries = r.get(24);
  const bool ordered = r.get(1);
  if (r.bad || !dim || !entries)
    return DaVorbisStatus::BAD_SETUP;
  // Each entry costs parse time, and an ordered book declares millions of them in a few bytes.
  if (entries > entriesLeft)
    return DaVorbisStatus::UNSUPPORTED_SETUP;
  entriesLeft -= entries;
  const Reader lengthsAt = r;
  LengthScan scan(lengthsAt, entries, ordered);
  for (uint32_t e = 0; e < entries && !scan.r.bad; e++)
    scan.next();
  r = scan.r;
  const uint32_t lookup = r.get(4);
  float minimum = 0, delta = 0;
  uint32_t valueBits = 0;
  uint64_t lookupValues = 0, multAt = 0;
  bool sequence = false;
  if (lookup)
  {
    minimum = unpack_float(r.get(32));
    delta = unpack_float(r.get(32));
    valueBits = r.get(4) + 1;
    sequence = r.get(1);
    lookupValues = lookup == 1 ? lookup1_values(entries, dim) : uint64_t(entries) * dim;
    multAt = r.pos;
    r.skip(lookupValues * valueBits);
  }
  if (r.bad || lookup > 2)
    return DaVorbisStatus::BAD_SETUP;
  if (uint64_t(entries) * dim >= (1u << (32 - LEN_BITS)))
    return DaVorbisStatus::UNSUPPORTED_SETUP;

  // Where the code word of an entry goes: 0 = the fast table; 1 = a sub table of its own (the value is too large for
  // a fast entry); 2 = the sub table of its first FAST_BITS bits; 3 = the search list.
  auto place = [](uint32_t len, uint32_t value) {
    return len > FAST_BITS + SUB_BITS ? 3 : len > FAST_BITS ? 2 : value > FAST_VALUE_MAX ? 1 : 0;
  };
  uint32_t subCount = 0, longCount = 0, used = 0, maxLen = 0;
  uint8_t seen[1 << FAST_BITS] = {};
  if (!for_each_code(lengthsAt, entries, ordered, [&](uint32_t e, uint32_t len, uint32_t code) {
        const uint32_t where = place(len, lookup ? used : e), prefix = bit_reverse(code) & FAST_MASK;
        used++;
        maxLen = max_u32(maxLen, len);
        longCount += where == 3;
        subCount += where == 1 || (where == 2 && !seen[prefix]);
        seen[prefix] |= where == 2;
      }))
    return DaVorbisStatus::BAD_SETUP;

  // One sub table more, all zero and the last: the place of each code word start that no code word has.
  // The vectors must be the block right after the fast table (book_values); each used one is written below.
  uint16_t *fast = a.take<uint16_t>(1u << FAST_BITS);
  float *values = lookup ? a.take<float>(uint64_t(used) * dim) : nullptr;
  uint32_t *sub = a.take<uint32_t>((uint64_t(subCount) + 1) << SUB_BITS);
  LongCode *longCodes = a.take<LongCode>(longCount);
  const uint8_t kind = uint8_t(dim == 2 ? 0 : dim == 4 ? 1 : dim == 8 ? 2 : 3);
  bk = {fast, sub, longCodes, longCount, entries, uint16_t(dim), uint8_t(maxLen),
    uint8_t(max_u32(REFILL_BITS / max_u32(maxLen, 1), 1)), kind, !lookup};
  if (!longCodes)
    return DaVorbisStatus::OK;
  // With 1 << FAST_BITS sub tables the number wraps to 0, and each place gets its own table below.
  for (uint32_t k = 0; k <= FAST_MASK; k++)
    fast[k] = uint16_t(subCount << LEN_BITS);
  memset(sub, 0, ((size_t(subCount) + 1) * sizeof(uint32_t)) << SUB_BITS);
  memset(seen, 0, sizeof(seen));

  uint32_t subAt = 0, longAt = 0, usedAt = 0;
  for_each_code(lengthsAt, entries, ordered, [&](uint32_t e, uint32_t len, uint32_t code) {
    const uint32_t value = lookup ? usedAt : e, where = place(len, value), entry = value << LEN_BITS | len;
    const uint32_t stream = bit_reverse(code), prefix = stream & FAST_MASK;
    if (values)
    {
      float last = 0;
      uint64_t div = 1;
      for (uint32_t i = 0; i < dim; i++)
      {
        const uint64_t k = lookup == 1 ? (e / div) % lookupValues : uint64_t(e) * dim + i;
        const float v = float(r.peek(multAt + k * valueBits, valueBits)) * delta + minimum + last;
        values[usedAt * dim + i] = v;
        last = sequence ? v : 0;
        div = div > entries ? div : div * lookupValues;
      }
    }
    usedAt++;
    // The one code word of a single-entry book is taken for a 0 bit and for a 1 bit.
    const uint32_t spread = used == 1 ? 1 : 1u << min_u32(len, FAST_BITS);
    if (where == 0)
      for (uint32_t k = prefix; k <= FAST_MASK; k += spread)
        fast[k] = uint16_t(entry);
    else if (where == 3)
      longCodes[longAt++] = {code, entry};
    else
    {
      if (where == 1 || !seen[prefix])
      {
        for (uint32_t k = prefix; k <= FAST_MASK; k += where == 1 ? spread : FAST_MASK + 1)
          fast[k] = uint16_t(subAt << LEN_BITS);
        subAt++;
        seen[prefix] = 1;
      }
      uint32_t *table = sub + ((fast[prefix] >> LEN_BITS) << SUB_BITS);
      const uint32_t step = where == 1 ? 1 : 1u << (len - FAST_BITS);
      for (uint32_t k = where == 1 ? 0 : stream >> FAST_BITS; k <= SUB_MASK; k += step)
        table[k] = entry;
    }
  });
  sort(longCodes, longCodes + longCount, [](const LongCode &x, const LongCode &y) { return x.code < y.code; });
  return DaVorbisStatus::OK;
}

DaVorbisStatus parse_floor(Reader &r, const Book *books, uint32_t book_count, Floor1 &f)
{
  memset(&f, 0, sizeof(f));
  auto scalar_book = [&](uint32_t b) { return b < book_count && books[b].scalar; };
  f.partitions = uint8_t(r.get(5));
  int maxClass = -1;
  for (uint32_t i = 0; i < f.partitions; i++)
  {
    f.partClass[i] = uint8_t(r.get(4));
    maxClass = maxClass > f.partClass[i] ? maxClass : f.partClass[i];
  }
  for (int c = 0; c <= maxClass; c++)
  {
    f.classDim[c] = uint8_t(r.get(3) + 1);
    f.classSubBits[c] = uint8_t(r.get(2));
    if (f.classSubBits[c])
    {
      f.classMaster[c] = uint8_t(r.get(8));
      if (!scalar_book(f.classMaster[c]))
        return f.classMaster[c] < book_count ? DaVorbisStatus::UNSUPPORTED_SETUP : DaVorbisStatus::BAD_SETUP;
    }
    for (uint32_t j = 0; j < (1u << f.classSubBits[c]); j++)
    {
      const int book = int(r.get(8)) - 1;
      f.subBook[c][j] = int16_t(book);
      if (book >= 0 && !scalar_book(book))
        return uint32_t(book) < book_count ? DaVorbisStatus::UNSUPPORTED_SETUP : DaVorbisStatus::BAD_SETUP;
    }
  }
  f.multiplier = uint8_t(r.get(2) + 1);
  const uint32_t rangeBits = r.get(4);
  uint32_t values = 2;
  f.x[1] = uint16_t(1u << rangeBits);
  for (uint32_t i = 0; i < f.partitions; i++)
    for (uint32_t j = 0; j < f.classDim[f.partClass[i]]; j++)
    {
      if (values >= MAX_POSTS)
        return DaVorbisStatus::BAD_SETUP;
      f.x[values++] = uint16_t(r.get(rangeBits));
    }
  f.values = uint8_t(values);
  for (uint32_t i = 0; i < values; i++)
    f.sorted[i] = uint8_t(i);
  sort(f.sorted, f.sorted + values, [&](uint8_t p, uint8_t q) { return f.x[p] < f.x[q]; });
  for (uint32_t i = 1; i < values; i++)
    if (f.x[f.sorted[i]] == f.x[f.sorted[i - 1]])
      return DaVorbisStatus::BAD_SETUP;
  for (uint32_t i = 2; i < values; i++)
  {
    uint32_t lo = 0, hi = 1;
    for (uint32_t j = 0; j < i; j++)
    {
      if (f.x[j] < f.x[i] && f.x[j] > f.x[lo])
        lo = j;
      if (f.x[j] > f.x[i] && f.x[j] < f.x[hi])
        hi = j;
    }
    f.lowNb[i] = uint8_t(lo);
    f.highNb[i] = uint8_t(hi);
    f.recip[i] = (1ull << RECIP_BITS) / (f.x[hi] - f.x[lo]) + 1;
  }
  if (rangeBits > MAX_FLOOR_RANGE_BITS)
    return DaVorbisStatus::UNSUPPORTED_SETUP;
  return r.bad ? DaVorbisStatus::BAD_SETUP : DaVorbisStatus::OK;
}

DaVorbisStatus parse_residue(Reader &r, Arena &a, const Book *books, uint32_t book_count, Residue &res)
{
  memset(&res, 0, sizeof(res));
  const uint32_t type = r.get(16);
  res.type = uint8_t(type);
  res.begin = r.get(24);
  res.end = r.get(24);
  res.partSize = r.get(24) + 1;
  const uint32_t classes = r.get(6) + 1;
  res.classBook = uint8_t(r.get(8));
  if (r.bad || type > 2 || res.begin > res.end || res.classBook >= book_count)
    return DaVorbisStatus::BAD_SETUP;
  const Book &classBook = books[res.classBook];
  if (!classBook.scalar || classBook.entries > 0x10000)
    return DaVorbisStatus::UNSUPPORTED_SETUP;
  // Each class word must be an entry of the class book: classes ^ dim <= entries.
  uint64_t classWordCount = 1;
  for (uint32_t i = 0; i < classBook.dim && classWordCount <= classBook.entries; i++)
    classWordCount *= classes;
  if (classWordCount > classBook.entries)
    return DaVorbisStatus::BAD_SETUP;
  // The class words are in the packet also when no class has a book: one pass at least.
  res.passes = 1;
  uint8_t cascade[64];
  for (uint32_t i = 0; i < classes; i++)
  {
    const uint32_t low = r.get(3);
    cascade[i] = uint8_t((r.get(1) ? r.get(5) : 0) * 8 + low);
  }
  ResidueBook *rb = a.take<ResidueBook>(classes * 8u);
  res.books = rb;
  for (uint32_t i = 0; i < classes; i++)
    for (uint32_t pass = 0; pass < 8; pass++)
    {
      ResidueBook item = {-1, 0};
      if ((cascade[i] >> pass) & 1)
      {
        const uint32_t book = r.get(8);
        if (book >= book_count || books[book].scalar)
          return DaVorbisStatus::BAD_SETUP;
        if (res.partSize % books[book].dim)
          return DaVorbisStatus::UNSUPPORTED_SETUP;
        item = {int32_t(book), res.partSize / books[book].dim * books[book].maxLen};
        res.passes = uint8_t(max_u32(res.passes, pass + 1));
      }
      if (rb)
        rb[i * 8 + pass] = item;
    }
  res.classWords = classBook.dim;
  uint8_t *digits = a.take<uint8_t>(uint64_t(classBook.entries) * res.classWords);
  res.classDigits = digits;
  for (uint32_t e = 0; digits && e < classBook.entries; e++)
    for (uint32_t i = res.classWords, t = e; i-- > 0; t /= classes)
      digits[e * res.classWords + i] = uint8_t(t % classes);
  return r.bad ? DaVorbisStatus::BAD_SETUP : DaVorbisStatus::OK;
}

bool parse_mapping(Reader &r, Arena &a, const DaVorbisSetup &s, Mapping &m)
{
  memset(&m, 0, sizeof(m));
  if (r.get(16))
    return false;
  m.submaps = uint8_t(r.get(1) ? r.get(4) + 1 : 1);
  m.couplingSteps = uint16_t(r.get(1) ? r.get(8) + 1 : 0);
  uint8_t *coupling = a.take<uint8_t>(m.couplingSteps * 2u);
  m.coupling = coupling;
  const uint32_t channelBits = ilog(s.channels - 1);
  for (uint32_t i = 0; i < m.couplingSteps; i++)
  {
    const uint32_t magnitude = r.get(channelBits), angle = r.get(channelBits);
    if (magnitude == angle || magnitude >= s.channels || angle >= s.channels)
      return false;
    if (coupling)
    {
      coupling[i * 2] = uint8_t(magnitude);
      coupling[i * 2 + 1] = uint8_t(angle);
    }
  }
  if (r.get(2))
    return false;
  for (uint32_t c = 0; m.submaps > 1 && c < s.channels; c++)
  {
    m.mux[c] = uint8_t(r.get(4));
    if (m.mux[c] >= m.submaps)
      return false;
  }
  for (uint32_t j = 0; j < m.submaps; j++)
  {
    r.get(8);
    m.floor[j] = uint8_t(r.get(8));
    m.residue[j] = uint8_t(r.get(8));
    if (m.floor[j] >= s.floorCount || m.residue[j] >= s.residueCount)
      return false;
  }
  return !r.bad;
}

// Position `pos` of the FFT input holds t[j]: the lowest digit of j picks the largest sub-block.
uint32_t digit_reverse(uint32_t pos, uint32_t m)
{
  uint32_t j = 0, scale = 1;
  if (ilog(m) % 2 == 0)
  {
    m /= 2;
    j = pos / m;
    pos %= m;
    scale = 2;
  }
  for (; m > 1; m /= 4, scale *= 4)
  {
    j += pos / (m / 4) * scale;
    pos %= m / 4;
  }
  return j;
}

void build_tables(Arena &a, uint32_t n, BlockTables &bt)
{
  const uint32_t fftSize = n / 4;
  const bool radix2 = ilog(fftSize) % 2 == 0;
  uint32_t passFloats = radix2 ? fftSize : 0;
  for (uint32_t s = 4; 4 * s <= fftSize; s *= 4)
    passFloats += 6 * s;
  float *window = a.take<float>(n), *pass = a.take<float>(passFloats), *post = a.take<float>(2 * fftSize);
  uint16_t *groupPos = a.take<uint16_t>(fftSize / 4);
  bt = {n, window, groupPos, pass, post};
  if (!groupPos)
    return;
  for (uint32_t x = 0; x < n; x++)
  {
    const double s = sin((x + 0.5) / n * PI);
    window[x] = float(sin(0.5 * PI * s * s));
  }
  for (uint32_t m = 0; m < fftSize; m++)
  {
    const double phi = PI * (8.0 * m + 1) / (4.0 * n);
    post[m] = float(cos(phi));
    post[fftSize + m] = float(-sin(phi));
  }
  for (uint32_t pos = 0; pos < fftSize; pos += 4)
    groupPos[digit_reverse(pos, fftSize)] = uint16_t(pos);
  for (uint32_t s = 4; 4 * s <= fftSize; pass += 6 * s, s *= 4)
    for (uint32_t k = 0; k < s; k++)
      for (uint32_t q = 0; q < 3; q++)
      {
        const double angle = -2 * PI * (q + 1) * k / (4.0 * s);
        pass[2 * q * s + k] = float(cos(angle));
        pass[(2 * q + 1) * s + k] = float(sin(angle));
      }
  for (uint32_t k = 0; radix2 && k < fftSize / 2; k++)
  {
    pass[k] = float(cos(-2 * PI * k / fftSize));
    pass[fftSize / 2 + k] = float(sin(-2 * PI * k / fftSize));
  }
}

DaVorbisStatus parse(const uint8_t *packet, uint32_t bytes, const DaVorbisConfig &cfg, Arena &a, const DaVorbisSetup **out)
{
  auto valid_block = [](uint32_t n) { return n >= DAVORBIS_MIN_BLOCK && n <= DAVORBIS_MAX_BLOCK && !(n & (n - 1)); };
  if (!cfg.channels || cfg.channels > DAVORBIS_MAX_CHANNELS)
    return DaVorbisStatus::BAD_CHANNELS;
  if (!valid_block(cfg.block0) || !valid_block(cfg.block1) || cfg.block0 > cfg.block1 || bytes < 8 ||
      memcmp(packet, "\x05vorbis", 7) != 0)
    return DaVorbisStatus::BAD_SETUP;
  Reader r = {packet, 56, uint64_t(bytes) * 8, false};
  DaVorbisSetup local, *s = a.take<DaVorbisSetup>(1);
  s = s ? s : &local;
  memset(s, 0, sizeof(*s));
  s->decode = isa_decode(cfg.isa);
  s->channels = cfg.channels;
  // The reader gives zeros after the packet end, and they can name a setup that is not decoded: a cut packet is bad.
  auto failed = [&](DaVorbisStatus st) { return r.bad ? DaVorbisStatus::BAD_SETUP : st; };

  Book localBooks[256], *books = a.take<Book>(s->bookCount = r.get(8) + 1);
  s->books = books = books ? books : localBooks;
  uint32_t entriesLeft = 1u << 20;
  for (uint32_t i = 0; i < s->bookCount; i++)
    if (const DaVorbisStatus st = parse_book(r, a, books[i], entriesLeft); st != DaVorbisStatus::OK)
      return failed(st);
  for (uint32_t i = r.get(6) + 1; i-- > 0;)
    if (r.get(16))
      return DaVorbisStatus::BAD_SETUP;

  Floor1 localFloor, *floors = a.take<Floor1>(s->floorCount = r.get(6) + 1);
  s->floors = floors;
  for (uint32_t i = 0; i < s->floorCount; i++)
  {
    const uint32_t type = r.get(16);
    if (type != 1)
      return failed(type == 0 ? DaVorbisStatus::UNSUPPORTED_SETUP : DaVorbisStatus::BAD_SETUP);
    const DaVorbisStatus st = parse_floor(r, books, s->bookCount, floors ? floors[i] : localFloor);
    if (st != DaVorbisStatus::OK)
      return failed(st);
  }
  Residue localResidue, *residues = a.take<Residue>(s->residueCount = r.get(6) + 1);
  s->residues = residues;
  for (uint32_t i = 0; i < s->residueCount; i++)
  {
    const DaVorbisStatus st = parse_residue(r, a, books, s->bookCount, residues ? residues[i] : localResidue);
    if (st != DaVorbisStatus::OK)
      return failed(st);
  }
  Mapping localMapping, *mappings = a.take<Mapping>(s->mappingCount = r.get(6) + 1);
  s->mappings = mappings;
  s->anyChannels = true;
  for (uint32_t i = 0; i < s->mappingCount; i++)
  {
    Mapping &m = mappings ? mappings[i] : localMapping;
    if (!parse_mapping(r, a, *s, m))
      return DaVorbisStatus::BAD_SETUP;
    s->anyChannels &= m.submaps == 1 && !m.couplingSteps;
  }
  s->modeCount = r.get(6) + 1;
  s->modeBits = ilog(s->modeCount - 1);
  for (uint32_t i = 0; i < s->modeCount; i++)
  {
    s->modes[i].blockFlag = uint8_t(r.get(1));
    const uint32_t windowAndTransform = r.get(32);
    s->modes[i].mapping = uint8_t(r.get(8));
    if (windowAndTransform || s->modes[i].mapping >= s->mappingCount)
      return DaVorbisStatus::BAD_SETUP;
  }
  if (!r.get(1) || r.bad)
    return DaVorbisStatus::BAD_SETUP;
  build_tables(a, cfg.block0, s->tables[0]);
  build_tables(a, cfg.block1, s->tables[1]);
  *out = s;
  return a.used < 0x7FFFFF00 ? DaVorbisStatus::OK : DaVorbisStatus::UNSUPPORTED_SETUP;
}

void set_status(DaVorbisStatus *status, DaVorbisStatus value)
{
  if (status)
    *status = value;
}

bool channels_ok(const DaVorbisSetup &s, uint32_t n) { return n && n <= DAVORBIS_MAX_CHANNELS && (n == s.channels || s.anyChannels); }

template <class T>
uint32_t read_ready(DaVorbisDecoder *dec, T *out, uint32_t max_frames, void (*read)(const DaVorbisDecoder *, T *, uint32_t))
{
  const uint32_t frames = min_u32(max_frames, dec->ready);
  if (out && frames)
    read(dec, out, frames);
  dec->readPos += frames;
  dec->ready -= frames;
  return frames;
}

template <class T>
uint32_t stream_read(DaVorbisDecoder *dec, DaVorbisStream *s, T *out, uint32_t frames, DaVorbisStatus *status,
  void (*read)(const DaVorbisDecoder *, T *, uint32_t))
{
  DaVorbisStatus st = DaVorbisStatus::OK;
  uint32_t done = 0;
  for (;;)
  {
    done += read_ready(dec, out ? out + size_t(done) * dec->channels : nullptr, frames - done, read);
    if (done == frames || st != DaVorbisStatus::OK)
      break;
    const uint32_t bytes = s->pos + 2 <= s->bytes ? s->data[s->pos] | s->data[s->pos + 1] << 8 : 0;
    if (!bytes || s->pos + 2 + bytes > s->bytes)
    {
      st = DaVorbisStatus::END_OF_DATA;
      break;
    }
    davorbis_decode_packet(dec, s->data + s->pos + 2, bytes, &st);
    s->pos += 2 + bytes;
  }
  set_status(status, st);
  return done;
}
} // namespace

uint32_t davorbis_setup_bytes(const uint8_t *packet, uint32_t bytes, const DaVorbisConfig &cfg, DaVorbisStatus *status)
{
  Arena a = {nullptr, 0, 0};
  const DaVorbisSetup *s = nullptr;
  const DaVorbisStatus st = parse(packet, bytes, cfg, a, &s);
  set_status(status, st);
  return st == DaVorbisStatus::OK ? align32(a.used) : 0;
}

const DaVorbisSetup *davorbis_setup_parse(const uint8_t *packet, uint32_t bytes, const DaVorbisConfig &cfg, void *mem,
  uint32_t mem_bytes, DaVorbisStatus *status)
{
  const bool usable = mem && !(uintptr_t(mem) & 31);
  Arena a = {usable ? static_cast<uint8_t *>(mem) : nullptr, 0, mem_bytes};
  const DaVorbisSetup *s = nullptr;
  DaVorbisStatus st = parse(packet, bytes, cfg, a, &s);
  if (st == DaVorbisStatus::OK && (!usable || align32(a.used) > mem_bytes))
    st = DaVorbisStatus::BAD_MEMORY;
  set_status(status, st);
  return st == DaVorbisStatus::OK ? s : nullptr;
}

uint32_t davorbis_decoder_bytes(const DaVorbisSetup *setup, uint32_t channels)
{
  if (!channels_ok(*setup, channels))
    return 0;
  const uint32_t half = setup->tables[1].n / 2;
  for (uint32_t i = 0; i < setup->residueCount; i++)
  {
    const Residue &r = setup->residues[i];
    const uint32_t size = r.type == 2 ? half * channels : half;
    const uint32_t parts = (min_u32(r.end, size) - min_u32(r.begin, size)) / r.partSize;
    // The slots of one stream, as residue_decode counts them: the partitions up to a full class word.
    const uint64_t slots = uint64_t(parts + r.classWords - 1) / r.classWords * r.classWords;
    if ((r.type == 2 ? 1 : channels) * slots > MAX_CLASS_SLOTS)
      return 0;
  }
  const uint32_t scratch = half * 2 > STACK_BLOCK ? half + 8 : 0;
  return align32(sizeof(DaVorbisDecoder)) + (channels * (half + CHANNEL_GAP + half / 2) + scratch) * sizeof(float);
}

DaVorbisDecoder *davorbis_decoder_open(const DaVorbisSetup *setup, uint32_t channels, void *mem, uint32_t mem_bytes,
  DaVorbisStatus *status)
{
  const uint32_t need = davorbis_decoder_bytes(setup, channels);
  const DaVorbisStatus st = !channels_ok(*setup, channels)                      ? DaVorbisStatus::BAD_CHANNELS
                            : !need                                             ? DaVorbisStatus::UNSUPPORTED_SETUP
                            : mem_bytes < need || !mem || (uintptr_t(mem) & 31) ? DaVorbisStatus::BAD_MEMORY
                                                                                : DaVorbisStatus::OK;
  set_status(status, st);
  if (st != DaVorbisStatus::OK)
    return nullptr;
  DaVorbisDecoder *dec = static_cast<DaVorbisDecoder *>(mem);
  const uint32_t half = setup->tables[1].n / 2;
  dec->setup = setup;
  dec->work = reinterpret_cast<float *>(static_cast<uint8_t *>(mem) + align32(sizeof(DaVorbisDecoder)));
  dec->tail = dec->work + channels * (half + CHANNEL_GAP);
  dec->scratch = half * 2 > STACK_BLOCK ? dec->tail + channels * (half / 2) : nullptr;
  dec->channels = channels;
  dec->stride = half + CHANNEL_GAP;
  dec->quarter = half / 2;
  davorbis_decoder_restart(dec);
  return dec;
}

void davorbis_decoder_restart(DaVorbisDecoder *dec)
{
  dec->prevBlock = dec->readPos = dec->ready = 0;
  memset(dec->tail, 0, size_t(dec->channels) * dec->quarter * sizeof(float));
}

uint32_t davorbis_decode_packet(DaVorbisDecoder *dec, const uint8_t *packet, uint32_t bytes, DaVorbisStatus *status)
{
  DaVorbisStatus st;
  const uint32_t frames = dec->setup->decode(dec, packet, bytes, &st);
  set_status(status, st);
  return frames;
}

uint32_t davorbis_skip_packet(DaVorbisDecoder *dec, const uint8_t *packet, uint32_t bytes, DaVorbisStatus *status)
{
  const DaVorbisSetup &s = *dec->setup;
  const uint32_t mode = bytes ? (packet[0] >> 1) & ((1u << s.modeBits) - 1) : s.modeCount;
  // The header of a long block has 2 window bits more: a packet cut in them is bad here as in the decode.
  const uint32_t flag = mode < s.modeCount ? s.modes[mode].blockFlag : 0;
  if (!bytes || (packet[0] & 1) || mode >= s.modeCount || 1 + s.modeBits + 2 * flag > uint64_t(bytes) * 8)
  {
    set_status(status, DaVorbisStatus::BAD_PACKET);
    return 0;
  }
  const uint32_t prev = dec->prevBlock, n = s.tables[flag].n;
  dec->prevBlock = n;
  dec->readPos = dec->ready = 0;
  set_status(status, DaVorbisStatus::OK);
  return prev ? (prev + n) / 4 : 0;
}

uint32_t davorbis_ready_frames(const DaVorbisDecoder *dec) { return dec->ready; }

uint32_t davorbis_read_float(DaVorbisDecoder *dec, float *out, uint32_t max_frames)
{
  return read_ready(dec, out, max_frames, read_float);
}

uint32_t davorbis_read_int16(DaVorbisDecoder *dec, int16_t *out, uint32_t max_frames)
{
  return read_ready(dec, out, max_frames, read_int16);
}

uint32_t davorbis_stream_read_float(DaVorbisDecoder *dec, DaVorbisStream *stream, float *out, uint32_t frames, DaVorbisStatus *status)
{
  return stream_read(dec, stream, out, frames, status, read_float);
}

uint32_t davorbis_stream_read_int16(DaVorbisDecoder *dec, DaVorbisStream *stream, int16_t *out, uint32_t frames,
  DaVorbisStatus *status)
{
  return stream_read(dec, stream, out, frames, status, read_int16);
}

void davorbis_stream_seek(DaVorbisDecoder *dec, DaVorbisStream *stream, uint32_t packet_offset)
{
  davorbis_decoder_restart(dec);
  stream->pos = packet_offset < stream->bytes ? packet_offset : stream->bytes;
}
} // namespace dacodecs
