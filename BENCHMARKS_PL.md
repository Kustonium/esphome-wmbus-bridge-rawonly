# BENCHMARKS_PL.md

[English version](BENCHMARKS.md)

Wnioski z pomiarów benchmarkowych dla `wmbus_radio`.

## Zakres

Ten dokument streszcza realne porównania `SX1276` i `SX1262` w środowisku bloku mieszkalnego użytkownika na ESPHome `2026.3.2`.

To nie są syntetyczne benchmarki laboratoryjne. To są praktyczne wyniki z pola.

## Środowisko testowe

- oba radia w tym samym miejscu fizycznym,
- blok mieszkalny,
- osobne testy `T1-only` oraz osobne testy `both` (`T1+C1`),
- środowisko z wieloma licznikami BMT oraz szybkimi licznikami referencyjnymi,
- obserwacje nocne i dzienne.

Główny profil ruchu użyty w analizie:

- ~29 urządzeń BMT, `77 B`, około `121 s`,
- `NES 00089907`, `143 B`, około `30 s`,
- `TCH 90830781`, `56 B`, około `34 s`,
- 2 dodatkowe wodomierze BMT, `77 B`, około `121 s`.

## Krytyczna zasada czytania danych

Nie porównuj `summary` i `meter_window` tak, jakby mierzyły to samo.

- `summary` mierzy czystość parsera / decode,
- `meter_window` mierzy realną skuteczność odbioru konkretnego licznika.

To była główna pułapka diagnostyczna w tych testach.

---

## 1. T1-only

## Główny wynik praktyczny

W tym środowisku bloku mieszkalnego:

- `SX1262` konsekwentnie wygrywał z `SX1276` dla częstych / dużych pakietów i gęstego RF,
- `SX1276` z `adaptive` był akceptowalny głównie dla wolniejszych liczników około **~120–150 s**,
- pozostały gap wyglądał już głównie na ograniczenie sprzętowe, a nie software’owe.

## Reprezentatywne wyniki pomiarów

| Licznik | Rozmiar | Interwał | SX1276 noc | SX1276 dzień | SX1262 dzień |
|---|---:|---:|---:|---:|---:|
| `00089907` | `143 B` | `~30 s` | `65%` | `~40%` | `~100%` |
| `90830781` | `56 B` | `~34 s` | `96%` | `~53%` | `~100%` |
| `03534159` | `77 B` | `~121 s` | — | `~88%` | `~100%` |
| `03528221` | `77 B` | `~121 s` | — | `~100%` | `~100%` |

## Co sugerowały dane

### Straty na `SX1276` były strukturalne, nie losowe

Dla `00089907` zaobserwowany wzorzec interwałów pokazywał powtarzalne straty, np.:

`30, 60, 30, 90, 60, 30, 89`

To wskazuje na krótkie cykle odzyskiwania, a nie na całkiem losowe gubienie.

### Niski `drop_pct` na `SX1276` bywał mylący

`SX1276` często pokazywał niski `drop_pct` w `summary`, ponieważ `adaptive` odcinał złe starty przed decode, więc te zdarzenia nie trafiały do `total`.

To **nie** oznaczało lepszego odbioru.

### Różnił się też noise floor

W identycznych warunkach RF liczba `false_start_like` nocą wynosiła około:

- `SX1276`: około `~75`,
- `SX1262`: około `~34`.

To sugeruje realną różnicę architektury / front-endu RX, a nie tylko różnicę tuningu softu.

## Próg praktyczny

Próg praktyczny rzędu **~120–150 s** był mocno potwierdzony **dla tego środowiska**.

Powyżej tego zakresu `SX1276` z `adaptive` potrafił zbliżyć się do `SX1262`.
Poniżej tego zakresu straty rosły szybko wraz z częstotliwością i rozmiarem pakietu.

To nie jest prawo uniwersalne. To jest próg potwierdzony terenowo dla tego budynku i tego profilu ruchu.

---

## 2. Tryb `both` (`T1+C1`)

## Uwaga metodologiczna

Wyników `both` nie wolno porównywać bezpośrednio z `T1-only`, jakby to był ten sam test.

Włączenie nasłuchu C1 zmienia model odbioru dla obu chipów.

Późniejszy fix rozdzielił statystyki liczników po `(meter_id, link_mode)`, więc mieszane dane T1/C1 dla tego samego ID nie zanieczyszczają już statystyk.

