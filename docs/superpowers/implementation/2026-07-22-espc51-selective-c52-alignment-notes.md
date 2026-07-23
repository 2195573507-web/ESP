# Alignment Notes: ESPC51 Selective C52 Alignment

## Selected for Synchronization

- `app_orchestrator` implementation and header
- C52 LCD service/UI headers, implementation, and assets
- BME690 driver/service implementation and header
- runtime worker placement/diagnostics
- radar HOME snapshot parser
- radar BLE transport robustness

## Intentionally Retained as C51

- terminal and server device identity configuration
- radar BLE binding configuration, enabled default, room, and MAC
- radar board local ID and device ID
- radar runtime C51-specific log labeling

## Known Equivalent Surfaces

- main entry
- mic/VAD and C5 audio transport
- Wi-Fi/gateway transport
- I2C and IIS

## Validation Boundary

Static parity checks and an ESP-IDF build establish source/build health only.
BLE discovery, LCD rendering, microphone capture, and WakeNet detection require
separate hardware acceptance.

## Result

`idf.py -B /tmp/espc51-c52-alignment-build build` completed with ESP-IDF 5.5.4.
Ninja then completed the image-size and bootloader-size checks. The build noted
that the already-missing `components/radar_domain/CMakeLists.txt` excludes that
component; it was not changed by this task.
