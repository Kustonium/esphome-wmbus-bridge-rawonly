# Fix: XIAO with Wio-SX1262 received through a disabled antenna switch

## EN

### Fixed
- The Seeed Wio-SX1262 does not connect its antenna unconditionally. Module pin 1 (`RF_SW`, "External IO control internal gate RF switch") must be held high by the host; on the XIAO ESP32S3 kit it is GPIO38. Nothing drove it, so the pin idled as a high-impedance input and the receiver ran on leakage alone.
- This is separate from `dio2_rf_switch`, and both are needed. Per the module datasheet the SX1262's own DIO2 chooses the TX/RX *direction* (high = TX, low = RX); `RF_SW` decides whether the switch conducts at all.
- The XIAO examples did carry a workaround - an `on_boot` action toggling a `gpio` output on GPIO38 - and it never worked. Priority 900 lands in the same ESPHome setup bucket as the `gpio output` component itself, so ordering falls out of registration order and the write happened before the pin was an output. No warning, no error, no log line. It has been removed from both examples.
- Measured on hardware, same board and antenna before and after: meter `00089907` went from -96 dBm to -68 dBm, and the receiver went from 4-6 frames per minute across 3 meters to 14 across 32.

### Added
- `rf_sw_pin` for `SX1262`. The pin is driven high inside the radio's own setup, before the chip reset, where ordering is guaranteed. Boards whose module gates the antenna path this way need it; Heltec V3/V4/V4-R8 do not - they use the `fem_*` pins and are unaffected.
- The XIAO example joined the CI build matrix. That board had never been compiled in CI, and it is the only config exercising the new option.

### Notes
- **Action required on XIAO ESP32S3 + Wio-SX1262.** Add `rf_sw_pin: GPIO38` and delete any earlier `output:` / `on_boot:` block driving GPIO38 - leaving both makes ESPHome reject the config on a duplicate pin declaration. Without the option the receiver keeps running roughly 30 dB deaf.
- The symptom is not silence. Frames still decode, `DIAG hint` still reports `GOOD`; there are simply several times fewer of them and every RSSI sits in a narrow band just above the sensitivity floor.

## PL

### Naprawiono
- Moduł Seeed Wio-SX1262 nie podłącza anteny bezwarunkowo. Wyprowadzenie nr 1 modułu (`RF_SW`, "External IO control internal gate RF switch") musi być trzymane w stanie wysokim przez host; w zestawie z XIAO ESP32S3 jest to GPIO38. Nic go nie sterowało, więc pin pozostawał wejściem w stanie wysokiej impedancji, a odbiornik pracował wyłącznie na przecieku sygnału.
- To jest coś innego niż `dio2_rf_switch` i potrzebne są oba. Zgodnie z notą katalogową modułu DIO2 układu SX1262 wybiera *kierunek* TX/RX (wysoki = TX, niski = RX), natomiast `RF_SW` decyduje, czy przełącznik w ogóle przewodzi.
- Przykłady dla XIAO zawierały obejście - akcję `on_boot` przełączającą wyjście `gpio` na GPIO38 - i nigdy ono nie działało. Priorytet 900 trafia w tym samym etapie inicjalizacji ESPHome co sam komponent `gpio output`, więc o kolejności decyduje kolejność rejestracji i zapis wykonywał się, zanim wyprowadzenie stało się wyjściem. Bez ostrzeżenia, bez błędu, bez śladu w logu. Obejście zostało usunięte z obu przykładów.
- Pomiar na sprzęcie, ta sama płytka i antena przed i po: licznik `00089907` z -96 dBm na -68 dBm, a odbiornik z 4-6 ramek na minutę od 3 liczników na 14 od 32.

### Dodano
- `rf_sw_pin` dla `SX1262`. Wyprowadzenie ustawiane jest w stan wysoki wewnątrz inicjalizacji radia, przed resetem układu, gdzie kolejność jest jednoznaczna. Opcja jest potrzebna płytkom, których moduł bramkuje w ten sposób tor antenowy; Heltec V3/V4/V4-R8 jej nie wymagają - korzystają z wyprowadzeń `fem_*` i zmiana ich nie dotyczy.
- Przykład dla XIAO dołączył do macierzy kompilacji CI. Ta płytka nie była dotąd objęta kompilacją weryfikacyjną, a jest jedyną konfiguracją używającą nowej opcji.

