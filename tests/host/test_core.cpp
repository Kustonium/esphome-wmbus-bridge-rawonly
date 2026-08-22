#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "decode3of6.h"
#include "dll_crc.h"
#include "meter_filter.h"
#include "packet.h"
#include "rx_metadata.h"

using esphome::wmbus_common::crc16_en13757;
using esphome::wmbus_common::trim_dll_crc_format_a;
using esphome::wmbus_common::trim_dll_crc_format_b;
using esphome::wmbus_radio::Decode3of6Stats;
using esphome::wmbus_radio::Frame;
using esphome::wmbus_radio::LinkMode;
using esphome::wmbus_radio::Packet;
using esphome::wmbus_radio::decode3of6;
using esphome::wmbus_radio::encoded_size;
using esphome::wmbus_radio::meter_id_allowed;
using esphome::wmbus_radio::meter_id_in_lists;
using esphome::wmbus_radio::frame_crc32;

static int failures = 0;

static void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    failures++;
  }
}

static void check(bool condition, const std::string &message) {
  check(condition, message.c_str());
}

struct GoldenFrameFixture {
  const char *name;
  const char *hex;
  uint32_t expected_meter_id;
};

static std::vector<uint8_t> encode_3of6(const std::vector<uint8_t> &decoded) {
  static const uint8_t lookup[16] = {
      0b010110, 0b001101, 0b001110, 0b001011,
      0b011100, 0b011001, 0b011010, 0b010011,
      0b101100, 0b100101, 0b100110, 0b100011,
      0b110100, 0b110001, 0b110010, 0b101001,
  };

  std::vector<uint8_t> out;
  uint8_t current = 0;
  uint8_t bits = 0;

  auto push_bit = [&](uint8_t bit) {
    current = (uint8_t) ((current << 1) | (bit & 0x01));
    bits++;
    if (bits == 8) {
      out.push_back(current);
      current = 0;
      bits = 0;
    }
  };

  for (uint8_t byte : decoded) {
    const uint8_t nibbles[2] = {
        (uint8_t) ((byte >> 4) & 0x0F),
        (uint8_t) (byte & 0x0F),
    };
    for (uint8_t nibble : nibbles) {
      uint8_t code = lookup[nibble];
      for (int bit = 5; bit >= 0; bit--) {
        push_bit((uint8_t) ((code >> bit) & 0x01));
      }
    }
  }

  if (bits != 0) {
    current = (uint8_t) (current << (8 - bits));
    out.push_back(current);
  }

  return out;
}

static void append_crc(std::vector<uint8_t> &payload, size_t pos, size_t len) {
  const uint16_t crc = crc16_en13757(payload.data() + pos, len);
  payload.push_back((uint8_t) ((crc >> 8) & 0xFF));
  payload.push_back((uint8_t) (crc & 0xFF));
}

static uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9') return (uint8_t) (c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t) (10 + c - 'a');
  if (c >= 'A' && c <= 'F') return (uint8_t) (10 + c - 'A');
  check(false, "hex fixture contains an invalid digit");
  return 0;
}

static std::vector<uint8_t> hex_to_bytes(const std::string &hex) {
  check((hex.size() % 2) == 0, "hex fixture has an even number of digits");
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back((uint8_t) ((hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1])));
  }
  return out;
}

static Packet make_packet(const std::vector<uint8_t> &raw, int8_t rssi,
                          std::optional<LinkMode> forced = std::nullopt) {
  Packet packet;
  auto *dst = packet.append_space(raw.size());
  std::copy(raw.begin(), raw.end(), dst);
  packet.set_rssi(rssi);
  if (forced) packet.set_forced_link_mode(*forced);
  return packet;
}

