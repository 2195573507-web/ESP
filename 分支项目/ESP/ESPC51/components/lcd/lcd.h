#ifndef __LCD_H
#define __LCD_H

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_dev.h"

/* ============================== LCD 可调参数区 ==============================
 *
 * 说明：
 *     1. 本文件集中保存 LCD 后续调试可能会改动的全部参数。
 *     2. 如果更换 LCD 屏幕、修改飞线 GPIO、调整屏幕方向或颜色顺序，优先修改本头文件宏定义。
 *     3. LCD 模块只依赖 ESP-IDF 的 SPI/GPIO/esp_lcd 驱动，不依赖 ENV、BME690、IIC、WiFi 等业务模块。
 */

/* LCD_SPI_HOST：LCD 使用的 SPI 主机控制器。
 * 功能：
 *     指定 LCD 挂载到 ESP32-C5 的哪个 SPI 控制器。
 * 调用方法：
 *     lcd_init();                    // 内部会使用 LCD_SPI_HOST 初始化 SPI 总线
 * 后续调试：
 *     如果 LCD 改接到其它 SPI 控制器，修改该宏即可。
 */
#define LCD_SPI_HOST                         SPI2_HOST

/* LCD_SPI_DMA_CH：SPI DMA 通道选择。
 * 功能：
 *     SPI 刷屏数据量较大，启用 DMA 可以降低 CPU 拷贝压力。
 * 取值：
 *     SPI_DMA_CH_AUTO：由 ESP-IDF 自动选择 DMA 通道；
 *     SPI_DMA_DISABLED：关闭 DMA，只建议低速调试时使用。
 */
#define LCD_SPI_DMA_CH                       SPI_DMA_CH_AUTO

/* LCD_PIN_NUM_SCLK：LCD SPI 时钟线 GPIO。
 * 功能：
 *     连接原理图中的 LCD_SCL 网络，也就是 ST7789P3 四线 SPI 的时钟线。
 * 原理图核对：
 *     ESP-SensairShuttle-MainBoard V1.0 原理图标注 LCD_SCL 连接 ESP32-C5 GPIO24。
 * 后续调试：
 *     如果后续硬件改版或飞线改变，只需要修改该 GPIO。
 */
#define LCD_PIN_NUM_SCLK                     GPIO_NUM_24

/* LCD_PIN_NUM_MOSI：LCD SPI 数据输出线 GPIO。
 * 功能：
 *     连接原理图中的 LCD_SDA 网络，也就是 ST7789P3 四线 SPI 的 MOSI 数据线。
 * 原理图核对：
 *     ESP-SensairShuttle-MainBoard V1.0 原理图标注 LCD_SDA 连接 ESP32-C5 GPIO23。
 */
#define LCD_PIN_NUM_MOSI                     GPIO_NUM_23

/* LCD_PIN_NUM_MISO：LCD SPI 数据输入线 MISO GPIO。
 * 功能：
 *     当前 LCD 模块只做写屏，不读取 LCD 数据，因此默认不连接。
 * 后续调试：
 *     如果后续需要读 LCD ID 或状态，可改为实际连接的 GPIO。
 */
#define LCD_PIN_NUM_MISO                     GPIO_NUM_NC

/* LCD_PIN_NUM_DC：LCD 数据/命令选择线 DC GPIO。
 * 功能：
 *     DC=0 通常表示发送命令，DC=1 通常表示发送数据，由 esp_lcd SPI IO 驱动自动控制。
 * 原理图核对：
 *     ESP-SensairShuttle-MainBoard V1.0 原理图标注 LCD_DC 连接 ESP32-C5 GPIO26。
 */
#define LCD_PIN_NUM_DC                       GPIO_NUM_26

/* LCD_PIN_NUM_CS：LCD 片选线 CS GPIO。
 * 功能：
 *     当 SPI 总线上只有 LCD 一个设备时也建议连接 CS，便于后续扩展其它 SPI 设备。
 * 原理图核对：
 *     ESP-SensairShuttle-MainBoard V1.0 原理图标注 LCD_CS 连接 ESP32-C5 GPIO25。
 */
