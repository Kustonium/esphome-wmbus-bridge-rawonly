# Diagnostyka czasu pracy LR1121 (2026-09-06)

[English version](LR1121-runtime-diagnostics.md)

Zmiana wyłącznie diagnostyczna; modulacja, maski przerwań, polityka timeoutu
BUSY i decyzje o restarcie RX pozostają bez zmian. Pozostałe sterowniki radiowe
nie zwracają diagnostyki czasu pracy.

Co 60 sekund zadanie główne publikuje migawkę JSON jako retained z QoS 1 na
`<diagnostic_topic>/radio_runtime`, od `diagnostic_mode: low` w górę. Ta sama
migawka trafia do logu **wyłącznie przy `diagnostic_mode: dev`**: zrzut
rejestrów i liczników raz na minutę to instrumentacja stanowiskowa, a na
działającym węźle nie mówi nic, czego nie mówi podsumowanie.

Liczniki są kumulatywne od startu urządzenia. Archiwizuj ten temat w trakcie
testu — retained zachowuje wyłącznie najnowszą migawkę, więc przebieg, na
którym Ci zależało, znika w chwili nadejścia kolejnego. `uptime_ms` zeruje się
przy restarcie i przepełnia po około 49 dniach.

- `busy_timeouts`: nieudane oczekiwania, łącznie z tymi przy starcie i przy
  odczytach bezpośrednich.
- `status_samples`, `cmd_fail_observations`, `cmd_perr_observations`: próbkowane
  wartości Stat1, a NIE liczba unikalnych nieudanych komend. Kolejne transakcje
  mogą raportować ten sam wynik poprzedniej komendy. Nieprawidłowe odpowiedzi
  SPI też potrafią wyglądać jak CMD_FAIL.
- `stat1`, `stat2`, `chip_mode`, `reset_status`: migawka z ostatniej komendy lub
  odczytu bezpośredniego, nie twierdzenie o trybie w chwili publikacji.
  ResetStatus jest lepki; ta łatka go nie czyści i nie potrafi liczyć
  poszczególnych resetów.
- `irq_samples`, `rx_done_observations`, `timeout_observations`,
  `len_error_observations`, `read_without_rx_done`, `last_irq`: próbkowane przed
  odczytami bufora. Powtórne odczyty niewyczyszczonego zatrzasku IRQ mogą być
  liczone wielokrotnie. To nie są liczniki preambuł ani liczba emisji w eterze.
- `packet_samples`, `packet_received_observations`, `packet_abort_observations`,
  `packet_length`, `packet_flags`: wartości GetPacketStatus, odczytywane i tak
  przez istniejącą ścieżkę RSSI. Flagi pakietu są surowe; bit 1 = odebrany,
  bit 2 = przerwany.

Pola współdzielone między zadaniami są atomowe, ale cały JSON nie jest jedną
chwilową migawką. Odczyt IRQ dokłada jeden bezpośredni odczyt SPI i jedno
oczekiwanie na BUSY na każdą próbę załadowania bufora. To nie jest zmiana
bezkosztowa: zweryfikuj nominalny odbiór, zanim porównasz wyniki przy słabym
sygnale. Żadne dodatkowe źródła przerwań nie są włączane.

Źródła: LR1121 User Manual rev. 2.2, s. 27-31, 38-39, 51, 80;
Semtech SWDR001 2.4.1, `lr11xx_system.c` i `lr11xx_radio.c`.

Znana, osobna sprawa dotycząca S1: istniejąca stała SYNC_WORD_VALID błędnie
używa bitu 2 (TX_DONE); udokumentowany jest bit 5. Celowo nie zmieniane w tym
miejscu: włączenie prawdziwego wczesnego przerwania bez naprawy dyspozytora
odbioru mogłoby przerywać pakiety. Nie dotyczy to maski IRQ dla T1 używanej
w eksperymencie z tłumieniem.

## Ograniczony bufor FIFO i próbki odrzuceń

Przy włączonej diagnostyce zbiorczej LR1121 publikuje dodatkowo wiadomości
retained z QoS 1:

- `<diagnostic_topic>/lr_pipeline`: kumulatywne liczniki konwersji z zadania
  głównego, co 60 sekund. `converted = valid + decode_failed + length_failed +
  crc_failed + other_failed`. Liczą przed filtrem trybu nasłuchu stosowanym po
  parsowaniu. NIE obejmują pakietów odrzuconych w zadaniu odbiorczym przed
  konwersją. Dla wczesnych odrzuceń porównuj z istniejącymi licznikami
  `rx_path` w podsumowaniu.
