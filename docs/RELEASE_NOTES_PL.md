# Historia zmian

[English version](RELEASE_NOTES.md)

## Nowość: bufor MQTT z QoS i sterowaniem pojemnością

- **Nowa opcja `mqtt_buffer_size` (domyślnie `0`, wyłączona).** Gdy broker znika, zdekodowane wiadomości czekają w RAM i idą przy ponownym połączeniu, zamiast przepaść. Razem z nią sterowanie QoS i priorytetem per licznik, plus regulacja pojemności i wybór QoS w czasie pracy.
- **Używać `auto`, nie sztywnej liczby — i nie tylko ze względu na rozmiar.** Przy `auto` sufit śledzi wolny heap, więc dojście do niego pokrywa się z prawdziwym brakiem pamięci i broni logika priorytetu. Przy sztywnej pojemności większej, niż heap udźwignie, bufor nigdy nie czuje się pełny, eksmisja nigdy się nie włącza, a jedyną obroną zostaje zawór RAM-owy, który o `buffer_priority` nie wie nic. `auto` zamienia więc stratę ślepą na wybraną.
- **Zweryfikowane na sprzęcie przez kogoś innego niż autor**, na obu ścieżkach pamięci — PSRAM i heap wewnętrzny — przez ponad trzy godziny wymuszonych awarii brokera: bez watchdoga, bez restartu, bez przepełnienia RX FIFO, z dokładnym rachunkiem w każdym przebiegu i bez utraty czegokolwiek, co weszło do bufora.
- **Zachowanie znane, nie usterka:** na płytce bez PSRAM największy wolny blok heapu nie wraca w pełni po opróżnieniu bufora, choć całkowity wolny heap wraca. Zaobserwowane poziomy trzymały się mniej więcej dwukrotności progu fragmentacji przez dwa cykle, więc zawór odmawiał zawsze na podstawie całkowitego wolnego heapu, nigdy fragmentacji. Warte obserwacji na płytkach z mniejszym heapem startowym.
- Domyślnie wyłączone, więc nic się nie zmienia, dopóki tego nie włączysz.

---

## Nowość: `min_preamble_bits` — wspólny próg preambuły dla SX1262, SX1276 i LR1121

- **Nowa opcja i jedna zmiana nazwy.** `min_preamble_bits` udostępnia tę bramkę na SX1262 i SX1276, gdzie wcześniej nie była konfigurowalna. Na LR1121 zastępuje `preamble_detector`, który robił to samo pod nazwą związaną z układem - jeśli masz ten klucz w YAML-u, zmień nazwę. LR1121 jest za bramką `lr1121_allow_experimental`, więc dotyczy to niewielu konfiguracji. Na CC1101 odrzucany z uzasadnieniem: ten układ ma tę samą bramkę co PQT, ale liczoną w stopniach jakości, nie w bitach preambuły.
- **To nie jest długość preambuły nadawanej.** Tamto to osobne pole `SetPacketParams`, nieustawiane z YAML-a. Stara nazwa zachęcała do pomyłki.
- **Dla `listen_mode: t1` i `both` maksimum to 16, a walidator odrzuca teraz więcej.** Zmierzone na sprzęcie: SX1262 przy 24 bitach zebrał 184 wyzwolenia odbiornika w dwie minuty i zdekodował zero ramek. Detektor czeka na 24 bity ciągłej preambuły, preambuła T1 jest krótsza, więc detekcja nigdy się nie kończy i każda ramka przelatuje. 16 działa, więc użyteczna preambuła mieści się w 16-23 bitach, zgodnie z powszechnie podawaną wartością 19 dla T-mode. Tak skonfigurowana płytka jest po cichu głucha, wyglądając na zdrową — dlatego odrzucamy to przy walidacji, a nie zostawiamy do odkrycia na dachu.
- `8` nadal działa i tego używa upstream, ale kosztuje około **16% słyszanych liczników** - powtórzone na dwóch parach płytek z dobranymi antenami i jeszcze raz dzień do dnia na dwóch płytkach. **To wszystko jedna lokalizacja**: pięć płytek w jednym bloku. Kierunek utrzymał się przy każdej wypróbowanej tam metodzie; wielkość efektu jest właściwością tamtego środowiska RF i nie należy jej czytać jako stałej.
- C1 i S1 zostają nietknięte. Ich preambuł tutaj nie mierzono, a zgadywanie w stronę, która po cichu wyłącza odbiornik, to zły kierunek na pomyłkę.

---

## Nowość: licznik wyzwoleń odbiornika i dwie diagnozy braku ramek

- **Nowy licznik `irq_fired`** w podsumowaniu diagnostycznym i w linii logu. Liczy samo przerwanie danych, przed jakimkolwiek parsowaniem, przez co jest jedynym licznikiem rozdzielającym „nic nie usłyszałem" od „usłyszałem i zgubiłem dalej".
- **Czytać obok `dropped`, nigdy samego `drop_pct`.** `drop_pct` poprawia się, gdy odbiór się psuje: ramka, której radio nigdy nie podjęło, nie liczy się jako odrzucona. Zmierzone na pięciu płytkach w jednym oknie: płytka z najniższym `drop_pct` (8%) dowoziła połowę tego co płytka z najwyższym (21%), a płytka z najgorszą konwersją (`total / irq_fired` = 11%) słyszała najwięcej liczników ze wszystkich.
- **`NO_DATA` rozdziela się na dwa stany.** Zero wyzwoleń nadal znaczy antena, częstotliwość albo połączenia. Wyzwolenia bez ramek to inna usterka i teraz tak jest opisana: coś NADAJE, ale nic nie przechodzi filtru - sprawdź `listen_mode`, `min_preamble_bits` i tryb samego licznika. To rozróżnienie zdiagnozowało głuchą płytkę w minutę podczas testów.

---

## Poprawka: przechwytywanie długich ramek zawsze trwało poza końcem ramki

- **Strumień `long_gfsk_packets` kończy się teraz tam, gdzie kończy się ramka.** Przechwytywanie w trybie T nie miało jak poznać długości ramki, więc szło do swojego limitu 512 bajtów, zbierając szum po ramce — nie od czasu do czasu, tylko za każdym razem, bo oba wcześniejsze wyjścia z pętli są w odbiorze ciągłym nieosiągalne: `RX_DONE` nie może zaskoczyć, dopóki kod dopycha rejestr końca ładunku przed wskaźnik zapisu, a warunek ciszy nigdy nie dojrzewa, bo demodulator wciąż produkuje bajty z szumu. Zweryfikowane na sprzęcie: `captured=134 exit=t1_length` zamiast `captured=512 exit=buffer_cap`.
- Długość bierze się z pola L samej ramki, dekodowanego z dwóch pierwszych bajtów surowych. Gdy się nie dekoduje, przechwytywanie przerywa natychmiast, zamiast zbierać krótsze okno — bez pola L nikt niżej nie znajdzie granic bloków, więc każdy kolejny bajt to czas spędzony w głuchocie nad ramką, która i tak zostanie odrzucona. `diagnostic_mode: verbose` zachowuje stare okno dla tych, którzy celowo patrzą.
- **Nadal aktualne i teraz zapisane w dokumentacji: `long_gfsk_packets` kosztuje około 7 dB przy słabym sygnale.** Zmierzone wyłączona/włączona/wyłączona na jednej płytce w trzech oknach, przy trzech nietkniętych kontrolach trzymających swój percentyl RSSI co do decybela. Zakończenie przechwytywania we właściwym miejscu tej kary nie usunęło, więc opcja pozostaje wymianą, nie darmowym zyskiem. Sprawdź swoją najdłuższą zdekodowaną ramkę, zanim włączysz ją profilaktycznie — poniżej mniej więcej 150 bajtów zdekodowanych nie daje nic.

---

## Poprawka: `false_start_like` liczył każdy fałszywy start na SX126x dwukrotnie

- `payload_size_unknown` i `raw_drain_skipped_weak` odpalają się na tej ścieżce przy tym samym zdarzeniu — drain jest pomijany właśnie dlatego, że długości nie dało się wyprowadzić, a sygnał był słaby — a suma dodawała oba. Dziesięć kolejnych podsumowań z czterech płytek miało te dwie wartości równe co do jedności, a sumę dokładnie dwukrotną.
- **To nie kosmetyka:** podpowiedź diagnostyczna niżej odpala się przy 40 i więcej, więc realnie wyzwalała się przy 20.
- Naprawione przez wzięcie większej z pary, a nie przez usunięcie jednego składnika. Na ścieżce SX1276 to nie jest to samo zdarzenie — `raw_drain_skipped_weak` stoi na zerze, a licznik niesie `probe_start_aborted` — i tamten przypadek był już poprawny.

---

## Nowość: CC1101 podpowiada sprawdzenie SPI, gdy zapis wymaga powtórzenia

- Kiedy `apply_radio_profile_()` potrzebuje powtórki, żeby zapisać rejestr, albo rejestr w ogóle nie utrzymuje wartości, istniejąca linia `CC1101 profile write-back` dostaje teraz kontynuację `CC1101 hint` tłumaczącą, co ta liczba prawdopodobnie znaczy.
- Dwa poziomy: rejestr, który nie utrzymał wartości mimo powtórek, wskazuje na trwałą usterkę - sprawdzić każde połączenie, szukać zwarcia między sąsiednimi pinami, spróbować krótszego kabla. Rejestr, który potrzebował powtórki, ale się zapisał, wskazuje na coś przejściowego - luźny pin, przejściowe zwarcie, kabel za długi jak na prędkość zegara.
- **Dlaczego „prawdopodobnie", nie „potwierdzone":** `chip_not_ready_count_` już wyklucza `CHIP_RDYn` jako przyczynę powtórki - jeśli zapis dostaje odpowiedź „układ gotowy", a wartość i tak się nie odczytuje z powrotem, to magistrala przekłamała bajt po drodze. Zgłoszenie #22 doprowadziło jeden realny taki przypadek do przejściowego zwarcia między dwoma przewodami: `reg_retry_count_` spadł z dwucyfrowej liczby do zera dokładnie w momencie naprawy zwarcia, bez żadnej innej zmiany w konfiguracji. Jedno skorelowane zgłoszenie to trop, nie dowód, dlatego hint mówi „prawdopodobnie", a nie wskazuje przyczynę wprost.
- To dotyczy tylko CC1101. Mechanizm zapisu-z-weryfikacją, z którego ten hint korzysta, istnieje tylko w tym sterowniku.

---

## Poprawka: brak weryfikacji zapisów FREQ i SYNC; istotny był FREQ

- Weryfikacja przez odczyt zwrotny dodana dla profilu startowego (`write_reg_verified_`) objęła każdy rejestr zapisywany bezpośrednio z `apply_radio_profile_()`, ale ominęła dwie wywoływane funkcje, które zapisują własne rejestry: `set_frequency_()` (`FREQ2/1/0`) i `set_sync_word_()` (`SYNC1/0`, zarówno ścieżka normalna, jak i S1).
- **Zgłoszenie #22 złapało to natychmiast.** Z `spi_data_rate: 1MHz` `FREQ1` doszedł, ale `FREQ2` i `FREQ0` zostały na wartościach domyślnych - radio słuchało na 790,961 MHz zamiast 868,950 MHz, przy reszcie profilu już poprawnej. Walidacja FREQ dodana tego samego dnia wcześniej jest tym, co sprawiło, że to było widoczne, a nie ciche.
- Wszystkie pięć rejestrów przechodzi teraz przez tę samą ścieżkę zapisz-zweryfikuj-powtórz co reszta profilu.

