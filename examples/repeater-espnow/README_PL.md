# Przedłużacz ESP-NOW: odbiornik bez sieci domowej

[English version](README.md)

Odbiornik stoi przy licznikach (piwnica, garaż, klatka schodowa), poza zasięgiem
Twojego Wi-Fi. Druga płytka ESP32, która jest w sieci, odbiera od niego ramki przez
ESP-NOW i publikuje je do MQTT w jego imieniu. Dodatek widzi odbiornik jak zwykłą
płytkę. Po żadnej stronie nic się nie wpisuje: ani adresów MAC, ani kanału.

```
liczniki --868 MHz--> PŁYTKA (radio wM-Bus, bez sieci) --ESP-NOW 2,4 GHz--> MOST (Wi-Fi domowe) --MQTT--> dodatek
```

Sprawdzone na stanowisku z ok. 70 prawdziwymi licznikami (ok. 1800 ramek/h
w dzień): 7,5 h i 13 344 ramki bez straty; most odpięty na 100 s bez utraty ramki.
Bufor powiększono potem ze 100 do 1500 ramek, po tym jak 15-minutowa przerwa mostu
kosztowała przy małym buforze 320 ramek.

## Pliki

| Plik | Co to jest |
|---|---|
| `node_*.yaml` | odbiornik (PŁYTKA) dla każdej płytki z przykładów, wygenerowany z jej `examples/.../*_clean.yaml` |
| `repeater_node_common.yaml` | część wspólna płytek: hotspot, strona stanu, wgrywanie wsadu, sensory bufora |
| `repeater_node.h` | logika płytki: parowanie, bufor, dostarczanie |
| `bridge_esp32c3.yaml` | MOST, dowolny ESP32 / S3 / C3 w sieci domowej, bez radia |
| `repeater_bridge.h` | logika mostu: przyjmowanie płytek, publikacja, potwierdzenia |

YAML płytki, `repeater_node_common.yaml` i `repeater_node.h` skopiuj do jednego
folderu w ESPHome; tak samo YAML mostu z `repeater_bridge.h`.

W `secrets.yaml`, poza zwykłymi wpisami:

```yaml
repeater_ap_password: "co najmniej 8 znaków"   # hotspot płytki
```

Nie ma pliku płytki dla LilyGO T3 V3 TCXO: jej przykład się nie waliduje
(`cs_pin: GPIO7` to na ESP32 nóżka pamięci flash), więc wygenerowana z niego
płytka też by się nie walidowała.

## Uruchomienie

1. **Wgraj most** i sprawdź log: Wi-Fi i MQTT połączone.
2. **Wgraj płytkę.** Po tym wgraniu płytka **nie jest już w Twojej sieci**:
   ESPHome / Home Assistant nie zaktualizuje jej więcej. Aktualizacje idą przez jej
   własny hotspot (niżej) albo USB. Wgraj ją, póki jest w zasięgu komputera, albo
   przez USB.
3. Włącz płytkę. Po kilku sekundach w logu mostu pojawi się `new node …` i
   `node ACCEPTED …`, potem `<nazwa> seq=1 …` przy każdej ramce. W dodatku płytka
   pojawi się pod swoją nazwą (`wmbus/<nazwa płytki>/…`).

Nazwą płytki w dodatku jest jej `esphome.name`.

## Na miejscu, z telefonem

Płytka zawsze wystawia hotspot nazwany jak ona (`esphome-wmbus-heltec-v4`, …).
Połącz telefon albo laptopa i otwórz `http://192.168.4.1`:

| Encja | Znaczenie |
|---|---|
| **Bridge** | `CONNECTED - channel 6, link -72 dBm`, `LOST for N s - X frames waiting`, `bridge WITHOUT MQTT`, `looking for the bridge…`, `NOT FOUND…` (po polsku z `log_language: pl`) |
| **Last meters** | 5 ostatnich liczników z RSSI i czasem |
| **Link to bridge** | sygnał ESP-NOW od mostu |
| **Frames delivered** | opublikowane przez most do MQTT |
| **Buffered / Buffer peak / Buffer capacity** | czeka teraz / najwięcej dotąd / pojemność |
| **Frames lost (buffer full)** | jedyny licznik prawdziwych strat |
| **Send failures (retried)** | ruch na łączu, nie straty |
| **OTA Update** | wgranie pliku wsadu (ESPHome: *Install → Manual download → format OTA*). Po wgraniu strona zostaje na „Update Successful!”; wróć na `http://192.168.4.1/` |

Z telefonem obchodzisz piwnicę i szukasz miejsca, gdzie działają i liczniki, i łącze
z mostem.