- `.../lr_fifo/0` do `.../lr_fifo/7`: rotacyjne osiem próbek bufora RX, brane
  najwyżej raz na pięć sekund, niezależnie od późniejszego powodzenia czy
  porażki. Dwuelementowa, nieblokująca kolejka FreeRTOS przenosi kopie z RX do
  zadania głównego. Przy pełnej kolejce ginie próbka, nigdy odebrany pakiet.
  Te bajty mogą zawierać szum po krótkim telegramie, bo odbiór ma stałą
  długość; nie licz nieprawidłowych symboli w ogonie jako uszkodzenia
  telegramu.

  Od 2026-09-22 próbką jest **cały 255-bajtowy bufor czytany od offsetu 0**
  (`ReadBuffer8(0, 255)`, UM 2.2 s. 35; RAM odbiorczy jest adresowalny poza
  trybem uśpienia, s. 88), a nie odczyt wielkości pakietu, który konsumuje
  dekoder. `fifo_dump` wynosi wtedy 1, a `raw_length` 255. Powód: odczyt
  wielkości pakietu bierze `payload_len` bajtów raportowanych przez
  `GetRxBufferStatus`, a to jest długość, którą silnikowi pakietowemu
  *zadeklarowano* — więc nigdy nie pokaże, czy silnik pisał dalej. Przy
  `payload_length` ustawionym poniżej 255 bajty za tą granicą są jedynym
  miejscem, gdzie ta odpowiedź może się pojawić. Koszt to jeden dodatkowy
  255-bajtowy odczyt SPI po `RX_DONE`, najwyżej raz na pięć sekund; dekoder
  dalej dostaje odczyt wielkości pakietu, bez zmian.

  `probe` niesie **tylko do odczytu** próbkę czterech niezadokumentowanych
  adresów rejestrów, braną zaraz po `RX_DONE` w tym samym przebiegu. `F20384`
  i `F20368` to para licznik pozycji / koniec pakietu, którą sterownik Sidewalk
  samego Semtecha odpytuje i zapisuje na LR11xx, bez nazwanego makra
  w publicznym SDK; `F30028` i `F30030` to rejestry adresu bazowego i rozmiaru
  FIFO odbiorczego, udokumentowane dla następcy LR20xx w datasheecie LR2021
  rev. 2.2, tabele 5-2 i 5-3. Czy LR1121 ma cokolwiek podpięte pod tymi
  adresami, to otwarte pytanie, dla którego te wartości istnieją. **Nic do nich
  nie jest zapisywane.** Wartości są interpretowalne wyłącznie wobec linii
  `Register probe baseline (pre-RX, read-only)`, logowanej raz przy starcie,
  branej po skonfigurowaniu radia, ale zanim RX zostanie uzbrojony — w tamtej
  chwili nic nie zostało odebrane, więc licznik pozycji nie może prawomocnie
  trzymać liczby bajtów. Wartość, która nigdy nie rusza się z punktu odniesienia
  albo czyta się jako same zera lub same jedynki, jest **wynikiem**, a nie
  usterką.

  Ten punkt odniesienia jest publikowany na własny temat retained,
  `<diagnostic_topic>/probe_baseline`, i logowany raz z zadania głównego jako
  `Register probe baseline (pre-RX, read-only)`. Dwa powody, dla których nie
  jest po prostu logowany z `setup()` ani wciśnięty do `radio_runtime`:
  `setup()` komponentu wykonuje się, zanim wstaną WiFi i API, więc linia
  wypisana stamtąd **nigdy** nie dociera do `esphome logs` (restart z podpiętym
  logiem nie pomaga), a dopisanie czterech wartości do JSON-a `radio_runtime`
  przepchnęło tę linię poza bufor loggera — log drukował wtedy obiekt ucięty
  w połowie klucza, podczas gdy MQTT niósł całość. Diagnostyka po cichu ucięta
  w jednym ze swoich dwóch wyjść jest gorsza niż rozbita na dwie linie.

  `packet_start` i `packet_len` niosą wartości `GetRxBufferStatus` dla tego
  przechwycenia, więc zrzut da się podzielić na zadeklarowany pakiet
  (`[packet_start, packet_start + packet_len)`) i wszystko poza nim. Bez nich
  podział trzeba by zakładać na podstawie skonfigurowanego `payload_length`
  — a to dokładnie to założenie, które ten zrzut ma sprawdzać. Bajty spoza tego
  zakresu nie są zdekodowanym telegramem i nikt tak ich nie przedstawia: to
  cokolwiek RAM odbiorczy trzymał w chwili odczytu, z tego przechwycenia,
  poprzedniego albo z szumu.

  Konsekwencja dla `verify`: porównanie dwóch odczytów obejmuje nadal wyłącznie
  `payload_len` bajtów, więc `differing_bytes` i `first_difference` są indeksami
  w odczycie wielkości pakietu, a **nie** w 255-bajtowym zrzucie, obok którego
  są publikowane. Zrzut nie jest tym buforem, który porównano.
