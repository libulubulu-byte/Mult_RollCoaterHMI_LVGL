/**
 * @file app_state.c
 * @brief 状态机实现（State1 / State2 / State3 / Admin / Alarm）
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

#include "app_state.h"
#include "app_config.h"
#include "app_motion.h"
#include "app_text.h"

static const char *TAG = "app_state";

#define APP_EVT_QUEUE_LEN   (16)

static SemaphoreHandle_t s_lock = NULL;
static QueueHandle_t s_evt_q = NULL;

static app_state_id_t s_state = APP_STATE_1;
static app_state_id_t s_state_before_admin = APP_STATE_1;
static float s_target = 0.0f;
static bool  s_pos_known = false;
static char  s_state_text[16] = "State 1";

/*==============================================================================
 * 内部工具
 *============================================================================*/
static void state_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void state_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void state_refresh_text(void)
{
    snprintf(s_state_text, sizeof(s_state_text), APP_TXT_STATE_FMT, (int)s_state);
}

void app_state_post_event(app_evt_id_t id, float value)
{
    if (s_evt_q == NULL) {
        return;
    }
    app_evt_t evt = { .id = id, .value = value };
    /* 队列满时丢弃最旧事件，保证 UI 最终能看到最新状态 */
    if (xQueueSend(s_evt_q, &evt, 0) != pdTRUE) {
        app_evt_t drop;
        (void)xQueueReceive(s_evt_q, &drop, 0);
        (void)xQueueSend(s_evt_q, &evt, 0);
    }
}

/*==============================================================================
 * 生命周期
 *============================================================================*/
esp_err_t app_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex create failed");

    s_evt_q = xQueueCreate(APP_EVT_QUEUE_LEN, sizeof(app_evt_t));
    ESP_RETURN_ON_FALSE(s_evt_q != NULL, ESP_ERR_NO_MEM, TAG, "event queue create failed");

    const app_config_t cfg = app_config_get();

    state_lock();
    s_state = APP_STATE_1;
    s_state_before_admin = APP_STATE_1;
    s_target = cfg.pos;
    s_pos_known = false;   /* 上电后必须先由操作员标定刻度盘实际位置 */
    state_refresh_text();
    state_unlock();

    ESP_LOGI(TAG, "State machine init: limits %.3f .. %.3f, pos %.3f",
             cfg.lower_lim, cfg.upper_lim, cfg.pos);
    return ESP_OK;
}

/*==============================================================================
 * 读取
 *============================================================================*/
app_state_id_t app_state_get(void)
{
    state_lock();
    app_state_id_t s = s_state;
    state_unlock();
    return s;
}

const char *app_state_get_text(void)
{
    state_lock();
    state_refresh_text();
    state_unlock();
    return s_state_text;
}

float app_state_get_current(void)
{
    return app_motion_get_position();
}

float app_state_get_target(void)
{
    state_lock();
    float t = s_target;
    state_unlock();
    return t;
}

bool app_state_is_position_known(void)
{
    state_lock();
    bool known = s_pos_known;
    state_unlock();
    return known;
}

/*==============================================================================
 * 状态切换
 *============================================================================*/
void app_state_set(app_state_id_t state)
{
    state_lock();
    if (s_state == state) {
        state_unlock();
        return;
    }
    s_state = state;
    state_refresh_text();
    state_unlock();

    /* ★★★ ENA（使能）策略：与同现场 Arduino 版 `Mult_ESP32-S3_arduino_lvgl` 逐状态对齐
     *   —— 现场定稿（2026-09-22）：**"运行时一直低电平"** ★★★
     *
     *   | 状态 | 参考工程 app.cpp | 本工程 |
     *   |---|---|---|
     *   | State1（输刻度盘读数） | `case ST_CALIB: motor_set_enabled(false)` | **脱机** |
     *   | **State2（输目标 / 运动）** | `case ST_TARGET: motor_set_enabled(true)` | **使能：一进来就拉低，整个停留期间保持** |
     *   | State3（越限，要用手轮摇回限位） | —（沿用前一状态） | **脱机**（手轮必须能摇动轴） |
     *
     *  ★ 为什么 State2 要"一进来就使能、并一直保持低"（而不是"发脉冲时才拉低"）：
     *    现场实测"**把 IO13(ENA) 强制拉低，电机就正常了**" —— 说明 ENA 这一路必须是
     *    **一个稳定的低**。原来只有 `motion_begin()`（临发脉冲那一下）才拉低，而
     *    `run` / 上电固定段跑完还会**主动抬 ENA 脱机** ⇒ 进入 State2 到下一次运动之间
     *    驱动器是失能的，与参考版行为并不一致；现在逐状态对齐。
     *  ★ 手盘相关：State1 / State3 必须脱机，否则保持力矩让人盘不动轴（参考工程同）。
     *    最容易踩的场景是**报警 ACK 之后回 State1** —— ACK 刚把 ENA 拉低使能，
     *    紧接着要求重新标定，不脱机就摇不动轴。
     *  ★ Admin / Alarm：这里不动 ENA（报警有自己的急停动作：抬 ENA）。 */
    switch (state) {
    case APP_STATE_1:
    case APP_STATE_3:
        app_motion_set_enabled(false);
        break;
    case APP_STATE_2:
        app_motion_set_enabled(true);
        break;
    default:
        break;
    }

    ESP_LOGI(TAG, "State -> %s", app_state_get_text());
    app_state_post_event(APP_EVT_STATE_CHANGED, (float)state);
}

