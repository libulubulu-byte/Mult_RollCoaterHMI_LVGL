/**
 * @file ui_settings.c
 * @brief SETTINGS 子界面实现（说明见 ui_settings.h）
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "ui_settings.h"
#include "ui_subpage.h"
#include "ui_theme.h"
#include "ui_wifi.h"
#include "ui_popup.h"
#include "app_settings.h"
#include "app_wifi.h"
#include "app_text.h"
#include "bsp_display.h"

static const char *TAG = "ui_settings";

#define S_TICK_MS           (1000)   /* 自动熄屏判定的粒度 */

/* 息屏后要用"双击"唤醒：
 * ① 单击唤醒有个隐患 —— 那一下会**穿到屏幕下面的按钮上**，醒来时可能已经
 *    误触了一个键（比如 SETTINGS 里的 RESET）。所以息屏时铺一层全屏遮罩
 *    把所有触摸吃掉，只有双击才唤醒。
 * ② 工业现场戴手套点两下很自然，比"划一下"可靠。 */
#define S_WAKE_TAPS         (2)
#define S_WAKE_WINDOW_MS    (1200)   /* 两次点击间隔超过这个时间就重新计数 */
#define S_ROW_X             (18)
#define S_ROW_W             (764)
#define S_ROW_Y0            (74)
#define S_ROW_H             (70)
#define S_ROW_GAP           (8)

#define S_ROW(i)            (S_ROW_Y0 + (i) * (S_ROW_H + S_ROW_GAP))
#define S_LABEL_X           (16)
#define S_LABEL_W           (300)
#define S_CTRL_RIGHT        (S_ROW_W - 16)      /* 控件区右边界（卡片内） */

static ui_subpage_t s_page;
static ui_slider_t  s_slider;
static lv_obj_t    *s_lbl_wifi;
static lv_obj_t    *s_lbl_pct;
static lv_obj_t    *s_btn_c;
static lv_obj_t    *s_btn_f;
static lv_obj_t    *s_btn_autooff_time;
static lv_obj_t    *s_btn_autooff_sw;
static lv_timer_t  *s_tick;

/* 缓存 */
static char    s_c_wifi[48];
static char    s_c_pct[16];
static uint8_t s_pct_drawn = 0xFF;
static bool    s_screen_off = false;
static lv_obj_t *s_wake_overlay = NULL;   /* 息屏期间的触摸遮罩（挂在 lv_layer_top） */
static uint8_t  s_wake_taps = 0;
static uint32_t s_wake_last_tap_ms = 0;
static uint8_t s_unit_drawn = 0xFF;
static bool    s_autooff_en_drawn = false;
static bool    s_autooff_en_valid = false;
static char    s_c_autooff[12];

/*==============================================================================
 * 自动熄屏服务 + 双击唤醒
 *============================================================================*/
static void screen_wake(const char *why)
{
    s_screen_off = false;
    s_wake_taps = 0;
    if (s_wake_overlay != NULL) {
        lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    /* 显式按设置里的亮度点亮，不用 bsp_display_backlight(true) ——
     * 后者依赖 BSP 内部"记忆的亮度"，多一层间接就多一个踩坑点 */
    const uint8_t pct = app_settings_get().disp_brightness;
    bsp_display_set_backlight_percent(pct);
    ESP_LOGI(TAG, "Screen woken (%s) -> backlight %u%%", why, pct);
}

/**
 * @brief 息屏期间遮罩上的"按下"：第 2 下（间隔在窗口内）就唤醒
 * @note 统计的是 LV_EVENT_PRESSED 而不是 CLICKED：戴手套点击时手指常有位移，
 *       CLICKED 需要"按下与抬起落在同一对象且没被判成拖动"，而 PRESSED 一碰就到，
 *       更不容易出现"点两下没反应"。反正遮罩本身就吃掉了所有触摸，
 *       不会误触到下面的按钮。
 */
static void wake_overlay_cb(lv_event_t *e)
{
    (void)e;
    const uint32_t now = lv_tick_get();
    if (s_wake_taps == 0 || (now - s_wake_last_tap_ms) > (uint32_t)S_WAKE_WINDOW_MS) {
        s_wake_taps = 1;
    } else {
        s_wake_taps++;
    }
    s_wake_last_tap_ms = now;

    ESP_LOGD(TAG, "Screen off: wake tap %u/%u", (unsigned)s_wake_taps, (unsigned)S_WAKE_TAPS);
    if (s_wake_taps >= S_WAKE_TAPS) {
        screen_wake("double tap");
    }
}

static void screen_sleep(uint32_t idle_ms, uint16_t timeout_s)
{
    s_screen_off = true;
    s_wake_taps = 0;

    /* 铺一层全屏透明遮罩：吃掉所有触摸，避免"唤醒那一下"穿到界面按钮上。
     * 挂在 lv_layer_top()，所以盖住所有屏幕（也盖住弹窗，反正背光是灭的）。 */
    if (s_wake_overlay == NULL) {
        s_wake_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_wake_overlay);
        lv_obj_set_pos(s_wake_overlay, 0, 0);
        lv_obj_set_size(s_wake_overlay, UI_SCR_W, UI_SCR_H);
        lv_obj_set_style_bg_opa(s_wake_overlay, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_wake_overlay, wake_overlay_cb, LV_EVENT_PRESSED, NULL);
    }
    lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wake_overlay);

    bsp_display_set_backlight_percent(0);   /* 只熄灭，不改动设置里的亮度 */
    ESP_LOGI(TAG, "Auto screen off: idle %ums >= %us -> backlight OFF, double-tap to wake",
             (unsigned)idle_ms, (unsigned)timeout_s);
}

