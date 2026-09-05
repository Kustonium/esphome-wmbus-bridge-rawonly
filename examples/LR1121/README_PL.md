# LR1121 — odbiór działa od 2026-08-19

[English version](README.md)

Sterownik (`components/wmbus_radio/transceiver_lr1121.{h,cpp}`) powstał
wyłącznie z dokumentacji, bez płytki na biurku. Od
2026-08-19 odbiera prawdziwe telegramy na tym sprzęcie: liczniki BMT i NES,
17 poprawnych ramek na minutę, RSSI -57..-96 dBm, bez obcięć,
z komunikatem `RF link looks stable`.

C1 też działa — jedna ramka C1 A od Techema obok ruchu T1 potwierdza
przełączanie słów synchronizacji w proporcji 3:1. S1 również odbiera,
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

## Płytka

Waveshare ESP32-S3-LR1121-HF (SKU 34011): ESP32-S3, 4 MB flash, 2 MB PSRAM,
Semtech LR1121 z TCXO na płytce. Sama płytka nie ma złącza USB — zestaw Kit
zawiera adapter Type-C i taśmę FFC, bez których nie można jej zaprogramować.

| Sygnał | GPIO |
|---|---|
| SCK | 40 |
| MOSI | 45 |
| MISO | 46 |
| CS | 42 |
| RESET | 39 |
| BUSY | 41 |
| IRQ | 38 |

Źródło: wiki Waveshare i `src/wavesahre_lora_1121.h` z pakietu producenta
(literówka w nazwie pliku pochodzi od producenta), porównane z połączeniami
na schemacie i tabelą 4-1 dokumentacji LR1121.

GPIO45/46 to piny konfiguracji startowej ESP32-S3. Nie powinny powodować problemu:
LR1121 zwalnia MISO, gdy CS jest w stanie wysokim (§3.6.3 dokumentacji).
Są jednak pierwszymi podejrzanymi, jeśli płytka odmówi startu.

## Konsola szeregowa

USB-C prowadzi bezpośrednio do natywnych pinów USB ESP32-S3 (GPIO19/GPIO20,
wyprowadzenia układu 25/26, rezystory szeregowe do złącza J3). Płytka nie ma
układu mostka USB-UART. Wynikają z tego dwie pułapki, przez które sprawna
płytka może wyglądać na martwą:

- **ESPHome:** `logger:` wymaga `hardware_uart: USB_SERIAL_JTAG`. Domyślny UART
  nie jest wyprowadzony do dostępnego złącza. Jeśli USB się nie zgłasza,
  `USB_CDC` jest drugim interfejsem na tych samych pinach.
- **Arduino IDE:** *Tools → USB CDC On Boot* musi mieć wartość **Enabled**,
  inaczej `Serial` kieruje dane do UART0 i nic się nie pojawia.

Potwierdzone na podstawie schematu i pliku `platformio.ini` Meshtastic
z pakietu producenta (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`).

## Co sprawdzić, zanim uznasz sterownik za przyczynę

1. **Gniazdo antenowe.** Płytka ma kilka gniazd u.FL: tor WiFi ESP32,
   port 2,4 GHz LR1121 oraz tor sub-GHz przez przełącznik RF. Złe gniazdo
   daje całkowitą ciszę bez żadnego błędu — tak samo jak uszkodzony sprzęt.
2. **`tcxo_voltage`.** Pakiet producenta podaje dwie wartości dla tej samej
   płytki: 3,0 V we wszystkich trzynastu przykładach C i 1,8 V w dołączonym
   wariancie Meshtastic. Uruchomienie Waveshare HF na sprzęcie potwierdziło
   3,0 V: przy 1,8 V układ zgłasza `HF_XOSC_START`, przy 3,0 V dochodzi do
   odbioru. Dlatego przykład Waveshare i komponent domyślnie używają 3,0 V.
3. **Log startowy.** Sterownik loguje `hw/type/fw` z GetVersion i opisuje
   bity błędów nazwami. Same zera albo jedynki w odpowiedzi GetVersion
   oznaczają błędne połączenie SPI lub BUSY; dalsze kroki nie zadziałają.

## Skąd pochodzą wartości rejestrów

Sterownik opiera się na dokumentacji, ale udokumentowanie wartości nie
oznacza jeszcze potwierdzenia jej pomiarem:

| Ustawienie | Źródło |
|---|---|
| Kody komend | sterownik Semtech LR11xx z pakietu Waveshare |
| Protokół odczytu w dwóch transakcjach | `lr11xx_hal.c` z tego samego pakietu |
| Mapowanie DIO → RFSW / SPI | tabela 4-1 dokumentacji LR1121, potwierdzona na schemacie |
| Tabela przełącznika RF (RX→RF2, TX→RF1) | tabela prawdy RichWave RTC6603SP, schemat i dołączony `rfswitch.h` Meshtastic, zgodny bit po bicie |
| Kalibracja obrazu 863–870 MHz | dokumentacja; **przykłady producenta mają aktywną parę 430–440 MHz we wszystkich 13 plikach konfiguracji** |
| TCXO przed kalibracją | §1.2.4 dokumentacji: z TCXO układ pomija kalibrację przy włączeniu, więc host musi ją powtórzyć; przykłady producenta kalibrują wcześniej |
| Oczekiwana czułość | tabela 3-8: −103,5 dBm przy 250 kb/s, −105 ze wzmocnieniem; przeliczenie na 100 kb/s sugeruje około −106…−109 dBm, minus ~0,34 dB na przełączniku RF |

## Niezrealizowane

Skanowanie widma. LR1121 potrafi przemiatać 150–960 MHz z szybkim odczytem
RSSI. To osobne zastosowanie od odbioru wM-Bus i wymaga osobnego trybu:
skanowanie oraz odbiór wykluczają się — radio albo mierzy pasmo, albo nasłuchuje.
