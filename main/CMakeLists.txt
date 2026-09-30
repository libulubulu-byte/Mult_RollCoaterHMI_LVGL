set(srcs
    "main.c"
    "bsp_display.c"
    "app_config.c"
    "app_motion.c"
    # ★ 台架 examples/peripherals/rmt/stepper_motor 的编码器**原样拷进来**：
    #   让主工程能用与台架**逐行相同**的方式发脉冲（uniform 编码器 + loop_count），
    #   现场用 `mtest mode 1/0` 做 A/B（见 app_motion.c 的 s_emit_bench）。
    "stepper_motor_encoder.c"
    "app_state.c"
    "app_settings.c"      # 界面偏好（背光/温单位/自动熄屏/灯状态）NVS 持久化
    "app_light.c"         # GPIO10 + LEDC 灯控
    "app_sensor.c"        # SHT30/31 温湿度（挂在与触摸共用的 I2C0 上）
    "app_wifi.c"          # WiFi 配网 + SNTP 对时
    "ui_theme.c"
    "ui_subpage.c"        # 子界面公共框架（顶栏/返回键/手势/滑条/圆环）
    "ui_menu.c"
    "ui_main.c"
    "ui_admin.c"
    "ui_light.c"
    "ui_env.c"
    "ui_history.c"
    "ui_settings.c"
    "ui_wifi.c"
    "ui_numpad.c"
    "ui_popup.c"
)

# -----------------------------------------------------------------------------
# 界面粗体字库（Lato Bold，LVGL 8/9 双兼容格式，源文件在 main/fonts/）
#   fonts/*.c 里都是 #include "../ui.h"，靠 main/ui.h 那个 shim 接住，
#   所以字库文件可以原样放进来、日后用 lv_font_conv 重新生成覆盖也不会丢改动。
#   字号与字形高度（实测）：
#     H1 50px -> 字形高 36px     H2 44px -> 31px
#     H3 35px -> 25px            P1 25px -> 18px
# -----------------------------------------------------------------------------
list(APPEND srcs
    "fonts/ui_font_H1_bold.c"
    "fonts/ui_font_H2_bold.c"
    "fonts/ui_font_H3_bold.c"
    "fonts/ui_font_P1_bold.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "fonts"
    REQUIRES
        driver            # gpio / rmt / i2c_master / ledc
        esp_lcd           # esp_lcd_panel_* / esp_lcd_touch_*
        esp_lvgl_port     # lvgl_port_init / lvgl_port_add_disp / lvgl_port_add_touch
        nvs_flash         # 参数 / 界面设置 / WiFi 凭证 持久化
        esp_wifi          # 配网（STA）
        esp_netif         # lwIP 网络接口
        esp_event         # WiFi/IP 事件
        lwip              # esp_sntp 对时
        espressif__esp_lcd_touch_gt911
        console           # 串口调试命令行（mtest）。★ 本 IDF(v5.5) 里组件名是 `console`，
                          #   头文件仍叫 esp_console.h；写成 esp_console 会报
                          #   "Failed to resolve component 'esp_console'"。见 app_motion.c 的 CLI 段。
)
