#include "packet.h"

#include <algorithm>
#include <ctime>
#include <cstdio>

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

static std::string dll_crc_detail_(const wmbus_common::DLLCRCResult &crc) {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "format=%s stage=%s calc=%04x expected=%04x data_pos=%u data_len=%u crc_pos=%u input_len=%u",
           crc.format,
           crc.stage,
           (unsigned) crc.calculated,
           (unsigned) crc.expected,
           (unsigned) crc.data_pos,
           (unsigned) crc.data_len,
           (unsigned) crc.crc_pos,
           (unsigned) crc.input_len);
  return std::string(buf);
}


static bool is_bcd_(uint8_t b) {
  return ((b & 0x0F) <= 9) && (((b >> 4) & 0x0F) <= 9);
}

static bool try_extract_meter_id_(const std::vector<uint8_t> &d, uint32_t &out_id) {
  out_id = 0;
  if (d.size() < 9) return false;

  int base = -1;
  // Raw C1 still carrying the two leading suffix bytes.
  if (d.size() >= 12 && (size_t) (d[2] + 1) == (d.size() - 2)) {
    base = 2;
  } else if (d.size() >= 10 && (size_t) (d[0] + 1) == d.size()) {
    // Final frame with explicit C-field after L-field.
    base = 1;
  } else {
    // Best effort for late-stage / already-trimmed packets.
    base = 0;
  }

  if ((size_t) (base + 6) >= d.size()) return false;
  if (!is_bcd_(d[base + 3]) || !is_bcd_(d[base + 4]) || !is_bcd_(d[base + 5]) || !is_bcd_(d[base + 6])) {
    return false;
  }

  out_id = (uint32_t) ((((d[base + 6] >> 4) & 0x0F) * 10000000U) + ((d[base + 6] & 0x0F) * 1000000U) +
                       (((d[base + 5] >> 4) & 0x0F) * 100000U) + ((d[base + 5] & 0x0F) * 10000U) +
                       (((d[base + 4] >> 4) & 0x0F) * 1000U) + ((d[base + 4] & 0x0F) * 100U) +
                       (((d[base + 3] >> 4) & 0x0F) * 10U) + (d[base + 3] & 0x0F));
  return out_id != 0;
}

Packet::Packet() { this->data_.reserve(WMBUS_PREAMBLE_SIZE); }

bool Packet::try_get_meter_id(uint32_t &out_id) const { return try_extract_meter_id_(this->data_, out_id); }

void Packet::set_drop_(const char *stage, const char *reason, const std::string &detail) {
  this->drop_stage_ = stage != nullptr ? stage : "";
  this->drop_reason_ = reason != nullptr ? reason : "";
  this->drop_detail_ = detail;
}

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
      auto decoded = decode3of6(tmp, nullptr);
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


namespace {

// Internal parse result used by the fallback parser.
// It lets us try more than one interpretation of the same raw packet without
// losing the diagnostic context of the best candidate.
struct ParseAttemptResult {
  bool ok{false};
  bool truncated{false};
  LinkMode mode{LinkMode::UNKNOWN};
  std::string frame_format{};
  std::vector<uint8_t> data{};

  size_t want_len{0};
  size_t got_len{0};
  size_t raw_got_len{0};
  size_t decoded_len{0};
  size_t final_len{0};
  uint16_t dll_crc_removed{0};
  uint8_t suffix_ignored{0};

