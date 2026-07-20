#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_types.h"

/* Board values are defaults, not runtime guesses. Product overrides may define
 * these symbols from its board profile without changing the LCD component. */
#ifndef LCD_BOARD_SPI_HOST
#define LCD_BOARD_SPI_HOST SPI2_HOST
#endif
#ifndef LCD_BOARD_SCLK
#define LCD_BOARD_SCLK GPIO_NUM_24
#endif
#ifndef LCD_BOARD_MOSI
#define LCD_BOARD_MOSI GPIO_NUM_23
#endif
#ifndef LCD_BOARD_MISO
#define LCD_BOARD_MISO GPIO_NUM_NC
#endif
#ifndef LCD_BOARD_CS
#define LCD_BOARD_CS GPIO_NUM_25
#endif
#ifndef LCD_BOARD_DC
#define LCD_BOARD_DC GPIO_NUM_26
#endif
#ifndef LCD_BOARD_RST
#define LCD_BOARD_RST GPIO_NUM_NC
#endif
#ifndef LCD_BOARD_BL
#define LCD_BOARD_BL GPIO_NUM_NC
#endif
#ifndef LCD_BOARD_BL_ON_LEVEL
#define LCD_BOARD_BL_ON_LEVEL 1
#endif
#ifndef LCD_BOARD_HRES
#define LCD_BOARD_HRES 240U
#endif
#ifndef LCD_BOARD_VRES
#define LCD_BOARD_VRES 284U
#endif
#ifndef LCD_BOARD_X_GAP
#define LCD_BOARD_X_GAP 0
#endif
#ifndef LCD_BOARD_Y_GAP
#define LCD_BOARD_Y_GAP 0
#endif
#ifndef LCD_BOARD_SWAP_XY
#define LCD_BOARD_SWAP_XY 0
#endif
#ifndef LCD_BOARD_MIRROR_X
#define LCD_BOARD_MIRROR_X 0
#endif
#ifndef LCD_BOARD_MIRROR_Y
#define LCD_BOARD_MIRROR_Y 0
#endif
#ifndef LCD_BOARD_INVERT
#define LCD_BOARD_INVERT 1
#endif
#ifndef LCD_BOARD_RGB_ORDER
#define LCD_BOARD_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#endif
#ifndef LCD_BOARD_PIXEL_CLOCK_HZ
#define LCD_BOARD_PIXEL_CLOCK_HZ (10U * 1000U * 1000U)
#endif
#ifndef LCD_BOARD_IO_QUEUE_DEPTH
#define LCD_BOARD_IO_QUEUE_DEPTH 4U
#endif

#define LCD_LEGACY_DMA_BYTES 9600U
#define LCD_LVGL_DRAW_LINES 10U
#define LCD_LVGL_DRAW_BYTES (LCD_BOARD_HRES * LCD_LVGL_DRAW_LINES * sizeof(uint16_t))
