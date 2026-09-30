/**
 * @file main.c
 * @brief MULTI Roll Coater HMI —— 应用入口
 *
 * 屏幕结构（两屏 + 一个弹窗层）：
 *   主菜单 MAIN MENU（ui_menu.c，界面稿 ui1_main.png）
 *     └─ MOTOR TEST ──> MOTOR TEST 子界面（ui_main.c，界面稿 rollcoater_main_screen_v3.png）
 *                        ├─ 长按 Enter 3 秒 ──> Admin 参数屏（ui_admin.c）
 *                        └─ 返回：左上角返回键 / 从右向左滑动
 *   弹窗统一挂在 lv_layer_top()（ui_popup.c），换屏不会丢
 *
 * 启动顺序（每一步都必须在下一步之前完成）：
 *   1. app_config_init()     NVS + 载入机器参数（限位/加减速/速度/位置/间隙）
 *   2. bsp_display_init()    先点亮背光 -> RGB 面板 -> 面板自检刷白 -> LVGL port
 *                            -> 注册显示 -> 注册触摸
 *   3. app_motion_init()     GPIO / RMT / 报警中断 + 创建 motion_task
 *   4. app_state_init()      状态机（State1，位置未标定）
 *   5. ui_theme_init() + ui_menu_create() + ui_main_create() + ui_admin_create()
 *                           在 LVGL 锁内建界面；最后 ui_menu_show() 上第一屏
 *
 * ★ 这里刻意**不用** ESP_ERROR_CHECK：任何一步失败都只打印日志并停在那里，
 *   而不是 abort 重启。否则一旦显示初始化失败就会进入重启循环，
 *   表现为"屏幕一直黑、串口日志反复重来"，很难定位。
 *
 * 之后：
 *   - UI 全部在 esp_lvgl_port 的 LVGL 任务里跑（ui_main 的 50ms 轮询定时器）
 *   - 运动在 motion_task 里跑，通过环形队列把位置推给 UI
 *   - LVGL 任务里没有任何阻塞调用（NVS 写由 cfg_save_task 异步完成）
 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_display.h"
#include "bsp_pins.h"
#include "app_config.h"
#include "app_motion.h"
#include "app_state.h"
#include "app_settings.h"
#include "app_light.h"
#include "app_sensor.h"
#include "app_wifi.h"
#include "ui_theme.h"
#include "ui_subpage.h"
#include "ui_menu.h"
#include "ui_main.h"
#include "ui_admin.h"
#include "ui_light.h"
#include "ui_env.h"
#include "ui_history.h"
#include "ui_settings.h"
#include "ui_wifi.h"
#include "app_text.h"

static const char *TAG = "roll_coater";

/**
 * @brief 复位原因转字符串
 * @note IDF 5.5 没有 esp_reset_reason_to_name()，这里自己映射一份，
 *       只覆盖各版本都存在的枚举值，避免跨 IDF 版本编不过。
 */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON (正常上电)";
    case ESP_RST_EXT:       return "EXT (外部复位脚)";
    case ESP_RST_SW:        return "SW (软件重启)";
    case ESP_RST_PANIC:     return "PANIC (异常/崩溃)";
    case ESP_RST_INT_WDT:   return "INT_WDT (中断看门狗)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (任务看门狗)";
    case ESP_RST_WDT:       return "WDT (其它看门狗)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (深睡唤醒)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (供电跌落)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

/** 失败时保留背光与日志，方便一眼看出卡在哪一步，而不是黑屏+重启循环 */
static void fatal_hold(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "==========================================================");
    ESP_LOGE(TAG, " FATAL: %s failed: %s", what, esp_err_to_name(err));
    ESP_LOGE(TAG, " 背光已点亮（若屏幕仍全黑，请检查供电/背光电路）");
    ESP_LOGE(TAG, " 屏幕亮但无图 -> 多半是板型(PCLK)选错，见 main/README.md");
    ESP_LOGE(TAG, "==========================================================");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGE(TAG, "Still halted at '%s'. Reset the board after fixing the issue.", what);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " Roll Coater HMI  (ESP-IDF %s, LVGL %d.%d.%d)",
             IDF_VER, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    ESP_LOGI(TAG, " Reset reason: %s", reset_reason_str(esp_reset_reason()));
#if BSP_BOARD == BSP_BOARD_CROWPANEL_50
    ESP_LOGI(TAG, " Board   : Elecrow CrowPanel 5.0 HMI (PCLK=GPIO%d)", (int)BSP_LCD_PCLK);
#else
    ESP_LOGI(TAG, " Board   : JC8048W550 / ESP32-8048S050 (PCLK=GPIO%d)", (int)BSP_LCD_PCLK);
