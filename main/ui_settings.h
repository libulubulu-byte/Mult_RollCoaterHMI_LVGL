/**
 * @file ui_settings.h
 * @brief SETTINGS 子界面（界面稿 ui5_settings.png）
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ (‹) SETTINGS                                                  │
 *   ├──────────────────────────────────────────────────────────────┤
 *   │ WiFi              Not configured            [ Configure ]     │
 *   │ Display Brightness  ├────●─────┤                70%           │
 *   │ Temperature Unit                  [ °C ] [ °F ]               │
 *   │ Auto Screen Off            [ 30s ]  [ ON ]                    │
 *   │ Factory Reset                              [ RESET ]          │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 全部是真功能（不是摆设）：
 *   WiFi              -> 进配网页（ui_wifi.c）：扫描 / 输密码 / 连接 / 忘记
 *   Display Brightness-> 背光 LEDC 占空比（需要 BSP_LCD_BL_USE_LEDC=1，见 bsp_pins.h）
 *   Temperature Unit  -> 只影响显示（ENVIRONMENT/HISTORY 的数值与之联动）
 *   Auto Screen Off   -> 无触摸 N 秒后关背光，**双击唤醒**（**最容易被误判成死机的一条**：
 *                        熄屏时铺了一层全屏遮罩吃掉触摸，所以单击不会亮、也不会误触下面的按钮；
 *                        日志判据：
 *                          熄屏 `Auto screen off: idle <ms> >= <s>s -> backlight OFF, double-tap to wake`
 *                          唤醒 `Screen woken (double tap) -> backlight <N>%`
 *                        单击只打 `Screen off: wake tap 1/2`（DEBUG 级））
 *   Factory Reset     -> 二次确认后擦掉整个 NVS（机器参数 + 设置 + WiFi 凭证）并重启
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_settings_create(void);
void ui_settings_show(void);
bool ui_settings_is_shown(void);

#ifdef __cplusplus
}
#endif