### Uwagi
- **Wymagane działanie na XIAO ESP32S3 + Wio-SX1262.** Dodaj `rf_sw_pin: GPIO38` i usuń wcześniejszy blok `output:` / `on_boot:` sterujący GPIO38 - pozostawienie obu powoduje odrzucenie konfiguracji przez ESPHome z powodu podwójnej deklaracji wyprowadzenia. Bez tej opcji odbiornik nadal pracuje z czułością niższą o około 30 dB.
- Objawem nie jest cisza. Ramki nadal się dekodują, `DIAG hint` nadal raportuje `GOOD`; jest ich tylko kilka razy mniej, a każde RSSI leży w wąskim paśmie tuż nad progiem czułości.

---

# Fix: SX1262 reported the noise floor as every frame's RSSI

## EN

### Fixed
- Every received frame reported the same near-floor RSSI regardless of meter or distance, because the level was read after the transmission had already ended. Both receive paths fell back to an instantaneous `GetRssiInst` that ran after `RX_DONE` and after the whole buffer had been drained, so it measured the empty channel. The constant value was not a placeholder - it was a real measurement of the noise floor.
- In the streamed path (AN1200.53) that fallback was taken every time: the procedure deliberately never lets the packet engine finish a packet, so `GetPacketStatus` never latches values for the frame being captured.
- The driver also preferred the wrong register. `RssiAvg` averages over the whole receive window, which is a fixed 255 bytes, so for a 134-byte wM-Bus frame more than half of it is post-transmission noise. Measured on one frame: `RssiSync` -97 dBm against `RssiAvg` -117 dBm.
- RSSI is now sampled while the frame is still on air - a peak-hold taken as bytes arrive in the streamed path, `RssiSync` in the FIFO path. The post-frame instantaneous read is gone.
- Scaling was written as `-((int) raw) >> 1` instead of `-raw / 2`; unary minus binds tighter than the shift, so it arithmetic-shifted a negative value and floored instead of truncating.
- A frame whose level cannot be sampled now reports -127 dBm ("not measured") instead of a fabricated value, and such samples are excluded from the per-meter and diagnostic averages rather than dragging them down.

### Notes
- **Historical RSSI data is not comparable with new data** on any SX1262 board. Per-meter averages, `win_avg_rssi` and every RSSI-derived diagnostic need to be re-collected after the update.
- The first frame on each receive path logs the raw radio values at INFO, so the active source can be confirmed on hardware without raising the log level.

## PL

### Naprawiono
- Każda odebrana ramka raportowała tę samą wartość RSSI tuż nad progiem szumu, niezależnie od licznika i odległości, ponieważ poziom odczytywany był po zakończeniu transmisji. Obie ścieżki odbioru schodziły na chwilowy pomiar `GetRssiInst`, wykonywany po `RX_DONE` i po wyczytaniu całego bufora, więc mierzący pusty kanał. Stała wartość nie była zastępnikiem - była rzeczywistym pomiarem szumu tła.
- W ścieżce strumieniowej (AN1200.53) ta droga zapasowa wykonywała się zawsze: procedura celowo nie pozwala silnikowi pakietowemu zakończyć pakietu, więc `GetPacketStatus` nigdy nie zatrzaskuje wartości dla przechwytywanej ramki.
- Sterownik preferował też niewłaściwy rejestr. `RssiAvg` uśrednia po całym oknie odbioru, które wynosi stałe 255 bajtów, więc dla ramki wM-Bus o długości 134 bajtów ponad połowa to szum po zakończeniu transmisji. Pomiar na jednej ramce: `RssiSync` -97 dBm wobec `RssiAvg` -117 dBm.
- RSSI jest teraz próbkowane w trakcie trwania ramki - z zapamiętaniem maksimum w miarę napływania bajtów w ścieżce strumieniowej, oraz z `RssiSync` w ścieżce FIFO. Pomiar chwilowy po ramce został usunięty.
- Skalowanie zapisane było jako `-((int) raw) >> 1` zamiast `-raw / 2`; unarny minus wiąże silniej niż przesunięcie, więc dawało to przesunięcie arytmetyczne wartości ujemnej i zaokrąglanie w dół zamiast obcinania.
- Ramka, dla której nie da się zmierzyć poziomu, raportuje teraz -127 dBm ("nie zmierzono") zamiast wartości zmyślonej, a takie próbki są wykluczane ze średnich per licznik i diagnostycznych, zamiast je zaniżać.

