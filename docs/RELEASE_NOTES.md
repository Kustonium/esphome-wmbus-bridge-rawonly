# Feature: CC1101 hints at a marginal SPI connection when a write needs a retry

## EN

- When `apply_radio_profile_()` needs a retry to land a register, or a register never holds its value at all, the existing `CC1101 profile write-back` line now gets a follow-up `CC1101 hint` explaining what that number likely means.
- Two tiers: a register that never held its value even after retries points at a persistent fault - reseat every connection, check for a short between adjacent pins, try a shorter cable. A register that needed a retry but landed points at something intermittent - a loose header pin, a marginal short, a cable too long for the clock speed.
- **Why "likely" and not "confirmed":** `chip_not_ready_count_` already rules out `CHIP_RDYn` as the cause of a retry - if a write is answered with the chip ready and the value still does not read back, the bus corrupted a byte in transit. Issue #22 traced one real case of this to an intermittent short between two jumper wires: `reg_retry_count_` went from double digits to zero the moment the short was fixed, with nothing else about the setup changed. One correlated report is a lead, not a proof, which is why the hint says "likely" rather than naming a cause.
- This is CC1101-only. The write-verify-retry mechanism it reads from only exists on that driver.

## PL

- Kiedy `apply_radio_profile_()` potrzebuje powtórki, żeby zapisać rejestr, albo rejestr w ogóle nie utrzymuje wartości, istniejąca linia `CC1101 profile write-back` dostaje teraz kontynuację `CC1101 hint` tłumaczącą, co ta liczba prawdopodobnie znaczy.
- Dwa poziomy: rejestr, który nie utrzymał wartości mimo powtórek, wskazuje na trwałą usterkę - sprawdzić każde połączenie, szukać zwarcia między sąsiednimi pinami, spróbować krótszego kabla. Rejestr, który potrzebował powtórki, ale się zapisał, wskazuje na coś przejściowego - luźny pin, przejściowe zwarcie, kabel za długi jak na prędkość zegara.
- **Dlaczego „prawdopodobnie", nie „potwierdzone":** `chip_not_ready_count_` już wyklucza `CHIP_RDYn` jako przyczynę powtórki - jeśli zapis dostaje odpowiedź „układ gotowy", a wartość i tak się nie odczytuje z powrotem, to magistrala przekłamała bajt po drodze. Zgłoszenie #22 doprowadziło jeden realny taki przypadek do przejściowego zwarcia między dwoma przewodami: `reg_retry_count_` spadł z dwucyfrowej liczby do zera dokładnie w momencie naprawy zwarcia, bez żadnej innej zmiany w konfiguracji. Jedno skorelowane zgłoszenie to trop, nie dowód, dlatego hint mówi „prawdopodobnie", a nie wskazuje przyczynę wprost.
- To dotyczy tylko CC1101. Mechanizm zapisu-z-weryfikacją, z którego ten hint korzysta, istnieje tylko w tym sterowniku.


# Fix: FREQ and SYNC writes were not verified, and FREQ was the one that mattered

## EN

- The read-back verification added for the boot profile (`write_reg_verified_`) covered every register written from `apply_radio_profile_()` directly, but missed two callees that write registers of their own: `set_frequency_()` (`FREQ2/1/0`) and `set_sync_word_()` (`SYNC1/0`, both the normal and the S1 path).
- **Issue #22 caught it immediately.** With `spi_data_rate: 1MHz` in place, `FREQ1` landed but `FREQ2` and `FREQ0` stayed at their reset defaults - the radio was listening on 790.961 MHz instead of 868.950 MHz, with everything else in the profile now correct. The FREQ validation added earlier this same day is what made this visible instead of silent.
- All five registers now go through the same write-verify-retry path as the rest of the profile.

## PL

- Weryfikacja przez odczyt zwrotny dodana dla profilu startowego (`write_reg_verified_`) objęła każdy rejestr zapisywany bezpośrednio z `apply_radio_profile_()`, ale ominęła dwie wywoływane funkcje, które zapisują własne rejestry: `set_frequency_()` (`FREQ2/1/0`) i `set_sync_word_()` (`SYNC1/0`, zarówno ścieżka normalna, jak i S1).
- **Zgłoszenie #22 złapało to natychmiast.** Z `spi_data_rate: 1MHz` `FREQ1` doszedł, ale `FREQ2` i `FREQ0` zostały na wartościach domyślnych - radio słuchało na 790,961 MHz zamiast 868,950 MHz, przy reszcie profilu już poprawnej. Walidacja FREQ dodana tego samego dnia wcześniej jest tym, co sprawiło, że to było widoczne, a nie ciche.
- Wszystkie pięć rejestrów przechodzi teraz przez tę samą ścieżkę zapisz-zweryfikuj-powtórz co reszta profilu.


# Feature: `spi_data_rate`, and proof that the SPI bus was the problem

## EN

- **New option `spi_data_rate`** (all radios, default unchanged at 2 MHz). Sets the SPI clock for the radio device only, not for the whole bus. Range 100 kHz to 8 MHz.
- **Why it exists.** On the board in issue #22, `reg_write_retries=4` with `reg_write_failed=0`: every configuration register eventually took its value, but four of them needed a second attempt. That is a bus corrupting bytes, measured rather than guessed — and it happened on a healthy 3.3 V supply, with the module on jumper leads.
- **The failure is silent, which is the point.** A dropped bit in a register write leaves that register at its reset default. Nothing reports an error; the radio simply behaves as if it had been configured for a different data rate and deviation, and every frame fails its CRC three layers further down. Before the read-back verification landed, this was indistinguishable from a dead antenna.
- **Lower it before suspecting the part.** Raising it above 2 MHz is permitted but has no known benefit here — the RX FIFO drain is paced by the radio, not by the bus.
- **What it does not fix.** Register writes are repeated on mismatch, so they recover. The RX FIFO read cannot be repeated: a second read consumes bytes the first one already took. If the bus corrupts a byte there, the frame is lost and no amount of retrying will bring it back. That is the case for setting a clock the wiring can actually carry.

## PL

- **Nowa opcja `spi_data_rate`** (wszystkie radia, domyślnie bez zmian — 2 MHz). Ustawia zegar SPI **tego urządzenia**, nie całej magistrali. Zakres od 100 kHz do 8 MHz.
- **Skąd się wzięła.** Na płytce ze zgłoszenia #22 `reg_write_retries=4` przy `reg_write_failed=0`: każdy rejestr konfiguracji ostatecznie przyjął swoją wartość, ale cztery potrzebowały drugiej próby. To jest magistrala przekłamująca bajty — zmierzona, nie zgadnięta — i to na zdrowym zasilaniu 3,3 V, z modułem na przewodach.
- **Awaria jest cicha i o to właśnie chodzi.** Zgubiony bit w zapisie rejestru zostawia ten rejestr na wartości domyślnej. Nic nie zgłasza błędu; radio po prostu zachowuje się tak, jakby skonfigurowano je na inną prędkość i dewiację, a każda ramka pada na CRC trzy warstwy niżej. Zanim doszła weryfikacja przez odczyt zwrotny, było to nie do odróżnienia od martwej anteny.
- **Obniż go, zanim zaczniesz podejrzewać układ.** Podniesienie powyżej 2 MHz jest dozwolone, ale nie ma tu znanego zysku — opróżnianie RX FIFO dyktuje radio, nie magistrala.
- **Czego to nie naprawia.** Zapisy rejestrów są powtarzane przy niezgodności, więc się podnoszą. Odczytu RX FIFO powtórzyć się nie da: drugi odczyt zabiera bajty, które pierwszy już pobrał. Jeśli magistrala przekłamie bajt w tym miejscu, ramka przepada i żadne powtarzanie jej nie odzyska. To jest właśnie argument za ustawieniem zegara, który okablowanie faktycznie udźwignie.


