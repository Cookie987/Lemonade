#include <stdint.h>
#include <stdbool.h>

#include "hal/adc_ll.h"
#include "hal/adc_types.h"
#include "soc/adc_channel.h"
#include "soc/adc_periph.h"
#include "soc/apb_saradc_reg.h"
#include "soc/sens_reg.h"
#include "soc/system_reg.h"
#include "hal/regi2c_ctrl.h"
#include "soc/regi2c_saradc.h"
#include "esp_efuse_rtc_calib.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_num.h"

#include "bl_adc.h"

#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHANNEL     ADC_CHANNEL_1   // GPIO2
#define ADC_SAMPLE_COUNT    16

// eFuse 校准参考点：电压 850mV 对应的 ADC1/6dB raw 计数（每颗芯片不同）
static uint32_t s_adc_cali_digi = 0;
static uint32_t s_adc_cali_vol  = 850;

void bootloader_adc_init(void)
{
    // 0. SAR 内部 I2C 总线上电（等价于应用侧 regi2c_saradc_enable()）。
    //    冷启动时该总线默认断电，REGI2C 写操作会静默失败。
    SET_PERI_REG_MASK(RTC_CNTL_ANA_CONF_REG, RTC_CNTL_SAR_I2C_PU_M);
    CLEAR_PERI_REG_MASK(ANA_CONFIG_REG, I2C_SAR_M);
    SET_PERI_REG_MASK(ANA_CONFIG2_REG, ANA_SAR_CFG2_M);

    // 1. 恢复 bootloader_random_enable() 留下的 I2C 校准位。
    //    RNG 会把 ENCAL_REF 置 1，将 SAR 采样源切到内部基准电压，不恢复会导致外部通道恒读 0。
    REGI2C_WRITE_MASK(I2C_SAR_ADC, ADC_SARADC_ENCAL_REF_ADDR, 0);   // 恢复为采样外部引脚
    REGI2C_WRITE_MASK(I2C_SAR_ADC, ADC_SARADC_ENT_TSENS_ADDR, 0);
    REGI2C_WRITE_MASK(I2C_SAR_ADC, ADC_SARADC_ENT_RTC_ADDR, 0);
    REGI2C_WRITE_MASK(I2C_SAR_ADC, ADC_SARADC_DTEST_RTC_ADDR, 0);

    // 2. 清理 bootloader_random_enable() 留下的 ADC1 数字控制器状态。
    //    RNG 会把 ADC1 切给 DIG 控制器并让定时器持续触发采样，不清理会导致 RTC 单次转换拿不到 SAR 而卡死。
    REG_SET_FIELD(SENS_SAR_POWER_XPD_SAR_REG, SENS_FORCE_XPD_SAR, 0);    // SAR 电源交还 FSM
    CLEAR_PERI_REG_MASK(SENS_SAR_MEAS1_MUX_REG, SENS_SAR1_DIG_FORCE);    // ADC1 交还 RTC 控制
    CLEAR_PERI_REG_MASK(APB_SARADC_CTRL2_REG, APB_SARADC_TIMER_EN);      // 停止 digi 定时触发
    CLEAR_PERI_REG_MASK(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_APB_SARADC_CLK_EN);
    SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_APB_SARADC_RST);

    // 3. 将 GPIO2 配置为模拟输入模式（禁用数字输入、输出、上下拉）
    gpio_ll_input_disable(&GPIO, GPIO_NUM_2);
    gpio_ll_output_disable(&GPIO, GPIO_NUM_2);
    gpio_ll_pullup_dis(&GPIO, GPIO_NUM_2);
    gpio_ll_pulldown_dis(&GPIO, GPIO_NUM_2);

    // 4. 使能 ADC1 控制器
    adc_oneshot_ll_enable(ADC_UNIT_1);

    adc_oneshot_ll_set_atten(ADC_UNIT_1, BAT_ADC_CHANNEL, ADC_ATTEN_DB_6);

    // 6. 设置 12-bit 位宽 (0 ~ 4095)
    adc_oneshot_ll_set_output_bits(ADC_UNIT_1, ADC_BITWIDTH_12);

    // 7. 关键：选中 ADC1 通道 1 (GPIO2)
    //    缺少这一步时 RTC 控制器不采样任何引脚，读取结果恒为 0
    adc_oneshot_ll_set_channel(ADC_UNIT_1, BAT_ADC_CHANNEL);

    // 8. 切换为 RTC 控制模式（SW 控制启动与通道位图）
    adc_ll_set_controller(ADC_UNIT_1, ADC_LL_CTRL_RTC);

    // 9. 与应用侧 adc_oneshot 驱动对齐的补充配置：
    //    - SAR 时钟分频（冷启动默认值可能为 0，导致 SAR 无时钟、采样为 0）
    //    - 强制 SAR 模拟部分上电（等价 sar_ctrl_ll POWER_ON）
    //    - 校准 DREF 初始化（等价 adc_ll_calibration_init）
    adc_ll_set_sar_clk_div(ADC_UNIT_1, ADC_LL_SAR_CLK_DIV_DEFAULT(ADC_UNIT_1));
    SENS.sar_peri_clk_gate_conf.saradc_clk_en = 1;
    SENS.sar_power_xpd_sar.force_xpd_sar = 0x3;   // SAR 强制上电（与应用侧一致）
    adc_ll_calibration_init(ADC_UNIT_1);

    // 10. 写入 eFuse 硬件校准码（INITIAL_CODE）。
    //     应用侧驱动每次读取都会执行 adc_calc_hw_calibration_code + adc_set_hw_calibration_code；
    //     缺少它时 SAR 内部偏移未补偿，原始值会明显偏高
    //     （实测 1S 电池 4.18V：未补偿 raw=3182，补偿后应约为 890）。
    int calib_ver = esp_efuse_rtc_calib_get_ver();
    if (calib_ver >= ESP_EFUSE_ADC_CALIB_VER_MIN && calib_ver <= ESP_EFUSE_ADC_CALIB_VER_MAX) {
        uint32_t init_code = esp_efuse_rtc_calib_get_init_code(calib_ver, (uint32_t)ADC_UNIT_1, (int)ADC_ATTEN_DB_6);
        adc_ll_set_calibration_param(ADC_UNIT_1, init_code);

        // 读取本芯片 6dB 档的校准参考点（digi, 850mV），用于电压换算
        uint32_t cali_digi = 0, cali_vol = 0;
        if (esp_efuse_rtc_calib_get_cal_voltage(calib_ver, (uint32_t)ADC_UNIT_1, (int)ADC_ATTEN_DB_6,
                                                &cali_digi, &cali_vol) == ESP_OK && cali_digi > 0) {
            s_adc_cali_digi = cali_digi;
            s_adc_cali_vol  = cali_vol;
        }
    }

    // 11. 给模拟部分 2ms 稳定时间，防止冷启动读 0
    esp_rom_delay_us(2000);
}