static std::vector<uint8_t> encode_s1_manchester(const std::vector<uint8_t> &decoded) {
  std::vector<uint8_t> out;
  uint8_t current = 0;
  uint8_t bits = 0;

  auto push_bit = [&](uint8_t bit) {
    current = (uint8_t) ((current << 1) | (bit & 0x01));
    bits++;
    if (bits == 8) {
      out.push_back(current);
      current = 0;
      bits = 0;
    }
  };

  for (uint8_t byte : decoded) {
    for (int bit = 7; bit >= 0; bit--) {
      const bool one = ((byte >> bit) & 0x01) != 0;
      // Parser polarity=false expects 01 -> 0 and 10 -> 1.
      push_bit(one ? 1 : 0);
      push_bit(one ? 0 : 1);
    }
  }

  if (bits != 0) {
    current = (uint8_t) (current << (8 - bits));
    out.push_back(current);
  }

  return out;
}

static void make_manchester_erasure(std::vector<uint8_t> &raw, size_t decoded_bit) {
  const size_t first_chip = decoded_bit * 2U;
  for (size_t chip = first_chip; chip <= first_chip + 1U; chip++)
    raw[chip >> 3U] &= (uint8_t) ~(0x80U >> (chip & 7U));  // invalid pair 00
}

static std::vector<size_t> one_bit_positions(const std::vector<uint8_t> &decoded,
                                             size_t first_bit, size_t end_bit, size_t count) {
  std::vector<size_t> out;
  for (size_t bit = first_bit; bit < end_bit && out.size() < count; bit++)
    if (((decoded[bit >> 3U] >> (7U - (bit & 7U))) & 1U) != 0) out.push_back(bit);
  check(out.size() == count, "fixture contains enough one bits for erasures");
  return out;
}

static std::vector<uint8_t> base_frame_body() {
  return {
      0x0B,  // L-field: 11 bytes follow, 12 bytes total including L-field.
      0x44, 0x12, 0x34, 0x56,
      0x78, 0x90, 0x01, 0x02,
      0x7A, 0xAA, 0x55,
  };
}

static std::vector<uint8_t> format_a_with_crc_from_body(const std::vector<uint8_t> &body) {
  check(body.size() >= 10, "Format A fixture has a first 10-byte block");
  std::vector<uint8_t> payload(body.begin(), body.begin() + 10);
  append_crc(payload, 0, 10);

  size_t offset = 10;
  while (body.size() - offset > 16) {
    const size_t crc_pos = payload.size();
    payload.insert(payload.end(), body.begin() + offset, body.begin() + offset + 16);
    append_crc(payload, crc_pos, 16);
    offset += 16;
  }

  if (offset < body.size()) {
    const size_t len = body.size() - offset;
    const size_t crc_pos = payload.size();
    payload.insert(payload.end(), body.begin() + offset, body.end());
    append_crc(payload, crc_pos, len);
  }

  return payload;
}

static std::vector<uint8_t> format_a_with_crc() {
  return format_a_with_crc_from_body(base_frame_body());
}

static std::vector<GoldenFrameFixture> golden_frame_fixtures() {
  return {
      {
          "long",
          "8e44b3380799080003027a09008005d518489e938d7741372ca5f34153b1f81e0e7f71fc2ce87ac30edb358dcd2b"
          "4731ecd84f7e705bb3f425aaa0a6df7f654a87b9289d49d7ad83cd2776a0627b6d528cb602da1b6455efca61"
          "f42dbc25387f1f6acdcbf633a5fde8e0b0e4f9153499cb94f3e64229969d9baaac567112988f3ba86728747"
          "b0dc9590d7a5afde493",
          89907,
      },
      {
          "short",
          "2f446850791255417462a2069f333904d00b09000000090000000000000000000002121113190b130400000000000100",
          41551279,
      },
      {
          "mid",
          "374468508107839027c3a2129f335c5600289e020000800e0000000030c0050f6cc1c7144b6921c12a0748e000000000000000000000c001",
          90830781,
      },
  };
}

static void test_decode3of6_round_trip() {
  const std::vector<uint8_t> decoded = {0x00, 0x12, 0xAB, 0xFF, 0x5C};
  auto coded = encode_3of6(decoded);
  Decode3of6Stats stats;
  auto actual = decode3of6(coded, &stats);

  check(actual.has_value(), "3-of-6 valid payload decodes");
  check(actual.value_or(std::vector<uint8_t>{}) == decoded, "3-of-6 round trip preserves bytes");
  check(stats.symbols_total == decoded.size() * 2, "3-of-6 stats count two symbols per byte");
  check(stats.symbols_invalid == 0, "3-of-6 stats report no invalid symbols");

  check(encoded_size(0) == 0, "encoded_size(0)");
  check(encoded_size(1) == 2, "encoded_size(1)");
  check(encoded_size(2) == 3, "encoded_size(2)");
  check(encoded_size(3) == 5, "encoded_size(3)");
}

