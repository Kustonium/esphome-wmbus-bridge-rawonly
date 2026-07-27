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

Zacznij od:

```yaml
sx1276_busy_ether_mode: adaptive
```

Zostań przy `adaptive`, jeśli:

- mieszkasz w bloku,
- widzisz dużo false startów,
- `meter_window` jest gorszy niż sugeruje `summary`,
- jeszcze nie wiesz, jak spokojny jest eter.

`normal` testuj dopiero wtedy, gdy:

- masz mało liczników,
- eter jest spokojny,
- `meter_window` już wygląda stabilnie.

`aggressive` traktuj jako ustawienie specjalne do testów, nie domyślne.

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
