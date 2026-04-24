#include "transceiver.h"

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "wmbus.transceiver";

bool RadioTransceiver::read_in_task(uint8_t *buffer, size_t length) {
  const uint8_t *buffer_end = buffer + length;
  int wait_count = 0;

  while (buffer != buffer_end) {
    auto byte = this->read();
    if (byte.has_value())
      *buffer++ = *byte;
    else if (this->consume_rx_abort_request())
      return false;
    else if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1)))
      return false;
    else
      wait_count++;
  }

  return true;
}

bool RadioTransceiver::read_in_task_partial(uint8_t *buffer, size_t max_length,
                                            size_t &out_read, uint32_t wait_ms,
                                            uint8_t idle_rounds) {
  out_read = 0;
  if (max_length == 0) return true;
  if (idle_rounds == 0) idle_rounds = 1;

  uint8_t idle_seen = 0;
  while (out_read < max_length) {
    auto byte = this->read();
    if (byte.has_value()) {
      buffer[out_read++] = *byte;
      idle_seen = 0;
      continue;
    }

    if (this->consume_rx_abort_request()) {
      break;
    }

    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms))) {
      idle_seen++;
      if (idle_seen >= idle_rounds) break;
    }
  }

  return out_read == max_length;
}

void RadioTransceiver::set_reset_pin(InternalGPIOPin *reset_pin) {
  this->reset_pin_ = reset_pin;
}

void RadioTransceiver::set_irq_pin(InternalGPIOPin *irq_pin) {
  this->irq_pin_ = irq_pin;
}

void RadioTransceiver::set_busy_pin(InternalGPIOPin *busy_pin) {
  this->busy_pin_ = busy_pin;
}

void RadioTransceiver::reset() {
  this->reset_pin_->digital_write(0);
  delay(5);
  this->reset_pin_->digital_write(1);
  delay(5);
}

void RadioTransceiver::common_setup() {
  this->reset_pin_->setup();
  this->irq_pin_->setup();
  if (this->busy_pin_ != nullptr)
    this->busy_pin_->setup();
  this->spi_setup();
}

uint8_t RadioTransceiver::spi_transaction(uint8_t operation, uint8_t address,
                                          std::initializer_list<uint8_t> data) {
  this->delegate_->begin_transaction();
  auto rval = this->delegate_->transfer(operation | address);
  for (auto byte : data)
    rval = this->delegate_->transfer(byte);
  this->delegate_->end_transaction();
  return rval;
}

uint8_t RadioTransceiver::spi_read(uint8_t address) {
  return this->spi_transaction(0x00, address, {0});
}

void RadioTransceiver::spi_write(uint8_t address,
                                 std::initializer_list<uint8_t> data) {
  this->spi_transaction(0x80, address, data);
}

void RadioTransceiver::spi_write(uint8_t address, uint8_t data) {
  this->spi_write(address, {data});
}

void RadioTransceiver::dump_config() {
  ESP_LOGCONFIG(TAG, "Transceiver: %s", this->get_name());
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  IRQ Pin: ", this->irq_pin_);
  if (this->busy_pin_ != nullptr)
    LOG_PIN("  Busy Pin: ", this->busy_pin_);
  const char *mode_str = (this->listen_mode_ == LISTEN_MODE_T1) ? "T1 only"
                       : (this->listen_mode_ == LISTEN_MODE_C1) ? "C1 only"
                       : "T1+C1 (both, 3:1 bias)";
  ESP_LOGCONFIG(TAG, "  Listen mode: %s", mode_str);
}
} // namespace wmbus_radio
} // namespace esphome
