#include "touch_cst816t.h"

#include "esp_log.h"
#include "iic.h"

#define CST816T_I2C_PORT IIC_MASTER_PORT
#define CST816T_I2C_ADDRESS 0x15U
#define CST816T_REG_CHIP_ID 0xA7U
#define CST816T_REG_POINT_DATA 0x01U
#define CST816T_POINT_DATA_LEN 6U
#define CST816T_COORD_HIGH_MASK 0x0FU

static const char *TAG = "LCD_TOUCH";
static i2c_obj_t *s_iic_bus;
static bool s_initialized;

static esp_err_t cst816t_read_registers(uint8_t reg, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_iic_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return iic_read(s_iic_bus,
                    CST816T_I2C_ADDRESS,
                    &reg,
                    sizeof(reg),
                    data,
                    len);
}

esp_err_t cst816t_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2c_obj_t iic = iic_init(CST816T_I2C_PORT);
    if (iic.init_flag != ESP_OK) {
        return iic.init_flag;
    }

    s_iic_bus = &iic_master[CST816T_I2C_PORT];
    if (s_iic_bus->init_flag != ESP_OK || s_iic_bus->bus_handle == NULL) {
        s_iic_bus = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;
    uint8_t chip_id = 0;
    esp_err_t ret = cst816t_read_registers(CST816T_REG_CHIP_ID, &chip_id, sizeof(chip_id));
    if (ret != ESP_OK) {
        s_initialized = false;
        s_iic_bus = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "LCD_TOUCH_INIT_OK");
    return ESP_OK;
}

esp_err_t cst816t_read_point(uint16_t *x, uint16_t *y, bool *pressed)
{
    if (x == NULL || y == NULL || pressed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *x = 0;
    *y = 0;
    *pressed = false;

    uint8_t data[CST816T_POINT_DATA_LEN] = {0};
    esp_err_t ret = cst816t_read_registers(CST816T_REG_POINT_DATA, data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    if (data[1] == 0U) {
        return ESP_OK;
    }

    *x = (uint16_t)((((uint16_t)data[2] & CST816T_COORD_HIGH_MASK) << 8U) | data[3]);
    *y = (uint16_t)((((uint16_t)data[4] & CST816T_COORD_HIGH_MASK) << 8U) | data[5]);
    *pressed = true;

    ESP_LOGI(TAG, "LCD_TOUCH_POINT x=%d y=%d", (int)*x, (int)*y);
    return ESP_OK;
}
