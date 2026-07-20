# LCD and Touch Source Audit

Date: 2026-07-14 (Asia/Shanghai)

## Authorized Read-only Sources

- LCD baseline: D:\ESPproject\board_bme690\components\BSP\LCD.
- Touch reference only: D:\ESPproject\board_bme690\components\lcd.
- lcd_source_manifest.sha256 records the before/after SHA-256 values. The final values match the values collected before target adaptation.

## LCD Baseline Facts

- Controller: ST7789 through ESP-IDF esp_lcd.
- SPI host: SPI2_HOST; SCLK GPIO24; MOSI GPIO23; CS GPIO25; DC GPIO26; MISO/RST/backlight are GPIO_NUM_NC.
- Resolution: 240 x 284; SPI clock: 20 MHz; panel RGB565 configuration uses source scheme B (RGB, big endian panel data, byte swap enabled).
- The source has one 20-line, 9600-byte DMA legacy drawing buffer. The target service preserves panel initialization and public drawing APIs, then releases that legacy buffer before the official LVGL port takes ownership of the panel.
- The source components/lcd touch reference contains CST816T/CST816S code that reuses the BME I2C bus. It provides no target pin/schematic confirmation for a safe touch port.

## Static Pin Gate

C51 and C52 currently reference no use of GPIO23/24/25/26. Their existing BSP uses GPIO1/2/3/7/8 for audio/I2C. Static source review therefore found no LCD GPIO conflict. Physical wiring and display behavior are **not verified**.