#define LCD_PIN_NUM_CS                       GPIO_NUM_25

/* LCD_PIN_NUM_RST：LCD 复位线 RST GPIO。
 * 功能：
 *     原理图中的 LCD_RST 通过 R35(10k) 上拉到 LCD_3V3，没有连接到 ESP32-C5 GPIO。
 *     因此这里配置为 GPIO_NUM_NC，让 esp_lcd 使用软件复位命令完成复位流程。
 * 取值：
 *     如果后续硬件改版把 LCD_RST 接到 MCU，再把本宏改为实际 GPIO。
 */
#define LCD_PIN_NUM_RST                      GPIO_NUM_NC

/* LCD_PIN_NUM_BK_LIGHT：LCD 背光控制 GPIO。
 * 功能：
 *     当前原理图中的 LCD 背光未连接 ESP32-C5 MCU GPIO，因此这里配置为 GPIO_NUM_NC。
 *     lcd_init() 会调用 lcd_config_backlight() 和 lcd_set_backlight()，当本宏为 GPIO_NUM_NC 时这两个函数会直接跳过背光 GPIO 操作。
 * 原理图核对：
 *     未发现 LCD_BL 连接到 ESP32-C5 GPIO，禁止用 PWR_CTRL 或其它电源控制脚替代背光脚。
 * 取值：
 *     如果后续硬件改版增加独立 LCD_BL 网络，请把本宏改为原理图标注的实际 GPIO。
 */
#define LCD_PIN_NUM_BK_LIGHT                 GPIO_NUM_NC

/* LCD_BK_LIGHT_ON_LEVEL：背光打开时 GPIO 输出电平。
 * 功能：
 *     大多数 LCD 背光为高电平点亮，如果你的模块低电平点亮，改为 0。
 */
#define LCD_BK_LIGHT_ON_LEVEL                1

/* LCD_BK_LIGHT_OFF_LEVEL：背光关闭时 GPIO 输出电平。
 * 功能：
 *     与 LCD_BK_LIGHT_ON_LEVEL 相反，用于 lcd_init() 早期先关闭背光，避免初始化过程闪屏。
 */
#define LCD_BK_LIGHT_OFF_LEVEL               (!LCD_BK_LIGHT_ON_LEVEL)

/* LCD_H_RES：LCD 可显示区域横向像素数。
 * 功能：
 *     lcd_clear()、lcd_fill_rect()、lcd_draw_string() 会根据该宽度做边界检查。
 * 后续调试：
 *     本项目当前 LCD 可视区域按原理图/屏幕参数配置为 240x284，如更换屏幕请同步修改本宏。
 *     常见 ST7789 小屏可能是 240x240、240x280、240x320 或 172x320，请按实际屏幕修改。
 */
#define LCD_H_RES                            240

/* LCD_V_RES：LCD 可显示区域纵向像素数。
 * 功能：
 *     LCD 模块所有绘图 API 的 y 坐标范围为 0 ~ LCD_V_RES-1。
 * 后续调试：
 *     本项目当前 LCD 可视区域按原理图/屏幕参数配置为 240x284，如更换屏幕请同步修改本宏。
 */
#define LCD_V_RES                            284

/* LCD_PIXEL_CLOCK_HZ：LCD SPI 像素时钟频率，单位 Hz。
 * 功能：
 *     控制 SPI 写屏速度。频率越高刷新越快，但飞线较长或屏幕质量较差时可能花屏。
 * 调试建议：
 *     点亮阶段建议 10MHz~20MHz；稳定后可逐步提高。
 */
#define LCD_PIXEL_CLOCK_HZ                   (20 * 1000 * 1000)

/* LCD_SPI_MODE：LCD SPI 模式。
 * 功能：
 *     大多数 4 线 SPI LCD 使用 mode 0。如果屏幕无响应且接线确认正确，可查屏幕手册后调整。
 */
