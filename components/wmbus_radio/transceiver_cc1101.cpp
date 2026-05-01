#include "transceiver_cc1101.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "CC1101";

// CC1101 command strobes
static constexpr uint8_t CC1101_SRES  = 0x30;
static constexpr uint8_t CC1101_SCAL  = 0x33;
static constexpr uint8_t CC1101_SRX   = 0x34;
static constexpr uint8_t CC1101_SIDLE = 0x36;
static constexpr uint8_t CC1101_SFRX  = 0x3A;
static constexpr uint8_t CC1101_SNOP  = 0x3D;

// CC1101 configuration registers
static constexpr uint8_t REG_IOCFG2   = 0x00;
static constexpr uint8_t REG_IOCFG1   = 0x01;
static constexpr uint8_t REG_IOCFG0   = 0x02;
static constexpr uint8_t REG_FIFOTHR  = 0x03;
static constexpr uint8_t REG_SYNC1    = 0x04;
static constexpr uint8_t REG_SYNC0    = 0x05;
static constexpr uint8_t REG_PKTLEN   = 0x06;
static constexpr uint8_t REG_PKTCTRL1 = 0x07;
static constexpr uint8_t REG_PKTCTRL0 = 0x08;
static constexpr uint8_t REG_ADDR     = 0x09;
static constexpr uint8_t REG_CHANNR   = 0x0A;
static constexpr uint8_t REG_FSCTRL1  = 0x0B;
static constexpr uint8_t REG_FSCTRL0  = 0x0C;
static constexpr uint8_t REG_FREQ2    = 0x0D;
static constexpr uint8_t REG_FREQ1    = 0x0E;
static constexpr uint8_t REG_FREQ0    = 0x0F;
static constexpr uint8_t REG_MDMCFG4  = 0x10;
static constexpr uint8_t REG_MDMCFG3  = 0x11;
static constexpr uint8_t REG_MDMCFG2  = 0x12;
static constexpr uint8_t REG_MDMCFG1  = 0x13;
static constexpr uint8_t REG_MDMCFG0  = 0x14;
static constexpr uint8_t REG_DEVIATN  = 0x15;
static constexpr uint8_t REG_MCSM2    = 0x16;
static constexpr uint8_t REG_MCSM1    = 0x17;
static constexpr uint8_t REG_MCSM0    = 0x18;
static constexpr uint8_t REG_FOCCFG   = 0x19;
static constexpr uint8_t REG_BSCFG    = 0x1A;
static constexpr uint8_t REG_AGCCTRL2 = 0x1B;
static constexpr uint8_t REG_AGCCTRL1 = 0x1C;
static constexpr uint8_t REG_AGCCTRL0 = 0x1D;
static constexpr uint8_t REG_WOREVT1  = 0x1E;
static constexpr uint8_t REG_WOREVT0  = 0x1F;
static constexpr uint8_t REG_WORCTRL  = 0x20;
static constexpr uint8_t REG_FREND1   = 0x21;
static constexpr uint8_t REG_FREND0   = 0x22;
static constexpr uint8_t REG_FSCAL3   = 0x23;
static constexpr uint8_t REG_FSCAL2   = 0x24;
static constexpr uint8_t REG_FSCAL1   = 0x25;
static constexpr uint8_t REG_FSCAL0   = 0x26;
static constexpr uint8_t REG_RCCTRL1  = 0x27;
static constexpr uint8_t REG_RCCTRL0  = 0x28;
static constexpr uint8_t REG_FSTEST   = 0x29;
static constexpr uint8_t REG_PTEST    = 0x2A;
static constexpr uint8_t REG_AGCTEST  = 0x2B;
static constexpr uint8_t REG_TEST2    = 0x2C;
static constexpr uint8_t REG_TEST1    = 0x2D;
static constexpr uint8_t REG_TEST0    = 0x2E;

// CC1101 status registers
static constexpr uint8_t REG_PARTNUM  = 0x30;
static constexpr uint8_t REG_VERSION  = 0x31;
static constexpr uint8_t REG_RSSI     = 0x34;
static constexpr uint8_t REG_MARCSTATE= 0x35;
static constexpr uint8_t REG_RXBYTES  = 0x3B;
static constexpr uint8_t REG_FIFO     = 0x3F;

// SPI access bits
static constexpr uint8_t CC1101_READ  = 0x80;
static constexpr uint8_t CC1101_BURST = 0x40;

// GDO function selectors
static constexpr uint8_t GDO_RXFIFO_THR = 0x00;  // RX FIFO above threshold
static constexpr uint8_t GDO_SYNC_WORD  = 0x06;  // sync detected / packet context

