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
