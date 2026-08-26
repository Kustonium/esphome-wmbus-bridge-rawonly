# TROUBLESHOOTING_PL.md

[English version](TROUBLESHOOTING.md)

Diagnostyka po objawach dla `wmbus_radio`.

## Złota zasada

Czytaj diagnostykę w tej kolejności:

1. `boot`
2. `summary`
3. `meter_window`
4. `dropped` / `truncated`
5. `rx_path`
6. `suggestion`
7. `busy_ether_changed`

Jeśli pominiesz `meter_window`, łatwo oszukasz samego siebie. Jeśli pominiesz `suggestion` i `busy_ether_changed`, łatwo też przegapisz to, co repo już samo mówi o przyczynie zmiany zachowania SX1276.

## 1. `summary` wygląda dobrze, ale licznik nadal gubi pakiety

Najbardziej prawdopodobne przyczyny:

- `SX1276` odcina złe starty jeszcze przed decode,
- gęste środowisko RF,
- overhead trybu `both`,
- licznik jest na tyle szybki, że straty widać dopiero w statystykach per licznik.

Co sprawdzić:

- `meter_window.win_avg_interval_s`
- `meter_window.count_window`
- `summary.total` względem oczekiwanego interwału licznika
- czy używasz `listen_mode: both`

Wniosek praktyczny:

Czyste `summary` **nie** dowodzi dobrego realnego odbioru. Najpierw ufaj `meter_window`.

## 2. `drop_pct` jest niski, ale realne wyniki są słabe

Najczęściej dotyczy `SX1276` z `adaptive`.

Co to zwykle oznacza:

- straty dzieją się przed decode,
- odrzucone starty nie trafiają do `summary.total`,
- radio wygląda czyściej na papierze niż w rzeczywistości.

Co zrobić:

- patrz na `meter_window`,
- porównaj z `listen_mode: t1` zamiast `both`,
- zostaw `sx1276_busy_ether_mode: adaptive`, chyba że masz dowód, że eter jest spokojny.

## 3. `meter_window.win_avg_interval_s` jest dużo większe niż powinno

To jeden z najmocniejszych dowodów realnej utraty pakietów.

Przykład:

- licznik powinien nadawać co `30 s`,
- `win_avg_interval_s` wynosi około `90 s`.

To znaczy, że odbierasz tylko około jednej trzeciej oczekiwanych pakietów.

Najbardziej prawdopodobne przyczyny:

- częste kolizje,
- busy RF,
- overhead `both`,
- limit `SX1276` pod presją czasu.

## 4. Dużo `false_start_like`, `probe_start_aborted` albo `preamble_read_failed`

Najbardziej prawdopodobne przyczyny:

- zapchany eter,
- słaby nakładający się ruch,
- dalekie liczniki,
- blokowy noise,
- `SX1276` pracujący blisko swojego praktycznego limitu.

Co zrobić:

- na `SX1276` zacznij od `adaptive`,
- unikaj `both`, jeśli nie jest konieczne,
- skup się na `meter_window` dla liczników z `highlight_meters`,
- porównaj dzień i noc.

## 5. Wysokie `dll_crc_failed` przy przyzwoitym RSSI

To zwykle wskazuje na:

- przester,
- multipath,
- lokalne zakłócenia,
- a nie tylko „słaby sygnał”.

Co sprawdzić:

- `summary.avg_ok_rssi`
- `summary.avg_drop_rssi`
- `dropped.stage`
- położenie anteny i lokalne źródła zakłóceń RF

## 6. Dużo `truncated`

To zwykle znaczy, że końcówka ramki nie jest doczytywana czysto.

Możliwe przyczyny:

- kolizje pod koniec ramki,
- presja FIFO / RX,
- słaba końcówka sygnału,
- duża presja czasowa w gęstym środowisku.

Co sprawdzić:

- eventy `truncated` z polami `want`, `got`, `raw_got`,
- czy problem dotyczy konkretnego dużego / częstego licznika,
- czy problem nasila się w dzień.

