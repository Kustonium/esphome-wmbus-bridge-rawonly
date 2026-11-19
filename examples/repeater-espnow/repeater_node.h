// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESP-NOW repeater - NODE side: the board with the wM-Bus radio, at the meter,
// with NO connection to the home network. Pairs with repeater_bridge.h.
//
// The node's Wi-Fi runs only as its own hotspot (ESPHome `wifi: ap:`) for the
// status page / OTA page. ESP-NOW goes out through the AP interface, so
// hotspot channel = ESP-NOW channel = the channel of the router the bridge is on.
//
// Pairing, nothing to type in:
//   1. the node hops channels 1..13 and broadcasts HELLO on each;
//   2. the bridge (sits on the router's channel) answers ACK: channel, accepted, time, ready;
//   3. the node keeps the channel and the bridge address (channel also in flash);
//   4. every 15 s the node sends HELLO straight to the bridge (keepalive + its stats).
// While a phone is connected to the hotspot the node does NOT change channel.
//
// Delivery (end-to-end, measured on a bench with ~70 meters, 2026-11-17/18):
//   - every frame goes into a BUFFER first: a ring of fixed slots in PSRAM
//     (BUFFER_FRAMES_PSRAM, ~30-60 min of traffic), plain RAM without PSRAM;
//   - one packet in the air at a time; a frame leaves the buffer ONLY after the
//     bridge confirms "published seq N to MQTT" (0x12). The radio ACK alone is not
//     enough: a bridge that has just rebooted takes frames over the air before it
//     has MQTT, and they would be lost while both sides count zero losses;
//   - FAIL_LIMIT failures in a row = link LOST at once; frames wait, a HELLO probe
//     every PROBE_MS; the bridge answering "ready" brings the link back;
//   - a full buffer drops its oldest frame (the "dropped" counter);
//   - 120 s without a link and no phone on the hotspot -> channel search again.
// seq is assigned on entry to the buffer (1..65535, never 0 - same contract as
// wmbus_radio): a gap at the bridge is a frame really lost, not a retried one.
//
// Packets (first byte = type):
//   0x10 HELLO  node->bridge  [0x10][ver=1][name len][name][send fails u32][dropped u32][queued u16]
//               then (optional, the health pulse the bridge publishes as <name>/health):
//               [uptime s u32][radio frames u32][s since last frame u32, 0xFFFFFFFF = none]
//               [average RSSI i8, 1 = none][len][radio chip][len][listen mode]
//   0x11 ACK    bridge->node  [0x11][channel][accepted 0/1][UTC s u32 LE, 0 = unknown][ready 0/1]
//                             [max packet u16 LE] (missing = 250, an older bridge)
//               The node has no SNTP; the time sets its clock (received_at, log anonymizing).
//   0x12 D-ACK  bridge->node  [0x12][seq hi][seq lo] - frame seq published to MQTT
//   0x02 DATA   node->bridge  [0x02][seq hi][seq lo][RSSI][mode][rx UTC s u32 LE][ms u16 LE][frame]
//               Up to 11 + 256 B: a wM-Bus frame is at most 256 B. Above 250 B this needs
//               ESP-NOW v2 (ESP-IDF 5.4+ on both sides) and a bridge built with
//               espnow max_payload_size >= 267; the bridge says its limit in the ACK and a
//               frame that would not fit is counted as lost instead of blocking the buffer.
//   0x01 DATA   older format without the time - the bridge still accepts it.
//   0x13 BLOB   node->bridge  [0x13][kind 1=config 2=settings][version][part][parts][bytes]
//               The JSON the component would publish on <diag>/config and <diag>/settings,
//               in parts of up to BLOB_PART bytes; the bridge joins them and publishes them
//               retained as wmbus/<name>/diag/config and .../diag/settings.
//   0x14 SET    bridge->node  [0x14][key len][key][value len][value]
//               A wmbus/<name>/set/<key> message from the add-on; the node hands it to the
//               component exactly as a board with MQTT would (stored, then a restart).
//
// Log/status language: Polish with `log_language: pl` in wmbus_radio, English otherwise.

#pragma once

#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <string>
#include <vector>

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/wmbus_radio/log_anon.h"  // same ID masking as the component log
#include "esphome/components/wmbus_radio/component.h"  // global_radio: config, settings, set/<key>

