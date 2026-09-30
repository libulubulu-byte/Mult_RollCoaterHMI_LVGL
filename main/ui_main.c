/**
 * @file ui_main.c
 * @brief MOTOR TEST 子界面（LVGL 8.3）—— 按参考界面稿 rollcoater_main_screen_v3.png 逐像素还原
 *
 * ★ 本屏现在是「主菜单（ui_menu.c，界面稿 ui1_main.png）-> MOTOR TEST 卡片」
 *   进去的子界面，不是上电第一屏。返回主菜单有两种方式（见 main_go_back）：
 *     ① 左上角圆形返回键   ② 在屏幕上从右向左滑动
 *
 * 坐标全部来自对界面稿的像素测量（见 ui_theme.h 与 main/README.md）：
 *   顶栏       y=0..46 白底 + y=47..48 两行 #CFCFCF 分隔线
 *   返回键     x=8 y=5 d=36 圆形（白底 + 2px 蓝边 + ‹）—— 子界面新增，稿里没有
 *   标题       x=56（让开返回键），大写高 16px -> 22 号字，色 #212121，文案 MOTOR TEST
 *   State      x=460 w=324 右对齐，22 号字，色 #616161
 *   卡片1      x=15 y=63  w=362 h=138 ；卡片2 y=215
 *     卡片标题 卡片内 y=5  h=44，20 号字，色 #616161，居中
 *     读数     卡片内 y=52 h=60，48 号字（见 README 说明为何不是 66 号）
 *   提示行     x=400 w=385 y=64 h=32，24 号字，居中（文字中心 x=592.5）
 *   键盘       x=400 y=104，键 120x64，列距 8 行距 8 -> 覆盖 y=104..383
 *   STOP       x=400 y=400 w=384 h=56，30 号字白字
 *
 * 线程模型：本文件所有函数都在 LVGL 任务里执行（由 esp_lvgl_port 定时器驱动），
 *           无需加锁，也绝不能有 >10ms 的阻塞调用。
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "ui_main.h"
#include "ui_menu.h"       /* MOTOR TEST 子界面的返回目标（主菜单） */
#include "ui_theme.h"
#include "ui_popup.h"
#include "ui_admin.h"
#include "app_state.h"
#include "app_config.h"
#include "app_motion.h"
#include "app_text.h"

static const char *TAG = "ui_main";

/* 轮询周期：50ms。读数与背景色都跟手，远低于"UI 任务不阻塞"的红线 */
#define MAIN_POLL_PERIOD_MS     (50)
/* Admin 长按检测周期：100ms（规范要求） */
#define MAIN_HOLD_PERIOD_MS     (100)
/* 提示行闪一条消息后恢复常态的时间 */
#define MAIN_FLASH_MS           (3000)

/* Enter 键在键盘数组里的下标（见 ui_numpad.c 的 s_key_layout） */
#define MAIN_KEY_ENTER_INDEX    (11)

/*------------------------------------------------------------------------------
 * 左上角圆形返回键（MOTOR TEST 子界面 -> 主菜单）
 * 样式与设计稿 ui2_light.png 的子页顶栏一致：白底圆 + 蓝描边 + ‹ 箭头
 *------------------------------------------------------------------------------*/
#define MAIN_BACK_X             (8)
#define MAIN_BACK_Y             (5)     /* 顶栏高 47，直径 36 -> (47-36)/2 = 5 */
#define MAIN_BACK_D             (36)
#define MAIN_BACK_BORDER_W      (2)

/*==============================================================================
 * 控件句柄（坐标见文件头注释）
 *============================================================================*/
static lv_obj_t *s_scr = NULL;          /* 主屏 800x480 */
static lv_obj_t *s_lbl_title = NULL;    /* 标题     x=16  y=0 w=420 h=47 */
static lv_obj_t *s_lbl_state = NULL;    /* State    x=460 y=0 w=324 h=47 右对齐 */
static lv_obj_t *s_card_cur = NULL;     /* 卡片1    x=15  y=63  w=362 h=138 */
static lv_obj_t *s_lbl_cur_cap = NULL;  /* 卡片内 y=5  h=44 */
static lv_obj_t *s_lbl_cur_val = NULL;  /* 卡片内 y=52 h=60 */
static lv_obj_t *s_card_tgt = NULL;     /* 卡片2    x=15  y=215 w=362 h=138 */
static lv_obj_t *s_lbl_tgt_cap = NULL;
static lv_obj_t *s_lbl_tgt_val = NULL;
static lv_obj_t *s_lbl_prompt = NULL;   /* 提示行   x=400 y=64 w=385 h=32 */
static lv_obj_t *s_btn_key[UI_KEY_COUNT];/* 键盘   起点 (400,104)，键 120x64 */
static lv_obj_t *s_btn_stop = NULL;     /* STOP     x=400 y=400 w=384 h=56 */

