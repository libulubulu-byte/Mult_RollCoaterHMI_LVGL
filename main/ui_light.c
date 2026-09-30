/**
 * @file ui_light.c
 * @brief LIGHT CONTROL 子界面实现（布局说明见 ui_light.h）
 *
 * 数据流：本页只跟 app_light 打交道，不直接碰 GPIO/LEDC；
 *         开关与亮度由 app_light 落到硬件 + 写 NVS。
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "ui_light.h"
#include "ui_subpage.h"
#include "ui_theme.h"
#include "app_light.h"
#include "app_text.h"

static const char *TAG = "ui_light";

/* 刷新周期：200ms。画面没有动画，靠缓存保证静止时零重绘 */
#define LIGHT_TICK_MS       (200)

/* 布局（对界面稿 ui2_light.png 的像素测量值） */
#define L_BULB_X            (104)
#define L_BULB_Y            (92)
#define L_BULB_D            (132)
#define L_COL_X             (40)
#define L_COL_W             (264)    /* 左列文本框：居中于灯泡下方 */
#define L_CH_Y              (254)
#define L_CH_H              (40)
#define L_STATUS_Y          (298)
#define L_STATUS_H          (36)
#define L_BTN_X             (540)
#define L_BTN_Y             (116)
#define L_BTN_D             (150)
#define L_ROW_Y             (392)
#define L_ROW_H             (44)
#define L_SLIDER_X          (252)
#define L_SLIDER_Y          (406)
#define L_SLIDER_W          (372)
#define L_PCT_X             (640)
#define L_PCT_W             (96)

static ui_subpage_t s_page;
static ui_slider_t  s_slider;
static lv_obj_t    *s_bulb;
static lv_obj_t    *s_lbl_status;
static lv_obj_t    *s_btn;
static lv_obj_t    *s_lbl_btn;
static lv_obj_t    *s_lbl_pct;
static lv_timer_t  *s_tick;

/* 文本/状态缓存（值不变就不写 LVGL，见 ui_subpage_set_text_cached 说明） */
static char    s_c_status[24];
static char    s_c_pct[16];
static bool    s_on_drawn;
static bool    s_on_valid;
static uint8_t s_pct_drawn;
static bool    s_pct_valid;

/*==============================================================================
 * 刷新
 *============================================================================*/
static void light_sync_visual(void)
{
    const bool on = app_light_is_on();
    const uint8_t pct = app_light_get_brightness();
    char buf[32];

    /* --- 只在开关状态真的变化时改样式/按钮文字（这些都会触发重绘） --- */
    if (!s_on_valid || s_on_drawn != on) {
        s_on_valid = true;
        s_on_drawn = on;

        /* 圆钮：开 = 绿，关 = 灰 */
        lv_obj_set_style_bg_color(s_btn, on ? UI_C_MENU_GREEN : UI_C_MENU_MUTED, 0);
        ui_label_set_text_bold(s_lbl_btn, on ? APP_TXT_LIGHT_ON : APP_TXT_LIGHT_OFF);

        /* 灯泡：开 = 亮黄底 + 橙圈；关 = 白底 + 灰圈（一眼能看出通没通电） */
        lv_obj_set_style_bg_color(s_bulb, on ? UI_C_MENU_BULB : UI_C_CARD_BG, 0);
        lv_obj_set_style_border_color(s_bulb, on ? UI_C_MENU_ORANGE : UI_C_MENU_MUTED, 0);

        lv_obj_set_style_text_color(s_lbl_status, on ? UI_C_MENU_GREEN : UI_C_TEXT_DIM, 0);
    }

    snprintf(buf, sizeof(buf), APP_TXT_LIGHT_STATUS_FMT, on ? APP_TXT_LIGHT_ON : APP_TXT_LIGHT_OFF);
    ui_subpage_set_text_cached(s_lbl_status, s_c_status, sizeof(s_c_status), buf);

    /* --- 亮度 --- */
    if (!s_pct_valid || s_pct_drawn != pct) {
        s_pct_valid = true;
        s_pct_drawn = pct;
        ui_slider_set_value(&s_slider, pct);
    }
    snprintf(buf, sizeof(buf), APP_TXT_PCT_FMT, (unsigned)pct);
    ui_subpage_set_text_cached(s_lbl_pct, s_c_pct, sizeof(s_c_pct), buf);

    /* --- 顶栏右上角：ON • 80% --- */
    snprintf(buf, sizeof(buf), APP_TXT_ON_PCT_FMT,
             on ? APP_TXT_LIGHT_ON : APP_TXT_LIGHT_OFF, (unsigned)pct);
    ui_subpage_set_status(&s_page, buf, on ? UI_C_MENU_GREEN : UI_C_TEXT_DIM);
}

