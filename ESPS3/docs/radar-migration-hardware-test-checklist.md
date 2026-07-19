# LD2450 hardware acceptance checklist

Date: 2026-07-16

Overall status: BLOCKED

Reason: confirmed UART controllers/GPIOs, three connected HLK-LD2450 units, and real-device runtime evidence are not available in this software session.

## Pin configuration prerequisite

Do not flash until the user confirms, independently for C51, C52, and S3:

- UART controller index.
- MCU TX GPIO connected to radar RX.
- MCU RX GPIO connected to radar TX.
- Power and ground wiring that matches the board and radar electrical documentation.
- No conflict with existing BME, Mic, Speaker, LCD/LVGL, flash/PSRAM, or console pins.

Then update only each board's own:

```text
components/radar_ld2450/include/radar_board_config.h
```

Current port/TX/RX values are `-1` and `RADAR_BOARD_UART_ENABLED` is `0` on all three boards. No pin has been guessed.

## HW-P1: single board and single radar

Status: BLOCKED

Run separately on C51, C52, and S3:

- [ ] Valid frames continue at approximately 10 Hz.
- [ ] Entry changes the room to MOTION within 500 ms.
- [ ] Exit changes MOTION to HOLD after the short gap.
- [ ] HOLD reaches VACANT_INFERRED only after the configured hold duration and valid empty frames.
- [ ] Radar power loss reaches UNKNOWN within 3 seconds.
- [ ] Radar recovery resynchronizes and requires valid confirmation frames.
- [ ] Left/right target coordinate direction matches physical movement.
- [ ] Up to three targets parse without invalid counts or array errors.

Capture diagnostics for parser valid/error counts, UART overflow/driver errors, state transitions, and task stack/heap.

## HW-P2: three rooms in parallel

Status: BLOCKED

- [ ] C51 motion does not change C52 or S3_LOCAL.
- [ ] C52 motion does not change C51 or S3_LOCAL.
- [ ] One radar or C5 going offline affects only its source.
- [ ] S3 restart lets each C5 resume with its current latest snapshot.
- [ ] Packet capture/logs show no legacy trigger traffic.

## HW-P3: full feature regression

Status: BLOCKED

- [ ] WiFi, register, heartbeat, and status remain normal.
- [ ] BME690 remains normal.
- [ ] WakeNet, Mic, VAD, and voice turn remain normal.
- [ ] Speaker remains normal.
- [ ] LCD/LVGL remains normal.
- [ ] Command polling and ACK remain normal.
- [ ] Voice activity does not stop UART parsing or cause sustained overflow.
- [ ] Network loss does not stop local radar state updates.
- [ ] Network recovery sends only the latest C5 radar state.

## HW-P4: stability

Status: BLOCKED

Minimum run: 8 hours. Release recommendation: 24 hours.

- [ ] Exercise multiple voice turns.
- [ ] Disconnect and restore WiFi.
- [ ] Power-cycle each radar independently.
- [ ] Run all three radars concurrently.
- [ ] No Guru Meditation or watchdog reset.
- [ ] No sustained UART overflow growth.
- [ ] No continuing heap decline or task stack overflow.
- [ ] No permanent stop of BME, voice, LCD, or heartbeat.

## Acceptance boundary

The software migration is complete when static tests and all three builds pass. The product-level radar replacement is not hardware-accepted until the user records PASS evidence for HW-P1, HW-P2, HW-P3, and HW-P4.
