/**
 * @file bsp_display.c
 * @brief RGB 并口屏 + 电容触摸 + esp_lvgl_port 初始化
 *
 * ┌─ RGB 屏三条关键经验（换屏/换板必读）──────────────────────────────────────┐
 * │ 1) 面板扫描率 = PCLK / ((H+hsync+hbp+hfp) * (V+vsync+vbp+vfp))              │
 * │    本板 = 820 x 500：12MHz -> 29.3Hz，16MHz -> 39.0Hz。                    │
 * │    LVGL 刷新周期由代码按"面板帧周期 x (1+SLOWDOWN)"自动算，见下。           │
 * │                                                                           │
 * │ 2) 帧缓冲必须放 PSRAM（800x480x2B = 768KB 内部 RAM 放不下），              │
 * │    而 LCD 的 DMA 是硬实时的 —— 两条路线各有代价，见下面三个方案。          │
 * │                                                                           │
 * │ 3) ★ 单 fb + bounce 下"闪"的真正来源是**写读竞争**，不是补数超时：       │
 * │    rgb_panel_draw_bitmap() 永远写 fbs[cur_fb_index]，而 bounce 引擎从      │
 * │    fbs[bb_fb_index] 顺序读（esp_lcd_panel_rgb.c:649 / :891）；普通局部刷新 │
 * │    模式下这两个索引恒等于 0，就是同一块内存。LVGL 写到一半的脏区被 bounce  │
 * │    搬上屏，屏幕上就出现一条"半新半旧"的横带 —— 这就是肉眼看到的闪。       │
 * │    PCLK 越高、LVGL 写 fb 越频繁，撞上的机会越大，所以 16MHz 比 12MHz 更闪。│
 * └───────────────────────────────────────────────────────────────────────────┘
 *
 * 三个显示方案（BSP_LVGL_DISPLAY_MODE 选择）：
 *
 *  A) BSP_DISPLAY_MODE_PARTIAL_BB（回退方案；单 fb，写读竞争**无解**）
 *     面板 num_fbs=1 + bounce buffer；LVGL 用 2 块 20 行**内部 RAM** 缓冲做局部刷新，
 *     rgb_panel_draw_bitmap() 把脏区 memcpy 进 PSRAM 帧缓冲，bounce 引擎再从该
 *     帧缓冲顺序读进内部 RAM 缓冲，最后由 DMA 输出。
 *     DMA 只读内部 RAM、完全不碰 PSRAM，所以不会整屏花屏 —— 但第 3 条那条**写读竞争
 *     在单 fb 下根本无解**（LVGL 写 fb 的同时 bounce 就在读同一块），只能靠
 *     BSP_LVGL_SLOWDOWN_PCT 调慢去撞概率；频繁刷屏/负载高时就是"横带 + 局部错位"。
 *
 *  B) BSP_DISPLAY_MODE_AVOID_TEARING（★★ **现在的默认** ★★）
 *     面板 num_fbs=2，两块帧缓冲直接交给 LVGL（direct_mode + avoid_tearing +
 *     **full_refresh = 1**）：LVGL 每帧把**整屏**重画进"后台那块 fb"，画完等 VSYNC
 *     才切显示 ⇒ 第 3 条那个写读竞争被**结构性消除**（画后台 / 读前台，天然错开）。
 *     ★★★ `full_refresh = 1` 是**必须**的：漏掉它，LVGL 只把脏区画进后台 fb，
 *     而两块 fb 是**交替显示**的 ⇒ 必然"半新半旧"（控件错位 / 只画一半 / 局部旧内容）。
 *     本板第一次试方案 B 就是这么失败的 —— 不是方案不行，是少了这一行。
 *     它必须**同时保留 bounce buffer**：否则 DMA 直读 PSRAM、而 LVGL 又在写 PSRAM
 *     的另一块 fb，读写抢 PSRAM 会让 DMA 断流 -> 抖动/花屏（本板实测确实如此）。
 *     代价：① 每帧整屏重绘（≈768KB PSRAM 写）；② 帧缓冲 2 x 768KB = 1.5MB PSRAM。
 *
 *  C) BSP_DISPLAY_MODE_DIRECT_FB（★ 本板不可用，仅留作对照）
 *     面板 num_fbs=1、无 bounce：DMA 直接突发读 PSRAM 帧缓冲。
 *     实测失败：此时 LVGL 写入的就是 DMA 正在连续扫描的那块 fb，而且没有任何同步，
 *     画面大面积撕裂/错位（"显示全乱"）。切记 num_fbs=1 时不能去掉 bounce。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_io.h"

#include "bsp_display.h"
#include "bsp_pins.h"

#if BSP_TOUCH_TYPE == BSP_TOUCH_TYPE_GT911
#include "esp_lcd_touch_gt911.h"
#elif BSP_TOUCH_TYPE == BSP_TOUCH_TYPE_AXS15231
#if __has_include("esp_lcd_touch_axs15231b.h")
#include "esp_lcd_touch_axs15231b.h"
#define BSP_TOUCH_AXS15231_AVAILABLE 1
#else
#define BSP_TOUCH_AXS15231_AVAILABLE 0
#endif
#endif

static const char *TAG = "bsp_display";

/*==============================================================================
 * 显示方案选择
 *============================================================================*/
