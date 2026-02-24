#include "packet.h"

#include <algorithm>
#include <ctime>

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include "decode3of6.h"
// Lightweight DLL CRC validation/removal helpers (EN13757 CRC16)
#include "dll_crc.h"

#define WMBUS_PREAMBLE_SIZE (3)
#define WMBUS_MODE_C_SUFIX_LEN (2)
#define WMBUS_MODE_C_PREAMBLE (0x54)
#define WMBUS_BLOCK_A_PREAMBLE (0xCD)
#define WMBUS_BLOCK_B_PREAMBLE (0x3D)

namespace esphome {
namespace wmbus_radio {

static const char *const TAG = "wmbus_radio.packet";

// Hex-encode up to max_bytes from input. Result contains only 0-9a-f.
static std::string hex_prefix_(const std::vector<uint8_t> &in, size_t max_bytes) {
  const size_t n = (max_bytes == 0) ? in.size() : std::min(in.size(), max_bytes);
  std::string out;
  out.reserve(n * 2);
  static const char *hex = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    uint8_t b = in[i];
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0x0F]);
  }
  return out;
}

Packet::Packet() { this->data_.reserve(WMBUS_PREAMBLE_SIZE); }

// Determine the link mode based on the first byte of the data
LinkMode Packet::link_mode() {
  if (this->link_mode_ == LinkMode::UNKNOWN)
    if (this->data_.size())
      if (this->data_[0] == WMBUS_MODE_C_PREAMBLE)
        this->link_mode_ = LinkMode::C1;
      else
        this->link_mode_ = LinkMode::T1;

  return this->link_mode_;
}

void Packet::set_rssi(int8_t rssi) { this->rssi_ = rssi; }

// Get value of L-field (best-effort; for RAW-only we avoid depending on this early)
uint8_t Packet::l_field() {
  switch (this->link_mode()) {
    case LinkMode::C1:
      if (this->data_.size() < 3) return 0;
      return this->data_[2];

    case LinkMode::T1: {
      // Decode a minimal prefix to obtain decoded[0] (L-field)
      const size_t n = std::min<size_t>(this->data_.size(), 18);  // safer than 3
      std::vector<uint8_t> tmp(this->data_.begin(), this->data_.begin() + n);
      auto decoded = decode3of6(tmp);
      if (decoded && !decoded->empty()) return (*decoded)[0];
      break;
    }

    default:
      break;
  }
  return 0;
}

// Keep expected_size() for callers that may want it, but RAW-only path below does not require it.
size_t Packet::expected_size() {
  if (this->data_.size() < WMBUS_PREAMBLE_SIZE) return 0;

  if (!this->expected_size_) {
    auto l_field = this->l_field();
    if (l_field == 0) return 0;

    // Number of blocks (format A/B framing rules)
    auto nrBlocks = l_field < 26 ? 2 : (l_field - 26) / 16 + 3;
    auto nrBytes = l_field + 1 + 2 * nrBlocks;

    if (this->link_mode() != LinkMode::C1) {
      this->expected_size_ = encoded_size(nrBytes);
    } else if (this->data_[1] == WMBUS_BLOCK_A_PREAMBLE) {
      this->expected_size_ = WMBUS_MODE_C_SUFIX_LEN + nrBytes;
    } else if (this->data_[1] == WMBUS_BLOCK_B_PREAMBLE) {
      this->expected_size_ = WMBUS_MODE_C_SUFIX_LEN + 1 + l_field;
    }
  }

  ESP_LOGV(TAG, "expected_size: %zu", this->expected_size_);
  return this->expected_size_;
}

uint8_t *Packet::append_space(size_t len) {
  const size_t old = this->data_.size();
  this->data_.resize(old + len);
  return this->data_.data() + old;
}

// Helpers for DLL size calculation
static inline size_t blocks_for_l_(uint8_t l_field) {
  // EN13757-3: number of blocks depends on L-field
  return (l_field < 26) ? 2 : (size_t) ((l_field - 26) / 16 + 3);
}

static inline size_t total_len_format_a_with_crc_(uint8_t l_field) {
  // L counts bytes excluding itself and excluding CRC bytes.
  // Total bytes *including* CRC bytes = (L+1) + 2*nrBlocks
  return (size_t) l_field + 1 + 2 * blocks_for_l_(l_field);
}

static inline size_t total_len_format_b_with_crc_(uint8_t l_field) {
  // In format B, L includes CRC bytes (already part of L bytes).
  return (size_t) l_field + 1;
}

