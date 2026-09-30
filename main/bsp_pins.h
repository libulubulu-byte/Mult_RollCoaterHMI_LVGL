/**
 * @file bsp_pins.h
 * @brief 板级引脚与面板时序宏定义（换屏/换板只改这一个文件）
 *
 * 板型由 menuconfig 选择（Kconfig.projbuild），也可在编译时用 -DBSP_BOARD=xxx 覆盖：
 *   BSP_BOARD_JC8048W550   : JC8048W550 / ESP32-8048S050（默认，本机实测能出图）
 *   BSP_BOARD_CROWPANEL_50 : Elecrow CrowPanel 5.0 HMI
 *
 * 两块板子的 RGB 走线是同一套参考设计：
 *   数据线 DATA0..15 = B1,B2,B3,B4,B5, G0..G5, R1..R5   （RGB565，蓝在低位）
 *   同步线 VSYNC=41 / HSYNC=39 / DE=40 / 背光=GPIO2
 *   触摸   GT911 SDA=19 / SCL=20
 * 差别只有：
 *   PCLK   JC8048W550=GPIO42，CrowPanel=GPIO0
 *   TP_RST JC8048W550=GPIO38，CrowPanel=NC（板上未引出）
 *
 * ★★★ PCLK 选错的典型现象：背光亮、屏幕全黑 ★★★
 *   PCLK 不对 -> 面板拿不到像素时钟 -> 数据线一直输出 0 -> 整屏黑。
 *   所以「屏幕不亮/全黑」第一件事就是核对这里选的板型。
 *   实测：本机这块 5 寸屏（SHT30_backled_5inch 工程验证过）用的是 GPIO42。
 */
#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"   /* BSP_TOUCH_I2C_PORT 用到的 I2C_NUM_x */

/*==============================================================================
 * 一、板型选择
 *============================================================================*/
#define BSP_BOARD_CROWPANEL_50  1
#define BSP_BOARD_JC8048W550    2

#if defined(CONFIG_ROLLCOATER_BOARD_CROWPANEL_50)
#define BSP_BOARD BSP_BOARD_CROWPANEL_50
#elif defined(CONFIG_ROLLCOATER_BOARD_JC8048W550)
#define BSP_BOARD BSP_BOARD_JC8048W550
#endif

#ifndef BSP_BOARD
/* 无 menuconfig 时的兜底：用本机实测过的那套（PCLK=GPIO42） */
#define BSP_BOARD BSP_BOARD_JC8048W550
#endif

/*==============================================================================
 * 二、触摸控制器选择
 *============================================================================*/
#define BSP_TOUCH_TYPE_GT911     1
#define BSP_TOUCH_TYPE_AXS15231  2

#if defined(CONFIG_ROLLCOATER_TOUCH_AXS15231)
#define BSP_TOUCH_TYPE BSP_TOUCH_TYPE_AXS15231
#else
#define BSP_TOUCH_TYPE BSP_TOUCH_TYPE_GT911
#endif

/*==============================================================================
 * 三、共同项（两块板一致）
 *============================================================================*/
#define BSP_LCD_H_RES                   (800)
#define BSP_LCD_V_RES                   (480)

/* 水平/垂直时序（由屏厂 800x480 RGB 例程给出：HBP/HFP=8/8，VBP/VFP=8/8） */
#define BSP_LCD_HSYNC_PULSE_WIDTH       (4)
#define BSP_LCD_HSYNC_BACK_PORCH        (8)
#define BSP_LCD_HSYNC_FRONT_PORCH       (8)
#define BSP_LCD_VSYNC_PULSE_WIDTH       (4)
#define BSP_LCD_VSYNC_BACK_PORCH        (8)
#define BSP_LCD_VSYNC_FRONT_PORCH       (8)

/* 极性：对齐屏厂例程（同步信号空闲低电平，像素时钟下降沿采样）。
 * 若画面整体偏移/抖动，或红蓝颠倒，优先调这里与数据线顺序。 */
#define BSP_LCD_HSYNC_IDLE_LOW          (true)
#define BSP_LCD_VSYNC_IDLE_LOW          (true)
#define BSP_LCD_PCLK_ACTIVE_NEG         (true)

