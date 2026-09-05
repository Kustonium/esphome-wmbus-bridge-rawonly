// SPDX-License-Identifier: GPL-3.0-or-later
#include "component.h"
#include "meter_filter.h"

#include "freertos/queue.h"
#include "freertos/task.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <esp_heap_caps.h>  // heap_caps_get_free_size() for the outbox boot log / auto-sizing seed

#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <cinttypes>

// Optional: publish diagnostics via ESPHome MQTT if mqtt component is present.
#include "esphome/components/mqtt/mqtt_client.h"

// Protocol constants shared with the split-out translation units (rf_runtime, ...).
#include "wmbus_radio_internal.h"

// xQueueCreate returns a handle (a pointer), xTaskCreate returns BaseType_t.
// Funnel both through one overload set so a single format specifier stays
// correct for either: printing a handle with %d is what warned on arduino.
static inline intptr_t assert_result_value(const void *value) {
  return reinterpret_cast<intptr_t>(value);
}
static inline intptr_t assert_result_value(intptr_t value) { return value; }

#define ASSERT(expr, expected, before_exit)                                    \
  {                                                                            \
    auto result = (expr);                                                      \
    if (!!result != expected) {                                                \
      ESP_LOGE(TAG, "Assertion failed: %s -> %" PRIdPTR, #expr,                \
               assert_result_value(result));                                   \
      before_exit;                                                             \
      return;                                                                  \
    }                                                                          \
  }

#define ASSERT_SETUP(expr) ASSERT(expr, 1, this->mark_failed())

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "wmbus";

// Logging policy for this component:
// - important user-facing INFO/WARN/ERROR messages may be concise bilingual EN/PL,
// - DEBUG/VERBOSE stays English to keep issue reports and grep output usable,
// - YAML/MQTT identifiers remain English-only because they form the stable technical API.

// Splits the YAML meter list into the two forms a meter can be addressed by.
//
// A plain decimal token is a BCD-decoded meter ID and lands in `out_bcd`. A
// token with a 0x prefix, or any token containing a-f/A-F, is the raw A-field
// value as printed in the log (id:XXXXXXXX) and lands in `out_raw`.
//
// The split is unambiguous: a non-BCD A-field must contain a nibble > 9, so its
// printed form always carries a hex letter, while a BCD ID never does. Writing
// a BCD meter in its 0x form works too - it simply matches on the raw list.
//
// `option` only names the YAML key in the warning below, so a bad token is
// reported against the option the user actually wrote.
static void parse_meter_id_csv_(const std::string &csv, std::vector<uint32_t> &out_bcd,
                                std::vector<uint32_t> &out_raw, const char *option) {
  out_bcd.clear();
  out_raw.clear();
  if (csv.empty()) return;
  size_t i = 0;
  while (i < csv.size()) {
    // skip separators/whitespace
    while (i < csv.size() && (csv[i] == ',' || csv[i] == ';' || csv[i] == ' ' || csv[i] == '\t' || csv[i] == '\n' || csv[i] == '\r')) i++;
    if (i >= csv.size()) break;
    size_t j = i;
    while (j < csv.size() && csv[j] != ',' && csv[j] != ';' && csv[j] != ' ' && csv[j] != '\t' && csv[j] != '\n' && csv[j] != '\r') j++;
    std::string tok = csv.substr(i, j - i);
    // trim
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
    if (!tok.empty()) {
      const char *s = tok.c_str();
      int base = 10;
      if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        // Explicit hex prefix: 0x417f0666
        s += 2;
        base = 16;
      } else {
        // Auto-detect bare hex: if the token contains a-f/A-F it must be hex
        bool has_hex_digit = false;
        for (char c : tok) {
          if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            has_hex_digit = true;
            break;
          }
        }
        if (has_hex_digit) {
          base = 16;
        }
      }
      char *endp = nullptr;
      unsigned long v = std::strtoul(s, &endp, base);
      if (endp != s && *endp == '\0') {
        (base == 16 ? out_raw : out_bcd).push_back((uint32_t) v);
      } else {
        // Token did not parse cleanly — warn the user
        ESP_LOGW("wmbus", "%s: could not parse meter ID '%s' — use the value the log prints as id:, either decimal (e.g. 12345678) or hex (e.g. 0x417F0666)", option, tok.c_str());
      }
    }
    i = j;
  }
  for (auto *out : {&out_bcd, &out_raw}) {
    if (!out->empty()) {
      std::sort(out->begin(), out->end());
      out->erase(std::unique(out->begin(), out->end()), out->end());
    }
  }
}


// Splits buffer_priority YAML ("<id>:<weight>,<id>:<weight>,...") into a
// weights map keyed by meter_bucket_key(). Each <id> token uses exactly the
// same BCD-vs-raw parsing rules as parse_meter_id_csv_ above (decimal = BCD,
// 0x-prefixed or containing a-f = raw A-field), so an entry here is written
// exactly like the id already used in forward_meters/highlight_meters. A
// meter present in forward_meters but absent here simply gets no entry -
// enqueue_or_publish_/recompute_buffer_quotas_ (mqtt_outbox.cpp) treat a
// missing entry as weight 1, so an unset buffer_priority means an equal
// split, not "no buffer".
static void parse_meter_priority_csv_(const std::string &csv,
                                       std::unordered_map<uint64_t, uint32_t> &out_weights) {
  out_weights.clear();
  if (csv.empty()) return;
  size_t i = 0;
  while (i < csv.size()) {
    while (i < csv.size() && (csv[i] == ',' || csv[i] == ';' || csv[i] == ' ' || csv[i] == '\t' || csv[i] == '\n' || csv[i] == '\r')) i++;
    if (i >= csv.size()) break;
    size_t j = i;
    while (j < csv.size() && csv[j] != ',' && csv[j] != ';' && csv[j] != '\t' && csv[j] != '\n' && csv[j] != '\r') j++;
    std::string tok = csv.substr(i, j - i);
    i = j;
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
    if (tok.empty()) continue;

    const size_t colon = tok.find(':');
    if (colon == std::string::npos) {
      ESP_LOGW("wmbus", "buffer_priority: malformed entry '%s' (expected <id>:<weight>), ignoring / "
                        "bledny wpis '%s' (oczekiwano <id>:<waga>), ignorowanie",
               tok.c_str(), tok.c_str());
      continue;
    }
    const std::string id_tok = tok.substr(0, colon);
    const std::string w_tok = tok.substr(colon + 1);

    std::vector<uint32_t> bcd, raw;
    parse_meter_id_csv_(id_tok, bcd, raw, "buffer_priority");
    if (bcd.empty() && raw.empty()) continue;  // parse_meter_id_csv_ already warned about the id itself

    char *endp = nullptr;
    const unsigned long w = std::strtoul(w_tok.c_str(), &endp, 10);
    if (endp == w_tok.c_str() || *endp != '\0' || w == 0) {
      ESP_LOGW("wmbus", "buffer_priority: invalid weight '%s' for '%s' (must be a positive integer), ignoring / "
                        "nieprawidlowa waga '%s' dla '%s' (wymagana dodatnia liczba calkowita), ignorowanie",
               w_tok.c_str(), id_tok.c_str(), w_tok.c_str(), id_tok.c_str());
      continue;
    }

    const uint64_t key = !bcd.empty() ? meter_bucket_key(bcd.front(), 0) : meter_bucket_key(0, raw.front());
    out_weights[key] = (uint32_t) w;
  }
}

std::string Radio::forward_whitelist_summary_() const {
  const size_t bcd_n = this->forward_meter_ids_.size();
  const size_t raw_n = this->forward_meter_raw_ids_.size();
  if (bcd_n == 0 && raw_n == 0)
    return "disabled - every decoded frame is published / wylaczona - publikowana jest kazda ramka";

  // List the parsed IDs, not just the count: a mistyped entry is only obvious
  // when the resulting values are visible. Each form is printed the way the log
  // prints that meter's id:, so a config entry can be compared by eye. Cap the
  // list so a long one cannot overrun the log buffer.
  const size_t log_limit = 16;
  size_t budget = log_limit;
  std::string ids;

  for (size_t i = 0; i < bcd_n && budget > 0; i++, budget--) {
    // Zero-padded to eight digits so an entry reads exactly like the id: field
    // of the reception log and the two can be compared at a glance. The buffer
    // is deliberately wider than eight digits: %08u pads but never truncates,
    // so a value too large to be a BCD ID still prints in full and stays
    // visible as the mistake it is.
    char buf[12];
    snprintf(buf, sizeof(buf), "%08u", (unsigned) this->forward_meter_ids_[i]);
    if (!ids.empty()) ids += ", ";
    ids += buf;
  }
  for (size_t i = 0; i < raw_n && budget > 0; i++, budget--) {
    char buf[13];
    snprintf(buf, sizeof(buf), "0x%08X", (unsigned) this->forward_meter_raw_ids_[i]);
    if (!ids.empty()) ids += ", ";
    ids += buf;
  }
  if (bcd_n + raw_n > log_limit)
    ids += ", ... (+" + std::to_string(bcd_n + raw_n - log_limit) + ")";

  return std::to_string(bcd_n + raw_n) + " ids" +
         (this->forward_meters_inherited_ ? " (from highlight_meters)" : "") + ": " + ids;
}

static bool radio_supports_preamble_retry_(const RadioTransceiver *radio) {
  return radio != nullptr && radio->supports_preamble_retry();
}