  std::string drop_reason{};
  std::string drop_stage{};
  std::string drop_detail{};
  uint16_t t1_symbols_total{0};
  uint16_t t1_symbols_invalid{0};
};

static inline void set_attempt_drop_(ParseAttemptResult &out, const char *stage, const char *reason,
                                     const std::string &detail = {}) {
  out.ok = false;
  out.drop_stage = stage != nullptr ? stage : "";
  out.drop_reason = reason != nullptr ? reason : "";
  out.drop_detail = detail;
}

// Rough stage ordering used when both parsing attempts fail.
// Higher rank means the parser got further and usually has a more useful reason.
static int stage_rank_(const std::string &stage) {
  if (stage == "precheck" || stage == "c1_precheck") return 1;
  if (stage == "c1_preamble" || stage == "c1_suffix" || stage == "t1_decode3of6") return 2;
  if (stage == "t1_l_field" || stage == "c1_l_field") return 3;
  if (stage == "t1_length_check" || stage == "c1_length_check") return 4;
  if (stage == "dll_crc_first" || stage == "dll_crc_mid" || stage == "dll_crc_final" ||
      stage == "dll_crc_b1" || stage == "dll_crc_b2") return 5;
  if (stage == "link_mode") return 0;
  return 0;
}

static const ParseAttemptResult &pick_better_failure_(const ParseAttemptResult &a, const ParseAttemptResult &b) {
  const int ra = stage_rank_(a.drop_stage);
  const int rb = stage_rank_(b.drop_stage);
  if (ra != rb) return (ra > rb) ? a : b;
  // Prefer the attempt that collected more decoded data.
  if (a.decoded_len != b.decoded_len) return (a.decoded_len > b.decoded_len) ? a : b;
  // Prefer truncated over generic decode failures: it usually means framing was at least partially correct.
  if (a.truncated != b.truncated) return a.truncated ? a : b;
  return a;
}

// T1 parser with a soft precheck.
// We intentionally avoid a hard early reject like "raw < 60 => drop" because
// some borderline packets can still reach a valid L-field after 3-of-6 decode.
static ParseAttemptResult try_parse_t1_(const std::vector<uint8_t> &raw) {
  ParseAttemptResult out;
  out.mode = LinkMode::T1;
  out.frame_format = "A";  // Current bridge handles T1 as format A.
  out.raw_got_len = raw.size();

  if (raw.size() < 12) {
    set_attempt_drop_(out, "precheck", "too_short",
                      "mode=T1 raw_len=" + std::to_string((unsigned) raw.size()) + " min=12");
    return out;
  }

  std::vector<uint8_t> decoded_input = raw;
  Decode3of6Stats st;
  auto decoded_data = decode3of6(decoded_input, &st);
  out.t1_symbols_total = st.symbols_total;
  out.t1_symbols_invalid = st.symbols_invalid;
  if (!decoded_data || decoded_data->size() < 2) {
    char detail[160];
    snprintf(detail, sizeof(detail), "symbols_total=%u symbols_invalid=%u raw_len=%u",
             (unsigned) out.t1_symbols_total, (unsigned) out.t1_symbols_invalid, (unsigned) out.raw_got_len);
    set_attempt_drop_(out, "t1_decode3of6", "decode_failed", detail);
    return out;
  }

  out.data = std::move(decoded_data.value());
  out.decoded_len = out.data.size();

  const uint8_t l = out.data[0];
  const size_t want = (size_t) l + 1;
  const size_t need_total = total_len_format_a_with_crc_(l);
  out.want_len = need_total;
  out.got_len = out.data.size();

  if (want < 12 || want > 260) {
    char detail[128];
    snprintf(detail, sizeof(detail), "l_field=%u decoded_len=%u want=%u need_total=%u",
             (unsigned) l, (unsigned) out.decoded_len, (unsigned) want, (unsigned) need_total);
    set_attempt_drop_(out, "t1_l_field", "l_field_invalid", detail);
    return out;
  }

  if (out.data.size() < need_total) {
    out.truncated = true;
    char detail[128];
    snprintf(detail, sizeof(detail), "decoded_len=%u need_total=%u l_field=%u",
             (unsigned) out.data.size(), (unsigned) need_total, (unsigned) l);
    set_attempt_drop_(out, "t1_length_check", "truncated", detail);
    return out;
  }

  if (out.data.size() > need_total) out.data.resize(need_total);

  wmbus_common::DLLCRCResult crc_diag;
  if (!wmbus_common::trim_dll_crc_format_a(out.data, &crc_diag)) {
    set_attempt_drop_(out, crc_diag.stage, "dll_crc_failed", dll_crc_detail_(crc_diag));
    return out;
  }

  out.dll_crc_removed = crc_diag.removed_bytes;
  out.final_len = out.data.size();
  out.ok = true;
  return out;
}

// C1 parser. Format A/B is selected from the block preamble after the two
// leading C-mode bytes are removed.
static ParseAttemptResult try_parse_c1_(const std::vector<uint8_t> &raw) {
  ParseAttemptResult out;
  out.mode = LinkMode::C1;
  out.raw_got_len = raw.size();

  if (raw.size() < 3) {
    set_attempt_drop_(out, "c1_precheck", "too_short",
                      "mode=C1 raw_len=" + std::to_string((unsigned) raw.size()) + " min=3");
    return out;
  }

  out.data = raw;
  if (out.data[0] != WMBUS_MODE_C_PREAMBLE) {
    char detail[64];
    snprintf(detail, sizeof(detail), "first_byte=%02X raw_len=%u",
             (unsigned) out.data[0], (unsigned) out.data.size());
    set_attempt_drop_(out, "c1_precheck", "unknown_preamble", detail);
    return out;
  }

  if (out.data[1] == WMBUS_BLOCK_A_PREAMBLE) {
    out.frame_format = "A";
  } else if (out.data[1] == WMBUS_BLOCK_B_PREAMBLE) {
    out.frame_format = "B";
  } else {
    char detail[64];
    snprintf(detail, sizeof(detail), "preamble=%02X raw_len=%u", (unsigned) out.data[1], (unsigned) out.data.size());
    set_attempt_drop_(out, "c1_preamble", "unknown_preamble", detail);
    return out;
  }

  if (out.data.size() < WMBUS_MODE_C_SUFIX_LEN) {
    set_attempt_drop_(out, "c1_suffix", "too_short",
                      "raw_len=" + std::to_string((unsigned) out.data.size()) + " min_suffix=2");
    return out;
  }

  out.data.erase(out.data.begin(), out.data.begin() + WMBUS_MODE_C_SUFIX_LEN);
  out.suffix_ignored = WMBUS_MODE_C_SUFIX_LEN;
  out.decoded_len = out.data.size();

  const uint8_t l = out.data[0];
  const size_t want = (size_t) l + 1;
  const size_t need_total = (out.frame_format == "A") ? total_len_format_a_with_crc_(l)
                                                        : total_len_format_b_with_crc_(l);
  out.want_len = need_total;
  out.got_len = out.data.size();

  if (want < 12 || want > 260) {
    char detail[144];
    snprintf(detail, sizeof(detail), "format=%s l_field=%u decoded_len=%u want=%u need_total=%u",
             out.frame_format.c_str(), (unsigned) l, (unsigned) out.decoded_len,
             (unsigned) want, (unsigned) need_total);
    set_attempt_drop_(out, "c1_l_field", "l_field_invalid", detail);
    return out;
  }

  if (out.data.size() < need_total) {
    out.truncated = true;
    char detail[144];
    snprintf(detail, sizeof(detail), "format=%s decoded_len=%u need_total=%u l_field=%u",
             out.frame_format.c_str(), (unsigned) out.data.size(), (unsigned) need_total, (unsigned) l);
    set_attempt_drop_(out, "c1_length_check", "truncated", detail);
    return out;
  }

  if (out.data.size() > need_total) out.data.resize(need_total);

  wmbus_common::DLLCRCResult crc_diag;
  if (out.frame_format == "A") {
    if (!wmbus_common::trim_dll_crc_format_a(out.data, &crc_diag)) {
      set_attempt_drop_(out, crc_diag.stage, "dll_crc_failed", dll_crc_detail_(crc_diag));
      return out;
    }
  } else {
    if (!wmbus_common::trim_dll_crc_format_b(out.data, &crc_diag)) {
      set_attempt_drop_(out, crc_diag.stage, "dll_crc_failed", dll_crc_detail_(crc_diag));
      return out;
    }
  }

  out.dll_crc_removed = crc_diag.removed_bytes;
  out.final_len = out.data.size();
  out.ok = true;
  return out;
}

}  // namespace