/*==============================================================================
 * 交互
 *============================================================================*/
static void light_toggle_cb(lv_event_t *e)
{
    (void)e;
    app_light_toggle();
    ESP_LOGI(TAG, "Round button -> light %s", app_light_is_on() ? "ON" : "OFF");
    light_sync_visual();
}

static void light_brightness_cb(uint8_t value, void *user_data)
{
    (void)user_data;
    /* 拖动过程中连续触发：app_light 内部只做一次 ledc 更新 + 内存写 + 异步入队，
     * 都不阻塞（NVS 写由 set_save 任务做） */
    app_light_set_brightness(value);
    light_sync_visual();
}

static void light_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!ui_light_is_shown()) {
        return;   /* 别的页面在前台时不刷（定时器是全局的，所有页都在跑） */
    }
    light_sync_visual();
}

/*==============================================================================
 * 创建 / 显示
 *============================================================================*/
void ui_light_create(void)
{
    if (s_page.created) {
        return;
    }

    ui_subpage_create(&s_page, APP_TXT_LIGHT_TITLE, "");
    lv_obj_t *scr = s_page.scr;

    /* --- 灯泡：圆 + 灯座 --- */
    s_bulb = ui_subpage_dot(scr, L_BULB_X, L_BULB_Y, L_BULB_D, UI_C_MENU_BULB);
    lv_obj_set_style_border_width(s_bulb, 4, 0);
    lv_obj_set_style_border_color(s_bulb, UI_C_MENU_ORANGE, 0);
    ui_subpage_rect(scr, L_BULB_X + L_BULB_D / 2 - 17, L_BULB_Y + L_BULB_D - 4,
                    34, 12, 4, UI_C_MENU_ORANGE);

    /* --- 通道名 + 状态 --- */
    ui_label_create_bold(scr, L_COL_X, L_CH_Y, L_COL_W, L_CH_H,
                         UI_FONT_ACTION, UI_C_TEXT, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_LIGHT_CH1);
    s_lbl_status = ui_label_create_bold(scr, L_COL_X, L_STATUS_Y, L_COL_W, L_STATUS_H,
                                        UI_FONT_CAPTION, UI_C_MENU_GREEN, LV_TEXT_ALIGN_CENTER,
                                        "");

    /* --- 圆形开关（整块可点） --- */
    s_btn = ui_subpage_dot(scr, L_BTN_X, L_BTN_Y, L_BTN_D, UI_C_MENU_GREEN);
    lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_btn, light_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_btn = ui_label_create_bold(s_btn, 0, 0, L_BTN_D, L_BTN_D,
                                     UI_FONT_ACTION, UI_C_TEXT_ON_DARK, LV_TEXT_ALIGN_CENTER,
                                     APP_TXT_LIGHT_ON);

    /* --- 亮度行：标题 + 滑条 + 百分比 --- */
    ui_label_create_bold(scr, 40, L_ROW_Y, 200, L_ROW_H,
                         UI_FONT_CAPTION, UI_C_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
                         APP_TXT_LIGHT_BRIGHTNESS);
    s_lbl_pct = ui_label_create_bold(scr, L_PCT_X, L_ROW_Y, L_PCT_W, L_ROW_H,
                                     UI_FONT_CAPTION, UI_C_MENU_BLUE, LV_TEXT_ALIGN_RIGHT,
                                     "0%");
    ui_slider_create(&s_slider, scr, L_SLIDER_X, L_SLIDER_Y, L_SLIDER_W,
                     app_light_get_brightness(), light_brightness_cb, NULL);

    ui_subpage_finish(&s_page);   /* 铺手势冒泡（滑条那棵子树会被自动跳过） */
    s_tick = lv_timer_create(light_tick_cb, LIGHT_TICK_MS, NULL);
    light_sync_visual();

    ESP_LOGI(TAG, "LIGHT page created: bulb %dx%d, switch d=%d, slider %d..%d",
             L_BULB_D, L_BULB_D, L_BTN_D, L_SLIDER_X, L_SLIDER_X + L_SLIDER_W);
}

void ui_light_show(void)
{
    if (!s_page.created) {
        return;
    }
    ui_subpage_show(&s_page);
    light_sync_visual();
    ESP_LOGI(TAG, "LIGHT page shown (light %s, %u%%)",
             app_light_is_on() ? "ON" : "OFF", app_light_get_brightness());
}

bool ui_light_is_shown(void)
{
    return s_page.created && (lv_scr_act() == s_page.scr);
}
