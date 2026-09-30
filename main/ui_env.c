/**
 * @file ui_env.c
 * @brief ENVIRONMENT 子界面实现（布局说明见 ui_env.h）
 *
 * 数据源：app_sensor（SHT30/31，每 2 秒采一次）。
 * 仪表环：用 ui_subpage 的折线圆环画，起始 135°、顺时针扫 270°（底部留缺口，
 *         与界面稿一致）；彩色段长度 = 数值 / 量程。
 *
 * ★ 量程与舒适度判据是本页的"业务常量"，见下面的宏 —— 换场合只改这里。
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "ui_env.h"
#include "ui_subpage.h"
#include "ui_theme.h"
#include "app_sensor.h"
#include "app_settings.h"
#include "app_text.h"

static const char *TAG = "ui_env";

#define E_TICK_MS           (500)
#define E_TEMP_MAX_C        (40.0f)    /* 温度仪表量程 0..40 °C（°F 显示时按 °C 比例） */
#define E_HUM_MAX           (100.0f)   /* 湿度量程 0..100 %RH */
#define E_COMFORT_T_MIN     (18.0f)    /* 舒适区间：18..26 °C */
#define E_COMFORT_T_MAX     (26.0f)
#define E_COMFORT_H_MIN     (40.0f)    /* 舒适区间：40..60 %RH */
#define E_COMFORT_H_MAX     (60.0f)

/* 布局（对界面稿 ui3_env.png 的像素测量值） */
#define E_RING_R            (86)
#define E_RING_T            (16)
#define E_CX_L              (210)
#define E_CX_R              (590)
#define E_CY                (200)
#define E_GAUGE_START       (135.0f)
#define E_GAUGE_SWEEP       (270.0f)

#define E_CAP_Y             (66)
#define E_VAL_Y             (156)
#define E_UNIT_Y            (214)
#define E_CARD_Y            (330)
#define E_CARD_H            (112)
#define E_CARD1_X           (18)
#define E_CARD1_W           (372)
#define E_CARD2_X           (407)
#define E_CARD2_W           (375)

static ui_subpage_t s_page;
static ui_arc_t     s_arc_temp_bg;    /* 温度：灰色底环（建完就不动） */
static ui_arc_t     s_arc_temp_val;   /* 温度：彩色值环（按值重画） */
static ui_arc_t     s_arc_hum_bg;     /* 湿度：灰色底环 */
static ui_arc_t     s_arc_hum_val;    /* 湿度：彩色值环 */
static lv_obj_t    *s_lbl_temp;
static lv_obj_t    *s_lbl_hum;
static lv_obj_t    *s_lbl_minmax;
static lv_obj_t    *s_lbl_comfort;
static lv_obj_t    *s_dot_comfort;
static lv_obj_t    *s_lbl_hint;
static lv_timer_t  *s_tick;

/* 缓存 */
static char  s_c_temp[16];
static char  s_c_hum[16];
static char  s_c_minmax[24];
static char  s_c_comfort[16];
static float s_ratio_temp_drawn = -1.0f;
static float s_ratio_hum_drawn = -1.0f;
static bool  s_hint_is_error_drawn;
static bool  s_hint_is_error_valid;

/*==============================================================================
 * 业务判据
 *============================================================================*/
static const char *env_comfort_text(float t_c, float h)
{
    if (t_c < E_COMFORT_T_MIN) {
        return APP_TXT_ENV_COLD;
    }
    if (t_c > E_COMFORT_T_MAX) {
        return APP_TXT_ENV_HOT;
    }
    if (h < E_COMFORT_H_MIN) {
        return APP_TXT_ENV_DRY;
    }
    if (h > E_COMFORT_H_MAX) {
        return APP_TXT_ENV_HUMID;
    }
    return APP_TXT_ENV_COMFORTABLE;
}

/** 彩色弧长 = 数值 / 量程（温度按摄氏算，这样切 °F 显示时环不会跳） */
static void env_set_ratio(ui_arc_t *arc, float *drawn, int32_t cx, float ratio)
{
    if (ratio < 0.0f) {
        ratio = 0.0f;
    }
    if (ratio > 1.0f) {
        ratio = 1.0f;
    }
    /* 变化小于 0.5% 就懒得重画（重画要重算 45 个点并 invalidate） */
    if (*drawn >= 0.0f && (ratio > *drawn ? ratio - *drawn : *drawn - ratio) < 0.005f) {
        return;
    }
    *drawn = ratio;
    ui_subpage_arc_update(arc, cx, E_CY, E_RING_R, E_RING_T,
                          E_GAUGE_START, E_GAUGE_SWEEP * ratio);
}

/*==============================================================================
 * 刷新
 *============================================================================*/