namespace wmbus_repeater_node {

static const char *const TAG = "repeater_node";

#ifdef WMBUS_LOG_LANG_PL
#define RPT_TR(en, pl) pl
#else
#define RPT_TR(en, pl) en
#endif
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const uint32_t SCAN_DWELL_MS = 180;      // wait for an ACK on one channel
static const uint8_t SCAN_CYCLES = 3;           // full 1..13 passes per attempt
static const uint32_t SCAN_PAUSE_MS = 60000;    // pause between attempts (time for a phone)
static const uint32_t KEEPALIVE_MS = 15000;
static const uint32_t RESCAN_AFTER_MS = 120000; // search again after this long without a link
static const uint8_t FAIL_LIMIT = 3;            // failures in a row = link lost
static const uint32_t RETRY_MS = 50;            // retry gap while the link is still up
static const uint32_t PROBE_MS = 2000;          // HELLO probe while the link is lost
static const size_t BLOB_PART = 200;            // config/settings bytes per packet
static const uint32_t BLOB_REFRESH_MS = 1800000;  // resend config/settings every 30 min
static const uint32_t BLOB_WAIT_MS = 5000;      // configuration report not ready yet: retry
static const size_t MAX_FRAME = 256;            // longest wM-Bus frame (L-field 255 + 1)
static const size_t DATA_HDR = 11;              // 0x02 DATA header
static const uint32_t INFLIGHT_TIMEOUT_MS = 500;
static const uint32_t DATA_ACK_TIMEOUT_MS = 400; // time for the bridge to confirm publishing
static const size_t BUFFER_FRAMES_PSRAM = 1500; // 1500 x 252 B = ~370 kB PSRAM, 30-60 min of traffic
static const size_t BUFFER_FRAMES_RAM = 100;    // board without PSRAM: ~27 kB of plain RAM

// A frame in the buffer, without copying.
struct FrameView {
  const uint8_t *d;
  uint16_t n;
  size_t size() const { return n; }
  uint8_t operator[](size_t i) const { return d[i]; }
};

// Ring of fixed-size slots: one allocation at start, no
// fragmentation, all of it in PSRAM (std::deque<std::vector> would use plain RAM).
class FrameRing {
 public:
  static const size_t SLOT = 2 + 11 + 256;  // [length u16 LE][DATA header + longest wM-Bus frame]
  bool init(size_t want_psram, size_t fallback) {
    this->slots_ = (uint8_t *) heap_caps_malloc(want_psram * SLOT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (this->slots_ != nullptr) {
      this->cap_ = want_psram;
      this->psram_ = true;
      return true;
    }
    this->slots_ = (uint8_t *) heap_caps_malloc(fallback * SLOT, MALLOC_CAP_8BIT);
    this->cap_ = this->slots_ != nullptr ? fallback : 0;
    return this->slots_ != nullptr;
  }
  size_t size() const { return this->n_; }
  bool empty() const { return this->n_ == 0; }
  size_t capacity() const { return this->cap_; }
  bool in_psram() const { return this->psram_; }
  FrameView front() const {
    const uint8_t *s = this->slot_(this->head_);
    return {s + 2, (uint16_t) (s[0] | (s[1] << 8))};
  }
  void pop_front() {
    if (this->n_ == 0) return;
    this->head_ = (this->head_ + 1) % this->cap_;
    this->n_--;
  }
  bool push_back(const std::vector<uint8_t> &p) {
    if (this->cap_ == 0 || this->n_ >= this->cap_ || p.size() > SLOT - 2) return false;
    uint8_t *s = this->slot_((this->head_ + this->n_) % this->cap_);
    s[0] = (uint8_t) (p.size() & 0xFF);
    s[1] = (uint8_t) (p.size() >> 8);
    memcpy(s + 2, p.data(), p.size());
    this->n_++;
    return true;
  }
  // Drop the SECOND oldest frame and keep the first (it is in the air right now):
  // the first moves into the second slot, then the head is popped. One slot copy.
  void erase_second() {
    if (this->n_ < 2) {
      this->pop_front();
      return;
    }
    memcpy(this->slot_((this->head_ + 1) % this->cap_), this->slot_(this->head_), SLOT);
    this->pop_front();
  }

 private:
  uint8_t *slot_(size_t i) const { return this->slots_ + i * SLOT; }
  uint8_t *slots_{nullptr};
  size_t cap_{0};
  size_t head_{0};
  size_t n_{0};
  bool psram_{false};
};

enum class State : uint8_t { BOOT, SCAN, PAUSE, LINKED, LOST };
enum class Flight : uint8_t { NONE, DATA, HELLO, BLOB };

struct LastMeter {
  char id[9];
  char mode[3];
  int8_t rssi;
  uint32_t ms;
};

struct Ctx {
  State state{State::BOOT};
  std::string name{"repeater"};
  uint8_t channel{1};
  uint8_t scan_ch{1};
  uint8_t scan_cycle{0};
  uint32_t step_ms{0};
  uint32_t pause_until{0};
  uint32_t last_ack_ms{0};      // last bridge answer (HELLO -> ACK)
  uint32_t link_ok_ms{0};       // last sign of life: radio ACK or bridge answer
  uint32_t lost_since_ms{0};
  uint32_t last_keepalive_ms{0};
  uint32_t last_probe_ms{0};
  uint32_t retry_at_ms{0};
  uint8_t fail_streak{0};
  uint8_t c3_mac[6]{};
  bool c3_known{false};
  bool accepted{false};
  bool c3_no_mqtt{false};
  uint16_t c3_max{250};         // largest packet the bridge takes (from its ACK)
  int link_rssi{0};
  uint16_t seq{1};  // 1..65535, never 0 - like wmbus_radio (the add-on rejects 0)
  uint32_t sent{0};             // frames delivered (published by the bridge)
  uint32_t dropped{0};          // frames lost: buffer overflow or too long
  uint32_t max_queue{0};
  FrameRing queue;
  Flight flight{Flight::NONE};
  uint32_t flight_ms{0};
  LastMeter last[5]{};
  uint8_t last_n{0};
  bool started{false};
  // health pulse for the add-on (HELLO -> bridge -> <name>/health)
  std::string chip{"unknown"};
  std::string listen_mode{"unknown"};
  uint32_t rx_total{0};     // frames from the radio (also those that did not fit the buffer)
  uint32_t last_rx_ms{0};
  int32_t rssi_avg_x16{0};  // moving average of RSSI x16
  bool rssi_valid{false};
  // config / settings for the add-on (0x13), sent part by part when the link is idle
  std::string blob[2];      // [0] config JSON, [1] settings JSON
  uint8_t blob_kind{0};     // 0 or 1 while sending
  uint8_t blob_part{0};
  uint8_t blob_version{0};
  bool blob_sending{false};
  uint32_t blob_due_ms{0};  // next (re)send; 0 = as soon as linked
};
static Ctx g;

// --- data from the callbacks (Wi-Fi task) -> loop ---
static std::atomic<bool> ack_flag{false};
static uint8_t ack_mac[6];
static uint8_t ack_channel;
static uint8_t ack_accepted;
static int8_t ack_rssi;
static uint32_t ack_time;  // 0 = bridge sent no time
static uint8_t ack_ready;  // 1 = bridge has MQTT
static uint16_t ack_max;   // largest packet the bridge takes
static std::atomic<bool> dack_flag{false};
static std::atomic<uint16_t> dack_seq{0};
static std::atomic<int> sent_result{-1};  // -1 none, 0 failure, 1 success (packet in the air)
static std::atomic<uint32_t> tx_fail{0};  // all failures (frames + HELLO)
// set/<key> from the bridge (0x14), applied in loop()
static std::atomic<bool> set_flag{false};
static char set_key[33];
static char set_value[201];

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len >= 3 && data[0] == 0x14) {  // set/<key> from the add-on, via the bridge
    if (set_flag.load() || !g.c3_known || memcmp(info->src_addr, g.c3_mac, 6) != 0) return;
    const int kl = data[1];
    if (kl < 1 || kl > 32 || 2 + kl + 1 > len) return;
    const int vl = data[2 + kl];
    if (vl > 200 || 3 + kl + vl > len) return;
    memcpy(set_key, data + 2, kl);
    set_key[kl] = '\0';
    memcpy(set_value, data + 3 + kl, vl);
    set_value[vl] = '\0';
    set_flag.store(true);
    return;
  }
  if (len >= 3 && data[0] == 0x12) {  // frame published confirmation
    dack_seq.store(((uint16_t) data[1] << 8) | data[2]);
    dack_flag.store(true);
    return;
  }
  if (len < 3 || data[0] != 0x11 || ack_flag.load()) return;
  memcpy(ack_mac, info->src_addr, 6);
  ack_channel = data[1];
  ack_accepted = data[2];
  ack_rssi = info->rx_ctrl != nullptr ? info->rx_ctrl->rssi : 0;
  ack_time = len >= 7 ? ((uint32_t) data[3] | ((uint32_t) data[4] << 8) | ((uint32_t) data[5] << 16) |
                         ((uint32_t) data[6] << 24))
                      : 0;
  ack_ready = len >= 8 ? data[7] : 1;
  ack_max = len >= 10 ? (uint16_t) (data[8] | (data[9] << 8)) : 250;
  ack_flag.store(true);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
static void on_sent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  const bool ok = status == ESP_NOW_SEND_SUCCESS;
  if (!ok) tx_fail++;
  sent_result.store(ok ? 1 : 0);
}

