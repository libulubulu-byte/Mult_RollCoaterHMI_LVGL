/**
 * @file app_settings.h
 * @brief 界面偏好设置（SETTINGS 页）+ nvs_flash 持久化
 *
 * 与 app_config 的分工：
 *   app_config   机器参数（限位/加减速/速度/背隙/位置）—— Admin 屏，影响安全
 *   app_settings 界面偏好（背光亮度/温度单位/自动熄屏/灯的开关与亮度）—— SETTINGS 屏
 * 两者都用 NVS，但命名空间不同（rc_hmi / rc_ui），互不影响。
 *
 * 设计要点（照抄 app_config.c 的成熟做法）：
 *   - 改动只写内存，落盘由独立的低优先级任务异步完成（一次 NVS 写可能几十 ms，
 *     绝不能在 LVGL 任务里做）。
 *   - 队列深度 1、覆盖式：连续拖滑条只会写最后一次。
 *
 * ★ 初始化顺序：必须在 app_config_init() 之后调用（NVS 由它先 nvs_flash_init）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SET_NVS_NAMESPACE   "rc_ui"     /* NVS 命名空间（≤15 字符） */

/** 温度单位 */
typedef enum {
    APP_TEMP_UNIT_C = 0,   /*!< 摄氏度 */
    APP_TEMP_UNIT_F = 1,   /*!< 华氏度 */
} app_temp_unit_t;

/** 自动熄屏的可选时长（秒），UI 上点一下按这个数组循环 */
#define APP_AUTO_OFF_CHOICES      { 15, 30, 60, 120, 300 }
#define APP_AUTO_OFF_CHOICE_COUNT (5)
#define APP_AUTO_OFF_DEFAULT_S    (30)     /* 界面稿 ui5_settings.png 上写的是 30s */

/** 全部界面偏好（整块存成 NVS blob） */
typedef struct {
    uint8_t  disp_brightness;    /*!< 背光亮度 10..100（%） */
    uint8_t  temp_unit;          /*!< app_temp_unit_t */
    uint8_t  auto_off_en;        /*!< 自动熄屏开关 */
    uint16_t auto_off_s;         /*!< 自动熄屏时长（秒） */
    uint8_t  light_on;           /*!< 灯：上次的开关状态（掉电恢复） */
    uint8_t  light_brightness;   /*!< 灯：亮度 0..100（%） */
    uint8_t  light_last_on;      /*!< 灯：上次 >0 的亮度，用于"关灯再开"时恢复 */
} app_settings_t;

/** @brief 载入设置（缺失项写入出厂默认值），并创建异步落盘任务 */
esp_err_t app_settings_init(void);

/** @brief 取当前设置快照（结构体按值返回，调用方无需加锁） */
app_settings_t app_settings_get(void);

/** @brief 设置背光亮度（10..100，立即生效 + 异步落盘） */
void app_settings_set_disp_brightness(uint8_t percent);

/** @brief 设置温度单位 */
void app_settings_set_temp_unit(app_temp_unit_t unit);

/** @brief 设置自动熄屏（开关 + 时长） */
void app_settings_set_auto_off(bool enabled, uint16_t seconds);

/** @brief 灯的开关与亮度（灯硬件动作由 app_light 负责，这里只管持久化） */
void app_settings_set_light(bool on, uint8_t brightness);

/** @brief 手工触发一次异步落盘（一般不直接用，上面几个 set 会自动调） */
esp_err_t app_settings_save_async(void);

/** @brief 摄氏 -> 当前单位的数值（=只换数字，单位符号见 app_settings_temp_unit_text） */
float app_settings_to_display_temp(float celsius);

/** @brief 当前单位的符号："C" / "F"（不含度符号，度符号单独画，见 ui_env.c 说明） */
const char *app_settings_temp_unit_text(void);

/** @brief 自动熄屏时长的显示文案，如 "30s" / "1min" / "5min"（静态缓冲） */
const char *app_settings_auto_off_text(uint16_t seconds);

/**
 * @brief 恢复出厂设置：擦掉整个 NVS（机器参数 + 界面设置 + WiFi 凭证）后重启
 * @note 内部起一个一次性任务执行（擦 NVS 会阻塞上百 ms，不能在 LVGL 任务里做），
 *       调用后界面会先显示提示，约 1.2 秒后重启。
 */
void app_settings_factory_reset(void);

#ifdef __cplusplus
}
#endif
