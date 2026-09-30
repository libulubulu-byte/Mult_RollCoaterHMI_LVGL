/**
 * @file ui.h
 * @brief 兼容 shim（只为接住字库源文件里的 #include "../ui.h"）
 *
 * main/fonts/ 下的 4 个 SquareLine 字库源文件，第 7 行都是：
 *
 *     #include "../ui.h"
 *
 * 那是它们原来所在工程（examples/get-started/squareline_coffee_5inch/
 * main/ui/fonts/）里的相对路径。本文件放在 main/ 下，正好把
 * main/fonts/xxx.c 的 "../ui.h" 接住，这样字库文件可以原样拷贝过来、
 * 一个字都不必改，日后用 lv_font_conv 重新生成覆盖也不会丢改动。
 *
 * 字库真正需要的只有 lvgl.h，这里给到即可。
 *
 * 注意：本文件不要被其它模块 include（工程里没有别的 "ui.h" 引用），
 *       它的存在仅仅是为了满足上面那条相对路径。
 */
#ifndef ROLLCOATER_UI_SHIM_H
#define ROLLCOATER_UI_SHIM_H

#include "lvgl.h"

#endif /* ROLLCOATER_UI_SHIM_H */
