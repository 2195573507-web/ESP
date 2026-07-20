#ifndef __BME690_H
#define __BME690_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================== BME690 可调参数区 ============================== */

/* BME690_IIC_PORT：BME690 使用的 I2C 控制器编号。
 * 当前工程 BSP/IIC 默认把 I2C0 配置为主机，因此这里默认填 0，对应 I2C_NUM_0。
 * 调用方法：
 *     bme690_init();                          // 内部会调用 iic_init(BME690_IIC_PORT)
 * 后续调试：
 *     如果 BME690 接到了其它 I2C 控制器，请修改该宏，并同步确认 BSP/IIC 的 SDA/SCL 宏。
 */
#define BME690_IIC_PORT                         0

/* BME690_I2C_ADDR：BME690 的 7bit I2C 地址。
 * 常见地址：
 *     0x76：SDO 引脚接 GND 时使用；
 *     0x77：SDO 引脚接 VDDIO 时使用。
 * 注意：这里必须填写 7bit 地址，不需要左移 1 位。
 */
#define BME690_I2C_ADDR                         0x77

/* BME690_EXPECTED_CHIP_ID：BME690/BME68x 系列常见 chip id。
 * 初始化时会读取 0xD0 寄存器并与该值对比，用于确认 I2C 通信和器件型号是否正常。
 */
#define BME690_EXPECTED_CHIP_ID                 0x61

/* BME690_CHECK_CHIP_ID_ENABLE：是否在初始化时强制检查 chip id。
 * 1：chip id 不等于 BME690_EXPECTED_CHIP_ID 时初始化失败；
 * 0：只打印 chip id，不因为 id 不匹配而返回失败，适合兼容调试阶段使用。
 */
#define BME690_CHECK_CHIP_ID_ENABLE             1

/* BME690_RESET_WAIT_MS：软复位后等待芯片内部恢复的时间，单位 ms。
 * 复位后需要等待寄存器和 NVM 校准数据加载完成，调试异常时可适当加大。
 */
#define BME690_RESET_WAIT_MS                    10

/* BME690_MEAS_WAIT_MS：触发 forced mode 后首次等待测量完成的时间，单位 ms。
 * 该时间需要覆盖温度/湿度/气压转换时间以及 gas heater 加热时间。
 */
#define BME690_MEAS_WAIT_MS                     230

/* BME690_MEAS_TIMEOUT_MS：等待新数据的总超时时间，单位 ms。
 * 如果超过该时间仍未读到 new data 标志，bme690_read() 会返回 ESP_ERR_TIMEOUT。
 */
#define BME690_MEAS_TIMEOUT_MS                  1000

/* BME690_POLL_INTERVAL_MS：轮询 new data 标志的间隔，单位 ms。
 * 数值越小响应越快，但 I2C 访问更频繁；一般保持 10ms 即可。
 */
#define BME690_POLL_INTERVAL_MS                 10

/* ============================== 采样与滤波配置 ============================== */

/* BME690_OSRS_*：温度/湿度/气压过采样倍率编码。
 * 这些值会写入 ctrl_hum 和 ctrl_meas 寄存器，用于平衡噪声、功耗和转换时间。
 */
#define BME690_OSRS_SKIP                        0x00    /* 跳过该通道采样 */
#define BME690_OSRS_1X                          0x01    /* 1 倍过采样，速度快，噪声较高 */
#define BME690_OSRS_2X                          0x02    /* 2 倍过采样，温度常用默认值 */
#define BME690_OSRS_4X                          0x03    /* 4 倍过采样 */
#define BME690_OSRS_8X                          0x04    /* 8 倍过采样 */
#define BME690_OSRS_16X                         0x05    /* 16 倍过采样，压力常用默认值 */

/* BME690_TEMP_OVERSAMPLING：温度过采样倍率。
 * 后续如果温度波动较大可以提高倍率；如果追求低功耗可以降低倍率。
 */
#define BME690_TEMP_OVERSAMPLING                BME690_OSRS_2X

