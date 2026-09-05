# Bufor MQTT w RAM, QoS per temat i znacznik czasu odbioru

[English version](BUFFER_QOS_TIMESTAMP.md)

Ten dokument opisuje trzy elementy przygotowane przy migracji odbiornika
z `SzczepanLeon/esphome-components@version_4` do mostu RAW-only:

1. bufor RAM przechowujący wiadomości podczas rozłączenia MQTT i wysyłający je po powrocie połączenia,
2. QoS MQTT (0/1/2) konfigurowalny w YAML dla poszczególnych grup tematów,
3. potwierdzenie, że znacznik czasu odbioru już towarzyszy RSSI — nowe pole nie było potrzebne.

## 1. Bufor RAM (`mqtt_buffer_size`)

Przed tą zmianą odbiór RF trwał podczas niedostępności lokalnego brokera,
ale publikacje MQTT były pomijane. Telegram ginął, zamiast zostać opóźniony.

`mqtt_buffer_size` (**domyślnie `0` — funkcja jest wyłączona, dopóki jej nie
ustawisz**, zakres `0`–`8192` albo `auto`) ustawia kolejkę FIFO w RAM.

**Jednostką są wiadomości MQTT, nie telegramy.** Jeden odebrany telegram
tworzy dwie wiadomości: surową ramkę na `.../telegram` i metadane na
`.../rx`, publikowane dla każdej przekazywanej ramki. Dlatego
`mqtt_buffer_size: 64` przechowuje około **32 telegramów**. Sensory
`buffer_depth` / `buffer_dropped_*` i przydziały `buffer_priority` również
liczą wiadomości. Usuwanie działa na pojedynczych wiadomościach: pełny bufor
może usunąć telegram, pozostawiając jego metadane, których backend nie
połączy wtedy z ramką.

Domyślne `0` zachowuje dotychczasowe działanie. Bufor zmienia sposób
dostarczania danych: po ponownym połączeniu odtwarza serię telegramów
z `received_at` sprzed kilku minut. Na płytkach bez PSRAM zużywa też pamięć
wewnętrzną. Włączenie go jest więc jawną decyzją w YAML.

Podczas rozłączenia MQTT surowy telegram i jego metadane `/rx` trafiają do
kolejki. Po powrocie połączenia są wysyłane od najstarszej wiadomości, po
kilka na cykl `loop()`, aby zaległości nie zagłodziły odbioru radiowego.
Pełny bufor usuwa najstarszą wiadomość, robiąc miejsce dla nowszej — długa
awaria zachowuje najświeższe odczyty, a nie tylko pierwsze otrzymane.

Celowo **nie są buforowane**: zachowywane przez broker RSSI per licznik
(`wmbus/<topic>/rssi/<meter_id>`), komunikaty health/meters,
podsumowania i sugestie diagnostyczne, temat debugowania wskazanego
licznika oraz surowy strumień deweloperski. Są to ostatnie wartości,
dane bieżącego okna albo pomocnicza diagnostyka; późniejsze odtworzenie
nieaktualnych wartości mogłoby wprowadzać odbiorcę w błąd.

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: 64   # 0 wyłącza buforowanie: wiadomości giną podczas rozłączenia
```

### Sterowanie podczas pracy bez ponownego wgrywania firmware

`mqtt_buffer_size` ustala skonfigurowany limit. Aby obserwować i zmieniać
pojemność podczas pracy, zadeklaruj opcjonalne platformy
`sensor:`/`number:` oraz wbudowany `web_server:` ESPHome z uwierzytelnianiem.
Komponent nie dodaje własnego serwera HTTP ani obsługi logowania.

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

`buffer_depth` pokazuje liczbę oczekujących wiadomości,
`buffer_dropped_total` liczy odrzucone od startu, a
`buffer_oldest_pending_age` podaje wiek najstarszej oczekującej w sekundach.

Encja `number` może zmniejszyć efektywną pojemność, ale nie przekroczy
skonfigurowanego limitu (w trybie `auto`: bieżącego wyliczonego limitu).
Kod C++ przycina wartość i publikuje ją ponownie, więc portal pokazuje
rzeczywistą dopuszczalną pojemność.

### Jeden wspólny bufor czy osobny dla każdego licznika?

Wybór zależy od listy `forward_meters`:

- **Bez listy dozwolonych liczników** (domyślnie pusta): wspólna kolejka FIFO.
  Gdy jest pełna, usuwa najstarszą wiadomość niezależnie od licznika.
- **Z listą dozwolonych liczników:** każdy licznik ma ważony przydział,
  egzekwowany dopiero po zapełnieniu całej kolejki. Licznik może korzystać
  z niewykorzystanego miejsca innych. Aby zrobić miejsce, kolejka usuwa
  najstarszą wiadomość licznika najbardziej przekraczającego swój przydział;
  jeśli żaden go nie przekracza, usuwa globalnie najstarszą wiadomość.

### Priorytet per licznik (`buffer_priority`)

Domyślnie każdy licznik z listy ma równy udział. `buffer_priority` pozwala
przyznać większy udział wybranym licznikom, np. słabo słyszalnemu licznikowi
na klatce schodowej w porównaniu z licznikiem przy antenie.

```yaml
wmbus_radio:
  # ...
  forward_meters: [12345678, 87654321, "0x417F0666"]
  buffer_priority:
    "12345678": 3   # udział 3 razy większy niż przy domyślnej wadze
    "87654321": 1   # tak samo jak pominięcie wpisu
    # pominięty "0x417F0666" także otrzymuje wagę 1
