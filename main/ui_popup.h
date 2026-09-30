/**
 * @file ui_popup.h
 * @brief 通用弹窗：二选一对话框 / 单键提示 / 全屏报警
 *
 * 全部挂在 lv_layer_top() 上，因此换屏（主屏 <-> Admin）不会把弹窗弄丢。
 *
 * 模态与非模态的区别（很关键）：
 *   模态   ：铺半透明遮罩并吃掉所有触摸事件，底下键盘不可用。
 *            用于「输入是否正确？」这种必须先回答的问题，以及报警。
 *   非模态 ：没有遮罩、卡片不接收点击，触摸会穿透到底下的数字键盘。
 *            用于 State3 的「用手轮摇到限位内，再按 Enter」——
 *            操作员既可以点卡片上的 Enter，也可以直接按屏幕键盘的 ↵。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 弹窗按钮回调（在 LVGL 任务里执行，禁止阻塞；回调时弹窗已经关闭） */
typedef void (*ui_popup_cb_t)(void *user_data);

/**
 * @brief 二选一对话框（模态）
 * @param btn_no_text  左键文案
 * @param cb_no        左键回调（可为 NULL）
 * @param btn_yes_text 右键文案
 * @param cb_yes       右键回调（可为 NULL）
 */
void ui_popup_show_choice(const char *title, const char *msg,
                          const char *btn_no_text, ui_popup_cb_t cb_no,
                          const char *btn_yes_text, ui_popup_cb_t cb_yes,
                          void *user_data);

/**
 * @brief 单键提示
 * @param modal true = 模态（带遮罩）；false = 不遮挡底下键盘
 */
void ui_popup_show_notice(const char *title, const char *msg, const char *btn_text,
                          ui_popup_cb_t cb, void *user_data, bool modal);

/**
 * @brief 全屏红色报警窗（模态，禁止一切操作，必须人工确认）
 */
void ui_popup_show_alarm(const char *title, const char *msg, const char *btn_text,
                         ui_popup_cb_t cb, void *user_data);

/** @brief 关闭当前弹窗（不触发回调） */
void ui_popup_close(void);

/** @brief 是否有弹窗处于打开状态 */
bool ui_popup_is_open(void);

#ifdef __cplusplus
}
#endif