static int station_count() {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
  return list.num;
}

static void add_peer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, mac, 6);
  p.ifidx = WIFI_IF_AP;  // no STA on the node - everything via the hotspot interface
  p.channel = 0;         // 0 = current channel
  p.encrypt = false;
  esp_err_t err = esp_now_add_peer(&p);
  if (err != ESP_OK) ESP_LOGW(TAG, "add_peer: %s", esp_err_to_name(err));
}

static bool set_channel(uint8_t ch) {
  wifi_config_t cfg;
  if (esp_wifi_get_config(WIFI_IF_AP, &cfg) != ESP_OK) return false;
  if (cfg.ap.channel == ch) return true;
  cfg.ap.channel = ch;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "channel %u: %s", (unsigned) ch, esp_err_to_name(err));
    return false;
  }
  return true;
}

static void put32(std::vector<uint8_t> &p, uint32_t v) {
  for (int i = 0; i < 4; i++) p.push_back((uint8_t) (v >> (8 * i)));
}

// One packet in the air at a time. false when it could not be queued.
static bool transmit(const uint8_t *to, const uint8_t *data, size_t len, Flight kind) {
  if (g.flight != Flight::NONE) return false;
  sent_result.store(-1);
  esp_err_t err = esp_now_send(to, data, len);  // ESP-NOW copies the data - the slot may change afterwards
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
    tx_fail++;
    return false;
  }
  g.flight = kind;
  g.flight_ms = esphome::millis();
  if (kind == Flight::DATA) dack_flag.store(false);
  return true;
}

