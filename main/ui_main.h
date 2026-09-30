/**
 * @file ui_main.h
 * @brief MOTOR TEST 子界面（原来的辊涂机主界面）
 *
 * ★ 本屏不是上电第一屏：上电显示的是主菜单（ui_menu.c，界面稿 ui1_main.png），
 *   点主菜单上的 MOTOR TEST 卡片进到这里。返回主菜单有两种方式：
 *     ① 顶栏左上角的圆形返回键
 *     ② 在屏幕上从右向左滑动（LVGL 手势）
 *
 * 布局（800x480，坐标照界面稿 rollcoater_main_screen_v3.png 逐像素量出）：
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ (‹) MOTOR TEST                                     State 1    │ y=0..48   白底顶栏
 *   ├───────────────────────┬──────────────────────────────────────┤
 *   │ Motor's Current Pos.  │ Enter Roll Coat Height（左对齐）      │ y=72..104 提示行
 *   │       0.000   绿      │  ┌────┬────┬────┐                   │
 *   ├───────────────────────┤  │ 1  │ 2  │ 3  │                   │
 *   │ Target Position       │  ├────┼────┼────┤   3x4 数字键盘     │ y=111..400
 *   │       0.000   黑      │  │ ⌫  │ 0  │ ↵  │                   │
 *   └───────────────────────┴──┴────┴────┴────┴──────────────────┬─┘
 *                                                               │ STOP │ y=410..458
 *                                                               └──────┘
 *
 * 卡片：x=14..378（w=364，h=137），y=62 / y=217；白底 + 1px 灰边 + 8px 圆角。
 * 键盘：起点 (404,111)，键 107x64，列距 17、行距 11 -> 覆盖 x=404..759。
 *
 * 背景色：|current - target| <= 0.005 -> 整屏绿；运动中 -> 整屏红；
 *         越限待确认 -> 橙；位置未标定 -> 浅灰（与界面稿一致）。
 *
 * 字体：全部粗体，见 ui_fonts.h（main/fonts/ 下的 4 个 LVGL 字库）。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "ui_numpad.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建主屏与轮询定时器
 * @note 必须在 LVGL 锁内调用（bsp_display_lock 之后），且 ui_theme_init 之后
 */
void ui_main_create(void);

/** @brief 主屏对象 */
lv_obj_t *ui_main_screen(void);

/** @brief 切到主屏 */
void ui_main_show(void);

/** @brief 主屏当前是否可见 */
bool ui_main_is_shown(void);

/** @brief 当前输入缓冲区（只读） */
const ui_entry_t *ui_main_entry(void);

/** @brief 清空输入缓冲区并刷新显示 */
void ui_main_clear_entry(void);

/**
 * @brief 在提示行上闪一条消息（3 秒后自动恢复该状态的固定提示）
 * @param color 文字颜色，例如 UI_C_STOP（红）/ UI_C_VALUE_CURRENT（绿）
 */
void ui_main_flash_prompt(const char *text, lv_color_t color);

/** @brief 立即按当前状态刷新标题栏/提示行/读数/背景（Admin 退出时用） */
void ui_main_refresh(void);

#ifdef __cplusplus
}
#endif