/*==============================================================================
 * 运行态
 *============================================================================*/
static ui_entry_t s_entry;
static lv_timer_t *s_poll_timer = NULL;
static lv_timer_t *s_hold_timer = NULL;
static bool s_shown = false;

static float s_live_pos = 0.0f;         /* 最近一次上报的实际位置 */
static bool  s_live_moving = false;     /* 是否正在运动 */
static lv_color_t s_bg_cached;          /* 背景色缓存：只在变化时才写样式 */
static bool  s_bg_cached_valid = false;

/* 提示行闪消息 */
static char s_flash_text[80];
static int64_t s_flash_until_ms = 0;
static bool s_flash_active = false;

/*------------------------------------------------------------------------------
 * 文本 / 样式缓存：内容没变就绝对不碰 LVGL
 *
 * ★ 为什么要这么做（现场踩过的"偶尔闪一下"）：
 *   lv_label_set_text() 内部是**无条件** lv_obj_invalidate()，即使文字一模一样；
 *   lv_obj_set_style_xxx() 也会刷新样式。而 motion 任务每 100ms 就会推一次位置，
 *   如果每来一条就直接写，空闲时也会变成每秒 ~10 次
 *     "同一内容重绘 -> 往面板帧缓冲搬运"
 *   既白烧 CPU/PSRAM 带宽，又会在搬运那一刻和面板扫描撞上 ->
 *   画面偶尔闪一下（局部横向错位）。
 *   加一层字符串/状态比较后，静置画面是完全不重绘的。
 *----------------------------------------------------------------------------*/
static char s_cache_cur_val[16];     /* 当前值卡片文字缓存 */
static char s_cache_tgt_val[16];     /* 目标值卡片文字缓存 */
static char s_cache_state[16];       /* 顶栏 State 文本缓存 */
static char s_cache_prompt[80];      /* 提示行文字缓存 */
static bool s_card_cur_active;       /* 当前值卡片是否已高亮 */
static bool s_card_tgt_active;       /* 目标值卡片是否已高亮 */
static bool s_cur_val_editing;       /* 当前值卡片是否处于"正在输入"配色 */
static bool s_tgt_val_editing;       /* 目标值卡片是否处于"正在输入"配色 */
static lv_color_t s_prompt_color;    /* 提示行当前颜色（避免重复设样式） */
static bool s_prompt_color_valid = false;

/* Admin 长按 Enter 检测 */
static bool s_enter_down = false;
static int64_t s_enter_down_us = 0;
static bool s_admin_consumed = false;

/*==============================================================================
 * 前向声明
 *============================================================================*/
static void main_refresh_cards(void);
static void main_refresh_prompt(void);
static void main_refresh_bg(void);
static void main_submit(void);
static void main_popup2_enter_cb(void *user_data);
static void main_go_back(const char *reason);

/**
 * @brief 只在文字真的变化时才写 label
 * @param cache 每个 label 一份的字符串缓存，长度必须 >= n
 */
static void main_set_text_cached(lv_obj_t *lbl, char *cache, size_t n, const char *text)
{
    if (lbl == NULL || text == NULL || cache == NULL || n == 0) {
        return;
    }
    if (strncmp(cache, text, n - 1) == 0) {
        return;   /* 内容没变：连 invalidate 都不要做 */
    }
    strncpy(cache, text, n - 1);
    cache[n - 1] = '\0';
    ui_label_set_text_bold(lbl, text);
}

/** @brief 只在状态真的切换时才改卡片高亮边框 */
static void main_set_active_card(lv_obj_t *card, bool active, bool *cached)
{
    if (card == NULL || cached == NULL || *cached == active) {
        return;
    }
    *cached = active;
    /* 正在输入的那张卡片用绿色粗边框提示，避免操作员看错卡片 */
    lv_obj_set_style_border_color(card, active ? UI_C_ACCENT_GREEN : UI_C_CARD_BORDER, 0);
    lv_obj_set_style_border_width(card, active ? UI_CARD_BORDER_W + 1 : UI_CARD_BORDER_W, 0);
}

/**
 * @brief 设置读数值的配色：正在输入时用蓝色，其余按常态色
 * @note 蓝色只在"缓冲区有内容"时出现，是判断缓冲区是否被清空的唯一肉眼依据，
 *       所以必须和卡片高亮一样做缓存，避免每 100ms 重设样式。
 */
static void main_set_value_color(lv_obj_t *lbl, bool editing, lv_color_t normal, bool *cached)
{
    if (lbl == NULL || cached == NULL || *cached == editing) {
        return;
    }
    *cached = editing;
    lv_obj_set_style_text_color(lbl, editing ? UI_C_VALUE_EDITING : normal, 0);
}

