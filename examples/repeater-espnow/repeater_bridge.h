// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESP-NOW repeater - BRIDGE side: any ESP32 on the home network (no wM-Bus radio).
// Receives frames from the node (repeater_node.h) over ESP-NOW and publishes them
// to MQTT on the node's behalf, in the same format as wmbus_radio:
//   wmbus/<node name>/telegram  - frame hex
//   wmbus/<node name>/rx        - metadata (schema 1, same frame_crc32, seq >= 1)
// The bridge is connected to the router, so it sits on the router's channel; the
// node finds that channel by itself.
//
// Accepting nodes (so a neighbour's node is not forwarded):
//   - no node accepted yet -> the first one that sends HELLO is accepted;
//   - "Forget node" button -> the next one to say HELLO is accepted;
//   - "Accept strongest node" button -> the candidate with the best link.
// Only HELLO accepts a node; DATA from a node that is not accepted is counted as
// "foreign" and ignored. The accepted address is kept in flash (globals).
// Candidates not heard for 5 min drop off the list.
//
// Delivery: a frame is confirmed to the node (0x12) only after BOTH MQTT
// publishes succeeded. Without MQTT nothing is confirmed and the node keeps the
// frames in its buffer. A frame retried because a confirmation was lost is
// recognised by seq and confirmed again without a second publish.
//
// seq in /rx follows the wmbus_radio contract the add-on expects: >= 1 and
// rising within one boot_id. The node counts 16-bit (1..65535), so a wrap
// 65535 -> 1 continues as 65536+ (epoch), and a node restart gets a NEW boot_id
// (a new series, not "out of order"). Committed only after a successful publish.
//
// Add-on view of the node: the node's health pulse (HELLO) is published as
// wmbus/<node>/health, its configuration report and settings (0x13, in parts) as
// retained wmbus/<node>/diag/config and .../diag/settings, and the add-on's
// wmbus/<node>/set/<key> commands are passed to the node (0x14). So the add-on
// shows and switches the node like a board with its own MQTT.
//
// Log language: Polish when built with -DWMBUS_LOG_LANG_PL (see the bridge YAML),
// English otherwise.

#pragma once

#include <esp_wifi.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "esphome/components/espnow/espnow_component.h"
#include "esphome/components/mqtt/mqtt_client.h"