static void test_rx_metadata_primitives() {
  const std::string standard = "123456789";
  check(frame_crc32(reinterpret_cast<const uint8_t *>(standard.data()), standard.size()) == 0xCBF43926U,
        "RX metadata uses standard IEEE CRC-32");

  auto payload = format_a_with_crc();
  std::vector<uint8_t> raw = {0x54, 0xCD};
  raw.insert(raw.end(), payload.begin(), payload.end());
  Packet packet = make_packet(raw, -65);
  packet.set_rx_task_wakeup_us(123456789ULL);
  auto frame = packet.convert_to_frame();
  check(frame.has_value(), "RX timestamp fixture converts to a frame");
  if (frame.has_value())
    check(frame->rx_task_wakeup_us() == 123456789ULL, "RX task wake timestamp survives packet conversion");
}

static void test_decode3of6_invalid_symbol() {
  std::vector<uint8_t> coded = {0xFF, 0xFF};
  Decode3of6Stats stats;
  auto actual = decode3of6(coded, &stats);

  check(!actual.has_value(), "3-of-6 invalid payload is rejected");
  check(stats.symbols_invalid > 0, "3-of-6 invalid stats are populated");
}

static void test_dll_crc_format_a() {
  auto payload = format_a_with_crc();
  auto expected = base_frame_body();
  check(trim_dll_crc_format_a(payload), "Format A DLL CRC validates");
  check(payload == expected, "Format A DLL CRC is stripped");

  auto bad = format_a_with_crc();
  bad.back() ^= 0x01;
  check(!trim_dll_crc_format_a(bad), "Format A bad DLL CRC is rejected");
}

static void test_dll_crc_format_b() {
  auto payload = base_frame_body();
  append_crc(payload, 0, payload.size());
  auto expected = base_frame_body();

  check(trim_dll_crc_format_b(payload), "Format B DLL CRC validates");
  check(payload == expected, "Format B DLL CRC is stripped");

  auto bad = base_frame_body();
  append_crc(bad, 0, bad.size());
  bad[bad.size() - 2] ^= 0x01;
  check(!trim_dll_crc_format_b(bad), "Format B bad DLL CRC is rejected");
}

static void test_packet_t1_frame_conversion() {
  auto raw = encode_3of6(format_a_with_crc());
  Packet packet = make_packet(raw, -71);

  auto frame = packet.convert_to_frame();
  check(frame.has_value(), "T1 raw packet converts to frame");
  if (!frame) return;
  check(frame->link_mode() == LinkMode::T1, "T1 frame mode is retained");
  check(frame->rssi() == -71, "T1 frame RSSI is retained");
  check(frame->as_raw() == base_frame_body(), "T1 frame strips coding and DLL CRC");

  uint32_t meter_id = 0;
  check(frame->try_get_meter_id(meter_id), "T1 frame meter id is extractable");
  check(meter_id == 1907856, "T1 frame meter id value");
}

static void test_packet_c1_frame_conversion() {
  auto payload = format_a_with_crc();
  std::vector<uint8_t> raw = {0x54, 0xCD};
  raw.insert(raw.end(), payload.begin(), payload.end());

  Packet packet = make_packet(raw, -65);

  auto frame = packet.convert_to_frame();
  check(frame.has_value(), "C1 raw packet converts to frame");
  if (!frame) return;
  check(frame->link_mode() == LinkMode::C1, "C1 frame mode is retained");
  check(frame->rssi() == -65, "C1 frame RSSI is retained");
  check(frame->format() == "A", "C1 frame format A is detected");
  check(frame->as_raw() == base_frame_body(), "C1 frame strips suffix and DLL CRC");
}