/** @brief 只在颜色真的变化时才改提示行颜色 */
static void main_set_prompt_color(lv_color_t color)
{
    if (s_lbl_prompt == NULL) {
        return;
    }
    if (s_prompt_color_valid && s_prompt_color.full == color.full) {
        return;
    }
    s_prompt_color = color;
    s_prompt_color_valid = true;
    lv_obj_set_style_text_color(s_lbl_prompt, color, 0);
}

/*==============================================================================
 * 提示行
 *============================================================================*/
static const char *prompt_text_for_state(app_state_id_t st)
{
    switch (st) {
    case APP_STATE_1: return APP_TXT_PROMPT_STATE1;
    case APP_STATE_2: return APP_TXT_PROMPT_STATE2;
    case APP_STATE_3: return APP_TXT_PROMPT_STATE3;
    case APP_STATE_ADMIN: return APP_TXT_PROMPT_ADMIN;
    case APP_STATE_ALARM: return APP_TXT_PROMPT_ALARM;
    default: return "";
    }
}

static void main_refresh_prompt(void)
{
    if (s_lbl_prompt == NULL || s_flash_active) {
        return;   /* 正在闪消息，别抢 */
    }
    main_set_prompt_color(UI_C_TEXT);
    main_set_text_cached(s_lbl_prompt, s_cache_prompt, sizeof(s_cache_prompt),
                         prompt_text_for_state(app_state_get()));
}

void ui_main_flash_prompt(const char *text, lv_color_t color)
{
    if (s_lbl_prompt == NULL || text == NULL) {
        return;
    }
    strncpy(s_flash_text, text, sizeof(s_flash_text) - 1);
    s_flash_text[sizeof(s_flash_text) - 1] = '\0';
    s_flash_until_ms = esp_timer_get_time() / 1000 + MAIN_FLASH_MS;
    s_flash_active = true;

    main_set_prompt_color(color);
    /* 这里直接写、并同步更新缓存：这样 3 秒后 main_refresh_prompt() 才能
     * 检测到"缓存里是闪消息、应该是常态提示"并把提示恢复回去。 */
    strncpy(s_cache_prompt, s_flash_text, sizeof(s_cache_prompt) - 1);
    s_cache_prompt[sizeof(s_cache_prompt) - 1] = '\0';
    ui_label_set_text_bold(s_lbl_prompt, s_cache_prompt);
}

static void main_tick_flash(void)
{
    if (!s_flash_active) {
        return;
    }
    if (esp_timer_get_time() / 1000 >= s_flash_until_ms) {
        s_flash_active = false;
        main_refresh_prompt();
    }
}

/*==============================================================================
 * 读数卡片
 *============================================================================*/
static void main_refresh_cards(void)
{
    if (s_lbl_cur_val == NULL) {
        return;
    }
    app_state_id_t st = app_state_get();
    bool entry_used = !ui_entry_is_empty(&s_entry);
    char txt[16];

    /* 当前值卡片：
     * State1 / State3 时它就是"刻度盘实际位置"的输入目标，输入缓冲区优先显示。
     * 显示缓冲区时用蓝色字 + 绿色粗边框，"正在输入"和"已生效"一眼可分。 */
    bool cur_editing = entry_used && (st == APP_STATE_1 || st == APP_STATE_3);
    if (cur_editing) {
        ui_entry_text(&s_entry, txt, sizeof(txt));
    } else {
        ui_fmt_value(txt, sizeof(txt), s_live_pos);
    }
    main_set_active_card(s_card_cur, cur_editing, &s_card_cur_active);
    main_set_value_color(s_lbl_cur_val, cur_editing, UI_C_VALUE_CURRENT, &s_cur_val_editing);
    main_set_text_cached(s_lbl_cur_val, s_cache_cur_val, sizeof(s_cache_cur_val), txt);

    /* 目标值卡片：State2 时显示正在输入的目标值 */
    bool tgt_editing = entry_used && (st == APP_STATE_2);
    if (tgt_editing) {
        ui_entry_text(&s_entry, txt, sizeof(txt));
    } else {
        ui_fmt_value(txt, sizeof(txt), app_state_get_target());
    }
    main_set_active_card(s_card_tgt, tgt_editing, &s_card_tgt_active);
    main_set_value_color(s_lbl_tgt_val, tgt_editing, UI_C_VALUE_TARGET, &s_tgt_val_editing);
    main_set_text_cached(s_lbl_tgt_val, s_cache_tgt_val, sizeof(s_cache_tgt_val), txt);
}

/*==============================================================================
 * 状态文本与背景色
 *============================================================================*/
static void main_refresh_state_text(void)
{
    main_set_text_cached(s_lbl_state, s_cache_state, sizeof(s_cache_state),
                         app_state_get_text());
}