// Known-good CC1101 T1 compatibility profile.
// These are treated as expected values by the self-check. If one of the
// critical registers differs, the user gets a clear verdict instead of a
// cryptic hexadecimal dump.
static constexpr uint8_t EXP_PARTNUM   = 0x00;
static constexpr uint8_t EXP_VERSION   = 0x14;
static constexpr uint8_t EXP_IOCFG2    = GDO_SYNC_WORD;
static constexpr uint8_t EXP_IOCFG0    = GDO_RXFIFO_THR;
static constexpr uint8_t EXP_FIFOTHR   = 0x07;
static constexpr uint8_t EXP_PKTCTRL1  = 0x00;
static constexpr uint8_t EXP_PKTCTRL0  = 0x02;  // infinite packet mode
static constexpr uint8_t EXP_SYNC1     = 0x54;
static constexpr uint8_t EXP_SYNC_T1   = 0x3D;
static constexpr uint8_t EXP_SYNC_C1   = 0xCD;
static constexpr uint8_t EXP_FSCTRL1   = 0x08;
static constexpr uint8_t EXP_MDMCFG4   = 0x5C;
static constexpr uint8_t EXP_MDMCFG3   = 0x04;
static constexpr uint8_t EXP_MDMCFG2   = 0x06;
static constexpr uint8_t EXP_DEVIATN   = 0x44;
static constexpr uint8_t EXP_FOCCFG    = 0x2E;
static constexpr uint8_t EXP_BSCFG     = 0xBF;
static constexpr uint8_t EXP_AGCCTRL2  = 0x43;
static constexpr uint8_t EXP_AGCCTRL1  = 0x09;
static constexpr uint8_t EXP_AGCCTRL0  = 0xB5;
static constexpr uint8_t EXP_FREND1    = 0xB6;
static constexpr uint8_t EXP_FSCAL3    = 0xEA;

static constexpr uint32_t CC1101_FOSC_HZ = 26000000UL;
static constexpr uint32_t CC1101_DEFAULT_FREQ_HZ = 868950000UL;
static constexpr uint32_t CC1101_READ_POLL_US = 1800;
static constexpr uint32_t CC1101_POLL_STEP_US = 80;

static const char *marc_state_name_(uint8_t state) {
  switch (state & 0x1F) {
    case 0x00: return "SLEEP";
    case 0x01: return "IDLE";
    case 0x02: return "XOFF";
    case 0x03: return "VCOON_MC";
    case 0x04: return "REGON_MC";
    case 0x05: return "MANCAL";
    case 0x06: return "VCOON";
    case 0x07: return "REGON";
    case 0x08: return "STARTCAL";
    case 0x09: return "BWBOOST";
    case 0x0A: return "FS_LOCK";
    case 0x0B: return "IFADCON";
    case 0x0C: return "ENDCAL";
    case 0x0D: return "RX";
    case 0x0E: return "RX_END";
    case 0x0F: return "RX_RST";
    case 0x10: return "TXRX_SWITCH";
    case 0x11: return "RXFIFO_OVERFLOW";
    case 0x12: return "FSTXON";
    case 0x13: return "TX";
    case 0x14: return "TX_END";
    case 0x15: return "RXTX_SWITCH";
    case 0x16: return "TXFIFO_UNDERFLOW";
    default: return "UNKNOWN";
  }
}

static const char *yes_no_(bool v) { return v ? "YES" : "NO"; }
static const char *ok_warn_(bool v) { return v ? "OK" : "WARN"; }
static const char *ok_bad_(bool v) { return v ? "OK" : "BAD"; }

static const char *pkt_length_mode_name_(uint8_t pktctrl0) {
  switch (pktctrl0 & 0x03) {
    case 0x00: return "fixed";
    case 0x01: return "variable";
    case 0x02: return "infinite";
    case 0x03: return "reserved";
    default: return "unknown";
  }
}

static const char *gdo_signal_name_(uint8_t value) {
  switch (value & 0x3F) {
    case GDO_RXFIFO_THR: return "RXFIFO_THRESHOLD";
    case GDO_SYNC_WORD: return "SYNC_WORD";
    case 0x2E: return "HIGH_Z";
    default: return "OTHER";
  }
}

static bool reg_ok_(uint8_t got, uint8_t expected) { return got == expected; }

static void log_expected_reg_(const char *name, uint8_t got, uint8_t expected,
                              const char *meaning_en, const char *meaning_pl) {
  if (got == expected) {
    ESP_LOGI(TAG, "CC1101 check OK / test OK: %s=0x%02X (%s / %s)",
             name, got, meaning_en, meaning_pl);
  } else {
    ESP_LOGE(TAG,
             "CC1101 CONFIG MISMATCH / blad konfiguracji: %s expected=0x%02X got=0x%02X (%s / %s)",
             name, expected, got, meaning_en, meaning_pl);
  }
}


uint8_t CC1101::strobe_(uint8_t cmd) {
  this->delegate_->begin_transaction();
  const uint8_t status = this->delegate_->transfer(cmd);
  this->delegate_->end_transaction();
  return status;
}

uint8_t CC1101::read_reg_(uint8_t address) {
  this->delegate_->begin_transaction();
  this->delegate_->transfer(address | CC1101_READ);
  const uint8_t value = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  return value;
}

uint8_t CC1101::read_status_(uint8_t address) {
  this->delegate_->begin_transaction();
  this->delegate_->transfer(address | CC1101_READ | CC1101_BURST);
  const uint8_t value = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  return value;
}

void CC1101::write_reg_(uint8_t address, uint8_t value) {
  this->delegate_->begin_transaction();
  this->delegate_->transfer(address);
  this->delegate_->transfer(value);
  this->delegate_->end_transaction();
}

void CC1101::write_burst_(uint8_t address, const uint8_t *data, size_t len) {
  if (len == 0) return;
  this->delegate_->begin_transaction();
  this->delegate_->transfer(address | CC1101_BURST);
  for (size_t i = 0; i < len; i++) this->delegate_->transfer(data[i]);
  this->delegate_->end_transaction();
}

void CC1101::read_burst_(uint8_t address, uint8_t *data, size_t len) {
  if (len == 0) return;
  this->delegate_->begin_transaction();
  this->delegate_->transfer(address | CC1101_READ | CC1101_BURST);
  for (size_t i = 0; i < len; i++) data[i] = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
}