### Uwagi
- **Dotychczasowe dane RSSI nie są porównywalne z nowymi** na żadnej płytce z SX1262. Średnie per licznik, `win_avg_rssi` i wszystkie metryki pochodne od RSSI wymagają ponownego zebrania po aktualizacji.
- Pierwsza ramka na każdej ścieżce odbioru zapisuje surowe wartości z radia na poziomie INFO, dzięki czemu można potwierdzić aktywne źródło na sprzęcie bez podnoszenia poziomu logowania.

---

# Fix: meters with a non-BCD ID can be matched by forward_meters and highlight_meters

## EN

### Fixed
- Meter matching decoded the A-field as BCD and gave up otherwise, so meters that do not use a BCD ID (Diehl/IZAR among others) had no usable ID at all. They could never be listed in `highlight_meters`, and with `forward_meters` active their telegrams were dropped silently - the one case where the whitelist discarded frames the user could not get back by any configuration.
- Both options now also match the raw A-field value, written the way the log prints it: `id:417F0666` is configured as `"0x417F0666"` (quoted, or YAML turns it into a number). Decimal entries keep their existing meaning, so no configuration changes behaviour.
- The two forms are told apart without ambiguity: a non-BCD A-field always contains a nibble above 9 and therefore always prints a hex letter, while a BCD ID never does. The `0x` form works for BCD meters too (`"0x00089907"` is meter `89907`).
- Per-meter statistics were keyed on the BCD ID, so every non-BCD meter collapsed into a single shared entry at key 0. They are now keyed on the raw A-field value, which is unique for every meter.
- `target_meter_id` still accepts only BCD IDs. A hex value there used to be accepted and then never match; it now logs a warning at boot pointing to `forward_meters`.

## PL

### Naprawiono
- Dopasowanie liczników dekodowało A-field jako BCD i w przeciwnym razie rezygnowało, więc liczniki bez ID w BCD (m.in. Diehl/IZAR) nie miały żadnego użytecznego ID. Nie dało się ich wpisać do `highlight_meters`, a przy aktywnym `forward_meters` ich telegramy znikały bez śladu - jedyny przypadek, w którym whitelista odrzucała ramki, których użytkownik nie mógł odzyskać żadną konfiguracją.
- Obie opcje dopasowują teraz również surową wartość A-field, zapisywaną tak, jak pokazuje ją log: `id:417F0666` konfigurujesz jako `"0x417F0666"` (w cudzyslowie, inaczej YAML zamieni to na liczbe). Wpisy dziesiętne zachowują dotychczasowe znaczenie, więc żadna konfiguracja nie zmienia zachowania.
- Rozróżnienie obu form jest jednoznaczne: A-field poza BCD zawsze zawiera półbajtówkę powyżej 9, więc zawsze wypisuje literę szesnastkową, a ID w BCD nigdy. Forma `0x` działa też dla liczników BCD (`"0x00089907"` to licznik `89907`).
- Statystyki per licznik były kluczowane po ID z BCD, więc wszystkie liczniki nie-BCD zlewały się w jeden wspólny wpis pod kluczem 0. Teraz kluczem jest surowa wartość A-field, unikalna dla każdego licznika.
- `target_meter_id` nadal przyjmuje wyłącznie ID w BCD. Wartość szesnastkowa była tam dotąd przyjmowana i po cichu nigdy nie pasowała; teraz przy starcie pojawia się ostrzeżenie kierujące do `forward_meters`.

