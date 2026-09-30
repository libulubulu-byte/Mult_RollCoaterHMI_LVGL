/**
 * @file app_sensor.c
 * @brief SHT30/SHT31 温湿度采集（实现与硬件说明见 app_sensor.h）
 *
 * 时序（SHT30 datasheet）：
 *   高重复性 + 非时钟拉伸 命令 0x2400 -> 等待测量（最大 15ms，这里等 20ms）
 *   -> 读 6 字节：T[2] TCRC H[2] HCRC
 *   换算：T = -45 + 175 * raw / 65535 ；RH = 100 * raw / 65535
 */
#include <string.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

#include "app_sensor.h"
#include "bsp_display.h"     /* bsp_display_i2c_bus()：复用触摸那条 I2C0 */

static const char *TAG = "app_sensor";

/* 连续失败几次才把读数标成无效（避免偶发一次读错就让界面闪 "--"） */
#define SENSOR_FAIL_TOLERANCE   (3)
/* 没探测到传感器时的重试间隔（支持上电后再插上） */
#define SENSOR_RETRY_MS         (5000)

static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t s_lock = NULL;

/* 以下数据全部受 s_lock 保护 */
static app_sensor_data_t s_data;
static sensor_sample_t s_hist[SENSOR_HIST_POINTS];
static uint32_t s_hist_head = 0;      /* 下一个写入位置 */
static uint32_t s_hist_count = 0;
static float s_t_sum = 0.0f;          /* 环形缓冲内所有点的累加，用于算平均 */
static float s_h_sum = 0.0f;

/* 采样任务私有状态（不需要锁） */
static int s_last_day = -1;           /* 上次采样时的 tm_yday，用于"今日最值"跨天清零 */
static int s_fail_streak = 0;

/*==============================================================================
 * SHT30 底层
 *============================================================================*/
/** CRC-8：多项式 0x31、初值 0xFF（SHT3x 数据手册 4.4 节） */
static uint8_t sht30_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/**
 * @brief 读一次温湿度
 * @note 所有传输都用有限超时：这条总线上还有触摸驱动（它用无限超时），
 *       万一总线被拖住，本函数只会返回超时，不会永久卡死采样任务。
 */
static esp_err_t sht30_read(float *temp_c, float *hum_pct)
{
    uint8_t cmd[2] = { 0x24, 0x00 };     /* 高重复性、非时钟拉伸 */
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, cmd, sizeof(cmd), SENSOR_IO_TIMEOUT_MS),
                        TAG, "measure cmd failed");
    /* 非时钟拉伸模式：命令发完就开始测，必须等测量完成才能读 */
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t raw[6] = { 0 };
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_dev, raw, sizeof(raw), SENSOR_IO_TIMEOUT_MS),
                        TAG, "read failed");

    if (sht30_crc8(&raw[0], 2) != raw[2] || sht30_crc8(&raw[3], 2) != raw[5]) {
        ESP_RETURN_ON_ERROR(ESP_ERR_INVALID_CRC, TAG, "crc error (raw %02X%02X/%02X %02X%02X/%02X)",
                            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    }

    const uint16_t t_raw = (uint16_t)((raw[0] << 8) | raw[1]);
    const uint16_t h_raw = (uint16_t)((raw[3] << 8) | raw[4]);
    *temp_c = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    *hum_pct = 100.0f * (float)h_raw / 65535.0f;
    return ESP_OK;
}

/*==============================================================================
 * 统计
 *============================================================================*/
/** 往环形缓冲压一个点，并重算 24h 统计（点数少，直接遍历，5 分钟才做一次） */
static void history_push(float temp_c, float hum_pct)
{
    sensor_sample_t *slot = &s_hist[s_hist_head];

    if (s_hist_count == SENSOR_HIST_POINTS) {
        /* 满了：把将被覆盖的那个点从累加和里扣掉 */
        s_t_sum -= slot->temp_c;
        s_h_sum -= slot->hum_pct;
    } else {
        s_hist_count++;
    }

    slot->temp_c = temp_c;
    slot->hum_pct = hum_pct;
    s_t_sum += temp_c;
    s_h_sum += hum_pct;
    s_hist_head = (s_hist_head + 1) % SENSOR_HIST_POINTS;

    float t_min = 1e9f, t_max = -1e9f, h_min = 1e9f, h_max = -1e9f;
    for (uint32_t i = 0; i < s_hist_count; i++) {
        const sensor_sample_t *p = &s_hist[i];
        if (p->temp_c < t_min) t_min = p->temp_c;
        if (p->temp_c > t_max) t_max = p->temp_c;
        if (p->hum_pct < h_min) h_min = p->hum_pct;
        if (p->hum_pct > h_max) h_max = p->hum_pct;
    }

    s_data.t_avg24 = s_t_sum / (float)s_hist_count;
    s_data.h_avg24 = s_h_sum / (float)s_hist_count;
    s_data.t_min24 = t_min;
    s_data.t_max24 = t_max;
    s_data.h_min24 = h_min;
    s_data.h_max24 = h_max;
    s_data.hist_count = s_hist_count;
}

/** 今日最值：系统时间可用时，跨天自动清零（时间不可用就只在上电时清零） */
static bool day_changed(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    if (now <= 0 || localtime_r(&now, &tm_now) == NULL || (tm_now.tm_year + 1900) < 2020) {
        return false;   /* 没对时：不跨天，保持上电以来的最值 */
    }
    if (tm_now.tm_yday != s_last_day) {
        bool first = (s_last_day < 0);
        s_last_day = tm_now.tm_yday;
        return !first;   /* 第一次只是记录当天，不算"跨天" */
    }
    return false;
}

