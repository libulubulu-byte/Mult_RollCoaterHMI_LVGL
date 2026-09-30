/**
 * @file ui_theme.c
 * @brief 主题与通用控件工厂实现（LVGL 8.3）
 */
#include <stdio.h>
#include "esp_log.h"
#include "ui_theme.h"

static const char *TAG = "ui_theme";

/*
 * 粗体策略
 *   UI_BOLD_EMULATE = 0（默认）：UI_FONT_* 指向 main/fonts/ 下的真粗体字库
 *                               （Lato Bold），不需要任何叠加。
 *   UI_BOLD_EMULATE = 1        ：换回 LVGL 内置 Montserrat（只有 Regular）时，
 *                               用「向右 1px 的幽灵副本」把文字叠成粗体。
 * 两种情况下面向调用方的 API 完全一样。
 */
#ifndef UI_BOLD_EMULATE
#define UI_BOLD_EMULATE 0
#endif
/* 幽灵副本相对主 label 的水平偏移（像素），仅 UI_BOLD_EMULATE=1 时有效 */
#define UI_BOLD_OFFSET_X    (1)

/* 共享 style（只初始化一次） */
static lv_style_t s_style_card;      /* 白底 + 边框 + 圆角 + 无内边距 */
static lv_style_t s_style_flat;      /* 无边框无阴影无内边距 */
static bool s_theme_ready = false;

void ui_theme_init(void)
{
    if (s_theme_ready) {
        return;
    }

    /* --- 读数卡片 --- */
    lv_style_init(&s_style_card);
    lv_style_set_bg_color(&s_style_card, UI_C_CARD_BG);
    lv_style_set_bg_opa(&s_style_card, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_card, UI_CARD_BORDER_W);
    lv_style_set_border_color(&s_style_card, UI_C_CARD_BORDER);
    lv_style_set_radius(&s_style_card, UI_CARD_RADIUS);
    lv_style_set_shadow_width(&s_style_card, 0);
    lv_style_set_pad_all(&s_style_card, 0);
    lv_style_set_text_color(&s_style_card, UI_C_TEXT);

    /* --- 扁平容器 --- */
    lv_style_init(&s_style_flat);
    lv_style_set_bg_opa(&s_style_flat, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_flat, 0);
    lv_style_set_radius(&s_style_flat, 0);
    lv_style_set_shadow_width(&s_style_flat, 0);
    lv_style_set_pad_all(&s_style_flat, 0);

    s_theme_ready = true;
    ESP_LOGI(TAG, "Theme ready (page #EEEEEE, key #E0E0E0, green #2E7D32, stop #D32F2F, bold=%s)",
             UI_BOLD_EMULATE ? "emulated" : "Lato Bold fonts");
}

/*==============================================================================
 * 文本
 *============================================================================*/
static lv_obj_t *label_create_internal(lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
                                       int32_t h, const lv_font_t *font, lv_color_t color,
                                       lv_text_align_t align, const char *text, bool wrap)
{
    lv_obj_t *lbl = lv_label_create(parent);
    if (lbl == NULL) {
        return NULL;
    }

    lv_obj_add_style(lbl, &s_style_flat, 0);
    lv_obj_set_width(lbl, w);
    /* 单行时裁剪而不换行，避免把布局撑变形 */
    lv_label_set_long_mode(lbl, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(lbl, text ? text : "");

    /* 垂直居中：字体行高已知，把文字行顶端压在 (h - line_height)/2 上。
     * 这样「文本框」的垂直位置可直接由界面稿反算，不依赖具体字体。 */
    int32_t line_h = font->line_height;
    int32_t ty = y + (h - line_h) / 2;
    if (ty < y) {
        ty = y;
    }
    lv_obj_set_pos(lbl, x, ty);
    return lbl;
}

lv_obj_t *ui_label_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          const lv_font_t *font, lv_color_t color, lv_text_align_t align,
                          const char *text)
{
    return label_create_internal(parent, x, y, w, h, font, color, align, text, false);
}

lv_obj_t *ui_label_create_para(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                               const lv_font_t *font, lv_color_t color, lv_text_align_t align,
                               const char *text)
{
    lv_obj_t *lbl = label_create_internal(parent, x, y, w, h, font, color, align, text, true);
    if (lbl != NULL) {
        lv_obj_set_height(lbl, h);
        lv_obj_set_pos(lbl, x, y);
    }
    return lbl;
}

/*
 * 「粗体」文本。
 *
 * UI_BOLD_EMULATE = 0：字体本身就是粗体，直接创建普通 label。
 * UI_BOLD_EMULATE = 1：另外建一个完全相同的 label 向右偏移 1px，两个同色文字
 *                      叠加后笔画变粗。幽灵副本句柄挂在主 label 的 user_data 上，
 *                      改文案时用 ui_label_set_text_bold() 一起改。
 */
