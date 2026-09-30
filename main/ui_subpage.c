/**
 * @file ui_subpage.c
 * @brief 子界面公共框架实现（说明见 ui_subpage.h）
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"

#include "ui_subpage.h"
#include "ui_theme.h"
#include "ui_menu.h"
#include "app_text.h"

static const char *TAG = "ui_subpage";

/* 顶栏几何（比 ui_menu 的顶栏多一个返回键，尺寸见 ui_theme.h 的说明） */
#define SUB_BAR_H        (UI_MENU_BAR_H)          /* 58 */
#define SUB_BACK_X       (8)
#define SUB_BACK_Y       (9)                      /* (58-40)/2 */
#define SUB_BACK_D       (40)
#define SUB_BACK_BORDER  (2)

/*==============================================================================
 * 返回
 *============================================================================*/
void ui_subpage_back_to_menu(void)
{
    ESP_LOGI(TAG, "Back to MAIN MENU from sub-page");
    ui_menu_show();
}

/** 统一走"该子页自己的返回行为"（没设就是回主菜单） */
static void sub_do_back(ui_subpage_t *page)
{
    if (page != NULL && page->back_cb != NULL) {
        page->back_cb();
    } else {
        ui_subpage_back_to_menu();
    }
}

static void sub_back_cb(lv_event_t *e)
{
    sub_do_back((ui_subpage_t *)lv_event_get_user_data(e));
}

static void sub_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    if (indev == NULL) {
        indev = lv_indev_get_act();
    }
    if (indev == NULL) {
        return;
    }
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_LEFT) {
        return;   /* 只认"从右向左"（手指从右往左划 -> 向量 x 为负 -> LV_DIR_LEFT） */
    }
    sub_do_back((ui_subpage_t *)lv_event_get_user_data(e));
}

/** 给整棵子树铺 GESTURE_BUBBLE（屏幕自己不加，事件才会停在屏幕上） */
static void sub_gesture_bubble(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        /* ★ LV_OBJ_FLAG_USER_1 = "别让手势冒泡到屏幕"：横向滑条用它，
         *   否则在滑条上往左拖会被当成"返回"手势，手指还没松就跳页了。
         *   带这个标志的对象，手势会发给它自己（它不处理），不会传给屏幕。 */
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) {
            continue;
        }
        lv_obj_add_flag(c, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        sub_gesture_bubble(c);
    }
}

static void sub_decorative_recursive(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        sub_decorative_recursive(c);
    }
}

/*==============================================================================
 * 框架
 *============================================================================*/
void ui_subpage_create(ui_subpage_t *out, const char *title, const char *status)
{
    if (out == NULL || out->created || title == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(out->scr);
    ui_screen_set_bg(out->scr, UI_C_MENU_BG);
    lv_obj_set_style_pad_all(out->scr, 0, 0);
    lv_obj_clear_flag(out->scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- 顶栏：白底 0..57 + 1px 分隔线 --- */
    ui_panel_create(out->scr, 0, 0, UI_SCR_W, SUB_BAR_H, UI_C_BAR_BG);
    ui_panel_create(out->scr, 0, SUB_BAR_H, UI_SCR_W, 1, UI_C_MENU_BAR_LINE);

    /* --- 左上角圆形返回键（回调带 page 指针，才能按页决定返回到哪） --- */
    lv_obj_t *back = lv_obj_create(out->scr);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, SUB_BACK_X, SUB_BACK_Y);
    lv_obj_set_size(back, SUB_BACK_D, SUB_BACK_D);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, UI_C_CARD_BG, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back, SUB_BACK_BORDER, 0);
    lv_obj_set_style_border_color(back, UI_C_MENU_BLUE, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, sub_back_cb, LV_EVENT_CLICKED, out);

    /* ‹ 箭头：Lato 粗体字库只有 ASCII，符号字形用内置 Montserrat */
    lv_obj_t *arrow = lv_label_create(back);
    lv_obj_set_style_text_font(arrow, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(arrow, UI_C_MENU_BLUE, 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);

    /* --- 居中标题（文本框让开左右两侧，文字看起来仍在屏幕中央） --- */
    ui_label_create_bold(out->scr, 56, 0, 688, SUB_BAR_H,
                         UI_FONT_TITLE, UI_C_TEXT, LV_TEXT_ALIGN_CENTER, title);

    /* --- 右上角状态：用内置 Montserrat 小字，可安全显示 ° • --- */
    out->lbl_status = ui_label_create(out->scr, 580, 0, 204, SUB_BAR_H,
                                      UI_FONT_SMALL, UI_C_TEXT_DIM, LV_TEXT_ALIGN_RIGHT,
                                      status ? status : "");

    out->created = true;
}

void ui_subpage_set_back_cb(ui_subpage_t *page, ui_subpage_back_cb_t cb)
{
    if (page != NULL && page->created) {
        page->back_cb = cb;
    }
}

void ui_subpage_set_status(ui_subpage_t *page, const char *text, lv_color_t color)
{
    if (page == NULL || !page->created || page->lbl_status == NULL || text == NULL) {
        return;
    }
    /* 缓存比较：lv_label_set_text 是无条件 invalidate，每 200~500ms 刷一次会白重绘。
     * 缓存放在 page 里（不是全局），因为所有子页的定时器都在跑。 */
    if (strncmp(page->last_status, text, sizeof(page->last_status) - 1) != 0) {
        strncpy(page->last_status, text, sizeof(page->last_status) - 1);
        page->last_status[sizeof(page->last_status) - 1] = '\0';
        lv_label_set_text(page->lbl_status, text);
        lv_obj_set_style_text_color(page->lbl_status, color, 0);
    }
}

void ui_subpage_show(ui_subpage_t *page)
{
    if (page == NULL || !page->created) {
        return;
    }
    lv_scr_load(page->scr);
}

lv_obj_t *ui_subpage_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *card = ui_card_create(parent, x, y, w, h);
    lv_obj_set_style_radius(card, UI_MENU_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, UI_C_MENU_CARD_BORDER, 0);
    return card;
}