/*==============================================================================
 * 采样任务
 *============================================================================*/
static bool sensor_probe(void)
{
    i2c_master_bus_handle_t bus = bsp_display_i2c_bus();
    if (bus == NULL) {
        return false;
    }
    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = SENSOR_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
            ESP_LOGE(TAG, "add device 0x%02X on I2C0 failed", SENSOR_I2C_ADDR);
            return false;
        }
    }
    return (i2c_master_probe(bus, SENSOR_I2C_ADDR, SENSOR_IO_TIMEOUT_MS) == ESP_OK);
}

static void sensor_task(void *arg)
{
    (void)arg;

    bool present = sensor_probe();
    if (!present) {
        ESP_LOGW(TAG, "SHT3x not answering on 0x%02X (I2C0 SDA=19 SCL=20). "
                      "ENVIRONMENT/HISTORY will show \"--\" until it appears",
                 SENSOR_I2C_ADDR);
    } else {
        ESP_LOGI(TAG, "SHT3x found at 0x%02X on I2C0", SENSOR_I2C_ADDR);
    }

    int64_t last_hist_us = 0;
    int64_t last_probe_us = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
        const int64_t now_us = esp_timer_get_time();

        if (!present) {
            /* 每 5 秒重试一次探测：支持"先上电、后插传感器" */
            if (now_us - last_probe_us >= (int64_t)SENSOR_RETRY_MS * 1000) {
                last_probe_us = now_us;
                present = sensor_probe();
                if (present) {
                    ESP_LOGI(TAG, "SHT3x appeared at 0x%02X", SENSOR_I2C_ADDR);
                }
            }
            continue;
        }

        float t = 0.0f, h = 0.0f;
        esp_err_t err = sht30_read(&t, &h);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (err == ESP_OK && t > -60.0f && t < 150.0f && h >= 0.0f && h <= 100.0f) {
            s_data.ok_count++;
            s_data.temp_c = t;
            s_data.hum_pct = h;
            s_data.present = true;

            /* 今日最值 */
            if (!s_data.today_valid || day_changed()) {
                s_data.t_min = s_data.t_max = t;
                s_data.h_min = s_data.h_max = h;
                s_data.today_valid = true;
                if (s_data.today_valid && s_last_day >= 0) {
                    ESP_LOGI(TAG, "Today min/max reset (new day)");
                }
            } else {
                if (t < s_data.t_min) s_data.t_min = t;
                if (t > s_data.t_max) s_data.t_max = t;
                if (h < s_data.h_min) s_data.h_min = h;
                if (h > s_data.h_max) s_data.h_max = h;
            }

            /* 历史点：每 5 分钟一个（第一个点在启动后立刻压入，曲线不至于空着） */
            if (last_hist_us == 0 ||
                    now_us - last_hist_us >= (int64_t)SENSOR_HIST_PERIOD_S * 1000000) {
                last_hist_us = now_us;
                history_push(t, h);
            }

            s_fail_streak = 0;
            s_data.valid = true;
        } else {
            s_data.err_count++;
            s_fail_streak++;
            if (s_fail_streak == 1 || s_fail_streak == SENSOR_FAIL_TOLERANCE) {
                ESP_LOGW(TAG, "read failed (%d in a row): %s", s_fail_streak,
                         esp_err_to_name(err));
            }
            if (s_fail_streak >= SENSOR_FAIL_TOLERANCE) {
                s_data.valid = false;   /* 连错 3 次才让界面显示 "--" */
            }
            if (s_data.err_count > 0 && (s_data.err_count % 50) == 0) {
                present = sensor_probe();   /* 偶尔整条总线掉线时重新挂载 */
            }
        }
        xSemaphoreGive(s_lock);
    }
}

/*==============================================================================
 * 对外接口
 *============================================================================*/
esp_err_t app_sensor_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    if (bsp_display_i2c_bus() == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready: call bsp_display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex create failed");

    memset(&s_data, 0, sizeof(s_data));
    memset(s_hist, 0, sizeof(s_hist));

    /* 采样任务：优先级低于 motion(5)、高于落盘(3)。
     * 栈给 4KB：里面要跑 i2c 传输 + 一次 288 点的遍历。 */
    BaseType_t ok = xTaskCreate(sensor_task, "sensor", 4096, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "sensor task create failed");

    ESP_LOGI(TAG, "Sensor task started: SHT3x @0x%02X on I2C0, every %dms, history every %ds (%d points)",
             SENSOR_I2C_ADDR, SENSOR_PERIOD_MS, SENSOR_HIST_PERIOD_S, SENSOR_HIST_POINTS);
    return ESP_OK;
}

bool app_sensor_get(app_sensor_data_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return false;
    }
    /* 5ms 上限：采样任务只持锁做几次赋值/一次 memcpy，等锁时间几乎为 0；
     * 给个上限是为了绝不在 LVGL 任务里卡住（红线 10ms）。 */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_data;
    xSemaphoreGive(s_lock);
    return true;
}

uint32_t app_sensor_history(sensor_sample_t *out, uint32_t max_points)
{
    if (out == NULL || max_points == 0 || s_lock == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }

    uint32_t n = s_hist_count;
    if (n > max_points) {
        n = max_points;
    }
    /* 最老的点：环形缓冲里"下一个要写的位置"往回退 count 个 */
    const uint32_t oldest = (s_hist_head + SENSOR_HIST_POINTS - s_hist_count) % SENSOR_HIST_POINTS;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_hist[(oldest + i) % SENSOR_HIST_POINTS];
    }

    xSemaphoreGive(s_lock);
    return n;
}
