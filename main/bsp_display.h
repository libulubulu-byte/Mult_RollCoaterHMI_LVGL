/**
 * @file bsp_display.h
 * @brief 显示 + 触摸 + LVGL port 初始化（RGB 并口屏 800x480 / 电容触摸）
 *
 * 任务模型（务必遵守）：
 *   - LVGL 只在 esp_lvgl_port 创建的任务里跑，其它任务想动 LVGL 必须
 *     先 bsp_display_lock() 再 bsp_display_unlock()。
 *   - LVGL 任务里禁止任何 >10ms 的阻塞操作（NVS 写、RMT 等待、vTaskDelay 大值）。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "driver/i2c_master.h"   /* bsp_display_i2c_bus() */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 RGB 面板 + 触摸 + LVGL port，并注册显示与触摸到 LVGL
 * @note 必须在 app_main 里、创建任何 LVGL 对象之前调用一次
 */
esp_err_t bsp_display_init(void);

/** @brief 取 LVGL 显示句柄（LVGL8 下即 lv_disp_t*） */
lv_display_t *bsp_display_get(void);

/** @brief 取触摸句柄（可能为 NULL，触摸初始化失败时降级为纯显示） */
esp_lcd_touch_handle_t bsp_display_get_touch(void);

/**
 * @brief 取 LVGL 互斥锁
 * @param timeout_ms 0 表示无限等待
 * @return true 取锁成功
 */
bool bsp_display_lock(uint32_t timeout_ms);

/** @brief 释放 LVGL 互斥锁 */
void bsp_display_unlock(void);

/** @brief 背光开 / 关（开时用 bsp_display_set_backlight_percent 记着的亮度） */
void bsp_display_backlight(bool on);

/**
 * @brief 设置背光亮度（LEDC PWM，1220Hz）
 * @param percent 0..100，0 = 熄灭
 * @note 调亮度也是现场排"微微在闪"的手段之一：
 *       若改亮度后闪烁随之变化 -> 问题在背光驱动或供电，不在面板数据通路。
 *       注意：只有 BSP_LCD_BL_USE_LEDC=1 时才真的可调；=0 时退化成开/关
 *       （percent>0 即点亮），见 bsp_display.c 文件头说明。
 */
esp_err_t bsp_display_set_backlight_percent(uint8_t percent);

/**
 * @brief 取触摸用的 I2C0 总线句柄（bsp_display_init 之后才有效）
 *
 * @note 给"挂在同一条总线的其它从机"用：SHT30/SHT31 温湿度（0x44）就挂在
 *       这条线上（GT911 是 0x5D，地址不冲突）。新 i2c_master 驱动对每条总线
 *       内部有 bus_lock_mux 信号量，多个任务并发访问是安全的。
 * @return NULL 表示触摸初始化失败/还没初始化（此时不要往上加从机）
 */
i2c_master_bus_handle_t bsp_display_i2c_bus(void);

#ifdef __cplusplus
}
#endif
