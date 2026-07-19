# ESPS3 startup ESP_ERR_NO_MEM diagnostic

Date: 2026-07-11

Scope: read-only analysis of current ESPS3 source, sdkconfig, and current
build objects. Firmware code, configuration, and generated outputs were not
modified.

## Verdict

The ESP_ERR_NO_MEM returned to gateway_orchestrator.c:67 can only come from
one of four xTaskCreate calls in s3_scheduler_start():

| Order | Task | Source line | Stack | TCB | Internal RAM request |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | protocol_worker | s3_scheduler.c:1887 | 10,240 | 340 | 10,580 |
| 2 | csi_fusion_worker | s3_scheduler.c:1899 | 12,288 | 340 | 12,628 |
| 3 | stream_worker | s3_scheduler.c:1911 | 8,192 | 340 | 8,532 |
| 4 | s3_scheduler | s3_scheduler.c:1926 | 12,288 | 340 | 12,628 |
| Total | four tasks | - | 43,008 | 1,360 | 44,368 |

The exact failing one cannot be identified from the reported error. All four
failure paths return the same ESP_ERR_NO_MEM and do not log a task name. The
first possible location is line 1887; the other three remain candidates only
if their preceding task creations succeeded.

This rules out scheduler queue, semaphore, timer, malloc, and calloc failure
at line 67:

- s3_scheduler_init is called at gateway_orchestrator.c:59. It creates the
  event bus, queues, and scheduler mutex. A failure there would stop at line
  59, not line 67.
- s3_scheduler_start contains only the four task creates. It contains no
  xQueueCreate, xQueueCreateStatic, semaphore creation, esp_timer_create,
  malloc, or calloc.
- gateway_wifi_start is line 63 and s3_scheduler_start is line 67. The
  scheduler is already started after Wi-Fi initialization.

## Scheduler queue inventory

ESP-IDF target facts confirmed from current object DWARF:

- StaticTask_t and the dynamic task TCB are 340 bytes.
- QueueDefinition and StaticQueue_t are 84 bytes.
- Queue allocation request is QueueDefinition plus depth times element size.
- Dynamic FreeRTOS tasks, queues, and semaphores are internal RAM. IDF
  heap_idf.c uses MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT.

### Priority event bus

The event bus is static BSS pointer storage, not a FreeRTOS queue.

| Logical store | Depth | Element size | Internal BSS |
| --- | ---: | ---: | ---: |
| CRITICAL FIFO | 12 | 4-byte event pointer | 52 |
| REALTIME FIFO | 12 | 4-byte event pointer | 52 |
| BACKGROUND FIFO | 6 | 4-byte event pointer | 28 |
| STATE slots, including NONE | 7 | 4-byte event pointer | 28 |
| Entire event-bus BSS | - | - | 304 |

s3_event_bus_init also creates one mutex and one counting semaphore. The
counting maximum is 12 + 12 + 7 + 6 = 37. Both use zero item storage, so they
request 84 bytes each, or 168 bytes of internal dynamic RAM.

### Worker queues from s3_scheduler_init

| Queue or lock | Depth | Element | Element size | Payload | Allocation | RAM |
| --- | ---: | --- | ---: | ---: | ---: | --- |
| s_protocol_queue | 12 | s3_runtime_ingress_t pointer | 4 | 48 | 132 | internal |
| s_csi_fusion_queue | 16 | s3_csi_fusion_work_item_t | 8 | 128 | 212 | internal |
| s_stream_queue | 12 | s3_stream_work_item_t | 56 | 672 | 756 | internal |
| s_csi_fusion_queue_lock | mutex | zero-byte item | 0 | 0 | 84 | internal |
| Total | - | - | - | 848 | 1,184 | internal |

No xQueueCreateStatic or esp_timer_create exists in the scheduler or event
bus. esp_timer_get_time is a time read and allocates no timer.

## Scheduler heap allocations outside startup

