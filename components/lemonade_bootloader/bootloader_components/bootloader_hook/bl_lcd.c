#include <stdint.h>

#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "esp_rom_sys.h"

#include "bl_lcd.h"              // 必须在引脚宏定义之前/后正确配合
#include "font8x16.h"

// ================= LCD GPIO 引脚定义 =================
#define LCD_CS_PIN       GPIO_NUM_6
#define LCD_RST_PIN      GPIO_NUM_7
#define LCD_DC_PIN       GPIO_NUM_15
#define LCD_SCL_PIN      GPIO_NUM_18   // SCLK
#define LCD_SDA_PIN      GPIO_NUM_17   // MOSI

// ================= LCD GPIO 快速控制宏 =================
#define LCD_CS(x)        gpio_ll_set_level(&GPIO, LCD_CS_PIN, x)
#define LCD_DC(x)        gpio_ll_set_level(&GPIO, LCD_DC_PIN, x)
#define LCD_RST(x)       gpio_ll_set_level(&GPIO, LCD_RST_PIN, x)
#define LCD_SCL(x)       gpio_ll_set_level(&GPIO, LCD_SCL_PIN, x)
#define LCD_SDA(x)       gpio_ll_set_level(&GPIO, LCD_SDA_PIN, x)

// 模拟 SPI 极简发送单字节
static inline void bootloader_lcd_spi_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        LCD_SCL(0);
        LCD_SDA((data & 0x80) ? 1 : 0);
        LCD_SCL(1);
        data <<= 1;
    }
}

// 发送命令
static void lcd_write_cmd(uint8_t cmd) {
    LCD_CS(0);
    LCD_DC(0);
    bootloader_lcd_spi_write_byte(cmd);
    LCD_CS(1);
}

// 发送数据
static void lcd_write_data(uint8_t data) {
    LCD_CS(0);
    LCD_DC(1);
    bootloader_lcd_spi_write_byte(data);
    LCD_CS(1);
}

static void lcd_write_cmd_bytes(uint8_t cmd, const uint8_t *data, uint8_t len) {
    lcd_write_cmd(cmd);
    for (uint8_t i = 0; i < len; i++) {
        lcd_write_data(data[i]);
    }
}

