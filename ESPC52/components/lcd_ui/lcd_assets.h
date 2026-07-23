#pragma once

#include "lvgl.h"

/* I4 Pixel Cat frames stay in flash. UI animation changes descriptors only. */
#define LCD_ASSET_CAT_W 64U
#define LCD_ASSET_CAT_H 48U
#define LCD_ASSET_CAT_STRIDE (LCD_ASSET_CAT_W / 2U)
#define LCD_ASSET_VOICE_CAT_W 128U
#define LCD_ASSET_VOICE_CAT_H 96U
#define LCD_ASSET_VOICE_CAT_STRIDE (LCD_ASSET_VOICE_CAT_W / 2U)
#define LCD_ASSET_BOOT_CAT_W 96U
#define LCD_ASSET_BOOT_CAT_H 72U
#define LCD_ASSET_BOOT_CAT_STRIDE (LCD_ASSET_BOOT_CAT_W / 2U)
#define LCD_ASSET_BYTE(hi, lo) ((uint8_t)((((uint8_t)(hi)) << 4U) | (uint8_t)(lo)))
#define LCD_ASSET_PAIR(c) LCD_ASSET_BYTE(c, c)
#define LCD_ASSET_BLOCK(c) LCD_ASSET_PAIR(c), LCD_ASSET_PAIR(c),
#define LCD_ASSET_ROW16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    LCD_ASSET_BLOCK(a) LCD_ASSET_BLOCK(b) LCD_ASSET_BLOCK(c) LCD_ASSET_BLOCK(d) \
    LCD_ASSET_BLOCK(e) LCD_ASSET_BLOCK(f) LCD_ASSET_BLOCK(g) LCD_ASSET_BLOCK(h) \
    LCD_ASSET_BLOCK(i) LCD_ASSET_BLOCK(j) LCD_ASSET_BLOCK(k) LCD_ASSET_BLOCK(l) \
    LCD_ASSET_BLOCK(m) LCD_ASSET_BLOCK(n) LCD_ASSET_BLOCK(o) LCD_ASSET_BLOCK(p)
#define LCD_ASSET_REPEAT4(row) row row row row
#define LCD_ASSET_VOICE_BLOCK(c) LCD_ASSET_PAIR(c), LCD_ASSET_PAIR(c), LCD_ASSET_PAIR(c), LCD_ASSET_PAIR(c),
#define LCD_ASSET_VOICE_ROW16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    LCD_ASSET_VOICE_BLOCK(a) LCD_ASSET_VOICE_BLOCK(b) LCD_ASSET_VOICE_BLOCK(c) LCD_ASSET_VOICE_BLOCK(d) \
    LCD_ASSET_VOICE_BLOCK(e) LCD_ASSET_VOICE_BLOCK(f) LCD_ASSET_VOICE_BLOCK(g) LCD_ASSET_VOICE_BLOCK(h) \
    LCD_ASSET_VOICE_BLOCK(i) LCD_ASSET_VOICE_BLOCK(j) LCD_ASSET_VOICE_BLOCK(k) LCD_ASSET_VOICE_BLOCK(l) \
    LCD_ASSET_VOICE_BLOCK(m) LCD_ASSET_VOICE_BLOCK(n) LCD_ASSET_VOICE_BLOCK(o) LCD_ASSET_VOICE_BLOCK(p)
#define LCD_ASSET_REPEAT8(row) row row row row row row row row
#define LCD_ASSET_BOOT_BLOCK(c) LCD_ASSET_PAIR(c), LCD_ASSET_PAIR(c), LCD_ASSET_PAIR(c),
#define LCD_ASSET_BOOT_ROW16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    LCD_ASSET_BOOT_BLOCK(a) LCD_ASSET_BOOT_BLOCK(b) LCD_ASSET_BOOT_BLOCK(c) LCD_ASSET_BOOT_BLOCK(d) \
    LCD_ASSET_BOOT_BLOCK(e) LCD_ASSET_BOOT_BLOCK(f) LCD_ASSET_BOOT_BLOCK(g) LCD_ASSET_BOOT_BLOCK(h) \
    LCD_ASSET_BOOT_BLOCK(i) LCD_ASSET_BOOT_BLOCK(j) LCD_ASSET_BOOT_BLOCK(k) LCD_ASSET_BOOT_BLOCK(l) \
    LCD_ASSET_BOOT_BLOCK(m) LCD_ASSET_BOOT_BLOCK(n) LCD_ASSET_BOOT_BLOCK(o) LCD_ASSET_BOOT_BLOCK(p)
#define LCD_ASSET_REPEAT6(row) row row row row row row

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
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_EARS)
    LCD_ASSET_REPEAT4(LCD_CAT_HEAD)
    LCD_ASSET_REPEAT4(LCD_CAT_FACE)
    LCD_ASSET_REPEAT4(LCD_CAT_EYES)
    LCD_ASSET_REPEAT4(LCD_CAT_FACE)
    LCD_ASSET_REPEAT4(LCD_CAT_SMILE)
    LCD_ASSET_REPEAT4(LCD_CAT_FACE)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
};

