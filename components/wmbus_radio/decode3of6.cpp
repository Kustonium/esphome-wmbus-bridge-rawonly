// SPDX-License-Identifier: GPL-3.0-or-later
#include "decode3of6.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "3of6";

// Flat 6-bit -> nibble lookup. `code` is always masked to 0..63 below, so a
// 64-entry array is a direct index (O(1), cache-friendly) instead of an
// std::map tree walk per symbol. INVALID marks the 48 codes that are not part
// of the 16-symbol 3-of-6 alphabet (EN 13757-4); seeing one means a bit error
// or collision, exactly like a failed map lookup did before.
static constexpr uint8_t INVALID = 0xFF;
static const uint8_t LOOKUP_3OF6[64] = {
    // 0x00-0x07
    INVALID, INVALID, INVALID, INVALID, INVALID, INVALID, INVALID, INVALID,
    // 0x08-0x0F
    INVALID, INVALID, INVALID, 0x3,     INVALID, 0x1,     0x2,     INVALID,
    // 0x10-0x17
    INVALID, INVALID, INVALID, 0x7,     INVALID, INVALID, 0x0,     INVALID,
    // 0x18-0x1F
    INVALID, 0x5,     0x6,     INVALID, 0x4,     INVALID, INVALID, INVALID,
    // 0x20-0x27
    INVALID, INVALID, INVALID, 0xB,     INVALID, 0x9,     0xA,     INVALID,
    // 0x28-0x2F
    INVALID, 0xF,     INVALID, INVALID, 0x8,     INVALID, INVALID, INVALID,
    // 0x30-0x37
    INVALID, 0xD,     0xE,     INVALID, 0xC,     INVALID, INVALID, INVALID,
    // 0x38-0x3F
    INVALID, INVALID, INVALID, INVALID, INVALID, INVALID, INVALID, INVALID,
};

std::optional<std::vector<uint8_t>>
decode3of6(const std::vector<uint8_t> &coded_data, Decode3of6Stats *stats) {

  // ESP_LOGD(TAG, "Decoding 3of6 data: %s", format_hex(coded_data).c_str());

  std::vector<uint8_t> decodedBytes;
  // Number of 6-bit symbols that can be extracted from the coded buffer.
  // NOTE: decoding a symbol can span across byte boundary, so we must guard
  // against reading past the end of the buffer.
  auto segments = coded_data.size() * 8 / 6;
  auto data = coded_data.data();

  uint16_t invalid = 0;
  if (stats != nullptr) {
    stats->symbols_total = (uint16_t) segments;
    stats->symbols_invalid = 0;
  }

  for (size_t i = 0; i < segments; i++) {
    auto bit_idx = i * 6;
    auto byte_idx = bit_idx / 8;
    auto bit_offset = bit_idx % 8;

    uint8_t code = (data[byte_idx] << bit_offset);
    if (bit_offset > 0) {
      // Guard against out-of-bounds for the last symbol.
      uint8_t next = 0;
      if ((byte_idx + 1) < coded_data.size())
        next = data[byte_idx + 1];
      code |= (next >> (8 - bit_offset));
    }
    code >>= 2;

    // `code` holds the 6-bit symbol in bits 5..0 (top two bits already shifted
    // out above), so it is guaranteed to be 0..63 — a safe direct LUT index.
    const uint8_t nibble = LOOKUP_3OF6[code];
    if (nibble == INVALID) {
      // Invalid 6-bit symbol.
      invalid++;
      // Keep alignment so we can continue counting and preserve nibble pairing.
      // We still return nullopt at the end if any invalid symbols were seen.
      if (i % 2 == 0)
        decodedBytes.push_back(0x00);
      continue;
    }

    if (i % 2 == 0)
      decodedBytes.push_back(nibble << 4);
    else
      decodedBytes.back() |= nibble;
  }

  if (stats != nullptr) {
    stats->symbols_invalid = invalid;
  }

  if (invalid > 0) {
    return {};
  }

  // ESP_LOGV(TAG, "Successfully decoded %zu bytes", decodedBytes.size());
  return decodedBytes;
}

size_t encoded_size(size_t decoded_size) {
  // Every 2 bytes (4 nibbles by 6 bits = 24b) of decoded data is encoded into 3
  // bytes of coded data +1 for rounding up
  return (3 * decoded_size + 1) / 2;
}
} // namespace wmbus_radio
} // namespace esphome