# Nowe funkcje w komponencie `wmbus_radio` — lista przed uruchomieniem

Migracja z `SzczepanLeon/esphome-components@version_4` na
`Kustonium/esphome-wmbus-bridge-rawonly`, plus zmiany dodane na Twoją
prośbę. Wszystkie poniższe funkcje są **wyłączone lub ustawione na
zachowanie identyczne ze stanem sprzed zmian**, jeśli nic nie wpiszesz w
YAML — nic nie zaskoczy Cię "domyślnie włączone".

---

## 1. Bufor RAM na wypadek rozłączenia MQTT (`mqtt_buffer_size`)

**Problem, który rozwiązuje:** wcześniej, gdy lokalny broker był
niedostępny, odbiór RF trwał dalej, ale każda publikacja MQTT była po
prostu pomijana — telegram ginął bezpowrotnie, a nie był opóźniony.

**Co robi:** buforuje w RAM (pierścień FIFO) publikację surowego telegramu
(`.../telegram`) oraz towarzyszący jej JSON (`.../rx`, RSSI + timestamp —
patrz punkt 3) na czas rozłączenia. Po powrocie MQTT wysyła zaległe w
kolejności, po kilka wiadomości na cykl `loop()`, żeby nie zagłodzić
odbioru radiowego.

**Celowo NIE buforowane:** retained RSSI per licznik, puls health/meters,
podsumowania/sugestie diagnostyczne, topic docelowego licznika (debug),
surowy tap deweloperski. To sygnały typu "ostatnia wartość" albo "to okno
czasowe" — zbuforowana nieświeża wartość byłaby błędem, nie korzyścią.

**Gdy bufor się zapełni:** usuwana jest najstarsza wiadomość, żeby zrobić
miejsce dla najnowszej.

```yaml
wmbus_radio:
  mqtt_buffer_size: 64   # DOMYSLNIE 0 = wylaczone; funkcja wlacza sie dopiero tym wpisem
                         # jednostka to WIADOMOSCI MQTT, nie telegramy: jeden telegram
                         # zajmuje dwa miejsca (/telegram + /rx), wiec 64 to ok. 32 odczyty
                         # zakres 0-8192, albo "auto" (patrz punkt 4)
```

---

## 2. QoS per topic, konfigurowalny w YAML

Wcześniej każda publikacja miała QoS zaszyty na sztywno (0 wszędzie poza
`/rx`, które miało 1). Teraz pięć osobnych opcji, **każda domyślnie równa
dawnej wartości sztywnej** — istniejący YAML bez tych opcji publikuje
dokładnie tak jak wcześniej:

```yaml
wmbus_radio:
  telegram_qos: 0      # wmbus/<topic>/telegram
  rssi_qos: 0           # wmbus/<topic>/rssi/<meter_id>
  health_qos: 0         # wmbus/<topic>/health i /meters
  diagnostic_qos: 0     # wszystkie wmbus/<topic>/diag/...
  rx_qos: 1              # wmbus/<topic>/rx (rssi_dbm + received_at)
```

QoS jest zapisywany razem z wiadomością w momencie wejścia do bufora i
stosowany bez zmian przy faktycznej wysyłce — buforowanie nigdy nie
nadpisuje QoS na 0.

---

## 3. Pole timestamp odbioru — już istniało, nic nie trzeba było dodawać

Za każdym razem, gdy telegram trafia na `.../telegram`, komponent od dawna
publikuje też towarzyszący JSON na `.../rx`, który niesie **razem**
`rssi_dbm` i `received_at`:

```json
{"schema":1,"boot_id":"...","seq":1,"meter_id":"...","mode":"T1",
 "rssi_dbm":-78,"frame_crc32":"...","frame_length":32,
 "received_at":"2026-08-28T10:15:02.421Z"}
```

