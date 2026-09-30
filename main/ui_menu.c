/**
 * @file ui_menu.c
 * @brief 主菜单实现（LVGL 8.3）—— 按界面稿 ui1_main.png 还原
 *
 * 坐标来源：工程根目录的 ui1_main.png（800x480）解码后逐像素测量。
 *           卡片圆角/边框这类肉眼难判的按同组图（ui2_light.png / ui5_settings.png）
 *           统一取 12px 圆角 + #E3E7EC 细边。
 *
 * ★ 字体限制（改文案前必读）：
 *   本工程的 4 个粗体字库是 Lato Bold，只生成了 ASCII（0x20-0x7F）。
 *   界面稿里的「26.5°C」的度符号（U+00B0）和「ON · 80%」的间隔点都不在字库里，
 *   直接写会渲染成空白/方框。处理办法：
 *     - 度符号：拆成两个 label，「26.5」用 Lato Bold、后面的「°C」用内置
 *       Montserrat（它的字符集是 0x20-0x7F,0xB0,0x2022，见 lv_font_montserrat_16.c 头部注释）
 *     - 间隔点：不用字符，直接画一个 6x6 的实心圆点
 *   （⌫ / ↵ 也是同理，见 ui_theme.h 的 UI_FONT_SYMBOL）
 *
 * ★ 文本缓存：所有会被定时刷新的 label（时钟、底栏状态）都走 menu_set_text_cached()。
 *   lv_label_set_text() 内部是无条件 lv_obj_invalidate()，不做缓存的话静止画面
 *   每 500ms 就会被重绘一次 —— 既白烧带宽，又会在搬运那一刻和面板扫描撞上。
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_log.h"

#include "ui_menu.h"
#include "ui_theme.h"
#include "ui_main.h"      /* MOTOR TEST 子界面（原来的辊涂机主屏） */
#include "ui_light.h"
#include "ui_env.h"
#include "ui_history.h"
#include "ui_settings.h"
#include "app_state.h"
#include "app_light.h"
#include "app_sensor.h"
#include "app_settings.h"
#include "app_wifi.h"
#include "app_text.h"

static const char *TAG = "ui_menu";

/*==============================================================================
 * 几何（对界面稿 ui1_main.png 的像素测量值）
 *============================================================================*/
#define MENU_TICK_PERIOD_MS   (500)    /* 时钟/底栏刷新周期 */

/* 卡片区：三列 + 两列，两行都与左右各留 18px，合计 764 宽 */
#define MENU_MARGIN_X         (18)
#define MENU_ROW1_Y           (70)
#define MENU_ROW1_W           (246)
#define MENU_ROW1_H           (170)
#define MENU_GAP_X1           (13)     /* 3 列：3*246 + 2*13 = 764 */
#define MENU_ROW2_Y           (258)
#define MENU_ROW2_W           (375)
#define MENU_ROW2_H           (167)
#define MENU_GAP_X2           (14)     /* 2 列：2*375 + 1*14 = 764 */

/* 单列卡片内部（w=246）*/
#define MENU1_ICON_Y          (12)
#define MENU1_ICON_H          (88)
#define MENU1_SUB_Y           (114)
#define MENU1_SUB_H           (26)
#define MENU1_TITLE_Y         (142)
#define MENU1_TITLE_H         (26)

/* 宽卡片内部（w=375）*/
#define MENU2_BODY_Y          (18)
#define MENU2_TITLE_Y         (126)
#define MENU2_TITLE_H         (28)

/* 底部状态栏 */
#define MENU_FOOT_Y           (UI_MENU_FOOT_Y)

/* 提示条（"未实现"提示） */
#define MENU_TOAST_W          (460)
#define MENU_TOAST_H          (48)
#define MENU_TOAST_X          ((UI_SCR_W - MENU_TOAST_W) / 2)
#define MENU_TOAST_Y          (372)
#define MENU_TOAST_MS         (2200)