---

## Nowość: `spi_data_rate` i potwierdzenie problemu magistrali SPI

- **Nowa opcja `spi_data_rate`** (wszystkie radia, domyślnie bez zmian — 2 MHz). Ustawia zegar SPI **tego urządzenia**, nie całej magistrali. Zakres od 100 kHz do 8 MHz.
- **Skąd się wzięła.** Na płytce ze zgłoszenia #22 `reg_write_retries=4` przy `reg_write_failed=0`: każdy rejestr konfiguracji ostatecznie przyjął swoją wartość, ale cztery potrzebowały drugiej próby. To jest magistrala przekłamująca bajty — zmierzona, nie zgadnięta — i to na zdrowym zasilaniu 3,3 V, z modułem na przewodach.
- **Awaria jest cicha i o to właśnie chodzi.** Zgubiony bit w zapisie rejestru zostawia ten rejestr na wartości domyślnej. Nic nie zgłasza błędu; radio po prostu zachowuje się tak, jakby skonfigurowano je na inną prędkość i dewiację, a każda ramka pada na CRC trzy warstwy niżej. Zanim doszła weryfikacja przez odczyt zwrotny, było to nie do odróżnienia od martwej anteny.
- **Obniż go, zanim zaczniesz podejrzewać układ.** Podniesienie powyżej 2 MHz jest dozwolone, ale nie ma tu znanego zysku — opróżnianie RX FIFO dyktuje radio, nie magistrala.
- **Czego to nie naprawia.** Zapisy rejestrów są powtarzane przy niezgodności, więc się podnoszą. Odczytu RX FIFO powtórzyć się nie da: drugi odczyt zabiera bajty, które pierwszy już pobrał. Jeśli magistrala przekłamie bajt w tym miejscu, ramka przepada i żadne powtarzanie jej nie odzyska. To jest właśnie argument za ustawieniem zegara, który okablowanie faktycznie udźwignie.

---

## Poprawka: CC1101 gubił zapisy rejestrów, gdy układ nie był gotowy

- **`CHIP_RDYn` jest wreszcie sprawdzane.** CC1101 odpowiada na każdy bajt nagłówka bajtem statusu, którego bit 7 to `CHIP_RDYn`; datasheet TI SWRS061I 10.1 wymaga, żeby był niski przed pierwszym zboczem `SCLK`. Sterownik dostawał ten bajt przy każdej transakcji i go wyrzucał. Zapisy i strobe'y teraz go oglądają i powtarzają transakcję, gdy układ zgłosił, że nie był gotowy - do 5 prób co 200 us.
- **Odczyty są liczone, nigdy nie ponawiane.** Powtórny odczyt RX FIFO zabrałby bajty, które pierwszy już pobrał. Odczyty pojedynczych rejestrów też podbijają licznik, bo odczyt sprzed opadnięcia `CHIP_RDYn` zwraca `0xFF`, a nie zawartość rejestru.
- **Sekwencja resetu jest zgodna z datasheetem.** `reset_cc1101_()` czeka na `CHIP_RDYn` przed `SRES` i jeszcze raz po nim, a odczekanie po resecie wzrosło z 5 ms do 10 ms, tak jak w obu znanych działających sterownikach CC1101. Wcześniej strobe leciał natychmiast po opadnięciu `CSn`, co jest dopuszczalne tylko wtedy, gdy zamiast tego dotrzyma się `tsp,pd` z Table 22 - a te 150 us zmierzono na referencyjnej płytce CC1101EM z konkretnym kwarcem, nie na dowolnym module.
- **Cykl sync `0x54CD` na razie zostaje.** Został usunięty i tego samego dnia przywrócony. Argument za usunięciem jest realny - tryb T i tryb C dzielą sync word `0x543D`, a następujące po nim `0x54CD` w telegramie trybu C przychodzi jako *dane*, dlatego `packet.cpp` odcina je jako `WMBUS_MODE_C_SUFIX_LEN`. Ale jedyny zrzut, na którym to rozumowanie stoi, pochodzi z płytki, której profil RF nie przykłada się poprawnie, więc nie mówi nic o tym, co widzi sprawny odbiornik, a siostrzane sterowniki cyklują z udokumentowanego powodu. Rozstrzygnie to pomiar z działającego CC1101, nie rozumowanie.
- **`FREQ2/1/0` jest weryfikowane.** Rejestry nośnej były zapisywane i nigdy nieodczytywane, więc słowo częstotliwości, które nie doszło, zostawiało radio nastrojone gdzie indziej, a autotest meldował, że wszystko gra. To jedyna część profilu zależna od YAML-a użytkownika, a po cichu zła nośna jest w każdej innej linii logu nie do odróżnienia od martwej anteny.
- **Każdy rejestr profilu startowego jest teraz zapisywany, odczytywany z powrotem i przepisywany przy niezgodności** (3 próby). Autotest i wcześniej meldował, że profil jest zły, ale dopiero na końcu i tylko jako lista wartości końcowych - nie potrafił powiedzieć, który zapis zawiódł, czy powtórzenie pomaga i jak często. A na tym rozróżnieniu stoi cała diagnoza: wartość, którą powtórzenie naprawia, znaczy, że transport przekłamuje bajty, a rejestr uparcie wracający do wartości domyślnej znaczy, że żadne powtarzanie nie pomoże i wina leży w układzie albo jego zasilaniu. Niezgodności są logowane, nigdy nie są krytyczne: kilka rejestrów może zgodnie z projektem odczytywać się inaczej, niż zostały zapisane (bity zarezerwowane albo tylko do odczytu), a odmowa startu z tego powodu byłaby gorsza niż mierzony problem.
- **Nowe pola `chip_not_ready`, `reg_write_failed` i `reg_write_retries` w linii `CC1101 debug status`**, liczące transakcje, na które układ odpowiedział z `CHIP_RDYn` wciąż wysokim. Wartość niezerowa wskazuje na zasilanie, długość okablowania albo zegar SPI - nie na dekoder.
- **Skąd to wyszło.** Użytkownik z modułem CC1101 zasilanym 2,932 V odkrył, że mniej więcej dwie trzecie rejestrów konfiguracji nigdy się nie zapisuje, a wstawienie opóźnienia po każdym zapisie problem usuwa. Na dobrze zasilonym module układ zdąża być gotowy i nikt braku sprawdzenia nie zauważa; wolniejszy start kwarcu go odsłania. Zgłoszone przez @lente-cz, którego logi są jedynym sprzętowym materiałem, jaki ten sterownik ma.

---

## Nowość: pomiar podłogi szumu i opcjonalny próg oparty na tym pomiarze

- Podsumowanie diagnostyczne zyskuje `noise_floor_dbm` i `noise_floor_n`: RSSI eteru mierzone, gdy odbiornik jest uzbrojony i bezczynny, oraz liczba próbek, które za tym stoją. **Zawsze włączone** — bez żadnej opcji.
- Próbkowane wyłącznie tam, gdzie minęło pełne 5 s bez przerwania, czyli nic nie było odbierane. Raportowane jako MINIMUM z pierścienia 16 próbek, nie średnia: próbka wzięta w trakcie cudzej transmisji jest wysoka, a średnia pozwoliłaby jej podciągnąć podłogę w górę.
- Nowe opcje `use_noise_floor_threshold` (domyślnie `false`) i `noise_floor_margin_db` (domyślnie 6). Po włączeniu próg przerywania słabych startów to `podłoga + margines` zamiast `recent_ok_rssi_avg - 12`.
- **Dlaczego to ma znaczenie.** Dotychczasowy próg liczy się ze średniej *udanych* odbiorów, więc przerywanie słabych ramek podnosi tę średnią, co podnosi próg, co przerywa jeszcze więcej — pętla sprzężenia zwrotnego bez zewnętrznego punktu odniesienia. Podłoga szumu takiej pętli nie ma: to jest to, co robi kanał, gdy nie odbieramy.
- **I jest przenośna.** Płytka z FEM czyta o jakieś 10 dB „goręcej" niż ten sam chip bez niego — zmierzone na stanowisku 25.08.2026, dwie płytki SX1262, mediany −59 vs −68 dBm i minima −79 vs −89. Klamra w bezwzględnych dBm, jak `[-96, -86]`, znaczy więc na każdej płytce co innego fizycznie, a „N dB nad podłogą" znaczy wszędzie to samo.
- **Próg jest domyślnie wyłączony celowo.** W chwili pisania nie istniał żaden pomiar realnej podłogi szumu, więc każdy margines byłby zgadywaniem. Pomiar wchodzi włączony właśnie po to, żeby margines dało się wybrać z liczb.

---

## Nowość: zachowywany przez broker zrzut konfiguracji `/diag/config`

- Jeden JSON publikowany z `retain=true` na `wmbus/<topic_name>/diag/config` po pierwszym połączeniu z brokerem, odświeżany raz na boot.
- Kształt: `{"radio":"SX1276","lines":["  listen_mode: t1 (CHANGED, default: c1)", ...]}` — dokładnie te same linie, które płytka drukuje w boot logu, razem z markerem, więc czytelnik może porównać panel i log bez tłumaczenia.
- Słownik markerów: `(default)`, `(CHANGED, default: X)`, `(set)` dla pól bez domyślnej wartości, `(required)` i `(mode default: X)` dla `frequency`, gdy nie ustawione. Panel diagnostyczny dodatku parsuje końcowy marker na kolor odznaki i drukuje resztę dosłownie.
- Retained, żeby czytelnik otwierający panel długo po starcie widział tę konfigurację, z którą płytka wystartowała. Bez chunkowania — cały snapshot mieści się w jednym publish przy aktualnym schemacie.
- Po co: boot log od dawna niósł każde efektywne ustawienie z markerem default/CHANGED, ale tylko po transporcie szeregowym lub `esphome logs`. Publikacja tego samego tekstu robi to widocznym z dodatku bez pytania o YAML.

---

## Zmiana: domyślna wartość `sx1276_busy_ether_mode` to teraz `normal`

- **Domyślna zmienia się z `adaptive` na `normal`.** `adaptive` i `aggressive` nie stroją odbiornika: przerywają słabe starty, żeby radio nadążyło, gdy się przeciąża. Jeśli się nie przeciąża, ta czułość jest wydawana na nic.
- Zmierzone w gęstej zabudowie 2026-08-23, przy czterech płytkach **w jednym punkcie**: przy `adaptive` **żadna ramka słabsza niż −84 dBm nie przeszła w ogóle**, a płytka słyszała **27** liczników; przy `normal` ramki docierały do **−97 dBm**, a liczników było **53**. Po znormalizowaniu wobec trzech płytek kontrolnych z tego samego okna: u nich liczba liczników nie drgnęła (44→45, 33→33, 27→24), u SX1276 wzrosła 27→53.
- Mechanizm zgadza się z kodem: próg w `should_abort_t1_probe_start_()` jest zaciśnięty klamrą do `[-96, -86]`, a `adaptive` dokłada +4 dB, co dopycha go do tej klamry. Efektem jest twarda podłoga czułości, a nie płynny kompromis.
- Przez cały ten dzień `fifo_overrun`, `truncated`, `payload_read_failed` i `irq_timeout` wynosiły **zero** — odbiornik się nie przeciążał, więc ochrona nie kupowała niczego.
- **Nowa sugestia diagnostyczna `CONSIDER_BUSY_ETHER_ADAPTIVE`**: komponent proponuje `adaptive` dopiero przy *zmierzonym* przeciążeniu (`fifo_overrun > 0` albo `truncated > 0`) razem z realnymi stratami (`drop_pct >= 10`). Sam wysoki `false_start_like` jawnie nie jest powodem — liczy wyzwolenia na szumie i przez cały dzień wynosił około 60/min przy zerowych przeciążeniach.
- `CHIP_SELECTION{,_PL}.md` oraz `TROUBLESHOOTING{,_PL}.md` §8 przepisane wokół tej zasady, razem z ostrzeżeniem, że `drop_pct` poprawia się sam przy `adaptive`, bo ramki, które policzyłby jako odrzucone, nie są już próbowane. Tryb oceniać po liczbach per licznik.
- Przykłady SX1276 nie ustawiają już `adaptive` aktywnie; w wersjach z komentarzami adnotacja to `# default: normal`.
- **Zastrzeżenie, powtórzone też w dokumentacji:** jedna płytka, jeden budynek, jeden wieczór. Mechanizm jest zrozumiały, skala efektu gdzie indziej — nie.