# Fix: CC1101 lost register writes when the chip was not ready

## EN

- **`CHIP_RDYn` is now honoured.** The CC1101 answers every header byte with a status byte whose bit 7 is `CHIP_RDYn`; TI SWRS061I 10.1 requires it to be low before the first `SCLK` edge. The driver received that byte on every transaction and discarded it. Writes and strobes now inspect it and repeat the transaction when the chip reported itself not ready - up to 5 attempts, 200 us apart.
- **Reads are counted, never repeated.** A second read of the RX FIFO would consume bytes the first one already took. Single-register reads set the counter too, because a read taken before `CHIP_RDYn` goes low returns `0xFF` rather than the register.
- **The reset sequence follows the datasheet.** `reset_cc1101_()` now waits for `CHIP_RDYn` before issuing `SRES` and again afterwards, and the post-reset settle went from 5 ms to 10 ms, matching both known working CC1101 drivers. Previously the strobe went out immediately after `CSn` fell, which is only legal if the caller instead honours `tsp,pd` from Table 22 - and that 150 us figure was measured on a CC1101EM reference board with a specific crystal, not on an arbitrary module.
- **The `0x54CD` sync cycle stays, for now.** It was removed and then restored the same day. The case for removing it is real - mode T and mode C share the sync word `0x543D`, and the `0x54CD` that follows in a mode C telegram arrives as *data*, which is why `packet.cpp` strips it as `WMBUS_MODE_C_SUFIX_LEN`. But the only capture behind that reasoning came from a board whose RF profile does not apply correctly, so it says nothing about what a healthy receiver sees, and the sibling drivers cycle for a documented reason. Settling this needs a measurement from a working CC1101.
- **`FREQ2/1/0` is validated.** The carrier registers were written and never read back, so a frequency word that failed to land left the radio tuned elsewhere while the self-check reported everything fine. This is the only part of the profile that depends on user YAML, and a silently wrong carrier is indistinguishable from a dead antenna in every other log line.
- **Every register of the boot profile is now written, read back, and rewritten on mismatch** (3 attempts). The self-check already reported that the profile was wrong, but only at the end and only as a list of final values - it could not say which write failed, whether repeating it helped, or how often. That distinction is the whole diagnosis: a value that a repeat fixes means the transport is corrupting bytes, while a register that keeps reverting to its reset default means no amount of retrying will help and the fault is in the part or its supply. Mismatches are logged, never fatal: a few registers may legitimately read back differently (reserved or read-only bits), and refusing to boot over that would be worse than the problem being measured.
- **New fields `chip_not_ready`, `reg_write_failed` and `reg_write_retries` in the `CC1101 debug status` line**, counting transactions the chip answered with `CHIP_RDYn` still high. A non-zero value points at supply voltage, wiring length or SPI clock - not at the decoder.
- **Why this surfaced.** A user running a CC1101 module at 2.932 V found that roughly two thirds of the configuration registers never took, and that inserting a delay after every register write made the problem disappear. On a well-supplied module the chip happens to be ready in time and nobody notices the missing check; a slower crystal start-up exposes it. Reported by @lente-cz, whose logs are the only hardware evidence this driver has.

## PL

- **`CHIP_RDYn` jest wreszcie sprawdzane.** CC1101 odpowiada na każdy bajt nagłówka bajtem statusu, którego bit 7 to `CHIP_RDYn`; datasheet TI SWRS061I 10.1 wymaga, żeby był niski przed pierwszym zboczem `SCLK`. Sterownik dostawał ten bajt przy każdej transakcji i go wyrzucał. Zapisy i strobe'y teraz go oglądają i powtarzają transakcję, gdy układ zgłosił, że nie był gotowy - do 5 prób co 200 us.
- **Odczyty są liczone, nigdy nie ponawiane.** Powtórny odczyt RX FIFO zabrałby bajty, które pierwszy już pobrał. Odczyty pojedynczych rejestrów też podbijają licznik, bo odczyt sprzed opadnięcia `CHIP_RDYn` zwraca `0xFF`, a nie zawartość rejestru.
- **Sekwencja resetu jest zgodna z datasheetem.** `reset_cc1101_()` czeka na `CHIP_RDYn` przed `SRES` i jeszcze raz po nim, a odczekanie po resecie wzrosło z 5 ms do 10 ms, tak jak w obu znanych działających sterownikach CC1101. Wcześniej strobe leciał natychmiast po opadnięciu `CSn`, co jest dopuszczalne tylko wtedy, gdy zamiast tego dotrzyma się `tsp,pd` z Table 22 - a te 150 us zmierzono na referencyjnej płytce CC1101EM z konkretnym kwarcem, nie na dowolnym module.
- **Cykl sync `0x54CD` na razie zostaje.** Został usunięty i tego samego dnia przywrócony. Argument za usunięciem jest realny - tryb T i tryb C dzielą sync word `0x543D`, a następujące po nim `0x54CD` w telegramie trybu C przychodzi jako *dane*, dlatego `packet.cpp` odcina je jako `WMBUS_MODE_C_SUFIX_LEN`. Ale jedyny zrzut, na którym to rozumowanie stoi, pochodzi z płytki, której profil RF nie przykłada się poprawnie, więc nie mówi nic o tym, co widzi sprawny odbiornik, a siostrzane sterowniki cyklują z udokumentowanego powodu. Rozstrzygnie to pomiar z działającego CC1101, nie rozumowanie.
- **`FREQ2/1/0` jest weryfikowane.** Rejestry nośnej były zapisywane i nigdy nieodczytywane, więc słowo częstotliwości, które nie doszło, zostawiało radio nastrojone gdzie indziej, a autotest meldował, że wszystko gra. To jedyna część profilu zależna od YAML-a użytkownika, a po cichu zła nośna jest w każdej innej linii logu nie do odróżnienia od martwej anteny.
- **Każdy rejestr profilu startowego jest teraz zapisywany, odczytywany z powrotem i przepisywany przy niezgodności** (3 próby). Autotest i wcześniej meldował, że profil jest zły, ale dopiero na końcu i tylko jako lista wartości końcowych - nie potrafił powiedzieć, który zapis zawiódł, czy powtórzenie pomaga i jak często. A na tym rozróżnieniu stoi cała diagnoza: wartość, którą powtórzenie naprawia, znaczy, że transport przekłamuje bajty, a rejestr uparcie wracający do wartości domyślnej znaczy, że żadne powtarzanie nie pomoże i wina leży w układzie albo jego zasilaniu. Niezgodności są logowane, nigdy nie są krytyczne: kilka rejestrów może zgodnie z projektem odczytywać się inaczej, niż zostały zapisane (bity zarezerwowane albo tylko do odczytu), a odmowa startu z tego powodu byłaby gorsza niż mierzony problem.
- **Nowe pola `chip_not_ready`, `reg_write_failed` i `reg_write_retries` w linii `CC1101 debug status`**, liczące transakcje, na które układ odpowiedział z `CHIP_RDYn` wciąż wysokim. Wartość niezerowa wskazuje na zasilanie, długość okablowania albo zegar SPI - nie na dekoder.
- **Skąd to wyszło.** Użytkownik z modułem CC1101 zasilanym 2,932 V odkrył, że mniej więcej dwie trzecie rejestrów konfiguracji nigdy się nie zapisuje, a wstawienie opóźnienia po każdym zapisie problem usuwa. Na dobrze zasilonym module układ zdąża być gotowy i nikt braku sprawdzenia nie zauważa; wolniejszy start kwarcu go odsłania. Zgłoszone przez @lente-cz, którego logi są jedynym sprzętowym materiałem, jaki ten sterownik ma.


