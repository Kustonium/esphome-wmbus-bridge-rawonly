// SPDX-License-Identifier: GPL-3.0-or-later
//
// RF runtime heuristics for the wmbus_radio component: busy-ether adaptive
// state, weak/partial-start and T1-probe abort decisions, raw-drain gating and
// the related RX diagnostics collection. Split out of component.cpp unchanged
// (move-only refactor); behaviour, timings and thresholds are identical.

#include "component.h"
#include "wmbus_radio_internal.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

// Optional: publish busy-ether transition events via ESPHome MQTT if present.
#include "esphome/components/mqtt/mqtt_client.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "wmbus";

static bool radio_supports_unknown_size_raw_drain_(const RadioTransceiver *radio) {
  return radio != nullptr && radio->supports_unknown_size_raw_drain();
}

static bool radio_supports_weak_partial_start_abort_(const RadioTransceiver *radio) {
  return radio != nullptr && radio->supports_weak_partial_start_abort();
}

void Radio::collect_radio_rx_diag_() {
  if (this->radio == nullptr) return;
  const uint32_t fifo_overrun = this->radio->take_fifo_overrun_count();
  this->diag_rx_path_.fifo_overrun += fifo_overrun;
  this->diag_15m_rx_path_.fifo_overrun += fifo_overrun;
  this->diag_60min_rx_path_.fifo_overrun += fifo_overrun;
}

uint32_t Radio::current_false_start_like_() const {
  return this->diag_rx_path_.preamble_read_failed +
         this->diag_rx_path_.payload_size_unknown +
         this->diag_rx_path_.weak_start_aborted +
         this->diag_rx_path_.probe_start_aborted +
         this->diag_rx_path_.raw_drain_skipped_weak;
}

bool Radio::sx1276_busy_ether_aggressive_now_() const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL) return false;
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE) return true;
  // ADAPTIVE: hold state is evaluated once per diagnostic summary window (evaluate_busy_ether_adaptive_).
  // millis() is a simple hardware timer read, safe from any context.
  return millis() < this->busy_ether_active_until_ms_;
}

bool Radio::sx1276_busy_ether_severe_now_() const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  const uint32_t false_start_like = this->current_false_start_like_();
  const uint32_t drop_pct_window = (this->diag_total_ > 0 && this->diag_total_ > this->diag_ok_)
      ? (((this->diag_total_ - this->diag_ok_) * 100U) / this->diag_total_) : 0U;
  const uint32_t t1_sym_invalid_pct = (this->diag_t1_symbols_total_ > 0)
      ? ((this->diag_t1_symbols_invalid_ * 100U) / this->diag_t1_symbols_total_) : 0U;

  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE) {
    // Raised thresholds: previous values (20/10/15) fired almost immediately in a typical
    // wMBus building due to normal transmission collisions, making AGGRESSIVE + SEVERE the
    // permanent state and killing distant-but-valid meters via RSSI threshold escalation.
    return false_start_like >= 60 || this->diag_rx_path_.preamble_read_failed >= 30 || drop_pct_window >= 25;
  }
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL) return false;

  if (false_start_like >= 140) return true;
  if (this->diag_rx_path_.preamble_read_failed >= 40 && this->diag_rx_path_.probe_start_aborted >= 40) return true;
  if (drop_pct_window >= 30) return true;
  if (t1_sym_invalid_pct >= 8 && false_start_like >= 30) return true;
  return false;
}

