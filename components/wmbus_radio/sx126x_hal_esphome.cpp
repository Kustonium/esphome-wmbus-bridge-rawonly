#include "transceiver_sx1262.h"
#include "esphome/core/log.h"

extern "C" {
#include "sx126x_hal.h"
#include "sx126x.h"
}
}
namespace {
static const char *TAG = "SX1262_HAL";
}

extern "C" {

sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command, const uint16_t command_length,
                                     const uint8_t *data, const uint16_t data_length) {
  auto *self = static_cast<esphome::wmbus_radio::SX1262 *>(const_cast<void *>(context));
  if (self == nullptr || self->delegate_ == nullptr)
    return SX126X_HAL_STATUS_ERROR;

  self->wait_while_busy_();
  self->delegate_->begin_transaction();
  for (uint16_t i = 0; i < command_length; i++)
    self->delegate_->transfer(command[i]);
  for (uint16_t i = 0; i < data_length; i++)
    self->delegate_->transfer(data[i]);
  self->delegate_->end_transaction();
  self->wait_while_busy_();
  return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command, const uint16_t command_length,
                                    uint8_t *data, const uint16_t data_length) {
  auto *self = static_cast<esphome::wmbus_radio::SX1262 *>(const_cast<void *>(context));
  if (self == nullptr || self->delegate_ == nullptr)
    return SX126X_HAL_STATUS_ERROR;

  self->wait_while_busy_();
  self->delegate_->begin_transaction();
  for (uint16_t i = 0; i < command_length; i++)
    self->delegate_->transfer(command[i]);
  for (uint16_t i = 0; i < data_length; i++)
    data[i] = self->delegate_->transfer(SX126X_NOP);
  self->delegate_->end_transaction();
  self->wait_while_busy_();
  return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context) {
  auto *self = static_cast<esphome::wmbus_radio::SX1262 *>(const_cast<void *>(context));
  if (self == nullptr || self->reset_pin_ == nullptr)
    return SX126X_HAL_STATUS_ERROR;
  // Same sequence as esphome::wmbus_radio::RadioTransceiver::reset()
  self->reset_pin_->digital_write(false);
  esphome::delay(10);
  self->reset_pin_->digital_write(true);
  esphome::delay(10);
  return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context) {
  auto *self = static_cast<esphome::wmbus_radio::SX1262 *>(const_cast<void *>(context));
  if (self == nullptr || self->delegate_ == nullptr)
    return SX126X_HAL_STATUS_ERROR;

  // Semtech wakeup procedure: NSS low + send NOP (0x00)
  // In ESPHome SPIDevice, begin_transaction toggles NSS.
  self->delegate_->begin_transaction();
  self->delegate_->transfer(SX126X_NOP);
  self->delegate_->end_transaction();
  self->wait_while_busy_();
  return SX126X_HAL_STATUS_OK;
}

}  // extern "C"
