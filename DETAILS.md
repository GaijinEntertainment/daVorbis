# daVorbis

A Vorbis I audio decoder for streams whose setup packet comes apart from the audio packets, as the sound
banks store them: raw audio packets with a 16-bit byte count before each, no Ogg pages, and the setup
packet in a table of the engine. It is about 1,700 lines of C++17, not counting comments and blank lines.

Public header: `include/daVorbis/daVorbis.h`, used as `<daVorbis/daVorbis.h>`. Each call has its contract
there (memory sizes, units, status values, order of calls).

## What it decodes

- Floor type 1, residue types 0, 1 and 2, mapping type 0, code book lookup types 0, 1 and 2.
- 1 to 8 channels, block sizes 64 to 8192.
- Not decoded (`UNSUPPORTED_SETUP`): floor type 0; a book with a value lookup that a floor or a residue
  class word uses as a scalar book; a residue book whose vector size does not divide the partition size;
  a floor with more than 13 range bits; a book whose entries x vector size is 2^26 or more; a residue class
  book with more than 65536 entries; books with more than 2^20 entries in all; a setup that needs 0x7FFFFF00
  bytes of memory or more.
- A decoder limit: a residue with more than 2048 partition class slots for the channel count
  (`davorbis_decoder_bytes` returns 0 and `davorbis_decoder_open` gives `UNSUPPORTED_SETUP`).
- The window flags of a long block are skipped and not used: the real sizes of the blocks beside it give its
  window. A valid stream has the two equal.

The identification and comment headers are not read. The caller gives what they hold: the channel count
and the two block sizes (`DaVorbisConfig`). The sample rate is not a decode input.

## Use

```cpp
using namespace dacodecs;
// 0 bytes or a null is a failure: check each result before the next call. A read can give fewer frames.
DaVorbisConfig cfg = {channels, 256, 2048, DaVorbisIsa::AUTO};
uint32_t setupBytes = davorbis_setup_bytes(packet, packetBytes, cfg, &status);
const DaVorbisSetup *setup = davorbis_setup_parse(packet, packetBytes, cfg, setupMem, setupBytes, &status);

uint32_t decoderBytes = davorbis_decoder_bytes(setup, channels);
DaVorbisDecoder *dec = davorbis_decoder_open(setup, channels, decoderMem, decoderBytes, &status);

DaVorbisStream stream = {payload, payloadBytes, 0};
uint32_t got = davorbis_stream_read_float(dec, &stream, out, frames, &status);
```

- A setup is parsed one time and is read-only after that: all decoders of all threads share it.
- A decoder is the state of one stream: 3 x block1 + 256 bytes for each channel (6400 bytes at 2048) plus a
  small header, and 2 x block1 + 32 bytes more for a block1 over 2048; `davorbis_decoder_bytes` gives the
  size. Both memory blocks come from the caller, 32-byte aligned; no call allocates.
- Output is interleaved float (not clamped) or int16 (saturated). Both come from one pass over the decoded
  frames, so neither is a convert of the other.
- The first packet after a start or a restart gives no frames: it only fills the overlap.
- 16 bytes after each packet must be readable (`DAVORBIS_PACKET_PAD`); they are never used as data.
- Seek: `davorbis_stream_seek` to the byte offset of a packet, then read and drop frames up to the wanted
  one. The frame position of an offset comes from the caller.

## Parts

| File | Content |
|---|---|
| `src/daVorbisSetup.cpp` | Setup packet parse, the tables of each book, the FFT and window tables, the calls that do not depend on the instruction set. |
| `src/daVorbisDecode.inl` | The decode of one packet. One text, with internal linkage. |
| `src/daVorbisDecodeBase.cpp`, `...Avx.cpp`, `...Avx2.cpp` | Each includes the decode text after one vector layer and exports its decode function. The base file has the output pass too (the interleave to float or int16), built one time. |
| `src/daVorbisSimd4.h` | The 4-lane vector layer through vecmath (SSE and NEON). |
| `src/daVorbisSimd8.h` | The 8-lane layer for the AVX and AVX2 files. It has no vecmath: a file built for AVX must not instance inline functions that other files share. |
| `src/daVorbisIsaX86.cpp`, `...IsaGeneric.cpp` | The pick of the decode function: x86_64 builds take the first, other targets and a macOS universal binary the second. |
| `config/std/daVorbisConfig.h`, `config/eastl/daVorbisConfig.h` | The sort of the setup parse, `std::sort` or `eastl::sort`. The include path of the build takes one of the two folders. |

