#include "component.h"

#include "freertos/queue.h"
#include "freertos/task.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cstring>

// Optional: publish diagnostics via ESPHome MQTT if mqtt component is present.
#include "esphome/components/mqtt/mqtt_client.h"

#define WMBUS_PREAMBLE_SIZE (3)

#define ASSERT(expr, expected, before_exit)                                    \
  {                                                                            \
    auto result = (expr);                                                      \
    if (!!result != expected) {                                                \
      ESP_LOGE(TAG, "Assertion failed: %s -> %d", #expr, result);              \
      before_exit;                                                             \
      return;                                                                  \
    }                                                                          \
  }

#define ASSERT_SETUP(expr) ASSERT(expr, 1, this->mark_failed())

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "wmbus";


Radio::DropBucket Radio::bucket_for_reason_(const std::string &reason) {
  // Keep this stable: these strings come from Packet::set_drop_reason()
  if (reason == "too_short") return DB_TOO_SHORT;
  if (reason == "decode_failed") return DB_DECODE_FAILED;
  // Backwards compatible: older builds used dll_crc_strip_failed
  if (reason == "dll_crc_failed" || reason == "dll_crc_strip_failed") return DB_DLL_CRC_FAILED;
  if (reason == "unknown_preamble") return DB_UNKNOWN_PREAMBLE;
  if (reason == "l_field_invalid") return DB_L_FIELD_INVALID;
  if (reason == "unknown_link_mode") return DB_UNKNOWN_LINK_MODE;
  return DB_OTHER;
}

void Radio::maybe_publish_diag_summary_(uint32_t now_ms) {
  if (this->diag_topic_.empty()) return;
  if (this->last_diag_summary_ms_ == 0) {
    this->last_diag_summary_ms_ = now_ms;
    return;
  }
  uint32_t elapsed = now_ms - this->last_diag_summary_ms_;
  if (elapsed < this->diag_summary_interval_ms_) return;
  this->last_diag_summary_ms_ = now_ms;

  // Publish summary only if MQTT is available and connected
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  char payload[1100];
  const uint32_t crc_failed = this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED];
  const uint32_t total = this->diag_total_;
  const uint32_t ok = this->diag_ok_;
  // Percent as integer (0..100). Avoid floats to keep it light.
  const uint32_t crc_fail_pct = (total == 0) ? 0 : (crc_failed * 100U) / total;
  const uint32_t drop_pct = (total == 0) ? 0 : (this->diag_dropped_ * 100U) / total;
  const uint32_t trunc_pct = (total == 0) ? 0 : (this->diag_truncated_ * 100U) / total;
  const int32_t avg_ok_rssi = (this->diag_rssi_ok_n_ == 0) ? 0 : (this->diag_rssi_ok_sum_ / (int32_t) this->diag_rssi_ok_n_);
  const int32_t avg_drop_rssi = (this->diag_rssi_drop_n_ == 0) ? 0 : (this->diag_rssi_drop_sum_ / (int32_t) this->diag_rssi_drop_n_);
  // Per-mode indices: 1=T1, 2=C1 (0 is unknown)
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

  const uint32_t reasons_sum =
      this->diag_dropped_by_bucket_[DB_TOO_SHORT] +
      this->diag_dropped_by_bucket_[DB_DECODE_FAILED] +
      this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED] +
      this->diag_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE] +
      this->diag_dropped_by_bucket_[DB_L_FIELD_INVALID] +
      this->diag_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE] +
      this->diag_dropped_by_bucket_[DB_OTHER];
  const uint32_t reasons_sum_mismatch = (reasons_sum != this->diag_dropped_) ? 1U : 0U;

  // Human-friendly hint based on diagnostics (kept short; intended for quick triage).
  const char *hint_code = "OK";
  const char *hint_en = "looks good";
  const char *hint_pl = "wygląda dobrze";
  if (total == 0) {
    hint_code = "NO_DATA";
    hint_en = "no packets received yet";
    hint_pl = "brak odebranych ramek";
  } else {
    // C1 triage: most common confusion is 'wrong key' vs RF corruption.
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
    // Overload / near-field multipath suspicion: drops/CRC failures despite strong RSSI.
    // Common when a meter is very close to the antenna (front-end overload) or in reflective environments (pipes/metal).
    } else if (c1_total > 0 && c1_crc > 0 && c1_avg_ok_rssi >= -65 && c1_avg_drop_rssi >= -80) {
      hint_code = "C1_OVERLOAD_OR_MULTIPATH";
      hint_en = "C1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move antenna 0.5-2m, change polarization, or attenuate.";
      hint_pl = "C1: CRC pada mimo dobrego RSSI; możliwy przester odbiornika lub wielodrogowość. Odsuń antenę 0,5-2m, zmień polaryzację lub stłum sygnał.";
    } else if (t1_total > 0 && t1_crc > 0 && t1_avg_ok_rssi >= -65 && t1_avg_drop_rssi >= -80) {
      hint_code = "T1_OVERLOAD_OR_MULTIPATH";
      hint_en = "T1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move/rotate antenna or attenuate.";
      hint_pl = "T1: CRC pada mimo dobrego RSSI; możliwy przester lub wielodrogowość. Przestaw/obróć antenę lub stłum sygnał.";
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely bit errors/interference";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe błędy bitów/zakłócenia";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }
  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\","
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
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\""
           "}",
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
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl);

  mqtt->publish(this->diag_topic_, payload);
  ESP_LOGI(TAG, "DIAG summary published to %s (total=%u ok=%u truncated=%u dropped=%u crc_failed=%u)",
           this->diag_topic_.c_str(), (unsigned) total, (unsigned) this->diag_ok_,
           (unsigned) this->diag_truncated_, (unsigned) this->diag_dropped_, (unsigned) crc_failed);

  