- `.../lr_drop/0` do `.../lr_drop/7`: rotacyjne osiem nieudanych konwersji,
  najwyżej raz na pięć sekund. Zawiera faktyczny powód i etap z parsera,
  długości, statystyki symboli 3-z-6 oraz surowe wejście (do 256 bajtów). To
  wejście może być już przycięte przez zadanie odbiorcze; niekoniecznie jest
  pełnym FIFO.

Żadne ustawienia radia, polityka restartu ani decyzje dekodera nie ulegają
zmianie. Próbkowanie dokłada ograniczony narzut na CPU, pamięć i MQTT;
zweryfikuj nominalny odbiór kontrolny. Nie trzeba włączać istniejącego
`diagnostic_publish_raw` ani szczegółowego logowania. Próbkowanie surowego FIFO
i odrzuceń działa niezależnie: **nie parować ich po numerze slotu**. Do
korelacji używać identyfikatora bootu oraz czasu przechwycenia i pobudki. Sloty
retained są nadpisywane; stare sloty z wcześniejszych bootów zostają, dopóki nie
zostaną nadpisane. Zawsze filtrować po boot ID i czasie testu. Po teście
eksportować wszystkie te tematy, a także `radio_runtime`, istniejące
podsumowanie diagnostyczne i JSON odbiornika. Przy rozłączonym MQTT nic nie jest
publikowane; te próbki nie są trwałym rejestratorem.

## Wczesne wyniki RX i opcjonalna weryfikacja SPI

`lr_pipeline` zawiera teraz końcowe liczniki zadania odbiorczego od startu:
`rx_entered`, `rx_queued`, `rx_queue_failed`, `rx_preamble_failed`,
`rx_weak_probe_aborted`, `rx_size_failed`, `rx_payload_failed`, `rx_s1_failed`.
W spoczynku `rx_entered` równa się sumie wszystkich siedmiu wyników. W trakcie
odbioru atomowe migawki mogą różnić się o jedną próbę w locie. Liczą one próby
odbioru wyzwolone przerwaniem, a nie unikalne przechwycenia radia. Rozbieżności
między kolejką a konwersją mogą też obejmować wpisy oczekujące w kolejce oraz
filtrowanie po trybie nasłuchu. Kluczami korelacji pozostają czas pobudki przy
odrzuconych pakietach i czas przechwycenia przy próbkach FIFO; nie twierdzi się,
że istnieje dokładna odpowiedniość jeden-do-jednego między niezależnie
próbkowanymi rekordami.

Żeby uruchomić ingerujący eksperyment z buforem, tylko na LR1121:

```yaml
wmbus_radio:
  lr1121_verify_buffer: true
```

Domyślnie wyłączone. Dla najwyżej jednego próbkowanego przechwycenia na 5 sekund,
po `RX_DONE`: `SetStandby(XOSC)`, oczekiwanie na BUSY, weryfikacja powodzenia
komendy i trybu standby, pobranie offsetu i długości bufora, dwukrotny odczyt
FIFO spod tego samego adresu, ponowna weryfikacja offsetu, długości i trybu.
Nie jest wydawane ani `ClearRxBuffer`, ani reset. Pierwotna pierwsza kopia idzie
dalej przez niezmieniony dekoder; druga kopia służy wyłącznie do porównania.
Istniejące `restart_rx` uzbraja odbiór ponownie. **To może przerwać kolejny
pakiet i NIE jest pasywnym pomiarem czułości.** UM 2.2 s. 16, 35, 88 dokumentuje
standby i adresowalny RAM odbiorczy dostępny poza trybem uśpienia.

Pola próbki FIFO: `fifo_dump` = 1, gdy `raw` to cały 255-bajtowy bufor czytany
od offsetu 0, a 0, gdy to odczyt wielkości pakietu (patrz wyżej); `packet_start`
i `packet_len` lokalizują zadeklarowany pakiet wewnątrz tego zrzutu. `verify` = 0
wyłączone lub niezamówione, 1 nierozstrzygające (kontrole BUSY, statusu komendy,
trybu lub wskaźników nie przeszły), 2 identyczne, 3 różne. `differing_bytes`
liczy nierówne bajty; `first_difference` to offset bajtowy liczony od zera albo
255, gdy różnic nie ma.

Pola `verify` opisują **odczyt wielkości pakietu, czyli `payload_len` bajtów**,
co nie jest tym samym zakresem co zrzut `raw` publikowany obok, i jest krótsze
niż 255, ilekroć `payload_length` skonfigurowano poniżej 255. Równość nie
dowodzi poprawnej demodulacji RF, a stabilny, deterministyczny błąd SPI nie jest
wykluczony. Rozbieżność przy zweryfikowanym standby jest przesłanką do zbadania
ścieżki odczytu, a nie automatycznym dowodem złego odbioru RF.

