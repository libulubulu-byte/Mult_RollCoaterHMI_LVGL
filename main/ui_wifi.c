/**
 * @file ui_wifi.c
 * @brief WiFi 配网页实现（说明见 ui_wifi.h）
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "ui_wifi.h"
#include "ui_subpage.h"
#include "ui_theme.h"
#include "app_wifi.h"
#include "app_text.h"

static const char *TAG = "ui_wifi";

#define W_TICK_MS           (500)
#define W_AP_ROWS           (APP_WIFI_MAX_AP)

/* --- 列表页布局 --- */
#define W_CUR_Y             (74)
#define W_CUR_H             (76)
#define W_SCAN_Y            (164)
#define W_SCAN_W            (240)
#define W_SCAN_H            (52)
#define W_LIST_Y            (228)
#define W_LIST_W            (764)
#define W_LIST_H            (232)
#define W_AP_ROW_H          (52)

/* --- 密码页布局 --- */
#define W_NET_Y             (66)
#define W_BOX_Y             (100)
#define W_BOX_H             (52)
#define W_KB_X0             (20)
#define W_KB_Y0             (152)
#define W_KB_KEY_W          (70)
#define W_KB_KEY_H          (44)
#define W_KB_GAP_X          (6)
#define W_KB_GAP_Y          (8)
#define W_KB_ROW(i)         (W_KB_Y0 + (i) * (W_KB_KEY_H + W_KB_GAP_Y))
#define W_BTN_Y             (W_KB_ROW(4))
#define W_BTN_H             (44)
#define W_KB_SLOTS          (10)                          /* 每行 10 个键位 */
#define W_KB_SLOT_W         (W_KB_KEY_W + W_KB_GAP_X)      /* 76 */
#define W_KB_X(slot)        (W_KB_X0 + (slot) * W_KB_SLOT_W)
#define W_KB_W(n)           ((n) * W_KB_KEY_W + ((n) - 1) * W_KB_GAP_X)

/* 可切换大小写的键：第 2、3 行各 10 个 + 第 4 行的 z..m、.（共 8 个）= 28 个。
 * ★ 这个数必须 >= 实际写入的下标数，否则越界（改键盘时先看 wifi_build_keyboard）。 */
#define W_SHIFT_KEYS        (28)

static ui_subpage_t s_list;
static ui_subpage_t s_pass;

/* --- 列表页控件 --- */
static lv_obj_t *s_lbl_cur;
static lv_obj_t *s_lbl_list_hint;
static lv_obj_t *s_btn_scan;
static lv_obj_t *s_ap_row[W_AP_ROWS];
static lv_obj_t *s_ap_ssid[W_AP_ROWS];
static lv_obj_t *s_ap_rssi[W_AP_ROWS];
static lv_obj_t *s_ap_list;
static bool      s_ap_row_used[W_AP_ROWS];
static lv_timer_t *s_tick;

/* --- 密码页控件 --- */
static lv_obj_t *s_lbl_net;
static lv_obj_t *s_lbl_count;
static lv_obj_t *s_box;          /* 密码显示框（满时边框变红） */
static lv_obj_t *s_lbl_pass;
static lv_obj_t *s_ltr_key[W_SHIFT_KEYS];
static char      s_ltr_lo[W_SHIFT_KEYS][2];
static char      s_ltr_hi[W_SHIFT_KEYS][2];

/* --- 状态 --- */
static char    s_ssid_sel[APP_WIFI_SSID_LEN];
static char    s_pass_buf[APP_WIFI_PASS_LEN];
static uint8_t s_pass_len = 0;
static bool    s_shift = false;
static bool    s_scan_done = false;
static char    s_c_cur[48];      /* 列表页"当前状态"缓存 */
static char    s_c_net[80];      /* 密码页"Network: xxx"缓存 */
static char    s_c_hint[40];     /* 列表页提示/扫描中缓存 */
static char    s_c_count[16];    /* 密码页 n/63 缓存 */
static char    s_c_scan_btn[16]; /* 扫描按钮文字缓存 */

