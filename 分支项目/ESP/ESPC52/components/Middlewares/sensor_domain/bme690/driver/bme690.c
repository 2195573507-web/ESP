/**
 * @file bme690.c
 * @brief C5 终端 BME690 I2C 驱动和补偿计算。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责通过 BSP/IIC 初始化 BME690、
 * 读取芯片校准参数、触发 forced mode 测量并把原始 ADC 补偿成物理量。本文件不负责
 * 后台任务、空气质量 baseline、HTTP 上传或 S3/Server 协议。
 */

#include "bme690.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iic.h"

static const char *TAG = "BME690";

/* BME690 校准参数。
 * 这些参数由芯片出厂时写入 NVM，不同芯片不相同。
 * 初始化阶段读取一次，后续每次 bme690_read() 都用它们把 ADC 原始值补偿成真实物理量。
 */
typedef struct _bme690_calib_t
{
    uint16_t par_t1;                /* 温度补偿参数 T1 */
    int16_t par_t2;                 /* 温度补偿参数 T2 */
    int8_t par_t3;                  /* 温度补偿参数 T3 */

    uint16_t par_p1;                /* 气压补偿参数 P1，Bosch BME690 官方 O_C，无符号 16bit */
    uint16_t par_p2;                /* 气压补偿参数 P2，Bosch BME690 官方 TK10_C，无符号 16bit */
    int8_t par_p3;                  /* 气压补偿参数 P3，Bosch BME690 官方 TK20_C，有符号 8bit */
    int8_t par_p4;                  /* 气压补偿参数 P4，Bosch BME690 官方 TK30_C，有符号 8bit */
    int16_t par_p5;                 /* 气压补偿参数 P5，Bosch BME690 官方 S_C，有符号 16bit */
    int16_t par_p6;                 /* 气压补偿参数 P6，Bosch BME690 官方 TK1S_C，有符号 16bit */
    int8_t par_p7;                  /* 气压补偿参数 P7，Bosch BME690 官方 TK2S_C，有符号 8bit */
    int8_t par_p8;                  /* 气压补偿参数 P8，Bosch BME690 官方 TK3S_C，有符号 8bit */
    int16_t par_p9;                 /* 气压补偿参数 P9，Bosch BME690 官方 NLS_C，有符号 16bit */
    int8_t par_p10;                 /* 气压补偿参数 P10，Bosch BME690 官方 TKNLS_C，有符号 8bit */
    int8_t par_p11;                 /* 气压补偿参数 P11，Bosch BME690 官方 NLS3_C，压力三次项需要使用 */

    uint16_t par_h1;                /* 湿度补偿参数 H1 */
    uint16_t par_h2;                /* 湿度补偿参数 H2 */
    int8_t par_h3;                  /* 湿度补偿参数 H3 */
    int8_t par_h4;                  /* 湿度补偿参数 H4 */
    int8_t par_h5;                  /* 湿度补偿参数 H5 */
    uint8_t par_h6;                 /* 湿度补偿参数 H6 */
    int8_t par_h7;                  /* 湿度补偿参数 H7 */

    int8_t par_gh1;                 /* gas heater 补偿参数 GH1 */
    int16_t par_gh2;                /* gas heater 补偿参数 GH2 */
    int8_t par_gh3;                 /* gas heater 补偿参数 GH3 */

    uint8_t res_heat_range;         /* heater 电阻范围校准值 */
    int8_t res_heat_val;            /* heater 电阻修正值 */
    int8_t range_sw_err;            /* gas range 切换误差修正值 */
    uint8_t variant_id;             /* gas resistance 计算公式变体 ID */

    float t_fine;                   /* 温度补偿中间量，气压/湿度补偿也会用到 */
} bme690_calib_t;

/* BME690 原始测量数据。
 * 该结构只在 bme690.c 内部使用，用于把 field0 寄存器解析后的 ADC 值传给补偿函数。
 */
typedef struct _bme690_raw_data_t
{
    uint32_t adc_temperature;        /* 温度 ADC 原始值，20bit，保持现有温度补偿链路不变 */
    uint32_t adc_pressure;           /* 气压 ADC 原始值，24bit，BME690 官方压力公式使用完整 3 字节 */
    uint16_t adc_humidity;           /* 湿度 ADC 原始值，16bit */
    uint16_t adc_gas_resistance;     /* gas ADC 原始值 */
    uint8_t gas_range;              /* gas ADC 量程 */
    bool new_data;                  /* field0 是否为新数据 */
    bool gas_valid;                 /* gas 数据是否有效 */
    bool heat_stable;               /* heater 是否稳定 */
    uint8_t gas_index;              /* gas profile 序号 */
    uint8_t measurement_index;       /* 测量序号 */
} bme690_raw_data_t;

/* BME690 gas resistance 补偿调试数据。
 * 该结构体只在 bme690.c 内部使用，用来把官方公式的关键中间值集中保存，便于日志确认
 * raw_gas_adc、gas_range、range_switching_error 和最终 Ohm 是否符合 Bosch 算法。
 */
typedef struct _bme690_gas_debug_t
{
    uint16_t raw_gas_adc;            /* gas ADC 原始值，官方 field[15] << 2 | field[16] >> 6 */
    uint8_t gas_range;               /* gas ADC 量程，来自 gas 数据低字节 bit[3:0] */
    int8_t range_switching_error;    /* range switching error 校准值，来自 0x04 bit[7:4] */
    bool gas_valid;                  /* gas valid 标志，来自 gas 数据低字节 bit5 */
    bool heat_stable;                /* heater stable 标志，来自 gas 数据低字节 bit4 */
    uint8_t variant_id;              /* 当前芯片 variant_id，用于确认公式分支 */
    uint32_t high_range_factor;      /* high gas 公式中的 262144 >> gas_range */
    int32_t high_adc_offset;         /* high gas 公式中的 raw_gas_adc - 512 */
    int32_t high_denominator;        /* high gas 公式中的 4096 + 3 * (raw_gas_adc - 512) */
    uint64_t high_numerator;         /* high gas 公式中的 1000000 * high_range_factor */
    int64_t low_var1;                /* low gas 公式中的 range_switching_error 修正项 */
    int64_t low_var2;                /* low gas 公式中的 ADC 分母项 */
    int64_t low_var3;                /* low gas 公式中的 lookup_table2 缩放项 */
    uint32_t gas_resistance_ohm;     /* 最终 gas resistance，单位 Ohm */
} bme690_gas_debug_t;