uint8_t CC1101::rxbytes_raw_() { return this->read_status_(REG_RXBYTES); }
uint8_t CC1101::rxbytes_count_() { return this->rxbytes_raw_() & 0x7F; }
bool CC1101::rx_overflow_() { return (this->rxbytes_raw_() & 0x80) != 0; }

void CC1101::flush_rx_() {
  this->strobe_(CC1101_SIDLE);
  esp_rom_delay_us(120);
  this->strobe_(CC1101_SFRX);
  esp_rom_delay_us(120);
}

void CC1101::reset_cc1101_() {
  // Standard command reset. Keep CS toggling under the SPI delegate.
  this->strobe_(CC1101_SRES);
  delay(5);
}

void CC1101::set_frequency_(uint32_t frequency_hz) {
  const uint32_t freq = (uint32_t) (((uint64_t) frequency_hz << 16) / CC1101_FOSC_HZ);
  this->write_reg_(REG_FREQ2, (uint8_t) ((freq >> 16) & 0xFF));
  this->write_reg_(REG_FREQ1, (uint8_t) ((freq >> 8) & 0xFF));
  this->write_reg_(REG_FREQ0, (uint8_t) (freq & 0xFF));
}

void CC1101::set_sync_word_(uint8_t sync2) {
  this->write_reg_(REG_SYNC1, 0x54);
  this->write_reg_(REG_SYNC0, sync2);
}

void CC1101::apply_radio_profile_() {
  // CC1101 RF profile adjusted to the known-working Szczepan/Kubasa-style
  // wM-Bus 868.950 MHz profile. Keep our hardware model intact:
  // - GDO2 = sync detect IRQ
  // - GDO0 = FIFO threshold/data hint
  // - non-inverted GDO levels, because this driver uses rising-edge IRQ and
  //   polls the actual GDO levels directly.
  this->write_reg_(REG_IOCFG2, GDO_SYNC_WORD);
  this->write_reg_(REG_IOCFG1, 0x2E);  // GDO1 high impedance, do not fight MISO/GDO1
  this->write_reg_(REG_IOCFG0, GDO_RXFIFO_THR);

  // FIFO threshold: 0x07 means RX FIFO threshold around 32 bytes.
  this->write_reg_(REG_FIFOTHR, 0x07);

  this->set_frequency_(CC1101_DEFAULT_FREQ_HZ);
  this->write_reg_(REG_SYNC1, 0x54);
  this->write_reg_(REG_SYNC0, 0x3D);
  this->write_reg_(REG_PKTLEN, 0xFF);
  this->write_reg_(REG_PKTCTRL1, 0x00);  // no address check, no appended status

  // Keep infinite packet mode. Szczepan main uses 0x00, but that would be a bad
  // fit for this raw bridge because T1 3-of-6 raw packets can exceed 255 bytes.
  this->write_reg_(REG_PKTCTRL0, 0x02);  // infinite packet length, CRC off, whitening off
  this->write_reg_(REG_ADDR, 0x00);
  this->write_reg_(REG_CHANNR, 0x00);

  // Known-working 100 kbps 2-FSK-ish wM-Bus profile used as compatibility base.
  // This replaces the previous profile that stayed in RX but never detected sync
  // on hardware which worked with Szczepan's firmware.
  this->write_reg_(REG_FSCTRL1, 0x08);
  this->write_reg_(REG_FSCTRL0, 0x00);
  this->write_reg_(REG_MDMCFG4, 0x5C);
  this->write_reg_(REG_MDMCFG3, 0x04);
  this->write_reg_(REG_MDMCFG2, 0x06);
  this->write_reg_(REG_MDMCFG1, 0x22);
  this->write_reg_(REG_MDMCFG0, 0xF8);
  this->write_reg_(REG_DEVIATN, 0x44);

  this->write_reg_(REG_MCSM2, 0x07);
  this->write_reg_(REG_MCSM1, 0x00);
  this->write_reg_(REG_MCSM0, 0x18);  // auto-calibrate from IDLE to RX/TX
  this->write_reg_(REG_FOCCFG, 0x2E);
  this->write_reg_(REG_BSCFG, 0xBF);
  this->write_reg_(REG_AGCCTRL2, 0x43);
  this->write_reg_(REG_AGCCTRL1, 0x09);
  this->write_reg_(REG_AGCCTRL0, 0xB5);
  this->write_reg_(REG_WOREVT1, 0x87);
  this->write_reg_(REG_WOREVT0, 0x6B);
  this->write_reg_(REG_WORCTRL, 0xFB);
  this->write_reg_(REG_FREND1, 0xB6);
  this->write_reg_(REG_FREND0, 0x10);
  this->write_reg_(REG_FSCAL3, 0xEA);
  this->write_reg_(REG_FSCAL2, 0x2A);
  this->write_reg_(REG_FSCAL1, 0x00);
  this->write_reg_(REG_FSCAL0, 0x1F);
  this->write_reg_(REG_RCCTRL1, 0x41);
  this->write_reg_(REG_RCCTRL0, 0x00);
  this->write_reg_(REG_FSTEST, 0x59);
  this->write_reg_(REG_PTEST, 0x7F);
  this->write_reg_(REG_AGCTEST, 0x3F);
  this->write_reg_(REG_TEST2, 0x81);
  this->write_reg_(REG_TEST1, 0x35);
  this->write_reg_(REG_TEST0, 0x09);

  this->strobe_(CC1101_SCAL);
  delay(2);
}

