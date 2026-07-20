#include "lcd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCD";

/* LCD 内部句柄：
 *     s_lcd_io：ESP-IDF esp_lcd SPI IO 句柄，负责发送命令和像素数据；
 *     s_lcd_panel：ST7789 面板句柄，负责复位、初始化、设置显示窗口；
 *     s_lcd_line_buf：DMA 可访问的临时像素缓存，lcd_clear()/lcd_fill_rect()/lcd_draw_char() 共用。
 *
 * 说明：
 *     这些变量只在 lcd.c 内部使用，外部业务只通过 lcd.h 里的 6 个 API 访问 LCD，
 *     这样后续移植 LCD 模块时不会牵连 ENV/BME690/IIC。
 */
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static uint16_t *s_lcd_line_buf = NULL;
static size_t s_lcd_line_buf_pixels = 0;
static bool s_lcd_initialized = false;

typedef struct _lcd_font5x7_t
{
    char ch;             /* ASCII 字符 */
    uint8_t column[5];   /* 5 列点阵数据，bit0 为顶部像素，bit6 为底部像素 */
} lcd_font5x7_t;

/* 5x7 ASCII 点阵字库。
 * 功能：
 *     lcd_draw_char() 通过字符查表得到 5 列点阵，再按 LCD_FONT_SCALE 放大显示。
 * 范围：
 *     覆盖当前点亮测试需要的 "ESP32-C5"、"SensAir Shuttle"，并补充常用数字和英文字母。
 * 扩展方法：
 *     后续如果需要更多符号，在该表追加 {字符, {5列点阵}} 即可，不影响 LCD 对外 API。
 */
static const lcd_font5x7_t s_lcd_font5x7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x7F, 0x20, 0x18, 0x20, 0x7F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'a', {0x20, 0x54, 0x54, 0x54, 0x78}},
    {'b', {0x7F, 0x48, 0x44, 0x44, 0x38}},
    {'c', {0x38, 0x44, 0x44, 0x44, 0x20}},
    {'d', {0x38, 0x44, 0x44, 0x48, 0x7F}},
    {'e', {0x38, 0x54, 0x54, 0x54, 0x18}},
    {'f', {0x08, 0x7E, 0x09, 0x01, 0x02}},
    {'g', {0x0C, 0x52, 0x52, 0x52, 0x3E}},
    {'h', {0x7F, 0x08, 0x04, 0x04, 0x78}},
    {'i', {0x00, 0x44, 0x7D, 0x40, 0x00}},
    {'j', {0x20, 0x40, 0x44, 0x3D, 0x00}},
    {'k', {0x7F, 0x10, 0x28, 0x44, 0x00}},
    {'l', {0x00, 0x41, 0x7F, 0x40, 0x00}},
    {'m', {0x7C, 0x04, 0x18, 0x04, 0x78}},
    {'n', {0x7C, 0x08, 0x04, 0x04, 0x78}},
    {'o', {0x38, 0x44, 0x44, 0x44, 0x38}},
    {'p', {0x7C, 0x14, 0x14, 0x14, 0x08}},
    {'q', {0x08, 0x14, 0x14, 0x18, 0x7C}},
    {'r', {0x7C, 0x08, 0x04, 0x04, 0x08}},
    {'s', {0x48, 0x54, 0x54, 0x54, 0x20}},
    {'t', {0x04, 0x3F, 0x44, 0x40, 0x20}},
    {'u', {0x3C, 0x40, 0x40, 0x20, 0x7C}},
    {'v', {0x1C, 0x20, 0x40, 0x20, 0x1C}},
    {'w', {0x3C, 0x40, 0x30, 0x40, 0x3C}},
    {'x', {0x44, 0x28, 0x10, 0x28, 0x44}},
    {'y', {0x0C, 0x50, 0x50, 0x50, 0x3C}},
    {'z', {0x44, 0x64, 0x54, 0x4C, 0x44}},
    {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
};

/* lcd_rgb_order_to_string：把 RGB/BGR 枚举转换为日志字符串。
 *
 * 功能：
 *     lcd_init() 打印颜色配置时使用，让串口日志能直接看到当前是 RGB 还是 BGR。
 *
 * 调用方法：
 *     ESP_LOGI(TAG, "order=%s", lcd_rgb_order_to_string(LCD_RGB_ELEMENT_ORDER));
 *
 * 说明：
 *     该函数只服务 LCD 模块内部日志，不影响 ST7789 初始化流程。
 */