---

## Nowość: metadane `/rx` zawierają czas odbioru

- Payload `wmbus/<topic_name>/rx` zyskuje `received_at`, znacznik ISO-8601 UTC z milisekundami.
- Opisuje moment **odbioru** ramki, a nie jej publikacji. Ramka jest przechwytywana w zadaniu odbiorczym i trafia na MQTT później, więc wartość liczona jest wstecz z monotonicznego `rx_task_wakeup_us`. Czas publikacji przekłamywałby ramkę — czyli robiłby dokładnie to, czemu znacznik ma zapobiegać, i co najbardziej boli każdego, kto buforuje ramki przy niedostępnym brokerze.
- **Nieobecne, a nie puste, gdy zegar nie jest ustawiony.** Po restarcie radio odbiera normalnie tak długo, jak SNTP potrzebuje na odpowiedź; ramka z tego okna nie może nieść 1970 ani czasu pracy udającego datę. Odbiorca, który nigdy nie zobaczy klucza, nie pomyli zastępnika z pomiarem.
- Bez podbicia schematu: pole jest dodatkowe i opcjonalne, więc konsument pisany pod schemat 1 nie odczuwa zmiany.
- Poproszone na forum, razem z trwałym buforem store-and-forward. Znacznik czasu to ta połowa, która jest tania i jednoznaczna; buforowanie nie jest, bo zapis do flasha ląduje na wrażliwej czasowo ścieżce odbiorczej.

---

## Nowość: log startowy pokazuje całą konfigurację i zmienione opcje

- Każde radio wypisuje teraz przy starcie **raport konfiguracji**: każda efektywna opcja w osobnej linii, pogrupowane w `[core] [pins] [<radio>] [output] [diagnostics]`, z oznaczeniem `(default)`, `(CHANGED, default: X)`, `(set)` albo `(required)`. Wcześniej log niósł kilka ręcznie wybranych kontroli, więc cokolwiek poza tą listą było niewidoczne, a źle skonfigurowana płytka i tak wyglądała zdrowo.
- Raport powstaje **przy kompilacji, ze schematu**, a nie jest powtarzany w sterowniku — więc domyślna w logu nie może rozjechać się z domyślną, której komponent faktycznie używa. Wypisywane są wyłącznie opcje dotyczące wybranego radia.
- **`rf_sw_pin` jest teraz raportowany dla SX1262.** Jego brak to ta sama klasa cichej usterki co `has_tcxo: false`: radio się inicjalizuje, log wygląda zdrowo, a XIAO ESP32-S3 + Wio-SX1262 pracuje z czułością niższą o jakieś 30 dB, bo moduł nigdy nie otwiera toru antenowego. Stan jest podawany w obie strony, więc „nie skonfigurowany" jest stwierdzeniem, a nie brakującą linią.
- **CC1101 nie miał bloku sanity w ogóle** i teraz go ma: bramka eksperymentalna oraz `gdo0_pin`/`gdo2_pin`, żeby okablowanie dwóch przerwań było potwierdzone, a nie domniemane.
- Pokrycie przed tą zmianą było nierówne — SX1276 logował jedną kontrolę, SX1262 cztery, LR1121 sześć, CC1101 ani jednej.
- Opisane w `DIAGNOSTIC{,_PL}.md` razem z tabelą znaczników i listą per radio.

---

## Dokumentacja: przykłady SX1262 z TCXO czyszczą rejestr błędów układu

- Wszystkie osiem przykładów `SX1262` (Heltec V3, V4, V4-R8, XIAO) ustawia `clear_device_errors_on_boot: true` oraz `publish_dev_err_after_clear: true`, zamiast wymieniać je jako opcjonalny dodatek. Każda z tych płytek deklaruje `has_tcxo: true`, a na płytce z TCXO `XOSC_START_ERR` zapala się przy każdym starcie w sposób normalny: układ próbuje uruchomić własny kwarc, zanim DIO3 dostanie polecenie zasilenia TCXO, a DIO3 konfiguruje się dopiero po resecie.
- Bez skasowania flaga jest więc zawsze zapalona i nie niesie żadnej informacji. Skasowana po ustawieniu referencji - czego oczekuje datasheet - staje się diagnostyką: flaga, która zostaje skasowana, była artefaktem startu; flaga, która wraca po czystym skasowaniu, to referencja, która naprawdę nie startuje.
- `publish_dev_err_after_clear` wysyła ponownie odczytany stan na MQTT. To jedyny sposób, żeby zobaczyć go na węźle, który nic nie odbiera - a właśnie tam rejestr błędów jest ostatnią rzeczą do odczytania; dokładnie o tym była poprawka z 2026-08-01.
- Domyślne w schemacie bez zmian (`false` dla obu). To zmiana tego, co zalecają przykłady, a nie zachowania komponentu.

---

## Poprawka: przykłady nie restartują samodzielnego odbiornika MQTT co 15 minut

- Każdy przykład YAML łączący `mqtt:` z `api:` ustawia teraz `api.reboot_timeout: 0s` wraz z komentarzem wyjaśniającym powód. Domyślną wartością ESPHome jest `15min`, a ten licznik restartuje płytkę zawsze, gdy nie jest podłączony żaden *klient* Native API - czyli w normalnym stanie odbiornika publikującego wyłącznie do MQTT.
- Zmierzone, nie wywnioskowane: metadane `/rx` z nocy 2026-08-20/21 pokazały **51 różnych `boot_id` na płytkę, mediana odstępu 900 s**, przy braku jakiegokolwiek urządzenia ESPHome dodanego w Home Assistant. Po zmianie te same płytki przepracowały **14,1 h na jednym `boot_id`**, a `seq` rósł bez przerwy.
- `api:` zostaje, więc Native API i `time: platform: homeassistant` nadal działają; wyłączony jest tylko watchdog. `mqtt.reboot_timeout` to osobny mechanizm na utratę brokera i celowo nie jest ruszany.
- `TROUBLESHOOTING{,_PL}.md` dostały sekcję o tym objawie, razem ze sposobem potwierdzenia go przez `boot_id` i `seq`, zamiast zgadywania z liczby telegramów.

---

## Dokumentacja: czwarte radio i wynik testów S1

- `CHIP_SELECTION_PL.md` obejmuje wszystkie cztery obsługiwane radia (`CC1101`, `SX1276`, `SX1262`, `LR1121`) zamiast dwóch i zyskał **sekcję o S1**: `SX1262` dekoduje S1 mniej więcej do -82 dBm i zawodzi przy -85, a `SX1276` zdekodował tę samą rzeczywistą emisję w tej samej sekundzie przy -99/-100 dBm. Zasada praktyczna — `SX1276` do S1, `SX1262` do T1 — była zmierzona 2026-08-01 i 2026-08-14, ale nigdy nie trafiła do dokumentu czytanego przed zakupem płytki.
- `LR1121` jest teraz tam, gdzie czytelnik go szuka: `README_PL.md`, `START_HERE_PL`, `RADIO_OPTIONS_MINIMAL_PL.md`, `README_FULL_PL`, `TROUBLESHOOTING_PL` (nowa sekcja o trzech usterkach, które wyglądają jak martwy sprzęt) oraz listy `busy_ether_state: n/a` w dokumentach diagnostycznych. `radio_type` w `CONFIG_REFERENCE_MINIMAL_PL.md` wymienia go jako dopuszczalną wartość.
- README przykładu LR1121 podaje zmierzony najsłabszy odbiór **-114 dBm**, odczytany z eksportu `/api/esp-rx` w biegu 14,1 h dnia 2026-08-21, w miejsce wcześniejszych -100 dBm. Ten sam bieg dał 1401 ramek na poziomie -105 dBm i niżej. Zastrzeżenie „S1 niesprawdzone" znika, bo S1 zostało sprawdzone 2026-08-19.
- `BENCHMARKS_PL.md` mówi wprost, że `CC1101` i `LR1121` **nie** są tam zmierzone: tamto porównanie opiera się na tym, że oba radia stoją w tym samym miejscu, a dla tych dwóch takiego biegu nie ma.
- `RX_PIPELINE_PL.md` opisuje towarzyszący temat `/rx`, który miała dotąd wyłącznie wersja angielska.
- `diagnostic_publish_suggestion` był jedyną opcją schematu bez wpisu w referencji; teraz go ma.
- Oba pliki `CHIP_SELECTION` mówią też, czego liczby nie udźwigną: RSSI nie jest porównywalne między płytkami (zewnętrzny LNA/FEM czyta 13-15 dB wyżej), a liczby ramek porównują tylko płytki stojące w tym samym miejscu.

---

## Nowość: opcjonalne RSSI per licznik i domyślne wartości w przykładach

- Nowa opcja `publish_rssi` (**domyślnie `false`**). Po włączeniu płytka publikuje poziom ostatniej ramki każdego przekazywanego licznika jako zachowaną liczbę całkowitą na `wmbus/<topic_name>/rssi/<meter_id>`. Kto jej nie włączy, nie zobaczy żadnej zmiany — temat po prostu nie powstaje.
- Publikowane są wyłącznie ramki z rzeczywistym pomiarem. Ramka, dla której radio nie oddało poziomu, jest pomijana zamiast wysyłana jako znacznik, więc odbiorca nie musi zgadywać, czy `0`, `1` albo `-127` znaczy „brak sygnału", czy „brak odczytu".
- Wartość to ta, którą sterownik zatrzasnął dla tej ramki (SX1276 przy pierwszym bajcie, SX1262/LR1121 na sync-word, CC1101 przy odczycie). Nic nie jest liczone od nowa i poziom nie jest doczytywany po RX_DONE, bo wtedy mierzyłby pusty kanał.
- `forward_meters` obowiązuje tak samo jak dla telegramów: odfiltrowany licznik nie ma też publikowanego RSSI.
- Niezależne od `diagnostic_mode`. Pola `last_rssi` / `win_avg_rssi` w payloadach diagnostycznych zostają bez zmian i dalej służą do czytania obrazu RF płytki.
- W parze z dodatkiem wMBus MQTT Bridge każda płytka odbiorcza daje własną encję siły sygnału dla tego samego licznika — i to jest sens tej opcji.
- Wszystkie przykłady `*_commented.yaml` mają teraz przy każdej opcjonalnej pozycji adnotację `# default: <wartość>`, a `tests/ci/check_example_defaults.py` (wpięty w CI) pilnuje zgodności tych adnotacji oraz kolumny `Domyślnie` w `CONFIG_REFERENCE_MINIMAL_PL.md` ze schematem. Zmiana defaultu w `__init__.py` wywala build zamiast po cichu unieważniać dziesięć plików.
- `CONFIG_REFERENCE_MINIMAL_PL.md` dostał szesnaście opcji, które miały default w schemacie, a nie miały wpisu, oraz sekcję o tym, co RSSI per licznik mówi, a czego nie.