static void env_sync_visual(void)
{
    app_sensor_data_t d;
    const bool have = app_sensor_get(&d);
    static app_sensor_data_t last;      /* 拿不到锁时沿用上一次的数（避免闪 "--"） */
    if (have) {
        last = d;
    }
    const app_sensor_data_t *s = have ? &d : &last;
    const bool valid = s->valid && s->present;

    char buf[32];

    /* --- 温度：数值 + 单位后缀 --- */
    if (valid) {
        snprintf(buf, sizeof(buf), "%.1f", app_settings_to_display_temp(s->temp_c));
        env_set_ratio(&s_arc_temp_val, &s_ratio_temp_drawn, E_CX_L, s->temp_c / E_TEMP_MAX_C);
    } else {
        snprintf(buf, sizeof(buf), "%s", APP_TXT_DASH);
    }
    ui_subpage_set_text_cached(s_lbl_temp, s_c_temp, sizeof(s_c_temp), buf);

    /* --- 湿度 --- */
    if (valid) {
        snprintf(buf, sizeof(buf), "%.0f", s->hum_pct);
        env_set_ratio(&s_arc_hum_val, &s_ratio_hum_drawn, E_CX_R, s->hum_pct / E_HUM_MAX);
    } else {
        snprintf(buf, sizeof(buf), "%s", APP_TXT_DASH);
    }
    ui_subpage_set_text_cached(s_lbl_hum, s_c_hum, sizeof(s_c_hum), buf);

    /* --- 今日最值（单位跟随设置，后缀 °C 用一个单独小字 label 画） --- */
    if (valid && s->today_valid) {
        snprintf(buf, sizeof(buf), "%.1f / %.1f",
                 app_settings_to_display_temp(s->t_min), app_settings_to_display_temp(s->t_max));
    } else {
        snprintf(buf, sizeof(buf), "%s", APP_TXT_DASH);
    }
    ui_subpage_set_text_cached(s_lbl_minmax, s_c_minmax, sizeof(s_c_minmax), buf);

    /* --- 舒适度 --- */
    const char *comfort = valid ? env_comfort_text(s->temp_c, s->hum_pct) : APP_TXT_DASH;
    const lv_color_t comfort_color = (!valid) ? UI_C_TEXT_DIM
                                    : (strcmp(comfort, APP_TXT_ENV_COMFORTABLE) == 0
                                       ? UI_C_MENU_GREEN : UI_C_MENU_ORANGE);
    if (strncmp(s_c_comfort, comfort, sizeof(s_c_comfort) - 1) != 0) {
        strncpy(s_c_comfort, comfort, sizeof(s_c_comfort) - 1);
        s_c_comfort[sizeof(s_c_comfort) - 1] = '\0';
        lv_label_set_text(s_lbl_comfort, comfort);
        lv_obj_set_style_text_color(s_lbl_comfort, comfort_color, 0);
        lv_obj_set_style_bg_color(s_dot_comfort, comfort_color, 0);   /* 圆点跟着变 */
    }

    /* --- 右下角提示行：正常时给出舒适区间，传感器不在时给出排查提示 --- */
    const bool show_error = !s->present;
    if (!s_hint_is_error_valid || s_hint_is_error_drawn != show_error) {
        s_hint_is_error_valid = true;
        s_hint_is_error_drawn = show_error;
        lv_label_set_text(s_lbl_hint, show_error ? APP_TXT_ENV_NO_SENSOR : APP_TXT_ENV_HINT);
        lv_obj_set_style_text_color(s_lbl_hint, show_error ? UI_C_STOP : UI_C_TEXT_DIM, 0);
    }

    /* --- 顶栏状态 --- */
    if (show_error) {
        ui_subpage_set_status(&s_page, APP_TXT_ENV_NO_SENSOR_SHORT, UI_C_STOP);
    } else {
        snprintf(buf, sizeof(buf), APP_TXT_ENV_SENSOR_FMT, SENSOR_PERIOD_MS / 1000);
        ui_subpage_set_status(&s_page, buf, UI_C_TEXT_DIM);
    }
}

static void env_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!ui_env_is_shown()) {
        return;
    }
    env_sync_visual();
}

/*==============================================================================
 * 创建 / 显示
 *============================================================================*/
