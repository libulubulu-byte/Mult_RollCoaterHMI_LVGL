/**
 * @file ui_history.c
 * @brief HISTORY 子界面实现（说明见 ui_history.h）
 *
 * 曲线用 lv_line 直接画：采样点数组是静态的（288 点 x 2 条 = 2.3KB），
 * 只在历史点数变化时才重算点位（每 5 分钟一次），平时不重绘。
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "ui_history.h"
#include "ui_subpage.h"
#include "ui_theme.h"
#include "app_sensor.h"
#include "app_settings.h"
#include "app_text.h"

static const char *TAG = "ui_history";

/* 历史数据 5 分钟一个点、改动很慢，5 秒看一次足够（点数不变时不重绘） */
#define H_TICK_MS           (5000)
#define H_USE_FIXED_HUM     (0)     /* 1 = 湿度固定按 0..100 归一化 */

/* 布局（对界面稿 ui4_history.png 的像素测量值） */
#define H_CARD_X            (18)
#define H_CARD_Y            (74)
#define H_CARD_W            (764)
#define H_CARD_H            (276)
#define H_PLOT_X            (44)    /* 卡片内 */
#define H_PLOT_Y            (44)
#define H_PLOT_W            (676)
#define H_PLOT_H            (170)
#define H_XLABEL_Y          (226)   /* 卡片内，x 轴刻度 */
#define H_BOT_Y             (362)
#define H_BOT_H             (84)
#define H_BOT1_W            (372)
#define H_BOT2_X            (407)
#define H_BOT2_W            (375)

/* x 轴刻度（24h 表示成 00..24 小时） */
static const char *const H_X_LABELS[7] = { "00", "04", "08", "12", "16", "20", "24" };

static ui_subpage_t s_page;
static lv_obj_t    *s_card_chart;
static lv_obj_t    *s_plot;          /* 曲线容器（点坐标相对它） */
static lv_obj_t    *s_line_temp;
static lv_obj_t    *s_line_hum;
static lv_obj_t    *s_lbl_empty;     /* "Collecting data..." */
static lv_obj_t    *s_lbl_avg_t;
static lv_obj_t    *s_lbl_avg_h;
static lv_obj_t    *s_lbl_rng_t;
static lv_obj_t    *s_lbl_rng_h;
static lv_obj_t    *s_lbl_avg_cap;
static lv_timer_t  *s_tick;

/* 曲线点位（静态存储：lv_line 只保存指针） */
static lv_point_t s_pts_temp[SENSOR_HIST_POINTS];
static lv_point_t s_pts_hum[SENSOR_HIST_POINTS];
static sensor_sample_t s_samples[SENSOR_HIST_POINTS];
static uint32_t s_last_count = 0xFFFFFFFFu;   /* 上次画图时的点数 */
static bool     s_curves_visible = false;

/* 文本缓存 */
static char s_c_avg_t[16];
static char s_c_avg_h[16];
static char s_c_rng_t[24];
static char s_c_rng_h[24];
static char s_c_cap[40];

/*==============================================================================
 * 画曲线
 *============================================================================*/
/** 把一串数值按 [lo,hi] 归一化后铺到绘图区宽度上 */
static void build_points(lv_point_t *pts, const float *v, uint32_t n, float lo, float hi)
{
    if (hi - lo < 0.01f) {
        hi = lo + 1.0f;   /* 全平线：给个假跨度，画在中间 */
    }
    for (uint32_t i = 0; i < n; i++) {
        float r = (v[i] - lo) / (hi - lo);
        if (r < 0.0f) {
            r = 0.0f;
        }
        if (r > 1.0f) {
            r = 1.0f;
        }
        pts[i].x = (lv_coord_t)((int32_t)((H_PLOT_W - 1) * (int32_t)i / (int32_t)(n > 1 ? n - 1 : 1)));
        /* y 轴向下：数值大的画在上面 */
        pts[i].y = (lv_coord_t)((int32_t)((float)(H_PLOT_H - 1) * (1.0f - r)));
    }
}