bool CC1101::validate_startup_config_() {
  const uint8_t partnum = this->read_status_(REG_PARTNUM);
  const uint8_t version = this->read_status_(REG_VERSION);

  const uint8_t iocfg2 = this->read_reg_(REG_IOCFG2);
  const uint8_t iocfg0 = this->read_reg_(REG_IOCFG0);
  const uint8_t fifothr = this->read_reg_(REG_FIFOTHR);
  const uint8_t pktctrl1 = this->read_reg_(REG_PKTCTRL1);
  const uint8_t pktctrl0 = this->read_reg_(REG_PKTCTRL0);
  const uint8_t sync1 = this->read_reg_(REG_SYNC1);
  const uint8_t sync0 = this->read_reg_(REG_SYNC0);

  const uint8_t fsctrl1 = this->read_reg_(REG_FSCTRL1);
  const uint8_t mdmcfg4 = this->read_reg_(REG_MDMCFG4);
  const uint8_t mdmcfg3 = this->read_reg_(REG_MDMCFG3);
  const uint8_t mdmcfg2 = this->read_reg_(REG_MDMCFG2);
  const uint8_t deviatn = this->read_reg_(REG_DEVIATN);
  const uint8_t foccfg = this->read_reg_(REG_FOCCFG);
  const uint8_t bscfg = this->read_reg_(REG_BSCFG);
  const uint8_t agc2 = this->read_reg_(REG_AGCCTRL2);
  const uint8_t agc1 = this->read_reg_(REG_AGCCTRL1);
  const uint8_t agc0 = this->read_reg_(REG_AGCCTRL0);
  const uint8_t frend1 = this->read_reg_(REG_FREND1);
  const uint8_t fscal3 = this->read_reg_(REG_FSCAL3);

  bool ok = true;

  ESP_LOGI(TAG, "CC1101 self-check / autotest konfiguracji CC1101");

  auto check = [&ok](const char *name, uint8_t got, uint8_t expected,
                    const char *meaning_en, const char *meaning_pl) {
    const bool pass = got == expected;
    ok = ok && pass;
    log_expected_reg_(name, got, expected, meaning_en, meaning_pl);
  };

  check("PARTNUM", partnum, EXP_PARTNUM, "CC1101 part number", "numer ukladu CC1101");
  check("VERSION", version, EXP_VERSION, "CC1101 SPI response", "odpowiedz CC1101 po SPI");
  check("IOCFG2", iocfg2, EXP_IOCFG2, "GDO2 = sync detect IRQ", "GDO2 = przerwanie wykrycia sync");
  check("IOCFG0", iocfg0, EXP_IOCFG0, "GDO0 = RX FIFO threshold", "GDO0 = prog RX FIFO");
  check("FIFOTHR", fifothr, EXP_FIFOTHR, "FIFO threshold around 32 bytes", "prog FIFO okolo 32 bajtow");
  check("PKTCTRL1", pktctrl1, EXP_PKTCTRL1, "no address filtering/status append", "bez filtrowania adresu i statusu");
  check("PKTCTRL0", pktctrl0, EXP_PKTCTRL0, "infinite packet mode for long T1 raw frames", "tryb infinite dla dlugich ramek T1 raw");
  check("SYNC1", sync1, EXP_SYNC1, "wM-Bus sync high byte", "starszy bajt sync wM-Bus");
  check("SYNC0", sync0, EXP_SYNC_T1, "T1 sync low byte", "mlodszy bajt sync T1");

  check("FSCTRL1", fsctrl1, EXP_FSCTRL1, "RF frequency offset profile", "profil przesuniecia czestotliwosci RF");
  check("MDMCFG4", mdmcfg4, EXP_MDMCFG4, "RX bandwidth / data rate profile", "profil pasma RX / predkosci danych");
  check("MDMCFG3", mdmcfg3, EXP_MDMCFG3, "data rate profile", "profil predkosci danych");
  check("MDMCFG2", mdmcfg2, EXP_MDMCFG2, "modem/sync profile", "profil modemu/synchronizacji");
  check("DEVIATN", deviatn, EXP_DEVIATN, "frequency deviation profile", "profil dewiacji czestotliwosci");
  check("FOCCFG", foccfg, EXP_FOCCFG, "frequency offset compensation", "kompensacja odchylki czestotliwosci");
  check("BSCFG", bscfg, EXP_BSCFG, "bit synchronization", "synchronizacja bitow");
  check("AGCCTRL2", agc2, EXP_AGCCTRL2, "AGC profile", "profil AGC");
  check("AGCCTRL1", agc1, EXP_AGCCTRL1, "AGC profile", "profil AGC");
  check("AGCCTRL0", agc0, EXP_AGCCTRL0, "AGC profile", "profil AGC");
  check("FREND1", frend1, EXP_FREND1, "RF frontend profile", "profil toru RF");
  check("FSCAL3", fscal3, EXP_FSCAL3, "frequency synthesizer calibration", "kalibracja syntezy czestotliwosci");

  if (ok) {
    ESP_LOGI(TAG,
             "CC1101 self-check result / wynik autotestu: CONFIG_OK - SPI, GDO mapping, packet mode and RF profile look correct / SPI, GDO, tryb pakietu i profil RF wygladaja poprawnie");
  } else {
    ESP_LOGE(TAG,
             "CC1101 self-check result / wynik autotestu: CONFIG_ERROR - receiver will be stopped / odbiornik zostanie zatrzymany");
    ESP_LOGE(TAG,
             "CC1101 hint / wskazowka: this is not an RF-range problem; the chip configuration does not match the expected T1 profile / to nie jest problem zasiegu RF, konfiguracja ukladu nie zgadza sie z oczekiwanym profilem T1");
  }

  return ok;
}