// True when an RSSI reading is an actual measurement. The transceivers publish
// -127 dBm as "not measured for this frame" (restart_rx() / drain_fifo_once_()
// in transceiver_sx1276.cpp, restart_rx() in transceiver_sx1262.cpp). Such a
// sample is not a signal level, so it must never enter an average or an EMA;
// rf_runtime.cpp applies the same <= -126 test before acting on an RSSI.
static inline bool rssi_is_measured_(int rssi_dbm) { return rssi_dbm > -126; }

// Returns the RSSI bucket index for abort RSSI distributions.
// [0] > -70   [1] -70..-79   [2] -80..-89   [3] -90..-99   [4] <= -100
static inline uint8_t rssi_abort_bucket_(int rssi_dbm) {
  if (rssi_dbm > -70)  return 0;
  if (rssi_dbm > -80)  return 1;
  if (rssi_dbm > -90)  return 2;
  if (rssi_dbm > -100) return 3;
  return 4;
}

void Radio::setup() {
  // One random identifier per boot separates sequence-number domains. It is
  // metadata only; no radio setting or packet path depends on its value.
  this->rx_boot_id_ = esp_random();
  if (this->rx_boot_id_ == 0) this->rx_boot_id_ = 1;
  for (const auto &warning : this->config_warnings_) {
    ESP_LOGW(TAG, "Config warning / ostrzezenie konfiguracji: %s", warning.c_str());
  }
  // Parse optional highlight meter list (CSV provided by python/YAML).
  parse_meter_id_csv_(this->highlight_meters_csv_, this->highlight_meter_ids_,
                      this->highlight_meter_raw_ids_, "highlight_meters");
  parse_meter_id_csv_(this->forward_meters_csv_, this->forward_meter_ids_,
                      this->forward_meter_raw_ids_, "forward_meters");
  if (!this->target_meter_id_str_.empty()) {
    std::vector<uint32_t> tmp, tmp_raw;
    parse_meter_id_csv_(this->target_meter_id_str_, tmp, tmp_raw, "target_meter_id");
    if (tmp.empty() && !tmp_raw.empty()) {
      // Would otherwise be accepted and then never match anything, because the
      // target is compared against the BCD-decoded ID only.
      ESP_LOGW(TAG, "target_meter_id: non-BCD (hex) meter IDs are not supported here - use forward_meters for such meters / "
                    "szesnastkowe ID nie sa tu obslugiwane - dla takich licznikow uzyj forward_meters");
    }
    if (!tmp.empty()) {
      this->target_meter_id_ = tmp.front();
      this->target_meter_enabled_ = true;
      if (this->target_topic_.empty()) {
        this->target_topic_ = this->derived_target_topic_();
      }
      char id_buf[9];
      snprintf(id_buf, sizeof(id_buf), "%08u", (unsigned) this->target_meter_id_);
      ESP_LOGI(TAG, "Target meter forwarding enabled / wlaczono przekazywanie docelowego licznika id=%s topic=%s",
               id_buf, this->target_topic_.empty() ? "<derived at runtime>" : this->target_topic_.c_str());
    }
  }

  if (!this->telegram_topic_.empty()) {
    ESP_LOGI(TAG, "Frame RAW forwarding topic / topic publikacji RAW: %s", this->telegram_topic_.c_str());
    ESP_LOGI(TAG, "Frame RX metadata topic / topic metadanych RX: %s", this->rx_topic_.c_str());
  }
  if (this->mqtt_outbox_auto_) {
    // Seed an initial estimate right away rather than waiting for the first
    // periodic re-check (~30s, see maybe_reautosize_outbox_ in loop()). Free
    // heap at this point in setup() is only a rough first read - WiFi/
    // Ethernet/MQTT/TLS have not necessarily finished claiming their share
    // yet - so this gets refined automatically once things settle.
    const size_t suggested = this->suggested_mqtt_outbox_capacity_();
    this->set_mqtt_outbox_max_capacity(suggested);
    this->set_mqtt_outbox_capacity(suggested);
  }
  {
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const bool store_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    ESP_LOGI(TAG,
             "MQTT outbox / bufor MQTT: capacity=%u (max=%u, %s, store=%s) free_heap=%u B free_psram=%u B / "
             "pojemnosc=%u (maksimum=%u, %s, storage=%s) wolny_heap=%u B wolny_psram=%u B",
             (unsigned) this->mqtt_outbox_capacity_, (unsigned) this->mqtt_outbox_max_capacity_,
             this->mqtt_outbox_auto_ ? "auto" : "fixed", store_psram ? "PSRAM" : "internal",
             (unsigned) free_internal, (unsigned) free_psram,
             (unsigned) this->mqtt_outbox_capacity_, (unsigned) this->mqtt_outbox_max_capacity_,
             this->mqtt_outbox_auto_ ? "auto" : "fixed", store_psram ? "PSRAM" : "internal",
             (unsigned) free_internal, (unsigned) free_psram);
  }
  // Push the compiled starting capacity to the optional number entity (if the
  // user declared one under number:) so a web_server/HA panel shows the real
  // value from the first load, not 0 before any interaction.
  this->publish_initial_buffer_state_();

  // buffer_priority (per-meter RAM buffer weights): parsed here, after
  // forward_meters above and after mqtt_outbox_capacity_ has its final
  // boot-time value (fixed, or the auto estimate seeded a moment ago), and
  // only meaningful once forward_meters is a non-empty whitelist - see
  // meter_filter.h / mqtt_outbox.cpp for why plain integer weights (not
  // percentages) are used, and how they become quotas that always sum to
  // exactly the current capacity regardless of what the weights are.
  parse_meter_priority_csv_(this->buffer_priority_csv_, this->buffer_priority_weights_);
  if (!this->buffer_priority_weights_.empty() &&
      this->forward_meter_ids_.empty() && this->forward_meter_raw_ids_.empty()) {
    ESP_LOGW(TAG, "buffer_priority is set but forward_meters is empty: nothing to prioritise, ignoring / "
                  "ustawiono buffer_priority bez forward_meters: brak whitelisty do priorytetyzacji, ignorowanie");
  }

  // First real per-meter quota computation. Must happen only now: any earlier
  // (e.g. from the capacity setters themselves) and it would see an empty
  // whitelist regardless of what YAML actually configured - see setup_done_'s
  // comment in component.h. From here on the setters recompute live on their
  // own (auto re-sizing every ~30s, or a runtime edit via the buffer_capacity
  // number entity).
  this->recompute_buffer_quotas_();
  this->setup_done_ = true;
  if (!this->mqtt_outbox_meter_quotas_.empty()) {
    std::string quota_list;
    for (const auto &mq : this->mqtt_outbox_meter_quotas_) {
      const uint32_t tag = (uint32_t) (mq.key >> 32);
      const uint32_t low = (uint32_t) (mq.key & 0xFFFFFFFFu);
      char id_buf[16];
      if (tag == 1) {
        snprintf(id_buf, sizeof(id_buf), "%08u", (unsigned) low);
      } else {
        snprintf(id_buf, sizeof(id_buf), "0x%08X", (unsigned) low);
      }
      if (!quota_list.empty()) quota_list += ", ";
      quota_list += std::string(id_buf) + ":w" + std::to_string(mq.weight) + "->" + std::to_string(mq.quota);
    }
    ESP_LOGI(TAG, "MQTT outbox per-meter quotas (id:weight->slots) / bufor MQTT wg licznika (id:waga->miejsca): %s",
             quota_list.c_str());
  }

  if (!this->forward_meter_ids_.empty() || !this->forward_meter_raw_ids_.empty()) {
    ESP_LOGI(TAG, "Forward whitelist / whitelista przekazywania: %s",
             this->forward_whitelist_summary_().c_str());
  }

  if (this->publish_radio_raw_) {
    ESP_LOGI(TAG, "Internal radio RAW tap enabled / wlaczono wewnetrzny RAW tap: wmbus_bridge/raw");
  }

  if (this->publish_rssi_) {
    ESP_LOGI(TAG, "Per-meter RSSI publishing enabled / wlaczono publikacje RSSI per licznik: %s/<meter_id>",
             this->rssi_topic_.c_str());
  }

  // Both lists, or a config listing only non-BCD meters would skip the window
  // clamp below and log nothing.
  if (!this->highlight_meter_ids_.empty() || !this->highlight_meter_raw_ids_.empty()) {
    // meter_window_interval_ms_ defaults to 15 min; cap it at diag_summary_interval_ms_ minimum
    if (this->meter_window_interval_ms_ < this->diag_summary_interval_ms_)
      this->meter_window_interval_ms_ = this->diag_summary_interval_ms_;
    ESP_LOGI(TAG, "Highlight meters enabled / wlaczono wyroznione liczniki (%u ids) tag=%s ansi=%s window=%us",
             (unsigned) (this->highlight_meter_ids_.size() + this->highlight_meter_raw_ids_.size()),
             this->highlight_tag_.empty() ? "wmbus_user" : this->highlight_tag_.c_str(),
             this->highlight_ansi_ ? "true" : "false",
             (unsigned) (this->meter_window_interval_ms_ / 1000));
  }

  ASSERT_SETUP(this->packet_queue_ = xQueueCreate(3, sizeof(Packet *)));

  // Push verbose diagnostics into the driver before the receiver task exists,
  // so no frame can be handled while the two disagree about how much to report.
  if (this->radio != nullptr)
    this->radio->set_diag_verbose(this->diag_verbose_);

  // This component uses its own FreeRTOS receiver task instead of ESPHome's
  // main loop task. Because of that, ESPHome's loop_task_stack_size YAML option
  // is not enough here.
  //
  // Why this is configurable: some boards with tighter RAM/headroom may need a
  // larger receiver stack on newer builds with heavier diagnostics, while the
  // default 3 KB is still fine for existing setups. Making it runtime-configured
  // avoids board-specific forks and keeps one shared codebase.
  // Pin to Core 1 on dual-core SoCs to avoid WiFi ISR preemption (Core 0).
  // Priority 24: high enough to preempt WiFi (23) and ensure sub-ms wakeup
  // after IRQ — critical for FIFO-based chips (CC1101: 64B fills in 5ms at 100kbps)
  // and for fast radio re-arm after packet capture.
#if portNUM_PROCESSORS > 1
  ASSERT_SETUP(xTaskCreatePinnedToCore((TaskFunction_t)this->receiver_task, "radio_recv",
                           this->receiver_task_stack_size_, this, 24, &(this->receiver_task_handle_), 1));
#else
  ASSERT_SETUP(xTaskCreate((TaskFunction_t)this->receiver_task, "radio_recv",
                           this->receiver_task_stack_size_, this, 24, &(this->receiver_task_handle_)));
#endif

  ESP_LOGI(TAG, "Receiver task created / utworzono task odbiornika [%p], stack=%u bytes",
           this->receiver_task_handle_, (unsigned) this->receiver_task_stack_size_);

  this->radio->attach_data_interrupt(Radio::wakeup_receiver_task_from_isr,
                                     &(this->receiver_task_handle_));

  // One-shot publication of SX1262 device errors before/after boot clear.
  // This is best-effort; if MQTT isn't ready yet we publish from loop().
  if (this->publish_dev_err_after_clear_ && this->radio != nullptr) {
    uint16_t before = 0, after = 0;
    if (this->radio->get_boot_device_errors(before, after)) {
      this->dev_err_before_ = before;
      this->dev_err_after_ = after;
      this->dev_err_cleared_pending_ = true;
    }
  }

  this->boot_log_done_ = false;
  this->boot_log_last_ms_ = (uint32_t) esphome::millis();
  this->boot_log_count_ = 0;
  this->boot_info_mqtt_pending_ = true;
  this->boot_info_event_pending_ = true;
}