// Print hint to logs for quick triage (same content as in MQTT diag summary).
if (std::strcmp(hint_code, "OK") == 0) {
  ESP_LOGI(TAG, "DIAG hint: %s | %s", hint_code, hint_pl);
} else {
  ESP_LOGW(TAG, "DIAG hint: %s | %s", hint_code, hint_pl);
}
// Report per-window stats (so it is easy to spot spikes)
  this->diag_total_ = 0;
  this->diag_ok_ = 0;
  this->diag_truncated_ = 0;
  this->diag_dropped_ = 0;
  this->diag_dropped_by_bucket_.fill(0);
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
}

void Radio::setup() {
  ASSERT_SETUP(this->packet_queue_ = xQueueCreate(3, sizeof(Packet *)));

  ASSERT_SETUP(xTaskCreate((TaskFunction_t)this->receiver_task, "radio_recv",
                           3 * 1024, this, 2, &(this->receiver_task_handle_)));

  ESP_LOGI(TAG, "Receiver task created [%p]", this->receiver_task_handle_);

  this->radio->attach_data_interrupt(Radio::wakeup_receiver_task_from_isr,
                                     &(this->receiver_task_handle_));
}

void Radio::loop() {
  this->maybe_publish_diag_summary_((uint32_t) esphome::millis());
  Packet *p;
  if (xQueueReceive(this->packet_queue_, &p, 0) != pdPASS)
    return;

  // Every item dequeued is a "received attempt" for diagnostics.
  this->diag_total_++;
  const uint8_t mode_idx = (uint8_t) p->get_link_mode();
  if (mode_idx < this->diag_mode_total_.size()) this->diag_mode_total_[mode_idx]++;

  auto frame = p->convert_to_frame();

  // T1 symbol-level diagnostics (available after convert_to_frame() ran)
  if (mode_idx == (uint8_t) LinkMode::T1) {
    this->diag_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
  }

  if (!frame) {
    // ---- Diagnostics accounting (always count, even if verbose is disabled)
    const char *mode = link_mode_name(p->get_link_mode());
    if (p->is_truncated()) {
      this->diag_truncated_++;

      // Publish diagnostics to MQTT regardless of diag_verbose_
      // (so YAML can silence logs but still get drop/trunc events).
      if (mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[900];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"raw\":\"%s\"}",
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u}",
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len());
        }
        mqtt::global_mqtt_client->publish(this->diag_topic_, payload);
      }

      if (this->diag_verbose_) {
        ESP_LOGW(TAG,
                 "TRUNCATED frame: mode=%s want=%u got=%u raw_got=%u RSSI=%ddBm",
                 mode, (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (int) p->get_rssi());

        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "TRUNCATED raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    } else if (!p->drop_reason().empty()) {
      this->diag_dropped_++;
      this->diag_rssi_drop_sum_ += (int32_t) p->get_rssi();
      this->diag_rssi_drop_n_++;
      if (mode_idx < this->diag_mode_dropped_.size()) {
        this->diag_mode_dropped_[mode_idx]++;
        this->diag_mode_rssi_drop_sum_[mode_idx] += (int32_t) p->get_rssi();
        this->diag_mode_rssi_drop_n_[mode_idx]++;
      }
      auto bucket = bucket_for_reason_(p->drop_reason());
      this->diag_dropped_by_bucket_[bucket]++;
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_mode_crc_failed_.size()) {
        this->diag_mode_crc_failed_[mode_idx]++;
      }

      // Publish diagnostics to MQTT regardless of diag_verbose_
      // (so YAML can silence logs but still get drop/trunc events).
      if (mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[900];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"reason\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"raw\":\"%s\"}",
                   p->drop_reason().c_str(), mode, (int) p->get_rssi(),
                   (unsigned) p->want_len(), (unsigned) p->got_len(),
                   (unsigned) p->raw_got_len(), p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"reason\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u}",
                   p->drop_reason().c_str(), mode, (int) p->get_rssi(),
                   (unsigned) p->want_len(), (unsigned) p->got_len(),
                   (unsigned) p->raw_got_len());
        }
        mqtt::global_mqtt_client->publish(this->diag_topic_, payload);
      }

      if (this->diag_verbose_) {
        ESP_LOGW(TAG,
                 "DROPPED packet: reason=%s mode=%s want=%u got=%u raw_got=%u RSSI=%ddBm",
                 p->drop_reason().c_str(), mode, (unsigned) p->want_len(),
                 (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                 (int) p->get_rssi());

        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "DROPPED raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    }

    delete p;
    return;
  }

  // Successful decode/validation
  this->diag_ok_++;
  this->diag_rssi_ok_sum_ += (int32_t) frame->rssi();
  this->diag_rssi_ok_n_++;
  if (mode_idx < this->diag_mode_ok_.size()) {
    this->diag_mode_ok_[mode_idx]++;
    this->diag_mode_rssi_ok_sum_[mode_idx] += (int32_t) frame->rssi();
    this->diag_mode_rssi_ok_n_[mode_idx]++;
  }

  auto &d = frame->data();

// Bazowo: nie wywalaj niczego, jak nie da się sparsować
const char *mfr = "???";
char id_str[9] = "????????";
uint8_t ver = 0xFF;
uint8_t dev = 0xFF;
uint8_t ci = 0xFF;

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

// base = indeks C-field
int base = -1;
// wariant z L-field (typowo: d[0]=L, d[1]=C)
if (d.size() >= 10 && (size_t)(d[0] + 1) == d.size())
  base = 1;
// wariant bez L-field (gdyby storage kiedyś był inny)
else if (d.size() >= 9)
  base = 0;

if (base >= 0 && (int)d.size() >= base + 10) {
  uint16_t m = (uint16_t)d[base + 1] | ((uint16_t)d[base + 2] << 8);
  decode_mfr(m, mfr_buf);
  mfr = mfr_buf;

  // ID bytes: base+3..base+6 (little endian) -> druk 6..3
  bool bcd_ok = is_bcd(d[base + 3]) && is_bcd(d[base + 4]) && is_bcd(d[base + 5]) && is_bcd(d[base + 6]);
  if (bcd_ok) {
    snprintf(id_str, sizeof(id_str), "%01u%01u%01u%01u%01u%01u%01u%01u",
             (d[base + 6] >> 4) & 0x0F, d[base + 6] & 0x0F,
             (d[base + 5] >> 4) & 0x0F, d[base + 5] & 0x0F,
             (d[base + 4] >> 4) & 0x0F, d[base + 4] & 0x0F,
             (d[base + 3] >> 4) & 0x0F, d[base + 3] & 0x0F);
  } else {
    // fallback HEX (żeby nie walić głupot)
    snprintf(id_str, sizeof(id_str), "%02X%02X%02X%02X",
             d[base + 6], d[base + 5], d[base + 4], d[base + 3]);
  }

  ver = d[base + 7];
  dev = d[base + 8];
  ci  = d[base + 9];
}

ESP_LOGI(TAG, "Have data (%zu bytes) [RSSI: %ddBm, mode: %s %s, mfr:%s id:%s ver:%u type:%u ci:%02X]",
         d.size(), frame->rssi(),
         link_mode_name(frame->link_mode()),
         frame->format().c_str(),
         mfr, id_str, (unsigned)ver, (unsigned)dev, (unsigned)ci);for (auto &handler : this->handlers_)
    handler(&frame.value());

  if (frame->handlers_count())
    ESP_LOGI(TAG, "Telegram handled by %d handlers", frame->handlers_count());
  else
    ESP_LOGD(TAG, "Telegram not handled by any handler");

  delete p;
}