static bool send_hello(const uint8_t *to) {
  std::vector<uint8_t> p;
  const uint8_t n = (uint8_t) std::min<size_t>(g.name.size(), 32);
  p.push_back(0x10);
  p.push_back(1);
  p.push_back(n);
  p.insert(p.end(), g.name.begin(), g.name.begin() + n);
  put32(p, tx_fail.load());
  put32(p, g.dropped);
  const uint16_t q = (uint16_t) std::min<size_t>(g.queue.size(), 0xFFFF);
  p.push_back(q & 0xFF);
  p.push_back(q >> 8);
  // health pulse
  const uint32_t now = esphome::millis();
  put32(p, now / 1000U);
  put32(p, g.rx_total);
  put32(p, g.rx_total > 0 ? (uint32_t) ((now - g.last_rx_ms) / 1000U) : 0xFFFFFFFFu);
  p.push_back((uint8_t) (int8_t) (g.rssi_valid ? g.rssi_avg_x16 / 16 : 1));
  const uint8_t cl = (uint8_t) std::min<size_t>(g.chip.size(), 16);
  p.push_back(cl);
  p.insert(p.end(), g.chip.begin(), g.chip.begin() + cl);
  const uint8_t ml = (uint8_t) std::min<size_t>(g.listen_mode.size(), 16);
  p.push_back(ml);
  p.insert(p.end(), g.listen_mode.begin(), g.listen_mode.begin() + ml);
  return transmit(to, p.data(), p.size(), Flight::HELLO);
}

static void set_lost(uint32_t now, const char *why) {
  if (g.state == State::LOST) return;
  g.state = State::LOST;
  g.lost_since_ms = now;
  g.last_probe_ms = 0;
  ESP_LOGW(TAG, RPT_TR("link to bridge LOST (%s) - frames wait in the buffer (%u)", "lacze z mostem UTRACONE (%s) - ramki czekaja w buforze (%u)"), why, (unsigned) g.queue.size());
}

static void set_linked(uint32_t now) {
  if (g.state == State::LOST) {
    // The bridge may have restarted meanwhile: send config/settings again, whole.
    g.blob_sending = false;
    g.blob_due_ms = now;
  }
  if (g.state == State::LOST)
    ESP_LOGI(TAG, RPT_TR("link to bridge back after %u s - %u frame(s) in the buffer", "lacze z mostem wrocilo po %u s - w buforze %u ramek"), (unsigned) ((now - g.lost_since_ms) / 1000),
             (unsigned) g.queue.size());
  g.state = State::LINKED;
  g.fail_streak = 0;
  g.link_ok_ms = now;
}

static void start_scan() {
  g.state = State::SCAN;
  g.scan_ch = g.channel;
  g.scan_cycle = 0;
  g.step_ms = 0;
}