Only x86_64 builds have the AVX files, each built with the flags of its instruction set. A setup takes its
decode function at parse time. `BASE`, `AVX` and `AVX2` of `DaVorbisIsa` force a level. `AUTO` takes the
level of the compiler target when that target has AVX2 or AVX (a console build is made for its one CPU).
Else it asks the CPU: AVX2 when the CPU has AVX2, FMA, BMI1, BMI2 and LZCNT, AVX when it has AVX, and in
both cases only when the system keeps the 256-bit registers (XGETBV); else base. That query runs one time,
in the static init of the library, so then a setup parsed in an earlier static init gets base. A build
without the AVX files (a target that is not x86_64, or a macOS universal binary) has the base level only,
and each value gives it.

## Build

The folder builds with CMake 3.13 or later as the static library `daVorbis` (C++17). Its one dependency
is vecmath, the header-only SIMD library of the Dagor engine, in the git submodule `external/vecmath`. A
clone with `--recursive` builds with no options; a clone without it needs `git submodule update --init`.

```
git clone --recursive https://github.com/GaijinEntertainment/daVorbis.git
cmake -S daVorbis -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

A project that has its own CMake build adds the folder with `add_subdirectory` and links `daVorbis`; the
public include folder comes with it.

- `VECMATH_INCLUDE_DIR`: the folder that holds `vecmath/`. Default: `external`, the submodule. When the
  submodule is empty, `../../..`, the place of vecmath when the folder is inside the engine tree
  (`prog/1stPartyLibs`). The configure stops with an error when that folder has no vecmath.
- `DAVORBIS_CONFIG_DIR`: the folder of `daVorbisConfig.h`. Default: `config/std`. A build that uses EASTL
  sets `<daVorbis folder>/config/eastl` and has the EASTL include folder on its include path.
- Each target builds `src/daVorbisSetup.cpp`, `src/daVorbisFloorTable.cpp` and `src/daVorbisDecodeBase.cpp`.
- x86_64 builds all files with SSE4.1 (`-msse4.1`; MSVC: `/D_TARGET_SIMD_SSE=4`) and adds
  `src/daVorbisDecodeAvx.cpp` with `-mavx` (MSVC: `/arch:AVX`), `src/daVorbisDecodeAvx2.cpp` with
  `-mavx2 -mfma -mbmi -mbmi2 -mlzcnt -ffp-contract=fast` (MSVC: `/arch:AVX2 /fp:contract`; clang-cl:
  `/clang:-ffp-contract=fast`), and `src/daVorbisIsaX86.cpp`. With the contraction the compiler joins
  a multiply and an add into one FMA instruction.
- Other targets (arm64 and others, and a macOS universal binary) add `src/daVorbisIsaGeneric.cpp`. On arm64
  vecmath uses NEON. A universal binary builds its x86_64 slice with SSE4.1 (`-Xarch_x86_64 -msse4.1`).

A build without CMake takes the same files and options. With gcc or clang (not clang-cl), the CMake build
defines `VECMATH_FINLINE=inline __attribute__((always_inline))`, and a build by hand must define it too:
without it vecmath uses `__forceinline`, which gcc and clang do not have. The engine build needs nothing, as
its compatibility header, included in each file, defines `__forceinline`.

The speeds in this file come from the Dagor engine build and its release options (fast math on all
files). In one session on the PC of the tables, a CMake Release build decoded within 6% of it with clang-cl
and 5% to 11% slower with MSVC 19.38 at each level; its worst SNR was within 0.1 dB of the quality table.

## How a packet is decoded

1. Bit reader: a 64-bit window, refilled with one 8-byte load. One refill serves as many code words as fit.
2. Floor 1 posts, then the residue. A code word is one read of a 1024-entry table of its book; longer codes
   take a second table, and codes of more than 16 bits a search in a sorted list. Vectors of the usual
   sizes (2, 4, 8) are added by loops of their own; a stereo type 2 residue writes directly to the two
   channels.
3. Inverse coupling, then the floor curve: the integer line of the specification, computed for 4 or 8 bins at
   a time, each value through the dB table of the specification (section 10.1).
4. Inverse MDCT as an FFT of a quarter of the block size with a pre and a post rotation. The middle half of
   its output is enough: the outer quarters are mirrors.
5. Window and overlap-add with the kept quarter of the block before, into the ready frames of each channel.

## Source

Written from the Vorbis I specification (xiph.org). No source text of another decoder is in it, except the
CPU check, a copy from daGEA; the one table from outside is `floor1_inverse_dB_table` of the specification.

## Comparison with an optimized proprietary decoder

An optimized proprietary Vorbis decoder decodes the same streams. A harness that the author of daVorbis did
not see timed the two decoders on 2419 Vorbis samples of game sound banks, with int16 output. The number is
the decode time of daVorbis divided by the decode time of the optimized proprietary decoder: under 1,
daVorbis is faster. The last column is the decode time of daVorbis for each int16 value, on one thread:
the median of 5 runs after one run to warm up (CPU time of the thread on the PC, wall time on the consoles).

| Machine | daVorbis time / time of the optimized proprietary decoder | daVorbis, ns for each int16 value |
|---|---|---|
| PC, Intel Core i9-13980HX, AVX2 level | 0.68 | 1.98 |
| PC, AVX level | 0.64 | 2.06 |
| PC, base level | 0.73 | 2.38 |
| curgen console | 0.76 | 2.97 |
| prevgen console | 0.87 | 17.11 |

The number of each setup, taken by itself, is under 1 on each machine. The two decoders have equal quality
against libvorbis (SNR within 0.1 dB for each setup). The samples and the harness are not public.

## Comparison with stb_vorbis and libvorbis

The other decoders are stb_vorbis 1.22 (`stb_vorbis.c`, sha256
`4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`, its default options) and stock libvorbis
1.3.7 (`libvorbis-1.3.7.tar.gz`, sha256 `0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab`).
The streams are 164 Vorbis samples of game sound banks: 14 setups (7 mono and 7 stereo; 3 of the mono setups
also decode samples of 4 channels), 8000 to 48000 Hz, 250 million channel-frames (a channel-frame is one frame
of one channel). The samples are not public.

Quality: the float output of daVorbis and of stb_vorbis against the float output of libvorbis, sample by
sample.

| Decoder | SNR of the worst sample | Largest absolute difference |
|---|---|---|
| daVorbis base | 134.8 dB | 7.2e-7 |
| daVorbis AVX | 134.8 dB | 7.2e-7 |
| daVorbis AVX2 | 134.9 dB | 7.2e-7 |
| daVorbis base on arm64 (NEON), Apple M4 Pro | 135.3 dB | 7.8e-7 |
| stb_vorbis 1.22 | 142.8 dB | 4.2e-7 |
| stb_vorbis 1.22, Apple M4 Pro | 137.0 dB | 7.2e-7 |

The levels and the machines do not have to give the same bits: fused multiply-add and 8-lane sums round in
another order. Of the 164 samples, 7 have the same output bits on the base level, the AVX2 level and arm64.

Decode time to float, in ns for each channel-frame, on one thread of an Intel Core i9-13980HX:

| Decoder | 7 mono setups | 7 stereo setups | 4 channels (3 of the mono setups) |
|---|---|---|---|
| daVorbis AVX2 | 1.85 | 1.76 | 1.70 |
| daVorbis AVX | 1.93 | 1.81 | 1.79 |
| daVorbis base | 2.15 | 2.09 | 2.06 |
| stb_vorbis 1.22 | 7.66 | 7.60 | 7.21 |
| libvorbis 1.3.7 | 9.18 | 9.58 | 8.48 |

The same on one thread of an Apple M4 Pro (performance cores):

| Decoder | 7 mono setups | 7 stereo setups | 4 channels (3 of the mono setups) |
|---|---|---|---|
| daVorbis base (NEON) | 1.64 | 1.56 | 1.46 |
| stb_vorbis 1.22 | 4.87 | 4.85 | 4.47 |
| libvorbis 1.3.7 | 7.25 | 7.40 | 6.43 |

How the numbers were made:

- The three decoders get the same packets of each sample from memory. The parse of the setup packet and the
  open of the decoder are not in the time.
- libvorbis, timed: `vorbis_synthesis`, `vorbis_synthesis_blockin`, `vorbis_synthesis_pcmout` and
  `vorbis_synthesis_read` for each packet. Its output stays in its own buffers.
- stb_vorbis, timed: the `stb_vorbis_get_frame_float` calls of a sample. Its output stays in its own buffers.
  It reads Ogg pages only, so the packets of a sample go into Ogg pages (libogg) after the identification,
  comment and setup headers that libvorbis gets. Not in the time: the page build, `stb_vorbis_open_memory`
  (which decodes the first audio packet, a packet that gives no frames) and `stb_vorbis_close`.
- daVorbis, timed: one `davorbis_stream_read_float` of the full sample into a buffer of the caller. So its
  time has the pass that interleaves the channels, and the other two times have no such pass.
- The time of a sample is the best of 5 runs; each program ran two times and the lower result counts. The
  number of a row is the sum of the times of its samples divided by the sum of their channel-frames.
- All rows of a machine come from one session: the PC on 2026-11-16 (the three programs one after the
  other), the Apple M4 Pro on 2026-11-17 (one program that decodes with all three).
- One compiler and one set of options for the three. PC: clang-cl of LLVM 21.1.8 with
  `-O3 -msse4.1 -mpopcnt -mcx16 -fp:fast -Xclang -mrecip=none`, the release options of the Dagor engine;
  libvorbis and stb_vorbis have one code path, and the AVX and AVX2 files of daVorbis have the options of their
  instruction set in addition. Apple M4 Pro: Apple clang 21.0.0 on macOS 26.5.2 with
  `-O3 -ffast-math -ffinite-math-only -mrecip=none`, the macOS release options of the Dagor engine.
- The quality reference on the PC is another build of libvorbis 1.3.7, with `/O2` and no fast math. On the
  Apple M4 Pro it is the libvorbis build that is timed there.

## Tests

The tests are not in this folder: they are the Catch2 suite `unittests/daVorbis` of the Dagor engine
repository. A hidden case runs only when a Catch2 filter names its tag, for example `[.davorbis_ref]`.

The default cases need no data. `vorbisWriter.h` makes setup packets and audio packets, and has a slow decoder
written from the specification. Some cases compare the library with it: each residue type and book shape,
coupling, cuts of each packet after the first at each byte length, and each instruction set level that the CPU
has (the levels are compared with each other too). The other cases check rules of the contract: damaged setups
and random packets (no crash, the status values, no use of the pad bytes), stream reads in pieces, a bad packet
in a stream, seek and skip, and the memory blocks.

The hidden cases:

- `[.davorbis_spec_gaps]` needs no data: it shows the one known output difference from the specification, a
  stream whose window flags do not agree with its block sizes. Its tag `[!shouldfail]` makes a run pass while
  the difference stays.
- `[.davorbis_ref]` needs outside data: it decodes bank samples and compares with float reference files of
  libvorbis (`DAVORBIS_REF_INDEX`, `DAVORBIS_SETUPS`, `DAVORBIS_ISA` = `base`, `avx` or `avx2`); a sample
  under 130 dB SNR or over 2e-6 of absolute difference fails. It prints the decode time of each setup too,
  the best of `DAVORBIS_REF_RUNS` runs for each sample: the daVorbis rows of the PC table of the comparison
  with stb_vorbis and libvorbis.
- `[.davorbis_bench]` needs outside data: decode time of the Vorbis samples of a list of bank files
  (`DAVORBIS_BANK_LIST`, `DAVORBIS_SETUPS`, `DAVORBIS_ISA`, `DAVORBIS_BENCH_RUNS`, `DAVORBIS_BENCH_FRAMES`,
  `DAVORBIS_BENCH_SETUP`).