## 7. `both` działa, ale T1 zrobiło się dużo gorsze

To jest spodziewane w wielu realnych środowiskach.

Dlaczego:

- `both` dokłada overhead przełączania nawet przy małym realnym ruchu C1,
- ten koszt szczególnie boli na `SX1276`.

Co zrobić:

- najpierw porównaj z `listen_mode: t1`,
- jeśli chcesz niezawodnego mixed-mode, użyj dwóch urządzeń,
- przy jednym urządzeniu preferuj `SX1262` zamiast `SX1276`.

## 8. Którego `sx1276_busy_ether_mode` używać?

Zostań przy domyślnym:

```yaml
sx1276_busy_ether_mode: normal
```

`adaptive` i `aggressive` nie poprawiają odbiornika. One **przerywają słabe
starty**, żeby radio nadążyło, gdy naprawdę się przeciąża — a jeśli się nie
przeciąża, płacisz cenę za nic.

Cena jest duża. Zmierzone w gęstej zabudowie 2026-08-23, cztery płytki w jednym
punkcie: przy `adaptive` żadna ramka słabsza niż **−84 dBm** nie przeszła w ogóle,
a płytka słyszała **27** liczników; przy `normal` ramki docierały aż do **−97 dBm**
i słyszała **53**. Próg przerwania jest zaciśnięty klamrą przy −86 dBm, a
`adaptive` dopycha go do tej klamry — więc efektem jest twarda podłoga czułości,
a nie płynny kompromis.

Podnoś dopiero przy **zmierzonym** przeciążeniu, a nie wtedy, gdy w eterze wydaje
się tłoczno:

- `fifo_overrun` > 0 albo `truncated` > 0,
- **i** realne straty w `drop_pct`.

Sam wysoki `false_start_like` nie jest powodem: liczy wyzwolenia na szumie, a na
powyższej płytce przez cały dzień wynosił około 60/min przy `fifo_overrun` równym
zero.

Gdy już spróbujesz, oceniaj po **liczbach per licznik przed i po**, a nie po
`drop_pct`. `drop_pct` poprawia się sam z siebie, bo ramki, które policzyłby jako
odrzucone, nie są już w ogóle próbowane.

`aggressive` to ustawienie do świadomych testów, nie na co dzień.

## 9. Potrzebuję sensownego profilu diagnostycznego

Typowy bezpieczny profil:

```yaml
listen_mode: t1
diagnostic_mode: normal
highlight_meters:
  - "12345678"

# Opcjonalnie, tylko jeśli chcesz ograniczyć szczegółowe eventy do highlight_meters:
# diagnostic_events_highlight_only: true

# Tylko SX1276:
sx1276_busy_ether_mode: adaptive
```

## 10. Tryb S1 nic nie odbiera

Najpierw sprawdź założenie: `listen_mode: s1` to dedykowany profil RF tylko dla S1. Nie jest częścią `both`.

Domyślne częstotliwości:

- `t1`, `c1`, `both` -> `868.950 MHz`
- `s1` -> `868.300 MHz`

Jeżeli testujesz urządzenia, które mogą używać przesuniętej częstotliwości S-mode, nadpisz ją jawnie:

```yaml
listen_mode: s1
frequency: 868.36
```

Jeżeli poprawne telegramy S1 zostaną odebrane, trafią do MQTT tak samo jak telegramy T1/C1. Jeśli nie pojawia się nic, prawdopodobne przyczyny to:

- urządzenie nie nadaje standardowego pasywnego S1,
- urządzenie używa systemu zamkniętego albo odpytywanego,
- rzeczywista częstotliwość jest inna,
- urządzenie nadaje bardzo rzadko,
- antena albo lokalizacja są słabe.

Nie debuguj driverów liczników ani kluczy AES, dopóki ESP nie publikuje poprawnych telegramów do MQTT.

## 11. Najkrótsza ścieżka decyzji