/* 历史曲线 */
#define MENU_HIST_POINTS      (24)
#define MENU_HIST_W           (320)
#define MENU_HIST_H           (86)

/* 风扇叶片（在 88x88 的图标容器内，圆心 (44,44)，半径 42）*/
static lv_point_t s_fan_h[]  = {{2, 44}, {86, 44}};
static lv_point_t s_fan_d1[] = {{23, 8}, {65, 80}};
static lv_point_t s_fan_d2[] = {{65, 8}, {23, 80}};

/* 历史曲线两条折线的点（创建时算一次，之后只读） */
static lv_point_t s_hist_a[MENU_HIST_POINTS];
static lv_point_t s_hist_b[MENU_HIST_POINTS];

/*==============================================================================
 * 控件句柄
 *============================================================================*/
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_lbl_clock = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_timer_t *s_tick_timer = NULL;

/* 卡片摘要（会随真实数据变化，所以要留着句柄做"值不变就不写"的刷新） */
static lv_obj_t *s_lbl_light_a = NULL;   /* 「ON」   */
static lv_obj_t *s_dot_light = NULL;     /* 中间那个圆点 */
static lv_obj_t *s_lbl_light_b = NULL;   /* 「80%」  */
static lv_obj_t *s_lbl_env_t = NULL;     /* 「26.5」 */
static lv_obj_t *s_lbl_env_h = NULL;     /* 「55%」  */
static lv_obj_t *s_bar[4] = { NULL };    /* 顶栏信号格（按 WiFi RSSI 亮几格） */

/* 文本/颜色缓存：内容没变就绝对不碰 LVGL（见文件头说明） */
static char s_cache_clock[8];
static char s_cache_status[24];
static lv_color_t s_status_color;
static bool s_status_color_valid = false;

/*==============================================================================
 * 菜单项
 *============================================================================*/
typedef enum {
    MENU_ITEM_LIGHT = 0,
    MENU_ITEM_ENVIRONMENT,
    MENU_ITEM_MOTOR_TEST,
    MENU_ITEM_HISTORY,
    MENU_ITEM_SETTINGS,
    MENU_ITEM_COUNT,
} menu_item_t;

static const char *menu_item_name(menu_item_t item)
{
    switch (item) {
    case MENU_ITEM_LIGHT:       return APP_TXT_MENU_LIGHT;
    case MENU_ITEM_ENVIRONMENT: return APP_TXT_MENU_ENVIRONMENT;
    case MENU_ITEM_MOTOR_TEST:  return APP_TXT_MOTOR_TEST;
    case MENU_ITEM_HISTORY:     return APP_TXT_MENU_HISTORY;
    case MENU_ITEM_SETTINGS:    return APP_TXT_MENU_SETTINGS;
    default:                    return "?";
    }
}

/*==============================================================================
 * 小工具
 *============================================================================*/
/** @brief 只在文字真的变化时才写 label */
static void menu_set_text_cached(lv_obj_t *lbl, char *cache, size_t n, const char *text)
{
    if (lbl == NULL || text == NULL || cache == NULL || n == 0) {
        return;
    }
    if (strncmp(cache, text, n - 1) == 0) {
        return;
    }
    strncpy(cache, text, n - 1);
    cache[n - 1] = '\0';
    ui_label_set_text_bold(lbl, text);
}

/** @brief 实心圆点（画间隔点等装饰用；不可点击，免得多吃了卡片的点击） */
static lv_obj_t *menu_dot(lv_obj_t *parent, int32_t x, int32_t y, int32_t d, lv_color_t color)
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

/** @brief 圆角实心矩形（信号格 / 滑条轨道等） */
static lv_obj_t *menu_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
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

/**
 * @brief 一段彩色圆弧（"C" 形），界面稿里 ENVIRONMENT 的两个仪表弧
 * @param open_right true = 保留左半圈（开口朝右），false = 保留右半圈（开口朝左）
 * @note LVGL 的角度约定：0° 在 3 点钟方向，顺时针为正。
 *       bg 弧 90°->270° 走的是 6 点 -> 9 点 -> 12 点，即左半圈。
 */