#define BSP_DISPLAY_MODE_PARTIAL_BB     1   /* 单 fb + bounce buffer（默认）                 */
#define BSP_DISPLAY_MODE_AVOID_TEARING  2   /* 双 fb 给 LVGL 画 + 保留 bounce（消除写读竞争）*/
#define BSP_DISPLAY_MODE_DIRECT_FB      3   /* 单 fb、无 bounce，DMA 直读 PSRAM（本板不可用）*/

#ifndef BSP_LVGL_DISPLAY_MODE
/* ★★ 默认 PARTIAL_BB（2026-09-22 深夜按现场实测退回）★★
 *
 * 现场现象（**所有界面**都乱，不是某一页的问题）：开机后控件错位、只画了一半、
 * 有的地方还是旧内容 —— 这正是方案 B（avoid_tearing）在本板上的失效表现：
 * 该方案把面板的两块 fb 直接交给 LVGL，LVGL 只把**脏区**画进"后台那块 fb"，
 * 再等 VSYNC 交换。只要交换/预填任一时机不对，没被重画过的区域就会留在屏幕上
 * = 看起来"控件跑到别的位置、半屏是旧的"。它当初是为了消除方案 A 的"偶发横带闪"，
 * 但那个代价（画面整体错乱）比横带严重得多，所以**退回 A 作默认**。
 *
 *  A) PARTIAL_BB（回退方案）：num_fbs=1 + bounce buffer；LVGL 用 2 块 20 行
 *     **内部 RAM** 缓冲做局部刷新，DMA 只读内部 RAM —— 不会整屏花屏，但
 *     **写读竞争在单 fb 下无解** ⇒ 频繁刷屏/负载高时"横带 + 局部错位"。
 *     现场后来就是在这个方案下报"界面乱"，才改回 B。
 *  B) AVOID_TEARING（**现在的默认**）：见文件头 —— 关键在 `full_refresh = 1`。
 *
 * 千万不要改成 DIRECT_FB：num_fbs=1 又没有 bounce 时，LVGL 直接写正在被 DMA
 * 扫描的那块 fb、毫无同步，画面会大面积撕裂/错位（实测"显示全乱"）。
 *
 * ★★★ 2026-09-22 深夜（第二次改回方案 B）：**AVOID_TEARING + `full_refresh = 1`** ★★★
 *   现场在方案 A（PARTIAL_BB）下仍然报"界面乱"。方案 A 的结构性缺陷就是上面第 3 条
 *   那条**写读竞争**（LVGL 写 fb 的同时 bounce 在读同一块）—— 单 fb 下**无解**，
 *   只能靠调快慢去撞概率。⇒ 改回方案 B：双 fb，LVGL 画后台、bounce 读前台，天然错开。
 *
 *   ★ 上一次方案 B 之所以失败（"控件错位 / 只画了一半 / 局部还是旧内容"），
 *     是因为**漏了 `full_refresh = 1`**：`direct_mode` + 双缓冲时，LVGL 只把
 *     **脏区**画进"后台那块 fb"，而两块 fb 是**交替显示**的 ⇒ 没被重画过的区域
 *     永远留着，两块 fb 各有各的残缺 = 看起来"半新半旧、控件跑到别处"。
 *     补上 `full_refresh = 1`（每帧整屏重画）后两个 fb 始终是完整画面 ——
 *     这是 LVGL 手册对"direct_mode + 双缓冲"的**硬性要求**，不是可选项。
 *
 *   代价：① 每帧整屏重绘 ≈ 768KB PSRAM 写（LVGL 29.4Hz ⇒ ≈22.5MB/s）；
 *         ② 帧缓冲 2 x 768KB = 1.5MB PSRAM（本板 8MB，够用）。
 *   ⇒ 要退回方案 A：把这行换回 `BSP_DISPLAY_MODE_PARTIAL_BB` 即可（一行）。 */
#define BSP_LVGL_DISPLAY_MODE BSP_DISPLAY_MODE_AVOID_TEARING
#endif

/* 上电把整屏刷成白色做面板自检（验证 PCLK/时序/数据线通路）。
 * 确认硬件没问题后可以改成 0，省掉上电时那一下白闪。 */
#ifndef BSP_LCD_SELF_TEST
#define BSP_LCD_SELF_TEST 1
#endif

/* 上电预填帧缓冲用的颜色（RGB565）。默认白，和自检色一致。
 * 双 fb 方案下必须把**每一块** fb 都预填：否则切到从未写过的那一块时，
 * 屏幕会把 PSRAM 里的随机数据打成满屏雪花。 */
#ifndef BSP_LCD_INIT_FB_COLOR
#define BSP_LCD_INIT_FB_COLOR 0xFFFF
#endif

