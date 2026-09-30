/**
 * @file app_wifi.h
 * @brief WiFi 配网（STA 扫描 / 连接 / 记住凭证）+ SNTP 对时
 *
 * 范围（按需求：**只配网**）：把设备连上家里的 2.4G 路由器，拿到 IP，
 * 顺便用 SNTP 对一次时（主菜单顶栏的时钟因此从 "--:--" 变成真实时间）。
 * 不做 MQTT / 云平台 / 远程控制。
 *
 * 线程模型：
 *   - 扫描 `esp_wifi_scan_start(..., true)` 是**阻塞**的（1~3 秒），绝不能在
 *     LVGL 任务里调；本模块内部有一个 wifi 任务消费命令队列来干这件事。
 *   - 状态变化通过一个事件队列抛给 UI 轮询（与 app_state 的做法一致），
 *     UI 不需要注册任何回调。
 *   - 凭证存在 NVS（命名空间 rc_wifi），并设 esp_wifi_set_storage(WIFI_STORAGE_RAM)，
 *     避免 WiFi 驱动自己也存一份（否则"忘记网络"要清两处）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_WIFI_MAX_AP      (16)     /* 扫描列表最多保留几个 */
#define APP_WIFI_SSID_LEN    (33)
#define APP_WIFI_PASS_LEN    (65)

/** 连接状态 */
typedef enum {
    APP_WIFI_STATE_NO_CREDS = 0,   /*!< 还没配过网（NVS 里没有凭证） */
    APP_WIFI_STATE_CONNECTING,     /*!< 正在连 */
    APP_WIFI_STATE_CONNECTED,      /*!< 已连上并拿到 IP */
    APP_WIFI_STATE_FAILED,         /*!< 连不上（密码错 / 信号差 / 路由器不在） */
} app_wifi_state_t;

/** 扫描到的一个热点 */
typedef struct {
    char    ssid[APP_WIFI_SSID_LEN];
    int8_t  rssi;
    bool    secure;                /*!< 需要密码（authmode != OPEN） */
} app_wifi_ap_t;

/** 抛给 UI 的事件 */
typedef enum {
    APP_WIFI_EVT_SCAN_DONE = 0,    /*!< 扫描完成，可以去取 ap 列表 */
    APP_WIFI_EVT_CONNECTED,        /*!< 已连接（拿到 IP） */
    APP_WIFI_EVT_DISCONNECTED,     /*!< 掉线（会自动重连） */
    APP_WIFI_EVT_CONNECT_FAILED,   /*!< 重试若干次仍失败 */
    APP_WIFI_EVT_TIME_SYNCED,      /*!< SNTP 对时完成（主菜单时钟可用了） */
} app_wifi_evt_id_t;

typedef struct {
    app_wifi_evt_id_t id;
    int value;
} app_wifi_evt_t;

/**
 * @brief 初始化 WiFi（STA 模式）+ 载入凭证并自动连接
 * @note 必须在 app_config_init()（NVS）之后调用。没有凭证时只是 idle，不报错。
 */
esp_err_t app_wifi_init(void);

/** @brief 当前状态 */
app_wifi_state_t app_wifi_state(void);

/** @brief 是否已连接且拿到 IP */
bool app_wifi_is_connected(void);

/** @brief 是否已经配过网（NVS 里有凭证），用于 SETTINGS 页显示"未配置/已配置" */
bool app_wifi_has_creds(void);

/** @brief 当前/上次连接的 SSID（没配过网时返回空串） */
const char *app_wifi_ssid(void);

/** @brief 当前 IP 字符串（未连接时返回 ""） */
const char *app_wifi_ip(void);

/** @brief 信号强度 dBm（未连接返回 0） */
int8_t app_wifi_rssi(void);

/** @brief 状态文案（"Not configured" / "Connecting..." / SSID / "Failed"），静态缓冲 */
const char *app_wifi_state_text(void);

/**
 * @brief 请求扫描一次热点（异步，完成后抛 APP_WIFI_EVT_SCAN_DONE）
 * @return ESP_ERR_INVALID_STATE = 上一次还在扫
 */
esp_err_t app_wifi_scan_start(void);

/** @brief 扫描是否进行中（UI 用来显示 "Scanning..." 并禁用按钮） */
bool app_wifi_scan_busy(void);

/**
 * @brief 取扫描结果（按信号从强到弱）
 * @return 实际条数
 */
uint32_t app_wifi_ap_list(app_wifi_ap_t *out, uint32_t max);

/**
 * @brief 连接指定热点，并把凭证写入 NVS（之后每次上电自动连）
 * @note 非阻塞：真正的连接动作由 wifi 任务做，结果通过事件通知
 */
esp_err_t app_wifi_connect(const char *ssid, const char *pass);

/** @brief 忘记当前网络（清凭证、断开），回到"未配置"状态 */
esp_err_t app_wifi_forget(void);

/** @brief 非阻塞取一条事件（UI 轮询用） */
bool app_wifi_poll_event(app_wifi_evt_t *out);

#ifdef __cplusplus
}
#endif
