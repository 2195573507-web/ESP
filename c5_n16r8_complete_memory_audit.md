# ESP32-C5 N16R8 Complete Memory Audit

**Scope:** read-only audit of the active C5 terminal architecture in `ESPC51`; `ESPC52` has the same C5 target, N16R8 configuration, WiFi/lwIP/NimBLE resource configuration, and CMake component set. Branch copies, `archive/`, and build output are not counted as runtime sources. No firmware source, configuration, resource placement, or build setting was changed.

**Evidence boundary:** the workspace was already dirty and `ESPC51/main/main.c` changed during the audit. Findings that depend on source use the captured active-source snapshot (C51 `main.c` SHA-256 `819b80d7...eacb`, `app_orchestrator.c` `1a5fe718...5694`). Existing map output is labelled as a static-build baseline, never as live heap telemetry. No device boot log containing the five requested heap fields was available.

## 1. Hardware Resource Confirmation

| Resource | Confirmed model | Audit interpretation |
|---|---:|---|
| Target | ESP32-C5, RISC-V, IDF 5.5.4 | `ESPC51/sdkconfig:482-486`; C52 matches. |
| Main internal D/IRAM | 384 KiB (393,216 B), shared instruction/data physical RAM | `0x40800000..0x40860000`; do not count it as separate 384 KiB IRAM plus 384 KiB DRAM. |
| LP/RTC RAM | 16 KiB | Separate from ordinary internal/DMA heap. |
| Flash | 16 MiB | Configured and partitioned as N16: factory 5 MiB, model 2 MiB, storage 8.9375 MiB; `ESPC51/partitions.csv:1-6`. |
| PSRAM | N16R8 premise: 8 MiB physical | Quad 40 MHz, heap integration and memtest enabled (`sdkconfig:1701-1723`). Exact detected/usable bytes require boot output or `esp_psram_get_size()`; source alone cannot prove the installed chip or runtime free size. |
| DMA | Internal shared RAM is standard DMA-capable | C5 also declares PSRAM DMA capability for supporting drivers, but generic `MALLOC_CAP_DMA` telemetry is the internal DMA-capable heap. External DMA needs driver support, appropriate caps, alignment/cache handling. |

The currently saved C51 linker baseline exposes a 320,928 B application static-linkable shared-SRAM region. Its map has 269,088 B static consumption and 51,840 B remaining in that linker segment. This is not a live `internal_free` value: ROM reservations, heap layout, dynamic WiFi/BLE allocations, and the captured-build/source mismatch remain outside that number.

## 2. Internal RAM Use Map

| Module / owner | Known size | Type / region | Lifecycle |
|---|---:|---|---|
| `app_startup` | 16,384 B | task stack, explicit internal | Boot only; task deletes itself after orchestration. |
| `c5_event_dispatcher` | 12,288 B | static task stack, internal | Persistent. |
| `mic_adc_test` | 12,288 B | task stack, explicit internal | While Mic/VAD is active. |
| `gateway_link` | 8,192 B | default-heap task stack, region not source-pinned | Persistent; default allocation must be measured. |
| `speaker_iis_writer` | 6,144 B | static task stack, internal | From speaker init to deinit. |
| `voice_chain` | 4,096 B | static task stack, internal | Persistent after voice start. |
| `wifi_reconnect` | 3,072 B | default-heap task stack, region not source-pinned | Persistent. |
| NimBLE MSYS pools | 10,752 B payload | internal 8-bit heap | At `nimble_port_init`; headers/allocator overhead additional. |
| NimBLE ACL pools | 6,120 B payload | internal 8-bit heap | At controller/host initialization; headers additional. |
| NimBLE HCI event pools | 2,660 B payload | internal 8-bit heap | At controller/host initialization; headers additional. |
| Speaker DMA staging | 1,024 B | internal DMA | While player exists. |
| LCD draw buffer | 4,800 B | internal DMA | Retained after LCD panel setup. |
| LCD legacy init buffer | 9,600 B peak | internal DMA | Temporary at LCD startup, then released. |
| Mic raw/parsed buffers | source-defined static arrays | internal DMA/static | Exact target ABI/map size needs the current ELF; not fabricated here. |
| Static queues + TCBs + semaphores | see below | `.bss` / internal | Persistent where their owners persist. |