static bme690_calib_t s_calib;       /* 当前 BME690 的校准参数缓存 */
static i2c_obj_t *s_iic = NULL;      /* BSP/IIC 总线对象指针 */
static bool s_inited = false;        /* BME690 初始化完成标志 */
static uint8_t s_chip_id = 0;        /* 缓存 chip id，便于每次 read 返回 */

/* bme690_delay_ms：毫秒级阻塞延时。
 * ESP-IDF v5 推荐在任务上下文使用 vTaskDelay()，这里封装一下便于后续移植替换。
 */
static void bme690_delay_ms(uint32_t delay_ms)
{
    if (delay_ms == 0)
    {
        return;
    }

    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    if (ticks == 0)
    {
        ticks = 1;
    }

    vTaskDelay(ticks);
}

/* bme690_u16_le：把芯片寄存器中的小端 2 字节数据合成为 uint16_t。 */
static uint16_t bme690_u16_le(uint8_t lsb, uint8_t msb)
{
    return (uint16_t)(((uint16_t)msb << 8) | (uint16_t)lsb);
}

/* bme690_i16_le：把芯片寄存器中的小端 2 字节数据合成为 int16_t。 */
static int16_t bme690_i16_le(uint8_t lsb, uint8_t msb)
{
    return (int16_t)bme690_u16_le(lsb, msb);
}

/* bme690_is_high_gas_variant：判断当前芯片是否应使用 high gas 数据区和补偿公式。
 * 调用方法：
 *     if (bme690_is_high_gas_variant()) {
 *         // 使用 field[15]/field[16] 解析 gas ADC，并走 high gas 官方公式
 *     }
 */
static bool bme690_is_high_gas_variant(void)
{
    return (s_calib.variant_id == BME690_VARIANT_BME68X_GAS_HIGH) ||
           (s_calib.variant_id == BME690_VARIANT_BME690_GAS_HIGH);
}

/* bme690_check_ready：检查 BME690 模块是否已经初始化。
 * 所有寄存器访问和读取接口都会先检查，避免未初始化时访问 I2C 总线。
 */
static esp_err_t bme690_check_ready(void)
{
    if ((s_iic == NULL) || (s_iic->init_flag != ESP_OK))
    {
        ESP_LOGE(TAG, "BME690 使用的 I2C 总线未初始化，请先调用 bme690_init()");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/* bme690_write_reg：写 BME690 单个寄存器。
 * 调用方法：
 *     bme690_write_reg(BME690_REG_CTRL_HUM, BME690_HUMIDITY_OVERSAMPLING);
 */
static esp_err_t bme690_write_reg(uint8_t reg_addr, uint8_t data)
{
    esp_err_t ret = bme690_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    uint8_t write_buf[2] = {reg_addr, data};
    ret = iic_write(s_iic, BME690_I2C_ADDR, write_buf, sizeof(write_buf));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "写寄存器失败,reg: 0x%02X, data: 0x%02X, ret: %d", reg_addr, data, ret);
    }

    return ret;
}

/* bme690_read_regs：从 BME690 连续读取寄存器。
 * 调用方法：
 *     uint8_t id = 0;
 *     bme690_read_regs(BME690_REG_CHIP_ID, &id, 1);
 */
static esp_err_t bme690_read_regs(uint8_t reg_addr, uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0))
    {
        ESP_LOGE(TAG, "读寄存器参数错误,data: %p, len: %d", (void *)data, (int)len);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = bme690_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = iic_read(s_iic, BME690_I2C_ADDR, &reg_addr, 1, data, len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "读寄存器失败,reg: 0x%02X, len: %d, ret: %d", reg_addr, (int)len, ret);
    }

    return ret;
}

/* bme690_read_reg：读取 BME690 单个寄存器。 */
static esp_err_t bme690_read_reg(uint8_t reg_addr, uint8_t *data)
{
    return bme690_read_regs(reg_addr, data, 1);
}

/* bme690_soft_reset：执行 BME690 软复位。
 * 复位后芯片会重新加载 NVM 校准参数，所以必须等待 BME690_RESET_WAIT_MS。
 */
static esp_err_t bme690_soft_reset(void)
{
    esp_err_t ret = bme690_write_reg(BME690_REG_SOFT_RESET, BME690_SOFT_RESET_CMD);
    if (ret != ESP_OK)
    {
        return ret;
    }

    bme690_delay_ms(BME690_RESET_WAIT_MS);
    ESP_LOGD(TAG, "BME690 soft reset 完成，等待 %d ms", BME690_RESET_WAIT_MS);
    return ESP_OK;
}

/* bme690_read_chip_id：读取 chip id，用于确认 I2C 地址和芯片连接是否正确。 */
static esp_err_t bme690_read_chip_id(void)
{
    esp_err_t ret = bme690_read_reg(BME690_REG_CHIP_ID, &s_chip_id);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGD(TAG, "读取 chip id 成功,chip id: 0x%02X", s_chip_id);

#if BME690_CHECK_CHIP_ID_ENABLE
    if (s_chip_id != BME690_EXPECTED_CHIP_ID)
    {
        ESP_LOGE(TAG, "chip id 不匹配，期望: 0x%02X, 实际: 0x%02X",
                 BME690_EXPECTED_CHIP_ID, s_chip_id);
        return ESP_ERR_NOT_FOUND;
    }
#endif

    return ESP_OK;
}

/* bme690_read_calibration：读取芯片 NVM 校准参数。
 * BME690 的温度/湿度/气压/gas resistance 都必须通过这些参数补偿，不能直接使用 ADC 原始值。
 */
