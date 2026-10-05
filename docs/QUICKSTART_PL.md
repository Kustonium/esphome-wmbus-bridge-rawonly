# Szybki start: liczniki w Home Assistant w 15 minut

[English version](QUICKSTART.md)

Dla kogo: masz Home Assistant OS (albo Supervised), kilka liczników w domu i jedną płytkę z radiem.
Nie musisz znać nazw driverów, pól ani pisać YAML-a od zera.

```text
licznik -> płytka ESP (odbiera) -> MQTT -> dodatek w HA (dekoduje) -> encje w Home Assistant
```

Płytka tylko odbiera. Drivery, klucze AES i encje są w dodatku — dodanie licznika albo zmiana
klucza to kliknięcie, bez ponownego wgrywania firmware.

## Czego potrzebujesz

- Home Assistant OS z dodatkiem **ESPHome**,
- jedną z płytek z tabeli niżej,
- numery liczników albo przynajmniej ich odczyty z wyświetlacza,
- klucze AES od dostawcy, jeśli liczniki szyfrują.

| Płytka | Przykład YAML |
|---|---|
| **XIAO ESP32-S3 + Wio-SX1262** (polecana na start) | [`examples/SX1262/XIAO ESP32 S3/xiao_esp32_s3_clean.yaml`](../examples/SX1262/XIAO%20ESP32%20S3/xiao_esp32_s3_clean.yaml) |
| Heltec WiFi LoRa 32 V4 | [`examples/SX1262/Heltec V4/`](../examples/SX1262/Heltec%20V4/) |
| Heltec WiFi LoRa 32 V4-R8 | [`examples/SX1262/Heltec V4-R8/`](../examples/SX1262/Heltec%20V4-R8/) |
| Heltec WiFi LoRa 32 V3 | [`examples/SX1262/Heltec V3/`](../examples/SX1262/Heltec%20V3/) |
| Heltec V2 / LilyGO T3 (SX1276) | [`examples/SX1276/`](../examples/SX1276/) |
| ESP32 + moduł CC1101 (eksperymentalnie, patrz niżej) | [`examples/CC1101/CC1101_clean.yaml`](../examples/CC1101/CC1101_clean.yaml) |

Piny, przełączniki RF i zasilanie modułu są w przykładach już ustawione. Nie przepisuj ich z innej
płytki — Heltec V4 to nie V3.

### CC1101: tani, działa, eksperymentalny

Moduł CC1101 na dowolnym ESP32 działa z tym projektem, a reszta tego przewodnika jest dla niego
taka sama. Przed zakupem albo podłączeniem trzeba wiedzieć trzy rzeczy:

1. **Kup wersję 868 MHz.** Wiele tanich modułów CC1101 jest zrobionych na 433 MHz - sam układ
   dostroi się do 868 MHz, ale antena i elementy dopasowania już nie, i płytka jest prawie głucha.
   Szukaj „868 MHz” na samym module, nie tylko w tytule ogłoszenia.
2. **Podłącz GDO0 i GDO2**, do tego SPI i CS, zasilanie 3,3 V. Przykład używa ESP32-C3:

   | CC1101 | ESP32-C3 (przykład) |
   |---|---|
   | SCK | GPIO5 |
   | MOSI (SI) | GPIO6 |
   | MISO (SO) | GPIO7 |
   | CSN | GPIO4 |
   | GDO0 | GPIO3 |
   | GDO2 | GPIO2 |
   | VCC / GND | 3,3 V / GND |

   Inne warianty ESP32 powinny działać po zmianie pinów w YAML-u, ale w CI budowany jest tylko
   ESP32-C3. Zapewnij modułowi stabilne 3,3 V: moduł pracujący na 2,93 V gubił większość zapisów
   rejestrów, dopóki sterownik nie zaczął ich powtarzać.
3. **Sterownik trzeba włączyć jawnie** przez `cc1101_allow_experimental: true` (jest już
   w przykładzie). Jest eksperymentalny, bo nie był mierzony na stanowisku tego projektu: dowody
   sprzętowe pochodzą dotąd z logów użytkowników. Znaleziony w ten sposób problem z zapisem
   rejestrów został naprawiony.

