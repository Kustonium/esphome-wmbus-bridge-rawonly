// SPDX-License-Identifier: GPL-3.0-or-later
//
// Diagnostics for the wmbus_radio component: drop/stage bucket classification,
// RX-path and drop events, actionable suggestions, the health publish and the
// periodic diagnostic summaries (summary, summary_15min, summary_60min). Split
// out of component.cpp unchanged (move-only refactor); event names, field names,
// payload structure, topics and thresholds are identical.

#include "component.h"
#include "wmbus_radio_internal.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cinttypes>
#include <string>

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "wmbus";

Radio::DropBucket Radio::bucket_for_reason_(const std::string &reason) {
  // Keep this stable: these strings come from Packet::set_drop_()
  if (reason == "too_short") return DB_TOO_SHORT;
  if (reason == "decode_failed") return DB_DECODE_FAILED;
  // Backwards compatible: older builds used dll_crc_strip_failed
  if (reason == "dll_crc_failed" || reason == "dll_crc_strip_failed") return DB_DLL_CRC_FAILED;
  if (reason == "unknown_preamble") return DB_UNKNOWN_PREAMBLE;
  if (reason == "l_field_invalid") return DB_L_FIELD_INVALID;
  if (reason == "unknown_link_mode") return DB_UNKNOWN_LINK_MODE;
  return DB_OTHER;
}

Radio::StageBucket Radio::bucket_for_stage_(const std::string &stage) {
  if (stage == "precheck") return SB_PRECHECK;
  if (stage == "t1_decode3of6") return SB_T1_DECODE3OF6;
  if (stage == "t1_l_field") return SB_T1_L_FIELD;
  if (stage == "t1_length_check") return SB_T1_LENGTH_CHECK;
  if (stage == "c1_precheck") return SB_C1_PRECHECK;
  if (stage == "c1_preamble") return SB_C1_PREAMBLE;
  if (stage == "c1_suffix") return SB_C1_SUFFIX;
  if (stage == "c1_l_field") return SB_C1_L_FIELD;
  if (stage == "c1_length_check") return SB_C1_LENGTH_CHECK;
  if (stage == "dll_crc_first") return SB_DLL_CRC_FIRST;
  if (stage == "dll_crc_mid") return SB_DLL_CRC_MID;
  if (stage == "dll_crc_final") return SB_DLL_CRC_FINAL;
  if (stage == "dll_crc_b1") return SB_DLL_CRC_B1;
  if (stage == "dll_crc_b2") return SB_DLL_CRC_B2;
  if (stage == "link_mode" || stage == "listen_mode_filter") return SB_LINK_MODE;
  return SB_OTHER;
}

bool Radio::should_publish_packet_event_(const Packet *packet) const {
  if (packet == nullptr || !this->diag_publish_drop_events_) return false;
  if (!this->diag_publish_highlight_only_ || this->highlight_meter_ids_.empty()) return true;

  uint32_t meter_id = 0;
  if (!packet->try_get_meter_id(meter_id)) return false;
  return this->meter_is_highlighted_(meter_id);
}

void Radio::publish_rx_path_event_(const char *event, const char *stage, const char *detail, int rssi) {
  if (!this->diag_publish_rx_path_events_) return;
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected() || this->diag_topic_.empty()) return;

  const uint32_t now_ms = (uint32_t) esphome::millis();
  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";

  char payload[448];
  if (detail != nullptr && detail[0] != '\0') {
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"stage\":\"%s\",\"rssi\":%d,\"detail\":\"%s\"}",
             event, (unsigned long) now_ms, listen_mode, stage, rssi, detail);
  } else {
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"stage\":\"%s\",\"rssi\":%d}",
             event, (unsigned long) now_ms, listen_mode, stage, rssi);
  }
  mqtt->publish(this->diag_topic_, payload);
}

// Publish a suggestion event, throttled to once per hour per code.
// Suggestions are not retained — they are one-time actionable hints.
// yaml_snippet is a ready-to-copy YAML fragment the user can paste directly.
static void publish_suggestion_(esphome::mqtt::MQTTClientComponent *mqtt,
                                const std::string &topic,
                                std::unordered_map<std::string, uint32_t> &last_ms,
                                uint32_t now_ms,
                                uint32_t throttle_ms,
                                const char *chip,
                                const char *code,
                                const char *yaml_key,
                                const char *suggested_value,
                                const char *yaml_snippet,
                                const char *hint_en,
                                const char *hint_pl) {
  if (topic.empty()) return;
  auto it = last_ms.find(code);
  if (it != last_ms.end() && (now_ms - it->second) < throttle_ms) return;
  last_ms[code] = now_ms;

  char payload[640];
  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"suggestion\","
           "\"chip\":\"%s\","
           "\"code\":\"%s\","
           "\"yaml_key\":\"%s\","
           "\"suggested_value\":\"%s\","
           "\"yaml_snippet\":\"%s\","
           "\"hint_en\":\"%s\","
           "\"hint_pl\":\"%s\""
           "}",
           chip, code, yaml_key, suggested_value, yaml_snippet, hint_en, hint_pl);

  mqtt->publish(topic, std::string(payload), static_cast<uint8_t>(0), false);
  ESP_LOGI("wmbus", "SUGGESTION / SUGESTIA [%s]: %s", code, hint_en);
}

void Radio::maybe_publish_suggestion_(uint32_t now_ms) {
  if (!this->diag_publish_suggestion_) return;
  if (this->diag_topic_.empty()) return;
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const std::string topic = this->diag_suggestion_topic_();
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const char *chip = is_sx1276 ? "SX1276" : "SX1262";

  const uint32_t fsl = this->current_false_start_like_();
  const uint32_t total = this->diag_total_;
  const uint32_t drop_pct = (total > 0) ? (((total - this->diag_ok_) * 100U) / total) : 0U;
  const uint32_t t1_sym_inv_pct = (this->diag_t1_symbols_total_ > 0)
      ? ((this->diag_t1_symbols_invalid_ * 100U) / this->diag_t1_symbols_total_) : 0U;

  // ── STAGE 1: orientation ────────────────────────────────────────────────────
  // No packets at all — user may have a wiring/config problem.
  if (total == 0) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "NO_METERS_DETECTED",
        "listen_mode", "t1",
        "listen_mode: t1",
        "No wM-Bus frames received. Check antenna, SPI pins, listen_mode (t1/c1/s1/both) and radio_type.",
        "Brak odebranych ramek wM-Bus. Sprawdź antenę, piny SPI, listen_mode (t1/c1/s1/both) i radio_type.");
    return; // nothing more to suggest until we have data
  }

  // Packets arriving but highlight_meters not configured — user doesn't know
  // which meter IDs they have. They need to check wmbusmeters first.
  // NOTE: check highlight_meter_ids_ (user config), not highlight_meter_stats_ (runtime),
  // because stats may be empty simply because the listed meters haven't been seen yet.
  if (this->highlight_meter_ids_.empty()) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "ADD_HIGHLIGHT_METERS",
        "highlight_meters", "<meter_id>",
        "highlight_meters:\n  - \"<meter_id>\"",
        "Meters are being received. Check which IDs appear in wmbusmeters, then add them to highlight_meters to track per-meter reception quality.",
        "Liczniki są odbierane. Sprawdź w wmbusmeters jakie ID pojawiają się, następnie dodaj je do highlight_meters aby śledzić skuteczność odbioru per licznik.");
    return; // advanced suggestions only make sense once user knows their meters
  }

  // ── STAGE 2: diagnostic suggestions (highlight_meters configured) ───────────

  // SX1276: many false starts causing real losses — suggest rx_path_events.
  // Require drop_pct >= 10 (same threshold as adaptive trigger) to avoid false alarms
  // when high fsl is just RF background noise with no actual losses.
  // Skip if already enabled in YAML.
  if (is_sx1276 && fsl >= 80 && drop_pct >= 10 && !this->diag_publish_rx_path_events_) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "ENABLE_RX_PATH_EVENTS",
        "diagnostic_publish_rx_path_events", "true",
        "diagnostic_publish_rx_path_events: true",
        "Many false starts with real packet losses (SX1276). Enable diagnostic_publish_rx_path_events: true to investigate the RX path.",
        "Dużo fałszywych startów z realnymi stratami (SX1276). Włącz diagnostic_publish_rx_path_events: true aby zbadać ścieżkę RX.");
  }

  // Weak signal — suggest drop_events + raw.
  // Require total >= 20 to avoid firing on a handful of packets right after boot.
  // Skip if already enabled in YAML.
  if (total >= 20 && drop_pct >= 40 && this->diag_rssi_ok_n_ > 0 && (!this->diag_publish_drop_events_ || !this->diag_publish_raw_)) {
    const int32_t avg_ok_rssi = this->diag_rssi_ok_sum_ / (int32_t) this->diag_rssi_ok_n_;
    if (avg_ok_rssi <= -85) {
      publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
          chip, "ENABLE_DROP_EVENTS_RAW",
          "diagnostic_publish_drop_events", "true",
          "diagnostic_publish_drop_events: true\ndiagnostic_publish_raw: true",
          "Many drops at weak RSSI. Enable diagnostic_publish_drop_events: true and diagnostic_publish_raw: true to analyze dropped frames.",
          "Dużo dropów przy słabym sygnale. Włącz diagnostic_publish_drop_events: true i diagnostic_publish_raw: true aby przeanalizować odrzucone ramki.");
    }
  }

  // SX1262: symbol errors — suggest checking cpu_frequency.
  // Requires: >= 10% invalid symbols (not just noise), >= 500 symbols counted (statistical
  // significance), drop_pct >= 5 (symbol errors must cause actual losses to be actionable),
  // total >= 20 (enough packets to distinguish real pattern from boot transient).
  // No YAML state to check — cpu_frequency is an ESPHome board setting, not a wmbus option.
  if (!is_sx1276
      && t1_sym_inv_pct >= 10
      && this->diag_t1_symbols_total_ >= 500
      && drop_pct >= 5
      && total >= 20) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "SX1262_SYMBOL_ERRORS",
        "cpu_frequency", "160MHz",
        "cpu_frequency: 160MHz",
        "SX1262 shows T1 symbol errors (>=10%) with real packet losses. Check cpu_frequency — 240MHz can cause EMI affecting reception. Use 160MHz.",
        "SX1262 pokazuje błędy symboli T1 (>=10%) z realnymi stratami. Sprawdź cpu_frequency — 240MHz może powodować EMI wpływające na odbiór. Użyj 160MHz.");
  }

  // Quiet ether on SX1276 with adaptive — suggest considering normal mode.
  // Require total >= 30 to avoid firing too early after boot on sparse RF environments.
  // Skip if already set to normal in YAML.
  if (is_sx1276 &&
      this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::ADAPTIVE &&
      now_ms > this->busy_ether_active_until_ms_ &&
      fsl < 20 && total >= 30) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "QUIET_ETHER_ADAPTIVE_IDLE",
        "sx1276_busy_ether_mode", "normal",
        "sx1276_busy_ether_mode: normal",
        "RF environment looks quiet and adaptive has not activated. Consider sx1276_busy_ether_mode: normal if this is consistent.",
        "Eter wygląda spokojnie a adaptive nie aktywował się. Rozważ sx1276_busy_ether_mode: normal jeśli jest to konsekwentne.");
  }
}


