/**
 * @file app_text.h
 * @brief 全部界面文案集中定义（改文案只改这里，方便后续做多语言）
 *
 * 说明：LV_SYMBOL_* 是 LVGL 内置矢量图标字符，只有 Montserrat 内置字体带这些字形。
 * 主界面用的是只含 ASCII 的粗体字库（见 ui_fonts.h），所以 ⌫ / ↵ 这两个键会在
 * ui_numpad.c 里单独套用 UI_FONT_SYMBOL（内置 Montserrat）来渲染图标。
 * 若不想在固件里保留 Montserrat，把下面两个宏改成 "DEL" / "ENT" 纯文字即可。
 */
#pragma once

#include "lvgl.h"

/*----------------------------- 顶部状态栏 -------------------------------*/
#define APP_TXT_TITLE                 "ROLL COATER HMI"
#define APP_TXT_STATE_FMT             "State %d"

/*----------------------------- 读数卡片 ---------------------------------*/
#define APP_TXT_CAP_CURRENT           "Motor's Current Position"
#define APP_TXT_CAP_TARGET            "Target Position"

/*----------------------------- 键盘上方提示行 ---------------------------*/
#define APP_TXT_PROMPT_STATE1         "Enter Roll Coat Height"
#define APP_TXT_PROMPT_STATE2         "Enter Target Position"
#define APP_TXT_PROMPT_STATE3         "Axis is out of limits"
#define APP_TXT_PROMPT_ADMIN          "Admin - Parameters"
#define APP_TXT_PROMPT_ALARM          "ALARM - motion disabled"
#define APP_TXT_PROMPT_MOVING         "Moving to target..."

/*----------------------------- 键盘 / 按钮 ------------------------------*/
#define APP_TXT_STOP                  "STOP"
#define APP_TXT_KEY_BACKSPACE         LV_SYMBOL_BACKSPACE
#define APP_TXT_KEY_ENTER             LV_SYMBOL_NEW_LINE

/*----------------------------- 越限提示 --------------------------------*/
/* %.3f / %.3f / %.3f -> 输入值 / 下限 / 上限 */
#define APP_TXT_LIMIT_REJECT_FMT      "Out of range: %.3f  (allowed %.3f .. %.3f)"
#define APP_TXT_MOVE_START_FMT        "Moving to %.3f"
#define APP_TXT_ARRIVED_FMT           "Arrived at %.3f"
#define APP_TXT_CURRENT_SET_FMT       "Position set to %.3f"

/*----------------------------- State3 弹窗 ------------------------------*/
#define APP_TXT_P1_TITLE              "Check the entered value"
#define APP_TXT_P1_MSG                "Was the entered value typed correctly?"
#define APP_TXT_P1_YES                "Yes"
#define APP_TXT_P1_NO                 "No"

#define APP_TXT_P2_TITLE              "Manual positioning"
#define APP_TXT_P2_MSG                "Manually move the axis with the hand dial into limits, then press Enter."
#define APP_TXT_P2_ENTER              "Enter"

/*----------------------------- 报警 ------------------------------------*/
#define APP_TXT_ALARM_TITLE           "ALARM"
#define APP_TXT_ALARM_MSG             "Axis alarm input is active.\nMotion is disabled until the fault is acknowledged."
#define APP_TXT_ALARM_ACK             "ACKNOWLEDGE"

/*----------------------------- Admin 屏 ---------------------------------*/
#define APP_TXT_ADMIN_TITLE           "Admin - Machine Parameters"
#define APP_TXT_ADMIN_EXIT            "Exit and Accept"
#define APP_TXT_ADMIN_EDIT            "Edit"
#define APP_TXT_ADMIN_OK              "OK"
#define APP_TXT_ADMIN_CANCEL          "Cancel"
#define APP_TXT_ADMIN_SAVED           "Parameters saved"

#define APP_TXT_CFG_UPPER_LIMIT       "Upper Limit"
#define APP_TXT_CFG_LOWER_LIMIT       "Lower Limit"
#define APP_TXT_CFG_ACCELERATION      "Acceleration"
#define APP_TXT_CFG_DECELERATION      "Deceleration"
#define APP_TXT_CFG_SPEED             "Speed"
#define APP_TXT_CFG_CURRENT_POS       "Current Position"
#define APP_TXT_CFG_BACKLASH          "Backlash"

