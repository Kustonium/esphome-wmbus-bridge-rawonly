# `wmbus_tx` — nadajnik stanowiskowy

[English version](README.md)

**Nadajnik** wM-Bus na płytkę LilyGO/SX1276. Istnieje po to, żeby generować
znane ramki — takie, których dokładny strumień bitów znamy z góry — i mierzyć
wobec nich odbiornik. To przyrząd pomiarowy, nie część produktu.

To jedyny kod nadawczy w tym repozytorium. Komponent odbiorczy świadomie nie ma
ścieżki TX (patrz notka w `components/wmbus_radio/transceiver.h`) i to się nie
zmienia: ten kod leży w `tests/`, nie trafia do żadnego przykładu i nie wybiera
go żadna konfiguracja odbiornika.

## Dlaczego trafił do repo

Powstał na potrzeby prac nad długimi ramkami LR1121 i mieszkał wyłącznie na
hoście Home Assistanta, poza kontrolą wersji — czyli wynik pomiaru zależał od
pliku, którego nikt nie mógł przejrzeć ani porównać. Dwie wady znalezione w nim
przy tej okazji — pętla taktowania bitów potrafiąca mielić bez karmienia
watchdoga oraz tryb C1, który nigdy nie wysyłał wskaźnika trybu C — to dokładnie
ten rodzaj, który ukrywa plik poza gitem.

## Co robi

| tryb | kodowanie liniowe | przepływność | ładunek w eterze dla ramki `L` |
|---|---|---|---|
| `t1` | 3-z-6 | 100 kb/s | `ceil(1,5 x (L + 1 + 2 x bloki))` |
| `c1` | brak | 100 kb/s | `2 + (L + 1 + 2 x bloki)` |
| `s1` | Manchester | 32768 b/s | `2 x (L + 1 + 2 x bloki)` |

`frame` to kompletne ciało warstwy łącza w hex, zaczynające się od pola L;
komponent sam dokłada CRC formatu A. Walidator wymusza `L + 1 == len(frame)`
i sufit 256 bajtów, który jest zarazem najdłuższą ramką, jaką wM-Bus potrafi
wyrazić.

Bity są wytaktowywane programowo wobec DCLK z SX1276 w ciągłym trybie FSK.
Timing należy do radia; ten kod ma tylko zdążyć postawić kolejny bit DATA przed
kolejnym zboczem zegara.

### Tryb C niesie wskaźnik w ładunku

Tryb C poprzedza warstwę łącza bajtami `0x54` i `0xCD` (format A) albo `0x3D`
(format B). Te dwa bajty siedzą **za** słowem synchronizacji, w ładunku — i tam
czyta je odbiornik: `Packet::link_mode()` rozpoznaje tryb po wiodącym `0x54`,
a `l_field()` bierze długość z indeksu 2, nie 0.

Do 23.09.2026 komponent ich nie wysyłał, więc `mode: c1` emitował nagą ramkę
DLL, której żaden odbiornik nie umiał sklasyfikować — widział pole L tam, gdzie
spodziewał się `0x54`, uznawał ramkę za T1 i przewracał się na 3-z-6 przy
danych, które nigdy nie były kodowane 3-z-6. Tryb najwyraźniej nigdy nie
przeszedł testu od końca do końca.

## `dclk_diagnostics`

Domyślnie wyłączone. Po włączeniu każda zmiana DATA dostaje znacznik czasu,
a po każdej udanej transmisji na `wmbus/txgen/diag/dclk` idzie raport:
`updates`, `span_us`, `max_us`, `long_gaps` i do 64 par `[stream_bit, dt_us]`.

**Użyteczną liczbą jest `span_us`, nie `long_gaps`.** DCLK jest sprzętowo
dokładny (`32 MHz / 320 = 100.000 kb/s`), więc `updates` interwałów musi zająć
`updates x 10 us`; każde 10 us ponad to oznacza jeden dodatkowy cykl DCLK,
czyli jeden nadmiarowy bit w eterze. Szum tej miary to około ±2 bity, bo
pierwszy i ostatni znacznik mają własny jitter. `long_gaps` używa progu 1,5
okresu bitu, więc łapie też spóźnienia nadrobione bez utraty cyklu — jeden
raport pokazał 13 długich przerw przy 2,8 bitu realnego nadmiaru.

To znaczniki wykonania kodu, nie sprzętowy pomiar zboczy DCLK. Klient MQTT
i sama instrumentacja zmieniają obciążenie pętli.

Zmierzone 22–23.09.2026: nadmiarowe bity pojawiają się mniej więcej co 100
bitów, czyli co 1 ms przy 100 kb/s, co odpowiada okresowi tiku FreeRTOS
ustawianemu przez ESPHome (`CONFIG_FREERTOS_HZ = 1000`). Rozjazd zegarów jest
wykluczony ilościowo: jeden nadmiarowy bit na sto to 1% błędu tempa, jakieś
500× tolerancja kwarcu. Podniesienie `cpu_frequency` płytki do 240 MHz
zmniejszyło zjawisko, ale go nie usunęło.

## Kompilacja

`check.yaml` to konfiguracja wyłącznie do kompilacji. Piny są zastępcze,
a adres brokera pochodzi z zakresu dokumentacyjnego — **nie wgrywać jej**.
Istnieje po to, żeby ten komponent był gdziekolwiek kompilowany; kod, którego
nie wybiera żadna konfiguracja, nie jest budowany przez nikogo — i tak właśnie
przetrwały obie opisane wyżej wady.

```bash
esphome compile tests/tools/wmbus_tx/check.yaml
```

## Użycie na sprzęcie

Nadawanie na 868 MHz jest regulowane. `power` i `interval` mają niskie wartości
domyślne, a pasmo 868 MHz ma limit wypełnienia; 869,7–870,0 MHz go nie ma
i dlatego prace stanowiskowe idą na 869,850. Identyfikator licznika w ramce
trzymać wyraźnie sztuczny, żeby przechwycenie nie dało się pomylić z realnym
urządzeniem.