static esp_err_t bme690_read_calibration(void)
{
    uint8_t coeff1[BME690_COEFF1_LEN] = {0};
    uint8_t coeff2[BME690_COEFF2_LEN] = {0};
    uint8_t reg_data = 0;

    esp_err_t ret = bme690_read_regs(BME690_REG_COEFF1_START, coeff1, sizeof(coeff1));
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_read_regs(BME690_REG_COEFF2_START, coeff2, sizeof(coeff2));
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 温度校准参数：
     * T2/T3 位于 0x8A~0x8C，T1 位于 0xE9~0xEA。
     */
    s_calib.par_t2 = bme690_i16_le(coeff1[0], coeff1[1]);
    s_calib.par_t3 = (int8_t)coeff1[2];
    s_calib.par_t1 = bme690_u16_le(coeff2[8], coeff2[9]);

    /* 气压校准参数：
     * 严格按照 Bosch BME690 SensorAPI 的 coeff_array 下标读取。
     * BME690 与旧 BME68x 压力参数含义不同：P1/P2 是无符号 16bit，P3/P4/P7/P8/P10/P11 是有符号 8bit，
     * P5/P6/P9 是有符号 16bit；大小端均为寄存器低地址保存 LSB、高地址保存 MSB。
     */
    s_calib.par_p5 = bme690_i16_le(coeff1[BME690_COEFF_IDX_P5_LSB], coeff1[BME690_COEFF_IDX_P5_MSB]);
    s_calib.par_p6 = bme690_i16_le(coeff1[BME690_COEFF_IDX_P6_LSB], coeff1[BME690_COEFF_IDX_P6_MSB]);
    s_calib.par_p7 = (int8_t)coeff1[BME690_COEFF_IDX_P7];
    s_calib.par_p8 = (int8_t)coeff1[BME690_COEFF_IDX_P8];
    s_calib.par_p1 = bme690_u16_le(coeff1[BME690_COEFF_IDX_P1_LSB], coeff1[BME690_COEFF_IDX_P1_MSB]);
    s_calib.par_p2 = bme690_u16_le(coeff1[BME690_COEFF_IDX_P2_LSB], coeff1[BME690_COEFF_IDX_P2_MSB]);
    s_calib.par_p3 = (int8_t)coeff1[BME690_COEFF_IDX_P3];
    s_calib.par_p4 = (int8_t)coeff1[BME690_COEFF_IDX_P4];
    s_calib.par_p9 = bme690_i16_le(coeff1[BME690_COEFF_IDX_P9_LSB], coeff1[BME690_COEFF_IDX_P9_MSB]);
    s_calib.par_p10 = (int8_t)coeff1[BME690_COEFF_IDX_P10];
    s_calib.par_p11 = (int8_t)coeff1[BME690_COEFF_IDX_P11];

    /* 湿度校准参数：
     * H1/H2 共享 0xE2 的高低 4bit，这是 Bosch BME68x 系列比较容易写错的地方。
     */
    s_calib.par_h2 = (uint16_t)(((uint16_t)coeff2[0] << 4) | (coeff2[1] >> 4));
    s_calib.par_h1 = (uint16_t)(((uint16_t)coeff2[2] << 4) | (coeff2[1] & 0x0F));
    s_calib.par_h3 = (int8_t)coeff2[3];
    s_calib.par_h4 = (int8_t)coeff2[4];
    s_calib.par_h5 = (int8_t)coeff2[5];
    s_calib.par_h6 = coeff2[6];
    s_calib.par_h7 = (int8_t)coeff2[7];

    /* Gas heater 校准参数。 */
    s_calib.par_gh2 = bme690_i16_le(coeff2[10], coeff2[11]);
    s_calib.par_gh1 = (int8_t)coeff2[12];
    s_calib.par_gh3 = (int8_t)coeff2[13];

    ret = bme690_read_reg(BME690_REG_RES_HEAT_RANGE, &reg_data);
    if (ret != ESP_OK)
    {
        return ret;
    }
    s_calib.res_heat_range = (uint8_t)((reg_data & BME690_RES_HEAT_RANGE_MASK) >> BME690_RES_HEAT_RANGE_POS);

    ret = bme690_read_reg(BME690_REG_RES_HEAT_VAL, &reg_data);
    if (ret != ESP_OK)
    {
        return ret;
    }
    s_calib.res_heat_val = (int8_t)reg_data;

    ret = bme690_read_reg(BME690_REG_RANGE_SW_ERR, &reg_data);
    if (ret != ESP_OK)
    {
        return ret;
    }
    s_calib.range_sw_err = (int8_t)((int8_t)(reg_data & BME690_RANGE_SW_ERR_MASK) / 16);

    ret = bme690_read_reg(BME690_REG_VARIANT_ID, &s_calib.variant_id);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGD(TAG, "校准参数读取完成,variant id: 0x%02X, high_gas:%d, heat_range: %u, heat_val: %d, range_switching_error: %d",
             s_calib.variant_id,
             bme690_is_high_gas_variant(),
             (unsigned int)s_calib.res_heat_range,
             (int)s_calib.res_heat_val,
             (int)s_calib.range_sw_err);
    ESP_LOGD(TAG,
             "气压校准参数 par_p1:%u, par_p2:%u, par_p3:%d, par_p4:%d, par_p5:%d, par_p6:%d, par_p7:%d, par_p8:%d, par_p9:%d, par_p10:%d, par_p11:%d",
             (unsigned int)s_calib.par_p1,
             (unsigned int)s_calib.par_p2,
             (int)s_calib.par_p3,
             (int)s_calib.par_p4,
             (int)s_calib.par_p5,
             (int)s_calib.par_p6,
             (int)s_calib.par_p7,
             (int)s_calib.par_p8,
             (int)s_calib.par_p9,
             (int)s_calib.par_p10,
             (int)s_calib.par_p11);

    return ESP_OK;
}

/* bme690_calc_res_heat：根据目标温度和校准参数计算 heater 电阻寄存器值。
 * 返回值会写入 RES_HEAT_x，芯片内部用它把 heater 控制到目标温度附近。
 */
static uint8_t bme690_calc_res_heat(uint16_t target_temp_c)
{
    if (target_temp_c < BME690_GAS_HEATER_TEMP_MIN_C)
    {
        target_temp_c = BME690_GAS_HEATER_TEMP_MIN_C;
    }
    else if (target_temp_c > BME690_GAS_HEATER_TEMP_MAX_C)
    {
        target_temp_c = BME690_GAS_HEATER_TEMP_MAX_C;
    }

    float var1 = (((float)s_calib.par_gh1 / 16.0f) + 49.0f);
    float var2 = ((((float)s_calib.par_gh2 / 32768.0f) * 0.0005f) + 0.00235f);
    float var3 = ((float)s_calib.par_gh3 / 1024.0f);
    float var4 = var1 * (1.0f + (var2 * (float)target_temp_c));
    float var5 = var4 + (var3 * (float)BME690_AMBIENT_TEMP_C);
    float heat_range = 4.0f / (4.0f + (float)s_calib.res_heat_range);
    float heat_val = 1.0f / (1.0f + ((float)s_calib.res_heat_val * 0.002f));
    float res_heat = 3.4f * ((var5 * heat_range * heat_val) - 25.0f);

    if (res_heat < 0.0f)
    {
        res_heat = 0.0f;
    }
    else if (res_heat > 255.0f)
    {
        res_heat = 255.0f;
    }

    return (uint8_t)(res_heat + 0.5f);
}

