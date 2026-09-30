/**
 * @file app_settings.c
 * @brief 界面偏好设置的 nvs_flash 读写（实现见 app_settings.h 的说明）
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_settings.h"

static const char *TAG = "app_settings";

/* 整份设置存成一个 blob：字段少、改动频繁、读的时候要一起用，比逐键存简单可靠 */
#define SET_NVS_KEY_BLOB        "prefs"

/* 出厂默认：亮度与自动熄屏按界面稿 ui5_settings.png（70% / 30s） */
#define SET_DEF_BRIGHTNESS      (70)
#define SET_DEF_TEMP_UNIT       (APP_TEMP_UNIT_C)
#define SET_DEF_AUTO_OFF_EN     (1)
#define SET_DEF_AUTO_OFF_S      (APP_AUTO_OFF_DEFAULT_S)
#define SET_DEF_LIGHT_ON        (0)     /* 上电默认关灯：现场更安全 */
#define SET_DEF_LIGHT_BRIGHT    (80)

static app_settings_t s_set;
static QueueHandle_t s_save_q = NULL;

/*==============================================================================
 * 异步落盘
 *============================================================================*/
static void settings_save_now(const app_settings_t *set)
{
    nvs_handle_t nh;
    if (nvs_open(APP_SET_NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(" APP_SET_NVS_NAMESPACE ") failed");
        return;
    }
    esp_err_t err = nvs_set_blob(nh, SET_NVS_KEY_BLOB, set, sizeof(*set));
    if (err == ESP_OK) {
        err = nvs_commit(nh);
    }
    nvs_close(nh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save prefs failed: %s", esp_err_to_name(err));
    }
}

static void settings_save_task(void *arg)
{
    (void)arg;
    app_settings_t set;

    for (;;) {
        if (xQueueReceive(s_save_q, &set, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        settings_save_now(&set);
        ESP_LOGD(TAG, "prefs committed (bright=%u unit=%u auto_off=%u/%us light=%u/%u)",
                 set.disp_brightness, set.temp_unit, set.auto_off_en, set.auto_off_s,
                 set.light_on, set.light_brightness);
    }
}

esp_err_t app_settings_save_async(void)
{
    if (s_save_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 覆盖式入队：连续拖滑条只写最后一次 */
    return (xQueueOverwrite(s_save_q, &s_set) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
static app_settings_t settings_defaults(void)
{
    app_settings_t s = {
        .disp_brightness = SET_DEF_BRIGHTNESS,
        .temp_unit = SET_DEF_TEMP_UNIT,
        .auto_off_en = SET_DEF_AUTO_OFF_EN,
        .auto_off_s = SET_DEF_AUTO_OFF_S,
        .light_on = SET_DEF_LIGHT_ON,
        .light_brightness = SET_DEF_LIGHT_BRIGHT,
        .light_last_on = SET_DEF_LIGHT_BRIGHT,
    };
    return s;
}

/** 把载入的值夹到合法范围内（防止 NVS 里是旧版本/被改坏的脏数据） */
static void settings_sanitize(app_settings_t *s)
{
    if (s->disp_brightness < 10 || s->disp_brightness > 100) {
        s->disp_brightness = SET_DEF_BRIGHTNESS;
    }
    if (s->temp_unit != APP_TEMP_UNIT_C && s->temp_unit != APP_TEMP_UNIT_F) {
        s->temp_unit = APP_TEMP_UNIT_C;
    }
    if (s->auto_off_en > 1) {
        s->auto_off_en = 1;
    }
    /* 时长必须是选项表里的值，否则 UI 上"点一下轮换"会跳得莫名其妙 */
    static const uint16_t choices[] = APP_AUTO_OFF_CHOICES;
    bool ok = false;
    for (int i = 0; i < APP_AUTO_OFF_CHOICE_COUNT; i++) {
        if (s->auto_off_s == choices[i]) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        s->auto_off_s = APP_AUTO_OFF_DEFAULT_S;
    }
    if (s->light_brightness > 100) {
        s->light_brightness = SET_DEF_LIGHT_BRIGHT;
    }
    if (s->light_last_on == 0 || s->light_last_on > 100) {
        s->light_last_on = SET_DEF_LIGHT_BRIGHT;
    }
    if (s->light_on > 1) {
        s->light_on = 0;
    }
}

esp_err_t app_settings_init(void)
{
    /* NVS 通常已由 app_config_init() 初始化过；重复 init 返回 ESP_OK，
     * 这里只补上"首次上电 + 坏页"的情况。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), erasing...", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    s_set = settings_defaults();

    nvs_handle_t nh;
    ESP_RETURN_ON_ERROR(nvs_open(APP_SET_NVS_NAMESPACE, NVS_READWRITE, &nh), TAG,
                        "nvs_open failed");

    size_t len = sizeof(s_set);
    app_settings_t loaded;
    err = nvs_get_blob(nh, SET_NVS_KEY_BLOB, &loaded, &len);
    if (err == ESP_OK && len == sizeof(loaded)) {
        s_set = loaded;
        settings_sanitize(&s_set);
    } else {
        /* 首次上电 / 旧数据尺寸不符：写一份默认值进去 */
        ESP_LOGI(TAG, "prefs not found (%s), writing defaults", esp_err_to_name(err));
        nvs_set_blob(nh, SET_NVS_KEY_BLOB, &s_set, sizeof(s_set));
        nvs_commit(nh);
    }
    nvs_close(nh);

    ESP_LOGI(TAG, "Prefs loaded: brightness=%u%% unit=%s auto_off=%s(%us) light=%s/%u%%",
             s_set.disp_brightness, app_settings_temp_unit_text(),
             s_set.auto_off_en ? "on" : "off", (unsigned)s_set.auto_off_s,
             s_set.light_on ? "on" : "off", s_set.light_brightness);

    if (s_save_q == NULL) {
        s_save_q = xQueueCreate(1, sizeof(app_settings_t));
        ESP_RETURN_ON_FALSE(s_save_q != NULL, ESP_ERR_NO_MEM, TAG, "save queue create failed");
        BaseType_t ok = xTaskCreate(settings_save_task, "set_save", 4096, NULL, 3, NULL);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "save task create failed");
    }
    return ESP_OK;
}

/*==============================================================================
 * 访问与修改
 *============================================================================*/
app_settings_t app_settings_get(void)
{
    return s_set;
}

void app_settings_set_disp_brightness(uint8_t percent)
{
    if (percent < 10) {
        percent = 10;      /* 低于 10% 现场基本看不清，当"熄屏"用自动熄屏功能 */
    }
    if (percent > 100) {
        percent = 100;
    }
    s_set.disp_brightness = percent;
    app_settings_save_async();
}

void app_settings_set_temp_unit(app_temp_unit_t unit)
{
    s_set.temp_unit = (unit == APP_TEMP_UNIT_F) ? APP_TEMP_UNIT_F : APP_TEMP_UNIT_C;
    app_settings_save_async();
}

void app_settings_set_auto_off(bool enabled, uint16_t seconds)
{
    s_set.auto_off_en = enabled ? 1 : 0;
    if (seconds >= 15 && seconds <= 3600) {
        s_set.auto_off_s = seconds;
    }
    app_settings_save_async();
}

void app_settings_set_light(bool on, uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }
    s_set.light_brightness = brightness;
    if (brightness > 0) {
        s_set.light_last_on = brightness;
    }
    s_set.light_on = on ? 1 : 0;
    app_settings_save_async();
}

float app_settings_to_display_temp(float celsius)
{
    return (s_set.temp_unit == APP_TEMP_UNIT_F) ? (celsius * 9.0f / 5.0f + 32.0f) : celsius;
}

const char *app_settings_temp_unit_text(void)
{
    return (s_set.temp_unit == APP_TEMP_UNIT_F) ? "F" : "C";
}

const char *app_settings_auto_off_text(uint16_t seconds)
{
    static char buf[12];
    if (seconds < 60) {
        snprintf(buf, sizeof(buf), "%us", (unsigned)seconds);
    } else {
        snprintf(buf, sizeof(buf), "%umin", (unsigned)(seconds / 60));
    }
    return buf;
}

/*==============================================================================
 * 恢复出厂设置
 *============================================================================*/
static void factory_reset_task(void *arg)
{
    (void)arg;
    /* 给 UI 一点时间把"Resetting..."提示画出来（此时屏幕还在，用户看得到） */
    vTaskDelay(pdMS_TO_TICKS(1200));

    ESP_LOGW(TAG, "Factory reset: erasing NVS and restarting");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(150));   /* 让日志先出去 */
    esp_restart();
}

void app_settings_factory_reset(void)
{
    /* 不在 LVGL 任务里擦 NVS（上百 ms 的阻塞），交给一次性任务 */
    BaseType_t ok = xTaskCreate(factory_reset_task, "fac_rst", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "factory reset task create failed");
    }
}
