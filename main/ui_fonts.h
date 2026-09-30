/**
 * @file ui_fonts.h
 * @brief 主界面用到的**粗体**字库（对齐 800x480 界面稿的字重）
 *
 * 界面稿里所有文字都是粗体，而 LVGL 内置的 lv_font_montserrat_* 只有常规字重、
 * 也没有加粗开关，所以这里挂上 4 个已经生成好的 LVGL 格式粗体字库
 * （源文件在 main/fonts/，编译时一起编进固件）：
 *
 *   变量名             字号   字重  字型        界面稿对应元素（实测字号）
 *   ui_font_H1_bold    50px  Bold  Lato Bold   读数框大数字 0.000   （稿 ~60）
 *   ui_font_H2_bold    42px  Bold  Lato Bold   键盘 1~9 / 0        （稿 ~45）
 *   ui_font_H3_bold    35px  Bold  Lato Bold   STOP                （稿 ~31）
 *   ui_font_P1_bold    24px  Bold  Lato Bold   标题 / State / 提示行 /
 *                                              读数框小标题         （稿 20~24）
 *
 * 来源：本工作区 examples/get-started/squareline_coffee_5inch/main/ui/fonts/
 *       （SquareLine Studio 生成，字符范围 0x20~0x7F、bpp 4、未压缩、LVGL v8 格式）
 *
 * ---------------------------------------------------------------------------
 * 注意 1：这 4 个字库**只含 ASCII**，不含 LVGL 符号字形（U+F0xx / U+F8xx）。
 *         键盘上的 ⌫ / ↵ 正是符号字形，所以这两个键单独用带符号的内置字体，
 *         见 ui_theme.h 的 UI_FONT_SYMBOL（ui_numpad.c 里按需套用）。
 *
 * 注意 2：想换成与界面稿字形完全一致的 Montserrat Bold，用 lv_font_conv 按
 *         同样的**文件名和变量名**重新生成、覆盖 main/fonts/ 下的文件即可，
 *         其余代码一行都不用改（想同时微调字号就改 ui_theme.h 里的字号映射）：
 *
 *           npx lv_font_conv --font Montserrat-Bold.ttf --size 50 --bpp 4 \
 *               --format lvgl -r 0x20-0x7F --no-compress \
 *               -o main/fonts/ui_font_H1_bold.c
 *
 *         4 个字号分别跑一遍：50 / 42 / 35 / 24。
 * ---------------------------------------------------------------------------
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t ui_font_H1_bold;   /* 50px Bold */
extern const lv_font_t ui_font_H2_bold;   /* 42px Bold */
extern const lv_font_t ui_font_H3_bold;   /* 35px Bold */
extern const lv_font_t ui_font_P1_bold;   /* 24px Bold */

#ifdef __cplusplus
}
#endif