/* BME690_PRESSURE_OVERSAMPLING：气压过采样倍率。
 * 压力读数对噪声较敏感，默认使用 16X；如果转换时间太长可改为 4X 或 8X。
 */
#define BME690_PRESSURE_OVERSAMPLING            BME690_OSRS_16X

/* BME690_HUMIDITY_OVERSAMPLING：湿度过采样倍率。
 * 湿度通常变化较慢，默认 1X 已够用；需要更平滑时可以提高。
 */
#define BME690_HUMIDITY_OVERSAMPLING            BME690_OSRS_1X

/* BME690_FILTER_*：IIR 滤波器系数编码。
 * 滤波越强，输出越平稳，但响应越慢。
 */
#define BME690_FILTER_OFF                       0x00    /* 关闭 IIR 滤波 */
#define BME690_FILTER_SIZE_1                    0x01    /* IIR 系数 1 */
#define BME690_FILTER_SIZE_3                    0x02    /* IIR 系数 3 */
#define BME690_FILTER_SIZE_7                    0x03    /* IIR 系数 7 */
#define BME690_FILTER_SIZE_15                   0x04    /* IIR 系数 15 */
#define BME690_FILTER_SIZE_31                   0x05    /* IIR 系数 31 */
#define BME690_FILTER_SIZE_63                   0x06    /* IIR 系数 63 */
#define BME690_FILTER_SIZE_127                  0x07    /* IIR 系数 127 */

/* BME690_FILTER_SIZE：当前工程使用的 IIR 滤波系数。
 * 环境数据刷新较慢时推荐 3 或 7；如果要看快速变化，可改为 BME690_FILTER_OFF。
 */
#define BME690_FILTER_SIZE                      BME690_FILTER_SIZE_3

/* BME690_STANDBY_TIME：standby 时间编码。
 * 当前模块使用 forced mode，standby 时间基本不参与测量流程，默认 0 即可。
 */
#define BME690_STANDBY_TIME                     0x00

/* ============================== Gas heater 配置 ============================== */

/* BME690_HEATER_PROFILE：gas heater 配置档位。
 * BME690 支持多个 heater profile，本模块默认只使用 profile 0，便于单次 forced mode 读取。
 */
#define BME690_HEATER_PROFILE                   0

/* BME690_GAS_HEATER_TEMP_C：gas heater 目标温度，单位摄氏度。
 * 当前固定为 260 摄氏度，用于降低 heater 强度，避免 raw_gas_adc 长时间顶到 1023 满量程。
 * 后续如果 ADC 仍长期偏高，可继续降低该宏；如果 VOC 响应过弱，可小幅提高该宏。
 */
#define BME690_GAS_HEATER_TEMP_C                260

/* BME690_GAS_HEATER_TEMP_MIN_C：gas heater 目标温度下限，单位摄氏度。
 * 防止误填过低导致 gas 传感器加热不足。
 */
#define BME690_GAS_HEATER_TEMP_MIN_C            200

/* BME690_GAS_HEATER_TEMP_MAX_C：gas heater 目标温度上限，单位摄氏度。
 * 防止误填过高导致功耗和热影响过大。
 */
#define BME690_GAS_HEATER_TEMP_MAX_C            400

/* BME690_GAS_HEATER_DURATION_MS：gas heater 加热保持时间，单位 ms。
 * 当前固定为 100ms，用于缩短单次 forced mode 内的加热时间，降低 gas ADC 饱和概率。
 * 后续调试目标是让 raw_gas_adc 在 400~900 区间动态变化，同时保持 heat_stable 尽量有效。
 */
#define BME690_GAS_HEATER_DURATION_MS           100

/* BME690_AMBIENT_TEMP_C：计算 heater 电阻时使用的环境温度估计值，单位摄氏度。
 * 如果设备长期工作在高温/低温环境，可根据实际环境修改该值。
 */
#define BME690_AMBIENT_TEMP_C                   25

