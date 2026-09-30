/**
 * @file app_wifi.c
 * @brief WiFi 配网实现（说明见 app_wifi.h）
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_wifi.h"

static const char *TAG = "app_wifi";

#define WIFI_NVS_NAMESPACE   "rc_wifi"
#define WIFI_NVS_KEY_SSID    "ssid"
#define WIFI_NVS_KEY_PASS    "pass"

#define WIFI_RETRY_MAX       (0)      /* 0 = 无限重试（掉线自动重连） */
#define WIFI_RETRY_DELAY_MS  (5000)
#define SNTP_SERVER          "pool.ntp.org"
#define SNTP_TZ              "CST-8"  /* 中国标准时间（UTC+8），改时区改这里 */

/* wifi 任务的命令 */
typedef enum {
    WIFI_CMD_SCAN = 0,
    WIFI_CMD_CONNECT,
    WIFI_CMD_FORGET,
} wifi_cmd_t;

typedef struct {
    wifi_cmd_t cmd;
    char ssid[APP_WIFI_SSID_LEN];
    char pass[APP_WIFI_PASS_LEN];
} wifi_cmd_msg_t;

static esp_netif_t *s_netif = NULL;
static QueueHandle_t s_cmd_q = NULL;
static QueueHandle_t s_evt_q = NULL;
static QueueHandle_t s_ap_lock = NULL;   /* 保护 s_ap_list（wifi 任务写、UI 读） */

static app_wifi_state_t s_state = APP_WIFI_STATE_NO_CREDS;
static char s_ssid[APP_WIFI_SSID_LEN];
static char s_pass[APP_WIFI_PASS_LEN];
static char s_ip[16];
static volatile bool s_scan_busy = false;
static int s_retry = 0;
static esp_timer_handle_t s_retry_timer = NULL;

static app_wifi_ap_t s_ap_list[APP_WIFI_MAX_AP];
static uint32_t s_ap_count = 0;

/*==============================================================================
 * 事件投递（给 UI 轮询）
 *============================================================================*/
static void wifi_post_event(app_wifi_evt_id_t id, int value)
{
    if (s_evt_q == NULL) {
        return;
    }
    const app_wifi_evt_t evt = { .id = id, .value = value };
    /* 非阻塞：队列满了就丢（UI 下一轮会自己读状态，不会因此丢关键信息） */
    (void)xQueueSend(s_evt_q, &evt, 0);
}

bool app_wifi_poll_event(app_wifi_evt_t *out)
{
    if (out == NULL || s_evt_q == NULL) {
        return false;
    }
    return xQueueReceive(s_evt_q, out, 0) == pdTRUE;
}

/*==============================================================================
 * NVS 凭证
 *============================================================================*/
static bool wifi_load_creds(void)
{
    nvs_handle_t nh;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_ssid);
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    esp_err_t err = nvs_get_str(nh, WIFI_NVS_KEY_SSID, s_ssid, &len);
    if (err == ESP_OK && s_ssid[0] != '\0') {
        len = sizeof(s_pass);
        if (nvs_get_str(nh, WIFI_NVS_KEY_PASS, s_pass, &len) != ESP_OK) {
            s_pass[0] = '\0';   /* 开放网络没存密码也算正常 */
        }
        nvs_close(nh);
        return true;
    }
    nvs_close(nh);
    return false;
}

static void wifi_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nh;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for creds failed");
        return;
    }
    nvs_set_str(nh, WIFI_NVS_KEY_SSID, ssid);
    nvs_set_str(nh, WIFI_NVS_KEY_PASS, pass ? pass : "");
    esp_err_t err = nvs_commit(nh);
    nvs_close(nh);
    ESP_LOGI(TAG, "WiFi credentials saved (ssid=%s, %s)", ssid, esp_err_to_name(err));
}

static void wifi_clear_creds(void)
{
    nvs_handle_t nh;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) {
        return;
    }
    nvs_erase_key(nh, WIFI_NVS_KEY_SSID);
    nvs_erase_key(nh, WIFI_NVS_KEY_PASS);
    nvs_commit(nh);
    nvs_close(nh);
    ESP_LOGI(TAG, "WiFi credentials erased");
}

/*==============================================================================
 * SNTP
 *============================================================================*/
static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP synced: %s", ctime(&tv->tv_sec));   /* ctime 自带换行 */
    wifi_post_event(APP_WIFI_EVT_TIME_SYNCED, 0);
}

static void sntp_start(void)
{
    setenv("TZ", SNTP_TZ, 1);
    tzset();

    if (esp_sntp_enabled()) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (%s, TZ=%s)", SNTP_SERVER, SNTP_TZ);
}

/*==============================================================================
 * 事件处理
 *============================================================================*/
static void wifi_retry_cb(void *arg)
{
    (void)arg;
    if (s_ssid[0] == '\0') {
        return;
    }
    ESP_LOGI(TAG, "Reconnecting to '%s' (retry %d)", s_ssid, s_retry);
    s_state = APP_WIFI_STATE_CONNECTING;
    esp_wifi_connect();
}

