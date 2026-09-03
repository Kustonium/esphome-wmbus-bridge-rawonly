# RAM MQTT buffer, per-topic QoS, and the RX timestamp field

*(Polski poniżej / Polish below)*

This note documents three additions made on top of the upstream RAW-only
bridge, done as preparation for migrating a receiver from
`SzczepanLeon/esphome-components@version_4` to this project:

1. a RAM store-and-forward buffer for the telegram stream while MQTT is
   disconnected,
2. per-topic MQTT QoS (0/1/2), configurable in YAML,
3. confirmation that the reception timestamp already travels next to RSSI —
   no new field was needed.

## 1. RAM buffer (`mqtt_buffer_size`)

Before this change, the project's own docs were explicit: when the local
broker is unreachable, RF reception continues but every MQTT publish is
silently skipped. The telegram is lost, not delayed.

`mqtt_buffer_size` (**default `0` — the feature is off until you set it**,
range `0`-`8192`, or `auto` — see below) sets a RAM ring buffer.

**The unit is queued MQTT messages, not telegrams.** One received telegram
enqueues two of them: the raw frame on `.../telegram` and its metadata
companion on `.../rx`, which is published for every forwarded frame. So
`mqtt_buffer_size: 64` carries roughly **32 telegrams** through an outage, and
the `buffer_depth` / `buffer_dropped_*` sensors and the per-meter
`buffer_priority` quotas all count in the same messages. Eviction also works
per message, so a full buffer can drop a telegram while its companion stays
queued — the backend then sees metadata it cannot pair with a frame.

The default is `0` because store-and-forward is not a free improvement that
can be turned on for everyone: it changes delivery semantics (a reconnect
replays a burst of telegrams whose `received_at` is minutes old) and it spends
internal heap on boards without PSRAM. An upgraded config must behave exactly
as it did before, so enabling it is a decision made in YAML.
While MQTT is disconnected, the raw telegram publish and its `/rx` metadata
companion (see part 3) are queued instead of dropped; once MQTT reconnects
they are flushed in order, oldest first, a few messages per `loop()` tick so
a large backlog cannot starve radio reception. If the buffer fills up before
reconnecting, the *oldest* queued frame is dropped to make room for the
newest one — a long outage keeps the most recent readings rather than
locking onto whichever frame arrived first.

Deliberately **not** buffered: the retained per-meter RSSI scalar
(`wmbus/<topic>/rssi/<meter_id>`), health/meters pulses, diagnostic
summaries/suggestions, the target-meter debug topic, and the raw dev tap.
Those are either "latest value" or "this window" signals where a stale
queued entry would be actively wrong, or best-effort debug aids that were
never part of the data-loss problem this buffer solves.

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: 64   # 0 disables buffering (old behaviour: drop on disconnect)
```

### Runtime control without reflashing

`mqtt_buffer_size` sets the *compiled ceiling*. To inspect and adjust the
buffer live, declare the optional `sensor:`/`number:` sub-platforms and
ESPHome's own `web_server:` with authentication — no custom HTTP server or
auth code was added to this component for this:

```yaml
web_server:
  version: 3
  auth:
    username: !secret web_username
    password: !secret web_password

sensor:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here     # the id: of your wmbus_radio: block
    buffer_depth:
      name: "WMBus Buffer Depth"
    buffer_dropped_total:
      name: "WMBus Buffer Dropped"
    buffer_oldest_pending_age:
      name: "WMBus Buffer Oldest Pending Age"

number:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here
    buffer_capacity:
      name: "WMBus Buffer Capacity"
