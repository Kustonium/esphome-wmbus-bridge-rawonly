# Change: halve the S1 capture budget once the header is known undecodable

## EN

### Changed
- An S1 stream capture now stops at 256 raw bytes instead of 512 from the moment `s1_expected_raw_len_()` has looked at the header and produced nothing. The full 512-byte budget is kept whenever a length is derivable.
- The moment is knowable: `s1_expected_raw_len_()` only ever reads raw bytes 0..3, and those do not change during a capture. Once it has seen them and failed, it will fail for the rest of that capture, so the frame is already lost and the only remaining question is how long to stay deaf gathering evidence about it.
- 512 bytes is 127 ms, measured against an air rate of 244 us per raw byte. Half of that is still ample to see what the stream looked like, and hands the receiver back 63 ms sooner - which matters on a band where a second transmission can follow closely.

### Notes
- The cap is deliberately not lowered unconditionally. A legitimate long S-mode frame needs the full budget: an L-field of 150 works out to roughly 340 raw bytes, which a fixed 256-byte cap would truncate. Only captures that have already failed are shortened.
- Measured alongside: at -82/-85 dBm the errors in a marginal capture begin in the C-field or later and the length is still derived; at -90/-95 dBm they are already in the L-field. Nothing can rescue the second case, and nothing should try - a substituted bit in L yields a wrong length rather than a recoverable one.

## PL

### Zmieniono
- Przechwytywanie strumienia S1 zatrzymuje się teraz na 256 bajtach surowych zamiast 512 od chwili, gdy `s1_expected_raw_len_()` obejrzało nagłówek i nic z niego nie wyprowadziło. Pełny budżet 512 bajtów zostaje wszędzie tam, gdzie długość da się wyliczyć.
- Tę chwilę da się rozpoznać: `s1_expected_raw_len_()` czyta wyłącznie surowe bajty 0..3, a te nie zmieniają się w trakcie przechwytywania. Gdy raz je zobaczy i zawiedzie, będzie zawodzić do końca tego przechwycenia - ramka jest już stracona, a jedyne pozostałe pytanie brzmi, jak długo pozostawać głuchym, zbierając o niej dowody.
- 512 bajtów to 127 ms, zmierzone wobec tempa eteru 244 us na bajt surowy. Połowa nadal w zupełności wystarcza, żeby zobaczyć, jak wyglądał strumień, a oddaje odbiornik 63 ms wcześniej - co ma znaczenie w paśmie, gdzie druga transmisja może przyjść tuż po pierwszej.

### Uwagi
- Limit celowo nie jest obniżony bezwarunkowo. Prawidłowa długa ramka S-mode potrzebuje pełnego budżetu: pole L równe 150 daje około 340 bajtów surowych, które sztywny limit 256 bajtów by uciął. Skracane są wyłącznie przechwycenia już nieudane.
- Zmierzone przy okazji: przy -82/-85 dBm błędy w przechwyceniu na granicy zaczynają się w polu C albo dalej i długość nadal się wylicza; przy -90/-95 dBm siedzą już w polu L. Tego drugiego przypadku nic nie uratuje i nic nie powinno próbować - podstawiony bit w L daje złą długość, a nie taką, którą da się odzyskać.

---

# Fix: one bad chip in the C-field threw away the whole S1 capture

## EN

### Fixed
- `s1_expected_raw_len_()` required all sixteen Manchester pairs of the L- and C-fields to decode. The C-field now tolerates up to two invalid pairs and is matched against 0x44 / 0x46 on the bits that did decode; the L-field still demands perfection, because its value cuts the capture and a substituted bit there produces a wrong length rather than a recoverable one.
- The C-field exists only to stop the complemented polarity selecting a plausible but wrong L-field. It contributes nothing to the length, so demanding it decode perfectly bought nothing and cost a great deal.

### Measured
At the sensitivity threshold on 2026-08-01, two captures of the same 85-byte transmission 30 seconds apart:

| RssiSync | first bytes | outcome |
|---|---|---|
| -88 dBm | `66 65 65 65 …` | `exit=s1_length`, 194 B captured, decoded, one bad pair in 776 |
| -89.5 dBm | `66 65 65 6D …` | `exit=buffer_cap`, 512 B captured, lost |

- One chip error, in raw byte 3, turned `0x65` into `0x6D`. That byte is the second half of the C-field. No length could be derived, so the capture ran to the cap and collected roughly 85 ms of post-frame noise on top of a 47 ms frame.
- The measurement consequence was worse than the lost frame. A successful capture reports invalid pairs out of 776, a failed one out of 2048 with three quarters of it noise, so the same signal degrading by a fraction of a dB appears to fall off a cliff. That artefact made the S1 failures look categorically different from marginal reception when they were the same thing.

### Notes
- This does not make a transmitter below the noise floor decodable. It recovers frames at the very edge where the unlucky chip landed in the header instead of the payload, and it makes `symbols_invalid` and the capture-quality figures measure signal quality rather than whether the header happened to survive.

## PL

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

# Result: S1 bandwidth sweep finished, 234.3 kHz is the optimum

## EN

### Changed
- The S1 receive bandwidth returns to 234.3 kHz and stays there. The two test commits that moved it to 312 kHz and then 156.2 kHz have served their purpose.

### Measured
Longest run of valid Manchester pairs in an SX1262 capture, each taken on a transmission an SX1276 decoded in the same second, out of the 680 pairs an 85-byte telegram needs. Random data gives about 11.

| RX bandwidth | longest valid run |
|---|---|
| 312.0 kHz | 30 pairs |
| **234.3 kHz** | **191 pairs** |
| 156.2 kHz | 47 pairs |

- A genuine peak, not a trend: both directions cost a factor of four to six. Widening admits noise, narrowing starts cutting a signal whose occupied spectrum is wider than either Carson's rule (133 kHz) or Semtech's sizing rule (143 kHz) predicts for a Manchester-coded BT=0.5 chip stream.
- The 2026-07-30 finding that 156.2 kHz "stopped reception entirely" is now qualified: it does capture, it captures worse. That earlier measurement was taken before the AN1200.53 capture fix, when no bandwidth decoded anything.

### Notes
- At the optimum the capture still holds 191 of the 680 pairs a frame needs. Bandwidth is worth a factor of four here and no more; it is not what stops the SX1262 decoding S-mode. The sweep is closed and does not need re-running.

## PL

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

# Test: S1 bandwidth sweep, third point at 156.2 kHz

## EN

### Changed
- The S1 receive bandwidth moves from 312 kHz to 156.2 kHz. Third measurement point of a sweep, still a test. C1 keeps 234.3 kHz and is not part of this.

### Measured
Longest run of valid Manchester pairs in an SX1262 capture, taken on the same transmission an SX1276 decoded in the same second, out of the 680 pairs an 85-byte telegram needs (random data gives about 11):

| RX bandwidth | longest valid run |
|---|---|
| 312.0 kHz | 30 pairs |
| 234.3 kHz | 191 pairs |
| 156.2 kHz | this build |

- Widening to 312 kHz made recovery six times worse, which disposes of the argument that a filter narrower than the signal was smearing chip edges. What remains is ordinary noise bandwidth, and the trend between the two measured points runs towards narrower.
- 156.2 kHz was tried once before, on 2026-07-30, and reported as stopping S1 reception entirely. That was measured before the AN1200.53 capture fix, when no bandwidth decoded anything - the same objection that justified re-testing 312 kHz.

## PL

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

# Test: S1 on SX1262 goes back to the 312 kHz receive bandwidth

## EN