## Główne odkrycie

W `both` `summary` dawał obraz odwrotny do rzeczywistości:

| Radio | `ok` | `dropped` | `hint` |
|---|---:|---:|---|
| `SX1276` | `28/28` | `0` | `GOOD` |
| `SX1262` | `20/24` | `4` | `OK` |

Na papierze `SX1276` wyglądał lepiej.
`meter_window` pokazywał coś odwrotnego.

Czyli znowu:

- `summary` mierzył czystość decode,
- `meter_window` mierzył realną skuteczność odbioru.

## Reprezentatywne wyniki — dzień, `both`

| Licznik | Rozmiar | Interwał | SX1276 T1-only | SX1276 both | SX1262 T1-only | SX1262 both |
|---|---:|---:|---:|---:|---:|---:|
| `00089907` | `143 B` | `~30 s` | `~40%` | `~13%` | `~100%` | `~63%` |
| `90830781` T1 | `56 B` | `~35 s` | `~53%` | `~4%` | `~100%` | `~81%` |
| `03534159` | `77 B` | `~121 s` | `~88%` | `~95%`* | `~100%` | `~100%` |
| `03528221` | `77 B` | `~121 s` | `~100%` | `~41%` | `~100%` | `~54%` |

`*` `03534159` był przypadkiem specjalnym z wyjątkowo mocnym sygnałem około `-52 dBm`.

## Czysty koszt przełączania

W badanych oknach realny ruch C1 był mały lub epizodyczny, a mimo to sama obecność harmonogramu `both` wyraźnie obniżała skuteczność T1.

To znaczy, że degradacja wynikała z samego overheadu przełączania, a nie tylko z obsługi realnych pakietów C1.

### Zaobserwowany zakres kosztu

| Koszt | SX1276 | SX1262 |
|---|---|---|
| Szybkie liczniki (`~30–35 s`) | `-60%` do `-96%` | `-19%` do `-37%` |
| Wolniejsze liczniki (`~121 s`) | około `-46%` do `-59%`* | około `0%` do `-46%` |

`*` z wyłączeniem wyjątkowo mocnego przypadku `03534159`.

## Wniosek praktyczny dla `both`

- `SX1276 + both` jest generalnie **niezalecane**, jeśli zależy Ci na T1,
- `SX1262 + both` ma sens tylko wtedy, gdy korzyść z C1 uzasadnia koszt po stronie T1,
- **dwa urządzenia** (`T1-only` + `C1-only`) są najlepszym rozwiązaniem dla środowiska mieszanego.

---

## `summary` vs `meter_window` — dowód praktyczny

Bezpośrednie porównanie tego samego licznika (`00089907`) w tym samym środowisku bloku mieszkalnego, oba układy na 160 MHz T1-only, to samo okno 900 s:

| | SX1262 (Heltec) | SX1276 (Lilygo) |
|---|---|---|
| `summary drop_pct` | **12%** | **2%** |
| `summary hint` | OK | GOOD |
| `meter_window count` | **28 / 30** | **17 / 30** |
| `meter_window skuteczność` | **~93%** | **~57%** |
| RSSI | –74 dBm | –48 dBm |

Na pierwszy rzut oka `summary` sugeruje, że lepszy jest SX1276. `meter_window` pokazuje coś odwrotnego: SX1262 odbiera znacznie więcej realnych pakietów z tego samego licznika — mimo słabszego sygnału.

Kluczowa zasada:

- `summary` mierzy **czystość ścieżki decode**,
- `meter_window` mierzy **realną skuteczność odbioru konkretnego licznika**.

SX1276 może wyglądać lepiej w `summary`, bo `adaptive` odrzuca problematyczne próby odbioru zanim trafią do decode. To obniża raportowany `drop_pct` — ale nie oznacza lepszego odbioru w praktyce.

---

## Końcowy wniosek benchmarkowy

### `SX1276`

Akceptowalny głównie wtedy, gdy:

- środowisko jest łatwiejsze,
- liczniki są wolniejsze,
- pakiety są mniejsze,
- możesz pozostać przy dedykowanym nasłuchu single-mode.

### `SX1262`

Lepszy wybór domyślny, gdy:

- środowisko RF jest gęste,
- pakiety są częste,
- pakiety są duże,
- zależy Ci na maksymalnej niezawodności,
- ważny jest mieszany odbiór T1/C1.