```

The `number` entity can only lower the effective capacity, never raise it
past `mqtt_buffer_size` — the C++ side clamps and republishes the effective
value, so the portal never claims a capacity larger than what was actually
compiled in.

### Is it one shared buffer, or one per meter?

Both, selected automatically by whether `forward_meters` is a whitelist:

- **No whitelist configured** (`forward_meters` empty, the default): one
  shared FIFO queue for the whole receiver, exactly as described above - the
  oldest *message*, from whichever meter, is what gets dropped when the
  buffer is full.
- **Whitelist configured**: the buffer is split into one fixed slice per
  whitelisted meter, so a meter transmitting often can never crowd out one
  that transmits rarely - each meter only ever evicts its *own* oldest queued
  message, never another meter's.

### Per-meter priority (`buffer_priority`)

With a whitelist, every meter defaults to an equal-sized slice of the
buffer. `buffer_priority` lets some meters get a bigger slice than others -
for example a meter in a stairwell with poor signal that needs more retries
worth of headroom, versus one right next to the antenna:

```yaml
wmbus_radio:
  # ...
  forward_meters: [12345678, 87654321, "0x417F0666"]
  buffer_priority:
    "12345678": 3   # gets 3x the slice of a default-weight meter
    "87654321": 1   # same as leaving it unset
    # "0x417F0666" not listed -> also defaults to weight 1
```

**Why plain weights, not percentages.** A percentage-per-meter scheme has an
obvious failure mode: what happens when the percentages the user wrote do
not add up to 100? Either an error, or an implicit and confusing
renormalization. Plain positive integers avoid the question entirely - there
is no total they have to sum to. Each meter's exact share of the current
capacity is `capacity * weight / sum(all weights)`; the code floors every
share, then hands out the few leftover slots (`capacity` minus the sum of the
floors) one each to the meters with the largest fractional remainder. This is
the same *largest-remainder* (Hamilton) apportionment method used to allocate
parliamentary seats, and it has exactly the property this needs: quotas
always sum to **exactly** the current buffer capacity, for any positive
integer weights, with no rounding leftover and nothing for the user to get
wrong. A meter with no explicit entry defaults to weight 1, so leaving
`buffer_priority` unset entirely - or leaving one meter out of it - means an
equal split for that meter, not "gets nothing".

Quotas are recomputed automatically whenever the capacity changes (auto
re-sizing every ~30s, or a runtime edit via the `buffer_capacity` number
entity), trimming any meter that ends up over its new, possibly smaller,
quota. The computed quotas are also logged once at boot
(`MQTT outbox per-meter quotas (id:weight->slots)`), so what each meter
actually got is visible without guessing.

`buffer_priority` without a whitelist has nothing to prioritise (there is no
fixed meter set to slice capacity across) and is ignored with a boot warning
if set that way.

### Sizing the buffer from free RAM (`mqtt_buffer_size: auto`)

The earlier fixed ceiling (256 frames) was picked as a round number, not
derived from what is actually free on the target board - a legitimate
concern, since a plain ESP32 (no PSRAM - e.g. the Olimex ESP32-POE this
project's own migration targets) only has a few hundred KB of heap total,
shared with WiFi/Ethernet/MQTT/TLS.

`mqtt_buffer_size: auto` replaces the fixed number with a value computed on
the device itself:

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: auto
```

How it is computed (`Radio::suggested_mqtt_outbox_capacity_()` in
`mqtt_outbox.cpp`): read currently-free **internal** heap
(`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`), reserve the last 40 KB
untouched (a conservative floor below which WiFi/TLS allocations start
failing), budget at most 25% of whatever is free above that reserve for the
outbox, and divide that budget by ~400 bytes (a generous estimate per queued
message - topic + payload strings plus allocator overhead). The result is
clamped to a sane range (4-512 frames).

This runs once at boot (a rough first estimate, since WiFi/Ethernet/MQTT/TLS
have not necessarily finished claiming their share of heap yet at that
point) and is re-evaluated every ~30 seconds afterwards, so it adapts if free
RAM changes later - more diagnostics enabled, TLS buffers, heap
fragmentation over uptime - instead of freezing a single boot-time guess.

**What happens if a re-check needs to shrink the buffer while it is full?**
It trims immediately, not gradually. The moment the new, smaller capacity is
applied (`recompute_buffer_quotas_()`, called from the same setter that
updates the capacity), anything that no longer fits is dropped right then -
oldest message first in shared-pool mode, or oldest-message-of-its-own-meter
first for every meter that is now over its (possibly smaller) `buffer_priority`
quota, if a whitelist is configured. Nothing waits for the buffer to drain
on its own first. This is logged when it happens:
`MQTT outbox auto-size shrink dropped N still-queued frame(s)...`, so a
shrink-triggered drop is visible in the log rather than silent. The same
trimming - and the same kind of log line - happens if you lower the
`buffer_capacity` number: entity manually below what is currently queued.