/* LVGL 刷新周期 = 面板帧周期 x (1 + BSP_LVGL_SLOWDOWN_PCT/100)，默认慢 50%
 * （18MHz -> 43.9Hz 面板 / 29.3Hz LVGL，约 34ms，操作手感仍然跟手）。
 *
 * ★ 这个值只在 BSP_DISPLAY_MODE_PARTIAL_BB（单 fb + bounce，**现在的默认**）下才需要：
 *   那种模式下 LVGL 写 fbs[cur_fb_index]、bounce 同时读 fbs[bb_fb_index]，
 *   而两个索引恒等 -> LVGL 写到一半的脏区会被 bounce 搬上屏，就是"偶发横带闪"。
 *   把刷新率压低 = 单位时间内"写 fb"的次数变少 = 撞上的机会变少：
 *     50 -> 写频率降到 2/3；100 -> 降到 1/2（最干净，代价是 LVGL 只有 22Hz）。
 *   ★ 注意：调到 100 只会让横带**变少**，不会让它消失（这是写读竞争的本质）；
 *     真要彻底消除得回到方案 B，但方案 B 在**本板**会让整屏控件错位（见上面说明），
 *     所以这里选择"偶发细横带"而不是"整屏乱"。 */
#ifndef BSP_LVGL_SLOWDOWN_PCT
#define BSP_LVGL_SLOWDOWN_PCT 50
#endif

/* LVGL 局部刷新缓冲高度（行）。
 * 800 宽 20 行 = 16000 px = 32KB/块，双缓冲共 64KB。
 * ★ 必须放**内部 RAM**（见下面 buff_spiram=0 的说明）：
 *   放 PSRAM 的话 LVGL 渲染会持续占用 PSRAM 通路，把 bounce buffer 在 EOF 中断
 *   里的 PSRAM memcpy 挤晚 -> LCD 断流 -> 抖动/花屏。放内部 RAM 后渲染几乎不碰
 *   PSRAM，实时通路干净很多；内部 SRAM 读写也快得多。
 * 20 行是为了在内部 RAM 里留出余量（bounce buffer 还要吃掉 128KB）。 */
#define BSP_LVGL_BUFFER_HEIGHT          (20)

/* LVGL port 任务栈：UI 里有较多 label / 事件回调，给足 8KB */
#define BSP_LVGL_TASK_STACK             (8 * 1024)
#define BSP_LVGL_TASK_PRIO              (4)

/*==============================================================================
 * 静态状态
 *============================================================================*/
static esp_lcd_panel_handle_t s_panel = NULL;
/** 面板帧缓冲块数（由 BSP_LVGL_DISPLAY_MODE 决定：PARTIAL_BB/DIRECT_FB = 1，AVOID_TEARING = 2）。
 *  ★ 供 `bsp_lcd_fill_all_fbs()` 判断"能不能按 2 块取" —— 单 fb 时若还去
 *    `esp_lcd_rgb_panel_get_frame_buffer(panel, 2, …)`，IDF 会打一条
 *    `E lcd_panel.rgb: invalid frame buffer number`（功能上会自动退回整屏刷色，
 *    但那条红色 ERROR 很容易被当成真故障，见 main/README.md 2.1 的开机日志样本）。 */
static uint32_t s_num_fbs = 1;
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_display_t *s_disp = NULL;
static bool s_backlight_state = false;

/*==============================================================================
 * 背光
 *
 * 两种驱动方式，用 BSP_LCD_BL_USE_LEDC 切换：
 *   0 = GPIO 直接输出直流高电平（★ 默认）。变量最少、最干净，排障首选。
 *   1 = LEDC PWM 1220Hz（与 Elecrow 官方配置一致），可调亮度。
 *
 * ★ 为什么默认改回直流：
 *   排"画面静止时仍微微闪"时，LEDC 是一个无法排除的变量 —— 注意 LEDC 在
 *   10bit 下 duty 上限是 1023（= 1023/1024），并不是真正的"绝对恒高"。
 *   先把背光退成干净直流，让"闪"的归因只剩面板/数据通路一条；
 *   确认画面稳了之后再切回 1 拿亮度调节能力。
 *============================================================================*/
#ifndef BSP_LCD_BL_USE_LEDC
#define BSP_LCD_BL_USE_LEDC        (0)
#endif
#define BSP_LCD_BL_LEDC_FREQ_HZ    (1220)                    /* 与 Elecrow 官方一致 */
#define BSP_LCD_BL_LEDC_RES        (LEDC_TIMER_10_BIT)       /* 0..1023 */
#ifndef BSP_LCD_BL_DUTY_PERCENT
#define BSP_LCD_BL_DUTY_PERCENT    (100)                     /* 默认满亮度 */
#endif

static uint8_t s_bl_percent = BSP_LCD_BL_DUTY_PERCENT;