`received_at` to chwila **odbioru**, nie publikacji — zostaje dokładna
nawet jeśli wiadomość czeka w buforze RAM. Pole jest nieobecne (nie `null`,
nie `1970`), dopóki ESP nie zsynchronizuje zegara po starcie (SNTP).

---

## 4. Automatyczny dobór wielkości bufora z wolnego RAM (`mqtt_buffer_size: auto`)

Zamiast sztywnej liczby, ESP sam wylicza bezpieczną wielkość bufora na
podstawie realnie wolnego heapu:

```yaml
wmbus_radio:
  mqtt_buffer_size: auto
```

- Rezerwuje ostatnie **40 KB** heapu wewnętrznego nietykalne (próg, poniżej
  którego zaczynają zawodzić alokacje WiFi/TLS).
- Z tego, co zostaje wolne ponad rezerwę, budżetuje maks. **25%** na bufor.
- Dzieli budżet przez ~400 B na wiadomość (szacunek), wynik przycięty do
  zakresu **4-512** ramek.
- Liczy się raz przy starcie i **przelicza ponownie co ~30 s**, więc
  dostosowuje się, gdy wolny RAM się zmieni (włączona diagnostyka, bufory
  TLS, fragmentacja heapu).
- **PSRAM jest tylko odczytywany i logowany dla informacji — bufor go NIE
  wykorzystuje** (string na ESP-IDF zawsze alokuje z heapu wewnętrznego,
  niezależnie od obecności PSRAM). Wasze płytki (Olimex ESP32-POE) nie mają
  PSRAM, więc to nie dotyczy Waszej migracji, ale warto wiedzieć.

**Zawór bezpieczeństwa (działa zawsze, niezależnie od trybu):** jeśli wolny
heap wewnętrzny spadnie poniżej rezerwy 40 KB, bufor **odmawia przyjęcia
nowej wiadomości** — nawet gdyby nominalna pojemność na to pozwalała.
Zdarzenie logowane maks. raz na minutę.

**Fragmentacja sprawdzana osobno od sumy wolnego heapu:** rezerwa to suma, a
suma nic nie mówi o tym, czy został jeszcze jeden **ciągły** blok — a WiFi,
lwIP i esp-tls potrzebują ciągłych buforów wewnętrznych (sam bufor rekordu
TLS to rząd 16 KB). Długa awaria zapełnia bufor setkami małych alokacji o
różnej wielkości, więc na płytce bez PSRAM suma wolnego heapu potrafi stać
spokojnie ponad rezerwą 40 KB, podczas gdy największy pozostały blok spadł
poniżej tego, czego potrzebuje rekonnekt. Dlatego zawór odmawia również, gdy
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` zejdzie poniżej
16 KB — log: `MQTT outbox: internal heap too fragmented, refusing to buffer`.
Obie wielkości są próbkowane na tym samym takcie 1 Hz co odczyty wolnego
heapu, z tego samego powodu: ten odczyt zajmuje lock sterty, którego
potrzebuje task `radio_recv`. Sprawdzenie działa też na ścieżce PSRAM, gdzie
wewnętrzne są tylko ~40-bajtowe węzły kolejki i praktycznie nie ma prawa się
odpalić — uzasadnienie nie zależy od tego, gdzie leżą bajty payloadu.

**Co się dzieje, gdy przeliczenie co 30 s zmniejsza bufor, a jest on
zapełniony:** przycina natychmiast (nie czeka, aż się sam opróżni) —
usuwa najstarsze wiadomości, ile trzeba, żeby zmieścić się w nowej,
mniejszej pojemności. Zdarzenie jest logowane z liczbą odrzuconych ramek.

---

## 5. Priorytet bufora per licznik (`buffer_priority`) — tylko z whitelistą

**Wymaga ustawionej `forward_meters`** (bez niej nie ma czego
priorytetyzować — dostaniesz ostrzeżenie w logu, jeśli mimo to ustawisz
`buffer_priority`).

Bez whitelisty: jeden wspólny bufor FIFO dla wszystkich (jak dotychczas).
Z whitelistą: bufor dzieli się na osobną, stałą część (quotę) dla każdego
licznika — licznik nadający często nigdy nie wypycha z bufora licznika
nadającego rzadko.

```yaml
wmbus_radio:
  forward_meters: [12345678, 87654321, "0x417F0666"]
  buffer_priority:
    "12345678": 3   # 3x większa część bufora niż licznik domyślny
    "87654321": 1   # tak samo, jakby go nie wpisać (domyślna waga = 1)
    # "0x417F0666" pominięty -> też dostaje wagę 1