These allocations occur only after a worker is running; they cannot produce
the failure returned by s3_scheduler_start.

| Object | Size | Capability | PSRAM guarantee |
| --- | ---: | --- | --- |
| s3_scheduler_event_t | 76 | MALLOC_CAP_8BIT | no |
| s3_runtime_ingress_t | 4,328 | MALLOC_CAP_8BIT | no |
| stream payload | maximum 129 | MALLOC_CAP_8BIT | no |

MALLOC_CAP_8BIT permits an 8-bit heap; it does not require PSRAM. No scheduler
heap allocation explicitly requests MALLOC_CAP_SPIRAM.

## Requested startup task stacks

gateway_orchestrator is not a FreeRTOS task. It runs inside gateway_startup,
so giving it a separate stack would double-count memory.

| Requested name | Actual task | Source line | Stack | TCB | Internal RAM at line 67 |
| --- | --- | ---: | ---: | ---: | ---: |
| gateway_startup_task | gateway_startup | main.c:58 | 8,192 | 340 | 8,532 |
| gateway_orchestrator | none; shares gateway_startup | main.c:36 | 0 | 0 | 0 |
| network_worker | network_worker | network_worker.c:3057 | 16,384 | 340 | 16,724 |
| upload_worker | upload_worker | network_worker.c:3069 | 12,288 | 340 | 12,628 |
| command_worker | command_worker | network_worker.c:3081 | 16,384 | 340 | 16,724 |
| snapshot_worker | snapshot_worker | network_worker.c:3093 | 12,288 | 340 | 12,628 |
| network_replay_worker | bme_replay_worker | network_replay_worker.c:258 | 8,192 | 340 | 8,532 |
| Total before line 67 | six tasks | - | 73,728 | 2,040 | 75,768 |

network_worker_init creates its four tasks before gateway_wifi_start. The
replay task is also created before Wi-Fi. All six are present when line 67
attempts the first scheduler worker.

## Internal RAM at scheduler start

The build objects are newer than or match the analysed source timestamps. The
current linked map gives the stronger full DRAM0 accounting below. Because
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is disabled, application BSS is
internal RAM.

| Category | Bytes | Evidence |
| --- | ---: | --- |
| Startup middleware BSS subset | 107,930 | Current object symbols for the modules initialized on this path |
| BME cache BSS | 84,036 | 300 records, dominant item |
| Six pre-existing task stacks and TCBs | 75,768 | Task table |
| Network worker queues and four mutexes | 12,224 | 16x40, 4x40, 16x336, 16x336 plus controls |
| Scheduler queues and mutex | 1,184 | Created at line 59 |
| Event-bus mutex and counting semaphore | 168 | Created at line 59 |
| Twelve earlier module mutexes | 1,008 | 12 x 84 |
| Source-accounted FreeRTOS dynamic requests before line 67 | 90,352 | Six tasks and all listed queues / semaphores |
| Four scheduler task stacks and TCBs | 44,368 | s3_scheduler_start |
| Source-accounted FreeRTOS dynamic requests after success | 134,720 | Before-line-67 requests plus scheduler tasks |

The current linked map reserves the complete dram0_0_seg as follows:

| Linked DRAM0 item | Bytes | Explanation |
| --- | ---: | --- |
| dram0_0_seg capacity | 341,760 | Map Memory Configuration |
| .dram0.dummy | 81,152 | D/IRAM shared-region reservation caused by linked IRAM use |
| .dram0.data | 21,788 | Initialized internal data |
| .dram0.bss | 128,488 | Full linked BSS, including ESP-IDF and middleware state |
| Remaining before dynamic heap requests | 110,332 | Segment arithmetic only |
| Source-accounted FreeRTOS requests before line 67 | 90,352 | Task, queue, and semaphore requests above |
| Arithmetic remainder before Wi-Fi runtime allocations | 19,980 | Not a live heap measurement |