/* bme690_calc_gas_wait：把毫秒加热时间转换成 BME690 gas_wait 寄存器编码。
 * gas_wait 使用 6bit mantissa + 2bit multiplier 编码，最大可表达约 4032ms。
 */
static uint8_t bme690_calc_gas_wait(uint16_t duration_ms)
{
    uint8_t factor = 0;

    if (duration_ms >= 4032)
    {
        return 0xFF;
    }

    while (duration_ms > 0x3F)
    {
        duration_ms = (uint16_t)(duration_ms / 4);
        factor++;
    }

    return (uint8_t)(duration_ms + (factor * 64));
}

/* bme690_config_sensor：配置过采样、滤波和 gas heater。
 * 注意：BME690 的湿度过采样写入 ctrl_hum 后，需要再写 ctrl_meas 才会生效。
 */
static esp_err_t bme690_config_sensor(void)
{
    uint8_t reg_data = 0;
    uint8_t ctrl_gas_1 = 0;
    uint8_t run_gas_bits = 0;
    uint8_t res_heat = bme690_calc_res_heat(BME690_GAS_HEATER_TEMP_C);
    uint8_t gas_wait = bme690_calc_gas_wait(BME690_GAS_HEATER_DURATION_MS);
    uint8_t config = (uint8_t)((BME690_STANDBY_TIME << BME690_CONFIG_STANDBY_POS) |
                               (BME690_FILTER_SIZE << BME690_CONFIG_FILTER_POS));
    uint8_t ctrl_meas_sleep = (uint8_t)((BME690_TEMP_OVERSAMPLING << BME690_CTRL_MEAS_OSRS_T_POS) |
                                        (BME690_PRESSURE_OVERSAMPLING << BME690_CTRL_MEAS_OSRS_P_POS) |
                                        BME690_MODE_SLEEP);

    esp_err_t ret = bme690_write_reg(BME690_REG_CTRL_HUM, BME690_HUMIDITY_OVERSAMPLING);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_write_reg(BME690_REG_CONFIG, config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_write_reg(BME690_REG_CTRL_MEAS, ctrl_meas_sleep);
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 配置 gas heater：目标温度、加热时间和 profile 都来自 bme690.h 宏定义。 */
    ret = bme690_write_reg((uint8_t)(BME690_REG_RES_HEAT_0 + BME690_HEATER_PROFILE), res_heat);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_write_reg((uint8_t)(BME690_REG_GAS_WAIT_0 + BME690_HEATER_PROFILE), gas_wait);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_read_reg(BME690_REG_CTRL_GAS_0, &reg_data);
    if (ret != ESP_OK)
    {
        return ret;
    }
    reg_data = (uint8_t)(reg_data & (uint8_t)(~BME690_CTRL_GAS_HEAT_OFF_MASK));
    ret = bme690_write_reg(BME690_REG_CTRL_GAS_0, reg_data);
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* Bosch 官方 SensorAPI 会根据 variant_id 写入不同的 run_gas 位：
     * low gas 变体写 bit4(0x10)，BME690/BME68x high gas 变体写 bit5(0x20)。
     * 这里不能固定写 0x10，否则 BME690 high gas 会被当作 low gas 模式配置，
     * 后续 field[15]/field[16] 的 gas ADC 可能长期不更新，表现为气体电阻固定。
     */
    run_gas_bits = bme690_is_high_gas_variant() ? BME690_CTRL_GAS_RUN_GAS_HIGH_BITS :
                                                  BME690_CTRL_GAS_RUN_GAS_LOW_BITS;
    ctrl_gas_1 = (uint8_t)((run_gas_bits & BME690_CTRL_GAS_RUN_GAS_MASK) |
                           (BME690_HEATER_PROFILE & BME690_CTRL_GAS_NB_CONV_MASK));
    ret = bme690_write_reg(BME690_REG_CTRL_GAS_1, ctrl_gas_1);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGD(TAG,
             "BME690 heater 配置: profile:%u, target:%u C, duration:%u ms, res_heat_reg:0x%02X, gas_wait_reg:0x%02X, run_gas_bits:0x%02X, ctrl_gas_1:0x%02X",
             (unsigned int)BME690_HEATER_PROFILE,
             (unsigned int)BME690_GAS_HEATER_TEMP_C,
             (unsigned int)BME690_GAS_HEATER_DURATION_MS,
             (unsigned int)res_heat,
             (unsigned int)gas_wait,
             (unsigned int)run_gas_bits,
             (unsigned int)ctrl_gas_1);
    ESP_LOGD(TAG, "BME690 配置完成,osrs_t:%u, osrs_p:%u, osrs_h:%u, filter:%u",
             (unsigned int)BME690_TEMP_OVERSAMPLING,
             (unsigned int)BME690_PRESSURE_OVERSAMPLING,
             (unsigned int)BME690_HUMIDITY_OVERSAMPLING,
             (unsigned int)BME690_FILTER_SIZE);

    return ESP_OK;
}

/* bme690_set_forced_mode：触发一次 forced mode 测量。
 * forced mode 会执行一次温度/湿度/气压/gas 测量，完成后芯片自动回到 sleep mode。
 */
static esp_err_t bme690_set_forced_mode(void)
{
    uint8_t ctrl_meas = (uint8_t)((BME690_TEMP_OVERSAMPLING << BME690_CTRL_MEAS_OSRS_T_POS) |
                                  (BME690_PRESSURE_OVERSAMPLING << BME690_CTRL_MEAS_OSRS_P_POS) |
                                  BME690_MODE_FORCED);

    esp_err_t ret = bme690_write_reg(BME690_REG_CTRL_HUM, BME690_HUMIDITY_OVERSAMPLING);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_write_reg(BME690_REG_CTRL_MEAS, ctrl_meas);
#if BME690_LOG_TRIGGER_ENABLE
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "已触发 BME690 forced mode 单次测量");
    }
#endif

    return ret;
}

/* bme690_parse_field_data：解析 field0 寄存器中的 ADC 原始值。
 * BME68x gas low/high 变体的 gas 数据字节位置不同，所以需要根据 variant_id 区分。
 */
