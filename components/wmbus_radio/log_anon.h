// SPDX-License-Identifier: GPL-3.0-or-later
//
// Meter-ID masking for screenshots (forum posts, issue reports).
//
// When switched on (set_log_anonymize(true), usually from a template switch on
// the board's web page), every meter ID this component writes to the LOG is
// replaced by a stand-in. Nothing else changes: MQTT topics and payloads, the
// add-on, on_frame and anything forwarded keep the real ID. Only text meant for
// human eyes is masked.
//
// The stand-in: each digit of the ID plus the matching digit of today's date
// (UTC, YYYYMMDD), modulo 10. Hex letters in a non-BCD A-field shift within
// A..F the same way. So on one day the same meter gets the same stand-in on
// every board, and two screenshots can still be lined up.
//
// This is a cover, not encryption: the method is public, so anyone who knows
// the day can undo it. It keeps real IDs off a forum page, nothing more.
//
// No valid clock yet (a board without SNTP that has not been told the time):
// the ID is shown as ???????? - never the real one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace esphome {
namespace wmbus_radio {

extern bool g_log_anonymize;

inline void set_log_anonymize(bool on) { g_log_anonymize = on; }
inline bool log_anonymize() { return g_log_anonymize; }

// Today's key as 8 digits, or false if the clock is not set.
inline bool anon_date_key(uint8_t key[8]) {
  const time_t now = ::time(nullptr);  // :: - esphome::time is a namespace
  struct tm t;
  if (gmtime_r(&now, &t) == nullptr || t.tm_year + 1900 < 2024) return false;
  char d[40];  // 3x %d = do 33 B w teorii; 16 wystarcza dla prawdziwej daty, ale gcc o tym nie wie (-Wformat-truncation)
  snprintf(d, sizeof(d), "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  for (int i = 0; i < 8; i++) key[i] = (uint8_t) (d[i] - '0');
  return true;
}

// Returns `real` unchanged when masking is off, otherwise writes the stand-in
// into buf (at least 9 bytes) and returns buf.
inline const char *anon_meter_id(const char *real, char *buf, size_t n) {
  if (!g_log_anonymize || real == nullptr) return real;
  if (n < 9) return "????????";
  uint8_t key[8];
  if (!anon_date_key(key)) {
    snprintf(buf, n, "????????");
    return buf;
  }
  size_t i = 0;
  for (; real[i] != '\0' && i + 1 < n; i++) {
    const char c = real[i];
    const uint8_t k = key[i % 8];
    if (c >= '0' && c <= '9') buf[i] = (char) ('0' + (c - '0' + k) % 10);
    else if (c >= 'A' && c <= 'F') buf[i] = (char) ('A' + (c - 'A' + k) % 6);
    else if (c >= 'a' && c <= 'f') buf[i] = (char) ('a' + (c - 'a' + k) % 6);
    else buf[i] = c;
  }
  buf[i] = '\0';
  return buf;
}

// Same for an ID held as a number and printed %08X (outbox lines).
inline const char *anon_meter_id_hex(uint32_t id, char *buf, size_t n) {
  char real[9];
  snprintf(real, sizeof(real), "%08X", (unsigned) id);
  if (!g_log_anonymize) {
    snprintf(buf, n, "%s", real);
    return buf;
  }
  return anon_meter_id(real, buf, n);
}

}  // namespace wmbus_radio
}  // namespace esphome
