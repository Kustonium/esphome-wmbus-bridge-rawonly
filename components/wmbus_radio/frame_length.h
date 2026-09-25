// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How many RAW (line-coded) bytes a wM-Bus frame occupies, derived from the
// L-field in its first bytes.
//
// This is what lets a receiver stop a capture at the real end of the frame
// instead of at a configured ceiling. The SX1262 driver has used it since the
// AN1200.53 streaming path was written; it was moved here on 2026-09-24 so the
// LR1121 drain can use the same arithmetic rather than a second copy of it.
// The bodies are unchanged - the tolerances in the S-mode helper come from a
// measurement at the sensitivity threshold (see its comments) and are not
// something to re-derive.
//
// Zero always means "cannot tell". The caller then has to fall back to
// whatever ceiling it would have used anyway.

#include "decode3of6.h"

#include <cstdint>
#include <vector>

namespace esphome {
namespace wmbus_radio {

// One Manchester pair at absolute chip index `chip` in raw. Returns false when
// the pair is 00 or 11, which encodes nothing.
inline bool manchester_chip_pair(const std::vector<uint8_t> &raw, size_t chip, bool polarity, uint8_t &bit_out) {
  const size_t a_i = chip * 2U;
  const size_t b_i = a_i + 1U;
  if ((b_i >> 3) >= raw.size())
    return false;
  const uint8_t a = (uint8_t) ((raw[a_i >> 3] >> (7U - (a_i & 7U))) & 0x01U);
  const uint8_t b = (uint8_t) ((raw[b_i >> 3] >> (7U - (b_i & 7U))) & 0x01U);
  if (a == b)
    return false;
  bit_out = (uint8_t) ((a == 0 && b == 1) ? 0 : 1);
  if (polarity)
    bit_out ^= 1U;
  return true;
}

// Decode the S1 L- and C-fields from the start of the captured stream and
// return the exact number of raw Manchester bytes a complete format-A frame
// occupies, DLL CRC bytes included. Zero means "cannot tell", which is what
// makes capture_rx_stream_() run on to its 512-byte cap.
inline size_t expected_raw_len_s1(const std::vector<uint8_t> &raw) {
  if (raw.size() < 4)
    return 0;

  for (uint8_t polarity = 0; polarity < 2; polarity++) {
    // L-field, raw bytes 0-1. Every pair must decode: this value cuts the
    // capture, so a substituted bit here yields a wrong length rather than a
    // recoverable one. No tolerance.
    uint8_t l_field = 0;
    bool l_ok = true;
    for (size_t bit = 0; bit < 8; bit++) {
      uint8_t v = 0;
      if (!manchester_chip_pair(raw, bit, polarity != 0, v)) {
        l_ok = false;
        break;
      }
      l_field = (uint8_t) ((l_field << 1U) | v);
    }
    if (!l_ok)
      continue;

    const size_t frame_len = (size_t) l_field + 1U;
    if (frame_len < 12U || frame_len > 260U)
      continue;

    // C-field, raw bytes 2-3. Up to two invalid pairs tolerated, and only the
    // bits that did decode are compared against 0x44 / 0x46.
    //
    // The C-field is here to stop the complemented polarity selecting a
    // plausible but wrong L-field - it contributes nothing to the length. So
    // demanding it decode perfectly buys nothing and costs a great deal:
    // measured 2026-08-01 at the sensitivity threshold, a single chip error in
    // raw byte 3 turned 0x65 into 0x6D, no length was derived, and the capture
    // ran to the 512-byte cap collecting 85 ms of post-frame noise. The frame
    // itself had one bad pair in 776.
    //
    // Two masked bits still leave six to match, so the complement (0xBB against
    // 0x44) is not going to slip through.
    uint8_t c_bits = 0, c_mask = 0;
    size_t c_invalid = 0;
    for (size_t bit = 0; bit < 8; bit++) {
      uint8_t v = 0;
      c_bits = (uint8_t) (c_bits << 1U);
      c_mask = (uint8_t) (c_mask << 1U);
      if (manchester_chip_pair(raw, 8U + bit, polarity != 0, v)) {
        c_bits = (uint8_t) (c_bits | v);
        c_mask = (uint8_t) (c_mask | 1U);
      } else {
        c_invalid++;
      }
    }
    if (c_invalid > 2)
      continue;
    if ((c_bits & c_mask) != (0x44U & c_mask) && (c_bits & c_mask) != (0x46U & c_mask))
      continue;

    const size_t blocks = (l_field < 26U) ? 2U : (size_t) ((l_field - 26U) / 16U + 3U);
    const size_t decoded_with_crc = frame_len + 2U * blocks;
    return decoded_with_crc * 2U;
  }
  return 0;
}

// expected_raw_len_t1: how many raw (3-of-6 coded) bytes this T-mode frame
// occupies, derived from its own L field. The T counterpart of
// expected_raw_len_s1() above, and it exists for the same reason: with no
// length the stream capture does not know where the frame ends, so it runs on
// to its byte cap collecting post-frame noise.
//
// That was not a corner case in T mode, it was every single capture, because
// both of the loop's early exits are unreachable in this configuration:
//
//   * `end_irq` waits for RX_DONE or TIMEOUT. RX_DONE cannot fire - the loop
//     pushes REG_RXTX_PAYLOAD_LEN ahead of the write pointer on every poll,
//     which is exactly what AN1200.53 asks for - and RX is armed continuous,
//     so there is no TIMEOUT either.
//   * `silence` waits 30 ms without new bytes. In continuous RX the
//     demodulator keeps producing bits out of noise once the frame has ended,
//     so bytes keep arriving, last_change_ms keeps being refreshed, and the
//     silence never happens. (The S-mode branch does reach this exit, which is
//     why the note there reads the other way round - a different packet
//     configuration, and not a claim that holds for T mode.)
//
// So every capture ran to `buffer_cap`: 512 bytes, 125 ms of deafness, and a
// short frame buried under repeated reads of a 256-byte circular buffer. That
// is where `long_gfsk_packets: true` spent its 7.5 dB - not in sensitivity,
// which is why no register ever explained it. Confirmed by the field data: the
// exit reason recorded in the RSSI diagnostics was `buffer_cap` every time,
// and 512 copied bytes means 512 bytes really did arrive.
//
// Two differences from the S-mode helper, both making this one simpler:
//
//   * No polarity search. Manchester has two, so expected_raw_len_s1() tries
//     both and needs the C field to reject the complement. 3-of-6 has no
//     polarity ambiguity, so the C field buys nothing here and is not read -
//     one less byte that has to survive at the sensitivity threshold.
//   * A decoded byte is exactly 1.5 raw bytes, so encoded_size() from the
//     3-of-6 decoder answers directly instead of the S-mode x2.
//
// Returns 0 when no length can be derived; the caller then keeps its cap.
// ---------------------------------------------------------------------------
inline size_t expected_raw_len_t1(const std::vector<uint8_t> &raw) {
  // Two raw bytes carry the two 6-bit symbols that make up the L field (with
  // four bits to spare). That is the entire input this needs.
  if (raw.size() < 2)
    return 0;

  const std::vector<uint8_t> head(raw.begin(), raw.begin() + 2);
  const auto decoded = decode3of6(head);
  if (!decoded.has_value() || decoded->empty())
    return 0;

  const uint8_t l_field = (*decoded)[0];
  const size_t frame_len = (size_t) l_field + 1U;
  // Same bounds as the S-mode helper: under 12 there is no room for a DLL
  // header, over 260 no wM-Bus frame exists.
  if (frame_len < 12U || frame_len > 260U)
    return 0;

  // Format A block layout, identical in both modes: block 1 carries 10 bytes,
  // every further block 16, each followed by a 2-byte CRC.
  const size_t blocks = (l_field < 26U) ? 2U : (size_t) ((l_field - 26U) / 16U + 3U);
  const size_t decoded_with_crc = frame_len + 2U * blocks;
  return encoded_size(decoded_with_crc);
}

// expected_raw_len_c1: mode C carries no line coding, so the raw length is the
// link layer plus the two-byte mode-C indicator that precedes it. The indicator
// is 0x54 then 0xCD for format A or 0x3D for format B, and the L-field sits at
// raw index 2 - which is also why Packet::l_field() reads index 2 in this mode.
//
// The two formats size differently and the arithmetic mirrors
// Packet::expected_size() exactly, because that is what the layer above will
// use on the same bytes; the two disagreeing would be worse than either being
// wrong alone.
inline size_t expected_raw_len_c1(const std::vector<uint8_t> &raw) {
  if (raw.size() < 3)
    return 0;
  if (raw[0] != 0x54U)
    return 0;

  const uint8_t l_field = raw[2];
  const size_t frame_len = (size_t) l_field + 1U;
  // Same bounds as the other two helpers.
  if (frame_len < 12U || frame_len > 260U)
    return 0;

  if (raw[1] == 0xCDU) {  // format A: full block layout with its CRCs
    const size_t blocks = (l_field < 26U) ? 2U : (size_t) ((l_field - 26U) / 16U + 3U);
    return 2U + frame_len + 2U * blocks;
  }
  if (raw[1] == 0x3DU)    // format B: CRCs already counted inside the L-field
    return 2U + 1U + (size_t) l_field;
  return 0;
}

} // namespace wmbus_radio
} // namespace esphome