void Radio::wakeup_receiver_task_from_isr(TaskHandle_t *arg) {
  BaseType_t xHigherPriorityTaskWoken;
  vTaskNotifyGiveFromISR(*arg, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void Radio::receive_frame() {
  // Ping-pong helper: restart RX in short windows to alternate sync bytes.
  // This dramatically improves hit rate for devices that transmit rarely.
  const uint32_t total_wait_ms = 60000;
  const uint32_t hop_ms = 500;
  uint32_t waited = 0;
  bool got_irq = false;
  while (waited < total_wait_ms) {
    this->radio->restart_rx();
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hop_ms))) {
      got_irq = true;
      break;
    }
    waited += hop_ms;
  }
  if (!got_irq) {
    ESP_LOGD(TAG, "Radio interrupt timeout");
    return;
  }
  auto packet = std::make_unique<Packet>();

  // Read the minimal header needed to determine expected length.
  auto *preamble = packet->append_space(WMBUS_PREAMBLE_SIZE);
  if (!this->radio->read_in_task(preamble, WMBUS_PREAMBLE_SIZE)) {
    ESP_LOGV(TAG, "Failed to read preamble");
    return;
  }

  const size_t total_len = packet->expected_size();
  if (total_len == 0 || total_len < WMBUS_PREAMBLE_SIZE) {
    ESP_LOGD(TAG, "Cannot calculate payload size");
    return;
  }

  const size_t remaining = total_len - WMBUS_PREAMBLE_SIZE;
  if (remaining > 0) {
    auto *rest = packet->append_space(remaining);
    if (!this->radio->read_in_task(rest, remaining)) {
      ESP_LOGW(TAG, "Failed to read data");
      return;
    }
  }

  packet->set_rssi(this->radio->get_rssi());
  auto packet_ptr = packet.get();

  if (xQueueSend(this->packet_queue_, &packet_ptr, 0) == pdTRUE) {
    ESP_LOGV(TAG, "Queue items: %zu",
             uxQueueMessagesWaiting(this->packet_queue_));
    ESP_LOGV(TAG, "Queue send success");
    packet.release();
  } else
    ESP_LOGW(TAG, "Queue send failed");
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