## Zapis oczekiwanej długości pakietu (eksperyment stanowiskowy, domyślnie off)

```yaml
wmbus_radio:
  lr1121_expected_len_override: 64   # 0 = wyłączone, domyślnie
```

Wartość niezerowa jest wpisywana w bity `[31:20]` rejestru `0x00F20368` po
każdym `SetStandby(XOSC)` i przed `SetRx`, nadpisując długość zadeklarowaną
przez `SetPacketParams`. **To pierwszy zapis do niezadokumentowanego rejestru
w tym komponencie.** To praca stanowiskowa, nie wspierana konfiguracja, i przy
włączeniu wypisuje jedno głośne ostrzeżenie.

Co czyni ten adres używalnym, a nie zgadywanym — zmierzone 2026-09-22: pole
czytało 255 przy radiu skonfigurowanym na 255 i *niczym jeszcze nieodebranym*,
a 64 po zmianie `payload_length` na 64. Punkt odniesienia sprzed uzbrojenia RX
jest tym, co odróżnia „odbija `SetPacketParams`" od „trzyma długość ostatniego
pakietu" — w trybie stałej długości te dwie wartości są poza tym zawsze równe.
Pole jest 12-bitowe, więc wyraża do 4095, czyli powyżej 8-bitowego
`pld_len_in_bytes` z publicznego API.

Zapis jest bramkowany wersją firmware radia i odmawia, z ostrzeżeniem, na
czymkolwiek innym niż obraz, wobec którego został zweryfikowany (tutaj `0x0101`).
Niezadokumentowany rejestr jest własnością obrazu firmware, nie obietnicą.

**Jak czytać wynik:** `packet_len` w próbkach FIFO pochodzi z
`GetRxBufferStatus`. Jeśli idzie za nadpisaniem, a nie za skonfigurowanym
`payload_length`, silnik słucha rejestru i zapis jest skuteczny. Jeśli dalej
raportuje wartość skonfigurowaną, rejestr nie jest ścieżką sterowania z tej
strony — co **zamyka** pytanie, a nie jest porażką.

Zaczynaj w bezpieczną stronę, nadpisując *poniżej* skonfigurowanej długości:
bufor ma 255 bajtów, a obniżenie oczekiwania nie może go przepełnić.
Podniesienie powyżej 255 to osobny krok i wymaga nadajnika, który faktycznie
wysyła więcej niż 255 bajtów surowych; najdłuższy telegram w normalnym ruchu
polowym to tutaj 245.

## Próbkowanie licznika pozycji w trakcie ramki (eksperyment, domyślnie off)

```yaml
wmbus_radio:
  lr1121_sync_probe: true
```

Dodaje `SYNC_WORD_VALID` do maski przerwań T1/C1, żeby zadanie odbiorcze budziło
się, gdy ramka jeszcze przychodzi, czytało raz `0x00F20384` bity `[27:16]`,
czyściło **wyłącznie** ten zatrzask i wracało. Wyniki trafiają na
`<diagnostic_topic>/sync_probe` co 60 s: `sync_wakes`, `ptr_last`, `ptr_max`.

Pobudka jest **pochłaniana wewnątrz sterownika**: po spróbkowaniu odpytuje on
licznik aż do `RX_DONE`, a potem wchodzi w zwykłe przechwycenie, więc wołający
nigdy się nie dowiaduje, że była wczesna pobudka. Zwracanie zamiast tego to
właśnie robiły dwie pierwsze próby — i za każdym razem wyłączały odbiór:
`receive_frame()` otwiera każdą próbę przez `restart_rx()`, czyli
`SetStandby(XOSC)` + `SetRx`, co przerywa ramkę będącą jeszcze w eterze.
Zmierzone: 59 pobudek na minutę, zero przechwyceń. `sync_polls` liczy odczyty
wykonane w tym oknie, a `sync_timeouts` ramki, przy których `RX_DONE` nie
przyszło w czasie trwania ramki powiększonym o 50 ms.

Czyszczenie bitu słowa synchronizacji też jest nośne. DIO1 stoi wysoko, dopóki
jakiekolwiek niezamaskowane przerwanie jest zatrzaśnięte, a pin czytany jest na
zboczu narastającym — więc niewyczyszczony zatrzask słowa synchronizacji trzyma
linię wysoko i `RX_DONE` w ogóle nie wytwarza zbocza. Pierwsza wersja tej sondy
celowo nie czyściła niczego, żeby „nie wchodzić w drogę", i zmierzyła 59 pobudek
przy zerze przechwyceń — po cichu wyłączyła odbiór. Czyszczony jest wyłącznie
bit 5; `RX_DONE` i bity błędów muszą przeżyć. Jeśli pakiet zdąży się skończyć
w trakcie próbkowania, obsługa czyta `GetRxBufferStatus` ponownie i przechodzi do
normalnego przechwycenia, zamiast czekać na zbocze, które już minęło.