static const uint8_t lcd_asset_cat_talk_map[] __attribute__((aligned(4))) = {
    0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff, 0x5a,0x3d,0x2a,0xff, 0xa6,0xd2,0x79,0xff,
    0xb5,0x9b,0xf4,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_EARS)
    LCD_ASSET_REPEAT4(LCD_CAT_HEAD)
    LCD_ASSET_REPEAT4(LCD_CAT_FACE)
    LCD_ASSET_REPEAT4(LCD_CAT_EYES)
    LCD_ASSET_REPEAT4(LCD_CAT_FACE)
    LCD_ASSET_REPEAT4(LCD_CAT_TALK)
    LCD_ASSET_REPEAT4(LCD_CAT_FACE)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
    LCD_ASSET_REPEAT4(LCD_CAT_CLEAR)
};

static const uint8_t lcd_asset_cat_idle_voice_map[] __attribute__((aligned(4))) = {
    0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff, 0x5a,0x3d,0x2a,0xff, 0xa6,0xd2,0x79,0xff,
    0xb5,0x9b,0xf4,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,1,3,1,1,1,1,1,1,1,1,3,1,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,2,2,1,1,1,1,2,2,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,1,4,1,1,4,1,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
};

static const uint8_t lcd_asset_cat_talk_voice_map[] __attribute__((aligned(4))) = {
    0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff, 0x5a,0x3d,0x2a,0xff, 0xa6,0xd2,0x79,0xff,
    0xb5,0x9b,0xf4,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,1,3,1,1,1,1,1,1,1,1,3,1,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,2,2,1,1,1,1,2,2,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,4,4,4,4,4,4,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT8(LCD_ASSET_VOICE_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
};

static const uint8_t lcd_asset_cat_boot_map[] __attribute__((aligned(4))) = {
    0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff, 0x5a,0x3d,0x2a,0xff, 0xa6,0xd2,0x79,0xff,
    0xb5,0x9b,0xf4,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,0,1,3,1,1,1,1,1,1,1,1,3,1,0,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,1,1,1,2,2,1,1,1,1,2,2,1,1,1,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,1,1,1,1,1,4,1,1,4,1,1,1,1,1,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
    LCD_ASSET_REPEAT6(LCD_ASSET_BOOT_ROW16(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
};

_Static_assert(sizeof(lcd_asset_cat_idle_map) == 64U + LCD_ASSET_CAT_STRIDE * LCD_ASSET_CAT_H,
               "dashboard cat image must match its I4 descriptor");
_Static_assert(sizeof(lcd_asset_cat_talk_map) == 64U + LCD_ASSET_CAT_STRIDE * LCD_ASSET_CAT_H,
               "dashboard cat image must match its I4 descriptor");
_Static_assert(sizeof(lcd_asset_cat_idle_voice_map) == 64U + LCD_ASSET_VOICE_CAT_STRIDE * LCD_ASSET_VOICE_CAT_H,
               "voice cat image must match its I4 descriptor");
_Static_assert(sizeof(lcd_asset_cat_talk_voice_map) == 64U + LCD_ASSET_VOICE_CAT_STRIDE * LCD_ASSET_VOICE_CAT_H,
               "voice cat image must match its I4 descriptor");
_Static_assert(sizeof(lcd_asset_cat_boot_map) == 64U + LCD_ASSET_BOOT_CAT_STRIDE * LCD_ASSET_BOOT_CAT_H,
               "boot cat image must match its I4 descriptor");

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
static const lv_image_dsc_t lcd_asset_cat_idle_voice = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I4, .flags = 0,
               .w = LCD_ASSET_VOICE_CAT_W, .h = LCD_ASSET_VOICE_CAT_H, .stride = LCD_ASSET_VOICE_CAT_STRIDE},
    .data_size = sizeof(lcd_asset_cat_idle_voice_map), .data = lcd_asset_cat_idle_voice_map, .reserved = NULL,
};
static const lv_image_dsc_t lcd_asset_cat_talk_voice = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I4, .flags = 0,
               .w = LCD_ASSET_VOICE_CAT_W, .h = LCD_ASSET_VOICE_CAT_H, .stride = LCD_ASSET_VOICE_CAT_STRIDE},
    .data_size = sizeof(lcd_asset_cat_talk_voice_map), .data = lcd_asset_cat_talk_voice_map, .reserved = NULL,
};
static const lv_image_dsc_t lcd_asset_cat_boot = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I4, .flags = 0,
               .w = LCD_ASSET_BOOT_CAT_W, .h = LCD_ASSET_BOOT_CAT_H, .stride = LCD_ASSET_BOOT_CAT_STRIDE},
    .data_size = sizeof(lcd_asset_cat_boot_map), .data = lcd_asset_cat_boot_map, .reserved = NULL,
};

#undef LCD_CAT_TALK
#undef LCD_CAT_SMILE
#undef LCD_CAT_EYES
#undef LCD_CAT_FACE
#undef LCD_CAT_HEAD
#undef LCD_CAT_EARS
#undef LCD_CAT_CLEAR
#undef LCD_ASSET_REPEAT8
#undef LCD_ASSET_VOICE_ROW16
#undef LCD_ASSET_VOICE_BLOCK
#undef LCD_ASSET_REPEAT6
#undef LCD_ASSET_BOOT_ROW16
#undef LCD_ASSET_BOOT_BLOCK
#undef LCD_ASSET_REPEAT4
#undef LCD_ASSET_ROW16
#undef LCD_ASSET_BLOCK
#undef LCD_ASSET_PAIR
#undef LCD_ASSET_BYTE