# Feature: measured noise floor, and an opt-in threshold based on it

## EN

- The diagnostic summary gains `noise_floor_dbm` and `noise_floor_n`: the ambient RSSI of the channel, sampled while the receiver sits armed and idle, plus how many samples stand behind it. **Always on** - no option needed.
- Sampled only where a full 5 s hop elapsed with no interrupt, so nothing was being received. Reported as the MINIMUM of a 16-sample ring, not a mean: a sample taken while a neighbouring meter transmits reads high, and averaging would let that drag the floor up.
- New options `use_noise_floor_threshold` (default `false`) and `noise_floor_margin_db` (default 6). When enabled, the weak-start abort threshold becomes `floor + margin` instead of `recent_ok_rssi_avg - 12`.
- **Why this matters.** The existing threshold is derived from an average over SUCCESSFUL receptions, so aborting weak frames raises the average, which raises the threshold, which aborts more - a feedback loop with no external reference. The noise floor has no such loop: it is what the channel does when we are not receiving.
- **And it is portable.** A board with a FEM reads roughly 10 dB hotter than the same chip without one - measured on the bench 2026-08-25, two SX1262 boards, medians -59 vs -68 dBm and minima -79 vs -89. An absolute clamp like `[-96, -86] dBm` therefore means something physically different on each board, while "N dB above the floor" means the same everywhere.
- **The threshold is off by default on purpose.** No measurement of a real noise floor existed when this was written, so any margin would have been a guess. The measurement ships enabled precisely so the margin can be chosen from numbers instead.

## PL

- Podsumowanie diagnostyczne zyskuje `noise_floor_dbm` i `noise_floor_n`: RSSI eteru mierzone, gdy odbiornik jest uzbrojony i bezczynny, oraz liczba próbek, które za tym stoją. **Zawsze włączone** — bez żadnej opcji.
- Próbkowane wyłącznie tam, gdzie minęło pełne 5 s bez przerwania, czyli nic nie było odbierane. Raportowane jako MINIMUM z pierścienia 16 próbek, nie średnia: próbka wzięta w trakcie cudzej transmisji jest wysoka, a średnia pozwoliłaby jej podciągnąć podłogę w górę.
- Nowe opcje `use_noise_floor_threshold` (domyślnie `false`) i `noise_floor_margin_db` (domyślnie 6). Po włączeniu próg przerywania słabych startów to `podłoga + margines` zamiast `recent_ok_rssi_avg - 12`.
- **Dlaczego to ma znaczenie.** Dotychczasowy próg liczy się ze średniej *udanych* odbiorów, więc przerywanie słabych ramek podnosi tę średnią, co podnosi próg, co przerywa jeszcze więcej — pętla sprzężenia zwrotnego bez zewnętrznego punktu odniesienia. Podłoga szumu takiej pętli nie ma: to jest to, co robi kanał, gdy nie odbieramy.
- **I jest przenośna.** Płytka z FEM czyta o jakieś 10 dB „goręcej" niż ten sam chip bez niego — zmierzone na stanowisku 25.08.2026, dwie płytki SX1262, mediany −59 vs −68 dBm i minima −79 vs −89. Klamra w bezwzględnych dBm, jak `[-96, -86]`, znaczy więc na każdej płytce co innego fizycznie, a „N dB nad podłogą" znaczy wszędzie to samo.
- **Próg jest domyślnie wyłączony celowo.** W chwili pisania nie istniał żaden pomiar realnej podłogi szumu, więc każdy margines byłby zgadywaniem. Pomiar wchodzi włączony właśnie po to, żeby margines dało się wybrać z liczb.


# Feature: `/diag/config` retained configuration snapshot

## EN

- One JSON payload published retained on `wmbus/<topic_name>/diag/config` after the first MQTT connect, refreshed once per boot.
- Shape: `{"radio":"SX1276","lines":["  listen_mode: t1 (CHANGED, default: c1)", ...]}` - the exact same lines the boot log prints, marker included, so a reader can compare panel and log without translation.
- Marker vocabulary: `(default)`, `(CHANGED, default: X)`, `(set)` for fields without a default, `(required)`, and `(mode default: X)` for `frequency` when unset. The add-on's diagnostics panel parses the trailing marker for the badge color and prints the rest verbatim.
- Retained so a reader opening the panel long after boot still sees the configuration the board came up with. Not chunked - the whole snapshot fits in one MQTT publish for the current schema.
- Why: the boot log already carried every effective setting with default/changed markers, but only over the serial or `esphome logs` transport. Publishing the same text makes it visible from the add-on without asking for the YAML.

## PL

- Jeden JSON publikowany z `retain=true` na `wmbus/<topic_name>/diag/config` po pierwszym połączeniu z brokerem, odświeżany raz na boot.
- Kształt: `{"radio":"SX1276","lines":["  listen_mode: t1 (CHANGED, default: c1)", ...]}` — dokładnie te same linie, które płytka drukuje w boot logu, razem z markerem, więc czytelnik może porównać panel i log bez tłumaczenia.
- Słownik markerów: `(default)`, `(CHANGED, default: X)`, `(set)` dla pól bez domyślnej wartości, `(required)` i `(mode default: X)` dla `frequency`, gdy nie ustawione. Panel diagnostyczny dodatku parsuje końcowy marker na kolor odznaki i drukuje resztę dosłownie.
- Retained, żeby czytelnik otwierający panel długo po starcie widział tę konfigurację, z którą płytka wystartowała. Bez chunkowania — cały snapshot mieści się w jednym publish przy aktualnym schemacie.
- Po co: boot log od dawna niósł każde efektywne ustawienie z markerem default/CHANGED, ale tylko po transporcie szeregowym lub `esphome logs`. Publikacja tego samego tekstu robi to widocznym z dodatku bez pytania o YAML.


