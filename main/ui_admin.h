/**
 * @file ui_admin.h
 * @brief Admin 屏：机器参数浏览 / 编辑 / 保存
 *
 * 布局（800x480）：
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ Admin - Machine Parameters            [ Exit and Accept ]     │ y=0..48
 *   ├──────────────────────────────────────────────────────────────┤
 *   │ Upper Limit            50.000                    [ Edit ]     │ 行高 56
 *   │ Lower Limit             0.000                    [ Edit ]     │
 *   │ Acceleration          150.000                    [ Edit ]     │ 可滚动容器
 *   │ Deceleration          150.000                    [ Edit ]     │ x=16,y=64,w=768,h=404
 *   │ Speed                  20.000                    [ Edit ]     │
 *   │ Current Position        0.000                    [ Edit ]     │
 *   │ Backlash                0.200                    [ Edit ]     │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 权限：只有在 State1 / State2 下输入 9.999 并长按 Enter 满 3 秒才能进来
 *       （检测逻辑在 ui_main.c 的 main_hold_cb）。
 *
 * 保存策略：
 *   编辑只改内存副本 s_pending；点「Exit and Accept」才异步写 NVS 并返回主屏。
 *   NVS 写由 app_config 的落盘任务完成，UI 任务只做一次入队，不会卡界面。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建 Admin 屏（必须在 LVGL 锁内调用） */
void ui_admin_create(void);

/** @brief 切到 Admin 屏并从当前生效参数重新载入编辑副本 */
void ui_admin_show(void);

/** @brief 离开 Admin 屏 */
void ui_admin_hide(void);

/** @brief Admin 屏当前是否可见 */
bool ui_admin_is_shown(void);

#ifdef __cplusplus
}
#endif
