#pragma once
#include "esphome/components/spi/spi.h"
#include "esphome/core/optional.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

#define BYTE(x, n) ((uint8_t)(x >> (n * 8)))

namespace esphome {
namespace wmbus_radio {
enum ListenMode : uint8_t { LISTEN_MODE_BOTH = 0, LISTEN_MODE_T1 = 1, LISTEN_MODE_C1 = 2 };
class RadioTransceiver
    : public Component,
      public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                            spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_2MHZ> {
public:
  virtual void setup() override = 0;
  void dump_config() override;

  template <typename T>
  void attach_data_interrupt(void (*callback)(T *), T *arg) {
    this->irq_pin_->attach_interrupt(callback, arg, this->irq_edge_);
  }
  virtual void restart_rx() = 0;
  virtual int8_t get_rssi() = 0;
  virtual const char *get_name() = 0;

  // Optional: report SX126x device errors captured during boot clear.
  // Default: not supported.
  virtual bool get_boot_device_errors(uint16_t &before, uint16_t &after) const { return false; }

  bool read_in_task(uint8_t *buffer, size_t length);

  void set_spi(spi::SPIDelegate *spi);
  void set_reset_pin(InternalGPIOPin *reset_pin);
  void set_irq_pin(InternalGPIOPin *irq_pin);
  void set_busy_pin(InternalGPIOPin *busy_pin);
  void set_listen_mode(ListenMode mode) { this->listen_mode_ = mode; }
  ListenMode get_listen_mode() const { return this->listen_mode_; }

protected:
  InternalGPIOPin *reset_pin_;
  InternalGPIOPin *irq_pin_;
  InternalGPIOPin *busy_pin_{nullptr};

  // SX127x DIO for FIFO level is typically active-low (falling edge).
  // SX126x DIO for IRQ is active-high (rising edge).
  gpio::InterruptType irq_edge_{gpio::INTERRUPT_FALLING_EDGE};

  ListenMode listen_mode_{LISTEN_MODE_BOTH};

  virtual optional<uint8_t> read() = 0;

  void reset();
  void common_setup();
  uint8_t spi_transaction(uint8_t operation, uint8_t address,
                          std::initializer_list<uint8_t> data);
  uint8_t spi_read(uint8_t address);
  void spi_write(uint8_t address, std::initializer_list<uint8_t> data);
  void spi_write(uint8_t address, uint8_t data);
};

} // namespace wmbus_radio
} // namespace esphome
