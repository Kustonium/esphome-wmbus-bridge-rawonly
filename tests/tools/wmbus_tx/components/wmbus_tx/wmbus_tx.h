// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/spi/spi.h"

#include <cstdint>
#include <vector>

namespace esphome {
namespace wmbus_tx {

// Which wM-Bus link mode the frame is shaped for. Decides bitrate, deviation,
// sync word and line coding.
enum TxMode : uint8_t {
  TX_MODE_T1 = 0,  // 100 kbps, 3-of-6 coded
  TX_MODE_C1 = 1,  // 100 kbps, no line coding
  TX_MODE_S1 = 2,  // 32768 bps, Manchester coded
};

// SX1276 wM-Bus test transmitter.
//
// One class rather than the component/transceiver split the receiver uses:
// there is no receive task to own and only one supported radio, so the split
// bought nothing but indirection.
//
// The radio is driven in FSK continuous mode. The SX1276 generates the bit
// clock on DIO1/DCLK and we feed data bits to DIO2/DATA in step with it, which
// is why dclk_pin is mandatory - without that clock there is nothing to
// synchronise against.
class SX1276Transmitter : public Component,
                          public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_2MHZ> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_reset_pin(InternalGPIOPin *pin) { this->reset_pin_ = pin; }
  void set_dclk_pin(InternalGPIOPin *pin) { this->dclk_pin_ = pin; }
  void set_data_gpio(uint8_t gpio) { this->data_gpio_ = gpio; }
  void set_mode(TxMode mode) { this->mode_ = mode; }
  void set_frequency_hz(uint32_t hz) { this->frequency_hz_ = hz; }
  void set_power_dbm(uint8_t dbm) { this->power_dbm_ = dbm; }
  void set_interval_ms(uint32_t ms) { this->interval_ms_ = ms; }
  void set_dclk_diagnostics(bool enabled) { this->dclk_diagnostics_ = enabled; }

  // Complete link-layer body starting with the L-field. CRC and line coding
  // are added here; what goes in is what a receiver reports as the telegram.
  void set_frame(const std::vector<uint8_t> &frame) { this->frame_ = frame; }

  // Sends one frame. Public so an automation could trigger it on demand.
  bool transmit();

 protected:
  void reset_();
  uint8_t spi_read_(uint8_t address);
  void spi_write_(uint8_t address, uint8_t data);
  void spi_write_(uint8_t address, std::initializer_list<uint8_t> data);
  uint8_t spi_transaction_(uint8_t operation, uint8_t address, std::initializer_list<uint8_t> data);

  InternalGPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *dclk_pin_{nullptr};
  uint8_t data_gpio_{34};
  TxMode mode_{TX_MODE_T1};
  uint32_t frequency_hz_{868950000};
  uint8_t power_dbm_{12};
  uint32_t interval_ms_{30000};
  uint32_t last_tx_ms_{0};
  std::vector<uint8_t> frame_{};
  bool ready_{false};
  bool dclk_diagnostics_{false};
  uint32_t dclk_seq_{0};
};

}  // namespace wmbus_tx
}  // namespace esphome
