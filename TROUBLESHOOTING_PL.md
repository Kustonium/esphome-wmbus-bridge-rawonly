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
highlight_meters:
  - "00089907"

diagnostic_verbose: false
diagnostic_publish_summary: true
diagnostic_publish_drop_events: true
diagnostic_publish_rx_path_events: false
diagnostic_publish_highlight_only: true
diagnostic_publish_raw: false
sx1276_busy_ether_mode: adaptive
```

## 10. Najkrótsza ścieżka decyzji

- użyj `SX1262`, jeśli zależy Ci na niezawodności,
- używaj `SX1276` tylko wtedy, gdy środowisko jest łatwiejsze albo ruch wolniejszy,
- nie ufaj samemu `summary`,
- dla środowisk mieszanych T1/C1 dwa dedykowane urządzenia są lepsze niż jeden setup `both`.
