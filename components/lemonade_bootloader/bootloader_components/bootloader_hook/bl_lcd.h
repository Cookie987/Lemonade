#pragma once

#include <stdint.h>

/**
 * @brief 初始化 ST7789V LCD
 */
void bootloader_st7789v_init(void);

/**
 * @brief 填充矩形区域
 */
void bootloader_lcd_fill_rect(
    uint16_t x,
    uint16_t y,
    uint16_t w,
    uint16_t h,
    uint16_t color_rgb565
);

/**
 * @brief 显示 8x16 字符
 */
void lcd_show_char_8x16(
    uint16_t x,
    uint16_t y,
    char ch,
    uint16_t fc,
    uint16_t bc
);

/**
 * @brief 显示字符串
 */
void lcd_show_string(
    uint16_t x,
    uint16_t y,
    const char *str,
    uint16_t fc,
    uint16_t bc
);