static void test_packet_s1_frame_conversion() {
  auto raw = encode_s1_manchester(format_a_with_crc());
  Packet packet = make_packet(raw, -83, LinkMode::S1);

  auto frame = packet.convert_to_frame();
  check(frame.has_value(), "S1 Manchester raw packet converts to frame");
  if (!frame) return;
  check(frame->link_mode() == LinkMode::S1, "S1 frame mode is retained");
  check(frame->rssi() == -83, "S1 frame RSSI is retained");
  check(frame->format() == "A", "S1 frame format A is detected");
  check(frame->as_raw() == base_frame_body(), "S1 frame strips Manchester coding and DLL CRC");
}

static void test_packet_t1_bad_crc_is_rejected() {
  auto payload = format_a_with_crc();
  payload.back() ^= 0x01;
  Packet packet = make_packet(encode_3of6(payload), -72);

  auto frame = packet.convert_to_frame();
  check(!frame.has_value(), "T1 packet with bad DLL CRC is rejected");
  check(packet.drop_reason() == "dll_crc_failed", "T1 bad CRC reports dll_crc_failed");
  check(packet.drop_stage() == "dll_crc_final", "T1 bad final CRC reports dll_crc_final");
}

static void test_packet_t1_truncated_is_rejected() {
  auto payload = format_a_with_crc();
  payload.pop_back();
  Packet packet = make_packet(encode_3of6(payload), -74);

  auto frame = packet.convert_to_frame();
  check(!frame.has_value(), "T1 truncated packet is rejected");
  check(packet.is_truncated(), "T1 truncated packet sets truncated flag");
  check(packet.drop_reason() == "truncated", "T1 truncated packet reports truncated reason");
  check(packet.drop_stage() == "t1_length_check", "T1 truncated packet reports length-check stage");
}

static void test_packet_c1_unknown_preamble_is_rejected() {
  auto payload = format_a_with_crc();
  std::vector<uint8_t> raw = {0x54, 0x00};
  raw.insert(raw.end(), payload.begin(), payload.end());
  Packet packet = make_packet(raw, -67);

  auto frame = packet.convert_to_frame();
  check(!frame.has_value(), "C1 packet with unknown block preamble is rejected");
  check(packet.drop_stage() == "c1_preamble", "C1 bad preamble reports c1_preamble stage");
  check(packet.drop_reason() == "unknown_preamble", "C1 bad preamble reports unknown_preamble");
}

static void test_packet_s1_invalid_manchester_is_rejected() {
  std::vector<uint8_t> raw(8, 0x00);
  Packet packet = make_packet(raw, -90, LinkMode::S1);

  auto frame = packet.convert_to_frame();
  check(!frame.has_value(), "S1 invalid Manchester packet is rejected");
  check(packet.drop_stage() == "s1_l_field", "S1 invalid Manchester reports S1 l-field stage");
  check(packet.drop_reason() == "l_field_invalid", "S1 invalid Manchester reports invalid L-field");
  check(packet.t1_symbols_invalid() > 0, "S1 invalid Manchester records invalid symbols");
}

static void test_packet_s1_erasures_are_recovered_per_crc_block() {
  const auto encoded = format_a_with_crc();
  auto raw = encode_s1_manchester(encoded);
  // Three erasures in the first block and two in the final block. All replace
  // transmitted one bits with invalid 00 pairs, so the ordinary zero-
  // substitution decoder necessarily fails CRC before recovery runs.
  //
  // The search starts after the L-field (bit 8), and that is a property of the
  // feature, not a convenience: block-wise CRC recovery needs the frame length
  // first, because the length is what says where the blocks end. Erase the
  // L-field itself and the packet is dropped at s1_length_check before any
  // recovery can run - there is nothing to recover into.
  auto first = one_bit_positions(encoded, 8U, 12U * 8U, 3);
  auto final = one_bit_positions(encoded, 12U * 8U, encoded.size() * 8U, 2);
  for (size_t bit : first) make_manchester_erasure(raw, bit);
  for (size_t bit : final) make_manchester_erasure(raw, bit);

  Packet packet = make_packet(raw, -85, LinkMode::S1);
  auto frame = packet.convert_to_frame();
  check(frame.has_value(), "S1 erasures are resolved independently by Format-A CRC block");
  if (frame) check(frame->as_raw() == base_frame_body(), "S1 erasure recovery restores exact frame");
}