- użyj `SX1262`, jeśli zależy Ci na niezawodności,
- używaj `SX1276` tylko wtedy, gdy środowisko jest łatwiejsze albo ruch wolniejszy,
- nie ufaj samemu `summary`,
- dla środowisk mieszanych T1/C1 dwa dedykowane urządzenia są lepsze niż jeden setup `both`.

## 12. Radio jest aktywne, ale nie ma linii `Have data`

Nie zaczynaj od MQTT ani backendu.

Najpierw sprawdź, czy ESP lokalnie widzi jakiekolwiek ramki radiowe:

```text
Have data / odebrano dane (...)
```

Jeżeli tej linii nie ma, problem nadal jest w warstwie RF / konfiguracji płytki.

Dla `SX1262` przeczytaj raport sanity w logu startowym. Na płytkach z TCXO, takich jak Heltec WiFi LoRa 32 V4, brak:

```yaml
has_tcxo: true
```

może nadal pozwolić na inicjalizację radia i wypisanie `Radio active`, ale RX może być całkowicie martwy.

Sprawdź też opcje sprzętowe płytki:

```yaml
dio2_rf_switch: true
long_gfsk_packets: true
rx_gain: boosted
```

Dla płytek z zewnętrznym FEM sprawdź również piny `fem_*`, a dla modułów bramkujących tor antenowy — `rf_sw_pin` (patrz sekcja 13). `dio2_rf_switch` odpowiada wyłącznie za kierunek TX/RX i nie zastępuje tej bramki.

Dla `SX1276` zwykłe płytki nie wymagają `tcxo_pin`. Warianty TCXO, na przykład LILYGO T3 V3.0 TCXO OLED LoRa32, wymagają jawnego pinu TCXO enable:

```yaml
tcxo_pin: GPIO12
```

Komponent nie wykrywa okablowania płytki. Sprawdź schemat albo dokumentację producenta.

## 13. Ramki są, ale mało — i wszystkie RSSI w wąskim paśmie

Ten objaw jest podstępny, bo nic nie wygląda na zepsute. Ramki się dekodują, `dropped` jest niski, `DIAG hint` raportuje `GOOD`. Tylko liczników jest kilka zamiast kilkudziesięciu.

Rozstrzyga **rozkład RSSI, a nie liczba ramek**.

Zbierz kilkanaście minut i porównaj najsilniejszy odczyt z najsłabszym:

```text
-93, -94, -95, -96, -97, -98 dBm      → pas 5 dB, wszystko tuż nad progiem
-58, -64, -71, -76, -80, -87 dBm      → rozrzut 29 dB, zdrowy tor
```

Czułość SX1262 przy 100 kbps to rząd -105 dBm. Wąski pas przyklejony do progu **nie** oznacza, że w okolicy jest mało liczników — gdyby tak było, wartości byłyby rozproszone. Oznacza, że słychać wyłącznie to, co ledwo przekracza granicę, a wszystko poniżej znika bez śladu. To jest odcisk palca odbiornika obciętego czułością.

Kolejność sprawdzania:

1. **Bramka przełącznika RF.** Jeśli moduł jej wymaga, a nie jest sterowana, tracisz około 30 dB. Na XIAO ESP32-S3 + Wio-SX1262:

   ```yaml
   rf_sw_pin: GPIO38
   ```

   Log startowy mówi wprost, czy jest sterowana:

   ```text
   RF switch gate / bramka przelacznika RF: driven high (rf_sw_pin) / sterowana
   ```

   `not configured / nieskonfigurowana` na płytce, która tego wymaga, jest odpowiedzią.

2. **Nie steruj tego wyprowadzenia z YAML-a akcją `on_boot`.** Konstrukcja poniżej przechodzi walidację, kompiluje się, nie generuje ostrzeżenia i **nie działa** — priorytet 900 trafia w ten sam etap inicjalizacji co sam komponent `gpio output`, więc zapis wykonuje się, zanim wyprowadzenie stanie się wyjściem:

   ```yaml
   # NIE tak:
   esphome:
     on_boot:
       priority: 900
       then:
         - output.turn_on: lora_rf_sw
   ```

   Jeśli masz to w konfiguracji, usuń wraz z blokiem `output:` — pozostawienie obok `rf_sw_pin` powoduje odrzucenie konfiguracji z powodu podwójnej deklaracji pinu.