void CC1101::dump_debug_status(const char *reason) {
  const uint8_t partnum = this->read_status_(REG_PARTNUM);
  const uint8_t version = this->read_status_(REG_VERSION);
  const uint8_t marc_raw = this->read_status_(REG_MARCSTATE);
  const uint8_t marc = marc_raw & 0x1F;
  const uint8_t rxbytes_raw = this->read_status_(REG_RXBYTES);
  const uint8_t rxbytes = rxbytes_raw & 0x7F;
  const bool overflow = (rxbytes_raw & 0x80) != 0;

  const uint8_t iocfg2 = this->read_reg_(REG_IOCFG2);
  const uint8_t iocfg0 = this->read_reg_(REG_IOCFG0);
  const uint8_t fifothr = this->read_reg_(REG_FIFOTHR);
  const uint8_t pktctrl1 = this->read_reg_(REG_PKTCTRL1);
  const uint8_t pktctrl0 = this->read_reg_(REG_PKTCTRL0);
  const uint8_t fsctrl1 = this->read_reg_(REG_FSCTRL1);
  const uint8_t mdmcfg4 = this->read_reg_(REG_MDMCFG4);
  const uint8_t mdmcfg3 = this->read_reg_(REG_MDMCFG3);
  const uint8_t mdmcfg2 = this->read_reg_(REG_MDMCFG2);
  const uint8_t deviatn = this->read_reg_(REG_DEVIATN);
  const uint8_t foccfg = this->read_reg_(REG_FOCCFG);
  const uint8_t bscfg = this->read_reg_(REG_BSCFG);
  const uint8_t agc2 = this->read_reg_(REG_AGCCTRL2);
  const uint8_t agc1 = this->read_reg_(REG_AGCCTRL1);
  const uint8_t agc0 = this->read_reg_(REG_AGCCTRL0);
  const uint8_t frend1 = this->read_reg_(REG_FREND1);
  const uint8_t fscal3 = this->read_reg_(REG_FSCAL3);
  const uint8_t sync1 = this->read_reg_(REG_SYNC1);
  const uint8_t sync0 = this->read_reg_(REG_SYNC0);

  const int gdo0 = this->gdo0_pin_ != nullptr ? (int) this->gdo0_pin_->digital_read() : -1;
  const int gdo2 = this->gdo2_pin_ != nullptr ? (int) this->gdo2_pin_->digital_read() : -1;

  const bool after_setup = reason != nullptr && std::strcmp(reason, "after_setup") == 0;

  const bool spi_ok = version != 0x00 && version != 0xFF;
  const bool version_ok = version == EXP_VERSION;
  const bool partnum_ok = partnum == EXP_PARTNUM;
  const bool rx_running = marc == 0x0D;
  const bool rx_recovering = marc == 0x0E || marc == 0x0F;
  const bool rx_state_ok = rx_running || rx_recovering;
  const bool rx_overflow_state = marc == 0x11 || overflow;
  const bool sync_seen_now = gdo2 > 0;
  const bool fifo_threshold_now = gdo0 > 0;
  const bool has_fifo_data = rxbytes > 0;

  const bool gdo2_cfg_ok = reg_ok_(iocfg2, EXP_IOCFG2);
  const bool gdo0_cfg_ok = reg_ok_(iocfg0, EXP_IOCFG0);
  const bool fifothr_ok = reg_ok_(fifothr, EXP_FIFOTHR);
  const bool pktctrl1_ok = reg_ok_(pktctrl1, EXP_PKTCTRL1);
  const bool pktctrl0_ok = reg_ok_(pktctrl0, EXP_PKTCTRL0);
  const bool pkt_infinite = (pktctrl0 & 0x03) == 0x02;
  const bool sync_t1 = sync1 == EXP_SYNC1 && sync0 == EXP_SYNC_T1;
  const bool sync_c1 = sync1 == EXP_SYNC1 && sync0 == EXP_SYNC_C1;
  const bool sync_known = sync_t1 || sync_c1;

  const bool rf_profile_ok =
      reg_ok_(fsctrl1, EXP_FSCTRL1) && reg_ok_(mdmcfg4, EXP_MDMCFG4) &&
      reg_ok_(mdmcfg3, EXP_MDMCFG3) && reg_ok_(mdmcfg2, EXP_MDMCFG2) &&
      reg_ok_(deviatn, EXP_DEVIATN) && reg_ok_(foccfg, EXP_FOCCFG) &&
      reg_ok_(bscfg, EXP_BSCFG) && reg_ok_(agc2, EXP_AGCCTRL2) &&
      reg_ok_(agc1, EXP_AGCCTRL1) && reg_ok_(agc0, EXP_AGCCTRL0) &&
      reg_ok_(frend1, EXP_FREND1) && reg_ok_(fscal3, EXP_FSCAL3);

  const bool config_ok = spi_ok && version_ok && partnum_ok && gdo2_cfg_ok && gdo0_cfg_ok &&
                         fifothr_ok && pktctrl1_ok && pktctrl0_ok && rf_profile_ok && sync_known;

  const char *diag_code = "UNKNOWN";
  const char *diag_en = "State could not be classified.";
  const char *diag_pl = "Nie udalo sie sklasyfikowac stanu.";
  bool severe = false;
  bool warning = true;

  if (!spi_ok) {
    diag_code = "SPI_FAIL";
    diag_en = "CC1101 does not respond on SPI. Check CS, MISO, MOSI, SCK and power.";
    diag_pl = "CC1101 nie odpowiada po SPI. Sprawdz CS, MISO, MOSI, SCK oraz zasilanie.";
    severe = true;
  } else if (!version_ok || !partnum_ok) {
    diag_code = "UNEXPECTED_CHIP_ID";
    diag_en = "SPI responds, but PARTNUM/VERSION is unexpected for CC1101.";
    diag_pl = "SPI odpowiada, ale PARTNUM/VERSION nie pasuje do oczekiwanego CC1101.";
    severe = true;
  } else if (!gdo2_cfg_ok) {
    diag_code = "CONFIG_BAD_GDO2";
    diag_en = "GDO2 is not configured as sync-detect IRQ. Receiver may never start packet read.";
    diag_pl = "GDO2 nie jest ustawione jako wykrycie sync. Odbiornik moze nigdy nie zaczac czytania ramki.";
    severe = true;
  } else if (!gdo0_cfg_ok) {
    diag_code = "CONFIG_BAD_GDO0";
    diag_en = "GDO0 is not configured as RX FIFO threshold. FIFO drain timing will be wrong.";
    diag_pl = "GDO0 nie jest ustawione jako prog RX FIFO. Oproznianie FIFO bedzie bledne.";
    severe = true;
  } else if (!pktctrl0_ok) {
    diag_code = "CONFIG_BAD_PACKET_MODE";
    diag_en = "Packet mode is not infinite. Long T1 raw frames may fail or be truncated.";
    diag_pl = "Tryb pakietu nie jest infinite. Dlugie ramki T1 raw moga sie ucinac albo nie przechodzic.";
    severe = true;
  } else if (!rf_profile_ok) {
    diag_code = "CONFIG_BAD_RF_PROFILE";
    diag_en = "CC1101 RF modem profile does not match the known-good wM-Bus T1 profile.";
    diag_pl = "Profil modemu RF CC1101 nie zgadza sie ze sprawdzonym profilem wM-Bus T1.";
    severe = true;
  } else if (!sync_known) {
    diag_code = "CONFIG_BAD_SYNC";
    diag_en = "Sync word is not T1 or C1. Receiver will not detect expected wM-Bus frames.";
    diag_pl = "Slowo sync nie jest T1 ani C1. Odbiornik nie wykryje oczekiwanych ramek wM-Bus.";
    severe = true;
  } else if (rx_overflow_state) {
    diag_code = "RX_FIFO_OVERFLOW";
    diag_en = "CC1101 received data, but the 64-byte RX FIFO overflowed before it was drained. Frame was dropped and RX will be restarted.";
    diag_pl = "CC1101 odebral dane, ale 64-bajtowe RX FIFO przepelnilo sie zanim zostalo oproznione. Ramka zostala odrzucona i RX zostanie uruchomiony ponownie.";
  } else if (!rx_state_ok) {
    diag_code = "RADIO_NOT_IN_RX";
    diag_en = "CC1101 is not in RX/listening state. RX should be restarted.";
    diag_pl = "CC1101 nie jest w stanie RX/nasluchu. RX powinien zostac uruchomiony ponownie.";
  } else if (after_setup && config_ok && rx_state_ok) {
    diag_code = "CONFIG_OK";
    diag_en = "Startup configuration is valid. SPI, GDO mapping, packet mode and RF profile look correct.";
    diag_pl = "Konfiguracja startowa jest poprawna. SPI, GDO, tryb pakietu i profil RF wygladaja dobrze.";
    warning = false;
  } else if (rx_running && !has_fifo_data && !sync_seen_now && !fifo_threshold_now) {
    diag_code = "RADIO_OK_NO_RF_SYNC";
    diag_en = "CC1101 is alive and listening, but no T1/C1 sync or data was detected. This is not a decoder problem. Check 868 MHz module, antenna, RF signal, transmitter presence or RF profile.";
    diag_pl = "CC1101 dziala i slucha, ale nie wykryto sync ani danych T1/C1. To nie jest problem dekodera. Sprawdz modul 868 MHz, antene, sygnal RF, obecnosc nadajnika albo profil RF.";
  } else if (has_fifo_data && !sync_seen_now) {
    diag_code = "DATA_IN_FIFO_NO_SYNC_NOW";
    diag_en = "FIFO contains bytes but GDO2 sync is currently idle. This can be normal after sync deasserts; if frequent, inspect GDO2 wiring/timing.";
    diag_pl = "FIFO zawiera bajty, ale GDO2 sync jest teraz nieaktywne. To moze byc normalne po zaniku sync; jesli powtarza sie czesto, sprawdz GDO2 i timing.";
  } else if (sync_seen_now && !has_fifo_data) {
    diag_code = "SYNC_ACTIVE_FIFO_EMPTY";
    diag_en = "GDO2 sync is active but FIFO is empty. Check GDO2 polarity/wiring or RF false triggers.";
    diag_pl = "GDO2 sync jest aktywne, ale FIFO jest puste. Sprawdz polaryzacje/przewod GDO2 albo falszywe wyzwolenia RF.";
  } else if (has_fifo_data) {
    diag_code = "DATA_STAGE_REACHED";
    diag_en = "RX path reached FIFO data stage. If frame later drops, inspect FIFO drain, T1 decode or CRC stage.";
    diag_pl = "Tor RX doszedl do etapu danych w FIFO. Jesli ramka pozniej spadnie, sprawdz oproznianie FIFO, dekodowanie T1 albo CRC.";
    warning = false;
  } else {
    diag_code = "STATE_SANE";
    diag_en = "State looks sane.";
    diag_pl = "Stan wyglada poprawnie.";
    warning = false;
  }

  if (severe) {
    ESP_LOGE(TAG, "CC1101 DIAG result / wynik diag: %s", diag_code);
    ESP_LOGE(TAG, "CC1101 DIAG explanation / wyjasnienie: %s / %s", diag_en, diag_pl);
  } else if (warning) {
    ESP_LOGW(TAG, "CC1101 DIAG result / wynik diag: %s", diag_code);
    ESP_LOGW(TAG, "CC1101 DIAG explanation / wyjasnienie: %s / %s", diag_en, diag_pl);
  } else {
    ESP_LOGI(TAG, "CC1101 DIAG result / wynik diag: %s", diag_code);
    ESP_LOGI(TAG, "CC1101 DIAG explanation / wyjasnienie: %s / %s", diag_en, diag_pl);
  }

  const char *rx_summary = rx_running ? "LISTENING" : (rx_overflow_state ? "FIFO_OVERFLOW" : marc_state_name_(marc));
  const char *data_summary = has_fifo_data ? "DATA_IN_FIFO" : "FIFO_EMPTY";
  const char *event_summary = sync_seen_now ? "SYNC_ACTIVE" : (fifo_threshold_now ? "FIFO_THRESHOLD_ACTIVE" : "NO_RF_EVENT_NOW");
  const bool gdo2_inverted = (iocfg2 & 0x40) != 0;
  const bool gdo0_inverted = (iocfg0 & 0x40) != 0;

  ESP_LOGW(TAG,
           "CC1101 health / ocena: SPI=%s config=%s RX=%s(%s) FIFO=%s overflow=%s event=%s "
           "GDO0=%s/%s%s GDO2=%s/%s%s packet=%s sync=%s",
           ok_bad_(spi_ok), ok_bad_(config_ok), rx_summary, marc_state_name_(marc),
           data_summary, yes_no_(overflow), event_summary,
           gdo_signal_name_(iocfg0), ok_warn_(gdo0_cfg_ok), gdo0_inverted ? "/INVERTED" : "",
           gdo_signal_name_(iocfg2), ok_warn_(gdo2_cfg_ok), gdo2_inverted ? "/INVERTED" : "",
           pkt_length_mode_name_(pktctrl0),
           sync_t1 ? "T1(0x543D)" : (sync_c1 ? "C1(0x54CD)" : "UNKNOWN"));

  ESP_LOGW(TAG,
           "CC1101 plain status / po ludzku: chip=%s, config=%s, radio=%s, fifo_bytes=%u, "
           "sync_pin=%s, fifo_pin=%s, packet_mode=%s, rf_profile=%s",
           spi_ok ? "responds" : "NOT_RESPONDING",
           config_ok ? "OK" : "BAD",
           rx_running ? "listening" : marc_state_name_(marc),
           (unsigned) rxbytes,
           sync_seen_now ? "active" : "idle",
           fifo_threshold_now ? "active" : "idle",
           pkt_infinite ? "OK(infinite)" : "BAD(not infinite)",
           rf_profile_ok ? "OK" : "BAD");

  ESP_LOGW(TAG,
           "CC1101 debug status / status debug: reason=%s PARTNUM=0x%02X VERSION=0x%02X "
           "MARCSTATE=0x%02X(%u) RXBYTES=0x%02X(count=%u overflow=%s) "
           "GDO0=%d GDO2=%d",
           reason ? reason : "unknown", partnum, version,
           marc_raw, (unsigned) marc, rxbytes_raw, (unsigned) rxbytes,
           overflow ? "true" : "false", gdo0, gdo2);

  ESP_LOGW(TAG,
           "CC1101 config snapshot / zrzut konfiguracji: IOCFG2=0x%02X IOCFG0=0x%02X "
           "FIFOTHR=0x%02X PKTCTRL1=0x%02X PKTCTRL0=0x%02X FSCTRL1=0x%02X "
           "MDMCFG4=0x%02X MDMCFG3=0x%02X MDMCFG2=0x%02X DEVIATN=0x%02X "
           "FOCCFG=0x%02X BSCFG=0x%02X AGCCTRL2=0x%02X AGCCTRL1=0x%02X AGCCTRL0=0x%02X "
           "FREND1=0x%02X FSCAL3=0x%02X SYNC=0x%02X%02X",
           iocfg2, iocfg0, fifothr, pktctrl1, pktctrl0, fsctrl1,
           mdmcfg4, mdmcfg3, mdmcfg2, deviatn, foccfg, bscfg, agc2, agc1, agc0,
           frend1, fscal3, sync1, sync0);
}

