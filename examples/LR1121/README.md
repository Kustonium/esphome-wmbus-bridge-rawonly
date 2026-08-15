# LR1121 — experimental, never run on hardware

**EN.** Every other radio in this repository was tuned against real meters. This
one has not been powered on even once. The driver
(`components/wmbus_radio/transceiver_lr1121.{h,cpp}`) was written from
documentation, and the YAML here describes a board that was still in the post
when it was authored. Treat it as a starting point for someone with the board in
hand, not as support.

**PL.** Każde inne radio w tym repozytorium było strojone na prawdziwych
licznikach. To nie zostało ani razu włączone. Sterownik powstał z dokumentacji,
a przykładowy YAML opisuje płytkę, która w chwili pisania była jeszcze w drodze.
To punkt startowy dla kogoś, kto ma płytkę w ręku — nie wsparcie.

## Board

Waveshare ESP32-S3-LR1121-HF (SKU 34011). ESP32-S3, 4 MB flash, 2 MB PSRAM,
Semtech LR1121 with an on-board TCXO. No USB connector on the board itself — the
Kit ships a Type-C adapter and an FFC ribbon, and without them there is no way
to flash it.

| Signal | GPIO |
|---|---|
| SCK | 40 |
| MOSI | 45 |
| MISO | 46 |
| CS | 42 |
| RESET | 39 |
| BUSY | 41 |
| IRQ | 38 |

Source: Waveshare wiki and `src/wavesahre_lora_1121.h` from the vendor package
(the typo in the filename is theirs), cross-checked against the schematic
netlist and the LR1121 datasheet Table 4-1.

GPIO45/46 are ESP32-S3 strapping pins. Expected to be harmless — the LR1121
releases MISO while CS is high (datasheet §3.6.3) — but they are the first
suspects if the board ever refuses to boot.

## Serial console

The USB-C goes straight to the ESP32-S3's native USB pins (GPIO19/GPIO20, chip
pins 25/26, series resistors to the J3 header). There is no USB-serial bridge
chip on this board. Two consequences, and both produce a board that looks dead
while working perfectly:

* **ESPHome:** `logger:` needs `hardware_uart: USB_SERIAL_JTAG`. The default
  UART is not wired anywhere reachable. (If it fails to enumerate, `USB_CDC` is
  the other peripheral on the same two pins.)
* **Arduino IDE:** *Tools → USB CDC On Boot* must be **Enabled**, otherwise
  `Serial` targets UART0 and nothing appears.

Confirmed from the schematic netlist and from the vendor's own Meshtastic
`platformio.ini` in the same package (`ARDUINO_USB_MODE=1`,
`ARDUINO_USB_CDC_ON_BOOT=1`).

## What to check before blaming the driver

1. **Antenna socket.** The board has more than one u.FL socket: one is the
   ESP32's WiFi front end, one is the LR1121's 2.4 GHz port, and only one is the
   sub-GHz path through the RF switch. The wrong socket gives perfect silence
   with no error anywhere — indistinguishable from dead hardware.
2. **`tcxo_voltage`.** The vendor package states two different values for the
   same board (3.0 V in all thirteen C examples, 1.8 V in the bundled Meshtastic
   variant) and neither is a measurement. The default here is the low one
   because that failure is loud: the chip reports `HF_XOSC_START` in the boot
   error log and the driver spells out what it means. Too high a voltage starts
   the oscillator anyway, out of spec, silently.
3. **Boot log.** The driver logs `hw/type/fw` from GetVersion and decodes the
   error word by name. If GetVersion answers all-zeros or all-ones, SPI or BUSY
   is mis-wired and nothing further will work.

## Where the register values come from

Nothing in the driver is a guess, but "documented" is not "verified":

| Setting | Source |
|---|---|
| Command opcodes | Semtech LR11xx driver in the Waveshare package |
| Two-transaction read protocol | `lr11xx_hal.c` from the same package |
| DIO → RFSW / SPI mapping | LR1121 datasheet Table 4-1, confirmed on the schematic netlist |
| RF switch table (RX→RF2, TX→RF1) | RichWave RTC6603SP datasheet truth table + schematic + the bundled Meshtastic `rfswitch.h`, which agrees bit for bit |
| Image calibration 863–870 MHz | Datasheet; **the vendor examples ship the 430–440 MHz pair active instead, in all 13 config files** |
| TCXO before calibration | Datasheet §1.2.4 — with a TCXO the chip skips power-on calibration and the host must redo it. The vendor examples calibrate first. |
| Sensitivity expectations | Datasheet Table 3-8: −103.5 dBm at 250 kb/s, −105 boosted; scaled to 100 kb/s that suggests roughly −106…−109 dBm, minus ~0.34 dB for the RF switch |

## Not implemented

Spectrum scanning. The LR1121's most interesting property for this project is
not that it is another wM-Bus receiver — it is barely better than an SX1262 at
that — but that it can sweep 150–960 MHz with a fast RSSI read. That belongs in
a separate mode, not in a receiver, because scanning and receiving are mutually
exclusive: the radio either measures the band or listens to it.