Explicit persistent internal task stacks total **at least 34,816 B** (`dispatcher + mic + voice_chain + speaker`), excluding TCBs, queues, driver tasks, default-heap WiFi/gateway stacks, WiFi, lwIP, and NimBLE controller overhead.

### Task, Queue, Semaphore Inventory

| Owner | Object | Size / storage | Region / lifetime |
|---|---|---|---|
| BME / system | `bme_worker`, `system_worker` | 8,192 B each | PSRAM static stacks; persistent, deferred start. |
| Radar | `radar_worker`, `radar_upload`, `radar_ble_rx` | 4,096 + 3,072 + 2,048 B | PSRAM task stacks; persistent while radar/BLE active. |
| Voice | `server_voice_rx` | 8,192 B | PSRAM static stack; persistent after voice init. |
| LCD | `lcd_events`, `lcd_touch` | 2,048 + 2,048 B | PSRAM stacks; persistent after LCD startup. |
| LCD | `lcd_bootstrap` | 4,096 B | PSRAM stack. Task self-deletes, but the current source has no matching free of its manually allocated stack, so this 4,096 B remains retained. |
| Dispatcher | event bus | `24 * sizeof(c5_event_t)` | Static queue payload + control structure in internal `.bss`. |
| BME / system | worker queues | each `6 * sizeof(c5_event_t)` | Static internal `.bss`. |
| Voice | voice event queue | `4 * sizeof(voice_chain_item_t)` | Static internal `.bss`; each item includes a 96 B error string, so this is the largest directly identifiable application queue payload. |
| LCD | wake queue | `1 * sizeof(lcd_wake_event_t)` | Static internal `.bss`. |
| Radar / LCD / speaker | mutexes, counting/binary semaphores | control structures | Mostly static `.bss`; IIS and system-server scratch mutexes are default-heap. |

## 3. PSRAM Use Map

| Module / owner | Known size | Type | Lifecycle |
|---|---:|---|---|
| Voice server upload buffer | 16,384 B initially, grows to 327,680 B maximum | PSRAM session buffer | Recording only; released at session end. This is the largest proven application allocation peak. |
| Radar raw ring | `512 * sizeof(radar_raw_frame_t)`; source-layout estimate 40,960 B | PSRAM | Radar-domain lifetime. Exact ABI size needs current target ELF. |
| Radar target history | `128 * sizeof(radar_target_sample_t)`; source-layout estimate 9,728 B | PSRAM | Radar-domain lifetime. Exact ABI size needs current target ELF. |
| BME + system stacks | 16,384 B | PSRAM task stacks | Persistent. |
| Server voice stack | 8,192 B | PSRAM task stack | Persistent. |
| Radar task stacks | 9,216 B | PSRAM task stacks | Persistent while radar/BLE active. |
| LCD persistent task stacks | 8,192 B plus retained bootstrap 4,096 B | PSRAM task stacks | LCD runtime; bootstrap allocation remains after self-delete. |
| Speaker PCM ring + scratch | about 5,200 B source-layout estimate | PSRAM | Player lifetime; item ABI requires current ELF for exact size. |
| Mic pre-roll | 16,000 B | PSRAM | Mic service lifetime. |
| LCD UI arena | 98,304 B | PSRAM | LCD UI lifetime. |
| LCD service context | 2,048 B | PSRAM | LCD service lifetime. |
| Radar upload JSON | 1,024 B | PSRAM | Radar runtime. |

Known steady-state PSRAM task stacks are **35,840 B**, or **39,936 B** including the retained bootstrap allocation. This excludes radar structures and active voice recording data.

## 4. Startup-Phase Memory Curve