lv_obj_t *ui_subpage_dot(lv_obj_t *parent, int32_t x, int32_t y, int32_t d, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_subpage_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t radius, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void ui_subpage_set_text_cached(lv_obj_t *lbl, char *cache, size_t n, const char *text)
{
    if (lbl == NULL || cache == NULL || n == 0 || text == NULL) {
        return;
    }
    if (strncmp(cache, text, n - 1) == 0) {
        return;   /* 内容没变：连 invalidate 都不要做 */
    }
    strncpy(cache, text, n - 1);
    cache[n - 1] = '\0';
    ui_label_set_text_bold(lbl, text);
}

void ui_subpage_card_clickable(lv_obj_t *card, lv_event_cb_t cb, void *user_data)
{
    if (card == NULL) {
        return;
    }
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* 按下时底色略深：强光下戴手套也要能看出"点到了" */
    lv_obj_set_style_bg_color(card, UI_C_MENU_CARD_PRESSED, LV_STATE_PRESSED);
    if (cb != NULL) {
        lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);
    }
    sub_decorative_recursive(card);
}

void ui_subpage_finish(ui_subpage_t *page)
{
    if (page == NULL || !page->created) {
        return;
    }
    lv_obj_add_event_cb(page->scr, sub_gesture_cb, LV_EVENT_GESTURE, page);
    sub_gesture_bubble(page->scr);
}

/*==============================================================================
 * 圆环弧段（折线逼近）
 *============================================================================*/
/** 生成弧上的折线点，返回点数；点坐标相对"弧的外接正方形左上角" */
static uint32_t arc_build_points(ui_arc_t *arc, int32_t radius, int32_t thickness,
                                float start_deg, float sweep_deg)
{
    /* 折线段数：每 6 度一段，够平滑又省点 */
    uint32_t n = (uint32_t)(sweep_deg / 6.0f) + 2;
    if (n > UI_ARC_MAX_PTS) {
        n = UI_ARC_MAX_PTS;
    }
    if (n < 2) {
        n = 2;
    }

    for (uint32_t i = 0; i < n; i++) {
        const float deg = start_deg + sweep_deg * (float)i / (float)(n - 1);
        const float rad = deg * 3.14159265f / 180.0f;
        /* 屏幕 y 向下：cos/sin 直接算出来就是"顺时针为正" */
        arc->pts[i].x = (lv_coord_t)((float)(radius + thickness) + (float)radius * cosf(rad));
        arc->pts[i].y = (lv_coord_t)((float)(radius + thickness) + (float)radius * sinf(rad));
    }
    return n;
}

/** 把 sweep 规范到 (0, 360)：整圆留一个小缺口，避免首尾点重合 */
static float arc_clamp_sweep(float sweep_deg)
{
    if (sweep_deg <= 1.0f) {
        return 1.0f;
    }
    return (sweep_deg > 359.9f) ? 359.9f : sweep_deg;
}

void ui_subpage_arc(ui_arc_t *out, lv_obj_t *parent, int32_t cx, int32_t cy, int32_t radius,
                    int32_t thickness, float start_deg, float sweep_deg, lv_color_t color)
{
    if (out == NULL || parent == NULL || radius <= 0) {
        return;
    }
    sweep_deg = arc_clamp_sweep(sweep_deg);

    const int32_t box = 2 * (radius + thickness);
    const uint32_t n = arc_build_points(out, radius, thickness, start_deg, sweep_deg);

    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, cx - radius - thickness, cy - radius - thickness);
    lv_obj_set_size(line, box, box);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, out->pts, n);
    lv_obj_set_style_line_width(line, thickness, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);

    out->line = line;
    out->created = true;
}