void Radio::maybe_publish_health_(uint32_t now_ms) {
  // Always-on (independent of diagnostic_mode). Two sibling topics:
  //   {health_topic}  -> liveness + identity pulse (no quality fields)
  //   {meters_topic}  -> meters the ESP explicitly knows about (target + highlight)
  // Both retain=false: a liveness/flag signal must never replay as a stale
  // tombstone to a freshly-connected subscriber (the addon applies its own TTL).
  if (this->health_topic_.empty() && this->meters_topic_.empty()) return;
  // last_health_ms_ == 0 means "publish ASAP" so the addon sees us within one
  // loop of MQTT connect, not after a full interval.
  if (this->last_health_ms_ != 0 && (now_ms - this->last_health_ms_) < HEALTH_INTERVAL_MS_) return;

  auto *mqtt = mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;  // retry next loop; do not advance timer
  this->last_health_ms_ = now_ms;

  if (!this->health_topic_.empty()) {
    const char *chip = (this->radio != nullptr) ? this->radio->get_name() : "unknown";
    const char *listen_mode = (this->radio != nullptr)
                                  ? listen_mode_to_string_(this->radio->get_listen_mode())
                                  : "unknown";
    // -1 until the first frame is received (honest "never heard anything yet").
    const int32_t sec_since_last_rx =
        this->any_rx_ ? (int32_t) ((now_ms - this->last_rx_ms_) / 1000U) : -1;
    // Recent average RSSI of OK frames (EWMA), maintained always — even with
    // diagnostics off — because the adaptive RF logic depends on it. This is the
    // real radio RSSI (negative dBm), unlike the 0 that wmbusmeters reports from
    // the RAW-hex stream downstream. 1 = "no valid sample yet" (RSSI is always
    // negative); the consumer should treat rx_total==0 as "no signal data".
    const int32_t rssi = this->recent_ok_rssi_valid_ ? this->recent_ok_rssi_avg_ : 1;
    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"uptime_s\":%lu,\"rx_total\":%u,\"sec_since_last_rx\":%ld,"
             "\"rssi\":%ld,\"chip\":\"%s\",\"listen_mode\":\"%s\"}",
             (unsigned long) (now_ms / 1000U),
             (unsigned) this->rx_total_lifetime_,
             (long) sec_since_last_rx,
             (long) rssi,
             chip, listen_mode);
    // std::string(...) disambiguates the publish() overload set: a bare char[]
    // is ambiguous between the (const char*, size_t, ...) and (const std::string&,
    // ...) signatures; wrapping forces the string overload (as elsewhere here).
    mqtt->publish(this->health_topic_, std::string(payload), static_cast<uint8_t>(0), false);
  }

  if (!this->meters_topic_.empty()) {
    // highlight_meters_csv_ is a comma-separated list joined in python; emit it
    // as a JSON array of quoted ids. target is a single id ("" when unset).
    std::string arr = "[";
    bool first = true;
    const std::string &csv = this->highlight_meters_csv_;
    size_t start = 0;
    while (start <= csv.size()) {
      size_t comma = csv.find(',', start);
      std::string tok = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      // trim spaces
      while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
      while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
      if (!tok.empty()) {
        if (!first) arr += ',';
        arr += '"';
        arr += tok;
        arr += '"';
        first = false;
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    arr += ']';
    std::string payload = "{\"target\":\"" + this->target_meter_id_str_ + "\",\"highlight\":" + arr + "}";
    mqtt->publish(this->meters_topic_, payload, static_cast<uint8_t>(0), false);
  }
}

void Radio::maybe_publish_diag_summary_(uint32_t now_ms) {
  if (!this->diag_publish_summary_) return;
  if (this->diag_topic_.empty()) return;
  if (this->last_diag_summary_ms_ == 0) {
    this->last_diag_summary_ms_ = now_ms;
    return;
  }
  uint32_t elapsed = now_ms - this->last_diag_summary_ms_;
  if (elapsed < this->diag_summary_interval_ms_) return;
  this->last_diag_summary_ms_ = now_ms;

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";
  const uint32_t interval_s = elapsed / 1000U;

  char payload[2048];
  const uint32_t crc_failed = this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED];
  const uint32_t total = this->diag_total_;
  const uint32_t ok = this->diag_ok_;
  const uint32_t crc_fail_pct = (total == 0) ? 0 : (crc_failed * 100U) / total;
  const uint32_t drop_pct = (total == 0) ? 0 : (this->diag_dropped_ * 100U) / total;
  const uint32_t trunc_pct = (total == 0) ? 0 : (this->diag_truncated_ * 100U) / total;
  const int32_t avg_ok_rssi = (this->diag_rssi_ok_n_ == 0) ? 0 : (this->diag_rssi_ok_sum_ / (int32_t) this->diag_rssi_ok_n_);
  const int32_t avg_drop_rssi = (this->diag_rssi_drop_n_ == 0) ? 0 : (this->diag_rssi_drop_sum_ / (int32_t) this->diag_rssi_drop_n_);

  const uint8_t T1 = (uint8_t) LinkMode::T1;
  const uint8_t C1 = (uint8_t) LinkMode::C1;
  const uint32_t t1_total = this->diag_mode_total_[T1];
  const uint32_t c1_total = this->diag_mode_total_[C1];
  const uint32_t t1_ok = this->diag_mode_ok_[T1];
  const uint32_t c1_ok = this->diag_mode_ok_[C1];
  const uint32_t t1_drop = this->diag_mode_dropped_[T1];
  const uint32_t c1_drop = this->diag_mode_dropped_[C1];
  const uint32_t t1_crc = this->diag_mode_crc_failed_[T1];
  const uint32_t c1_crc = this->diag_mode_crc_failed_[C1];
  const uint32_t t1_per_pct = (t1_total == 0) ? 0 : (t1_drop * 100U) / t1_total;
  const uint32_t c1_per_pct = (c1_total == 0) ? 0 : (c1_drop * 100U) / c1_total;
  const uint32_t t1_crc_pct = (t1_total == 0) ? 0 : (t1_crc * 100U) / t1_total;
  const uint32_t c1_crc_pct = (c1_total == 0) ? 0 : (c1_crc * 100U) / c1_total;
  const int32_t t1_avg_ok_rssi = (this->diag_mode_rssi_ok_n_[T1] == 0) ? 0 : (this->diag_mode_rssi_ok_sum_[T1] / (int32_t) this->diag_mode_rssi_ok_n_[T1]);
  const int32_t c1_avg_ok_rssi = (this->diag_mode_rssi_ok_n_[C1] == 0) ? 0 : (this->diag_mode_rssi_ok_sum_[C1] / (int32_t) this->diag_mode_rssi_ok_n_[C1]);
  const int32_t t1_avg_drop_rssi = (this->diag_mode_rssi_drop_n_[T1] == 0) ? 0 : (this->diag_mode_rssi_drop_sum_[T1] / (int32_t) this->diag_mode_rssi_drop_n_[T1]);
  const int32_t c1_avg_drop_rssi = (this->diag_mode_rssi_drop_n_[C1] == 0) ? 0 : (this->diag_mode_rssi_drop_sum_[C1] / (int32_t) this->diag_mode_rssi_drop_n_[C1]);

  const uint32_t t1_sym_total = this->diag_t1_symbols_total_;
  const uint32_t t1_sym_invalid = this->diag_t1_symbols_invalid_;
  const uint32_t t1_sym_invalid_pct = (t1_sym_total == 0) ? 0 : (t1_sym_invalid * 100U) / t1_sym_total;
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const int32_t ok_vs_drop_rssi_gap =
      (this->diag_rssi_ok_n_ == 0 || this->diag_rssi_drop_n_ == 0) ? 0 : (avg_ok_rssi - avg_drop_rssi);
  const uint32_t rx_false_start_like =
      this->diag_rx_path_.preamble_read_failed + this->diag_rx_path_.payload_size_unknown +
      this->diag_rx_path_.weak_start_aborted + this->diag_rx_path_.probe_start_aborted +
      this->diag_rx_path_.raw_drain_skipped_weak;
  const uint32_t raw_drain_recovery_pct =
      (this->diag_rx_path_.raw_drain_attempted == 0)
          ? 0
          : (this->diag_rx_path_.raw_drain_recovered * 100U) / this->diag_rx_path_.raw_drain_attempted;

  const uint32_t reasons_sum =
      this->diag_dropped_by_bucket_[DB_TOO_SHORT] +
      this->diag_dropped_by_bucket_[DB_DECODE_FAILED] +
      this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED] +
      this->diag_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE] +
      this->diag_dropped_by_bucket_[DB_L_FIELD_INVALID] +
      this->diag_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE] +
      this->diag_dropped_by_bucket_[DB_OTHER];
  const uint32_t reasons_sum_mismatch = (reasons_sum != this->diag_dropped_) ? 1U : 0U;

  const char *hint_code = "OK";
  const char *hint_en = "looks good";
  const char *hint_pl = "wygląda dobrze";
  if (total == 0) {
    hint_code = "NO_DATA";
    hint_en = "no packets received yet";
    hint_pl = "brak odebranych ramek";
  } else {
    if (c1_total > 0 && c1_ok == 0 && c1_crc == c1_total) {
      if (c1_avg_drop_rssi <= -95) {
        hint_code = "C1_WEAK_SIGNAL";
        hint_en = "C1 frames fail DLL CRC at very low RSSI; improve antenna/placement";
        hint_pl = "C1: CRC DLL nie przechodzi przy bardzo niskim RSSI; popraw antenę/pozycję";
      } else {
        hint_code = "C1_INTERFERENCE_OR_RX";
        hint_en = "C1 frames fail DLL CRC despite decent RSSI; check interference/RX settings";
        hint_pl = "C1: CRC DLL nie przechodzi mimo niezłego RSSI; sprawdź zakłócenia/ustawienia RX";
      }
    } else if (c1_total > 0 && c1_crc > 0 && c1_avg_ok_rssi >= -65 && c1_avg_drop_rssi >= -80) {
      hint_code = "C1_OVERLOAD_OR_MULTIPATH";
      hint_en = "C1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move antenna 0.5-2m, change polarization, or attenuate.";
      hint_pl = "C1: CRC pada mimo dobrego RSSI; możliwy przester odbiornika lub wielodrogowość. Odsuń antenę 0,5-2m, zmień polaryzację lub stłum sygnał.";
    } else if (t1_total > 0 && t1_crc > 0 && t1_avg_ok_rssi >= -65 && t1_avg_drop_rssi >= -80) {
      hint_code = "T1_OVERLOAD_OR_MULTIPATH";
      hint_en = "T1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move/rotate antenna or attenuate.";
      hint_pl = "T1: CRC pada mimo dobrego RSSI; możliwy przester lub wielodrogowość. Przestaw/obróć antenę lub stłum sygnał.";
    } else if (is_sx1276 &&
               drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10 &&
               (rx_false_start_like >= 40 || this->diag_rx_path_.raw_drain_attempted >= 20 || this->diag_rx_path_.fifo_overrun > 0)) {
      hint_code = "SX1276_BUSY_ETHER";
      hint_en = "SX1276 is picking up many very weak/partial T1 starts while valid packets are much stronger; this looks like dense ether with distant or overlapping meters, not just a bad antenna.";
      hint_pl = "SX1276 łapie dużo bardzo słabych/częściowych startów T1, podczas gdy poprawne pakiety są znacznie silniejsze; to wygląda na gęsty eter z dalekimi lub nakładającymi się licznikami, a nie tylko złą antenę.";
    } else if (drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10) {
      hint_code = "BUSY_ETHER_WEAK_TRASH";
      hint_en = "many dropped packets are far weaker than valid ones; the receiver likely hears lots of distant or overlapping neighbor meters.";
      hint_pl = "wiele odrzuconych ramek jest dużo słabszych niż poprawne; odbiornik prawdopodobnie słyszy dużo dalekich lub nakładających się liczników sąsiadów.";
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92 && (ok == 0 || avg_ok_rssi <= -86)) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI and weak valid packets; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI i słabych poprawnych pakietach; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely collisions, interference, or bit errors";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe kolizje, zakłócenia lub błędy bitów";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (is_sx1276 &&
               drop_pct >= 20 &&
               (this->diag_rx_path_.preamble_read_failed >= 20 ||
                this->diag_rx_path_.payload_size_unknown >= 10)) {
      hint_code = "SX1276_RX_NOISY";
      hint_en = "SX1276 sees many false starts or undecodable starts; RX frontend likely needs tuning";
      hint_pl = "SX1276 ma dużo fałszywych startów lub nieustalonych początków ramek; ścieżka RX wymaga strojenia";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }

  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\"," 
           "\"interval_s\":%u,"
           "\"uptime_ms\":%lu,"
           "\"listen_mode\":\"%s\","
           "\"total\":%u,"
           "\"ok\":%u,"
           "\"truncated\":%u,"
           "\"dropped\":%u,"
           "\"crc_failed\":%u,"
           "\"crc_fail_pct\":%u,"
           "\"drop_pct\":%u,"
           "\"trunc_pct\":%u,"
           "\"avg_ok_rssi\":%d,"
           "\"avg_drop_rssi\":%d,"
           "\"t1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d,"
             "\"sym_total\":%u,\"sym_invalid\":%u,\"sym_invalid_pct\":%u"
           "},"
           "\"c1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d"
           "},"
           "\"dropped_by_reason\":{"
             "\"too_short\":%u,"
             "\"decode_failed\":%u,"
             "\"dll_crc_failed\":%u,"
             "\"unknown_preamble\":%u,"
             "\"l_field_invalid\":%u,"
             "\"unknown_link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"dropped_by_stage\":{"
             "\"precheck\":%u,"
             "\"t1_decode3of6\":%u,"
             "\"t1_l_field\":%u,"
             "\"t1_length_check\":%u,"
             "\"c1_precheck\":%u,"
             "\"c1_preamble\":%u,"
             "\"c1_suffix\":%u,"
             "\"c1_l_field\":%u,"
             "\"c1_length_check\":%u,"
             "\"dll_crc_first\":%u,"
             "\"dll_crc_mid\":%u,"
             "\"dll_crc_final\":%u,"
             "\"dll_crc_b1\":%u,"
             "\"dll_crc_b2\":%u,"
             "\"link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"rx_path\":{"
             "\"irq_timeout\":%u,"
             "\"preamble_read_failed\":%u,"
             "\"preamble_retry_recovered\":%u,"
             "\"t1_header_read_failed\":%u,"
             "\"payload_size_unknown\":%u,"
             "\"raw_drain_attempted\":%u,"
             "\"raw_drain_recovered\":%u,"
             "\"raw_drain_recovery_pct\":%u,"
             "\"raw_drain_bytes\":%u,"
             "\"payload_read_failed\":%u,"
             "\"queue_send_failed\":%u,"
             "\"fifo_overrun\":%u,"
             "\"weak_start_aborted\":%u,"
             "\"probe_start_aborted\":%u,"
             "\"raw_drain_skipped_weak\":%u,"
             "\"false_start_like\":%u,"
             "\"probe_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u},"
             "\"weak_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u}"
           "},"
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\","
           "\"busy_ether_state\":\"%s\""
           "}",
           (unsigned) interval_s,
           (unsigned long) now_ms,
           listen_mode,
           (unsigned) total,
           (unsigned) this->diag_ok_,
           (unsigned) this->diag_truncated_,
           (unsigned) this->diag_dropped_,
           (unsigned) crc_failed,
           (unsigned) crc_fail_pct,
           (unsigned) drop_pct,
           (unsigned) trunc_pct,
           (int) avg_ok_rssi,
           (int) avg_drop_rssi,
           (unsigned) t1_total,
           (unsigned) t1_ok,
           (unsigned) t1_drop,
           (unsigned) t1_per_pct,
           (unsigned) t1_crc,
           (unsigned) t1_crc_pct,
           (int) t1_avg_ok_rssi,
           (int) t1_avg_drop_rssi,
           (unsigned) t1_sym_total,
           (unsigned) t1_sym_invalid,
           (unsigned) t1_sym_invalid_pct,
           (unsigned) c1_total,
           (unsigned) c1_ok,
           (unsigned) c1_drop,
           (unsigned) c1_per_pct,
           (unsigned) c1_crc,
           (unsigned) c1_crc_pct,
           (int) c1_avg_ok_rssi,
           (int) c1_avg_drop_rssi,
           (unsigned) this->diag_dropped_by_bucket_[DB_TOO_SHORT],
           (unsigned) this->diag_dropped_by_bucket_[DB_DECODE_FAILED],
           (unsigned) this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED],
           (unsigned) this->diag_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE],
           (unsigned) this->diag_dropped_by_bucket_[DB_L_FIELD_INVALID],
           (unsigned) this->diag_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE],
           (unsigned) this->diag_dropped_by_bucket_[DB_OTHER],
           (unsigned) this->diag_dropped_by_stage_[SB_PRECHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_T1_DECODE3OF6],
           (unsigned) this->diag_dropped_by_stage_[SB_T1_L_FIELD],
           (unsigned) this->diag_dropped_by_stage_[SB_T1_LENGTH_CHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_PRECHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_PREAMBLE],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_SUFFIX],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_L_FIELD],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_LENGTH_CHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_FIRST],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_MID],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_FINAL],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_B1],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_B2],
           (unsigned) this->diag_dropped_by_stage_[SB_LINK_MODE],
           (unsigned) this->diag_dropped_by_stage_[SB_OTHER],
           (unsigned) this->diag_rx_path_.irq_timeout,
           (unsigned) this->diag_rx_path_.preamble_read_failed,
           (unsigned) this->diag_rx_path_.preamble_retry_recovered,
           (unsigned) this->diag_rx_path_.t1_header_read_failed,
           (unsigned) this->diag_rx_path_.payload_size_unknown,
           (unsigned) this->diag_rx_path_.raw_drain_attempted,
           (unsigned) this->diag_rx_path_.raw_drain_recovered,
           (unsigned) raw_drain_recovery_pct,
           (unsigned) this->diag_rx_path_.raw_drain_bytes,
           (unsigned) this->diag_rx_path_.payload_read_failed,
           (unsigned) this->diag_rx_path_.queue_send_failed,
           (unsigned) this->diag_rx_path_.fifo_overrun,
           (unsigned) this->diag_rx_path_.weak_start_aborted,
           (unsigned) this->diag_rx_path_.probe_start_aborted,
           (unsigned) this->diag_rx_path_.raw_drain_skipped_weak,
           (unsigned) rx_false_start_like,
           // probe_abort_rssi buckets
           (unsigned) this->diag_rx_path_.probe_abort_rssi[0],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[1],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[2],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[3],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[4],
           // weak_abort_rssi buckets
           (unsigned) this->diag_rx_path_.weak_abort_rssi[0],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[1],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[2],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[3],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[4],
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl,
           // busy_ether_state: reflects state BEFORE evaluating this window.
           // evaluate_busy_ether_adaptive_() runs after publish to access full window counters.
           // Use busy_ether_changed event for precise transition timestamps.
           !is_sx1276 ? "n/a"
               : (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::ADAPTIVE)
                   ? (this->busy_ether_was_active_ ? "adaptive_active" : "adaptive_passive")
                   : (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE ? "aggressive" : "normal"));

  const std::string summary_topic = this->diag_summary_topic_();
  mqtt->publish(summary_topic, payload);
  ESP_LOGI(TAG, "DIAG summary / podsumowanie diag: topic=%s interval=%us uptime_ms=%lu listen_mode=%s total=%u ok=%u truncated=%u dropped=%u crc_failed=%u",
           summary_topic.c_str(), (unsigned) interval_s, (unsigned long) now_ms, listen_mode,
           (unsigned) total, (unsigned) this->diag_ok_,
           (unsigned) this->diag_truncated_, (unsigned) this->diag_dropped_, (unsigned) crc_failed);

  if (std::strcmp(hint_code, "OK") == 0 || std::strcmp(hint_code, "GOOD") == 0) {
    ESP_LOGI(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  } else {
    ESP_LOGW(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  }

  // Evaluate adaptive busy-ether state BEFORE resetting windowed counters —
  // this is the only point where all current-window accumulations are visible.
  this->evaluate_busy_ether_adaptive_(now_ms);

  // Publish suggestion event based on current window data (before reset).
  this->maybe_publish_suggestion_(now_ms);

  this->diag_total_ = 0;
  this->diag_ok_ = 0;
  this->diag_truncated_ = 0;
  this->diag_dropped_ = 0;
  this->diag_dropped_by_bucket_.fill(0);
  this->diag_dropped_by_stage_.fill(0);
  this->diag_rssi_ok_sum_ = 0;
  this->diag_rssi_ok_n_ = 0;
  this->diag_rssi_drop_sum_ = 0;
  this->diag_rssi_drop_n_ = 0;
  this->diag_mode_total_.fill(0);
  this->diag_mode_ok_.fill(0);
  this->diag_mode_dropped_.fill(0);
  this->diag_mode_crc_failed_.fill(0);
  this->diag_mode_rssi_ok_sum_.fill(0);
  this->diag_mode_rssi_ok_n_.fill(0);
  this->diag_mode_rssi_drop_sum_.fill(0);
  this->diag_mode_rssi_drop_n_.fill(0);
  this->diag_t1_symbols_total_ = 0;
  this->diag_t1_symbols_invalid_ = 0;
  this->diag_rx_path_ = {};
}


void Radio::maybe_publish_diag_15min_summary_(uint32_t now_ms) {
  if (!this->diag_publish_summary_) return;
  if (!this->diag_publish_summary_15min_) return;
  if (this->diag_topic_.empty()) return;
  if (this->last_diag_15min_summary_ms_ == 0) {
    this->last_diag_15min_summary_ms_ = now_ms;
    return;
  }
  uint32_t elapsed = now_ms - this->last_diag_15min_summary_ms_;
  if (elapsed < this->DIAG_15MIN_INTERVAL_MS_) return;
  this->last_diag_15min_summary_ms_ = now_ms;

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";
  const uint32_t interval_s = elapsed / 1000U;

  char payload[2048];
  const uint32_t crc_failed = this->diag_15m_dropped_by_bucket_[DB_DLL_CRC_FAILED];
  const uint32_t total = this->diag_15m_total_;
  const uint32_t ok = this->diag_15m_ok_;
  const uint32_t crc_fail_pct = (total == 0) ? 0 : (crc_failed * 100U) / total;
  const uint32_t drop_pct = (total == 0) ? 0 : (this->diag_15m_dropped_ * 100U) / total;
  const uint32_t trunc_pct = (total == 0) ? 0 : (this->diag_15m_truncated_ * 100U) / total;
  const int32_t avg_ok_rssi = (this->diag_15m_rssi_ok_n_ == 0) ? 0 : (this->diag_15m_rssi_ok_sum_ / (int32_t) this->diag_15m_rssi_ok_n_);
  const int32_t avg_drop_rssi = (this->diag_15m_rssi_drop_n_ == 0) ? 0 : (this->diag_15m_rssi_drop_sum_ / (int32_t) this->diag_15m_rssi_drop_n_);

  const uint8_t T1 = (uint8_t) LinkMode::T1;
  const uint8_t C1 = (uint8_t) LinkMode::C1;
  const uint32_t t1_total = this->diag_15m_mode_total_[T1];
  const uint32_t c1_total = this->diag_15m_mode_total_[C1];
  const uint32_t t1_ok = this->diag_15m_mode_ok_[T1];
  const uint32_t c1_ok = this->diag_15m_mode_ok_[C1];
  const uint32_t t1_drop = this->diag_15m_mode_dropped_[T1];
  const uint32_t c1_drop = this->diag_15m_mode_dropped_[C1];
  const uint32_t t1_crc = this->diag_15m_mode_crc_failed_[T1];
  const uint32_t c1_crc = this->diag_15m_mode_crc_failed_[C1];
  const uint32_t t1_per_pct = (t1_total == 0) ? 0 : (t1_drop * 100U) / t1_total;
  const uint32_t c1_per_pct = (c1_total == 0) ? 0 : (c1_drop * 100U) / c1_total;
  const uint32_t t1_crc_pct = (t1_total == 0) ? 0 : (t1_crc * 100U) / t1_total;
  const uint32_t c1_crc_pct = (c1_total == 0) ? 0 : (c1_crc * 100U) / c1_total;
  const int32_t t1_avg_ok_rssi = (this->diag_15m_mode_rssi_ok_n_[T1] == 0) ? 0 : (this->diag_15m_mode_rssi_ok_sum_[T1] / (int32_t) this->diag_15m_mode_rssi_ok_n_[T1]);
  const int32_t c1_avg_ok_rssi = (this->diag_15m_mode_rssi_ok_n_[C1] == 0) ? 0 : (this->diag_15m_mode_rssi_ok_sum_[C1] / (int32_t) this->diag_15m_mode_rssi_ok_n_[C1]);
  const int32_t t1_avg_drop_rssi = (this->diag_15m_mode_rssi_drop_n_[T1] == 0) ? 0 : (this->diag_15m_mode_rssi_drop_sum_[T1] / (int32_t) this->diag_15m_mode_rssi_drop_n_[T1]);
  const int32_t c1_avg_drop_rssi = (this->diag_15m_mode_rssi_drop_n_[C1] == 0) ? 0 : (this->diag_15m_mode_rssi_drop_sum_[C1] / (int32_t) this->diag_15m_mode_rssi_drop_n_[C1]);

  const uint32_t t1_sym_total = this->diag_15m_t1_symbols_total_;
  const uint32_t t1_sym_invalid = this->diag_15m_t1_symbols_invalid_;
  const uint32_t t1_sym_invalid_pct = (t1_sym_total == 0) ? 0 : (t1_sym_invalid * 100U) / t1_sym_total;
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const int32_t ok_vs_drop_rssi_gap =
      (this->diag_15m_rssi_ok_n_ == 0 || this->diag_15m_rssi_drop_n_ == 0) ? 0 : (avg_ok_rssi - avg_drop_rssi);
  const uint32_t rx_false_start_like =
      this->diag_15m_rx_path_.preamble_read_failed + this->diag_15m_rx_path_.payload_size_unknown +
      this->diag_15m_rx_path_.weak_start_aborted + this->diag_15m_rx_path_.probe_start_aborted +
      this->diag_15m_rx_path_.raw_drain_skipped_weak;
  const uint32_t raw_drain_recovery_pct =
      (this->diag_15m_rx_path_.raw_drain_attempted == 0)
          ? 0
          : (this->diag_15m_rx_path_.raw_drain_recovered * 100U) / this->diag_15m_rx_path_.raw_drain_attempted;

  const uint32_t reasons_sum =
      this->diag_15m_dropped_by_bucket_[DB_TOO_SHORT] +
      this->diag_15m_dropped_by_bucket_[DB_DECODE_FAILED] +
      this->diag_15m_dropped_by_bucket_[DB_DLL_CRC_FAILED] +
      this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE] +
      this->diag_15m_dropped_by_bucket_[DB_L_FIELD_INVALID] +
      this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE] +
      this->diag_15m_dropped_by_bucket_[DB_OTHER];
  const uint32_t reasons_sum_mismatch = (reasons_sum != this->diag_15m_dropped_) ? 1U : 0U;

  const char *hint_code = "OK";
  const char *hint_en = "looks good";
  const char *hint_pl = "wygląda dobrze";
  if (total == 0) {
    hint_code = "NO_DATA";
    hint_en = "no packets received yet";
    hint_pl = "brak odebranych ramek";
  } else {
    if (c1_total > 0 && c1_ok == 0 && c1_crc == c1_total) {
      if (c1_avg_drop_rssi <= -95) {
        hint_code = "C1_WEAK_SIGNAL";
        hint_en = "C1 frames fail DLL CRC at very low RSSI; improve antenna/placement";
        hint_pl = "C1: CRC DLL nie przechodzi przy bardzo niskim RSSI; popraw antenę/pozycję";
      } else {
        hint_code = "C1_INTERFERENCE_OR_RX";
        hint_en = "C1 frames fail DLL CRC despite decent RSSI; check interference/RX settings";
        hint_pl = "C1: CRC DLL nie przechodzi mimo niezłego RSSI; sprawdź zakłócenia/ustawienia RX";
      }
    } else if (c1_total > 0 && c1_crc > 0 && c1_avg_ok_rssi >= -65 && c1_avg_drop_rssi >= -80) {
      hint_code = "C1_OVERLOAD_OR_MULTIPATH";
      hint_en = "C1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move antenna 0.5-2m, change polarization, or attenuate.";
      hint_pl = "C1: CRC pada mimo dobrego RSSI; możliwy przester odbiornika lub wielodrogowość. Odsuń antenę 0,5-2m, zmień polaryzację lub stłum sygnał.";
    } else if (t1_total > 0 && t1_crc > 0 && t1_avg_ok_rssi >= -65 && t1_avg_drop_rssi >= -80) {
      hint_code = "T1_OVERLOAD_OR_MULTIPATH";
      hint_en = "T1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move/rotate antenna or attenuate.";
      hint_pl = "T1: CRC pada mimo dobrego RSSI; możliwy przester lub wielodrogowość. Przestaw/obróć antenę lub stłum sygnał.";
    } else if (is_sx1276 &&
               drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10 &&
               (rx_false_start_like >= 40 || this->diag_15m_rx_path_.raw_drain_attempted >= 20 || this->diag_15m_rx_path_.fifo_overrun > 0)) {
      hint_code = "SX1276_BUSY_ETHER";
      hint_en = "SX1276 is picking up many very weak/partial T1 starts while valid packets are much stronger; this looks like dense ether with distant or overlapping meters, not just a bad antenna.";
      hint_pl = "SX1276 łapie dużo bardzo słabych/częściowych startów T1, podczas gdy poprawne pakiety są znacznie silniejsze; to wygląda na gęsty eter z dalekimi lub nakładającymi się licznikami, a nie tylko złą antenę.";
    } else if (drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10) {
      hint_code = "BUSY_ETHER_WEAK_TRASH";
      hint_en = "many dropped packets are far weaker than valid ones; the receiver likely hears lots of distant or overlapping neighbor meters.";
      hint_pl = "wiele odrzuconych ramek jest dużo słabszych niż poprawne; odbiornik prawdopodobnie słyszy dużo dalekich lub nakładających się liczników sąsiadów.";
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92 && (ok == 0 || avg_ok_rssi <= -86)) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI and weak valid packets; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI i słabych poprawnych pakietach; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely collisions, interference, or bit errors";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe kolizje, zakłócenia lub błędy bitów";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (is_sx1276 &&
               drop_pct >= 20 &&
               (this->diag_15m_rx_path_.preamble_read_failed >= 20 ||
                this->diag_15m_rx_path_.payload_size_unknown >= 10)) {
      hint_code = "SX1276_RX_NOISY";
      hint_en = "SX1276 sees many false starts or undecodable starts; RX frontend likely needs tuning";
      hint_pl = "SX1276 ma dużo fałszywych startów lub nieustalonych początków ramek; ścieżka RX wymaga strojenia";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }

  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\"," 
           "\"interval_s\":%u,"
           "\"uptime_ms\":%lu,"
           "\"listen_mode\":\"%s\","
           "\"total\":%u,"
           "\"ok\":%u,"
           "\"truncated\":%u,"
           "\"dropped\":%u,"
           "\"crc_failed\":%u,"
           "\"crc_fail_pct\":%u,"
           "\"drop_pct\":%u,"
           "\"trunc_pct\":%u,"
           "\"avg_ok_rssi\":%d,"
           "\"avg_drop_rssi\":%d,"
           "\"t1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d,"
             "\"sym_total\":%u,\"sym_invalid\":%u,\"sym_invalid_pct\":%u"
           "},"
           "\"c1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d"
           "},"
           "\"dropped_by_reason\":{"
             "\"too_short\":%u,"
             "\"decode_failed\":%u,"
             "\"dll_crc_failed\":%u,"
             "\"unknown_preamble\":%u,"
             "\"l_field_invalid\":%u,"
             "\"unknown_link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"dropped_by_stage\":{"
             "\"precheck\":%u,"
             "\"t1_decode3of6\":%u,"
             "\"t1_l_field\":%u,"
             "\"t1_length_check\":%u,"
             "\"c1_precheck\":%u,"
             "\"c1_preamble\":%u,"
             "\"c1_suffix\":%u,"
             "\"c1_l_field\":%u,"
             "\"c1_length_check\":%u,"
             "\"dll_crc_first\":%u,"
             "\"dll_crc_mid\":%u,"
             "\"dll_crc_final\":%u,"
             "\"dll_crc_b1\":%u,"
             "\"dll_crc_b2\":%u,"
             "\"link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"rx_path\":{"
             "\"irq_timeout\":%u,"
             "\"preamble_read_failed\":%u,"
             "\"preamble_retry_recovered\":%u,"
             "\"t1_header_read_failed\":%u,"
             "\"payload_size_unknown\":%u,"
             "\"raw_drain_attempted\":%u,"
             "\"raw_drain_recovered\":%u,"
             "\"raw_drain_recovery_pct\":%u,"
             "\"raw_drain_bytes\":%u,"
             "\"payload_read_failed\":%u,"
             "\"queue_send_failed\":%u,"
             "\"fifo_overrun\":%u,"
             "\"weak_start_aborted\":%u,"
             "\"probe_start_aborted\":%u,"
             "\"raw_drain_skipped_weak\":%u,"
             "\"false_start_like\":%u,"
             "\"probe_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u},"
             "\"weak_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u}"
           "},"
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\""
           "}",
           (unsigned) interval_s,
           (unsigned long) now_ms,
           listen_mode,
           (unsigned) total,
           (unsigned) this->diag_15m_ok_,
           (unsigned) this->diag_15m_truncated_,
           (unsigned) this->diag_15m_dropped_,
           (unsigned) crc_failed,
           (unsigned) crc_fail_pct,
           (unsigned) drop_pct,
           (unsigned) trunc_pct,
           (int) avg_ok_rssi,
           (int) avg_drop_rssi,
           (unsigned) t1_total,
           (unsigned) t1_ok,
           (unsigned) t1_drop,
           (unsigned) t1_per_pct,
           (unsigned) t1_crc,
           (unsigned) t1_crc_pct,
           (int) t1_avg_ok_rssi,
           (int) t1_avg_drop_rssi,
           (unsigned) t1_sym_total,
           (unsigned) t1_sym_invalid,
           (unsigned) t1_sym_invalid_pct,
           (unsigned) c1_total,
           (unsigned) c1_ok,
           (unsigned) c1_drop,
           (unsigned) c1_per_pct,
           (unsigned) c1_crc,
           (unsigned) c1_crc_pct,
           (int) c1_avg_ok_rssi,
           (int) c1_avg_drop_rssi,
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_TOO_SHORT],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_DECODE_FAILED],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_DLL_CRC_FAILED],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_L_FIELD_INVALID],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_OTHER],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_PRECHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_T1_DECODE3OF6],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_T1_L_FIELD],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_T1_LENGTH_CHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_PRECHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_PREAMBLE],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_SUFFIX],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_L_FIELD],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_LENGTH_CHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_FIRST],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_MID],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_FINAL],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_B1],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_B2],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_LINK_MODE],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_OTHER],
           (unsigned) this->diag_15m_rx_path_.irq_timeout,
           (unsigned) this->diag_15m_rx_path_.preamble_read_failed,
           (unsigned) this->diag_15m_rx_path_.preamble_retry_recovered,
           (unsigned) this->diag_15m_rx_path_.t1_header_read_failed,
           (unsigned) this->diag_15m_rx_path_.payload_size_unknown,
           (unsigned) this->diag_15m_rx_path_.raw_drain_attempted,
           (unsigned) this->diag_15m_rx_path_.raw_drain_recovered,
           (unsigned) raw_drain_recovery_pct,
           (unsigned) this->diag_15m_rx_path_.raw_drain_bytes,
           (unsigned) this->diag_15m_rx_path_.payload_read_failed,
           (unsigned) this->diag_15m_rx_path_.queue_send_failed,
           (unsigned) this->diag_15m_rx_path_.fifo_overrun,
           (unsigned) this->diag_15m_rx_path_.weak_start_aborted,
           (unsigned) this->diag_15m_rx_path_.probe_start_aborted,
           (unsigned) this->diag_15m_rx_path_.raw_drain_skipped_weak,
           (unsigned) rx_false_start_like,
           // probe_abort_rssi buckets
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[0],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[1],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[2],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[3],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[4],
           // weak_abort_rssi buckets
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[0],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[1],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[2],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[3],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[4],
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl);

  const std::string summary_topic = this->diag_summary_15min_topic_();
  mqtt->publish(summary_topic, payload);
  ESP_LOGI(TAG, "DIAG 15min summary / podsumowanie 15min diag: topic=%s interval=%us uptime_ms=%lu listen_mode=%s total=%u ok=%u truncated=%u dropped=%u crc_failed=%u",
           summary_topic.c_str(), (unsigned) interval_s, (unsigned long) now_ms, listen_mode,
           (unsigned) total, (unsigned) this->diag_15m_ok_,
           (unsigned) this->diag_15m_truncated_, (unsigned) this->diag_15m_dropped_, (unsigned) crc_failed);

  if (std::strcmp(hint_code, "OK") == 0 || std::strcmp(hint_code, "GOOD") == 0) {
    ESP_LOGI(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  } else {
    ESP_LOGW(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  }

  // Publish snapshot of all highlight meters alongside this summary (read-only, no window reset).
  if (this->diag_publish_summary_highlight_meters_ && !this->highlight_meter_stats_.empty()) {
    for (auto &kv : this->highlight_meter_stats_) {
      const uint64_t key = kv.first; // uint64_t — meter_id can exceed uint32_t range when shifted
      MeterStats &st = kv.second;
      const uint32_t meter_id = key >> 8;
      const uint8_t mode_byte = key & 0xFF;
      char id_str[12];
      snprintf(id_str, sizeof(id_str), "%08" PRIu32, meter_id);
      const char *mode_str = (mode_byte == (uint8_t) LinkMode::C1) ? "C1" : ((mode_byte == (uint8_t) LinkMode::S1) ? "S1" : "T1");
      const uint32_t st_elapsed_s = elapsed / 1000U;
      this->publish_meter_window_for_("summary_15min", st_elapsed_s, id_str, mode_str, st,
                                      st.count_window_time, st.rssi_sum_window_time,
                                      st.rssi_n_window_time, st.interval_sum_window_time_ms,
                                      st.interval_n_window_time,
                                      false, false);
    }
    // Publish all highlight meters as a single batch payload for easier log analysis.
    this->publish_meter_window_batch_("summary_15min", elapsed / 1000U, now_ms);
  }
  this->diag_15m_total_ = 0;
  this->diag_15m_ok_ = 0;
  this->diag_15m_truncated_ = 0;
  this->diag_15m_dropped_ = 0;
  this->diag_15m_dropped_by_bucket_.fill(0);
  this->diag_15m_dropped_by_stage_.fill(0);
  this->diag_15m_rssi_ok_sum_ = 0;
  this->diag_15m_rssi_ok_n_ = 0;
  this->diag_15m_rssi_drop_sum_ = 0;
  this->diag_15m_rssi_drop_n_ = 0;
  this->diag_15m_mode_total_.fill(0);
  this->diag_15m_mode_ok_.fill(0);
  this->diag_15m_mode_dropped_.fill(0);
  this->diag_15m_mode_crc_failed_.fill(0);
  this->diag_15m_mode_rssi_ok_sum_.fill(0);
  this->diag_15m_mode_rssi_ok_n_.fill(0);
  this->diag_15m_mode_rssi_drop_sum_.fill(0);
  this->diag_15m_mode_rssi_drop_n_.fill(0);
  this->diag_15m_t1_symbols_total_ = 0;
  this->diag_15m_t1_symbols_invalid_ = 0;
  this->diag_15m_rx_path_ = {};
}


void Radio::maybe_publish_diag_60min_summary_(uint32_t now_ms) {
  if (!this->diag_publish_summary_) return;
  if (!this->diag_publish_summary_60min_) return;
  if (this->diag_topic_.empty()) return;
  if (this->last_diag_60min_summary_ms_ == 0) {
    this->last_diag_60min_summary_ms_ = now_ms;
    return;
  }
  uint32_t elapsed = now_ms - this->last_diag_60min_summary_ms_;
  if (elapsed < this->DIAG_60MIN_INTERVAL_MS_) return;
  this->last_diag_60min_summary_ms_ = now_ms;

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";
  const uint32_t interval_s = elapsed / 1000U;

  char payload[2048];
  const uint32_t crc_failed = this->diag_60min_dropped_by_bucket_[DB_DLL_CRC_FAILED];
  const uint32_t total = this->diag_60min_total_;
  const uint32_t ok = this->diag_60min_ok_;
  const uint32_t crc_fail_pct = (total == 0) ? 0 : (crc_failed * 100U) / total;
  const uint32_t drop_pct = (total == 0) ? 0 : (this->diag_60min_dropped_ * 100U) / total;
  const uint32_t trunc_pct = (total == 0) ? 0 : (this->diag_60min_truncated_ * 100U) / total;
  const int32_t avg_ok_rssi = (this->diag_60min_rssi_ok_n_ == 0) ? 0 : (this->diag_60min_rssi_ok_sum_ / (int32_t) this->diag_60min_rssi_ok_n_);
  const int32_t avg_drop_rssi = (this->diag_60min_rssi_drop_n_ == 0) ? 0 : (this->diag_60min_rssi_drop_sum_ / (int32_t) this->diag_60min_rssi_drop_n_);

  const uint8_t T1 = (uint8_t) LinkMode::T1;
  const uint8_t C1 = (uint8_t) LinkMode::C1;
  const uint32_t t1_total = this->diag_60min_mode_total_[T1];
  const uint32_t c1_total = this->diag_60min_mode_total_[C1];
  const uint32_t t1_ok = this->diag_60min_mode_ok_[T1];
  const uint32_t c1_ok = this->diag_60min_mode_ok_[C1];
  const uint32_t t1_drop = this->diag_60min_mode_dropped_[T1];
  const uint32_t c1_drop = this->diag_60min_mode_dropped_[C1];
  const uint32_t t1_crc = this->diag_60min_mode_crc_failed_[T1];
  const uint32_t c1_crc = this->diag_60min_mode_crc_failed_[C1];
  const uint32_t t1_per_pct = (t1_total == 0) ? 0 : (t1_drop * 100U) / t1_total;
  const uint32_t c1_per_pct = (c1_total == 0) ? 0 : (c1_drop * 100U) / c1_total;
  const uint32_t t1_crc_pct = (t1_total == 0) ? 0 : (t1_crc * 100U) / t1_total;
  const uint32_t c1_crc_pct = (c1_total == 0) ? 0 : (c1_crc * 100U) / c1_total;
  const int32_t t1_avg_ok_rssi = (this->diag_60min_mode_rssi_ok_n_[T1] == 0) ? 0 : (this->diag_60min_mode_rssi_ok_sum_[T1] / (int32_t) this->diag_60min_mode_rssi_ok_n_[T1]);
  const int32_t c1_avg_ok_rssi = (this->diag_60min_mode_rssi_ok_n_[C1] == 0) ? 0 : (this->diag_60min_mode_rssi_ok_sum_[C1] / (int32_t) this->diag_60min_mode_rssi_ok_n_[C1]);
  const int32_t t1_avg_drop_rssi = (this->diag_60min_mode_rssi_drop_n_[T1] == 0) ? 0 : (this->diag_60min_mode_rssi_drop_sum_[T1] / (int32_t) this->diag_60min_mode_rssi_drop_n_[T1]);
  const int32_t c1_avg_drop_rssi = (this->diag_60min_mode_rssi_drop_n_[C1] == 0) ? 0 : (this->diag_60min_mode_rssi_drop_sum_[C1] / (int32_t) this->diag_60min_mode_rssi_drop_n_[C1]);

  const uint32_t t1_sym_total = this->diag_60min_t1_symbols_total_;
  const uint32_t t1_sym_invalid = this->diag_60min_t1_symbols_invalid_;
  const uint32_t t1_sym_invalid_pct = (t1_sym_total == 0) ? 0 : (t1_sym_invalid * 100U) / t1_sym_total;
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const int32_t ok_vs_drop_rssi_gap =
      (this->diag_60min_rssi_ok_n_ == 0 || this->diag_60min_rssi_drop_n_ == 0) ? 0 : (avg_ok_rssi - avg_drop_rssi);
  const uint32_t rx_false_start_like =
      this->diag_60min_rx_path_.preamble_read_failed + this->diag_60min_rx_path_.payload_size_unknown +
      this->diag_60min_rx_path_.weak_start_aborted + this->diag_60min_rx_path_.probe_start_aborted +
      this->diag_60min_rx_path_.raw_drain_skipped_weak;
  const uint32_t raw_drain_recovery_pct =
      (this->diag_60min_rx_path_.raw_drain_attempted == 0)
          ? 0
          : (this->diag_60min_rx_path_.raw_drain_recovered * 100U) / this->diag_60min_rx_path_.raw_drain_attempted;

  const uint32_t reasons_sum =
      this->diag_60min_dropped_by_bucket_[DB_TOO_SHORT] +
      this->diag_60min_dropped_by_bucket_[DB_DECODE_FAILED] +
      this->diag_60min_dropped_by_bucket_[DB_DLL_CRC_FAILED] +
      this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE] +
      this->diag_60min_dropped_by_bucket_[DB_L_FIELD_INVALID] +
      this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE] +
      this->diag_60min_dropped_by_bucket_[DB_OTHER];
  const uint32_t reasons_sum_mismatch = (reasons_sum != this->diag_60min_dropped_) ? 1U : 0U;

  const char *hint_code = "OK";
  const char *hint_en = "looks good";
  const char *hint_pl = "wygląda dobrze";
  if (total == 0) {
    hint_code = "NO_DATA";
    hint_en = "no packets received yet";
    hint_pl = "brak odebranych ramek";
  } else {
    if (c1_total > 0 && c1_ok == 0 && c1_crc == c1_total) {
      if (c1_avg_drop_rssi <= -95) {
        hint_code = "C1_WEAK_SIGNAL";
        hint_en = "C1 frames fail DLL CRC at very low RSSI; improve antenna/placement";
        hint_pl = "C1: CRC DLL nie przechodzi przy bardzo niskim RSSI; popraw antenę/pozycję";
      } else {
        hint_code = "C1_INTERFERENCE_OR_RX";
        hint_en = "C1 frames fail DLL CRC despite decent RSSI; check interference/RX settings";
        hint_pl = "C1: CRC DLL nie przechodzi mimo niezłego RSSI; sprawdź zakłócenia/ustawienia RX";
      }
    } else if (c1_total > 0 && c1_crc > 0 && c1_avg_ok_rssi >= -65 && c1_avg_drop_rssi >= -80) {
      hint_code = "C1_OVERLOAD_OR_MULTIPATH";
      hint_en = "C1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move antenna 0.5-2m, change polarization, or attenuate.";
      hint_pl = "C1: CRC pada mimo dobrego RSSI; możliwy przester odbiornika lub wielodrogowość. Odsuń antenę 0,5-2m, zmień polaryzację lub stłum sygnał.";
    } else if (t1_total > 0 && t1_crc > 0 && t1_avg_ok_rssi >= -65 && t1_avg_drop_rssi >= -80) {
      hint_code = "T1_OVERLOAD_OR_MULTIPATH";
      hint_en = "T1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move/rotate antenna or attenuate.";
      hint_pl = "T1: CRC pada mimo dobrego RSSI; możliwy przester lub wielodrogowość. Przestaw/obróć antenę lub stłum sygnał.";
    } else if (is_sx1276 &&
               drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10 &&
               (rx_false_start_like >= 40 || this->diag_60min_rx_path_.raw_drain_attempted >= 20 || this->diag_60min_rx_path_.fifo_overrun > 0)) {
      hint_code = "SX1276_BUSY_ETHER";
      hint_en = "SX1276 is picking up many very weak/partial T1 starts while valid packets are much stronger; this looks like dense ether with distant or overlapping meters, not just a bad antenna.";
      hint_pl = "SX1276 łapie dużo bardzo słabych/częściowych startów T1, podczas gdy poprawne pakiety są znacznie silniejsze; to wygląda na gęsty eter z dalekimi lub nakładającymi się licznikami, a nie tylko złą antenę.";
    } else if (drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10) {
      hint_code = "BUSY_ETHER_WEAK_TRASH";
      hint_en = "many dropped packets are far weaker than valid ones; the receiver likely hears lots of distant or overlapping neighbor meters.";
      hint_pl = "wiele odrzuconych ramek jest dużo słabszych niż poprawne; odbiornik prawdopodobnie słyszy dużo dalekich lub nakładających się liczników sąsiadów.";
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92 && (ok == 0 || avg_ok_rssi <= -86)) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI and weak valid packets; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI i słabych poprawnych pakietach; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely collisions, interference, or bit errors";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe kolizje, zakłócenia lub błędy bitów";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (is_sx1276 &&
               drop_pct >= 20 &&
               (this->diag_60min_rx_path_.preamble_read_failed >= 20 ||
                this->diag_60min_rx_path_.payload_size_unknown >= 10)) {
      hint_code = "SX1276_RX_NOISY";
      hint_en = "SX1276 sees many false starts or undecodable starts; RX frontend likely needs tuning";
      hint_pl = "SX1276 ma dużo fałszywych startów lub nieustalonych początków ramek; ścieżka RX wymaga strojenia";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }

  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\"," 
           "\"interval_s\":%u,"
           "\"uptime_ms\":%lu,"
           "\"listen_mode\":\"%s\","
           "\"total\":%u,"
           "\"ok\":%u,"
           "\"truncated\":%u,"
           "\"dropped\":%u,"
           "\"crc_failed\":%u,"
           "\"crc_fail_pct\":%u,"
           "\"drop_pct\":%u,"
           "\"trunc_pct\":%u,"
           "\"avg_ok_rssi\":%d,"
           "\"avg_drop_rssi\":%d,"
           "\"t1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d,"
             "\"sym_total\":%u,\"sym_invalid\":%u,\"sym_invalid_pct\":%u"
           "},"
           "\"c1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d"
           "},"
           "\"dropped_by_reason\":{"
             "\"too_short\":%u,"
             "\"decode_failed\":%u,"
             "\"dll_crc_failed\":%u,"
             "\"unknown_preamble\":%u,"
             "\"l_field_invalid\":%u,"
             "\"unknown_link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"dropped_by_stage\":{"
             "\"precheck\":%u,"
             "\"t1_decode3of6\":%u,"
             "\"t1_l_field\":%u,"
             "\"t1_length_check\":%u,"
             "\"c1_precheck\":%u,"
             "\"c1_preamble\":%u,"
             "\"c1_suffix\":%u,"
             "\"c1_l_field\":%u,"
             "\"c1_length_check\":%u,"
             "\"dll_crc_first\":%u,"
             "\"dll_crc_mid\":%u,"
             "\"dll_crc_final\":%u,"
             "\"dll_crc_b1\":%u,"
             "\"dll_crc_b2\":%u,"
             "\"link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"rx_path\":{"
             "\"irq_timeout\":%u,"
             "\"preamble_read_failed\":%u,"
             "\"preamble_retry_recovered\":%u,"
             "\"t1_header_read_failed\":%u,"
             "\"payload_size_unknown\":%u,"
             "\"raw_drain_attempted\":%u,"
             "\"raw_drain_recovered\":%u,"
             "\"raw_drain_recovery_pct\":%u,"
             "\"raw_drain_bytes\":%u,"
             "\"payload_read_failed\":%u,"
             "\"queue_send_failed\":%u,"
             "\"fifo_overrun\":%u,"
             "\"weak_start_aborted\":%u,"
             "\"probe_start_aborted\":%u,"
             "\"raw_drain_skipped_weak\":%u,"
             "\"false_start_like\":%u,"
             "\"probe_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u},"
             "\"weak_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u}"
           "},"
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\""
           "}",
           (unsigned) interval_s,
           (unsigned long) now_ms,
           listen_mode,
           (unsigned) total,
           (unsigned) this->diag_60min_ok_,
           (unsigned) this->diag_60min_truncated_,
           (unsigned) this->diag_60min_dropped_,
           (unsigned) crc_failed,
           (unsigned) crc_fail_pct,
           (unsigned) drop_pct,
           (unsigned) trunc_pct,
           (int) avg_ok_rssi,
           (int) avg_drop_rssi,
           (unsigned) t1_total,
           (unsigned) t1_ok,
           (unsigned) t1_drop,
           (unsigned) t1_per_pct,
           (unsigned) t1_crc,
           (unsigned) t1_crc_pct,
           (int) t1_avg_ok_rssi,
           (int) t1_avg_drop_rssi,
           (unsigned) t1_sym_total,
           (unsigned) t1_sym_invalid,
           (unsigned) t1_sym_invalid_pct,
           (unsigned) c1_total,
           (unsigned) c1_ok,
           (unsigned) c1_drop,
           (unsigned) c1_per_pct,
           (unsigned) c1_crc,
           (unsigned) c1_crc_pct,
           (int) c1_avg_ok_rssi,
           (int) c1_avg_drop_rssi,
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_TOO_SHORT],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_DECODE_FAILED],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_DLL_CRC_FAILED],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_L_FIELD_INVALID],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_OTHER],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_PRECHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_T1_DECODE3OF6],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_T1_L_FIELD],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_T1_LENGTH_CHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_PRECHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_PREAMBLE],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_SUFFIX],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_L_FIELD],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_LENGTH_CHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_FIRST],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_MID],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_FINAL],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_B1],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_B2],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_LINK_MODE],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_OTHER],
           (unsigned) this->diag_60min_rx_path_.irq_timeout,
           (unsigned) this->diag_60min_rx_path_.preamble_read_failed,
           (unsigned) this->diag_60min_rx_path_.preamble_retry_recovered,
           (unsigned) this->diag_60min_rx_path_.t1_header_read_failed,
           (unsigned) this->diag_60min_rx_path_.payload_size_unknown,
           (unsigned) this->diag_60min_rx_path_.raw_drain_attempted,
           (unsigned) this->diag_60min_rx_path_.raw_drain_recovered,
           (unsigned) raw_drain_recovery_pct,
           (unsigned) this->diag_60min_rx_path_.raw_drain_bytes,
           (unsigned) this->diag_60min_rx_path_.payload_read_failed,
           (unsigned) this->diag_60min_rx_path_.queue_send_failed,
           (unsigned) this->diag_60min_rx_path_.fifo_overrun,
           (unsigned) this->diag_60min_rx_path_.weak_start_aborted,
           (unsigned) this->diag_60min_rx_path_.probe_start_aborted,
           (unsigned) this->diag_60min_rx_path_.raw_drain_skipped_weak,
           (unsigned) rx_false_start_like,
           // probe_abort_rssi buckets
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[0],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[1],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[2],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[3],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[4],
           // weak_abort_rssi buckets
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[0],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[1],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[2],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[3],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[4],
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl);

  const std::string summary_topic = this->diag_summary_60min_topic_();
  mqtt->publish(summary_topic, payload);
  ESP_LOGI(TAG, "DIAG 60min summary / podsumowanie 60min diag: topic=%s interval=%us uptime_ms=%lu listen_mode=%s total=%u ok=%u truncated=%u dropped=%u crc_failed=%u",
           summary_topic.c_str(), (unsigned) interval_s, (unsigned long) now_ms, listen_mode,
           (unsigned) total, (unsigned) this->diag_60min_ok_,
           (unsigned) this->diag_60min_truncated_, (unsigned) this->diag_60min_dropped_, (unsigned) crc_failed);

  if (std::strcmp(hint_code, "OK") == 0 || std::strcmp(hint_code, "GOOD") == 0) {
    ESP_LOGI(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  } else {
    ESP_LOGW(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  }

  // Publish snapshot of all highlight meters alongside this summary (read-only, no window reset).
  if (this->diag_publish_summary_highlight_meters_ && !this->highlight_meter_stats_.empty()) {
    for (auto &kv : this->highlight_meter_stats_) {
      const uint64_t key = kv.first; // uint64_t — meter_id can exceed uint32_t range when shifted
      MeterStats &st = kv.second;
      const uint32_t meter_id = key >> 8;
      const uint8_t mode_byte = key & 0xFF;
      char id_str[12];
      snprintf(id_str, sizeof(id_str), "%08" PRIu32, meter_id);
      const char *mode_str = (mode_byte == (uint8_t) LinkMode::C1) ? "C1" : ((mode_byte == (uint8_t) LinkMode::S1) ? "S1" : "T1");
      const uint32_t st_elapsed_s = elapsed / 1000U;
      this->publish_meter_window_for_("summary_60min", st_elapsed_s, id_str, mode_str, st,
                                      st.count_window_60min, st.rssi_sum_window_60min,
                                      st.rssi_n_window_60min, st.interval_sum_window_60min_ms,
                                      st.interval_n_window_60min,
                                      false, false);
    }
    // Publish batch BEFORE resetting 60min counters — batch reads the same fields.
    this->publish_meter_window_batch_("summary_60min", elapsed / 1000U, now_ms);
    // Reset 60min window after publish — only here, never in 15min summary.
    for (auto &kv2 : this->highlight_meter_stats_) {
      MeterStats &st2 = kv2.second;
      st2.count_window_60min = 0;
      st2.rssi_sum_window_60min = 0;
      st2.rssi_n_window_60min = 0;
      st2.interval_sum_window_60min_ms = 0;
      st2.interval_n_window_60min = 0;
    }
  }

  this->diag_60min_total_ = 0;
  this->diag_60min_ok_ = 0;
  this->diag_60min_truncated_ = 0;
  this->diag_60min_dropped_ = 0;
  this->diag_60min_dropped_by_bucket_.fill(0);
  this->diag_60min_dropped_by_stage_.fill(0);
  this->diag_60min_rssi_ok_sum_ = 0;
  this->diag_60min_rssi_ok_n_ = 0;
  this->diag_60min_rssi_drop_sum_ = 0;
  this->diag_60min_rssi_drop_n_ = 0;
  this->diag_60min_mode_total_.fill(0);
  this->diag_60min_mode_ok_.fill(0);
  this->diag_60min_mode_dropped_.fill(0);
  this->diag_60min_mode_crc_failed_.fill(0);
  this->diag_60min_mode_rssi_ok_sum_.fill(0);
  this->diag_60min_mode_rssi_ok_n_.fill(0);
  this->diag_60min_mode_rssi_drop_sum_.fill(0);
  this->diag_60min_mode_rssi_drop_n_.fill(0);
  this->diag_60min_t1_symbols_total_ = 0;
  this->diag_60min_t1_symbols_invalid_ = 0;
  this->diag_60min_rx_path_ = {};
}

}  // namespace wmbus_radio
}  // namespace esphome