static void bme690_parse_field_data(const uint8_t field[BME690_FIELD_DATA_LEN], bme690_raw_data_t *raw)
{
    raw->new_data = (field[0] & BME690_FIELD_NEW_DATA_MASK) != 0;
    raw->gas_index = (uint8_t)(field[0] & BME690_FIELD_GAS_INDEX_MASK);
    raw->measurement_index = field[1];
    /* BME690 官方 SensorAPI 对 pressure ADC 使用完整 24bit：
     * field[2] 是 MSB，field[3] 是 LSB，field[4] 是 XLSB，不能像旧 BME68x 20bit 算法那样右移 4 位。
     * 旧 20bit 拼接会把 raw_press 缩小 16 倍，随后进入 BME690 压力公式会把气压推到 2600~2700hPa 一类的异常值。
     */
    raw->adc_pressure = ((uint32_t)field[BME690_FIELD_PRESS_MSB_INDEX] << 16) |
                        ((uint32_t)field[BME690_FIELD_PRESS_LSB_INDEX] << 8) |
                        (uint32_t)field[BME690_FIELD_PRESS_XLSB_INDEX];
    raw->adc_temperature = ((uint32_t)field[BME690_FIELD_TEMP_MSB_INDEX] << 12) |
                           ((uint32_t)field[BME690_FIELD_TEMP_LSB_INDEX] << 4) |
                           ((uint32_t)field[BME690_FIELD_TEMP_XLSB_INDEX] >> 4);
    raw->adc_humidity = (uint16_t)(((uint16_t)field[BME690_FIELD_HUM_MSB_INDEX] << 8) |
                                   (uint16_t)field[BME690_FIELD_HUM_LSB_INDEX]);

    /* gas ADC 为 10bit：高字节提供 bit[9:2]，低字节 bit[7:6] 提供 bit[1:0]。
     * gas_range/gas_valid/heat_stable 与 ADC 低 2bit 共用同一个低字节，必须先右移 6 位取 ADC，
     * 再用低 4bit 和 bit5/bit4 解析 range 与状态。
     */
    if (bme690_is_high_gas_variant())
    {
        /* BME690/BME68x high gas 官方 SensorAPI 使用 field[15]/field[16]。
         * 这是 BME690 的正常路径，不能误用 low gas 的 field[13]/field[14]。
         */
        raw->adc_gas_resistance = (uint16_t)(((uint16_t)field[BME690_FIELD_GAS_HIGH_MSB_INDEX] << BME690_GAS_ADC_MSB_SHIFT) |
                                             ((uint16_t)field[BME690_FIELD_GAS_HIGH_LSB_INDEX] >> BME690_GAS_ADC_LSB_SHIFT));
        raw->gas_range = (uint8_t)(field[BME690_FIELD_GAS_HIGH_LSB_INDEX] & BME690_GAS_RANGE_MASK);
        raw->gas_valid = (field[BME690_FIELD_GAS_HIGH_LSB_INDEX] & BME690_GAS_VALID_MASK) != 0;
        raw->heat_stable = (field[BME690_FIELD_GAS_HIGH_LSB_INDEX] & BME690_HEAT_STAB_MASK) != 0;
    }
    else
    {
        /* 兼容 BME68x low gas 变体：low gas 数据区位于 field[13]/field[14]。 */
        raw->adc_gas_resistance = (uint16_t)(((uint16_t)field[BME690_FIELD_GAS_LOW_MSB_INDEX] << BME690_GAS_ADC_MSB_SHIFT) |
                                             ((uint16_t)field[BME690_FIELD_GAS_LOW_LSB_INDEX] >> BME690_GAS_ADC_LSB_SHIFT));
        raw->gas_range = (uint8_t)(field[BME690_FIELD_GAS_LOW_LSB_INDEX] & BME690_GAS_RANGE_MASK);
        raw->gas_valid = (field[BME690_FIELD_GAS_LOW_LSB_INDEX] & BME690_GAS_VALID_MASK) != 0;
        raw->heat_stable = (field[BME690_FIELD_GAS_LOW_LSB_INDEX] & BME690_HEAT_STAB_MASK) != 0;
    }
}