# Change: `sx1276_busy_ether_mode` now defaults to `normal`

## EN

- **The default changes from `adaptive` to `normal`.** `adaptive` and `aggressive` do not tune the receiver: they abort weak starts so the radio can keep up when it is overrunning. If it is not overrunning, that sensitivity is spent for nothing.
- Measured on a dense apartment block, 2026-08-23, with all four boards **in one physical spot**: under `adaptive` **no frame weaker than −84 dBm got through at all** and the board heard **27** meters; under `normal` frames arrived down to **−97 dBm** and it heard **53**. Normalised against the three control boards in the same window, their meter counts did not move (44→45, 33→33, 27→24) while the SX1276 went 27→53.
- The mechanism matches the code: the abort threshold in `should_abort_t1_probe_start_()` is clamped to `[-96, -86]` and `adaptive` adds +4 dB, which pins it to the clamp. The result is a hard sensitivity floor, not a gradual trade.
- Throughout that day `fifo_overrun`, `truncated`, `payload_read_failed` and `irq_timeout` were **zero** - the receiver was never overrunning, so the protection was buying nothing.
- **New diagnostic suggestion `CONSIDER_BUSY_ETHER_ADAPTIVE`**: the component now proposes `adaptive` only when overload is *measured* (`fifo_overrun > 0` or `truncated > 0`) together with real losses (`drop_pct >= 10`). A high `false_start_like` alone is explicitly not a reason - it counts noise triggers, and it sat near 60/min all day with zero overruns.
- `CHIP_SELECTION{,_PL}.md` and `TROUBLESHOOTING{,_PL}.md` §8 rewritten around that rule, including a warning that `drop_pct` improves by itself under `adaptive` because the frames it would have counted are no longer attempted. Judge the mode by per-meter counts instead.
- SX1276 examples no longer set `adaptive` actively; the commented ones annotate `# default: normal`.
- **Caveat, stated in the docs as well:** one board, one building, one evening. The mechanism is understood; the size of the effect elsewhere is not.

## PL

- **Domyślna zmienia się z `adaptive` na `normal`.** `adaptive` i `aggressive` nie stroją odbiornika: przerywają słabe starty, żeby radio nadążyło, gdy się przeciąża. Jeśli się nie przeciąża, ta czułość jest wydawana na nic.
- Zmierzone w gęstej zabudowie 2026-08-23, przy czterech płytkach **w jednym punkcie**: przy `adaptive` **żadna ramka słabsza niż −84 dBm nie przeszła w ogóle**, a płytka słyszała **27** liczników; przy `normal` ramki docierały do **−97 dBm**, a liczników było **53**. Po znormalizowaniu wobec trzech płytek kontrolnych z tego samego okna: u nich liczba liczników nie drgnęła (44→45, 33→33, 27→24), u SX1276 wzrosła 27→53.
- Mechanizm zgadza się z kodem: próg w `should_abort_t1_probe_start_()` jest zaciśnięty klamrą do `[-96, -86]`, a `adaptive` dokłada +4 dB, co dopycha go do tej klamry. Efektem jest twarda podłoga czułości, a nie płynny kompromis.
- Przez cały ten dzień `fifo_overrun`, `truncated`, `payload_read_failed` i `irq_timeout` wynosiły **zero** — odbiornik się nie przeciążał, więc ochrona nie kupowała niczego.
- **Nowa sugestia diagnostyczna `CONSIDER_BUSY_ETHER_ADAPTIVE`**: komponent proponuje `adaptive` dopiero przy *zmierzonym* przeciążeniu (`fifo_overrun > 0` albo `truncated > 0`) razem z realnymi stratami (`drop_pct >= 10`). Sam wysoki `false_start_like` jawnie nie jest powodem — liczy wyzwolenia na szumie i przez cały dzień wynosił około 60/min przy zerowych przeciążeniach.
- `CHIP_SELECTION{,_PL}.md` oraz `TROUBLESHOOTING{,_PL}.md` §8 przepisane wokół tej zasady, razem z ostrzeżeniem, że `drop_pct` poprawia się sam przy `adaptive`, bo ramki, które policzyłby jako odrzucone, nie są już próbowane. Tryb oceniać po liczbach per licznik.
- Przykłady SX1276 nie ustawiają już `adaptive` aktywnie; w wersjach z komentarzami adnotacja to `# default: normal`.
- **Zastrzeżenie, powtórzone też w dokumentacji:** jedna płytka, jeden budynek, jeden wieczór. Mechanizm jest zrozumiały, skala efektu gdzie indziej — nie.

# Feature: `/rx` metadata carries the reception time

## EN

- The `wmbus/<topic_name>/rx` payload gains `received_at`, an ISO-8601 UTC stamp with milliseconds.
- It marks when the frame was **received**, not when it was published. The frame is captured in the receiver task and reaches MQTT later, so the value is computed backwards from the monotonic `rx_task_wakeup_us`. Publish time would relabel the frame - the exact failure a timestamp exists to prevent, and the one that matters most to anyone buffering frames while the broker is unreachable.
- **Absent, not null, when the clock is unset.** After a restart the radio receives normally for as long as SNTP takes to answer; a frame from that window must not carry 1970 or an uptime pretending to be a date. A reader that never sees the key cannot mistake a placeholder for a measurement.
- No schema bump: the field is additive and optional, so a consumer written against schema 1 is unaffected.
- Asked for on the forum, alongside a durable store-and-forward buffer. The timestamp is the half that is cheap and unambiguous; buffering is not, because a flash write lands on the timing-sensitive receive path.

## PL

- Payload `wmbus/<topic_name>/rx` zyskuje `received_at`, znacznik ISO-8601 UTC z milisekundami.
- Opisuje moment **odbioru** ramki, a nie jej publikacji. Ramka jest przechwytywana w zadaniu odbiorczym i trafia na MQTT później, więc wartość liczona jest wstecz z monotonicznego `rx_task_wakeup_us`. Czas publikacji przekłamywałby ramkę — czyli robiłby dokładnie to, czemu znacznik ma zapobiegać, i co najbardziej boli każdego, kto buforuje ramki przy niedostępnym brokerze.
- **Nieobecne, a nie puste, gdy zegar nie jest ustawiony.** Po restarcie radio odbiera normalnie tak długo, jak SNTP potrzebuje na odpowiedź; ramka z tego okna nie może nieść 1970 ani czasu pracy udającego datę. Odbiorca, który nigdy nie zobaczy klucza, nie pomyli zastępnika z pomiarem.
- Bez podbicia schematu: pole jest dodatkowe i opcjonalne, więc konsument pisany pod schemat 1 nie odczuwa zmiany.
- Poproszone na forum, razem z trwałym buforem store-and-forward. Znacznik czasu to ta połowa, która jest tania i jednoznaczna; buforowanie nie jest, bo zapis do flasha ląduje na wrażliwej czasowo ścieżce odbiorczej.

