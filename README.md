# BasmioTwo

Dual-speaker Basmio firmware for ESP32-S3 (JC3248W535EN, AXS15231B QSPI LCD).
Forked from BasmioOne (ESP32-P4 + C6).

## Build

Requires ESP-IDF >= 5.4 (developed on 5.5.1).

```
idf.py set-target esp32s3
idf.py build flash monitor
```

Managed components are fetched by the IDF component manager from `dependencies.lock`.
