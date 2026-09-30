/**
 * @file app_state.h
 * @brief 应用状态机：State1 / State2 / State3 / Admin / Alarm
 *
 *   State1  输入刻度盘实际位置（Roll Coat Height）
 *           Enter -> 在 [lower, upper] 内则设为当前位置并进入 State2
 *                 越限则进入 State3
 *   State2  输入目标位置
 *           Enter -> 校验限值：合法则下发运动；越限拒绝并提示
 *   State3  弹窗1「输入是否正确？」No -> 回 State1；Yes -> 弹窗2「用手轮把轴摇到限位内，再按 Enter」
 *           按 Enter 且值在限内 -> 进入 State2
 *   Admin   在 State1/State2 下输入 9.999 并长按 Enter 满 3 秒进入
 *   Alarm   ALARM 输入有效 -> 锁存，禁止运动，必须人工 ACK
 *
 * 线程模型：
 *   - 状态机本身只在 LVGL 任务里被调用（键盘事件 / 轮询定时器），因此内部只用一个
 *     轻量互斥锁保护跨任务的读（motion_task 不直接改状态机）。
 *   - 需要通知 UI 的事件放进一个环形队列，由 UI 轮询定时器消费，
 *     这样任何任务都可以非阻塞地投递事件。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态编号与 app_text.h 里的 "State %d" 直接对应 */
typedef enum {
    APP_STATE_1 = 1,   /*!< 输入刻度盘实际位置 */
    APP_STATE_2,       /*!< 输入目标位置 */
    APP_STATE_3,       /*!< 越限，需要人工确认 */
    APP_STATE_ADMIN,   /*!< 参数设置 */
    APP_STATE_ALARM,   /*!< 报警锁存 */
} app_state_id_t;

/** UI 事件（由 UI 轮询定时器消费） */
typedef enum {
    APP_EVT_STATE_CHANGED = 0,  /*!< 状态切换，value = 新状态 */
    APP_EVT_LIMIT_REJECT,       /*!< 输入越限被拒，value = 被拒的值（State2） */
    APP_EVT_STATE3_ABOUT_TO,    /*!< 即将进入 State3（弹窗1） */
    APP_EVT_STATE3_STILL_OUT,   /*!< State3 里手轮确认后仍然越限 */
    APP_EVT_MOVE_STARTED,       /*!< 已下发运动，value = 目标 */
    APP_EVT_POSITION_SET,       /*!< 当前位置被标定，value = 新位置 */
    APP_EVT_ALARM_RAISED,       /*!< 报警锁存 */
    APP_EVT_ALARM_CLEARED,      /*!< 报警已确认 */
} app_evt_id_t;

typedef struct {
    app_evt_id_t id;
    float value;
} app_evt_t;

/** 初始化（创建互斥锁与事件队列），必须在 UI 创建之前调用 */
esp_err_t app_state_init(void);

/** @brief 当前状态 */
app_state_id_t app_state_get(void);

/** @brief 状态显示名（app_text.h 的 APP_TXT_STATE_FMT 产物，静态缓冲） */
const char *app_state_get_text(void);

/** @brief 设置状态（会投递 APP_EVT_STATE_CHANGED） */
void app_state_set(app_state_id_t state);

/** @brief 当前报告位置（mm） */
float app_state_get_current(void);

/** @brief 当前目标位置（mm） */
float app_state_get_target(void);

/** @brief 当前位置是否已经由操作员标定过（决定背景色是否可判到位） */
bool app_state_is_position_known(void);

/**
 * @brief 处理一次「Enter 提交」（键盘 Enter / 弹窗 Enter）
 * @param value 输入缓冲区解析出的值（mm）
 */
void app_state_submit(float value);

/** @brief 报警锁存处理（UI 轮询检测到 alarm 后调用） */
void app_state_enter_alarm(void);

/** @brief 报警确认（人工 ACK） */
void app_state_ack_alarm(void);

/** @brief 进入 Admin（记录返回状态） */
void app_state_enter_admin(void);

/**
 * @brief 退出 Admin
 * @param accepted true = Exit and Accept（参数已生效，需要重新标定当前位置）
 */
void app_state_exit_admin(bool accepted);

/** @brief 非阻塞取一条 UI 事件 */
bool app_state_poll_event(app_evt_t *out);

/** @brief 非阻塞地投递 UI 事件（任何任务都可调用） */
void app_state_post_event(app_evt_id_t id, float value);

#ifdef __cplusplus
}
#endif