**PSRAM**: free PSRAM is read and logged at boot for visibility (via
`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`, `0` if the board has none),
but the sizing formula above does **not** budget from it. `std::string` on
ESP-IDF allocates from the default (internal) heap regardless of whether
PSRAM is present, so a queued message does not actually spend PSRAM today -
basing the suggestion on PSRAM would overstate how big the buffer can safely
get. Routing outbox storage through a PSRAM allocator so PSRAM boards really
can use much larger buffers is a reasonable follow-up, not done here.

**The safety valve applies either way.** Whether `mqtt_buffer_size` is a
fixed number or `auto`, `enqueue_or_publish_()` refuses to grow the queue any
further once free internal heap drops below that same 40 KB reserve,
independent of the nominal capacity - so a manually-chosen number that looked
safe at boot can never be the thing that starves the rest of the device of
RAM later. This is logged (rate-limited to once a minute) as `MQTT outbox:
free heap below reserve, refusing to buffer`.

**Fragmentation is checked separately from total free heap.** A reserve is a
sum, and a sum says nothing about whether a single contiguous block is still
available - but WiFi, lwIP and esp-tls all need contiguous internal buffers
(a TLS record buffer alone is on the order of 16 KB). A long outage fills the
outbox with hundreds of small, variable-sized allocations, so on a board
without PSRAM free internal heap can still read comfortably above the 40 KB
reserve while the largest remaining block has dropped below what a reconnect
needs. The valve therefore also refuses once
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` falls under 16 KB,
logged as `MQTT outbox: internal heap too fragmented, refusing to buffer`.
Both figures are sampled on the same 1 Hz tick as the free-heap reads, for
the same reason: that call takes the heap lock the `radio_recv` task needs.
The check applies on the PSRAM path too, where only the ~40 B deque nodes are
internal and it should essentially never trigger - the argument for it does
not depend on where the payload bytes live.

## 2. Per-topic QoS

Every publish in this component used to be hardcoded: QoS 0 everywhere
except the `/rx` metadata topic, which was QoS 1. Five options now control
QoS per topic group, all defaulting to the previous hardcoded value so an
existing YAML with none of them set behaves exactly as before:

```yaml
wmbus_radio:
  # ...
  telegram_qos: 0      # wmbus/<topic>/telegram
  rssi_qos: 0           # wmbus/<topic>/rssi/<meter_id>
  health_qos: 0         # wmbus/<topic>/health and /meters
  diagnostic_qos: 0     # all wmbus/<topic>/diag/... topics (boot, config,
                         # summaries, suggestions, meter windows, drop/
                         # truncated events, busy_ether_changed)
  rx_qos: 1              # wmbus/<topic>/rx (rssi_dbm + received_at)
```

Per the migration spec's own QoS table (section 17/Table 26): QoS 1 is
recommended on the segments that actually need at-least-once delivery once a
consumer is idempotent (deduplicates on `raw_hash`/access number), and QoS 2
is not required anywhere in this system. A reasonable starting point once
the buffer above is enabled is `telegram_qos: 1` (matches `rx_qos`, which
was already 1); leave the rest at 0 unless a specific consumer needs more.

### Does QoS still apply when a message goes through the RAM buffer?

Yes, unchanged. `enqueue_or_publish_()` (the function every buffered publish
goes through - see part 1) takes the QoS value the caller already resolved
from `telegram_qos`/`rx_qos` and stores it *on the queued message itself*
(`OutboxMsg::qos`). When the message is later flushed - immediately if MQTT
is connected, or once it reconnects if it was queued - it is published with
that same stored QoS, not a fixed value. Buffering only changes *when* a
message reaches the broker, never *how* it is published. This required no
extra code to make correct: it falls straight out of storing the fully
resolved topic/payload/qos/retain of each message rather than a "buffer
everything at QoS 0" shortcut.

### Runtime QoS control (`select:` `telegram_qos` / `rx_qos`)

`telegram_qos`/`rx_qos` above set the *compiled starting value*. To raise
QoS for the two RAM-buffered topics from `0` to `1` or `2` live - without
reflashing - declare the optional `select:` sub-platform, on the same
lightweight authenticated `web_server:` portal the `buffer_capacity`
number: entity already uses:

```yaml
web_server:
  version: 3
  auth:
    username: !secret web_username
    password: !secret web_password

select:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here     # the id: of your wmbus_radio: block
    telegram_qos:
      name: "WMBus Telegram QoS"
    rx_qos:
      name: "WMBus RX QoS"
```

Each entity is a dropdown with options `0`/`1`/`2`, starts at the value
`telegram_qos`/`rx_qos` compiled to, and calls straight into the same
`set_telegram_qos()`/`set_rx_qos()` setters the YAML options use - so a
change here is exactly as effective as reflashing with a different YAML
value, just without the reflash. It only affects **new** publishes from
that point on: a message already sitting in the RAM buffer keeps whichever
QoS it was queued with (see the answer above - QoS is captured per-message
at enqueue time).

## 3. The RX timestamp field: already there, next to RSSI

No code change was needed here. Every time a telegram is forwarded on
`wmbus/<topic>/telegram`, this component already publishes a companion JSON
message on `wmbus/<topic>/rx` (QoS controlled by `rx_qos` above) that
carries **both** fields together:

```json
{"schema":1,"boot_id":"...","seq":1,"rx_task_wakeup_us":...,
 "meter_id":"...","mode":"T1","rssi_dbm":-78,"frame_crc32":"...",
 "frame_length":32,"received_at":"2026-08-28T10:15:02.421Z"}
```

`received_at` is the wall-clock instant the frame was *received* (not
published — the two differ once buffering is involved), computed from
monotonic uptime at reception time. It is present only once SNTP has
synced; on a receiver that has not yet synced (the first seconds/minutes
after boot), the key is simply absent rather than carrying a false `1970`
or an uptime value disguised as a date — a consumer should treat a missing
key as "no reliable timestamp yet", not as an error.

Because the payload is built at reception time and only *enqueued or
published* afterwards, `received_at` stays accurate to the actual reception
instant even when the message spends time queued in the RAM buffer above.

---

## Polski

Ta notatka opisuje trzy zmiany dodane na bazie projektu RAW-only, w ramach
przygotowań do migracji odbiornika z `SzczepanLeon/esphome-components@version_4`
na ten projekt:

1. bufor RAM (store-and-forward) na strumień telegramów, gdy MQTT jest
   rozłączony,
2. konfigurowalny w YAML QoS per topic (0/1/2),
3. potwierdzenie, że znacznik czasu odbioru już jest publikowany obok RSSI —
   nowe pole nie było potrzebne.

### 1. Bufor RAM (`mqtt_buffer_size`)

Wcześniej dokumentacja projektu wprost mówiła: gdy lokalny broker jest
niedostępny, odbiór RF trwa dalej, ale każda publikacja MQTT jest po prostu
pomijana. Telegram ginie, a nie jest opóźniony.

`mqtt_buffer_size` (domyślnie `64`, zakres `0`-`512`, albo `auto` — patrz
niżej) ustawia bufor pierścieniowy w RAM. Gdy MQTT jest rozłączony, publikacja surowego
telegramu oraz towarzyszący jej JSON na `/rx` (patrz punkt 3) trafiają do
kolejki zamiast być odrzucane; po odzyskaniu połączenia są wysyłane w
kolejności FIFO, po kilka wiadomości na cykl `loop()`, żeby duży zaległy
bufor nie zag³odzi³ odbioru radiowego. Jeśli bufor zapełni się zanim MQTT
wróci, usuwana jest *najstarsza* ramka w kolejce, żeby zrobić miejsce dla
najnowszej — długa awaria zachowuje najświeższe odczyty zamiast utknąć na
pierwszej ramce z okna awarii.

Celowo **nie** buforowane: retained skalar RSSI per licznik
(`wmbus/<topic>/rssi/<meter_id>`), puls health/meters, podsumowania i
sugestie diagnostyczne, topic docelowego licznika (debug) oraz surowy tap
deweloperski. To sygnały typu "ostatnia wartość" albo "to okno czasowe",
gdzie zbuforowana nieświeża wartość byłaby wręcz błędem, albo funkcje
best-effort niezwiązane z problemem utraty danych, który rozwiązuje ten
bufor.

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: 64   # 0 wylacza buforowanie (stare zachowanie: drop przy rozlaczeniu)
```

