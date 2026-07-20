# ESPC52-C51 Alignment Notes

## Baseline

The source-tree comparison found nine differences. Four are required C52
identity differences: CSI link/report ID, server device ID, and terminal
configuration defaults.

## Approved Runtime Mirrors

- IIS DMA heap diagnostic before I2S channel creation.
- `spiffs` CMake dependency.
- Voice timeout aliases and PSRAM-first buffer allocation fallback.
- Shared `VOICE_REQUEST_TIMEOUT_MS` protocol macro.
- Wake-prompt SPIFFS spool-and-play implementation.

## Verification Rules

After the patch, a source-tree comparison may retain only C52 identity,
hardware, editor, and generated build differences. Both C5 projects must
build independently using ESP-IDF 5.5.4, with no flashing.

## Results

- `ESPC51`: build completed; application binary has 74 percent free space.
- `ESPC52`: reconfigured for the `spiffs` dependency and built successfully;
  application binary has 74 percent free space.
- No flash command was run.