void Radio::dump_config() {
  ESP_LOGCONFIG(TAG, "wM-Bus Radio:");
  if (this->radio == nullptr) {
    ESP_LOGCONFIG(TAG, "  Radio: <null>");
    return;
  }

  ESP_LOGCONFIG(TAG, "  Radio type: %s", this->radio->get_name());
  ESP_LOGCONFIG(TAG, "  Listen mode: %s",
                listen_mode_to_string_(this->radio->get_listen_mode()));
  ESP_LOGCONFIG(TAG, "  Listen mode filter: %s",
                this->listen_mode_filter_after_parse_ ? "after parse (experimental)" : "before parse (legacy)");
  ESP_LOGCONFIG(TAG, "  Receiver task stack: %u bytes", (unsigned) this->receiver_task_stack_size_);
  ESP_LOGCONFIG(TAG, "  Operation: rx (receive only - this component does not transmit)");
  const char *busy_mode = (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL) ? "normal"
                           : (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE) ? "aggressive"
                           : "adaptive";
  if (std::strcmp(this->radio->get_name(), "SX1276") == 0) {
    ESP_LOGCONFIG(TAG, "  SX1276 busy ether mode: %s", busy_mode);
  } else {
    ESP_LOGCONFIG(TAG, "  Busy ether mode: n/a (SX1276 only)");
  }
  ESP_LOGCONFIG(TAG, "  Forward whitelist: %s", this->forward_whitelist_summary_().c_str());
  if (!this->diag_topic_.empty()) {
    ESP_LOGCONFIG(TAG, "  Diagnostics MQTT topic: %s", this->diag_topic_.c_str());
    ESP_LOGCONFIG(TAG, "  MQTT boot topic: %s/boot", this->diag_topic_.c_str());
    ESP_LOGCONFIG(TAG, "  Summary interval: %us -> %s", (unsigned) (this->diag_summary_interval_ms_ / 1000), this->diag_summary_topic_().c_str());
    ESP_LOGCONFIG(TAG, "  15min summary: %s -> %s", this->diag_publish_summary_15min_ ? "enabled (900s)" : "disabled", this->diag_summary_15min_topic_().c_str());
    ESP_LOGCONFIG(TAG, "  60min summary: %s -> %s", this->diag_publish_summary_60min_ ? "enabled (3600s)" : "disabled", this->diag_summary_60min_topic_().c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Diagnostics MQTT publishing: disabled (opt-in)");
  }
}

void Radio::loop() {
  const uint32_t loop_now_ms = (uint32_t) esphome::millis();

  // Report where a frame's RSSI came from. The receive path records this but
  // cannot log it - it runs in the receiver task, whose output only reaches
  // UART, never the API log stream - so it is drained and printed here on the
  // main task. Drivers report the first frame on each receive path and then go
  // quiet, so this is a handful of lines per boot, not per frame.
  if (this->radio != nullptr) {
    // Drain every pending snapshot, not just one: a driver reports once per
    // receive path, and both can be waiting after a single busy stretch.
    RadioTransceiver::RssiDiag rssi_diag{};
    while (this->radio->take_rssi_diag(rssi_diag)) {
      ESP_LOGI(TAG,
               "RSSI source / zrodlo RSSI: %s (path=%s RssiSync=0x%02X RssiAvg=0x%02X inflight=%ddBm) -> %ddBm",
               rssi_diag.source, rssi_diag.path, (unsigned) rssi_diag.raw_sync,
               (unsigned) rssi_diag.raw_avg, (int) rssi_diag.inflight, (int) rssi_diag.result);
      if (rssi_diag.trigger_irq != 0 || rssi_diag.captured != 0) {
        ESP_LOGI(TAG,
                 "%s RX snapshot: IRQ=0x%04X captured=%u exit=%s first[%u]=%02X%02X%02X%02X%02X%02X%02X%02X",
                 this->radio->get_name(), (unsigned) rssi_diag.trigger_irq, (unsigned) rssi_diag.captured,
                 rssi_diag.exit_reason, (unsigned) rssi_diag.first_len,
                 (unsigned) rssi_diag.first_bytes[0], (unsigned) rssi_diag.first_bytes[1],
                 (unsigned) rssi_diag.first_bytes[2], (unsigned) rssi_diag.first_bytes[3],
                 (unsigned) rssi_diag.first_bytes[4], (unsigned) rssi_diag.first_bytes[5],
                 (unsigned) rssi_diag.first_bytes[6], (unsigned) rssi_diag.first_bytes[7]);
      }
    }
  }

if (!this->boot_log_done_ && this->radio != nullptr) {
  if (loop_now_ms - this->boot_log_last_ms_ >= 10000) {
    const char *radio_name = this->radio->get_name();

    if (strcmp(radio_name, "SX1276") == 0) {
      const char *busy_mode = "unknown";
      const char *busy_state = "n/a";

      switch (this->sx1276_busy_ether_mode_) {
        case SX1276BusyEtherMode::NORMAL:
          busy_mode = "normal";
          break;
        case SX1276BusyEtherMode::AGGRESSIVE:
          busy_mode = "aggressive";
          break;
        case SX1276BusyEtherMode::ADAPTIVE:
          busy_mode = "adaptive";
          busy_state = this->busy_ether_was_active_ ? "active" : "passive";
          break;
        default:
          busy_mode = "unknown";
          break;
      }

      ESP_LOGI(TAG,
               "Radio active / radio aktywne: %s | Listen mode / tryb nasluchu: %s | receiver_stack=%u bytes | diagnostic_mode=%s | meter_stats=%s | busy_ether=%s | state=%s | RF: %s",
               radio_name,
               listen_mode_to_string_(this->radio->get_listen_mode()),
               (unsigned) this->receiver_task_stack_size_,
               this->diag_mode_str_.c_str(),
               this->meter_stats_str_.c_str(),
               busy_mode,
               busy_state,
               this->radio->get_rf_params_str().empty() ? "n/a" : this->radio->get_rf_params_str().c_str());

      if (this->sx1276_yaml_sanity_configured_) {
        ESP_LOGI(TAG, "SX1276 YAML sanity / sprawdzenie YAML SX1276:");
        if (this->sx1276_yaml_tcxo_pin_configured_) {
          ESP_LOGI(TAG, "  tcxo_pin: configured -> TCXO enable pin driven HIGH before radio init / pin TCXO ustawiany HIGH przed inicjalizacja radia");
        } else {
          ESP_LOGI(TAG, "  tcxo_pin: not configured -> OK for normal SX1276 boards; LilyGO T3 V3.0 TCXO uses tcxo_pin: GPIO12 / OK dla zwyklych plytek SX1276; LilyGO T3 V3.0 TCXO uzywa tcxo_pin: GPIO12");
        }
      }
      this->radio->log_reg_status();
    } else if (strcmp(radio_name, "SX1262") == 0 && this->sx1262_yaml_sanity_configured_) {
      const bool t1_like = this->radio->get_listen_mode() == LISTEN_MODE_T1 || this->radio->get_listen_mode() == LISTEN_MODE_BOTH;

      ESP_LOGI(TAG,
               "Radio active / radio aktywne: %s | Listen mode / tryb nasluchu: %s | receiver_stack=%u bytes | diagnostic_mode=%s | meter_stats=%s | RF: %s",
               radio_name,
               listen_mode_to_string_(this->radio->get_listen_mode()),
               (unsigned) this->receiver_task_stack_size_,
               this->diag_mode_str_.c_str(),
               this->meter_stats_str_.c_str(),
               this->radio->get_rf_params_str().empty() ? "n/a" : this->radio->get_rf_params_str().c_str());

      ESP_LOGI(TAG, "SX1262 YAML sanity / sprawdzenie YAML SX1262:");

      if (this->sx1262_yaml_has_tcxo_) {
        ESP_LOGI(TAG, "  has_tcxo: true -> TCXO enabled / TCXO wlaczone");
      } else {
        ESP_LOGW(TAG,
                 "  has_tcxo: false -> RISK(!): radio may initialize but receive no frames on TCXO boards, including Heltec V4 / radio moze sie zainicjalizowac, ale nie odbierac ramek na plytkach z TCXO, w tym Heltec V4");
      }

      if (this->sx1262_yaml_dio2_rf_switch_) {
        ESP_LOGI(TAG, "  dio2_rf_switch: true -> DIO2 RF switch enabled / przelacznik RF na DIO2 wlaczony");
      } else {
        ESP_LOGW(TAG,
                 "  dio2_rf_switch: false -> check board wiring; OK only for boards without DIO2 RF switch / sprawdz plytke; OK tylko bez przelacznika RF na DIO2");
      }

      // Which of these two is the warning was inverted on 2026-09-03, because
      // the measurement went the other way than the original guess.
      //
      // `true` costs about 7 dB in weak signal. Measured off/on/off on one
      // board over three windows, with three untouched control boards holding
      // their RSSI percentile to the decibel across all of it. That price is
      // paid on every frame, whether or not a long one ever arrives.
      //
      // `false` costs nothing unless a meter actually sends more than about
      // 150 decoded bytes - a three-phase electricity meter will, a typical
      // water or heat meter will not. So the conditional cost belongs to
      // `false` and the unconditional one to `true`, which is the opposite of
      // how this used to read.
      if (t1_like) {
        if (this->sx1262_yaml_long_gfsk_packets_) {
          ESP_LOGW(TAG,
                   "  long_gfsk_packets: true -> costs ~7 dB in weak signal; worth it only if you receive frames above ~150 decoded bytes / kosztuje ~7 dB przy slabym sygnale; oplaca sie tylko, jesli odbierasz ramki powyzej ~150 bajtow zdekodowanych");
        } else {
          ESP_LOGI(TAG,
                   "  long_gfsk_packets: false -> full sensitivity; frames above ~150 decoded bytes are truncated / pelna czulosc; ramki powyzej ~150 bajtow zdekodowanych sa ucinane");
        }
      } else {
        ESP_LOGI(TAG, "  long_gfsk_packets: %s -> long T1 check not applicable for this listen_mode / kontrola dlugich T1 nie dotyczy tego trybu",
                 this->sx1262_yaml_long_gfsk_packets_ ? "true" : "false");
      }

      ESP_LOGI(TAG, "  rx_gain: %s", this->sx1262_yaml_rx_gain_.c_str());

      // Same failure shape as has_tcxo: the radio initializes, the log looks
      // healthy, and the board is ~30 dB deaf because the module never opens
      // its antenna path. Reported in both states so "not configured" is a
      // positive statement rather than a missing line.
      if (this->sx1262_yaml_rf_sw_pin_) {
        ESP_LOGI(TAG, "  rf_sw_pin: configured -> module RF switch gated by the driver / przelacznik RF modulu sterowany przez sterownik");
      } else {
        ESP_LOGI(TAG,
                 "  rf_sw_pin: not configured -> OK for boards without a module RF switch; REQUIRED on XIAO ESP32-S3 + Wio-SX1262 (GPIO38), otherwise ~30 dB less sensitivity / OK dla plytek bez przelacznika RF w module; WYMAGANE na XIAO ESP32-S3 + Wio-SX1262 (GPIO38), inaczej czulosc nizsza o ~30 dB");
      }
      this->radio->log_reg_status();
    } else {
      ESP_LOGI(TAG,
               "Radio active / radio aktywne: %s | Listen mode / tryb nasluchu: %s | receiver_stack=%u bytes | diagnostic_mode=%s | meter_stats=%s | RF: %s",
               radio_name,
               listen_mode_to_string_(this->radio->get_listen_mode()),
               (unsigned) this->receiver_task_stack_size_,
               this->diag_mode_str_.c_str(),
               this->meter_stats_str_.c_str(),
               this->radio->get_rf_params_str().empty() ? "n/a" : this->radio->get_rf_params_str().c_str());

      if (this->cc1101_yaml_sanity_configured_) {
        ESP_LOGI(TAG, "CC1101 YAML sanity / sprawdzenie YAML CC1101:");
        ESP_LOGI(TAG, "  cc1101_allow_experimental: true -> experimental gate open / bramka eksperymentalna otwarta");
        // The schema already refuses to build without both pins, so these can
        // only read "configured". They are logged anyway: a reader debugging a
        // silent CC1101 should see dual-IRQ confirmed, not have to infer it.
        ESP_LOGI(TAG, "  gdo0_pin: %s / gdo2_pin: %s -> dual IRQ; single-IRQ CC1101 wiring is not supported / dwa przerwania; okablowanie single-IRQ nie jest wspierane",
                 this->cc1101_yaml_gdo0_ ? "configured" : "MISSING",
                 this->cc1101_yaml_gdo2_ ? "configured" : "MISSING");
      }
      this->radio->log_reg_status();
    }

    // Repeated here on purpose. The same line is logged from setup(), but that
    // happens before the network logger attaches, so over `esphome logs` it is
    // never seen - exactly the reason the YAML sanity block lives here too.
    // Logged in both states, so "no filter configured" is a positive statement
    // rather than a missing line.
    ESP_LOGI(TAG, "Forward whitelist / whitelista przekazywania: %s",
             this->forward_whitelist_summary_().c_str());

    // Every effective setting, marked (default) / (CHANGED) / (set). Logged for
    // every radio, not only the ones that happen to have a sanity block, so a
    // support question can be answered from the log alone instead of asking for
    // the YAML - and so "I did not change that" becomes checkable.
    // Deliberately here and not in setup(): setup() runs before the network
    // logger attaches, so over `esphome logs` those lines are never seen.
    if (!this->config_report_.empty()) {
      ESP_LOGI(TAG, "Configuration / konfiguracja (%s):", radio_name);
      for (const auto &line : this->config_report_)
        ESP_LOGI(TAG, "%s", line.c_str());
    }

    this->boot_log_last_ms_ = loop_now_ms;
    this->boot_log_count_++;
    this->boot_log_done_ = true;
  }
}

  auto *mqtt = mqtt::global_mqtt_client;
  if (this->radio != nullptr && mqtt != nullptr && mqtt->is_connected() && !this->diag_topic_.empty()) {
    std::string boot_payload = str_sprintf(
        "{\"event\":\"boot\",\"radio\":\"%s\",\"listen_mode\":\"%s\",\"uptime_ms\":%lu}",
        this->radio->get_name(),
        listen_mode_to_string_(this->radio->get_listen_mode()),
        (unsigned long) loop_now_ms);

    if (this->boot_info_mqtt_pending_) {
      std::string boot_topic = this->diag_topic_ + "/boot";
      mqtt->publish(boot_topic, boot_payload, this->diag_qos_, true);
      this->boot_info_mqtt_pending_ = false;
    }
    if (this->boot_info_event_pending_) {
      mqtt->publish(this->diag_topic_, std::string(boot_payload), this->diag_qos_, false);
      this->boot_info_event_pending_ = false;
    }

    // Effective configuration as one retained JSON snapshot per boot.
    // The add-on renders it as the "what the board is running" panel;
    // publishing here means the marker (default / CHANGED / set) matches
    // exactly what a reader would see in the boot log.
    if (this->config_report_mqtt_pending_ && !this->config_report_.empty()) {
      std::string cfg_payload = "{\"radio\":\"";
      cfg_payload += (this->radio != nullptr) ? this->radio->get_name() : "unknown";
      cfg_payload += "\",\"lines\":[";
      bool cfg_first = true;
      for (const auto &cfg_line : this->config_report_) {
        if (!cfg_first) cfg_payload += ",";
        cfg_first = false;
        cfg_payload += "\"";
        for (char ch : cfg_line) {
          if (ch == '\\' || ch == '\"') cfg_payload += '\\';
          if (ch == '\n' || ch == '\r' || ch == '\t') { cfg_payload += ' '; continue; }
          cfg_payload += ch;
        }
        cfg_payload += "\"";
      }
      cfg_payload += "]}";
      std::string cfg_topic = this->diag_topic_ + "/config";
      mqtt->publish(cfg_topic, cfg_payload, this->diag_qos_, true);
      this->config_report_mqtt_pending_ = false;
    }
  }

  if (this->dev_err_cleared_pending_ && mqtt != nullptr && mqtt->is_connected() && !this->diag_topic_.empty()) {
    const char *listen_mode = (this->radio != nullptr)
                                  ? listen_mode_to_string_(this->radio->get_listen_mode())
                                  : "unknown";
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"dev_err_cleared\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"before\":%u,\"before_hex\":\"%04X\",\"after\":%u,\"after_hex\":\"%04X\"}",
             (unsigned long) loop_now_ms, listen_mode,
             (unsigned) this->dev_err_before_, (unsigned) this->dev_err_before_,
             (unsigned) this->dev_err_after_, (unsigned) this->dev_err_after_);
    mqtt->publish(this->diag_topic_, std::string(payload), this->diag_qos_, false);
    this->dev_err_cleared_pending_ = false;
  }

  this->flush_mqtt_outbox_();
  this->update_outbox_stats_(loop_now_ms);
  this->maybe_reautosize_outbox_(loop_now_ms);
  this->maybe_publish_health_(loop_now_ms);
  this->maybe_publish_diag_summary_(loop_now_ms);
  this->maybe_publish_diag_15min_summary_(loop_now_ms);
  this->maybe_publish_diag_60min_summary_(loop_now_ms);
  this->maybe_publish_meter_windows_(loop_now_ms);
  Packet *p;
  if (xQueueReceive(this->packet_queue_, &p, 0) != pdPASS)
    return;

  this->maybe_publish_radio_raw_(p, loop_now_ms);

  // listen_mode filtering has two modes:
  // - default/legacy: filter by preliminary raw packet mode before parsing;
  // - experimental: parse first, then filter by parser/CRC-selected final mode.
  uint8_t mode_idx = (uint8_t) p->get_link_mode();

  if (!this->listen_mode_filter_after_parse_ && this->radio != nullptr) {
    const auto want = this->radio->get_listen_mode();
    const LinkMode got = p->get_link_mode();
    const bool reject =
        (want == LISTEN_MODE_C1 && got != LinkMode::C1) ||
        (want == LISTEN_MODE_T1 && got != LinkMode::T1) ||
        (want == LISTEN_MODE_S1 && got != LinkMode::S1);
    if (reject) {
      ESP_LOGD(TAG, "Filtered by listen_mode before parse: want=%s got=%s RSSI=%ddBm",
               (want == LISTEN_MODE_C1) ? "C1" : ((want == LISTEN_MODE_S1) ? "S1" : "T1"),
               link_mode_name(got),
               (int) p->get_rssi());
      delete p;
      return;
    }
  }

  // The raw-hex capture inside convert_to_frame() is only ever read behind
  // diag_publish_raw_, so let the packet skip it when that's off.
  p->set_capture_raw_hex(this->diag_publish_raw_);
  auto frame = p->convert_to_frame();

  if (this->listen_mode_filter_after_parse_) {
    mode_idx = (uint8_t) p->get_link_mode();
    if (this->radio != nullptr) {
      const auto want = this->radio->get_listen_mode();
      const LinkMode got = p->get_link_mode();
      const bool reject =
          (want == LISTEN_MODE_C1 && got != LinkMode::C1) ||
          (want == LISTEN_MODE_T1 && got != LinkMode::T1) ||
          (want == LISTEN_MODE_S1 && got != LinkMode::S1);
      if (reject) {
        ESP_LOGD(TAG, "Filtered by listen_mode after parse: want=%s got=%s RSSI=%ddBm",
                 (want == LISTEN_MODE_C1) ? "C1" : ((want == LISTEN_MODE_S1) ? "S1" : "T1"),
                 link_mode_name(got),
                 (int) p->get_rssi());
        delete p;
        return;
      }
    }
  }

  // Count only packets that pass the listen_mode filter.
  this->diag_total_++;
  this->diag_15m_total_++;
  this->diag_60min_total_++;
  // Always-on liveness: proof the RX path delivered a frame (not just that the
  // main loop ticks). Drives the health pulse's sec_since_last_rx, independent
  // of diagnostic_mode.
  this->rx_total_lifetime_++;
  this->last_rx_ms_ = loop_now_ms;
  this->any_rx_ = true;
  if (mode_idx < this->diag_mode_total_.size()) this->diag_mode_total_[mode_idx]++;
  if (mode_idx < this->diag_15m_mode_total_.size()) this->diag_15m_mode_total_[mode_idx]++;
  if (mode_idx < this->diag_60min_mode_total_.size()) this->diag_60min_mode_total_[mode_idx]++;

  if (mode_idx == (uint8_t) LinkMode::T1) {
    this->diag_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
    this->diag_15m_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_15m_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
    this->diag_60min_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_60min_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
  }

  if (!frame) {
    const char *mode = link_mode_name(p->get_link_mode());
    const char *listen_mode = (this->radio != nullptr)
                                  ? listen_mode_to_string_(this->radio->get_listen_mode())
                                  : "unknown";
    this->diag_dropped_by_stage_[bucket_for_stage_(p->drop_stage())]++;
    this->diag_15m_dropped_by_stage_[bucket_for_stage_(p->drop_stage())]++;
    this->diag_60min_dropped_by_stage_[bucket_for_stage_(p->drop_stage())]++;

    if (p->is_truncated()) {
      this->diag_truncated_++;
      this->diag_15m_truncated_++;
      this->diag_60min_truncated_++;
      if (this->should_publish_packet_event_(p) && mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[1280];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u,\"raw\":\"%s\"}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored(),
                   p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored());
        }
        mqtt::global_mqtt_client->publish(this->diag_topic_, std::string(payload), this->diag_qos_, false);
      }

      if (this->diag_verbose_) {
        ESP_LOGW(TAG,
                 "TRUNCATED frame / ucieta ramka: uptime_ms=%lu listen_mode=%s stage=%s reason=%s mode=%s want=%u got=%u raw_got=%u decoded_len=%u final_len=%u RSSI=%ddBm detail=%s",
                 (unsigned long) loop_now_ms, listen_mode, p->drop_stage().c_str(), p->drop_reason().c_str(), mode,
                 (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (unsigned) p->decoded_len(),
                 (unsigned) p->final_len(), (int) p->get_rssi(), p->drop_detail().c_str());
        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "TRUNCATED raw(hex) / ucieta ramka raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    } else if (!p->drop_reason().empty()) {
      this->diag_dropped_++;
      this->diag_15m_dropped_++;
      this->diag_60min_dropped_++;
      // Packet counters always advance; the RSSI sums only take real readings,
      // so an unmeasured frame does not drag the drop-side averages towards -127.
      const int32_t drop_rssi = (int32_t) p->get_rssi();
      const bool drop_rssi_ok = rssi_is_measured_((int) drop_rssi);
      if (drop_rssi_ok) {
        this->diag_rssi_drop_sum_ += drop_rssi;
        this->diag_rssi_drop_n_++;
        this->diag_15m_rssi_drop_sum_ += drop_rssi;
        this->diag_15m_rssi_drop_n_++;
        this->diag_60min_rssi_drop_sum_ += drop_rssi;
        this->diag_60min_rssi_drop_n_++;
      }
      if (mode_idx < this->diag_mode_dropped_.size()) {
        this->diag_mode_dropped_[mode_idx]++;
        if (drop_rssi_ok) {
          this->diag_mode_rssi_drop_sum_[mode_idx] += drop_rssi;
          this->diag_mode_rssi_drop_n_[mode_idx]++;
        }
      }
      if (mode_idx < this->diag_15m_mode_dropped_.size()) {
        this->diag_15m_mode_dropped_[mode_idx]++;
        if (drop_rssi_ok) {
          this->diag_15m_mode_rssi_drop_sum_[mode_idx] += drop_rssi;
          this->diag_15m_mode_rssi_drop_n_[mode_idx]++;
        }
      }
      if (mode_idx < this->diag_60min_mode_dropped_.size()) {
        this->diag_60min_mode_dropped_[mode_idx]++;
        if (drop_rssi_ok) {
          this->diag_60min_mode_rssi_drop_sum_[mode_idx] += drop_rssi;
          this->diag_60min_mode_rssi_drop_n_[mode_idx]++;
        }
      }
      auto bucket = bucket_for_reason_(p->drop_reason());
      this->diag_dropped_by_bucket_[bucket]++;
      this->diag_15m_dropped_by_bucket_[bucket]++;
      this->diag_60min_dropped_by_bucket_[bucket]++;
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_mode_crc_failed_.size()) {
        this->diag_mode_crc_failed_[mode_idx]++;
      }
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_15m_mode_crc_failed_.size()) {
        this->diag_15m_mode_crc_failed_[mode_idx]++;
      }
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_60min_mode_crc_failed_.size()) {
        this->diag_60min_mode_crc_failed_[mode_idx]++;
      }

      if (this->should_publish_packet_event_(p) && mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[1280];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u,\"raw\":\"%s\"}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored(),
                   p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored());
        }
        mqtt::global_mqtt_client->publish(this->diag_topic_, std::string(payload), this->diag_qos_, false);
      }

      if (this->diag_verbose_) {
        ESP_LOGW(TAG,
                 "DROPPED packet / odrzucony pakiet: uptime_ms=%lu listen_mode=%s stage=%s reason=%s mode=%s want=%u got=%u raw_got=%u decoded_len=%u final_len=%u RSSI=%ddBm detail=%s",
                 (unsigned long) loop_now_ms, listen_mode, p->drop_stage().c_str(), p->drop_reason().c_str(), mode,
                 (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (unsigned) p->decoded_len(),
                 (unsigned) p->final_len(), (int) p->get_rssi(), p->drop_detail().c_str());
        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "DROPPED raw(hex) / odrzucony pakiet raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    }

    this->collect_radio_rx_diag_();
    delete p;
    return;
  }

  this->diag_ok_++;
  this->diag_15m_ok_++;
  this->diag_60min_ok_++;
  // Frame counters always advance; the RSSI sums only take real readings. This
  // matters most for recent_ok_rssi_avg_ below, which feeds the weak-signal
  // thresholds in rf_runtime.cpp - a single -127 sentinel folded into that EMA
  // would push the abort thresholds down for the next several frames.
  const int32_t ok_rssi = (int32_t) frame->rssi();
  const bool ok_rssi_measured = rssi_is_measured_((int) ok_rssi);
  if (ok_rssi_measured) {
    this->diag_rssi_ok_sum_ += ok_rssi;
    this->diag_rssi_ok_n_++;
    this->diag_15m_rssi_ok_sum_ += ok_rssi;
    this->diag_15m_rssi_ok_n_++;
    this->diag_60min_rssi_ok_sum_ += ok_rssi;
    this->diag_60min_rssi_ok_n_++;
    if (!this->recent_ok_rssi_valid_) {
      this->recent_ok_rssi_avg_ = ok_rssi;
      this->recent_ok_rssi_valid_ = true;
    } else {
      this->recent_ok_rssi_avg_ = ((this->recent_ok_rssi_avg_ * 7) + ok_rssi) / 8;
    }
  }
  if (mode_idx < this->diag_mode_ok_.size()) {
    this->diag_mode_ok_[mode_idx]++;
    if (ok_rssi_measured) {
      this->diag_mode_rssi_ok_sum_[mode_idx] += ok_rssi;
      this->diag_mode_rssi_ok_n_[mode_idx]++;
    }
  }
  if (mode_idx < this->diag_15m_mode_ok_.size()) {
    this->diag_15m_mode_ok_[mode_idx]++;
    if (ok_rssi_measured) {
      this->diag_15m_mode_rssi_ok_sum_[mode_idx] += ok_rssi;
      this->diag_15m_mode_rssi_ok_n_[mode_idx]++;
    }
  }
  if (mode_idx < this->diag_60min_mode_ok_.size()) {
    this->diag_60min_mode_ok_[mode_idx]++;
    if (ok_rssi_measured) {
      this->diag_60min_mode_rssi_ok_sum_[mode_idx] += ok_rssi;
      this->diag_60min_mode_rssi_ok_n_[mode_idx]++;
    }
  }

  this->collect_radio_rx_diag_();

  auto &d = frame->data();

  const char *mfr = "???";
  char id_str[9] = "????????";
  uint8_t ver = 0xFF;
  uint8_t dev = 0xFF;
  uint8_t ci = 0xFF;
  uint32_t id_val = 0;
  uint32_t id_raw = 0;

  auto is_bcd = [](uint8_t b) -> bool {
    return ((b & 0x0F) <= 9) && (((b >> 4) & 0x0F) <= 9);
  };

  auto decode_mfr = [](uint16_t m, char out[4]) {
    out[0] = (char)(((m >> 10) & 0x1F) + 64);
    out[1] = (char)(((m >> 5) & 0x1F) + 64);
    out[2] = (char)((m & 0x1F) + 64);
    out[3] = 0;
    auto ok = [](char c) { return c >= 'A' && c <= 'Z'; };
    if (!ok(out[0]) || !ok(out[1]) || !ok(out[2])) {
      out[0] = out[1] = out[2] = '?';
    }
  };

  char mfr_buf[4] = "???";

  int base = -1;
  if (d.size() >= 10 && (size_t) (d[0] + 1) == d.size())
    base = 1;
  else if (d.size() >= 9)
    base = 0;

  if (base >= 0 && (int) d.size() >= base + 10) {
    uint16_t m = (uint16_t) d[base + 1] | ((uint16_t) d[base + 2] << 8);
    decode_mfr(m, mfr_buf);
    mfr = mfr_buf;

    bool bcd_ok = is_bcd(d[base + 3]) && is_bcd(d[base + 4]) && is_bcd(d[base + 5]) && is_bcd(d[base + 6]);
    if (bcd_ok) {
      id_val = (uint32_t) ((((d[base + 6] >> 4) & 0x0F) * 10000000U) + ((d[base + 6] & 0x0F) * 1000000U) +
                           (((d[base + 5] >> 4) & 0x0F) * 100000U) + ((d[base + 5] & 0x0F) * 10000U) +
                           (((d[base + 4] >> 4) & 0x0F) * 1000U) + ((d[base + 4] & 0x0F) * 100U) +
                           (((d[base + 3] >> 4) & 0x0F) * 10U) + (d[base + 3] & 0x0F));
      snprintf(id_str, sizeof(id_str), "%01u%01u%01u%01u%01u%01u%01u%01u",
               (d[base + 6] >> 4) & 0x0F, d[base + 6] & 0x0F,
               (d[base + 5] >> 4) & 0x0F, d[base + 5] & 0x0F,
               (d[base + 4] >> 4) & 0x0F, d[base + 4] & 0x0F,
               (d[base + 3] >> 4) & 0x0F, d[base + 3] & 0x0F);
    } else {
      snprintf(id_str, sizeof(id_str), "%02X%02X%02X%02X",
               d[base + 6], d[base + 5], d[base + 4], d[base + 3]);
    }

    // Same four bytes in the same order the hex form above prints them, so a
    // config entry copied straight out of the log matches. Unlike id_val this
    // is available for every meter, BCD or not.
    id_raw = ((uint32_t) d[base + 6] << 24) | ((uint32_t) d[base + 5] << 16) |
             ((uint32_t) d[base + 4] << 8) | (uint32_t) d[base + 3];

    ver = d[base + 7];
    dev = d[base + 8];
    ci = d[base + 9];
  }

  const bool highlight = meter_id_in_lists(this->highlight_meter_ids_, this->highlight_meter_raw_ids_,
                                           id_val, id_raw);

  // Update per-meter statistics for highlighted meters, or for all meters in diagnostic_meter_stats: all.
  // Keyed on id_raw, not id_val: it is unique per meter and, unlike id_val,
  // non-zero for non-BCD meters too. Keying on id_val collapsed every such
  // meter into one shared entry at key 0.
  if (id_raw != 0 && (highlight || this->diag_meter_stats_all_)) {
    uint32_t now_ms = (uint32_t) esphome::millis();
    // Composite key keeps T1 and C1 streams separate for dual-mode meters.
    const uint64_t stats_key = ((uint64_t) id_raw << 8) | (uint8_t) frame->link_mode();
    auto &stats = this->highlight_meter_stats_[stats_key];
    // Remember how this meter is printed; the key alone cannot reproduce it.
    std::strncpy(stats.id_str, id_str, sizeof(stats.id_str) - 1);
    stats.count++;
    // Packet counters always advance. rssi_last keeps the previous measured
    // value when this frame carries no measurement, so last_rssi stays a real
    // reading instead of flipping to the -127 sentinel; if the meter has never
    // been measured it stays at its 0 initialiser, the same "no data" value
    // win_avg_rssi already publishes for an empty window.
    if (ok_rssi_measured) {
      stats.rssi_last = ok_rssi;
      stats.rssi_sum += ok_rssi;
      stats.rssi_n++;
    }
    // Independent windowed counters for time-trigger and count-trigger.
    stats.count_window_time++;
    if (ok_rssi_measured) {
      stats.rssi_sum_window_time += ok_rssi;
      stats.rssi_n_window_time++;
    }

    // 60min window — reset only at summary_60min, never at summary_15min.
    stats.count_window_60min++;
    if (ok_rssi_measured) {
      stats.rssi_sum_window_60min += ok_rssi;
      stats.rssi_n_window_60min++;
    }

    if (stats.count_window_count == 0) {
      stats.count_window_started_ms = now_ms;
    }
    stats.count_window_count++;
    if (ok_rssi_measured) {
      stats.rssi_sum_window_count += ok_rssi;
      stats.rssi_n_window_count++;
    }

    if (stats.last_seen_ms != 0) {
      stats.last_interval_ms = now_ms - stats.last_seen_ms;
      stats.interval_sum_ms += stats.last_interval_ms;
      stats.interval_n++;
      stats.interval_sum_window_time_ms += stats.last_interval_ms;
      stats.interval_n_window_time++;
      stats.interval_sum_window_60min_ms += stats.last_interval_ms;
      stats.interval_n_window_60min++;
      if (stats.count_window_count > 1) {
        stats.interval_sum_window_count_ms += stats.last_interval_ms;
        stats.interval_n_window_count++;
      }
    }
    stats.last_seen_ms = now_ms;

    // Count-based trigger: publish when the dedicated count-window reaches threshold.
    if (this->diag_publish_summary_highlight_meters_ &&
        this->meter_window_count_threshold_ > 0 &&
        stats.count_window_count >= this->meter_window_count_threshold_) {
      const uint32_t elapsed_s = (stats.count_window_started_ms > 0)
          ? ((uint32_t) esphome::millis() - stats.count_window_started_ms) / 1000 : 0;
      const char *count_mode_str = link_mode_name(frame->link_mode());
      this->publish_meter_window_for_("count", elapsed_s, id_str, count_mode_str, stats,
                                      stats.count_window_count,
                                      stats.rssi_sum_window_count,
                                      stats.rssi_n_window_count,
                                      stats.interval_sum_window_count_ms,
                                      stats.interval_n_window_count,
                                      false, true);
    }
  }

  const char *log_tag = TAG;
  if (highlight) {
    if (!this->highlight_tag_.empty()) log_tag = this->highlight_tag_.c_str();
    const char *ansi_pre = this->highlight_ansi_ ? "\033[1;32m" : "";
    const char *ansi_suf = this->highlight_ansi_ ? "\033[0m" : "";
    ESP_LOGI(log_tag, "%s%sHave data / odebrano dane (decoded=%zu bytes, raw=%zu bytes) [RSSI: %ddBm, mode: %s %s, mfr:%s id:%s ver:%u type:%u ci:%02X]%s",
             ansi_pre, this->highlight_prefix_.c_str(),
             d.size(), p->raw_got_len(), frame->rssi(),
             link_mode_name(frame->link_mode()),
             frame->format().c_str(),
             mfr, id_str, (unsigned) ver, (unsigned) dev, (unsigned) ci,
             ansi_suf);

    // Keep highlight_meters lightweight by default: local emphasis plus packet number only.
    const uint64_t stats_key_ro = ((uint64_t) id_raw << 8) | (uint8_t) frame->link_mode();
    const auto &stats = this->highlight_meter_stats_[stats_key_ro];
    if (stats.count == 1) {
      ESP_LOGI(log_tag, "%s[id:%s] first packet / pierwszy pakiet (packet #1)",
               this->highlight_prefix_.c_str(), id_str);
    } else {
      ESP_LOGI(log_tag, "%s[id:%s] packet #%u received / odebrano pakiet nr %u",
               this->highlight_prefix_.c_str(), id_str,
               (unsigned) stats.count,
               (unsigned) stats.count);
    }
  } else {
    ESP_LOGI(TAG, "Have data / odebrano dane (decoded=%zu bytes, raw=%zu bytes) [RSSI: %ddBm, mode: %s %s, mfr:%s id:%s ver:%u type:%u ci:%02X]",
             d.size(), p->raw_got_len(), frame->rssi(),
             link_mode_name(frame->link_mode()),
             frame->format().c_str(),
             mfr, id_str, (unsigned) ver, (unsigned) dev, (unsigned) ci);
  }

  this->maybe_forward_frame_(frame.value(), id_val, id_raw, id_str, log_tag);

  for (auto &handler : this->handlers_)
    handler(&frame.value());

  if (frame->handlers_count()) {
    ESP_LOGI(TAG, "Telegram handled / obsluzono przez %d handlers", frame->handlers_count());
  } else {
    // Braces are required: at log level INFO the ESP_LOGD below compiles to an
    // empty statement, and an unbraced 'else' with an empty body warns
    // -Wempty-body (seen on the SX1276 arduino build, 2026.7.0).
    ESP_LOGD(TAG, "Telegram not handled by any handler");
  }

  delete p;
}