#define LCD_SPI_MODE                         0

/* LCD_CMD_BITS：LCD 命令位宽。
 * 功能：
 *     ST7789 常用 8bit 命令，通常不需要修改。
 */
#define LCD_CMD_BITS                         8

/* LCD_PARAM_BITS：LCD 参数位宽。
 * 功能：
 *     ST7789 常用 8bit 参数，通常不需要修改。
 */
#define LCD_PARAM_BITS                       8

/* LCD_TRANS_QUEUE_DEPTH：LCD SPI 内部事务队列深度。
 * 功能：
 *     esp_lcd SPI IO 使用该队列缓存待发送事务。当前 BSP 同步调用为主，默认 10 足够。
 */
#define LCD_TRANS_QUEUE_DEPTH                10

/* LCD_DRAW_BUFFER_LINES：LCD 填充矩形/清屏时一次刷新的行数。
 * 功能：
 *     lcd_clear() 不申请整屏缓存，而是用若干行缓存分块刷新，降低内存占用。
 * 调试建议：
 *     数值越大清屏越快但占用 RAM 越多。默认 10 行约占 LCD_H_RES*10*2 字节。
 */
#define LCD_DRAW_BUFFER_LINES                10

/* LCD_MAX_TRANSFER_SIZE：SPI 单次 DMA 传输的最大字节数。
 * 功能：
 *     spi_bus_initialize() 使用该值配置 LCD 总线最大传输块。
 */
#define LCD_MAX_TRANSFER_SIZE                (LCD_H_RES * LCD_DRAW_BUFFER_LINES * sizeof(uint16_t))

/* LCD_COLOR_TEST_SCHEME_*：LCD 颜色格式测试方案编号。
 * 功能：
 *     用一个宏在三组颜色格式配置之间切换，避免调试时同时改多处参数。
 * 调用方法：
 *     1. 修改 LCD_COLOR_ACTIVE_TEST_SCHEME 为 A/B/C 其中一个；
 *     2. 重新编译、烧录；
 *     3. 调用 lcd_color_test(); 观察红、绿、蓝、白、黑是否正常。
 *
 * 方案A：
 *     LCD_RGB_ELEMENT_ORDER_RGB
 *     LCD_RGB_DATA_ENDIAN_BIG
 *     LCD_COLOR_SWAP_BYTES=0
 *
 * 方案B：
 *     LCD_RGB_ELEMENT_ORDER_RGB
 *     LCD_RGB_DATA_ENDIAN_BIG
 *     LCD_COLOR_SWAP_BYTES=1
 *
 * 方案C：
 *     LCD_RGB_ELEMENT_ORDER_BGR
 *     LCD_RGB_DATA_ENDIAN_BIG
 *     LCD_COLOR_SWAP_BYTES=0
 */
#define LCD_COLOR_TEST_SCHEME_A              1
#define LCD_COLOR_TEST_SCHEME_B              2
#define LCD_COLOR_TEST_SCHEME_C              3

/* LCD_COLOR_ACTIVE_TEST_SCHEME：当前启用的 LCD 颜色测试方案。
 * 功能：
 *     统一选择 LCD_RGB_ELEMENT_ORDER、LCD_RGB_DATA_ENDIAN、LCD_COLOR_SWAP_BYTES。
 * 当前推荐：
 *     先使用方案B。ESP32-C5 是小端 CPU，ST7789 常用大端 RGB565，
 *     因此 uint16_t 颜色写入 DMA 缓冲区前通常需要交换高低字节。
 * 后续调试：
 *     如果红蓝互换，先观察 LCD_COLOR_INVERT_ENABLE 是否正确，再切换到方案C 对比。
 */
#ifndef LCD_COLOR_ACTIVE_TEST_SCHEME
#define LCD_COLOR_ACTIVE_TEST_SCHEME         LCD_COLOR_TEST_SCHEME_B
#endif