esp_err_t bsp_display_set_backlight_percent(uint8_t percent)
{
    if (BSP_LCD_BL == GPIO_NUM_NC) {
        return ESP_OK;
    }
    if (percent > 100) {
        percent = 100;
    }
    /* ★ percent = 0 只表示"临时熄灭"（自动熄屏），**不能覆盖记忆的亮度** ——
     *   否则 bsp_display_backlight(false) 会把 s_bl_percent 冲成 0，
     *   之后 bsp_display_backlight(true) 恢复的是 0% = 还是黑屏
     *   （实测症状：熄屏后双击唤醒，屏幕不亮，日志却是 Backlight ON (0%)）。 */
    if (percent > 0) {
        s_bl_percent = percent;
    }

#if BSP_LCD_BL_USE_LEDC
    const uint32_t max_duty = (1u << BSP_LCD_BL_LEDC_RES) - 1u;
    const uint32_t duty = (max_duty * (uint32_t)percent) / 100u;

    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG,
                        "ledc_set_duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    /* 直流模式只能开 / 关（percent > 0 即点亮） */
    gpio_set_level(BSP_LCD_BL, (percent > 0) ? 1 : 0);
    return ESP_OK;
#endif
}

void bsp_display_backlight(bool on)
{
    if (BSP_LCD_BL == GPIO_NUM_NC) {
        return;
    }
    bsp_display_set_backlight_percent(on ? s_bl_percent : 0);
    s_backlight_state = on;
    ESP_LOGI(TAG, "Backlight %s (%u%%, %s)", on ? "ON" : "OFF",
             (unsigned)(on ? s_bl_percent : 0),
             BSP_LCD_BL_USE_LEDC ? "LEDC PWM 1220Hz" : "GPIO DC");
}

static esp_err_t bsp_backlight_init(void)
{
    if (BSP_LCD_BL == GPIO_NUM_NC) {
        return ESP_OK;
    }

#if BSP_LCD_BL_USE_LEDC
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BSP_LCD_BL_LEDC_RES,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = BSP_LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer config failed");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = BSP_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,      /* 先全黑；随后由 bsp_display_init() 第一时间点亮 */
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "ledc channel config failed");
#else
    const gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 1ULL << BSP_LCD_BL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight gpio config failed");
    gpio_set_level(BSP_LCD_BL, 0);   /* 先灭；随后由 bsp_display_init() 第一时间点亮 */
#endif

    s_backlight_state = false;
    return ESP_OK;
}

/*==============================================================================
 * RGB 面板
 *============================================================================*/
/**
 * @brief 面板自检：把整屏刷成一个纯色
 *
 * 这一步完全不走 LVGL，只验证「PCLK + 时序 + 数据线 + 面板」这条硬件通路。
 * 看到纯色说明：
 *   - 背光电路正常（否则屏幕是黑的）
 *   - PCLK 引脚选择正确（选错则面板收不到像素时钟，数据线恒 0 -> 黑屏）
 * 看到纯色之后再开始 LVGL，出问题就能立刻区分"显示不通"和"UI 没建起来"。
 */