Po co to istnieje: ramka dłuższa niż 255-bajtowy bufor zawija się, a bajty, które
nadpisze, przepadają — po `RX_DONE` początek takiej ramki nie istnieje już
nigdzie. Przechwycenie jej oznacza więc drenowanie w trakcie odbioru, a to
wymaga wiedzy, ile już przyszło. `0x00F20384` czyta 0 po `RX_DONE`; czy jest
żywy w trakcie ramki, to dokładnie to, co ta sonda mierzy. Licznik, który tu
zostaje na zerze, oznaczałby, że drenaż trzeba taktować przepływnością.

**`IRQ_SYNC_WORD_VALID` było błędne do 2026-09-22** — wskazywało bit 2, czyli
`TX_DONE`. W sterowniku wyłącznie odbiorczym ten bit nigdy nie strzela, więc
maska S1, która go zawierała, niosła martwy bit, a stojąca za nim diagnostyka S1
„sync trafił, ale nie ma pakietu" nigdy się nie uruchomiła. Stała wskazuje teraz
udokumentowany bit 5 (UM 2.2 s. 37-39). Poprawienie numeru samo w sobie niczego
w S1 nie zmienia, ale *włączenie* prawdziwego przerwania już tak: ścieżka S1
czyści cały zatrzask IRQ, co zabrałoby ze sobą `RX_DONE`. Dlatego S1 nie prosi
już o ten bit wcale, a włączenie go dla S1 wymaga najpierw pracy w dyspozytorze.

**Znany efekt uboczny przy włączonej sondzie:** każda wczesna pobudka przebiega
normalną ścieżką odbioru, nie znajduje pakietu i liczy się jako
`rx_preamble_failed`. Ten licznik jest zawyżony, dopóki sonda jest włączona,
i nie należy go porównywać z przebiegami bez niej.

## Drenowanie ramki w trakcie odbioru (eksperyment, domyślnie off)

```yaml
wmbus_radio:
  lr1121_sync_probe: true     # wymagane - bez wczesnej pobudki nie ma okna
  lr1121_drain: true
```

Wewnątrz okna słowa synchronizacji sterownik kopiuje teraz ramkę na bieżąco.
`0x00F20384` liczy bajty odebrane w bieżącej ramce **bezwzględnie** — doszedł do
325 na ramce 326-bajtowej, więc nie zawija się na 256, mimo że bufor tak.
Bajt *k* leży zatem na pozycji *k* mod 256 i pozostaje czytelny do przyjścia
bajtu *k*+256. Zmierzony margines przy 100 kb/s: jeden odczyt co ~2,8 ms wobec
20,5 ms terminu nadpisania, około 9 odczytów na ramkę.

Odczyty zatrzymują się na szwie pierścienia, żeby pojedynczy `ReadBuffer8` nigdy
nie przekraczał zawinięcia, a jego pole długości jest 8-bitowe, więc żaden
odczyt nie przekracza 255 bajtów. Przechwycenie jest ograniczone do 512 bajtów.

**Testuj najpierw na ramce, która się nie zawija.** Przy oczekiwanej długości
równej 255 lub mniejszej zwykły odczyt po `RX_DONE` jest kompletną, poprawną
kopią tych samych bajtów, więc drenaż da się sprawdzić wobec niego na układzie,
bajt w bajt, bez żadnej rekonstrukcji offline:

| pole | znaczenie |
|---|---|
| `drain_frames` | ramki, w których porównanie było możliwe |
| `drain_match` / `drain_mismatch` | jak te porównania wypadły |
| `drain_bytes_last` | ile bajtów zdrenowano z ostatniej ramki |
| `drain_diff_last`, `drain_first_diff` | rozmiar i pozycja ostatniej rozbieżności |
| `drain_served` | ramki, które dekoder dostał z drenażu zamiast z odczytu bufora |

`sync_probe` był schematem 2, gdy dodano `drain_served`, a teraz jest schematem 3;
patrz sekcja o automacie długości niżej.

Gdy ramka się zawinie, ten wzorzec odniesienia przestaje istnieć — odczyt po
`RX_DONE` nie zawiera już początku ramki — i te liczniki porównania przestają
cokolwiek znaczyć. Dokładnie dlatego drenaż dowodzi się poniżej 255, zanim
zaufa mu się powyżej.