/* RGB 数据总线（B1..B5, G0..G5, R1..R5）*/
#define BSP_LCD_DATA0                   (GPIO_NUM_8)    /* B1 */
#define BSP_LCD_DATA1                   (GPIO_NUM_3)    /* B2 */
#define BSP_LCD_DATA2                   (GPIO_NUM_46)   /* B3 */
#define BSP_LCD_DATA3                   (GPIO_NUM_9)    /* B4 */
#define BSP_LCD_DATA4                   (GPIO_NUM_1)    /* B5 */
#define BSP_LCD_DATA5                   (GPIO_NUM_5)    /* G0 */
#define BSP_LCD_DATA6                   (GPIO_NUM_6)    /* G1 */
#define BSP_LCD_DATA7                   (GPIO_NUM_7)    /* G2 */
#define BSP_LCD_DATA8                   (GPIO_NUM_15)   /* G3 */
#define BSP_LCD_DATA9                   (GPIO_NUM_16)   /* G4 */
#define BSP_LCD_DATA10                  (GPIO_NUM_4)    /* G5 */
#define BSP_LCD_DATA11                  (GPIO_NUM_45)   /* R1 */
#define BSP_LCD_DATA12                  (GPIO_NUM_48)   /* R2 */
#define BSP_LCD_DATA13                  (GPIO_NUM_47)   /* R3 */
#define BSP_LCD_DATA14                  (GPIO_NUM_21)   /* R4 */
#define BSP_LCD_DATA15                  (GPIO_NUM_14)   /* R5 */

#define BSP_LCD_VSYNC                   (GPIO_NUM_41)
#define BSP_LCD_HSYNC                   (GPIO_NUM_39)
#define BSP_LCD_DE                      (GPIO_NUM_40)
#define BSP_LCD_BL                      (GPIO_NUM_2)    /* 背光，高电平点亮 */
#define BSP_LCD_DISP                    (GPIO_NUM_NC)   /* 无独立 DISP 脚 */

/*------------------------------------------------------------------------------
 * ★ 背光驱动方式：本工程改成 LEDC PWM（bsp_display.c 里的默认值是 0 = 直流）
 *
 * 为什么必须改成 1：
 *   SETTINGS 页的「Display Brightness」滑条和「Auto Screen Off」都要真的能调亮度、
 *   能熄灭背光，直流 GPIO 只能开/关（亮度 1% 和 100% 没区别）。
 *
 * 副作用与回退办法：
 *   LEDC 在 10bit 下占空比上限是 1023/1024，不是"绝对恒高"，官方也提示过
 *   它可能成为"画面静止仍微微闪"的一个变量。如果遇到那种闪烁，
 *   把下面这行改成 (0) 即可退回直流背光 —— 代价是亮度滑条退化成开/关。
 *   两种方式都由 bsp_display.c 里的 bsp_backlight_init() 自动处理。
 *----------------------------------------------------------------------------*/
#define BSP_LCD_BL_USE_LEDC             (1)

#define BSP_LCD_DATA_WIDTH              (16)
#define BSP_LCD_BITS_PER_PIXEL          (16)

/*------------------------------------------------------------------------------
 * ★ PCLK（像素时钟）= 画质总开关，menuconfig 里可调
 *   -> Roll Coater HMI 配置 -> RGB 面板像素时钟 PCLK (MHz)
 *
 * 扫描率 = PCLK / ((H+hsync+hbp+hfp) * (V+vsync+vbp+vfp))，本板 = 820 x 500：
 *     12MHz -> 29.3Hz   (带宽 ≈ 22.5MB/s)  最稳，但大色块有液晶闪烁
 *     16MHz -> 39.0Hz   (带宽 ≈ 30.0MB/s)
 *     18MHz -> 43.9Hz   (带宽 ≈ 33.7MB/s)  ★ 默认，闪烁与搬运压力的折中
 *     24MHz -> 58.5Hz   (带宽 ≈ 45.0MB/s)  bounce 只剩约 30% 余量，不建议超过
 *
 * 方向判断（默认方案 PARTIAL_BB：单 fb + bounce，DMA 只读内部 RAM）：
 *   只有某个大色块在闪（绿色 Enter 键最典型）-> **面板帧率偏低**造成的液晶闪烁。
 *       0x2E7D32 的绿是 31/31 满量程，驱动电压最大、帧间漏电最明显，
 *       所以最先被看出来。往上调 PCLK 能减轻。
 *   偶发一条"半新半旧"的横带           -> 写读竞争（用 AVOID_TEARING 解决，
 *                                         与 PCLK 无关，调 PCLK 没用）。
 *   横向抖动 / 花屏 / 整体错位         -> 要么实时带宽不够（往下调），
 *                                         要么 PCLK 太高导致 bounce 补数超时。
 * 本工程默认 **18MHz / 43.9Hz**：兼顾闪烁与 CPU 搬运量。
 * 上限提醒：bounce 补数要把整屏从 PSRAM 搬一遍，PCLK 越高总搬运量越大
 * （18MHz 时约 33.7MB/s、CPU 占用 40%+），24MHz 以上容易补数超时反而变糟。
 * LVGL 刷新周期不用手工配 —— bsp_display.c 会按"面板帧周期 x (1+SLOWDOWN)"自动设置。
 *----------------------------------------------------------------------------*/