// Call once, after Wi-Fi is up (on_boot with a low priority).
// chip / listen_mode: only for the health pulse in the add-on (e.g. "SX1262", "t1").
inline void start(const std::string &topic_name, uint8_t saved_channel, const char *chip = nullptr,
                  const char *listen_mode = nullptr) {
  if (g.started) return;
  g.name = topic_name;
  if (chip != nullptr) g.chip = chip;
  if (listen_mode != nullptr) g.listen_mode = listen_mode;
  g.channel = (saved_channel >= 1 && saved_channel <= 13) ? saved_channel : 1;
  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
    return;
  }
  if (!g.queue.init(BUFFER_FRAMES_PSRAM, BUFFER_FRAMES_RAM))
    ESP_LOGE(TAG, RPT_TR("no memory for the frame buffer - frames are passed on live only", "brak pamieci na bufor ramek - ramki beda tylko przekazywane na biezaco"));
  esp_now_register_recv_cb(on_recv);
  esp_now_register_send_cb(on_sent);
  add_peer(BCAST);
  g.started = true;
  start_scan();  // start from the last known channel
  ESP_LOGI(TAG, RPT_TR("start: name '%s', looking for the bridge from channel %u, buffer %u frames (%s)", "start: nazwa '%s', szukam mostu od kanalu %u, bufor %u ramek (%s)"), g.name.c_str(),
           (unsigned) g.channel, (unsigned) g.queue.capacity(), g.queue.in_psram() ? "PSRAM" : "RAM");
}

// Config / settings for the add-on: taken from the component when a send starts,
// so a refresh always carries what is in effect now.
static bool blob_prepare() {
  auto *radio = esphome::wmbus_radio::global_radio;
  if (radio == nullptr) return false;
  g.blob[0] = radio->remote_config_json();
  g.blob[1] = radio->remote_settings_json();
  if (g.blob[0].empty() || g.blob[1].empty()) return false;
  g.blob_kind = 0;
  g.blob_part = 0;
  g.blob_version++;
  g.blob_sending = true;
  return true;
}

static uint8_t blob_parts(const std::string &b) { return (uint8_t) ((b.size() + BLOB_PART - 1) / BLOB_PART); }

static void blob_send_part() {
  const std::string &b = g.blob[g.blob_kind];
  const uint8_t parts = blob_parts(b);
  const size_t off = (size_t) g.blob_part * BLOB_PART;
  const size_t n = std::min(BLOB_PART, b.size() - off);
  std::vector<uint8_t> p;
  p.reserve(n + 5);
  p.push_back(0x13);
  p.push_back(g.blob_kind + 1);
  p.push_back(g.blob_version);
  p.push_back(g.blob_part);
  p.push_back(parts);
  p.insert(p.end(), b.begin() + off, b.begin() + off + n);
  transmit(g.c3_mac, p.data(), p.size(), Flight::BLOB);
}

static void blob_advance(uint32_t now) {
  if (++g.blob_part < blob_parts(g.blob[g.blob_kind])) return;
  g.blob_part = 0;
  if (++g.blob_kind < 2) return;
  g.blob_sending = false;
  g.blob[0].clear();
  g.blob[1].clear();
  g.blob_due_ms = now + BLOB_REFRESH_MS;
  ESP_LOGD(TAG, RPT_TR("configuration and settings sent to the bridge", "konfiguracja i ustawienia wyslane do mostu"));
}

// Outcome of the packet in the air.
//   HELLO: radio ACK (success/failure) or timeout.
//   DATA:  success = publish confirmation (0x12) carrying the seq of the buffer head;
//          failure = no radio ACK, or no confirmation within DATA_ACK_TIMEOUT_MS.
static uint16_t head_seq() {
  if (g.queue.empty() || g.queue.front().size() < 3) return 0;
  const FrameView f = g.queue.front();
  return ((uint16_t) f[1] << 8) | f[2];
}

static void settle_flight(uint32_t now) {
  if (g.flight == Flight::NONE) return;
  const Flight kind = g.flight;
  const int r = sent_result.load();
  bool ok;
  if (kind == Flight::DATA) {
    if (dack_flag.load() && dack_seq.load() == head_seq()) {
      ok = true;
    } else if (r == 0) {
      ok = false;  // radio did not deliver
    } else if (now - g.flight_ms >= DATA_ACK_TIMEOUT_MS) {
      ok = false;  // arrived over the air (or unknown), but the bridge did not confirm publishing
      if (r != 0) tx_fail++;
    } else {
      return;  // keep waiting
    }
  } else {
    if (r < 0) {
      if (now - g.flight_ms < INFLIGHT_TIMEOUT_MS) return;
      tx_fail++;  // no callback - treat as failure
      ok = false;
    } else {
      ok = r == 1;
    }
  }
  g.flight = Flight::NONE;
  sent_result.store(-1);
  dack_flag.store(false);
  // A broadcast while scanning has no radio ACK - its outcome says nothing about the bridge.
  if (g.state == State::SCAN || g.state == State::PAUSE) return;

  if (ok && kind == Flight::BLOB) blob_advance(now);
  if (ok) {
    if (kind == Flight::DATA && !g.queue.empty()) {
      g.queue.pop_front();
      g.sent++;
    }
    // A HELLO probe that got a radio ACK does NOT restore the link - only a bridge answer
    // with "ready" does (a bridge without MQTT still ACKs over the air). A confirmed frame does.
    if (kind == Flight::DATA && g.state == State::LOST) set_linked(now);
    g.fail_streak = 0;
    g.link_ok_ms = now;
  } else {
    if (g.fail_streak < 255) g.fail_streak++;
    g.retry_at_ms = now + RETRY_MS;
    if (g.state == State::LINKED && g.fail_streak >= FAIL_LIMIT)
      set_lost(now, kind == Flight::DATA ? RPT_TR("no confirmations from the bridge", "brak potwierdzen od mostu") : RPT_TR("no ESP-NOW confirmations", "brak potwierdzen ESP-NOW"));
  }
}