// Evaluates whether the ether is busy enough to activate aggressive filtering.
// Called once per diagnostic summary window, BEFORE the windowed counters are reset,
// so all current-window accumulations are visible.
// If conditions are met the adaptive hold timer is extended by 5 minutes; logging is
// emitted on state transitions so the user can observe adaptive behaviour via serial/MQTT.
void Radio::evaluate_busy_ether_adaptive_(uint32_t now_ms) {
  if (this->sx1276_busy_ether_mode_ != SX1276BusyEtherMode::ADAPTIVE) return;
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return;

  const char *chip = (this->radio != nullptr) ? this->radio->get_name() : "unknown";
  const uint32_t fsl = this->current_false_start_like_();
  const uint32_t drop_pct = (this->diag_total_ > 0 && this->diag_total_ > this->diag_ok_)
      ? (((this->diag_total_ - this->diag_ok_) * 100U) / this->diag_total_) : 0U;
  const uint32_t t1_sym_inv_pct = (this->diag_t1_symbols_total_ > 0)
      ? ((this->diag_t1_symbols_invalid_ * 100U) / this->diag_t1_symbols_total_) : 0U;

  const bool trigger =
      (this->diag_rx_path_.fifo_overrun > 0) ||
      // fsl alone is not enough — high fsl with low drop_pct means RF background noise
      // that does not actually harm reception. Require drop_pct >= 10 to confirm real collisions.
      (fsl >= 80 && drop_pct >= 10) ||
      (this->diag_rx_path_.preamble_read_failed >= 25 && this->diag_rx_path_.probe_start_aborted >= 20 && drop_pct >= 10) ||
      (drop_pct >= 20 && fsl >= 30) ||
      (t1_sym_inv_pct >= 5 && fsl >= 20 && drop_pct >= 10);

  // was_active tracks the last known state via busy_ether_was_active_ (persistent bool).
  // is_active_now reflects the current timer state.
  // These must be separate — was_active=true means we were active last window,
  // is_active_now=false means the hold has since expired. Using the timer for both
  // caused a dead code path where the passive transition event never fired.
  const bool was_active = this->busy_ether_was_active_;
  const bool is_active_now = (now_ms < this->busy_ether_active_until_ms_);

  if (trigger) {
    this->busy_ether_active_until_ms_ = now_ms + 300000; // 5-minute hold
    if (!was_active) {
      ESP_LOGW(TAG, "BusyEther [ADAPTIVE]: noisy window detected / wykryto zaszumione okno — activating / aktywacja na 5 min "
               "(fsl=%" PRIu32 " drop_pct=%" PRIu32 " t1_sym_inv_pct=%" PRIu32
               " preamble_fail=%" PRIu32 " probe_abort=%" PRIu32 " fifo_overrun=%" PRIu32 ")",
               fsl, drop_pct, t1_sym_inv_pct,
               this->diag_rx_path_.preamble_read_failed,
               this->diag_rx_path_.probe_start_aborted,
               this->diag_rx_path_.fifo_overrun);
      // Publish busy_ether_changed event: passive -> active
      if (!this->diag_topic_.empty()) {
        auto *mqtt = esphome::mqtt::global_mqtt_client;
        if (mqtt != nullptr && mqtt->is_connected()) {
          char ev[256];
          snprintf(ev, sizeof(ev),
                   "{\"event\":\"busy_ether_changed\",\"chip\":\"%s\","
                   "\"state\":\"adaptive_active\","
                   "\"fsl\":%" PRIu32 ",\"drop_pct\":%" PRIu32 "}",
                   chip, fsl, drop_pct);
          mqtt->publish(this->diag_topic_ + "/busy_ether_changed", std::string(ev), static_cast<uint8_t>(0), false);
        }
      }
      this->busy_ether_was_active_ = true;
    } else {
      ESP_LOGI(TAG, "BusyEther [ADAPTIVE]: hold extended / przedluzono hold (fsl=%" PRIu32 " drop_pct=%" PRIu32 ")", fsl, drop_pct);
    }
  } else if (was_active && !is_active_now) {
    // Hold has expired and no new trigger — transition to passive.
    ESP_LOGI(TAG, "BusyEther [ADAPTIVE]: hold expired / hold wygasl, returning to passive mode / powrot do trybu pasywnego (fsl=%" PRIu32 " drop_pct=%" PRIu32 ")", fsl, drop_pct);
    // Publish busy_ether_changed event: active -> passive
    if (!this->diag_topic_.empty()) {
      auto *mqtt = esphome::mqtt::global_mqtt_client;
      if (mqtt != nullptr && mqtt->is_connected()) {
        char ev[256];
        snprintf(ev, sizeof(ev),
                 "{\"event\":\"busy_ether_changed\",\"chip\":\"%s\","
                 "\"state\":\"adaptive_passive\","
                 "\"fsl\":%" PRIu32 ",\"drop_pct\":%" PRIu32 "}",
                 chip, fsl, drop_pct);
        mqtt->publish(this->diag_topic_ + "/busy_ether_changed", std::string(ev), static_cast<uint8_t>(0), false);
      }
    }
    this->busy_ether_was_active_ = false;
  }
  // else: was_active && is_active_now && !trigger — still in hold, quiet window, don't extend, don't log.
}