static void main_refresh_bg(void)
{
    if (s_scr == NULL) {
        return;
    }
    app_state_id_t st = app_state_get();
    lv_color_t color;

    switch (st) {
    case APP_STATE_ALARM:
        color = UI_C_BG_MOVING;           /* 报警：红 */
        break;
    case APP_STATE_3:
        color = UI_C_BG_WARN;             /* 越限待确认：橙 */
        break;
    case APP_STATE_ADMIN:
        color = UI_C_PAGE_BG;             /* Admin 屏不参与状态色 */
        break;
    default:
        if (!app_state_is_position_known()) {
            color = UI_C_BG_NEUTRAL;      /* 位置未标定：浅灰（与界面稿一致） */
        } else if (s_live_moving) {
            color = UI_C_BG_MOVING;       /* 运动中：红 */
        } else if (fabsf(s_live_pos - app_state_get_target()) <= MOTION_DEADBAND_MM) {
            color = UI_C_BG_IN_POS;       /* |cur - tgt| <= 0.005：绿 */
        } else {
            color = UI_C_BG_WARN;
        }
        break;
    }

    /* 只在颜色真的变化时写样式，避免每 50ms 触发一次全屏重绘 */
    if (!s_bg_cached_valid || s_bg_cached.full != color.full) {
        ui_screen_set_bg(s_scr, color);
        s_bg_cached = color;
        s_bg_cached_valid = true;
    }
}

/*==============================================================================
 * 键盘事件
 *============================================================================*/
static void main_submit(void)
{
    /* 输入为空时按 Enter 视为 0.000 */
    app_state_submit(ui_entry_value(&s_entry));
}

static void main_key_cb(uint8_t key, void *user_data)
{
    (void)user_data;

    if (key <= UI_KEY_DIGIT_9) {
        if (ui_entry_push_digit(&s_entry, key)) {
            main_refresh_cards();
        }
    } else if (key == UI_KEY_BACKSPACE) {
        if (ui_entry_backspace(&s_entry)) {
            main_refresh_cards();
        }
    } else if (key == UI_KEY_ENTER) {
        /* 长按解锁 Admin 时这次点击要被吞掉（长按优先于普通提交） */
        if (s_admin_consumed) {
            ESP_LOGD(TAG, "Enter click consumed by admin long-press");
            return;
        }
        main_submit();
    }
}

/** 记录 Enter 按下时刻，用于长按检测 */
static void main_enter_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_enter_down = true;
    s_enter_down_us = esp_timer_get_time();
}

/** 松开 / 滑出：结束长按 */
static void main_enter_released_cb(lv_event_t *e)
{
    (void)e;
    s_enter_down = false;
}

/*==============================================================================
 * STOP
 *============================================================================*/
static void main_stop_cb(lv_event_t *e)
{
    (void)e;

    /* 先把缓冲区内容记下来，现场排查"到底清没清"时看这条日志即可 */
    const ui_entry_t *entry = ui_main_entry();
    char before[16];
    ui_entry_text(entry, before, sizeof(before));

    /* 只做入队 / 置标志，不阻塞；motion_task 会立刻停止发脉冲 */
    app_motion_stop();
    ui_main_clear_entry();
    ui_main_flash_prompt("STOP - motion halted", UI_C_STOP);

    ESP_LOGW(TAG, "STOP pressed in %s: entry was '%s' (len=%u) -> cleared",
             app_state_get_text(), before, (unsigned)entry->len);
}

/*==============================================================================
 * 弹窗回调
 *============================================================================*/
static void main_show_state3_popup2(bool still_out)
{
    const app_config_t cfg = app_config_get();
    static char msg[200];

    if (still_out) {
        snprintf(msg, sizeof(msg), "%s\n" APP_TXT_ERR_RANGE_FMT,
                 APP_TXT_P2_MSG, cfg.lower_lim, cfg.upper_lim);
    } else {
        snprintf(msg, sizeof(msg), "%s", APP_TXT_P2_MSG);
    }

    /* 非模态：不遮键盘，操作员可以点卡片上的 Enter，也可以直接按屏幕键盘的 ↵。
     * 卡片上的 Enter 与键盘 ↵ 行为完全一致：都提交当前输入缓冲区。 */
    ui_popup_show_notice(APP_TXT_P2_TITLE, msg, APP_TXT_P2_ENTER,
                         main_popup2_enter_cb, NULL, false);
}

static void main_popup2_enter_cb(void *user_data)
{
    (void)user_data;
    main_submit();
}

static void main_popup1_no_cb(void *user_data)
{
    (void)user_data;
    ui_main_clear_entry();
    app_state_set(APP_STATE_1);
}

static void main_popup1_yes_cb(void *user_data)
{
    (void)user_data;

    /* ★ 进弹窗2 之前必须把输入缓冲区丢掉。
     *   弹窗2 要求操作员"用手轮摇到限位内，再按 Enter"，接下来要输入的是
     *   **摇好之后刻度盘上的新读数**，而不是刚才那个越限的旧值。留着旧值有两个坑：
     *     ① 缓冲区是 7 位满的（UI_ENTRY_MAX_DIGITS），新敲的数字会被静默丢弃
     *        —— 现场表现就是"按了没反应"；
     *     ② 直接按 ↵ 提交的还是旧值，又被判越限，弹窗2 再次弹出，
     *        看起来像"卡在 State3 出不来"。
     *   所以这里清空，让操作员从零开始输入。 */
    ui_main_clear_entry();

    main_show_state3_popup2(false);
}

