# CHIP_SELECTION_PL.md

[English version](CHIP_SELECTION.md)

Praktyczny przewodnik wyboru radia dla `wmbus_radio`.

## Krótka odpowiedź

- **Dom / kilka liczników / spokojny eter / głównie wolne T1** → `SX1276` zwykle wystarczy.
- **Blok / dużo liczników / częste pakiety / większe pakiety** → wybierz `SX1262`.
- **Mieszane T1 + C1 na jednym urządzeniu** → działa, ale kosztuje skuteczność odbioru.
- **Najlepszy układ dla środowiska mieszanego** → dwa osobne urządzenia: `T1-only` i `C1-only`.

## Skąd bierze się różnica

`SX1276` ma starszą architekturę odbiorczą i znacznie mniejszy margines sprzętowy dla takiego obciążenia. W gęstym eterze łatwiej gubi, ucina albo w ogóle nie wpuszcza do pipeline częstych pakietów.

`SX1262` lepiej znosi presję czasową. W praktyce wygrywa tam, gdzie pakiety są:

- częste,
- dłuższe,
- otoczone inną aktywnością radiową,
- mieszane z overheadem harmonogramu T1/C1.

## Co ma największe znaczenie

Najważniejsze czynniki to:

1. **interwał pakietów**,
2. **rozmiar pakietu**,
3. **gęstość RF / eter blokowy**,
4. **tryb pojedynczy vs `both`**.

Sama liczba liczników nie mówi wszystkiego. Kilka szybkich liczników może szkodzić bardziej niż wiele wolnych.

## Tabela praktycznego wyboru

| Sytuacja | `SX1276` | `SX1262` |
|---|---|---|
| Spokojne środowisko, kilka wolnych liczników | wystarczający | też dobry |
| Blok z wieloma licznikami w pobliżu | akceptowalny tylko w łatwiejszych przypadkach | zalecany |
| Szybkie liczniki rzędu 30–60 s | często słaby | zalecany |
| Duże pakiety pod presją czasu | słaby | zalecany |
| `both` na jednym urządzeniu | niezalecane przy istotnym ruchu T1 | możliwe, ale nadal kompromis |
| Maksymalna niezawodność | ograniczony | zalecany |

## Wniosek z realnych testów T1-only

W testowanym środowisku bloku mieszkalnego na ESPHome `2026.3.2`:

- `SX1262` konsekwentnie wygrywał z `SX1276` przy gęstym RF i częstych / dużych pakietach,
- `SX1276` z `adaptive` był akceptowalny głównie dla wolniejszych liczników około **~120–150 s** w tym środowisku testowym,
- poniżej tego praktycznego progu straty na `SX1276` rosły wraz z częstotliwością i rozmiarem pakietu.

To jest próg **praktyczny, nie absolutny**. Zależy od budynku, poziomów sygnału i obciążenia RF.

## Wniosek dla trybu `both`

`both` to nie jest po prostu „T1 plus trochę C1”. To dodatkowy koszt harmonogramu nawet wtedy, gdy realny ruch C1 jest mały.

Praktyczny wniosek:

- na `SX1276` `both` jest z reguły złym pomysłem, jeśli zależy Ci na T1,
- na `SX1262` `both` ma sens, ale nadal ma mierzalny koszt,
- jeśli naprawdę zależy Ci na niezawodnym odbiorze mieszanym, użyj **dwóch urządzeń**.

## Rekomendacja dla `adaptive`

Dla `SX1276` zacznij od:

```yaml
sx1276_busy_ether_mode: adaptive
```

Na `normal` schodź dopiero wtedy, gdy:

- eter jest spokojny,
- masz tylko kilka liczników,
- `meter_window` wygląda dobrze,
- nie ma wyraźnych objawów busy ether.

`aggressive` traktuj jako tryb specjalny do testów albo bardzo ciężkiego środowiska.

## Ograniczenia, które warto zaakceptować od razu

- Niski `drop_pct` **nie** oznacza automatycznie lepszego realnego odbioru.
- `summary` może wyglądać czyściej na `SX1276`, a `meter_window` jednocześnie pokazywać gorszy wynik rzeczywisty.
- `both` na jednym radiu zawsze jest kompromisem.
- Soft może poprawiać margines, ale nie usuwa różnicy klasy sprzętowej między `SX1276` a `SX1262`.