/* BME690_CTRL_GAS_RUN_GAS_ENABLE：开启 gas measurement 的字段值。
 * Bosch BME690/BME69x 官方 SensorAPI 中 ctrl_gas_1 的 run_gas 字段位于 bit[5:4]，
 * 写入字段值 0x01 后实际寄存器位为 0x20。旧 BME68x low gas 常见 0x10，
 * BME690 不能直接写 0x10，否则可能读到错误的 gas 数据区并导致电阻值固定。
 * 调用方法：
 *     ctrl_gas_1 = (BME690_CTRL_GAS_RUN_GAS_ENABLE << BME690_CTRL_GAS_RUN_GAS_POS);
 */
#define BME690_CTRL_GAS_RUN_GAS_ENABLE          0x01

/* BME690_CTRL_GAS_RUN_GAS_DISABLE：关闭 gas measurement 的字段值。
 * 调用方法：
 *     需要临时关闭 gas 测量时，把 run_gas 字段写成该值。
 */
#define BME690_CTRL_GAS_RUN_GAS_DISABLE         0x00

/* BME690_CTRL_GAS_RUN_GAS_POS：ctrl_gas_1 中 run_gas 字段偏移。
 * 官方 BME690/BME69x 定义为 bit[5:4]，字段值需要先左移该位数再写入寄存器。
 */
#define BME690_CTRL_GAS_RUN_GAS_POS             5

/* BME690_CTRL_GAS_RUN_GAS_MASK：ctrl_gas_1 中 run_gas 字段掩码。
 * 用于清除或写入 bit[5:4]，避免修改 nb_conv 等其它字段。
 */
#define BME690_CTRL_GAS_RUN_GAS_MASK            0x30

/* BME690_CTRL_GAS_RUN_GAS_LOW_BITS：BME68x low gas 变体开启 gas 的实际寄存器位。
 * 仅当 variant_id 为 BME690_VARIANT_GAS_LOW 时使用，写入 ctrl_gas_1 的 bit4。
 */
#define BME690_CTRL_GAS_RUN_GAS_LOW_BITS        0x10

/* BME690_CTRL_GAS_RUN_GAS_HIGH_BITS：BME690/BME68x high gas 变体开启 gas 的实际寄存器位。
 * BME690 官方 SensorAPI 开启 gas 后 ctrl_gas_1 的 run_gas 字段实际写入 0x20。
 */
#define BME690_CTRL_GAS_RUN_GAS_HIGH_BITS       0x20

/* BME690_LOG_RAW_DATA_ENABLE：是否打印全部 ADC 原始值。
 * 1：打印温度、气压、湿度、gas ADC 和 gas range，方便整体排查采样数据；
 * 0：关闭通用原始值日志，避免串口日志过多。
 */
#define BME690_LOG_RAW_DATA_ENABLE              0

/* BME690_LOG_TRIGGER_ENABLE：是否打印每次 forced mode 触发日志。
 * 1：每次 bme690_read() 触发测量时打印一行触发日志；
 * 0：关闭触发日志，让串口周期输出主要保留最终数据行。
 */
#define BME690_LOG_TRIGGER_ENABLE               0

/* BME690_LOG_GAS_DEBUG_ENABLE：是否打印 gas 解析和补偿详细日志。
 * 1：每次 bme690_read() 打印 raw_gas_adc、gas_range、range_switching_error、
 *    gas_valid、heat_stable、heater_temp、heater_duration、gas resistance 官方公式中间值和最终 Ohm；
 * 0：关闭 gas 详细调试日志，只保留常规 BME690 数据日志。
 * 后续调试：
 *     gas resistance 已确认会随 VOC 变化后可改为 0，降低串口日志量。
 */
#define BME690_LOG_GAS_DEBUG_ENABLE             0

/* BME690_LOG_PRESSURE_DEBUG_ENABLE：是否打印气压补偿详细日志。
 * 1：每次 bme690_read() 打印 raw_press、t_fine、par_p1~par_p11、官方压力公式中间值、最终 Pa/hPa；
 * 0：关闭详细压力日志，只保留常规数据日志。
 * 后续调试：
 *     气压恢复正常后可改为 0，降低串口日志量。
 */
#define BME690_LOG_PRESSURE_DEBUG_ENABLE        0

