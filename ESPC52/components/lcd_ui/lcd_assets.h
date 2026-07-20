#pragma once

#include "lvgl.h"

/* Small I4 frames stay in flash. UI animation changes the descriptor only. */
#define LCD_ASSET_CAT_W 32U
#define LCD_ASSET_CAT_H 24U
#define LCD_ASSET_CAT_STRIDE (LCD_ASSET_CAT_W / 2U)
#define LCD_ASSET_BYTE(hi, lo) ((uint8_t)((((uint8_t)(hi)) << 4U) | (uint8_t)(lo)))
#define LCD_ASSET_PAIR(c) LCD_ASSET_BYTE(c, c)
#define LCD_ASSET_ROW16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    LCD_ASSET_PAIR(a), LCD_ASSET_PAIR(b), LCD_ASSET_PAIR(c), LCD_ASSET_PAIR(d), \
    LCD_ASSET_PAIR(e), LCD_ASSET_PAIR(f), LCD_ASSET_PAIR(g), LCD_ASSET_PAIR(h), \
    LCD_ASSET_PAIR(i), LCD_ASSET_PAIR(j), LCD_ASSET_PAIR(k), LCD_ASSET_PAIR(l), \
    LCD_ASSET_PAIR(m), LCD_ASSET_PAIR(n), LCD_ASSET_PAIR(o), LCD_ASSET_PAIR(p)
#define LCD_ASSET_REPEAT2(row) row, row,

#define LCD_CAT_CLEAR LCD_ASSET_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
#define LCD_CAT_EARS LCD_ASSET_ROW16(0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0)
#define LCD_CAT_HEAD LCD_ASSET_ROW16(0,0,1,3,1,1,1,1,1,1,1,1,3,1,0,0)
#define LCD_CAT_FACE LCD_ASSET_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0)
#define LCD_CAT_EYES LCD_ASSET_ROW16(0,1,1,1,2,2,1,1,1,1,2,2,1,1,1,0)
#define LCD_CAT_SMILE LCD_ASSET_ROW16(0,1,1,1,1,1,4,1,1,4,1,1,1,1,1,0)
#define LCD_CAT_TALK LCD_ASSET_ROW16(0,1,1,1,1,4,4,4,4,4,4,1,1,1,1,0)

static const uint8_t lcd_asset_cat_idle_map[] __attribute__((aligned(4))) = {
    0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff, 0x5a,0x3d,0x2a,0xff, 0xa6,0xd2,0x79,0xff,
    0xb5,0x9b,0xf4,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_EARS)
    LCD_ASSET_REPEAT2(LCD_CAT_HEAD)
    LCD_ASSET_REPEAT2(LCD_CAT_FACE)
    LCD_ASSET_REPEAT2(LCD_CAT_EYES)
    LCD_ASSET_REPEAT2(LCD_CAT_FACE)
    LCD_ASSET_REPEAT2(LCD_CAT_SMILE)
    LCD_ASSET_REPEAT2(LCD_CAT_FACE)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
};

static const uint8_t lcd_asset_cat_talk_map[] __attribute__((aligned(4))) = {
    0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff, 0x5a,0x3d,0x2a,0xff, 0xa6,0xd2,0x79,0xff,
    0xb5,0x9b,0xf4,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_EARS)
    LCD_ASSET_REPEAT2(LCD_CAT_HEAD)
    LCD_ASSET_REPEAT2(LCD_CAT_FACE)
    LCD_ASSET_REPEAT2(LCD_CAT_EYES)
    LCD_ASSET_REPEAT2(LCD_CAT_FACE)
    LCD_ASSET_REPEAT2(LCD_CAT_TALK)
    LCD_ASSET_REPEAT2(LCD_CAT_FACE)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT2(LCD_CAT_CLEAR)
};

static const lv_image_dsc_t lcd_asset_cat_idle = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I4, .flags = 0,
               .w = LCD_ASSET_CAT_W, .h = LCD_ASSET_CAT_H, .stride = LCD_ASSET_CAT_STRIDE},
    .data_size = sizeof(lcd_asset_cat_idle_map), .data = lcd_asset_cat_idle_map, .reserved = NULL,
};
static const lv_image_dsc_t lcd_asset_cat_talk = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I4, .flags = 0,
               .w = LCD_ASSET_CAT_W, .h = LCD_ASSET_CAT_H, .stride = LCD_ASSET_CAT_STRIDE},
    .data_size = sizeof(lcd_asset_cat_talk_map), .data = lcd_asset_cat_talk_map, .reserved = NULL,
};

#undef LCD_CAT_TALK
#undef LCD_CAT_SMILE
#undef LCD_CAT_EYES
#undef LCD_CAT_FACE
#undef LCD_CAT_HEAD
#undef LCD_CAT_EARS
#undef LCD_CAT_CLEAR
#undef LCD_ASSET_REPEAT2
#undef LCD_ASSET_ROW16
#undef LCD_ASSET_PAIR
#undef LCD_ASSET_BYTE