3. **Piny FEM**, jeśli płytka ma zewnętrzny front-end (Heltec V4: `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin`).

4. **Antena** — złącze, pigtail, pasmo. Sprawdzaj po powyższych, nie przed.

Miarą kontrolną po naprawie jest `meter_window.win_avg_interval_s` dla znanego licznika: równy rzeczywistemu interwałowi nadawania oznacza, że nie gubisz transmisji (patrz sekcja 3).

## 14. `task stack overflow` w logach (XIAO i podobne płytki)

Objaw: w logu serial pojawia się panic / komunikat FreeRTOS w stylu `Task stack overflow`, zwykle po włączeniu cięższej diagnostyki albo po aktualizacji do buildu z większą liczbą liczników.

Odbiornik `wmbus_radio` działa we własnym tasku RTOS (oddzielnym od głównej pętli ESPHome), więc `loop_task_stack_size` z ESPHome nie ma na niego wpływu. Mniejsze płytki — w szczególności XIAO ESP32-S3 — potrafią działać bez problemu na starszych buildach i przepełnić ten stos po przejściu na nowszy build z większą liczbą diagnostyk.

Opcja YAML:

```yaml
wmbus_radio:
  receiver_task_stack_size: 4096
```

Domyślnie `3072` bajty. Dozwolony zakres `2048..16384`. Jeśli widzisz stack overflow na XIAO lub innej małej płytce, spróbuj kolejno `4096`, `6144`, `8192` — zwiększaj tylko tyle, ile faktycznie potrzebne.

## 15. MQTT leży, ale radio powinno dalej działać

Problemy MQTT są problemami transportu, a nie dowodem awarii RF.

Jeżeli broker jest niedostępny, hasło jest złe, zdalny broker jest nieosiągalny albo negocjacja TLS się nie powiedzie, klient MQTT ESPHome może wypisywać błędy. `wmbus_radio` powinien nadal odbierać radio i logować ramki lokalnie.

Oczekiwane zachowanie:

```text
Have data / odebrano dane (...)
MQTT unavailable / MQTT niedostepny: skip telegram publish ... radio reception continues
```

TLS, certyfikaty, fingerprinty i szczegóły zdalnego brokera należą do standardowej sekcji `mqtt:` ESPHome. Nie konfiguruje się ich w `wmbus_radio`.

Jeżeli lokalne linie `Have data` są widoczne, ale backend nic nie odbiera, najpierw debuguj MQTT. Jeżeli nie ma linii `Have data`, najpierw debuguj radio i konfigurację płytki.

## 16. Płytka LR1121 wygląda na martwą albo S1 jest gorsze, niż powinno

Trzy usterki na tej płytce dają ciszę bez żadnego błędu, więc sprawdź je, zanim
zaczniesz podejrzewać sterownik:

- **Złe gniazdo u.FL.** Płytka Waveshare HF ma ich kilka: front end WiFi ESP32,
  port 2,4 GHz LR1121 i tor sub-GHz przez przełącznik RF. Tylko ten ostatni
  odbiera wM-Bus.
- **Brak logu na konsoli.** Na płytce nie ma mostka USB-UART — USB-C idzie wprost
  do natywnych pinów USB ESP32-S3. `logger:` wymaga
  `hardware_uart: USB_SERIAL_JTAG`.
- **`tcxo_voltage: 1.8v`.** Układ zgłasza wtedy `HF_XOSC_START` i nie dochodzi do
  odbioru. Użyj `3.0v` — ta wartość jest zmierzona na tej płytce.

