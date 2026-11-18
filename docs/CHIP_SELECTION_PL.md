# CHIP_SELECTION_PL.md

[English version](CHIP_SELECTION.md)

Praktyczny przewodnik wyboru radia dla `wmbus_radio`. Obsługiwane są cztery
radia: `CC1101`, `SX1276`, `SX1262` i `LR1121`.

## Krótka odpowiedź

- **Dom / kilka liczników / spokojny eter / głównie wolne T1** → `SX1276` zwykle wystarczy.
- **Blok / dużo liczników / częste pakiety / większe pakiety** → wybierz `SX1262`.
- **Liczniki S1** → na czystym sygnale `LR1121`, `SX1276` i `SX1262` słyszą S1 w granicach ok. 1 dB (stanowisko, 17.11.2026). Na prawdziwych licznikach `SX1276` nadal łapie więcej, bo toleruje nadajniki odbiegające od normy; resztę rozstrzyga antena i jej miejsce. `CC1101` jest o ok. 5 dB słabszy. Patrz [S1 to osobne pytanie](#s1-to-osobne-pytanie).
- **Liczniki C1** → `SX1262` w `listen_mode: both` teraz je słyszy (10 liczników C1 na T-Beamie, ponowny pomiar 01.10.2026 po poprawce słowa synchronizacji). `SX1276` w `both` usłyszał 14. Patrz [C1 też jest osobnym pytaniem](#c1-też-jest-osobnym-pytaniem).
- **Mieszane T1 + C1 na jednym urządzeniu** → od poprawki z 30.09.2026 brak mierzalnego kosztu dla T1 na `SX1262`, `LR1121` i `SX1276`; `CC1101` nie zmierzony ponownie.
- **Dwa osobne urządzenia** (`T1-only` i `C1-only`) → już niepotrzebne: na `SX1262`, `LR1121` i `SX1276` wystarczy jedno urządzenie w `both`.
- **`CC1101`** → tylko jeśli już masz taki sprzęt. Stoi za jawną bramką
  bezpieczeństwa i nie jest układem, który warto kupić pod ten projekt.
- **`LR1121`** → najnowszy. Na wspólnej antenie usłyszał więcej liczników niż
  `SX1276` (11.11.2026), a na czystym S1 dzieli pierwsze miejsce (17.11.2026), ale
  to jedna płytka i najmłodszy sterownik. Traktuj jako działający punkt wyjścia,
  nie jako domyślny wybór ze wsparciem.

## Skąd bierze się różnica

`SX1276` ma starszą architekturę odbiorczą i znacznie mniejszy margines sprzętowy dla takiego obciążenia. W gęstym eterze łatwiej gubi, ucina albo w ogóle nie wpuszcza do pipeline częstych pakietów.

`SX1262` lepiej znosi presję czasową. W praktyce wygrywa tam, gdzie pakiety są:

- częste,
- dłuższe,
- otoczone inną aktywnością radiową,
- mieszane z overheadem harmonogramu T1/C1.

`CC1101` jest układem znacznie starszym i prostszym. Działa, ale ma najwolniejszy
tor odczytu z całej czwórki (termin odpytywania FIFO to 1800 µs wobec 1000 µs na
`SX1276`). Od 13.11.2026 ma własny profil modemu S1; na S1 jest o ok. 5 dB mniej
czuły od pozostałych trzech.

`LR1121` to najnowszy krzem. Wcześniejsza wersja tej strony mówiła, że **wypadł
ostatni**; to nie przetrwało uczciwego porównania. Opierało się na RSSI
poprawionym o −11 dB, a ta poprawka zawierała też gorsze miejsce anteny (`LR1121`
miał antenę na bardzo krótkim pigtailu). Na **jednej antenie przez rozgałęźnik**
(11.11.2026, z zamianą wyjść między fazami) `LR1121` czyta **6 dB niżej** niż
`SX1276` na tych samych ramkach, odebrał mniej więcej **dwa razy więcej ramek**
i **174–190 liczników wobec 121–136**, a z najsłabszych ramek, które zdekodował
`SX1276`, złapał 86–98%. T-Beam na `SX1262` na drugim wyjściu rozgałęźnika usłyszał
0,84 jego liczników (12–13.11.2026). Na czystym S1 (17.11.2026) zremisował
z najlepszym `SX1262`. Na stanowisku tłumikowym T1 (31.08.2026) dwie płytki `SX1262`
dekodowały jeszcze ok. 4–5 dB dalej niż on, więc jego przewaga na wspólnej antenie
w T1 nie wynika z samej czułości.

Co pozostaje prawdą: nie ma tolerancji błędu w wykrywaniu preambuły i synchronizacji
(tak jak `SX1262`), więc nadajnik odbiegający o kilka procent od szybkości normy
ginie całkowicie - patrz [S1 to osobne pytanie](#s1-to-osobne-pytanie).

Jedno mieszkanie i po jednej płytce każdego typu, więc to szereguje te pięć
egzemplarzy, a nie krzem.

## Co ma największe znaczenie

Najważniejsze czynniki to:

1. **interwał pakietów**,
2. **rozmiar pakietu**,
3. **gęstość RF / eter blokowy**,
4. **tryb pojedynczy vs `both`**,
5. **który tryb łącza realnie Cię interesuje** — T1/C1 i S1 nie ustawiają radiów
   w tej samej kolejności,
6. **antena i jej miejsce** — antena 10 cm zamiast 6 cm dołączonej do płytki dała
   Heltecowi V4 ok. **+5 dB**, a XIAO ok. **+3 dB** na licznik (15.10.2026, te same
   płytki, to samo miejsce), więcej niż większość różnic między układami na tej
   stronie.

Sama liczba liczników nie mówi wszystkiego. Kilka szybkich liczników może szkodzić bardziej niż wiele wolnych.

## Tabela praktycznego wyboru

| Sytuacja | `CC1101` | `SX1276` | `SX1262` | `LR1121` |
|---|---|---|---|---|
| Spokojne środowisko, kilka wolnych liczników | działa | wystarczający | też dobry | dobry |
| Blok z wieloma licznikami w pobliżu | słaby | akceptowalny tylko w łatwiejszych przypadkach | zalecany | dobry, najsłabiej sprawdzony |
| Szybkie liczniki rzędu 30–60 s | słaby | często słaby | zalecany | dobry |
| Duże pakiety pod presją czasu | słaby | słaby | zalecany | dobry |
| `both` na jednym urządzeniu | niezalecane | **w porządku** - brak mierzalnego kosztu T1 | **w porządku** - brak mierzalnego kosztu T1 | w porządku - brak mierzalnego kosztu T1 |
| Liczniki S1 przy progu szumu | o ok. 5 dB słabszy | **zalecany** - ta sama czułość, toleruje liczniki odbiegające od normy | ta sama czułość na czystym sygnale, bez tolerancji | ta sama czułość na czystym sygnale, bez tolerancji |
| Liczniki C1 | niesprawdzony tutaj | **najlepszy** w `both` (14 liczników C1) | **dobry** w `both`, patrz niżej | słyszy liczniki C1 w swoim (krótszym) zasięgu T1 |
| Maksymalna niezawodność | nie | ograniczony | zalecany | jeszcze nie do udowodnienia |
| Dostępność drugiej opinii | szeroka | szeroka | szeroka | jedna płytka, jeden dom |

## Wniosek z realnych testów T1-only

W testowanym środowisku bloku mieszkalnego na ESPHome `2026.3.2`:

- `SX1262` konsekwentnie wygrywał z `SX1276` przy gęstym RF i częstych / dużych pakietach,
- `SX1276` z `adaptive` był akceptowalny głównie dla wolniejszych liczników około **~120–150 s** w tym środowisku testowym,
- poniżej tego praktycznego progu straty na `SX1276` rosły wraz z częstotliwością i rozmiarem pakietu.

To jest próg **praktyczny, nie absolutny**. Zależy od budynku, poziomów sygnału i obciążenia RF.

Od 15.11.2026 `sx1262_rx_bandwidth` ma domyślnie **234 kHz** (było 312 kHz,
odziedziczone i nigdy niezmierzone dla T1). Zmierzone 12–13.11.2026 wobec `LR1121`
na rozgałęźniku: T-Beam złapał **52,5%** ramek `LR1121` w zakresie −110…−105 dBm
przy 234 kHz wobec 30,6% przy 312 kHz, usłyszał 0,84 jego liczników wobec 0,76
i złapał 77% jego ramek C1 wobec 60%.

## S1 to osobne pytanie

S1 nie ustawia radiów w tej samej kolejności co T1, więc powyższa tabela T1 tu
nie obowiązuje.

**Zmierzone na czystym sygnale, 17.11.2026.** Płytka-nadajnik wysyłała poprawną
ramkę S1 na 869,850 MHz (tam nie nadaje żaden prawdziwy licznik) przez tłumiki
2×30 dB, zmniejszając moc o 1 dB na krok, 20–30 ramek na krok. Każda płytka
dzieliła jedną antenę z `LR1121` przez rozgałęźnik, więc każdy wynik to różnica
wobec `LR1121` **w tym samym przebiegu** (sam `LR1121` przesuwa się o 1,5–3 dB
między przebiegami). Próg to moc, przy której płytka odbiera połowę ramek.

| płytka | układ | wobec `LR1121` | próg 50%, ≈ skala LilyGO |
|---|---|---|---|
| Waveshare | LR1121 | **0 dB** (odniesienie) | ≈ −103,5 dBm |
| LilyGO T-Beam | SX1262 | **0 dB** | ≈ −103,5 dBm |
| LilyGO T3-S3 | SX1276 | **≈ −1 dB** | −102,6 dBm |
| Heltec V4 | SX1262 | **≈ −1 dB** | ≈ −102,5 dBm |
| SIH Reader | CC1101 | **≈ −5 dB** | ≈ −98,5 dBm |
| XIAO ESP32-S3 + Wio | SX1262 | **≈ −8 dB** | ≈ −95,5 dBm |

Dokładność ok. ±1–2 dB. „Skala LilyGO” to własny odczyt RSSI LilyGO plus zmierzona
różnica do `LR1121` - przybliżenie, a nie laboratoryjne dBm na złączu.

Co z tego wynika:

- **`LR1121`, `SX1276` i `SX1262` słyszą czysty sygnał S1 tak samo**, w granicach
  decybela. W tych samych godzinach na prawdziwych licznikach jedna płytka złapała
  256 emisji S1, a inna 58 - to nie jest czułość.
- **Na prawdziwych licznikach decydują dwie rzeczy.** Po pierwsze tolerancja na
  nadajnik: prawdziwe liczniki odbiegają częstotliwością, szybkością albo preambułą,
  a `SX1276` ma tolerancję błędu w detektorze preambuły
  (`sx1276_preamble_tolerance`, domyślnie 10), której `SX1262` i `LR1121` nie mają -
  przy szybkości −6% `SX1276` nadal dekodował 8,4% ramek, obie płytki `SX1262`
  i `LR1121` żadnej (05.09.2026). Po drugie antena i jej miejsce: LilyGO na
  własnej, najdłuższej antenie zyskał ok. 5 dB względem rozgałęźnika, z czego
  ok. 3,5 dB to strata samego rozgałęźnika.
- **`CC1101` jest na S1 o ok. 5 dB słabszy**, to realna różnica radia: liczniki,
  które docierają do `SX1276` na −96…−101 dBm, są poniżej jego progu, dlatego na
  stanowisku nie odbiera żadnego prawdziwego licznika S1, choć mocną ramkę testową
  dekoduje w 99%. Od 13.11.2026 ma własny profil modemu S1 (TI AN067 Radio Link A).
- **Egzemplarz XIAO jest o ok. 8 dB słabszy** od dwóch pozostałych płytek
  `SX1262` - to ta płytka z jej torem antenowym, nie układ.

**Wycofane:** „`SX1262` zawodzi na S1 przy −85 dBm, 3–10 dB za `SX1276`”
(08.2026). Porównywało RSSI odczytane przez różne płytki, które nie mają wspólnej
skali: na swoim progu Heltec czytał −87 dBm tam, gdzie `LR1121` czytał −109 dBm
dla tego samego sygnału.

Zasada praktyczna dla S1: **najpierw antena i miejsce.** Każdy z `LR1121`,
`SX1276`, `SX1262` słyszy czyste S1 tak samo; przy wielu słabych, starszych
licznikach wybierz `SX1276` ze względu na tolerancję; do słabego S1 unikaj `CC1101`.

## C1 też jest osobnym pytaniem

**Zmierzone ponownie 30.09–01.10.2026, po poprawce słowa synchronizacji** (jedno
słowo, 0x543D, w `c1` i `both`; patrz `RELEASE_NOTES_PL.md`), to samo mieszkanie
i płytki, prawdziwe liczniki, dzień roboczy 07:00–16:00:

| odbiornik, `listen_mode: both` | słyszane liczniki C1 | uwagi |
|---|---|---|
| `SX1262` (LilyGO T-Beam) | **10** | 56 ramek C1/h; z błędem było 2 |
| `LR1121` | 1 | najsilniejszy Techem, odbierany w pełni; pozostałe liczniki C1 są poza jego zasięgiem także w T1 |
| `SX1276` (LilyGO) | **14** | 01–02.10.2026, 18,5 h; silne po 8-9 ramek/h, najsłabsze (−94…−98 dBm) sporadycznie |

**Niskie liczby C1 poniżej wynikały głównie z błędu słowa synchronizacji, nie
z układów.** Wycofany jest wyciągnięty z nich wniosek, że różnicy do `SX1276`
nie da się nadrobić programowo. Zostają niżej jako zapis tego, co zmierzono z
błędem.

### Przed poprawką (25–26.09.2026)

Zmierzone na prawdziwych licznikach Techem C1, wszystkie płytki w jednym pokoju,
868,95 MHz:

| odbiornik | słyszane liczniki C1 | uwagi |
|---|---|---|
| `SX1276` (LilyGO), domyślne `sx1276_preamble_tolerance: 10` | **8–9** | 100 ramek w 2,2 h, 487 przez noc |
| `SX1276` z `sx1276_preamble_tolerance: 0` | **1** | ten sam jeden licznik co układy niżej |
| `SX1262` (Heltec V4.2, zewnętrzny LNA) | 1 | |
| `SX1262` (LilyGO T-Beam, bez LNA, inny producent i antena) | 2 | drugi: 4 ramki przez noc |
| `LR1121` | 1 | |

Po drodze wykluczone, każde bezpośrednim pomiarem: pasmo odbiornika (234, 312
i 467 kHz), przesunięcie częstotliwości nadajnika (wszystkie liczniki w ±4 kHz
według AFC `SX1276`), `min_preamble_bits` 16 / 8 / 0 oraz ustawienie dewiacji
(`LR1121` odbiera C1 z 50 kHz i zachowuje się tak samo). Na `SX1262` słabsze
liczniki C1 nie wywołują nawet przerwania - patrz `rx_path.irq_start` w
[DIAGNOSTIC_PL.md](DIAGNOSTIC_PL.md).

Przy aktywnym błędzie przewaga `SX1276` w C1 szła za tolerancją błędów jego
detektora preambuły: przy tolerancji 0 słyszał dokładnie to samo co `SX1262`
i `LR1121`. Ta część jest zmierzona i obowiązuje, ale wniosek, że decyduje
układ, już nie: po poprawce T-Beam przeszedł z 2 na 10 liczników C1.

Dla części liczników ma to mniejsze znaczenie, niż się wydaje: ciepłomierze
Techem wysyłają jawny telegram we własnym formacie w **T1** i osobny,
zaszyfrowany AES telegram OMS w C1. Czytelne wartości przychodzą w T1, które
`SX1262` odbiera dobrze; telegram C1 bez klucza i tak jest bezużyteczny.

Zasada praktyczna: **jeśli masz liczniki C1, ustaw `SX1262` w `listen_mode: both`.**
`SX1276` w `both` usłyszał najwięcej liczników C1 (14 wobec 10 na T-Beamie) i
nadal lepiej słyszy te najsłabsze.

## Wniosek dla trybu `both`

**Zmierzone ponownie po poprawce słowa synchronizacji z 30.09.2026: na `SX1262`
i `LR1121` `both` nie kosztuje już T1.** Dzień roboczy 07:00–16:00, ta sama płytka
porównana sama ze sobą:

| płytka | `t1` (przed) | `both` po poprawce |
|---|---|---|
| `SX1262` LilyGO T-Beam | 152 liczniki T1, RSSI p1/p5 −98/−96 dBm | **161** liczników T1, p1/p5 **−98/−96** dBm |
| `LR1121` | 55 liczników T1 | 51 liczników T1 |
| `SX1262` XIAO, `both` z błędem → `both` po poprawce, ten sam dzień | 496 ramek T1/h, 29% ramek płytki odniesienia | **805** ramek/h, **47%** |

W dwóch pierwszych wierszach to różne dni (dzień z `both` był bardziej
zatłoczony), więc kilka liczników w jedną czy drugą stronę to szum; wyraźniejszym
sygnałem jest identyczny ogon RSSI.

Praktyczny wniosek:

- na `SX1262` i `LR1121` **`both` jest w porządku** - włącz go, jeśli masz liczniki C1,
- na `SX1276` **`both` też jest w porządku**: LilyGO w `both` usłyszał 143 liczniki
  T1 w godzinach 07:00-11:00 wobec 131 i 132 w tych samych godzinach w `t1`, przy
  płaskich płytkach kontrolnych (T-Beam 141 → 142). Wcześniejszy spadek 119 → 108
  zmierzono z błędem,
- wcześniejsze liczby (118 → 64 na T-Beamie, 62 → 40 na `LR1121`, jedna noc
  25–26.09.2026) to był błąd słowa synchronizacji plus sąsiednia antena na
  stanowisku, a nie koszt trybu `both`.

`both` na każdym radiu obejmuje wyłącznie T1/C1. **S1 nigdy nie bierze udziału
w `both`** i trzeba go wybrać jawnie przez `listen_mode: s1`, co zmienia też
domyślną częstotliwość na 868,300 MHz.

## Rekomendacja dla `adaptive`

Dla `SX1276` zacznij od domyślnej:

```yaml
sx1276_busy_ether_mode: normal
```

`adaptive` i `aggressive` niczego nie stroją — one **rezygnują ze słabych
startów**, żeby radio nadążyło w zajętym eterze. To jest wymiana, którą warto
zrobić dopiero wtedy, gdy odbiornik naprawdę się przeciąża.

Zmierzone w gęstej zabudowie 2026-08-23, cztery płytki w jednym punkcie:

| tryb | najsłabsza odebrana ramka | liczba liczników |
|---|---:|---:|
| `adaptive` | −84 dBm (nic słabszego nie przeszło w ogóle) | 27 |
| `normal` | **−97 dBm** | **53** |

W tym samym czasie `fifo_overrun`, `truncated`, `payload_read_failed` i
`irq_timeout` przez cały dzień wynosiły zero: odbiornik się nie przeciążał, więc
czułość była wydawana na nic. Dlatego domyślną jest teraz `normal`.

Podnoś dopiero wtedy, gdy mówią o tym liczniki przeciążenia, a nie wrażenie, że
w eterze jest tłoczno:

- `fifo_overrun` > 0 albo `truncated` > 0, **i** realne straty w `drop_pct`,
- potem porównaj liczby per licznik przed i po — nie `drop_pct`, bo ten poprawia
  się już przez to, że ramki, które by policzył, nie są w ogóle próbowane.

`aggressive` jest do świadomych testów, nie do codziennej pracy.

Ta opcja dotyczy wyłącznie `SX1276`. Na `SX1262`, `CC1101` i `LR1121` nie ma
maszyny busy-ether, a `busy_ether_state` raportuje `n/a`.

**Zastrzeżenie:** jedna płytka, jeden budynek, jeden wieczór. Mechanizm jest
zrozumiały (próg przerwania jest zaciśnięty klamrą przy −86 dBm, a tryb dopycha
go do tej klamry), ale skala efektu w innych warunkach jest nieznana.

## Ograniczenia, które warto zaakceptować od razu

- Niski `drop_pct` **nie** oznacza automatycznie lepszego realnego odbioru.
- `summary` może wyglądać czyściej na `SX1276`, a `meter_window` jednocześnie pokazywać gorszy wynik rzeczywisty.
- `both` na jednym radiu zawsze jest kompromisem.
- Soft może poprawiać margines, ale nie usuwa różnicy klasy sprzętowej między tymi układami.
- **Wartości RSSI nie są porównywalne między płytkami.** Płytka z zewnętrznym
  LNA/FEM czyta 13–15 dB wyżej na identycznych ramkach. Na jednej wspólnej antenie
  (11.11 i 17.11.2026) `LR1121` czyta ok. 6 dB niżej niż `SX1276` LilyGO, a Heltec V4
  ok. 15 dB wyżej. Zestawianie bezwzględnych dBm z dwóch różnych płytek mierzy front
  end, a nie odbiór.
- **Liczby ramek są porównywalne tylko wtedy, gdy płytki stoją w tym samym
  miejscu.** W żadnym z cytowanych tu testów tak nie było, więc każda liczba
  „międzyukładowa" opisuje płytkę w danym położeniu, a nie sam układ. Ile waży
  położenie, zmierzono 2026-10-20/21: przesunięcie anteny jednej płytki SX1262
  o 30-50 cm wzdłuż tej samej ściany dało jej ok. 11% więcej ramek od większości
  liczników i zabrało całą grupę liczników, którą wcześniej słyszała codziennie -
  ta sama płytka, te same ustawienia, ta sama antena, kontrola bez zmian. Na
  868 MHz odbicia wygaszają się co ~17 cm (pół długości fali), a każdy licznik ma
  własny układ takich miejsc. Zobacz TROUBLESHOOTING, punkt 19.
