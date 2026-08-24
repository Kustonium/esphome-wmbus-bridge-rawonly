# CHIP_SELECTION_PL.md

[English version](CHIP_SELECTION.md)

Praktyczny przewodnik wyboru radia dla `wmbus_radio`. Obsługiwane są cztery
radia: `CC1101`, `SX1276`, `SX1262` i `LR1121`.

## Krótka odpowiedź

- **Dom / kilka liczników / spokojny eter / głównie wolne T1** → `SX1276` zwykle wystarczy.
- **Blok / dużo liczników / częste pakiety / większe pakiety** → wybierz `SX1262`.
- **Liczniki S1 na granicy słyszalności** → `SX1276`. Patrz [S1 to osobne pytanie](#s1-to-osobne-pytanie).
- **Mieszane T1 + C1 na jednym urządzeniu** → działa, ale kosztuje skuteczność odbioru.
- **Najlepszy układ dla środowiska mieszanego** → dwa osobne urządzenia: `T1-only` i `C1-only`.
- **`CC1101`** → tylko jeśli już masz taki sprzęt. Stoi za jawną bramką
  bezpieczeństwa i nie jest układem, który warto kupić pod ten projekt.
- **`LR1121`** → najnowszy i najlepszy odbiornik z dotąd zmierzonych tutaj, ale
  najsłabiej sprawdzony. Traktuj jako działający punkt wyjścia, nie jako domyślny
  wybór ze wsparciem.

## Skąd bierze się różnica

`SX1276` ma starszą architekturę odbiorczą i znacznie mniejszy margines sprzętowy dla takiego obciążenia. W gęstym eterze łatwiej gubi, ucina albo w ogóle nie wpuszcza do pipeline częstych pakietów.

`SX1262` lepiej znosi presję czasową. W praktyce wygrywa tam, gdzie pakiety są:

- częste,
- dłuższe,
- otoczone inną aktywnością radiową,
- mieszane z overheadem harmonogramu T1/C1.

`CC1101` jest układem znacznie starszym i prostszym. Działa, ale ma najwolniejszy
tor odczytu z całej czwórki (termin odpytywania FIFO to 1800 µs wobec 1000 µs na
`SX1276`), a jego obsługa S1 jest surowym snifferem, nie ścieżką odbiorczą.

`LR1121` to najnowszy krzem. Na testowanym tu sprzęcie dekodował ramki wyraźnie
poniżej tego, co w tym samym domu osiągały pozostałe radia — ale nie chodził
tygodniami i **nigdy nie był porównany z innym radiem w tym samym miejscu**,
a tylko takie porównanie oddziela układ od anteny i ściany, przy której stoi.

## Co ma największe znaczenie

Najważniejsze czynniki to:

1. **interwał pakietów**,
2. **rozmiar pakietu**,
3. **gęstość RF / eter blokowy**,
4. **tryb pojedynczy vs `both`**,
5. **który tryb łącza realnie Cię interesuje** — T1/C1 i S1 nie ustawiają radiów
   w tej samej kolejności.

Sama liczba liczników nie mówi wszystkiego. Kilka szybkich liczników może szkodzić bardziej niż wiele wolnych.

## Tabela praktycznego wyboru

| Sytuacja | `CC1101` | `SX1276` | `SX1262` | `LR1121` |
|---|---|---|---|---|
| Spokojne środowisko, kilka wolnych liczników | działa | wystarczający | też dobry | dobry |
| Blok z wieloma licznikami w pobliżu | słaby | akceptowalny tylko w łatwiejszych przypadkach | zalecany | dobry, najsłabiej sprawdzony |
| Szybkie liczniki rzędu 30–60 s | słaby | często słaby | zalecany | dobry |
| Duże pakiety pod presją czasu | słaby | słaby | zalecany | dobry |
| `both` na jednym urządzeniu | niezalecane | niezalecane przy istotnym ruchu T1 | możliwe, ale nadal kompromis | możliwe, niesprawdzone w czasie |
| Liczniki S1 przy progu szumu | tylko surowy sniffer | **zalecany** | słabszy, patrz niżej | obiecujący, jeden test |
| Maksymalna niezawodność | nie | ograniczony | zalecany | jeszcze nie do udowodnienia |
| Dostępność drugiej opinii | szeroka | szeroka | szeroka | jedna płytka, jeden dom |

## Wniosek z realnych testów T1-only

W testowanym środowisku bloku mieszkalnego na ESPHome `2026.3.2`:

- `SX1262` konsekwentnie wygrywał z `SX1276` przy gęstym RF i częstych / dużych pakietach,
- `SX1276` z `adaptive` był akceptowalny głównie dla wolniejszych liczników około **~120–150 s** w tym środowisku testowym,
- poniżej tego praktycznego progu straty na `SX1276` rosły wraz z częstotliwością i rozmiarem pakietu.

To jest próg **praktyczny, nie absolutny**. Zależy od budynku, poziomów sygnału i obciążenia RF.

## S1 to osobne pytanie

S1 nie ustawia radiów w tej samej kolejności co T1, więc powyższa tabela T1 tu
nie obowiązuje. Zmierzone 2026-08-01 i 2026-08-14:

- `SX1262` dekoduje S1 mniej więcej do **−82 dBm** i zawodzi przy **−85 dBm**.
  Trzy niezależne metody dały ten sam próg.
- `SX1276` zdekodował tę samą, rzeczywistą emisję, w tej samej sekundzie, przy
  **−99/−100 dBm**.
- Różnica to zatem około **3–10 dB**, a w testowanym budynku cała lokalna
  populacja S1 dociera na progu `SX1262` albo poniżej niego.

**`SX1262` nie ma defektu przy S1. Jest tam po prostu mniej czuły.** Żadne
strojenie AGC ani rejestrów nie odrobiło tej różnicy; eksperymenty, które
wyglądały na pomocne, nie powtórzyły się w układzie A–B–A i zostały usunięte
z kodu.

Zasada praktyczna: **`SX1276` do S1, `SX1262` do T1.** Jeśli potrzebujesz obu,
jest to argument za dwoma urządzeniami, a nie za jednym kompromisem.

`CC1101` w `listen_mode: s1` to eksperymentalny surowy sniffer wyłącznie na sync
S-mode, a nie ścieżka odbiorcza — sterownik mówi to wprost przy starcie. `LR1121`
odebrał S1 poprawnie za pierwszym razem, ale wyłącznie z nadajnika warsztatowego
przy −59 dBm, co dowodzi działania toru i nie mówi nic o czułości.

## Wniosek dla trybu `both`

`both` to nie jest po prostu „T1 plus trochę C1”. To dodatkowy koszt harmonogramu nawet wtedy, gdy realny ruch C1 jest mały.

Praktyczny wniosek:

- na `SX1276` `both` jest z reguły złym pomysłem, jeśli zależy Ci na T1,
- na `SX1262` `both` ma sens, ale nadal ma mierzalny koszt,
- jeśli naprawdę zależy Ci na niezawodnym odbiorze mieszanym, użyj **dwóch urządzeń**.

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
  LNA/FEM czyta 13–15 dB wyżej na identycznych ramkach. Zestawianie bezwzględnych
  dBm z dwóch różnych płytek mierzy front end, a nie odbiór.
- **Liczby ramek są porównywalne tylko wtedy, gdy płytki stoją w tym samym
  miejscu.** W żadnym z cytowanych tu testów tak nie było, więc każda liczba
  „międzyukładowa" opisuje płytkę w danym położeniu, a nie sam układ.
