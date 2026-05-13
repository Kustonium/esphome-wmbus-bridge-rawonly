# Tor RX i kwalifikacja ramek

Ten projekt jest mostkiem **RAW-only**, ale `RAW` nie znaczy tutaj „każdy blob bajtów z radia”.

Komponent publikuje tylko telegramy, które przeszły wewnętrzną walidację ramki wM-Bus. Nieudane kandydaty mogą być liczone i raportowane w diagnostyce, ale nie trafiają na `telegram_topic`.

## Model przetwarzania

```text
IRQ z radia
  -> odczyt bajtów PHY z SX1262/SX1276
  -> zbudowanie kandydata na pakiet
  -> wskazanie trybu: T1, C1 albo wymuszone S1
  -> wyliczenie oczekiwanej długości kandydata
  -> parsowanie i walidacja
  -> usunięcie bajtów DLL CRC
  -> publikacja zweryfikowanego HEX do MQTT
```

## Ścieżka T1

T1 używa kodowania 3-out-of-6. Odbiornik nie ufa ślepo pierwszym 3 bajtom.

Obecna logika:

- czyta pierwsze 3 bajty,
- dla T1 doczytuje rozszerzony probe do `WMBUS_T1_LEN_PROBE_BYTES`, zanim policzy długość,
- dekoduje prefiks T1 3-out-of-6, żeby odczytać L-field,
- doczytuje oczekiwaną resztę,
- dekoduje cały T1,
- waliduje L-field i bloki DLL CRC,
- odrzuca kandydata, jeśli którykolwiek etap się nie zgadza.

Typowe etapy odrzucenia T1:

- `t1_decode3of6` — błędne symbole 3-out-of-6,
- `t1_l_field` — nieprawidłowy L-field po dekodowaniu,
- `t1_length_check` — kandydat krótszy niż oczekiwano,
- `dll_crc_first`, `dll_crc_mid`, `dll_crc_final` — błąd DLL CRC.

## Ścieżka S1

S1 jest dedykowaną, eksperymentalną ścieżką odbioru. Wybiera się ją jawnie przez `listen_mode: s1`; nie jest częścią trybu `both`.

Obecna logika:

- konfiguruje radio dla profilu S-mode / Manchester,
- wymusza `LinkMode::S1` dla kandydata na pakiet,
- czyta surowe bajty po sync S1,
- próbuje dekodowania Manchester,
- waliduje L-field i bloki DLL CRC,
- publikuje zweryfikowany HEX telegramu do MQTT,
- odrzuca kandydata, jeśli nie przejdzie Manchester, długość albo CRC.

Typowe etapy odrzucenia S1:

- `s1_precheck` — kandydat za krótki,
- `s1_manchester` — błąd dekodowania Manchester,
- `s1_l_field` — nieprawidłowy L-field po dekodowaniu,
- `s1_length_check` — kandydat krótszy niż oczekiwano,
- `dll_crc_*` — błąd DLL CRC.

`listen_mode: both` nadal oznacza tylko T1/C1. S1 trzeba wybrać jawnie i używa własnego profilu RF oraz własnej domyślnej częstotliwości.

## Ścieżka C1

C1 zaczyna się od `0x54` i używa wariantów drugiego bajtu sync. Radia cyklują drugi bajt sync w trybach C1/BOTH, bo rzeczywiste urządzenia C1 mogą używać więcej niż jednego wariantu.

Obecna logika:

- rozpoznaje C1 po pierwszym bajcie `0x54`,
- sprawdza preambułę bloku C1,
- usuwa dwa początkowe bajty C-mode,
- używa L-field i formatu ramki do policzenia długości,
- waliduje DLL CRC,
- odrzuca kandydata, jeśli którykolwiek etap się nie zgadza.

Typowe etapy odrzucenia C1:

- `c1_precheck`,
- `c1_preamble`,
- `c1_l_field`,
- `c1_length_check`,
- `dll_crc_*`.

## Co publikuje `telegram_topic`

`telegram_topic` publikuje `frame->as_hex()` tylko dla poprawnie zweryfikowanych ramek.

To znaczy:

- T1 został zdekodowany z 3-out-of-6,
- S1 został zdekodowany z kodowania Manchester,
- C1 został znormalizowany przez usunięcie początkowych bajtów C-mode,
- bajty DLL CRC zostały sprawdzone i usunięte,
- payload nadal nie jest zdekodowany jako licznik.

Czyli `RAW-only` oznacza **brak dekodowania licznika na ESP**, a nie przepychanie dowolnych śmieci z radia.

## Diagnostyka a publikacja

Diagnostyka może liczyć albo opcjonalnie publikować nieudane kandydaty:

- `payload_size_unknown`,
- `false_start_like`,
- `preamble_read_failed`,
- `t1_decode3of6`,
- `s1_manchester`,
- `dll_crc_failed`,
- `truncated`.

To są dane przydatne do analizy RF i odbiornika, ale nie są poprawnymi telegramami.

Zasada w skrócie:

```text
kandydat != poprawny telegram
surowy blob diagnostyczny != payload z telegram_topic
```