/*----------------------------- 校验错误文案 -----------------------------*/
#define APP_TXT_ERR_RANGE_FMT         "Value must be %.3f .. %.3f"
#define APP_TXT_ERR_GREATER_ZERO      "Value must be greater than 0.000"
#define APP_TXT_ERR_NOT_NEGATIVE      "Value must not be negative"
#define APP_TXT_ERR_UPPER_BELOW_LOWER "Upper limit must be >= lower limit"
#define APP_TXT_ERR_LOWER_ABOVE_UPPER "Lower limit must be <= upper limit"
#define APP_TXT_ERR_EMPTY             "No value entered"

/*----------------------------- Admin 入口判定 ---------------------------*/
/* 在 State1/State2 下输入 9.999 并长按 Enter 满 3 秒进入 Admin */
#define APP_ADMIN_UNLOCK_VALUE        (9.999f)
#define APP_ADMIN_HOLD_MS             (3000)

/*==============================================================================
 * 主菜单（界面稿 ui1_main.png，见 ui_menu.c）
 *
 * ⚠ 字体限制：粗体字库（Lato Bold）只生成了 ASCII，下面这些文案里
 *   不能出现 ° · ℃ 之类的非 ASCII 字符 —— 会渲染成空白/方框。
 *   度符号单独用 APP_TXT_MENU_ENV_TEMP_UNIT（内置 Montserrat 有 U+00B0），
 *   间隔点用画出来的圆点，见 ui_menu.c 的 menu_dot()。
 *============================================================================*/
#define APP_TXT_MENU_TITLE            "ESP32 SMART CONTROL PANEL"
#define APP_TXT_MOTOR_TEST            "MOTOR TEST"   /* 主菜单卡片名 + 子界面顶栏标题 */
#define APP_TXT_MENU_LIGHT            "LIGHT"
#define APP_TXT_MENU_ENVIRONMENT      "ENVIRONMENT"
#define APP_TXT_MENU_HISTORY          "HISTORY"
#define APP_TXT_MENU_SETTINGS         "SETTINGS"

/* 卡片上的摘要（目前是界面稿上的静态占位：传感器/灯控还没接） */
#define APP_TXT_MENU_LIGHT_SUB_A      "ON"
#define APP_TXT_MENU_LIGHT_SUB_B      "80%"
#define APP_TXT_MENU_ENV_TEMP         "26.5"
#define APP_TXT_MENU_ENV_TEMP_UNIT    "\xC2\xB0" "C"   /* UTF-8 的 U+00B0 + C = °C */
#define APP_TXT_MENU_ENV_HUM          "55%"
#define APP_TXT_MENU_MOTOR_SUB        "READY"

/* 顶栏 / 底栏 */
#define APP_TXT_MENU_CLOCK_UNSET      "--:--"          /* 系统时间没被设置时显示 */
#define APP_TXT_MENU_STATUS_OK        "System: OK"
#define APP_TXT_MENU_STATUS_ALARM     "System: ALARM"
#define APP_TXT_MENU_FW               "FW v1.0.0"
#define APP_TXT_MENU_NA_FMT           "%s not available yet"   /* 未实现入口的提示 */

/*==============================================================================
 * 子界面公共（ui_subpage.c）与 LIGHT / ENVIRONMENT / HISTORY / SETTINGS
 *
 * ⚠ 同样的字体限制：粗体字库只有 ASCII。凡是带 ° • ⌫ ↵ 这类符号的地方，
 *   都要用 UI_FONT_SMALL（内置 Montserrat，字符集含 0xB0 度符号 / 0x2022 圆点）：
 *     - 子页右上角状态（如 "SHT31 • 2s"）用 APP_TXT_DOT_MID 拼
 *     - 温度单位后缀 °C 用 APP_TXT_UNIT_DEG_C
 *============================================================================*/
#define APP_TXT_DOT_MID               "\xE2\x80\xA2"   /* U+2022 •（只在 Montserrat 里有） */
#define APP_TXT_UNIT_DEG_C            "\xC2\xB0" "C"   /* °C */
#define APP_TXT_DASH                  "--"
#define APP_TXT_NA_FMT                "%s"             /* 占位 */

