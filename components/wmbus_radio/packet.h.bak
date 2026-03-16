#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

// Keep wmbus_radio lightweight: do NOT pull full wmbusmeters/wmbus_common.
// We only need LinkMode names and basic helpers.
#include "link_mode.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace wmbus_radio {

struct Frame;

struct Packet {
  friend class Frame;

public:
  Packet();

  // Reserve/extend internal buffer and return pointer to the newly appended
  // region. The returned memory is valid until the next reallocation.
  uint8_t *append_space(size_t len);

  // Expected total packet size (including PHY header bytes as provided by
  // the transceiver). Returns 0 if it can't be determined from current data.
  size_t expected_size();

  void set_rssi(int8_t rssi);

  std::optional<Frame> convert_to_frame();

  // Basic getters for diagnostics
  LinkMode get_link_mode() { return this->link_mode(); }
  int8_t get_rssi() const { return this->rssi_; }

  // Diagnostics (populated when convert_to_frame() rejects a packet)
  bool is_truncated() const { return this->truncated_; }
  size_t want_len() const { return this->want_len_; }
  size_t got_len() const { return this->got_len_; }
  size_t raw_got_len() const { return this->raw_got_len_; }
  const std::string &drop_reason() const { return this->drop_reason_; }

  // Raw packet bytes (hex) captured at the beginning of convert_to_frame().
  // Intended for diagnostics; may be truncated to keep MQTT/log payloads small.
  const std::string &raw_hex() const { return this->raw_hex_; }

  // T1 (3-of-6) symbol diagnostics (only meaningful for LinkMode::T1)
  uint16_t t1_symbols_total() const { return this->t1_symbols_total_; }
  uint16_t t1_symbols_invalid() const { return this->t1_symbols_invalid_; }

protected:
  std::vector<uint8_t> data_;

  size_t expected_size_ = 0;

  uint8_t l_field();
  int8_t rssi_ = 0;

  LinkMode link_mode();
  LinkMode link_mode_ = LinkMode::UNKNOWN;

  std::string frame_format_;

  // Diagnostics
  bool truncated_{false};
  size_t want_len_{0};
  size_t got_len_{0};
  size_t raw_got_len_{0};
  std::string drop_reason_{};
  std::string raw_hex_{};

  // T1 (3-of-6) symbol diagnostics
  uint16_t t1_symbols_total_{0};
  uint16_t t1_symbols_invalid_{0};
};

struct Frame {
public:
  Frame(Packet *packet);

  std::vector<uint8_t> &data();
  LinkMode link_mode();
  int8_t rssi();
  std::string format();

  std::vector<uint8_t> as_raw();
  std::string as_hex();
  std::string as_rtlwmbus();

  void mark_as_handled();
  uint8_t handlers_count();

protected:
  std::vector<uint8_t> data_;
  LinkMode link_mode_;
  int8_t rssi_;
  std::string format_;
  uint8_t handlers_count_ = 0;
};

} // namespace wmbus_radio
} // namespace esphome