The table deliberately records `not captured` rather than inventing heap values. All five fields are available in the current diagnostic functions (`C5_MEM`, radar, voice, LCD) but no serial output containing their values is in scope.

| Phase | `internal_free` | `internal_largest` | `dma_free` | `dma_largest` | `psram_free` | Source-supported change statement |
|---|---|---|---|---|---|---|
| boot | not captured | not captured | not captured | not captured | not captured | `app_startup` is already allocated before the first startup sample, so there is no true boot baseline. |
| WiFi | not captured | not captured | not captured | not captured | not captured | First comparable label is `after_wifi_connect`, already combining WiFi init, STA netif, gateway link, reconnect task and connection. |
| BME | not captured | not captured | not captured | not captured | not captured | `after_bme_start` exists; BME's own prior log is aggregate 8-bit heap only. |
| radar | not captured | not captured | not captured | not captured | not captured | `RADAR_MEMORY before_init/after_init` provides a valid phase pair when observed. Radar stacks/buffers are primarily PSRAM. |
| dispatcher | not captured | not captured | not captured | not captured | not captured | Valid adjacent `C5_MEM` before/after pair; allocates the 12,288 B internal dispatcher stack. |
| voice | not captured | not captured | not captured | not captured | not captured | `VOICE_START_STAGE`, task boundaries, and Mic VAD after-start diagnostics can resolve the step on device. |
| LCD | not captured | not captured | not captured | not captured | not captured | Starts after `app_startup` ends; temporary 9,600 B internal DMA is released, 4,800 B retained. Do not subtract it from earlier stages that still included startup stack. |

**First large drop:** no actual first drop can be established without the above runtime samples. The earliest *directly attributable* large internal step in source is dispatcher creation (**12,288 B**). WiFi/lwIP/netif may have an earlier and larger opaque step, but current sampling does not isolate it; asserting otherwise would be speculation.

## 5. Maximum Internal-RAM Consumers (Top 10)

This is a ranked, auditable list of explicit current architecture consumers or configured payloads. It is not a live heap ranking; opaque WiFi/controller, library static data, TCBs, and allocator overhead are deliberately not invented.

| Rank | Source | Known / bounded size | Notes |
|---:|---|---:|---|
| 1 | `app_startup` task | 16,384 B | Internal, boot transient. |
| 2 | dispatcher task | 12,288 B | Internal, persistent. |
| 3 | Mic task | 12,288 B | Internal while Mic/VAD active. |
| 4 | NimBLE configured payload pools | >=19,532 B total | 10,752 MSYS + 6,120 ACL + 2,660 HCI events; internal; headers/controller overhead extra. |
| 5 | gateway link task | 8,192 B | Default heap; its actual region requires measurement. |
| 6 | speaker writer task | 6,144 B | Internal, retained with player. |
| 7 | voice-chain task | 4,096 B | Internal, persistent. |
| 8 | WiFi reconnect task | 3,072 B | Default heap; actual region requires measurement. |
| 9 | lwIP tcpip task | 3,072 B | IDF configuration limit; runtime allocation requires measurement. |
| 10 | LCD retained draw buffer | 4,800 B | Internal DMA, persistent after LCD startup. |

Existing C51 map snapshot additionally contains a 65,536 B `work_mem_int.0` static symbol and a 5,800 B `g_cnxMgr`-class symbol. Because that ELF predates the captured source snapshot, it is evidence of a prior static baseline, not a current exact source-owned ranking; it must be regenerated only when a build is authorized.

## 6. Fragmentation Sources

