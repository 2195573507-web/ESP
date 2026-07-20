# `esp-rs/esp-hal` 固定提交深读

## 研究口径

- 仓库：[`esp-rs/esp-hal`](https://github.com/esp-rs/esp-hal)
- 固定提交：[`035f29ce083794e845a81efe3c43cca6c944c1dd`](https://github.com/esp-rs/esp-hal/tree/035f29ce083794e845a81efe3c43cca6c944c1dd)，提交时间 `2026-07-09T17:55:46Z`
- 本轮方法：只读固定提交源码，未执行仓库脚本、构建、安装、烧录或真机测试。因此“源码可证明”只表示静态代码/配置事实；调度时延、无线稳定性、功耗和故障恢复均不是运行验证结论。
- 关注范围：ESP32-C5 / ESP32-S3 target 支持、`no_std` HAL、Embassy async executor、interrupt executor/priority、radio/RTOS 边界、critical section/channel/timer、资源生命周期与恢复限制。

## 许可证

仓库根 README 声明全部 package 可在 Apache-2.0 或 MIT 中任选其一，并给出两份完整许可证；这是标准的 `MIT OR Apache-2.0` 双许可，不等于依赖项或芯片厂商二进制也自动获得同样许可。证据：[README 许可声明](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/README.md#L101-L112)、[Apache-2.0](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/LICENSE-APACHE#L1-L201)、[MIT](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/LICENSE-MIT#L1-L21)。

## C5 / S3 支持强度

根 README 把 ESP32-C5 和 ESP32-S3 都列为受支持芯片；S3 还可通过独立 `esp-lp-hal` 编程低功耗 RISC-V 核。证据：[支持列表](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/README.md#L17-L25)。但两者成熟度并不相同：固定提交的 HAL CHANGELOG 把 C5 支持记录为近期的 initial/basic 增量，随后才逐项加入 GPIO、PCNT、UART、SPI、RMT、DMA、I2C、密码学、USB Serial/JTAG、PSRAM 等；S3 则已有长期的双核、PSRAM、LCD_CAM、USB、LP core 等专用路径。证据：[C5 初始支持清单](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-hal/CHANGELOG.md#L61-L102)。

两颗芯片都不是只有名称占位：`esp-rtos` 的 feature 将各自 HAL、生成元数据和 ROM 绑定接入；async hello-world / serial / ESP-NOW 示例也为两者组合 `esp-hal + esp-rtos`，无线示例再加 `esp-radio`。证据：[RTOS target features](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/Cargo.toml#L111-L130)、[Embassy hello-world features](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/async/embassy_hello_world/Cargo.toml#L34-L79)、[ESP-NOW async features](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/esp-now/embassy_esp_now/Cargo.toml#L45-L83)。这能证明源码/配置支持，不能证明该提交的 CI job 已通过或真机稳定。

芯片元数据给出硬边界：C5 是 `riscv32imac-unknown-none-elf`、1 核；S3 是 `xtensa-esp32s3-none-elf`、2 核且启用 `multi_core`。C5 元数据里仍有被注释的未定义外设，文件头还明确说空 driver table 表示 partial support；因此“crate 能选 C5 target”不等于所有外设稳定。证据：[C5 metadata](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-metadata/devices/esp32c5/soc.toml#L1-L13)、[S3 metadata](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-metadata/devices/esp32s3/soc.toml#L1-L35)。

元数据的 driver status 给出更具体的成熟度信号：C5 仅 I2C master、SPI master、UART 等少数项标为 `supported`，ADC/AES/DMA/GPIO/RMT/RNG/BT/PHY/sleep 等仍是 `partial`，I2C slave、TWAI、LEDC、MCPWM、ETM、HMAC、ECDSA、ULP RISC-V 等还明确 `not_supported`；S3 的 GPIO/I2C master/SPI master/UART 标为 `supported`，但 ADC/AES/DMA/RMT/RNG/BT/PHY/sleep 等同样仍有 `partial`。证据：[C5 driver status](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-metadata/devices/esp32c5/soc.toml#L411-L621)、[S3 driver status](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-metadata/devices/esp32s3/soc.toml#L608-L819)。所以 C5 是“广覆盖但仍在 bring-up/补齐期”，S3 是“覆盖更久且有更多专用外设，但也不是全 stable”。

`esp-radio` 的维护者矩阵对 C5 标记 Wi-Fi、BLE、coex、ESP-NOW、IEEE 802.15.4 均有实现；S3 标记前四项，硬件没有 802.15.4。这个勾只表示“有 driver implementation”，README 自己没有承诺运行质量；而 crate 版本仍为 `1.0.0-beta.0`，radio 还要求 HAL 的 `unstable` feature，BLE/ESP-NOW/coex 等 feature 也落在不受稳定 semver 保护的范围。证据：[radio 支持矩阵与限定](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/README.md#L9-L28)、[radio package/feature policy](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/Cargo.toml#L1-L42)。

radio metadata 进一步区分两条实现：C5 Wi-Fi MAC v3、`has_5g=true`、CSI enabled，BLE 是 `npl` controller且 status=`partial`，另有 802.15.4；S3 Wi-Fi MAC v1、CSI enabled，BLE 是 `btdm` controller且同样 `partial`。证据：[C5 radio metadata](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-metadata/devices/esp32c5/soc.toml#L546-L560)、[S3 radio metadata](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-metadata/devices/esp32s3/soc.toml#L810-L820)。这既证明 C5/S3 CSI 有 target gate，也说明 BLE 后端与资源参数不能跨芯片直接照抄。

## `no_std`、ownership 与初始化

`esp-hal` 和 `esp-rtos` 都是 `#![no_std]`。HAL 的 `init()` 一次性返回 peripheral singleton；驱动可按值独占，也可 `reborrow()` 获取短生命周期句柄，Drop 驱动后再复用外设。文档明确禁止对 esp crate 类型 `mem::forget`，因为很多类型依赖 Drop 把硬件恢复到定义状态。证据：[HAL no_std 与 singleton/reborrow](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-hal/src/lib.rs#L5-L74)、[Drop 警告](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-hal/src/lib.rs#L160-L166)、[`#![no_std]`](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-hal/src/lib.rs#L195-L212)。

这套所有权模型能阻止普通 Rust 代码同时构造两个同一硬件驱动，但不能消除 FFI/`unsafe` 的全局状态风险；radio 适配层为预编译 driver blob 暴露裸指针、allocator、task/queue/semaphore 回调，必须由 wrapper 的 leak/from_ptr/Drop 合同配对。它属于“Rust 在 C ABI 外围建立资源合同”，不是纯安全 Rust。证据：[queue leak/from_ptr/Drop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/queue.rs#L429-L470)、[queue Drop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/queue.rs#L643-L648)。

## Embassy 与 `esp-rtos` 调度模型

### 启动与线程 executor

`esp_rtos::start()` 把当前上下文转换为固定在 core 0 的 main task，要求从 thread mode 调用，并用一个硬件 one-shot timer和 software interrupt 0 驱动调度。默认 tick rate 是 100 Hz；默认启用 stack-pointer range check 与硬件栈溢出检测。证据：[start 合同与实现](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/lib.rs#L336-L430)、[RTOS config defaults](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/esp_config.yml#L1-L33)。

thread-mode `Executor` 必须有 `'static mut` 生命周期，`run()` 永不返回。其循环在一个 RTOS thread 中 `poll()` ready futures；无工作时用内部单 bit `ThreadFlag` 挂起 owner task，flag 明确不是通用 wait queue，没有 timeout 且只允许一个 waiter。证据：[ThreadFlag 限制/唤醒](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/embassy/mod.rs#L42-L140)、[Executor 生命周期与 poll loop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/embassy/mod.rs#L172-L293)。

因此同一 executor 内的 futures 是协作式推进：任何 future 在一次 poll 中忙等/阻塞，会饿死同 executor 的其他任务。官方 multiprio 示例专门用 5 秒 busy loop 展示低优先级 async task 被饿死；它不是调度器自动抢占每个 future 的证据。证据：[阻塞反例](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/async/embassy_multiprio/src/main.rs#L43-L67)。

### Interrupt executor 与优先级

`InterruptExecutor` 在 software interrupt handler 中直接调用 `raw::Executor::poll()`；任务唤醒会重新 pend 对应 software interrupt。实例上限由四个 software interrupt slot 的静态数组限定，`start(priority)` 把 handler 绑定到给定硬件中断优先级，并返回跨上下文的 `SendSpawner`。证据：[interrupt poll/storage](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/embassy/mod.rs#L302-L355)、[start/priority](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/embassy/mod.rs#L357-L400)。

这给高优先级异步任务更低唤醒延迟，却也意味着 future 的 poll 真正在 ISR context 运行。源码可证明的是执行上下文与优先级；“ISR 内必须短、不得做阻塞 I/O/长计算”是由该上下文推导出的工程约束。multiprio 示例把高优先级 ticker 放到 priority 3 interrupt executor，使它能打断 thread executor 的 5 秒忙等；不能据此把任意业务 future 放进 ISR。证据：[示例拓扑](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/async/embassy_multiprio/src/main.rs#L69-L89)。

### C5 单核与 S3 双核

C5 只有一个调度核；async、radio controller task、ISR 和普通任务必须在同一核上靠优先级、bounded poll 和 yield 共存。S3 可先启动 core 0 scheduler，再以独立静态 stack 启动 core 1 scheduler；启动函数最多自旋 1 秒等待第二核初始化，失败即 panic，成功后故意 forget CPU guard 以保持第二核运行。证据：[second-core 合同/超时](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/lib.rs#L434-L543)。

S3 的 thread executor 只有在对应核 scheduler 已启动时才能运行；官方双核示例在 core 1 创建独立 executor，并用 `Signal<CriticalSectionRawMutex, bool>` 跨核传递最新控制值。另一个示例证明 interrupt executor 可在没有 core 1 RTOS scheduler 时依靠 `CpuControl` 与 software interrupt 独立运行。证据：[双核 thread executor](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/async/embassy_multicore/src/main.rs#L52-L96)、[双核 interrupt executor](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/async/embassy_multicore_interrupt/src/main.rs#L71-L113)。两例只有 `CHIP_FILTER: multi_core`，对 C5 不开放。

RTOS ready queue 按优先级分层 FIFO；单核在新 ready task 优先级不低于当前最高 ready level 时触发调度，连同级也触发以建立 time slicing。双核对未绑核 task 选择当前运行优先级较低的核，对绑核 task只能选指定核；pop 时跳过绑在另一核的 task。证据：[ready/触发规则](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/run_queue.rs#L139-L250)、[双核 pop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/run_queue.rs#L253-L299)。这说明 S3 有调度选择，不说明负载会自动形成产品所需的 radio/CSI/应用隔离。

## Radio / Wi-Fi / BLE 的 RTOS 边界

`esp-radio` 不是单纯 Embassy driver：package 依赖芯片专用 `esp-wifi-sys-*` 预编译驱动和 `esp-radio-rtos-driver`，C5/S3 target feature 都同时接入 HAL、RTOS、PHY、PAC、radio driver。证据：[依赖与 target feature](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/Cargo.toml#L54-L102)、[C5/S3 feature wiring](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/Cargo.toml#L195-L203)、[S3 feature wiring](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/Cargo.toml#L246-L255)。

`esp-rtos` 为 blob 实现 `SchedulerImplementation`：初始化必须发生在当前核 scheduler 已运行之后；controller 请求的 task priority 会被截断到 RTOS 最大值，core 0/1 pin 映射到对应 CPU，无效核号 warning 后退化为不绑核；task、queue、semaphore、timer、wait queue 分别接到 RTOS/兼容实现。证据：[radio scheduler init/task mapping](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/esp_radio/mod.rs#L29-L96)、[IPC 注册](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/esp_radio/mod.rs#L131-L174)。所以应用的 async Wi-Fi/BLE API 与 controller 内部优先级 task 是两层调度，不应画成一个 Embassy task。

radio 全局 init 还要求 scheduler 已启动、CPU 至少 80 MHz且 interrupts 没有禁用；它持有 `WakeLock`，最后一个 radio guard Drop 时关闭 coex/ISR并释放 wake lock。证据：[radio init/deinit](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/lib.rs#L279-L362)、[refcount guard](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/lib.rs#L364-L394)。这意味着启用 Wi-Fi/BLE 时不能期待 RTOS automatic light sleep 自行介入；radio 生命周期和省电状态必须一起设计。

Wi-Fi controller 把 `wifi_task_core_id` 固定为调用 `WifiController::new()` 时的当前核；S3 上从哪个核初始化会直接决定 blob Wi-Fi task 的核归属。默认 controller RX queue=5、TX queue=3、static RX buffers=10、dynamic RX/TX buffers=32；这些是 RAM/吞吐调参，不是无界缓冲。证据：[controller defaults](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2217-L2255)、[init/core binding](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2321-L2381)。对 S3 而言，不能一面随意在 core 1 初始化 radio，一面假设 core 1 完全留给 CSI/算法。

BLE async 不是一个独立 Embassy controller task：`BleConnector` 包装 BT singleton、PHY guard和 radio guard；HCI RX callback 把包放入全局队列并唤醒 `AtomicWaker`，read future 注册 waker后检查数据。该 waker 明确“新注册覆盖旧 waker”，因此接口天然更适合单一 HCI consumer；多个并发 reader 需要上层串行化。证据：[AtomicWaker](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/asynch.rs#L1-L31)、[BLE connector/init/drop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/controller/mod.rs#L38-L113)、[HCI future](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/controller/mod.rs#L180-L246)。

C5 NPL controller默认 RTOS task priority=`max-2`、stack=4096、core 0，并以容量 16 的 pointer event queue衔接 controller；S3 BTDM controller默认同样 priority=`max-2`、stack=8192、core 0。两条 backend 最终都通过 `preempt::task_create()` 进入 `esp-rtos`，而不是在调用者的 Embassy future 中运行 controller。证据：[C5 BLE config](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/os_adapter_esp32c5.rs#L86-L99)、[C5 defaults/core](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/os_adapter_esp32c5.rs#L206-L212)、[C5 controller wiring](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/os_adapter_esp32c5.rs#L266-L304)、[NPL task adapter](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/npl.rs#L392-L430)、[NPL queue capacity](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/npl.rs#L17-L26)、[NPL event queue](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/npl.rs#L979-L995)、[S3 BTDM defaults/core](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/os_adapter_esp32c3_s3.rs#L460-L523)。

S3 BTDM adapter仍把 mutex create/delete/lock/unlock 和多项 sleep转换/enter/exit callback注册进 OSI table，而这些 callback在固定提交中是 `todo!()`；controller config又显式设置 `sleep_mode=0`。静态可证明的是“存在可 panic 的未实现 callback”和“默认关闭 controller sleep”，不能在未运行时断言 blob 一定会调用这些路径或 BLE 一定失败。证据：[registered callbacks](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/os_adapter_esp32c3_s3.rs#L94-L137)、[unimplemented callbacks](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/btdm.rs#L127-L147)、[sleep callbacks](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/btdm.rs#L213-L253)、[sleep disabled](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/ble/os_adapter_esp32c3_s3.rs#L513-L529)。这是 S3 BLE“源码支持但成熟度需保守标注”的最直接证据。

## Critical section、queue、channel 与 timer

### Critical section / mutex

`esp-sync::RawMutex` 在当前核禁用 interrupts；C5 单核仅此即可保护共享状态，S3 双核还用 atomic owner 在另一核竞争时自旋。它不是全局 critical section，但持锁核的 interrupts 仍被禁用；回调必须非常短。`NonReentrantMutex` 检测同上下文重入并 panic。证据：[单核 interrupt lock](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-sync/src/raw.rs#L27-L145)、[双核 owner/spin](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-sync/src/lib.rs#L115-L217)、[RawMutex/NonReentrantMutex](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-sync/src/lib.rs#L291-L416)。

HAL 还提供 priority-limited mutex：只屏蔽指定优先级范围，若在更高 run level 获取/释放会 panic；全局 `critical_section` 实现仍由一个 `RawMutex` 承担。证据：[priority lock 与 critical-section adapter](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-hal/src/sync.rs#L1-L111)。这是减小中断屏蔽范围的工具，不是允许在锁内等待 queue/semaphore 的许可。

### Radio controller queue / semaphore

兼容 queue 是固定容量、固定 item-size 的字节拷贝 ring；empty/full counting semaphores提供有 deadline 或无限等待的 send/receive，ISR 版本严格 try-only并可返回 `higher_priority_task_waken`。证据：[queue storage/容量](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/queue.rs#L662-L785)、[send/receive/ISR](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/queue.rs#L796-L910)。它避免 queue 内部保留借用，但若 item 本身是裸 pointer，所指对象的失败所有权仍由调用方负责。

兼容 mutex 在 task context 实现 priority inheritance：高优先级 waiter 遇到低优先级 owner 时临时提高 owner 优先级，最终 release 恢复原优先级；ISR context 没有 current task，无法提供同样继承。证据：[mutex acquire/inheritance](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/semaphore.rs#L468-L559)、[release restore](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/semaphore.rs#L561-L621)。这降低经典 priority inversion，但不能补救在 critical section 中阻塞或长时间持锁。

### Wi-Fi event channel

Wi-Fi 状态先写入 relaxed atomic snapshot，再尝试把 typed event 投到 Embassy `PubSubChannel`。默认 channel capacity=2、max subscribers=2；`try_publish` 失败只 warning “Lost event”。订阅 API能返回 `Lagged(missed)`，但 controller 的 connect/scan/disconnect 等内部等待循环多用 `next_message_pure()` 或忽略 `Lagged`，本身没有 deadline。证据：[channel defaults](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/esp_config.yml#L20-L39)、[producer drop path](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/os_adapter/mod.rs#L658-L700)、[Lagged API](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/event.rs#L1299-L1351)、[connect wait loop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2962-L3010)。

因此它是“bounded event notification + separate current-state snapshot”，不是命令必达通道。产品代码应对 connect/scan/disconnect future 外包 deadline/cancel/retry，并把 `Lagged` 与 producer drop 计入指标；只增大 capacity 不能替代恢复状态机。证据：[atomic state snapshot](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/state.rs#L75-L113)、[disconnect wait without deadline](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L3034-L3065)。

### 两套 timer 路径

Embassy timer queue 与 RTOS sleeping-task queue共享一个 one-shot hardware timer。timer ISR 先处理 Embassy wakers，再处理 RTOS task wakeups与 time slice，最后重新 arm；硬件 schedule 对过大 timeout 会折半重试，其他错误 panic。证据：[Embassy schedule_wake](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/timer/embassy.rs#L7-L66)、[RTOS timer ISR](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/timer/mod.rs#L232-L312)、[arm/retry](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/timer/mod.rs#L160-L187)。

radio blob timer 则另有一条兼容路径：第一次使用时懒创建一个全局 timer queue和不可停止的后台 task，task priority 2、未绑核、stack 8192；全部 C callback在这个 task 中串行执行。timer Drop 被放入 `scheduled_for_drop`，等当前 processing 结束再释放，避免回调执行中 use-after-free。证据：[timer task lazy init](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/timer.rs#L322-L396)、[callback/延迟释放](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/timer.rs#L418-L496)、[timer delete/task loop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/timer.rs#L586-L670)。长 timer callback 会串行拖延所有 radio timers；它不能承担 CSI处理或网络请求。

RTOS 源码还明确标注 repeated `schedule_wakeup()` 不受支持：若 OS blocking API 在 critical section 中循环，第一次把 task 标为 sleeping 后又无法真正切换，下一次会触发断言/崩溃。证据：[critical-section sleep 限制](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/timer/mod.rs#L190-L228)。

## 生命周期与恢复边界

### 已实现的闭合路径

- HAL peripheral singleton + reborrow 把驱动 ownership 放进类型系统；许多 driver Drop 负责硬件回收，不能 `mem::forget`。证据：[singleton/reborrow](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-hal/src/lib.rs#L24-L74)、[mem::forget 禁令](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-hal/src/lib.rs#L160-L166)。
- radio 使用 lock-free全局 refcount，首个 guard 初始化 radio、最后一个 guard关闭 ISR/coex并释放 wake lock。init/deinit 中间态用计数 `1` 表示，其他调用者会自旋等待。证据：[refcount state machine](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/refcount.rs#L1-L55)。
- `WifiController::drop()` 先把 station/AP state 置为 uninitialized，再 stop、排空 RX `PacketBuffer`、deinit Wi-Fi/supplicant；排空刻意放在 queue critical section 外，避免 free buffer 时内部 mutex导致死锁。Interface singleton也在 Drop 时释放。证据：[Wi-Fi drain/deinit](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L1000-L1038)、[interface singleton/drop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L1315-L1419)、[controller Drop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2258-L2287)。
- `set_config()` 用 `ResetModeOnDrop` 做 transaction-like rollback：任意中途错误把 mode 重置 NULL并 stop；成功才 defuse guard。证据：[configuration rollback](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2611-L2712)。
- radio task 返回后自动 schedule deletion；scheduler 先从 run/timer/wait/all-task queues 移除，再 Drop TLS semaphore、task和heap stack。证据：[task return/delete](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/task/mod.rs#L437-L523)、[scheduled deletion loop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/task/mod.rs#L699-L737)、[scheduler cleanup](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/scheduler.rs#L383-L419)。

### 不能据此宣称的恢复能力

- thread/interrupt executor、core 1 scheduler与 radio timer task 都设计为静态活到重启，没有通用 stop/join/restart；timer queue和 second-core guard有意 `forget`。动态重建只覆盖部分 driver/controller，不覆盖整个 runtime。证据：[Executor never returns](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/embassy/mod.rs#L207-L228)、[second-core guard](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/lib.rs#L476-L543)、[timer queue leak/lifetime](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/timer.rs#L328-L396)。
- `WifiController::drop()` 的 deinit error只能 warning，Drop 无法把失败返回给调用者；`wifi_deinit()` 若第一步 `esp_wifi_stop()` 失败会提前返回，后续 drain/deinit不会执行。源码证明有失败日志，未做 fault injection，不能断言一定泄漏或一定可重试。证据：[wifi_deinit early-return order](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L1000-L1018)、[Drop warning](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2269-L2286)。
- `WifiController::new()` 在 controller 对象构造前调用多步 `wifi_init()`；若中间某一步返回 error，radio guard会 Drop，但源码没有显式调用 `wifi_deinit()` 回滚已经完成的 Wi-Fi子步骤。也是静态风险，不是运行复现。证据：[Wi-Fi init sequence](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L938-L965)、[controller construction order](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2376-L2409)。
- refcount 的 init/deinit 中间态会 busy-spin；若 init panic，嵌入式通常走 panic handler/reset而不是在进程内恢复，不能把这个 guard理解为异常安全 transaction。源码可证明中间态和自旋；panic后的系统行为未运行验证。证据：[refcount loop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/refcount.rs#L5-L53)。
- async Wi-Fi等待和 BLE HCI等待没有内建 end-to-end timeout/cancellation result；调用方必须设计 deadline。扫描路径额外用 Drop guard释放 AP list，说明 future cancellation需要逐资源显式处理。证据：[scan cancellation guard](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2901-L2929)。

## 低功耗与恢复

experimental automatic light sleep 只有在无 ready task且无 `WakeLock` 时进入；C5/S3 示例均有 target 分支。S3 会 park另一核，但源码明确保留“另一核被冻结在持有跨核锁的 ISR 中，sleep prep 永久自旋”的罕见 deadlock FIXME。唤醒后必须强制 re-arm被门控的 timer，并唤醒另一核 scheduler。证据：[auto light sleep 条件与 S3风险](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/sleep.rs#L78-L143)、[wake/rearm](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/sleep.rs#L145-L180)、[C5/S3 example](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/async/embassy_auto_light_sleep/src/main.rs#L37-L55)。因此目前更适合实验 profile，不宜直接作为高可靠网关默认策略。

## 对 ESP-111 的可迁移结论

1. **C5：不要把 async 当并行。** 单核上同一 executor 的 future依然互相协作；CSI callback只做拷贝/覆写并唤醒 worker是正确方向。任何 parsing、日志、HTTP或长滤波放进 poll/ISR都会挤压 radio controller task。依据：[单核 metadata](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-metadata/devices/esp32c5/soc.toml#L8-L13)、[同 executor 阻塞反例](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/examples/async/embassy_multiprio/src/main.rs#L43-L67)。
2. **S3：初始化核就是 radio归属决策。** 若尝试把 CSI fusion放 core 1，必须先固定 `WifiController::new()` 的调用核和 radio task core，再用 latency/drop/WDT profile验证；不能只看应用 task affinity。依据：[wifi task core binding](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2341-L2363)。
3. **区分四类通道。** STATE 用 atomic/latest-only或 depth-1 signal；事件流用 bounded pubsub且暴露 lag；命令流用有限 deadline + ACK/retry；radio blob queue保留其 C ABI value-copy合同。不要把一个 queue policy套遍全系统。依据：[state snapshot](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/state.rs#L75-L113)、[event Lagged](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/event.rs#L1299-L1351)、[value-copy queue](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/queue.rs#L662-L715)。
4. **所有 async网络等待外包 deadline。** `connect_async()` 等待 event channel，本身没有恢复超时；ESP-111 的 Wi-Fi重连/Server命令必须有 reason分类、bounded backoff、jitter和状态重读。依据：[connect wait loop](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2962-L3010)。
5. **critical section内不阻塞。** C5 会屏蔽唯一核 interrupts，S3 另一核还可能 spin；queue/semaphore/timer callback中只做 bounded状态更新，重活交 worker。依据：[RawMutex](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-sync/src/lib.rs#L291-L416)、[critical-section sleep crash guard](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/timer/mod.rs#L190-L228)。
6. **timer callback只负责投递。** radio timer task是 priority 2、串行、不可停止；ESP-111 自有周期任务也应让 timer产生 event，不直接执行 CSI fusion、网络 I/O或持久化。依据：[timer task](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/timer.rs#L322-L396)、[serialized callback](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/timer.rs#L418-L496)。
7. **Drop 是合同，不是完整恢复策略。** 可借鉴 Wi-Fi queue drain、configuration rollback和 deferred timer free；同时要为 deinit失败、partial init、worker stop/join、所有 waiter唤醒设计显式状态机与指标。依据：[Wi-Fi drain](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L1000-L1018)、[rollback](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/wifi/mod.rs#L2640-L2712)、[deferred timer free](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio-rtos-driver/src/timer.rs#L609-L632)。
8. **低功耗用 WakeLock状态机。** 连接、CSI高频采样、校准时持锁，稳定空闲才释放；C5/S3都需实测 cadence、丢包、wake latency。S3 auto-light-sleep的跨核 deadlock FIXME意味着生产启用前必须 fault/profile验证。依据：[radio WakeLock](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-radio/src/lib.rs#L301-L359)、[sleep FIXME](https://github.com/esp-rs/esp-hal/blob/035f29ce083794e845a81efe3c43cca6c944c1dd/esp-rtos/src/sleep.rs#L78-L108)。

## 证据边界与未验证项

- 已源码证明：固定 SHA、双许可、C5/S3 target/core差异、executor poll上下文、优先级 ready queue、radio task/IPC adapter、queue/channel/timer边界、Drop与rollback代码路径。
- 未验证：该提交在线 CI具体 job结果、Rust构建、C5/S3真机启动、radio blob运行稳定性、调度时延、queue丢失率、BLE/Wi-Fi/coex互操作、功耗、light-sleep deadlock与所有故障路径。
- 本报告把 CHANGELOG/README 的“支持”视为维护者声明，把 Cargo/metadata/source branch视为静态源码支持；只有运行或 CI证据才能升级为构建/硬件验证。

## 终审状态

- 所有源码证据链接均固定到 `035f29ce083794e845a81efe3c43cca6c944c1dd`；本轮只做静态阅读，没有把推断升级为运行结论。
- 只创建本报告；未编辑总 `findings.md`、`task_plan.md`、`progress.md`、`docs/` 或业务代码。
