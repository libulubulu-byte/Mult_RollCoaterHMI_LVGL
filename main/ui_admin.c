/**
 * @file ui_admin.c
 * @brief Admin 参数屏实现（LVGL 8.3）
 *
 * 交互：
 *   - 行内「Edit」-> 全屏数字键盘（ui_numpad，与主屏同款键盘）
 *   - 校验失败 -> 在输入页的提示行显示原因，并允许继续修改
 *   - 「Exit and Accept」-> 异步写 NVS + 重新建立坐标系（回到 State1 要求重新标定）
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "ui_admin.h"
#include "ui_theme.h"
#include "ui_numpad.h"
#include "ui_main.h"
#include "app_config.h"
#include "app_state.h"
#include "app_text.h"

static const char *TAG = "ui_admin";

/* 布局常量（800x480） */
#define ADMIN_BAR_H         (UI_TOP_BAR_H)          /* 48 */
#define ADMIN_EXIT_W        (204)                   /* 顶部右侧绿色按钮 */
#define ADMIN_EXIT_H        (36)
#define ADMIN_EXIT_X        (800 - ADMIN_EXIT_W - 16)
#define ADMIN_EXIT_Y        (6)

#define ADMIN_LIST_X        (16)
#define ADMIN_LIST_Y        (64)
#define ADMIN_LIST_W        (768)
#define ADMIN_LIST_H        (404)

#define ADMIN_ROW_H         (56)                    /* 行高：戴手套可点 */
#define ADMIN_ROW_W         (750)

#define ADMIN_NAME_X        (16)
#define ADMIN_NAME_W        (300)
#define ADMIN_VALUE_X       (320)
#define ADMIN_VALUE_W       (250)
#define ADMIN_EDIT_W        (150)
#define ADMIN_EDIT_H        (44)
#define ADMIN_EDIT_X        (590)
#define ADMIN_EDIT_Y        (6)                     /* (56-44)/2 */

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_lbl_value[APP_CFG_FIELD_COUNT];
static app_config_t s_pending;                      /* 编辑副本，退出时才生效 */
static app_cfg_field_t s_edit_field = APP_CFG_UPPER_LIMIT;
static bool s_shown = false;

bool ui_admin_is_shown(void)
{
    return s_shown;
}

/*==============================================================================
 * 行刷新
 *============================================================================*/
static void admin_refresh_rows(void)
{
    char txt[16];

    for (app_cfg_field_t f = 0; f < APP_CFG_FIELD_COUNT; f++) {
        if (s_lbl_value[f] == NULL) {
            continue;
        }
        ui_fmt_value(txt, sizeof(txt), app_config_get_field(&s_pending, f));
        ui_label_set_text_bold(s_lbl_value[f], txt);
    }
}

/*==============================================================================
 * 数字键盘输入完成
 *============================================================================*/
static void admin_edit_done_cb(bool accepted, float value, void *user_data);

static void admin_edit_cb(lv_event_t *e)
{
    app_cfg_field_t field = (app_cfg_field_t)(uintptr_t)lv_event_get_user_data(e);
    s_edit_field = field;

    const char *hint = NULL;
    /* 限位类参数把当前允许区间显示出来，减少来回试错 */
    if (field == APP_CFG_UPPER_LIMIT || field == APP_CFG_LOWER_LIMIT ||
            field == APP_CFG_CURRENT_POSITION) {
        static char hint_buf[64];
        snprintf(hint_buf, sizeof(hint_buf), APP_TXT_ERR_RANGE_FMT,
                 s_pending.lower_lim, s_pending.upper_lim);
        hint = hint_buf;
    }

    ui_numpad_open(app_config_field_name(field), hint,
                   app_config_get_field(&s_pending, field),
                   admin_edit_done_cb, NULL);
}

static void admin_edit_done_cb(bool accepted, float value, void *user_data)
{
    (void)user_data;

    if (!accepted) {
        return;   /* 取消：保持原值 */
    }

    const char *err = app_config_validate(&s_pending, s_edit_field, value);
    if (err != NULL) {
        ESP_LOGW(TAG, "Reject %s = %.3f: %s", app_config_field_name(s_edit_field), value, err);
        /* 校验失败：把原因显示在输入页上，并让操作员继续改 */
        ui_numpad_open(app_config_field_name(s_edit_field), err, value,
                       admin_edit_done_cb, NULL);
        return;
    }

    app_config_set_field(&s_pending, s_edit_field, value);
    admin_refresh_rows();
    ESP_LOGI(TAG, "%s -> %.3f", app_config_field_name(s_edit_field), value);
}

/*==============================================================================
 * 退出并保存
 *============================================================================*/