### Changed
- The S1 receive bandwidth returns from 234.3 kHz to 312 kHz. This is a test, marked as one, to be reverted if it changes nothing. C1 keeps 234.3 kHz - it has no comparable history and decodes normally.
- Why re-run a setting that was already replaced: the AN1200.53 capture fix landed 38 minutes after the move to 234.3 kHz. Every wide-bandwidth test therefore ran on a broken capture path, and every fixed-capture test ran narrow. 312 kHz has never been tried with the receive path in its current state.
- Why the original argument for narrowing looks wrong: it was about noise, and 234.3 kHz does admit about 1.25 dB less than 312 kHz. But that is a sensitivity argument and says nothing about distortion. The measurements run the other way - 156.2 kHz stopped S1 reception outright, and 234.3 kHz syncs on real S-mode frames and has never once decoded one. Carson's rule gives 133 kHz for this signal and Semtech's sizing rule 143 kHz; a setting above both killed reception completely, so both under-describe the occupied spectrum of a Manchester-coded BT=0.5 chip stream. A filter narrower than the signal does not only reject noise, it smears the chip edges.

### Notes
- An earlier note in this file claimed S1 kept the wide 312 kHz bandwidth. That stopped being true on 2026-07-30 and the note was left stale for two days; it is corrected here.

## PL

### Zmieniono
- Szerokość pasma odbioru dla S1 wraca z 234,3 kHz na 312 kHz. To jest test, oznaczony jako test, do cofnięcia jeśli nic nie zmieni. C1 zostaje na 234,3 kHz - nie ma porównywalnej historii i dekoduje normalnie.
- Dlaczego powtarzać ustawienie, które już raz zastąpiono: poprawka przechwytywania AN1200.53 weszła 38 minut po zmianie na 234,3 kHz. Każdy test szerokiego pasma odbył się więc na zepsutej ścieżce przechwytywania, a każdy test naprawionej ścieżki odbył się na wąskim paśmie. 312 kHz nigdy nie było próbowane z torem odbiorczym w obecnym stanie.
- Dlaczego pierwotny argument za zwężeniem wygląda na błędny: dotyczył szumu i faktycznie 234,3 kHz wpuszcza o ~1,25 dB mniej niż 312 kHz. Ale to argument o czułości i nie mówi nic o zniekształceniach. Pomiary układają się odwrotnie - 156,2 kHz zatrzymało odbiór S1 całkowicie, a 234,3 kHz łapie sync na realnych ramkach S-mode i nie zdekodowało ani jednej. Reguła Carsona daje dla tego sygnału 133 kHz, reguła Semtecha 143 kHz; ustawienie powyżej obu zabiło odbiór, więc obie zaniżają zajętość widma strumienia chipów Manchester z BT=0,5. Filtr węższy od sygnału nie tłumi tylko szumu, ale też rozmywa zbocza chipów.

### Uwagi
- Wcześniejsza notatka w tym pliku twierdziła, że S1 pozostaje na szerokim paśmie 312 kHz. Przestało to być prawdą 2026-07-30 i notatka wisiała nieaktualna przez dwa dni; zostaje tu sprostowana.

---

# Diagnostics: find where the S1 frame actually starts in a capture

## EN

### Added
- Under `diagnostic_verbose`, every S1 stream capture on the SX1262 is searched for the chip offset at which a valid L+C header actually sits - all offsets up to 512 chips, both Manchester polarities - and the result is logged with the frame length and the number of invalid pairs inside the frame itself.
- `s1_expected_raw_len_()` assumes the frame begins at chip 0, because the radio strips the sync word in hardware and the payload should follow immediately. Every S1 capture on this driver ends at `exit=buffer_cap`, which is exactly what happens when no length can be derived from those first bytes.
- What prompted it: three captures of the same repeater transmission, decoded identically by an SX1276 in the same second on 2026-08-01, produced three completely different first bytes here - `99363510…`, `998A9A9A…`, `DDFBDA9A…`. Identical air content, different buffer content.

### Notes
- The output separates three cases that were previously indistinguishable. A stable `chip=0` clears the capture path and points at demodulation. An offset that moves between captures locates a start-alignment bug. No valid header anywhere means the frame is not in the capture at all.
- The invalid-pair count is deliberately taken over the frame body only. Counted across the whole 416-byte buffer it is dominated by the post-frame noise the capture keeps collecting, which is what made earlier readings of that number misleading.
- Nothing is changed in how captures are taken. This only looks at what was captured.

## PL

### Dodano
- Przy `diagnostic_verbose` każde przechwycenie strumienia S1 na SX1262 jest przeszukiwane pod kątem przesunięcia chipowego, na którym faktycznie siedzi poprawny nagłówek L+C - wszystkie przesunięcia do 512 chipów, obie polaryzacje Manchester - a wynik trafia do logu razem z długością ramki i liczbą niepoprawnych par wewnątrz samej ramki.
- `s1_expected_raw_len_()` zakłada, że ramka zaczyna się na chipie 0, bo radio zdejmuje słowo synchronizacji sprzętowo i ładunek powinien następować od razu po nim. Każde przechwycenie S1 na tym sterowniku kończy się na `exit=buffer_cap`, czyli dokładnie tym, co dzieje się, gdy z tych pierwszych bajtów nie da się wyprowadzić długości.
- Co to sprowokowało: trzy przechwycenia tej samej transmisji repeatera, zdekodowane identycznie przez SX1276 w tej samej sekundzie dnia 2026-08-01, dały tutaj trzy zupełnie różne pierwsze bajty - `99363510…`, `998A9A9A…`, `DDFBDA9A…`. Identyczna treść w eterze, różna treść w buforze.

### Uwagi
- Wynik rozdziela trzy przypadki, dotąd nieodróżnialne. Stabilne `chip=0` oczyszcza ścieżkę przechwytywania i wskazuje na demodulację. Przesunięcie zmieniające się między przechwyceniami lokalizuje błąd wyrównania startu. Brak poprawnego nagłówka gdziekolwiek oznacza, że ramki w przechwyceniu w ogóle nie ma.
- Liczba niepoprawnych par jest celowo liczona wyłącznie po ciele ramki. Liczona po całym 416-bajtowym buforze jest zdominowana przez szum po ramce, który przechwytywanie zbiera dalej - i to właśnie czyniło wcześniejsze odczyty tej liczby mylącymi.
- Sposób przechwytywania nie zmienia się w niczym. To tylko patrzy na to, co zostało przechwycone.

---

# Fix: the frequency-error readout described noise, not the decoded frame

## EN

### Fixed
- `RegAfc` and `RegFei` are now sampled once per frame by `latch_frame_metrics_()`, at the moment its first bytes reach the FIFO, together with RSSI. `dump_debug_status()` reports the latched values and how many seconds old they are, and no longer re-reads the registers.
- The previous version read them live inside the dump. That dump runs on a receive-wait timeout - by definition when nothing has arrived for a minute - so it returned whatever noise last tripped the preamble detector. Measured on 2026-08-01 on a node decoding one transmitter every 123 seconds: +23.5 kHz two seconds before a frame, then -17.6 kHz corrected with a +68.5 kHz residual on a preamble that never matched sync, then -16.1 kHz three seconds before the next frame. None of those came from the transmitter being decoded.
- RSSI is copied into a separate sticky field for the diagnostic. `restart_rx()` resets the value handed to the packet to the not-measured sentinel on every re-arm, which is correct there but would have made the dump report -127 for a frame whose level was measured.

### Notes
- The general shape of this is the same mistake twice: a register that is only valid at one instant, read at another instant, producing a number that looks authoritative and describes nothing. The RSSI sampling fix in July had exactly this cause.

## PL

### Naprawiono
- `RegAfc` i `RegFei` są teraz próbkowane raz na ramkę przez `latch_frame_metrics_()`, w chwili gdy jej pierwsze bajty trafiają do FIFO, razem z RSSI. `dump_debug_status()` raportuje zatrzaśnięte wartości oraz to, ile mają sekund, i nie odczytuje już rejestrów ponownie.
- Poprzednia wersja czytała je na żywo wewnątrz dumpu. Ten dump biegnie na przeterminowaniu oczekiwania na ramkę - czyli z definicji wtedy, gdy od minuty nic nie przyszło - więc zwracał to, co ostatnio potrąciło detektor preambuły, czyli szum. Zmierzone 2026-08-01 na węźle dekodującym jeden nadajnik co 123 sekundy: +23,5 kHz dwie sekundy przed ramką, potem −17,6 kHz korekty z residuum +68,5 kHz na preambule, która nigdy nie dopasowała sync, potem −16,1 kHz trzy sekundy przed kolejną ramką. Żadna z tych liczb nie pochodziła od dekodowanego nadajnika.
- RSSI jest kopiowane do osobnego, trwałego pola na potrzeby diagnostyki. `restart_rx()` przy każdym uzbrojeniu zeruje wartość przekazywaną do pakietu do sentinela „nie zmierzono", co jest tam poprawne, ale sprawiłoby, że dump raportowałby −127 dla ramki, której poziom zmierzono.