#### Sterowanie w czasie działania, bez przeszywania

`mqtt_buffer_size` ustawia *skompilowany sufit*. Aby podejrzeć i zmienić
bufor na żywo, zadeklaruj opcjonalne pod-platformy `sensor:`/`number:` oraz
wbudowany w ESPHome `web_server:` z autoryzacją — do tego komponentu nie
dodano własnego serwera HTTP ani obsługi logowania:

```yaml
web_server:
  version: 3
  auth:
    username: !secret web_username
    password: !secret web_password

sensor:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here     # id: Twojego bloku wmbus_radio:
    buffer_depth:
      name: "WMBus Buffer Depth"
    buffer_dropped_total:
      name: "WMBus Buffer Dropped"
    buffer_oldest_pending_age:
      name: "WMBus Buffer Oldest Pending Age"

number:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here
    buffer_capacity:
      name: "WMBus Buffer Capacity"
```

Encja `number` może tylko zmniejszyć efektywną pojemność, nigdy podnieść
jej powyżej `mqtt_buffer_size` — strona C++ przycina wartość i publikuje
efektywną liczbę z powrotem, więc portal nigdy nie pokaże pojemności
większej niż faktycznie skompilowana.

### Jeden wspólny bufor, czy jeden per licznik?

Jedno i drugie, wybierane automatycznie w zależności od tego, czy
`forward_meters` jest whitelistą:

- **Brak whitelisty** (`forward_meters` puste, domyślnie): jedna wspólna
  kolejka FIFO dla całego odbiornika, dokładnie jak opisano wyżej — gdy
  bufor się zapełni, usuwana jest najstarsza *wiadomość*, niezależnie od
  tego, z którego licznika pochodzi.
- **Whitelist skonfigurowana**: bufor jest dzielony na stałą część (quota)
  dla każdego licznika z whitelisty, więc licznik nadający często nigdy nie
  wypchnie z bufora licznika nadającego rzadko — każdy licznik usuwa
  wyłącznie *swoją własną* najstarszą zakolejkowaną wiadomość, nigdy cudzej.

### Priorytet per licznik (`buffer_priority`)

Przy skonfigurowanej whiteliście każdy licznik domyślnie dostaje równą część
bufora. `buffer_priority` pozwala dać niektórym licznikom większą część —
np. licznikowi na klatce schodowej ze słabym sygnałem (potrzebuje więcej
zapasu na powtórki) w porównaniu do licznika tuż przy antenie:

```yaml
wmbus_radio:
  # ...
  forward_meters: [12345678, 87654321, "0x417F0666"]
  buffer_priority:
    "12345678": 3   # dostaje 3x część licznika o domyślnej wadze
    "87654321": 1   # tak samo, jakby go nie wpisać
    # "0x417F0666" nie wypisany -> też domyślnie waga 1
```