static void main_alarm_ack_cb(void *user_data)
{
    (void)user_data;
    app_state_ack_alarm();
}

/*==============================================================================
 * 状态事件
 *============================================================================*/
static void main_on_state_changed(app_state_id_t st)
{
    main_refresh_state_text();

    if (st == APP_STATE_ADMIN) {
        ui_popup_close();
        ui_admin_show();
        return;
    }

    if (ui_admin_is_shown()) {
        ui_admin_hide();
        ui_main_show();
    }

    /* 回到 State1 / State2 说明上一次输入已经被消费掉了。
     * ★ 这里必须顺手把弹窗收掉：State3 的弹窗2 是**非模态**的（要放行底下键盘），
     *   所以它不会自己消失。走「弹窗2 上的 Enter 按钮」时按钮回调会先关弹窗，
     *   但直接按**键盘 ↵** 提交成功时没人管它 —— 结果进了 State2、背景都变绿了，
     *   弹窗2 还盖在屏幕上。 */
    if (st == APP_STATE_1 || st == APP_STATE_2) {
        ui_popup_close();
        ui_main_clear_entry();
    }

    main_refresh_prompt();
    main_refresh_cards();
    main_refresh_bg();
}

static void main_handle_event(const app_evt_t *evt)
{
    const app_config_t cfg = app_config_get();
    char buf[96];

    switch (evt->id) {
    case APP_EVT_STATE_CHANGED:
        main_on_state_changed((app_state_id_t)evt->value);
        break;

    case APP_EVT_LIMIT_REJECT:
        /* 越限拒绝：不弹窗，直接在提示行上给红色提示，避免打断输入节奏 */
        snprintf(buf, sizeof(buf), APP_TXT_LIMIT_REJECT_FMT,
                 evt->value, cfg.lower_lim, cfg.upper_lim);
        ui_main_flash_prompt(buf, UI_C_STOP);
        break;

    case APP_EVT_STATE3_ABOUT_TO:
        ui_popup_show_choice(APP_TXT_P1_TITLE, APP_TXT_P1_MSG,
                             APP_TXT_P1_NO, main_popup1_no_cb,
                             APP_TXT_P1_YES, main_popup1_yes_cb, NULL);
        break;

    case APP_EVT_STATE3_STILL_OUT:
        main_show_state3_popup2(true);
        break;

    case APP_EVT_MOVE_STARTED:
        ui_main_clear_entry();     /* 目标已下发，缓冲区交还给操作员 */
        snprintf(buf, sizeof(buf), APP_TXT_MOVE_START_FMT, evt->value);
        ui_main_flash_prompt(buf, UI_C_TEXT);
        break;

    case APP_EVT_POSITION_SET:
        snprintf(buf, sizeof(buf), APP_TXT_CURRENT_SET_FMT, evt->value);
        ui_main_flash_prompt(buf, UI_C_ACCENT_GREEN);
        break;

    case APP_EVT_ALARM_RAISED:
        ui_popup_show_alarm(APP_TXT_ALARM_TITLE, APP_TXT_ALARM_MSG,
                            APP_TXT_ALARM_ACK, main_alarm_ack_cb, NULL);
        break;

    case APP_EVT_ALARM_CLEARED:
        ui_popup_close();
        ui_main_clear_entry();
        break;

    default:
        break;
    }
}

/*==============================================================================
 * 轮询定时器（在 LVGL 任务里跑）
 *============================================================================*/
static void main_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 1) 取走 motion_task 推来的位置上报（队列非阻塞） */
    motion_report_t rep;
    bool got = false;
    while (app_motion_poll_report(&rep)) {
        got = true;
    }
    if (got) {
        s_live_pos = rep.pos;
        s_live_moving = rep.moving;
        if (rep.arrived) {
            char buf[48];
            snprintf(buf, sizeof(buf), APP_TXT_ARRIVED_FMT, rep.pos);
            ui_main_flash_prompt(buf, UI_C_ACCENT_GREEN);
        }
        /* 注意：这里**不要**再闪 "Motion stopped"。
         * 按 STOP 时 main_stop_cb() 已经闪了 "STOP - motion halted"，
         * 而 motion_stop() 同时置了 aborted 标志，下一次轮询（50ms 后）如果
         * 再闪一条，就会把刚显示出来的 STOP 提示顶掉 —— 现场看起来像"提示一闪就变"。
         * 报警中断导致的 abort 由全屏报警窗负责提示，这里不需要重复。 */
    }

    /* 2) 报警输入 -> 进入报警态（只在这里做，保证 UI 与状态机一致） */
    if (app_motion_is_alarm_latched() && app_state_get() != APP_STATE_ALARM) {
        app_state_enter_alarm();
    }

    /* 3) 消费状态机事件 */
    app_evt_t evt;
    while (app_state_poll_event(&evt)) {
        main_handle_event(&evt);
    }

    /* 4) 周期刷新 */
    main_tick_flash();
    if (got) {
        main_refresh_cards();
    }
    main_refresh_bg();
}