static const char *lcd_rgb_order_to_string(lcd_rgb_element_order_t order)
{
    switch (order)
    {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        return "LCD_RGB_ELEMENT_ORDER_RGB";
    case LCD_RGB_ELEMENT_ORDER_BGR:
        return "LCD_RGB_ELEMENT_ORDER_BGR";
    default:
        return "LCD_RGB_ELEMENT_ORDER_UNKNOWN";
    }
}

/* lcd_rgb_endian_to_string：把 RGB565 数据字节序枚举转换为日志字符串。
 *
 * 功能：
 *     lcd_init() 打印 LCD_RGB_DATA_ENDIAN 时使用，方便确认当前 ST7789 RAMCTRL 字节序配置。
 *
 * 调用方法：
 *     ESP_LOGI(TAG, "endian=%s", lcd_rgb_endian_to_string(LCD_RGB_DATA_ENDIAN));
 *
 * 说明：
 *     ESP-IDF 的 ST7789 驱动会根据该枚举配置 ST7789 的 RAMCTRL 寄存器，
 *     但 SPI IO 发送颜色缓冲区时不会自动交换 uint16_t 内存里的高低字节。
 */
static const char *lcd_rgb_endian_to_string(lcd_rgb_data_endian_t endian)
{
    switch (endian)
    {
    case LCD_RGB_DATA_ENDIAN_BIG:
        return "LCD_RGB_DATA_ENDIAN_BIG";
    case LCD_RGB_DATA_ENDIAN_LITTLE:
        return "LCD_RGB_DATA_ENDIAN_LITTLE";
    default:
        return "LCD_RGB_DATA_ENDIAN_UNKNOWN";
    }
}

/* lcd_color_to_panel：把标准 RGB565 颜色转换成 LCD SPI 实际发送顺序。
 *
 * 参数：
 *     color：调用者传入的标准 RGB565 颜色，例如 0xF800 表示红色。
 *
 * 返回：
 *     写入 DMA 缓冲区的 16bit 数据。
 *
 * 说明：
 *     1. ESP32-C5 是小端 CPU，uint16_t color=0xF800 写入内存后字节顺序是 00 F8。
 *     2. ESP-IDF 5.5.4 的 esp_lcd ST7789 驱动中，LCD_RGB_DATA_ENDIAN_BIG 会配置
 *        ST7789 的 RAMCTRL 寄存器，让屏幕按高字节在前解释 RGB565 数据。
 *     3. esp_lcd 的 SPI IO 层发送颜色数据时使用调用者传入的 tx_buffer，不会再帮
 *        uint16_t 缓冲区做一次高低字节交换，因此这里的 LCD_COLOR_SWAP_BYTES 不是
 *        与 SPI IO 重复交换，而是把小端内存整理成屏幕期望的线上字节顺序。
 *     4. 如果后续把 LCD_RGB_DATA_ENDIAN 改成 LCD_RGB_DATA_ENDIAN_LITTLE，通常也要
 *        把 LCD_COLOR_SWAP_BYTES 改为 0，再通过 lcd_color_test() 实测确认。
 */
static uint16_t lcd_color_to_panel(uint16_t color)
{
#if LCD_COLOR_SWAP_BYTES
    return (uint16_t)((color << 8) | (color >> 8));
#else
    return color;
#endif
}

/* lcd_check_ready：检查 LCD 是否已经完成初始化。
 *
 * 功能：
 *     所有绘图函数都会先调用该函数，避免在 lcd_init() 失败或未调用时访问空句柄。
 */
