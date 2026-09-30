/**
 * @file ui_popup.c
 * @brief 弹窗实现（LVGL 8.3）
 */
#include <string.h>
#include "esp_log.h"

#include "ui_popup.h"
#include "ui_theme.h"

static const char *TAG = "ui_popup";

/* 同一时刻只允许一个弹窗，回调直接放静态变量，避免为每个按钮做内存分配 */
static lv_obj_t *s_pop_root = NULL;
static ui_popup_cb_t s_pop_cb1 = NULL;   /* 左键 / 单键 */
static ui_popup_cb_t s_pop_cb2 = NULL;   /* 右键 */
static void *s_pop_user = NULL;

bool ui_popup_is_open(void)
{
    return s_pop_root != NULL;
}

void ui_popup_close(void)
{
    if (s_pop_root != NULL) {
        /* 回调可能就是从弹窗按钮里发出来的，必须异步删除，否则 LVGL 会在
         * 事件链还没走完时把事件目标一起删掉。 */
        lv_obj_del_async(s_pop_root);
        s_pop_root = NULL;
    }
    s_pop_cb1 = NULL;
    s_pop_cb2 = NULL;
    s_pop_user = NULL;
}

static void popup_button_cb(lv_event_t *e)
{
    int which = (int)(uintptr_t)lv_event_get_user_data(e);
    ui_popup_cb_t cb = (which == 2) ? s_pop_cb2 : s_pop_cb1;
    void *user = s_pop_user;

    ui_popup_close();
    if (cb != NULL) {
        cb(user);
    }
}

/**
 * @brief 建一个弹窗骨架
 * @param modal   是否吃掉触摸事件
 * @param scrim   遮罩颜色（仅 modal 有效）
 */
static lv_obj_t *popup_root_create(bool modal, lv_color_t scrim)
{
    lv_obj_t *root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    if (modal) {
        lv_obj_set_style_bg_color(root, scrim, 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_40, 0);
        /* 默认 lv_obj 就是 CLICKABLE 的，会吃掉底下所有点击 */
        lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    } else {
        /* 非模态：完全透明且不可点击，事件穿透到底层屏幕 */
        lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    }
    return root;
}

/** @brief 建白色卡片并把标题/正文放进去，返回正文的 y 下界 */
static lv_obj_t *popup_card_create(lv_obj_t *root, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, UI_C_CARD_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, UI_C_CARD_BORDER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_shadow_width(card, 12, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void ui_popup_show_choice(const char *title, const char *msg,
                          const char *btn_no_text, ui_popup_cb_t cb_no,
                          const char *btn_yes_text, ui_popup_cb_t cb_yes,
                          void *user_data)
{
    ui_popup_close();

    s_pop_cb1 = cb_no;
    s_pop_cb2 = cb_yes;
    s_pop_user = user_data;

    /* 卡片 600x260 居中：标题 + 正文 + 两个按钮 */
    const int32_t card_w = 600;
    const int32_t card_h = 260;
    const int32_t card_x = (800 - card_w) / 2;
    const int32_t card_y = (480 - card_h) / 2 - 10;

    s_pop_root = popup_root_create(true, UI_C_SCRIM);
    lv_obj_t *card = popup_card_create(s_pop_root, card_x, card_y, card_w, card_h);

    ui_label_create(card, 0, 18, card_w, 34, UI_FONT_PROMPT, UI_C_TEXT,
                    LV_TEXT_ALIGN_CENTER, title);
    ui_label_create_para(card, 30, 62, card_w - 60, 84, UI_FONT_CAPTION, UI_C_TEXT,
                         LV_TEXT_ALIGN_CENTER, msg);

    const int32_t btn_w = 250;
    const int32_t btn_h = 64;   /* 戴手套可点 */
    const int32_t btn_y = card_h - btn_h - 22;
    ui_button_create(card, 30, btn_y, btn_w, btn_h, UI_C_BTN_GREY, UI_C_BTN_GREY_FG,
                     UI_FONT_ACTION, btn_no_text, popup_button_cb, (void *)(uintptr_t)1);
    ui_button_create(card, card_w - btn_w - 30, btn_y, btn_w, btn_h, UI_C_BTN_GREEN,
                     UI_C_KEY_FG_LIGHT, UI_FONT_ACTION, btn_yes_text, popup_button_cb,
                     (void *)(uintptr_t)2);

    ESP_LOGD(TAG, "Choice popup: %s", title ? title : "");
}

void ui_popup_show_notice(const char *title, const char *msg, const char *btn_text,
                          ui_popup_cb_t cb, void *user_data, bool modal)
{
    ui_popup_close();

    s_pop_cb1 = cb;
    s_pop_cb2 = NULL;
    s_pop_user = user_data;

    const int32_t card_w = 620;
    const int32_t card_h = 240;
    const int32_t card_x = (800 - card_w) / 2;
    const int32_t card_y = (480 - card_h) / 2 + 10;

    s_pop_root = popup_root_create(modal, UI_C_SCRIM);
    lv_obj_t *card = popup_card_create(s_pop_root, card_x, card_y, card_w, card_h);

    ui_label_create(card, 0, 16, card_w, 32, UI_FONT_PROMPT, UI_C_TEXT,
                    LV_TEXT_ALIGN_CENTER, title);
    ui_label_create_para(card, 30, 58, card_w - 60, 80, UI_FONT_CAPTION, UI_C_TEXT,
                         LV_TEXT_ALIGN_CENTER, msg);

    const int32_t btn_w = 300;
    const int32_t btn_h = 64;
    ui_button_create(card, (card_w - btn_w) / 2, card_h - btn_h - 20, btn_w, btn_h,
                     UI_C_BTN_GREEN, UI_C_KEY_FG_LIGHT, UI_FONT_ACTION, btn_text,
                     popup_button_cb, (void *)(uintptr_t)1);

    ESP_LOGD(TAG, "Notice popup (%s): %s", modal ? "modal" : "pass-through",
             title ? title : "");
}

void ui_popup_show_alarm(const char *title, const char *msg, const char *btn_text,
                         ui_popup_cb_t cb, void *user_data)
{
    ui_popup_close();

    s_pop_cb1 = cb;
    s_pop_cb2 = NULL;
    s_pop_user = user_data;

    /* 报警用整屏深红：现场任何角度都能立刻看出机器不可操作 */
    lv_obj_t *root = popup_root_create(true, UI_C_BG_MOVING);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    s_pop_root = root;

    const int32_t card_w = 660;
    const int32_t card_h = 330;
    lv_obj_t *card = popup_card_create(root, (800 - card_w) / 2, (480 - card_h) / 2,
                                       card_w, card_h);

    ui_label_create(card, 0, 22, card_w, 48, UI_FONT_PROMPT, UI_C_STOP,
                    LV_TEXT_ALIGN_CENTER, title);
    ui_label_create_para(card, 36, 86, card_w - 72, 110, UI_FONT_ROW, UI_C_TEXT,
                         LV_TEXT_ALIGN_CENTER, msg);

    const int32_t btn_w = 340;
    const int32_t btn_h = 70;
    ui_button_create(card, (card_w - btn_w) / 2, card_h - btn_h - 26, btn_w, btn_h,
                     UI_C_STOP, UI_C_STOP_FG, UI_FONT_ACTION, btn_text,
                     popup_button_cb, (void *)(uintptr_t)1);

    ESP_LOGE(TAG, "ALARM popup shown");
}