static void autooff_tick(void)
{
    const app_settings_t set = app_settings_get();

    /* 关掉自动熄屏就立刻恢复背光（别把用户留在黑屏上） */
    if (!set.auto_off_en) {
        if (s_screen_off) {
            screen_wake("auto screen off disabled");
        }
        return;
    }

    if (s_screen_off) {
        /* 兜底：万一遮罩没建起来（内存不足），至少还能靠"碰一下"唤醒 */
        if (s_wake_overlay == NULL && lv_disp_get_inactive_time(NULL) < 500u) {
            screen_wake("touch (overlay missing)");
        }
        return;
    }

    const uint32_t idle_ms = lv_disp_get_inactive_time(NULL);
    if (idle_ms >= (uint32_t)set.auto_off_s * 1000u) {
        screen_sleep(idle_ms, set.auto_off_s);
    }
}

/*==============================================================================
 * 各行的交互
 *============================================================================*/
static void settings_sync_visual(void);

static void brightness_cb(uint8_t value, void *user_data)
{
    (void)user_data;
    /* app_settings 内部把下限夹到 10%（0% 会看不见屏，当"熄屏"要交给自动熄屏） */
    app_settings_set_disp_brightness(value);
    bsp_display_set_backlight_percent(app_settings_get().disp_brightness);
    settings_sync_visual();
}

static void temp_unit_cb(lv_event_t *e)
{
    app_temp_unit_t unit = (app_temp_unit_t)(uintptr_t)lv_event_get_user_data(e);
    app_settings_set_temp_unit(unit);
    ESP_LOGI(TAG, "Temperature unit -> %s", app_settings_temp_unit_text());
}

static void autooff_time_cb(lv_event_t *e)
{
    (void)e;
    /* 点一下在候选时长里轮换：15s -> 30s -> 1min -> 2min -> 5min -> 15s */
    static const uint16_t choices[] = APP_AUTO_OFF_CHOICES;
    const app_settings_t set = app_settings_get();
    uint16_t next = choices[0];
    for (int i = 0; i < APP_AUTO_OFF_CHOICE_COUNT; i++) {
        if (set.auto_off_s == choices[i]) {
            next = choices[(i + 1) % APP_AUTO_OFF_CHOICE_COUNT];
            break;
        }
    }
    app_settings_set_auto_off(set.auto_off_en, next);
    ESP_LOGI(TAG, "Auto screen off time -> %us", (unsigned)next);
}

static void autooff_switch_cb(lv_event_t *e)
{
    (void)e;
    const app_settings_t set = app_settings_get();
    app_settings_set_auto_off(!set.auto_off_en, set.auto_off_s);
    ESP_LOGI(TAG, "Auto screen off -> %s", (!set.auto_off_en) ? "ON" : "OFF");
}

static void wifi_config_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_show();
}

static void factory_reset_yes(void *user_data)
{
    (void)user_data;
    ESP_LOGW(TAG, "Factory reset confirmed");
    /* 用弹窗提示（MOTOR TEST 那个提示行在当前界面看不见），1.2 秒后设备重启 */
    ui_popup_show_notice(APP_TXT_SET_FACTORY, APP_TXT_SET_RESETTING, APP_TXT_WIFI_CANCEL,
                         NULL, NULL, true);
    app_settings_factory_reset();
}