void ui_env_create(void)
{
    if (s_page.created) {
        return;
    }

    ui_subpage_create(&s_page, APP_TXT_ENV_TITLE, "");
    lv_obj_t *scr = s_page.scr;

    /* --- 左：温度（灰底环 + 彩色值环，两个独立句柄） --- */
    ui_label_create_bold(scr, E_CX_L - 150, E_CAP_Y, 300, 32,
                         UI_FONT_CAPTION, UI_C_MENU_ORANGE, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_ENV_TEMPERATURE);
    ui_subpage_arc(&s_arc_temp_bg, scr, E_CX_L, E_CY, E_RING_R, 14, E_GAUGE_START,
                   E_GAUGE_SWEEP, UI_C_MENU_TRACK);
    ui_subpage_arc(&s_arc_temp_val, scr, E_CX_L, E_CY, E_RING_R, E_RING_T, E_GAUGE_START,
                   1.0f, UI_C_MENU_ORANGE);   /* 先画一小段，第一次刷新就按值重画 */

    s_lbl_temp = ui_label_create_bold(scr, E_CX_L - 100, E_VAL_Y, 200, 56,
                                      UI_FONT_VALUE, UI_C_MENU_ORANGE, LV_TEXT_ALIGN_CENTER,
                                      APP_TXT_DASH);
    ui_label_create(scr, E_CX_L - 100, E_UNIT_Y, 200, 28,
                    UI_FONT_SMALL, UI_C_MENU_ORANGE, LV_TEXT_ALIGN_CENTER,
                    APP_TXT_UNIT_DEG_C);

    /* --- 右：湿度 --- */
    ui_label_create_bold(scr, E_CX_R - 150, E_CAP_Y, 300, 32,
                         UI_FONT_CAPTION, UI_C_MENU_BLUE, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_ENV_HUMIDITY);
    ui_subpage_arc(&s_arc_hum_bg, scr, E_CX_R, E_CY, E_RING_R, 14, E_GAUGE_START,
                   E_GAUGE_SWEEP, UI_C_MENU_TRACK);
    ui_subpage_arc(&s_arc_hum_val, scr, E_CX_R, E_CY, E_RING_R, E_RING_T, E_GAUGE_START,
                   1.0f, UI_C_MENU_BLUE);

    s_lbl_hum = ui_label_create_bold(scr, E_CX_R - 100, E_VAL_Y, 200, 56,
                                     UI_FONT_VALUE, UI_C_MENU_BLUE, LV_TEXT_ALIGN_CENTER,
                                     APP_TXT_DASH);
    ui_label_create(scr, E_CX_R - 100, E_UNIT_Y, 200, 28,
                    UI_FONT_SMALL, UI_C_MENU_BLUE, LV_TEXT_ALIGN_CENTER,
                    APP_TXT_ENV_UNIT_RH);

    /* --- 左下：今日最值 --- */
    lv_obj_t *c1 = ui_subpage_card(scr, E_CARD1_X, E_CARD_Y, E_CARD1_W, E_CARD_H);
    ui_label_create_bold(c1, 16, 12, 340, 30, UI_FONT_CAPTION, UI_C_TEXT_DIM,
                         LV_TEXT_ALIGN_LEFT, APP_TXT_ENV_TODAY);
    /* 数值右对齐到固定位置，单位后缀就能算准放在它右边 */
    s_lbl_minmax = ui_label_create_bold(c1, 16, 42, 240, 44, UI_FONT_ACTION,
                                        UI_C_MENU_ORANGE, LV_TEXT_ALIGN_RIGHT, APP_TXT_DASH);
    ui_label_create(c1, 258, 50, 40, 26, UI_FONT_SMALL, UI_C_MENU_ORANGE,
                    LV_TEXT_ALIGN_LEFT, APP_TXT_UNIT_DEG_C);

    /* --- 右下：舒适度 --- */
    lv_obj_t *c2 = ui_subpage_card(scr, E_CARD2_X, E_CARD_Y, E_CARD2_W, E_CARD_H);
    s_dot_comfort = ui_subpage_dot(c2, 20, 22, 16, UI_C_MENU_GREEN);
    ui_label_create_bold(c2, 46, 12, 90, 32, UI_FONT_CAPTION, UI_C_TEXT_DIM,
                         LV_TEXT_ALIGN_LEFT, APP_TXT_ENV_STATUS);
    s_lbl_comfort = ui_label_create_bold(c2, 136, 12, 220, 32, UI_FONT_CAPTION,
                                         UI_C_MENU_GREEN, LV_TEXT_ALIGN_LEFT, APP_TXT_DASH);
    s_lbl_hint = ui_label_create(c2, 20, 52, 340, 26, UI_FONT_SMALL, UI_C_TEXT_DIM,
                                 LV_TEXT_ALIGN_LEFT, APP_TXT_ENV_HINT);

    ui_subpage_finish(&s_page);
    s_tick = lv_timer_create(env_tick_cb, E_TICK_MS, NULL);
    env_sync_visual();

    ESP_LOGI(TAG, "ENVIRONMENT page created: gauges r=%d cx=%d/%d, temp range 0..%.0fC",
             E_RING_R, E_CX_L, E_CX_R, E_TEMP_MAX_C);
}

void ui_env_show(void)
{
    if (!s_page.created) {
        return;
    }
    ui_subpage_show(&s_page);
    env_sync_visual();
    ESP_LOGI(TAG, "ENVIRONMENT page shown");
}

bool ui_env_is_shown(void)
{
    return s_page.created && (lv_scr_act() == s_page.scr);
}
