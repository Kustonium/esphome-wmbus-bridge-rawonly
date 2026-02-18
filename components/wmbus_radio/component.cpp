#include "component.h"

#include "freertos/queue.h"
#include "freertos/task.h"

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

void Radio::setup() {
  ASSERT_SETUP(this->packet_queue_ = xQueueCreate(3, sizeof(Packet *)));

  ASSERT_SETUP(xTaskCreate((TaskFunction_t)this->receiver_task, "radio_recv",
                           3 * 1024, this, 2, &(this->receiver_task_handle_)));

  ESP_LOGI(TAG, "Receiver task created [%p]", this->receiver_task_handle_);

  this->radio->attach_data_interrupt(Radio::wakeup_receiver_task_from_isr,
                                     &(this->receiver_task_handle_));
}

void Radio::loop() {
  Packet *p;
  if (xQueueReceive(this->packet_queue_, &p, 0) != pdPASS)
    return;

  auto frame = p->convert_to_frame();
  if (!frame)
    return;

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