Sam `HF_XOSC_START` w logu startowym **nie jest** usterką. Zatrzaskuje się przy
wejściu w `STDBY_XOSC`; jeżeli linie kalibracji po nim pokazują
`XOSC=0x0020 IMAGE=0x0000 ALL=0x0000`, radio jest sprawne i sterownik to napisze.

Ramki przychodzą, ale są ucięte: podnieś `payload_length` — ramki NES mają 245
bajtów surowych, więc wartością roboczą jest `255`.

Przy S1 pamiętaj, że kolejność radiów jest inna niż przy T1: układem sprawdzonym
przy progu szumu S1 jest `SX1276`. Patrz [`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md).

## 17. Płytka restartuje się co 15 minut

Objaw: odbiór działa, po czym cyklicznie ustaje i rusza od nowa. Czas pracy nigdy
nie przekracza kwadransa, a liczniki zerowane przy starcie pokazują absurdy.

Przyczyna, jeśli YAML ma samo `api:`, a płytka publikuje przez MQTT: ESPHome
przyjmuje domyślnie `api.reboot_timeout: 15min`, a ten licznik restartuje
urządzenie, gdy nie jest podłączony żaden *klient* Native API. Samodzielny
odbiornik MQTT zwykle takiego klienta nie ma, więc restartuje się w kółko.

Poprawka — każdy przykład w tym repo już ją zawiera:

```yaml
api:
  reboot_timeout: 0s
```

`api:` zostaje, więc Native API i `time: platform: homeassistant` nadal działają;
wyłączony jest wyłącznie watchdog.

Jak to potwierdzić, zamiast zakładać: przy włączonym temacie metadanych odczytaj
`boot_id` i `seq` z `wmbus/<topic_name>/rx`. Restartująca się płytka pokazuje nowy
`boot_id` mniej więcej co 900 s; zdrowa trzyma jeden `boot_id`, a `seq` rośnie
przez granice 15 i 30 minut. Zmierzone tutaj 2026-08-20/21: 51 różnych `boot_id`
na płytkę w ciągu nocy przed zmianą, jeden `boot_id` przez 14,1 h po niej.

`mqtt.reboot_timeout` to inny mechanizm, reagujący na utratę brokera. Nie zmieniaj
go przy okazji tej poprawki.

## 18. Dodatek pisze „brak znacznika czasu", chociaż płytka ma `time:` w YAML

Objaw: panel diagnostyczny dodatku pokazuje **`Zegar ESP: brak znacznika czasu`**
dla płytki, w której YAML wyraźnie widać blok `time:`, więc firmware „ma
zegar wkompilowany". Panel nie kłamie — liczy ramki, w których było pole
`received_at`, a ta płytka publikuje `/rx` bez niego.

Przyczyna jest niemal zawsze ta sama: YAML używa `time: - platform:
homeassistant` **samotnie**, a Native API nie jest połączone. Ta platforma
bierze czas z HA po API, więc jeśli integracja ESPHome nie sparowała
urządzenia (albo API padło), zegar systemowy zostaje na epoce. Firmware
odmawia stemplowania ramek datą starszą niż 2020-09-13 — stempel „1970"
jest gorszy niż brak stempla, więc pole jest pomijane w całości.

Rozwiązanie: dopisz SNTP jako pierwszy, HA time jako fallback. SNTP działa
z samego WiFi, niezależnie od tego, czy HA widzi płytkę.

```yaml
time:
  - platform: sntp
    id: sntp_time
    servers:
      - pl.pool.ntp.org
      - 0.pool.ntp.org
  - platform: homeassistant
    id: ha_time
```

Po przebudowie i wgraniu przez jedną sesję dodatku będziesz widzieć
**`Zegar ESP: częściowo stemplowane`**: radio zawsze startuje szybciej,
niż NTP odpowie, więc kilka pierwszych ramek leci bez stempla i zostają
w liczniku sesji, dopóki się nie zresetuje. Po restarcie dodatku (z SNTP
już działającym na płytce) powinno być `zsynchronizowany`.