Powyżej 255 drenaż jest publikowany zamiast porównywany: ostatni ukończony
drenaż trafia na `<diagnostic_topic>/lr_drain` jako `{seq, len, raw}` co 60 s,
bo odczyt po `RX_DONE` nie może już służyć za wzorzec, gdy początek ramki został
nadpisany. Sprawdzenie go oznacza korelację tych bajtów ze strumieniem
odtworzonym poza urządzeniem. Migawka i zadanie odbiorcze nie są zazębione, więc
próbka wzięta podczas drenowania kolejnej ramki może się rozerwać; `seq`
identyfikuje próbę, a rozerwana próbka wywraca korelację wprost, zamiast
produkować wiarygodnie wyglądające błędne bajty.

Żadne automatyczne wdrożenie firmware ani rozpoczęcie eksperymentu nie jest
częścią tej zmiany.

# Eksperyment z offsetem przy długich ramkach (2026-09-22)

Przy `lr1121_drain: true` `lr_drain` używa schematu 2 i zawiera ograniczony ślad
wywołań `ReadBuffer8` obok surowej ramki. Każdy wiersz ma układ z `trace_fields`:
`us`, `target`, `copied`, `packet_len`, `start`, `offset`, `size`. `us` to czas
przed odczytem bufora, liczony od wejścia w pętlę drenażu; `target` to
spróbkowany licznik bezwzględny (albo oczekiwana długość dla ogona po
`RX_DONE`), `copied` to liczba już zdrenowanych bajtów. `packet_len` i `start`
pochodzą z `GetRxBufferStatus` tuż przed drenowaniem do tego celu. Podział na
granicy pierścienia współdzieli tę samą próbkę statusu. Przechowywanych jest
najwyżej 16 wywołań; `trace_total` podaje łączną liczbę, więc ucięcie jest
widoczne.

Adresowanie pozostaje `copied % 256`. Ten eksperyment mierzy, czy wskaźnik
początku się zmienia albo jest niezerowy; nie zakłada, że dodanie go jest
poprawne. Dodatkowa transakcja statusu zmienia tempo odpytywania. Jeśli treść
się poprawi, samo to nie odróżni problemu ze wskaźnikiem od opóźnienia
widoczności zapisu. Wiersz śladu nie jest dodawany, gdy nie ma nowych bajtów do
skopiowania.

Zadanie RX przenosi surowe bajty i ślad razem przez jednoelementową kolejkę
nadpisującą. Zadanie główne publikuje najnowszą kompletną próbkę; to nie jest
archiwum wszystkich ramek. Usuwa to też wcześniejszą niesynchronizowaną
współdzieloną migawkę. Publikowane są wyłącznie ukończone przechwycenia.

Uruchamiać z istniejącą ramką testową 326 B, `override: 326`, włączoną sondą
i drenażem. Potwierdzić nowy boot i `lr_drain.schema == 2`, wyeksportować
retained z MQTT, a potem skorelować bajty na zapisanych granicach fragmentów.
Zerowe `start` we wszystkich obserwowanych odczytach osłabia proponowaną
poprawkę ze wskaźnikiem początku. Dla ramek zawijających się **nie** używać
`drain_mismatch` jako werdyktu o poprawności.

**Wynik (2026-09-22/23).** `start` było zerem w każdym zapisanym odczycie, więc
poprawkę ze wskaźnikiem początku porzucono. Przy dopasowaniu uwzględniającym
wstawienia i usunięcia, zamiast korelacji wyrównanej bajtowo, drenaż okazuje się
poprawny: ramka 326 B wróciła co do bitu, 2604/2604, przez zawinięcie.
Pozostałe różnice to nadmiarowe bity wstawione przez *nadajnik* — programowa
pętla DCLK wywłaszczana mniej więcej co 1 ms — potwierdzone ze znaczników czasu
samego nadajnika, a nie wywnioskowane z odebranego strumienia. Porównanie
wyrównane bajtowo nie odróżnia jednego wstawionego bitu od złego adresu odczytu:
oba załamują się do poziomu losowego w jednym punkcie.

## Wyprowadzanie długości z samej ramki

```yaml
wmbus_radio:
  lr1121_sync_probe: true     # wymagane - bez wczesnej pobudki nie ma okna
  lr1121_drain: true          # wymagane - nagłówek musi być w ręku, żeby go przeczytać
  lr1121_auto_length: true
```

Domyślnie wyłączone, a te trzy opcje działają wyłącznie razem: ustawienie samego
`lr1121_auto_length` uzbroiłoby silnik sufitem i nigdy go nie zawęziło, więc
konfiguracja jest **odrzucana**, zamiast po cichu robić coś złego. Odrzucana jest
też razem z `lr1121_expected_len_override`, który robi to samo odwrotnym
sposobem — przypina każde przechwycenie do jednej długości.