#endif
    ESP_LOGI(TAG, " Panel   : %dx%d RGB565 parallel, BL=GPIO%d", BSP_LCD_H_RES, BSP_LCD_V_RES,
             (int)BSP_LCD_BL);
#if BSP_TOUCH_TYPE == BSP_TOUCH_TYPE_GT911
    ESP_LOGI(TAG, " Touch   : GT911 @ I2C%d SDA=%d SCL=%d", (int)BSP_TOUCH_I2C_PORT,
             (int)BSP_TOUCH_I2C_SDA, (int)BSP_TOUCH_I2C_SCL);
#else
    ESP_LOGI(TAG, " Touch   : AXS15231 @ I2C%d SDA=%d SCL=%d", (int)BSP_TOUCH_I2C_PORT,
             (int)BSP_TOUCH_I2C_SDA, (int)BSP_TOUCH_I2C_SCL);
#endif
    ESP_LOGI(TAG, " Motion  : STEP=%d DIR=%d ENA=%d ALARM=%d (%s)",
             MOTION_GPIO_STEP, MOTION_GPIO_DIR, MOTION_GPIO_ENA, MOTION_GPIO_ALARM,
             APP_MOTION_SIMULATE ? "SIMULATE" : "REAL");
    ESP_LOGI(TAG, "=================================================");

    /* --- 1. 机器参数（NVS） --- */
    esp_err_t err = app_config_init();
    if (err != ESP_OK) {
        fatal_hold("app_config_init", err);
    }

    /* --- 2. 显示 + 触摸 + LVGL port --- */
    err = bsp_display_init();
    if (err != ESP_OK) {
        fatal_hold("bsp_display_init", err);
    }

    /* --- 3. 运动控制 --- */
    err = app_motion_init();
    if (err != ESP_OK) {
        fatal_hold("app_motion_init", err);
    }

    /* --- 4. 状态机 --- */
    err = app_state_init();
    if (err != ESP_OK) {
        fatal_hold("app_state_init", err);
    }

    /* --- 4.5 界面偏好 / 灯 / 传感器 / 配网 ---
     * 这四件都不影响运动安全，失败只告警不停机（例如没插传感器、
     * 或这块板子没接 WiFi 天线），界面照常可用。 */
    if (app_settings_init() != ESP_OK) {
        ESP_LOGW(TAG, "app_settings_init failed: UI prefs fall back to defaults");
    }
    if (app_light_init() != ESP_OK) {
        ESP_LOGW(TAG, "app_light_init failed: LIGHT page will not drive hardware");
    }
    if (app_sensor_init() != ESP_OK) {
        ESP_LOGW(TAG, "app_sensor_init failed: ENVIRONMENT/HISTORY will show \"--\"");
    }
    if (app_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "app_wifi_init failed: WiFi provisioning unavailable");
    }

    /* 背光亮度按上次设置恢复（bsp_display_init 里先按 100% 点亮，保证能看到开机画面） */
    {
        const app_settings_t set = app_settings_get();
        bsp_display_set_backlight_percent(set.disp_brightness);
        ESP_LOGI(TAG, "Backlight brightness restored to %u%% (auto screen off: %s, %us)",
                 set.disp_brightness, set.auto_off_en ? "on" : "off", (unsigned)set.auto_off_s);
    }

    /* --- 5. 界面（必须持 LVGL 锁创建） --- */
    if (!bsp_display_lock(0)) {
        fatal_hold("bsp_display_lock", ESP_FAIL);
    }
    ui_theme_init();
    ui_menu_create();       /* 主菜单（上电第一屏） */
    ui_main_create();       /* MOTOR TEST 子界面 */
    ui_admin_create();      /* Admin 参数屏 */
    ui_light_create();      /* LIGHT CONTROL     */
    ui_env_create();        /* ENVIRONMENT       */
    ui_history_create();    /* HISTORY           */
    ui_settings_create();   /* SETTINGS          */
    ui_wifi_create();       /* WiFi 配网（列表 + 密码页） */
    /* 所有屏幕都建好之后再决定显示谁：主菜单。
     * （ui_main_create() 里不再自己 lv_scr_load，避免装配顺序决定第一屏） */
    ui_menu_show();
    bsp_display_unlock();

    /* 上电姿态：主菜单 -> MOTOR TEST；进去后是 State1，等待操作员把刻度盘读数输进来 */
    ESP_LOGI(TAG, "System ready. Tap MOTOR TEST on MAIN MENU. %s", APP_TXT_PROMPT_STATE1);
    ESP_LOGI(TAG, "Back from sub-pages: top-left button or swipe right->left");
    ESP_LOGI(TAG, "Admin: type 9.999 then hold Enter for %d ms", APP_ADMIN_HOLD_MS);
}