Progi łącza w przybliżeniu: powyżej −80 dBm komfort, −80…−88 dBm dobrze, poniżej
−90 dBm rosną nieudane wysyłki; dopóki „Frames lost” stoi na 0, nic nie ginie.

## Jak to działa

**Parowanie.** Płytka przeskakuje kanały 1–13 i na każdym woła `HELLO`. Most stoi
na kanale routera i odpowiada: ten kanał, czy płytka jest przyjęta, aktualny czas
i czy ma MQTT. Płytka zapamiętuje kanał (także we flashu, więc kolejny start zaczyna
od niego) i co 15 s wysyła `HELLO` jako sprawdzenie łącza. Gdy router zmieni kanał,
płytka znajdzie go w ciągu 2 minut. Gdy telefon jest w hotspocie, płytka nie zmienia
kanału.

**Przyjmowanie płytek.** Pierwsza płytka, która powie `HELLO`, zostaje przyjęta
i zapamiętana we flashu. Inne widać jako „Other nodes heard”. Przyciski: *Accept
strongest node*, *Forget node*. Płytka sąsiada nie jest przekazywana.

**Dostarczanie.** Każda ramka trafia najpierw do bufora płytki. Schodzi z niego
dopiero, gdy most potwierdzi, że **opublikował ją w MQTT** — samemu potwierdzeniu
radiowemu ESP-NOW płytka nie ufa, bo most tuż po restarcie odbiera ramki, zanim ma
MQTT. Bez MQTT most niczego nie potwierdza, a płytka buforuje (`bridge WITHOUT
MQTT`). Trzy nieudane wysyłki pod rząd oznaczają utratę łącza od razu; zapytanie co
2 s przywraca łącze. Gdy potwierdzenie zginie po drodze, płytka wyśle ramkę
ponownie, a most rozpozna ją po numerze i potwierdzi bez drugiej publikacji.

**Bufor.** 1500 ramek w PSRAM (ok. 370 kB): ok. 50 minut dziennego ruchu przy
ok. 70 licznikach, w nocy kilka godzin. Płytki bez PSRAM mają 100 ramek w RAM.
Pełny bufor wyrzuca najstarszą ramkę.

**Numeracja.** Ramki są numerowane od 1. Utracona ramka to luka w `seq` w dodatku;
restart płytki zaczyna nowy `boot_id`. Most ustawia też `received_at` na chwilę, gdy
płytka odebrała ramkę (zegar płytka dostaje od mostu), więc ramka, która czekała
w buforze, ma swój prawdziwy czas.

**Statystyki.** Most co 60 s publikuje `wmbus/repeater/link`: `rx`, `lost` (luki
w seq), `dup` (rozpoznane ponowienia), `foreign`, RSSI łącza oraz liczniki samej
płytki `a_dropped`, `a_send_fail`, `a_queue` (przeżywają restart mostu).

**Zrzuty ekranu.** Obie płytki mają przełącznik *Anonymize*: numery liczników
w logu i na stronie są zamazane (ta sama metoda co `set_log_anonymize`, patrz
[DIAGNOSTIC](../../docs/DIAGNOSTIC_PL.md)). MQTT dostaje prawdziwe numery.

## Zasięg

ESP-NOW nadaje tu z prędkością 1 Mb/s, najczulszą w zwykłym Wi-Fi. Z łącza
zmierzonego na miejscu można szacować: każde podwojenie odległości kosztuje ok. 6 dB
w otwartej przestrzeni i ok. 9 dB w budynku; ściana działowa 3–5 dB, ceglana
6–10 dB, strop żelbetowy 15–25 dB. W praktyce: w obrębie domu przez kilka ścian,
jedno piętro przez klatkę schodową; zewnętrzna antena 2,4 GHz na którejkolwiek
płytce (XIAO ESP32-S3, niektóre wersje T-Beam) dodaje 3–6 dB.

## Ograniczenia

- **Jedna płytka na most.** Most przekazuje jedną przyjętą płytkę.
- **Ramki do 239 bajtów** (ESP-NOW mieści 250, minus nagłówek). Ramki wM-Bus T1/C1
  na typowym osiedlu są dużo krótsze.
- **Bez szyfrowania.** Szyfrowanie ESP-NOW jest wyłączone. Liczniki i tak nadają
  otwarcie w eterze; liczniki z AES zostają zaszyfrowane od początku do końca.
- **Strona płytki nie ma logowania**; chroni ją hasło hotspotu, łącznie z wgrywaniem
  wsadu.
- Wymaga komponentu z `repeater:` (`wmbus_radio` z `repeater: true` nic nie
  publikuje przez MQTT; patrz [CONFIG_REFERENCE_MINIMAL](../../docs/CONFIG_REFERENCE_MINIMAL_PL.md)).