| Candidate | Classification | Evidence / consequence |
|---|---|---|
| WiFi dynamic RX/TX pools | allocation-time internal pressure, opaque sizes | Config permits 32 dynamic RX + 32 dynamic TX buffers. Their actual sizes and allocation order require runtime heap trace. |
| NimBLE pool/controller initialization | allocation-time internal pressure | All NimBLE allocation mode is internal; known buffer payload >=19,532 B plus overhead. |
| Repeated HTTP requests | transient heap churn | 640 B URL each request; raw stream RX/TX 1,024/512 B, and a 1,024 B read chunk. Generic 8-bit allocations are internal-preferred below the 16,384 B policy threshold. |
| Voice recording growth | PSRAM churn | Upload buffer grows from 16 KiB to maximum 320 KiB, then frees. It does not itself fragment internal heap. |
| LCD temporary DMA buffer | transient internal-DMA pressure | 9,600 B during setup, released after panel init. |
| Small default-heap task/semaphore allocations | possible internal fragmentation | WiFi/gateway stacks, event groups and a few mutexes do not request caps. No runtime largest-block decline was captured, so fragmentation is not proven. |

## 7. Current Failure Determination

| Question | Determination | Basis |
|---|---|---|
| Total capacity insufficient? | **No evidence; not established.** | N16R8 gives a configured PSRAM path and source moves major radar/BME/voice buffers/stacks there. No runtime allocation failure or free-space series was provided. |
| Internal fragmentation? | **Not established.** | There is no observed `internal_largest` trend or allocation failure log. Candidate churn exists but is not proof. |
| A module incorrectly occupying memory? | **Confirmed only for LCD bootstrap PSRAM retention; not a confirmed internal failure cause.** | 4,096 B PSRAM stack remains allocated after the bootstrap task self-deletes. No confirmed erroneous internal-RAM ownership was found. |
| Admission policy too strict? | **Not established.** | HTTP admission requires free >=8,192 B and largest >=4,096 B. This is a gate, not allocation evidence; it may reject work before OOM but no rejection log was supplied. |

**Overall conclusion:** the current input has no runtime crash/OOM/admission-fail record, so a definitive "current failure cause" cannot responsibly be assigned. The architecture shows a constrained shared internal pool, with WiFi and NimBLE both consuming it and PSRAM deliberately not used for WiFi/lwIP. It does **not** demonstrate total N16R8 capacity exhaustion.

## 8. Optimization Suggestions Only (No Changes Performed)

1. Capture one cold-boot serial run containing `C5_MEM`, `RADAR_MEMORY`, `VOICE_START_STAGE`, `LCD_MEM`, and any `*_ADMISSION_FAIL`/allocation failure lines. Preserve all five fields at every requested phase; this resolves capacity versus fragmentation without guessing.
2. When a build is separately authorized, regenerate the C51/C52 ELF map and archive-size report from the exact audited source hash. Use that to replace source-layout estimates and the stale-build symbol ranking.
3. Treat the WiFi/NimBLE step as the first measurement priority because both use internal RAM and their opaque allocation sizes dominate source uncertainty.
4. Validate whether the retained LCD bootstrap PSRAM allocation is intentional before treating it as a leak; it is an explicit lifecycle discrepancy, but it is outside internal-RAM pressure.
5. Keep admission-policy rejections distinct from malloc failures in operational logs; a policy rejection is not proof of total-memory exhaustion.

## Evidence Index

- Active component selection: `ESPC51/components/Middlewares/CMakeLists.txt:1-96`.
- Startup and phase order: `ESPC51/main/main.c:32-112`; `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c:375-532`.
- C5 resource policy and phase metrics: `ESPC51/components/Middlewares/memory/c5_memory.c:1-130`; `ESPC51/sdkconfig:1701-1723,1852-1870,2159-2269,775-1017`.
- Tasks/queues: `c5_backpressure_controller.c:431-458`, `c5_runtime_workers.c:188-303`, `voice_chain.c:1086-1152`, `radar_worker.c:440-492`, `radar_ble_runtime.c:58-102`, `lcd_driver.c:294-394`, `lcd_service.c:189-222`.
- WiFi/lwIP/BLE: `wifi_manager.c:189-241`, `radar_ble_transport.c:424-562`, and local ESP-IDF 5.5.4 `soc.h`, `soc_caps.h`, `memory_layout.c`, NimBLE Kconfig/initializers.
- Static baseline only: `build-c5-memory-static-c51/00_Learn.map`, timestamp 2026-07-20 14:40 +0800.
