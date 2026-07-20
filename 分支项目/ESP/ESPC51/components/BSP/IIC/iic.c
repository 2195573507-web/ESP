#include "iic.h"

#include "esp_log.h"

static const char *TAG = "IIC";

i2c_obj_t iic_master[I2C_NUM_MAX];

static esp_err_t iic_get_gpio(i2c_port_num_t port, gpio_num_t *sda, gpio_num_t *scl)
{
    if (sda == NULL || scl == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (port) {
    case I2C_NUM_0:
        *sda = HPIIC0_SDA_GPIO_PIN;
        *scl = HPIIC0_SCL_GPIO_PIN;
        return ESP_OK;
    default:
        ESP_LOGE(TAG, "I2C%d SDA/SCL not configured", (int)port);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t iic_check_ready(const i2c_obj_t *self)
{
    if (self == NULL) {
        ESP_LOGE(TAG, "I2C object is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (self->init_flag != ESP_OK || self->bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C%d not initialized", (int)self->port);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t iic_add_device(i2c_obj_t *self, uint16_t addr, i2c_master_dev_handle_t *dev_handle)
{
    if (dev_handle == NULL || addr > IIC_ADDR_7BIT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = iic_check_ready(self);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = IIC_DEVICE_ADDR_LEN,
        .device_address = addr,
        .scl_speed_hz = IIC_MASTER_FREQ_HZ,
        .scl_wait_us = IIC_SCL_WAIT_US,
    };
    ret = i2c_master_bus_add_device(self->bus_handle, &dev_cfg, dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add I2C device 0x%02X failed: %s", (unsigned int)addr, esp_err_to_name(ret));
    }
    return ret;
}

static void iic_remove_device(i2c_master_dev_handle_t dev_handle)
{
    if (dev_handle != NULL) {
        esp_err_t ret = i2c_master_bus_rm_device(dev_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "remove I2C device failed: %s", esp_err_to_name(ret));
        }
    }
}

i2c_obj_t iic_init(uint8_t iic_port)
{
    i2c_obj_t obj = {
        .port = (i2c_port_num_t)iic_port,
        .scl = HPIIC0_SCL_GPIO_PIN,
        .sda = HPIIC0_SDA_GPIO_PIN,
        .init_flag = ESP_FAIL,
        .bus_handle = NULL,
    };

    if (iic_port >= I2C_NUM_MAX) {
        obj.init_flag = ESP_ERR_INVALID_ARG;
        return obj;
    }

    i2c_obj_t *self = &iic_master[iic_port];
    if (self->init_flag == ESP_OK && self->bus_handle != NULL) {
        return *self;
    }

    self->port = (i2c_port_num_t)iic_port;
    self->init_flag = ESP_FAIL;
    self->bus_handle = NULL;

    esp_err_t ret = iic_get_gpio(self->port, &self->sda, &self->scl);
    if (ret != ESP_OK) {
        self->init_flag = ret;
        return *self;
    }

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = self->port,
        .sda_io_num = self->sda,
        .scl_io_num = self->scl,
        .glitch_ignore_cnt = IIC_GLITCH_IGNORE_CNT,
        .intr_priority = IIC_MASTER_INTR_PRIORITY,
        .trans_queue_depth = IIC_MASTER_TRANS_QUEUE_DEPTH,
        .flags.enable_internal_pullup = IIC_ENABLE_INTERNAL_PULLUP,
    };

    ESP_LOGD(TAG, "I2C%d init start SDA=%d SCL=%d freq=%d",
             (int)self->port,
             (int)self->sda,
             (int)self->scl,
             IIC_MASTER_FREQ_HZ);

    ret = i2c_new_master_bus(&bus_cfg, &self->bus_handle);
    self->init_flag = ret;
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "I2C%d init success", (int)self->port);
    } else {
        self->bus_handle = NULL;
        ESP_LOGE(TAG, "I2C%d init fail: %s", (int)self->port, esp_err_to_name(ret));
    }

    return *self;
}

esp_err_t iic_write(i2c_obj_t *self, uint16_t addr, const uint8_t *write_buf, size_t write_len)
{
    if (write_buf == NULL || write_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t ret = iic_add_device(self, addr, &dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_master_transmit(dev_handle, write_buf, write_len, IIC_TIMEOUT_MS);
#if IIC_LOG_TRANSFER_SUCCESS_ENABLE
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "I2C write ok addr=0x%02X len=%u", (unsigned int)addr, (unsigned int)write_len);
    }
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed addr=0x%02X len=%u ret=%s",
                 (unsigned int)addr,
                 (unsigned int)write_len,
                 esp_err_to_name(ret));
    }

    iic_remove_device(dev_handle);
    return ret;
}

esp_err_t iic_read(i2c_obj_t *self,
                   uint16_t addr,
                   const uint8_t *write_buf,
                   size_t write_len,
                   uint8_t *read_buf,
                   size_t read_len)
{
    if (read_buf == NULL || read_len == 0 || (write_len > 0 && write_buf == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t ret = iic_add_device(self, addr, &dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (write_len > 0) {
        ret = i2c_master_transmit_receive(dev_handle,
                                          write_buf,
                                          write_len,
                                          read_buf,
                                          read_len,
                                          IIC_TIMEOUT_MS);
    } else {
        ret = i2c_master_receive(dev_handle, read_buf, read_len, IIC_TIMEOUT_MS);
    }

#if IIC_LOG_TRANSFER_SUCCESS_ENABLE
    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
                 "I2C read ok addr=0x%02X write_len=%u read_len=%u",
                 (unsigned int)addr,
                 (unsigned int)write_len,
                 (unsigned int)read_len);
    }
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "I2C read failed addr=0x%02X write_len=%u read_len=%u ret=%s",
                 (unsigned int)addr,
                 (unsigned int)write_len,
                 (unsigned int)read_len,
                 esp_err_to_name(ret));
    }

    iic_remove_device(dev_handle);
    return ret;
}