static void test_packet_s1_too_many_erasures_are_rejected() {
  const auto encoded = format_a_with_crc();
  auto raw = encode_s1_manchester(encoded);
  // Nine erasures in one block, one above the search cap of eight, and again
  // past the L-field so the packet reaches the CRC stage at all.
  auto first = one_bit_positions(encoded, 8U, 12U * 8U, 9);
  for (size_t bit : first) make_manchester_erasure(raw, bit);

  Packet packet = make_packet(raw, -89, LinkMode::S1);
  auto frame = packet.convert_to_frame();
  check(!frame.has_value(), "S1 block above the erasure search cap is rejected");
  check(packet.drop_reason() == "dll_crc_failed", "S1 over-cap erasures retain CRC failure reason");
}

static void test_packet_s1_field_erasures_from_2026_08_14() {
  const auto expected = hex_to_bytes(
      "5444A51166067F4170077A550000000C13913456780C13A24567890C13B3567890"
      "0C13C46789010C13D57890120C13E68901230C13F79012340C13080123450C1319"
      "1234560C132A2345670C133B3456782F2F2F2F");
  const char *captures[] = {
      // RssiSync 0xA9 (-84.5 dBm): 7 erasures, blocks [3,1,0,3,0,0].
      "6665654599665656496955696aaa65566a54556a69a555996ab9666655555555555555a5565a96565a6566696a9555a5565a99596566696a6a966695959655a5565a9a5a66696a95965555a5565aa565696a9596555655a5565aa666959996596a959655565955a5565aa96915965556595a55a5565aaa6a967556595a6555a595d9596a565a55955556595a656655a5565a569656595a65666955a5565a5999595a6566666a9656696a55a5565a5a9a5a6566696a9559aa59aa59aa59aa99a599aa",
      // RssiSync 0xA7 (-83.5 dBm): 10 erasures, blocks [2,1,0,2,2,3].
      "6665656599665656696955696aaa65566a55556a49a555196a99666655555555555555a5565a96565a6566696a9555a5565a99596566686a6a966695959655a5565a9a5a66696a95965555a5565aa565696a9596555655a5565aa666959996596a959655465955a5565aa96995965556595a55a4565aaa6a965556595a6555a59559596a565a55955556595a656655a5565a569656595a65666955a5565a5999590a6566666a9656696a55a5565a5a9a5a6566696a9559aa59aa59aa598299a519aa",
  };

  for (const char *capture : captures) {
    Packet packet = make_packet(hex_to_bytes(capture), -84, LinkMode::S1);
    auto frame = packet.convert_to_frame();
    check(frame.has_value(), "real marginal S1 capture is recovered");
    if (frame) check(frame->as_raw() == expected, "real marginal S1 capture restores exact fake frame");
  }
}

// For a BCD meter the raw A-field value is simply its decimal digits read as
// hex: meter 89907 is stored as the bytes that print 00089907. This pins the
// byte order of the raw extraction, which is where a silent bug would live.
static uint32_t bcd_digits_as_hex(uint32_t decimal_id) {
  uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 4) {
    out |= (decimal_id % 10) << shift;
    decimal_id /= 10;
  }
  return out;
}

static void check_golden_body_shape(const std::vector<uint8_t> &body,
                                    const GoldenFrameFixture &fixture) {
  const std::string prefix = std::string(fixture.name) + " golden frame";
  check(!body.empty(), prefix + " is present");
  if (body.empty()) return;
  check(body.size() == (size_t) body[0] + 1, prefix + " L-field matches body length");

  Packet packet = make_packet(body, -60);
  uint32_t meter_id = 0;
  check(packet.try_get_meter_id(meter_id), prefix + " meter id is extractable");
  check(meter_id == fixture.expected_meter_id, prefix + " meter id matches expected value");

  uint32_t meter_id_raw = 0;
  check(packet.try_get_meter_id_raw(meter_id_raw), prefix + " raw meter id is extractable");
  check(meter_id_raw == bcd_digits_as_hex(fixture.expected_meter_id),
        prefix + " raw meter id byte order matches the printed id");
}