/*==============================================================================
 * 密码缓冲区
 *============================================================================*/
static void pass_refresh_box(void)
{
    char buf[16];

    lv_label_set_text(s_lbl_pass, s_pass_buf);
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)s_pass_len, (unsigned)(APP_WIFI_PASS_LEN - 1));
    ui_subpage_set_text_cached(s_lbl_count, s_c_count, sizeof(s_c_count), buf);

    /* 缓冲区有内容/满了要有明确视觉判据（不然"按了没反应"无从判断） */
    const bool full = (s_pass_len >= APP_WIFI_PASS_LEN - 1);
    lv_obj_set_style_border_color(s_box, full ? UI_C_STOP
                                  : (s_pass_len > 0 ? UI_C_MENU_BLUE : UI_C_MENU_CARD_BORDER), 0);
    lv_obj_set_style_border_width(s_box, (full || s_pass_len > 0) ? 2 : 1, 0);
}

static void pass_push(char c)
{
    if (s_pass_len >= APP_WIFI_PASS_LEN - 1) {
        /* 静默丢弃是最难排查的现象：满了一定要打日志 */
        ESP_LOGW(TAG, "password buffer full (%u), key '%c' dropped", (unsigned)s_pass_len, c);
        return;
    }
    s_pass_buf[s_pass_len++] = c;
    s_pass_buf[s_pass_len] = '\0';
    pass_refresh_box();
}

static void pass_backspace(void)
{
    if (s_pass_len == 0) {
        return;
    }
    s_pass_buf[--s_pass_len] = '\0';
    pass_refresh_box();
}

/*==============================================================================
 * 密码页：键盘
 *============================================================================*/
static void key_char_cb(lv_event_t *e)
{
    const char c = (char)(uintptr_t)lv_event_get_user_data(e);
    pass_push(c);
}

static void key_shift_cb(lv_event_t *e)
{
    (void)e;
    s_shift = !s_shift;
    for (int i = 0; i < W_SHIFT_KEYS; i++) {
        if (s_ltr_key[i] != NULL) {
            ui_button_set_text(s_ltr_key[i], s_shift ? s_ltr_hi[i] : s_ltr_lo[i]);
        }
    }
    ESP_LOGD(TAG, "Shift %s", s_shift ? "on" : "off");
}

static void key_backspace_cb(lv_event_t *e)
{
    (void)e;
    pass_backspace();
}

static void key_space_cb(lv_event_t *e)
{
    (void)e;
    pass_push(' ');
}

static void key_connect_cb(lv_event_t *e)
{
    (void)e;
    if (s_ssid_sel[0] == '\0') {
        return;
    }
    ESP_LOGI(TAG, "Connecting to '%s' (password %u chars)", s_ssid_sel, (unsigned)s_pass_len);
    app_wifi_connect(s_ssid_sel, s_pass_buf);
    ui_subpage_show(&s_list);   /* 回列表页看连接状态 */
}

static void key_cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_subpage_show(&s_list);
}

/** 建一个普通按键（宽 w、显示 text） */
static lv_obj_t *wifi_key(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const char *text,
                          const lv_font_t *font, lv_event_cb_t cb, void *ud)
{
    return ui_button_create(parent, x, y, w, W_KB_KEY_H,
                            UI_C_KEY_BG, UI_C_TEXT, font, text, cb, ud);
}

