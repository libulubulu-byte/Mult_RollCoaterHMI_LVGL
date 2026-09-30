/**
 * @file app_light.h
 * @brief LIGHT 页的灯输出：GPIO10 + LEDC PWM 调光
 *
 * ┌─ 硬件（实机接线，别改错）───────────────────────────────────────────────┐
 * │  GPIO10 → 继电器 / MOS 驱动模块的输入                                  │
 * │  高电平点亮（LIGHT_ACTIVE_LEVEL = 1）                                  │
 * │  亮度 = LEDC PWM 占空比，1kHz，10bit（0..1023）                        │
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * ★ 为什么用 LEDC_CHANNEL_1 / LEDC_TIMER_0：
 *   屏幕背光（GPIO2）已经占用了 LEDC_LOW_SPEED_MODE 的 TIMER_1 + CHANNEL_0
 *   （见 bsp_display.c 的 bsp_backlight_init），两者必须错开，
 *   否则 ledc_channel_config 会把背光的通道重新接到 GPIO10 上（屏幕会黑）。
 *
 * ★ 本板的可用引脚只有 GPIO10 / GPIO11 两个：
 *   其它脚分别被 LCD 数据线(1,3,4,5,6,7,8,9,14,15,16,21,45,46,47,48)、
 *   PCLK42/VSYNC41/HSYNC39/DE40/背光2、触摸I2C0(19,20)+RST38、
 *   步进(12,13,17,18)、PSRAM(33~37)、串口 console(43,44) 占用，
 *   GPIO0 是 strapping 且 CrowPanel 变体拿它当 PCLK。
 *
 * 状态持久化：开关与亮度都存在 NVS（见 app_settings），掉电后恢复上次状态。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 灯的输出引脚（本板只剩 10/11 可用，见文件头说明） */
#define LIGHT_GPIO              (GPIO_NUM_10)
/** 有效电平：1 = 高电平点亮 */
#define LIGHT_ACTIVE_LEVEL      (1)

#define LIGHT_LEDC_FREQ_HZ      (1000)          /* 1kHz：继电器不响、MOS 无可见闪 */
#define LIGHT_LEDC_TIMER        (LEDC_TIMER_0)  /* 背光用了 TIMER_1 */
#define LIGHT_LEDC_CHANNEL      (LEDC_CHANNEL_1)/* 背光用了 CHANNEL_0 */
#define LIGHT_LEDC_RES          (LEDC_TIMER_10_BIT)

/** 亮度上限（把"关灯"和"亮度 0"统一起来：0 = 灭） */
#define LIGHT_BRIGHTNESS_MIN    (0)
#define LIGHT_BRIGHTNESS_MAX    (100)

/**
 * @brief 初始化 GPIO/LEDC，并按 NVS 里记着的状态恢复灯的开关与亮度
 * @note 必须在 app_settings_init() 之后调用（要读持久化的状态）
 */
esp_err_t app_light_init(void);

/** @brief 开 / 关灯（亮度沿用上次设置；关灯不清零亮度） */
void app_light_set(bool on);

/** @brief 取反 */
void app_light_toggle(void);

/**
 * @brief 设置亮度（0..100）
 * @note 0 = 熄灭（同时把开关置为 off，避免"显示 ON 但灯不亮"的自相矛盾状态）；
 *       从 0 往上调会自动开灯。
 */
void app_light_set_brightness(uint8_t percent);

/** @brief 灯当前是否亮着 */
bool app_light_is_on(void);

/** @brief 当前亮度（0..100） */
uint8_t app_light_get_brightness(void);

#ifdef __cplusplus
}
#endif
