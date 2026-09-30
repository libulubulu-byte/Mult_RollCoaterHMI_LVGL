/**
 * @file ui_numpad.c
 * @brief 数字键盘 / 输入缓冲区 / 全屏数字输入页实现
 *
 * 主屏键盘与 Admin「Edit」输入页共用 ui_keypad_create()，
 * 两处的按键尺寸、配色、字号、点击手感完全一致。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"

#include "ui_numpad.h"
#include "ui_theme.h"
#include "app_text.h"

static const char *TAG = "ui_numpad";

/*==============================================================================
 * 输入缓冲区
 *============================================================================*/
void ui_entry_reset(ui_entry_t *e)
{
    if (e == NULL) {
        return;
    }
    e->len = 0;
    e->buf[0] = '\0';
}

bool ui_entry_push_digit(ui_entry_t *e, uint8_t digit)
{
    if (e == NULL || digit > 9) {
        return false;
    }
    if (e->len >= UI_ENTRY_MAX_DIGITS) {
        /* ★ 静默丢弃是现场最难排查的现象之一（"按了没反应"），
         *   所以缓冲区满的时候一定留一条日志。 */
        ESP_LOGW(TAG, "entry buffer full (%d digits), key '%d' dropped", UI_ENTRY_MAX_DIGITS, digit);
        return false;   /* 已满，忽略 */
    }
    /* 首位输入 0 直接忽略，避免出现 "0123" 这种无意义缓冲 */
    if (e->len == 0 && digit == 0) {
        return false;
    }
    e->buf[e->len++] = (char)('0' + digit);
    e->buf[e->len] = '\0';
    return true;
}

bool ui_entry_backspace(ui_entry_t *e)
{
    if (e == NULL || e->len == 0) {
        return false;
    }
    e->buf[--e->len] = '\0';
    return true;
}

bool ui_entry_is_empty(const ui_entry_t *e)
{
    return (e == NULL) || (e->len == 0);
}

float ui_entry_value(const ui_entry_t *e)
{
    if (ui_entry_is_empty(e)) {
        return 0.0f;
    }
    /* 缓冲区是纯数字，除以 1000 得到 3 位小数 */
    return (float)strtoul(e->buf, NULL, 10) / 1000.0f;
}

void ui_entry_text(const ui_entry_t *e, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%.3f", (double)ui_entry_value(e));
}

bool ui_entry_equals(const ui_entry_t *e, float value)
{
    return fabsf(ui_entry_value(e) - value) < 0.0005f;
}

void ui_entry_set_value(ui_entry_t *e, float value)
{
    if (e == NULL) {
        return;
    }
    ui_entry_reset(e);
    if (value < 0.0f) {
        value = 0.0f;
    }
    long milli = lroundf(value * 1000.0f);
    if (milli < 0) {
        milli = 0;
    }

    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%ld", milli);

    size_t n = strlen(tmp);
    if (n > UI_ENTRY_MAX_DIGITS) {
        /* 超出可输入范围：截取高位，保证显示值不小于真实值 */
        tmp[UI_ENTRY_MAX_DIGITS] = '\0';
        n = UI_ENTRY_MAX_DIGITS;
    }
    memcpy(e->buf, tmp, n + 1);
    e->len = (uint8_t)n;
}

/*==============================================================================
 * 3x4 键盘
 *============================================================================*/
/* 按键布局（行优先）：数字 1-9、退格、0、回车 */
static const uint8_t s_key_layout[UI_KEY_COUNT] = {
    UI_KEY_DIGIT_1, UI_KEY_DIGIT_2, UI_KEY_DIGIT_3,
    UI_KEY_DIGIT_4, UI_KEY_DIGIT_5, UI_KEY_DIGIT_6,
    UI_KEY_DIGIT_7, UI_KEY_DIGIT_8, UI_KEY_DIGIT_9,
    UI_KEY_BACKSPACE, UI_KEY_DIGIT_0, UI_KEY_ENTER,
};

static const char *key_text(uint8_t key)
{
    static char digit_txt[2] = {0, 0};
    if (key <= UI_KEY_DIGIT_9) {
        digit_txt[0] = (char)('0' + key);
        return digit_txt;
    }
    if (key == UI_KEY_BACKSPACE) {
        return APP_TXT_KEY_BACKSPACE;
    }
    return APP_TXT_KEY_ENTER;
}

static void keypad_event_cb(lv_event_t *e)
{
    uint8_t key = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    ui_key_cb_t cb = (ui_key_cb_t)lv_obj_get_user_data(btn);
    if (cb != NULL) {
        cb(key, lv_obj_get_user_data(lv_obj_get_parent(btn)));
    }
}