lv_obj_t *ui_label_create_bold(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                               const lv_font_t *font, lv_color_t color, lv_text_align_t align,
                               const char *text)
{
#if UI_BOLD_EMULATE
    lv_obj_t *ghost = label_create_internal(parent, x + UI_BOLD_OFFSET_X, y, w, h,
                                            font, color, align, text, false);
    lv_obj_t *lbl = label_create_internal(parent, x, y, w, h, font, color, align, text, false);
    if (lbl == NULL) {
        if (ghost) {
            lv_obj_del(ghost);
        }
        return NULL;
    }
    if (ghost == NULL) {
        return lbl;   /* 内存不足时退化为普通文本 */
    }
    /* 幽灵在下面、主 label 在上面，改文案时两个都改 */
    lv_obj_move_background(ghost);
    lv_obj_set_user_data(lbl, ghost);
    return lbl;
#else
    return label_create_internal(parent, x, y, w, h, font, color, align, text, false);
#endif
}

void ui_label_set_text_bold(lv_obj_t *lbl, const char *text)
{
    if (lbl == NULL) {
        return;
    }
    lv_label_set_text(lbl, text ? text : "");
#if UI_BOLD_EMULATE
    lv_obj_t *ghost = (lv_obj_t *)lv_obj_get_user_data(lbl);
    if (ghost != NULL) {
        lv_label_set_text(ghost, text ? text : "");
    }
#else
    (void)0;
#endif
}

void ui_fmt_value(char *buf, size_t n, float value)
{
    /* 显示统一 3 位小数，例如 0.000 / 12.345 */
    snprintf(buf, n, "%.3f", (double)value);
}

void ui_label_set_value(lv_obj_t *lbl, float value)
{
    if (lbl == NULL) {
        return;
    }
    char buf[16];
    ui_fmt_value(buf, sizeof(buf), value);
    lv_label_set_text(lbl, buf);
}

void ui_label_set_value_bold(lv_obj_t *lbl, float value)
{
    if (lbl == NULL) {
        return;
    }
    char buf[16];
    ui_fmt_value(buf, sizeof(buf), value);
    ui_label_set_text_bold(lbl, buf);
}

/*==============================================================================
 * 容器与按钮
 *============================================================================*/
lv_obj_t *ui_panel_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          lv_color_t bg)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        return NULL;
    }
    lv_obj_add_style(obj, &s_style_flat, 0);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *ui_card_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        return NULL;
    }
    lv_obj_add_style(obj, &s_style_card, 0);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *ui_button_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           lv_color_t bg, lv_color_t fg, const lv_font_t *font,
                           const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    if (btn == NULL) {
        return NULL;
    }
    lv_obj_add_style(btn, &s_style_flat, 0);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, UI_KEY_RADIUS, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    /* 按下反馈：底色加深（触摸屏在强光下也要能看出反馈） */
    lv_obj_set_style_bg_color(btn, lv_color_darken(bg, 40), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_mix(UI_C_DISABLED, bg, 128), LV_STATE_DISABLED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_add_style(lbl, &s_style_flat, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_center(lbl);

    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

lv_obj_t *ui_button_get_label(lv_obj_t *btn)
{
    if (btn == NULL) {
        return NULL;
    }
    return lv_obj_get_child(btn, 0);
}

void ui_button_move_label(lv_obj_t *btn, int32_t dy)
{
    lv_obj_t *lbl = ui_button_get_label(btn);
    if (lbl != NULL) {
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, dy);
    }
}

void ui_button_make_bold(lv_obj_t *btn, int32_t dy)
{
    lv_obj_t *lbl = ui_button_get_label(btn);
    if (lbl == NULL) {
        return;
    }
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, dy);

#if UI_BOLD_EMULATE
    /* 幽灵副本：同样的字体/颜色/文案，向右 1px */
    lv_obj_t *ghost = lv_label_create(btn);
    lv_obj_add_style(ghost, &s_style_flat, 0);
    lv_obj_set_style_text_font(ghost, lv_obj_get_style_text_font(lbl, 0), 0);
    lv_obj_set_style_text_color(ghost, lv_obj_get_style_text_color(lbl, 0), 0);
    lv_label_set_long_mode(ghost, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ghost, lv_label_get_text(lbl));
    lv_obj_align(ghost, LV_ALIGN_CENTER, UI_BOLD_OFFSET_X, dy);
    /* 注意：这里不能 move_background，否则主 label 不再是 btn 的第 0 个子对象，
     * ui_button_get_label() 就会拿到幽灵副本。幽灵同色同字，画在上面视觉等价。 */

    /* 让 ui_label_set_text_bold() 也能更新按钮文字 */
    lv_obj_set_user_data(lbl, ghost);
#endif
}

void ui_button_set_text(lv_obj_t *btn, const char *text)
{
    lv_obj_t *lbl = ui_button_get_label(btn);
    if (lbl != NULL) {
        ui_label_set_text_bold(lbl, text);
    }
}

void ui_screen_set_bg(lv_obj_t *scr, lv_color_t color)
{
    if (scr == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(scr, color, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}
