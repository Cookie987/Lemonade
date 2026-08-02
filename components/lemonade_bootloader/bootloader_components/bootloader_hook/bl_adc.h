#pragma once

#include <stdint.h>

/**
 * @brief Bootloader 极简 ADC1 初始化 (GPIO2)
 */
void bootloader_adc_init(void);

/**
 * @brief 读取 ADC1 原始采样值 (12-bit, 0~4095)
 */
uint32_t bootloader_adc_read_raw(void);

/**
 * @brief 读取计算后的电池电压 (单位: mV)
 */
uint32_t bootloader_adc_read_mv(void);