/**
 * @file app_sensor.h
 * @brief SHT30/SHT31 温湿度采集（ENVIRONMENT / HISTORY 两个界面的数据源）
 *
 * ┌─ 硬件 ─────────────────────────────────────────────────────────────────┐
 * │  传感器挂在与 GT911 触摸**共用**的 I2C0 上：SDA=GPIO19 / SCL=GPIO20    │
 * │  100kHz，从机地址 0x44（GT911 是 0x5D，同总线不冲突）                  │
 * │  接线：传感器的 SDA/SCL 与触摸屏那两根线并联，VCC=3V3，GND 共地        │
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * ★ 共用总线的并发安全（已从 IDF 源码确认）：
 *   drivers/i2c/i2c_master.c 内部对每条总线有 bus_lock_mux 信号量，
 *   所有传输（含 i2c_master_probe）都会先取锁，所以"触摸读坐标"和
 *   "本模块读温湿度"同时发生也不会互相踩。
 *   注意：esp_lcd 的触摸驱动用的是**无限超时**，本模块统一用有限超时
 *   （SENSOR_IO_TIMEOUT_MS），万一总线被挂死也只是本模块返回超时，
 *   不会把采样任务永久卡住。
 *
 * 采样与统计：
 *   每 SENSOR_PERIOD_MS（2s，与界面稿上的 "SHT31 · 2s" 一致）读一次；
 *   每 SENSOR_HIST_PERIOD_S（5min）往环形缓冲压一个历史点，
 *   24h = 288 点（HISTORY 页那条曲线）。
 *   "今日最值"在系统时间可用时按日期翻转自动清零。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 从机地址：SHT30/31 默认 0x44（ADDR 脚接高电平时是 0x45，改这里） */
#define SENSOR_I2C_ADDR         (0x44)
#define SENSOR_IO_TIMEOUT_MS    (100)     /* 单次 I2C 传输超时，别用 -1 */

#define SENSOR_PERIOD_MS        (2000)    /* 采样周期 */
#define SENSOR_HIST_PERIOD_S    (300)     /* 历史点间隔 5 分钟 */
#define SENSOR_HIST_POINTS      (288)     /* 24h / 5min = 288 点 */

/** 一个历史点 */
typedef struct {
    float temp_c;
    float hum_pct;
} sensor_sample_t;

/** 供 UI 读取的数据快照 */
typedef struct {
    bool     valid;          /*!< 当前读数是否有效（无效时 UI 显示 "--"） */
    bool     present;        /*!< 总线上是否探测到传感器 */
    uint32_t ok_count;       /*!< 成功次数（排查"偶尔读到 0"很有用） */
    uint32_t err_count;      /*!< 失败次数 */

    float    temp_c;         /*!< 最新温度（摄氏，UI 按设置换算） */
    float    hum_pct;        /*!< 最新相对湿度（%） */

    /*---- 今日最值（跨天自动清零） ----*/
    float    t_min, t_max;
    float    h_min, h_max;
    bool     today_valid;

    /*---- 最近 24h 统计（由历史环形缓冲算出） ----*/
    float    t_avg24, h_avg24;
    float    t_min24, t_max24;
    float    h_min24, h_max24;
    uint32_t hist_count;     /*!< 已攒到的历史点数；< 288 表示还没跑满 24h */
} app_sensor_data_t;

/**
 * @brief 在 I2C0 上挂载传感器并创建采样任务
 * @note 必须在 bsp_display_init()（它会建立 I2C0 总线）之后调用。
 *       传感器没插也不会失败：会周期性重试探测（支持热插拔）。
 */
esp_err_t app_sensor_init(void);

/**
 * @brief 取一份数据快照（加锁拷贝，非阻塞）
 * @return false = 现在拿不到锁（UI 保持上一次显示即可）
 */
bool app_sensor_get(app_sensor_data_t *out);

/**
 * @brief 拷贝历史点，按时间升序（最老的在前）
 * @param out 目标数组；max_points 建议 >= SENSOR_HIST_POINTS
 * @return 实际写入的点数（<= 288）
 */
uint32_t app_sensor_history(sensor_sample_t *out, uint32_t max_points);

#ifdef __cplusplus
}
#endif