/* ============================== BME690 寄存器地址 ============================== */

#define BME690_REG_RES_HEAT_VAL                 0x00    /* heater 电阻校准值寄存器 */
#define BME690_REG_RES_HEAT_RANGE               0x02    /* heater 电阻范围校准寄存器 */
#define BME690_REG_RANGE_SW_ERR                 0x04    /* gas range 切换误差校准寄存器 */
#define BME690_REG_CTRL_GAS_0                   0x70    /* gas heater 开关控制寄存器 */
#define BME690_REG_CTRL_GAS_1                   0x71    /* gas measurement 和 heater profile 控制寄存器 */
#define BME690_REG_CTRL_HUM                     0x72    /* 湿度过采样控制寄存器 */
#define BME690_REG_CTRL_MEAS                    0x74    /* 温度/气压过采样和工作模式控制寄存器 */
#define BME690_REG_CONFIG                       0x75    /* IIR 滤波和 standby 配置寄存器 */
#define BME690_REG_FIELD0                       0x1D    /* forced mode 第 0 组测量结果起始寄存器 */
#define BME690_REG_RES_HEAT_0                   0x5A    /* heater profile 0 电阻配置起始寄存器 */
#define BME690_REG_GAS_WAIT_0                   0x64    /* heater profile 0 加热时间配置起始寄存器 */
#define BME690_REG_CHIP_ID                      0xD0    /* chip id 寄存器 */
#define BME690_REG_SOFT_RESET                   0xE0    /* 软复位寄存器 */
#define BME690_REG_VARIANT_ID                   0xF0    /* BME68x gas 计算变体 ID 寄存器 */
#define BME690_REG_COEFF1_START                 0x8A    /* 第一段校准参数起始寄存器 */
#define BME690_REG_COEFF2_START                 0xE1    /* 第二段校准参数起始寄存器 */

/* ============================== BME690 固定编码与掩码 ============================== */

#define BME690_SOFT_RESET_CMD                   0xB6    /* 写入 soft reset 寄存器的复位命令 */
#define BME690_COEFF1_LEN                       23      /* 第一段校准参数长度，覆盖 0x8A~0xA0 */
#define BME690_COEFF2_LEN                       14      /* 第二段校准参数长度，覆盖 0xE1~0xEE */
#define BME690_FIELD_DATA_LEN                   17      /* field0 测量数据长度 */
#define BME690_MODE_SLEEP                       0x00    /* sleep mode 编码 */
#define BME690_MODE_FORCED                      0x01    /* forced mode 编码，触发单次测量 */
#define BME690_CTRL_MEAS_OSRS_P_POS             2       /* ctrl_meas 中气压过采样字段偏移 */
#define BME690_CTRL_MEAS_OSRS_T_POS             5       /* ctrl_meas 中温度过采样字段偏移 */
#define BME690_CONFIG_FILTER_POS                2       /* config 中 IIR filter 字段偏移 */
#define BME690_CONFIG_STANDBY_POS               5       /* config 中 standby 字段偏移 */
#define BME690_CTRL_GAS_HEAT_OFF_MASK           0x08    /* ctrl_gas_0 中 heater 关闭位，清 0 表示允许加热 */
#define BME690_CTRL_GAS_NB_CONV_MASK            0x0F    /* ctrl_gas_1 中 heater profile 选择掩码 */
#define BME690_FIELD_NEW_DATA_MASK              0x80    /* field0 status 中 new data 标志 */
#define BME690_FIELD_GAS_INDEX_MASK             0x0F    /* field0 status 中 gas index 掩码 */
#define BME690_GAS_RANGE_MASK                   0x0F    /* gas 数据低字节中的 range 掩码 */
#define BME690_HEAT_STAB_MASK                   0x10    /* gas 数据低字节中的 heater stable 标志 */
#define BME690_GAS_VALID_MASK                   0x20    /* gas 数据低字节中的 gas valid 标志 */
#define BME690_GAS_ADC_MSB_SHIFT                2       /* gas ADC 高字节左移位数，官方公式为 msb << 2 */
#define BME690_GAS_ADC_LSB_SHIFT                6       /* gas ADC 低字节高 2bit 右移位数，官方公式为 lsb >> 6 */
#define BME690_GAS_RANGE_COUNT                  16      /* gas_range 共有 0~15 共 16 个量程 */
#define BME690_RES_HEAT_RANGE_MASK              0x30    /* res_heat_range 校准字段掩码 */
#define BME690_RES_HEAT_RANGE_POS               4       /* res_heat_range 校准字段偏移 */
#define BME690_RANGE_SW_ERR_MASK                0xF0    /* range switching error 校准字段掩码 */
#define BME690_VARIANT_GAS_LOW                  0x00    /* BME68x low gas 变体，使用低量程 gas resistance 计算公式 */
#define BME690_VARIANT_BME68X_GAS_HIGH          0x01    /* BME68x high gas 变体，使用高量程 gas resistance 计算公式 */
#define BME690_VARIANT_BME690_GAS_HIGH          0x02    /* BME690/BME69x 官方 high gas 变体，使用 field[15]/field[16] */