```

**Wagi zamiast procentów.** Wagi są dodatnimi liczbami całkowitymi i nie
muszą sumować się do 100. Dokładny udział to
`capacity * weight / sum(all weights)`. Kod zaokrągla udziały w dół,
a pozostałe miejsca rozdziela po jednym według największych reszt
ułamkowych. To metoda największej reszty (Hamiltona), dzięki której
przydziały sumują się **dokładnie** do bieżącej pojemności. Pominięty
licznik ma wagę 1; nie traci przez to udziału.

Przydziały są przeliczane po zmianie pojemności: przy automatycznych
kontrolach co około 60 sekund albo po zmianie `buffer_capacity`.
Samo przekroczenie nowego przydziału nie usuwa pożyczonych miejsc —
usuwane są dopiero wiadomości ponad nową globalną pojemność. Przydziały
są logowane przy starcie jako
`MQTT outbox per-meter quotas (id:weight->slots)`.

Bez listy `forward_meters` nie ma stałego zbioru liczników, między które
można podzielić pojemność. Ustawione wtedy `buffer_priority` jest
ignorowane z ostrzeżeniem przy starcie.

### Dobór pojemności z wolnego RAM (`mqtt_buffer_size: auto`)

Wcześniejszy limit 256 ramek był okrągłą liczbą, a nie wynikiem pomiaru
wolnej pamięci. Zwykły ESP32 bez PSRAM, np. Olimex ESP32-POE, ma zaledwie
kilkaset KB pamięci współdzielonej z WiFi/Ethernet/MQTT/TLS.

`mqtt_buffer_size: auto` zastępuje stałą liczbę wynikiem obliczanym na płytce:

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: auto
```

Obliczenia wykonuje `Radio::suggested_mqtt_outbox_capacity_()`
w `mqtt_outbox.cpp`:

- **Bez PSRAM:** rezerwa 40 KB pamięci wewnętrznej, budżet 25% wolnej
  pamięci ponad rezerwę, szacunkowo 400 B na wiadomość, wynik ograniczony
  do 4–512 wiadomości.
- **Z PSRAM:** rezerwa 256 KB PSRAM i 24 KB pamięci wewnętrznej.
  Budżet 50% wolnego PSRAM ponad rezerwę przy 400 B na wiadomość,
  dodatkowo ograniczony wolną pamięcią wewnętrzną przy około 40 B na wpis
  kolejki. Mniejszy wynik jest ograniczany do 4–4096 wiadomości.
  Temat i zawartość wiadomości korzystają z `RAMAllocator<char>`:
  najpierw PSRAM, z możliwością użycia pamięci wewnętrznej w razie
  niepowodzenia. Struktura kolejki nadal używa pamięci wewnętrznej.

Są to szacunki, nie gwarancja zmieszczenia danej liczby wiadomości.
Pierwsze obliczenie odbywa się przy starcie, kolejne kontrole co około
60 sekund. Zmiany do 15% są pomijane, chyba że proponowany limit jest
mniejszy od bieżącej długości kolejki. Automatyczna zmiana aktualizuje
zarówno limit, jak i efektywną pojemność, więc może zastąpić ręczne
ustawienie pojemności podczas pracy.

**Zmniejszenie pełnego bufora:** wiadomości ponad nową globalną pojemność
są usuwane od razu, od najstarszej bez listy liczników albo według
opisanych wyżej przydziałów z listą. Samo przekroczenie indywidualnego
przydziału nie usuwa danych. Automatyczne zmniejszenie loguje liczbę
usuniętych wiadomości (`MQTT outbox auto-size shrink dropped ...`).
Ręczne zmniejszenie `buffer_capacity` poniżej długości kolejki także
przycina ją natychmiast.

**Zabezpieczenie pamięci działa przy stałym limicie i w trybie automatycznym.**
Odmawia powiększenia kolejki, jeśli wolna pamięć wewnętrzna spadnie poniżej
40 KB na płytce bez PSRAM, albo wolny PSRAM spadnie poniżej 256 KB lub
pamięć wewnętrzna poniżej 24 KB na płytce z PSRAM. Ostrzeżenie jest logowane
najwyżej raz na minutę. Minimalny nominalny limit czterech wiadomości nie
omija tej ochrony — bufor może nadal odmówić przyjęcia wiadomości.