static void admin_exit_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Exit and Accept: commit parameters to NVS");

    /* 异步落盘（UI 任务只入队，不写 flash） */
    app_config_commit_async(&s_pending);

    /* 参数已生效 -> 重新建立坐标系（当前位置需要重新标定） */
    app_state_exit_admin(true);

    ui_admin_hide();
    ui_main_show();
    ui_main_refresh();
}

/*==============================================================================
 * 创建
 *============================================================================*/
static void admin_create_row(lv_obj_t *list, app_cfg_field_t field, int32_t index)
{
    int32_t y = index * ADMIN_ROW_H;

    /* 行底板：白底 + 底部 1px 分隔线 */
    lv_obj_t *row = ui_panel_create(list, 0, y, ADMIN_ROW_W, ADMIN_ROW_H, UI_C_CARD_BG);
    ui_panel_create(list, 0, y + ADMIN_ROW_H - 1, ADMIN_ROW_W, 1, UI_C_CARD_BORDER);

    /* 参数名 x=16, w=300, 左对齐 */
    ui_label_create(row, ADMIN_NAME_X, 0, ADMIN_NAME_W, ADMIN_ROW_H,
                    UI_FONT_ROW, UI_C_TEXT, LV_TEXT_ALIGN_LEFT,
                    app_config_field_name(field));

    /* 当前值 x=320, w=250, 右对齐 */
    s_lbl_value[field] = ui_label_create_bold(row, ADMIN_VALUE_X, 0, ADMIN_VALUE_W, ADMIN_ROW_H,
                                              UI_FONT_ROW_VALUE, UI_C_VALUE_TARGET,
                                              LV_TEXT_ALIGN_RIGHT, "0.000");

    /* Edit 按钮 x=590, y=6, 150x44 */
    ui_button_create(row, ADMIN_EDIT_X, ADMIN_EDIT_Y, ADMIN_EDIT_W, ADMIN_EDIT_H,
                     UI_C_BTN_GREY, UI_C_BTN_GREY_FG, UI_FONT_ROW, APP_TXT_ADMIN_EDIT,
                     admin_edit_cb, (void *)(uintptr_t)field);
}

void ui_admin_create(void)
{
    if (s_scr != NULL) {
        return;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    ui_screen_set_bg(s_scr, UI_C_SCREEN_BG);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- 顶栏（与主屏同一套尺寸：白底 0..46 + 2px 分隔线） --- */
    ui_panel_create(s_scr, 0, 0, UI_SCR_W, UI_TOP_BAR_H, UI_C_BAR_BG);
    ui_panel_create(s_scr, 0, UI_TOP_BAR_LINE_Y, UI_SCR_W, UI_TOP_BAR_LINE_H, UI_C_BAR_LINE);
    ui_label_create_bold(s_scr, UI_PAD, 0, 480, UI_TOP_BAR_H, UI_FONT_TITLE, UI_C_TEXT,
                         LV_TEXT_ALIGN_LEFT, APP_TXT_ADMIN_TITLE);
    /* 右上角绿色「Exit and Accept」 */
    ui_button_create(s_scr, ADMIN_EXIT_X, ADMIN_EXIT_Y, ADMIN_EXIT_W, ADMIN_EXIT_H,
                     UI_C_BTN_GREEN, UI_C_KEY_FG_LIGHT, UI_FONT_CAPTION,
                     APP_TXT_ADMIN_EXIT, admin_exit_cb, NULL);

    /* --- 参数列表（可滚动容器，行高 56） --- */
    lv_obj_t *list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, ADMIN_LIST_X, ADMIN_LIST_Y);
    lv_obj_set_size(list, ADMIN_LIST_W, ADMIN_LIST_H);
    lv_obj_set_style_bg_color(list, UI_C_CARD_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, UI_C_CARD_BORDER, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (app_cfg_field_t f = 0; f < APP_CFG_FIELD_COUNT; f++) {
        admin_create_row(list, f, (int32_t)f);
    }

    s_pending = app_config_get();
    admin_refresh_rows();

    ESP_LOGI(TAG, "Admin screen created: %d rows, row height %d",
             (int)APP_CFG_FIELD_COUNT, ADMIN_ROW_H);
}

void ui_admin_show(void)
{
    if (s_scr == NULL) {
        return;
    }
    if (s_shown) {
        return;   /* 已经是当前屏，别把编辑中的副本冲掉 */
    }
    /* 每次进入都以"当前生效参数"为起点 */
    s_pending = app_config_get();
    admin_refresh_rows();

    lv_scr_load(s_scr);
    s_shown = true;
    ESP_LOGI(TAG, "Admin screen shown");
}

void ui_admin_hide(void)
{
    s_shown = false;
}