static void check_golden_round_trip_c1(const std::vector<uint8_t> &body, const char *name) {
  const std::string prefix = std::string(name) + " golden C1 frame";
  auto payload = format_a_with_crc_from_body(body);
  std::vector<uint8_t> raw = {0x54, 0xCD};
  raw.insert(raw.end(), payload.begin(), payload.end());
  Packet packet = make_packet(raw, -68);

  auto frame = packet.convert_to_frame();
  check(frame.has_value(), prefix + " converts through parser");
  if (!frame) return;
  check(frame->link_mode() == LinkMode::C1, prefix + " keeps C1 mode");
  check(frame->format() == "A", prefix + " uses Format A fixture");
  check(frame->as_raw() == body, prefix + " round-trips to normalized body");
}

static void check_golden_round_trip_t1(const std::vector<uint8_t> &body, const char *name) {
  const std::string prefix = std::string(name) + " golden T1 frame";
  auto raw = encode_3of6(format_a_with_crc_from_body(body));
  Packet packet = make_packet(raw, -73);

  auto frame = packet.convert_to_frame();
  check(frame.has_value(), prefix + " converts through parser");
  if (!frame) return;
  check(frame->link_mode() == LinkMode::T1, prefix + " keeps T1 mode");
  check(frame->format() == "A", prefix + " uses Format A fixture");
  check(frame->as_raw() == body, prefix + " round-trips to normalized body");
}

// Same shape as base_frame_body(), but the A-field carries 0x7F, whose low
// nibble is above 9 and therefore cannot be BCD. This is the Diehl/IZAR case:
// the meter has no decimal ID at all and is only addressable by its raw value.
// The bytes are chosen so the printed form is exactly 417F0666.
static std::vector<uint8_t> non_bcd_frame_body() {
  return {
      0x0B,        // L-field: 11 bytes follow.
      0x44,        // C-field.
      0xA5, 0x11,  // M-field, little endian -> "DME" (Diehl Metering).
      0x66, 0x06, 0x7F, 0x41,  // A-field -> id:417F0666
      0x70,        // version.
      0x07,        // device type: water.
      0x7A,        // CI: short header.
      0x55,        // payload byte, enough to keep the L-field honest.
  };
}

static void test_non_bcd_meter_id() {
  const auto body = non_bcd_frame_body();
  Packet packet = make_packet(body, -70);

  uint32_t bcd_id = 0;
  check(!packet.try_get_meter_id(bcd_id), "non-BCD A-field yields no decimal meter id");
  check(bcd_id == 0, "failed BCD extraction leaves the id at zero");

  uint32_t raw_id = 0;
  check(packet.try_get_meter_id_raw(raw_id), "non-BCD A-field still yields a raw meter id");
  check(raw_id == 0x417F0666u, "raw meter id matches the printed id:417F0666");

  // The same meter arriving over the air as a T1 frame must survive the parser
  // and still be identifiable, which is the whole point of the raw form.
  auto raw_wire = encode_3of6(format_a_with_crc_from_body(body));
  Packet t1 = make_packet(raw_wire, -70);
  auto frame = t1.convert_to_frame();
  check(frame.has_value(), "non-BCD T1 frame converts through the parser");
  if (!frame) return;
  check(frame->as_raw() == body, "non-BCD T1 frame round-trips to the same body");

  // Read from the frame, not the packet: Frame's constructor moves the decoded
  // bytes out of the Packet, leaving it empty.
  uint32_t decoded_raw = 0;
  check(frame->try_get_meter_id_raw(decoded_raw), "raw id is extractable after T1 decoding");
  check(decoded_raw == 0x417F0666u, "raw id survives T1 decoding unchanged");
  uint32_t decoded_bcd = 0;
  check(!frame->try_get_meter_id(decoded_bcd), "decoded frame still has no decimal id");

  // And it is matchable by a whitelist entry written as 0x417F0666.
  const std::vector<uint32_t> none;
  const std::vector<uint32_t> raw_list = {0x417F0666};
  check(meter_id_allowed(none, raw_list, 0, decoded_raw), "non-BCD meter passes a raw whitelist");
  check(!meter_id_allowed(raw_list, none, 0, decoded_raw), "non-BCD meter is not matched by a decimal list");
}