This map arithmetic is not a substitute for heap_caps_get_free_size or
heap_caps_get_largest_free_block: ESP-IDF can expose more than one internal
heap region, and allocator metadata and fragmentation apply. It does show why
the four additional 8-12 KiB task-stack requests are not credible after Wi-Fi
startup. ESP-IDF event loop, AP/STA netif, Wi-Fi, lwIP, NVS, and Wi-Fi driver
allocations are already created by gateway_wifi_start before line 67, so the
actual free internal RAM and its largest contiguous block are lower still.

The BME cache alone occupies 84,032 bytes plus four bytes of state even when
empty. JSON payload strings are extra runtime allocations only after BME
records arrive.

## Remedy assessment

### Delay scheduler until Wi-Fi

No. The current code already starts it after Wi-Fi. Delaying further cannot
free Wi-Fi memory. Starting it before Wi-Fi might reduce immediate pressure,
but changes the established ordering and is not the narrow fix.

### Reduce scheduler queue depth

No as the primary remedy. The three worker queues plus mutex use only 1,184
bytes; event-bus BSS plus its semaphores is 472 bytes. That cannot recover the
8-12 KiB required by one failed task stack. Reducing network work queues can
recover more memory, but changes backpressure outside the failing function.

### Switch to static allocation

No by itself. Static allocation changes when allocation occurs, not total RAM
needed. With external BSS disabled, ordinary static stacks and queue storage
remain internal and can enlarge the BSS baseline.

### Use PSRAM allocation

Yes, for the four scheduler task stacks only. Current sdkconfig has both
CONFIG_SPIRAM enabled and CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM enabled.
ESP-IDF xTaskCreateWithCaps can allocate a task stack with
MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT while keeping its 340-byte TCB internal.
The existing device_stream_gateway already creates its 8 KiB UDP task using
this API.

Do not move FreeRTOS queues, mutexes, or semaphore controls to PSRAM in the
minimal change. The normal FreeRTOS allocator deliberately keeps those
synchronization objects internal.

Moving BME cache BSS to PSRAM is a larger follow-up. It requires enabling
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY and safely placing the 84 KiB
object with EXT_RAM_BSS_ATTR, then validating all cache accesses.

## Minimal modification plan

No change was made in this audit.

1. Replace the four ordinary xTaskCreate calls in s3_scheduler_start at lines
   1887, 1899, 1911, and 1926 with xTaskCreateWithCaps using
   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT.
2. Keep stack sizes, priorities, queue depths, creation order, and error
   propagation unchanged.

This moves exactly 43,008 stack bytes out of internal RAM and leaves 1,360
bytes of TCBs internal. It directly targets every possible line-67 failure
without changing Wi-Fi sequence or queue behaviour.

Hardware acceptance must confirm all four task entry logs and the existing
HEAP_MONITOR records. ESP-IDF cautions that PSRAM-backed task stacks cannot
run while flash cache is disabled; this needs hardware validation. The same
constraint already applies to the existing PSRAM-backed device_stream_udp
task.

## Evidence

- ESPS3/components/Middlewares/gateway_orchestrator/gateway_orchestrator.c,
  lines 59-67: init, Wi-Fi, and scheduler start order.
- ESPS3/components/Middlewares/runtime/s3_scheduler.c, lines 1818-1936:
  all init and start allocations plus all return paths.
- ESPS3/components/Middlewares/runtime/s3_event_bus.c, lines 22-190:
  priority depths, static storage, and semaphore creation.
- ESPS3/components/Middlewares/network_worker/network_worker.c, lines 48-66
  and 3002-3104: pre-existing task and queue allocation.
- ESPS3/components/Middlewares/network_replay_worker/network_replay_worker.c,
  lines 25-30 and 252-268: replay task.
- ESPS3/main/main.c, lines 58-72, and app_main_config.h, lines 13-18:
  startup task.
- ESPS3/sdkconfig, lines 1231-1236, 1571, and 2444: PSRAM and external-stack
  support enabled; external BSS placement disabled.