# Feature: the boot log now states the whole configuration, and says what you changed

## EN

- Every radio now logs a **configuration report** at boot: each effective option on its own line, grouped into `[core] [pins] [<radio>] [output] [diagnostics]`, and marked `(default)`, `(CHANGED, default: X)`, `(set)` or `(required)`. Previously the log carried a handful of hand-picked checks, so any option outside that list was invisible and a misconfigured board still looked healthy.
- The report is generated at **compile time from the schema**, not restated in the driver, so a default in the log can never drift away from the default the component uses. Only options that apply to the selected radio are printed.
- **`rf_sw_pin` is now reported for SX1262.** Its absence is the same class of silent failure as `has_tcxo: false`: the radio initializes, the log looks healthy, and a XIAO ESP32-S3 + Wio-SX1262 runs roughly 30 dB deaf because the module never opens its antenna path. It is stated in both directions, so "not configured" is a positive statement rather than a missing line.
- **CC1101 had no sanity block at all** and now has one: the experimental gate plus `gdo0_pin`/`gdo2_pin`, so dual-IRQ wiring is confirmed instead of inferred.
- Coverage before this change was uneven — SX1276 logged one check, SX1262 four, LR1121 six, CC1101 none.
- Documented in `DIAGNOSTIC{,_PL}.md` with the marker table and the per-radio list.

## PL

- Każde radio wypisuje teraz przy starcie **raport konfiguracji**: każda efektywna opcja w osobnej linii, pogrupowane w `[core] [pins] [<radio>] [output] [diagnostics]`, z oznaczeniem `(default)`, `(CHANGED, default: X)`, `(set)` albo `(required)`. Wcześniej log niósł kilka ręcznie wybranych kontroli, więc cokolwiek poza tą listą było niewidoczne, a źle skonfigurowana płytka i tak wyglądała zdrowo.
- Raport powstaje **przy kompilacji, ze schematu**, a nie jest powtarzany w sterowniku — więc domyślna w logu nie może rozjechać się z domyślną, której komponent faktycznie używa. Wypisywane są wyłącznie opcje dotyczące wybranego radia.
- **`rf_sw_pin` jest teraz raportowany dla SX1262.** Jego brak to ta sama klasa cichej usterki co `has_tcxo: false`: radio się inicjalizuje, log wygląda zdrowo, a XIAO ESP32-S3 + Wio-SX1262 pracuje z czułością niższą o jakieś 30 dB, bo moduł nigdy nie otwiera toru antenowego. Stan jest podawany w obie strony, więc „nie skonfigurowany" jest stwierdzeniem, a nie brakującą linią.
- **CC1101 nie miał bloku sanity w ogóle** i teraz go ma: bramka eksperymentalna oraz `gdo0_pin`/`gdo2_pin`, żeby okablowanie dwóch przerwań było potwierdzone, a nie domniemane.
- Pokrycie przed tą zmianą było nierówne — SX1276 logował jedną kontrolę, SX1262 cztery, LR1121 sześć, CC1101 ani jednej.
- Opisane w `DIAGNOSTIC{,_PL}.md` razem z tabelą znaczników i listą per radio.

# Docs: SX1262 examples with a TCXO now clear the device-error register

## EN

- All eight `SX1262` examples (Heltec V3, V4, V4-R8, XIAO) set `clear_device_errors_on_boot: true` and `publish_dev_err_after_clear: true` instead of listing them as optional extras. Every one of those boards declares `has_tcxo: true`, and on a TCXO board `XOSC_START_ERR` is set on every power-up as a matter of course: the chip tries its own crystal before DIO3 has been told to power the TCXO, and DIO3 is configured after reset.
- Left off, the flag is therefore always set and carries no information. Cleared once the reference is up - which is what the datasheet expects - it becomes a diagnostic: a flag that stays cleared was a power-on artefact, a flag that comes back after a clean clear is a reference that genuinely is not starting.
- `publish_dev_err_after_clear` sends the re-read state to MQTT. That is the only way to see it on a node receiving nothing, which is precisely the case where the error register is the last thing left to read - the situation the 2026-08-01 fix was about.
- Defaults in the schema are unchanged (`false` for both). This is a change to what the examples recommend, not to component behaviour.

## PL

- Wszystkie osiem przykładów `SX1262` (Heltec V3, V4, V4-R8, XIAO) ustawia `clear_device_errors_on_boot: true` oraz `publish_dev_err_after_clear: true`, zamiast wymieniać je jako opcjonalny dodatek. Każda z tych płytek deklaruje `has_tcxo: true`, a na płytce z TCXO `XOSC_START_ERR` zapala się przy każdym starcie w sposób normalny: układ próbuje uruchomić własny kwarc, zanim DIO3 dostanie polecenie zasilenia TCXO, a DIO3 konfiguruje się dopiero po resecie.
- Bez skasowania flaga jest więc zawsze zapalona i nie niesie żadnej informacji. Skasowana po ustawieniu referencji - czego oczekuje datasheet - staje się diagnostyką: flaga, która zostaje skasowana, była artefaktem startu; flaga, która wraca po czystym skasowaniu, to referencja, która naprawdę nie startuje.
- `publish_dev_err_after_clear` wysyła ponownie odczytany stan na MQTT. To jedyny sposób, żeby zobaczyć go na węźle, który nic nie odbiera - a właśnie tam rejestr błędów jest ostatnią rzeczą do odczytania; dokładnie o tym była poprawka z 2026-08-01.
- Domyślne w schemacie bez zmian (`false` dla obu). To zmiana tego, co zalecają przykłady, a nie zachowania komponentu.

# Fix: examples no longer reboot a standalone MQTT receiver every 15 minutes

## EN

- Every example YAML that pairs `mqtt:` with `api:` now sets `api.reboot_timeout: 0s`, with a comment explaining why. ESPHome's default is `15min`, and that timer restarts the board whenever no Native API *client* is connected - which is the normal state of a receiver that only publishes to MQTT.
- Measured, not deduced: the `/rx` metadata for the night of 2026-08-20/21 showed **51 distinct `boot_id` per board, median 900 s apart**, while Home Assistant had no ESPHome device added. After the change the same boards ran **14.1 h on one `boot_id`** with `seq` rising continuously.
- `api:` is kept, so Native API and `time: platform: homeassistant` still work; only the watchdog is off. `mqtt.reboot_timeout` is a separate mechanism for broker loss and is deliberately left alone.
- `TROUBLESHOOTING{,_PL}.md` gained a section for the symptom, including how to confirm it from `boot_id` and `seq` instead of guessing from telegram counts.

## PL