---

# Fix: NO_METERS_DETECTED no longer fires on a quiet summary window

## EN

### Fixed
- `NO_METERS_DETECTED` claimed a wiring or radio-configuration fault whenever a single summary window contained no frames. That counter (`diag_total_`) is reset after every window, so a receiver that had been working all day still reported `total == 0` for any quiet minute - and meters are routinely quiet at night. The suggestion is now limited to receivers that have not seen a single frame since boot, and additionally suppressed for the first 5 minutes of uptime, where silence carries no information because meters transmit tens of seconds to several minutes apart.
- A receiver that used to work and then went silent is a different diagnosis and stays with the health pulse (`sec_since_last_rx`); it was never what this suggestion measured.
- The `NO_DATA` summary hint said "no packets received yet", which reads as "nothing ever arrived" even though it describes a single window. It now says "no packets in this window". The machine-readable `hint_code` is unchanged.

## PL

### Naprawiono
- `NO_METERS_DETECTED` sugerowało usterkę okablowania lub konfiguracji radia za każdym razem, gdy pojedyncze okno podsumowania nie zawierało ramek. Ten licznik (`diag_total_`) jest zerowany po każdym oknie, więc odbiornik działający cały dzień i tak raportował `total == 0` w dowolnej cichej minucie - a liczniki nocą standardowo milczą. Sugestia ogranicza się teraz do odbiorników, które od startu nie odebrały ani jednej ramki, i dodatkowo jest wyciszona przez pierwsze 5 minut pracy, gdzie cisza nic nie znaczy, bo liczniki nadają co kilkadziesiąt sekund do kilku minut.
- Odbiornik, który działał i zamilkł, to inna diagnoza - pozostaje przy pulsie health (`sec_since_last_rx`); ta sugestia nigdy tego nie mierzyła.
- Hint `NO_DATA` w podsumowaniu mówił "no packets received yet", co brzmi jak "nic nigdy nie przyszło", choć opisuje pojedyncze okno. Teraz mówi "no packets in this window". Maszynowe `hint_code` bez zmian.

---

# Feature: forward_meters - forwarding whitelist for the RAW telegram topic

## EN

### Added
- `forward_meters` limits which meters are published to `wmbus/<topic_name>/telegram`. Useful where most of the received traffic belongs to neighbouring meters and only a few are your own.
- Accepts an explicit list of meter IDs, or `true` to reuse the IDs already listed in `highlight_meters` so the same list is not written twice.
- An empty list (the default) or `false` forwards every decoded frame, so existing configurations are unaffected.
- The parsed IDs are logged from `setup()` and again in the delayed boot status block, next to the YAML sanity output. The repetition is deliberate: `setup()` runs before the network logger attaches, so over `esphome logs` only the second one is visible. `dump_config()` reports the same state, but is filtered out at `logger: level: info` - it needs `level: config` or higher.

### Notes
- The filter runs after decoding and the DLL CRC check, so it matches a meter ID the parser has already validated. Filtering on the raw header would be cheaper but unreliable: an ID read from a frame that failed CRC can be corrupted.
- Matching uses the BCD-decoded 8-digit meter ID. A meter whose log line shows a hex ID (`id:417F0666`) has a non-BCD A-field and cannot be whitelisted - it is dropped while the filter is active.
- `forward_meters: true` with an empty `highlight_meters` does not silence the stream: filtering stays off and a configuration warning is printed at boot.
- Diagnostics are unaffected. Counters and RSSI statistics are updated before publishing, so summaries still cover the whole ether including neighbours; only the RAW stream is reduced.
- `target_meter_id` keeps its own topic and is deliberately not subject to the whitelist.

## PL