---

## Poprawka: odzyskiwanie granicznych ramek S1 z uszkodzonych par Manchester

- S1 zachowuje teraz pozycje niepoprawnych par Manchester. Gdy zwykła walidacja CRC formatu A zawiedzie, sprawdza obie wartości bitu niezależnie w każdym bloku CRC i przyjmuje wyłącznie jednoznaczne rozwiązanie zgodne z CRC.
- Wyszukiwanie ma limit ośmiu erasure na blok (256 podstawień). Większe, nierozwiązywalne albo niejednoznaczne bloki pozostają `dll_crc_failed`; T1 i C1 są bez zmian.
- Pomiar na dwóch rzeczywistych 85-bajtowych przechwyceniach z 2026-08-14: mapy `[3,1,0,3,0,0]` i `[2,1,0,2,2,3]` zostały odtworzone bajt w bajt do nadanej ramki odpowiednio w 16 i 12 próbach CRC.
- Regresje hosta obejmują korekcję przez wiele bloków, odrzucenie powyżej limitu i oba rzeczywiste RAW.

---

## Diagnostyka: rozkład niepoprawnych par S1 między blokami CRC

### Dodano
- Przy `diagnostic_verbose` każdy kandydat na ramkę S1 zgłoszony przez wyszukiwanie nagłówka dostaje teraz mapę erasure: `S1 erasure map 1: 6 erasures in 776/776 frame pairs, per CRC block [1,1,1,1,1,1], worst block 1 (2^1 tries)`.

### Dlaczego
- Niepoprawna para Manchester (`00`/`11`) to znana **pozycja** błędu, a nie nieznany bit. Dekoder podstawia zero i idzie dalej, wyrzucając tę informację. Rozstrzygnięcie takiej pozycji przeciw CRC oznacza sprawdzenie obu wartości, a format A sprawdza każdy blok osobno — koszt ramki to więc 2^(najgorszy blok), nigdy 2^(suma). Sześć erasure po jednym na blok to sześć problemów po dwie próby; te same sześć w jednym bloku to 64 próby. Nie wiadomo, którą postać mają realne odbiory, a logowana dotąd suma nie odróżnia jednej od drugiej. To jest pomiar przed decyzją o czymkolwiek w dekoderze.
- Liczone wyłącznie w oknie ramki. `symbols_invalid` z toru dekodowania liczy się po całym przechwyceniu, więc przy limicie 512 bajtów opisuje głównie szum za ramką: same 246 bajtów szumu daje około 492 niepoprawnych par.

### Uwagi
- Wypisywane dla każdego zgłoszonego kandydata, nie tylko dla pierwszego. Kandydaci są szeregowani po liczbie niepoprawnych par na parę sprawdzoną, więc przypadkowe trafienie na krótkiej domniemanej ramce potrafi wyprzedzić prawdziwy nagłówek; mapa erasure jest tym, co je rozdziela.
- Tylko w trybie verbose i tylko na torze surowego przechwytywania SX1262, który i tak jest przeszukiwaniem do 512 pozycji chipowych. W torze normalnego odbioru nic się nie zmienia.
- Wyliczanie bloków zostało porównane z `s1_raw_len_from_l_` dla każdej wartości pola L, a przydział erasure do bloków przetestowany na syntetycznych przechwyceniach Manchester z błędami na znanych pozycjach: granice bloków, sześć skupionych w jednym bloku, ramka zaczynająca się nie od chipu 0 oraz przechwycenie urwane w środku ramki.

### Czego nie zweryfikowano
- Nic w dekodowaniu się nie zmieniło i nic tutaj nie twierdzi, że rozstrzyganie erasure odzyskałoby ramkę. To jest właśnie otwarte pytanie, na które ta linia logu ma odpowiedzieć. Oczekiwanie, że może to być warte kilka dB, jest arytmetyką na liczbie podstawień, nie pomiarem, i upada w całości, jeśli okaże się, że ramki przychodzą albo czyste, albo z kilkudziesięcioma erasure i nie ma nic pomiędzy.
- Nie uruchomione jeszcze na żywym przechwyceniu.

---

## Poprawka: oczekiwanie na powiadomienie przez jeden tick mogło wygasnąć natychmiast

### Naprawiono
- Podczas odczytu ramki tor odbiorczy czekał na kolejny bajt przez `pdMS_TO_TICKS(1)`, zanim uznał, że radio nie ma już nic do oddania. Czas blokady w FreeRTOS liczony jest w tikach i wygasa na **następnym** przerwaniu tikowym, a nie po pełnym okresie — czekanie na jeden tik rozpoczęte tuż przed tym przerwaniem kończy się niemal natychmiast. Przy `CONFIG_FREERTOS_HZ = 1000`, które ustawia ESPHome, „1 ms" było w praktyce losową wartością z przedziału 0–1 ms, a decyzja o przerwaniu odczytu mogła wynikać z fazy tiku, a nie z pustego FIFO. Wszystkie sześć miejsc czeka teraz dwa tiki (`WMBUS_NOTIFY_WAIT_MS`), co gwarantuje jeden pełny okres.

### Uwagi
- To czekanie jest drugą, nie pierwszą linią toru odczytu. `read()` każdego sterownika najpierw odpytuje FIFO względem terminu opartego na timerze sprzętowym (1000 µs na SX1276, 1800 µs na CC1101), którego ani częstotliwość tiku, ani scheduler nie skrócą — podłoga była więc już wcześniej gwarantowana sprzętowo, a urwane czekanie kosztowało jej końcówkę, nie całość.
- Koszt: do około 1 ms więcej na ścieżkach rezygnacji, do około 3 ms przy odczycie surowym S1, który dopuszcza trzy rundy bezczynności. Nic w torze udanego odbioru nie zwalnia.
- Zmiana samego domyślnego parametru byłaby kosmetyczna — każde wywołanie przekazywało `1` jawnie.
- Ta sama kwantyzacja ujawniła się gdzie indziej jako realny regres odbioru na ESPHome 2026.7.1 i nowszych, w komponencie, którego pętla bajtowa opiera się na tym czekaniu jako **pierwszej** linii. To tamta diagnoza (`IoTLabs-pl/esphome-components`, commit `72e76be`) skłoniła do tego przeglądu i to jej należy się autorstwo.

### Czego nie zweryfikowano
- Rzecz jest wywnioskowana z semantyki czasu blokady w FreeRTOS i z częstotliwości tiku ustawianej przez ESPHome, a nie z porównania liczników `payload_read_failed` na sprzęcie przed zmianą i po niej. Nie zaobserwowano tutaj nieprawidłowości wymagającej naprawy; zmiana usuwa znany sposób na przedwczesne porzucenie odczytu, a nie zgłoszony objaw.

---

## Poprawka: moduły CC1101 z VERSION 0x04 były odrzucane przy starcie

### Naprawiono
- Autotest startowy CC1101 porównywał rejestr statusu `VERSION` z jedną wartością, `0x14`. Układ zgłaszający `0x04` nie przechodził testu, co kończyło się wywołaniem `mark_failed()` — radio w ogóle nie ruszało, a `dump_debug_status()` opisywał to jako `UNEXPECTED_CHIP_ID`, czyli werdyktem wskazującym nie na to, co trzeba. `0x04` i `0x14` to dwie rewizje tego samego układu; obie są teraz rozpoznawane.
- Zgłoszone przez użytkownika w Discussions, u którego CC1101 zwraca `VERSION` równe `0x04`.

### Zmieniono
- Bajt rewizji przestał w ogóle blokować odbiornik. `PARTNUM`/`VERSION` są odczytywane i logowane; wartość spoza znanego zbioru daje ostrzeżenie, a start trwa dalej. `config_ok` w zrzucie diagnostycznym nie obejmuje już tożsamości układu, a werdykt `UNEXPECTED_CHIP_ID` zniknął — bez niego klasyfikator pokazuje stan, który naprawdę ma znaczenie (mapowanie GDO, tryb pakietu, profil RF, stan RX), zamiast zatrzymywać się na pierwszym nierozpoznanym bajcie.
- Milcząca magistrala SPI nadal przerywa uruchomienie: `VERSION` równe `0x00` albo `0xFF` to nie rewizja, tylko szyna, na której nic nie odpowiada.

### Dlaczego ten test można było usunąć bez straty
Autotest nigdy nie dowodził tożsamości układu. Odczytywanych i porównywanych jest dziewiętnaście rejestrów z profilem wM-Bus, który komponent przed chwilą zapisał — mapowanie GDO, próg FIFO, tryb pakietu, słowo sync, modem, AGC, tor wejściowy. Cokolwiek zwraca te wartości spod tych adresów, jest CC1101; cokolwiek nie zwraca, odpada na teście rejestrów, niezależnie od tego, co deklaruje w `VERSION`.

### Czego nie zweryfikowano
Poprawka jest wyprowadzona z semantyki rejestrów i przejrzana, nie zmierzona: autor nie ma egzemplarza z `VERSION=0x04`, żeby ją sprawdzić. Na sprzęcie `0x14` zachowanie jest bez zmian poza brzmieniem logów.

---

## Zmiana: połowa budżetu przechwycenia S1 po wykryciu nieczytelnego nagłówka

### Zmieniono
- Przechwytywanie strumienia S1 zatrzymuje się teraz na 256 bajtach surowych zamiast 512 od chwili, gdy `s1_expected_raw_len_()` obejrzało nagłówek i nic z niego nie wyprowadziło. Pełny budżet 512 bajtów zostaje wszędzie tam, gdzie długość da się wyliczyć.
- Tę chwilę da się rozpoznać: `s1_expected_raw_len_()` czyta wyłącznie surowe bajty 0..3, a te nie zmieniają się w trakcie przechwytywania. Gdy raz je zobaczy i zawiedzie, będzie zawodzić do końca tego przechwycenia - ramka jest już stracona, a jedyne pozostałe pytanie brzmi, jak długo pozostawać głuchym, zbierając o niej dowody.
- 512 bajtów to 127 ms, zmierzone wobec tempa eteru 244 us na bajt surowy. Połowa nadal w zupełności wystarcza, żeby zobaczyć, jak wyglądał strumień, a oddaje odbiornik 63 ms wcześniej - co ma znaczenie w paśmie, gdzie druga transmisja może przyjść tuż po pierwszej.

### Uwagi
- Limit celowo nie jest obniżony bezwarunkowo. Prawidłowa długa ramka S-mode potrzebuje pełnego budżetu: pole L równe 150 daje około 340 bajtów surowych, które sztywny limit 256 bajtów by uciął. Skracane są wyłącznie przechwycenia już nieudane.
- Zmierzone przy okazji: przy -82/-85 dBm błędy w przechwyceniu na granicy zaczynają się w polu C albo dalej i długość nadal się wylicza; przy -90/-95 dBm siedzą już w polu L. Tego drugiego przypadku nic nie uratuje i nic nie powinno próbować - podstawiony bit w L daje złą długość, a nie taką, którą da się odzyskać.

---

## Poprawka: jeden błędny chip w polu C odrzucał całe przechwycenie S1