- Każdy przykład YAML łączący `mqtt:` z `api:` ustawia teraz `api.reboot_timeout: 0s` wraz z komentarzem wyjaśniającym powód. Domyślną wartością ESPHome jest `15min`, a ten licznik restartuje płytkę zawsze, gdy nie jest podłączony żaden *klient* Native API - czyli w normalnym stanie odbiornika publikującego wyłącznie do MQTT.
- Zmierzone, nie wywnioskowane: metadane `/rx` z nocy 2026-08-20/21 pokazały **51 różnych `boot_id` na płytkę, mediana odstępu 900 s**, przy braku jakiegokolwiek urządzenia ESPHome dodanego w Home Assistant. Po zmianie te same płytki przepracowały **14,1 h na jednym `boot_id`**, a `seq` rósł bez przerwy.
- `api:` zostaje, więc Native API i `time: platform: homeassistant` nadal działają; wyłączony jest tylko watchdog. `mqtt.reboot_timeout` to osobny mechanizm na utratę brokera i celowo nie jest ruszany.
- `TROUBLESHOOTING{,_PL}.md` dostały sekcję o tym objawie, razem ze sposobem potwierdzenia go przez `boot_id` i `seq`, zamiast zgadywania z liczby telegramów.

# Docs: the fourth radio, and the S1 answer, are now in the documentation

## EN

- `CHIP_SELECTION.md` covers all four supported radios (`CC1101`, `SX1276`, `SX1262`, `LR1121`) instead of two, and gained an **S1 section**: `SX1262` decodes S1 to about -82 dBm and fails at -85, while `SX1276` decoded the same real transmission in the same second at -99/-100 dBm. The practical rule — `SX1276` for S1, `SX1262` for T1 — was measured on 2026-08-01 and 2026-08-14 but had never reached the document people read before buying a board.
- `LR1121` is now present where a reader looks for it: `README.md`, `START_HERE`, `RADIO_OPTIONS_MINIMAL.md`, `README_FULL`, `TROUBLESHOOTING` (a new section for the three failures that look like dead hardware), and the `busy_ether_state: n/a` lists in the diagnostics docs. `radio_type` in `CONFIG_REFERENCE_MINIMAL.md` lists it as a valid value.
- The LR1121 example README states the measured weakest decode as **-114 dBm**, read from the `/api/esp-rx` export over a 14.1 h run on 2026-08-21, replacing the earlier -100 dBm. The same run produced 1401 frames at -105 dBm or below. Its "S1 untested" caveat is gone, because S1 was verified on 2026-08-19.
- `BENCHMARKS.md` states explicitly that `CC1101` and `LR1121` are **not** benchmarked: that comparison depends on both radios standing in the same place, and no such run exists for them.
- `RX_PIPELINE_PL.md` documents the companion `/rx` topic, which only the English version had.
- `diagnostic_publish_suggestion` was the one schema option with no entry in the reference; it has one now.
- Both `CHIP_SELECTION` files now say what the numbers cannot support: RSSI is not comparable between boards (an external LNA/FEM reads 13-15 dB higher), and frame counts only compare boards standing in the same position.

## PL

- `CHIP_SELECTION_PL.md` obejmuje wszystkie cztery obsługiwane radia (`CC1101`, `SX1276`, `SX1262`, `LR1121`) zamiast dwóch i zyskał **sekcję o S1**: `SX1262` dekoduje S1 mniej więcej do -82 dBm i zawodzi przy -85, a `SX1276` zdekodował tę samą rzeczywistą emisję w tej samej sekundzie przy -99/-100 dBm. Zasada praktyczna — `SX1276` do S1, `SX1262` do T1 — była zmierzona 2026-08-01 i 2026-08-14, ale nigdy nie trafiła do dokumentu czytanego przed zakupem płytki.
- `LR1121` jest teraz tam, gdzie czytelnik go szuka: `README.md`, `START_HERE_PL`, `RADIO_OPTIONS_MINIMAL.md`, `README_FULL_PL`, `TROUBLESHOOTING_PL` (nowa sekcja o trzech usterkach, które wyglądają jak martwy sprzęt) oraz listy `busy_ether_state: n/a` w dokumentach diagnostycznych. `radio_type` w `CONFIG_REFERENCE_MINIMAL.md` wymienia go jako dopuszczalną wartość.
- README przykładu LR1121 podaje zmierzony najsłabszy odbiór **-114 dBm**, odczytany z eksportu `/api/esp-rx` w biegu 14,1 h dnia 2026-08-21, w miejsce wcześniejszych -100 dBm. Ten sam bieg dał 1401 ramek na poziomie -105 dBm i niżej. Zastrzeżenie „S1 niesprawdzone" znika, bo S1 zostało sprawdzone 2026-08-19.
- `BENCHMARKS_PL.md` mówi wprost, że `CC1101` i `LR1121` **nie** są tam zmierzone: tamto porównanie opiera się na tym, że oba radia stoją w tym samym miejscu, a dla tych dwóch takiego biegu nie ma.
- `RX_PIPELINE_PL.md` opisuje towarzyszący temat `/rx`, który miała dotąd wyłącznie wersja angielska.
- `diagnostic_publish_suggestion` był jedyną opcją schematu bez wpisu w referencji; teraz go ma.
- Oba pliki `CHIP_SELECTION` mówią też, czego liczby nie udźwigną: RSSI nie jest porównywalne między płytkami (zewnętrzny LNA/FEM czyta 13-15 dB wyżej), a liczby ramek porównują tylko płytki stojące w tym samym miejscu.

# Feature: opt-in per-meter RSSI, and examples that state their defaults

## EN

- New option `publish_rssi` (**default `false`**). With it on, the board publishes the level of each forwarded meter's last frame as a retained integer to `wmbus/<topic_name>/rssi/<meter_id>`. Nothing changes for anyone who leaves it off — the topic simply never appears.
- Only frames with a real measurement are published. A frame the radio gave no level for is skipped rather than sent as a sentinel, so a consumer never has to guess whether `0`, `1` or `-127` means "no signal" or "no reading".
- The value is the one the driver already latched for that frame (SX1276 at the first byte, SX1262/LR1121 at sync word, CC1101 on read). Nothing new is measured, and the level is not re-read after RX_DONE, which would report an empty channel.
- `forward_meters` applies as it does to telegrams: a meter that is filtered out publishes no RSSI either.
- Independent of `diagnostic_mode`. The `last_rssi` / `win_avg_rssi` fields inside the diagnostic payloads are unchanged and remain the tool for reading the board's RF picture.
- Paired with the wMBus MQTT Bridge add-on, each receiving board produces its own signal-strength entity for the same meter, which is what makes two boards comparable.
- All `*_commented.yaml` examples now annotate every optional setting with `# default: <value>`, and `tests/ci/check_example_defaults.py` (wired into CI) holds those annotations and the `Domyślnie` column of `CONFIG_REFERENCE_MINIMAL.md` to the schema. A default changed in `__init__.py` now fails the build instead of quietly outdating ten files.
- `CONFIG_REFERENCE_MINIMAL.md` gained the sixteen options that had a schema default but no entry, plus a section on what per-meter RSSI does and does not tell you.

## PL