void CC1101::setup() {
  // CC1101 deliberately does not use the generic reset_pin/irq_pin-only setup.
  // It needs both GDO2(sync) and GDO0(FIFO threshold) for deterministic RX.
  this->spi_setup();
  if (this->gdo2_pin_ != nullptr) this->gdo2_pin_->setup();
  if (this->gdo0_pin_ != nullptr) this->gdo0_pin_->setup();
  this->irq_pin_ = this->gdo2_pin_;

  ESP_LOGI(TAG, "Setup CC1101 experimental RX path: GDO2=sync IRQ, GDO0=FIFO threshold");
  this->reset_cc1101_();
  const uint8_t version = this->read_status_(REG_VERSION);
  if (version == 0x00 || version == 0xFF) {
    ESP_LOGE(TAG, "Invalid CC1101 VERSION=0x%02X. Check SPI wiring / zly odczyt VERSION, sprawdz SPI", version);
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "CC1101 VERSION=0x%02X", version);

  this->apply_radio_profile_();
  this->rf_params_str_ = "CC1101 compat: 868.950MHz DR=100kbps fdev~47kHz RxBW~325kHz infinite-packet";
  if (!this->validate_startup_config_()) {
    this->dump_debug_status("startup_config_error");
    this->mark_failed();
    return;
  }
  this->restart_rx();
  this->dump_debug_status("after_setup");
}