/**
 * @brief 把字符串拷进定长字段（自动截断 + 补 0）
 * @note 这里不能用 snprintf：esp_wifi 的 ssid/password 是定长数组，
 *       源串长度与目标长度同量级时，GCC 的 -Werror=format-truncation 会直接报错。
 */
static void wifi_copy_str(uint8_t *dst, size_t dst_size, const char *src)
{
    memset(dst, 0, dst_size);
    size_t n = (src != NULL) ? strlen(src) : 0;
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_retry++;
        /* reason 201 = 密码错/认证失败；15 = 找不到 AP；205 = 掉线 */
        ESP_LOGW(TAG, "Disconnected from '%s' (reason=%d, retry=%d)", s_ssid, d->reason, s_retry);

        s_ip[0] = '\0';
        s_state = (s_retry > 2) ? APP_WIFI_STATE_FAILED : APP_WIFI_STATE_CONNECTING;
        wifi_post_event(APP_WIFI_EVT_DISCONNECTED, d->reason);

        if (s_retry > 8) {
            /* 连 8 次还不行就别刷日志了，等用户改配置 */
            wifi_post_event(APP_WIFI_EVT_CONNECT_FAILED, d->reason);
            return;
        }
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)WIFI_RETRY_DELAY_MS * 1000);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_state = APP_WIFI_STATE_CONNECTED;
        s_retry = 0;
        ESP_LOGI(TAG, "Connected to '%s', IP=%s", s_ssid, s_ip);
        wifi_post_event(APP_WIFI_EVT_CONNECTED, 0);
        sntp_start();     /* 连上网就顺便对一次时：主菜单时钟从此是真的 */
        return;
    }
}

/*==============================================================================
 * WiFi 任务：干"会阻塞"的活（扫描 / 连 NVS / 发起连接）
 *============================================================================*/
static void wifi_task(void *arg)
{
    (void)arg;
    wifi_cmd_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_cmd_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.cmd) {
        case WIFI_CMD_SCAN: {
            ESP_LOGI(TAG, "Scanning...");
            wifi_scan_config_t scan_cfg = { .show_hidden = 0 };
            esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);   /* 阻塞 1~3s */
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
                s_scan_busy = false;
                wifi_post_event(APP_WIFI_EVT_SCAN_DONE, 0);
                break;
            }

            uint16_t num = APP_WIFI_MAX_AP;
            wifi_ap_record_t recs[APP_WIFI_MAX_AP];
            memset(recs, 0, sizeof(recs));
            if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
                num = 0;
            }
            esp_wifi_clear_ap_list();

            /* 按信号强度从强到弱排（现场先看见自家的路由器） */
            for (uint16_t i = 0; i < num; i++) {
                for (uint16_t j = i + 1; j < num; j++) {
                    if (recs[j].rssi > recs[i].rssi) {
                        wifi_ap_record_t t = recs[i];
                        recs[i] = recs[j];
                        recs[j] = t;
                    }
                }
            }

            xSemaphoreTake(s_ap_lock, portMAX_DELAY);
            s_ap_count = (num > APP_WIFI_MAX_AP) ? APP_WIFI_MAX_AP : num;
            for (uint32_t i = 0; i < s_ap_count; i++) {
                snprintf(s_ap_list[i].ssid, sizeof(s_ap_list[i].ssid), "%s",
                         (const char *)recs[i].ssid);
                s_ap_list[i].rssi = recs[i].rssi;
                s_ap_list[i].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
            }
            xSemaphoreGive(s_ap_lock);

            s_scan_busy = false;
            ESP_LOGI(TAG, "Scan done: %u APs (strongest '%s' %ddBm)", (unsigned)s_ap_count,
                     s_ap_count ? s_ap_list[0].ssid : "-", s_ap_count ? s_ap_list[0].rssi : 0);
            wifi_post_event(APP_WIFI_EVT_SCAN_DONE, (int)s_ap_count);
            break;
        }

        case WIFI_CMD_CONNECT: {
            snprintf(s_ssid, sizeof(s_ssid), "%s", msg.ssid);
            snprintf(s_pass, sizeof(s_pass), "%s", msg.pass);
            wifi_save_creds(s_ssid, s_pass);

            wifi_config_t wc;
            memset(&wc, 0, sizeof(wc));
            wifi_copy_str(wc.sta.ssid, sizeof(wc.sta.ssid), s_ssid);
            wifi_copy_str(wc.sta.password, sizeof(wc.sta.password), s_pass);
            wc.sta.threshold.authmode = (s_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

            s_retry = 0;
            esp_wifi_disconnect();
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wc));
            s_state = APP_WIFI_STATE_CONNECTING;
            ESP_LOGI(TAG, "Connecting to '%s'...", s_ssid);
            esp_wifi_connect();
            break;
        }

        case WIFI_CMD_FORGET:
            esp_wifi_disconnect();
            wifi_clear_creds();
            s_ssid[0] = '\0';
            s_pass[0] = '\0';
            s_ip[0] = '\0';
            s_state = APP_WIFI_STATE_NO_CREDS;
            ESP_LOGI(TAG, "WiFi forgotten");
            break;

        default:
            break;
        }
    }
}