// Returns a channel to store in flash (0 = unchanged).
inline uint8_t loop() {
  if (!g.started) return 0;
  const uint32_t now = esphome::millis();
  uint8_t to_save = 0;

  settle_flight(now);

  if (set_flag.load()) {
    auto *radio = esphome::wmbus_radio::global_radio;
    ESP_LOGI(TAG, RPT_TR("set/%s = '%s' from the add-on (via the bridge)", "set/%s = '%s' z dodatku (przez most)"), set_key, set_value);
    if (radio != nullptr) radio->apply_remote_setting(set_key, set_value);
    set_flag.store(false);
  }

  if (ack_flag.load()) {
    memcpy(g.c3_mac, ack_mac, 6);
    g.c3_known = true;
    add_peer(g.c3_mac);
    g.accepted = ack_accepted != 0;
    g.link_rssi = ack_rssi;
    g.last_ack_ms = now;
    if (ack_max != g.c3_max)
      ESP_LOGI(TAG, RPT_TR("bridge takes packets up to %u B (frames up to %u B)", "most przyjmuje pakiety do %u B (ramki do %u B)"),
               (unsigned) ack_max, (unsigned) (ack_max > DATA_HDR ? ack_max - DATA_HDR : 0));
    g.c3_max = ack_max;
    // Clock from the bridge: set it when unset or more than 60 s off.
    if (ack_time > 1700000000u) {
      const time_t mine = ::time(nullptr);
      const long diff = (long) mine - (long) ack_time;
      if (mine < 1700000000 || diff > 60 || diff < -60) {
        struct timeval tv{(time_t) ack_time, 0};
        settimeofday(&tv, nullptr);
        ESP_LOGI(TAG, RPT_TR("clock set from the bridge (UTC %lu)", "zegar ustawiony z mostu (UTC %lu)"), (unsigned long) ack_time);
      }
    }
    if ((g.state != State::LINKED && g.state != State::LOST) || g.channel != ack_channel) {
      ESP_LOGI(TAG, RPT_TR("bridge %02X:%02X:%02X:%02X:%02X:%02X on channel %u, link %d dBm, %s", "most %02X:%02X:%02X:%02X:%02X:%02X na kanale %u, lacze %d dBm, %s"),
               g.c3_mac[0], g.c3_mac[1], g.c3_mac[2], g.c3_mac[3], g.c3_mac[4], g.c3_mac[5],
               (unsigned) ack_channel, (int) ack_rssi, g.accepted ? RPT_TR("accepted", "przyjeta") : RPT_TR("NOT accepted", "NIE przyjeta"));
      if (ack_channel >= 1 && ack_channel <= 13) {
        set_channel(ack_channel);
        if (g.channel != ack_channel) to_save = ack_channel;
        g.channel = ack_channel;
      }
    }
    if (ack_ready) {
      set_linked(now);
    } else {
      // Bridge is alive but has no MQTT - frames cannot go further, they stay buffered.
      if (g.state != State::LOST) set_lost(now, RPT_TR("bridge has no MQTT", "most bez polaczenia z MQTT"));
      g.c3_no_mqtt = true;
    }
    if (ack_ready) g.c3_no_mqtt = false;
    ack_flag.store(false);
  }

  switch (g.state) {
    case State::SCAN:
      if (station_count() > 0) {  // phone on the hotspot - do not hop channels
        set_channel(g.channel);
        g.state = State::PAUSE;
        g.pause_until = now + SCAN_PAUSE_MS;
        break;
      }
      if (g.flight != Flight::NONE) break;
      if (g.step_ms == 0 || now - g.step_ms >= SCAN_DWELL_MS) {
        if (g.step_ms != 0) {
          g.scan_ch = g.scan_ch % 13 + 1;
          if (g.scan_ch == g.channel && ++g.scan_cycle >= SCAN_CYCLES) {
            ESP_LOGI(TAG, RPT_TR("bridge not found after %u passes - pause %u s", "most nie znaleziony po %u przejsciach - przerwa %u s"), (unsigned) SCAN_CYCLES,
                     (unsigned) (SCAN_PAUSE_MS / 1000));
            set_channel(g.channel);
            g.state = State::PAUSE;
            g.pause_until = now + SCAN_PAUSE_MS;
            break;
          }
        }
        set_channel(g.scan_ch);
        send_hello(BCAST);
        g.step_ms = now;
      }
      break;

    case State::PAUSE:
      if ((int32_t) (now - g.pause_until) >= 0 && station_count() == 0) start_scan();
      break;

    case State::LINKED:
      if (g.flight != Flight::NONE) break;
      if (now - g.last_keepalive_ms >= KEEPALIVE_MS) {
        if (send_hello(g.c3_mac)) g.last_keepalive_ms = now;
        break;
      }
      if (g.accepted && !g.queue.empty() && (int32_t) (now - g.retry_at_ms) >= 0)
      {
        const FrameView f = g.queue.front();
        if (f.n > g.c3_max) {
          // An older bridge (or a smaller max_payload_size) cannot take it: retrying would
          // block every frame behind it. Count it as lost, like a full buffer.
          g.queue.pop_front();
          g.dropped++;
          ESP_LOGW(TAG, RPT_TR("frame of %u B is longer than the bridge takes (%u B) - lost; set espnow max_payload_size: 300 on the bridge",
                               "ramka %u B dluzsza niz przyjmuje most (%u B) - utracona; ustaw na moscie espnow max_payload_size: 300"),
                   (unsigned) (f.n - DATA_HDR), (unsigned) g.c3_max);
          break;
        }
        transmit(g.c3_mac, f.d, f.n, Flight::DATA);
        break;
      }
      // Frames first; config/settings only when the buffer is empty.
      if (g.accepted && g.queue.empty() && (int32_t) (now - g.retry_at_ms) >= 0) {
        if (!g.blob_sending && (int32_t) (now - g.blob_due_ms) >= 0 && !blob_prepare())
          g.blob_due_ms = now + BLOB_WAIT_MS;
        if (g.blob_sending) blob_send_part();
      }
      break;

    case State::LOST:
      if (g.flight == Flight::NONE && now - g.last_probe_ms >= PROBE_MS) {
        send_hello(g.c3_mac);  // probe: the bridge answering "ready" restores the link
        g.last_probe_ms = now;
      }
      if (now - g.lost_since_ms >= RESCAN_AFTER_MS && station_count() == 0) {
        ESP_LOGW(TAG, RPT_TR("no link to the bridge for %u s - searching channels again (buffer %u)", "brak lacza z mostem od %u s - szukam kanalu od nowa (bufor %u)"),
                 (unsigned) ((now - g.lost_since_ms) / 1000), (unsigned) g.queue.size());
        start_scan();
      }
      break;

    default:
      break;
  }
  return to_save;
}