### Uwagi
- Kształt tego błędu jest ten sam co poprzednio: rejestr ważny tylko w jednej chwili, odczytany w innej, dający liczbę, która wygląda autorytatywnie i nie opisuje niczego. Lipcowa poprawka próbkowania RSSI miała dokładnie tę przyczynę.

---

# Diagnostics: SX1276 reports the frequency error of the last reception

## EN

### Added
- `dump_debug_status()` now prints `RegAfc` and `RegFei` with both the raw register value and the converted offset in Hz. `RegFei` is what the receiver measured, `RegAfc` is what the AFC actually corrected by; both are latched from the last received frame, so on a link mode where frames are minutes apart the reading still describes the last real transmitter rather than noise.
- The reason it is worth having: the SX126x has no AFC in GFSK at all, so whatever offset this register reports is error an SX1262 has to swallow whole. Measured on T1 on a LilyGO T3-S3 on 2026-08-01, the AFC was correcting −37.8 kHz on live meters.

### Notes
- This turns a frequency sweep into a single read. Instead of retuning a receiver in steps and watching whether reception improves, the offset of the transmitter that was actually decoded can be read off directly and applied once.

## PL

### Dodano
- `dump_debug_status()` wypisuje teraz `RegAfc` i `RegFei`, zarówno surową wartość rejestru, jak i przeliczone przesunięcie w hercach. `RegFei` to to, co odbiornik zmierzył, `RegAfc` to to, o ile AFC faktycznie skorygowało; obie wartości są zatrzaśnięte z ostatnio odebranej ramki, więc w trybie łącza, gdzie ramki dzielą minuty, odczyt nadal opisuje ostatni realny nadajnik, a nie szum.
- Dlaczego warto: SX126x nie ma w GFSK żadnego AFC, więc każde przesunięcie raportowane przez ten rejestr jest błędem, który SX1262 musi przyjąć w całości. Zmierzone na T1 na LilyGO T3-S3 dnia 2026-08-01: AFC korygowało −37,8 kHz na żywych licznikach.

### Uwagi
- To zamienia przemiatanie częstotliwości w jeden odczyt. Zamiast przestrajać odbiornik krokami i patrzeć, czy odbiór się poprawia, można odczytać wprost przesunięcie nadajnika, który faktycznie się zdekodował, i zastosować je raz.

---

# Fix: SX1276 ran with the high-frequency LNA boost off

## EN

### Fixed
- `RegLna` (0x0C) is now written as `0x23`: maximum gain plus `LnaBoostHf`. The driver never wrote the register at all, so it ran on the reset default `0x20` - same gain, boost off. Confirmed on hardware 2026-08-01, a register-bank dump of a LilyGO T3-S3 read `0x0C = 0x20`.
- `0x23` is Semtech's own value. It is what LoRaMac-node puts in `RADIO_INIT_REGISTERS_VALUE` for every FSK board, and the omission was found by diffing this driver's setup sequence against that table rather than by observing a symptom.
- The gain half of the register matters less than it looks: `AgcAutoOn` is set immediately afterwards, so the AGC drives `LnaGain` itself. `LnaBoostHf` is the part that persists - it raises LNA current by 50% for a better noise figure. It applies above 525 MHz and is ignored below, so the write is unconditional.

### Notes
- This is a sensitivity change on every listen mode, not just S1. It has not been measured here; the argument for it is that the manufacturer's reference driver sets it and this one did not. Frame counts before and after on an unchanged node are the way to find out whether it is worth anything.

## PL

### Naprawiono
- `RegLna` (0x0C) jest teraz zapisywany wartością `0x23`: maksymalne wzmocnienie plus `LnaBoostHf`. Sterownik w ogóle nie dotykał tego rejestru, więc pracował na wartości resetowej `0x20` - to samo wzmocnienie, boost wyłączony. Potwierdzone na sprzęcie 2026-08-01, zrzut banku rejestrów LilyGO T3-S3 pokazał `0x0C = 0x20`.
- `0x23` to wartość samego Semtecha. Tyle wpisuje LoRaMac-node w `RADIO_INIT_REGISTERS_VALUE` dla każdej płytki FSK, a brak wyszedł z porównania sekwencji `setup()` z tamtą tablicą, nie z obserwacji objawu.
- Połowa dotycząca wzmocnienia znaczy mniej, niż wygląda: `AgcAutoOn` jest ustawiane linijkę dalej, więc `LnaGain` prowadzi sam układ AGC. Zostaje `LnaBoostHf` - podnosi prąd LNA o 50% dla lepszej liczby szumowej. Działa powyżej 525 MHz, poniżej jest ignorowany, więc zapis jest bezwarunkowy.

### Uwagi
- To zmiana czułości we wszystkich trybach nasłuchu, nie tylko w S1. Nie została tutaj zmierzona; argumentem za nią jest to, że sterownik referencyjny producenta to ustawia, a ten nie ustawiał. Liczba ramek przed i po na niezmienionym węźle jest sposobem, żeby się dowiedzieć, czy to cokolwiek daje.

---

# Note: on the SX127x in FSK, RegOpMode does not tell you whether RX is running

## EN

After writing RX (`0b101`) to `RegOpMode`, the SX1276 reads the register back as `0b100` - FSRX, frequency synthesis with the receiver off - and reports `ModeReady` and `RxReady` clear in `RegIrqFlags1`, on a receiver that is working normally. Measured on a LilyGO T3-S3 on 2026-08-01: a node in that exact state decoded three T1 frames at -75, -91 and -95 dBm within the same second, and printed the register readback in between.

This is known behaviour, not a board fault. RadioLib's `SX127x::setMode()` masks the low mode bit out of its write verification for FSK RX specifically, with the comment "disable checking of RX bit in FSK RX mode, as it sometimes seem to fail (#276)".

### Notes
- Nothing in the driver tests the mode readback any more. `dump_debug_status()` still prints `RegOpMode`, because the value is worth seeing, but it no longer derives a `receiver_running` claim from it and no longer warns that nothing can be received. An earlier version of that warning fired for hours on a receiver that was decoding frames while it fired.
- A `ModeReady` wait was added to the S1 arming path on the strength of that reading and has been removed again. It was polling for a bit that does not come back even when the transition succeeds, and it cost 2 ms of busy-waiting twice per re-arm.
- The general lesson is the one this project already applies to RSSI: a register that reports something impossible is worse than a register nobody reads, because the impossible value still gets reasoned about.

## PL

Po zapisaniu RX (`0b101`) do `RegOpMode` SX1276 odczytuje ten rejestr jako `0b100` - FSRX, czyli synteza częstotliwości z wyłączonym odbiornikiem - i raportuje wyzerowane `ModeReady` oraz `RxReady` w `RegIrqFlags1`, na odbiorniku pracującym normalnie. Zmierzone na LilyGO T3-S3 dnia 2026-08-01: węzeł dokładnie w tym stanie zdekodował trzy ramki T1 przy -75, -91 i -95 dBm w tej samej sekundzie, a pomiędzy nimi wypisał ten odczyt rejestru.