bool Radio::should_abort_weak_partial_start_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  if (is_c_mode) return false;
  if (rssi_dbm <= -126 || rssi_dbm >= 0) return false;

  const uint32_t false_start_like = this->current_false_start_like_();
  int32_t recent_ok = this->recent_ok_rssi_valid_ ? this->recent_ok_rssi_avg_ : -80;
  int32_t threshold = recent_ok - 10;
  if (false_start_like >= 40 || this->diag_rx_path_.fifo_overrun > 0) threshold += 2;
  if (false_start_like >= 100) threshold += 2;
  if (this->sx1276_busy_ether_aggressive_now_()) threshold += 3;
  if (this->sx1276_busy_ether_severe_now_()) threshold += 2;
  // Clamp upper bound at -88 dBm: wMBus meters in a building routinely transmit at
  // -80..-90 dBm. The previous -78 limit killed distant-but-valid meters even without severe.
  threshold = std::clamp<int32_t>(threshold, -96, -88);
  if (rssi_dbm > threshold) return false;

  size_t max_partial = WMBUS_T1_LEN_PROBE_BYTES + 8;
  if (this->sx1276_busy_ether_aggressive_now_()) max_partial = WMBUS_T1_LEN_PROBE_BYTES + 4;
  if (this->sx1276_busy_ether_severe_now_()) max_partial = WMBUS_T1_LEN_PROBE_BYTES + 2;

  // Only for clearly partial / short starts. Once we already have a long body, keep the current path.
  if (bytes_read > max_partial) return false;
  return true;
}

bool Radio::should_abort_t1_probe_start_(int rssi_dbm) const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  if (rssi_dbm <= -126 || rssi_dbm >= 0) return false;

  const uint32_t false_start_like = this->current_false_start_like_();
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL &&
      false_start_like < 20 && this->diag_rx_path_.fifo_overrun == 0) return false;

  int32_t recent_ok = this->recent_ok_rssi_valid_ ? this->recent_ok_rssi_avg_ : -80;
  int32_t threshold = recent_ok - 12;
  if (false_start_like >= 60 || this->diag_rx_path_.fifo_overrun > 0) threshold += 2;
  if (false_start_like >= 120) threshold += 2;
  if (this->sx1276_busy_ether_aggressive_now_()) threshold += 4;
  if (this->sx1276_busy_ether_severe_now_()) threshold += 3;
  // Clamp upper bound at -86 dBm: the previous -76 limit aborted T1 probe starts for
  // any meter weaker than -76 dBm once AGGRESSIVE+SEVERE was active (which was almost always).
  threshold = std::clamp<int32_t>(threshold, -96, -86);
  return rssi_dbm <= threshold;
}

bool Radio::should_attempt_raw_drain_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const {
  if (!radio_supports_unknown_size_raw_drain_(this->radio)) return false;
  if (bytes_read == 0 || bytes_read >= WMBUS_RAW_DRAIN_MAX_BYTES) return false;
  if (is_c_mode) return true;

  if (this->should_abort_weak_partial_start_(rssi_dbm, bytes_read, is_c_mode)) return false;
  if (bytes_read <= (WMBUS_T1_LEN_PROBE_BYTES + 4) && this->should_abort_t1_probe_start_(rssi_dbm)) return false;

  const int32_t recent_ok = this->recent_ok_rssi_valid_ ? this->recent_ok_rssi_avg_ : -80;
  if (this->sx1276_busy_ether_severe_now_()) {
    if (bytes_read <= (WMBUS_T1_LEN_PROBE_BYTES + 16)) return false;
    if (rssi_dbm <= (recent_ok - 8)) return false;
  } else if (this->sx1276_busy_ether_aggressive_now_()) {
    if (bytes_read <= (WMBUS_T1_LEN_PROBE_BYTES + 10) && rssi_dbm <= (recent_ok - 6)) return false;
  }

  return true;
}

}  // namespace wmbus_radio
}  // namespace esphome