static void history_rebuild_curves(void)
{
    const uint32_t n = app_sensor_history(s_samples, SENSOR_HIST_POINTS);
    if (n == s_last_count) {
        return;   /* 数据没变：不重算、不重绘（历史点 5 分钟才加一个） */
    }
    s_last_count = n;

    if (n < 2) {
        /* 点数不够画线：显示提示，曲线藏起来 */
        lv_obj_add_flag(s_line_temp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_line_hum, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
        s_curves_visible = false;
        return;
    }

    float t_lo = s_samples[0].temp_c, t_hi = s_samples[0].temp_c;
    float h_lo = s_samples[0].hum_pct, h_hi = s_samples[0].hum_pct;
    static float t_buf[SENSOR_HIST_POINTS];
    static float h_buf[SENSOR_HIST_POINTS];
    for (uint32_t i = 0; i < n; i++) {
        t_buf[i] = s_samples[i].temp_c;
        h_buf[i] = s_samples[i].hum_pct;
        if (t_buf[i] < t_lo) t_lo = t_buf[i];
        if (t_buf[i] > t_hi) t_hi = t_buf[i];
        if (h_buf[i] < h_lo) h_lo = h_buf[i];
        if (h_buf[i] > h_hi) h_hi = h_buf[i];
    }
    /* 上下留 10% 余量，曲线不贴边 */
    const float t_pad = (t_hi - t_lo) * 0.1f + 0.2f;
    t_lo -= t_pad;
    t_hi += t_pad;

    build_points(s_pts_temp, t_buf, n, t_lo, t_hi);
#if H_USE_FIXED_HUM
    build_points(s_pts_hum, h_buf, n, 0.0f, 100.0f);
#else
    const float h_pad = (h_hi - h_lo) * 0.1f + 1.0f;
    build_points(s_pts_hum, h_buf, n, h_lo - h_pad, h_hi + h_pad);
#endif

    lv_line_set_points(s_line_temp, s_pts_temp, n);
    lv_line_set_points(s_line_hum, s_pts_hum, n);
    lv_obj_clear_flag(s_line_temp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_line_hum, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    s_curves_visible = true;

    ESP_LOGI(TAG, "Curves rebuilt: %u points, temp %.1f..%.1f, hum %.1f..%.1f",
             (unsigned)n, t_lo, t_hi, h_lo, h_hi);
}

/*==============================================================================
 * 刷新文字
 *============================================================================*/
static void history_sync_text(void)
{
    app_sensor_data_t d;
    static app_sensor_data_t last;
    if (app_sensor_get(&d)) {
        last = d;
    }
    const app_sensor_data_t *s = &last;
    char buf[32];

    /* --- 平均 --- */
    if (s->hist_count >= 2 && s->today_valid) {
        snprintf(buf, sizeof(buf), "%.1f", app_settings_to_display_temp(s->t_avg24));
        ui_subpage_set_text_cached(s_lbl_avg_t, s_c_avg_t, sizeof(s_c_avg_t), buf);
        snprintf(buf, sizeof(buf), "%.0f", s->h_avg24);
        ui_subpage_set_text_cached(s_lbl_avg_h, s_c_avg_h, sizeof(s_c_avg_h), buf);
        ui_subpage_set_text_cached(s_lbl_avg_cap, s_c_cap, sizeof(s_c_cap), APP_TXT_HIST_AVERAGE);
    } else {
        ui_subpage_set_text_cached(s_lbl_avg_t, s_c_avg_t, sizeof(s_c_avg_t), APP_TXT_DASH);
        ui_subpage_set_text_cached(s_lbl_avg_h, s_c_avg_h, sizeof(s_c_avg_h), APP_TXT_DASH);
        /* 点数不够时把"还在攒数据"说清楚，并显示进度，避免被当成故障 */
        snprintf(buf, sizeof(buf), APP_TXT_HIST_NO_DATA);
        ui_subpage_set_text_cached(s_lbl_avg_cap, s_c_cap, sizeof(s_c_cap), buf);
    }

    /* --- 极值范围 --- */
    if (s->hist_count >= 2) {
        snprintf(buf, sizeof(buf), "%.1f - %.1f", app_settings_to_display_temp(s->t_min24),
                 app_settings_to_display_temp(s->t_max24));
        ui_subpage_set_text_cached(s_lbl_rng_t, s_c_rng_t, sizeof(s_c_rng_t), buf);
        snprintf(buf, sizeof(buf), "%.0f - %.0f", s->h_min24, s->h_max24);
        ui_subpage_set_text_cached(s_lbl_rng_h, s_c_rng_h, sizeof(s_c_rng_h), buf);
    } else {
        ui_subpage_set_text_cached(s_lbl_rng_t, s_c_rng_t, sizeof(s_c_rng_t), APP_TXT_DASH);
        ui_subpage_set_text_cached(s_lbl_rng_h, s_c_rng_h, sizeof(s_c_rng_h), APP_TXT_DASH);
    }

    /* --- 顶栏右上角：24H + 已攒点数（攒满 288 就不显示了） --- */
    if (s->hist_count >= SENSOR_HIST_POINTS) {
        ui_subpage_set_status(&s_page, APP_TXT_HIST_SPAN, UI_C_TEXT_DIM);
    } else {
        snprintf(buf, sizeof(buf), "%s %u/%u", APP_TXT_HIST_SPAN,
                 (unsigned)s->hist_count, (unsigned)SENSOR_HIST_POINTS);
        ui_subpage_set_status(&s_page, buf, UI_C_MENU_ORANGE);
    }
}

static void history_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!ui_history_is_shown()) {
        return;
    }
    history_rebuild_curves();
    history_sync_text();
}