// From on_frame: the frame goes into the buffer; loop() sends it when the link allows.
inline void send_frame(const std::vector<uint8_t> &raw, int8_t rssi, uint8_t mode, const char *meter_id) {
  g.rx_total++;
  g.last_rx_ms = esphome::millis();
  if (!g.rssi_valid) {
    g.rssi_avg_x16 = rssi * 16;
    g.rssi_valid = true;
  } else {
    g.rssi_avg_x16 += (rssi * 16 - g.rssi_avg_x16) / 8;
  }
  if (g.last_n < 5) g.last_n++;
  for (int i = 4; i > 0; i--) g.last[i] = g.last[i - 1];
  snprintf(g.last[0].id, sizeof(g.last[0].id), "%s", meter_id != nullptr ? meter_id : "????????");
  snprintf(g.last[0].mode, sizeof(g.last[0].mode), "%s", mode == 1 ? "T1" : mode == 2 ? "C1" : mode == 3 ? "S1" : "?");
  g.last[0].rssi = rssi;
  g.last[0].ms = esphome::millis();

  if (!g.started || raw.size() > MAX_FRAME) {  // cannot happen with a valid wM-Bus frame
    g.dropped++;
    return;
  }
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const bool clock_ok = tv.tv_sec > 1700000000;
  std::vector<uint8_t> p;
  p.reserve(raw.size() + DATA_HDR);
  p.push_back(0x02);
  p.push_back(g.seq >> 8);
  p.push_back(g.seq & 0xFF);
  p.push_back((uint8_t) rssi);
  p.push_back(mode);
  put32(p, clock_ok ? (uint32_t) tv.tv_sec : 0);
  const uint16_t ms = clock_ok ? (uint16_t) (tv.tv_usec / 1000) : 0;
  p.push_back(ms & 0xFF);
  p.push_back(ms >> 8);
  p.insert(p.end(), raw.begin(), raw.end());
  if (++g.seq == 0) g.seq = 1;

  if (g.queue.capacity() == 0) {
    g.dropped++;
    return;
  }
  if (g.queue.size() >= g.queue.capacity()) {
    // Do not drop the packet in the air - drop the next oldest.
    if (g.flight == Flight::DATA) g.queue.erase_second();
    else g.queue.pop_front();
    g.dropped++;
  }
  g.queue.push_back(p);
  g.max_queue = std::max<uint32_t>(g.max_queue, g.queue.size());
}