#if LCD_COLOR_ACTIVE_TEST_SCHEME == LCD_COLOR_TEST_SCHEME_A
#define LCD_COLOR_ACTIVE_TEST_SCHEME_NAME    "方案A: RGB/BIG/SWAP=0"
#define LCD_RGB_ELEMENT_ORDER                LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_RGB_DATA_ENDIAN                  LCD_RGB_DATA_ENDIAN_BIG
#define LCD_COLOR_SWAP_BYTES                 0
#elif LCD_COLOR_ACTIVE_TEST_SCHEME == LCD_COLOR_TEST_SCHEME_B
#define LCD_COLOR_ACTIVE_TEST_SCHEME_NAME    "方案B: RGB/BIG/SWAP=1"
#define LCD_RGB_ELEMENT_ORDER                LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_RGB_DATA_ENDIAN                  LCD_RGB_DATA_ENDIAN_BIG
#define LCD_COLOR_SWAP_BYTES                 1
#elif LCD_COLOR_ACTIVE_TEST_SCHEME == LCD_COLOR_TEST_SCHEME_C
#define LCD_COLOR_ACTIVE_TEST_SCHEME_NAME    "方案C: BGR/BIG/SWAP=0"
#define LCD_RGB_ELEMENT_ORDER                LCD_RGB_ELEMENT_ORDER_BGR
#define LCD_RGB_DATA_ENDIAN                  LCD_RGB_DATA_ENDIAN_BIG
#define LCD_COLOR_SWAP_BYTES                 0
#else
#error "LCD_COLOR_ACTIVE_TEST_SCHEME 只能选择 LCD_COLOR_TEST_SCHEME_A/B/C"
#endif

/* LCD_COLOR_INVERT_ENABLE：是否开启 LCD 反色显示。
 * 功能：
 *     控制 esp_lcd_panel_invert_color() 的参数，用于匹配 ST7789 屏幕玻璃的极性。
 *     如果白色显示为黑色、绿色显示为紫色、蓝色显示为黄色，通常说明反色极性不匹配。
 * 当前推荐：
 *     本屏当前现象包含 WHITE -> BLACK，优先设置为 1 进行验证。
 * 调用方法：
 *     lcd_init();        // 初始化时会读取该宏并打印日志
 *     lcd_color_test();  // 观察白色和黑色是否已经恢复正常
 */
#define LCD_COLOR_INVERT_ENABLE              1

/* LCD_COLOR_TEST_DELAY_MS：lcd_color_test() 每个测试颜色停留时间。
 * 功能：
 *     控制红、绿、蓝、白、黑五个纯色画面之间的等待时间，单位 ms。
 * 调用方法：
 *     lcd_color_test();  // 每个颜色停留 LCD_COLOR_TEST_DELAY_MS 毫秒
 */
#define LCD_COLOR_TEST_DELAY_MS              3000

/* LCD_MIRROR_X_ENABLE / LCD_MIRROR_Y_ENABLE：屏幕 X/Y 方向镜像。
 * 功能：
 *     用于调整显示方向。文字上下或左右颠倒时修改这两个宏。
 */
#define LCD_MIRROR_X_ENABLE                  0
#define LCD_MIRROR_Y_ENABLE                  0

/* LCD_SWAP_XY_ENABLE：是否交换 X/Y 轴。
 * 功能：
 *     与 LCD_MIRROR_X_ENABLE、LCD_MIRROR_Y_ENABLE 配合使用，可实现横屏/竖屏方向调整。
 */
#define LCD_SWAP_XY_ENABLE                   0

/* LCD_X_GAP / LCD_Y_GAP：显示区域相对 LCD 内部显存的偏移。
 * 功能：
 *     同为 ST7789 的不同屏幕玻璃可视区域不同，可能需要设置 x/y 偏移才能居中显示。
 * 调试现象：
 *     如果图像整体偏移、缺边或有黑边，可调整这两个宏。
 */
#define LCD_X_GAP                            0
#define LCD_Y_GAP                            0