/*==============================================================================
 * Admin 长按 Enter 解锁
 *============================================================================*/
static void main_hold_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_enter_down) {
        /* 手已经松开：本轮"吞掉一次点击"的标记到此为止 */
        s_admin_consumed = false;
        return;
    }
    if (s_admin_consumed) {
        return;
    }

    app_state_id_t st = app_state_get();
    /* 只在 State1 / State2 解锁，且不能叠在别的弹窗/输入页之上 */
    if (st != APP_STATE_1 && st != APP_STATE_2) {
        return;
    }
    if (ui_popup_is_open() || ui_numpad_is_open() || ui_admin_is_shown()) {
        return;
    }
    /* 必须正好是 9.999 */
    if (!ui_entry_equals(&s_entry, APP_ADMIN_UNLOCK_VALUE)) {
        return;
    }
    if ((esp_timer_get_time() - s_enter_down_us) < (int64_t)APP_ADMIN_HOLD_MS * 1000) {
        return;
    }

    ESP_LOGI(TAG, "Admin unlocked by holding Enter for %d ms", APP_ADMIN_HOLD_MS);
    s_admin_consumed = true;   /* 长按优先：这次 Enter 不再当作提交 */
    ui_main_clear_entry();
    app_state_enter_admin();
}

/*==============================================================================
 * 返回主菜单（MOTOR TEST 子界面 -> MAIN MENU）
 *
 * 两条触发路径：
 *   ① 左上角圆形返回键（与设计稿 ui2_light.png 的子页顶栏一致）
 *   ② 在屏幕上「从右向左」滑动（需求指定的返回手势）
 *
 * 离开前必须做的三件事，顺序不能变：
 *   app_motion_stop()      屏幕切走了但轴还在跑，是最危险的"看起来没事"
 *   ui_popup_close()       弹窗挂在 lv_layer_top()，不关的话会盖在主菜单上
 *   ui_main_clear_entry()  丢弃未提交的输入缓冲区
 *============================================================================*/
static void main_go_back(const char *reason)
{
    if (ui_admin_is_shown()) {
        return;   /* Admin 屏有自己的 Exit and Accept，不接受从这里"溜走" */
    }
    if (ui_popup_is_open()) {
        /* 报警确认窗 / State3 弹窗：先把问题回答掉再走，避免留下
         * "没人处理的弹窗 + 有内容的输入缓冲区"这种半截状态 */
        ESP_LOGW(TAG, "Back ignored (%s): a popup is waiting for an answer", reason);
        return;
    }

    app_motion_stop();
    ui_popup_close();
    ui_main_clear_entry();

    ESP_LOGI(TAG, "Back to MAIN MENU (%s)", reason);
    ui_menu_show();
}

static void main_back_cb(lv_event_t *e)
{
    (void)e;
    main_go_back("back button");
}

/**
 * @brief 屏幕手势：从右向左滑动 = 返回主菜单
 *
 * @note 手势要想被 LVGL 判定出来，必须同时满足（见 lv_indev.c 的 indev_gesture）：
 *   ① 被按下的对象及其祖先里**不能有可滚动的** —— 一旦拖拽被当成滚动，
 *      LVGL 就直接跳过手势检测。本工程所有控件都清了
 *      LV_OBJ_FLAG_SCROLLABLE（ui_theme.c 的各个工厂函数），所以不会撞上这条；
 *   ② 累计位移 > 50px（LV_INDEV_DEF_GESTURE_LIMIT）且横向分量大于纵向；
 *   ③ 每次采样的移动量 > 3px（LV_INDEV_DEF_GESTURE_MIN_VELOCITY）
 *      —— 也就是**要快划**，用手指慢拖到 50px 是不触发的。
 *   LVGL 的方向约定：手指从右往左 -> 手势向量 x 为负 -> LV_DIR_LEFT。
 */
static void main_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    if (indev == NULL) {
        indev = lv_indev_get_act();
    }
    if (indev == NULL) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT) {
        ESP_LOGD(TAG, "Gesture dir %d ignored (only right->left goes back)", (int)dir);
        return;
    }
    main_go_back("swipe");
}