static void factory_reset_cb(lv_event_t *e)
{
    (void)e;
    /* 不可逆操作：一定要二次确认 */
    ui_popup_show_choice(APP_TXT_SET_FACTORY, APP_TXT_SET_RESET_CONFIRM,
                         APP_TXT_WIFI_CANCEL, NULL,
                         APP_TXT_SET_RESET, factory_reset_yes, NULL);
}

/*==============================================================================
 * 刷新
 *============================================================================*/
static void settings_sync_visual(void)
{
    const app_settings_t set = app_settings_get();
    char buf[48];

    /* --- WiFi 行：状态文字随时会变（连接中/已连接/失败） --- */
    const bool connected = app_wifi_is_connected();
    snprintf(buf, sizeof(buf), "%s", app_wifi_state_text());
    if (strncmp(s_c_wifi, buf, sizeof(s_c_wifi) - 1) != 0) {
        strncpy(s_c_wifi, buf, sizeof(s_c_wifi) - 1);
        s_c_wifi[sizeof(s_c_wifi) - 1] = '\0';
        lv_label_set_text(s_lbl_wifi, buf);
        lv_obj_set_style_text_color(s_lbl_wifi,
                                    connected ? UI_C_MENU_GREEN : UI_C_TEXT_DIM, 0);
    }

    /* --- 背光亮度 --- */
    if (set.disp_brightness != s_pct_drawn) {
        s_pct_drawn = set.disp_brightness;
        ui_slider_set_value(&s_slider, set.disp_brightness);
    }
    snprintf(buf, sizeof(buf), APP_TXT_PCT_FMT, (unsigned)set.disp_brightness);
    ui_subpage_set_text_cached(s_lbl_pct, s_c_pct, sizeof(s_c_pct), buf);

    /* --- 温度单位：选中的那个用蓝底白字 --- */
    if (set.temp_unit != s_unit_drawn) {
        s_unit_drawn = set.temp_unit;
        const bool is_c = (set.temp_unit == APP_TEMP_UNIT_C);
        lv_obj_set_style_bg_color(s_btn_c, is_c ? UI_C_MENU_BLUE : UI_C_BTN_GREY, 0);
        lv_obj_set_style_bg_color(s_btn_f, is_c ? UI_C_BTN_GREY : UI_C_MENU_BLUE, 0);
        lv_obj_set_style_text_color(ui_button_get_label(s_btn_c),
                                    is_c ? UI_C_TEXT_ON_DARK : UI_C_TEXT, 0);
        lv_obj_set_style_text_color(ui_button_get_label(s_btn_f),
                                    is_c ? UI_C_TEXT : UI_C_TEXT_ON_DARK, 0);
    }

    /* --- 自动熄屏：时长 + 开关 --- */
    ui_subpage_set_text_cached(s_btn_autooff_time ? ui_button_get_label(s_btn_autooff_time) : NULL,
                               s_c_autooff, sizeof(s_c_autooff),
                               app_settings_auto_off_text(set.auto_off_s));
    if (!s_autooff_en_valid || s_autooff_en_drawn != (set.auto_off_en != 0)) {
        s_autooff_en_valid = true;
        s_autooff_en_drawn = (set.auto_off_en != 0);
        ui_button_set_text(s_btn_autooff_sw, set.auto_off_en ? "ON" : "OFF");
        lv_obj_set_style_bg_color(s_btn_autooff_sw,
                                  set.auto_off_en ? UI_C_MENU_GREEN : UI_C_BTN_GREY, 0);
        lv_obj_set_style_text_color(ui_button_get_label(s_btn_autooff_sw),
                                    set.auto_off_en ? UI_C_TEXT_ON_DARK : UI_C_TEXT, 0);
    }
}

static void settings_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    autooff_tick();   /* 自动熄屏要一直有效（不管当前在哪个界面） */

    if (s_screen_off || !ui_settings_is_shown()) {
        return;       /* 黑屏期间没必要刷界面 */
    }
    settings_sync_visual();
}

/*==============================================================================
 * 创建 / 显示
 *============================================================================*/
/** 建一行：返回行卡片，并在其中放好左侧标签 */
static lv_obj_t *settings_row(int index, const char *label)
{
    lv_obj_t *row = ui_subpage_card(s_page.scr, S_ROW_X, S_ROW(index), S_ROW_W, S_ROW_H);
    ui_label_create_bold(row, S_LABEL_X, 0, S_LABEL_W, S_ROW_H,
                         UI_FONT_CAPTION, UI_C_TEXT, LV_TEXT_ALIGN_LEFT, label);
    return row;
}