void Radio::wakeup_receiver_task_from_isr(TaskHandle_t *arg) {
  BaseType_t xHigherPriorityTaskWoken;
  vTaskNotifyGiveFromISR(*arg, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void Radio::receive_frame() {
  const uint32_t total_wait_ms = 60000;
  // Hop interval: how often restart_rx() is called while waiting for a packet.
  // 500ms was too aggressive — radio is blind during SPI re-arm, so a packet
  // arriving in that window is lost. At 40+ meters with ~30s TX intervals the
  // blind window hit statistically every few cycles. 5000ms keeps the safety-net
  // re-arm while reducing the chance of colliding with an incoming packet by 10x.
  const uint32_t hop_ms = 5000;
  uint32_t waited = 0;
  bool got_irq = false;
  while (waited < total_wait_ms) {
    this->radio->restart_rx();
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hop_ms))) {
      got_irq = true;
      this->diag_rx_path_.irq_fired++;
      this->diag_15m_rx_path_.irq_fired++;
      this->diag_60min_rx_path_.irq_fired++;
      break;
    }
    // A whole hop with no interrupt: the receiver has been armed and quiet, so
    // this reading is the ambient floor. Sampling anywhere else would catch a
    // transmission in progress or an AGC still settled on the last strong frame.
    //
    // read_channel_rssi_dbm(), NOT get_rssi(). Every driver caches get_rssi()
    // from the last packet capture, so sampling it here would measure the last
    // frame received rather than the channel - rebuilding the exact
    // successful-receptions-only feedback loop this is meant to escape. A radio
    // that cannot answer simply contributes no samples.
    int8_t channel_rssi = 0;
    if (this->radio->read_channel_rssi_dbm(&channel_rssi))
      this->note_idle_rssi_((int) channel_rssi);
    waited += hop_ms;
  }
  if (!got_irq) {
    this->diag_rx_path_.irq_timeout++;
    this->diag_15m_rx_path_.irq_timeout++;
    this->diag_60min_rx_path_.irq_timeout++;
    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "receive_wait", "interrupt_timeout");
    if (this->diag_verbose_) {
      this->radio->dump_debug_status("interrupt_timeout");
    }
    ESP_LOGD(TAG, "Radio interrupt timeout");
    return;
  }

  // This is the receiver-task wake time immediately after the radio IRQ. It is
  // intentionally not called an on-air start or RX_DONE timestamp: different
  // transceivers signal at different receive stages.
  const uint64_t rx_task_wakeup_us = (uint64_t) esp_timer_get_time();
  auto packet = std::make_unique<Packet>();
  packet->set_rx_task_wakeup_us(rx_task_wakeup_us);

  auto queue_packet = [this](std::unique_ptr<Packet> &pkt) -> bool {
    pkt->set_rssi(this->radio->get_rssi());
    auto packet_ptr = pkt.get();
    if (xQueueSend(this->packet_queue_, &packet_ptr, 0) == pdTRUE) {
      ESP_LOGV(TAG, "Queue items: %zu", uxQueueMessagesWaiting(this->packet_queue_));
      ESP_LOGV(TAG, "Queue send success");
      this->collect_radio_rx_diag_();
      pkt.release();
      return true;
    }

    this->diag_rx_path_.queue_send_failed++;
    this->diag_15m_rx_path_.queue_send_failed++;
    this->diag_60min_rx_path_.queue_send_failed++;
    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "queue_send", "queue_full_or_busy", this->radio->get_rssi());
    ESP_LOGW(TAG, "Queue send failed / wyslanie do kolejki nie powiodlo sie");
    return false;
  };

  if (this->radio != nullptr && this->radio->get_listen_mode() == LISTEN_MODE_S1) {
    packet->set_forced_link_mode(LinkMode::S1);
    const size_t max_raw = WMBUS_RAW_DRAIN_MAX_BYTES;
    auto *raw = packet->append_space(max_raw);
    size_t got_raw = 0;
    this->radio->read_in_task_partial(raw, max_raw, got_raw, WMBUS_NOTIFY_WAIT_MS, 3);
    packet->resize(got_raw);
    if (got_raw == 0) {
      this->diag_rx_path_.preamble_read_failed++;
      this->diag_15m_rx_path_.preamble_read_failed++;
      this->diag_60min_rx_path_.preamble_read_failed++;
      this->collect_radio_rx_diag_();
      this->publish_rx_path_event_("rx_path", "receive_s1_raw", "no_bytes_after_s1_sync", this->radio->get_rssi());
      ESP_LOGV(TAG, "S1 sync IRQ but no raw bytes read");
      return;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "s1_raw_len=%u", (unsigned) got_raw);
    this->publish_rx_path_event_("rx_path", "receive_s1_raw", detail, this->radio->get_rssi());
    queue_packet(packet);
    return;
  }

  auto raw_drain_fallback = [this, &packet, &queue_packet](const char *stage, const char *reason_detail,
                                                           size_t already_read, bool is_c_mode) -> bool {
    const int current_rssi = this->radio->get_rssi();
    if (!this->should_attempt_raw_drain_(current_rssi, already_read, is_c_mode)) {
      this->diag_rx_path_.raw_drain_skipped_weak++;
      this->diag_15m_rx_path_.raw_drain_skipped_weak++;
      this->diag_60min_rx_path_.raw_drain_skipped_weak++;
      return false;
    }

    this->diag_rx_path_.raw_drain_attempted++;
    this->diag_15m_rx_path_.raw_drain_attempted++;
    this->diag_60min_rx_path_.raw_drain_attempted++;
    const size_t max_extra = (already_read < WMBUS_RAW_DRAIN_MAX_BYTES)
                                 ? (WMBUS_RAW_DRAIN_MAX_BYTES - already_read)
                                 : 0;
    if (max_extra == 0) return false;

    auto *tail = packet->append_space(max_extra);
    size_t extra_read = 0;
    this->radio->read_in_task_partial(tail, max_extra, extra_read, WMBUS_NOTIFY_WAIT_MS, 1);
    packet->resize(already_read + extra_read);
    this->diag_rx_path_.raw_drain_bytes += (uint32_t) extra_read;
    this->diag_15m_rx_path_.raw_drain_bytes += (uint32_t) extra_read;
    this->diag_60min_rx_path_.raw_drain_bytes += (uint32_t) extra_read;

    char detail[144];
    snprintf(detail, sizeof(detail), "%s already_read=%u extra=%u final_raw=%u",
             reason_detail != nullptr ? reason_detail : "raw_drain",
             (unsigned) already_read, (unsigned) extra_read, (unsigned) packet->size());
    this->publish_rx_path_event_("rx_path", stage, detail, current_rssi);

    if (packet->size() > already_read) {
      this->diag_rx_path_.raw_drain_recovered++;
      this->diag_15m_rx_path_.raw_drain_recovered++;
      this->diag_60min_rx_path_.raw_drain_recovered++;
      ESP_LOGD(TAG, "Queued raw-drain fallback packet (%u -> %u bytes)",
               (unsigned) already_read, (unsigned) packet->size());
      return queue_packet(packet);
    }

    return false;
  };

  auto *preamble = packet->append_space(WMBUS_PREAMBLE_SIZE);
  size_t got_preamble = 0;
  this->radio->read_in_task_partial(preamble, WMBUS_PREAMBLE_SIZE, got_preamble, WMBUS_NOTIFY_WAIT_MS, 1);

  if (got_preamble < WMBUS_PREAMBLE_SIZE && radio_supports_preamble_retry_(this->radio)) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2))) {
      size_t got_retry = 0;
      this->radio->read_in_task_partial(preamble + got_preamble, WMBUS_PREAMBLE_SIZE - got_preamble,
                                        got_retry, WMBUS_NOTIFY_WAIT_MS, 1);
      got_preamble += got_retry;
      if (got_preamble == WMBUS_PREAMBLE_SIZE) {
        this->diag_rx_path_.preamble_retry_recovered++;
        this->diag_15m_rx_path_.preamble_retry_recovered++;
        this->diag_60min_rx_path_.preamble_retry_recovered++;
      }
    }
  }

  if (got_preamble < WMBUS_PREAMBLE_SIZE) {
    packet->resize(got_preamble);
    this->diag_rx_path_.preamble_read_failed++;
    this->diag_15m_rx_path_.preamble_read_failed++;
    this->diag_60min_rx_path_.preamble_read_failed++;
    const int current_rssi = this->radio->get_rssi();
    char detail[128];
    snprintf(detail, sizeof(detail), "got=%u need=%u", (unsigned) got_preamble, (unsigned) WMBUS_PREAMBLE_SIZE);
    if (this->should_abort_weak_partial_start_(current_rssi, got_preamble, false)) {
      this->diag_rx_path_.weak_start_aborted++;
      this->diag_15m_rx_path_.weak_start_aborted++;
      this->diag_60min_rx_path_.weak_start_aborted++;
      this->diag_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_15m_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_60min_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      strlcat(detail, " weak_partial_start", sizeof(detail));
    }
    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "receive_preamble", detail, current_rssi);
    ESP_LOGV(TAG, "Failed to read preamble");
    return;
  }

  const bool is_c_mode = (preamble[0] == WMBUS_MODE_C_PREAMBLE);
  size_t already_read = WMBUS_PREAMBLE_SIZE;
  if (!is_c_mode) {
    const int current_rssi = this->radio->get_rssi();
    if (this->should_abort_t1_probe_start_(current_rssi)) {
      this->diag_rx_path_.probe_start_aborted++;
      this->diag_15m_rx_path_.probe_start_aborted++;
      this->diag_60min_rx_path_.probe_start_aborted++;
      this->diag_rx_path_.probe_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_15m_rx_path_.probe_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_60min_rx_path_.probe_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->collect_radio_rx_diag_();
      this->publish_rx_path_event_("rx_path", "receive_probe_start", "weak_t1_probe_start", current_rssi);
      ESP_LOGV(TAG, "Abort weak T1 start before probe read");
      return;
    }
  }
  if (!is_c_mode && WMBUS_T1_LEN_PROBE_BYTES > WMBUS_PREAMBLE_SIZE) {
    const size_t extra = WMBUS_T1_LEN_PROBE_BYTES - WMBUS_PREAMBLE_SIZE;
    auto *hdr = packet->append_space(extra);
    size_t got_hdr = 0;
    this->radio->read_in_task_partial(hdr, extra, got_hdr, WMBUS_NOTIFY_WAIT_MS, 1);
    already_read += got_hdr;
    if (got_hdr < extra) {
      this->diag_rx_path_.t1_header_read_failed++;
      this->diag_15m_rx_path_.t1_header_read_failed++;
      this->diag_60min_rx_path_.t1_header_read_failed++;
      packet->resize(already_read);
      ESP_LOGV(TAG, "Short T1 probe read: got=%u need=%u", (unsigned) got_hdr, (unsigned) extra);
    }
  }

  const size_t total_len = packet->expected_size();
  if (total_len == 0 || total_len < already_read) {
    this->diag_rx_path_.payload_size_unknown++;
    this->diag_15m_rx_path_.payload_size_unknown++;
    this->diag_60min_rx_path_.payload_size_unknown++;
    const int current_rssi = this->radio->get_rssi();
    char detail[144];
    snprintf(detail, sizeof(detail), "total_len=%u already_read=%u", (unsigned) total_len, (unsigned) already_read);

    if (this->should_abort_weak_partial_start_(current_rssi, already_read, is_c_mode)) {
      this->diag_rx_path_.weak_start_aborted++;
      this->diag_15m_rx_path_.weak_start_aborted++;
      this->diag_60min_rx_path_.weak_start_aborted++;
      this->diag_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_15m_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_60min_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      strlcat(detail, " weak_partial_start", sizeof(detail));
      this->collect_radio_rx_diag_();
      this->publish_rx_path_event_("rx_path", "receive_expected_size", detail, current_rssi);
      ESP_LOGD(TAG, "Abort weak partial start before raw-drain");
      return;
    }

    if (raw_drain_fallback("receive_expected_size", detail, already_read, is_c_mode)) {
      return;
    }
    if (this->diag_rx_path_.raw_drain_skipped_weak > 0 &&
        this->should_abort_weak_partial_start_(current_rssi, already_read, is_c_mode)) {
      strlcat(detail, " raw_drain_skipped_weak", sizeof(detail));
    }

    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "receive_expected_size", detail, current_rssi);
    ESP_LOGD(TAG, "Cannot calculate payload size");
    return;
  }

  const size_t remaining = total_len - already_read;
  if (remaining > 0) {
    auto *rest = packet->append_space(remaining);
    if (!this->radio->read_in_task(rest, remaining)) {
      packet->resize(already_read);
      this->diag_rx_path_.payload_read_failed++;
      this->diag_15m_rx_path_.payload_read_failed++;
      this->diag_60min_rx_path_.payload_read_failed++;
      char detail[112];
      snprintf(detail, sizeof(detail), "remaining=%u total_len=%u already_read=%u", (unsigned) remaining,
               (unsigned) total_len, (unsigned) already_read);

      // Diagnostic, 2026-09-02: read the radio's own receive-chain registers at
      // the moment the payload read runs short, not at boot. Added to answer one
      // question - whether REG_RXTX_PAYLOAD_LEN really holds 0x01 here, as the
      // rx_configured dump on one board suggests, while SetPacketParams writes
      // 0xFF. A setup-time dump cannot tell "wrong from the start" from "written
      // later by the stream path"; this can.
      //
      // Placed before raw_drain_fallback() so it sees the register state closest
      // to the failure. That means it also covers reads the drain fallback then
      // recovers, which do not log "Failed to read data" - so expect more of
      // these lines than of that warning.
      //
      // Once per boot, and that is deliberate. The first run of this diagnostic
      // produced 35 samples on one board in 21 minutes and every single one was
      // byte-identical (RxAddrPtr=0x5C RxTxPldLen=0x5C, already_read=18,
      // regardless of frame length), so repeating it adds nothing and buries the
      // rest of the log. A reboot re-arms it, which is all that is needed to
      // check another board or another day. Same convention the RX snapshots
      // already follow: report once per receive path, then go quiet.
      {
        static bool pld_dump_done = false;
        if (!pld_dump_done) {
          pld_dump_done = true;
          ESP_LOGW(TAG, "payload read short / urwany odczyt payloadu: %s", detail);
          this->radio->dump_debug_status("payload_read_failed");
        }
      }

      if (raw_drain_fallback("receive_payload", detail, already_read, is_c_mode)) {
        return;
      }
      if (this->diag_rx_path_.raw_drain_skipped_weak > 0 &&
          this->should_abort_weak_partial_start_(this->radio->get_rssi(), already_read, is_c_mode)) {
        strlcat(detail, " raw_drain_skipped_weak", sizeof(detail));
      }

      this->collect_radio_rx_diag_();
      this->publish_rx_path_event_("rx_path", "receive_payload", detail, this->radio->get_rssi());
      ESP_LOGW(TAG, "Failed to read data / nie udalo sie odczytac danych");
      return;
    }
  }

  queue_packet(packet);
}