- Nowa opcja `publish_rssi` (**domyślnie `false`**). Po włączeniu płytka publikuje poziom ostatniej ramki każdego przekazywanego licznika jako zachowaną liczbę całkowitą na `wmbus/<topic_name>/rssi/<meter_id>`. Kto jej nie włączy, nie zobaczy żadnej zmiany — temat po prostu nie powstaje.
- Publikowane są wyłącznie ramki z rzeczywistym pomiarem. Ramka, dla której radio nie oddało poziomu, jest pomijana zamiast wysyłana jako znacznik, więc odbiorca nie musi zgadywać, czy `0`, `1` albo `-127` znaczy „brak sygnału", czy „brak odczytu".
- Wartość to ta, którą sterownik zatrzasnął dla tej ramki (SX1276 przy pierwszym bajcie, SX1262/LR1121 na sync-word, CC1101 przy odczycie). Nic nie jest liczone od nowa i poziom nie jest doczytywany po RX_DONE, bo wtedy mierzyłby pusty kanał.
- `forward_meters` obowiązuje tak samo jak dla telegramów: odfiltrowany licznik nie ma też publikowanego RSSI.
- Niezależne od `diagnostic_mode`. Pola `last_rssi` / `win_avg_rssi` w payloadach diagnostycznych zostają bez zmian i dalej służą do czytania obrazu RF płytki.
- W parze z dodatkiem wMBus MQTT Bridge każda płytka odbiorcza daje własną encję siły sygnału dla tego samego licznika — i to jest sens tej opcji.
- Wszystkie przykłady `*_commented.yaml` mają teraz przy każdej opcjonalnej pozycji adnotację `# default: <wartość>`, a `tests/ci/check_example_defaults.py` (wpięty w CI) pilnuje zgodności tych adnotacji oraz kolumny `Domyślnie` w `CONFIG_REFERENCE_MINIMAL.md` ze schematem. Zmiana defaultu w `__init__.py` wywala build zamiast po cichu unieważniać dziesięć plików.
- `CONFIG_REFERENCE_MINIMAL.md` dostał szesnaście opcji, które miały default w schemacie, a nie miały wpisu, oraz sekcję o tym, co RSSI per licznik mówi, a czego nie.

# Fix: recover marginal S1 frames from Manchester erasures

## EN

- S1 now retains invalid Manchester-pair positions. When ordinary Format-A CRC validation fails, it tries both bit values independently per CRC block and accepts only a unique CRC-valid assignment.
- The search is capped at eight erasures per block (256 assignments). Larger, unsolved, or ambiguous blocks remain `dll_crc_failed`; T1 and C1 are unchanged.
- Measured on the two real 85-byte captures from 2026-08-14: maps `[3,1,0,3,0,0]` and `[2,1,0,2,2,3]` were restored byte-for-byte to the transmitted frame in 16 and 12 CRC trials.
- Host regressions include multi-block recovery, rejection above the cap, and both real RAW captures.

## PL

- S1 zachowuje teraz pozycje niepoprawnych par Manchester. Gdy zwykła walidacja CRC formatu A zawiedzie, sprawdza obie wartości bitu niezależnie w każdym bloku CRC i przyjmuje wyłącznie jednoznaczne rozwiązanie zgodne z CRC.
- Wyszukiwanie ma limit ośmiu erasure na blok (256 podstawień). Większe, nierozwiązywalne albo niejednoznaczne bloki pozostają `dll_crc_failed`; T1 i C1 są bez zmian.
- Pomiar na dwóch rzeczywistych 85-bajtowych przechwyceniach z 2026-08-14: mapy `[3,1,0,3,0,0]` i `[2,1,0,2,2,3]` zostały odtworzone bajt w bajt do nadanej ramki odpowiednio w 16 i 12 próbach CRC.
- Regresje hosta obejmują korekcję przez wiele bloków, odrzucenie powyżej limitu i oba rzeczywiste RAW.

# Diagnostics: how the invalid pairs of an S1 frame spread over its CRC blocks

## EN

### Added
- Under `diagnostic_verbose`, every S1 frame candidate the header search reports now also gets an erasure map: `S1 erasure map 1: 6 erasures in 776/776 frame pairs, per CRC block [1,1,1,1,1,1], worst block 1 (2^1 tries)`.

### Why
- An invalid Manchester pair (`00`/`11`) is a known error *position*, not an unknown bit. The decoder substitutes a zero and moves on, throwing that information away. Resolving one such position against the CRC means trying both values, and format A checks each block on its own — so the cost for a frame is 2^(worst block), never 2^(total). Six erasures spread one per block are six two-try problems; the same six inside one block are 64 tries. Which shape real receptions have is unknown, and the total that was already being logged cannot tell them apart. This measures it before anything is decided about the decoder.
- Counted over the frame window only. `symbols_invalid` on the decode path is counted over the whole capture, so at the 512-byte cap it mostly describes the noise trailing the frame: 246 bytes of noise contribute roughly 492 invalid pairs by themselves.

### Notes
- Printed for every reported candidate, not for the top one alone. Candidates are ranked by invalid pairs per checked pair, so a coincidence over a short implied frame can outrank a real header; the erasure map is what separates them.
- Verbose-only, and only on the SX1262 raw-capture path, which is a search over up to 512 chip offsets already. Nothing on a normal receive path changed.
- The block walk was cross-checked against `s1_raw_len_from_l_` for every L-field value, and the bucketing exercised on synthetic Manchester captures with erasures at known byte positions: block boundaries, six clustered in one block, a frame starting away from chip 0, and a capture ending mid-frame.

### Not verified
- Nothing about decoding changed, and nothing here says erasure resolution would recover a frame. That is the open question this log line exists to answer. The expectation that it could be worth a few dB is arithmetic over substitution counts, not a measurement, and it collapses entirely if frames turn out to arrive either clean or with dozens of erasures and nothing in between.
- Not yet run against a live capture.

## PL

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

# Fix: a one-tick notify wait could expire before it had waited

## EN

### Fixed
- While reading a frame, the receive path waited `pdMS_TO_TICKS(1)` for the next byte before concluding the radio had nothing more to give. A FreeRTOS block time is counted in ticks and expires at the *next* tick interrupt, not one full period later, so a one-tick wait issued shortly before that interrupt returns almost immediately. At `CONFIG_FREERTOS_HZ = 1000`, which is what ESPHome configures, "1 ms" was in practice a random draw from 0–1 ms, and the decision to stop reading could come from tick phase rather than from an empty FIFO. All six waits now use two ticks (`WMBUS_NOTIFY_WAIT_MS`), which guarantees one whole period.

### Notes
- This wait is the second line of the read path, not the first. Each driver's `read()` first polls the FIFO against a hardware-timer deadline (1000 µs on SX1276, 1800 µs on CC1101) that no tick rate or scheduler can shorten, so the floor was already a hardware-timed millisecond and a truncated notify wait cost the tail of it rather than the whole thing.
- Cost: up to about 1 ms more on the paths that give up, up to about 3 ms for the S1 raw read, which allows three idle rounds. Nothing on a successful receive path gets slower.
- Changing only the default argument would have been cosmetic — every caller passed `1` explicitly.
- The same quantization surfaced elsewhere as an actual receive regression on ESPHome 2026.7.1 and later, in a component whose byte loop depends on this wait as its *first* line. That diagnosis (`IoTLabs-pl/esphome-components`, commit `72e76be`) is what prompted this audit, and the credit belongs there.