// --- for the hotspot status page ---
inline std::string status_text() {
  const uint32_t now = esphome::millis();
  char b[160];
  if (!g.started) return RPT_TR("ESP-NOW did not start", "radio ESP-NOW nie wystartowalo");
  if (g.state == State::LINKED && g.accepted)
    snprintf(b, sizeof(b), RPT_TR("CONNECTED - channel %u, link %d dBm, frames up to %u B%s", "POLACZONA - kanal %u, lacze %d dBm, ramki do %u B%s"),
             (unsigned) g.channel, g.link_rssi, (unsigned) (g.c3_max > DATA_HDR ? g.c3_max - DATA_HDR : 0),
             g.queue.size() > 1 ? RPT_TR(", draining buffer", ", oproznia bufor") : "");
  else if (g.state == State::LINKED)
    snprintf(b, sizeof(b), RPT_TR("bridge hears this board but has NOT accepted it - accept it on the bridge page", "most slyszy, ale NIE PRZYJAL tej plytki - przyjmij ja na stronie mostu"));
  else if (g.state == State::LOST && g.c3_no_mqtt)
    snprintf(b, sizeof(b), RPT_TR("bridge WITHOUT MQTT for %u s - %u frame(s) waiting (channel %u)", "most BEZ MQTT od %u s - %u ramek czeka w buforze (kanal %u)"),
             (unsigned) ((now - g.lost_since_ms) / 1000), (unsigned) g.queue.size(), (unsigned) g.channel);
  else if (g.state == State::LOST)
    snprintf(b, sizeof(b), RPT_TR("LOST for %u s - %u frame(s) waiting (channel %u)", "UTRACONA od %u s - %u ramek czeka w buforze (kanal %u)"),
             (unsigned) ((now - g.lost_since_ms) / 1000), (unsigned) g.queue.size(), (unsigned) g.channel);
  else if (g.state == State::SCAN)
    snprintf(b, sizeof(b), RPT_TR("looking for the bridge (channel %u)...%s", "szukam mostu (kanal %u)...%s"), (unsigned) g.scan_ch, g.queue.empty() ? "" : RPT_TR(" frames buffered", " ramki w buforze"));
  else if (station_count() > 0)
    snprintf(b, sizeof(b), RPT_TR("NOT FOUND - search paused while a phone is connected", "NIE ZNALEZIONY - szukanie wstrzymane, dopoki telefon jest podlaczony"));
  else
    snprintf(b, sizeof(b), RPT_TR("NOT FOUND - next try in %u s", "NIE ZNALEZIONY - nastepna proba za %u s"),
             (unsigned) (((int32_t) (g.pause_until - now) > 0 ? g.pause_until - now : 0) / 1000));
  return b;
}

inline std::string last_meters_text() {
  if (g.last_n == 0) return RPT_TR("no frames yet", "brak ramek");
  const uint32_t now = esphome::millis();
  std::string s;
  char b[48];
  for (uint8_t i = 0; i < g.last_n; i++) {
    char idb[12];
    const char *id = esphome::wmbus_radio::anon_meter_id(g.last[i].id, idb, sizeof(idb));
    snprintf(b, sizeof(b), "%s%s %s %d dBm (%us)", i ? " | " : "", id, g.last[i].mode, (int) g.last[i].rssi,
             (unsigned) ((now - g.last[i].ms) / 1000));
    s += b;
  }
  return s;
}

inline int link_rssi() { return g.link_rssi; }
inline uint32_t failed() { return tx_fail.load(); }   // failures (retries, probes) - NOT lost frames
inline uint32_t sent() { return g.sent; }             // frames delivered (published by the bridge)
inline uint32_t dropped() { return g.dropped; }       // frames lost (buffer full / too long)
inline uint32_t queued() { return (uint32_t) g.queue.size(); }
inline uint32_t max_queued() { return g.max_queue; }
inline uint32_t capacity() { return (uint32_t) g.queue.capacity(); }

}  // namespace wmbus_repeater_node