Czego się spodziewać: użytkownicy używają go na **T1**, ale nie ma tu porównania CC1101 z innymi
radiami, więc nie ma liczb do obiecania. To najwolniejsze z czterech radiów przy odczycie ramek
(termin FIFO 1800 µs wobec 1000 µs na SX1276), co ma największe znaczenie przy wielu szybkich
licznikach, a S1 na CC1101 to surowy sniffer, nie ścieżka odbiorcza. Raport z wynikami
w Discussions pomoże bardziej niż cokolwiek innego. Jeśli i tak
kupujesz nowy sprzęt, najpierw zajrzyj do [`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md); jeśli
CC1101 już masz, używaj go.

## 1. Broker MQTT

Jeśli masz już MQTT w Home Assistant — pomiń.

1. **Ustawienia → Dodatki → Sklep z dodatkami → Mosquitto broker** → Zainstaluj → Uruchom.
2. **Ustawienia → Urządzenia i usługi** → integracja **MQTT** pojawi się sama → Skonfiguruj.
3. Załóż w HA zwykłego użytkownika (np. `mqtt`) — jego login i hasło wpiszesz w płytce.

## 2. Dodatek wMBus MQTT Bridge

[![Dodaj repozytorium do Home Assistant](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FKustonium%2Fhomeassistant-wmbus-mqtt-bridge)

1. Kliknij przycisk wyżej (albo dodaj repozytorium `https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge` ręcznie).
2. Zainstaluj **wMBus MQTT Bridge** → Uruchom. Listę liczników zostaw pustą.

Dodatek sam znajdzie brokera Mosquitto.

## 3. Płytka

1. W ESPHome: **+ New device** → wklej przykład `*_clean.yaml` dla swojej płytki.
2. W **Secrets** (prawy górny róg ESPHome) uzupełnij:

   ```yaml
   wifi_ssid: "twoja_siec"
   wifi_password: "haslo_wifi"
   mqtt_broker: "192.168.1.10"   # adres IP Home Assistant
   mqtt_user: "mqtt"             # użytkownik z kroku 1
   mqtt_password: "haslo_mqtt"
   ```

3. **Install** — pierwszy raz przez USB, potem już bezprzewodowo.
4. W logu płytki powinny pojawiać się linie `Have data ... id:XXXXXXXX`. To odebrane liczniki —
   twoje i sąsiadów.

## 4. Dodanie liczników

1. W dodatku: **OPEN WEB UI** → widok **Odbierane / Szukaj**.
2. Zobaczysz słyszane liczniki z sugerowanym driverem i — dla nieszyfrowanych — bieżącą wartością.
3. W bloku słychać dziesiątki cudzych liczników. Wpisz w **Filtruj po wartości** stan z wyświetlacza
   swojego licznika — zostaną tylko pasujące.
4. **Dodaj licznik** → potwierdź driver → wpisz klucz AES, jeśli licznik szyfruje.
5. Powtórz dla każdego licznika.

Encje pojawią się w Home Assistant same (MQTT Discovery), ze wszystkimi polami, jakie podaje driver.

## Nie działa?

| Objaw | Co sprawdzić |
|---|---|
| W logu płytki brak `Have data` | właściwy przykład dla płytki, antena podłączona, płytka nie leży przy routerze |
| Płytka odbiera, a dodatek nic nie widzi | dane MQTT w Secrets, czy broker działa, czy płytka jest połączona z MQTT |
| Licznik C1 (np. część Techem) nie pojawia się | w YAML płytki zmień `listen_mode: t1` na `listen_mode: both` |
| Licznik widać, ale bez wartości | licznik szyfruje — potrzebny klucz AES od dostawcy |
| Długi telegram (licznik prądu 3-fazowy) nie przechodzi | `long_gfsk_packets: true` na SX1262 — tylko wtedy, kosztuje czułość |

Więcej: [`START_HERE_PL.md`](START_HERE_PL.md) (pełna ścieżka i diagnostyka RF),
[`TROUBLESHOOTING_PL.md`](TROUBLESHOOTING_PL.md), dokumentacja
[dodatku](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge).

Home Assistant w Dockerze (bez dodatków): płytka działa tak samo, ale `wmbusmeters` musisz
uruchomić sam — patrz README dodatku, sekcja Docker.
