# Bezpieczeństwo

[English version](SECURITY.md)

## Zgłaszanie podatności

Problemy bezpieczeństwa zgłaszaj prywatnie przez GitHub: **Security → Report a vulnerability** w tym repozytorium. Nie opisuj szczegółów nienaprawionej podatności w publicznym zgłoszeniu. To projekt rozwijany po godzinach (zobacz [SUPPORT_PL.md](SUPPORT_PL.md)): odpowiadam najszybciej, jak mogę, bez gwarantowanego czasu reakcji.

## Co wysyła płytka i w jakiej postaci

`wmbus_radio` działa w trybie RAW-only: płytka odbiera ramki wM-Bus i publikuje je w MQTT; nie dekoduje liczników i **nie przechowuje kluczy AES liczników**. Dekodowanie i klucze zostają po stronie `wmbusmeters`, poza płytką.

Co idzie przez MQTT:

| Temat | Zawartość |
|---|---|
| `wmbus/<płytka>/telegram` | ramka w postaci odebranej (hex): numer, producent i typ licznika zawsze czytelne; same odczyty czytelne dla liczników bez szyfrowania, zaszyfrowane dla liczników z AES |
| `wmbus/<płytka>/rx` | metadane ramki: numer licznika, RSSI, czas odbioru, suma kontrolna ramki |
| `wmbus/<płytka>/diag/...` | stan płytki i statystyki radia (tylko to, co włącza `diagnostic_mode`) |
| `wmbus/<płytka>/rssi/<id>` | ostatnie RSSI na licznik (tylko przy `publish_rssi: true`) |

Ramki z liczników bez szyfrowania pokazują zużycie w czasie, z którego da się wyczytać, kiedy ktoś jest w domu. Te dane są tak wrażliwe jak liczniki, z których pochodzą.

## Zalecana konfiguracja

- **Broker MQTT trzymaj w sieci lokalnej.** Nie wystawiaj portu 1883 do internetu.
- **Używaj logowania do brokera**: `username` / `password` z `secrets.yaml`, jak we wszystkich przykładach.
- **TLS** do brokera to standardowy komponent `mqtt:` ESPHome, nie część `wmbus_radio`; zobacz dokumentację MQTT w ESPHome (certyfikat CA, port 8883). Kosztuje pamięć RAM (sam bufor rekordu TLS to około 16 KB, zobacz [BUFFER_QOS_TIMESTAMP_PL](docs/BUFFER_QOS_TIMESTAMP_PL.md)), więc na płytkach bez PSRAM trzymaj `mqtt_buffer_size` umiarkowany.
- **Od ESPHome 2026.11 `skip_cert_cn_check: true` włącza TLS.** Przy zwykłym brokerze (bez TLS) nie dodawaj tej opcji, bo płytka nigdy się nie połączy. Wszystkie przykłady są poprawione.
- **Opcjonalnie zabezpiecz OTA i stronę płytki**: przykłady nie ustawiają hasła `ota:`, żeby pierwsza kompilacja nie wymagała dodatkowego sekretu; przykłady z komentarzami mają gotową linię (`password: !secret ota_password`). Ustaw je, jeśli z twojej sieci korzystają inni. Jeśli włączasz `web_server:`, daj mu logowanie. Przykład przedłużacza ESP-NOW robi to dla strony płytki (logowanie digest), zobacz [examples/repeater-espnow](examples/repeater-espnow/README_PL.md).

Odbieranie ramek z cudzych liczników jest technicznie nieuniknione (radio słyszy wszystko w zasięgu). `forward_meters` ogranicza to, co trafia do MQTT; użyj go, jeśli na brokerze chcesz mieć tylko własne liczniki.
