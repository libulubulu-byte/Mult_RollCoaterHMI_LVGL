/**
 * @file app_config.h
 * @brief 机器参数结构体 + nvs_flash 持久化
 *
 * 设计要点：
 *   - 参数只在内存里改（app_config_set_field），Admin 屏确认时才异步落盘，
 *     避免在 LVGL 任务里做 NVS 写（一次 commit 可能几十 ms，会卡 UI）。
 *   - 落盘由独立的低优先级任务 cfg_save_task 完成，队列深度 1（覆盖式），
 *     连续点几次 Exit 也只会写最后一次。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS 命名空间与键名（键名 ≤ 15 字符） */
#define APP_CFG_NVS_NAMESPACE   "rc_hmi"

typedef enum {
    APP_CFG_UPPER_LIMIT = 0,   /*!< 上限位（mm） */
    APP_CFG_LOWER_LIMIT,       /*!< 下限位（mm） */
    APP_CFG_ACCELERATION,      /*!< 加速度（mm/s^2） */
    APP_CFG_DECELERATION,      /*!< 减速度（mm/s^2） */
    APP_CFG_SPEED,             /*!< 最大速度（mm/s） */
    APP_CFG_CURRENT_POSITION,  /*!< 当前（刻度盘实测）位置（mm） */
    APP_CFG_BACKLASH,          /*!< 反向间隙补偿量（mm） */
    APP_CFG_FIELD_COUNT
} app_cfg_field_t;

/** 全部可持久化参数 */
typedef struct {
    float upper_lim;   /*!< nvs key: upper_lim */
    float lower_lim;   /*!< nvs key: lower_lim */
    float accel;       /*!< nvs key: accel     */
    float decel;       /*!< nvs key: decel     */
    float speed;       /*!< nvs key: speed     */
    float pos;         /*!< nvs key: pos       */
    float backlash;    /*!< nvs key: backlash  */
} app_config_t;

/**
 * @brief 初始化 NVS 并载入参数（缺失的键写入出厂默认值）
 */
esp_err_t app_config_init(void);

/** @brief 取当前参数快照（结构体按值返回，调用方无需加锁） */
app_config_t app_config_get(void);

/** @brief 取某个字段的值 */
float app_config_get_field(const app_config_t *cfg, app_cfg_field_t field);

/** @brief 设置某个字段（仅改内存） */
void app_config_set_field(app_config_t *cfg, app_cfg_field_t field, float value);

/** @brief 取字段显示名（Admin 屏用，文案见 app_text.h） */
const char *app_config_field_name(app_cfg_field_t field);

/**
 * @brief 校验某个候选值是否可接受
 * @return NULL 表示合法；否则返回可直接显示的静态错误文案（见 app_text.h）
 */
const char *app_config_validate(const app_config_t *cfg, app_cfg_field_t field, float value);

/**
 * @brief 异步把整份参数写入 NVS（由 cfg_save_task 执行，调用方立即返回）
 */
esp_err_t app_config_commit_async(const app_config_t *cfg);

/** @brief 异步只更新当前位置（运动到点后调用，避免频繁写整表） */
esp_err_t app_config_save_position_async(float pos);

/** @brief 取出厂默认参数（也用于 NVS 初值） */
app_config_t app_config_defaults(void);

#ifdef __cplusplus
}
#endif