/* LCD_FONT_WIDTH / LCD_FONT_HEIGHT：内置 ASCII 字体点阵尺寸。
 * 功能：
 *     lcd_draw_char() 使用 5x7 点阵字符，并额外保留行/列间距。
 */
#define LCD_FONT_WIDTH                       5
#define LCD_FONT_HEIGHT                      7

/* LCD_FONT_SPACE_X：字符之间的横向空白列数。
 * 功能：
 *     lcd_draw_string() 每显示一个字符后会额外留出该宽度，避免字符粘连。
 */
#define LCD_FONT_SPACE_X                     1

/* LCD_FONT_SPACE_Y：字符串换行时的额外空白行数。
 * 功能：
 *     lcd_draw_string() 处理 '\n' 或自动换行时使用该值。
 */
#define LCD_FONT_SPACE_Y                     1

/* LCD_FONT_SCALE：字体放大倍数。
 * 功能：
 *     1 表示 5x7 原始点阵；2 表示每个字体像素放大为 2x2，适合小屏显示。
 */
#define LCD_FONT_SCALE                       2

/* LCD_TEXT_WRAP_ENABLE：字符串超出右边界时是否自动换行。
 * 功能：
 *     1：自动换到下一行；0：超出屏幕右侧后停止绘制。
 */
#define LCD_TEXT_WRAP_ENABLE                 1

/* ============================== RGB565 常用颜色 ============================== */

/* LCD_COLOR_*：RGB565 颜色值。
 * 调用方法：
 *     lcd_clear(LCD_COLOR_RED);
 *     lcd_draw_string(20, 40, "ESP32-C5", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
 */
#define LCD_COLOR_BLACK                      0x0000
#define LCD_COLOR_WHITE                      0xFFFF
#define LCD_COLOR_RED                        0xF800
#define LCD_COLOR_GREEN                      0x07E0
#define LCD_COLOR_BLUE                       0x001F
#define LCD_COLOR_YELLOW                     0xFFE0
#define LCD_COLOR_CYAN                       0x07FF
#define LCD_COLOR_MAGENTA                    0xF81F

/* RED/GREEN/BLUE/WHITE/BLACK：点亮测试阶段的简写颜色宏。
 * 功能：
 *     便于按任务说明直接调用 lcd_clear(RED)、lcd_clear(GREEN)、lcd_clear(BLUE)、lcd_clear(WHITE)。
 * 注意：
 *     如果其它库已经定义了同名宏，这里不会重复定义，避免宏重定义警告。
 */
#ifndef RED
#define RED                                  LCD_COLOR_RED
#endif

#ifndef GREEN
#define GREEN                                LCD_COLOR_GREEN
#endif

#ifndef BLUE
#define BLUE                                 LCD_COLOR_BLUE
#endif

#ifndef WHITE
#define WHITE                                LCD_COLOR_WHITE
#endif

#ifndef BLACK
#define BLACK                                LCD_COLOR_BLACK
#endif

/* ============================== 对外函数声明 ============================== */

/* lcd_init：初始化 LCD 屏幕。
 *
 * 功能：
 *     1. 初始化背光 GPIO；
 *     2. 初始化 SPI 总线；
 *     3. 创建 esp_lcd SPI IO 和 ST7789 面板对象；
 *     4. 复位并初始化 LCD；
 *     5. 按宏配置反色、镜像、坐标偏移，并打开显示和背光。
 *
 * 返回：
 *     ESP_OK 表示初始化成功，其它值表示失败。
 *
 * 调用方法：
 *     esp_err_t ret = lcd_init();
 *     if (ret != ESP_OK) {
 *         // 根据 ret 做错误处理
 *     }
 */
esp_err_t lcd_init(void);

/* The LVGL service is the single display owner after it starts. */
esp_lcd_panel_io_handle_t lcd_get_io_handle(void);
esp_lcd_panel_handle_t lcd_get_panel_handle(void);
esp_err_t lcd_set_display_enabled(bool enabled);