static esp_err_t bsp_lcd_fill_solid(esp_lcd_panel_handle_t panel, uint16_t rgb565)
{
    const int32_t w = BSP_LCD_H_RES;
    uint16_t *line = heap_caps_malloc(w * sizeof(uint16_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(line != NULL, ESP_ERR_NO_MEM, TAG, "fill line buffer alloc failed");

    for (int32_t i = 0; i < w; i++) {
        line[i] = rgb565;
    }

    esp_err_t err = ESP_OK;
    for (int32_t y = 0; y < BSP_LCD_V_RES; y++) {
        err = esp_lcd_panel_draw_bitmap(panel, 0, y, w, y + 1, line);
        if (err != ESP_OK) {
            break;
        }
    }
    heap_caps_free(line);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Panel self-test: filled 0x%04X (screen should be light)", rgb565);
    } else {
        ESP_LOGE(TAG, "Panel self-test draw failed: %s", esp_err_to_name(err));
    }
    return err;
}

/** @brief 面板一帧的像素总数（含消隐区） */
static uint32_t bsp_lcd_pixels_per_frame(void)
{
    const uint32_t h_total = BSP_LCD_H_RES + BSP_LCD_HSYNC_PULSE_WIDTH +
                             BSP_LCD_HSYNC_BACK_PORCH + BSP_LCD_HSYNC_FRONT_PORCH;
    const uint32_t v_total = BSP_LCD_V_RES + BSP_LCD_VSYNC_PULSE_WIDTH +
                             BSP_LCD_VSYNC_BACK_PORCH + BSP_LCD_VSYNC_FRONT_PORCH;
    return h_total * v_total;
}

/** @brief 面板扫描率（Hz）= PCLK / 每帧像素数（含消隐） */
static float bsp_lcd_scan_hz(void)
{
    return (float)BSP_LCD_PIXEL_CLOCK_HZ / (float)bsp_lcd_pixels_per_frame();
}

/**
 * @brief 由面板扫描率反算 LVGL 刷新周期
 *
 * 取 面板帧周期 x 1.25（即慢 20%）：
 *   - 两者接近会差频，表现为画面"微微在闪"
 *   - LVGL 比面板快更糟：会持续提交新帧、和面板扫描抢 PSRAM/总线
 * 做成自动计算后，menuconfig 里随便改 PCLK 都不会再配错刷新率。
 */
static uint32_t bsp_calc_lvgl_period_ms(void)
{
    const float factor = (100.0f + (float)BSP_LVGL_SLOWDOWN_PCT) / 100.0f;
    uint32_t ms = (uint32_t)(1000.0f / bsp_lcd_scan_hz() * factor + 0.5f);
    if (ms < 10) {
        ms = 10;
    }
    return ms;
}

static esp_err_t bsp_lcd_init(void)
{
    ESP_LOGI(TAG, "Init RGB panel %dx%d @ %dMHz, PCLK=GPIO%d, BL=GPIO%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_PIXEL_CLOCK_HZ / 1000000,
             (int)BSP_LCD_PCLK, (int)BSP_LCD_BL);

#if BSP_LVGL_DISPLAY_MODE == BSP_DISPLAY_MODE_AVOID_TEARING
    /* 双 fb + ★ 保留 bounce：LVGL 画后台 fb、bounce 读前台 fb，两个索引天然错开。
     * 这里绝不能把 bounce 关掉，否则 DMA 直读 PSRAM 会和 LVGL 的写抢带宽 -> 花屏。 */
    const uint32_t num_fbs = 2;
    const uint32_t bounce_px = BSP_LCD_H_RES * BSP_LCD_BOUNCE_BUFFER_HEIGHT;
#elif BSP_LVGL_DISPLAY_MODE == BSP_DISPLAY_MODE_DIRECT_FB
    /* ★ 无 bounce：帧缓冲就在 PSRAM，GDMA 直接按突发读，CPU 不参与搬运。
     * num_fbs 必须是 1（单 fb + 连续扫描），否则 IDF 会走另一套切换流程。 */
    const uint32_t num_fbs = 1;
    const uint32_t bounce_px = 0;
#else
    const uint32_t num_fbs = 1;
    const uint32_t bounce_px = BSP_LCD_H_RES * BSP_LCD_BOUNCE_BUFFER_HEIGHT;
#endif
    s_num_fbs = num_fbs;    /* 给 bsp_lcd_fill_all_fbs() 用，见它的说明 */

    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dma_burst_size = 64,
        .data_width = BSP_LCD_DATA_WIDTH,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .vsync_gpio_num = BSP_LCD_VSYNC,
        .hsync_gpio_num = BSP_LCD_HSYNC,
        .disp_gpio_num = BSP_LCD_DISP,
        .data_gpio_nums = {
            BSP_LCD_DATA0,  BSP_LCD_DATA1,  BSP_LCD_DATA2,  BSP_LCD_DATA3,
            BSP_LCD_DATA4,  BSP_LCD_DATA5,  BSP_LCD_DATA6,  BSP_LCD_DATA7,
            BSP_LCD_DATA8,  BSP_LCD_DATA9,  BSP_LCD_DATA10, BSP_LCD_DATA11,
            BSP_LCD_DATA12, BSP_LCD_DATA13, BSP_LCD_DATA14, BSP_LCD_DATA15,
        },
        .timings = {
            .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
            .h_res = BSP_LCD_H_RES,
            .v_res = BSP_LCD_V_RES,
            .hsync_back_porch = BSP_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = BSP_LCD_HSYNC_FRONT_PORCH,
            .hsync_pulse_width = BSP_LCD_HSYNC_PULSE_WIDTH,
            .vsync_back_porch = BSP_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = BSP_LCD_VSYNC_FRONT_PORCH,
            .vsync_pulse_width = BSP_LCD_VSYNC_PULSE_WIDTH,
            .flags = {
                .hsync_idle_low = BSP_LCD_HSYNC_IDLE_LOW,
                .vsync_idle_low = BSP_LCD_VSYNC_IDLE_LOW,
                .pclk_active_neg = BSP_LCD_PCLK_ACTIVE_NEG,
            },
        },
        /* 帧缓冲放 PSRAM：800*480*2 = 768KB，内部 RAM 放不下 */
        .flags.fb_in_psram = 1,
        .num_fbs = num_fbs,
        .bounce_buffer_size_px = bounce_px,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_cfg, &s_panel), TAG, "new rgb panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");

    if (bounce_px == 0) {
        ESP_LOGI(TAG, "RGB panel ready (num_fbs=%u, NO bounce buffer -> GDMA reads PSRAM directly)",
                 (unsigned)num_fbs);
    } else {
        ESP_LOGI(TAG, "RGB panel ready (num_fbs=%u, bounce=%u px = %u B x2 in internal DMA RAM)",
                 (unsigned)num_fbs, (unsigned)bounce_px,
                 (unsigned)(bounce_px * sizeof(uint16_t)));
    }

    /* 打印面板扫描率：与 LVGL 刷新率的比值是"微微闪/卡顿"的第一判据 */
    ESP_LOGI(TAG, "Panel scan %.1f Hz (PCLK %dMHz / %u px per frame)",
             bsp_lcd_scan_hz(), BSP_LCD_PIXEL_CLOCK_HZ / 1000000,
             (unsigned)bsp_lcd_pixels_per_frame());
    return ESP_OK;
}