/* BME690_GAS_HIGH_*：Bosch BME690/BME69x high gas 官方补偿公式常量。
 * 调用方法：
 *     range_factor = BME690_GAS_HIGH_RANGE_FACTOR_BASE >> gas_range;
 *     denominator = BME690_GAS_HIGH_DENOMINATOR_BASE +
 *                   (raw_gas_adc - BME690_GAS_HIGH_ADC_OFFSET) *
 *                   BME690_GAS_HIGH_ADC_MULTIPLIER;
 *     gas_resistance_ohm = BME690_GAS_HIGH_RESISTANCE_BASE_OHM * range_factor / denominator;
 */
#define BME690_GAS_HIGH_RANGE_FACTOR_BASE       262144U
#define BME690_GAS_HIGH_ADC_OFFSET              512
#define BME690_GAS_HIGH_ADC_MULTIPLIER          3
#define BME690_GAS_HIGH_DENOMINATOR_BASE        4096
#define BME690_GAS_HIGH_RESISTANCE_BASE_OHM     1000000ULL

/* BME690_GAS_LOW_*：Bosch BME68x low gas 官方补偿公式常量。
 * BME690 正常会走 high gas 公式；保留 low gas 公式是为了兼容同系列 low gas 变体。
 */
#define BME690_GAS_LOW_VAR1_BASE                1340
#define BME690_GAS_LOW_RANGE_SW_ERR_GAIN        5
#define BME690_GAS_LOW_LOOKUP1_SHIFT            16
#define BME690_GAS_LOW_ADC_SHIFT                15
#define BME690_GAS_LOW_ADC_OFFSET               16777216
#define BME690_GAS_LOW_LOOKUP2_SHIFT            9

/* ============================== BME690 气压数据解析索引 ============================== */

#define BME690_FIELD_PRESS_MSB_INDEX            2       /* field0 中 pressure ADC 最高字节位置 */
#define BME690_FIELD_PRESS_LSB_INDEX            3       /* field0 中 pressure ADC 中间字节位置 */
#define BME690_FIELD_PRESS_XLSB_INDEX           4       /* field0 中 pressure ADC 最低字节位置；BME690 官方算法按 24bit 全部使用 */
#define BME690_FIELD_TEMP_MSB_INDEX             5       /* field0 中 temperature ADC 最高字节位置 */
#define BME690_FIELD_TEMP_LSB_INDEX             6       /* field0 中 temperature ADC 中间字节位置 */
#define BME690_FIELD_TEMP_XLSB_INDEX            7       /* field0 中 temperature ADC 最低字节位置 */
#define BME690_FIELD_HUM_MSB_INDEX              8       /* field0 中 humidity ADC 高字节位置 */
#define BME690_FIELD_HUM_LSB_INDEX              9       /* field0 中 humidity ADC 低字节位置 */
#define BME690_FIELD_GAS_LOW_MSB_INDEX          13      /* gas low 变体的 gas ADC 高字节位置 */
#define BME690_FIELD_GAS_LOW_LSB_INDEX          14      /* gas low 变体的 gas ADC 低字节/状态位置 */
#define BME690_FIELD_GAS_HIGH_MSB_INDEX         15      /* gas high/BME690 变体的 gas ADC 高字节位置 */
#define BME690_FIELD_GAS_HIGH_LSB_INDEX         16      /* gas high/BME690 变体的 gas ADC 低字节/状态位置 */