void CC1101::dump_config() {
  ESP_LOGCONFIG(TAG, "Transceiver: %s", this->get_name());
  LOG_PIN("  GDO0/FIFO Pin: ", this->gdo0_pin_);
  LOG_PIN("  GDO2/SYNC Pin: ", this->gdo2_pin_);
  ESP_LOGCONFIG(TAG, "  Frequency: 868.950 MHz");
  ESP_LOGCONFIG(TAG, "  SPI/RX: experimental CC1101, requires GDO0+GDO2");
  ESP_LOGCONFIG(TAG, "  RF profile: compatibility modem profile, infinite packet mode");
  const char *mode_str = (this->listen_mode_ == LISTEN_MODE_T1) ? "T1 only"
                       : (this->listen_mode_ == LISTEN_MODE_C1) ? "C1 only"
                       : "T1+C1 (both, 3:1 sync-cycle bias)";
  ESP_LOGCONFIG(TAG, "  Listen mode: %s", mode_str);
}

void CC1101::restart_rx() {
  uint8_t sync2;
  if (this->listen_mode_ == LISTEN_MODE_T1) {
    sync2 = 0x3D;
  } else if (this->listen_mode_ == LISTEN_MODE_C1) {
    sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
    this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);
  } else {
    sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
    this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);
  }

  this->flush_rx_();
  this->set_sync_word_(sync2);
  this->chunk_len_ = 0;
  this->chunk_idx_ = 0;
  this->rssi_captured_ = false;
  this->last_rssi_dbm_ = -127;
  this->abort_requested_ = false;
  this->strobe_(CC1101_SRX);
}