static esp_err_t lcd_check_ready(void)
{
    if ((!s_lcd_initialized) || (s_lcd_panel == NULL) || (s_lcd_line_buf == NULL))
    {
        ESP_LOGE(TAG, "LCD 未初始化，请先调用 lcd_init()");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/* lcd_gpio_to_pin_mask：把单个 GPIO 编号转换为 gpio_config() 使用的 bit mask。
 *
 * 参数：
 *     gpio_num：已经确认连接到 MCU 的 GPIO 编号，不能传入 GPIO_NUM_NC。
 *
 * 返回：
 *     gpio_config_t.pin_bit_mask 需要的 64bit 掩码。
 *
 * 调用方法：
 *     uint64_t mask = lcd_gpio_to_pin_mask(LCD_PIN_NUM_BK_LIGHT);
 *
 * 说明：
 *     LCD 背光当前未连接 MCU，LCD_PIN_NUM_BK_LIGHT 为 GPIO_NUM_NC。
 *     只有在调用者确认 gpio_num 不是 GPIO_NUM_NC 后才生成 bit mask，避免对负数 GPIO 做位移。
 */
static uint64_t lcd_gpio_to_pin_mask(gpio_num_t gpio_num)
{
    return 1ULL << (uint32_t)gpio_num;
}

/* lcd_wait_flush_done：等待上一笔 LCD 颜色数据发送完成。
 *
 * 功能：
 *     esp_lcd_panel_draw_bitmap() 会把颜色数据提交到 SPI 队列后返回；
 *     本模块复用同一块 DMA 行缓存，因此每次改写缓存前都要先等待上一笔传输结束，
 *     避免 lcd_draw_string() 连续绘制字符时覆盖还没发送完的像素数据。
 *
 * 调用方法：
 *     该函数只在 lcd.c 内部使用，外部业务不需要直接调用。
 */
static esp_err_t lcd_wait_flush_done(void)
{
    if (s_lcd_io == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_lcd_panel_io_tx_param(s_lcd_io, -1, NULL, 0);
}

/* lcd_config_backlight：初始化并关闭背光。
 *
 * 说明：
 *     初始化阶段先关闭背光，等屏幕寄存器配置完成后再打开，可以减少上电闪屏。
 */
static esp_err_t lcd_config_backlight(void)
{
    gpio_num_t bk_light_gpio = LCD_PIN_NUM_BK_LIGHT;

    if (bk_light_gpio == GPIO_NUM_NC)
    {
        return ESP_OK;
    }

    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = lcd_gpio_to_pin_mask(bk_light_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&bk_gpio_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "背光 GPIO 配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    gpio_set_level(bk_light_gpio, LCD_BK_LIGHT_OFF_LEVEL);
    return ESP_OK;
}

/* lcd_set_backlight：设置背光开关。
 *
 * 参数：
 *     on：true 表示打开背光，false 表示关闭背光。
 */
static void lcd_set_backlight(bool on)
{
    gpio_num_t bk_light_gpio = LCD_PIN_NUM_BK_LIGHT;

    if (bk_light_gpio == GPIO_NUM_NC)
    {
        return;
    }

    gpio_set_level(bk_light_gpio, on ? LCD_BK_LIGHT_ON_LEVEL : LCD_BK_LIGHT_OFF_LEVEL);
}

/* lcd_find_glyph：查找字符点阵。
 *
 * 返回：
 *     找到字符时返回对应点阵；未找到时返回 '?'，保证 draw_string 不会因为单个未知字符中断。
 */
static const uint8_t *lcd_find_glyph(char ch)
{
    for (size_t i = 0; i < (sizeof(s_lcd_font5x7) / sizeof(s_lcd_font5x7[0])); i++)
    {
        if (s_lcd_font5x7[i].ch == ch)
        {
            return s_lcd_font5x7[i].column;
        }
    }

    return s_lcd_font5x7[sizeof(s_lcd_font5x7) / sizeof(s_lcd_font5x7[0]) - 1].column;
}

esp_err_t lcd_init(void)
{
    if (s_lcd_initialized)
    {
        ESP_LOGI(TAG, "LCD 已初始化，直接返回");
        return ESP_OK;
    }

    esp_err_t ret = lcd_config_backlight();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGI(TAG, "LCD 颜色测试方案: %s", LCD_COLOR_ACTIVE_TEST_SCHEME_NAME);
    ESP_LOGI(TAG, "LCD_RGB_ELEMENT_ORDER=%s (%d)",
             lcd_rgb_order_to_string(LCD_RGB_ELEMENT_ORDER),
             (int)LCD_RGB_ELEMENT_ORDER);
    ESP_LOGI(TAG, "LCD_RGB_DATA_ENDIAN=%s (%d)",
             lcd_rgb_endian_to_string(LCD_RGB_DATA_ENDIAN),
             (int)LCD_RGB_DATA_ENDIAN);
    ESP_LOGI(TAG, "LCD_COLOR_SWAP_BYTES=%d，RGB565 红色 0x%04X 写入缓冲区后为 0x%04X",
             LCD_COLOR_SWAP_BYTES,
             LCD_COLOR_RED,
             lcd_color_to_panel(LCD_COLOR_RED));
    ESP_LOGI(TAG, "LCD_COLOR_INVERT_ENABLE=%d，%s LCD 反色命令",
             LCD_COLOR_INVERT_ENABLE,
             LCD_COLOR_INVERT_ENABLE ? "开启" : "关闭");

    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_NUM_SCLK,
        .mosi_io_num = LCD_PIN_NUM_MOSI,
        .miso_io_num = LCD_PIN_NUM_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_MAX_TRANSFER_SIZE,
    };

    ESP_LOGI(TAG, "初始化 LCD SPI 总线，SCLK=%d, MOSI=%d, DC=%d, CS=%d, RST=%d, BL=%d",
             (int)LCD_PIN_NUM_SCLK,
             (int)LCD_PIN_NUM_MOSI,
             (int)LCD_PIN_NUM_DC,
             (int)LCD_PIN_NUM_CS,
             (int)LCD_PIN_NUM_RST,
             (int)LCD_PIN_NUM_BK_LIGHT);

    /* STEP1：初始化 LCD 使用的 SPI 总线。
     * 功能：
     *     把 LCD_SCL、LCD_SDA 等 SPI 引脚注册到 ESP-IDF SPI 主机驱动。
     * 调用方法：
     *     lcd_init();     // 内部执行本步骤，并打印 STEP1 的开始日志和返回值。
     */
    ESP_LOGI(TAG, "STEP1 spi_bus_initialize begin");
    ret = spi_bus_initialize(LCD_SPI_HOST, &bus_config, LCD_SPI_DMA_CH);
    ESP_LOGI(TAG, "STEP1 spi_bus_initialize ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE))
    {
        ESP_LOGE(TAG, "LCD SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "LCD SPI 总线已被初始化，继续挂载 LCD 设备");
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_NUM_CS,
        .dc_gpio_num = LCD_PIN_NUM_DC,
        .spi_mode = LCD_SPI_MODE,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = LCD_TRANS_QUEUE_DEPTH,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };

    /* STEP2：创建 esp_lcd 的 SPI IO 对象。
     * 功能：
     *     绑定 LCD_CS、LCD_DC、SPI 模式、像素时钟和命令/参数位宽。
     * 调用方法：
     *     lcd_init();     // 内部执行本步骤，并打印 STEP2 的开始日志和返回值。
     */
    ESP_LOGI(TAG, "STEP2 esp_lcd_new_panel_io_spi begin");
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_lcd_io);
    ESP_LOGI(TAG, "STEP2 esp_lcd_new_panel_io_spi ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建 LCD SPI IO 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER,
        .data_endian = LCD_RGB_DATA_ENDIAN,
        .bits_per_pixel = 16,
    };

    /* STEP3：创建 ST7789 面板对象。
     * 功能：
     *     使用 STEP2 创建的 SPI IO 对象生成 ST7789 面板控制句柄。
     * 调用方法：
     *     lcd_init();     // 内部执行本步骤，并打印 STEP3 的开始日志和返回值。
     */
    ESP_LOGI(TAG, "STEP3 esp_lcd_new_panel_st7789 begin");
    ret = esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel);
    ESP_LOGI(TAG, "STEP3 esp_lcd_new_panel_st7789 ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建 ST7789 LCD 面板失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* STEP4：复位 LCD 面板。
     * 功能：
     *     LCD_RST 未连接 MCU，因此 esp_lcd 会通过软件复位命令执行复位流程。
     * 调用方法：
     *     lcd_init();     // 内部执行本步骤，并打印 STEP4 的开始日志和返回值。
     */
    ESP_LOGI(TAG, "STEP4 esp_lcd_panel_reset begin");
    ret = esp_lcd_panel_reset(s_lcd_panel);
    ESP_LOGI(TAG, "STEP4 esp_lcd_panel_reset ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD 复位失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* STEP5：初始化 LCD 面板寄存器。
     * 功能：
     *     向 ST7789 发送初始化命令序列，使面板进入可显示状态。
     * 调用方法：
     *     lcd_init();     // 内部执行本步骤，并打印 STEP5 的开始日志和返回值。
     */
    ESP_LOGI(TAG, "STEP5 esp_lcd_panel_init begin");
    ret = esp_lcd_panel_init(s_lcd_panel);
    ESP_LOGI(TAG, "STEP5 esp_lcd_panel_init ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_invert_color(s_lcd_panel, LCD_COLOR_INVERT_ENABLE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD 反色配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_swap_xy(s_lcd_panel, LCD_SWAP_XY_ENABLE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD XY 轴交换配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_mirror(s_lcd_panel, LCD_MIRROR_X_ENABLE, LCD_MIRROR_Y_ENABLE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD 镜像配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(s_lcd_panel, LCD_X_GAP, LCD_Y_GAP);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD 显示偏移配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* STEP6：保持 LCD 面板显示关闭。
     * 功能：
     *     LVGL 完成任务、display、draw buffer 和初始 UI 后，再由 lcd_service_start() 打开显示。
     * 调用方法：
     *     lcd_init();     // 内部执行本步骤，并打印 STEP6 的开始日志和返回值。
     */
    ESP_LOGI(TAG, "STEP6 esp_lcd_panel_disp_on_off(false) begin");
    ret = esp_lcd_panel_disp_on_off(s_lcd_panel, false);
    ESP_LOGI(TAG, "STEP6 esp_lcd_panel_disp_on_off(false) ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD 保持显示关闭失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_lcd_line_buf_pixels = LCD_H_RES * LCD_DRAW_BUFFER_LINES;
    s_lcd_line_buf = (uint16_t *)heap_caps_malloc(s_lcd_line_buf_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (s_lcd_line_buf == NULL)
    {
        ESP_LOGE(TAG, "申请 LCD DMA 行缓存失败，size=%d bytes", (int)(s_lcd_line_buf_pixels * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    lcd_set_backlight(true);
    s_lcd_initialized = true;

    ESP_LOGI(TAG, "LCD 初始化完成，分辨率 %dx%d，SPI 时钟 %d Hz", LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLOCK_HZ);
    return ESP_OK;
}

esp_lcd_panel_io_handle_t lcd_get_io_handle(void)
{
    return s_lcd_initialized ? s_lcd_io : NULL;
}

esp_lcd_panel_handle_t lcd_get_panel_handle(void)
{
    return s_lcd_initialized ? s_lcd_panel : NULL;
}

esp_err_t lcd_set_display_enabled(bool enabled)
{
    if ((!s_lcd_initialized) || (s_lcd_panel == NULL))
    {
        ESP_LOGE(TAG, "LCD display state change rejected: LCD is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG,
             "LCD display-%s begin",
             enabled ? "on" : "off");
    esp_err_t ret = esp_lcd_panel_disp_on_off(s_lcd_panel, enabled);
    ESP_LOGI(TAG,
             "LCD display-%s ret=%d (%s)",
             enabled ? "on" : "off",
             (int)ret,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t lcd_release_legacy_draw_buffer(void)
{
    if ((!s_lcd_initialized) || (s_lcd_panel == NULL) || (s_lcd_io == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lcd_line_buf == NULL)
    {
        return ESP_OK;
    }

    esp_err_t ret = lcd_wait_flush_done();
    if (ret != ESP_OK)
    {
        return ret;
    }

    heap_caps_free(s_lcd_line_buf);
    s_lcd_line_buf = NULL;
    s_lcd_line_buf_pixels = 0;
    ESP_LOGI(TAG, "released legacy LCD DMA buffer before LVGL start");
    return ESP_OK;
}
esp_err_t lcd_clear(uint16_t color)
{
    return lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
}

esp_err_t lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    esp_err_t ret = lcd_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if ((x >= LCD_H_RES) || (y >= LCD_V_RES))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = lcd_wait_flush_done();
    if (ret != ESP_OK)
    {
        return ret;
    }

    s_lcd_line_buf[0] = lcd_color_to_panel(color);
    return esp_lcd_panel_draw_bitmap(s_lcd_panel, x, y, x + 1, y + 1, s_lcd_line_buf);
}

esp_err_t lcd_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color)
{
    esp_err_t ret = lcd_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if ((width == 0) || (height == 0))
    {
        return ESP_OK;
    }

    if ((x >= LCD_H_RES) || (y >= LCD_V_RES))
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t x_end = x + width;
    uint16_t y_end = y + height;

    if (x_end > LCD_H_RES)
    {
        x_end = LCD_H_RES;
    }

    if (y_end > LCD_V_RES)
    {
        y_end = LCD_V_RES;
    }

    uint16_t draw_width = x_end - x;
    uint16_t draw_height = y_end - y;
    size_t chunk_lines = s_lcd_line_buf_pixels / draw_width;

    if (chunk_lines == 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (chunk_lines > draw_height)
    {
        chunk_lines = draw_height;
    }

    uint16_t panel_color = lcd_color_to_panel(color);
    size_t chunk_pixels = draw_width * chunk_lines;

    ret = lcd_wait_flush_done();
    if (ret != ESP_OK)
    {
        return ret;
    }

    for (size_t i = 0; i < chunk_pixels; i++)
    {
        s_lcd_line_buf[i] = panel_color;
    }

    uint16_t current_y = y;
    uint16_t remaining_lines = draw_height;

    while (remaining_lines > 0)
    {
        uint16_t lines = (remaining_lines > chunk_lines) ? (uint16_t)chunk_lines : remaining_lines;

        ret = esp_lcd_panel_draw_bitmap(s_lcd_panel,
                                        x,
                                        current_y,
                                        x_end,
                                        current_y + lines,
                                        s_lcd_line_buf);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "LCD 填充矩形失败: %s", esp_err_to_name(ret));
            return ret;
        }

        current_y += lines;
        remaining_lines -= lines;
    }

    return ESP_OK;
}

esp_err_t lcd_draw_bitmap(uint16_t x,
                          uint16_t y,
                          uint16_t width,
                          uint16_t height,
                          const uint16_t *pixels)
{
    esp_err_t ret = lcd_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (pixels == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((width == 0) || (height == 0))
    {
        return ESP_OK;
    }

    size_t x_end = (size_t)x + (size_t)width;
    size_t y_end = (size_t)y + (size_t)height;

    if ((x_end > LCD_H_RES) || (y_end > LCD_V_RES))
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t chunk_lines = s_lcd_line_buf_pixels / width;
    if (chunk_lines == 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (chunk_lines > height)
    {
        chunk_lines = height;
    }

    size_t src_offset = 0;
    uint16_t current_y = y;
    size_t remaining_lines = height;

    while (remaining_lines > 0)
    {
        uint16_t lines = (remaining_lines > chunk_lines) ? (uint16_t)chunk_lines : (uint16_t)remaining_lines;
        size_t chunk_pixels = (size_t)width * lines;

        ret = lcd_wait_flush_done();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "LCD bitmap wait flush failed: %s", esp_err_to_name(ret));
            return ret;
        }

        for (size_t i = 0; i < chunk_pixels; i++)
        {
            s_lcd_line_buf[i] = lcd_color_to_panel(pixels[src_offset + i]);
        }

        ret = esp_lcd_panel_draw_bitmap(s_lcd_panel,
                                        x,
                                        current_y,
                                        (int)x_end,
                                        current_y + lines,
                                        s_lcd_line_buf);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "LCD bitmap draw failed: %s", esp_err_to_name(ret));
            return ret;
        }

        src_offset += chunk_pixels;
        current_y += lines;
        remaining_lines -= lines;
    }

    return ESP_OK;
}

esp_err_t lcd_draw_char(uint16_t x, uint16_t y, char ch, uint16_t color, uint16_t bg_color)
{
    esp_err_t ret = lcd_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    const uint16_t scale = LCD_FONT_SCALE;
    const uint16_t char_width = (LCD_FONT_WIDTH + LCD_FONT_SPACE_X) * scale;
    const uint16_t char_height = (LCD_FONT_HEIGHT + LCD_FONT_SPACE_Y) * scale;

    if ((scale == 0) || (char_width == 0) || (char_height == 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((x >= LCD_H_RES) || (y >= LCD_V_RES) || ((x + char_width) > LCD_H_RES) || ((y + char_height) > LCD_V_RES))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((size_t)char_width * char_height > s_lcd_line_buf_pixels)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *glyph = lcd_find_glyph(ch);
    uint16_t fg = lcd_color_to_panel(color);
    uint16_t bg = lcd_color_to_panel(bg_color);

    ret = lcd_wait_flush_done();
    if (ret != ESP_OK)
    {
        return ret;
    }

    for (uint16_t row = 0; row < char_height; row++)
    {
        uint16_t font_row = row / scale;

        for (uint16_t col = 0; col < char_width; col++)
        {
            uint16_t font_col = col / scale;
            bool draw_fg = false;

            if ((font_col < LCD_FONT_WIDTH) && (font_row < LCD_FONT_HEIGHT))
            {
                draw_fg = ((glyph[font_col] >> font_row) & 0x01) != 0;
            }

            s_lcd_line_buf[(row * char_width) + col] = draw_fg ? fg : bg;
        }
    }

    return esp_lcd_panel_draw_bitmap(s_lcd_panel, x, y, x + char_width, y + char_height, s_lcd_line_buf);
}

esp_err_t lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color)
{
    esp_err_t ret = lcd_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (str == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t scale = LCD_FONT_SCALE;
    const uint16_t char_width = (LCD_FONT_WIDTH + LCD_FONT_SPACE_X) * scale;
    const uint16_t char_height = (LCD_FONT_HEIGHT + LCD_FONT_SPACE_Y) * scale;
    uint16_t cursor_x = x;
    uint16_t cursor_y = y;

    while (*str != '\0')
    {
        if (*str == '\n')
        {
            cursor_x = x;
            cursor_y += char_height;
            str++;
            continue;
        }

        if ((cursor_x + char_width) > LCD_H_RES)
        {
#if LCD_TEXT_WRAP_ENABLE
            cursor_x = x;
            cursor_y += char_height;
#else
            return ESP_OK;
#endif
        }

        if ((cursor_y + char_height) > LCD_V_RES)
        {
            return ESP_OK;
        }

        ret = lcd_draw_char(cursor_x, cursor_y, *str, color, bg_color);
        if (ret != ESP_OK)
        {
            return ret;
        }

        cursor_x += char_width;
        str++;
    }

    return ESP_OK;
}

esp_err_t lcd_color_test(void)
{
    esp_err_t ret = lcd_check_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 颜色测试表：
     *     color：标准 RGB565 原始颜色值，便于直接和 ST7789/RGB565 手册核对；
     *     name：串口日志使用的颜色名称，方便观察实物屏幕时记录现象。
     *
     * 调用方法：
     *     ESP_ERROR_CHECK(lcd_init());
     *     ESP_ERROR_CHECK(lcd_color_test());
     */
    const struct
    {
        uint16_t color;
        const char *name;
    } color_test_items[] = {
        {0xF800, "RED   0xF800"},
        {0x07E0, "GREEN 0x07E0"},
        {0x001F, "BLUE  0x001F"},
        {0xFFFF, "WHITE 0xFFFF"},
        {0x0000, "BLACK 0x0000"},
    };

    ESP_LOGI(TAG, "开始 LCD 颜色测试，当前方案=%s，每个颜色停留 %d ms",
             LCD_COLOR_ACTIVE_TEST_SCHEME_NAME,
             LCD_COLOR_TEST_DELAY_MS);
    ESP_LOGI(TAG, "观察顺序应为：红 -> 绿 -> 蓝 -> 白 -> 黑");

    for (size_t i = 0; i < (sizeof(color_test_items) / sizeof(color_test_items[0])); i++)
    {
        ESP_LOGI(TAG, "LCD 颜色测试: %s，写入面板缓冲值=0x%04X",
                 color_test_items[i].name,
                 lcd_color_to_panel(color_test_items[i].color));

        ret = lcd_clear(color_test_items[i].color);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "LCD 颜色测试失败: %s", esp_err_to_name(ret));
            return ret;
        }

        vTaskDelay(pdMS_TO_TICKS(LCD_COLOR_TEST_DELAY_MS));
    }

    ESP_LOGI(TAG, "LCD 颜色测试完成");
    return ESP_OK;
}
