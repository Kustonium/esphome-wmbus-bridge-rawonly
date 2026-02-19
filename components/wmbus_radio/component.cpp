#include "component.h"

#include "freertos/queue.h"
#include "freertos/task.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

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
  if (reason == "dll_crc_strip_failed") return DB_DLL_CRC_STRIP_FAILED;
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

  char payload[512];
  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\","
           "\"truncated\":%u,"
           "\"dropped\":%u,"
           "\"dropped_by_reason\":{"
           "\"too_short\":%u,"
           "\"decode_failed\":%u,"
           "\"dll_crc_strip_failed\":%u,"
           "\"unknown_preamble\":%u,"
           "\"l_field_invalid\":%u,"
           "\"unknown_link_mode\":%u,"
           "\"other\":%u"
           "}"
           "}",
           (unsigned) this->diag_truncated_,
           (unsigned) this->diag_dropped_,
           (unsigned) this->diag_dropped_by_bucket_[DB_TOO_SHORT],
           (unsigned) this->diag_dropped_by_bucket_[DB_DECODE_FAILED],
           (unsigned) this->diag_dropped_by_bucket_[DB_DLL_CRC_STRIP_FAILED],
           (unsigned) this->diag_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE],
           (unsigned) this->diag_dropped_by_bucket_[DB_L_FIELD_INVALID],
           (unsigned) this->diag_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE],
           (unsigned) this->diag_dropped_by_bucket_[DB_OTHER]);

  mqtt->publish(this->diag_topic_, payload);
  ESP_LOGI(TAG, "DIAG summary published to %s (truncated=%u dropped=%u)",
           this->diag_topic_.c_str(), (unsigned) this->diag_truncated_, (unsigned) this->diag_dropped_);

  // Report per-window stats (so it is easy to spot spikes)
  this->diag_truncated_ = 0;
  this->diag_dropped_ = 0;
  this->diag_dropped_by_bucket_.fill(0);
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

  auto frame = p->convert_to_frame();
  if (!frame) {
    // ---- Diagnostics accounting (always count, even if verbose is disabled)
    const char *mode = link_mode_name(p->get_link_mode());
    if (p->is_truncated()) {
      this->diag_truncated_++;

      if (this->diag_verbose_) {
        // Build payload (optionally with raw)
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

        ESP_LOGW(TAG,
                 "TRUNCATED frame: mode=%s want=%u got=%u raw_got=%u RSSI=%ddBm",
                 mode, (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (int) p->get_rssi());

        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "TRUNCATED raw(hex)=%s", p->raw_hex().c_str());
        }

        if (mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
          mqtt::global_mqtt_client->publish(this->diag_topic_, payload);
        }
      }
    } else if (!p->drop_reason().empty()) {
      this->diag_dropped_++;
      auto bucket = bucket_for_reason_(p->drop_reason());
      this->diag_dropped_by_bucket_[bucket]++;

      if (this->diag_verbose_) {
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

        ESP_LOGW(TAG,
                 "DROPPED packet: reason=%s mode=%s want=%u got=%u raw_got=%u RSSI=%ddBm",
                 p->drop_reason().c_str(), mode, (unsigned) p->want_len(),
                 (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                 (int) p->get_rssi());

        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "DROPPED raw(hex)=%s", p->raw_hex().c_str());
        }

        if (mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
          mqtt::global_mqtt_client->publish(this->diag_topic_, payload);
        }
      }
    }

    delete p;
    return;
  }

  ESP_LOGI(TAG, "Have data (%zu bytes) [RSSI: %ddBm, mode: %s %s]",
           frame->data().size(), frame->rssi(),
           link_mode_name(frame->link_mode()),
           frame->format().c_str());

  for (auto &handler : this->handlers_)
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
