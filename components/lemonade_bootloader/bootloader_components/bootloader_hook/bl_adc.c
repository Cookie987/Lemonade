#include <stdint.h>

#include "hal/adc_ll.h"
#include "hal/adc_types.h"
#include "soc/adc_channel.h"
#include "soc/adc_periph.h" // 提供 ADC 硬件外设基地址定义
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "hal/rtc_io_ll.h"
#include "soc/rtc_io_channel.h"

#include "bl_adc.h"

#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHANNEL     ADC_CHANNEL_1   // GPIO2

void bootloader_adc_init(void)
{
    /* 1. 将 GPIO2 配置为 RTC 管脚模式（切断普通 Digital GPIO 干扰） */
    // GPIO2 对应 RTCIO CHANNEL 11
    rtcio_ll_function_select(11, 0); // 0 即为 RTC/Analog 功能模式
    rtcio_ll_pulldown_disable(11);
    rtcio_ll_pullup_disable(11);
    rtcio_ll_input_enable(11);

    /* 2. 时钟分频与采样格式 */
    adc_ll_digi_set_clk_div(8);
    adc_oneshot_ll_set_output_bits(BAT_ADC_UNIT, ADC_BITWIDTH_12);

    /* 3. 配置 ADC 通道 */
    adc_oneshot_ll_set_channel(BAT_ADC_UNIT, BAT_ADC_CHANNEL);

    /* 4. 重点：同时配置 ADC 内部衰减和 RTC IO 硬件 Pad 衰减开关 */
    adc_oneshot_ll_set_atten(BAT_ADC_UNIT, BAT_ADC_CHANNEL, ADC_ATTEN_DB_12);

    /* 5. 使能 ADC1 硬件模块使能 */
    adc_oneshot_ll_enable(BAT_ADC_UNIT);
    esp_rom_delay_us(2000);
}

uint32_t bootloader_adc_read_raw(void)
{
    uint32_t raw_val = 0;

    // 2. 软件触发一次转换 (1个参数: ADC_UNIT_1)
    adc_oneshot_ll_start(ADC_UNIT_1);

    // 3. 读取硬件原始值 (需结合底层逻辑/寄存器获取 raw)
    // 注意：adc_oneshot_ll_get_raw_result 在部分 LL 库中为直接读寄存器
    raw_val = adc_oneshot_ll_get_raw_result(ADC_UNIT_1);

    // 4. 校验数据的有效性 (2个参数: ADC_UNIT_1 和 raw_val)
    if (!adc_oneshot_ll_raw_check_valid(ADC_UNIT_1, raw_val)) {
        // 如果第一帧无效（上电不稳定），再次采样一帧
        esp_rom_delay_us(200);
        adc_oneshot_ll_start(ADC_UNIT_1);
        raw_val = adc_oneshot_ll_get_raw_result(ADC_UNIT_1);
    }

    return raw_val;
}

uint32_t bootloader_adc_read_mv(void)
{
    uint32_t raw = bootloader_adc_read_raw();

    // 显式指定 2200U 和 4095U 无符号数
    uint32_t pin_mv = (raw * 2200U) / 4095U;

    // 注意：pin_mv * 251U 如果值非常大，可能会溢出
    // 使用 64 位无符号整数 (uint64_t) 进行中间过程运算，防止溢出
    uint64_t temp = ((uint64_t)pin_mv * 251U) / 51U;

    return (uint32_t)temp;
}