/*==============================================================================
 * 创建 / 显示
 *============================================================================*/
void ui_history_create(void)
{
    if (s_page.created) {
        return;
    }

    ui_subpage_create(&s_page, APP_TXT_HIST_TITLE, APP_TXT_HIST_SPAN);
    lv_obj_t *scr = s_page.scr;

    /* --- 图表卡片 --- */
    s_card_chart = ui_subpage_card(scr, H_CARD_X, H_CARD_Y, H_CARD_W, H_CARD_H);

    /* 图例（右上角）：一小段彩线 + 名称，字体用内置小字（要显示 ° 等符号的话也够用） */
    ui_subpage_rect(s_card_chart, 536, 22, 26, 4, 2, UI_C_MENU_ORANGE);
    ui_label_create(s_card_chart, 570, 8, 70, 30, UI_FONT_SMALL, UI_C_TEXT_DIM,
                    LV_TEXT_ALIGN_LEFT, APP_TXT_HIST_LEGEND_TEMP);
    ui_subpage_rect(s_card_chart, 636, 22, 26, 4, 2, UI_C_MENU_BLUE);
    ui_label_create(s_card_chart, 670, 8, 70, 30, UI_FONT_SMALL, UI_C_TEXT_DIM,
                    LV_TEXT_ALIGN_LEFT, APP_TXT_HIST_LEGEND_HUM);

    /* 绘图区：白底 + 三条浅灰网格线 */
    s_plot = lv_obj_create(s_card_chart);
    lv_obj_remove_style_all(s_plot);
    lv_obj_set_pos(s_plot, H_PLOT_X, H_PLOT_Y);
    lv_obj_set_size(s_plot, H_PLOT_W, H_PLOT_H);
    lv_obj_clear_flag(s_plot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 1; i <= 3; i++) {
        ui_subpage_rect(s_plot, 0, (H_PLOT_H * i) / 4, H_PLOT_W, 1, 0, UI_C_MENU_BAR_LINE);
    }

    /* 两条曲线（点位在 UI 线程里算，见 history_rebuild_curves） */
    s_line_temp = lv_line_create(s_plot);
    lv_obj_remove_style_all(s_line_temp);
    lv_obj_clear_flag(s_line_temp, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(s_line_temp, 3, 0);
    lv_obj_set_style_line_color(s_line_temp, UI_C_MENU_ORANGE, 0);
    lv_obj_set_style_line_rounded(s_line_temp, true, 0);

    s_line_hum = lv_line_create(s_plot);
    lv_obj_remove_style_all(s_line_hum);
    lv_obj_clear_flag(s_line_hum, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(s_line_hum, 3, 0);
    lv_obj_set_style_line_color(s_line_hum, UI_C_MENU_BLUE, 0);
    lv_obj_set_style_line_rounded(s_line_hum, true, 0);

    /* 数据还没攒够时的占位提示 */
    s_lbl_empty = ui_label_create(s_card_chart, H_PLOT_X, H_PLOT_Y + H_PLOT_H / 2 - 20,
                                  H_PLOT_W, 40, UI_FONT_CAPTION, UI_C_TEXT_MUTED,
                                  LV_TEXT_ALIGN_CENTER, APP_TXT_HIST_NO_DATA);

    /* x 轴刻度 00 04 08 12 16 20 24 */
    for (int i = 0; i < 7; i++) {
        const int32_t x = H_PLOT_X + (H_PLOT_W - 60) * i / 6;
        ui_label_create(s_card_chart, x, H_XLABEL_Y, 60, 26, UI_FONT_SMALL, UI_C_TEXT_MUTED,
                        LV_TEXT_ALIGN_CENTER, H_X_LABELS[i]);
    }

    /* --- 左下：24h 平均 --- */
    lv_obj_t *c1 = ui_subpage_card(scr, H_CARD_X, H_BOT_Y, H_BOT1_W, H_BOT_H);
    s_lbl_avg_cap = ui_label_create_bold(c1, 16, 6, 340, 28, UI_FONT_CAPTION, UI_C_TEXT_DIM,
                                         LV_TEXT_ALIGN_LEFT, APP_TXT_HIST_AVERAGE);
    s_lbl_avg_t = ui_label_create_bold(c1, 16, 38, 110, 30, UI_FONT_CAPTION, UI_C_MENU_ORANGE,
                                       LV_TEXT_ALIGN_RIGHT, APP_TXT_DASH);
    ui_label_create(c1, 128, 45, 32, 20, UI_FONT_SMALL, UI_C_MENU_ORANGE,
                    LV_TEXT_ALIGN_LEFT, APP_TXT_UNIT_DEG_C);
    s_lbl_avg_h = ui_label_create_bold(c1, 200, 38, 60, 30, UI_FONT_CAPTION, UI_C_MENU_BLUE,
                                       LV_TEXT_ALIGN_RIGHT, APP_TXT_DASH);
    ui_label_create(c1, 262, 45, 60, 20, UI_FONT_SMALL, UI_C_MENU_BLUE,
                    LV_TEXT_ALIGN_LEFT, APP_TXT_ENV_UNIT_RH);

    /* --- 右下：窗口内极值 --- */
    lv_obj_t *c2 = ui_subpage_card(scr, H_BOT2_X, H_BOT_Y, H_BOT2_W, H_BOT_H);
    ui_label_create_bold(c2, 16, 8, 130, 28, UI_FONT_CAPTION, UI_C_TEXT_DIM,
                         LV_TEXT_ALIGN_LEFT, APP_TXT_HIST_RANGE_TEMP);
    s_lbl_rng_t = ui_label_create_bold(c2, 150, 8, 140, 28, UI_FONT_CAPTION, UI_C_MENU_ORANGE,
                                       LV_TEXT_ALIGN_RIGHT, APP_TXT_DASH);
    ui_label_create(c2, 292, 15, 40, 20, UI_FONT_SMALL, UI_C_MENU_ORANGE,
                    LV_TEXT_ALIGN_LEFT, APP_TXT_UNIT_DEG_C);
    ui_label_create_bold(c2, 16, 42, 130, 28, UI_FONT_CAPTION, UI_C_TEXT_DIM,
                         LV_TEXT_ALIGN_LEFT, APP_TXT_HIST_RANGE_HUM);
    s_lbl_rng_h = ui_label_create_bold(c2, 150, 42, 140, 28, UI_FONT_CAPTION, UI_C_MENU_BLUE,
                                       LV_TEXT_ALIGN_RIGHT, APP_TXT_DASH);
    ui_label_create(c2, 292, 49, 60, 20, UI_FONT_SMALL, UI_C_MENU_BLUE,
                    LV_TEXT_ALIGN_LEFT, APP_TXT_ENV_UNIT_RH);

    ui_subpage_finish(&s_page);
    s_tick = lv_timer_create(history_tick_cb, H_TICK_MS, NULL);
    history_rebuild_curves();
    history_sync_text();

    ESP_LOGI(TAG, "HISTORY page created: plot %dx%d, up to %d points per curve",
             H_PLOT_W, H_PLOT_H, SENSOR_HIST_POINTS);
}

void ui_history_show(void)
{
    if (!s_page.created) {
        return;
    }
    ui_subpage_show(&s_page);
    history_rebuild_curves();
    history_sync_text();
    ESP_LOGI(TAG, "HISTORY page shown (%u points collected)",
             (unsigned)((s_last_count == 0xFFFFFFFFu) ? 0 : s_last_count));
}

bool ui_history_is_shown(void)
{
    return s_page.created && (lv_scr_act() == s_page.scr);
}