/*----------------------------- LIGHT 页 ---------------------------------*/
#define APP_TXT_LIGHT_TITLE           "LIGHT CONTROL"
#define APP_TXT_LIGHT_CH1             "Light 1"
#define APP_TXT_LIGHT_ON              "ON"
#define APP_TXT_LIGHT_OFF             "OFF"
#define APP_TXT_LIGHT_STATUS_FMT      "Status: %s"
#define APP_TXT_LIGHT_BRIGHTNESS      "Brightness"
#define APP_TXT_PCT_FMT               "%u%%"
#define APP_TXT_ON_PCT_FMT            "%s " APP_TXT_DOT_MID " %u%%"   /* ON • 80% */

/*----------------------------- ENVIRONMENT 页 ---------------------------*/
#define APP_TXT_ENV_TITLE             "ENVIRONMENT"
#define APP_TXT_ENV_SENSOR_FMT        "SHT3x " APP_TXT_DOT_MID " %ds"
#define APP_TXT_ENV_TEMPERATURE       "TEMPERATURE"
#define APP_TXT_ENV_HUMIDITY          "HUMIDITY"
#define APP_TXT_ENV_UNIT_RH           "%RH"
#define APP_TXT_ENV_TODAY             "Today Min / Max"
#define APP_TXT_ENV_COMFORT_FMT       "%s"
#define APP_TXT_ENV_HINT              "40-60% " APP_TXT_DOT_MID " 18-26" APP_TXT_UNIT_DEG_C
#define APP_TXT_ENV_STATUS            "Status:"
#define APP_TXT_ENV_DRY               "DRY"
#define APP_TXT_ENV_COMFORTABLE       "COMFORTABLE"
#define APP_TXT_ENV_HUMID             "HUMID"
#define APP_TXT_ENV_COLD              "COLD"
#define APP_TXT_ENV_HOT               "HOT"
#define APP_TXT_ENV_NO_SENSOR_SHORT   "No sensor"
#define APP_TXT_ENV_NO_SENSOR         "Sensor not found (I2C0 / 0x44)"

/*----------------------------- HISTORY 页 -------------------------------*/
#define APP_TXT_HIST_TITLE            "HISTORY"
#define APP_TXT_HIST_SPAN             "24H"
#define APP_TXT_HIST_LEGEND_TEMP      "Temp"
#define APP_TXT_HIST_LEGEND_HUM       "Hum"
#define APP_TXT_HIST_AVERAGE          "24h Average"
#define APP_TXT_HIST_RANGE_TEMP       "Temp range"
#define APP_TXT_HIST_RANGE_HUM        "Hum range"
#define APP_TXT_HIST_NO_DATA          "Collecting data..."
#define APP_TXT_HIST_POINTS_FMT       "%u samples"

/*----------------------------- SETTINGS 页 ------------------------------*/
#define APP_TXT_SET_TITLE             "SETTINGS"
#define APP_TXT_SET_WIFI              "WiFi"
#define APP_TXT_SET_CONFIGURE         "Configure"
#define APP_TXT_SET_BRIGHTNESS        "Display Brightness"
#define APP_TXT_SET_TEMP_UNIT         "Temperature Unit"
#define APP_TXT_SET_AUTO_OFF          "Auto Screen Off"
#define APP_TXT_SET_FACTORY           "Factory Reset"
#define APP_TXT_SET_RESET             "RESET"
#define APP_TXT_SET_RESETTING         "Resetting and rebooting..."
#define APP_TXT_SET_RESET_CONFIRM     "Erase ALL settings and reboot?"

/*----------------------------- WiFi 配网页 ------------------------------*/
#define APP_TXT_WIFI_TITLE            "WIFI SETUP"
#define APP_TXT_WIFI_SCAN             "SCAN"
#define APP_TXT_WIFI_RESCAN           "SCAN AGAIN"
#define APP_TXT_WIFI_CONNECT          "CONNECT"
#define APP_TXT_WIFI_FORGET           "FORGET"
#define APP_TXT_WIFI_CANCEL           "CANCEL"
#define APP_TXT_WIFI_SCANNING         "Scanning..."
#define APP_TXT_WIFI_NO_AP            "No network found - tap SCAN"
#define APP_TXT_WIFI_PASSWORD         "Password"
#define APP_TXT_WIFI_PICK_FMT         "Network: %s"
#define APP_TXT_WIFI_READY            "Ready"
#define APP_TXT_WIFI_CONNECTED        "Connected"
#define APP_TXT_WIFI_FAILED           "Failed - check password"
#define APP_TXT_WIFI_OPEN_HINT        "Open network - no password needed"