namespace wmbus_repeater_bridge {

static const char *const TAG = "repeater_bridge";

#ifdef WMBUS_LOG_LANG_PL
#define RPT_TR(en, pl) pl
#else
#define RPT_TR(en, pl) en
#endif

// Meter-ID masking in the LOG (screenshots for forum posts). Same algorithm as
// wmbus_radio/log_anon.h: ID digit + digit of the UTC date (YYYYMMDD) mod 10, letters
// A-F within A-F. MQTT gets the real IDs. No clock: ????????.
static bool anonymize = false;
inline void set_anonymize(bool on) { anonymize = on; }
static const char *anon_id(const char *real, char *buf, size_t n) {
  if (!anonymize) return real;
  const time_t now = ::time(nullptr);
  struct tm t;
  if (gmtime_r(&now, &t) == nullptr || t.tm_year + 1900 < 2024) {
    snprintf(buf, n, "????????");
    return buf;
  }
  char d[40];  // 3x %d = up to 33 B in theory; a real date fits 16, gcc cannot prove it (-Wformat-truncation)
  snprintf(d, sizeof(d), "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  size_t i = 0;
  for (; real[i] != '\0' && i + 1 < n; i++) {
    const char c = real[i];
    const int k = d[i % 8] - '0';
    if (c >= '0' && c <= '9') buf[i] = (char) ('0' + (c - '0' + k) % 10);
    else if (c >= 'A' && c <= 'F') buf[i] = (char) ('A' + (c - 'A' + k) % 6);
    else buf[i] = c;
  }
  buf[i] = '\0';
  return buf;
}

struct Board {
  uint8_t mac[6];
  std::string name;
  int rssi;
  uint32_t last_ms;
  bool has_stats{false};   // node stats from HELLO
  uint32_t a_fail{0};      // failures on the node (retries, probes)
  uint32_t a_dropped{0};   // frames lost on the node (buffer full / too long)
  uint16_t a_queue{0};     // frames waiting in the node buffer
  // node health pulse (newer node): published by the bridge as <name>/health
  bool has_health{false};
  uint32_t hello_ms{0};      // when the last HELLO with a pulse arrived
  uint32_t a_uptime_s{0};
  uint32_t a_rx_total{0};
  int32_t a_rx_age_s{-1};    // -1 = the node has not received a frame yet
  int8_t a_rssi{1};          // 1 = none
  std::string a_chip;
  std::string a_mode;
};

// One JSON document from the node (config or settings), joined from 0x13 parts.
struct Blob {
  uint8_t version{0};
  uint8_t parts{0};
  uint32_t have{0};          // bit per part received (up to 32 parts)
  std::string part[32];
  bool pending{false};       // complete, waiting for MQTT to publish it
  std::string json;
};

// A set/<key> command waiting to be passed to the node.
struct PendingSet {
  std::string key;
  std::string value;
  uint8_t tries{0};
};

struct Ctx {
  esphome::espnow::ESPNowComponent *now{nullptr};
  Blob blob[2];                 // [0] config, [1] settings - of the accepted node
  std::string subscribed;       // node name whose set/+ topic is subscribed
  PendingSet pending_set;
  uint64_t *accepted{nullptr};  // 0 = none
  uint32_t boot_id{0};
  std::vector<Board> boards;    // every node heard (candidates + accepted)
  int32_t last_seq{-1};
  uint32_t seq_epoch{0};  // wraps of the node 16-bit seq (65535 -> 1) within one boot_id
  uint32_t n_rx{0};
  uint32_t n_lost{0};
  uint32_t n_bad{0};
  uint32_t n_foreign{0};
  uint32_t n_dup{0};
  uint32_t n_unready{0};  // frames refused for lack of MQTT (the node retries them)
  int32_t rssi_sum{0};
  uint32_t rssi_cnt{0};
  int link_rssi_last{0};
};
static Ctx g;

static uint64_t mac64(const uint8_t *m) {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v = (v << 8) | m[i];
  return v;
}

static std::string mac_str(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

static Board *find(const uint8_t *mac) {
  for (auto &b : g.boards)
    if (memcmp(b.mac, mac, 6) == 0) return &b;
  return nullptr;
}

static Board &touch(const uint8_t *mac, int rssi) {
  Board *b = find(mac);
  if (b == nullptr) {
    Board n{};
    memcpy(n.mac, mac, 6);
    n.name = "repeater";
    g.boards.push_back(n);
    b = &g.boards.back();
    ESP_LOGI(TAG, RPT_TR("new node %s", "nowa plytka %s"), mac_str(mac).c_str());
  }
  b->rssi = rssi;
  b->last_ms = esphome::millis();
  return *b;
}

static bool is_accepted(const uint8_t *mac) { return g.accepted && *g.accepted == mac64(mac); }

static void accept(const uint8_t *mac) {
  *g.accepted = mac64(mac);
  g.last_seq = -1;
  g.blob[0] = Blob();  // documents of a previous node do not belong to this one
  g.blob[1] = Blob();
  g.pending_set = PendingSet();
  ESP_LOGI(TAG, RPT_TR("node ACCEPTED %s", "PRZYJETA plytka %s"), mac_str(mac).c_str());
}

static uint8_t own_channel() {
  uint8_t ch = 0;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&ch, &sec);
  return ch;
}

static bool mqtt_ready() {
  auto *m = esphome::mqtt::global_mqtt_client;
  return m != nullptr && m->is_connected();
}

// Publish confirmation: only this makes the node drop the frame from its buffer.
static void send_data_ack(const uint8_t *to, uint16_t seq) {
  if (g.now == nullptr) return;
  std::vector<uint8_t> p{0x12, (uint8_t) (seq >> 8), (uint8_t) (seq & 0xFF)};
  g.now->send(to, p);
}

static void send_ack(const uint8_t *to) {
  if (g.now == nullptr) return;
  std::vector<uint8_t> p{0x11, own_channel(), (uint8_t) (is_accepted(to) ? 1 : 0)};
  // UTC time for the node (no SNTP there); 0 = own clock not set yet.
  const time_t now = ::time(nullptr);
  const uint32_t u = now > 1700000000 ? (uint32_t) now : 0;
  p.push_back(u & 0xFF);
  p.push_back((u >> 8) & 0xFF);
  p.push_back((u >> 16) & 0xFF);
  p.push_back((u >> 24) & 0xFF);
  // "Ready": the bridge has MQTT and can publish. 0 -> the node keeps frames buffered.
  p.push_back(mqtt_ready() ? 1 : 0);
  // Largest packet this bridge takes (espnow max_payload_size): the node does not send
  // a frame that would not fit, so a long frame cannot block its buffer.
  const uint16_t mx = (uint16_t) esphome::espnow::ESPNOW_MAX_DATA_LEN;
  p.push_back(mx & 0xFF);
  p.push_back(mx >> 8);
  g.now->send(to, p);  // auto_add_peer adds the node as a peer
}

inline void setup(esphome::espnow::ESPNowComponent *now, uint64_t *accepted_global) {
  g.now = now;
  g.accepted = accepted_global;
  g.boot_id = esp_random();
}

// Defined further down (add-on settings through the bridge).
static void ensure_set_subscription(const Board &b);
static void send_set(const uint8_t *to);

static void handle_hello(const uint8_t *src, int rssi, const uint8_t *data, uint16_t size) {
  Board &b = touch(src, rssi);
  if (size >= 3) {
    const uint8_t n = data[2];
    if (n > 0 && 3 + n <= size) b.name.assign((const char *) data + 3, n);
    const uint8_t *st = data + 3 + n;  // node stats (optional): fail u32, dropped u32, queue u16
    if (3 + n + 10 <= size) {
      b.a_fail = st[0] | (st[1] << 8) | (st[2] << 16) | ((uint32_t) st[3] << 24);
      b.a_dropped = st[4] | (st[5] << 8) | (st[6] << 16) | ((uint32_t) st[7] << 24);
      b.a_queue = st[8] | (st[9] << 8);
      b.has_stats = true;
      // pulse (optional): uptime u32, rx u32, age of the last frame u32 (0xFFFFFFFF = none),
      // RSSI i8, [len][chip], [len][listen mode]
      size_t o = 3 + n + 10;
      if (o + 13 + 2 <= size) {
        auto rd32 = [&](size_t i) {
          return (uint32_t) data[i] | ((uint32_t) data[i + 1] << 8) | ((uint32_t) data[i + 2] << 16) |
                 ((uint32_t) data[i + 3] << 24);
        };
        b.a_uptime_s = rd32(o);
        b.a_rx_total = rd32(o + 4);
        const uint32_t age = rd32(o + 8);
        b.a_rx_age_s = age == 0xFFFFFFFFu ? -1 : (int32_t) age;
        b.a_rssi = (int8_t) data[o + 12];
        o += 13;
        const uint8_t cl = data[o];
        if (o + 1 + cl + 1 <= size) {
          b.a_chip.assign((const char *) data + o + 1, cl);
          o += 1 + cl;
          const uint8_t ml = data[o];
          if (o + 1 + ml <= size) b.a_mode.assign((const char *) data + o + 1, ml);
        }
        b.has_health = true;
        b.hello_ms = esphome::millis();
      }
    }
  }
  if (*g.accepted == 0) accept(src);
  send_ack(src);
  if (is_accepted(src)) {
    ensure_set_subscription(b);
    if (!g.pending_set.key.empty()) send_set(src);
  }
}

static void handle_data(const uint8_t *src, int rssi, const uint8_t *data, uint16_t size) {
  // 0x01: [type][seq2][rssi][mode][frame]                  (older, no time)
  // 0x02: [type][seq2][rssi][mode][UTC s u32][ms u16][frame]  (reception time on the node)
  const bool timed = data[0] == 0x02;
  const size_t hdr = timed ? 11 : 5;
  if (size < hdr + 1) {
    g.n_bad++;
    return;
  }
  Board &b = touch(src, rssi);
  if (!is_accepted(src)) {  // accepted only by HELLO or a button
    g.n_foreign++;
    return;
  }

  const uint16_t seq = ((uint16_t) data[1] << 8) | data[2];
  const int8_t frssi = (int8_t) data[3];
  const uint8_t mode = data[4];
  const uint8_t *f = data + hdr;
  const size_t flen = size - hdr;
  uint32_t rx_s = 0;
  uint16_t rx_ms = 0;
  if (timed) {
    rx_s = data[5] | (data[6] << 8) | (data[7] << 16) | ((uint32_t) data[8] << 24);
    rx_ms = data[9] | (data[10] << 8);
  }

  g.link_rssi_last = rssi;
  g.rssi_sum += rssi;
  g.rssi_cnt++;
  if (g.last_seq >= 0 && seq == (uint16_t) g.last_seq) {
    // The node retried because a confirmation was lost - already in MQTT: confirm only.
    g.n_dup++;
    send_data_ack(src, seq);
    return;
  }
  if (!mqtt_ready()) {
    // No MQTT, no confirmation - the frame stays in the node buffer and comes again.
    if (g.n_unready++ % 20 == 0)
      ESP_LOGW(TAG, RPT_TR("no MQTT - frame seq=%u stays in the node buffer", "brak MQTT - ramka seq=%u zostaje w buforze plytki"), (unsigned) seq);
    return;
  }

  uint32_t crc = 0xFFFFFFFFu;  // identical to wmbus_radio frame_crc32()
  for (size_t i = 0; i < flen; i++) {
    crc ^= f[i];
    for (uint8_t k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  crc ^= 0xFFFFFFFFu;

  std::string hex;
  hex.reserve(flen * 2);
  static const char *H = "0123456789ABCDEF";
  for (size_t i = 0; i < flen; i++) {
    hex += H[f[i] >> 4];
    hex += H[f[i] & 15];
  }
  char meter[9] = "????????";
  if (flen >= 8) snprintf(meter, sizeof(meter), "%02X%02X%02X%02X", f[7], f[6], f[5], f[4]);
  const char *mname = mode == 1 ? "T1" : mode == 2 ? "C1" : mode == 3 ? "S1" : "unknown";
  char rssi_json[8];
  if (frssi >= -125 && frssi <= -1) snprintf(rssi_json, sizeof(rssi_json), "%d", (int) frssi);
  else snprintf(rssi_json, sizeof(rssi_json), "null");

  // received_at = when the node received it (a buffered frame may be minutes old),
  // like wmbus_radio: ISO UTC with ms; left out when the node had no clock yet.
  char received_at[48] = "";
  if (rx_s > 1600000000u) {
    const time_t at = (time_t) rx_s;
    struct tm t;
    if (gmtime_r(&at, &t) != nullptr) {
      char base[24];
      strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &t);
      snprintf(received_at, sizeof(received_at), ",\"received_at\":\"%s.%03uZ\"", base, (unsigned) (rx_ms % 1000));
    }
  }
  // /rx numbering as in wmbus_radio (what the add-on expects): seq >= 1, rising
  // within one boot_id. The node counts 16-bit (1..65535, no 0), so:
  //   - wrap 65535 -> 1: seq_epoch++ (the published seq keeps rising, 32-bit);
  //   - node restart (seq drops elsewhere): NEW boot_id - a new series for the add-on,
  //     not frames "out of order".
  // Committed only after a successful publish (the frame may come back from the node buffer).
  uint32_t boot_id = g.boot_id;
  uint32_t epoch = g.seq_epoch;
  uint16_t gap = 0;
  bool a_restart = false;
  if (g.last_seq >= 0) {
    const uint16_t last = (uint16_t) g.last_seq;
    uint16_t expected = (uint16_t) (last + 1);
    if (expected == 0) expected = 1;
    if (seq != expected) {
      const uint16_t fwd = (uint16_t) (seq - expected);
      if (fwd < 1000) gap = fwd;  // frames lost on the node (buffer overflow)
      else a_restart = true;
    }
    if (a_restart) {
      boot_id = esp_random();
      epoch = 0;
    } else if (seq < last) {
      epoch++;  // counter wrap
    }
  }
  const uint32_t pub_seq = (epoch << 16) | seq;

  char rx[360];
  snprintf(rx, sizeof(rx),
           "{\"schema\":1,\"boot_id\":\"%08lX\",\"seq\":%lu,\"rx_task_wakeup_us\":%llu,"
           "\"meter_id\":\"%s\",\"mode\":\"%s\",\"rssi_dbm\":%s,\"frame_crc32\":\"%08lX\",\"frame_length\":%u%s}",
           (unsigned long) boot_id, (unsigned long) pub_seq, (unsigned long long) esphome::micros(), meter, mname, rssi_json,
           (unsigned long) crc, (unsigned) flen, received_at);

  auto *m = esphome::mqtt::global_mqtt_client;
  const bool published = m != nullptr && m->is_connected() &&
                         m->publish("wmbus/" + b.name + "/telegram", hex, 0, false) &&
                         m->publish("wmbus/" + b.name + "/rx", std::string(rx), 0, false);
  if (!published) {
    ESP_LOGW(TAG, RPT_TR("publish of seq=%u failed - frame stays in the node buffer", "publikacja seq=%u nieudana - ramka zostaje w buforze plytki"), (unsigned) seq);
    return;
  }
  g.n_lost += gap;
  if (a_restart)
    ESP_LOGI(TAG, RPT_TR("seq %u -> %u: node restart - new boot_id %08lX", "seq %u -> %u: restart plytki - nowy boot_id %08lX"), (unsigned) g.last_seq, (unsigned) seq,
             (unsigned long) boot_id);
  g.boot_id = boot_id;
  g.seq_epoch = epoch;
  g.last_seq = seq;
  g.n_rx++;
  send_data_ack(src, seq);
  char log_id[12];
  ESP_LOGI(TAG, RPT_TR("%s seq=%u %s %s frame RSSI %d, link %d dBm, %u B", "%s seq=%u %s %s RSSI ramki %d, lacze %d dBm, %u B"), b.name.c_str(), (unsigned) seq,
           anon_id(meter, log_id, sizeof(log_id)), mname,
           (int) frssi, rssi, (unsigned) flen);
}

static void publish_blob(const Board &b, int kind) {
  Blob &x = g.blob[kind];
  if (!x.pending) return;
  auto *m = esphome::mqtt::global_mqtt_client;
  if (m == nullptr || !m->is_connected() || b.name.empty()) return;
  const std::string topic = "wmbus/" + b.name + (kind == 0 ? "/diag/config" : "/diag/settings");
  if (m->publish(topic, x.json, 0, true)) {
    x.pending = false;
    ESP_LOGI(TAG, RPT_TR("%s: %s published for the node (%u B)", "%s: %s opublikowane za plytke (%u B)"), b.name.c_str(),
             kind == 0 ? "diag/config" : "diag/settings", (unsigned) x.json.size());
  }
}

static void handle_blob(const uint8_t *src, int rssi, const uint8_t *data, uint16_t size) {
  // [0x13][kind 1/2][version][part][parts][bytes]
  if (size < 6) {
    g.n_bad++;
    return;
  }
  Board &b = touch(src, rssi);
  if (!is_accepted(src)) return;
  const uint8_t kind = data[1], version = data[2], part = data[3], parts = data[4];
  if (kind < 1 || kind > 2 || parts == 0 || parts > 32 || part >= parts) {
    g.n_bad++;
    return;
  }
  Blob &x = g.blob[kind - 1];
  if (x.version != version || x.parts != parts) {  // a new document: start over
    x.version = version;
    x.parts = parts;
    x.have = 0;
  }
  x.part[part].assign((const char *) data + 5, size - 5);
  x.have |= 1u << part;
  const uint32_t all = parts == 32 ? 0xFFFFFFFFu : ((1u << parts) - 1);
  if (x.have != all) return;
  x.json.clear();
  for (uint8_t i = 0; i < parts; i++) x.json += x.part[i];
  x.have = 0;
  x.pending = true;
  if (kind == 2) g.pending_set.tries = 0;  // new settings from the node: the last command landed
  if (kind == 2) g.pending_set.key.clear();
  publish_blob(b, kind - 1);
}

static void send_set(const uint8_t *to) {
  if (g.now == nullptr || g.pending_set.key.empty()) return;
  if (g.pending_set.tries >= 3) {
    g.pending_set.key.clear();
    return;
  }
  const std::string &k = g.pending_set.key, &v = g.pending_set.value;
  std::vector<uint8_t> p;
  p.push_back(0x14);
  p.push_back((uint8_t) k.size());
  p.insert(p.end(), k.begin(), k.end());
  p.push_back((uint8_t) v.size());
  p.insert(p.end(), v.begin(), v.end());
  g.now->send(to, p);
  g.pending_set.tries++;
}

// Subscribe wmbus/<node>/set/+ for the accepted node (once per name; the MQTT
// client renews it after a reconnect). A command goes to the node at once and
// again with its next HELLOs (up to 3 sends) until the node reports settings again.
static void ensure_set_subscription(const Board &b) {
  auto *m = esphome::mqtt::global_mqtt_client;
  if (m == nullptr || !m->is_connected() || b.name.empty() || g.subscribed == b.name) return;
  if (!g.subscribed.empty()) m->unsubscribe("wmbus/" + g.subscribed + "/set/+");
  g.subscribed = b.name;
  const std::string prefix = "wmbus/" + b.name + "/set/";
  m->subscribe(
      prefix + "+",
      [prefix](const std::string &topic, const std::string &payload) {
        if (topic.size() <= prefix.size() || topic.compare(0, prefix.size(), prefix) != 0) return;
        const std::string key = topic.substr(prefix.size());
        if (key.size() > 32 || payload.size() > 200) return;
        g.pending_set.key = key;
        g.pending_set.value = payload;
        g.pending_set.tries = 0;
        ESP_LOGI(TAG, RPT_TR("set/%s = '%s' from the add-on -> node", "set/%s = '%s' z dodatku -> plytka"), key.c_str(),
                 payload.c_str());
        for (auto &n : g.boards)
          if (is_accepted(n.mac)) send_set(n.mac);
      },
      1);
  ESP_LOGI(TAG, RPT_TR("listening for add-on settings on %s+", "nasluch ustawien z dodatku na %s+"), prefix.c_str());
}

// Called from on_receive and on_broadcast of the espnow component.
// size is uint16_t: with espnow max_payload_size > 250 (ESP-NOW v2) a packet is longer than 255 B.
inline void on_packet(const esphome::espnow::ESPNowRecvInfo &info, const uint8_t *data, uint16_t size) {
  if (size < 1) return;
  const int rssi = info.rx_ctrl != nullptr ? info.rx_ctrl->rssi : 0;
  switch (data[0]) {
    case 0x10:
      handle_hello(info.src_addr, rssi, data, size);
      break;
    case 0x01:
    case 0x02:
      handle_data(info.src_addr, rssi, data, size);
      break;
    case 0x13:
      handle_blob(info.src_addr, rssi, data, size);
      break;
    default:
      g.n_bad++;
      ESP_LOGW(TAG, RPT_TR("unknown packet type 0x%02X (%u B) from %s", "nieznany pakiet typu 0x%02X (%u B) od %s"), data[0], (unsigned) size,
               mac_str(info.src_addr).c_str());
  }
}

inline void forget() {
  *g.accepted = 0;
  g.last_seq = -1;
  ESP_LOGI(TAG, RPT_TR("accepted node forgotten - the next one to say HELLO will be accepted", "zapomniana przyjeta plytka - nastepna zglaszajaca sie zostanie przyjeta"));
}

inline void accept_strongest() {
  const uint32_t now = esphome::millis();
  Board *best = nullptr;
  for (auto &b : g.boards)
    if (now - b.last_ms < 60000 && (best == nullptr || b.rssi > best->rssi)) best = &b;
  if (best != nullptr) accept(best->mac);
  else ESP_LOGW(TAG, RPT_TR("no node heard in the last minute", "brak plytek slyszanych w ostatniej minucie"));
}

// --- for the bridge page / HA ---
inline std::string accepted_text() {
  if (*g.accepted == 0) return RPT_TR("none - waiting for the first node", "brak - czekam na pierwsza plytke");
  const uint32_t now = esphome::millis();
  for (auto &b : g.boards)
    if (is_accepted(b.mac)) {
      char s[200];
      int n = snprintf(s, sizeof(s), RPT_TR("%s (%s), link %d dBm, last heard %u s ago", "%s (%s), lacze %d dBm, ostatnio %u s temu"), b.name.c_str(),
                       mac_str(b.mac).c_str(), b.rssi, (unsigned) ((now - b.last_ms) / 1000));
      if (b.has_stats && n > 0 && (size_t) n < sizeof(s))
        snprintf(s + n, sizeof(s) - n, RPT_TR(" | on node: buffered %u, dropped %lu, send failures %lu", " | w plytce: bufor %u, utracone %lu, nieudane wysylki %lu"), (unsigned) b.a_queue,
                 (unsigned long) b.a_dropped, (unsigned long) b.a_fail);
      return s;
    }
  uint8_t m[6];
  for (int i = 0; i < 6; i++) m[i] = (uint8_t) (*g.accepted >> (8 * (5 - i)));
  return mac_str(m) + RPT_TR(" - not heard yet", " - jeszcze sie nie odezwala");
}

inline std::string candidates_text() {
  const uint32_t now = esphome::millis();
  // Expiry: a candidate silent for 5 min drops off (the accepted node always stays).
  for (size_t i = 0; i < g.boards.size();) {
    if (!is_accepted(g.boards[i].mac) && now - g.boards[i].last_ms > 300000) {
      ESP_LOGI(TAG, RPT_TR("candidate %s dropped from the list (silent > 5 min)", "kandydat %s zniknal z listy (cisza > 5 min)"), mac_str(g.boards[i].mac).c_str());
      g.boards.erase(g.boards.begin() + i);
    } else {
      i++;
    }
  }
  std::string s;
  for (auto &b : g.boards) {
    if (is_accepted(b.mac)) continue;
    char t[96];
    snprintf(t, sizeof(t), RPT_TR("%s%s (%s) %d dBm, %us ago", "%s%s (%s) %d dBm, %us temu"), s.empty() ? "" : " | ", b.name.c_str(),
             mac_str(b.mac).c_str(), b.rssi, (unsigned) ((now - b.last_ms) / 1000));
    s += t;
  }
  return s.empty() ? RPT_TR("none", "brak") : s;
}

// The accepted node's pulse as wmbus/<name>/health - same fields as wmbus_radio
// (uptime_s, rx_total, sec_since_last_rx, rssi, chip, listen_mode) plus
// "via":"espnow", the bridge name and the link RSSI, so the add-on knows the
// board is behind a bridge (and has no settings it could change). Call every
// 60 s; nothing is published when the last pulse is older than 60 s (the node is
// silent - the add-on then sees the pulse stop).
inline void publish_health() {
  auto *m = esphome::mqtt::global_mqtt_client;
  if (m == nullptr || !m->is_connected()) return;
  for (auto &b : g.boards) {
    if (is_accepted(b.mac)) {
      publish_blob(b, 0);  // a document that arrived while MQTT was down
      publish_blob(b, 1);
    }
    if (!is_accepted(b.mac) || !b.has_health || b.name.empty()) continue;
    const uint32_t age_ms = esphome::millis() - b.hello_ms;
    if (age_ms > 60000) continue;
    const uint32_t age_s = age_ms / 1000U;
    const long since = b.a_rx_age_s < 0 ? -1L : (long) (b.a_rx_age_s + age_s);
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"uptime_s\":%lu,\"rx_total\":%lu,\"sec_since_last_rx\":%ld,\"rssi\":%d,"
             "\"chip\":\"%s\",\"listen_mode\":\"%s\",\"via\":\"espnow\",\"bridge\":\"%s\",\"link_rssi\":%d}",
             (unsigned long) (b.a_uptime_s + age_s), (unsigned long) b.a_rx_total, since, (int) b.a_rssi,
             b.a_chip.c_str(), b.a_mode.c_str(), esphome::App.get_name().c_str(), b.rssi);
    m->publish("wmbus/" + b.name + "/health", std::string(payload), 0, false);
  }
}

inline std::string link_json_and_reset() {
  const int avg = g.rssi_cnt ? (int) (g.rssi_sum / (int32_t) g.rssi_cnt) : 0;
  // a_*: counters from the node (its HELLO) - they survive a bridge restart, unlike "lost".
  const Board *a = nullptr;
  for (auto &b : g.boards)
    if (is_accepted(b.mac) && b.has_stats) a = &b;
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"rx\":%lu,\"lost\":%lu,\"dup\":%lu,\"bad\":%lu,\"foreign\":%lu,\"link_rssi_avg_60s\":%d,"
           "\"link_rssi_last\":%d,\"a_dropped\":%ld,\"a_send_fail\":%ld,\"a_queue\":%d}",
           (unsigned long) g.n_rx, (unsigned long) g.n_lost, (unsigned long) g.n_dup, (unsigned long) g.n_bad,
           (unsigned long) g.n_foreign, avg, g.link_rssi_last, a ? (long) a->a_dropped : -1L,
           a ? (long) a->a_fail : -1L, a ? (int) a->a_queue : -1);
  g.rssi_sum = 0;
  g.rssi_cnt = 0;
  return buf;
}

inline uint32_t rx() { return g.n_rx; }
inline uint32_t lost() { return g.n_lost; }
// Dropped as counted by the node (buffer full) - a bridge restart does not reset it.
inline int a_dropped() {
  for (auto &b : g.boards)
    if (is_accepted(b.mac) && b.has_stats) return (int) b.a_dropped;
  return -1;
}
inline int link_rssi() { return g.link_rssi_last; }

}  // namespace wmbus_repeater_bridge
