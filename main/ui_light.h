/**
 * @file ui_light.h
 * @brief LIGHT CONTROL 子界面（界面稿 ui2_light.png）
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ (‹)  LIGHT CONTROL                        ON • 80%           │ 顶栏（ui_subpage）
 *   ├──────────────────────────────────────────────────────────────┤
 *   │      (灯泡)                        ┌──────────┐              │
 *   │                                    │          │              │
 *   │        Light 1                     │   ON     │  <- 圆形开关 │
 *   │      Status: ON                    │          │              │
 *   │                                    └──────────┘              │
 *   │  Brightness      ├──────●───────┤                 80%        │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 真实硬件：GPIO10 继电器/MOS，高电平点亮，亮度走 LEDC PWM 1kHz（见 app_light.h）。
 * 开关与亮度会写进 NVS，掉电后恢复。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建 LIGHT 子界面（LVGL 锁内调用） */
void ui_light_create(void);

/** @brief 切到该子界面并立刻刷新一次显示 */
void ui_light_show(void);

/** @brief 当前是否显示中 */
bool ui_light_is_shown(void);

#ifdef __cplusplus
}
#endif
