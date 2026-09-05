# LR1121 — receiving since 2026-08-19

**EN.** The driver (`components/wmbus_radio/transceiver_lr1121.{h,cpp}`) was
written entirely from documentation, with no board on the desk. It has since
decoded real telegrams on this exact hardware: BMT and NES meters, 17 good
frames per minute, RSSI -57..-96 dBm, no truncations, `RF link looks stable`.

C1 works as well - a Techem C1 A frame decoded alongside the T1 traffic, which
exercises the 3:1 sync-word cycling. S1 receives too, verified on 2026-08-19
against a workshop transmitter.

Weakest successful decode so far: **-114 dBm**, measured over a 14.1 h run on
2026-08-21 and read from the `/api/esp-rx` export rather than off a log screen.
That run also produced 1401 frames at -105 dBm or below. The datasheet-derived
estimate for this bitrate was -106..-109 dBm, so the front end is doing slightly
better than predicted, not worse.

The missing comparison has since been made, and it is worth knowing before you
build around this board. Running for weeks beside four other receivers in one
flat, it heard 48 meters over 12 hours where a T-Beam SX1262 on an identical
10 cm antenna heard 113. On an attenuator bench with a shared input it was the
first to fall silent. Swapping antennas between the two ruled the antenna out.

Not a fault, and the counters say why: it converts 80% of its receiver triggers
into frames - the best of the five - while triggering least often. Conservative
receivers waste less and hear less.

C-mode Format B has still never been seen here. Treat this as a working starting
point, not as a supported configuration.

**PL.** Sterownik powstał wyłącznie z dokumentacji, bez płytki na biurku. Od
2026-08-19 odbiera prawdziwe telegramy na tym sprzęcie: liczniki BMT i NES,
17 poprawnych ramek na minutę, RSSI -57..-96 dBm, bez obcięć.

C1 też działa — jedna ramka C1 A od Techema obok ruchu T1. S1 również odbiera,
sprawdzone 2026-08-19 na nadajniku warsztatowym.

Najsłabszy udany odbiór: **−114 dBm**, zmierzone w biegu 14,1 h dnia 2026-08-21
i odczytane z eksportu `/api/esp-rx`, a nie z ekranu logu. W tym samym biegu było
1401 ramek na poziomie −105 dBm i niżej — czyli lepiej niż oszacowanie
−106…−109 dBm z datasheetu.

Brakujące porównanie zostało w międzyczasie zrobione i warto je znać, zanim
zbudujesz coś wokół tej płytki. Chodząc tygodniami obok czterech innych
odbiorników w jednym mieszkaniu, usłyszała przez 12 godzin 48 liczników tam,
gdzie T-Beam na SX1262 przy identycznej antenie 10 cm usłyszał 113. Na stanowisku
tłumikowym ze wspólnym wejściem jako pierwsza zamilkła. Zamiana anten między nimi
wykluczyła antenę.

To nie usterka, a liczniki mówią dlaczego: zamienia 80% wyzwoleń odbiornika
w ramki — najlepiej z piątki — wyzwalając się przy tym najrzadziej. Ostrożne
odbiorniki mniej marnują i mniej słyszą.

Formatu B C-mode nadal tu nie widziano. Traktuj to jako działający punkt wyjścia,
a nie konfigurację wspieraną.

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
   variant). Hardware bring-up of the Waveshare HF board selected 3.0 V: at
   1.8 V the chip reports `HF_XOSC_START`, while 3.0 V reaches the receive path.
   The Waveshare example and component default therefore use 3.0 V.
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
