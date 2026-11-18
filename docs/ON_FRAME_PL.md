# Automatyzacja on_frame

[English version](ON_FRAME.md)

`on_frame` to opcjonalny trigger automatyzacji ESPHome, który odpala się za każdym razem
gdy radio odbierze poprawną, przeszłą CRC ramkę wMBus. Daje bezpośredni dostęp do
surowych bajtów ramki wewnątrz bloku `lambda:` — przed (lub zamiast) wbudowanym
publikowaniem MQTT.

## Kiedy używać

Użyj `on_frame` gdy potrzebujesz niestandardowego przetwarzania, którego wbudowane
przekierowanie MQTT nie obsługuje — na przykład wysyłania ramek przez socket TCP,
filtrowania po ID licznika w firmware, albo lokalnego logowania wybranych pól.

Dla większości instalacji wbudowane przekierowanie przez `topic_name` / `telegram_topic`
w zupełności wystarczy i `on_frame` nie jest potrzebne.

## Podstawowa składnia

```yaml
wmbus_radio:
  radio_type: SX1276
  # ... inne opcje ...
  on_frame:
    - then:
        - lambda: |-
            std::string hex = frame->as_hex();
            ESP_LOGI("on_frame", "hex: %s  RSSI: %d dBm", hex.c_str(), (int)frame->rssi());
```

## Metody obiektu frame

Wewnątrz bloku `lambda:` zmienna `frame` to wskaźnik na odebraną ramkę.

| Metoda | Typ zwracany | Opis |
|---|---|---|
| `frame->as_hex()` | `std::string` | Pełna ramka jako hex string (wielkie litery) — gotowa do publikacji na MQTT lub przekazania do wmbusmeters |
| `frame->as_raw()` | `std::vector<uint8_t>` | Surowe bajty ramki |
| `frame->as_rtlwmbus()` | `std::string` | Ramka w formacie tekstowym rtl-wmbus |
| `frame->rssi()` | `int8_t` | RSSI w dBm w chwili odbioru ramki |
| `frame->link_mode()` | `LinkMode` | `LISTEN_MODE_T1`, `LISTEN_MODE_C1`, `LISTEN_MODE_S1` |
| `frame->format()` | `std::string` | String formatu ramki, np. `"T1 A"` |
| `frame->try_get_meter_id(uint32_t &id)` | `bool` | Wyciąga ID licznika z ramki; zwraca `false` jeśli wyciągnięcie się nie powiodło |
| `frame->mark_as_handled()` | `void` | Liczy ramkę jako obsłużoną (log: „obsłużony przez N handlerów”); **nie** wyłącza publikacji MQTT |

## mark_as_handled

`mark_as_handled: true` w bloku triggera tylko liczy ramkę jako obsłużoną: linia
logu po triggerach mówi „Telegram obsłużony przez N handlerów” zamiast „nie obsłużony
przez żaden handler”. **Nie** wyłącza wbudowanej publikacji MQTT. Ta publikacja
odbywa się przed triggerami `on_frame`, więc gdy lambda dostaje ramkę, ta już wyszła.

Żeby wyłączyć wbudowaną publikację:

- `repeater: true` - bez MQTT w ogóle; ramki wychodzą tylko przez `on_frame` (płytka,
  która je przekazuje dalej, np. przez ESP-NOW, i może nie mieć sekcji `mqtt:`);
- `forward_meters:` - publikuj tylko wymienione liczniki.

```yaml
on_frame:
  - mark_as_handled: true
    then:
      - lambda: |-
          // ramka liczona jako obsłużona; publikacja MQTT już się odbyła
          std::string hex = frame->as_hex();
```

Wcześniejsze wersje tej strony podawały, że `mark_as_handled` wyłącza publikację MQTT. Kod nigdy tego nie robił; strona była błędna (poprawione 17.11.2026).

## Filtrowanie po ID licznika

```yaml
on_frame:
  - then:
      - lambda: |-
          uint32_t id = 0;
          if (!frame->try_get_meter_id(id)) return;
          if (id != 0x12345678) return;
          ESP_LOGI("on_frame", "docelowy licznik: %s  RSSI: %d", frame->as_hex().c_str(), (int)frame->rssi());
```

## Wysyłanie przez socket TCP (socket_transmitter)

Jeśli komponent `socket_transmitter` jest załadowany, możesz użyć akcji
`wmbus_radio.send_frame_with_socket`:

```yaml
on_frame:
  - wmbus_radio.send_frame_with_socket:
      id: my_socket
      format: hex       # hex | raw | rtlwmbus
```

## Wiele triggerów

Obsługiwanych jest wiele triggerów `on_frame`. Każdy odpala się niezależnie dla każdej ramki.

```yaml
on_frame:
  - then:
      - lambda: |-
          ESP_LOGI("on_frame", "trigger 1: %s", frame->as_hex().c_str());
  - mark_as_handled: true
    then:
      - lambda: |-
          ESP_LOGI("on_frame", "trigger 2 (obsłużony)");
```

## Uwagi

- `on_frame` odpala się tylko dla ramek, które przeszły walidację CRC. Pakiety
  odrzucone/skrócone nie wyzwalają triggera.
- Lambda działa w tasku głównej pętli ESPHome `loop()`, nie w tasku odbiornika radiowego.
- Wskaźnik `frame` jest ważny tylko przez czas wykonania lambdy. Nie przechowuj go.
