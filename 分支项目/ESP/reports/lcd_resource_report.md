# LCD/LVGL Static Resource Report

## Firmware Size

| Build | Baseline bin | LCD/LVGL bin | Delta |
| --- | ---: | ---: | ---: |
| ESPC51 | 0x1502c0 (1376960) | 0x19a710 (1681168) | 304208 bytes |
| ESPC52 | 0x1502c0 (1376960) | 0x19a720 (1681184) | 304224 bytes |

## Link Map Summary

| Target | Used Flash | Total image | HP SRAM used / total | HP SRAM remaining |
| --- | ---: | ---: | ---: | ---: |
| ESPC51 | 1542158 | 1681062 | 274008 / 320928 (85.38%) | 46920 |
| ESPC52 | 1542166 | 1681070 | 274008 / 320928 (85.38%) | 46920 |

## Intended Runtime Allocation

- Source legacy DMA buffer: 9600 bytes, released before LVGL display registration.
- LVGL draw buffer: 4800 bytes, single DMA-capable internal-RAM buffer.
- LVGL port task: priority 1, 4096-byte internal stack, 20 ms timer period, maximum sleep 1000 ms.
- No full-screen framebuffer, double buffer, PSRAM draw buffer, refresh-path allocation, image decoder, filesystem, animation, or software rotation.

## Not Verified on Hardware

Internal free/largest block, DMA free/largest block, PSRAM free/largest block, LVGL task high-water, audio DMA coexistence, display throughput, and all 10-minute/24-hour measurements are **not verified**. Compile-time map values cannot prove the task-book runtime gates.