/*==============================================================================
 * 初始化
 *============================================================================*/
esp_err_t app_wifi_init(void)
{
    if (s_cmd_q != NULL) {
        return ESP_OK;
    }

    s_evt_q = xQueueCreate(8, sizeof(app_wifi_evt_t));
    s_cmd_q = xQueueCreate(4, sizeof(wifi_cmd_msg_t));
    s_ap_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_evt_q && s_cmd_q && s_ap_lock, ESP_ERR_NO_MEM, TAG,
                        "queue/mutex create failed");

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "esp_netif_init failed");
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop create failed");
    }

    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_FAIL, TAG, "create default wifi sta failed");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init failed");

    /* 自己管凭证（NVS rc_wifi）：让 WiFi 驱动别再自己存一份 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                           wifi_event_handler, NULL, NULL),
                        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                           wifi_event_handler, NULL, NULL),
                        TAG, "register IP_EVENT failed");

    const esp_timer_create_args_t retry_args = { .callback = wifi_retry_cb, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "retry timer failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    BaseType_t ok = xTaskCreate(wifi_task, "wifi", 4096, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "wifi task create failed");

    if (wifi_load_creds()) {
        ESP_LOGI(TAG, "Saved network found: '%s' -> connecting", s_ssid);
        s_state = APP_WIFI_STATE_CONNECTING;
        wifi_cmd_msg_t msg = { .cmd = WIFI_CMD_CONNECT };
        snprintf(msg.ssid, sizeof(msg.ssid), "%s", s_ssid);
        snprintf(msg.pass, sizeof(msg.pass), "%s", s_pass);
        xQueueSend(s_cmd_q, &msg, 0);
    } else {
        s_state = APP_WIFI_STATE_NO_CREDS;
        ESP_LOGI(TAG, "No saved network: open SETTINGS -> WiFi -> Configure to provision");
    }
    return ESP_OK;
}

/*==============================================================================
 * 状态查询
 *============================================================================*/
app_wifi_state_t app_wifi_state(void)
{
    return s_state;
}

bool app_wifi_is_connected(void)
{
    return s_state == APP_WIFI_STATE_CONNECTED;
}

bool app_wifi_has_creds(void)
{
    return s_ssid[0] != '\0';
}

const char *app_wifi_ssid(void)
{
    return s_ssid;
}

const char *app_wifi_ip(void)
{
    return s_ip;
}

int8_t app_wifi_rssi(void)
{
    if (s_state != APP_WIFI_STATE_CONNECTED) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

const char *app_wifi_state_text(void)
{
    static char buf[48];
    switch (s_state) {
    case APP_WIFI_STATE_CONNECTED:
        snprintf(buf, sizeof(buf), "%s", s_ssid);
        break;
    case APP_WIFI_STATE_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting...");
        break;
    case APP_WIFI_STATE_FAILED:
        snprintf(buf, sizeof(buf), "Failed (%s)", s_ssid[0] ? s_ssid : "-");
        break;
    default:
        snprintf(buf, sizeof(buf), "Not configured");
        break;
    }
    return buf;
}

esp_err_t app_wifi_scan_start(void)
{
    if (s_cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_busy = true;
    const wifi_cmd_msg_t msg = { .cmd = WIFI_CMD_SCAN };
    if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE) {
        s_scan_busy = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool app_wifi_scan_busy(void)
{
    return s_scan_busy;
}

uint32_t app_wifi_ap_list(app_wifi_ap_t *out, uint32_t max)
{
    if (out == NULL || max == 0 || s_ap_lock == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_ap_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    uint32_t n = (s_ap_count < max) ? s_ap_count : max;
    memcpy(out, s_ap_list, n * sizeof(app_wifi_ap_t));
    xSemaphoreGive(s_ap_lock);
    return n;
}

esp_err_t app_wifi_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0' || s_cmd_q == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_cmd_msg_t msg = { .cmd = WIFI_CMD_CONNECT };
    snprintf(msg.ssid, sizeof(msg.ssid), "%s", ssid);
    snprintf(msg.pass, sizeof(msg.pass), "%s", pass ? pass : "");
    /* 连 NVS 写一起交给 wifi 任务，UI 任务立刻返回 */
    return (xQueueSend(s_cmd_q, &msg, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_wifi_forget(void)
{
    if (s_cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const wifi_cmd_msg_t msg = { .cmd = WIFI_CMD_FORGET };
    return (xQueueSend(s_cmd_q, &msg, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
