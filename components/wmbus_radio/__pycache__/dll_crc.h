#pragma once
#include <cstdint>
#include <vector>

namespace esphome {
namespace wmbus_common {

// EN 13757 CRC16 used in wM-Bus DLL
static constexpr uint16_t CRC16_EN_13757_POLY = 0x3D65;

inline uint16_t crc16_en13757_per_byte(uint16_t crc, uint8_t b) {
  for (int i = 0; i < 8; i++) {
    if ((((crc & 0x8000) >> 8) ^ (b & 0x80)) != 0) {
      crc = (uint16_t)((crc << 1) ^ CRC16_EN_13757_POLY);
    } else {
      crc = (uint16_t)(crc << 1);
    }
    b <<= 1;
  }
  return crc;
}

inline uint16_t crc16_en13757(const uint8_t *data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    crc = crc16_en13757_per_byte(crc, data[i]);
  }
  return (uint16_t)(~crc);
}

struct DLLCRCResult {
  bool ok{false};
  const char *format{""};
  const char *stage{"none"};
  uint16_t calculated{0};
  uint16_t expected{0};
  size_t data_pos{0};
  size_t data_len{0};
  size_t crc_pos{0};
  size_t input_len{0};
  size_t output_len{0};
  uint16_t removed_bytes{0};
};

inline void dll_crc_result_reset_(DLLCRCResult *diag, const char *format, size_t input_len) {
  if (diag == nullptr) return;
  *diag = {};
  diag->format = format;
  diag->input_len = input_len;
  diag->stage = "none";
}

// Try trim DLL CRC for Frame Format A (returns true if trimmed & CRCs OK)
inline bool trim_dll_crc_format_a(std::vector<uint8_t> &payload, DLLCRCResult *diag = nullptr) {
  dll_crc_result_reset_(diag, "A", payload.size());
  if (payload.size() < 12) {
    if (diag != nullptr) diag->stage = "format_a_too_short";
    return false;
  }

  const size_t len = payload.size();
  std::vector<uint8_t> out;

  // First block: 10 bytes + 2 CRC
  {
    uint16_t calc = crc16_en13757(payload.data(), 10);
    uint16_t check = (uint16_t)(payload[10] << 8 | payload[11]);
    if (diag != nullptr) {
      diag->stage = "dll_crc_first";
      diag->calculated = calc;
      diag->expected = check;
      diag->data_pos = 0;
      diag->data_len = 10;
      diag->crc_pos = 10;
    }
    if (calc != check) return false;
    out.insert(out.end(), payload.begin(), payload.begin() + 10);
  }

  // Middle blocks: 16 bytes + 2 CRC
  size_t pos = 12;
  for (; pos + 18 <= len; pos += 18) {
    uint16_t calc = crc16_en13757(payload.data() + pos, 16);
    size_t crc_pos = pos + 16;
    uint16_t check = (uint16_t)(payload[crc_pos] << 8 | payload[crc_pos + 1]);
    if (diag != nullptr) {
      diag->stage = "dll_crc_mid";
      diag->calculated = calc;
      diag->expected = check;
      diag->data_pos = pos;
      diag->data_len = 16;
      diag->crc_pos = crc_pos;
    }
    if (calc != check) return false;
    out.insert(out.end(), payload.begin() + pos, payload.begin() + pos + 16);
  }

  // Final block: (len-2 - pos) bytes + 2 CRC
  if (pos < len - 2) {
    const size_t data_len = (len - 2) - pos;
    uint16_t calc = crc16_en13757(payload.data() + pos, data_len);
    uint16_t check = (uint16_t)(payload[len - 2] << 8 | payload[len - 1]);
    if (diag != nullptr) {
      diag->stage = "dll_crc_final";
      diag->calculated = calc;
      diag->expected = check;
      diag->data_pos = pos;
      diag->data_len = data_len;
      diag->crc_pos = len - 2;
    }
    if (calc != check) return false;
    out.insert(out.end(), payload.begin() + pos, payload.begin() + (len - 2));
  }

  if (out.empty()) {
    if (diag != nullptr) diag->stage = "format_a_empty_output";
    return false;
  }

  // Fix L-field (length without itself)
  out[0] = (uint8_t)(out.size() - 1);
  if (diag != nullptr) {
    diag->ok = true;
    diag->stage = "ok";
    diag->output_len = out.size();
    diag->removed_bytes = (uint16_t)(len - out.size());
  }
  payload = std::move(out);
  return true;
}

// Try trim DLL CRC for Frame Format B (returns true if trimmed & CRCs OK)
inline bool trim_dll_crc_format_b(std::vector<uint8_t> &payload, DLLCRCResult *diag = nullptr) {
  dll_crc_result_reset_(diag, "B", payload.size());
  if (payload.size() < 12) {
    if (diag != nullptr) diag->stage = "format_b_too_short";
    return false;
  }

  const size_t len = payload.size();
  std::vector<uint8_t> out;

  size_t crc1_pos, crc2_pos;
  if (len <= 128) {
    crc1_pos = len - 2;
    crc2_pos = 0;
  } else {
    crc1_pos = 126;
    crc2_pos = len - 2;
  }

  // CRC1 over [0..crc1_pos)
  {
    uint16_t calc = crc16_en13757(payload.data(), crc1_pos);
    uint16_t check = (uint16_t)(payload[crc1_pos] << 8 | payload[crc1_pos + 1]);
    if (diag != nullptr) {
      diag->stage = "dll_crc_b1";
      diag->calculated = calc;
      diag->expected = check;
      diag->data_pos = 0;
      diag->data_len = crc1_pos;
      diag->crc_pos = crc1_pos;
    }
    if (calc != check) return false;
    out.insert(out.end(), payload.begin(), payload.begin() + crc1_pos);
  }

  // Optional CRC2 over [crc1_pos+2 .. crc2_pos)
  if (crc2_pos > 0) {
    const size_t start = crc1_pos + 2;
    const size_t data_len = crc2_pos - start;
    uint16_t calc = crc16_en13757(payload.data() + start, data_len);
    uint16_t check = (uint16_t)(payload[crc2_pos] << 8 | payload[crc2_pos + 1]);
    if (diag != nullptr) {
      diag->stage = "dll_crc_b2";
      diag->calculated = calc;
      diag->expected = check;
      diag->data_pos = start;
      diag->data_len = data_len;
      diag->crc_pos = crc2_pos;
    }
    if (calc != check) return false;
    out.insert(out.end(), payload.begin() + start, payload.begin() + crc2_pos);
  }

  if (out.empty()) {
    if (diag != nullptr) diag->stage = "format_b_empty_output";
    return false;
  }

  out[0] = (uint8_t)(out.size() - 1);
  if (diag != nullptr) {
    diag->ok = true;
    diag->stage = "ok";
    diag->output_len = out.size();
    diag->removed_bytes = (uint16_t)(len - out.size());
  }
  payload = std::move(out);
  return true;
}

// Strict: returns true only if CRC(s) validated and were removed.
inline bool removeAnyDLLCRCs(std::vector<uint8_t> &payload) {
  if (trim_dll_crc_format_a(payload)) return true;
  if (trim_dll_crc_format_b(payload)) return true;
  return false;
}

}  // namespace wmbus_common
}  // namespace esphome