std::optional<Frame> Packet::convert_to_frame() {
  std::optional<Frame> frame = {};

  // Reset diagnostics collected for the current packet.
  this->truncated_ = false;
  this->want_len_ = 0;
  this->got_len_ = 0;
  this->raw_got_len_ = this->data_.size();
  this->decoded_len_ = 0;
  this->final_len_ = 0;
  this->dll_crc_removed_ = 0;
  this->suffix_ignored_ = 0;
  this->drop_reason_.clear();
  this->drop_stage_.clear();
  this->drop_detail_.clear();
  this->t1_symbols_total_ = 0;
  this->t1_symbols_invalid_ = 0;

  // Capture raw bytes early so dropped packets can be inspected later from MQTT/logs.
  this->raw_hex_ = hex_prefix_(this->data_, 256);

  const std::vector<uint8_t> raw = this->data_;
  const bool looks_c1 = !raw.empty() && raw[0] == WMBUS_MODE_C_PREAMBLE;

  // Preferred path is based on the first byte, but we always keep a fallback.
  // This avoids a hard one-shot decision where a borderline packet gets pushed
  // through the wrong parser and we lose a valid candidate.
  const ParseAttemptResult first = looks_c1 ? try_parse_c1_(raw) : try_parse_t1_(raw);
  const ParseAttemptResult second = looks_c1 ? try_parse_t1_(raw) : try_parse_c1_(raw);

  const ParseAttemptResult *chosen = nullptr;
  bool fallback_used = false;
  if (first.ok) {
    chosen = &first;
  } else if (second.ok) {
    chosen = &second;
    fallback_used = true;
  } else {
    chosen = &pick_better_failure_(first, second);
  }

  // Copy chosen result back into the packet object used by the rest of the pipeline.
  this->data_ = chosen->data;
  this->link_mode_ = chosen->mode;
  this->frame_format_ = chosen->frame_format;
  this->truncated_ = chosen->truncated;
  this->want_len_ = chosen->want_len;
  this->got_len_ = chosen->got_len;
  this->decoded_len_ = chosen->decoded_len;
  this->final_len_ = chosen->final_len;
  this->dll_crc_removed_ = chosen->dll_crc_removed;
  this->suffix_ignored_ = chosen->suffix_ignored;
  this->drop_reason_ = chosen->drop_reason;
  this->drop_stage_ = chosen->drop_stage;
  this->drop_detail_ = chosen->drop_detail;
  this->t1_symbols_total_ = chosen->t1_symbols_total;
  this->t1_symbols_invalid_ = chosen->t1_symbols_invalid;

  if (!chosen->ok) {
    if (fallback_used) {
      this->drop_detail_ += (this->drop_detail_.empty() ? "" : " ");
      this->drop_detail_ += "fallback_used=1";
    }
    return {};
  }

  if (fallback_used) {
    // Keep a short breadcrumb in diagnostics: it helps explain why an apparently
    // odd packet still decoded successfully.
    this->drop_detail_ = "fallback_used=1";
  }

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

bool Frame::try_get_meter_id(uint32_t &out_id) const { return try_extract_meter_id_(this->data_, out_id); }

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