/**
 * @brief 把面板的**每一块**帧缓冲都预填成同一颜色
 *
 * ★ 只在双 fb（num_fbs=2，即 avoid-tearing 方案）下才必须做：
 *   双 fb 是交替显示的，如果某一块 fb 从头到尾没被写过，切到它的那一帧就会把
 *   PSRAM 里的随机数据当像素打出去 —— 肉眼看到的是满屏雪花闪一下。
 *   num_fbs=1 时 esp_lcd_panel_draw_bitmap() 只能写到唯一那块 fb，退化成整屏刷色即可。
 *
 * 关于 cache：面板挂着 bounce buffer 时 IDF 不做 cache 同步
 * （见 esp_lcd_panel_rgb.c 里 `if (!rgb_panel->bb_size && ...)` 那段判断），
 * 因为 bounce 的补数 memcpy 与这里的写都走同一份 DCache，天然一致，无需 msync。
 */
static void bsp_lcd_fill_all_fbs(esp_lcd_panel_handle_t panel, uint16_t color)
{
    void *fbs[2] = {NULL, NULL};

    /* ★ 单 fb（当前的 PARTIAL_BB / DIRECT_FB 方案）**不要**去要 2 块：
     *   那样 IDF 会打一条 `E lcd_panel.rgb: invalid frame buffer number`，
     *   功能上虽会自动退回下面的"整屏刷一次"，但那条红色 ERROR 会被误当成故障。 */
    if (s_num_fbs < 2) {
        bsp_lcd_fill_solid(panel, color);
        return;
    }

    if (esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &fbs[0], &fbs[1]) != ESP_OK) {
        /* 保险：真的要不到 2 块，就退回整屏刷一次 */
        bsp_lcd_fill_solid(panel, color);
        return;
    }

    const size_t fb_px = (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES;
    const uint32_t color32 = ((uint32_t)color << 16) | (uint32_t)color;

    for (int i = 0; i < 2; i++) {
        if (fbs[i] == NULL) {
            continue;
        }
        /* 按 32bit 写，比逐像素快一倍 */
        uint32_t *p = (uint32_t *)fbs[i];
        for (size_t n = 0; n < fb_px / 2; n++) {
            p[n] = color32;
        }
    }
    ESP_LOGI(TAG, "Both frame buffers pre-filled with 0x%04X", color);
}

/*==============================================================================
 * 触摸
 *============================================================================*/
static i2c_master_bus_handle_t s_i2c_bus = NULL;

static esp_err_t bsp_touch_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_TOUCH_I2C_SDA,
        .scl_io_num = BSP_TOUCH_I2C_SCL,
        .i2c_port = BSP_TOUCH_I2C_PORT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "touch i2c bus init failed");
    ESP_LOGI(TAG, "Touch I2C: SDA=%d SCL=%d @%dHz", BSP_TOUCH_I2C_SDA, BSP_TOUCH_I2C_SCL,
             BSP_TOUCH_I2C_SPEED_HZ);
    return ESP_OK;
}

#if BSP_TOUCH_TYPE == BSP_TOUCH_TYPE_GT911
/*
 * GT911 的从机地址由“复位释放瞬间 INT 引脚电平”决定：低=0x5D，高=0x14。
 * 板上 INT 未接到 MCU 时驱动会走默认流程，地址猜错就会在读 0x8140 时 NACK。
 * 这里两个地址都探测一遍，探测不到再回落到默认值。
 * 注意：esp_lcd_touch_gt911 只保存 config 里的 driver_data 指针（不拷贝内容），
 * 所以相关结构体必须是静态存储。
 */
static uint8_t gt911_probe_addr(void)
{
    static const uint8_t addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,        /* 0x5D */
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, /* 0x14 */
    };
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        if (i2c_master_probe(s_i2c_bus, addrs[i], 100) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 found at 0x%02X", addrs[i]);
            return addrs[i];
        }
    }
    ESP_LOGW(TAG, "GT911 not answering on 0x%02X/0x%02X, fallback 0x%02X",
             addrs[0], addrs[1], addrs[0]);
    return addrs[0];
}
#endif /* GT911 */

static esp_err_t bsp_touch_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_touch_i2c_init(), TAG, "touch i2c init failed");

    /* 屏幕为 800x480 横屏原生方向，触摸不做 swap/mirror。
     * 若发现触摸坐标上下/左右颠倒，改这里的 swap_xy / mirror_x / mirror_y。 */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_RST,
        .int_gpio_num = BSP_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;