// ST7789V 极简初始化
void bootloader_st7789v_init(void) {
    gpio_num_t pins[] = {LCD_CS_PIN, LCD_RST_PIN, LCD_DC_PIN, LCD_SCL_PIN, LCD_SDA_PIN};
    for (int i = 0; i < 5; i++) {
        esp_rom_gpio_pad_select_gpio(pins[i]);
        gpio_ll_output_enable(&GPIO, pins[i]);
    }

    // 硬件复位
    LCD_RST(1); esp_rom_delay_us(5000);
    LCD_RST(0); esp_rom_delay_us(20000);
    LCD_RST(1); esp_rom_delay_us(120000);

    const uint8_t b6_data[] = {0x0A, 0x82};
    lcd_write_cmd_bytes(0xB6, b6_data, 2);

    const uint8_t b2_data[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    lcd_write_cmd_bytes(0xB2, b2_data, 5);

    const uint8_t b7_data[] = {0x35};
    lcd_write_cmd_bytes(0xB7, b7_data, 1);

    const uint8_t bb_data[] = {0x28};
    lcd_write_cmd_bytes(0xBB, bb_data, 1);

    const uint8_t cmd3a_temp[] = {0x06};
    lcd_write_cmd_bytes(0x3A, cmd3a_temp, 1);

    const uint8_t c0_data[] = {0x0C};
    lcd_write_cmd_bytes(0xC0, c0_data, 1);

    const uint8_t c2_data[] = {0x01, 0xFF};
    lcd_write_cmd_bytes(0xC2, c2_data, 2);

    const uint8_t c3_data[] = {0x10};
    lcd_write_cmd_bytes(0xC3, c3_data, 1);

    const uint8_t c4_data[] = {0x20};
    lcd_write_cmd_bytes(0xC4, c4_data, 1);

    const uint8_t c6_data[] = {0x0F};
    lcd_write_cmd_bytes(0xC6, c6_data, 1);

    const uint8_t d0_data[] = {0xA4, 0xA1};
    lcd_write_cmd_bytes(0xD0, d0_data, 2);

    const uint8_t e0_data[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17};
    lcd_write_cmd_bytes(0xE0, e0_data, 14);

    const uint8_t e1_data[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E};
    lcd_write_cmd_bytes(0xE1, e1_data, 14);

    const uint8_t colmod_data[] = {0x55};
    lcd_write_cmd_bytes(0x3A, colmod_data, 1);

    lcd_write_cmd(0x21);
    esp_rom_delay_us(96 * 1000);

    lcd_write_cmd(0x11);
    esp_rom_delay_us(10 * 1000);

    lcd_write_cmd(0x29);
    esp_rom_delay_us(10 * 1000);

    lcd_write_cmd(0x21);

    const uint8_t madctl_data[] = {0xA0};
    lcd_write_cmd_bytes(0x36, madctl_data, 1);

    esp_rom_delay_us(10 * 1000);
}

void bootloader_lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color_rgb565) {
    lcd_write_cmd(0x2A);
    lcd_write_data(x >> 8); lcd_write_data(x & 0xFF);
    lcd_write_data((x + w - 1) >> 8); lcd_write_data((x + w - 1) & 0xFF);

    lcd_write_cmd(0x2B);
    lcd_write_data(y >> 8); lcd_write_data(y & 0xFF);
    lcd_write_data((y + h - 1) >> 8); lcd_write_data((y + h - 1) & 0xFF);

    lcd_write_cmd(0x2C);

    LCD_CS(0);
    LCD_DC(1);
    uint8_t msb = color_rgb565 >> 8;
    uint8_t lsb = color_rgb565 & 0xFF;

    for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
        bootloader_lcd_spi_write_byte(msb);
        bootloader_lcd_spi_write_byte(lsb);
    }
    LCD_CS(1);
}

void lcd_show_char_8x16(uint16_t x, uint16_t y, char ch, uint16_t fc, uint16_t bc) {
    if (ch < ' ' || ch > '~') {
        ch = ' ';
    }
    uint16_t font_idx = (ch - ' ') * 16;

    lcd_write_cmd(0x2A);
    lcd_write_data(x >> 8);          lcd_write_data(x & 0xFF);
    lcd_write_data((x + 7) >> 8);    lcd_write_data((x + 7) & 0xFF);

    lcd_write_cmd(0x2B);
    lcd_write_data(y >> 8);          lcd_write_data(y & 0xFF);
    lcd_write_data((y + 15) >> 8);   lcd_write_data((y + 15) & 0xFF);

    lcd_write_cmd(0x2C);

    uint8_t fc_h = fc >> 8, fc_l = fc & 0xFF;
    uint8_t bc_h = bc >> 8, bc_l = bc & 0xFF;

    for (uint8_t row = 0; row < 16; row++) {
        for (uint8_t col = 0; col < 8; col++) {
            uint8_t byte_val;
            uint8_t bit_pos;

            if (row < 8) {
                byte_val = F8X16[font_idx + col];
                bit_pos = row;
            } else {
                byte_val = F8X16[font_idx + col + 8];
                bit_pos = row - 8;
            }

            if ((byte_val >> bit_pos) & 0x01) {
                lcd_write_data(fc_h);
                lcd_write_data(fc_l);
            } else {
                lcd_write_data(bc_h);
                lcd_write_data(bc_l);
            }
        }
    }
}

void lcd_show_string(uint16_t x, uint16_t y, const char *str, uint16_t fc, uint16_t bc) {
    while (*str) {
        lcd_show_char_8x16(x, y, *str, fc, bc);
        x += 8;
        str++;
    }
}