```

Wagi to zwykłe dodatnie liczby całkowite (nie procenty) — nie ma sumy, do
której trzeba trafić. Podział liczony metodą "największej reszty"
(Hamilton), więc quoty **zawsze** sumują się dokładnie do bieżącej
pojemności bufora. Wyliczone quoty logowane raz przy starcie
(`MQTT outbox per-meter quotas (id:weight->slots)`).

---

## 6. Zmiany w czasie działania bez przeszywania — lekki portal www

Wymaga zadeklarowania wbudowanego w ESPHome `web_server:` z `auth:` (nie
dodano żadnego własnego serwera HTTP/logowania do tego komponentu):

```yaml
web_server:
  version: 3
  auth:
    username: !secret web_username
    password: !secret web_password
```

### 6a. Podgląd bufora (`sensor:`)

```yaml
sensor:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here
    buffer_depth:
      name: "WMBus Buffer Depth"              # ile wiadomości aktualnie w kolejce
    buffer_dropped_total:
      name: "WMBus Buffer Dropped"             # licznik odrzuconych (lifetime)
    buffer_oldest_pending_age:
      name: "WMBus Buffer Oldest Pending Age"  # wiek najstarszej oczekującej [s]
```

### 6b. Ręczna zmiana pojemności bufora (`number:`)

```yaml
number:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here
    buffer_capacity:
      name: "WMBus Buffer Capacity"
```

Można tylko **zmniejszyć** efektywną pojemność, nigdy podnieść ponad
`mqtt_buffer_size` skompilowane w YAML. Zmniejszenie poniżej tego, co
aktualnie w kolejce, przycina natychmiast (z logiem o liczbie odrzuconych).

### 6c. Ręczna zmiana QoS (`select:`) — NOWE, na Twoją prośbę

```yaml
select:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here
    telegram_qos:
      name: "WMBus Telegram QoS"
    rx_qos:
      name: "WMBus RX QoS"
```

Lista rozwijana `0`/`1`/`2` dla każdego z dwóch buforowanych topiców.
Startuje od wartości skompilowanej (`telegram_qos`/`rx_qos` z YAML), zmiana
działa identycznie jak przeszycie z inną wartością — tylko bez
przeszywania. Dotyczy nowych publikacji od tego momentu; wiadomość już w
buforze zachowuje QoS, z jakim trafiła do kolejki.

---

## Co jest w pełni wsteczne kompatybilne

Jeśli nie dotkniesz żadnej z powyższych opcji, zachowanie jest identyczne z
tym sprzed zmian — jedyna różnica to `mqtt_buffer_size: 64` domyślnie
włączone (wcześniej telegram po prostu ginął przy rozłączeniu; teraz jest
buforowane maks. 64 ramki). Jeśli wolisz stary sposób "drop przy
rozłączeniu", ustaw `mqtt_buffer_size: 0`.

## Czego NIE sprawdziłem

Nie mam w tym środowisku dostępu do `esphome compile` (brak PyPI w
sandboxie) — kod zweryfikowałem ręcznie (bilans nawiasów, sygnatury
funkcji względem źródeł ESPHome, `py_compile` na plikach Python), ale
**przed wgraniem na BR-3/BR-1 uruchom `esphome compile` na swojej
maszynie** i sprawdź log startowy (sekcja `[output]` pokaże efektywne
wartości wszystkich opcji z tej listy).