void ui_subpage_arc_update(ui_arc_t *arc, int32_t cx, int32_t cy, int32_t radius,
                           int32_t thickness, float start_deg, float sweep_deg)
{
    if (arc == NULL || !arc->created || radius <= 0) {
        return;
    }
    sweep_deg = arc_clamp_sweep(sweep_deg);

    /* lv_line 只是保存点数组的指针（数组是调用方的），所以重新算一遍点、
     * 再把同一个数组交回去就行，不需要重建控件 */
    const uint32_t n = arc_build_points(arc, radius, thickness, start_deg, sweep_deg);
    lv_obj_set_pos(arc->line, cx - radius - thickness, cy - radius - thickness);
    lv_obj_set_size(arc->line, 2 * (radius + thickness), 2 * (radius + thickness));
    lv_line_set_points(arc->line, arc->pts, n);
}

/*==============================================================================
 * 横向滑条
 *============================================================================*/
#define SLIDER_TRACK_H     (8)
#define SLIDER_KNOB_D      (24)
#define SLIDER_TOUCH_PAD_Y (16)     /* 触摸区上下扩这么多，戴手套也好点 */

void ui_slider_set_value(ui_slider_t *slider, uint8_t value)
{
    if (slider == NULL || !slider->created || slider->fill == NULL || slider->knob == NULL) {
        return;
    }
    if (value > 100) {
        value = 100;
    }
    slider->value = value;
    if (value == slider->shown) {
        return;   /* 值没变就不动控件（lv_obj_set_style_* 会触发重绘） */
    }
    slider->shown = value;

    const int32_t filled = (slider->track_w * (int32_t)value) / 100;
    lv_obj_set_width(slider->fill, filled);
    lv_obj_set_x(slider->knob, slider->track_x + filled - slider->knob_d / 2);
}

static void slider_touch_cb(lv_event_t *e)
{
    ui_slider_t *s = (ui_slider_t *)lv_event_get_user_data(e);
    if (s == NULL || !s->created) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }

    /* 直接按触点的 x 换算数值：不走 LVGL 的滚动机制，也不依赖 lv_slider */
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t rel = p.x - s->track_x;
    if (rel < 0) {
        rel = 0;
    }
    if (rel > s->track_w) {
        rel = s->track_w;
    }
    const uint8_t v = (s->track_w > 0) ? (uint8_t)((rel * 100) / s->track_w) : 0;

    ui_slider_set_value(s, v);     /* 先更新视觉，跟手 */
    if (s->cb != NULL) {
        s->cb(v, s->cb_user);
    }
}

void ui_slider_create(ui_slider_t *out, lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
                      uint8_t value, ui_slider_cb_t cb, void *user_data)
{
    if (out == NULL || parent == NULL || w <= 0) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->track_x = x;
    out->track_w = w;
    out->knob_d = SLIDER_KNOB_D;
    out->cb = cb;
    out->cb_user = user_data;
    out->shown = 0xFF;   /* 强制第一次一定会画 */

    /* 触摸捕获区：透明、盖住整行（含上下留白），并且不参与手势冒泡 */
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, x - SLIDER_KNOB_D / 2, y - SLIDER_TOUCH_PAD_Y);
    lv_obj_set_size(root, w + SLIDER_KNOB_D, SLIDER_TRACK_H + 2 * SLIDER_TOUCH_PAD_Y);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, slider_touch_cb, LV_EVENT_PRESSED, out);
    lv_obj_add_event_cb(root, slider_touch_cb, LV_EVENT_PRESSING, out);
    out->root = root;

    /* 轨道（灰底） */
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, x, y);
    lv_obj_set_size(track, w, SLIDER_TRACK_H);
    lv_obj_set_style_radius(track, SLIDER_TRACK_H / 2, 0);
    lv_obj_set_style_bg_color(track, UI_C_MENU_TRACK, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 已填充部分（蓝） */
    lv_obj_t *fill = lv_obj_create(parent);
    lv_obj_remove_style_all(fill);
    lv_obj_set_pos(fill, x, y);
    lv_obj_set_size(fill, 0, SLIDER_TRACK_H);
    lv_obj_set_style_radius(fill, SLIDER_TRACK_H / 2, 0);
    lv_obj_set_style_bg_color(fill, UI_C_MENU_BLUE, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    out->fill = fill;

    /* 圆钮（白底 + 蓝圈） */
    lv_obj_t *knob = lv_obj_create(parent);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, SLIDER_KNOB_D, SLIDER_KNOB_D);
    lv_obj_set_y(knob, y + SLIDER_TRACK_H / 2 - SLIDER_KNOB_D / 2);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, UI_C_CARD_BG, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 3, 0);
    lv_obj_set_style_border_color(knob, UI_C_MENU_BLUE, 0);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    out->knob = knob;

    out->created = true;
    ui_slider_set_value(out, value);

    /* 触摸区提到最上层：它下面的轨道/圆钮都不接收点击，提到上面保证全区域可拖 */
    lv_obj_move_foreground(root);
}