static lv_obj_t *menu_arc(lv_obj_t *parent, int32_t x, int32_t y, int32_t d,
                          lv_color_t color, bool open_right)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, d, d);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (open_right) {
        lv_arc_set_bg_angles(arc, 90, 270);
    } else {
        lv_arc_set_bg_angles(arc, 270, 90);
    }
    lv_arc_set_angles(arc, 0, 0);        /* 指示弧不用 */

    lv_obj_set_style_arc_width(arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    return arc;
}

/** @brief 折线（历史曲线 / 风扇叶片用；点坐标是相对本对象的） */
static lv_obj_t *menu_line(lv_obj_t *parent, lv_point_t *pts, uint32_t n,
                           int32_t w, int32_t h, lv_color_t color)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_size(line, w, h);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, pts, n);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

/**
 * @brief 把卡片里所有装饰性子孙设为"不接收点击"
 * @note lv_obj_create 出来的对象默认是 CLICKABLE 的，若不清掉，
 *       点在弧形/曲线/圆点上是触发不到卡片回调的（LVGL 的 CLICKED 不冒泡）。
 */
static void menu_decorative_recursive(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        menu_decorative_recursive(c);
    }
}

/*==============================================================================
 * 卡片摘要（实时数据；5 个入口现在都实现了，所以这里只剩"把状态显示出来"）
 *============================================================================*/
static void menu_refresh_cards(void)
{
    char buf[32];

    /* --- LIGHT：ON • 80%（跟 app_light 的真实状态一致） --- */
    {
        static bool     s_on_drawn;
        static uint8_t  s_pct_drawn;
        static bool     s_valid;
        const bool on = app_light_is_on();
        const uint8_t pct = app_light_get_brightness();
        if (!s_valid || s_on_drawn != on || s_pct_drawn != pct) {
            s_valid = true;
            s_on_drawn = on;
            s_pct_drawn = pct;
            const lv_color_t col = on ? UI_C_MENU_GREEN : UI_C_MENU_MUTED;
            ui_label_set_text_bold(s_lbl_light_a, on ? APP_TXT_LIGHT_ON : APP_TXT_LIGHT_OFF);
            snprintf(buf, sizeof(buf), APP_TXT_PCT_FMT, (unsigned)pct);
            ui_label_set_text_bold(s_lbl_light_b, buf);
            lv_obj_set_style_text_color(s_lbl_light_a, col, 0);
            lv_obj_set_style_text_color(s_lbl_light_b, col, 0);
            lv_obj_set_style_bg_color(s_dot_light, col, 0);
        }
    }

    /* --- ENVIRONMENT：26.5°C / 55%（读不到传感器就显示 --） --- */
    {
        static char s_c_t[16];
        static char s_c_h[16];
        app_sensor_data_t d;
        if (app_sensor_get(&d)) {
            const bool ok = d.valid && d.present;
            if (ok) {
                snprintf(buf, sizeof(buf), "%.1f", app_settings_to_display_temp(d.temp_c));
            } else {
                snprintf(buf, sizeof(buf), "%s", APP_TXT_DASH);
            }
            menu_set_text_cached(s_lbl_env_t, s_c_t, sizeof(s_c_t), buf);

            if (ok) {
                snprintf(buf, sizeof(buf), "%.0f%%", d.hum_pct);
            } else {
                snprintf(buf, sizeof(buf), "%s", APP_TXT_DASH);
            }
            menu_set_text_cached(s_lbl_env_h, s_c_h, sizeof(s_c_h), buf);
        }
    }

    /* --- 顶栏信号格：按 WiFi RSSI 亮几格（没连网全灰） --- */
    {
        static int s_bars_drawn = -1;
        int bars = 0;
        if (app_wifi_is_connected()) {
            const int8_t r = app_wifi_rssi();
            bars = (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : 1;
        }
        if (bars != s_bars_drawn) {
            s_bars_drawn = bars;
            for (int i = 0; i < 4; i++) {
                lv_obj_set_style_bg_color(s_bar[i],
                                          (i < bars) ? UI_C_MENU_BLUE : UI_C_MENU_MUTED, 0);
            }
        }
    }
}

/*==============================================================================
 * 卡片点击
 *============================================================================*/
static void menu_card_cb(lv_event_t *e)
{
    menu_item_t item = (menu_item_t)(uintptr_t)lv_event_get_user_data(e);

    switch (item) {
    case MENU_ITEM_MOTOR_TEST:
        /* MOTOR TEST = 辊涂机界面（状态机/键盘/STOP 全在那边） */
        ESP_LOGI(TAG, "%s tapped -> motor test screen", menu_item_name(item));
        ui_main_show();
        ui_main_refresh();
        break;

    case MENU_ITEM_LIGHT:
        ESP_LOGI(TAG, "%s tapped -> LIGHT CONTROL", menu_item_name(item));
        ui_light_show();
        break;

    case MENU_ITEM_ENVIRONMENT:
        ESP_LOGI(TAG, "%s tapped -> ENVIRONMENT", menu_item_name(item));
        ui_env_show();
        break;

    case MENU_ITEM_HISTORY:
        ESP_LOGI(TAG, "%s tapped -> HISTORY", menu_item_name(item));
        ui_history_show();
        break;

    case MENU_ITEM_SETTINGS:
        ESP_LOGI(TAG, "%s tapped -> SETTINGS", menu_item_name(item));
        ui_settings_show();
        break;

    default:
        break;
    }
}

/** @brief 把卡片变成可点击的入口，并把内部装饰设成不挡点击 */
static void menu_card_make_entry(lv_obj_t *card, menu_item_t item)
{
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* 按下时底色略微加深：戴手套在强光下也要能看出"点到了" */
    lv_obj_set_style_bg_color(card, UI_C_MENU_CARD_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, menu_card_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)item);
    menu_decorative_recursive(card);
}

