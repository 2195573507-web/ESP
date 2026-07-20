#ifndef IIC_H
#define IIC_H

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

/* IIC_MASTER_PORT：默认 I2C 控制器编号，BME690 默认使用 I2C0。 */
#ifndef IIC_MASTER_PORT
#define IIC_MASTER_PORT I2C_NUM_0
#endif

/* HPIIC0_SDA_GPIO_PIN：I2C0 SDA 引脚，换线时优先修改这里。 */
#ifndef HPIIC0_SDA_GPIO_PIN
#define HPIIC0_SDA_GPIO_PIN GPIO_NUM_2
#endif

/* HPIIC0_SCL_GPIO_PIN：I2C0 SCL 引脚，换线时优先修改这里。 */
#ifndef HPIIC0_SCL_GPIO_PIN
#define HPIIC0_SCL_GPIO_PIN GPIO_NUM_3
#endif

/* IIC_MASTER_FREQ_HZ：I2C 频率，长线或干扰大时可降到 100000。 */
#ifndef IIC_MASTER_FREQ_HZ
#define IIC_MASTER_FREQ_HZ 400000
#endif

#ifndef IIC_FREQ
#define IIC_FREQ IIC_MASTER_FREQ_HZ
#endif

/* IIC_ENABLE_INTERNAL_PULLUP：短线调试可用内部上拉，正式硬件仍建议外接上拉。 */
#ifndef IIC_ENABLE_INTERNAL_PULLUP
#define IIC_ENABLE_INTERNAL_PULLUP 1
#endif

/* IIC_TIMEOUT_MS：普通 I2C 读写超时。 */
#ifndef IIC_TIMEOUT_MS
#define IIC_TIMEOUT_MS 1000
#endif

/* IIC_LOG_TRANSFER_SUCCESS_ENABLE：置 1 时打印每次成功读写，默认关闭避免刷屏。 */
#ifndef IIC_LOG_TRANSFER_SUCCESS_ENABLE
#define IIC_LOG_TRANSFER_SUCCESS_ENABLE 0
#endif

#ifndef IIC_GLITCH_IGNORE_CNT
#define IIC_GLITCH_IGNORE_CNT 7
#endif

#ifndef IIC_MASTER_INTR_PRIORITY
#define IIC_MASTER_INTR_PRIORITY 0
#endif

#ifndef IIC_MASTER_TRANS_QUEUE_DEPTH
#define IIC_MASTER_TRANS_QUEUE_DEPTH 0
#endif

#ifndef IIC_DEVICE_ADDR_LEN
#define IIC_DEVICE_ADDR_LEN I2C_ADDR_BIT_LEN_7
#endif

#ifndef IIC_SCL_WAIT_US
#define IIC_SCL_WAIT_US 0
#endif

#ifndef IIC_ADDR_7BIT_MAX
#define IIC_ADDR_7BIT_MAX 0x7F
#endif

typedef struct {
    i2c_port_num_t port;
    gpio_num_t scl;
    gpio_num_t sda;
    esp_err_t init_flag;
    i2c_master_bus_handle_t bus_handle;
} i2c_obj_t;

extern i2c_obj_t iic_master[I2C_NUM_MAX];

/** 调用方法：传 IIC_MASTER_PORT 初始化总线；返回对象的 init_flag 为 ESP_OK 表示可读写。 */
i2c_obj_t iic_init(uint8_t iic_port);

/** 调用方法：向 7-bit I2C 设备写寄存器或数据；self 用 iic_init() 返回对象。 */
esp_err_t iic_write(i2c_obj_t *self, uint16_t addr, const uint8_t *write_buf, size_t write_len);

/** 调用方法：先写寄存器地址再读取数据；write_len 可为 0 表示直接读。 */
esp_err_t iic_read(i2c_obj_t *self,
                   uint16_t addr,
                   const uint8_t *write_buf,
                   size_t write_len,
                   uint8_t *read_buf,
                   size_t read_len);

#endif /* IIC_H */