### Naprawiono
- `s1_expected_raw_len_()` wymagało, żeby wszystkie szesnaście par Manchester pól L i C zdekodowało się bezbłędnie. Pole C toleruje teraz do dwóch niepoprawnych par i jest porównywane z 0x44 / 0x46 tylko na bitach, które się zdekodowały; pole L nadal wymaga bezbłędności, bo jego wartość tnie przechwytywanie, a podstawiony bit daje tam złą długość, a nie taką, którą da się odzyskać.
- Pole C istnieje wyłącznie po to, żeby odwrócona polaryzacja nie wybrała wiarygodnego, ale błędnego L. Do długości nie wnosi nic, więc wymaganie od niego bezbłędności nic nie dawało, a kosztowało bardzo dużo.

### Zmierzono
Na progu czułości 2026-08-01, dwa przechwycenia tej samej 85-bajtowej transmisji w odstępie 30 sekund:

| RssiSync | pierwsze bajty | wynik |
|---|---|---|
| -88 dBm | `66 65 65 65 …` | `exit=s1_length`, 194 B, zdekodowana, jedna błędna para na 776 |
| -89,5 dBm | `66 65 65 6D …` | `exit=buffer_cap`, 512 B, przepadła |

- Jeden błąd chipowy w surowym bajcie 3 zamienił `0x65` w `0x6D`. Ten bajt to druga połowa pola C. Długości nie dało się wyprowadzić, więc przechwytywanie doszło do limitu i zebrało około 85 ms szumu po ramce na 47 ms samej ramki.
- Skutek pomiarowy był gorszy niż utrata ramki. Udane przechwycenie raportuje niepoprawne pary z 776, nieudane z 2048, z czego trzy czwarte to szum - więc ten sam sygnał pogarszający się o ułamek decybela wygląda na urwanie z klifu. Ten artefakt sprawiał, że porażki S1 wyglądały jakościowo inaczej niż odbiór na granicy, choć są tym samym.

### Uwagi
- To nie sprawi, że nadajnik pod podłogą szumu zacznie się dekodować. Odzyskuje ramki na samej granicy, gdzie pechowy chip trafił w nagłówek zamiast w ładunek, i sprawia, że `symbols_invalid` oraz miary jakości przechwycenia mierzą jakość sygnału, a nie to, czy nagłówek akurat przeżył.

---

## Wynik: zakończono test pasma S1 — optimum to 234,3 kHz

### Zmieniono
- Szerokość pasma odbioru dla S1 wraca na 234,3 kHz i tam zostaje. Dwa commity testowe, które przestawiły ją na 312 kHz, a potem na 156,2 kHz, spełniły swoje zadanie.

### Zmierzono
Najdłuższa seria poprawnych par Manchester w przechwyceniu SX1262, każda na transmisji zdekodowanej przez SX1276 w tej samej sekundzie, wobec 680 par potrzebnych na telegram 85-bajtowy. Dane losowe dają około 11.

| pasmo RX | najdłuższa poprawna seria |
|---|---|
| 312,0 kHz | 30 par |
| **234,3 kHz** | **191 par** |
| 156,2 kHz | 47 par |

- To jest rzeczywiste maksimum, a nie trend: oba kierunki kosztują czynnik cztery do sześciu. Poszerzanie wpuszcza szum, zwężanie zaczyna obcinać sygnał, którego zajętość widma jest szersza, niż przewiduje reguła Carsona (133 kHz) czy reguła doboru Semtecha (143 kHz) dla strumienia chipów Manchester z BT=0,5.
- Ustalenie z 2026-07-30, że 156,2 kHz „całkowicie zatrzymuje odbiór", zostaje doprecyzowane: przechwytuje, tylko gorzej. Tamten pomiar wykonano przed poprawką przechwytywania AN1200.53, gdy żadne pasmo niczego nie dekodowało.

### Uwagi
- W optimum przechwycenie nadal zawiera 191 z 680 par potrzebnych na ramkę. Pasmo jest tu warte czynnik cztery i nic ponadto; nie to zatrzymuje dekodowanie S-mode na SX1262. Przemiatanie jest zamknięte i nie wymaga powtarzania.

---

## Test: trzeci punkt testu pasma S1 — 156,2 kHz

### Zmieniono
- Szerokość pasma odbioru dla S1 przechodzi z 312 kHz na 156,2 kHz. Trzeci punkt pomiarowy przemiatania, nadal test. C1 zostaje na 234,3 kHz i nie jest tym objęte.

### Zmierzono
Najdłuższa seria poprawnych par Manchester w przechwyceniu SX1262, na tej samej transmisji, którą SX1276 zdekodował w tej samej sekundzie, wobec 680 par potrzebnych na telegram 85-bajtowy (dane losowe dają około 11):

| pasmo RX | najdłuższa poprawna seria |
|---|---|
| 312,0 kHz | 30 par |
| 234,3 kHz | 191 par |
| 156,2 kHz | ten build |

- Poszerzenie do 312 kHz pogorszyło odzysk sześciokrotnie, co obala argument, że filtr węższy od sygnału rozmywał zbocza chipów. Zostaje zwykłe pasmo szumowe, a trend między dwoma zmierzonymi punktami biegnie w stronę węższego.
- 156,2 kHz próbowano raz wcześniej, 2026-07-30, i zaraportowano jako całkowite zatrzymanie odbioru S1. Zmierzono to jednak przed poprawką przechwytywania AN1200.53, gdy żadne pasmo niczego nie dekodowało - to samo zastrzeżenie, które uzasadniło ponowny test 312 kHz.

---

## Test: S1 na SX1262 wraca do pasma odbioru 312 kHz

### Zmieniono
- Szerokość pasma odbioru dla S1 wraca z 234,3 kHz na 312 kHz. To jest test, oznaczony jako test, do cofnięcia jeśli nic nie zmieni. C1 zostaje na 234,3 kHz - nie ma porównywalnej historii i dekoduje normalnie.
- Dlaczego powtarzać ustawienie, które już raz zastąpiono: poprawka przechwytywania AN1200.53 weszła 38 minut po zmianie na 234,3 kHz. Każdy test szerokiego pasma odbył się więc na zepsutej ścieżce przechwytywania, a każdy test naprawionej ścieżki odbył się na wąskim paśmie. 312 kHz nigdy nie było próbowane z torem odbiorczym w obecnym stanie.
- Dlaczego pierwotny argument za zwężeniem wygląda na błędny: dotyczył szumu i faktycznie 234,3 kHz wpuszcza o ~1,25 dB mniej niż 312 kHz. Ale to argument o czułości i nie mówi nic o zniekształceniach. Pomiary układają się odwrotnie - 156,2 kHz zatrzymało odbiór S1 całkowicie, a 234,3 kHz łapie sync na realnych ramkach S-mode i nie zdekodowało ani jednej. Reguła Carsona daje dla tego sygnału 133 kHz, reguła Semtecha 143 kHz; ustawienie powyżej obu zabiło odbiór, więc obie zaniżają zajętość widma strumienia chipów Manchester z BT=0,5. Filtr węższy od sygnału nie tłumi tylko szumu, ale też rozmywa zbocza chipów.

### Uwagi
- Wcześniejsza notatka w tym pliku twierdziła, że S1 pozostaje na szerokim paśmie 312 kHz. Przestało to być prawdą 2026-07-30 i notatka wisiała nieaktualna przez dwa dni; zostaje tu sprostowana.

---

## Diagnostyka: znajdowanie początku ramki S1 w przechwyceniu

### Dodano
- Przy `diagnostic_verbose` każde przechwycenie strumienia S1 na SX1262 jest przeszukiwane pod kątem przesunięcia chipowego, na którym faktycznie siedzi poprawny nagłówek L+C - wszystkie przesunięcia do 512 chipów, obie polaryzacje Manchester - a wynik trafia do logu razem z długością ramki i liczbą niepoprawnych par wewnątrz samej ramki.
- `s1_expected_raw_len_()` zakłada, że ramka zaczyna się na chipie 0, bo radio zdejmuje słowo synchronizacji sprzętowo i ładunek powinien następować od razu po nim. Każde przechwycenie S1 na tym sterowniku kończy się na `exit=buffer_cap`, czyli dokładnie tym, co dzieje się, gdy z tych pierwszych bajtów nie da się wyprowadzić długości.
- Co to sprowokowało: trzy przechwycenia tej samej transmisji repeatera, zdekodowane identycznie przez SX1276 w tej samej sekundzie dnia 2026-08-01, dały tutaj trzy zupełnie różne pierwsze bajty - `99363510…`, `998A9A9A…`, `DDFBDA9A…`. Identyczna treść w eterze, różna treść w buforze.

### Uwagi
- Wynik rozdziela trzy przypadki, dotąd nieodróżnialne. Stabilne `chip=0` oczyszcza ścieżkę przechwytywania i wskazuje na demodulację. Przesunięcie zmieniające się między przechwyceniami lokalizuje błąd wyrównania startu. Brak poprawnego nagłówka gdziekolwiek oznacza, że ramki w przechwyceniu w ogóle nie ma.
- Liczba niepoprawnych par jest celowo liczona wyłącznie po ciele ramki. Liczona po całym 416-bajtowym buforze jest zdominowana przez szum po ramce, który przechwytywanie zbiera dalej - i to właśnie czyniło wcześniejsze odczyty tej liczby mylącymi.
- Sposób przechwytywania nie zmienia się w niczym. To tylko patrzy na to, co zostało przechwycone.

---

## Poprawka: odczyt błędu częstotliwości opisywał szum zamiast ramki

### Naprawiono
- `RegAfc` i `RegFei` są teraz próbkowane raz na ramkę przez `latch_frame_metrics_()`, w chwili gdy jej pierwsze bajty trafiają do FIFO, razem z RSSI. `dump_debug_status()` raportuje zatrzaśnięte wartości oraz to, ile mają sekund, i nie odczytuje już rejestrów ponownie.
- Poprzednia wersja czytała je na żywo wewnątrz dumpu. Ten dump biegnie na przeterminowaniu oczekiwania na ramkę - czyli z definicji wtedy, gdy od minuty nic nie przyszło - więc zwracał to, co ostatnio potrąciło detektor preambuły, czyli szum. Zmierzone 2026-08-01 na węźle dekodującym jeden nadajnik co 123 sekundy: +23,5 kHz dwie sekundy przed ramką, potem −17,6 kHz korekty z residuum +68,5 kHz na preambule, która nigdy nie dopasowała sync, potem −16,1 kHz trzy sekundy przed kolejną ramką. Żadna z tych liczb nie pochodziła od dekodowanego nadajnika.
- RSSI jest kopiowane do osobnego, trwałego pola na potrzeby diagnostyki. `restart_rx()` przy każdym uzbrojeniu zeruje wartość przekazywaną do pakietu do sentinela „nie zmierzono", co jest tam poprawne, ale sprawiłoby, że dump raportowałby −127 dla ramki, której poziom zmierzono.

### Uwagi
- Kształt tego błędu jest ten sam co poprzednio: rejestr ważny tylko w jednej chwili, odczytany w innej, dający liczbę, która wygląda autorytatywnie i nie opisuje niczego. Lipcowa poprawka próbkowania RSSI miała dokładnie tę przyczynę.

---

## Diagnostyka: SX1276 raportuje błąd częstotliwości ostatniego odbioru