/*==============================================================================
 * 卡片内容（按界面稿）
 *============================================================================*/
/** LIGHT：灯泡 + 「ON · 80%」+ LIGHT */
static void menu_fill_light(lv_obj_t *card)
{
    const int32_t w = MENU_ROW1_W;

    /* 灯泡：圆形泡体（浅黄底 + 橙色描边）+ 下方灯座 */
    lv_obj_t *bulb = menu_dot(card, (w - 80) / 2, MENU1_ICON_Y + 2, 80, UI_C_MENU_BULB);
    lv_obj_set_style_border_width(bulb, 3, 0);
    lv_obj_set_style_border_color(bulb, UI_C_MENU_ORANGE, 0);
    menu_rect(card, w / 2 - 16, MENU1_ICON_Y + 82, 32, 10, 3, UI_C_MENU_ORANGE);

    /* 「ON · 80%」：间隔点不是字符，是画出来的 6x6 圆点
     * （Lato 字库没有 U+00B7/U+2022，写字符会渲染成方框）
     * 这三个句柄要留着 —— 摘要会跟着真实的灯状态变（见 menu_refresh_cards） */
    s_lbl_light_a = ui_label_create_bold(card, 72, MENU1_SUB_Y, 36, MENU1_SUB_H,
                                         UI_FONT_CAPTION, UI_C_MENU_GREEN, LV_TEXT_ALIGN_RIGHT,
                                         APP_TXT_MENU_LIGHT_SUB_A);
    s_dot_light = menu_dot(card, 113, MENU1_SUB_Y + 10, 6, UI_C_MENU_GREEN);
    s_lbl_light_b = ui_label_create_bold(card, 122, MENU1_SUB_Y, 52, MENU1_SUB_H,
                                         UI_FONT_CAPTION, UI_C_MENU_GREEN, LV_TEXT_ALIGN_LEFT,
                                         APP_TXT_MENU_LIGHT_SUB_B);

    ui_label_create_bold(card, 0, MENU1_TITLE_Y, w, MENU1_TITLE_H,
                         UI_FONT_CAPTION, UI_C_TEXT, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_MENU_LIGHT);
}