#ifdef CONFIG_ROLLCOATER_LCD_PCLK_MHZ
#define BSP_LCD_PIXEL_CLOCK_HZ          (CONFIG_ROLLCOATER_LCD_PCLK_MHZ * 1000 * 1000)
#else
#define BSP_LCD_PIXEL_CLOCK_HZ          (18 * 1000 * 1000)
#endif

/* bounce buffer 行数。★ 仅当 BSP_DISPLAY_MODE_PARTIAL_BB（老方案）时使用，
 * 默认的 DIRECT_FB 方案不需要它。
 * 机制：DMA 从内部 RAM 流式输出，CPU 在 GDMA EOF 中断里从 PSRAM 帧缓冲补数据。
 * 块越大，单次 DMA 断流的容忍时间越长（40 行 = 64KB，@16MHz 约 2ms），
 * 代价是占内部 DMA RAM：2 块 x 64KB = 128KB，且 CPU 占用 30%+。
 * 注意 800*480 必须能被它整除以保证每帧正好 12 段。 */
#define BSP_LCD_BOUNCE_BUFFER_HEIGHT    (40)

/* 触摸 I2C。100k 在本类排线上更稳：esp_lcd 的 I2C 传输用无限超时，
 * 一旦 NACK/挂死会把 LVGL 任务拖住并触发 Task WDT。 */
#define BSP_TOUCH_I2C_PORT              (I2C_NUM_0)
#define BSP_TOUCH_I2C_SPEED_HZ          (100000)
#define BSP_TOUCH_I2C_SDA               (GPIO_NUM_19)
#define BSP_TOUCH_I2C_SCL               (GPIO_NUM_20)

/*==============================================================================
 * 四、板型差异项
 *============================================================================*/
#if BSP_BOARD == BSP_BOARD_CROWPANEL_50
/* --- Elecrow CrowPanel 5.0 HMI ---
 * 引脚来源：Elecrow 官方 wiki / CrowPanel-5.0-HMI-ESP32-Display-800x480 例程
 *   PCLK = GPIO0，其余同参考设计。 */
#define BSP_LCD_PCLK                    (GPIO_NUM_0)
#define BSP_TOUCH_RST                   (GPIO_NUM_NC)  /* 板上未引出，走外部复位 */
#define BSP_TOUCH_INT                   (GPIO_NUM_NC)
/* PCLK 由 CONFIG_ROLLCOATER_LCD_PCLK_MHZ 统一决定，见文件上方说明 */

#elif BSP_BOARD == BSP_BOARD_JC8048W550
/* --- JC8048W550 / ESP32-8048S050（本机调试板，已实测出图）--- */
#define BSP_LCD_PCLK                    (GPIO_NUM_42)
#define BSP_TOUCH_RST                   (GPIO_NUM_38)
#define BSP_TOUCH_INT                   (GPIO_NUM_NC)
/* PCLK 由 CONFIG_ROLLCOATER_LCD_PCLK_MHZ 统一决定，见文件上方说明 */

#else
#error "未知的 BSP_BOARD，请检查 Kconfig.projbuild / bsp_pins.h"
#endif
