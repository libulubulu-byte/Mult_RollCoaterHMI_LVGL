/**
 * @file app_light.c
 * @brief GPIO10 + LEDC PWM 灯控（实现见 app_light.h）
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"

#include "app_light.h"
#include "app_settings.h"

static const char *TAG = "app_light";

static bool s_on = false;
static uint8_t s_brightness = 0;      /* 当前亮度（0 = 灭） */
static bool s_ready = false;

/** 亮度百分比 -> LEDC 占空比（按有效电平台反相） */
static uint32_t light_duty_from_percent(uint8_t percent)
{
    const uint32_t max_duty = (1u << LIGHT_LEDC_RES) - 1u;   /* 10bit -> 1023 */
    uint32_t duty = (max_duty * (uint32_t)percent) / 100u;
#if !LIGHT_ACTIVE_LEVEL
    duty = max_duty - duty;   /* 低电平点亮的模块，占空比要反相 */
#endif
    return duty;
}

/** 把当前 (s_on, s_brightness) 真正写到硬件上 */
static void light_apply(void)
{
    if (!s_ready) {
        return;
    }
    const uint32_t duty = light_duty_from_percent(s_on ? s_brightness : 0);
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LIGHT_LEDC_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LIGHT_LEDC_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc duty update failed: %s", esp_err_to_name(err));
    }
}

esp_err_t app_light_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* --- LEDC 定时器 + 通道 ---
     * 注意：TIMER_0 / CHANNEL_1 是被特意错开背光（TIMER_1 / CHANNEL_0）的，
     * 改这里之前先看 app_light.h 的说明。 */
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LIGHT_LEDC_RES,
        .timer_num = LIGHT_LEDC_TIMER,
        .freq_hz = LIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc_timer_config failed");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = LIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LIGHT_LEDC_CHANNEL,
        .timer_sel = LIGHT_LEDC_TIMER,
        .duty = light_duty_from_percent(0),   /* 上电先灭 */
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "ledc_channel_config failed");
    s_ready = true;

    /* --- 从 NVS 恢复上次状态 --- */
    const app_settings_t set = app_settings_get();
    s_brightness = set.light_brightness;
    if (s_brightness == 0) {
        s_brightness = set.light_last_on;
    }
    s_on = (set.light_on != 0) && (s_brightness > 0);
    light_apply();

    ESP_LOGI(TAG, "Light on GPIO%d (%s active), LEDC %dHz %dbit -> %s at %u%%",
             (int)LIGHT_GPIO, LIGHT_ACTIVE_LEVEL ? "HIGH" : "LOW",
             LIGHT_LEDC_FREQ_HZ, LIGHT_LEDC_RES, s_on ? "ON" : "OFF", s_brightness);
    return ESP_OK;
}

void app_light_set(bool on)
{
    if (on && s_brightness == 0) {
        /* 亮度是 0 时"开灯"没有意义：先恢复到上次的非零亮度 */
        s_brightness = app_settings_get().light_last_on;
    }
    s_on = on;
    light_apply();
    app_settings_set_light(s_on, s_brightness);
    ESP_LOGI(TAG, "Light %s (%u%%)", s_on ? "ON" : "OFF", s_brightness);
}

void app_light_toggle(void)
{
    app_light_set(!s_on);
}

void app_light_set_brightness(uint8_t percent)
{
    if (percent > LIGHT_BRIGHTNESS_MAX) {
        percent = LIGHT_BRIGHTNESS_MAX;
    }
    s_brightness = percent;
    /* 拖到 0 = 灭灯；从 0 往上拖 = 自动开灯。这样"亮度"和"开关"永远不矛盾 */
    s_on = (percent > 0);
    light_apply();
    app_settings_set_light(s_on, s_brightness);
}

bool app_light_is_on(void)
{
    return s_on;
}

uint8_t app_light_get_brightness(void)
{
    return s_brightness;
}