/** ENVIRONMENT：两个仪表弧 + 「26.5°C」「55%」+ ENVIRONMENT */
static void menu_fill_environment(lv_obj_t *card)
{
    const int32_t w = MENU_ROW1_W;
    const int32_t arc_d = 76;
    const int32_t arc_y = MENU1_ICON_Y + 8;

    /* 左：橙色弧（开口朝右）；右：蓝色弧（开口朝左）—— 与界面稿一致 */
    menu_arc(card, 26, arc_y, arc_d, UI_C_MENU_ORANGE, true);
    menu_arc(card, w - 26 - arc_d, arc_y, arc_d, UI_C_MENU_BLUE, false);

    /* 「26.5°C」：度符号 Lato 没有，单独用内置 Montserrat 画一个 °C 后缀。
     * 数值句柄要留着 —— 它显示的是 SHT30 的真实读数（见 menu_refresh_cards）。 */
    s_lbl_env_t = ui_label_create_bold(card, 30, MENU1_SUB_Y, 44, MENU1_SUB_H,
                                       UI_FONT_CAPTION, UI_C_MENU_ORANGE, LV_TEXT_ALIGN_RIGHT,
                                       APP_TXT_MENU_ENV_TEMP);
    ui_label_create(card, 74, MENU1_SUB_Y + 7, 24, 20,
                    UI_FONT_SMALL, UI_C_MENU_ORANGE, LV_TEXT_ALIGN_LEFT,
                    APP_TXT_MENU_ENV_TEMP_UNIT);

    /* 「55%」居右弧下方 */
    s_lbl_env_h = ui_label_create_bold(card, w - 26 - arc_d, MENU1_SUB_Y, arc_d, MENU1_SUB_H,
                                       UI_FONT_CAPTION, UI_C_MENU_BLUE, LV_TEXT_ALIGN_CENTER,
                                       APP_TXT_MENU_ENV_HUM);

    ui_label_create_bold(card, 0, MENU1_TITLE_Y, w, MENU1_TITLE_H,
                         UI_FONT_CAPTION, UI_C_TEXT, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_MENU_ENVIRONMENT);
}

/** MOTOR TEST：叶轮图标 + READY + MOTOR TEST（本工程唯一可用的入口） */
static void menu_fill_motor(lv_obj_t *card)
{
    const int32_t w = MENU_ROW1_W;
    const int32_t d = MENU1_ICON_H;          /* 88x88 */

    lv_obj_t *icon = lv_obj_create(card);
    lv_obj_remove_style_all(icon);
    lv_obj_set_pos(icon, (w - d) / 2, MENU1_ICON_Y);
    lv_obj_set_size(icon, d, d);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 外圈 */
    lv_obj_t *ring = menu_dot(icon, 0, 0, d, UI_C_MENU_BLUE);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, UI_C_MENU_BLUE, 0);

    /* 三根过圆心的叶片 + 轮毂 */
    menu_line(icon, s_fan_h, 2, d, d, UI_C_MENU_BLUE);
    menu_line(icon, s_fan_d1, 2, d, d, UI_C_MENU_BLUE);
    menu_line(icon, s_fan_d2, 2, d, d, UI_C_MENU_BLUE);
    menu_dot(icon, d / 2 - 12, d / 2 - 12, 24, UI_C_MENU_BLUE);

    ui_label_create_bold(card, 0, MENU1_SUB_Y, w, MENU1_SUB_H,
                         UI_FONT_CAPTION, UI_C_MENU_MUTED, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_MENU_MOTOR_SUB);
    ui_label_create_bold(card, 0, MENU1_TITLE_Y, w, MENU1_TITLE_H,
                         UI_FONT_CAPTION, UI_C_TEXT, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_MOTOR_TEST);
}

