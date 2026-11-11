# Wkład w projekt

[English version](CONTRIBUTING.md)

Dzięki za zainteresowanie. Mile widziane są zgłoszenia błędów, sprawdzone
konfiguracje płytek, pomiary i kod. Zgłoszenia i dyskusje mogą być po polsku
albo po angielsku.

## Jak zmiana trafia do repozytorium

To repozytorium publikuje z osobnego drzewa deweloperskiego, więc zmiana nie
wchodzi przez gałąź pull requesta — jest tam najpierw przenoszona i trafia tutaj
jednym zgniecionym commitem sync. Twoje commity nie pojawią się w historii tego
repozytorium, mimo że kod jest Twój. Autorstwo jest odnotowywane w dwóch innych
miejscach: commit przenoszący w drzewie deweloperskim wymienia Twoje oryginalne
commity, a komentarz zamykający pull requesta mówi wprost, że kod jest Twój.

Drobną poprawkę zgłoś od razu jako pull request. Przy wszystkim, co zmienia
sposób odbioru radia, najpierw otwórz zgłoszenie albo dyskusję, żebyśmy ustalili,
jak to zmierzyć (niżej).

## Od czego zacząć czytanie

- [docs/RX_PIPELINE_PL.md](docs/RX_PIPELINE_PL.md) — jak ramka przechodzi od
  przerwania radia do MQTT.
- [docs/CHIP_SELECTION_PL.md](docs/CHIP_SELECTION_PL.md) — w czym który układ
  radiowy jest dobry, a gdzie ma trudności.
- `components/wmbus_radio/transceiver_<układ>.cpp` — po jednym sterowniku na
  układ (SX1262, SX1276, CC1101, LR1121). `packet.cpp` i `decode3of6.cpp` to
  wspólne dla wszystkich parsowanie ramki.
- [docs/DIAGNOSTIC_PL.md](docs/DIAGNOSTIC_PL.md) — co znaczą liczniki
  diagnostyczne.

## Sprawdzanie zmiany

**Testy parsera na komputerze**, bez ESP — tak samo jak w CI:

```bash
mkdir -p build
g++ -std=c++17 -Wall -Wextra -Wno-unused-variable -Itests/host -Icomponents/wmbus_radio \
  tests/host/test_core.cpp components/wmbus_radio/decode3of6.cpp components/wmbus_radio/packet.cpp \
  -o build/test_core && ./build/test_core
python tests/ci/check_example_defaults.py
```

Więcej o nich w [tests/README_PL.md](tests/README_PL.md).

**Kompilacja firmware:** skompiluj w ESPHome jedną z konfiguracji z `examples/`
na swojej kopii repozytorium. CI przy każdym pull requeście buduje wszystkie
układy i warianty płytek.

## Zmiany w odbiorze radia

Zmiana może poprawić RSSI, a mimo to gubić ramki, albo odwrotnie. Liczy się to,
czy odbieranych i dekodowanych ramek jest więcej, dlatego mierz tak:

1. Trzymaj **drugi, niezmieniony odbiornik** w tym samym miejscu i w tym samym
   czasie. Liczniki milkną na noc, a część w weekend, więc „przed” z wczoraj to
   inny zestaw liczników.
2. Porównuj **liczby, nie RSSI**: ramki odebrane, odrzucone i z jakiego powodu
   oraz które liczniki słychać. `diagnostic_mode: normal` publikuje to co 60 s
   (zobacz [docs/DIAGNOSTIC_PL.md](docs/DIAGNOSTIC_PL.md)).
3. Jeśli używasz dodatku do Home Assistant, jego eksport RX wypisuje każdą ramkę
   z każdej płytki z `frame_crc32`: ta sama emisja ma to samo CRC na każdym
   odbiorniku, więc widać dokładnie, które ramki złapała jedna płytka, a które
   zgubiła druga.
4. **Jedna zmiana naraz** i porównanie tego samego typu dnia (dzień roboczy
   z dniem roboczym).

W [docs/BENCHMARKS_PL.md](docs/BENCHMARKS_PL.md) są pomiary zrobione w ten
sposób.

## Nowe opcje

- Wartość domyślna musi zachować dzisiejsze działanie.
- Opcja dla użytkowników jest opisana w dokumentacji konfiguracji, po polsku
  i po angielsku. CI sprawdza, czy wartości domyślne wpisane w przykładach
  i tabelach zgadzają się ze schematem.

## Konfiguracja dla Twojej płytki

Sprawdzona konfiguracja płytki, której jeszcze nie ma w `examples/`, to jeden
z najbardziej przydatnych wkładów: dodaj ją w `examples/<układ>/<płytka>/`
i napisz, na jakim sprzęcie ją sprawdziłeś.

## Licencja

GPL-3.0. Skąd pochodzi kod, opisuje [docs/ATTRIBUTION_PL.md](docs/ATTRIBUTION_PL.md).
Jeśli przy zmianie pomagało Ci AI, to w porządku — napisz o tym w pull requeście
i napisz, co sprawdziłeś na prawdziwym sprzęcie.