/* Release the legacy DMA buffer before LVGL allocates its partial draw buffer. */
esp_err_t lcd_release_legacy_draw_buffer(void);

/* lcd_clear：使用指定颜色清空整屏。
 *
 * 参数：
 *     color：RGB565 颜色，例如 RED、GREEN、BLUE、WHITE 或 LCD_COLOR_BLACK。
 *
 * 返回：
 *     ESP_OK 表示清屏成功，其它值表示 LCD 未初始化或 SPI 发送失败。
 *
 * 调用方法：
 *     lcd_clear(RED);
 *     lcd_clear(GREEN);
 *     lcd_clear(BLUE);
 *     lcd_clear(WHITE);
 */
esp_err_t lcd_clear(uint16_t color);

/* lcd_draw_pixel：绘制单个像素点。
 *
 * 参数：
 *     x：像素横坐标，范围 0 ~ LCD_H_RES-1；
 *     y：像素纵坐标，范围 0 ~ LCD_V_RES-1；
 *     color：RGB565 颜色。
 *
 * 调用方法：
 *     lcd_draw_pixel(10, 20, LCD_COLOR_RED);
 */
esp_err_t lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/* lcd_fill_rect：填充矩形区域。
 *
 * 参数：
 *     x：矩形左上角横坐标；
 *     y：矩形左上角纵坐标；
 *     width：矩形宽度，单位像素；
 *     height：矩形高度，单位像素；
 *     color：RGB565 填充颜色。
 *
 * 调用方法：
 *     lcd_fill_rect(0, 0, 40, 40, LCD_COLOR_BLUE);
 */
esp_err_t lcd_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color);

esp_err_t lcd_draw_bitmap(uint16_t x,
                          uint16_t y,
                          uint16_t width,
                          uint16_t height,
                          const uint16_t *pixels);

/* lcd_draw_char：绘制单个 ASCII 字符。
 *
 * 参数：
 *     x：字符左上角横坐标；
 *     y：字符左上角纵坐标；
 *     ch：待显示字符。当前内置字库覆盖数字、大小写英文字母、空格、'-' 等常用字符；
 *     color：字符前景色；
 *     bg_color：字符背景色。
 *
 * 调用方法：
 *     lcd_draw_char(20, 20, 'A', LCD_COLOR_WHITE, LCD_COLOR_BLACK);
 */
esp_err_t lcd_draw_char(uint16_t x, uint16_t y, char ch, uint16_t color, uint16_t bg_color);

/* lcd_draw_string：绘制 ASCII 字符串。
 *
 * 参数：
 *     x：字符串起始横坐标；
 *     y：字符串起始纵坐标；
 *     str：以 '\0' 结尾的 C 字符串，可包含 '\n' 换行；
 *     color：字符前景色；
 *     bg_color：字符背景色。
 *
 * 调用方法：
 *     lcd_draw_string(20, 40, "ESP32-C5", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
 *     lcd_draw_string(20, 70, "SensAir Shuttle", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
 */
esp_err_t lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color);

/* lcd_color_test：LCD RGB565 颜色格式测试函数。
 *
 * 功能：
 *     依次整屏显示 0xF800、0x07E0、0x001F、0xFFFF、0x0000，
 *     分别对应标准 RGB565 的红、绿、蓝、白、黑，每个颜色停留 3 秒。
 *
 * 调用方法：
 *     ESP_ERROR_CHECK(lcd_init());
 *     ESP_ERROR_CHECK(lcd_color_test());
 *
 * 调试方法：
 *     如果五个画面依次显示为红、绿、蓝、白、黑，说明当前颜色格式配置正确。
 *     如果白色显示为黑色，优先切换 LCD_COLOR_INVERT_ENABLE。
 *     如果红色和蓝色互换，优先切换 LCD_COLOR_ACTIVE_TEST_SCHEME 到方案C对比。
 *     如果颜色发灰、发紫或红绿蓝都不对，优先在方案A和方案B之间对比字节交换。
 */
esp_err_t lcd_color_test(void);

#endif