**Dlaczego zwykłe wagi, a nie procenty.** Schemat "procent na licznik" ma
oczywistą wadę: co się dzieje, gdy wpisane procenty nie sumują się do 100?
Albo błąd, albo niejawna i myląca renormalizacja. Zwykłe dodatnie liczby
całkowite całkowicie omijają ten problem — nie ma żadnej sumy, do której
trzeba trafić. Dokładny udział każdego licznika w aktualnej pojemności to
`pojemnosc * waga / suma_wszystkich_wag`; kod najpierw zaokrągla każdy udział
w dół, a potem rozdaje pozostałe miejsca (`pojemnosc` minus suma zaokrągleń
w dół) po jednym licznikom o największej części ułamkowej. To ta sama metoda
apportionment (metoda największej reszty / Hamiltona), używana m.in. do
podziału mandatów parlamentarnych, i ma dokładnie tę własność, która tu jest
potrzebna: quoty zawsze sumują się **dokładnie** do aktualnej pojemności
bufora, dla dowolnych dodatnich wag całkowitych, bez żadnej reszty z
zaokrąglenia i bez niczego, co użytkownik mógłby źle policzyć. Licznik bez
jawnego wpisu domyślnie dostaje wagę 1, więc pozostawienie `buffer_priority`
całkowicie nieustawionego — albo pominięcie w nim jednego licznika — oznacza
dla tego licznika równy podział, a nie "nic nie dostaje".

Quoty są przeliczane automatycznie za każdym razem, gdy zmienia się
pojemność (auto-dobór co ~30s, albo ręczna zmiana przez encję `number`
`buffer_capacity`), przycinając licznik, który przekroczył swoją nową,
ewentualnie mniejszą, quotę. Wyliczone quoty są też logowane raz przy
starcie (`MQTT outbox per-meter quotas (id:weight->slots)`), więc widać bez
zgadywania, ile faktycznie dostał każdy licznik.

`buffer_priority` bez whitelisty nie ma czego priorytetyzować (nie ma
ustalonego zbioru liczników, między które dzielić pojemność) i jest
ignorowane z ostrzeżeniem w logu startowym, jeśli mimo to zostanie
ustawione.

### Dobór wielkości bufora z wolnego RAM (`mqtt_buffer_size: auto`)

Wcześniejszy sztywny sufit (256 ramek) był liczbą "w sam raz", a nie
wyliczoną z tego, co faktycznie jest wolne na danej płytce — słuszna
wątpliwość, bo zwykły ESP32 bez PSRAM (np. Olimex ESP32-POE, którego
dotyczy migracja opisana w specyfikacji) ma zaledwie kilkaset KB heapu w
sumie, dzielonego z WiFi/Ethernet/MQTT/TLS.