void CC1101::capture_rssi_() {
  const uint8_t raw = this->read_status_(REG_RSSI);
  int16_t rssi;
  if (raw >= 128) {
    rssi = ((int16_t) raw - 256) / 2 - 74;
  } else {
    rssi = ((int16_t) raw) / 2 - 74;
  }
  this->last_rssi_dbm_ = (int8_t) std::clamp<int16_t>(rssi, -127, 20);
  this->rssi_captured_ = true;
}

optional<uint8_t> CC1101::drain_fifo_once_() {
  if (this->rx_overflow_()) {
    this->fifo_overrun_count_++;
    this->abort_requested_ = true;
    ESP_LOGW(TAG, "RX FIFO overflow / przepelnienie RX FIFO");
    this->flush_rx_();
    return {};
  }

  const uint8_t rx_bytes = this->rxbytes_count_();
  if (rx_bytes == 0) return {};

  if (!this->rssi_captured_) this->capture_rssi_();

  // If sync is still asserted, keep one byte in FIFO while streaming.
  // Near the end / after deassert, drain everything that is left.
  const bool sync_asserted = this->gdo2_pin_ != nullptr && this->gdo2_pin_->digital_read();
  size_t safe = rx_bytes;
  if (sync_asserted && safe > 1) safe -= 1;
  if (safe == 0) return {};

  const size_t n = std::min<size_t>(safe, this->chunk_buffer_.size());
  this->read_burst_(REG_FIFO, this->chunk_buffer_.data(), n);
  this->chunk_len_ = n;
  this->chunk_idx_ = 0;
  return this->chunk_buffer_[this->chunk_idx_++];
}

optional<uint8_t> CC1101::read() {
  if (this->chunk_idx_ < this->chunk_len_) {
    return this->chunk_buffer_[this->chunk_idx_++];
  }

  const int64_t deadline = esp_timer_get_time() + CC1101_READ_POLL_US;
  while (esp_timer_get_time() < deadline) {
    if (auto byte = this->drain_fifo_once_(); byte.has_value()) {
      return byte;
    }
    if (this->abort_requested_) return {};
    esp_rom_delay_us(CC1101_POLL_STEP_US);
  }

  return {};
}

int8_t CC1101::get_rssi() { return this->last_rssi_dbm_; }

bool CC1101::consume_rx_abort_request() {
  const bool abort = this->abort_requested_;
  this->abort_requested_ = false;
  return abort;
}

uint32_t CC1101::take_fifo_overrun_count() {
  const uint32_t count = this->fifo_overrun_count_;
  this->fifo_overrun_count_ = 0;
  return count;
}

const char *CC1101::get_name() { return TAG; }

}  // namespace wmbus_radio
}  // namespace esphome