static void wifi_build_keyboard(void)
{
    static const char *ROW2_LO = "qwertyuiop";
    static const char *ROW2_HI = "QWERTYUIOP";
    static const char *ROW3_LO = "asdfghjkl-";
    static const char *ROW3_HI = "ASDFGHJKL_";
    lv_obj_t *scr = s_pass.scr;

    /* --- 第 1 行：数字（不受 SHIFT 影响） --- */
    for (int i = 0; i < 10; i++) {
        char t[2] = { (char)('0' + i), '\0' };
        wifi_key(scr, W_KB_X(i), W_KB_ROW(0), W_KB_KEY_W, t, UI_FONT_CAPTION,
                 key_char_cb, (void *)(uintptr_t)(uint8_t)t[0]);
    }

    /* --- 第 2、3 行：可切换大小写的键 --- */
    int idx = 0;
    for (int row = 0; row < 2; row++) {
        const char *lo = (row == 0) ? ROW2_LO : ROW3_LO;
        const char *hi = (row == 0) ? ROW2_HI : ROW3_HI;
        for (int i = 0; i < 10; i++) {
            s_ltr_lo[idx][0] = lo[i];
            s_ltr_lo[idx][1] = '\0';
            s_ltr_hi[idx][0] = hi[i];
            s_ltr_hi[idx][1] = '\0';
            char t[2] = { lo[i], '\0' };
            s_ltr_key[idx] = wifi_key(scr, W_KB_X(i), W_KB_ROW(row + 1), W_KB_KEY_W, t,
                                      UI_FONT_CAPTION, key_char_cb,
                                      (void *)(uintptr_t)(uint8_t)lo[i]);
            idx++;
        }
    }

    /* --- 第 4 行：[⇧] z x c v b n m . [⌫] --- */
    const int32_t y4 = W_KB_ROW(3);
    ui_button_create(scr, W_KB_X(0), y4, W_KB_KEY_W, W_KB_KEY_H, UI_C_BTN_GREY, UI_C_TEXT,
                     UI_FONT_CAPTION, "SHIFT", key_shift_cb, NULL);
    static const char *R4_LO = "zxcvbnm.";
    static const char *R4_HI = "ZXCVBNM@";
    for (int i = 0; i < 8; i++) {
        s_ltr_lo[idx][0] = R4_LO[i];
        s_ltr_lo[idx][1] = '\0';
        s_ltr_hi[idx][0] = R4_HI[i];
        s_ltr_hi[idx][1] = '\0';
        char t[2] = { R4_LO[i], '\0' };
        s_ltr_key[idx] = wifi_key(scr, W_KB_X(1 + i), y4, W_KB_KEY_W, t, UI_FONT_CAPTION,
                                  key_char_cb, (void *)(uintptr_t)(uint8_t)R4_LO[i]);
        idx++;
    }
    /* ⌫ 是 LVGL 符号字形，必须用内置 Montserrat */
    ui_button_create(scr, W_KB_X(9), y4, W_KB_KEY_W, W_KB_KEY_H, UI_C_KEY_BACKSPACE,
                     UI_C_KEY_FG_LIGHT, UI_FONT_SMALL, LV_SYMBOL_BACKSPACE,
                     key_backspace_cb, NULL);

    /* --- 第 5 行：[ SPACE ] [ CONNECT ] [ CANCEL ] --- */
    ui_button_create(scr, W_KB_X(0), W_BTN_Y, W_KB_W(4), W_BTN_H, UI_C_KEY_BG, UI_C_TEXT,
                     UI_FONT_CAPTION, "SPACE", key_space_cb, NULL);
    ui_button_create(scr, W_KB_X(4), W_BTN_Y, W_KB_W(3), W_BTN_H, UI_C_KEY_ENTER,
                     UI_C_KEY_FG_LIGHT, UI_FONT_CAPTION, APP_TXT_WIFI_CONNECT,
                     key_connect_cb, NULL);
    ui_button_create(scr, W_KB_X(7), W_BTN_Y, W_KB_W(3), W_BTN_H, UI_C_STOP, UI_C_STOP_FG,
                     UI_FONT_CAPTION, APP_TXT_WIFI_CANCEL, key_cancel_cb, NULL);
}

/*==============================================================================
 * 列表页
 *============================================================================*/
static void wifi_back_to_list(void)
{
    if (s_list.created) {
        ui_subpage_show(&s_list);
    }
}