RX jest uzbrajany sufitem `DRAIN_CAP` bajtów zamiast stałą długością. Gdy tylko
zdrenowane zostaną cztery bajty, sterownik czyta z nich pole L, liczy, ile
bajtów surowych ramka naprawdę zajmuje, i wpisuje to do `0x00F20368[31:20]`
**gdy ramka jest jeszcze w eterze**, więc `RX_DONE` pada w prawdziwym końcu.
Ten zapis w trakcie odbioru to sposób, w jaki tego rejestru używa sterownik
Sidewalk samego Semtecha, i jedyny sposób, w jaki silnik stałej długości może
zatrzymać się na długości, której nie mógł znać przy uzbrajaniu RX.

Arytmetyka jest ta sama, której SX1262 używa od czasu napisania swojej ścieżki
AN1200.53, przeniesiona do `frame_length.h`, żeby istniała w jednej kopii:
`expected_raw_len_t1()` (3-z-6, pole L z dwóch pierwszych bajtów surowych),
`expected_raw_len_c1()` (bez kodowania, L pod indeksem 2 za wskaźnikiem trybu C)
oraz `expected_raw_len_s1()` (Manchester, z przeszukiwaniem polaryzacji
i tolerancjami zmierzonymi na granicy czułości).

Dwie reguły, których implementacja pilnuje:

**Nigdy nie skraca wstecz.** Długość jest zapisywana tylko wtedy, gdy jest
większa od już zdrenowanej. Powiedzenie silnikowi, że pakiet skończył się
wcześniej, niż się skończył, jest nieodwracalne.

**Nieudane wyprowadzenie nigdy nie jest gorsze od nieprzymierzania się.** Jeśli
po 48 zdrenowanych bajtach nie da się odczytać długości, wpisywane jest
`payload_length_` — dokładnie to, co płytka przechwytuje dziś bez tej ścieżki.
Dla porównania: w SX1262 nieudane wyprowadzenie leci do limitu 512 bajtów
i kosztuje 125 ms głuchoty.

| pole w `sync_probe` | znaczenie |
|---|---|
| `auto_len_resolved` | ramki, których długość pochodzi z ich własnego pola L |
| `auto_len_fallback` | ramki, gdzie nie dało się jej odczytać, więc użyto `payload_length` |
| `auto_len_last` | ostatnia wyprowadzona długość, w bajtach surowych |

`sync_probe` jest schematem 3 od czasu ich dodania.

**Potwierdzone na sprzęcie 2026-09-24, we wszystkich trzech trybach.** Otwartym
pytaniem było, czy LR1121 honoruje zapis do tego rejestru *w trakcie* odbioru —
Sidewalk używa go tak, ale to był wniosek ze źródeł. Honoruje:
`drain_bytes_last` wraca jako wyprowadzona długość, a nie jako sufit, którym
uzbrojono RX.

Jeden telegram L=0xBE, trzy tryby, trzy różne długości, każda policzona z tego
samego pola L arytmetyką swojego trybu:

| tryb | `auto_len_last` | wyprowadzone / fallback | uwaga |
|---|---:|---|---|
| T1 | **326** | 57 / 1 | 3-z-6, ×1,5 |
| S1 | **434** | **44 / 0** | Manchester, ×2 |
| C1 | **219** | 59 / 59 | bez kodowania, + 2 bajty wskaźnika |

Żadnej z tych liczb nie ma w kodzie źródłowym, więc implementacja zwracająca
stałą albo powtarzająca poprzednią odpowiedź nie mogłaby ich wyprodukować.

S1 testowano pierwszy celowo. `transceiver_sx1262.cpp` odnotowuje, że na *tamtym*
układzie każde przechwycenie S1 kończy się na `buffer_cap`, bo ramka nie zaczyna
się na chipie 0 bufora, więc długość nigdy nie jest wyprowadzana. Na LR1121 jest
wyprowadzana 44 razy na 44 — założenie tutaj trzyma. S1 wypada też lepiej niż T1
z tego samego powodu, dla którego lepiej dekoduje: wyprowadzenie potrzebuje
czterech czystych pierwszych bajtów, a przy 32768 b/s nadajnik wstawia bity
znacznie rzadziej.

Równy podział w C1 nie jest wadą automatu. To cyklowanie słowa synchronizacji
trybu C, które fałszywie synchronizuje się na wskaźniku trybu C, więc takie
przechwycenia zaczynają się od `FF 44` zamiast `54 CD` i nie da się z nich
odczytać długości. Wpadają w fallback, co jest zachowaniem zaprojektowanym,
a nie jego porażką.

**Nadal niepokazane:** że automat dostosowuje się do *zmiennej* długości.
Stanowisko nadaje jedną ramkę w kółko, więc trzy tryby dają trzy liczby, ale nie
trzy długości w obrębie jednego trybu. Rozstrzygnąłby to realny ruch z różnymi
licznikami — obserwować, czy `auto_len_last` zmienia się między ramkami.

## S1: sonda działa i tam — i okazała się naprawą