### Dodano
- `forward_meters` ogranicza to, które liczniki trafiają na `wmbus/<topic_name>/telegram`. Przydatne tam, gdzie większość odbieranego ruchu pochodzi od liczników sąsiadów, a własnych jest kilka.
- Przyjmuje jawną listę ID liczników albo `true`, które bierze ID już wpisane w `highlight_meters` - dzięki temu ta sama lista nie musi być pisana dwa razy.
- Pusta lista (domyślnie) albo `false` przepuszcza każdą zdekodowaną ramkę, więc istniejące konfiguracje działają bez zmian.
- Sparsowane ID trafiają do logu z `setup()` oraz ponownie w opóźnionym bloku startowym, obok informacji sanity YAML. Powtórzenie jest celowe: `setup()` wykonuje się, zanim podłączy się logger sieciowy, więc przy `esphome logs` widać dopiero to drugie. `dump_config()` raportuje ten sam stan, ale przy `logger: level: info` jest odfiltrowany - wymaga `level: config` lub wyżej.

### Uwagi
- Filtr działa po dekodowaniu i sprawdzeniu DLL CRC, więc dopasowuje ID, które parser już zweryfikował. Filtrowanie po surowym nagłówku byłoby tańsze, ale zawodne: ID odczytane z ramki, która nie przeszła CRC, bywa przekłamane.
- Dopasowanie idzie po ośmiocyfrowym ID zdekodowanym z BCD. Licznik, którego log pokazuje ID szesnastkowo (`id:417F0666`), ma A-field poza BCD i nie da się go wpisać na whitelistę - przy aktywnym filtrze zostanie odrzucony.
- `forward_meters: true` przy pustym `highlight_meters` nie wycisza strumienia: filtr się nie włącza, a przy starcie pojawia się ostrzeżenie konfiguracyjne.
- Diagnostyka jest nietknięta. Liczniki i statystyki RSSI powstają przed publikacją, więc summary dalej obejmuje cały eter razem z sąsiadami; obcinany jest sam strumień RAW.
- `target_meter_id` ma własny topic i celowo nie podlega whiteliście.

---

# Fix: guard gmtime()/strftime() in rtlwmbus timestamp

## EN

### Fixed
- `Frame::as_rtlwmbus()` now guards `std::gmtime()` returning `nullptr` and `std::strftime()` returning `0`, falling back to a fixed `1970-01-01 00:00:00.00Z` timestamp. Prevents a potential null-dereference / unterminated-buffer read when the system clock holds a `time_t` value that cannot be represented (e.g. an unset or out-of-range clock). Hardening only — no wire or format change during normal operation.

## PL

### Naprawiono
- `Frame::as_rtlwmbus()` zabezpiecza teraz przypadki, gdy `std::gmtime()` zwraca `nullptr`, a `std::strftime()` zwraca `0` — z fallbackiem do stałego znacznika `1970-01-01 00:00:00.00Z`. Zapobiega potencjalnemu null-dereference / odczytowi niedokończonego bufora, gdy zegar systemowy trzyma wartość `time_t` niemożliwą do reprezentacji (np. nieustawiony lub poza zakresem). Wyłącznie utwardzenie — bez zmiany formatu wyjścia w normalnej pracy.

---

# Current documentation note

## EN

### Added
- Experimental S1 receive mode.
- `listen_mode: s1` uses a dedicated S1 receive path and does not fall back to T1/C1 parsing.
- `listen_mode: both` remains T1/C1 only.
- `listen_mode: s1` defaults to `868.300 MHz`; T1/C1/both remain at `868.950 MHz`.
- Explicit `frequency:` in YAML still overrides the mode default.

### Notes
- S1 support is intended for diagnostics and compatibility testing.
- If a valid S1 telegram is received, it is published to MQTT like other validated wM-Bus telegrams.
- Meter-value decoding remains external and depends on the backend driver and encryption key.
- Proprietary or polling-based systems may not produce standard passive S1 telegrams.

## PL

### Dodano
- Eksperymentalny tryb odbioru S1.
- `listen_mode: s1` używa dedykowanej ścieżki odbioru S1 i nie przechodzi przez logikę T1/C1.
- `listen_mode: both` nadal oznacza tylko T1/C1.
- `listen_mode: s1` domyślnie ustawia `868.300 MHz`; T1/C1/both zostają przy `868.950 MHz`.
- Jawne `frequency:` w YAML nadal nadpisuje domyślną częstotliwość trybu.