`mqtt_buffer_size: auto` zastępuje sztywną liczbę wartością wyliczaną na
samym urządzeniu:

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: auto
```

Jak to jest liczone (`Radio::suggested_mqtt_outbox_capacity_()` w
`mqtt_outbox.cpp`): odczytaj aktualnie wolny heap **wewnętrzny**
(`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`), zarezerwuj ostatnie 40 KB
nietykalne (konserwatywny próg, poniżej którego alokacje WiFi/TLS zaczynają
zawodzić), przeznacz na bufor maksymalnie 25% tego, co wolne ponad tę
rezerwę, i podziel ten budżet przez ~400 bajtów (hojne oszacowanie na jedną
zakolejkowaną wiadomość — stringi topic+payload plus narzut alokatora).
Wynik jest przycinany do rozsądnego zakresu (4-512 ramek).

To liczy się raz przy starcie (zgrubne pierwsze oszacowanie, bo
WiFi/Ethernet/MQTT/TLS niekoniecznie zdążyły jeszcze zająć swoją część
heapu w tym momencie) i jest przeliczane ponownie co ~30 sekund, więc
dostosowuje się, jeśli wolny RAM zmieni się później — więcej włączonej
diagnostyki, bufory TLS, fragmentacja heapu w czasie działania — zamiast
zamrażać jedno zgadywanie z momentu startu.

**Co się dzieje, jeśli przeliczenie musi zmniejszyć bufor, a bufor jest
zapełniony?** Przycina natychmiast, nie stopniowo. W chwili zastosowania
nowej, mniejszej pojemności (`recompute_buffer_quotas_()`, wywoływane z
tego samego settera, który zmienia pojemność) wszystko, co się już nie
mieści, jest od razu usuwane — najstarsza wiadomość jako pierwsza w trybie
wspólnego bufora, albo najstarsza wiadomość *danego* licznika jako pierwsza
dla każdego licznika, który przekroczył swoją (ewentualnie zmniejszoną)
quotę `buffer_priority`, jeśli skonfigurowana jest whitelist. Nic nie czeka,
aż bufor sam się opróżni. Jest to logowane w momencie wystąpienia:
`MQTT outbox auto-size shrink dropped N still-queued frame(s)...`, więc
odrzucenie wywołane zmniejszeniem bufora jest widoczne w logu, a nie ciche.
To samo przycinanie — i ten sam rodzaj wpisu w logu — następuje też, gdy
ręcznie zmniejszysz encję `number:` `buffer_capacity` poniżej tego, co jest
akurat w kolejce.

**PSRAM**: wolny PSRAM jest odczytywany i logowany przy starcie dla
informacji (przez `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`, `0` jeśli
płytka go nie ma), ale formuła powyżej **nie** czerpie z niego budżetu.
`std::string` na ESP-IDF alokuje z domyślnego (wewnętrznego) heapu
niezależnie od obecności PSRAM, więc zakolejkowana wiadomość faktycznie nie
zużywa dziś PSRAM — oparcie sugestii na PSRAM zawyżałoby, jak duży bufor
można bezpiecznie mieć. Przekierowanie magazynu bufora przez alokator PSRAM,
żeby płytki z PSRAM rzeczywiście mogły mieć dużo większe bufory, to rozsądny
krok następny, nie zrobiony tutaj.

**Zawór bezpieczeństwa działa niezależnie od trybu.** Czy `mqtt_buffer_size`
jest sztywną liczbą, czy `auto`, `enqueue_or_publish_()` odmawia dalszego
powiększania kolejki, gdy wolny heap wewnętrzny spadnie poniżej tej samej
rezerwy 40 KB, niezależnie od nominalnej pojemności — więc ręcznie wybrana
liczba, która wyglądała bezpiecznie przy starcie, nigdy nie stanie się
przyczyną braku RAM dla reszty urządzenia później. Jest to logowane (nie
częściej niż raz na minutę) jako `bufor MQTT: wolny heap ponizej rezerwy,
odmowa buforowania`.

### 2. QoS per topic

Wcześniej każda publikacja w tym komponencie miała zaszyty na sztywno QoS:
0 wszędzie poza topikiem metadanych `/rx`, który miał QoS 1. Teraz pięć
opcji steruje QoS per grupa topiców, każda domyślnie ustawiona na
dotychczasową wartość — istniejący YAML bez tych opcji zachowuje się
dokładnie jak wcześniej:

```yaml
wmbus_radio:
  # ...
  telegram_qos: 0      # wmbus/<topic>/telegram
  rssi_qos: 0           # wmbus/<topic>/rssi/<meter_id>
  health_qos: 0         # wmbus/<topic>/health i /meters
  diagnostic_qos: 0     # wszystkie wmbus/<topic>/diag/... (boot, config,
                         # podsumowania, sugestie, okna licznikow, zdarzenia
                         # drop/truncated, busy_ether_changed)
  rx_qos: 1              # wmbus/<topic>/rx (rssi_dbm + received_at)