void ui_keypad_create(lv_obj_t *parent, const ui_keypad_geom_t *geom, ui_key_cb_t cb,
                      void *user_data, lv_obj_t *out_btns[UI_KEY_COUNT])
{
    if (parent == NULL || geom == NULL) {
        return;
    }

    for (int i = 0; i < UI_KEY_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        uint8_t key = s_key_layout[i];

        lv_color_t bg = UI_C_KEY_BG;
        lv_color_t fg = UI_C_TEXT;
        /* ⌫ / ↵ 是 LVGL 符号字形，4 个 Lato 粗体字库都不含，必须用内置字体 */
        const lv_font_t *font = UI_FONT_KEY;
        if (key == UI_KEY_BACKSPACE) {
            bg = UI_C_KEY_BACKSPACE;
            fg = UI_C_KEY_FG_LIGHT;
            font = UI_FONT_SYMBOL;
        } else if (key == UI_KEY_ENTER) {
            bg = UI_C_KEY_ENTER;
            fg = UI_C_KEY_FG_LIGHT;
            font = UI_FONT_SYMBOL;
        }

        int32_t x = geom->x + col * (geom->key_w + geom->gap_x);
        int32_t y = geom->y + row * (geom->key_h + geom->gap_y);

        lv_obj_t *btn = ui_button_create(parent, x, y, geom->key_w, geom->key_h,
                                         bg, fg, font, key_text(key), NULL, NULL);
        if (btn == NULL) {
            continue;
        }
        /* 键面文字略上移，才能与界面稿的数字位置一致 */
        ui_button_make_bold(btn, geom->label_dy);

        /* user_data 分两层（无堆分配）：
         *   按钮自身 user_data = 键盘回调函数指针
         *   事件 user_data     = 键位编码
         * 键盘回调需要的 context 存在按钮的父对象上。 */
        lv_obj_set_user_data(btn, (void *)cb);
        if (i == 0) {
            lv_obj_set_user_data(parent, user_data);
        }
        lv_obj_add_event_cb(btn, keypad_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)key);

        if (out_btns != NULL) {
            out_btns[i] = btn;
        }
    }
}

/*==============================================================================
 * 全屏数字输入页（Admin Edit）
 *============================================================================*/
/* 布局（800x480）：
 *   0..46  白底标题栏（+ 47..48 分隔线）
 *   54..118 数值显示（48 号字）
 *   126..146 提示/错误行
 *   150..398 键盘（3x4，键 120x56，列距/行距 8）
 *   406..462 Cancel / OK（各 180x56） */
#define NUMPAD_KEY_W     (120)
#define NUMPAD_KEY_H     (56)
#define NUMPAD_KEY_GAP_X (8)
#define NUMPAD_KEY_GAP_Y (8)
#define NUMPAD_KEY_X     (212)   /* (800 - (3*120+2*8)) / 2 = 212 */
#define NUMPAD_KEY_Y     (150)

#define NUMPAD_BTN_Y     (406)
#define NUMPAD_BTN_W     (180)
#define NUMPAD_BTN_H     (56)

static lv_obj_t *s_np_root = NULL;       /* 全屏容器（挂在 top layer 上） */
static lv_obj_t *s_np_lbl_value = NULL;  /* 数值显示 */
static lv_obj_t *s_np_lbl_hint = NULL;   /* 提示 / 错误 */
static ui_entry_t s_np_entry;
static ui_numpad_done_cb_t s_np_cb = NULL;
static void *s_np_user = NULL;
static char s_np_title[48];
static char s_np_hint[96];

bool ui_numpad_is_open(void)
{
    return s_np_root != NULL;
}

static void numpad_refresh_value(void)
{
    if (s_np_lbl_value == NULL) {
        return;
    }
    char txt[16];
    ui_entry_text(&s_np_entry, txt, sizeof(txt));
    ui_label_set_text_bold(s_np_lbl_value, txt);
}

void ui_numpad_set_hint(const char *hint)
{
    if (s_np_lbl_hint == NULL) {
        return;
    }
    if (hint != NULL) {
        strncpy(s_np_hint, hint, sizeof(s_np_hint) - 1);
        s_np_hint[sizeof(s_np_hint) - 1] = '\0';
    }
    lv_label_set_text(s_np_lbl_hint, hint ? s_np_hint : "");
}

void ui_numpad_close(void)
{
    if (s_np_root != NULL) {
        /* 关闭动作常常是从键位回调里发起的（Enter / OK / Cancel），
         * 此时事件链还没结束，必须异步删除，否则会删掉正在处理事件的控件 */
        lv_obj_del_async(s_np_root);
        s_np_root = NULL;
    }
    s_np_lbl_value = NULL;
    s_np_lbl_hint = NULL;
    s_np_cb = NULL;
    s_np_user = NULL;
}

static void numpad_finish(bool accepted)
{
    ui_numpad_done_cb_t cb = s_np_cb;
    void *user = s_np_user;
    float value = ui_entry_value(&s_np_entry);

    /* 先关界面再回调：回调里可能会再 open 一次（校验失败重新输入） */
    ui_numpad_close();

    if (cb != NULL) {
        cb(accepted, value, user);
    }
}

