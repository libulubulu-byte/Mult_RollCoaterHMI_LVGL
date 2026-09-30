/**
 * @file ui_menu.h
 * @brief 主菜单（MAIN MENU）—— 按界面稿 ui1_main.png 逐像素还原
 *
 * 布局（800x480）：
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ ESP32 SMART CONTROL PANEL              ▂▄▆█   14:30          │ y=0..58  白底顶栏
 *   ├──────────────────────────────────────────────────────────────┤
 *   │ ┌──────────┐ ┌──────────┐ ┌──────────┐                       │
 *   │ │  ( 灯泡 ) │ │ (C)  (C) │ │   (风扇)  │                       │ 卡片 246x170
 *   │ │ ON · 80% │ │26.5°C 55%│ │  READY   │                       │ y=70..240
 *   │ │  LIGHT   │ │ENVIRONMENT│ │MOTOR TEST│                       │
 *   │ └──────────┘ └──────────┘ └──────────┘                       │
 *   │ ┌────────────────────┐ ┌────────────────────┐                │
 *   │ │   ~~~~~~~~~~~~~~   │ │  ----o------       │                │ 卡片 375x167
 *   │ │      HISTORY       │ │      SETTINGS      │                │ y=258..425
 *   │ └────────────────────┘ └────────────────────┘                │
 *   ├──────────────────────────────────────────────────────────────┤
 *   │ System: OK                                        FW v1.0.0  │ y=436..479 底栏
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 入口（本工程目前的实现状态）：
 *   MOTOR TEST  -> 现有的辊涂机界面（ui_main.c，见 ui_main.h），**唯一可用入口**
 *   LIGHT / ENVIRONMENT / HISTORY / SETTINGS -> 按界面稿画出，点击给"未实现"提示
 *   （需求只确认了 MOTOR TEST 一个入口，其余等后续规格）
 *
 * 返回方式（在 MOTOR TEST 子界面上）：
 *   ① 左上角圆形返回键  ② 从右向左滑动（见 ui_main.c 的 main_gesture_cb）
 *
 * 线程模型：本文件所有函数都在 LVGL 任务里执行（esp_lvgl_port 定时器驱动），
 *           禁止 >10ms 的阻塞调用。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建主菜单（必须在 LVGL 锁内、ui_theme_init 之后调用） */
void ui_menu_create(void);

/** @brief 切到主菜单（上电第一屏；也是 MOTOR TEST 子界面的返回目标） */
void ui_menu_show(void);

/** @brief 主菜单当前是否可见 */
bool ui_menu_is_shown(void);

#ifdef __cplusplus
}
#endif