// One idle RSSI reading. Called only from the receiver task, on the path where
// a full hop elapsed with no interrupt - so nothing was being received and the
// reading is the channel itself, not a neighbour's telegram.
void Radio::note_idle_rssi_(int rssi_dbm) {
  if (!rssi_is_measured_(rssi_dbm))
    return;
  // -127..0 fits int8_t; clamp anyway so a driver returning something odd
  // cannot corrupt the ring.
  if (rssi_dbm > 0)
    rssi_dbm = 0;
  if (rssi_dbm < -127)
    rssi_dbm = -127;
  this->noise_floor_ring_[this->noise_floor_pos_] = (int8_t) rssi_dbm;
  this->noise_floor_pos_ = (uint8_t) ((this->noise_floor_pos_ + 1) % NOISE_FLOOR_SAMPLES);
  if (this->noise_floor_count_ < NOISE_FLOOR_SAMPLES)
    this->noise_floor_count_++;
}

// Minimum of the ring. Deliberately not a mean: samples taken while another
// meter transmits read high, and averaging would let them drag the floor up -
// the same mistake recent_ok_rssi_avg_ makes on the other side.
//
// Requires half the ring before answering. With one sample per idle hop (5 s)
// that is well under a minute on a quiet channel, and on a busy one the caller
// simply keeps using the legacy threshold until enough idle time exists.
bool Radio::noise_floor_dbm_(int32_t *out) const {
  if (this->noise_floor_count_ < (NOISE_FLOOR_SAMPLES / 2))
    return false;
  int32_t lowest = 0;
  bool have = false;
  for (uint8_t i = 0; i < this->noise_floor_count_; i++) {
    const int32_t v = (int32_t) this->noise_floor_ring_[i];
    if (!have || v < lowest) {
      lowest = v;
      have = true;
    }
  }
  if (!have)
    return false;
  if (out != nullptr)
    *out = lowest;
  return true;
}

void Radio::receiver_task(Radio *arg) {
  while (true)
    arg->receive_frame();
}

void Radio::add_frame_handler(std::function<void(Frame *)> &&callback) {
  this->handlers_.push_back(std::move(callback));
}

} // namespace wmbus_radio
} // namespace esphome