void ui_settings_create(void)
{
    if (s_page.created) {
        return;
    }
    ui_subpage_create(&s_page, APP_TXT_SET_TITLE, "");
    const app_settings_t set = app_settings_get();

    /* --- 行 1：WiFi --- */
    lv_obj_t *r1 = settings_row(0, APP_TXT_SET_WIFI);
    s_lbl_wifi = ui_label_create_bold(r1, 320, 0, 250, S_ROW_H, UI_FONT_CAPTION,
                                      UI_C_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "");
    ui_button_create(r1, S_CTRL_RIGHT - 168, (S_ROW_H - 44) / 2, 168, 44,
                     UI_C_BTN_GREEN, UI_C_KEY_FG_LIGHT, UI_FONT_CAPTION,
                     APP_TXT_SET_CONFIGURE, wifi_config_cb, NULL);

    /* --- 行 2：背光亮度 --- */
    lv_obj_t *r2 = settings_row(1, APP_TXT_SET_BRIGHTNESS);
    s_lbl_pct = ui_label_create_bold(r2, S_CTRL_RIGHT - 96, 0, 96, S_ROW_H,
                                     UI_FONT_CAPTION, UI_C_MENU_BLUE, LV_TEXT_ALIGN_RIGHT, "");
    ui_slider_create(&s_slider, r2, 320, S_ROW_H / 2 - 4, 300,
                     set.disp_brightness, brightness_cb, NULL);

    /* --- 行 3：温度单位（两段式按钮） --- */
    lv_obj_t *r3 = settings_row(2, APP_TXT_SET_TEMP_UNIT);
    s_btn_c = ui_button_create(r3, S_CTRL_RIGHT - 244, (S_ROW_H - 44) / 2, 110, 44,
                               UI_C_MENU_BLUE, UI_C_TEXT_ON_DARK, UI_FONT_SMALL,
                               APP_TXT_UNIT_DEG_C, temp_unit_cb, (void *)(uintptr_t)APP_TEMP_UNIT_C);
    s_btn_f = ui_button_create(r3, S_CTRL_RIGHT - 124, (S_ROW_H - 44) / 2, 110, 44,
                               UI_C_BTN_GREY, UI_C_TEXT, UI_FONT_SMALL,
                               "\xC2\xB0" "F", temp_unit_cb, (void *)(uintptr_t)APP_TEMP_UNIT_F);

    /* --- 行 4：自动熄屏（时长轮换 + 开关） --- */
    lv_obj_t *r4 = settings_row(3, APP_TXT_SET_AUTO_OFF);
    s_btn_autooff_time = ui_button_create(r4, S_CTRL_RIGHT - 244, (S_ROW_H - 44) / 2, 110, 44,
                                          UI_C_BTN_GREY, UI_C_TEXT, UI_FONT_CAPTION,
                                          app_settings_auto_off_text(set.auto_off_s),
                                          autooff_time_cb, NULL);
    s_btn_autooff_sw = ui_button_create(r4, S_CTRL_RIGHT - 124, (S_ROW_H - 44) / 2, 110, 44,
                                        set.auto_off_en ? UI_C_MENU_GREEN : UI_C_BTN_GREY,
                                        set.auto_off_en ? UI_C_TEXT_ON_DARK : UI_C_TEXT,
                                        UI_FONT_CAPTION, set.auto_off_en ? "ON" : "OFF",
                                        autooff_switch_cb, NULL);

    /* --- 行 5：恢复出厂 --- */
    lv_obj_t *r5 = settings_row(4, APP_TXT_SET_FACTORY);
    ui_button_create(r5, S_CTRL_RIGHT - 168, (S_ROW_H - 44) / 2, 168, 44,
                     UI_C_STOP, UI_C_STOP_FG, UI_FONT_CAPTION,
                     APP_TXT_SET_RESET, factory_reset_cb, NULL);

    ui_subpage_finish(&s_page);
    s_tick = lv_timer_create(settings_tick_cb, S_TICK_MS, NULL);
    settings_sync_visual();

    ESP_LOGI(TAG, "SETTINGS page created: 5 rows (y=%d..%d, row %dx%d)",
             S_ROW(0), S_ROW(4) + S_ROW_H, S_ROW_W, S_ROW_H);
}

void ui_settings_show(void)
{
    if (!s_page.created) {
        return;
    }
    ui_subpage_show(&s_page);
    settings_sync_visual();
    ESP_LOGI(TAG, "SETTINGS page shown (WiFi: %s)", app_wifi_state_text());
}

bool ui_settings_is_shown(void)
{
    return s_page.created && (lv_scr_act() == s_page.scr);
}