/* ============================== BME690 官方气压校准参数索引 ============================== */

#define BME690_COEFF_IDX_P5_LSB                 4       /* par_p5 低字节，Bosch BME690 官方 S_C 参数 */
#define BME690_COEFF_IDX_P5_MSB                 5       /* par_p5 高字节，Bosch BME690 官方 S_C 参数 */
#define BME690_COEFF_IDX_P6_LSB                 6       /* par_p6 低字节，Bosch BME690 官方 TK1S_C 参数 */
#define BME690_COEFF_IDX_P6_MSB                 7       /* par_p6 高字节，Bosch BME690 官方 TK1S_C 参数 */
#define BME690_COEFF_IDX_P7                     8       /* par_p7 单字节有符号参数，Bosch BME690 官方 TK2S_C 参数 */
#define BME690_COEFF_IDX_P8                     9       /* par_p8 单字节有符号参数，Bosch BME690 官方 TK3S_C 参数 */
#define BME690_COEFF_IDX_P1_LSB                 10      /* par_p1 低字节，Bosch BME690 官方 O_C 参数 */
#define BME690_COEFF_IDX_P1_MSB                 11      /* par_p1 高字节，Bosch BME690 官方 O_C 参数 */
#define BME690_COEFF_IDX_P2_LSB                 12      /* par_p2 低字节，Bosch BME690 官方 TK10_C 参数 */
#define BME690_COEFF_IDX_P2_MSB                 13      /* par_p2 高字节，Bosch BME690 官方 TK10_C 参数 */
#define BME690_COEFF_IDX_P3                     14      /* par_p3 单字节有符号参数，Bosch BME690 官方 TK20_C 参数 */
#define BME690_COEFF_IDX_P4                     15      /* par_p4 单字节有符号参数，Bosch BME690 官方 TK30_C 参数 */
#define BME690_COEFF_IDX_P9_LSB                 18      /* par_p9 低字节，Bosch BME690 官方 NLS_C 参数 */
#define BME690_COEFF_IDX_P9_MSB                 19      /* par_p9 高字节，Bosch BME690 官方 NLS_C 参数 */
#define BME690_COEFF_IDX_P10                    20      /* par_p10 单字节有符号参数，Bosch BME690 官方 TKNLS_C 参数 */
#define BME690_COEFF_IDX_P11                    21      /* par_p11 单字节有符号参数，Bosch BME690 官方 NLS3_C 参数，压力三次项需要使用 */

/* ============================== BME690 官方气压公式尺度常量 ============================== */

#define BME690_PRESSURE_OFFSET_SHIFT            3       /* par_p1 在官方压力公式中的左移尺度，对应 O_C * 2^3 */
#define BME690_PRESSURE_TK10_SHIFT              6       /* par_p2 温度一次项尺度，对应 TK10_C / 2^6 */
#define BME690_PRESSURE_TK20_SHIFT              8       /* par_p3 温度二次项尺度，对应 TK20_C / 2^8 */
#define BME690_PRESSURE_TK30_SHIFT              15      /* par_p4 温度三次项尺度，对应 TK30_C / 2^15 */
#define BME690_PRESSURE_S_SHIFT                 20      /* par_p5 灵敏度基值尺度，对应 (S_C - 16384) / 2^20 */
#define BME690_PRESSURE_TK1S_SHIFT              29      /* par_p6 灵敏度温度一次项尺度，对应 (TK1S_C - 16384) / 2^29 */
#define BME690_PRESSURE_TK2S_SHIFT              32      /* par_p7 灵敏度温度二次项尺度，对应 TK2S_C / 2^32 */
#define BME690_PRESSURE_TK3S_SHIFT              37      /* par_p8 灵敏度温度三次项尺度，对应 TK3S_C / 2^37 */
#define BME690_PRESSURE_NLS_SHIFT               48      /* par_p9 非线性二次项尺度，对应 NLS_C / 2^48 */
#define BME690_PRESSURE_TKNLS_SHIFT             48      /* par_p10 非线性二次温度项尺度，对应 TKNLS_C / 2^48 */
#define BME690_PRESSURE_NLS3_SHIFT_A            35      /* par_p11 三次项尺度的第一段，和 SHIFT_B 相乘得到 2^65 */
#define BME690_PRESSURE_NLS3_SHIFT_B            30      /* par_p11 三次项尺度的第二段，避免在 64bit 整数中直接左移 65 位 */
#define BME690_PRESSURE_SENS_OFFSET             16384.0 /* 官方公式中 S_C、TK1S_C 的零点偏置，不能随意修改 */