### Not verified
- Reasoned from FreeRTOS block-time semantics and the tick rate ESPHome sets, not from a before/after count of `payload_read_failed` on hardware. No misbehaviour was observed here that needed fixing; this removes a known way for a read to be abandoned early rather than a symptom that was reported.

## PL

### Naprawiono
- Podczas odczytu ramki tor odbiorczy czekał na kolejny bajt przez `pdMS_TO_TICKS(1)`, zanim uznał, że radio nie ma już nic do oddania. Czas blokady w FreeRTOS liczony jest w tikach i wygasa na **następnym** przerwaniu tikowym, a nie po pełnym okresie — czekanie na jeden tik rozpoczęte tuż przed tym przerwaniem kończy się niemal natychmiast. Przy `CONFIG_FREERTOS_HZ = 1000`, które ustawia ESPHome, „1 ms" było w praktyce losową wartością z przedziału 0–1 ms, a decyzja o przerwaniu odczytu mogła wynikać z fazy tiku, a nie z pustego FIFO. Wszystkie sześć miejsc czeka teraz dwa tiki (`WMBUS_NOTIFY_WAIT_MS`), co gwarantuje jeden pełny okres.

### Uwagi
- To czekanie jest drugą, nie pierwszą linią toru odczytu. `read()` każdego sterownika najpierw odpytuje FIFO względem terminu opartego na timerze sprzętowym (1000 µs na SX1276, 1800 µs na CC1101), którego ani częstotliwość tiku, ani scheduler nie skrócą — podłoga była więc już wcześniej gwarantowana sprzętowo, a urwane czekanie kosztowało jej końcówkę, nie całość.
- Koszt: do około 1 ms więcej na ścieżkach rezygnacji, do około 3 ms przy odczycie surowym S1, który dopuszcza trzy rundy bezczynności. Nic w torze udanego odbioru nie zwalnia.
- Zmiana samego domyślnego parametru byłaby kosmetyczna — każde wywołanie przekazywało `1` jawnie.
- Ta sama kwantyzacja ujawniła się gdzie indziej jako realny regres odbioru na ESPHome 2026.7.1 i nowszych, w komponencie, którego pętla bajtowa opiera się na tym czekaniu jako **pierwszej** linii. To tamta diagnoza (`IoTLabs-pl/esphome-components`, commit `72e76be`) skłoniła do tego przeglądu i to jej należy się autorstwo.

### Czego nie zweryfikowano
- Rzecz jest wywnioskowana z semantyki czasu blokady w FreeRTOS i z częstotliwości tiku ustawianej przez ESPHome, a nie z porównania liczników `payload_read_failed` na sprzęcie przed zmianą i po niej. Nie zaobserwowano tutaj nieprawidłowości wymagającej naprawy; zmiana usuwa znany sposób na przedwczesne porzucenie odczytu, a nie zgłoszony objaw.

# Fix: CC1101 modules reporting VERSION 0x04 were refused at startup

## EN

### Fixed
- The CC1101 startup self-check compared the `VERSION` status register against a single value, `0x14`. A chip reporting `0x04` failed the check, which called `mark_failed()` — the radio never started, and `dump_debug_status()` labelled it `UNEXPECTED_CHIP_ID`, a verdict that pointed at the wrong thing. `0x04` and `0x14` are two silicon revisions of the same part; both are now recognised.
- Reported by a user on Discussions running a CC1101 whose `VERSION` reads `0x04`.

### Changed
- The revision byte no longer gates the receiver at all. `PARTNUM`/`VERSION` are read and logged; a value outside the known set produces a warning and startup continues. `config_ok` in the diagnostic dump no longer includes chip identity, and the `UNEXPECTED_CHIP_ID` verdict is gone — with it out of the way the classifier reports the state that actually matters (GDO mapping, packet mode, RF profile, RX state) instead of stopping at the first byte it did not recognise.
- A silent SPI bus still fails setup: `VERSION` reading `0x00` or `0xFF` is not a revision, it is a bus with nothing on it.

### Why the check was safe to drop
Chip identity was never what the self-check proved. Nineteen registers are read back and compared against the wM-Bus profile the component just wrote — GDO mapping, FIFO threshold, packet mode, sync word, modem, AGC, front end. Anything that echoes those values from those addresses is a CC1101; anything that does not is rejected on the register check, whatever it claims in `VERSION`.

### Not verified
The fix is reasoned from the register semantics and reviewed, not measured: the author has no `VERSION=0x04` part to test on. On `0x14` hardware the behaviour is unchanged apart from the log wording.

## PL

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
- Measured on hardware, same board and antenna before and after: meter `00088888` went from -96 dBm to -68 dBm, and the receiver went from 4-6 frames per minute across 3 meters to 14 across 32.

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
- Pomiar na sprzęcie, ta sama płytka i antena przed i po: licznik `00088888` z -96 dBm na -68 dBm, a odbiornik z 4-6 ramek na minutę od 3 liczników na 14 od 32.

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
- The two forms are told apart without ambiguity: a non-BCD A-field always contains a nibble above 9 and therefore always prints a hex letter, while a BCD ID never does. The `0x` form works for BCD meters too (`"0x00088888"` is meter `88888`).
- Per-meter statistics were keyed on the BCD ID, so every non-BCD meter collapsed into a single shared entry at key 0. They are now keyed on the raw A-field value, which is unique for every meter.
- `target_meter_id` still accepts only BCD IDs. A hex value there used to be accepted and then never match; it now logs a warning at boot pointing to `forward_meters`.

## PL

### Naprawiono
- Dopasowanie liczników dekodowało A-field jako BCD i w przeciwnym razie rezygnowało, więc liczniki bez ID w BCD (m.in. Diehl/IZAR) nie miały żadnego użytecznego ID. Nie dało się ich wpisać do `highlight_meters`, a przy aktywnym `forward_meters` ich telegramy znikały bez śladu - jedyny przypadek, w którym whitelista odrzucała ramki, których użytkownik nie mógł odzyskać żadną konfiguracją.
- Obie opcje dopasowują teraz również surową wartość A-field, zapisywaną tak, jak pokazuje ją log: `id:417F0666` konfigurujesz jako `"0x417F0666"` (w cudzyslowie, inaczej YAML zamieni to na liczbe). Wpisy dziesiętne zachowują dotychczasowe znaczenie, więc żadna konfiguracja nie zmienia zachowania.
- Rozróżnienie obu form jest jednoznaczne: A-field poza BCD zawsze zawiera półbajtówkę powyżej 9, więc zawsze wypisuje literę szesnastkową, a ID w BCD nigdy. Forma `0x` działa też dla liczników BCD (`"0x00088888"` to licznik `88888`).
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