#if BSP_TOUCH_TYPE == BSP_TOUCH_TYPE_GT911
    static esp_lcd_touch_config_t s_tp_cfg;
    static esp_lcd_touch_io_gt911_config_t s_gt911_io_cfg;

    s_gt911_io_cfg.dev_addr = gt911_probe_addr();

    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.dev_addr = s_gt911_io_cfg.dev_addr;
    tp_io_cfg.scl_speed_hz = BSP_TOUCH_I2C_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io), TAG,
                        "gt911 panel io failed");

    s_tp_cfg = tp_cfg;
    s_tp_cfg.driver_data = &s_gt911_io_cfg;
    return esp_lcd_touch_new_i2c_gt911(tp_io, &s_tp_cfg, &s_touch);

#elif BSP_TOUCH_TYPE == BSP_TOUCH_TYPE_AXS15231
#if BSP_TOUCH_AXS15231_AVAILABLE
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    tp_io_cfg.scl_speed_hz = BSP_TOUCH_I2C_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io), TAG,
                        "axs15231 panel io failed");
    return esp_lcd_touch_new_i2c_axs15231b(tp_io, &tp_cfg, &s_touch);
#else
    /* 切到 AXS15231 时必须先引入驱动组件，否则不会编译通过 —— 这是故意的，
     * 避免静默地跑在“没有触摸”的状态。修法见 main/README.md「切换触摸控制器」。 */
#error "AXS15231 需要组件 espressif/esp_lcd_touch_axs15231b（见 main/README.md）"
#endif

#else
#error "未知的 BSP_TOUCH_TYPE"
#endif
}

/*==============================================================================
 * 对外接口
 *============================================================================*/
i2c_master_bus_handle_t bsp_display_i2c_bus(void)
{
    /* 触摸初始化失败时这里是 NULL：调用方（app_sensor）要能容忍 */
    return s_i2c_bus;
}

esp_err_t bsp_display_init(void)
{
    /* --- 0. 背光最先点亮 ---
     * 顺序很关键：背光放在所有可能失败的初始化之前。
     * 这样即使后面面板/LVGL 初始化失败，屏幕也是亮的，能一眼区分
     *   「屏幕黑 = 背光/供电没起来」 和 「屏幕亮但没图 = 面板时序/PCLK 不对」。 */
    ESP_RETURN_ON_ERROR(bsp_backlight_init(), TAG, "backlight gpio init failed");
    bsp_display_backlight(true);
    vTaskDelay(pdMS_TO_TICKS(30));

    /* --- 1. 面板 --- */
    esp_err_t err = bsp_lcd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* --- 1.5 面板自检（不经过 LVGL，先确认硬件通路） --- */
#if BSP_LCD_SELF_TEST
    /* 白：最容易肉眼判断。注意这里要刷**每一块** fb，
     * 双 fb 方案下漏掉一块会在切到它时闪出满屏雪花（见函数注释）。 */
    bsp_lcd_fill_all_fbs(s_panel, BSP_LCD_INIT_FB_COLOR);
#endif

    /* --- 2. LVGL port：建 LVGL 任务 + tick 定时器 --- */
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = BSP_LVGL_TASK_PRIO,
        .task_stack = BSP_LVGL_TASK_STACK,
        .task_affinity = -1,          /* 不绑定核心，由调度器决定 */
        .task_max_sleep_ms = 500,     /* 无事件时最长睡 500ms */
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,         /* LVGL tick 精度 */
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");

    /* --- 3. 注册显示 --- */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = NULL,            /* RGB 屏没有 panel IO */
        .panel_handle = s_panel,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = 0,
            /* ★ 仅方案 A / C（LVGL 自带 draw buffer）生效，必须是 0（内部 RAM）：
             *   放 PSRAM 会让 LVGL 渲染持续占用 PSRAM 通路，把 bounce buffer 在
             *   GDMA EOF 中断里的 PSRAM memcpy 挤晚 -> LCD 断流（抖动/花屏）。
             *   方案 B（avoid_tearing）不使用这里的缓冲，LVGL 直接画面板的 fb。 */
            .buff_spiram = 0,
            .sw_rotate = 0,
            .full_refresh = 0,      /* 方案 B 里会被置 1 —— 那一条是**必须**的，见下面的长注释 */
            .direct_mode = 0,       /* 方案 B 里会被置 1 */
        },
    };
    lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = 0,
            .avoid_tearing = 0,
        },
    };