/**
 * @brief 让整棵子树都能把 GESTURE 事件冒泡到屏幕
 *
 * @note LVGL 默认只把 LV_EVENT_GESTURE 发给"被按下的那个对象"。本屏可点的
 *       对象有十几个（12 个按键、STOP、两张卡片……），逐个挂回调既啰嗦又
 *       容易漏；给子对象统一打开 LV_OBJ_FLAG_GESTURE_BUBBLE 之后，手势会
 *       逐级上抛，直到遇到一个**没有**该标志的祖先 —— 也就是屏幕本身。
 *       所以：子孙全部置位、屏幕保持不置位，事件必然落到 main_gesture_cb。
 */
static void main_gesture_bubble(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_obj_add_flag(c, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        main_gesture_bubble(c);
    }
}

/*==============================================================================
 * 创建
 *============================================================================*/
void ui_main_clear_entry(void)
{
    ui_entry_reset(&s_entry);
    main_refresh_cards();
}

const ui_entry_t *ui_main_entry(void)
{
    return &s_entry;
}

lv_obj_t *ui_main_screen(void)
{
    return s_scr;
}

void ui_main_show(void)
{
    if (s_scr == NULL) {
        return;
    }
    lv_scr_load(s_scr);
    s_shown = true;
}

bool ui_main_is_shown(void)
{
    return s_shown;
}

void ui_main_refresh(void)
{
    main_refresh_state_text();
    main_refresh_prompt();
    main_refresh_cards();
    main_refresh_bg();
}