/*==============================================================================
 * 提交处理（键盘 Enter）
 *============================================================================*/
/** State1 / State3 的“成功分支”：把输入值当作刻度盘实测位置
 *  @note 调用时不能持有 s_lock（内部会自己加锁） */
static void state_accept_current_position(float value)
{
    state_lock();
    s_pos_known = true;
    s_target = value;
    state_unlock();

    /* 标定当前位置：motion 侧会清空待执行目标，避免被旧目标继续拖走 */
    app_motion_set_current(value);
    app_config_save_position_async(value);

    app_state_post_event(APP_EVT_POSITION_SET, value);
    app_state_set(APP_STATE_2);
}

void app_state_submit(float value)
{
    const app_config_t cfg = app_config_get();
    bool in_range = (value >= cfg.lower_lim) && (value <= cfg.upper_lim);

    app_state_id_t state = app_state_get();
    ESP_LOGI(TAG, "Submit %.3f in %s (limits %.3f .. %.3f) %s",
             value, app_state_get_text(), cfg.lower_lim, cfg.upper_lim,
             in_range ? "OK" : "OUT-OF-RANGE");

    switch (state) {
    case APP_STATE_1:
        if (in_range) {
            state_accept_current_position(value);
        } else {
            /* 越限 -> State3，由 UI 弹「输入是否正确？」 */
            app_state_set(APP_STATE_3);
            app_state_post_event(APP_EVT_STATE3_ABOUT_TO, value);
        }
        break;

    case APP_STATE_2:
        if (in_range) {
            state_lock();
            s_target = value;
            state_unlock();
            app_motion_move_to(value);
            app_state_post_event(APP_EVT_MOVE_STARTED, value);
        } else {
            /* 拒绝并提示，状态保持在 State2 */
            app_state_post_event(APP_EVT_LIMIT_REJECT, value);
        }
        break;

    case APP_STATE_3:
        if (in_range) {
            /* 手轮已经把轴摇到限位内并确认 -> 与 State1 相同，进入 State2 */
            state_accept_current_position(value);
        } else {
            app_state_post_event(APP_EVT_STATE3_STILL_OUT, value);
        }
        break;

    case APP_STATE_ADMIN:
    case APP_STATE_ALARM:
    default:
        ESP_LOGW(TAG, "Submit ignored in state %d", (int)state);
        break;
    }
}

/*==============================================================================
 * 报警
 *============================================================================*/
void app_state_enter_alarm(void)
{
    app_motion_stop();
    app_state_set(APP_STATE_ALARM);
    app_state_post_event(APP_EVT_ALARM_RAISED, 0.0f);
}

void app_state_ack_alarm(void)
{
    if (app_motion_clear_alarm() != ESP_OK) {
        ESP_LOGW(TAG, "ACK refused: alarm input still active");
        return;
    }
    state_lock();
    /* 报警后位置可信度未知，回到 State1 重新标定 */
    s_pos_known = false;
    state_unlock();

    app_state_post_event(APP_EVT_ALARM_CLEARED, 0.0f);
    app_state_set(APP_STATE_1);
}

/*==============================================================================
 * Admin
 *============================================================================*/
void app_state_enter_admin(void)
{
    state_lock();
    s_state_before_admin = s_state;
    state_unlock();

    app_state_set(APP_STATE_ADMIN);
}

void app_state_exit_admin(bool accepted)
{
    if (accepted) {
        const app_config_t cfg = app_config_get();

        /* 参数可能改了限位/当前位置：以新参数重新建立坐标系 */
        state_lock();
        s_target = cfg.pos;
        s_pos_known = false;   /* 限位可能变了，要求重新标定刻度盘读数 */
        state_unlock();

        app_motion_set_current(cfg.pos);
    }

    app_state_set(APP_STATE_1);
}

/*==============================================================================
 * 事件出队
 *============================================================================*/
bool app_state_poll_event(app_evt_t *out)
{
    if (s_evt_q == NULL || out == NULL) {
        return false;
    }
    return xQueueReceive(s_evt_q, out, 0) == pdTRUE;
}