static void ap_row_cb(lv_event_t *e)
{
    const uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    app_wifi_ap_t aps[W_AP_ROWS];
    const uint32_t n = app_wifi_ap_list(aps, W_AP_ROWS);
    if (idx >= n) {
        return;
    }

    snprintf(s_ssid_sel, sizeof(s_ssid_sel), "%s", aps[idx].ssid);
    ESP_LOGI(TAG, "Selected AP '%s' (%d dBm, %s)", s_ssid_sel, aps[idx].rssi,
             aps[idx].secure ? "secured" : "open");

    /* 开放网络不用输密码，直接连 */
    if (!aps[idx].secure) {
        ESP_LOGI(TAG, "Open network -> connect without password");
        app_wifi_connect(s_ssid_sel, "");
        return;
    }

    /* 换网络时要清掉上一次输的密码，否则会拿旧密码去连 */
    s_pass_buf[0] = '\0';
    s_pass_len = 0;
    s_shift = false;
    for (int i = 0; i < W_SHIFT_KEYS; i++) {
        if (s_ltr_key[i] != NULL) {
            ui_button_set_text(s_ltr_key[i], s_ltr_lo[i]);
        }
    }
    pass_refresh_box();

    char buf[80];
    snprintf(buf, sizeof(buf), APP_TXT_WIFI_PICK_FMT, s_ssid_sel);
    ui_subpage_set_text_cached(s_lbl_net, s_c_net, sizeof(s_c_net), buf);
    ui_subpage_set_status(&s_pass, s_ssid_sel, UI_C_MENU_BLUE);
    ui_subpage_show(&s_pass);
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    if (app_wifi_scan_busy()) {
        return;
    }
    if (app_wifi_scan_start() == ESP_OK) {
        lv_obj_add_state(s_btn_scan, LV_STATE_DISABLED);
        lv_obj_clear_flag(s_lbl_list_hint, LV_OBJ_FLAG_HIDDEN);
        ui_subpage_set_text_cached(s_lbl_list_hint, s_c_hint, sizeof(s_c_hint),
                                   APP_TXT_WIFI_SCANNING);
        ESP_LOGI(TAG, "Scan requested");
    }
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    app_wifi_forget();
    ESP_LOGW(TAG, "WiFi credentials erased by user");
}

