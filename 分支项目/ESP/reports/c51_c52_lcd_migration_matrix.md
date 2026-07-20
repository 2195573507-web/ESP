# C51/C52 LCD/LVGL Migration Matrix

| Item | ESPC51 | ESPC52 | Result |
| --- | --- | --- | --- |
| Target | esp32c5 | esp32c5 | Same |
| ESP-IDF | 5.5.4 | 5.5.4 | Same |
| LVGL | 9.2.2 | 9.2.2 | Same |
| esp_lvgl_port | 2.6.2 | 2.6.2 | Same |
| dependencies.lock SHA-256 | 816d5dc879409d03c844243d0b001e87cd830f460913e8b441e4b4d5bf46c3fb | 816d5dc879409d03c844243d0b001e87cd830f460913e8b441e4b4d5bf46c3fb | Identical |
| LCD component files | Seven files | Seven files | SHA-256 identical |
| Source panel pins | GPIO23/24/25/26 | GPIO23/24/25/26 | Same source configuration |
| Final image | 0x19a710 | 0x19a720 | 16-byte expected device-identity delta |
| Clean/incremental build | Passed | Passed | Passed |
| Runtime/real hardware validation | Not verified | Not verified | Stop required |

The shared component uses one ST7789 panel/IO owner. It frees the source legacy 9600-byte DMA buffer before esp_lvgl_port allocates one 4800-byte partial DMA draw buffer. Double buffering, full refresh, software rotation, image decoding, filesystems, themes, animations, and touch initialization are not enabled.