```

Zgodnie z tabelą QoS z dostarczonej specyfikacji migracji (rozdział 17 /
Tabela 26): QoS 1 jest zalecany na odcinkach, które faktycznie potrzebują
dostarczenia at-least-once, o ile konsument jest idempotentny (deduplikuje
po `raw_hash`/access number), a QoS 2 nigdzie w tym systemie nie jest
wymagany. Rozsądny punkt startowy po włączeniu bufora powyżej to
`telegram_qos: 1` (spójne z `rx_qos`, które już miało 1); resztę zostaw na
0, chyba że konkretny konsument potrzebuje więcej.

### Czy QoS nadal działa, gdy wiadomość przechodzi przez bufor RAM?

Tak, bez zmian. `enqueue_or_publish_()` (funkcja, przez którą przechodzi
każda buforowana publikacja — patrz punkt 1) zapisuje wartość QoS, którą
wywołujący kod już wcześniej wziął z `telegram_qos`/`rx_qos`, *razem z samą
zakolejkowaną wiadomością* (`OutboxMsg::qos`). Gdy wiadomość jest później
wysyłana — natychmiast, jeśli MQTT jest połączone, albo po ponownym
połączeniu, jeśli czekała w kolejce — jest publikowana z tym samym zapisanym
QoS, a nie z jakąś sztywną wartością. Buforowanie zmienia tylko *kiedy*
wiadomość dotrze do brokera, nigdy *jak* jest publikowana. Nie wymagało to
żadnego dodatkowego kodu, żeby działało poprawnie — wynika wprost z tego, że
zapisywany jest w pełni rozwiązany zestaw topic/payload/qos/retain każdej
wiadomości, a nie skrót w stylu "buforuj wszystko z QoS 0".

### Sterowanie QoS w czasie działania (`select:` `telegram_qos` / `rx_qos`)

`telegram_qos`/`rx_qos` powyżej ustawiają *skompilowaną wartość startową*.
Aby podnieść QoS dla dwóch buforowanych w RAM topiców z `0` na `1` albo `2`
na żywo — bez przeszywania — zadeklaruj opcjonalną pod-platformę `select:`,
na tym samym lekkim, autoryzowanym portalu `web_server:`, którego już
używa encja `number:` `buffer_capacity`:

```yaml
web_server:
  version: 3
  auth:
    username: !secret web_username
    password: !secret web_password

select:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here     # id: Twojego bloku wmbus_radio:
    telegram_qos:
      name: "WMBus Telegram QoS"
    rx_qos:
      name: "WMBus RX QoS"
```

Każda encja to lista rozwijana z opcjami `0`/`1`/`2`, startuje od wartości,
na którą skompilowano `telegram_qos`/`rx_qos`, i wywołuje bezpośrednio te
same settery `set_telegram_qos()`/`set_rx_qos()`, których używają opcje
YAML — więc zmiana tutaj działa dokładnie tak samo, jakby przeszyć z inną
wartością YAML, tylko bez przeszywania. Dotyczy wyłącznie **nowych**
publikacji od tego momentu — wiadomość, która już siedzi w buforze RAM,
zachowuje QoS, z którym trafiła do kolejki (patrz odpowiedź wyżej — QoS
jest zapisywany per wiadomość w momencie jej zakolejkowania).

### 3. Pole timestamp: już jest, obok RSSI

Tu nie było potrzeby zmiany kodu. Za każdym razem, gdy telegram jest
przekazywany na `wmbus/<topic>/telegram`, ten komponent już publikuje
towarzyszący JSON na `wmbus/<topic>/rx` (QoS sterowany przez `rx_qos`
powyżej), który niesie **oba** pola razem:

```json
{"schema":1,"boot_id":"...","seq":1,"rx_task_wakeup_us":...,
 "meter_id":"...","mode":"T1","rssi_dbm":-78,"frame_crc32":"...",
 "frame_length":32,"received_at":"2026-08-28T10:15:02.421Z"}
```

`received_at` to chwila zegarowa, w której ramka zosta³a *odebrana* (nie
opublikowana — te dwie chwile różnią się, gdy w grę wchodzi buforowanie),
wyliczona z monotonicznego czasu działania w momencie odbioru. Pole
pojawia się dopiero, gdy SNTP zdąży się zsynchronizować; na odbiorniku,
który jeszcze się nie zsynchronizował (pierwsze sekundy/minuty po
starcie), klucz jest po prostu nieobecny, zamiast nieść fałszywy rok 1970
albo czas działania udający datę — konsument powinien traktować brak
klucza jako "brak wiarygodnego znacznika czasu", a nie jako błąd.

Ponieważ payload jest budowany w momencie odbioru, a dopiero potem
*kolejkowany albo publikowany*, `received_at` pozostaje zgodny z
rzeczywistą chwilą odbioru nawet, gdy wiadomość spędzi jakiś czas w
buforze RAM opisanym powyżej.