### Uwagi
- Obsługa S1 jest przeznaczona do diagnostyki i testów kompatybilności.
- Jeżeli poprawny telegram S1 zostanie odebrany, zostanie opublikowany do MQTT tak jak inne zweryfikowane telegramy wM-Bus.
- Dekodowanie wartości licznika pozostaje po stronie backendu i zależy od drivera oraz klucza szyfrowania.
- Systemy zamknięte albo odpytywane mogą nie nadawać standardowych pasywnych telegramów S1.

---

**EN**

**Summary**
Improve adaptive SX1276 behavior, add MQTT diagnostic suggestions, and expand runtime diagnostics.

**Description**
This release improves the real-world behavior of the RAW-only wM-Bus bridge, especially on SX1276 in noisy RF environments.

Main changes:

* improved `sx1276_busy_ether_mode: adaptive` logic so activation reacts to actual reception loss, not just RF noise
* added MQTT `suggestion` events with actionable diagnostic hints and YAML snippets
* added `busy_ether_changed` MQTT events for adaptive state transitions
* expanded diagnostic summaries with new runtime fields, including `busy_ether_state`
* added/expanded `summary_15min`, `summary_60min`, and per-meter snapshot reporting
* fixed multiple logic and documentation inconsistencies discovered during real hardware testing
* fixed: `busy_ether_state` in `/summary` JSON now emits `"n/a"` on SX1262 instead of the misleading `"adaptive_passive"` (the algorithm never ran on SX1262, only the stored mode value was serialised)
* fixed: `hint_code` no longer stays `"OK"` for windows with 11-99% drop rate and no specific diagnosis — new code `MODERATE_DROPS` is emitted instead so elevated drops are always visible as WARN in serial log
* changed: `highlight_meters` per-packet serial log now shows `packet #N received` instead of the previous `stats / statystyki: count=N interval=... avg_rssi=...`; per-meter stats remain available via MQTT `meter_window` events

This version does not change the project architecture: the ESP device still focuses on RF reception and RAW MQTT publishing, while meter decoding remains external.

**PL**

**Summary**
Poprawa działania adaptive dla SX1276, dodanie sugestii diagnostycznych MQTT oraz rozbudowa diagnostyki runtime.

**Description**
To wydanie poprawia rzeczywiste zachowanie mostka RAW-only wM-Bus, szczególnie dla SX1276 w zaszumionym środowisku RF.

Najważniejsze zmiany:

* poprawiono logikę `sx1276_busy_ether_mode: adaptive`, tak aby aktywacja reagowała na realne straty odbioru, a nie tylko sam szum radiowy
* dodano eventy MQTT `suggestion` z praktycznymi wskazówkami diagnostycznymi i gotowymi snippetami YAML
* dodano eventy MQTT `busy_ether_changed` dla zmian stanu adaptive
* rozszerzono raporty diagnostyczne o nowe pola runtime, w tym `busy_ether_state`
* dodano/rozszerzono raporty `summary_15min`, `summary_60min` oraz snapshoty per-meter
* poprawiono kilka niespójności logicznych i dokumentacyjnych wykrytych podczas testów na realnym sprzęcie
* poprawka: `busy_ether_state` w JSON `/summary` emituje teraz `"n/a"` na SX1262 zamiast mylącego `"adaptive_passive"` (algorytm nigdy nie działał na SX1262, serializowana była tylko wartość pola)
* poprawka: `hint_code` nie pozostaje już `"OK"` przy 11-99% dropów bez konkretnej diagnozy — nowy kod `MODERATE_DROPS` sprawia że podwyższone straty zawsze widoczne są jako WARN w logu
* zmiana: log per-pakietu `highlight_meters` wyświetla teraz `packet #N received` zamiast poprzedniego `stats / statystyki: count=N interval=... avg_rssi=...`; statystyki per-licznik nadal dostępne przez MQTT `meter_window`

To wydanie nie zmienia architektury projektu: urządzenie ESP nadal odpowiada za odbiór RF i publikację RAW do MQTT, a dekodowanie liczników pozostaje poza nim.