/* bme690_read_raw_data：读取并解析一次 field0 原始数据。 */
static esp_err_t bme690_read_raw_data(bme690_raw_data_t *raw)
{
    if (raw == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t field[BME690_FIELD_DATA_LEN] = {0};
    esp_err_t ret = bme690_read_regs(BME690_REG_FIELD0, field, sizeof(field));
    if (ret != ESP_OK)
    {
        return ret;
    }

    bme690_parse_field_data(field, raw);
    return ESP_OK;
}

/* bme690_compensate_temperature：温度补偿计算。
 * 返回单位：摄氏度。
 * 注意：该函数会更新 s_calib.t_fine，气压和湿度补偿必须在温度补偿之后调用。
 */
static float bme690_compensate_temperature(uint32_t adc_temperature)
{
    float var1 = (((float)adc_temperature / 16384.0f) - ((float)s_calib.par_t1 / 1024.0f)) *
                 (float)s_calib.par_t2;
    float var2 = ((((float)adc_temperature / 131072.0f) - ((float)s_calib.par_t1 / 8192.0f)) *
                  (((float)adc_temperature / 131072.0f) - ((float)s_calib.par_t1 / 8192.0f))) *
                 ((float)s_calib.par_t3 * 16.0f);

    s_calib.t_fine = var1 + var2;
    return s_calib.t_fine / 5120.0f;
}

/* bme690_compensate_pressure：气压补偿计算。
 * 返回单位：Pa。
 *
 * 调用方法：
 *     1. 先调用 bme690_compensate_temperature(raw.adc_temperature)，确保 s_calib.t_fine 已更新；
 *     2. 再调用 bme690_compensate_pressure(raw.adc_pressure)，使用本次 pressure ADC 得到 Pa。
 *
 * 算法来源：
 *     Bosch BME690 SensorAPI 的 BME690/BME69x 浮点 pressure compensation 公式。
 *     这里使用 double 保存中间值，避免 24bit pressure ADC 的平方/三次方项在 int32 中溢出。
 */
static float bme690_compensate_pressure(uint32_t adc_pressure)
{
    /* 当前温度输出已经验证正常，这里复用温度补偿得到的 t_fine 换算成摄氏度，
     * 与 Bosch BME690 SensorAPI 的 comp_temperature 参数含义一致。
     */
    double comp_temperature = (double)s_calib.t_fine / 5120.0;

    /* O_C：压力偏移基值，官方公式为 par_p1 * 2^3。 */
    double pressure_offset = (double)s_calib.par_p1 * (double)(1ULL << BME690_PRESSURE_OFFSET_SHIFT);

    /* TK10/TK20/TK30：压力偏移随温度变化的一次、二次、三次修正项。 */
    double tk10 = (double)s_calib.par_p2 / (double)(1ULL << BME690_PRESSURE_TK10_SHIFT);
    double tk20 = (double)s_calib.par_p3 / (double)(1ULL << BME690_PRESSURE_TK20_SHIFT);
    double tk30 = (double)s_calib.par_p4 / (double)(1ULL << BME690_PRESSURE_TK30_SHIFT);

    /* S/TK1S/TK2S/TK3S：压力灵敏度及其温度修正项。
     * par_p5、par_p6 需要先减去 Bosch 规定的 16384 偏置。
     */
    double sensitivity_base = ((double)s_calib.par_p5 - BME690_PRESSURE_SENS_OFFSET) /
                              (double)(1ULL << BME690_PRESSURE_S_SHIFT);
    double tk1s = ((double)s_calib.par_p6 - BME690_PRESSURE_SENS_OFFSET) /
                  (double)(1ULL << BME690_PRESSURE_TK1S_SHIFT);
    double tk2s = (double)s_calib.par_p7 / (double)(1ULL << BME690_PRESSURE_TK2S_SHIFT);
    double tk3s = (double)s_calib.par_p8 / (double)(1ULL << BME690_PRESSURE_TK3S_SHIFT);

    /* NLS/TKNLS/NLS3：压力 ADC 二次、温度耦合二次、三次非线性修正项。
     * 2^65 超出 64bit 左移可表达范围，所以按官方写法拆成 2^35 * 2^30。
     */
    double nls = (double)s_calib.par_p9 / (double)(1ULL << BME690_PRESSURE_NLS_SHIFT);
    double tknls = (double)s_calib.par_p10 / (double)(1ULL << BME690_PRESSURE_TKNLS_SHIFT);
    double nls3 = (double)s_calib.par_p11 /
                  ((double)(1ULL << BME690_PRESSURE_NLS3_SHIFT_A) *
                   (double)(1ULL << BME690_PRESSURE_NLS3_SHIFT_B));

    double temp_square = comp_temperature * comp_temperature;
    double temp_cube = temp_square * comp_temperature;
    double press_adc = (double)adc_pressure;

    /* tmp1：温度修正后的压力偏移量。 */
    double tmp1 = pressure_offset + (tk10 * comp_temperature) + (tk20 * temp_square) + (tk30 * temp_cube);

    /* tmp2：压力 ADC 乘以温度修正后的灵敏度。 */
    double tmp2 = press_adc * (sensitivity_base + (tk1s * comp_temperature) +
                              (tk2s * temp_square) + (tk3s * temp_cube));

    /* tmp3：pressure ADC 的二次非线性修正。 */
    double tmp3 = press_adc * press_adc * (nls + (tknls * comp_temperature));

    /* tmp4：pressure ADC 的三次非线性修正。 */
    double tmp4 = press_adc * press_adc * press_adc * nls3;

    double pressure_pa = tmp1 + tmp2 + tmp3 + tmp4;

#if BME690_LOG_PRESSURE_DEBUG_ENABLE
    ESP_LOGI(TAG, "pressure debug raw_press adc:%lu, t_fine:%.6f, comp_temp:%.6f C",
             (unsigned long)adc_pressure,
             (double)s_calib.t_fine,
             comp_temperature);
    ESP_LOGI(TAG,
             "pressure debug par_p1:%u, par_p2:%u, par_p3:%d, par_p4:%d, par_p5:%d, par_p6:%d, par_p7:%d, par_p8:%d, par_p9:%d, par_p10:%d, par_p11:%d",
             (unsigned int)s_calib.par_p1,
             (unsigned int)s_calib.par_p2,
             (int)s_calib.par_p3,
             (int)s_calib.par_p4,
             (int)s_calib.par_p5,
             (int)s_calib.par_p6,
             (int)s_calib.par_p7,
             (int)s_calib.par_p8,
             (int)s_calib.par_p9,
             (int)s_calib.par_p10,
             (int)s_calib.par_p11);
    ESP_LOGI(TAG, "pressure debug scale o:%.6f, tk10:%.12f, tk20:%.12f, tk30:%.12f",
             pressure_offset,
             tk10,
             tk20,
             tk30);
    ESP_LOGI(TAG, "pressure debug scale s:%.12f, tk1s:%.12f, tk2s:%.12f, tk3s:%.12f",
             sensitivity_base,
             tk1s,
             tk2s,
             tk3s);
    ESP_LOGI(TAG, "pressure debug scale nls:%.18f, tknls:%.18f, nls3:%.24f",
             nls,
             tknls,
             nls3);
    ESP_LOGI(TAG, "pressure debug tmp1_offset:%.6f, tmp2_sensitivity:%.6f, tmp3_nonlinear2:%.6f, tmp4_nonlinear3:%.6f",
             tmp1,
             tmp2,
             tmp3,
             tmp4);
    ESP_LOGI(TAG, "pressure debug final press_pa:%.6f, press_hpa:%.6f",
             pressure_pa,
             pressure_pa / (double)BME690_PRESSURE_HPA_DIVISOR);
#endif

    if (pressure_pa < 0.0)
    {
        ESP_LOGI(TAG, "气压补偿结果小于 0，raw_press:%lu, press_pa:%.6f",
                 (unsigned long)adc_pressure,
                 pressure_pa);
        return 0.0f;
    }

    return (float)pressure_pa;
}

/* bme690_compensate_humidity：湿度补偿计算。
 * 返回单位：%RH，结果会限制在 0~100。
 */
static float bme690_compensate_humidity(uint16_t adc_humidity)
{
    float temperature = s_calib.t_fine / 5120.0f;
    float var1 = (float)adc_humidity -
                 (((float)s_calib.par_h1 * 16.0f) + (((float)s_calib.par_h3 / 2.0f) * temperature));
    float var2 = var1 *
                 (((float)s_calib.par_h2 / 262144.0f) *
                  (1.0f + (((float)s_calib.par_h4 / 16384.0f) * temperature) +
                   (((float)s_calib.par_h5 / 1048576.0f) * temperature * temperature)));
    float var3 = (float)s_calib.par_h6 / 16384.0f;
    float var4 = (float)s_calib.par_h7 / 2097152.0f;
    float humidity = var2 + ((var3 + (var4 * temperature)) * var2 * var2);

    if (humidity > 100.0f)
    {
        humidity = 100.0f;
    }
    else if (humidity < 0.0f)
    {
        humidity = 0.0f;
    }

    return humidity;
}

/* bme690_calc_gas_resistance_low：低量程变体 gas resistance 官方补偿计算。
 * 返回单位：Ohm。
 *
 * 调用方法：
 *     bme690_gas_debug_t gas_debug = {0};
 *     uint32_t ohm = bme690_calc_gas_resistance_low(raw_adc, gas_range, &gas_debug);
 *
 * 算法来源：
 *     Bosch BME68x SensorAPI 的 calc_gas_resistance_low() 整数公式。
 *     low gas 公式必须使用 range_switching_error 和 gas_range 查表补偿，
 *     否则不同量程切换时电阻会出现明显偏差。
 */
static uint32_t bme690_calc_gas_resistance_low(uint16_t adc_gas_resistance,
                                               uint8_t gas_range,
                                               bme690_gas_debug_t *debug)
{
    static const uint32_t lookup_table1[16] = {
        2147483647U, 2147483647U, 2147483647U, 2147483647U,
        2147483647U, 2126008810U, 2147483647U, 2130303777U,
        2147483647U, 2147483647U, 2143188679U, 2136746228U,
        2147483647U, 2126008810U, 2147483647U, 2147483647U,
    };
    static const uint32_t lookup_table2[16] = {
        4096000000U, 2048000000U, 1024000000U, 512000000U,
        255744255U, 127110228U, 64000000U, 32258064U,
        16016016U, 8000000U, 4000000U, 2000000U,
        1000000U, 500000U, 250000U, 125000U,
    };

    gas_range &= BME690_GAS_RANGE_MASK;

    /* var1：官方 range_switching_error 修正项。
     * range_switching_error 是有符号 4bit 校准值，必须参与 low gas 补偿。
     */
    int64_t var1 = (int64_t)(((BME690_GAS_LOW_VAR1_BASE +
                               (BME690_GAS_LOW_RANGE_SW_ERR_GAIN * (int64_t)s_calib.range_sw_err)) *
                              (int64_t)lookup_table1[gas_range]) >>
                             BME690_GAS_LOW_LOOKUP1_SHIFT);

    /* var2：gas ADC 进入官方分母前的缩放项。
     * 使用 int64_t 是为了避免 adc << 15 和后续相加在 32bit 中溢出。
     */
    int64_t var2 = (((int64_t)adc_gas_resistance << BME690_GAS_LOW_ADC_SHIFT) -
                    (int64_t)BME690_GAS_LOW_ADC_OFFSET) +
                   var1;

    /* var3：gas_range 对应量程查表后的缩放项。 */
    int64_t var3 = ((int64_t)lookup_table2[gas_range] * var1) >> BME690_GAS_LOW_LOOKUP2_SHIFT;

    if (debug != NULL)
    {
        debug->low_var1 = var1;
        debug->low_var2 = var2;
        debug->low_var3 = var3;
    }

    if (var2 == 0)
    {
        return 0;
    }

    int64_t gas_resistance = (var3 + (var2 >> 1)) / var2;
    if (gas_resistance < 0)
    {
        return 0;
    }

    return (uint32_t)gas_resistance;
}

/* bme690_calc_gas_resistance_high：高量程变体 gas resistance 官方补偿计算。
 * 返回单位：Ohm。
 *
 * 调用方法：
 *     bme690_gas_debug_t gas_debug = {0};
 *     uint32_t ohm = bme690_calc_gas_resistance_high(raw_adc, gas_range, &gas_debug);
 *
 * 算法来源：
 *     Bosch BME690/BME69x SensorAPI 的 calc_gas_resistance() 公式：
 *     gas_res = 1000000 * (262144 >> gas_range) /
 *               (4096 + 3 * (raw_gas_adc - 512))
 */
static uint32_t bme690_calc_gas_resistance_high(uint16_t adc_gas_resistance,
                                                uint8_t gas_range,
                                                bme690_gas_debug_t *debug)
{
    gas_range &= BME690_GAS_RANGE_MASK;

    /* var1：官方量程缩放项，gas_range 越大，电阻量程越低。 */
    uint32_t var1 = BME690_GAS_HIGH_RANGE_FACTOR_BASE >> gas_range;

    /* var2：官方 ADC 分母项。
     * raw_gas_adc 是 10bit 数据，先减去 512，再乘以 3，最后加 4096。
     */
    int32_t adc_offset = (int32_t)adc_gas_resistance - BME690_GAS_HIGH_ADC_OFFSET;
    int32_t var2 = BME690_GAS_HIGH_DENOMINATOR_BASE +
                   (adc_offset * BME690_GAS_HIGH_ADC_MULTIPLIER);

    /* numerator：使用 64bit 保存 1000000 * var1，避免 32bit 乘法溢出。 */
    uint64_t numerator = BME690_GAS_HIGH_RESISTANCE_BASE_OHM * (uint64_t)var1;

    if (debug != NULL)
    {
        debug->high_range_factor = var1;
        debug->high_adc_offset = adc_offset;
        debug->high_denominator = var2;
        debug->high_numerator = numerator;
    }

    if (var2 <= 0)
    {
        return 0;
    }

    return (uint32_t)(numerator / (uint32_t)var2);
}

/* bme690_compensate_gas_resistance：根据芯片 variant_id 选择 gas resistance 公式。
 * 返回单位：Ohm。
 */
static uint32_t bme690_compensate_gas_resistance(const bme690_raw_data_t *raw)
{
    if (raw == NULL)
    {
        return 0;
    }

    bme690_gas_debug_t gas_debug = {
        .raw_gas_adc = raw->adc_gas_resistance,
        .gas_range = (uint8_t)(raw->gas_range & BME690_GAS_RANGE_MASK),
        .range_switching_error = s_calib.range_sw_err,
        .gas_valid = raw->gas_valid,
        .heat_stable = raw->heat_stable,
        .variant_id = s_calib.variant_id,
    };

    /* BME690/BME69x high gas 变体使用 field[15]/field[16] 和 high gas 公式。
     * low gas 分支只用于兼容 BME68x low gas 变体。
     */
    if (bme690_is_high_gas_variant())
    {
        gas_debug.gas_resistance_ohm =
            bme690_calc_gas_resistance_high(raw->adc_gas_resistance, raw->gas_range, &gas_debug);
    }
    else
    {
        gas_debug.gas_resistance_ohm =
            bme690_calc_gas_resistance_low(raw->adc_gas_resistance, raw->gas_range, &gas_debug);
    }

#if BME690_LOG_GAS_DEBUG_ENABLE
    /* gas ADC 防饱和核心日志：
     * 每次 forced mode 读取后把 raw_gas_adc、gas_range、最终电阻和 heater profile 参数放在同一行，
     * 方便串口侧直接观察 ADC 是否离开 1023，并确认当前固件使用的是 260C / 100ms 配置。
     */
    ESP_LOGI(TAG,
             "gas debug raw_gas_adc:%u, gas_range:%u, gas_resistance_ohm:%lu, heater_temp:%u C, heater_duration:%u ms, range_switching_error:%d, gas_valid:%d, heat_stable:%d, variant_id:0x%02X",
             (unsigned int)gas_debug.raw_gas_adc,
             (unsigned int)gas_debug.gas_range,
             (unsigned long)gas_debug.gas_resistance_ohm,
             (unsigned int)BME690_GAS_HEATER_TEMP_C,
             (unsigned int)BME690_GAS_HEATER_DURATION_MS,
             (int)gas_debug.range_switching_error,
             gas_debug.gas_valid,
             gas_debug.heat_stable,
             (unsigned int)gas_debug.variant_id);

    if (bme690_is_high_gas_variant())
    {
        ESP_LOGI(TAG,
                 "gas compensation high var_range_factor:%lu, var_adc_offset:%ld, denominator:%ld, numerator:%llu",
                 (unsigned long)gas_debug.high_range_factor,
                 (long)gas_debug.high_adc_offset,
                 (long)gas_debug.high_denominator,
                 (unsigned long long)gas_debug.high_numerator);
    }
    else
    {
        ESP_LOGI(TAG,
                 "gas compensation low var1_range_err:%lld, var2_adc_scaled:%lld, var3_lookup_scaled:%lld",
                 (long long)gas_debug.low_var1,
                 (long long)gas_debug.low_var2,
                 (long long)gas_debug.low_var3);
    }

    ESP_LOGI(TAG, "gas debug final gas_resistance_ohm:%lu",
             (unsigned long)gas_debug.gas_resistance_ohm);
#endif

    return gas_debug.gas_resistance_ohm;
}

esp_err_t bme690_init(void)
{
    if (s_inited)
    {
        ESP_LOGD(TAG, "BME690 已初始化,chip id: 0x%02X", s_chip_id);
        return ESP_OK;
    }

    ESP_LOGD(TAG, "开始初始化 BME690,I2C port: %d", BME690_IIC_PORT);
    ESP_LOGD(TAG, "BME690 当前使用的 I2C 地址: 0x%02X", BME690_I2C_ADDR);

    i2c_obj_t iic_obj = iic_init(BME690_IIC_PORT);
    if (iic_obj.init_flag != ESP_OK)
    {
        ESP_LOGE(TAG, "BME690 初始化失败,I2C 初始化 ret: %d", iic_obj.init_flag);
        return iic_obj.init_flag;
    }
    s_iic = &iic_master[BME690_IIC_PORT];

    memset(&s_calib, 0, sizeof(s_calib));

    esp_err_t ret = bme690_soft_reset();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_read_chip_id();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_read_calibration();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = bme690_config_sensor();
    if (ret != ESP_OK)
    {
        return ret;
    }

    s_inited = true;
    ESP_LOGI(TAG, "BME690 初始化成功");

    return ESP_OK;
}

esp_err_t bme690_read(bme690_data_t *data)
{
    if (data == NULL)
    {
        ESP_LOGE(TAG, "bme690_read 参数错误,data 不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_inited)
    {
        esp_err_t init_ret = bme690_init();
        if (init_ret != ESP_OK)
        {
            return init_ret;
        }
    }

    memset(data, 0, sizeof(*data));

    esp_err_t ret = bme690_set_forced_mode();
    if (ret != ESP_OK)
    {
        return ret;
    }

    bme690_delay_ms(BME690_MEAS_WAIT_MS);

    bme690_raw_data_t raw = {0};
    uint32_t waited_ms = BME690_MEAS_WAIT_MS;

    while (waited_ms <= BME690_MEAS_TIMEOUT_MS)
    {
        ret = bme690_read_raw_data(&raw);
        if (ret != ESP_OK)
        {
            return ret;
        }

        if (raw.new_data)
        {
            break;
        }

        bme690_delay_ms(BME690_POLL_INTERVAL_MS);
        waited_ms += BME690_POLL_INTERVAL_MS;
    }

    if (!raw.new_data)
    {
        ESP_LOGW(TAG, "BME690 读取超时，等待 %lu ms 后仍无新数据", (unsigned long)waited_ms);
        return ESP_ERR_TIMEOUT;
    }

#if BME690_LOG_RAW_DATA_ENABLE
    ESP_LOGI(TAG, "原始数据 adc_t:%lu, adc_p:%lu, adc_h:%u, adc_gas:%u, gas_range:%u",
             (unsigned long)raw.adc_temperature,
             (unsigned long)raw.adc_pressure,
             (unsigned int)raw.adc_humidity,
             (unsigned int)raw.adc_gas_resistance,
             (unsigned int)raw.gas_range);
#endif

    data->chip_id = s_chip_id;
    data->temperature_c = bme690_compensate_temperature(raw.adc_temperature);
    data->pressure_pa = bme690_compensate_pressure(raw.adc_pressure);
    data->pressure_hpa = data->pressure_pa / BME690_PRESSURE_HPA_DIVISOR;
    data->humidity_percent = bme690_compensate_humidity(raw.adc_humidity);
    data->gas_resistance_ohm = bme690_compensate_gas_resistance(&raw);
    data->new_data = raw.new_data;
    data->gas_valid = raw.gas_valid;
    data->heat_stable = raw.heat_stable;
    data->gas_adc = raw.adc_gas_resistance;
    data->gas_range = raw.gas_range;
    data->heater_temp = BME690_GAS_HEATER_TEMP_C;
    data->heater_time_ms = BME690_GAS_HEATER_DURATION_MS;
    data->gas_index = raw.gas_index;
    data->measurement_index = raw.measurement_index;

    ESP_LOGD(TAG,
             "BME690 数据: chip:0x%02X, temp:%.2f C, hum:%.2f %%, press:%.2f hPa, gas:%lu Ohm, gas_valid:%d, heat_stable:%d",
             data->chip_id,
             data->temperature_c,
             data->humidity_percent,
             data->pressure_hpa,
             (unsigned long)data->gas_resistance_ohm,
             data->gas_valid,
             data->heat_stable);

    return ESP_OK;
}
