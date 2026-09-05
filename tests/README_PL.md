# Testy

[English version](README.md)

Ten katalog zawiera kontrole wykonywane podczas rozwoju komponentu.
Nie wchodzą do firmware ESPHome; uruchamia je CI albo lokalny kompilator hosta.

## Testy parsera na hoście

`tests/host/test_core.cpp` kompiluje wybrane fragmenty parsera poza ESPHome i sprawdza:

- dekodowanie 3-z-6 w trybie T,
- usuwanie DLL CRC dla formatów A i B,
- poprawne ścieżki parsera T1, C1 i S1,
- odrzucanie błędnego CRC, uciętego T1, błędnej preambuły C1 i niepoprawnego Manchester S1,
- decyzję filtra `forward_meters` (`meter_filter.h`) i dopasowanie ID spoza BCD,
- wzorcowe telegramy RAW o trzech zakresach długości, sprawdzające podział
  formatu A na pierwsze 10 bajtów, kolejne bloki po 16 bajtów i resztę.
  Próbki są syntetyczne: zawierają fikcyjne numery liczników i wypełnienie,
  ponieważ rzeczywista ramka identyfikuje licznik i zawiera odczyty zużycia.

Workflow `.github/workflows/ci.yml` kompiluje i uruchamia te testy na Ubuntu.

## Konfiguracje wyłącznie dla CI

`tests/ci/` zawiera konfiguracje poszerzające zakres kontroli CI.
Nie są przykładami użytkowymi i nie wolno wgrywać ich na płytki.

- `forward_meters_ci.yaml` — sprawdza `forward_meters` przez rzeczywistą walidację
  ESPHome. Przykłady nie ustawiają tej opcji, więc bez tego pliku macierz firmware
  sprawdzałaby tylko domyślną pustą ścieżkę. Deklaruje dwa radia przez `MULTI_CONF`,
  aby jedna kompilacja objęła dziedziczenie (`forward_meters: true`) i jawną listę.
  Obie formy łączą ID dziesiętne i szesnastkowe w cudzysłowie.

## Dodawanie próbek wzorcowych

Próbki to znormalizowane telegramy RAW HEX, takie jak publikowane na
`wmbus/<device>/telegram`, z usuniętymi przez komponent bajtami DLL CRC.

Aby dodać próbkę:

1. Otwórz `tests/host/test_core.cpp`.
2. Dodaj wpis do `golden_frame_fixtures()`:

```cpp
{
    "short-name",
    "RAW_HEX_WITHOUT_SPACES",
    EXPECTED_METER_ID,
},
```

3. Zapisz commit i wypchnij go do `dev`.
4. Sprawdź, czy GitHub Actions zakończyło się powodzeniem.

Test sprawdza zgodność pola L z długością, oczekiwane ID licznika oraz
odtworzenie dokładnie tego samego RAW HEX po syntetycznym kodowaniu radiowym
C1 i T1 i ponownym parsowaniu.