/** 按扫描结果刷新热点列表（行对象一次性建好，这里只改文字与显隐） */
static void wifi_refresh_ap_rows(void)
{
    app_wifi_ap_t aps[W_AP_ROWS];
    const uint32_t n = app_wifi_ap_list(aps, W_AP_ROWS);
    char buf[64];

    for (uint32_t i = 0; i < W_AP_ROWS; i++) {
        if (i < n && aps[i].ssid[0] != '\0') {
            /* SSID 本身已经是 NUL 结尾的，直接给 label，不必再拷一遍 */
            ui_label_set_text_bold(s_ap_ssid[i], aps[i].ssid);
            snprintf(buf, sizeof(buf), "%d", aps[i].rssi);
            ui_label_set_text_bold(s_ap_rssi[i], buf);
            if (!s_ap_row_used[i]) {
                s_ap_row_used[i] = true;
                lv_obj_clear_flag(s_ap_row[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else if (s_ap_row_used[i]) {
            s_ap_row_used[i] = false;
            lv_obj_add_flag(s_ap_row[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 一个热点都没扫到：把提示行露出来（否则它盖在列表上） */
    if (n == 0) {
        lv_obj_clear_flag(s_lbl_list_hint, LV_OBJ_FLAG_HIDDEN);
        ui_subpage_set_text_cached(s_lbl_list_hint, s_c_hint, sizeof(s_c_hint),
                                   APP_TXT_WIFI_NO_AP);
    } else {
        lv_obj_add_flag(s_lbl_list_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_sync_list(void)
{
    /* --- 当前状态 --- */
    const char *st = app_wifi_state_text();
    if (strncmp(s_c_cur, st, sizeof(s_c_cur) - 1) != 0) {
        strncpy(s_c_cur, st, sizeof(s_c_cur) - 1);
        s_c_cur[sizeof(s_c_cur) - 1] = '\0';
        lv_label_set_text(s_lbl_cur, st);
        lv_obj_set_style_text_color(s_lbl_cur,
                                    app_wifi_is_connected() ? UI_C_MENU_GREEN : UI_C_TEXT_DIM, 0);
    }
    ui_subpage_set_status(&s_list, st, UI_C_TEXT_DIM);

    /* --- 扫描按钮：扫过一次就变成 "SCAN AGAIN"，扫描期间禁用 --- */
    const bool busy = app_wifi_scan_busy();
    if (busy) {
        lv_obj_add_state(s_btn_scan, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_btn_scan, LV_STATE_DISABLED);
        const char *txt = s_scan_done ? APP_TXT_WIFI_RESCAN : APP_TXT_WIFI_SCAN;
        if (strncmp(s_c_scan_btn, txt, sizeof(s_c_scan_btn) - 1) != 0) {
            strncpy(s_c_scan_btn, txt, sizeof(s_c_scan_btn) - 1);
            s_c_scan_btn[sizeof(s_c_scan_btn) - 1] = '\0';
            ui_button_set_text(s_btn_scan, txt);
        }
    }
}

static void wifi_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 配网事件（扫描完成 / 连上 / 掉线）通过队列抛过来 */
    app_wifi_evt_t evt;
    while (app_wifi_poll_event(&evt)) {
        switch (evt.id) {
        case APP_WIFI_EVT_SCAN_DONE:
            ESP_LOGI(TAG, "Scan done event: %d APs", evt.value);
            s_scan_done = true;
            wifi_refresh_ap_rows();
            break;
        case APP_WIFI_EVT_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected: %s", app_wifi_ip());
            break;
        case APP_WIFI_EVT_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected (reason %d)", evt.value);
            break;
        case APP_WIFI_EVT_CONNECT_FAILED:
            ESP_LOGE(TAG, "WiFi connect failed (reason %d) - check password/router", evt.value);
            break;
        case APP_WIFI_EVT_TIME_SYNCED:
            ESP_LOGI(TAG, "Time synced -> main menu clock is now real time");
            break;
        default:
            break;
        }
    }

    if (ui_wifi_is_shown()) {
        wifi_sync_list();
    }
}

/*==============================================================================
 * 创建 / 显示
 *============================================================================*/
void ui_wifi_create(void)
{
    if (s_list.created) {
        return;
    }

    /* ================= 列表页 ================= */
    ui_subpage_create(&s_list, APP_TXT_WIFI_TITLE, "");
    lv_obj_t *scr = s_list.scr;

    lv_obj_t *cur = ui_subpage_card(scr, 18, W_CUR_Y, 764, W_CUR_H);
    ui_label_create_bold(cur, 16, 0, 200, W_CUR_H, UI_FONT_CAPTION, UI_C_TEXT,
                         LV_TEXT_ALIGN_LEFT, APP_TXT_SET_WIFI);
    s_lbl_cur = ui_label_create_bold(cur, 220, 0, 340, W_CUR_H, UI_FONT_CAPTION,
                                     UI_C_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "");
    ui_button_create(cur, 764 - 16 - 168, (W_CUR_H - 44) / 2, 168, 44,
                     UI_C_BTN_GREY, UI_C_TEXT, UI_FONT_CAPTION,
                     APP_TXT_WIFI_FORGET, forget_cb, NULL);

    s_btn_scan = ui_button_create(scr, 18, W_SCAN_Y, W_SCAN_W, W_SCAN_H,
                                  UI_C_BTN_GREEN, UI_C_KEY_FG_LIGHT, UI_FONT_CAPTION,
                                  APP_TXT_WIFI_SCAN, scan_cb, NULL);

    /* 可滚动列表：★ 标 USER_1 让手势框架跳过，否则 SCROLLABLE 会被清掉、列表滚不动 */
    s_ap_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ap_list);
    lv_obj_set_pos(s_ap_list, 18, W_LIST_Y);
    lv_obj_set_size(s_ap_list, W_LIST_W, W_LIST_H);
    lv_obj_set_style_radius(s_ap_list, UI_MENU_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(s_ap_list, UI_C_CARD_BG, 0);
    lv_obj_set_style_bg_opa(s_ap_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ap_list, 1, 0);
    lv_obj_set_style_border_color(s_ap_list, UI_C_MENU_CARD_BORDER, 0);
    lv_obj_set_style_pad_all(s_ap_list, 0, 0);
    lv_obj_set_scroll_dir(s_ap_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ap_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_USER_1);

    for (uint32_t i = 0; i < W_AP_ROWS; i++) {
        lv_obj_t *row = ui_panel_create(s_ap_list, 0, (int32_t)i * W_AP_ROW_H,
                                        W_LIST_W - 2, W_AP_ROW_H, UI_C_CARD_BG);
        ui_panel_create(s_ap_list, 0, (int32_t)i * W_AP_ROW_H + W_AP_ROW_H - 1,
                        W_LIST_W - 2, 1, UI_C_MENU_BAR_LINE);
        s_ap_ssid[i] = ui_label_create_bold(row, 16, 0, 500, W_AP_ROW_H, UI_FONT_CAPTION,
                                            UI_C_TEXT, LV_TEXT_ALIGN_LEFT, "");
        s_ap_rssi[i] = ui_label_create_bold(row, 540, 0, 190, W_AP_ROW_H, UI_FONT_CAPTION,
                                            UI_C_TEXT_MUTED, LV_TEXT_ALIGN_RIGHT, "");
        /* 整行可点：点了进密码页 */
        ui_subpage_card_clickable(row, ap_row_cb, (void *)(uintptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        s_ap_row[i] = row;
        s_ap_row_used[i] = false;
    }

    s_lbl_list_hint = ui_label_create(s_ap_list, 0, 20, W_LIST_W - 2, 40, UI_FONT_CAPTION,
                                      UI_C_TEXT_MUTED, LV_TEXT_ALIGN_CENTER, APP_TXT_WIFI_NO_AP);

    /* ================= 密码页 ================= */
    ui_subpage_create(&s_pass, APP_TXT_WIFI_PASSWORD, "");
    ui_subpage_set_back_cb(&s_pass, wifi_back_to_list);   /* 返回回列表，不回主菜单 */
    lv_obj_t *pscr = s_pass.scr;

    s_lbl_net = ui_label_create_bold(pscr, 20, W_NET_Y, 560, 28, UI_FONT_CAPTION,
                                     UI_C_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "");
    s_lbl_count = ui_label_create_bold(pscr, 600, W_NET_Y, 180, 28, UI_FONT_CAPTION,
                                       UI_C_TEXT_MUTED, LV_TEXT_ALIGN_RIGHT, "");

    s_box = ui_subpage_card(pscr, 20, W_BOX_Y, 760, W_BOX_H);
    s_lbl_pass = ui_label_create_bold(s_box, 12, 0, 736, W_BOX_H, UI_FONT_CAPTION,
                                      UI_C_TEXT, LV_TEXT_ALIGN_LEFT, "");

    wifi_build_keyboard();

    /* 两个页面都要铺手势（列表页可滚那块被 USER_1 跳过了） */
    ui_subpage_finish(&s_list);
    ui_subpage_finish(&s_pass);

    pass_refresh_box();
    s_tick = lv_timer_create(wifi_tick_cb, W_TICK_MS, NULL);

    ESP_LOGI(TAG, "WiFi pages created: list + password (keyboard %d rows)", 5);
}

void ui_wifi_show(void)
{
    if (!s_list.created) {
        return;
    }
    ui_subpage_show(&s_list);
    wifi_sync_list();
    if (app_wifi_scan_busy()) {
        lv_obj_add_state(s_btn_scan, LV_STATE_DISABLED);
    }
    ESP_LOGI(TAG, "WiFi list shown (state: %s)", app_wifi_state_text());
}

bool ui_wifi_is_shown(void)
{
    if (!s_list.created) {
        return false;
    }
    lv_obj_t *act = lv_scr_act();
    return (act == s_list.scr) || (act == s_pass.scr);
}