/** HISTORY：图表卡片 + 两条趋势线 + HISTORY */
static void menu_fill_history(lv_obj_t *card)
{
    const int32_t w = MENU_ROW2_W;

    /* 图表底板（内嵌白底 + 细边） */
    lv_obj_t *plot = lv_obj_create(card);
    lv_obj_remove_style_all(plot);
    lv_obj_set_pos(plot, (w - MENU_HIST_W) / 2, MENU2_BODY_Y);
    lv_obj_set_size(plot, MENU_HIST_W, MENU_HIST_H);
    lv_obj_set_style_radius(plot, 6, 0);
    lv_obj_set_style_bg_color(plot, UI_C_CARD_BG, 0);
    lv_obj_set_style_bg_opa(plot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(plot, 1, 0);
    lv_obj_set_style_border_color(plot, UI_C_MENU_BAR_LINE, 0);
    lv_obj_set_style_pad_all(plot, 0, 0);
    lv_obj_clear_flag(plot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    menu_line(plot, s_hist_a, MENU_HIST_POINTS, MENU_HIST_W, MENU_HIST_H, UI_C_MENU_ORANGE);
    menu_line(plot, s_hist_b, MENU_HIST_POINTS, MENU_HIST_W, MENU_HIST_H, UI_C_MENU_BLUE);

    ui_label_create_bold(card, 0, MENU2_TITLE_Y, w, MENU2_TITLE_H,
                         UI_FONT_CAPTION, UI_C_TEXT, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_MENU_HISTORY);
}

/** SETTINGS：三条滑条 + SETTINGS */
static void menu_fill_settings(lv_obj_t *card)
{
    const int32_t w = MENU_ROW2_W;
    const int32_t track_w = 280;
    const int32_t track_x = (w - track_w) / 2;
    const int32_t knob_d = 18;
    /* 三条滑条的位置（比例与界面稿的 35% / 60% / 85% 对应） */
    static const int32_t row_y[3] = {26, 62, 98};
    static const float   ratio[3] = {0.35f, 0.60f, 0.85f};

    for (int i = 0; i < 3; i++) {
        int32_t y = MENU2_BODY_Y + row_y[i];
        int32_t filled = (int32_t)((float)track_w * ratio[i]);

        menu_rect(card, track_x, y, track_w, 6, 3, UI_C_MENU_TRACK);
        menu_rect(card, track_x, y, filled, 6, 3, UI_C_MENU_BLUE);

        /* 滑块：白底 + 蓝色描边 */
        lv_obj_t *knob = menu_dot(card, track_x + filled - knob_d / 2, y - (knob_d - 6) / 2,
                                  knob_d, UI_C_CARD_BG);
        lv_obj_set_style_border_width(knob, 3, 0);
        lv_obj_set_style_border_color(knob, UI_C_MENU_BLUE, 0);
    }

    ui_label_create_bold(card, 0, MENU2_TITLE_Y, w, MENU2_TITLE_H,
                         UI_FONT_CAPTION, UI_C_TEXT, LV_TEXT_ALIGN_CENTER,
                         APP_TXT_MENU_SETTINGS);
}

/*==============================================================================
 * 时钟 / 底栏状态（都会定时刷新，所以必须有缓存）
 *============================================================================*/
static void menu_refresh_clock(void)
{
    char txt[16];
    time_t now = time(NULL);
    struct tm tm_now;

    /* 本工程没有 RTC 也没有 SNTP，系统时间默认是 1970 —— 那种情况下显示 --:--
     * 而不是 08:00（1970 年 8 点），否则会让人以为时间是对的。 */
    if (now > 0 && localtime_r(&now, &tm_now) != NULL && (tm_now.tm_year + 1900) >= 2020) {
        snprintf(txt, sizeof(txt), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    } else {
        snprintf(txt, sizeof(txt), "%s", APP_TXT_MENU_CLOCK_UNSET);
    }
    menu_set_text_cached(s_lbl_clock, s_cache_clock, sizeof(s_cache_clock), txt);
}

static void menu_refresh_status(void)
{
    bool alarm = (app_state_get() == APP_STATE_ALARM);
    const char *txt = alarm ? APP_TXT_MENU_STATUS_ALARM : APP_TXT_MENU_STATUS_OK;

    menu_set_text_cached(s_lbl_status, s_cache_status, sizeof(s_cache_status), txt);

    lv_color_t color = alarm ? UI_C_STOP : UI_C_TEXT_DIM;
    if (!s_status_color_valid || s_status_color.full != color.full) {
        s_status_color = color;
        s_status_color_valid = true;
        lv_obj_set_style_text_color(s_lbl_status, color, 0);
    }
}

static void menu_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    menu_refresh_clock();
    menu_refresh_status();
    /* 卡片摘要只在主菜单可见时刷（定时器是全局的，别的屏在前台时没必要动） */
    if (ui_menu_is_shown()) {
        menu_refresh_cards();
    }
}

/*==============================================================================
 * 创建
 *============================================================================*/
static void menu_create_topbar(void)
{
    /* 顶栏：白底 0..57 + 1px 分隔线 */
    ui_panel_create(s_scr, 0, 0, UI_SCR_W, UI_MENU_BAR_H, UI_C_BAR_BG);
    ui_panel_create(s_scr, 0, UI_MENU_BAR_H, UI_SCR_W, 1, UI_C_MENU_BAR_LINE);

    ui_label_create_bold(s_scr, UI_PAD, 0, 520, UI_MENU_BAR_H,
                         UI_FONT_TITLE, UI_C_TEXT, LV_TEXT_ALIGN_LEFT,
                         APP_TXT_MENU_TITLE);

    /* 信号格：**按 WiFi 真实 RSSI 亮几格**（没配网/没连上时全灰），
     * 4 格共底对齐在顶栏内 10px 处（与右侧时钟的视觉底对齐）。 */
    static const int32_t bar_h[4] = {8, 12, 16, 20};
    int32_t bar_x = 640;
    for (int i = 0; i < 4; i++) {
        const int32_t h = bar_h[i];
        s_bar[i] = menu_rect(s_scr, bar_x + i * 10, UI_MENU_BAR_H - 10 - h, 6, h, 1,
                             UI_C_MENU_MUTED);
    }

    /* 时钟：右对齐到 x=784 */
    s_lbl_clock = ui_label_create_bold(s_scr, 690, 0, 94, UI_MENU_BAR_H,
                                       UI_FONT_TITLE, UI_C_TEXT, LV_TEXT_ALIGN_RIGHT,
                                       APP_TXT_MENU_CLOCK_UNSET);
}

static void menu_create_footer(void)
{
    ui_panel_create(s_scr, 0, MENU_FOOT_Y, UI_SCR_W, 1, UI_C_MENU_BAR_LINE);

    s_lbl_status = ui_label_create_bold(s_scr, UI_PAD, MENU_FOOT_Y + 1, 300,
                                        UI_SCR_H - MENU_FOOT_Y - 1,
                                        UI_FONT_CAPTION, UI_C_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
                                        APP_TXT_MENU_STATUS_OK);

    ui_label_create_bold(s_scr, 480, MENU_FOOT_Y + 1, 304,
                         UI_SCR_H - MENU_FOOT_Y - 1,
                         UI_FONT_CAPTION, UI_C_TEXT_MUTED, LV_TEXT_ALIGN_RIGHT,
                         APP_TXT_MENU_FW);
}

/** 历史曲线的形状（稳定的缩略图：菜单卡片上只是装饰，真实曲线在 HISTORY 页） */
static void menu_build_history_points(void)
{
    for (int i = 0; i < MENU_HIST_POINTS; i++) {
        int32_t x = 8 + i * 13;
        float t = (float)i * 0.42f;
        s_hist_a[i].x = x;
        s_hist_a[i].y = 43 + (int32_t)(20.0f * sinf(t + 0.4f) + 6.0f * sinf(t * 2.3f));
        s_hist_b[i].x = x;
        s_hist_b[i].y = 43 + (int32_t)(18.0f * sinf(t + 2.6f) + 5.0f * sinf(t * 1.7f + 1.0f));
    }
}

static lv_obj_t *menu_create_card(int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *card = ui_card_create(s_scr, x, y, w, h);
    lv_obj_set_style_radius(card, UI_MENU_CARD_RADIUS, 0);
    lv_obj_set_style_border_color(card, UI_C_MENU_CARD_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    return card;
}

void ui_menu_create(void)
{
    if (s_scr != NULL) {
        return;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    ui_screen_set_bg(s_scr, UI_C_MENU_BG);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    menu_build_history_points();
    menu_create_topbar();

    /* 第一行：3 张 246x170 的卡片 */
    const int32_t row1_x[3] = {
        MENU_MARGIN_X,
        MENU_MARGIN_X + MENU_ROW1_W + MENU_GAP_X1,
        MENU_MARGIN_X + 2 * (MENU_ROW1_W + MENU_GAP_X1),
    };
    lv_obj_t *c_light = menu_create_card(row1_x[0], MENU_ROW1_Y, MENU_ROW1_W, MENU_ROW1_H);
    lv_obj_t *c_env = menu_create_card(row1_x[1], MENU_ROW1_Y, MENU_ROW1_W, MENU_ROW1_H);
    lv_obj_t *c_motor = menu_create_card(row1_x[2], MENU_ROW1_Y, MENU_ROW1_W, MENU_ROW1_H);
    menu_fill_light(c_light);
    menu_fill_environment(c_env);
    menu_fill_motor(c_motor);

    /* 第二行：2 张 375x167 的卡片 */
    lv_obj_t *c_hist = menu_create_card(MENU_MARGIN_X, MENU_ROW2_Y, MENU_ROW2_W, MENU_ROW2_H);
    lv_obj_t *c_set = menu_create_card(MENU_MARGIN_X + MENU_ROW2_W + MENU_GAP_X2, MENU_ROW2_Y,
                                       MENU_ROW2_W, MENU_ROW2_H);
    menu_fill_history(c_hist);
    menu_fill_settings(c_set);

    /* 入口（装饰必须全部建完之后再设，否则后建的装饰会挡点击） */
    menu_card_make_entry(c_light, MENU_ITEM_LIGHT);
    menu_card_make_entry(c_env, MENU_ITEM_ENVIRONMENT);
    menu_card_make_entry(c_motor, MENU_ITEM_MOTOR_TEST);
    menu_card_make_entry(c_hist, MENU_ITEM_HISTORY);
    menu_card_make_entry(c_set, MENU_ITEM_SETTINGS);

    menu_create_footer();

    /* 时钟 + 底栏状态 + 卡片摘要的刷新定时器（一直在跑，切到别的屏也没关系：
     * 内容变了才写 LVGL，且非当前屏的 invalidate 不产生搬运） */
    s_tick_timer = lv_timer_create(menu_tick_cb, MENU_TICK_PERIOD_MS, NULL);

    menu_refresh_clock();
    menu_refresh_status();
    menu_refresh_cards();

    ESP_LOGI(TAG, "Main menu created: 5 cards (row1 3x %dx%d, row2 2x %dx%d), MOTOR TEST -> ui_main",
             MENU_ROW1_W, MENU_ROW1_H, MENU_ROW2_W, MENU_ROW2_H);
}

void ui_menu_show(void)
{
    if (s_scr == NULL) {
        return;
    }
    lv_scr_load(s_scr);
    menu_refresh_clock();
    menu_refresh_status();
    ESP_LOGI(TAG, "Main menu shown (swipe right->left on MOTOR TEST goes back here)");
}

bool ui_menu_is_shown(void)
{
    /* 直接问 LVGL 当前屏是谁，避免再维护一个可能和实际不同步的 bool */
    return (s_scr != NULL) && (lv_scr_act() == s_scr);
}
