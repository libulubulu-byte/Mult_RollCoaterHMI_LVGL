/**
 * @file ui_numpad.h
 * @brief 数字键盘（1-9 / ⌫ / 0 / ↵）+ 输入缓冲区 + 弹窗式输入对话框
 *
 * 复用关系：
 *   - 主屏右侧键盘：ui_keypad_create() 直接嵌在主屏上
 *   - Admin 屏「Edit」：ui_numpad_open() 弹出一个全屏输入页，内部同样用
 *     ui_keypad_create()，保证两处按键手感与校验逻辑完全一致
 *
 * 输入缓冲区语义：
 *   buf 里只存数字字符（不含小数点），显示时统一按 "值 = atoi(buf)/1000" 的
 *   "%.3f" 格式渲染。因此「1」「2」「3」显示为 0.123，「1230000」显示为 1230.000。
 *   最大 7 位 -> 最大可输入 9999.999。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * 键位
 *============================================================================*/
typedef enum {
    UI_KEY_DIGIT_0 = 0,
    UI_KEY_DIGIT_1,
    UI_KEY_DIGIT_2,
    UI_KEY_DIGIT_3,
    UI_KEY_DIGIT_4,
    UI_KEY_DIGIT_5,
    UI_KEY_DIGIT_6,
    UI_KEY_DIGIT_7,
    UI_KEY_DIGIT_8,
    UI_KEY_DIGIT_9,
    UI_KEY_BACKSPACE = 10,
    UI_KEY_ENTER = 11,
    UI_KEY_COUNT = 12,
} ui_key_id_t;

/** 键盘按键回调（在 LVGL 任务里执行，禁止阻塞） */
typedef void (*ui_key_cb_t)(uint8_t key, void *user_data);

/** 键盘几何参数（按 3 列 × 4 行排布） */
typedef struct {
    int32_t x;        /*!< 键盘左上角 x */
    int32_t y;        /*!< 键盘左上角 y */
    int32_t key_w;    /*!< 单键宽 */
    int32_t key_h;    /*!< 单键高（现场建议 >= 64） */
    int32_t gap_x;    /*!< 列间距 */
    int32_t gap_y;    /*!< 行间距 */
    int32_t label_dy; /*!< 键面文字相对按钮中心的垂直偏移（对齐界面稿用） */
} ui_keypad_geom_t;

/**
 * @brief 创建 3x4 数字键盘
 * @param out_btns 可选，返回 12 个按钮句柄，下标即 ui_key_id_t
 *
 * @note 回调参数的传递方式（无堆分配）：
 *         键位编码 -> 事件 user_data（每个按键一个）
 *         cb 指针  -> 按键自身的 lv_obj user_data
 *         user_data-> parent 的 lv_obj user_data
 *       因此传入的 parent 对象的 user_data 会被本函数占用，调用方不要再用它。
 */
void ui_keypad_create(lv_obj_t *parent, const ui_keypad_geom_t *geom, ui_key_cb_t cb,
                      void *user_data, lv_obj_t *out_btns[UI_KEY_COUNT]);

/*==============================================================================
 * 输入缓冲区
 *============================================================================*/
#define UI_ENTRY_MAX_DIGITS (7)

typedef struct {
    char    buf[UI_ENTRY_MAX_DIGITS + 1];
    uint8_t len;
} ui_entry_t;

void  ui_entry_reset(ui_entry_t *e);
/** @return false 表示已达最大位数，本次输入被忽略 */
bool  ui_entry_push_digit(ui_entry_t *e, uint8_t digit);
/** @return false 表示已经是空的 */
bool  ui_entry_backspace(ui_entry_t *e);
bool  ui_entry_is_empty(const ui_entry_t *e);
/** @brief 当前值（空缓冲 = 0.000） */
float ui_entry_value(const ui_entry_t *e);
/** @brief 渲染成 "%.3f" 字符串 */
void  ui_entry_text(const ui_entry_t *e, char *out, size_t out_len);
/** @brief 与给定值比较（用于判定 9.999 长按解锁 Admin） */
bool  ui_entry_equals(const ui_entry_t *e, float value);
/** @brief 用给定值预填缓冲区（Admin 编辑当前值用） */
void  ui_entry_set_value(ui_entry_t *e, float value);

/*==============================================================================
 * 弹窗式输入对话框（Admin 屏 Edit 复用）
 *============================================================================*/
/** @param accepted true = 按了 Enter/OK；false = 取消 */
typedef void (*ui_numpad_done_cb_t)(bool accepted, float value, void *user_data);

/**
 * @brief 打开全屏数字输入页
 * @param title  顶部标题（例如参数名），内部会拷贝
 * @param hint   副标题/错误提示（可为 NULL），内部会拷贝
 * @param initial 预填值
 */
void ui_numpad_open(const char *title, const char *hint, float initial,
                    ui_numpad_done_cb_t cb, void *user_data);

/** @brief 关闭输入页（不会触发回调） */
void ui_numpad_close(void);

/** @brief 是否处于打开状态 */
bool ui_numpad_is_open(void);

/** @brief 更新提示行（校验失败时在对话框内显示原因） */
void ui_numpad_set_hint(const char *hint);

#ifdef __cplusplus
}
#endif