**Fragmentacja jest sprawdzana osobno od sumy wolnej pamięci.** WiFi, lwIP
i esp-tls potrzebują ciągłych bloków (sam rekord TLS to około 16 KB).
Setki małych alokacji podczas długiej awarii mogą pozostawić dużo wolnej
pamięci, ale zbyt mały największy blok do ponownego połączenia.
Zabezpieczenie odmawia buforowania również wtedy, gdy
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` spadnie poniżej
16 KB. Log: `MQTT outbox: internal heap too fragmented, refusing to buffer`.
Odczyty odbywają się na tym samym takcie 1 Hz co pomiar wolnej pamięci,
aby ograniczyć zajmowanie blokady sterty potrzebnej zadaniu `radio_recv`.
Kontrola obejmuje też ścieżkę PSRAM, gdzie w pamięci wewnętrznej pozostają
głównie około 40-bajtowe wpisy kolejki — wymóg ciągłego bloku nadal obowiązuje.

## 2. QoS per temat

Wcześniej komponent używał QoS 0 dla wszystkich publikacji poza `/rx`,
które miało QoS 1. Pięć opcji konfiguruje teraz QoS grup tematów,
domyślnie zachowując wcześniejsze wartości:

```yaml
wmbus_radio:
  # ...
  telegram_qos: 0       # wmbus/<topic>/telegram
  rssi_qos: 0           # wmbus/<topic>/rssi/<meter_id>
  health_qos: 0         # wmbus/<topic>/health i /meters
  diagnostic_qos: 0     # wszystkie tematy wmbus/<topic>/diag/...:
                       # boot, config, podsumowania, sugestie, okna liczników,
                       # drop/truncated, busy_ether_changed
  rx_qos: 1             # wmbus/<topic>/rx (rssi_dbm + received_at)
```

Tabela QoS ze specyfikacji migracji (rozdział 17, tabela 26) zaleca QoS 1
tam, gdzie potrzebne jest dostarczenie co najmniej raz, a odbiorca
potrafi usuwać duplikaty po `raw_hash`/numerze dostępu. QoS 2 nie jest
wymagane w tym systemie. Po włączeniu bufora rozsądnym punktem startowym
jest `telegram_qos: 1`, zgodne z domyślnym `rx_qos`. Pozostałe wartości
zostaw na 0, chyba że konkretny odbiorca wymaga więcej.

### Czy QoS działa również przy przejściu przez bufor RAM?

Tak. `enqueue_or_publish_()` zapisuje rozstrzygnięte
`telegram_qos`/`rx_qos` w samej wiadomości (`OutboxMsg::qos`).
Publikacja natychmiastowa albo po ponownym połączeniu używa tej zapisanej
wartości. Buforowanie zmienia czas dostarczenia, a nie QoS.
Wynika to z przechowywania pełnego zestawu temat/zawartość/QoS/retain
dla każdej wiadomości, bez wymuszania wspólnego QoS kolejki.

### QoS podczas pracy (`select:` `telegram_qos` / `rx_qos`)

Opcje YAML określają wartości początkowe. Aby zmieniać QoS dwóch
buforowanych tematów między `0`, `1` i `2` bez ponownego wgrywania
firmware, zadeklaruj opcjonalną platformę `select:` i ten sam
uwierzytelniany `web_server:`, którego używa `buffer_capacity`:

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

Każda encja jest listą rozwijaną `0`/`1`/`2`, startuje od wartości YAML
i wywołuje te same settery `set_telegram_qos()`/`set_rx_qos()`.
Zmiana działa jak nowa wartość YAML bez ponownego wgrania firmware.
Dotyczy wyłącznie **nowych** publikacji: wiadomości już oczekujące
zachowują QoS zapisane podczas dodawania do kolejki.

## 3. Znacznik czasu odbioru: już istnieje obok RSSI

Nie było potrzeby zmiany kodu. Każdemu telegramowi przekazywanemu na
`wmbus/<topic>/telegram` już towarzyszy JSON na `wmbus/<topic>/rx`
(z QoS określanym przez `rx_qos`), zawierający oba pola:

```json
{"schema":1,"boot_id":"...","seq":1,"rx_task_wakeup_us":...,
 "meter_id":"...","mode":"T1","rssi_dbm":-78,"frame_crc32":"...",
 "frame_length":32,"received_at":"2026-08-28T10:15:02.421Z"}
```

`received_at` określa chwilę **odbioru**, nie publikacji — przy buforowaniu
to różne momenty. Jest wyliczane z czasu monotonicznego podczas odbioru.
Pojawia się po synchronizacji SNTP; wcześniej klucz jest nieobecny,
zamiast zawierać rok 1970 albo czas działania udający datę. Odbiorca
powinien rozumieć brak klucza jako brak wiarygodnego znacznika czasu.

Zawartość wiadomości powstaje przy odbiorze, przed publikacją albo dodaniem
do kolejki, więc `received_at` zachowuje rzeczywistą chwilę odbioru także
po oczekiwaniu w RAM.