#if BSP_LVGL_DISPLAY_MODE == BSP_DISPLAY_MODE_AVOID_TEARING
    /* 方案 B：直接用面板的两块帧缓冲当 LVGL 绘制缓冲。
     * 必须配 direct_mode，否则 LVGL 会往"正在被扫描的那块 fb"上画脏区 -> 撕裂。
     * bb_mode 仍要置 1（面板确实挂着 bounce buffer），否则 esp_lvgl_port 会去等
     * 一个不会到来的 on_vsync 事件流，刷新时序会对不上。 */
    disp_cfg.buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES;
    disp_cfg.double_buffer = true;
    disp_cfg.flags.direct_mode = 1;
    /* ★★★ 这一行是方案 B 能不能用的关键（上一版就是漏了它才"界面乱"）★★★
     *   direct_mode + 双缓冲时，LVGL 默认只把**脏区**画进"后台那块 fb"，
     *   而两块 fb 是交替显示的 ⇒ 没被重画过的区域会一直留着 = "控件错位/只画一半/
     *   局部旧内容"。`full_refresh = 1` 让 LVGL **每帧把整屏重画进后台 fb**，
     *   两个 fb 始终是完整画面。LVGL 手册对 direct_mode 双缓冲就是这么要求的。 */
    disp_cfg.flags.full_refresh = 1;
    rgb_cfg.flags.avoid_tearing = 1;
    rgb_cfg.flags.bb_mode = 1;
    ESP_LOGW(TAG, "Display mode: avoid-tearing + bounce | LVGL %dx%d x2 = PSRAM frames",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
#elif BSP_LVGL_DISPLAY_MODE == BSP_DISPLAY_MODE_DIRECT_FB
    /* 方案 C：LVGL 用 2 块 20 行内部 RAM 缓冲做局部刷新；面板侧没有 bounce，
     * GDMA 直接从 PSRAM 帧缓冲取数。注意 bb_mode 必须为 0：
     * esp_lvgl_port 在 bb_mode=1 时会去注册 on_bounce_frame_finish 回调，
     * 而没有 bounce 的面板永远不会触发它 -> 等不到信号，LVGL 刷新会卡住。 */
    disp_cfg.buffer_size = BSP_LCD_H_RES * BSP_LVGL_BUFFER_HEIGHT;
    disp_cfg.double_buffer = true;
    rgb_cfg.flags.bb_mode = 0;
    ESP_LOGI(TAG, "Display mode: direct-fb (no bounce) | LVGL %d lines x2 = %u B in INTERNAL RAM",
             BSP_LVGL_BUFFER_HEIGHT,
             (unsigned)(BSP_LCD_H_RES * BSP_LVGL_BUFFER_HEIGHT * 2 * sizeof(lv_color_t)));
#else
    /* 方案 A：LVGL 用 2 块 20 行内部 RAM 缓冲做局部刷新 + 面板 bounce buffer 输出 */
    disp_cfg.buffer_size = BSP_LCD_H_RES * BSP_LVGL_BUFFER_HEIGHT;
    disp_cfg.double_buffer = true;
    rgb_cfg.flags.bb_mode = 1;
    ESP_LOGI(TAG, "Display mode: partial + bounce | LVGL %d lines x2 = %u B in INTERNAL RAM",
             BSP_LVGL_BUFFER_HEIGHT,
             (unsigned)(BSP_LCD_H_RES * BSP_LVGL_BUFFER_HEIGHT * 2 * sizeof(lv_color_t)));
#endif

    ESP_LOGI(TAG, "Internal RAM before display: free %u, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed (mode=%d, buffer=%u px)",
                 BSP_LVGL_DISPLAY_MODE, (unsigned)disp_cfg.buffer_size);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LVGL display registered");

    /* --- 3.5 按实际面板帧周期自动设置 LVGL 刷新周期（慢 BSP_LVGL_SLOWDOWN_PCT%） ---
     * LVGL 8.3 没有 lv_disp_set_refr_period()，这里直接改公开字段 refr_timer。
     * 做成自动计算后，menuconfig 里改 PCLK 不需要再去同步 sdkconfig 的刷新周期，
     * 也就不会再出现"LVGL 和面板同频 -> 差频拍频 -> 微微在闪"这类问题。 */
    const uint32_t lvgl_period_ms = bsp_calc_lvgl_period_ms();
    if (s_disp->refr_timer != NULL) {
        s_disp->refr_timer->period = lvgl_period_ms;
        lv_timer_reset(s_disp->refr_timer);
    }
    ESP_LOGI(TAG, "LVGL refresh auto-set: %u ms (%.1f Hz) vs panel %.1f Hz",
             (unsigned)lvgl_period_ms, 1000.0f / (float)lvgl_period_ms, bsp_lcd_scan_hz());

    /* --- 4. 注册触摸（失败不致命：至少屏幕能用） --- */
    esp_err_t ret = bsp_touch_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (0x%x), continue without touch", ret);
        s_touch = NULL;
    } else {
        lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = s_touch,
            .scale = {
                .x = 1.0f,
                .y = 1.0f,
            },
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed");
        } else {
            ESP_LOGI(TAG, "Touch input registered");
        }
    }

    /* --- 5. 面板已开始扫帧，点亮背光 --- */
    bsp_display_backlight(true);

    ESP_LOGI(TAG, "Display ready: %dx%d, LVGL %d.%d.%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES,
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;
}

lv_display_t *bsp_display_get(void)
{
    return s_disp;
}

esp_lcd_touch_handle_t bsp_display_get_touch(void)
{
    return s_touch;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}