std::optional<Frame> Packet::convert_to_frame() {
  std::optional<Frame> frame = {};

  // reset diagnostics
  this->truncated_ = false;
  this->want_len_ = 0;
  this->got_len_ = 0;
  this->raw_got_len_ = this->data_.size();
  this->drop_reason_.clear();

  // Capture raw bytes (hex) early for diagnostics. Keep it bounded.
  // 256 bytes -> 512 hex chars, enough for typical dropped packets.
  this->raw_hex_ = hex_prefix_(this->data_, 256);

  // drop junk / partial frames (noise)
  const auto mode = this->link_mode();
  if (mode == LinkMode::T1 && this->data_.size() < 60) {
    this->drop_reason_ = "too_short";
    return {};
  }
  if (mode == LinkMode::C1 && this->data_.size() < 16) {
    this->drop_reason_ = "too_short";
    return {};
  }

  // RAW-only: do not rely on expected_size gating (it can be wrong on partial prefixes).
  // We instead require successful decode/sanity and then trim based on decoded L-field.
  if (mode == LinkMode::T1) {
    this->frame_format_ = "A";  // assumption (good enough for water meters you're seeing)
    auto decoded_data = decode3of6(this->data_);
    if (!decoded_data || decoded_data->size() < 2) {
      this->drop_reason_ = "decode_failed";
      return {};
    }
    this->data_ = decoded_data.value();

    // Sanity based on L-field
    const uint8_t l = this->data_[0];
    const size_t want = (size_t) l + 1;
    const size_t need_total = total_len_format_a_with_crc_(l);
    this->want_len_ = need_total;
    this->got_len_ = this->data_.size();
    if (want < 12 || want > 260) {
      this->drop_reason_ = "l_field_invalid";
      return {};
    }
    if (this->data_.size() < need_total) {
      this->truncated_ = true;
      this->drop_reason_ = "truncated";
      return {};
    }

    // Keep only what we need (drop any trailing garbage)
    if (this->data_.size() > need_total) this->data_.resize(need_total);

    // Validate and strip DLL CRC bytes (Format A)
    if (!wmbus_common::trim_dll_crc_format_a(this->data_)) {
      this->drop_reason_ = "dll_crc_failed";
      return {};
    }

  } else if (mode == LinkMode::C1) {
    if (this->data_.size() < 3) {
      this->drop_reason_ = "too_short";
      return {};
    }
    if (this->data_[1] == WMBUS_BLOCK_A_PREAMBLE) {
      this->frame_format_ = "A";
    } else if (this->data_[1] == WMBUS_BLOCK_B_PREAMBLE) {
      this->frame_format_ = "B";
    } else {
      this->drop_reason_ = "unknown_preamble";
      return {};
    }

    // remove C-mode suffix bytes
    if (this->data_.size() < WMBUS_MODE_C_SUFIX_LEN) {
      this->drop_reason_ = "too_short";
      return {};
    }
    this->data_.erase(this->data_.begin(), this->data_.begin() + WMBUS_MODE_C_SUFIX_LEN);

    // Sanity based on L-field now at [0]
    const uint8_t l = this->data_[0];
    const size_t want = (size_t) l + 1;
    const size_t need_total = (this->frame_format_ == "A")
                                 ? total_len_format_a_with_crc_(l)
                                 : total_len_format_b_with_crc_(l);
    this->want_len_ = need_total;
    this->got_len_ = this->data_.size();
    if (want < 12 || want > 260) {
      this->drop_reason_ = "l_field_invalid";
      return {};
    }
    if (this->data_.size() < need_total) {
      this->truncated_ = true;
      this->drop_reason_ = "truncated";
      return {};
    }
    if (this->data_.size() > need_total) this->data_.resize(need_total);

    // Validate and strip DLL CRC bytes for both formats
    if (this->frame_format_ == "A") {
      if (!wmbus_common::trim_dll_crc_format_a(this->data_)) {
        this->drop_reason_ = "dll_crc_failed";
        return {};
      }
    } else {  // "B"
      if (!wmbus_common::trim_dll_crc_format_b(this->data_)) {
        this->drop_reason_ = "dll_crc_failed";
        return {};
      }
    }

  } else {
    this->drop_reason_ = "unknown_link_mode";
    return {};
  }

  // OK -> publish
  frame.emplace(this);
  return frame;
}

Frame::Frame(Packet *packet)
    : data_(std::move(packet->data_)), link_mode_(packet->link_mode_),
      rssi_(packet->rssi_), format_(packet->frame_format_) {}

std::vector<uint8_t> &Frame::data() { return this->data_; }
LinkMode Frame::link_mode() { return this->link_mode_; }
int8_t Frame::rssi() { return this->rssi_; }
std::string Frame::format() { return this->format_; }

std::vector<uint8_t> Frame::as_raw() { return this->data_; }
std::string Frame::as_hex() { return format_hex(this->data_); }

std::string Frame::as_rtlwmbus() {
  const size_t time_repr_size = sizeof("YYYY-MM-DD HH:MM:SS.00Z");
  char time_buffer[time_repr_size];
  auto t = std::time(NULL);
  std::strftime(time_buffer, time_repr_size, "%F %T.00Z", std::gmtime(&t));

  auto output = std::string{};
  output.reserve(2 + 5 + 24 + 1 + 4 + 5 + 2 * this->data_.size() + 1);

  output += link_mode_name(this->link_mode_);
  output += ";1;1;";
  output += time_buffer;
  output += ';';
  output += std::to_string(this->rssi_);
  output += ";;;0x";
  output += this->as_hex();
  output += "\n";

  return output;
}

void Frame::mark_as_handled() { this->handlers_count_++; }
uint8_t Frame::handlers_count() { return this->handlers_count_; }

}  // namespace wmbus_radio
}  // namespace esphome