static void main_create_topbar(void)
{
    /* 顶栏：y=0..46 白底；y=47..48 两行 #CFCFCF 分隔线 */
    ui_panel_create(s_scr, 0, 0, UI_SCR_W, UI_TOP_BAR_H, UI_C_BAR_BG);
    ui_panel_create(s_scr, 0, UI_TOP_BAR_LINE_Y, UI_SCR_W, UI_TOP_BAR_LINE_H, UI_C_BAR_LINE);

    /* 左上角圆形返回键：本屏是主菜单的 MOTOR TEST 子界面（见 ui_menu.c），
     * 这是与设计稿 ui2_light.png 一致的子页返回样式；手势返回见 main_gesture_cb */
    lv_obj_t *back = lv_obj_create(s_scr);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, MAIN_BACK_X, MAIN_BACK_Y);
    lv_obj_set_size(back, MAIN_BACK_D, MAIN_BACK_D);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, UI_C_BAR_BG, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back, MAIN_BACK_BORDER_W, 0);
    lv_obj_set_style_border_color(back, UI_C_MENU_BLUE, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(back, main_back_cb, LV_EVENT_CLICKED, NULL);

    /* ‹ 箭头：Lato 粗体字库只有 ASCII，符号字形必须用内置 Montserrat */
    lv_obj_t *arrow = lv_label_create(back);
    lv_obj_set_style_text_font(arrow, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(arrow, UI_C_MENU_BLUE, 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);

    /* 标题：让开返回键后从 x=56 起，文案 = 子界面名 MOTOR TEST
     * （原来是 x=16 的 "ROLL COATER HMI"，那个名字现在由主菜单顶栏承担） */
    s_lbl_title = ui_label_create_bold(s_scr, 56, 0, 404, UI_TOP_BAR_H,
                                       UI_FONT_TITLE, UI_C_TEXT, LV_TEXT_ALIGN_LEFT,
                                       APP_TXT_MOTOR_TEST);
    /* State 文本：右对齐，右侧留 16px（界面稿实测文字右边界 x=782） */
    s_lbl_state = ui_label_create_bold(s_scr, 460, 0, 324, UI_TOP_BAR_H,
                                       UI_FONT_STATE, UI_C_TEXT_DIM, LV_TEXT_ALIGN_RIGHT,
                                       "");
}

static void main_create_cards(void)
{
    /* --- 卡片 1：Motor's Current Position  x=15 y=63 w=362 h=138 --- */
    s_card_cur = ui_card_create(s_scr, UI_CARD_X, UI_CARD_Y1, UI_CARD_W, UI_CARD_H);
    /* 卡片标题：卡片内 y=5 h=44，20 号字，色 #616161（界面稿实测大写高 14px） */
    s_lbl_cur_cap = ui_label_create_bold(s_card_cur, 0, UI_CARD_CAP_Y, UI_CARD_W, UI_CARD_CAP_H,
                                         UI_FONT_CAPTION, UI_C_TEXT_DIM, LV_TEXT_ALIGN_CENTER,
                                         APP_TXT_CAP_CURRENT);
    /* 读数：卡片内 y=52 h=60，居中（界面稿数字中心在卡片内 82px 处） */
    s_lbl_cur_val = ui_label_create_bold(s_card_cur, 0, UI_CARD_VAL_Y, UI_CARD_W, UI_CARD_VAL_H,
                                         UI_FONT_VALUE, UI_C_VALUE_CURRENT,
                                         LV_TEXT_ALIGN_CENTER, "0.000");

    /* --- 卡片 2：Target Position  x=15 y=215 w=362 h=138 --- */
    s_card_tgt = ui_card_create(s_scr, UI_CARD_X, UI_CARD_Y2, UI_CARD_W, UI_CARD_H);
    s_lbl_tgt_cap = ui_label_create_bold(s_card_tgt, 0, UI_CARD_CAP_Y, UI_CARD_W, UI_CARD_CAP_H,
                                         UI_FONT_CAPTION, UI_C_TEXT_DIM, LV_TEXT_ALIGN_CENTER,
                                         APP_TXT_CAP_TARGET);
    s_lbl_tgt_val = ui_label_create_bold(s_card_tgt, 0, UI_CARD_VAL_Y, UI_CARD_W, UI_CARD_VAL_H,
                                         UI_FONT_VALUE, UI_C_VALUE_TARGET,
                                         LV_TEXT_ALIGN_CENTER, "0.000");
}

static void main_create_keypad(void)
{
    /* 提示行：x=400 y=64 w=385 h=32（键盘正上方，文字中心 x=592.5） */
    s_lbl_prompt = ui_label_create_bold(s_scr, UI_RIGHT_X, UI_PROMPT_Y, UI_PROMPT_W,
                                        UI_PROMPT_H, UI_FONT_PROMPT, UI_C_TEXT,
                                        LV_TEXT_ALIGN_CENTER, APP_TXT_PROMPT_STATE1);

    /* 键盘：起点 (400,104)，键 120x64，列距 8 行距 8
     * 总宽 = 3*120 + 2*8   = 376 -> 400..775
     * 总高 = 4*64  + 3*8   = 280 -> 104..383 */
    const ui_keypad_geom_t geom = {
        .x = UI_KEYPAD_X,
        .y = UI_KEYPAD_Y,
        .key_w = UI_KEY_W,
        .key_h = UI_KEY_H,
        .gap_x = UI_KEY_GAP_X,
        .gap_y = UI_KEY_GAP_Y,
        .label_dy = UI_KEY_LABEL_DY,
    };
    ui_keypad_create(s_scr, &geom, main_key_cb, NULL, s_btn_key);

    /* Enter 键额外挂按下/松开事件，用于 Admin 长按检测 */
    if (s_btn_key[MAIN_KEY_ENTER_INDEX] != NULL) {
        lv_obj_add_event_cb(s_btn_key[MAIN_KEY_ENTER_INDEX], main_enter_pressed_cb,
                            LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn_key[MAIN_KEY_ENTER_INDEX], main_enter_released_cb,
                            LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_btn_key[MAIN_KEY_ENTER_INDEX], main_enter_released_cb,
                            LV_EVENT_PRESS_LOST, NULL);
    }

    /* STOP：x=400 y=400 w=384 h=56，H3 粗体白字（界面稿大写高 21px） */
    s_btn_stop = ui_button_create(s_scr, UI_STOP_X, UI_STOP_Y, UI_STOP_W, UI_STOP_H,
                                  UI_C_STOP, UI_C_STOP_FG, UI_FONT_ACTION,
                                  APP_TXT_STOP, main_stop_cb, NULL);
    ui_button_make_bold(s_btn_stop, UI_STOP_LABEL_DY);
}

void ui_main_create(void)
{
    if (s_scr != NULL) {
        return;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    ui_screen_set_bg(s_scr, UI_C_BG_NEUTRAL);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_entry_reset(&s_entry);

    main_create_topbar();
    main_create_cards();
    main_create_keypad();

    /* 轮询定时器：位置上报 + 状态事件 + 背景色 */
    s_poll_timer = lv_timer_create(main_poll_cb, MAIN_POLL_PERIOD_MS, NULL);
    /* 长按定时器：Admin 解锁检测 */
    s_hold_timer = lv_timer_create(main_hold_cb, MAIN_HOLD_PERIOD_MS, NULL);

    /* 手势返回：从右向左滑动 = 回主菜单。
     * 必须放在所有控件创建完之后（要铺到整棵子树），见 main_gesture_bubble。 */
    lv_obj_add_event_cb(s_scr, main_gesture_cb, LV_EVENT_GESTURE, NULL);
    main_gesture_bubble(s_scr);

    ui_main_refresh();

    /* ★ 这里故意不再 ui_main_show()：上电显示的是主菜单（ui_menu.c），
     *   本屏只在操作员点 MOTOR TEST 卡片时才被 lv_scr_load 上来。
     *   装配顺序见 main.c 第 5 步。 */
    ESP_LOGI(TAG, "Motor test screen created: card(%d,%d,%dx%d) keypad(%d,%d) key %dx%d stop(%d,%d,%dx%d)",
             UI_CARD_X, UI_CARD_Y1, UI_CARD_W, UI_CARD_H,
             UI_KEYPAD_X, UI_KEYPAD_Y, UI_KEY_W, UI_KEY_H,
             UI_STOP_X, UI_STOP_Y, UI_STOP_W, UI_STOP_H);
}
