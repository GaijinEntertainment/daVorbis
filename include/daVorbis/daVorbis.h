// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once
#include <stdint.h>

// A Vorbis I decoder for streams whose setup packet comes apart from the audio packets, as in sound banks.
// No call allocates memory and no call keeps global state: the caller gives each block and owns it.
namespace dacodecs
{
inline constexpr uint32_t DAVORBIS_MAX_CHANNELS = 8;
// Block sizes are powers of 2 in this range, block0 <= block1.
inline constexpr uint32_t DAVORBIS_MIN_BLOCK = 64;
inline constexpr uint32_t DAVORBIS_MAX_BLOCK = 8192;
// Bytes after each packet that must be readable; the decoder loads them but never uses them as data.
inline constexpr uint32_t DAVORBIS_PACKET_PAD = 16;

enum class DaVorbisStatus : uint8_t
{
  OK,
  // The setup packet does not follow the Vorbis I rules, or the block sizes of the config are not valid.
  BAD_SETUP,
  // A valid setup that this decoder does not take: "What it decodes" in DETAILS.md lists them.
  UNSUPPORTED_SETUP,
  // The memory block is null, smaller than the size function says, or not 32-byte aligned.
  BAD_MEMORY,
  // 0 channels, more than DAVORBIS_MAX_CHANNELS, or a decoder count that the setup cannot decode.
  BAD_CHANNELS,
  // Not an audio packet, or its header is cut. No state changes: the next packet overlaps the last good one.
  BAD_PACKET,
  // The stream has no more packets: fewer frames than asked were given.
  END_OF_DATA,
};

// The instruction set of the decode code. A build without the AVX files (not x86_64, or a macOS universal binary) decodes
// each value at BASE. Elsewhere AUTO takes the level of the compiler target if it has AVX2 or AVX, else the best that can run.
// In that case the CPU is asked in the static init of the library: a setup parsed in an earlier static init gets BASE for AUTO.
// There, a level that cannot run crashes: AVX needs AVX and the AVX state of the OS; AVX2 also needs AVX2, FMA, BMI1, BMI2 and LZCNT.
// All levels decode within the same error bound, not to the same bits.
enum class DaVorbisIsa : uint8_t
{
  AUTO,
  BASE,
  AVX,
  AVX2,
};

// A parsed setup packet. Read-only after the parse: decoders of all threads can share one with no lock.
struct DaVorbisSetup;
// The state of one stream. One thread at a time can use a decoder; separate decoders are independent.
struct DaVorbisDecoder;

// What the setup packet does not hold. channels: the count that the setup was made for (it sets how the mappings parse).
// block0, block1: the short and the long block size in frames.
struct DaVorbisConfig
{
  uint32_t channels, block0, block1;
  DaVorbisIsa isa;
};

// In each call a null `status` is valid.

// packet: `bytes` bytes of a Vorbis I setup header, from its 0x05 "vorbis" start. No pad is necessary.
// Returns the bytes that davorbis_setup_parse needs for this packet and config; 0 and a status if it cannot be used.
uint32_t davorbis_setup_bytes(const uint8_t *packet, uint32_t bytes, const DaVorbisConfig &cfg, DaVorbisStatus *status);
// mem: mem_bytes >= davorbis_setup_bytes, 32-byte aligned. The setup lives in it: keep it in place while a decoder uses the setup.
// The packet is not necessary after the call. Returns null and a status on failure.
const DaVorbisSetup *davorbis_setup_parse(const uint8_t *packet, uint32_t bytes, const DaVorbisConfig &cfg, void *mem,
  uint32_t mem_bytes, DaVorbisStatus *status);

// Returns the bytes of a decoder: 3 * block1 + 256 for each channel plus a header (more for block1 over 2048).
// A count other than the config's is valid only when every mapping has 1 submap and no coupling. Returns 0 if not valid.
uint32_t davorbis_decoder_bytes(const DaVorbisSetup *setup, uint32_t channels);
// mem: mem_bytes >= davorbis_decoder_bytes, 32-byte aligned. The decoder lives in it: keep it in place; there is no close call.
// Returns null and a status on failure. The new decoder is in the state that davorbis_decoder_restart gives.
DaVorbisDecoder *davorbis_decoder_open(const DaVorbisSetup *setup, uint32_t channels, void *mem, uint32_t mem_bytes,
  DaVorbisStatus *status);
// Call before the first packet of a new position (a seek, a loop start): the next packet gives 0 frames.
void davorbis_decoder_restart(DaVorbisDecoder *dec);

// Decodes one audio packet; DAVORBIS_PACKET_PAD bytes after it must be readable. A decoded packet drops the unread ready frames.
// Returns the frames that it makes ready: (last block + this block) / 4, 0 after a restart. The lost part of a cut packet is zero.
uint32_t davorbis_decode_packet(DaVorbisDecoder *dec, const uint8_t *packet, uint32_t bytes, DaVorbisStatus *status);
// For a seek scan: as davorbis_decode_packet with its frames dropped, but it reads only the first byte.
// Returns the frame count of the packet. The next decoded packet has a wrong overlap: drop its frames too.
uint32_t davorbis_skip_packet(DaVorbisDecoder *dec, const uint8_t *packet, uint32_t bytes, DaVorbisStatus *status);
uint32_t davorbis_ready_frames(const DaVorbisDecoder *dec);
// Moves up to max_frames ready frames to out, interleaved by channel: out holds max_frames * channels values.
// Returns the frames moved. A null out drops them. Float is not clamped; int16 is value x 32768, rounded half to even, saturated.
uint32_t davorbis_read_float(DaVorbisDecoder *dec, float *out, uint32_t max_frames);
uint32_t davorbis_read_int16(DaVorbisDecoder *dec, int16_t *out, uint32_t max_frames);

// A Vorbis sample payload: packets with a u16 little-endian byte count before each; a count of 0 ends it.
// data + bytes + DAVORBIS_PACKET_PAD must be readable. pos: the byte offset of the next packet, 0 at the start.
struct DaVorbisStream
{
  const uint8_t *data;
  uint32_t bytes, pos;
};

// Gives `frames` frames to out (frames * channels values, interleaved; null drops them), decoding packets as necessary.
// Returns the frames given: fewer only with END_OF_DATA or BAD_PACKET (the stream is then after the bad packet).
uint32_t davorbis_stream_read_float(DaVorbisDecoder *dec, DaVorbisStream *stream, float *out, uint32_t frames, DaVorbisStatus *status);
uint32_t davorbis_stream_read_int16(DaVorbisDecoder *dec, DaVorbisStream *stream, int16_t *out, uint32_t frames,
  DaVorbisStatus *status);
// Restarts the decoder at the packet whose u16 is at packet_offset. That packet gives no frames: the output goes on
// with the frames of the packet after it. The frame position of an offset comes from the caller (a seek table).
void davorbis_stream_seek(DaVorbisDecoder *dec, DaVorbisStream *stream, uint32_t packet_offset);
} // namespace dacodecs