static void numpad_key_cb(uint8_t key, void *user_data)
{
    (void)user_data;

    if (key <= UI_KEY_DIGIT_9) {
        if (ui_entry_push_digit(&s_np_entry, key)) {
            numpad_refresh_value();
        }
    } else if (key == UI_KEY_BACKSPACE) {
        if (ui_entry_backspace(&s_np_entry)) {
            numpad_refresh_value();
        }
    } else if (key == UI_KEY_ENTER) {
        numpad_finish(true);
    }
}

static void numpad_cancel_cb(lv_event_t *e)
{
    (void)e;
    numpad_finish(false);
}

static void numpad_ok_cb(lv_event_t *e)
{
    (void)e;
    numpad_finish(true);
}

void ui_numpad_open(const char *title, const char *hint, float initial,
                    ui_numpad_done_cb_t cb, void *user_data)
{
    ui_numpad_close();

    strncpy(s_np_title, title ? title : "", sizeof(s_np_title) - 1);
    s_np_title[sizeof(s_np_title) - 1] = '\0';
    strncpy(s_np_hint, hint ? hint : "", sizeof(s_np_hint) - 1);
    s_np_hint[sizeof(s_np_hint) - 1] = '\0';

    s_np_cb = cb;
    s_np_user = user_data;
    ui_entry_set_value(&s_np_entry, initial);

    /* 挂在 top layer：不受屏幕切换影响，也不会被主屏对象盖住 */
    uint16_t ws = lv_obj_get_width(lv_scr_act());
    uint16_t hs = lv_obj_get_height(lv_scr_act());
    if (ws == 0 || hs == 0) {
        ws = UI_SCR_W;
        hs = UI_SCR_H;
    }
    (void)hs;

    s_np_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_np_root);
    lv_obj_set_size(s_np_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_np_root, 0, 0);
    lv_obj_set_style_bg_color(s_np_root, UI_C_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(s_np_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_np_root, 0, 0);
    lv_obj_clear_flag(s_np_root, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题栏 */
    ui_panel_create(s_np_root, 0, 0, UI_SCR_W, UI_TOP_BAR_H, UI_C_BAR_BG);
    ui_panel_create(s_np_root, 0, UI_TOP_BAR_LINE_Y, UI_SCR_W, UI_TOP_BAR_LINE_H, UI_C_BAR_LINE);
    ui_label_create_bold(s_np_root, 16, 0, UI_SCR_W - 32, UI_TOP_BAR_H, UI_FONT_TITLE,
                         UI_C_TEXT, LV_TEXT_ALIGN_LEFT, s_np_title);

    /* 数值显示（H2 粗体，居中） */
    s_np_lbl_value = ui_label_create_bold(s_np_root, 0, 54, UI_SCR_W, 64, UI_FONT_NUMPAD_VALUE,
                                          UI_C_TEXT, LV_TEXT_ALIGN_CENTER, "0.000");

    /* 提示 / 错误行 */
    s_np_lbl_hint = ui_label_create(s_np_root, 0, 126, UI_SCR_W, 20, UI_FONT_SMALL,
                                    UI_C_STOP, LV_TEXT_ALIGN_CENTER, s_np_hint);
    numpad_refresh_value();

    /* 键盘 */
    const ui_keypad_geom_t geom = {
        .x = NUMPAD_KEY_X,
        .y = NUMPAD_KEY_Y,
        .key_w = NUMPAD_KEY_W,
        .key_h = NUMPAD_KEY_H,        /* 56px：比主屏键盘略矮 */
        .gap_x = NUMPAD_KEY_GAP_X,
        .gap_y = NUMPAD_KEY_GAP_Y,
        .label_dy = -2,
    };
    ui_keypad_create(s_np_root, &geom, numpad_key_cb, NULL, NULL);

    /* 取消 / 确定（与键盘左右边界对齐） */
    ui_button_create(s_np_root, NUMPAD_KEY_X, NUMPAD_BTN_Y, NUMPAD_BTN_W, NUMPAD_BTN_H,
                     UI_C_BTN_GREY, UI_C_BTN_GREY_FG, UI_FONT_ACTION,
                     APP_TXT_ADMIN_CANCEL, numpad_cancel_cb, NULL);
    lv_obj_t *ok = ui_button_create(s_np_root,
                                    NUMPAD_KEY_X + 2 * NUMPAD_KEY_W + NUMPAD_KEY_GAP_X * 2 + 4,
                                    NUMPAD_BTN_Y, NUMPAD_BTN_W, NUMPAD_BTN_H,
                                    UI_C_BTN_GREEN, UI_C_KEY_FG_LIGHT, UI_FONT_ACTION,
                                    APP_TXT_ADMIN_OK, numpad_ok_cb, NULL);
    ui_button_make_bold(ok, -2);

    ESP_LOGD(TAG, "Numpad opened for '%s'", s_np_title);
}