Od 2026-09-23 sonda słowa synchronizacji i drenaż działają także
w `listen_mode: s1`. **Sprostowane 2026-09-23, tego samego dnia:** było to
napisane jako pomiar i nim nie jest — zmienia zachowanie przechwytywania w S1,
i S1 zaczyna odbierać.

Maska IRQ ustawia `SYNC_WORD_VALID`, ilekroć `lr1121_sync_probe` jest włączone,
*niezależnie od trybu*, podczas gdy gałąź pochłaniająca tę pobudkę wykluczała S1.
Więc w S1 z włączoną sondą sterownik budził się na słowie synchronizacji, czytał
`GetRxBufferStatus`, widział `payload_len == 0`, zwracał nieudaną próbę — a
`receive_frame()` odpowiadał `restart_rx()`, przerywając ramkę, która wciąż
nadchodziła. To ta sama pułapka, która jest opisana wyżej dla T1, utajona w S1,
bo do tej pory nikt nie uruchomił S1 z sondą.

Zmierzone zaraz po zmianie: `converted` 323, `valid` 311 (96%), `decode_failed`
0, `rx_preamble_failed` 0, `sync_timeouts` 0 oraz **`drain_match` 323 wobec
`drain_mismatch` 0** — kontrola na układzie, miarodajna tutaj, bo ramka
255-bajtowa nie zawija pierścienia.

`ptr_max` nadal odpowiada na pierwotne pytanie, gdy S1 zawiedzie z innego
powodu: 0 znaczy, że modem nic nie słyszy, a rosnąca wartość — że bajty lądują
w buforze i brakuje wyłącznie warunku końca pakietu w silniku.

Dwie rzeczy, od których to zależy. Termin czasu w eterze liczony jest po
przepływności, którą S1 faktycznie pracuje: `bitrate_bps_` trzyma domyślne
100000 z trybu T, podczas gdy S1 chodzi 32768, a wartość niepodstawiona daje
termin trzykrotnie za krótki, zamieniając każdą ramkę w timeout, który nic nie
znaczy. Oraz: ścieżka timeoutu czyści w S1 wszystkie IRQ, bo DIO1 stoi wysoko,
dopóki jakikolwiek zatrzask jest ustawiony, a pin czytany jest na zboczu
narastającym — ścieżka S1 bez sondy czyści je z tego właśnie powodu, a wczesny
powrót by ją ominął.

`DRAIN_CAP` wynosi 640, co pokrywa najdłuższą ramkę, jaką potrafi wyprodukować
którykolwiek tryb: S1 jest Manchesterem, więc jego maksimum to 2 × 290 = 580
bajtów surowych, wobec 435 dla T1 i 292 dla C1.

## Podawanie zdrenowanej ramki do dekodera

Gdy drenaż się kończy, dekoder dostaje ramkę z niego, a nie z odczytu bufora po
`RX_DONE`. Powyżej 255 bajtów tamten odczyt nie może być ramką: bufor jest
pierścieniem, `GetRxBufferStatus` raportuje `oczekiwane mod 256` (70 przy ramce
326-bajtowej), a początek telegramu został nadpisany przez jego własny ogon.
Zdrenowana kopia jest jedyną kompletną.

Podstawienie następuje wyłącznie wtedy, gdy drenaż osiągnął zadeklarowaną
długość. Drenaż, który się urwał — przez limit 512 bajtów albo przez odczyty,
które zostały w tyle za wskaźnikiem zapisu — zostawia zwykły odczyt na miejscu,
bo fragment podany dekoderowi czytałby się jak uszkodzona ramka, a nie jak
nieudany drenaż.

Jest stosowane **po** kontroli na układzie i po porównaniu
`lr1121_verify_buffer`, z których oba czytają `rx_buffer_` po SPI. Podstawienie
wcześniej sprawiłoby, że `drain_match` porównywałby drenaż sam ze sobą —
a przyrząd, który z konstrukcji raportuje sukces, jest gorszy niż żaden.

Poniżej 255 bajtów nic obserwowalnego się nie zmienia: to te same bajty,
zmierzone 57/57 bajt w bajt, zanim to włączono. Podstawienie nie jest
warunkowane długością, więc ścieżka, którą idą długie ramki, jest tą, którą
krótkie ćwiczą codziennie.

`lr_fifo` nadal próbkuje bufor układu, który po podaniu drenażu nie jest już
tym, co dostaje dekoder — porównywać go z `lr_drain`, a nie z tym, co zostało
zdekodowane.

Układy magistrali sprawdzono wobec implementacji referencyjnej Semtecha:
[GetRxBufferStatus](https://github.com/Lora-net/SWDR001/blob/master/src/lr11xx_radio.c)
oraz [ReadBuffer8](https://github.com/Lora-net/SWDR001/blob/master/src/lr11xx_regmem.c).