void bootloader_adc_debug_dump(void)
{
    esp_rom_printf("ADC dbg: ctrl2=0x%08lx mux=0x%08lx pwr=0x%08lx rd1=0x%08lx status=0x%02lx\n",
                   (unsigned long)SENS.sar_meas1_ctrl2.val,
                   (unsigned long)SENS.sar_meas1_mux.val,
                   (unsigned long)SENS.sar_power_xpd_sar.val,
                   (unsigned long)SENS.sar_reader1_ctrl.val,
                   (unsigned long)SENS.sar_slave_addr1.meas_status);
    esp_rom_printf("ADC dbg: encal_ref=%lu dref=%lu done=%lu\n",
                   (unsigned long)REGI2C_READ_MASK(I2C_SAR_ADC, ADC_SARADC_ENCAL_REF_ADDR),
                   (unsigned long)REGI2C_READ_MASK(I2C_SAR_ADC, ADC_SAR1_DREF_ADDR),
                   (unsigned long)SENS.sar_meas1_ctrl2.meas1_done_sar);
    esp_rom_printf("ADC dbg: init_code=0x%04lx\n",
                   (unsigned long)((REGI2C_READ_MASK(I2C_SAR_ADC, ADC_SAR1_INITIAL_CODE_HIGH_ADDR) << 8) |
                                   REGI2C_READ_MASK(I2C_SAR_ADC, ADC_SAR1_INITIAL_CODE_LOW_ADDR)));
    esp_rom_printf("ADC dbg: cali_digi=%lu cali_vol=%lu\n",
                   (unsigned long)s_adc_cali_digi, (unsigned long)s_adc_cali_vol);
}

