#include "packet.h"

#include <algorithm>
#include <ctime>

// NOTE: Do NOT include full wmbusmeters/wmbus_common here.
// This file must stay lightweight to allow "RF -> RAW" usage.
#include "esphome/core/helpers.h"

#include "decode3of6.h"

#define WMBUS_PREAMBLE_SIZE (3)
#define WMBUS_MODE_C_SUFIX_LEN (2)
#define WMBUS_MODE_C_PREAMBLE (0x54)
#define WMBUS_BLOCK_A_PREAMBLE (0xCD)
#define WMBUS_BLOCK_B_PREAMBLE (0x3D)

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "packet";
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

// Get value of L-field
uint8_t Packet::l_field() {
  switch (this->link_mode()) {
  case LinkMode::C1:
    if (this->data_.size() < 3)
      return 0;
    return this->data_[2];
  case LinkMode::T1: {
    // For T-mode we only need enough coded bytes to decode the first decoded
    // byte (L-field). Using the whole (possibly partial) buffer increases the
    // risk of decode failure when only a header is present.
    const size_t n = std::min<size_t>(this->data_.size(), 3);
    std::vector<uint8_t> tmp(this->data_.begin(), this->data_.begin() + n);
    auto decoded = decode3of6(tmp);
    if (decoded)
      return (*decoded)[0];
  }
  }
  return 0;
}

size_t Packet::expected_size() {
  if (this->data_.size() < WMBUS_PREAMBLE_SIZE)
    return 0;
  if (!this->expected_size_) {
    // Format A
    //   L-field = length without CRC fields and without L (1 byte)
    // Format B
    //   L-field = length with CRC fields and without L (1 byte)
    auto l_field = this->l_field();

    // The 2 first blocks contains 25 bytes when excluding CRC and the L-field
    // The other blocks contains 16 bytes when excluding the CRC-fields
    // Less than 26 (15 + 10)
    auto nrBlocks = l_field < 26 ? 2 : (l_field - 26) / 16 + 3;

    // Add all extra fields, excluding the CRC fields + 2 CRC bytes for each
    // block
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

std::optional<Frame> Packet::convert_to_frame() {
  std::optional<Frame> frame = {};

  ESP_LOGD(TAG, "Have data from radio (%zu bytes)", this->data_.size());
  // Intentionally no wmbusmeters debugPayload() here to avoid pulling heavy code.

  const size_t exp = this->expected_size();
  if (exp > 0 && this->data_.size() >= exp) {
    if (this->data_.size() > exp) {
      ESP_LOGV(TAG, "Trimming extra bytes: have %zu, expected %zu", this->data_.size(), exp);
      this->data_.resize(exp);
    }
    if (this->link_mode() == LinkMode::T1) {
      // TODO: Remove assumption that T1 is always A
      this->frame_format_ = "A";
      auto decoded_data = decode3of6(this->data_);
      if (decoded_data)
        this->data_ = decoded_data.value();
    } else if (this->link_mode() == LinkMode::C1) {
      if (this->data_[1] == WMBUS_BLOCK_A_PREAMBLE)
        this->frame_format_ = "A";
      else if (this->data_[1] == WMBUS_BLOCK_B_PREAMBLE)
        this->frame_format_ = "B";
      this->data_.erase(this->data_.begin(),
                        this->data_.begin() + WMBUS_MODE_C_SUFIX_LEN);
    } else {
      ESP_LOGE(TAG, "unknown link mode!");
    }
  } else {
    ESP_LOGE(TAG, "expected_size: %zu NOT size: %zu", exp, this->data_.size());
  }

  // RAW-only mode: we do NOT validate/trim via wmbusmeters.
  // We only:
  //  - ensure we have the expected number of PHY bytes
  //  - decode 3of6 for T1
  //  - strip C-mode suffix
  // The resulting frame may still contain DLL CRC bytes; external decoders can handle that.
  frame.emplace(this);

  delete this;

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

  output += esphome::wmbus_common::linkModeName(this->link_mode_); // size 2
  output += ";1;1;";                        // size 5
  output += time_buffer;                    // size 24
  output += ';';                            // size 1
  output += std::to_string(this->rssi_);    // size up to 4
  output += ";;;0x";                        // size 5
  output += this->as_hex();                 // size 2 * frame.size()
  output += "\n";                           // size 1

  return output;
}

void Frame::mark_as_handled() { this->handlers_count_++; }
uint8_t Frame::handlers_count() { return this->handlers_count_; }

} // namespace wmbus_radio
} // namespace esphome
