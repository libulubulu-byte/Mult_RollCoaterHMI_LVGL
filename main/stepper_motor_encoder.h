/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ★★ 本文件是从 examples/peripherals/rmt/stepper_motor/main/stepper_motor_encoder.h
 *    **原样拷进来**的（只加了这段注释，代码一个字没改）★★
 *
 * 为什么要在主工程里放一份：
 *   现场发现"主工程设 12000Hz 与台架设 12000Hz 转的不一样"，逐行比对后确认两边
 *   的**发射方式**根本不是同一个写法（台架用下面这个 uniform 编码器 +
 *   `loop_count = 本批脉冲数`，让**硬件重复同一个脉冲符号**；主工程原来是自己摆
 *   n 个符号的数组、`loop_count = 0`）。要让"一样的频率产生一样的波形"，最可靠的
 *   办法就是把**台架那份编码器原样搬过来**，用它的方式发（见 app_motion.c 的
 *   `s_emit_bench` / `mtest mode`）。
 *
 * ★ 以后再改发射链路时：这个文件保持与台架一致，两边就可以拿 `mtest mode`
 *   做 A/B，结论才可复现（不要"顺手优化"它）。
 */

#pragma once

#include <stdint.h>
#include "driver/rmt_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stepper motor curve encoder configuration
 */
typedef struct {
    uint32_t resolution;    // Encoder resolution, in Hz
    uint32_t sample_points; // Sample points used for deceleration phase. Note: |end_freq_hz - start_freq_hz| >= sample_points
    uint32_t start_freq_hz; // Start frequency on the curve, in Hz
    uint32_t end_freq_hz;   // End frequency on the curve, in Hz
} stepper_motor_curve_encoder_config_t;

/**
 * @brief Stepper motor uniform encoder configuration
 */
typedef struct {
    uint32_t resolution; // Encoder resolution, in Hz
} stepper_motor_uniform_encoder_config_t;

/**
 * @brief Create stepper motor curve encoder
 *
 * @param[in] config Encoder configuration
 * @param[out] ret_encoder Returned encoder handle
 * @return
 *      - ESP_ERR_INVALID_ARG for any invalid arguments
 *      - ESP_ERR_NO_MEM out of memory when creating step motor encoder
 *      - ESP_OK if creating encoder successfully
 */
esp_err_t rmt_new_stepper_motor_curve_encoder(const stepper_motor_curve_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);

/**
 * @brief Create RMT encoder for encoding step motor uniform phase into RMT symbols
 *
 * @param[in] config Encoder configuration
 * @param[out] ret_encoder Returned encoder handle
 * @return
 *      - ESP_ERR_INVALID_ARG for any invalid arguments
 *      - ESP_ERR_NO_MEM out of memory when creating step motor encoder
 *      - ESP_OK if creating encoder successfully
 */
esp_err_t rmt_new_stepper_motor_uniform_encoder(const stepper_motor_uniform_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);

#ifdef __cplusplus
}
#endif