/* BME690_PRESSURE_HPA_DIVISOR：Pa 转 hPa 的换算系数。
 * 调用方法：
 *     pressure_hpa = pressure_pa / BME690_PRESSURE_HPA_DIVISOR;
 */
#define BME690_PRESSURE_HPA_DIVISOR             100.0f

/* ============================== 数据结构定义 ============================== */

typedef struct _bme690_data_t
{
    uint8_t chip_id;                /* BME690 chip id，正常情况下应为 BME690_EXPECTED_CHIP_ID */
    float temperature_c;            /* 温度，单位：摄氏度 */
    float humidity_percent;         /* 相对湿度，单位：%RH */
    float pressure_pa;              /* 气压，单位：Pa */
    float pressure_hpa;             /* 气压，单位：hPa，便于日志和常规显示 */
    uint32_t gas_resistance_ohm;    /* 气体电阻，单位：Ohm */
    bool new_data;                  /* 是否读到芯片标记的新数据 */
    bool gas_valid;                 /* gas resistance 是否有效 */
    bool heat_stable;               /* heater 是否达到稳定状态 */
    uint8_t gas_range;              /* gas ADC 当前量程，用于调试 gas resistance 计算 */
    uint8_t gas_index;              /* 芯片返回的 gas index，用于调试 profile 顺序 */
    uint8_t measurement_index;      /* 芯片返回的测量序号，用于判断数据更新 */
} bme690_data_t;

/* ============================== 函数声明 ============================== */

/* bme690_init：初始化 BME690。
 *
 * 功能：
 *     1. 调用 BSP/IIC 模块初始化 I2C 主机；
 *     2. 对 BME690 执行 soft reset；
 *     3. 读取并检查 chip id；
 *     4. 读取温度/湿度/气压/gas 校准参数；
 *     5. 配置过采样、IIR 滤波、gas heater 和 forced mode 读取所需寄存器。
 *
 * 返回：
 *     ESP_OK 表示初始化成功，其它值表示失败。
 *
 * 调用方法：
 *     esp_err_t ret = bme690_init();
 *     if (ret != ESP_OK) {
 *         // 初始化失败处理
 *     }
 */
esp_err_t bme690_init(void);

/* bme690_read：读取一次 BME690 数据。
 *
 * 功能：
 *     1. 触发 forced mode 单次测量；
 *     2. 读取 chip field0 原始数据；
 *     3. 根据芯片 NVM 校准参数补偿温度、湿度、气压和 gas resistance；
 *     4. 使用 ESP_LOGI 打印本次读取结果。
 *
 * 参数：
 *     data：保存读取结果的结构体指针，不能为空。
 *
 * 返回：
 *     ESP_OK 表示读取成功，其它值表示失败。
 *
 * 调用方法：
 *     bme690_data_t data = {0};
 *     esp_err_t ret = bme690_read(&data);
 *     if (ret == ESP_OK) {
 *         // data.temperature_c
 *         // data.humidity_percent
 *         // data.pressure_hpa
 *         // data.gas_resistance_ohm
 *     }
 */
esp_err_t bme690_read(bme690_data_t *data);

#endif