static void test_forward_meter_whitelist() {
  const std::vector<uint32_t> none;

  // No forward_meters configured: nothing is filtered, including frames whose
  // meter ID could not be decoded. This is the pre-existing behaviour.
  check(meter_id_allowed(none, none, 41551279, 0x41551279), "empty whitelist forwards a decoded meter");
  check(meter_id_allowed(none, none, 0, 0), "empty whitelist forwards a frame with no usable id");

  // Sorted + deduplicated, as parse_meter_id_csv_ produces.
  const std::vector<uint32_t> bcd = {41551279, 90830781};
  check(meter_id_allowed(bcd, none, 41551279, 0x41551279), "listed BCD meter is forwarded");
  check(meter_id_allowed(bcd, none, 90830781, 0x90830781), "second listed BCD meter is forwarded");
  check(!meter_id_allowed(bcd, none, 89907, 0x00089907), "unlisted meter is dropped");
  check(!meter_id_allowed(bcd, none, 0, 0), "frame with no usable id is dropped while filtering");

  // Non-BCD meters (Diehl/IZAR and friends) have no decimal ID at all: id is 0
  // and only the raw A-field value identifies them.
  const std::vector<uint32_t> raw = {0x417F0666};
  check(meter_id_allowed(raw, raw, 0, 0x417F0666), "non-BCD meter matches on the raw id");
  check(!meter_id_allowed(raw, raw, 0, 0x417F0667), "a different raw id is dropped");
  check(!meter_id_allowed(none, raw, 41551279, 0x41551279), "BCD meter not on the raw list is dropped");

  // A raw entry may also address a BCD meter, since the raw form exists for
  // every meter - writing 0x00089907 must match meter 89907.
  const std::vector<uint32_t> raw_of_bcd = {0x00089907};
  check(meter_id_allowed(none, raw_of_bcd, 89907, 0x00089907), "BCD meter matches via its raw form");

  // Either list may carry the match.
  check(meter_id_in_lists(bcd, raw, 41551279, 0x41551279), "bcd list hit");
  check(meter_id_in_lists(bcd, raw, 0, 0x417F0666), "raw list hit");
  check(!meter_id_in_lists(bcd, raw, 89907, 0x00089907), "no list hit");

  // highlight_meters semantics: an empty configuration highlights nothing,
  // unlike the whitelist where empty means "allow everything".
  check(!meter_id_in_lists(none, none, 41551279, 0x41551279), "empty highlight lists match nothing");
}

static void test_real_golden_frames_round_trip() {
  for (const auto &fixture : golden_frame_fixtures()) {
    const auto body = hex_to_bytes(fixture.hex);
    check_golden_body_shape(body, fixture);
    check_golden_round_trip_c1(body, fixture.name);
    check_golden_round_trip_t1(body, fixture.name);
  }
}

int main() {
  test_rx_metadata_primitives();
  test_decode3of6_round_trip();
  test_decode3of6_invalid_symbol();
  test_dll_crc_format_a();
  test_dll_crc_format_b();
  test_packet_t1_frame_conversion();
  test_packet_c1_frame_conversion();
  test_packet_s1_frame_conversion();
  test_packet_t1_bad_crc_is_rejected();
  test_packet_t1_truncated_is_rejected();
  test_packet_c1_unknown_preamble_is_rejected();
  test_packet_s1_invalid_manchester_is_rejected();
  test_packet_s1_erasures_are_recovered_per_crc_block();
  test_packet_s1_too_many_erasures_are_rejected();
  test_packet_s1_field_erasures_from_2026_08_14();
  test_non_bcd_meter_id();
  test_forward_meter_whitelist();
  test_real_golden_frames_round_trip();

  if (failures == 0) {
    std::cout << "All host parser tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