### Dodano
- `dump_debug_status()` wypisuje teraz `RegAfc` i `RegFei`, zarówno surową wartość rejestru, jak i przeliczone przesunięcie w hercach. `RegFei` to to, co odbiornik zmierzył, `RegAfc` to to, o ile AFC faktycznie skorygowało; obie wartości są zatrzaśnięte z ostatnio odebranej ramki, więc w trybie łącza, gdzie ramki dzielą minuty, odczyt nadal opisuje ostatni realny nadajnik, a nie szum.
- Dlaczego warto: SX126x nie ma w GFSK żadnego AFC, więc każde przesunięcie raportowane przez ten rejestr jest błędem, który SX1262 musi przyjąć w całości. Zmierzone na T1 na LilyGO T3-S3 dnia 2026-08-01: AFC korygowało −37,8 kHz na żywych licznikach.

### Uwagi
- To zamienia przemiatanie częstotliwości w jeden odczyt. Zamiast przestrajać odbiornik krokami i patrzeć, czy odbiór się poprawia, można odczytać wprost przesunięcie nadajnika, który faktycznie się zdekodował, i zastosować je raz.

---

## Poprawka: SX1276 pracował z wyłączonym dodatkowym wzmocnieniem LNA dla wysokich częstotliwości

### Naprawiono
- `RegLna` (0x0C) jest teraz zapisywany wartością `0x23`: maksymalne wzmocnienie plus `LnaBoostHf`. Sterownik w ogóle nie dotykał tego rejestru, więc pracował na wartości resetowej `0x20` - to samo wzmocnienie, boost wyłączony. Potwierdzone na sprzęcie 2026-08-01, zrzut banku rejestrów LilyGO T3-S3 pokazał `0x0C = 0x20`.
- `0x23` to wartość samego Semtecha. Tyle wpisuje LoRaMac-node w `RADIO_INIT_REGISTERS_VALUE` dla każdej płytki FSK, a brak wyszedł z porównania sekwencji `setup()` z tamtą tablicą, nie z obserwacji objawu.
- Połowa dotycząca wzmocnienia znaczy mniej, niż wygląda: `AgcAutoOn` jest ustawiane linijkę dalej, więc `LnaGain` prowadzi sam układ AGC. Zostaje `LnaBoostHf` - podnosi prąd LNA o 50% dla lepszej liczby szumowej. Działa powyżej 525 MHz, poniżej jest ignorowany, więc zapis jest bezwarunkowy.

### Uwagi
- To zmiana czułości we wszystkich trybach nasłuchu, nie tylko w S1. Nie została tutaj zmierzona; argumentem za nią jest to, że sterownik referencyjny producenta to ustawia, a ten nie ustawiał. Liczba ramek przed i po na niezmienionym węźle jest sposobem, żeby się dowiedzieć, czy to cokolwiek daje.

---

## Uwaga: RegOpMode w FSK na SX127x nie wskazuje, czy odbiór działa

Po zapisaniu RX (`0b101`) do `RegOpMode` SX1276 odczytuje ten rejestr jako `0b100` - FSRX, czyli synteza częstotliwości z wyłączonym odbiornikiem - i raportuje wyzerowane `ModeReady` oraz `RxReady` w `RegIrqFlags1`, na odbiorniku pracującym normalnie. Zmierzone na LilyGO T3-S3 dnia 2026-08-01: węzeł dokładnie w tym stanie zdekodował trzy ramki T1 przy -75, -91 i -95 dBm w tej samej sekundzie, a pomiędzy nimi wypisał ten odczyt rejestru.

