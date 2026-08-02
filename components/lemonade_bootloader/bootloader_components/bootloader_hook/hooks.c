#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#include "bl_lcd.h"
#include "bl_adc.h"

void bootloader_hooks_include(void) {
}

#define ACC_EN_PIN 40
#define BACKLIGHT_EN_PIN 8
#define BOOT_KEY_PIN 39

#define VER "1.1.0"

static bool bootloader_check_long_press(gpio_num_t key_gpio, uint32_t active_level, uint32_t hold_time_ms) {
    esp_rom_gpio_pad_select_gpio(key_gpio);
    gpio_ll_input_enable(&GPIO, key_gpio);
    
    if (active_level == 0) {
        gpio_ll_pullup_en(&GPIO, key_gpio);
        gpio_ll_pulldown_dis(&GPIO, key_gpio);
    } else {
        gpio_ll_pullup_dis(&GPIO, key_gpio);
        gpio_ll_pulldown_en(&GPIO, key_gpio);
    }

    const uint32_t sample_interval_ms = 10;
    uint32_t total_samples = hold_time_ms / sample_interval_ms;

    for (uint32_t i = 0; i < total_samples; i++) {
        if (gpio_ll_get_level(&GPIO, key_gpio) != active_level) {
            return false;
        }
        esp_rom_delay_us(sample_interval_ms * 1000);
    }

    return true;
}

static inline void bootloader_gpio_set_and_lock(gpio_num_t gpio_num, uint32_t level, bool is_locked) {
    esp_rom_gpio_pad_select_gpio(gpio_num);
    gpio_ll_hold_dis(&GPIO, gpio_num);
    gpio_ll_output_enable(&GPIO, gpio_num);
    gpio_ll_set_level(&GPIO, gpio_num, level);

    if (is_locked) {
        gpio_ll_hold_en(&GPIO, gpio_num);
    }
}

void bootloader_before_init(void) {
    bootloader_gpio_set_and_lock(ACC_EN_PIN, 1, true);
    esp_rom_printf(
    "   __                               __    ____  ____\n"
    "  / /  ___ __ _  ___  ___  ___ ____/ /__ / __ \\/ __/\n"
    " / /__/ -_) ' \\/ _ \\/ _ \\/ _ `/ _  / -_) / /_/ /\\ \\  \n"
    "/____/\\__/_/_/_/\\___/_//_/\\_,_/\\_,_/\\__/  \\____/___/  \n"
    );
    esp_rom_printf("\n");
    esp_rom_printf(" Lemonade 2nd stage Bootloader %s for SMC-TOUCH\n\n", VER);
    esp_rom_printf("NOTICE: This bootloader is designed exclusively for SMARTCLOCK-TOUCH series hardware.\n");
}

// | 颜色     | RGB         | RGB565   |
// | ------ | ----------- | -------- |
// | 黑      | 0,0,0       | `0x0000` |
// | 白      | 255,255,255 | `0xFFFF` |
// | 红      | 255,0,0     | `0xF800` |
// | 绿      | 0,255,0     | `0x07E0` |
// | 蓝      | 0,0,255     | `0x001F` |
// | 青      | 0,255,255   | `0x07FF` |
// | 黄      | 255,255,0   | `0xFFE0` |
// | 品红     | 255,0,255   | `0xF81F` |


void bootloader_after_init(void) {
    bootloader_st7789v_init();
    bootloader_lcd_fill_rect(0, 0, 320, 240, 0x0000);
    uint8_t reason = esp_rom_get_reset_reason(0);
    esp_rom_printf("Reset reason: %d\n", reason);
    bootloader_adc_init();
    uint32_t mv = bootloader_adc_read_mv();
    esp_rom_printf(
        "Battery: %lu mV\n",
        mv
    );
    // 打印十六进制和无符号/有符号十进制，暴露真实内存数值
    ESP_LOGI("pmserv", "Battery Check Raw MV: %lu (0x%08X), signed: %ld", mv, mv, (int32_t)mv);
    bootloader_gpio_set_and_lock(BACKLIGHT_EN_PIN, 1, true);
    if (bootloader_check_long_press(BOOT_KEY_PIN, 1, 200)) {
        ESP_LOGI("pmserv", "Long press confirmed.");
    } 
    else if(reason == 1) {  // RESET_REASON_CHIP_POWER_ON
        ESP_LOGI("pmserv", "Short press confirmed.");
        if (mv < 3200) {
            ESP_LOGI("pmserv", "Battery voltage is too low, please charge the device.");
            lcd_show_string(10, 10, "Battery voltage is too low!", 0xF800, 0x0000);
            lcd_show_string(10, 30, "Please charge the device.", 0xF800, 0x0000);
        }
        else {
            ESP_LOGI("pmserv", "Battery voltage is sufficient.");
        }
        // 5s 后自动关机
        lcd_show_string(10, 70, "Shutting down in 3s", 0xF800, 0x0000);
        esp_rom_delay_us(1000 * 1000);
        lcd_show_string(10, 70, "Shutting down in 2s", 0xF800, 0x0000);
        esp_rom_delay_us(1000 * 1000);
        lcd_show_string(10, 70, "Shutting down in 1s", 0xF800, 0x0000);
        esp_rom_delay_us(1000 * 1000);
        lcd_show_string(10, 70, "Shutting down in 0s", 0xF800, 0x0000);
        bootloader_gpio_set_and_lock(ACC_EN_PIN, 0, true);
    }
    ESP_LOGI("pmserv", "Pulling up Backlight pin...");

    lcd_show_string(10, 10," __                            _", 0x07FF, 0x0000);
    lcd_show_string(10, 30,"|  |   ___ _____ ___ ___ ___ _| |___", 0x07FF, 0x0000);
    lcd_show_string(10, 50,"|  |__| -_|     | . |   | .'| . | -_|", 0x07FF, 0x0000);
    lcd_show_string(10, 70,"|_____|___|_|_|_|___|_|_|__,|___|___|", 0x07FF, 0x0000);
    lcd_show_string(10, 110, "Lemonade Bootloader " VER, 0xFFE0, 0x0000);
    lcd_show_string(10, 130, "Loading Application...", 0xFFFF, 0x0000);
}