To jest znane zachowanie, nie usterka płytki. `SX127x::setMode()` w RadioLib maskuje najmłodszy bit trybu przy weryfikacji zapisu, konkretnie dla FSK RX, z komentarzem „disable checking of RX bit in FSK RX mode, as it sometimes seem to fail (#276)".

### Uwagi
- Nic w sterowniku nie testuje już odczytu trybu. `dump_debug_status()` nadal wypisuje `RegOpMode`, bo tę wartość warto widzieć, ale nie wyprowadza z niej twierdzenia `receiver_running` ani nie ostrzega, że nic nie zostanie odebrane. Wcześniejsza wersja tego ostrzeżenia wypisywała się godzinami na odbiorniku, który w trakcie dekodował ramki.
- Na podstawie tego odczytu dodano wcześniej czekanie na `ModeReady` na ścieżce uzbrajania S1 i zostało ono usunięte. Odpytywało o bit, który nie wraca nawet przy udanym przejściu, i kosztowało 2 ms zajętego oczekiwania dwukrotnie na każde uzbrojenie.
- Ogólny wniosek jest ten sam, który ten projekt stosuje już do RSSI: rejestr raportujący rzecz niemożliwą jest gorszy od rejestru, którego nikt nie czyta, bo o niemożliwej wartości i tak się potem argumentuje.

---

# Fix: `clear_device_errors_on_boot` did nothing on a node that received nothing

## EN

### Fixed
- The SX1262 device-error clear has moved from `capture_rx_stream_()` to `setup()`. It was gated on the first captured frame, so on a node receiving normally it ran within seconds and nobody ever saw the flag, while on a node receiving nothing it never ran at all - leaving `clear_device_errors_on_boot: true` inert in the one case where the error register is the only thing left to read. Observed on hardware 2026-08-01: two SX1262 nodes reporting `XOSC_START_ERR` for minutes on end while sitting in RX with a correct configuration.
- `XOSC_START_ERR` is set during the power-on sequence as a matter of course on a TCXO board, because the chip tries to start its crystal oscillator before DIO3 has been told to power the TCXO - DIO3 is configured after reset. Clearing it once the reference is set up is what the datasheet expects. Paired with the re-read at the end of `setup()`, the flag now means something: one that clears was a power-on artefact, one that comes back after a clean clear is a reference that genuinely is not starting.
- Removing the block from the receive path also takes a 7 ms blocking delay off the first capture.

### Notes
- This entry originally also announced an SX1276 warning derived from reading `RegOpMode` back after arming RX. That reading turned out not to mean what it looked like and the warning has been removed - see the note about `RegOpMode` in FSK RX above.

## PL

### Naprawiono
- Czyszczenie błędów układu SX1262 przeniesione z `capture_rx_stream_()` do `setup()`. Było uzależnione od pierwszej przechwyconej ramki, więc na węźle odbierającym normalnie wykonywało się w kilka sekund i nikt nigdy tej flagi nie zobaczył, a na węźle nieodbierającym nic nie wykonywało się wcale - przez co `clear_device_errors_on_boot: true` nie robiło nic dokładnie w tym jedynym przypadku, w którym rejestr błędów jest ostatnią rzeczą, jaka została do odczytania. Zaobserwowane na sprzęcie 2026-08-01: dwa węzły SX1262 raportujące `XOSC_START_ERR` przez wiele minut, stojąc w RX z poprawną konfiguracją.
- `XOSC_START_ERR` jest ustawiane przy starcie na płytce z TCXO w sposób normalny, bo układ próbuje uruchomić swój oscylator kwarcowy, zanim DIO3 dostanie polecenie zasilania TCXO - DIO3 konfiguruje się po resecie. Wyczyszczenie flagi po ustawieniu referencji jest tym, czego oczekuje datasheet. W parze z ponownym odczytem na końcu `setup()` flaga wreszcie coś znaczy: taka, która znika, była artefaktem startu, taka, która wraca po czystym wyczyszczeniu, to referencja, która naprawdę nie startuje.
- Usunięcie tego bloku ze ścieżki odbiorczej zdejmuje przy okazji 7 ms blokującego opóźnienia z pierwszego przechwycenia.

### Uwagi
- Ten wpis pierwotnie zapowiadał także ostrzeżenie SX1276 wyprowadzone z odczytu `RegOpMode` po uzbrojeniu RX. Ten odczyt okazał się nie znaczyć tego, na co wyglądał, i ostrzeżenie zostało usunięte - patrz notatka o `RegOpMode` w FSK RX powyżej.

---

# Diagnostics: repeatable radio state instead of one snapshot per boot

## EN

### Added
- `dump_debug_status()` is now implemented for SX1276 and SX1262. It was declared on the base transceiver and implemented only by the CC1101, so the two SX drivers inherited an empty method: their registers were readable exactly once, at boot, and never again. The component already calls this on every receive-wait timeout when diagnostics are verbose, so a silent node now reports its own state roughly once a minute.
- SX1276 reports `RegOpMode` decoded to a mode name, `RegIrqFlags1/2`, RSSI, `RegRxConfig`, `RegRxBw`, `RegPreambleDetect`, the sync configuration and the live sync word, plus the DIO1 level. `preamble_detected` and `sync_matched` are restated in words, because the two bits that say whether anything is arriving are one bit each in the middle of a hex byte. A chip sitting in `FSRX` instead of `RX` - synthesiser locked, receiver off - now says so with a warning instead of looking configured.
- SX1262 reports the `GetStatus` chip mode, latched IRQ status, device errors, `RegRxGain`, the live sync word, the stream pointers and the DIO1/BUSY levels, with the same restatement and the same warning when the chip is not in RX.
- The verbose flag is pushed into the driver before the receiver task is created, so no frame is handled while the component and the driver disagree about how much to report.

### Changed
- The SX1262 RSSI provenance snapshot (`IRQ=... captured=... first[8]=...`) repeats on every capture when diagnostics are verbose, instead of once per receive path per boot. One sample is the right volume when frames are decoding; it is useless when nothing is, because a single `first[8]` cannot separate a real frame captured out of alignment from the detector firing on noise. That needs a series. Outside verbose mode the one-shot behaviour is unchanged.
- A snapshot is dropped rather than overwritten if the previous one has not been drained yet. Writing into the slot while the main task copies out of it would produce a log line built from two different captures, which is worse than a missing line precisely because it still looks like data.

### Notes
- Both changes come from a day spent comparing three receivers with no repeatable register reads. Boot-time-only diagnostics answer "did the chip answer over SPI"; every question that matters when reception stops is about the current state.

## PL

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

# Fix: SX1262 was never recalibrated after the TCXO was enabled

## EN

### Fixed
- `SetDIO3AsTcxoCtrl` hands the reference over to the TCXO, but DIO3 is what powers that TCXO - so every calibration the chip performed at power-on ran against the internal RC oscillator, a reference that stops existing one command later. The SX1261/2 datasheet requires the calibration to be relaunched afterwards. `Calibrate(0x7F)` is now issued right after the TCXO is enabled, recalibrating RC64k, RC13M, PLL, the three ADC blocks and the image. This runs only when `has_tcxo: true`, so boards without a TCXO are untouched.
- Missing this step does not fail loudly. The radio starts, arms RX and receives a transmitter on the same desk perfectly well; what it loses is the last few dB, which is the band real meters arrive in. Nothing in the log distinguished that from a bad antenna.
- `GetDeviceErrors` is now read once at the end of `setup()` and logged, with `XOSC_START_ERR` and `PLL_CALIB_ERR` raised to `ESP_LOGE`. A device-error snapshot already existed, but it lives in `capture_rx_stream_()` and fires on the first captured frame - never, in the one case worth diagnosing, where nothing is being received.

### Notes
- Found while comparing an SX1276 against an SX1262 on S-mode. It is not the explanation for that comparison and should not be credited with fixing it; it is a datasheet requirement that was missing on its own terms.

## PL

### Naprawiono
- `SetDIO3AsTcxoCtrl` przekazuje referencję do TCXO, ale to właśnie DIO3 ten TCXO zasila - więc każda kalibracja, którą układ wykonał przy starcie, poszła względem wewnętrznego oscylatora RC, czyli referencji, która przestaje istnieć jedną komendę później. Datasheet SX1261/2 wymaga powtórzenia kalibracji po tym kroku. `Calibrate(0x7F)` jest teraz wysyłane bezpośrednio po włączeniu TCXO i przelicza RC64k, RC13M, PLL, trzy bloki ADC oraz obraz. Wykonuje się wyłącznie przy `has_tcxo: true`, więc płytki bez TCXO pozostają nietknięte.
- Brak tego kroku nie objawia się głośno. Radio startuje, uzbraja odbiór i bez problemu odbiera nadajnik z tego samego biurka; traci ostatnie kilka dB, czyli dokładnie zakres, w którym docierają prawdziwe liczniki. Nic w logu nie odróżniało tego od złej anteny.
- `GetDeviceErrors` jest teraz odczytywane raz na końcu `setup()` i logowane, przy czym `XOSC_START_ERR` i `PLL_CALIB_ERR` podniesione do `ESP_LOGE`. Snapshot błędów układu już istniał, ale mieszka w `capture_rx_stream_()` i odpala się na pierwszej przechwyconej ramce - czyli nigdy w jedynym przypadku wartym diagnozy, gdy nic nie jest odbierane.

### Uwagi
- Znalezione przy porównywaniu SX1276 z SX1262 na trybie S. To nie jest wyjaśnienie tamtego porównania i nie należy mu tego przypisywać; to wymóg datasheetu, którego brakowało niezależnie.

---

# Fix: a frame could be logged as "RSSI: 0dBm"

## EN

### Fixed
- `sx126x_rssi_dbm_()` treated raw 0 as "register never written" and converted everything else. Raw 1 is -0.5 dBm, and integer division turns that into a clean `0 dBm` - a level no wM-Bus frame can arrive at, since the front end saturates around -5 dBm and a transmitter on the same desk at minimum power still lands tens of dB below. Observed on hardware: a correctly decoded frame reported as `RSSI: 0dBm`. The conversion now rejects the whole impossible top of the scale (raw below 20, i.e. above -10 dBm) and returns the not-measured sentinel instead.
- The plausibility rule lives in the conversion, not at each call site. The three places that read a level - in-flight sampling, packet status on the FIFO path, packet status on the stream path - ask for a converted value and take the first one that is not the sentinel. They previously tested the raw byte for `!= 0`, which let an implausible sync reading shadow a usable average.
- `Packet::rssi_` defaulted to 0. That is not a sentinel but a reading, and the same impossible one: a packet whose level was never set reported a perfect signal instead of admitting it had none. It now defaults to -127, which the statistics already know to exclude.

### Notes
- A fabricated number is worse than a missing one. This was found while comparing two receivers, where `RSSI: 0dBm` sat in the middle of a measurement session and had to be argued about before it could be dismissed.

## PL

### Naprawiono
- `sx126x_rssi_dbm_()` traktował surowe 0 jako „rejestr nigdy nie zapisany" i przeliczał całą resztę. Surowe 1 to -0,5 dBm, a dzielenie całkowite zamienia to w czyste `0 dBm` - poziom, przy którym żadna ramka wM-Bus nie dociera, bo tor wejściowy nasyca się w okolicach -5 dBm, a nadajnik na tym samym biurku przy minimalnej mocy i tak leży dziesiątki dB niżej. Zaobserwowane na sprzęcie: poprawnie zdekodowana ramka zaraportowana jako `RSSI: 0dBm`. Przeliczenie odrzuca teraz całą niemożliwą górę skali (surowe poniżej 20, czyli powyżej -10 dBm) i zwraca sentinel „nie zmierzono".
- Reguła wiarygodności mieszka w przeliczeniu, a nie w każdym miejscu wywołania. Trzy miejsca odczytujące poziom - próbka w locie, status pakietu na ścieżce FIFO i status pakietu na ścieżce strumieniowej - proszą o wartość przeliczoną i biorą pierwszą, która nie jest sentinelem. Wcześniej sprawdzały surowy bajt na `!= 0`, przez co niewiarygodny odczyt z synchronizacji przesłaniał użyteczną średnią.
- `Packet::rssi_` miał domyślnie 0. To nie jest sentinel, tylko odczyt, i to ten sam niemożliwy: pakiet, któremu nigdy nie ustawiono poziomu, raportował idealny sygnał zamiast przyznać, że go nie ma. Domyślną wartością jest teraz -127, którą statystyki już umieją pomijać.

### Uwagi
- Zmyślona liczba jest gorsza niż brak liczby. Znalezione przy porównywaniu dwóch odbiorników, gdzie `RSSI: 0dBm` wylądowało w środku sesji pomiarowej i trzeba było je najpierw obalić, zanim dało się je odrzucić.

---

# Note: S1 on SX1262 keeps the wide 312 kHz receive bandwidth

## EN

A narrower window was tried and reverted. Carson's rule puts the S-mode requirement at 132.8 kHz - 2 * (fdev + chiprate/2) for a 32.768 kchip/s Manchester stream - so S1 was moved from the inherited 312 kHz down to 156.2 kHz, expecting about 3 dB less noise. On hardware the opposite happened: a Heltec V4 stopped receiving S1 frames altogether while an SX1276 beside it kept decoding the same transmitter.

Carson under-describes a Manchester-coded signal. The chip stream carries real energy past the nominal deviation, and the datasheet bandwidth is a -3 dB figure rather than a flat passband, so the usable window is narrower than the number suggests. The code keeps 312 kHz for S1 with a comment saying not to narrow it again without measuring the received spectrum first.

## PL

Węższe okno zostało wypróbowane i wycofane. Reguła Carsona daje dla S-mode 132,8 kHz - 2 * (dewiacja + chiprate/2) dla strumienia Manchester 32,768 kchip/s - więc S1 zszedł z odziedziczonych 312 kHz na 156,2 kHz w oczekiwaniu na około 3 dB niższy próg szumu. Na sprzęcie wyszło odwrotnie: Heltec V4 przestał odbierać ramki S1 w ogóle, podczas gdy stojący obok SX1276 dalej dekodował ten sam nadajnik.

Carson nie opisuje dobrze sygnału kodowanego Manchesterem. Strumień chipów niesie realną energię poza nominalną dewiacją, a wartość z noty katalogowej to poziom -3 dB, a nie płaskie pasmo, więc użyteczne okno jest węższe niż sugeruje liczba. Kod zostaje przy 312 kHz dla S1, z komentarzem, żeby nie zwężać go ponownie bez pomiaru widma odebranego sygnału.

---

# Fix: the summary payload buffer belongs in .bss, not on the loop stack

## EN

### Fixed
- Growing the summary payload buffer to 3072 B put 3 kB on the loop task stack inside a call chain that then nests `maybe_publish_suggestion_()` (another 640 B buffer) and the logger, which writes through newlib and the VFS. On a LilyGO SX1276 node this crashed with `Fault - LoadProhibited` in `esp_vfs_write`, reached from an `ESP_LOGI` that only formats string literals - the signature of a stack overflow, not of a bad pointer. It fired immediately after the first 60 s summary, on the node whose suggestion path actually runs.
- The three summaries now share one static buffer. They are called only from `Radio::loop()`, one after another and never concurrently, so they cannot clobber each other. This costs 3 kB of `.bss` and takes 3 kB off the loop stack - less stack than the code used before the buffer was ever enlarged.

## PL

### Naprawiono
- Powiększenie bufora payloadu podsumowania do 3072 B umieściło 3 kB na stosie zadania pętli, w ścieżce, która zagnieżdża dalej `maybe_publish_suggestion_()` (kolejne 640 B) i logger, piszący przez newlib i VFS. Na węźle LilyGO z SX1276 kończyło się to crashem `Fault - LoadProhibited` w `esp_vfs_write`, osiągniętym z `ESP_LOGI`, które formatuje wyłącznie literały - to sygnatura przepełnienia stosu, nie złego wskaźnika. Występowało tuż po pierwszym podsumowaniu 60 s, na węźle, u którego ścieżka sugestii faktycznie się wykonuje.
- Trzy podsumowania korzystają teraz ze wspólnego bufora statycznego. Są wołane wyłącznie z `Radio::loop()`, jedno po drugim i nigdy równolegle, więc nie mogą sobie nadpisać danych. Kosztuje to 3 kB `.bss` i zdejmuje 3 kB ze stosu pętli - mniej stosu, niż kod zajmował przed powiększeniem bufora.

---

# Feature: S1 gets the same diagnostics as T1 and C1

## EN

### Added
- Summaries publish an `s1` block next to `t1` and `c1` - `total`, `ok`, `dropped`, `per_pct`, `crc_failed`, `crc_pct`, `avg_ok_rssi`, `avg_drop_rssi` - plus `manchester_drop` and `manchester_pct`. The per-mode counters are indexed by link mode and S1 frames were always written to them; nothing read them back, so S1 traffic was invisible in every summary except the global totals.
- Hints `S1_WEAK_SIGNAL`, `S1_INTERFERENCE_OR_RX` and `S1_OVERLOAD_OR_MULTIPATH`, mirroring the C1 branches, and `S1_MANCHESTER_ERRORS` when at least 20% of S1 frames die at the Manchester stage. S-mode is Manchester coded, so that stage is the analogue of T1's invalid 3-of-6 symbols.
- `dropped_by_stage` gains `s1_precheck`, `s1_manchester`, `s1_l_field` and `s1_length_check`. The parser has emitted these stage names all along, but `bucket_for_stage_()` did not recognise them, so every S1 failure was filed under `other`.
- `stage_rank_()` in the parser now lists the `s1_*` stages alongside their T1/C1 equivalents. This changes nothing today - `try_parse_s1_()` runs only on the forced-S1 path, which returns before the ranking is consulted, and an S-mode frame cannot reach a radio tuned for T1/C1 anyway. It is there so the ordering is right if an S1 result ever gets compared with another parse attempt.

### Fixed
- The summary payload buffer was 2048 B while the JSON needs roughly 2.2 kB once a long hint text is included, and more as counters grow. `snprintf` truncated it silently and published invalid JSON. The buffer is now 3072 B and a truncation logs a warning instead of shipping a broken payload. This could already happen before the `s1` block was added, with any of the longer C1/T1 hints.

## PL

### Dodano
- Podsumowania publikują blok `s1` obok `t1` i `c1` - `total`, `ok`, `dropped`, `per_pct`, `crc_failed`, `crc_pct`, `avg_ok_rssi`, `avg_drop_rssi` - oraz `manchester_drop` i `manchester_pct`. Liczniki per tryb są indeksowane trybem łącza i ramki S1 zawsze do nich trafiały; nikt ich nie odczytywał, więc ruch S1 był niewidoczny w podsumowaniach poza sumami globalnymi.
- Hinty `S1_WEAK_SIGNAL`, `S1_INTERFERENCE_OR_RX` i `S1_OVERLOAD_OR_MULTIPATH`, odwzorowujące gałęzie C1, oraz `S1_MANCHESTER_ERRORS`, gdy co najmniej 20% ramek S1 pada na etapie Manchester. S-mode jest kodowany Manchesterem, więc ten etap jest odpowiednikiem błędnych symboli 3-of-6 w T1.
- `dropped_by_stage` zyskuje `s1_precheck`, `s1_manchester`, `s1_l_field` i `s1_length_check`. Parser od dawna emitował te nazwy etapów, ale `bucket_for_stage_()` ich nie rozpoznawał, więc każda porażka S1 lądowała w `other`.

### Naprawiono
- Bufor payloadu podsumowania miał 2048 B, podczas gdy JSON potrzebuje około 2,2 kB przy dłuższym tekście hinta, a rośnie wraz z licznikami. `snprintf` ucinał go po cichu i publikował niepoprawny JSON. Bufor ma teraz 3072 B, a ucięcie loguje ostrzeżenie zamiast wysyłać zepsuty payload. Mogło się to zdarzać już wcześniej, przy którymkolwiek z dłuższych hintów C1/T1.

---

# Fix: a window where every frame failed CRC was reported as "looks good"

## EN

### Fixed
- `DIAG hint` starts as `OK / "looks good"` and each branch only overrides it. No branch covered a window where frames arrived but none decoded: the mode-specific branches key on the `c1_*` and `t1_*` counters, which stay at zero under `listen_mode: s1`, and the generic weak-signal branches need `avg_drop_rssi <= -90`. A real S1 window - `total=1 ok=0 dropped=1 crc_failed=1` at -87 dBm - therefore reported `OK | looks good`, and at INFO level, while an empty window is logged as a warning. A window that failed completely read better than a quiet one.
- New hint `ALL_DROPPED` covers `ok == 0`. It sits after the specific branches, so `C1_WEAK_SIGNAL`, `T1_BITFLIPS` and the rest keep priority and only the wrong default is replaced. Like every non-`OK` hint it is logged as a warning. It applies to all three summary windows (60 s, 15 min, 60 min).
- The `ADD_HIGHLIGHT_METERS` suggestion fired on frames *arriving* (`total > 0`), so the same window advised "Meters are being received. Check which IDs appear in wmbusmeters" although nothing decoded and no id was ever published - sending the user to look for something that does not exist. It now requires at least one decoded frame in the window.

### Notes
- The hint tree still has no counters of its own for S1; only `c1_*` and `t1_*` exist, so S1 windows fall back to generic diagnoses. That is a separate change.

## PL

### Naprawiono
- `DIAG hint` startuje jako `OK / "looks good"`, a każda gałąź tylko go nadpisuje. Żadna nie obejmowała okna, w którym ramki dotarły, ale żadna się nie zdekodowała: gałęzie dla konkretnych trybów opierają się na licznikach `c1_*` i `t1_*`, a te w `listen_mode: s1` pozostają zerowe; gałęzie ogólne o słabym sygnale wymagają `avg_drop_rssi <= -90`. Realne okno S1 - `total=1 ok=0 dropped=1 crc_failed=1` przy -87 dBm - raportowało więc `OK | looks good`, i to na poziomie INFO, podczas gdy puste okno logowane jest jako ostrzeżenie. Okno w całości nieudane wypadało lepiej niż ciche.
- Nowy hint `ALL_DROPPED` obejmuje `ok == 0`. Stoi za gałęziami szczegółowymi, więc `C1_WEAK_SIGNAL`, `T1_BITFLIPS` i pozostałe zachowują pierwszeństwo, a zastąpiona zostaje wyłącznie błędna wartość domyślna. Jak każdy hint inny niż `OK` jest logowany jako ostrzeżenie. Dotyczy wszystkich trzech okien podsumowania (60 s, 15 min, 60 min).
- Sugestia `ADD_HIGHLIGHT_METERS` wyzwalała się na samym *dotarciu* ramki (`total > 0`), więc to samo okno radziło "Liczniki są odbierane. Sprawdź w wmbusmeters jakie ID pojawiają się", choć nic się nie zdekodowało i żadne ID nigdy nie zostało opublikowane - wysyłając użytkownika na poszukiwanie czegoś, czego nie ma. Teraz wymaga co najmniej jednej zdekodowanej ramki w oknie.

### Uwagi
- Drzewo hintów nadal nie ma własnych liczników dla S1; istnieją tylko `c1_*` i `t1_*`, więc okna S1 dostają diagnozy ogólne. To osobna zmiana.

---

# Fix: XIAO with Wio-SX1262 received through a disabled antenna switch

## EN

### Fixed
- The Seeed Wio-SX1262 does not connect its antenna unconditionally. Module pin 1 (`RF_SW`, "External IO control internal gate RF switch") must be held high by the host; on the XIAO ESP32S3 kit it is GPIO38. Nothing drove it, so the pin idled as a high-impedance input and the receiver ran on leakage alone.
- This is separate from `dio2_rf_switch`, and both are needed. Per the module datasheet the SX1262's own DIO2 chooses the TX/RX *direction* (high = TX, low = RX); `RF_SW` decides whether the switch conducts at all.
- The XIAO examples did carry a workaround - an `on_boot` action toggling a `gpio` output on GPIO38 - and it never worked. Priority 900 lands in the same ESPHome setup bucket as the `gpio output` component itself, so ordering falls out of registration order and the write happened before the pin was an output. No warning, no error, no log line. It has been removed from both examples.
- Measured on hardware, same board and antenna before and after: meter `00089907` went from -96 dBm to -68 dBm, and the receiver went from 4-6 frames per minute across 3 meters to 14 across 32.

### Added
- `rf_sw_pin` for `SX1262`. The pin is driven high inside the radio's own setup, before the chip reset, where ordering is guaranteed. Boards whose module gates the antenna path this way need it; Heltec V3/V4/V4-R8 do not - they use the `fem_*` pins and are unaffected.
- The XIAO example joined the CI build matrix. That board had never been compiled in CI, and it is the only config exercising the new option.

### Notes
- **Action required on XIAO ESP32S3 + Wio-SX1262.** Add `rf_sw_pin: GPIO38` and delete any earlier `output:` / `on_boot:` block driving GPIO38 - leaving both makes ESPHome reject the config on a duplicate pin declaration. Without the option the receiver keeps running roughly 30 dB deaf.
- The symptom is not silence. Frames still decode, `DIAG hint` still reports `GOOD`; there are simply several times fewer of them and every RSSI sits in a narrow band just above the sensitivity floor.

## PL

### Naprawiono
- Moduł Seeed Wio-SX1262 nie podłącza anteny bezwarunkowo. Wyprowadzenie nr 1 modułu (`RF_SW`, "External IO control internal gate RF switch") musi być trzymane w stanie wysokim przez host; w zestawie z XIAO ESP32S3 jest to GPIO38. Nic go nie sterowało, więc pin pozostawał wejściem w stanie wysokiej impedancji, a odbiornik pracował wyłącznie na przecieku sygnału.
- To jest coś innego niż `dio2_rf_switch` i potrzebne są oba. Zgodnie z notą katalogową modułu DIO2 układu SX1262 wybiera *kierunek* TX/RX (wysoki = TX, niski = RX), natomiast `RF_SW` decyduje, czy przełącznik w ogóle przewodzi.
- Przykłady dla XIAO zawierały obejście - akcję `on_boot` przełączającą wyjście `gpio` na GPIO38 - i nigdy ono nie działało. Priorytet 900 trafia w tym samym etapie inicjalizacji ESPHome co sam komponent `gpio output`, więc o kolejności decyduje kolejność rejestracji i zapis wykonywał się, zanim wyprowadzenie stało się wyjściem. Bez ostrzeżenia, bez błędu, bez śladu w logu. Obejście zostało usunięte z obu przykładów.
- Pomiar na sprzęcie, ta sama płytka i antena przed i po: licznik `00089907` z -96 dBm na -68 dBm, a odbiornik z 4-6 ramek na minutę od 3 liczników na 14 od 32.

### Dodano
- `rf_sw_pin` dla `SX1262`. Wyprowadzenie ustawiane jest w stan wysoki wewnątrz inicjalizacji radia, przed resetem układu, gdzie kolejność jest jednoznaczna. Opcja jest potrzebna płytkom, których moduł bramkuje w ten sposób tor antenowy; Heltec V3/V4/V4-R8 jej nie wymagają - korzystają z wyprowadzeń `fem_*` i zmiana ich nie dotyczy.
- Przykład dla XIAO dołączył do macierzy kompilacji CI. Ta płytka nie była dotąd objęta kompilacją weryfikacyjną, a jest jedyną konfiguracją używającą nowej opcji.

### Uwagi
- **Wymagane działanie na XIAO ESP32S3 + Wio-SX1262.** Dodaj `rf_sw_pin: GPIO38` i usuń wcześniejszy blok `output:` / `on_boot:` sterujący GPIO38 - pozostawienie obu powoduje odrzucenie konfiguracji przez ESPHome z powodu podwójnej deklaracji wyprowadzenia. Bez tej opcji odbiornik nadal pracuje z czułością niższą o około 30 dB.
- Objawem nie jest cisza. Ramki nadal się dekodują, `DIAG hint` nadal raportuje `GOOD`; jest ich tylko kilka razy mniej, a każde RSSI leży w wąskim paśmie tuż nad progiem czułości.

---

# Fix: SX1262 reported the noise floor as every frame's RSSI

## EN

### Fixed
- Every received frame reported the same near-floor RSSI regardless of meter or distance, because the level was read after the transmission had already ended. Both receive paths fell back to an instantaneous `GetRssiInst` that ran after `RX_DONE` and after the whole buffer had been drained, so it measured the empty channel. The constant value was not a placeholder - it was a real measurement of the noise floor.
- In the streamed path (AN1200.53) that fallback was taken every time: the procedure deliberately never lets the packet engine finish a packet, so `GetPacketStatus` never latches values for the frame being captured.
- The driver also preferred the wrong register. `RssiAvg` averages over the whole receive window, which is a fixed 255 bytes, so for a 134-byte wM-Bus frame more than half of it is post-transmission noise. Measured on one frame: `RssiSync` -97 dBm against `RssiAvg` -117 dBm.
- RSSI is now sampled while the frame is still on air - a peak-hold taken as bytes arrive in the streamed path, `RssiSync` in the FIFO path. The post-frame instantaneous read is gone.
- Scaling was written as `-((int) raw) >> 1` instead of `-raw / 2`; unary minus binds tighter than the shift, so it arithmetic-shifted a negative value and floored instead of truncating.
- A frame whose level cannot be sampled now reports -127 dBm ("not measured") instead of a fabricated value, and such samples are excluded from the per-meter and diagnostic averages rather than dragging them down.

### Notes
- **Historical RSSI data is not comparable with new data** on any SX1262 board. Per-meter averages, `win_avg_rssi` and every RSSI-derived diagnostic need to be re-collected after the update.
- The first frame on each receive path logs the raw radio values at INFO, so the active source can be confirmed on hardware without raising the log level.

## PL

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

# Fix: meters with a non-BCD ID can be matched by forward_meters and highlight_meters

## EN

### Fixed
- Meter matching decoded the A-field as BCD and gave up otherwise, so meters that do not use a BCD ID (Diehl/IZAR among others) had no usable ID at all. They could never be listed in `highlight_meters`, and with `forward_meters` active their telegrams were dropped silently - the one case where the whitelist discarded frames the user could not get back by any configuration.
- Both options now also match the raw A-field value, written the way the log prints it: `id:417F0666` is configured as `"0x417F0666"` (quoted, or YAML turns it into a number). Decimal entries keep their existing meaning, so no configuration changes behaviour.
- The two forms are told apart without ambiguity: a non-BCD A-field always contains a nibble above 9 and therefore always prints a hex letter, while a BCD ID never does. The `0x` form works for BCD meters too (`"0x00089907"` is meter `89907`).
- Per-meter statistics were keyed on the BCD ID, so every non-BCD meter collapsed into a single shared entry at key 0. They are now keyed on the raw A-field value, which is unique for every meter.
- `target_meter_id` still accepts only BCD IDs. A hex value there used to be accepted and then never match; it now logs a warning at boot pointing to `forward_meters`.

## PL

### Naprawiono
- Dopasowanie liczników dekodowało A-field jako BCD i w przeciwnym razie rezygnowało, więc liczniki bez ID w BCD (m.in. Diehl/IZAR) nie miały żadnego użytecznego ID. Nie dało się ich wpisać do `highlight_meters`, a przy aktywnym `forward_meters` ich telegramy znikały bez śladu - jedyny przypadek, w którym whitelista odrzucała ramki, których użytkownik nie mógł odzyskać żadną konfiguracją.
- Obie opcje dopasowują teraz również surową wartość A-field, zapisywaną tak, jak pokazuje ją log: `id:417F0666` konfigurujesz jako `"0x417F0666"` (w cudzyslowie, inaczej YAML zamieni to na liczbe). Wpisy dziesiętne zachowują dotychczasowe znaczenie, więc żadna konfiguracja nie zmienia zachowania.
- Rozróżnienie obu form jest jednoznaczne: A-field poza BCD zawsze zawiera półbajtówkę powyżej 9, więc zawsze wypisuje literę szesnastkową, a ID w BCD nigdy. Forma `0x` działa też dla liczników BCD (`"0x00089907"` to licznik `89907`).
- Statystyki per licznik były kluczowane po ID z BCD, więc wszystkie liczniki nie-BCD zlewały się w jeden wspólny wpis pod kluczem 0. Teraz kluczem jest surowa wartość A-field, unikalna dla każdego licznika.
- `target_meter_id` nadal przyjmuje wyłącznie ID w BCD. Wartość szesnastkowa była tam dotąd przyjmowana i po cichu nigdy nie pasowała; teraz przy starcie pojawia się ostrzeżenie kierujące do `forward_meters`.

---

# Fix: NO_METERS_DETECTED no longer fires on a quiet summary window

## EN

### Fixed
- `NO_METERS_DETECTED` claimed a wiring or radio-configuration fault whenever a single summary window contained no frames. That counter (`diag_total_`) is reset after every window, so a receiver that had been working all day still reported `total == 0` for any quiet minute - and meters are routinely quiet at night. The suggestion is now limited to receivers that have not seen a single frame since boot, and additionally suppressed for the first 5 minutes of uptime, where silence carries no information because meters transmit tens of seconds to several minutes apart.
- A receiver that used to work and then went silent is a different diagnosis and stays with the health pulse (`sec_since_last_rx`); it was never what this suggestion measured.
- The `NO_DATA` summary hint said "no packets received yet", which reads as "nothing ever arrived" even though it describes a single window. It now says "no packets in this window". The machine-readable `hint_code` is unchanged.

## PL

### Naprawiono
- `NO_METERS_DETECTED` sugerowało usterkę okablowania lub konfiguracji radia za każdym razem, gdy pojedyncze okno podsumowania nie zawierało ramek. Ten licznik (`diag_total_`) jest zerowany po każdym oknie, więc odbiornik działający cały dzień i tak raportował `total == 0` w dowolnej cichej minucie - a liczniki nocą standardowo milczą. Sugestia ogranicza się teraz do odbiorników, które od startu nie odebrały ani jednej ramki, i dodatkowo jest wyciszona przez pierwsze 5 minut pracy, gdzie cisza nic nie znaczy, bo liczniki nadają co kilkadziesiąt sekund do kilku minut.
- Odbiornik, który działał i zamilkł, to inna diagnoza - pozostaje przy pulsie health (`sec_since_last_rx`); ta sugestia nigdy tego nie mierzyła.
- Hint `NO_DATA` w podsumowaniu mówił "no packets received yet", co brzmi jak "nic nigdy nie przyszło", choć opisuje pojedyncze okno. Teraz mówi "no packets in this window". Maszynowe `hint_code` bez zmian.

---

# Feature: forward_meters - forwarding whitelist for the RAW telegram topic

## EN

### Added
- `forward_meters` limits which meters are published to `wmbus/<topic_name>/telegram`. Useful where most of the received traffic belongs to neighbouring meters and only a few are your own.
- Accepts an explicit list of meter IDs, or `true` to reuse the IDs already listed in `highlight_meters` so the same list is not written twice.
- An empty list (the default) or `false` forwards every decoded frame, so existing configurations are unaffected.
- The parsed IDs are logged from `setup()` and again in the delayed boot status block, next to the YAML sanity output. The repetition is deliberate: `setup()` runs before the network logger attaches, so over `esphome logs` only the second one is visible. `dump_config()` reports the same state, but is filtered out at `logger: level: info` - it needs `level: config` or higher.

### Notes
- The filter runs after decoding and the DLL CRC check, so it matches a meter ID the parser has already validated. Filtering on the raw header would be cheaper but unreliable: an ID read from a frame that failed CRC can be corrupted.
- Matching uses the BCD-decoded 8-digit meter ID. A meter whose log line shows a hex ID (`id:417F0666`) has a non-BCD A-field and cannot be whitelisted - it is dropped while the filter is active.
- `forward_meters: true` with an empty `highlight_meters` does not silence the stream: filtering stays off and a configuration warning is printed at boot.
- Diagnostics are unaffected. Counters and RSSI statistics are updated before publishing, so summaries still cover the whole ether including neighbours; only the RAW stream is reduced.
- `target_meter_id` keeps its own topic and is deliberately not subject to the whitelist.

## PL

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

# Fix: guard gmtime()/strftime() in rtlwmbus timestamp

## EN

### Fixed
- `Frame::as_rtlwmbus()` now guards `std::gmtime()` returning `nullptr` and `std::strftime()` returning `0`, falling back to a fixed `1970-01-01 00:00:00.00Z` timestamp. Prevents a potential null-dereference / unterminated-buffer read when the system clock holds a `time_t` value that cannot be represented (e.g. an unset or out-of-range clock). Hardening only — no wire or format change during normal operation.

## PL

### Naprawiono
- `Frame::as_rtlwmbus()` zabezpiecza teraz przypadki, gdy `std::gmtime()` zwraca `nullptr`, a `std::strftime()` zwraca `0` — z fallbackiem do stałego znacznika `1970-01-01 00:00:00.00Z`. Zapobiega potencjalnemu null-dereference / odczytowi niedokończonego bufora, gdy zegar systemowy trzyma wartość `time_t` niemożliwą do reprezentacji (np. nieustawiony lub poza zakresem). Wyłącznie utwardzenie — bez zmiany formatu wyjścia w normalnej pracy.

---

# Current documentation note

## EN

### Added
- Experimental S1 receive mode.
- `listen_mode: s1` uses a dedicated S1 receive path and does not fall back to T1/C1 parsing.
- `listen_mode: both` remains T1/C1 only.
- `listen_mode: s1` defaults to `868.300 MHz`; T1/C1/both remain at `868.950 MHz`.
- Explicit `frequency:` in YAML still overrides the mode default.

### Notes
- S1 support is intended for diagnostics and compatibility testing.
- If a valid S1 telegram is received, it is published to MQTT like other validated wM-Bus telegrams.
- Meter-value decoding remains external and depends on the backend driver and encryption key.
- Proprietary or polling-based systems may not produce standard passive S1 telegrams.

## PL

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

**EN**

**Summary**
Improve adaptive SX1276 behavior, add MQTT diagnostic suggestions, and expand runtime diagnostics.

**Description**
This release improves the real-world behavior of the RAW-only wM-Bus bridge, especially on SX1276 in noisy RF environments.

Main changes:

* improved `sx1276_busy_ether_mode: adaptive` logic so activation reacts to actual reception loss, not just RF noise
* added MQTT `suggestion` events with actionable diagnostic hints and YAML snippets
* added `busy_ether_changed` MQTT events for adaptive state transitions
* expanded diagnostic summaries with new runtime fields, including `busy_ether_state`
* added/expanded `summary_15min`, `summary_60min`, and per-meter snapshot reporting
* fixed multiple logic and documentation inconsistencies discovered during real hardware testing
* fixed: `busy_ether_state` in `/summary` JSON now emits `"n/a"` on SX1262 instead of the misleading `"adaptive_passive"` (the algorithm never ran on SX1262, only the stored mode value was serialised)
* fixed: `hint_code` no longer stays `"OK"` for windows with 11-99% drop rate and no specific diagnosis — new code `MODERATE_DROPS` is emitted instead so elevated drops are always visible as WARN in serial log
* changed: `highlight_meters` per-packet serial log now shows `packet #N received` instead of the previous `stats / statystyki: count=N interval=... avg_rssi=...`; per-meter stats remain available via MQTT `meter_window` events

This version does not change the project architecture: the ESP device still focuses on RF reception and RAW MQTT publishing, while meter decoding remains external.

**PL**

**Summary**
Poprawa działania adaptive dla SX1276, dodanie sugestii diagnostycznych MQTT oraz rozbudowa diagnostyki runtime.

**Description**
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