To jest znane zachowanie, nie usterka płytki. `SX127x::setMode()` w RadioLib maskuje najmłodszy bit trybu przy weryfikacji zapisu, konkretnie dla FSK RX, z komentarzem „disable checking of RX bit in FSK RX mode, as it sometimes seem to fail (#276)".

### Uwagi
- Nic w sterowniku nie testuje już odczytu trybu. `dump_debug_status()` nadal wypisuje `RegOpMode`, bo tę wartość warto widzieć, ale nie wyprowadza z niej twierdzenia `receiver_running` ani nie ostrzega, że nic nie zostanie odebrane. Wcześniejsza wersja tego ostrzeżenia wypisywała się godzinami na odbiorniku, który w trakcie dekodował ramki.
- Na podstawie tego odczytu dodano wcześniej czekanie na `ModeReady` na ścieżce uzbrajania S1 i zostało ono usunięte. Odpytywało o bit, który nie wraca nawet przy udanym przejściu, i kosztowało 2 ms zajętego oczekiwania dwukrotnie na każde uzbrojenie.
- Ogólny wniosek jest ten sam, który ten projekt stosuje już do RSSI: rejestr raportujący rzecz niemożliwą jest gorszy od rejestru, którego nikt nie czyta, bo o niemożliwej wartości i tak się potem argumentuje.

---

## Poprawka: `clear_device_errors_on_boot` nie działało na węźle bez odbioru

### Naprawiono
- Czyszczenie błędów układu SX1262 przeniesione z `capture_rx_stream_()` do `setup()`. Było uzależnione od pierwszej przechwyconej ramki, więc na węźle odbierającym normalnie wykonywało się w kilka sekund i nikt nigdy tej flagi nie zobaczył, a na węźle nieodbierającym nic nie wykonywało się wcale - przez co `clear_device_errors_on_boot: true` nie robiło nic dokładnie w tym jedynym przypadku, w którym rejestr błędów jest ostatnią rzeczą, jaka została do odczytania. Zaobserwowane na sprzęcie 2026-08-01: dwa węzły SX1262 raportujące `XOSC_START_ERR` przez wiele minut, stojąc w RX z poprawną konfiguracją.
- `XOSC_START_ERR` jest ustawiane przy starcie na płytce z TCXO w sposób normalny, bo układ próbuje uruchomić swój oscylator kwarcowy, zanim DIO3 dostanie polecenie zasilania TCXO - DIO3 konfiguruje się po resecie. Wyczyszczenie flagi po ustawieniu referencji jest tym, czego oczekuje datasheet. W parze z ponownym odczytem na końcu `setup()` flaga wreszcie coś znaczy: taka, która znika, była artefaktem startu, taka, która wraca po czystym wyczyszczeniu, to referencja, która naprawdę nie startuje.
- Usunięcie tego bloku ze ścieżki odbiorczej zdejmuje przy okazji 7 ms blokującego opóźnienia z pierwszego przechwycenia.

### Uwagi
- Ten wpis pierwotnie zapowiadał także ostrzeżenie SX1276 wyprowadzone z odczytu `RegOpMode` po uzbrojeniu RX. Ten odczyt okazał się nie znaczyć tego, na co wyglądał, i ostrzeżenie zostało usunięte - patrz notatka o `RegOpMode` w FSK RX powyżej.

---

## Diagnostyka: powtarzalny odczyt stanu radia zamiast jednego zrzutu przy starcie

### Dodano
- `dump_debug_status()` jest teraz zaimplementowane dla SX1276 i SX1262. Metoda była zadeklarowana w klasie bazowej i zaimplementowana wyłącznie przez CC1101, więc oba sterowniki SX dziedziczyły pustą wersję: ich rejestry dało się odczytać dokładnie raz, przy starcie, i nigdy więcej. Komponent i tak woła ją przy każdym przeterminowaniu oczekiwania na ramkę, gdy diagnostyka jest gadatliwa, więc milczący węzeł raportuje teraz swój stan mniej więcej raz na minutę.
- SX1276 raportuje `RegOpMode` rozwinięty do nazwy trybu, `RegIrqFlags1/2`, RSSI, `RegRxConfig`, `RegRxBw`, `RegPreambleDetect`, konfigurację synchronizacji i żywe słowo sync oraz poziom DIO1. `preamble_detected` i `sync_matched` są dodatkowo wypisane słowami, bo dwa bity mówiące, czy cokolwiek dociera, to po jednym bicie w środku bajtu w zapisie szesnastkowym. Układ stojący w `FSRX` zamiast `RX` - syntezer zestrojony, odbiornik wyłączony - mówi to teraz ostrzeżeniem, zamiast wyglądać na skonfigurowany.
- SX1262 raportuje tryb układu z `GetStatus`, zatrzaśnięty status przerwań, błędy układu, `RegRxGain`, żywe słowo sync, wskaźniki strumienia oraz poziomy DIO1/BUSY, z tym samym powtórzeniem słowami i tym samym ostrzeżeniem, gdy układ nie jest w RX.
- Flaga gadatliwej diagnostyki jest wpychana do sterownika przed utworzeniem zadania odbiorczego, więc żadna ramka nie jest obsłużona w chwili, gdy komponent i sterownik nie zgadzają się co do ilości raportowania.

### Zmieniono
- Snapshot pochodzenia RSSI w SX1262 (`IRQ=... captured=... first[8]=...`) powtarza się przy każdym przechwyceniu, gdy diagnostyka jest gadatliwa, zamiast raz na ścieżkę odbiorczą na boot. Jedna próbka to właściwa ilość, gdy ramki się dekodują; jest bezużyteczna, gdy żadna się nie dekoduje, bo pojedyncze `first[8]` nie odróżni realnej ramki przechwyconej bez wyrównania od detektora strzelającego w szum. Do tego potrzeba serii. Poza trybem gadatliwym zachowanie jednorazowe pozostaje bez zmian.
- Snapshot jest porzucany, a nie nadpisywany, jeśli poprzedni nie został jeszcze odebrany. Zapis do slotu w chwili, gdy zadanie główne z niego kopiuje, dałby linię logu złożoną z dwóch różnych przechwyceń, co jest gorsze niż brak linii właśnie dlatego, że nadal wygląda jak dane.

### Uwagi
- Obie zmiany biorą się z dnia spędzonego na porównywaniu trzech odbiorników bez możliwości powtórnego odczytu rejestrów. Diagnostyka tylko przy starcie odpowiada na pytanie „czy układ odezwał się po SPI"; każde pytanie, które ma znaczenie, gdy odbiór ustaje, dotyczy stanu bieżącego.

---

## Poprawka: SX1262 nie był ponownie kalibrowany po włączeniu TCXO

### Naprawiono
- `SetDIO3AsTcxoCtrl` przekazuje referencję do TCXO, ale to właśnie DIO3 ten TCXO zasila - więc każda kalibracja, którą układ wykonał przy starcie, poszła względem wewnętrznego oscylatora RC, czyli referencji, która przestaje istnieć jedną komendę później. Datasheet SX1261/2 wymaga powtórzenia kalibracji po tym kroku. `Calibrate(0x7F)` jest teraz wysyłane bezpośrednio po włączeniu TCXO i przelicza RC64k, RC13M, PLL, trzy bloki ADC oraz obraz. Wykonuje się wyłącznie przy `has_tcxo: true`, więc płytki bez TCXO pozostają nietknięte.
- Brak tego kroku nie objawia się głośno. Radio startuje, uzbraja odbiór i bez problemu odbiera nadajnik z tego samego biurka; traci ostatnie kilka dB, czyli dokładnie zakres, w którym docierają prawdziwe liczniki. Nic w logu nie odróżniało tego od złej anteny.
- `GetDeviceErrors` jest teraz odczytywane raz na końcu `setup()` i logowane, przy czym `XOSC_START_ERR` i `PLL_CALIB_ERR` podniesione do `ESP_LOGE`. Snapshot błędów układu już istniał, ale mieszka w `capture_rx_stream_()` i odpala się na pierwszej przechwyconej ramce - czyli nigdy w jedynym przypadku wartym diagnozy, gdy nic nie jest odbierane.

### Uwagi
- Znalezione przy porównywaniu SX1276 z SX1262 na trybie S. To nie jest wyjaśnienie tamtego porównania i nie należy mu tego przypisywać; to wymóg datasheetu, którego brakowało niezależnie.

---

## Poprawka: ramka mogła być logowana jako „RSSI: 0dBm”

### Naprawiono
- `sx126x_rssi_dbm_()` traktował surowe 0 jako „rejestr nigdy nie zapisany" i przeliczał całą resztę. Surowe 1 to -0,5 dBm, a dzielenie całkowite zamienia to w czyste `0 dBm` - poziom, przy którym żadna ramka wM-Bus nie dociera, bo tor wejściowy nasyca się w okolicach -5 dBm, a nadajnik na tym samym biurku przy minimalnej mocy i tak leży dziesiątki dB niżej. Zaobserwowane na sprzęcie: poprawnie zdekodowana ramka zaraportowana jako `RSSI: 0dBm`. Przeliczenie odrzuca teraz całą niemożliwą górę skali (surowe poniżej 20, czyli powyżej -10 dBm) i zwraca sentinel „nie zmierzono".
- Reguła wiarygodności mieszka w przeliczeniu, a nie w każdym miejscu wywołania. Trzy miejsca odczytujące poziom - próbka w locie, status pakietu na ścieżce FIFO i status pakietu na ścieżce strumieniowej - proszą o wartość przeliczoną i biorą pierwszą, która nie jest sentinelem. Wcześniej sprawdzały surowy bajt na `!= 0`, przez co niewiarygodny odczyt z synchronizacji przesłaniał użyteczną średnią.
- `Packet::rssi_` miał domyślnie 0. To nie jest sentinel, tylko odczyt, i to ten sam niemożliwy: pakiet, któremu nigdy nie ustawiono poziomu, raportował idealny sygnał zamiast przyznać, że go nie ma. Domyślną wartością jest teraz -127, którą statystyki już umieją pomijać.

### Uwagi
- Zmyślona liczba jest gorsza niż brak liczby. Znalezione przy porównywaniu dwóch odbiorników, gdzie `RSSI: 0dBm` wylądowało w środku sesji pomiarowej i trzeba było je najpierw obalić, zanim dało się je odrzucić.

---

## Uwaga: S1 na SX1262 zachowuje szerokie pasmo odbioru 312 kHz

Węższe okno zostało wypróbowane i wycofane. Reguła Carsona daje dla S-mode 132,8 kHz - 2 * (dewiacja + chiprate/2) dla strumienia Manchester 32,768 kchip/s - więc S1 zszedł z odziedziczonych 312 kHz na 156,2 kHz w oczekiwaniu na około 3 dB niższy próg szumu. Na sprzęcie wyszło odwrotnie: Heltec V4 przestał odbierać ramki S1 w ogóle, podczas gdy stojący obok SX1276 dalej dekodował ten sam nadajnik.

Carson nie opisuje dobrze sygnału kodowanego Manchesterem. Strumień chipów niesie realną energię poza nominalną dewiacją, a wartość z noty katalogowej to poziom -3 dB, a nie płaskie pasmo, więc użyteczne okno jest węższe niż sugeruje liczba. Kod zostaje przy 312 kHz dla S1, z komentarzem, żeby nie zwężać go ponownie bez pomiaru widma odebranego sygnału.

---

## Poprawka: bufor podsumowania w .bss zamiast na stosie pętli

### Naprawiono
- Powiększenie bufora payloadu podsumowania do 3072 B umieściło 3 kB na stosie zadania pętli, w ścieżce, która zagnieżdża dalej `maybe_publish_suggestion_()` (kolejne 640 B) i logger, piszący przez newlib i VFS. Na węźle LilyGO z SX1276 kończyło się to crashem `Fault - LoadProhibited` w `esp_vfs_write`, osiągniętym z `ESP_LOGI`, które formatuje wyłącznie literały - to sygnatura przepełnienia stosu, nie złego wskaźnika. Występowało tuż po pierwszym podsumowaniu 60 s, na węźle, u którego ścieżka sugestii faktycznie się wykonuje.
- Trzy podsumowania korzystają teraz ze wspólnego bufora statycznego. Są wołane wyłącznie z `Radio::loop()`, jedno po drugim i nigdy równolegle, więc nie mogą sobie nadpisać danych. Kosztuje to 3 kB `.bss` i zdejmuje 3 kB ze stosu pętli - mniej stosu, niż kod zajmował przed powiększeniem bufora.

---

## Nowość: S1 otrzymuje tę samą diagnostykę co T1 i C1

### Dodano
- Podsumowania publikują blok `s1` obok `t1` i `c1` - `total`, `ok`, `dropped`, `per_pct`, `crc_failed`, `crc_pct`, `avg_ok_rssi`, `avg_drop_rssi` - oraz `manchester_drop` i `manchester_pct`. Liczniki per tryb są indeksowane trybem łącza i ramki S1 zawsze do nich trafiały; nikt ich nie odczytywał, więc ruch S1 był niewidoczny w podsumowaniach poza sumami globalnymi.
- Hinty `S1_WEAK_SIGNAL`, `S1_INTERFERENCE_OR_RX` i `S1_OVERLOAD_OR_MULTIPATH`, odwzorowujące gałęzie C1, oraz `S1_MANCHESTER_ERRORS`, gdy co najmniej 20% ramek S1 pada na etapie Manchester. S-mode jest kodowany Manchesterem, więc ten etap jest odpowiednikiem błędnych symboli 3-of-6 w T1.
- `dropped_by_stage` zyskuje `s1_precheck`, `s1_manchester`, `s1_l_field` i `s1_length_check`. Parser od dawna emitował te nazwy etapów, ale `bucket_for_stage_()` ich nie rozpoznawał, więc każda porażka S1 lądowała w `other`.
- `stage_rank_()` w parserze uwzględnia teraz etapy `s1_*` obok odpowiedników T1/C1. Nie zmienia to obecnego działania: `try_parse_s1_()` jest wywoływane tylko na wymuszonej ścieżce S1, która kończy się przed porównywaniem etapów, a ramka S-mode nie trafi do radia dostrojonego do T1/C1. Kolejność będzie jednak poprawna, jeśli wynik S1 zostanie kiedyś porównany z inną próbą parsowania.

### Naprawiono
- Bufor payloadu podsumowania miał 2048 B, podczas gdy JSON potrzebuje około 2,2 kB przy dłuższym tekście hinta, a rośnie wraz z licznikami. `snprintf` ucinał go po cichu i publikował niepoprawny JSON. Bufor ma teraz 3072 B, a ucięcie loguje ostrzeżenie zamiast wysyłać zepsuty payload. Mogło się to zdarzać już wcześniej, przy którymkolwiek z dłuższych hintów C1/T1.

---

## Poprawka: okno samych błędów CRC było oceniane jako poprawne

### Naprawiono
- `DIAG hint` startuje jako `OK / "looks good"`, a każda gałąź tylko go nadpisuje. Żadna nie obejmowała okna, w którym ramki dotarły, ale żadna się nie zdekodowała: gałęzie dla konkretnych trybów opierają się na licznikach `c1_*` i `t1_*`, a te w `listen_mode: s1` pozostają zerowe; gałęzie ogólne o słabym sygnale wymagają `avg_drop_rssi <= -90`. Realne okno S1 - `total=1 ok=0 dropped=1 crc_failed=1` przy -87 dBm - raportowało więc `OK | looks good`, i to na poziomie INFO, podczas gdy puste okno logowane jest jako ostrzeżenie. Okno w całości nieudane wypadało lepiej niż ciche.
- Nowy hint `ALL_DROPPED` obejmuje `ok == 0`. Stoi za gałęziami szczegółowymi, więc `C1_WEAK_SIGNAL`, `T1_BITFLIPS` i pozostałe zachowują pierwszeństwo, a zastąpiona zostaje wyłącznie błędna wartość domyślna. Jak każdy hint inny niż `OK` jest logowany jako ostrzeżenie. Dotyczy wszystkich trzech okien podsumowania (60 s, 15 min, 60 min).
- Sugestia `ADD_HIGHLIGHT_METERS` wyzwalała się na samym *dotarciu* ramki (`total > 0`), więc to samo okno radziło "Liczniki są odbierane. Sprawdź w wmbusmeters jakie ID pojawiają się", choć nic się nie zdekodowało i żadne ID nigdy nie zostało opublikowane - wysyłając użytkownika na poszukiwanie czegoś, czego nie ma. Teraz wymaga co najmniej jednej zdekodowanej ramki w oknie.

### Uwagi
- Drzewo hintów nadal nie ma własnych liczników dla S1; istnieją tylko `c1_*` i `t1_*`, więc okna S1 dostają diagnozy ogólne. To osobna zmiana.

---

## Poprawka: XIAO z Wio-SX1262 odbierał przez wyłączony przełącznik antenowy

### Naprawiono
- Moduł Seeed Wio-SX1262 nie podłącza anteny bezwarunkowo. Wyprowadzenie nr 1 modułu (`RF_SW`, "External IO control internal gate RF switch") musi być trzymane w stanie wysokim przez host; w zestawie z XIAO ESP32S3 jest to GPIO38. Nic go nie sterowało, więc pin pozostawał wejściem w stanie wysokiej impedancji, a odbiornik pracował wyłącznie na przecieku sygnału.
- To jest coś innego niż `dio2_rf_switch` i potrzebne są oba. Zgodnie z notą katalogową modułu DIO2 układu SX1262 wybiera *kierunek* TX/RX (wysoki = TX, niski = RX), natomiast `RF_SW` decyduje, czy przełącznik w ogóle przewodzi.
- Przykłady dla XIAO zawierały obejście - akcję `on_boot` przełączającą wyjście `gpio` na GPIO38 - i nigdy ono nie działało. Priorytet 900 trafia w tym samym etapie inicjalizacji ESPHome co sam komponent `gpio output`, więc o kolejności decyduje kolejność rejestracji i zapis wykonywał się, zanim wyprowadzenie stało się wyjściem. Bez ostrzeżenia, bez błędu, bez śladu w logu. Obejście zostało usunięte z obu przykładów.
- Pomiar na sprzęcie, ta sama płytka i antena przed i po: licznik `00088888` z -96 dBm na -68 dBm, a odbiornik z 4-6 ramek na minutę od 3 liczników na 14 od 32.

### Dodano
- `rf_sw_pin` dla `SX1262`. Wyprowadzenie ustawiane jest w stan wysoki wewnątrz inicjalizacji radia, przed resetem układu, gdzie kolejność jest jednoznaczna. Opcja jest potrzebna płytkom, których moduł bramkuje w ten sposób tor antenowy; Heltec V3/V4/V4-R8 jej nie wymagają - korzystają z wyprowadzeń `fem_*` i zmiana ich nie dotyczy.
- Przykład dla XIAO dołączył do macierzy kompilacji CI. Ta płytka nie była dotąd objęta kompilacją weryfikacyjną, a jest jedyną konfiguracją używającą nowej opcji.

### Uwagi
- **Wymagane działanie na XIAO ESP32S3 + Wio-SX1262.** Dodaj `rf_sw_pin: GPIO38` i usuń wcześniejszy blok `output:` / `on_boot:` sterujący GPIO38 - pozostawienie obu powoduje odrzucenie konfiguracji przez ESPHome z powodu podwójnej deklaracji wyprowadzenia. Bez tej opcji odbiornik nadal pracuje z czułością niższą o około 30 dB.
- Objawem nie jest cisza. Ramki nadal się dekodują, `DIAG hint` nadal raportuje `GOOD`; jest ich tylko kilka razy mniej, a każde RSSI leży w wąskim paśmie tuż nad progiem czułości.

---

## Poprawka: SX1262 raportował podłogę szumu jako RSSI każdej ramki

### Naprawiono
- Każda odebrana ramka raportowała tę samą wartość RSSI tuż nad progiem szumu, niezależnie od licznika i odległości, ponieważ poziom odczytywany był po zakończeniu transmisji. Obie ścieżki odbioru schodziły na chwilowy pomiar `GetRssiInst`, wykonywany po `RX_DONE` i po wyczytaniu całego bufora, więc mierzący pusty kanał. Stała wartość nie była zastępnikiem - była rzeczywistym pomiarem szumu tła.
- W ścieżce strumieniowej (AN1200.53) ta droga zapasowa wykonywała się zawsze: procedura celowo nie pozwala silnikowi pakietowemu zakończyć pakietu, więc `GetPacketStatus` nigdy nie zatrzaskuje wartości dla przechwytywanej ramki.
- Sterownik preferował też niewłaściwy rejestr. `RssiAvg` uśrednia po całym oknie odbioru, które wynosi stałe 255 bajtów, więc dla ramki wM-Bus o długości 134 bajtów ponad połowa to szum po zakończeniu transmisji. Pomiar na jednej ramce: `RssiSync` -97 dBm wobec `RssiAvg` -117 dBm.
- RSSI jest teraz próbkowane w trakcie trwania ramki - z zapamiętaniem maksimum w miarę napływania bajtów w ścieżce strumieniowej, oraz z `RssiSync` w ścieżce FIFO. Pomiar chwilowy po ramce został usunięty.
- Skalowanie zapisane było jako `-((int) raw) >> 1` zamiast `-raw / 2`; unarny minus wiąże silniej niż przesunięcie, więc dawało to przesunięcie arytmetyczne wartości ujemnej i zaokrąglanie w dół zamiast obcinania.
- Ramka, dla której nie da się zmierzyć poziomu, raportuje teraz -127 dBm ("nie zmierzono") zamiast wartości zmyślonej, a takie próbki są wykluczane ze średnich per licznik i diagnostycznych, zamiast je zaniżać.

### Uwagi
- **Dotychczasowe dane RSSI nie są porównywalne z nowymi** na żadnej płytce z SX1262. Średnie per licznik, `win_avg_rssi` i wszystkie metryki pochodne od RSSI wymagają ponownego zebrania po aktualizacji.
- Pierwsza ramka na każdej ścieżce odbioru zapisuje surowe wartości z radia na poziomie INFO, dzięki czemu można potwierdzić aktywne źródło na sprzęcie bez podnoszenia poziomu logowania.

---

## Poprawka: dopasowanie ID spoza BCD w forward_meters i highlight_meters

### Naprawiono
- Dopasowanie liczników dekodowało A-field jako BCD i w przeciwnym razie rezygnowało, więc liczniki bez ID w BCD (m.in. Diehl/IZAR) nie miały żadnego użytecznego ID. Nie dało się ich wpisać do `highlight_meters`, a przy aktywnym `forward_meters` ich telegramy znikały bez śladu - jedyny przypadek, w którym whitelista odrzucała ramki, których użytkownik nie mógł odzyskać żadną konfiguracją.
- Obie opcje dopasowują teraz również surową wartość A-field, zapisywaną tak, jak pokazuje ją log: `id:417F0666` konfigurujesz jako `"0x417F0666"` (w cudzyslowie, inaczej YAML zamieni to na liczbe). Wpisy dziesiętne zachowują dotychczasowe znaczenie, więc żadna konfiguracja nie zmienia zachowania.
- Rozróżnienie obu form jest jednoznaczne: A-field poza BCD zawsze zawiera półbajtówkę powyżej 9, więc zawsze wypisuje literę szesnastkową, a ID w BCD nigdy. Forma `0x` działa też dla liczników BCD (`"0x00088888"` to licznik `88888`).
- Statystyki per licznik były kluczowane po ID z BCD, więc wszystkie liczniki nie-BCD zlewały się w jeden wspólny wpis pod kluczem 0. Teraz kluczem jest surowa wartość A-field, unikalna dla każdego licznika.
- `target_meter_id` nadal przyjmuje wyłącznie ID w BCD. Wartość szesnastkowa była tam dotąd przyjmowana i po cichu nigdy nie pasowała; teraz przy starcie pojawia się ostrzeżenie kierujące do `forward_meters`.

---

## Poprawka: NO_METERS_DETECTED nie wyzwala się po cichym oknie podsumowania

### Naprawiono
- `NO_METERS_DETECTED` sugerowało usterkę okablowania lub konfiguracji radia za każdym razem, gdy pojedyncze okno podsumowania nie zawierało ramek. Ten licznik (`diag_total_`) jest zerowany po każdym oknie, więc odbiornik działający cały dzień i tak raportował `total == 0` w dowolnej cichej minucie - a liczniki nocą standardowo milczą. Sugestia ogranicza się teraz do odbiorników, które od startu nie odebrały ani jednej ramki, i dodatkowo jest wyciszona przez pierwsze 5 minut pracy, gdzie cisza nic nie znaczy, bo liczniki nadają co kilkadziesiąt sekund do kilku minut.
- Odbiornik, który działał i zamilkł, to inna diagnoza - pozostaje przy pulsie health (`sec_since_last_rx`); ta sugestia nigdy tego nie mierzyła.
- Hint `NO_DATA` w podsumowaniu mówił "no packets received yet", co brzmi jak "nic nigdy nie przyszło", choć opisuje pojedyncze okno. Teraz mówi "no packets in this window". Maszynowe `hint_code` bez zmian.

---

## Nowość: forward_meters — lista dozwolonych liczników dla strumienia RAW

### Dodano
- `forward_meters` ogranicza to, które liczniki trafiają na `wmbus/<topic_name>/telegram`. Przydatne tam, gdzie większość odbieranego ruchu pochodzi od liczników sąsiadów, a własnych jest kilka.
- Przyjmuje jawną listę ID liczników albo `true`, które bierze ID już wpisane w `highlight_meters` - dzięki temu ta sama lista nie musi być pisana dwa razy.
- Pusta lista (domyślnie) albo `false` przepuszcza każdą zdekodowaną ramkę, więc istniejące konfiguracje działają bez zmian.
- Sparsowane ID trafiają do logu z `setup()` oraz ponownie w opóźnionym bloku startowym, obok informacji sanity YAML. Powtórzenie jest celowe: `setup()` wykonuje się, zanim podłączy się logger sieciowy, więc przy `esphome logs` widać dopiero to drugie. `dump_config()` raportuje ten sam stan, ale przy `logger: level: info` jest odfiltrowany - wymaga `level: config` lub wyżej.

### Uwagi
- Filtr działa po dekodowaniu i sprawdzeniu DLL CRC, więc dopasowuje ID, które parser już zweryfikował. Filtrowanie po surowym nagłówku byłoby tańsze, ale zawodne: ID odczytane z ramki, która nie przeszła CRC, bywa przekłamane.
- Dopasowanie idzie po ośmiocyfrowym ID zdekodowanym z BCD. Licznik, którego log pokazuje ID szesnastkowo (`id:417F0666`), ma A-field poza BCD i nie da się go wpisać na whitelistę - przy aktywnym filtrze zostanie odrzucony.
- `forward_meters: true` przy pustym `highlight_meters` nie wycisza strumienia: filtr się nie włącza, a przy starcie pojawia się ostrzeżenie konfiguracyjne.
- Diagnostyka jest nietknięta. Liczniki i statystyki RSSI powstają przed publikacją, więc summary dalej obejmuje cały eter razem z sąsiadami; obcinany jest sam strumień RAW.
- `target_meter_id` ma własny topic i celowo nie podlega whiteliście.

---

## Poprawka: zabezpieczenie gmtime()/strftime() w znaczniku czasu rtlwmbus

### Naprawiono
- `Frame::as_rtlwmbus()` zabezpiecza teraz przypadki, gdy `std::gmtime()` zwraca `nullptr`, a `std::strftime()` zwraca `0` — z fallbackiem do stałego znacznika `1970-01-01 00:00:00.00Z`. Zapobiega potencjalnemu null-dereference / odczytowi niedokończonego bufora, gdy zegar systemowy trzyma wartość `time_t` niemożliwą do reprezentacji (np. nieustawiony lub poza zakresem). Wyłącznie utwardzenie — bez zmiany formatu wyjścia w normalnej pracy.

---

## Aktualizacja dokumentacji

### Dodano
- Eksperymentalny tryb odbioru S1.
- `listen_mode: s1` używa dedykowanej ścieżki odbioru S1 i nie przechodzi przez logikę T1/C1.
- `listen_mode: both` nadal oznacza tylko T1/C1.
- `listen_mode: s1` domyślnie ustawia `868.300 MHz`; T1/C1/both zostają przy `868.950 MHz`.
- Jawne `frequency:` w YAML nadal nadpisuje domyślną częstotliwość trybu.

### Uwagi
- Obsługa S1 jest przeznaczona do diagnostyki i testów kompatybilności.
- Jeżeli poprawny telegram S1 zostanie odebrany, zostanie opublikowany do MQTT tak jak inne zweryfikowane telegramy wM-Bus.
- Dekodowanie wartości licznika pozostaje po stronie backendu i zależy od drivera oraz klucza szyfrowania.
- Systemy zamknięte albo odpytywane mogą nie nadawać standardowych pasywnych telegramów S1.

---

## Adaptacyjny SX1276 i diagnostyka MQTT

**Podsumowanie**
Poprawa działania adaptive dla SX1276, dodanie sugestii diagnostycznych MQTT oraz rozbudowa diagnostyki runtime.

**Opis**
To wydanie poprawia rzeczywiste zachowanie mostka RAW-only wM-Bus, szczególnie dla SX1276 w zaszumionym środowisku RF.

Najważniejsze zmiany:

* poprawiono logikę `sx1276_busy_ether_mode: adaptive`, tak aby aktywacja reagowała na realne straty odbioru, a nie tylko sam szum radiowy
* dodano eventy MQTT `suggestion` z praktycznymi wskazówkami diagnostycznymi i gotowymi snippetami YAML
* dodano eventy MQTT `busy_ether_changed` dla zmian stanu adaptive
* rozszerzono raporty diagnostyczne o nowe pola runtime, w tym `busy_ether_state`
* dodano/rozszerzono raporty `summary_15min`, `summary_60min` oraz snapshoty per-meter
* poprawiono kilka niespójności logicznych i dokumentacyjnych wykrytych podczas testów na realnym sprzęcie
* poprawka: `busy_ether_state` w JSON `/summary` emituje teraz `"n/a"` na SX1262 zamiast mylącego `"adaptive_passive"` (algorytm nigdy nie działał na SX1262, serializowana była tylko wartość pola)
* poprawka: `hint_code` nie pozostaje już `"OK"` przy 11-99% dropów bez konkretnej diagnozy — nowy kod `MODERATE_DROPS` sprawia że podwyższone straty zawsze widoczne są jako WARN w logu
* zmiana: log per-pakietu `highlight_meters` wyświetla teraz `packet #N received` zamiast poprzedniego `stats / statystyki: count=N interval=... avg_rssi=...`; statystyki per-licznik nadal dostępne przez MQTT `meter_window`

To wydanie nie zmienia architektury projektu: urządzenie ESP nadal odpowiada za odbiór RF i publikację RAW do MQTT, a dekodowanie liczników pozostaje poza nim.