uint32_t bootloader_adc_read_raw(void)
{
    uint32_t raw_val = 0;

    // 软件触发一次转换
    adc_oneshot_ll_start(ADC_UNIT_1);

    // 等待本次转换完成（冷启动首帧必须等 DONE 标志，
    // 转换未完成时直接读结果寄存器会读到 0）。
    // 带 1ms 超时保护，异常状态下也不会让 bootloader 卡死。
    for (int i = 0; i < 10 && !adc_oneshot_ll_get_event(ADC_LL_EVENT_ADC1_ONESHOT_DONE); i++) {
        esp_rom_delay_us(100);
    }

    // 读取硬件原始值
    raw_val = adc_oneshot_ll_get_raw_result(ADC_UNIT_1);

    return raw_val;
}

uint32_t bootloader_adc_read_mv(void)
{
    uint32_t raw_samples[ADC_SAMPLE_COUNT];
    uint32_t raw_sum = 0;
    uint32_t valid_count = 0;

    // 1. 给内部模拟电路 2ms 稳定偏置时间（解决冷启动读 0 问题）
    esp_rom_delay_us(2000);

    // 2. 预热采样：丢弃第一帧数据（第一帧往往极其不准）
    (void)bootloader_adc_read_raw();
    esp_rom_delay_us(200);

    // 3. 连续采样 16 次
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        raw_samples[i] = bootloader_adc_read_raw();
        esp_rom_delay_us(100);
    }

    // 4. 冒泡排序
    for (int i = 0; i < ADC_SAMPLE_COUNT - 1; i++) {
        for (int j = 0; j < ADC_SAMPLE_COUNT - i - 1; j++) {
            if (raw_samples[j] > raw_samples[j + 1]) {
                uint32_t temp = raw_samples[j];
                raw_samples[j] = raw_samples[j + 1];
                raw_samples[j + 1] = temp;
            }
        }
    }

    // 5. 去头去尾：丢弃最小 4 个和最大 4 个，取中间 8 个求平均
    for (int i = 4; i < (ADC_SAMPLE_COUNT - 4); i++) {
        raw_sum += raw_samples[i];
        valid_count++;
    }

    uint32_t raw_avg = raw_sum / valid_count;

    // 6. 电压计算
    // 与应用侧曲线拟合校准的第一步一致（eFuse 参考点，6dB 档）：
    //   pin_mv = raw * 850 / cali_digi
    // 再乘以分压倍数 5（与 ESPHome multiply: 5 一致）：
    //   bat_mv = pin_mv * 5
    // 若 eFuse 校准不可用，则兜底用 6dB 标称满量程 2200mV。
    uint64_t raw_64 = raw_avg;
    uint32_t pin_mv;
    if (s_adc_cali_digi > 0) {
        pin_mv = (uint32_t)((raw_64 * s_adc_cali_vol) / s_adc_cali_digi);
    } else {
        pin_mv = (uint32_t)((raw_64 * 2200U) / 4095U);
    }
    uint32_t bat_mv = (uint32_t)(((uint64_t)pin_mv * 5U));

    return bat_mv;
}
