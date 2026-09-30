/**
 * @file app_motion.c
 * @brief 步进轴运动控制实现（RMT 脉冲 + 梯形曲线 + 报警处理 + 模拟模式）
 *
 * 曲线说明（梯形速度曲线）：
 *   每一 tick（默认 5ms）做一次数值积分：
 *     1. 由「剩余距离」反推本 tick 允许的最大速度  v_stop = sqrt(2 * decel * remaining)
 *        这一步保证任何时刻都能刹得住，因此自动形成 加速段 / 匀速段 / 减速段；
 *     2. 当前速度向 min(v_max, v_stop) 逼近：低于就按 accel 加速，高于就按 decel 减速；
 *     3. 本 tick 应发脉冲数 = 速度 × dt，小数部分累加到下一次，避免低速时丢脉冲；
 *     4. 脉冲数不会超过剩余步数，因此落点精确。
 *
 * 为什么不用 examples/peripherals/rmt/stepper_motor 里的曲线编码器：
 *   那个例子里加速/减速是固定的频率表（start_freq -> end_freq），
 *   只能表达「固定行程」的加速段与减速段。本工程的目标位置由操作员随时输入，
 *   行程与巡航速度都是变量，用「逐 tick 积分 + copy encoder 发脉冲」可以精确覆盖
 *   任意行程，同时天然支持中途报警中止与实时位置回读。
 *   参考写法（RMT TX 通道 + copy encoder + transmit/wait）与该示例保持一致。
 */
#include <math.h>
#include <string.h>
#include <stdio.h>             /* printf：串口命令行（mtest）输出 */
#include <stdlib.h>            /* strtof / strtoul / strtol：串口命令行解析 */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"       /* esp_rom_delay_us：换向后等 DIR 稳定 */
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "app_motion.h"
#include "app_config.h"

static const char *TAG = "app_motion";

/* 无电机调试模式：统一从 app_motion.h 的 APP_MOTION_SIMULATE 取
 * （那里解释了为什么不能直接判断 CONFIG_ROLLCOATER_MOTION_SIMULATE）。 */
#define MOTION_SIMULATE   (APP_MOTION_SIMULATE)

/*==============================================================================
 * 串口调试命令行（mtest）—— 现场"换挡试转"用（menuconfig：
 * Roll Coater HMI 配置 -> ROLLCOATER_MOTION_CONSOLE，见 9.11）
 *
 * 为什么要它：日志已经能证明「脉冲数 / 速率 / DIR / ENA 全对」，剩下能在软件里
 * 扫的变量只有两个 —— **脉宽**（3.3V 直驱光耦时 5µs 窄脉冲可能丢）和
 * **频率**（=speed×步数/mm；这台机器低频不走、高频拖不动，只有中间一段能用）。
 * 与其让现场反复改代码/烧录，不如给一条命令：敲 `mtest 10000 50` 就按
 * 10000Hz + 50µs 正反转各跑一趟，自己听/看哪一档不卡。
 *
 * 控制台是 UART0（CONFIG_ESP_CONSOLE_UART_DEFAULT=y，115200）。
 * ★ 本板必须用板载 COM 口，不能用原生 USB（IO19/20 被 GT911 的 I2C 占了，见 9.1）。
 *============================================================================*/
#ifdef CONFIG_ROLLCOATER_MOTION_CONSOLE
#define MOTION_CLI_ON   (1)
#else
#define MOTION_CLI_ON   (0)
#endif

#if MOTION_CLI_ON && !MOTION_SIMULATE
#include "esp_console.h"
#endif

/* 曲线积分步长 */
#ifndef CONFIG_ROLLCOATER_MOTION_TICK_MS
#define CONFIG_ROLLCOATER_MOTION_TICK_MS 5
#endif
#define MOTION_TICK_MS    (CONFIG_ROLLCOATER_MOTION_TICK_MS)

/* 位置推送到 UI 的周期 */
#ifndef CONFIG_ROLLCOATER_UI_PUSH_MS
#define CONFIG_ROLLCOATER_UI_PUSH_MS 100
#endif

#if !MOTION_SIMULATE
#include "driver/rmt_tx.h"
#include "esp_attr.h"          /* IRAM_ATTR：RMT 完成回调要放进 IRAM */
#if MOTION_STEP_PWM_MODE
#include "driver/ledc.h"       /* ★ v22：STEP 用 LEDC 硬件 PWM 直接产生（见 MOTION_STEP_PWM_MODE）*/
#endif
#include "stepper_motor_encoder.h"  /* ★ v16：台架同款 uniform 编码器（原样拷入本工程） */
/* RMT 内部符号块大小。必须为偶数且 ≥ SOC_RMT_MEM_WORDS_PER_CHANNEL(48)。
 *
 * ★★★ v23（只影响 `MOTION_STEP_PWM_MODE = 0` 的 RMT 退路）★★★
 *   官方例程用 64 ⇒ 驱动分配 2 个内存块 = **96 个符号**。
 *   这里改成 **192** ⇒ 4 个内存块 = **192 个符号**，于是"一个事务"能装下
 *   ~190 个脉冲（@12000Hz ≈ **15.8ms**），把"事务边界空档"的出现次数再砍一半
 *   （边界越少 ⇒ 空档越少，机理见 motion_rmt_init 的长注释）。
 *   ⚠️ 别的模块如果也在用 RMT，192 个符号可能申请不到 ——
 *      `motion_rmt_init()` 里对此**有兜底**：失败会自动退回 64（不会拖挂启动）。 */
#define MOTION_RMT_MEM_SYMBOLS  (192)
#define MOTION_RMT_QUEUE_DEPTH  (4)

/** RMT「发送完成」中断的优先级（1~3，越大越高；**0 = 用驱动默认档**）。
 *  ★ 现场"IO11 方波不连续"的直接相关项：IDF 驱动在**每个事务结尾**都会写一个
 *    `duration0 = 0` 的 stop 符号让硬件停一下，再由这个中断去启动下一个事务 ——
 *    **中断延迟就是那段空档的长度**。原来用默认（最低）档，CPU 一忙（LVGL 全屏
 *    重绘 / WiFi / 屏的 DMA 中断）空档就从几微秒涨到几十~几百微秒。
 *    ⇒ 提到较高档是本工程能做的、最直接的补偿（详见 motion_rmt_init 的说明）。
 *  ⚠️ 若这一档在当前芯片/IDF 上不被允许，`rmt_new_tx_channel` 会返回错误，
 *    motion_rmt_init 里会**自动退回默认档重试**，不会把启动拖挂。 */
#ifndef MOTION_RMT_INTR_PRIORITY
#define MOTION_RMT_INTR_PRIORITY  (2)
#endif

/* STEP 高电平脉宽（µs）。
 *
 * 2026-09-22 现场用逻辑分析仪对比过两版波形（见 main/README.md 9.7「示波器实测」）：
 *   本工程（当时 10µs）：周期 125µs / 高 10µs / 低 115µs   ← 8kHz，speed=10mm/s
 *   参考那版（5µs）    ：周期 64.3µs / 高 5.5µs / 低 58.8µs ← 15.5kHz
 * 两者**形状本来就一致**（都是"窄高脉冲 + 长低电平"，占空比都在 8% 左右），
 * 差别只有频率（= speed）和脉宽。为了与参考波形逐项对齐、少一个变量，
 * 这里把脉宽**改回 5µs**（TB6600 手册的 PUL 最小脉宽是 2.5µs，5µs 是通行值）。
 * ★★ 2026-09-22 晚：改成 menuconfig 可调 —— 它是"最后一个未排除的软件变量" ★★
 *
 *   现场证据（见 main/README.md 9.7 第 1 步）：`脉冲核对 OK：请求 16000 个脉冲
 *   / 10025 个事务` + 耗时 10.4s = 16000÷1600 + `DIR=IO12(1) ENA=IO13(0)` 无 ★★，
 *   即**每个脉冲都在正确速率上出了引脚、两根控制线也确认到位**，电机却仍然只抖动。
 *   这时候唯一还没排除的软件变量就是**脉宽**：
 *     TB6600 是**光耦隔离输入**，而本工程是 3.3V 直驱 5V 光耦（光耦电流只有约 2mA，
 *     见 9.2），光耦 LED 的上升沿本来就慢；5µs 的窄脉冲在"低电流 + 慢光耦"下
 *     可能被判成"时有时无" → 丢步 / 原地抖动。
 *   把它调宽（25 ~ 50µs，或直接给 50% 方波）就能**一分钟内证实或排除**这一条。
 *
 * menuconfig：`Roll Coater HMI 配置 -> STEP 高电平脉宽 (µs，0 = 50% 方波)`
 * ★ 低电平永远不少于半个周期，所以填多大都不会把低电平挤没。
 * ★ 默认 5µs 时输出与旧固件**逐位相同**（钳位见 motion_emit_steps 里的说明）。 */
#ifdef CONFIG_ROLLCOATER_STEP_PULSE_US
#define MOTION_STEP_PULSE_US    (CONFIG_ROLLCOATER_STEP_PULSE_US)
#else
#define MOTION_STEP_PULSE_US    (5)
#endif

/* ★★ RMT 队列里要始终保持的「待发 + 在发」事务数 —— 脉冲连续性的关键 ★★
 *
 * 硬件执行完一个事务会**立刻**开始队列里的下一个，全程不需要 CPU 介入。
 * 只要队列里始终有货，STEP 脉冲串就不会被中断。
 * 取 2 = 一个正在发 + 一个排队等待，留一份余量容忍本任务被抢占。
 * （必须 < MOTION_RMT_QUEUE_DEPTH） */
#define MOTION_TX_QUEUE_TARGET  (3)

/** motion_task 的优先级（10，与同现场 Arduino 版一致）。
 *  ★ 上电固定段 / `mtest hold` 在补队列期间也会**临时**提到这个优先级：
 *    它们跑在 app_main(1) / 控制台任务(3) 上，被抢占几毫秒就会把 RMT 队列抽空
 *    ⇒ 脉冲断档 ⇒ 现场"起转卡一下 / 一顿一顿"。 */
#define MOTION_TASK_PRIO        (10)

/* ★★ 正常运动路径的**事务批大小**（ms）—— v12 ★★
 *
 * 为什么要有它：上电固定段（每事务 64 脉冲 ≈ 7.5ms）跑起来连续、不打顿，而界面运动
 * 原来是"**每 1ms 提交一个事务**"（8500Hz 时一个事务只有 8~9 个脉冲）——
 * 队列里 3 个事务总共才约 3ms 的量，一旦 motion_task 被 WiFi(prio 23) 之类抢占几毫秒，
 * RMT 队列就见底 ⇒ **脉冲串断档** ⇒ 现场就是"走起来一顿一顿 / 起转顿一下"
 * （这跟上电固定段中途"脉冲落后"是同一个机理，只是运动路径更脆）。
 *
 * 批到 15ms 后（v23：配合"190 符号/事务"的大事务）：队列 3 个事务 = **~45ms 余量**
 *   （抗抢占能力 ≈ ×15），事务数也少 15 倍（CPU 更省）。
 * ★★★ 更关键的是：**事务边界越少，"事务边界空档"就越少** ★★★
 *   （每个事务结尾驱动都会写 stop 符号让硬件停一下、再由中断重启下一个事务，
 *     机理见 motion_rmt_init 的长注释；所以这里"批大一点"不只是省 CPU）。
 * ★ 速度积分步长仍是 1ms（MOTION_PROFILE_DT_S）：只是"多久提交一次"变了，
 *   曲线的平滑度、加减速曲线、落点精度都不受影响。
 * ★ 用**拍数**（而不是累加秒数）判断攒够没 —— 1ms×N 在浮点里累加不保证正好等于 0.00N。 */
#define MOTION_TX_BATCH_MS      (15)
/** motion_task 填充循环"连续多少拍没交事务就让出 CPU"。
 *  真机：≥ 攒批拍数（否则每 1ms 只推进一步曲线、队列填不满 = 白改）；
 *  模拟模式：没有 RMT 硬件当节拍器，1 拍就退出，保持"实时"手感。 */
#if MOTION_SIMULATE
#define MOTION_FILL_IDLE_LIMIT  (1)
#else
#define MOTION_FILL_IDLE_LIMIT  (MOTION_TX_BATCH_MS)
#endif

/* ★★ 一个 RMT 事务里最多摆多少个 STEP 符号（= 多少个脉冲）★★
 *
 * 【这里为什么不再用 loop_count 重复同一个符号 —— 2026-09-22 修"马达不转"】
 *
 * 老写法：一个事务只编码 **1 个**脉冲符号 + `loop_count = n-1`，指望硬件把
 * 这个符号循环 n 次。它和官方 musical_buzzer 例程的用法一样，但对我们这个
 * 场景有一个致命的不确定点：
 *   驱动在编码结束后会**紧跟一个 duration=0 的结束符**
 *   （rmt_tx.c 的 rmt_tx_mark_eof()："a RMT word whose duration is zero means
 *   a 'stop' pattern"），也就是写完我们的符号，第 1 个符号位置就是"停止符"。
 *   循环模式到底是「循环这一段 payload」还是「循环整块内存（96 个符号，其中
 *   94 个是从没写过的残留数据）」——文档只说了"完成一轮后重新开始"，没有把
 *   结束符和循环边界的关系写死。一旦按后者解释，就会同时出现两种现场现象：
 *     a) 每个循环周期被 94 个残留符号的 duration 拉长（残留值不确定），
 *        实际 STEP 频率远低于指令值 -> 电机只是抖、不转；
 *     b) 循环永远收不了尾 -> on_trans_done 不来 -> s_tx_pending 只增不减
 *        -> 队列填满后 motion_run_profile() 再也不被调用 -> 后面**一个脉冲
 *        都不发**（连"到位"事件都不会有）。
 *   两种情况都符合现场那句"马达不转"，而且 `rmt_transmit()` 全程返回 ESP_OK，
 *   日志里一条报错都没有 —— 属于最难查的那种"静默失效"。
 *
 * 现在改成：**一个事务里老老实实摆 n 个脉冲符号，loop_count 恒为 0**（不循环）。
 * 于是三个数永远相等、一眼可核：
 *     写进 RMT 的符号数 = 要发的脉冲数 = on_trans_done 报告的 num_symbols - 1
 * （末尾那个 1 是驱动自动补的结束符）。不再依赖任何循环语义。
 * 这也是官方 peripherals/rmt/stepper_motor 例程加速/减速段的做法。
 *
 * 容量依据：曲线积分步长恒为 1ms（MOTION_PROFILE_DT_S），速度上限
 * MOTION_STEP_FREQ_MAX_HZ=20kHz -> 每拍最多 20 个脉冲。
 * 硬件内存（v23）：MOTION_RMT_MEM_SYMBOLS(192) ⇒ 实得 **4 个内存块 = 192 个符号**，
 * 减去驱动自动补的 1 个结束符 ⇒ payload 最多 191；这里取 **190** 留 1 个余量。
 *   @12000Hz 时 190 个脉冲 ≈ **15.8ms/事务**，队列里 3 个 ≈ **47ms 存货**。
 *   （若内存只申请到 64/2 块，payload 会被驱动**分段流式编码**，功能不受影响、
 *     只是事务内部多几次分段中断 —— 见 motion_rmt_init 的兜底。）
 *
 * ★★★ 64 → 88（v21）→ 190（v23）：**"事务越大，事务边界的空档越少"** ★★★
 *   事务边界 = 驱动写 stop 符号让硬件停一下、再由中断重启下一个事务的**空档**
 *   （见 motion_rmt_init 的长注释）⇒ 事务做长，同样一段行程里空档次数直接变少。
 *   ⚠️ 这条只对 `MOTION_STEP_PWM_MODE = 0`（RMT 退路）有意义：默认的 PWM 路径
 *      根本没有"事务"这个概念。 */
#define MOTION_MAX_STEPS_PER_TX (190)

/* ★ 速度曲线的更新周期（秒）—— 为什么用 1ms，而不是 Kconfig 里的 5ms ★
 *
 * 这个值决定「速度台阶」的大小：每一拍速度变化 a_acc × dt。
 *   accel = 150mm/s² = 120000 步/s² 时：
 *      dt = 5ms → 每拍跳 600 步/s（= 满速 16000 的 3.75%）
 *      dt = 1ms → 每拍跳 120 步/s（= 0.75%）
 * 现场实测现象：「转半圈之后开始原地振动」—— 电机能起步、能加速，但加速到
 * 接近高速段时脱出同步。这正是步进电机在**转矩拐点**附近的典型表现，
 * 而 5ms / 3.75% 一档的速度突变本身就是周期性冲击，会把拐点提前。
 * 同现场的 Arduino 版用 **1ms**（motor.cpp 的 motor_task：vTaskDelayUntil 1ms、
 * dt = 0.001f），台阶小 5 倍，同一个电机同一路电源却能跑完 20mm/s。
 * 所以这里与它对齐，改成 1ms。
 * 注意：切成 1ms 后每拍的脉冲数变少（16kHz 时 80 → 16 个），
 * 所以 RMT 的队列目标深度相应从 2 提到 3，保证硬件仍有足够存货连续发射。 */
#define MOTION_PROFILE_DT_S     (0.001f)
#endif

/*==============================================================================
 * 内部状态（全部只由 motion_task 读写，其它任务只读 s_report_pos / s_latched_alarm）
 *============================================================================*/
static QueueHandle_t s_cmd_q = NULL;      /* float 目标位置 */
static QueueHandle_t s_report_q = NULL;   /* motion_report_t */

/* ★★★ v18：**逻辑位置**（步）—— "轴应该在哪"，方向/距离一律按它算 ★★★
 *
 * 为什么必须单独有它（"一会正转一会反转"的根因，已用独立仿真复现）：
 *   旧写法用 `s_pos_steps`（= 已发脉冲累计）当位置来算 delta，而反向间隙补偿会把
 *   **额外的 backlash 脉冲**也算进 s_pos_steps ⇒ 两者相差 ±160 步(0.2mm)。
 *   于是"再下发一次同一个目标"会被算成 delta = ±160 ⇒ 轴**反向**短走 320 步(0.4mm)，
 *   而读数一直显示目标值不变 ⇒ 现场看到"正转后反转 / 一会正转一会反转"。
 *   仿真（目标 40→0→40 之后再连下发三次 40）：旧实现每次 ±320 步来回，
 *   按逻辑位置算的实现则是 **0 脉冲、不动**。
 *
 *   s_logic_steps = 逻辑位置（步，只在"真正推动轴"的脉冲上累加）
 *   s_pos_steps   = 脉冲域累计（含间隙补偿脉冲），只用来算"还剩多少脉冲要发"
 *   s_bl_left     = 本段开头要**空走**的间隙脉冲数（这段不推进逻辑位置）
 *   s_last_dir    = 上次**实际输出**的方向（间隙补偿按它判"有没有换向"）
 *  ⇒ 与参考工程 Mult_ESP32-S3_arduino_lvgl/motor.cpp 的三件套
 *    （s_pos_units / s_backlash_left / s_out_dir）完全同构。 */
static int32_t s_logic_steps;             /* ★ v18：逻辑位置（步）—— 方向/距离按它算 */
static int32_t s_bl_left;                 /* ★ v18：本段待空走的间隙脉冲数 */
static int32_t s_logic_target;            /* ★ v18：本段逻辑目标（步），到位时用于对齐 */
static bool    s_dir_committed;           /* ★ v18：本段方向是否已"真出过脉冲" */
static int32_t s_pos_steps;               /* 命令域：当前已发脉冲累计（含间隙补偿脉冲） */
static int32_t s_cmd_start;               /* 本次运动起点（命令域） */
static int32_t s_cmd_target;              /* 本次运动终点（命令域，含间隙补偿） */
static float   s_vel_steps;               /* 当前速度（步/秒，无符号） */
static float   s_step_acc;                /* 小数脉冲累加器 */
static float   s_batch_pulses;            /* v12：暂存待提交的脉冲数（攒够一批再交给 RMT） */
static uint32_t s_batch_ticks;            /* v12：本批已攒的曲线拍数（1 拍 = MOTION_PROFILE_DT_S） */
static bool    s_moving;
static int     s_last_dir;                /* 上次**实际输出**的方向：+1 / -1 / 0 */

/* ★ v23：**静止自动脱机**（现场："马达停了之后嗡嗡响"）——
 *   静止够久就抬 ENA（脱机）消掉保持电流的嗡嗡声/发热；下一次下发目标自动重新使能。
 *   取值与前提（负载不能把轴推动！）见 app_motion.h 的 MOTION_IDLE_RELEASE_MS。 */
static int64_t s_idle_since_ms;           /* 静止计时起点（ms；0 = 没在计时） */
static bool    s_ena_released;            /* 已自动脱机（避免重复设引脚/重复打日志） */

static float   s_report_pos;              /* 报告位置（mm） */
static float   s_report_start;            /* 本次运动起始报告位置 */
static float   s_target_pos;              /* 本次运动目标（mm） */

static volatile bool s_alarm_edge;        /* ISR 置位 */
static volatile bool s_alarm_latched;     /* 报警锁存，需人工 ACK */
static volatile float s_report_pos_atomic;/* 供其它任务读取的位置副本 */
static volatile float s_vel_mm_atomic;    /* 供其它任务读取的速度副本 */

static bool   s_arrived_latch;
static bool   s_aborted_latch;
static int64_t s_last_push_ms;
static bool   s_vmax_warned;                /* 速度被上限钳制，只提醒一次，避免刷屏 */
static bool   s_vmin_warned;                /* 速度被**起跳频率**下限抬高，只提醒一次（v7） */
/* ★ v13 诊断：本段**实际发射频率**的范围（min/max）—— 现场"界面运动不转"时，
 *   一眼就能看出"曲线到底跑到了多少 Hz"（是压根没升上去，还是频率对却没转）。 */
static float  s_emit_f_min;
static float  s_emit_f_max;
static int64_t s_move_t0_us;                /* v13：本次运动的起始时刻（用来算"实际耗时"） */
/* ★ v14：**运行期可调**的软启动起步频率（`mtest ss <hz>` 改，0 = 关闭软启动）。
 * 为什么做成运行期变量：现场实测"曲线从低频爬升会在死区里卡壳/左右抖动，然后才转"——
 * 到底从多少 Hz 起跳最合适，只能现场一格一格试；做成可调就不用每试一次重编一次。 */
static float  s_soft_start_hz = MOTION_SOFT_START_HZ;
/* ★★ v16：`mtest run` / `mtest stop` 的**单向连续运行**状态（实现见本文件的 CLI 段）★★
 *   放在这里而不是 CLI 段里，是因为 motion_task 也要看它：
 *   连续运行期间**忽略界面下发的运动** —— 否则两处同时往 RMT 灌脉冲、DIR/ENA 互相踩。
 *   s_run_hz 是运行中的频率，`mtest f <hz>` 可以运行中改（下一批脉冲生效）。 */
static volatile bool  s_run_active;
#if MOTION_CLI_ON
static volatile float s_run_hz = 12000.0f;   /* 只给调试命令行用（`f` / `run`） */
#endif
static uint32_t s_last_emit_n;              /* 上一拍实际产生的 RMT 脉冲数（0 = 没有事务） */
/* 已提交但尚未发完的 RMT 事务数。主循环用 __atomic 递增、RMT 完成中断里递减，
 * 主循环据此把队列填满，让**硬件**连续发事务（见 motion_task 里的说明）。 */
static volatile int s_tx_pending;

/*==============================================================================
 * 「脉冲到底有没有发出去」的核对计数（★ 专为现场"马达不转"加的判据）
 *
 * 现象分两类，必须能从串口一眼分开，否则只能在"供电/接线/驱动器/代码"之间瞎猜：
 *   ① 事务根本没提交      -> s_pulse_req / s_tx_ok / s_tx_fail 能看出（rmt_transmit 失败）
 *   ② 提交了但硬件没发完  -> s_tx_ok > s_tx_done（有事务卡住 = 老 loop_count 的坑）
 *   ③ 都正常但电机不转    -> 三个数都对得上 -> 100% 是接线 / ENA 极性 / 驱动器 / 供电
 * 每次运动（或自检）开始时清零，到位时由 motion_finish() 打印。
 * 只在 motion_task 里读写（s_sym_done 额外由 RMT 完成中断累加，故用 __atomic）。
 *============================================================================*/
static int      s_pulse_req;      /* 本次运动交给 RMT 的脉冲数（各事务 n 之和） */
static uint32_t s_tx_ok;          /* rmt_transmit() 成功入队的事务数 */
static uint32_t s_tx_fail;        /* rmt_transmit() 失败的事务数 */
static volatile uint32_t s_tx_done; /* on_trans_done 回调次数（= 真正发完的事务数，ISR 里 ++） */
static volatile int s_sym_done;   /* on_trans_done 报告的 num_symbols 之和（含结束符） */
static bool   s_tx_clamp_warned;  /* 单事务脉冲数被上限截断，只提醒一次 */
static bool   s_move_no_tx_warned;/* 本次运动"走了位置但一个事务都没发"只报一次 */

#if !MOTION_SIMULATE
static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_rmt_copy_encoder = NULL;
/* ★★ v16：**台架同款**发射（uniform 编码器 + `loop_count = 脉冲数`）★★
 *   现场："主工程设 12000Hz 与台架设 12000Hz 转的不一样"。逐行比对后确认两边
 *   **发射方式不是一个写法**（见 motion_emit_steps 里的 A/B 分支）：
 *     台架：1 个符号 + 硬件重复 n 次（dur = 1e6/f/2，12000Hz 实为 12195Hz）；
 *     本工程原来：n 个「先高后低」符号 + loop_count=0（dur = 1e6/f，12000Hz 实为 12048Hz）。
 *   为了让"同频率 ⇒ 同波形"，把台架那份编码器**原样**搬进本工程
 *   （main/stepper_motor_encoder.c）并默认用它；`mtest mode 0` 可切回原路径做 A/B。 */
static rmt_encoder_handle_t s_rmt_uniform_encoder = NULL;
/** 发射方式（`mtest mode 0|1` 运行期可改）：
 *     0 = **本工程原样（默认）**：`period = 1e6/f`、先高后低、队列预填（已验证能转）；
 *     1 = 台架同款：台架的波形公式（`dur = 1e6/f/2`，先低后高）+ 台架的批次节奏
 *         （64 个/批、等发完再发下一批）。
 *  ★★ 默认必须是 0 ★★：mode 1 是"照着台架改"的那条路，改动都发生在发射时序上，
 *     出问题时现场会立刻"动一下就彻底不动"（见 motion_emit_steps 里 loop_count 的教训）。
 *     先保证开箱能转，台架同款用一条命令切过去做 A/B。 */
static int s_emit_bench = 0;

/* ★★ 脉宽现在是**运行期变量**（默认取 menuconfig 的 MOTION_STEP_PULSE_US）★★
 *   为什么：参数扫描自检要在**同一频率下只改脉宽**做对照实验 —— 用来判定
 *   "3.3V 直驱光耦 + 5µs 窄脉冲"是不是丢脉冲（现象：转一点 + 抖）的根因。
 *   普通运动路径不受影响：它用默认值，自检结束时也会恢复默认值。
 *   单位 µs；**0 = 自动**（周期的一半，即 50% 方波）。 */
static uint32_t s_step_pulse_us = MOTION_STEP_PULSE_US;

/* ★ 每个「在途事务」都要有一份**独立**的符号缓冲 ★
 *
 * 为什么不能用一份共享的 symbol：
 *   rmt_transmit() 是**异步**的 —— 它只把 payload 指针存进事务描述符，
 *   真正的编码发生在其后的 ISR 里（见 esp_driver_rmt/src/rmt_tx.c：
 *   payload 交给 encoder 的时机由驱动安排）。所以「提交下一个事务」时若覆盖
 *   同一块缓冲，前一个还没编码的事务会读到新的 duration —— 编出来是
 *   **半新半旧的符号**（高低电平时间来自两个不同频率），波形畸变，
 *   电机表现为原地抖动而不转。
 *   这里给每个可能的在途事务各留一份，按索引轮转；轮转一圈的时间远长于
 *   编码所需（队列里最多只有 MOTION_TX_QUEUE_TARGET 个在途事务）。
 *   一份缓冲现在能装 MOTION_MAX_STEPS_PER_TX 个符号（= 一个事务里所有脉冲），
 *   因为一个事务不再只有 1 个符号。 */
static rmt_symbol_word_t s_step_symbol[MOTION_RMT_QUEUE_DEPTH][MOTION_MAX_STEPS_PER_TX];
static uint32_t s_step_symbol_idx;
#endif

/*==============================================================================
 * 硬件层
 *============================================================================*/
static void motion_gpio_set(int pin, int level)
{
    if (pin == MOTION_GPIO_STEP || pin == MOTION_GPIO_DIR || pin == MOTION_GPIO_ENA) {
#if !MOTION_SIMULATE
        gpio_set_level((gpio_num_t)pin, level);
#else
        (void)level;
#endif
    }
}

/** 使能/失能步进驱动器（ENA 低有效） */
static void motion_driver_enable(bool enable)
{
#if !MOTION_SIMULATE
    gpio_set_level(MOTION_GPIO_ENA, enable ? MOTION_ENA_ACTIVE_LEVEL : !MOTION_ENA_ACTIVE_LEVEL);
#else
    (void)enable;
#endif
}

/*==============================================================================
 * ★★★ v22：STEP 由 **LEDC 硬件 PWM** 产生（现场要求："你直接用PWM"）★★★
 *
 * 完整背景、代价与资源分配写在 app_motion.h 的 MOTION_STEP_PWM_MODE 里。这里只强调实现要点：
 *   · 一段"恒定频率、定量脉冲"的运动 = **启动 PWM → 按 (脉冲数 ÷ 实际频率) 定时 → 停 PWM**；
 *   · 定时用 `esp_timer` 单次定时器：回调跑在 **esp_timer 任务**上下文（不是 ISR），
 *     因此可以安全调用 `ledc_stop()`；
 *   · 停的是"波形"，收尾（记账/日志/存位置）由 motion_task 看到 `s_pwm_done` 后做
 *     —— 与原来"等 RMT 发完再收尾"一个套路，只是触发源从"事务完成中断"换成"定时器"；
 *   · 频率一律以 `ledc_get_freq()` 的**实际值**为准算时长（小数分频会有千分之几偏差）。
 *============================================================================*/
#if MOTION_STEP_PWM_MODE && !MOTION_SIMULATE
#define MOTION_PWM_MODE     LEDC_LOW_SPEED_MODE
#define MOTION_PWM_TIMER    LEDC_TIMER_2     /* 背光 TIMER_1/CH0、补光灯 TIMER_0/CH1（见 app_light.h）*/
#define MOTION_PWM_CH       LEDC_CHANNEL_2
#define MOTION_PWM_RES      LEDC_TIMER_10_BIT
#define MOTION_PWM_DUTY_50  (1u << 10) / 2u  /* 10bit 的 50%：与"脉宽=0 = 50% 方波"一致 */

static esp_timer_handle_t s_pwm_stop_timer;   /* 到点停 PWM（esp_timer 任务上下文）*/
static volatile bool s_pwm_on;                /* PWM 正在输出（STOP/报警要用它判断）*/
static volatile bool s_pwm_done;              /* 定时到 = 本段脉冲已发完，等 motion_task 收尾 */
static uint32_t      s_pwm_freq_hz;           /* 本段**实际**频率（ledc_get_freq 读回）*/
static uint32_t      s_pwm_pulses;            /* 本段要发的脉冲数（日志用）*/

/** 定时到：先把波形停掉（STEP 停在低电平），再置标志让 motion_task 去收尾 */
static void motion_pwm_stop_timer_cb(void *arg)
{
    (void)arg;
    ledc_stop(MOTION_PWM_MODE, MOTION_PWM_CH, 0);
    s_pwm_on   = false;
    s_pwm_done = true;
}

/** 立即停 PWM（STOP / 报警 / 兜底看门狗都用它；可在任意任务上下文调用） */
static void motion_pwm_abort(void)
{
    if (s_pwm_on) {
        ledc_stop(MOTION_PWM_MODE, MOTION_PWM_CH, 0);
        s_pwm_on = false;
    }
    if (s_pwm_stop_timer != NULL) {
        esp_timer_stop(s_pwm_stop_timer);   /* 没在跑会返回 INVALID_STATE，忽略即可 */
    }
    s_pwm_done = false;
}

static esp_err_t motion_pwm_init(void)
{
    const ledc_timer_config_t tcfg = {
        .speed_mode      = MOTION_PWM_MODE,
        .timer_num       = MOTION_PWM_TIMER,
        .duty_resolution = MOTION_PWM_RES,
        .freq_hz         = MOTION_WORK_FREQ_HZ,   /* 先按工作点配好，每次运动再按需改 */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "pwm: ledc timer config failed");

    const ledc_channel_config_t ccfg = {
        .gpio_num   = MOTION_GPIO_STEP,
        .speed_mode = MOTION_PWM_MODE,
        .channel    = MOTION_PWM_CH,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = MOTION_PWM_TIMER,
        .duty       = 0,                  /* 上电先不输出（duty=0 -> 常低） */
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "pwm: ledc channel config failed");
    ESP_RETURN_ON_ERROR(ledc_stop(MOTION_PWM_MODE, MOTION_PWM_CH, 0), TAG, "pwm: ledc_stop failed");

    const esp_timer_create_args_t targs = {
        .callback = &motion_pwm_stop_timer_cb,
        .name     = "step_pwm_stop",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_pwm_stop_timer), TAG, "pwm: timer create failed");

    ESP_LOGW(TAG, "★ STEP(IO%d) 由 **LEDC 硬件 PWM** 产生（TIMER_%d / CH%d，10bit）："
                  "实际 %.0f Hz（请求工作点 %.0f Hz）—— **没有事务/队列/边界，脉冲串中间不可能断**",
             MOTION_GPIO_STEP, (int)MOTION_PWM_TIMER, (int)MOTION_PWM_CH,
             (double)ledc_get_freq(MOTION_PWM_MODE, MOTION_PWM_TIMER), (double)MOTION_WORK_FREQ_HZ);
    return ESP_OK;
}

/** 开始一段"恒定频率 + 定量脉冲"的 PWM 输出（pulses 个脉冲 @ freq_hz） */
static void motion_pwm_start(float freq_hz, uint32_t pulses)
{
    if (pulses == 0u) {
        s_pwm_done = true;      /* 没有脉冲：让 caller 立刻判到位 */
        return;
    }
    uint32_t f = (uint32_t)(freq_hz + 0.5f);
    if (f < 2u) {
        f = 2u;
    }
    if (ledc_set_freq(MOTION_PWM_MODE, MOTION_PWM_TIMER, f) != ESP_OK) {
        ESP_LOGW(TAG, "pwm: 设频率 %uHz 失败，沿用当前频率", (unsigned)f);
    }
    const uint32_t f_real = ledc_get_freq(MOTION_PWM_MODE, MOTION_PWM_TIMER);
    s_pwm_freq_hz = (f_real > 0u) ? f_real : f;
    s_pwm_pulses  = pulses;

    ledc_set_duty(MOTION_PWM_MODE, MOTION_PWM_CH, MOTION_PWM_DUTY_50);
    ledc_update_duty(MOTION_PWM_MODE, MOTION_PWM_CH);

    /* 时长 = 脉冲数 ÷ **实际**频率（64bit µs，避免长行程溢出；+半周期做四舍五入） */
    const uint64_t us = ((uint64_t)pulses * 1000000ull + (uint64_t)s_pwm_freq_hz / 2u)
                        / (uint64_t)s_pwm_freq_hz;
    (void)esp_timer_stop(s_pwm_stop_timer);     /* 清掉上一次的残留（没在跑会返回错误，无所谓）*/
    s_pwm_done = false;
    s_pwm_on   = true;
    const esp_err_t err = esp_timer_start_once(s_pwm_stop_timer, us);
    if (err != ESP_OK) {
        /* 起定时器都失败：**绝不能让它一直跑**（轴会飞出去）——立即停波形并判到位 */
        ESP_LOGE(TAG, "pwm: 启动停止定时器失败(%s)：立即停 PWM（本段不发脉冲）", esp_err_to_name(err));
        motion_pwm_abort();
        s_pwm_done = true;
    }
}
#endif  /* MOTION_STEP_PWM_MODE && !MOTION_SIMULATE */

#if MOTION_GPIO_ALARM >= 0
static void IRAM_ATTR motion_alarm_isr(void *arg)
{
    (void)arg;
    /* 中断里只置标志：真正的判断（电平是否仍然有效）放在任务里做，天然消抖 */
    s_alarm_edge = true;
}
#endif

/* ★ IO11/IO12/IO13 的**输出驱动能力**：
 *   0（默认）= 不主动设置 ⇒ ESP32-S3 复位默认 CAP_2(≈20mA) **与 stepper_motor 台架一致**；
 *   1         = 三个脚都拉到 CAP_3(≈40mA)（本工程 v8~v15 的历史值）。
 *   为什么默认改成 0：现场"转动有一点点卡"，逐行比对两边初始化后，IO11/12/13 上
 *   **唯一的差异就是这个驱动能力** —— 台架从头到尾没碰过它。 */
#ifndef MOTION_GPIO_DRIVE_CAP_MAX
#define MOTION_GPIO_DRIVE_CAP_MAX   (0)
#endif

static esp_err_t motion_gpio_init(void)
{
#if MOTION_GPIO_ALARM >= 0
    /* ALARM 输入：低有效 + 内部上拉（驱动器报警多为开集输出）。
     * 本工程现场没有报警信号源，MOTION_GPIO_ALARM = -1 时这整段都不会编译。
     * 要演练报警：把它改成一个空闲脚（IO17/IO18/IO10），再短接那根线到 GND。
     * ★ 演练绝不要短接 IO13 —— 它现在是 ENA 输出脚。 */
    gpio_config_t alarm_cfg = {
        .pin_bit_mask = 1ULL << MOTION_GPIO_ALARM,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&alarm_cfg), TAG, "alarm gpio config failed");
#endif

#if !MOTION_SIMULATE
    /* ★ 2026-09-22：这里**只配置 DIR / ENA，不再碰 STEP**。
     *
     * 原因：STEP 由 RMT 外设驱动。先 gpio_config(GPIO_MODE_OUTPUT) 再让 RMT 接管，
     * 会让 rmt_new_tx_channel() 里的 esp_gpio_reserve() 报
     *   "GPIO 11 is not usable, maybe conflict with others"
     * （见 esp_driver_rmt/src/rmt_tx.c:337-341）。这条只是警告、暂时不影响输出，
     * 但它是"STEP 脚被别的模块抢走"的经典征兆，会掩盖真正的冲突；官方
     * peripherals/rmt/stepper_motor 例程的做法就是**只配 EN/DIR**，STEP 交给 RMT。
     * 保持这个顺序，以后一旦开机日志里再出现上面那句警告，就说明真有别的模块
     * 动了 IO11 —— 那是一眼可见的判据。 */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << MOTION_GPIO_DIR) | (1ULL << MOTION_GPIO_ENA),
        /* ★★ 用 INPUT_OUTPUT 而不是 OUTPUT —— 这是 2026-09-22 现场日志暴露的坑：
         *   `gpio_config(GPIO_MODE_OUTPUT)` 会**关掉该引脚的输入通路**
         *   （见 esp_driver_gpio/src/gpio.c:369-374：mode 里没有 INPUT 位就调
         *    gpio_input_disable()），于是 `gpio_get_level()` **恒读回 0**。
         *   所以日志里那句
         *       档 1/7：200 Hz …（DIR=IO12 实测=0；ENA=IO13 实测=0，0=使能）
         *   的 "DIR=IO12 实测=0" 是**读回假象**，不代表 DIR 真被拉低 ——
         *   它一度把我误导成"IO12 短路到 GND"。带上 INPUT 之后读回的就是
         *   引脚**真实电平**：这时如果"设 1 却读回 0"，那才真的是被外部拉低
         *   （接线错 / 短路），是可信的判据。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "dir/ena gpio config failed");
    gpio_set_level(MOTION_GPIO_ENA, !MOTION_ENA_ACTIVE_LEVEL);   /* 上电先失能 */
    gpio_set_level(MOTION_GPIO_DIR, MOTION_DIR_POSITIVE_LEVEL);

    /* ★★ 驱动能力：**默认与 stepper_motor 台架逐项一致，即"根本不设"** ★★
     *   （ESP32-S3 复位默认 GPIO_DRIVE_CAP_2 ≈ 20mA）
     *
     *   历史：本工程原来把 IO11/12/13 都拉到 CAP_3(≈40mA)，理由是"3.3V 直驱光耦
     *   本来只跑到约 2mA，输出别太弱"。但现场反馈"转动时有一点点卡（不明显）"，
     *   逐行比对两边初始化后发现 **IO11/IO12/IO13 唯一的差异就是这个驱动能力**：
     *     台架：只 gpio_config(DIR/ENA) + 让 RMT 接管 STEP，**不碰驱动能力**；
     *     本工程：三个脚都设 CAP_3。
     *   驱动拉满在长线/光耦输入上可能造成**过冲/振铃**，反而让驱动器多计或少计脉冲
     *   —— 先按现场要求对齐台架，把这一项从变量里去掉。
     *
     *   ★ 想调回去：把下面的 MOTION_GPIO_DRIVE_CAP_MAX 改成 1（一行，不用改别的）。 */
#if MOTION_GPIO_DRIVE_CAP_MAX
    gpio_set_drive_capability(MOTION_GPIO_DIR, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(MOTION_GPIO_STEP, GPIO_DRIVE_CAP_3);
#endif
    /* ★ IO13(ENA) **保持本工程原值 CAP_3**（现场要求"IO13 改回来"）：
     *   ENA 是"长时间保持使能"的静态电平（不像 STEP 是脉冲边沿），
     *   驱动能力强一点只会有利（光耦 LED 电流更足、抗干扰更好），
     *   也不会像 STEP 那样因为过冲/振铃影响计数 —— 所以它不在"对齐台架"的范围内。 */
    gpio_set_drive_capability(MOTION_GPIO_ENA, GPIO_DRIVE_CAP_3);
    ESP_LOGI(TAG, "GPIO drive: DIR/STEP = %s，ENA(IO13) = CAP_3(≈40mA，本工程原值)",
             MOTION_GPIO_DRIVE_CAP_MAX ? "CAP_3" : "默认 CAP_2(与 stepper_motor 台架一致)");
#endif

#if MOTION_GPIO_ALARM >= 0
    /* 安装 GPIO ISR 服务（重复安装返回 INVALID_STATE，可忽略） */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "gpio_install_isr_service failed");
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(MOTION_GPIO_ALARM, motion_alarm_isr, NULL), TAG,
                        "gpio isr handler add failed");
#else
    /* MOTION_GPIO_ALARM = -1：报警输入整段关掉（现场没有报警信号源，
     * 参考工程也是个做法）。这样也不会再把 ENA 线的电平误当成报警。
     * 注意：此时 HMI 的全屏报警窗不会被引脚触发。 */
    ESP_LOGW(TAG, "ALARM input DISABLED (MOTION_GPIO_ALARM = -1): no alarm source wired");
#endif
    return ESP_OK;
}

/*==============================================================================
 * ★ STEP/DIR/ENA「三线自检」—— 只动 DIR / ENA 两根线，**不发任何脉冲**
 *
 * 为什么单独做这一个（2026-09-22 晚）：
 *   现场已用逻辑分析仪把 STEP(IO11) 的波形确认到"与参考版逐项一致"
 *   （周期/脉宽/占空比都对上），于是怀疑落到另外两根线（DIR=IO12 / ENA=IO13）。
 *   但"运动到底发不发脉冲"和"这两根线到底动没动"是两件事 ——
 *   频率扫描自检验的是前者，一直没有独立工具证明后者。
 *
 * 本自检做的事（全程**不发脉冲，轴不会走**，可放心反复跑）：
 *   ENA：使能 2s → 脱机 2s → 使能 2s → 脱机 2s（低有效，0 = 使能）
 *   DIR：正方向电平 2s → 反方向电平 2s ×2
 *   每段都打印"设值 + 读回值"，读回用的是 INPUT_OUTPUT 的真实电平。
 *
 * 判读（配万用表 / 直接用手，10 秒出结论）：
 *   ① 万用表夹 IO13 与 GND：3.3V ⇄ 0V 交替 = 引脚确实在动；
 *      一直不变 = 被外部拉住（同一条日志的读回校验会一起打 ERROR）。
 *   ② ★★ 手扶轴（判 ENA 的决定性判据）：
 *      打印【使能】的那 2 秒轴应明显变硬（有保持力矩），
 *      打印【脱机】的那 2 秒应能轻松盘动。
 *        · 轴一直很松  -> 驱动器根本没收到使能信号：查 ENA 这一路
 *                        （端子接错 / 驱动器 ENA 是高有效 / 端子语义是【脱机】）
 *        · 轴一硬一松  -> ENA 这一路完全正常，回去查 STEP/DIR
 *   ③ 万用表夹 IO12 与 GND：两段之间应 3.3V ⇄ 0V 翻转 = DIR 在动。
 *
 * 打开方式：menuconfig -> Roll Coater HMI 配置 -> ROLLCOATER_MOTION_WIRE_TEST
 *============================================================================*/
#ifdef CONFIG_ROLLCOATER_MOTION_WIRE_TEST
#define MOTION_WIRE_TEST_ON_BOOT    (1)
#endif
#ifndef MOTION_WIRE_TEST_ON_BOOT
#define MOTION_WIRE_TEST_ON_BOOT    (0)
#endif

#if MOTION_WIRE_TEST_ON_BOOT && !MOTION_SIMULATE
/** 每段保持时间：足够用万用表看清，也足够用手感觉保持力矩 */
#define MOTION_WIRE_TEST_HOLD_MS    (2000)

static void motion_wire_test(void)
{
    ESP_LOGW(TAG, "=== 三线自检开始（不发脉冲，轴不会走）===");
    ESP_LOGW(TAG, "判读：① 万用表量 IO%d(DIR) / IO%d(ENA) 对 GND，应 3.3V 与 0V 交替；"
                  "② 手扶轴：打印【使能】时轴应明显变硬，打印【脱机】时应能轻松盘动；"
                  "③ 前提是驱动器电源地与 ESP32 共地。",
             MOTION_GPIO_DIR, MOTION_GPIO_ENA);

    for (int i = 0; i < 2; i++) {
        motion_driver_enable(true);
        int lv = gpio_get_level(MOTION_GPIO_ENA);
        ESP_LOGW(TAG, "ENA 使能：IO%d 设 %d 读回 %d（%s）——【现在轴应有保持力矩：盘起来明显变硬】",
                 MOTION_GPIO_ENA, MOTION_ENA_ACTIVE_LEVEL, lv,
                 (lv == MOTION_ENA_ACTIVE_LEVEL) ? "OK" : "★★ 读回不符：被外部拉住/接错端子");
        vTaskDelay(pdMS_TO_TICKS(MOTION_WIRE_TEST_HOLD_MS));

        motion_driver_enable(false);
        lv = gpio_get_level(MOTION_GPIO_ENA);
        ESP_LOGW(TAG, "ENA 脱机：IO%d 设 %d 读回 %d（%s）——【现在轴应能自由盘动：没有保持力矩】",
                 MOTION_GPIO_ENA, !MOTION_ENA_ACTIVE_LEVEL, lv,
                 (lv == !MOTION_ENA_ACTIVE_LEVEL) ? "OK" : "★★ 读回不符：被外部拉住/短路到 GND");
        vTaskDelay(pdMS_TO_TICKS(MOTION_WIRE_TEST_HOLD_MS));
    }

    for (int i = 0; i < 2; i++) {
        motion_gpio_set(MOTION_GPIO_DIR, MOTION_DIR_POSITIVE_LEVEL);
        int lv = gpio_get_level(MOTION_GPIO_DIR);
        ESP_LOGW(TAG, "DIR 正方向：IO%d 设 %d 读回 %d（%s）", MOTION_GPIO_DIR,
                 MOTION_DIR_POSITIVE_LEVEL, lv,
                 (lv == MOTION_DIR_POSITIVE_LEVEL) ? "OK" : "★★ 读回不符：被外部拉住");
        vTaskDelay(pdMS_TO_TICKS(MOTION_WIRE_TEST_HOLD_MS));

        motion_gpio_set(MOTION_GPIO_DIR, !MOTION_DIR_POSITIVE_LEVEL);
        lv = gpio_get_level(MOTION_GPIO_DIR);
        ESP_LOGW(TAG, "DIR 反方向：IO%d 设 %d 读回 %d（%s）", MOTION_GPIO_DIR,
                 !MOTION_DIR_POSITIVE_LEVEL, lv,
                 (lv == !MOTION_DIR_POSITIVE_LEVEL) ? "OK" : "★★ 读回不符：被外部拉住");
        vTaskDelay(pdMS_TO_TICKS(MOTION_WIRE_TEST_HOLD_MS));
    }

    /* 收尾回到"上电姿态"：DIR = 正方向电平、ENA = 脱机（与进入 State1 时一致） */
    motion_gpio_set(MOTION_GPIO_DIR, MOTION_DIR_POSITIVE_LEVEL);
    motion_driver_enable(false);
    ESP_LOGW(TAG, "=== 三线自检结束（已回到上电姿态：DIR=%d、ENA=脱机；测完请关掉 WIRE_TEST）===",
             MOTION_DIR_POSITIVE_LEVEL);
}
#endif  /* MOTION_WIRE_TEST_ON_BOOT && !MOTION_SIMULATE */

#if !MOTION_SIMULATE
/**
 * @brief RMT 事务完成回调（ISR 上下文）
 *
 * ★ 它**不承担**事务衔接工作 —— 那是硬件自动完成的（队列里只要有下一个事务，
 *   本事务结束后立刻开始，全程不需要 CPU 介入）。这个回调做两件事：
 *   1) 把「在途事务数」减一，供主循环判断队列还差几个；
 *   2) 把硬件**实际发完**的符号数累加起来（edata->num_symbols）。
 *      ★ 这是"马达不转"最关键的一条判据：num_symbols 里包含驱动自动补的
 *      1 个结束符，所以一个正常事务报告的是 n+1；到位时
 *      "符号总数 == 请求脉冲数 + 完成事务数" 就说明**每一个脉冲都真的出了引脚**，
 *      那问题就只剩接线 / ENA / 驱动器 / 供电，不用再怀疑代码。
 */
static bool IRAM_ATTR motion_rmt_on_trans_done(rmt_channel_handle_t chan,
                                               const rmt_tx_done_event_data_t *edata,
                                               void *user_ctx)
{
    (void)chan;
    (void)user_ctx;
    __atomic_fetch_sub(&s_tx_pending, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&s_tx_done, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&s_sym_done, (int)edata->num_symbols, __ATOMIC_SEQ_CST);
    return false;   /* 没有唤醒更高优先级的任务 */
}

static esp_err_t motion_rmt_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = MOTION_GPIO_STEP,
        .mem_block_symbols = MOTION_RMT_MEM_SYMBOLS,
        .resolution_hz = MOTION_RMT_RESOLUTION_HZ,
        .trans_queue_depth = MOTION_RMT_QUEUE_DEPTH,
        /* ★★★ v21：把 RMT 的中断优先级提到最高档 ★★★
         *
         *  现场实测（2026-09-22）：**IO11 的方波不连续** —— 这就是"电机来回/抖动"的总根源。
         *  机制（IDF 驱动 `esp_driver_rmt/src/rmt_tx.c:601-648` 的 `rmt_tx_mark_eof`）：
         *    每个事务结尾驱动都会写一个 **`duration0 = 0` 的 stop 符号**（"a RMT word whose
         *    duration is zero means a stop pattern"）⇒ **硬件在事务边界停一下**，
         *    再由"发送完成中断"里的 `rmt_tx_do_transaction()` 启动队列里的下一个事务。
         *    ⇒ **事务边界必定留一个"中断延迟"长的空档**。
         *  该中断原来的优先级是最低档（0 = 默认），会被 WiFi(prio 23 的任务/其 ISR)、
         *  RGB 屏 bounce 的 DMA ISR 等拖后；CPU 一忙（LVGL 全屏重绘 768KB/帧）
         *  这个空档就从"几微秒"涨到"几十~几百微秒" ⇒ 示波器上就是波不连续、
         *  电机那边就是失步/来回。**提优先级是把这个空档压到最小的直接手段。**
         *  ⚠️ 万一这一档在这个芯片/IDF 上不允许（驱动会返回 INVALID_ARG），
         *    自动退回默认优先级重试 —— 绝不让它把启动拖挂（见 main.c 的 fatal_hold）。 */
        .intr_priority = MOTION_RMT_INTR_PRIORITY,
    };
    /* ★★★ 创建通道：两级兜底，**绝不让它把启动拖挂** ★★★
     *   ① 先按"4 个内存块(192 符号) + 高中断优先级"申请；
     *   ② 失败（别处也在用 RMT / 这一档优先级不被允许）就先减内存到 64（= 2 块 = 96 符号）
     *      再试 —— payload 190 个符号超过 96 时驱动会自动**分段流式编码**，功能不受影响，
     *      只是事务内部多几次分段中断；
     *   ③ 仍失败就退回默认中断优先级（边界空档可能偏大）。
     *   main.c 里 app_motion_init 失败会 fatal_hold（屏幕看着"不显示"），所以这里必须兜住。 */
    esp_err_t chan_err = rmt_new_tx_channel(&chan_cfg, &s_rmt_chan);
    if (chan_err != ESP_OK) {
        ESP_LOGW(TAG, "★ RMT 通道创建失败(%s)：退回默认内存(64 符号 = 2 块)重试",
                 esp_err_to_name(chan_err));
        chan_cfg.mem_block_symbols = 64;
        chan_err = rmt_new_tx_channel(&chan_cfg, &s_rmt_chan);
    }
    if (chan_err != ESP_OK) {
        ESP_LOGW(TAG, "★ 仍失败(%s)：再退回默认中断优先级重试"
                      "（事务边界空档可能偏大，见 MOTION_RMT_INTR_PRIORITY 的说明）",
                 esp_err_to_name(chan_err));
        chan_cfg.intr_priority = 0;
        chan_err = rmt_new_tx_channel(&chan_cfg, &s_rmt_chan);
    }
    ESP_RETURN_ON_ERROR(chan_err, TAG, "rmt_new_tx_channel failed");

    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy_cfg, &s_rmt_copy_encoder), TAG,
                        "rmt_new_copy_encoder failed");

    /* ★ v16：**台架同款** uniform 编码器（与 stepper_motor 台架用的是同一个源文件，
     *   见 main/stepper_motor_encoder.c）—— 一点不能少：它决定了
     *   `symbol_duration = resolution / freq / 2` 的取整方式与电平顺序，
     *   也就是"同频率下的波形"是否与台架逐位一致。 */
    const stepper_motor_uniform_encoder_config_t uni_cfg = {
        .resolution = MOTION_RMT_RESOLUTION_HZ,
    };
    /* ★★ 故意**非致命** ★★：创建失败只降级（mode 0 = 本工程原样），绝不把
     *   app_motion_init 拖挂 —— 那样 main.c 会 fatal_hold，**整个系统停住、串口也没反应**，
     *   现场连"哪一步失败"都看不到（2026-09-22 现场踩过："Still halted at 'app_motion_init'"）。 */
    esp_err_t uni_err = rmt_new_stepper_motor_uniform_encoder(&uni_cfg, &s_rmt_uniform_encoder);
    if (uni_err != ESP_OK) {
        s_rmt_uniform_encoder = NULL;
        s_emit_bench = 0;
        ESP_LOGW(TAG, "★ 台架同款 uniform 编码器创建失败(%s)：发射方式自动降级为 mode 0"
                      "（本工程原样：符号数组 + loop_count=0），系统继续启动",
                 esp_err_to_name(uni_err));
    }

    /* 注册事务完成回调：主循环靠它维护「在途事务数」，从而把队列一直填满。
     * 队列满时硬件发完一个立刻开始下一个 —— 这是脉冲连续的保证。 */
    const rmt_tx_event_callbacks_t tx_cbs = {
        .on_trans_done = motion_rmt_on_trans_done,
    };
    ESP_RETURN_ON_ERROR(rmt_tx_register_event_callbacks(s_rmt_chan, &tx_cbs, NULL), TAG,
                        "rmt register event callbacks failed");

    ESP_RETURN_ON_ERROR(rmt_enable(s_rmt_chan), TAG, "rmt_enable failed");
    ESP_LOGI(TAG, "RMT STEP channel ready: GPIO%d @%dHz (queue %d, keep %d pending)",
             MOTION_GPIO_STEP, MOTION_RMT_RESOLUTION_HZ,
             MOTION_RMT_QUEUE_DEPTH, MOTION_TX_QUEUE_TARGET);
    ESP_LOGI(TAG, "  发射方式 mode=%d（%s）；台架同款 uniform 编码器 %s"
                  "（只用于对照：事务结构必须留在本工程，理由见 motion_emit_steps 的长注释）",
             s_emit_bench,
             s_emit_bench ? "台架波形(41+41µs@12000Hz)+台架批次节奏(64/批,等发完)"
                          : "本工程原样(83µs,先高后低,队列预填)",
             (s_rmt_uniform_encoder != NULL) ? "已创建" : "未创建");

    /* ★★★ v23：把"现在跑的是哪条发射路径"明确喊出来 ★★★
     *   本函数只在 **`MOTION_STEP_PWM_MODE = 0`（= 当前交付状态）** 时才被编译进来，
     *   所以这条 WARN **不是"出错"**，而是如实告知当前发射方式：
     *   跑的是 **RMT 事务路径** —— 它有"每个事务边界一个空档"的固有代价
     *   （见本函数上面的长注释），已用"提中断优先级 + 事务做大到 N 符号"压到最小
     *   （现场实测运行正常）。要**绝对连续**就把宏**改成 1**（LEDC PWM），那是 A/B 对照。 */
    ESP_LOGW(TAG, "★ 本机 STEP 走的是 **RMT 事务路径**（`MOTION_STEP_PWM_MODE = 0`，**当前交付**）："
                  "每个事务边界会有一个\"中断延迟\"长的空档，已用 中断优先级 %d + %d 符号/事务 "
                  "把它压到最小（现场实测运行正常）。**这不是报错**，只是如实告知发射方式；"
                  "将来若再出现\"来回 / 一顿一顿 / 失步\"，把 app_motion.h 的 MOTION_STEP_PWM_MODE "
                  "改成 1 重编（LEDC 硬件 PWM，无事务边界）即是 A/B 对照实验。",
             (int)MOTION_RMT_INTR_PRIORITY, MOTION_MAX_STEPS_PER_TX);
    return ESP_OK;
}
#endif

/**
 * @brief 以 freq_hz 的频率发射 n 个 STEP 脉冲
 *        （窄高脉冲 + 长低电平，与同现场 Arduino 版同一种波形）
 *
 * ★ 2026-09-22：改成「一个事务里摆 n 个符号，loop_count 恒为 0」。
 *   理由见 MOTION_MAX_STEPS_PER_TX 处的长注释（旧写法用 loop_count 循环 1 个
 *   符号，在"结束符算不算循环边界"上有歧义，可能整条脉冲链静默失效）。
 *   现在符号数 = 脉冲数，可以拿 on_trans_done 的 num_symbols 逐一对账。
 */
/** 本次发射允许的最高脉冲频率（Hz）。
 *  正常运动 = MOTION_STEP_FREQ_MAX_HZ（= 这台机器实测出来的安全上限）；
 *  扫频自检 / `mtest` 会临时抬到 MOTION_SCAN_FREQ_MAX_HZ —— 诊断的目的就是
 *  **越过安全线去找那条线**，它必须能发得比安全上限更高，否则 `mtest 4000`
 *  会被静默钳到 1000、量出来的"上限"是假的。
 *  只在 motion_task 与串口控制台里串行使用（同一时刻只有一个线程在发脉冲），
 *  所以普通变量就够，不需要加锁。 */
static float s_emit_freq_max_hz = MOTION_STEP_FREQ_MAX_HZ;

static void motion_emit_steps(uint32_t n, float freq_hz)
{
#if MOTION_SIMULATE
    /* 模拟模式没有真实 RMT 事务：固定置 0，让 motion_task 走"补最小延时"那条路。
     * 否则新加的无缝节拍会因为找不到阻塞点而空转、吃满 CPU。 */
    s_last_emit_n = 0;
    (void)n;
    (void)freq_hz;
    (void)s_tx_clamp_warned;    /* 模拟模式下用不到，显式引用避免 -Wunused 报错 */
#else
    s_last_emit_n = n;      /* ★ 记录本拍是否产生了 RMT 事务（0 = 没有） */
    if (n == 0) {
        return;
    }
    if (s_rmt_chan == NULL) {
        return;
    }
    /* 容量兜底：一个事务最多 MOTION_MAX_STEPS_PER_TX(64) 个符号。
     * v12 起由**攒批逻辑**按这个上限拆分（见 motion_run_profile），所以正常永远
     * 走不到这里；一旦触到，说明攒批逻辑或曲线累加器出了异常，
     * 必须留下痕迹而不是静默丢脉冲。 */
    if (n > MOTION_MAX_STEPS_PER_TX) {
        if (!s_tx_clamp_warned) {
            s_tx_clamp_warned = true;
            ESP_LOGW(TAG, "一拍要求 %" PRIu32 " 个脉冲 > 单事务上限 %d，已截断",
                     n, MOTION_MAX_STEPS_PER_TX);
        }
        n = MOTION_MAX_STEPS_PER_TX;
    }
    float f = freq_hz;
    if (f < MOTION_STEP_FREQ_MIN_HZ) {
        f = MOTION_STEP_FREQ_MIN_HZ;
    }
    if (f > s_emit_freq_max_hz) {
        f = s_emit_freq_max_hz;     /* 正常运动 = MOTION_STEP_FREQ_MAX_HZ（机器实测上限）；
                                     * 扫频自检 / mtest 期间 = MOTION_SCAN_FREQ_MAX_HZ */
    }
    /* ★ v13 诊断：记下本段**实际发射**的频率范围（clamp 之后的真值） */
    if (f < s_emit_f_min) {
        s_emit_f_min = f;
    }
    if (f > s_emit_f_max) {
        s_emit_f_max = f;
    }

    /* ★★★ v16：发射方式二选一（`mtest mode`）★★★
     *   mode 1（默认）= **台架同款波形 + 台架批次节奏**（`dur = 1e6/f/2`，先低后高，
     *       12000Hz 实际 12195Hz；节奏 64 个/批、等发完再发下一批）。
     *   mode 0 = 本工程原样（`period = 1e6/f`，先高后低，12000Hz 实际 12048Hz）。
     *   ★ 两种 mode 的**事务结构相同**（n 个符号 + `loop_count = 0`），因为
     *     `loop_count != 0` 会让驱动关掉"发送完成"中断（详见下面 mode 1 的说明）。 */
    /* 取一份**本次事务专用**的符号缓冲（见 s_step_symbol 声明处的说明：
     * rmt_transmit 是异步编码，共用一块缓冲会让先提交的事务编出半新半旧的符号）。
     * ★ 两种 mode 都用它。 */
    rmt_symbol_word_t *sym = s_step_symbol[s_step_symbol_idx];
    s_step_symbol_idx = (s_step_symbol_idx + 1) % MOTION_RMT_QUEUE_DEPTH;

    esp_err_t err;
    if (s_emit_bench) {
        /* ★★★ mode 1 = 台架同款的**波形**（照 stepper_motor_encoder.c 的 uniform 编码器逐行算）★★★
         *       symbol_duration = resolution / freq / 2   ← 整数除法
         *       level0 = 0（先低）→ level1 = 1（后高）
         *   12000Hz：1000000/12000 = 83，83/2 = **41** → 41+41 = 82µs = **12195Hz**
         *   （mode 0 本工程原样：period=83、高 41 低 42 = 83µs = 12048Hz）
         *
         * ★★★ 但**绝不能**像台架那样用 `loop_count` 发这个符号 ★★★
         *   —— 这正是上一步把电机搞停的原因。IDF 驱动 `esp_driver_rmt/src/rmt_tx.c:733`：
         *       rmt_ll_enable_interrupt(..., RMT_LL_EVENT_TX_DONE(channel_id), t->loop_count == 0);
         *   **loop_count != 0 时"发送完成"中断被关掉** ⇒ on_trans_done 回调不来 ⇒
         *   本工程的 s_tx_pending 只增不减 ⇒ 队列一满就**再也发不出脉冲**
         *   （现象："刚动一点就彻底不动"，而且日志里一条报错都没有）。
         *   台架不受影响，是因为它用**阻塞式** `rmt_tx_wait_all_done()` 判断完成，不依赖回调。
         *   ⇒ 结论：**波形 + 批次节奏照台架，事务结构必须留在本工程**
         *     （n 个符号 + `loop_count = 0`）—— 引脚上出来的脉冲串与台架逐位相同，
         *     而记账/完成回调仍然成立。这条坑本工程原来就记录过（见 MOTION_MAX_STEPS_PER_TX）。 */
        const uint32_t dur = (uint32_t)((uint32_t)MOTION_RMT_RESOLUTION_HZ
                                        / (uint32_t)(f + 0.5f) / 2u);
        const rmt_symbol_word_t pulse = {
            .level0    = 0,                     /* 先低（台架的顺序） */
            .duration0 = (dur > 0) ? dur : 1,
            .level1    = 1,                     /* 后高：上升沿在这里，一个符号 = 一个脉冲 */
            .duration1 = (dur > 0) ? dur : 1,
        };
        for (uint32_t i = 0; i < n; i++) {
            sym[i] = pulse;
        }
        rmt_transmit_config_t tx_cfg = {
            .loop_count = 0,                    /* ★ 必须为 0（见上面的中断说明） */
        };
        err = rmt_transmit(s_rmt_chan, s_rmt_copy_encoder, sym,
                           n * sizeof(rmt_symbol_word_t), &tx_cfg);
    } else {
    /* 一个 STEP 周期占用的 RMT 计数（分辨率 1MHz -> 1 计数 = 1µs）。
     * RMT 的 duration 是 15bit，上限 32767 个计数。 */
    uint32_t period = (uint32_t)(MOTION_RMT_RESOLUTION_HZ / f);
    if (period < 2) {
        period = 2;
    }
    if (period > 32767) {
        period = 32767;
    }

    /* ★ 波形 = 「窄高脉冲 + 长低电平」，**不是 50% 占空比** ★
     *
     * 依据是同现场 Arduino 版（motor.cpp 的 pulse_step + 补足间隔）：
     *     digitalWrite(HIGH) -> delayMicroseconds(MOTOR_STEP_PULSE_US) -> LOW
     *     -> delayMicroseconds(周期 - 脉宽)
     * 即 20mm/s(=16kHz、周期 62.5µs) 时是 **5µs 高 / 57.5µs 低（占空比 8%）**
     * —— 标准 STEP/DIR 的波形，宽的低电平让光耦隔离输入的驱动器有充分
     * 时间复位。50% 占空比理论上升沿也能被计数，但没有任何依据；
     * 保持"窄脉冲"才是稳妥做法（脉宽宏见 MOTION_STEP_PULSE_US 的说明）。
     *
     * ★ 顺序用「先高后低」：RMT 会让 GPIO 停在最后一个 duration 的电平上，
     *   这样事务结束后 STEP 停在**低**电平（空闲 = 低），符合 STEP 约定。 */
    uint32_t high = s_step_pulse_us;    /* 运行期可改（自检用），默认 = Kconfig 值 */
    if (high == 0) {
        high = period / 2;      /* 0 = 自动：50% 方波（最初那版波形） */
    }
    if (high < 1) {
        high = 1;
    }
    /* ★ 2026-09-22 晚：高电平**最多占半个周期** —— 低电平同样要给光耦留复位时间。
     *   默认 5µs 在任何可用频率下都触不到这一条（20kHz 时周期还有 50 个计数），
     *   所以**默认输出与旧固件逐位相同**；只有把脉宽调到 > 周期/2 时才生效
     *   （例：16kHz 周期 62µs 时填 50µs，会被收到 31µs，回到 50% 方波）。 */
    if (high > period / 2) {
        high = period / 2;
    }
    const uint32_t low = period - high;

    /* ★ 一个事务里摆满 n 个「先高后低」的脉冲符号（不再靠 loop_count 循环）。
     *   每个符号 = 一次上升沿 + 一次下降沿，符号个数就是脉冲个数。 */
    const rmt_symbol_word_t pulse = {
        .level0    = 1,         /* 先高：上升沿被驱动器计一个脉冲 */
        .duration0 = high,
        .level1    = 0,         /* 再低：留足低电平让光耦复位 */
        .duration1 = low,
    };
    for (uint32_t i = 0; i < n; i++) {
        sym[i] = pulse;
    }

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,        /* ★ 恒为 0：不循环，符号数 = 脉冲数 */
    };
    err = rmt_transmit(s_rmt_chan, s_rmt_copy_encoder, sym,
                       n * sizeof(rmt_symbol_word_t), &tx_cfg);
    }   /* ← 结束"mode 0 = 本工程原样"分支（见上面 v16 的 A/B 说明） */
    if (err == ESP_OK) {
        /* ★ 只有真正入队成功才记账；完成中断会把它减回去（一一对应）。 */
        __atomic_fetch_add(&s_tx_pending, 1, __ATOMIC_SEQ_CST);
        s_tx_ok++;
        s_pulse_req += (int)n;
    } else {
        s_tx_fail++;
        /* ★ 没入队 = 本拍没有产生事务：把 s_last_emit_n 归零，
         *   否则 motion_task 的填充循环会误以为"还在出脉冲"而空转。 */
        s_last_emit_n = 0;
        /* 前 3 次逐条报，之后每 200 次报一条：既不刷屏，也绝不静默丢脉冲 */
        if (s_tx_fail <= 3 || (s_tx_fail % 200) == 0) {
            ESP_LOGE(TAG, "rmt_transmit failed: %s (累计失败 %" PRIu32 " 次) -> 脉冲没有交给硬件",
                     esp_err_to_name(err), s_tx_fail);
        }
    }
#endif
}

/**
 * @brief 等待当前脉冲发完
 * @return false 表示等待期间检测到报警/中止，或等待超时
 *
 * ★ 必须带总超时：万一有事务卡在 RMT 里（硬件没发完、完成中断不来），
 *   无限等待会把 motion_task 永久挂在这里 —— 那也是"设备看起来死掉"的一种，
 *   而且不会有任何报错。超时就打 ERROR 并返回 false，让调用方继续走。
 *
 * ★★ 2026-09-22 改法：**不要循环调用 rmt_tx_wait_all_done(chan, 20)**。
 *   那个 API 一旦超时就会自己打一条 `E rmt: rmt_tx_wait_all_done(592):
 *   flush timeout`，而低频档一个事务可以长达 320ms（自检 200Hz 档 = 64 步），
 *   必然超时 → 一次频率扫描刷出十几条**红色 error**。
 *   实测日志里那一串 `flush timeout` 就是这么来的：**看着像故障，其实完全正常**
 *   （同一行下面紧跟的"脉冲核对 OK：请求 200 / 入队 4 / 发完 4"就是证据）。
 *   现在改成：等我们自己的事务计数 s_tx_pending 归零（ISR 每发完一个事务减一），
 *   最后再用**非阻塞**的 wait_all_done(chan, 0) 把事务描述符回收回 READY ——
 *   此时 COMPLETE 队列里一定已经有货，所以不会再触发那条 error。
 *
 *   超时上限 1500ms 的来历：自检最低档一个事务 = 64 步 @200Hz = 320ms，
 *   最多 3 个在途（MOTION_TX_QUEUE_TARGET）→ 最坏约 1s，留 1.5 倍余量。 */
#define MOTION_TX_DRAIN_TIMEOUT_MS  (1500)
static bool motion_wait_tx_done(void)
{
#if !MOTION_SIMULATE
    if (s_rmt_chan == NULL) {
        return true;
    }
    const int64_t deadline = esp_timer_get_time() / 1000 + MOTION_TX_DRAIN_TIMEOUT_MS;
    while (__atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) > 0) {
        if (s_alarm_edge || s_alarm_latched) {
            return false;
        }
        if ((esp_timer_get_time() / 1000) > deadline) {
            ESP_LOGE(TAG, "等待 RMT 发完超时 %d ms（仍有 %d 个事务在途）—— 硬件没发完",
                     MOTION_TX_DRAIN_TIMEOUT_MS,
                     __atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST));
            return false;
        }
        vTaskDelay(1);      /* 让出 CPU；事务的完成中断在另一个上下文里跑 */
    }
    /* 事务都已进 COMPLETE 队列（ISR 先入队、后回调，所以 s_tx_pending==0 时必有货），
     * 这里把描述符回收回 READY。非阻塞：正常立刻成功，也不会再打日志。 */
    (void)rmt_tx_wait_all_done(s_rmt_chan, 0);
#endif
    return true;
}

/*==============================================================================
 * 状态上报
 *============================================================================*/
static void motion_push_report(bool force)
{
    if (s_report_q == NULL) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!force && (now_ms - s_last_push_ms) < CONFIG_ROLLCOATER_UI_PUSH_MS) {
        return;
    }

    motion_report_t rep = {
        .pos = s_report_pos,
        .moving = s_moving,
        .arrived = s_arrived_latch,
        .aborted = s_aborted_latch,
    };
    /* 队列满就等下一次：一次性标志（arrived/aborted）会保留到成功入队为止 */
    if (xQueueSend(s_report_q, &rep, 0) == pdTRUE) {
        s_last_push_ms = now_ms;
        s_arrived_latch = false;
        s_aborted_latch = false;
    }
}

/** 按运动进度把命令域位置映射成报告位置（反向间隙补偿后仍能精确落到目标） */
static void motion_update_report_pos(void)
{
    int32_t span = s_cmd_target - s_cmd_start;
    if (span == 0) {
        s_report_pos = s_target_pos;
    } else {
        float progress = (float)(s_pos_steps - s_cmd_start) / (float)span;
        if (progress < 0.0f) {
            progress = 0.0f;
        }
        if (progress > 1.0f) {
            progress = 1.0f;
        }
        s_report_pos = s_report_start + (s_target_pos - s_report_start) * progress;
    }
    s_report_pos_atomic = s_report_pos;
}

/*==============================================================================
 * 运动流程
 *============================================================================*/
static void motion_finish(bool arrived)
{
    const bool was_moving = s_moving;   /* v13：区分"真跑了一段"与"目标=当前位置"的空收尾 */
    s_moving = false;
    s_vel_steps = 0.0f;
    s_step_acc = 0.0f;
    s_batch_pulses = 0.0f;                  /* v12：批次残量清零（正常到位时已被 flush） */
    s_batch_ticks = 0;
    s_report_pos = s_target_pos;
    s_report_pos_atomic = s_report_pos;
    s_vel_mm_atomic = 0.0f;

    /* ★ 先把预填在 RMT 队列里的事务放完（最多 MOTION_TX_QUEUE_TARGET 个 × 1ms）。
     *   不做这一步的话"入队数 > 发完数"是所有正常运动的固有现象，
     *   下面的核对日志会把每次到位都误报成异常 —— 那等于没有判据。 */
    if (s_tx_pending > 0) {
        motion_wait_tx_done();
    }

    /*==========================================================================
     * ★★ 脉冲核对：现场"马达不转"的第一判据（可照抄进串口记录）
     *
     *   SIMULATE         : 本就不驱动引脚，请求脉冲恒为 0，属正常
     *   核对 OK          : RMT 报告的符号数 == 请求脉冲数 + 完成事务数
     *                      （每个事务的符号数里含驱动自动补的 1 个结束符）
     *                      -> 每一个脉冲都真的交给了硬件，再不动就只能查
     *                         接线 / ENA 极性 / 驱动器 PUL 规格 / 供电
     *   ★★ 核对异常     : 入队数 ≠ 发完数（有事务卡在 RMT 里，旧 loop_count
     *                      方案的典型表现），或符号数对不上（脉冲被吞）
     *========================================================================*/
#if MOTION_STEP_PWM_MODE && !MOTION_SIMULATE
    /* ★★★ v22：本模式下 STEP 由 **LEDC 硬件 PWM** 产生 ⇒ **没有 RMT 事务/符号**，
     *   所以上面那套"入队/发完/符号核对"在这里不适用（硬套会打出假 ERROR）。
     *   这里换成 PWM 的说法：这一行同时把"实际发了多久"和"应该发多少个"对上。 */
    if (was_moving) {
        ESP_LOGI(TAG, "  脉冲由 LEDC 硬件 PWM 产生：%.0f Hz × %.3f s ≈ %u 个脉冲"
                      "（**没有事务边界 ⇒ 脉冲串中间不可能断**）",
                 (double)s_pwm_freq_hz,
                 (float)(esp_timer_get_time() - s_move_t0_us) / 1000000.0f,
                 (unsigned)s_pwm_pulses);
    }
#else
    const int sym_done = __atomic_load_n(&s_sym_done, __ATOMIC_SEQ_CST);
    if (MOTION_SIMULATE) {
        ESP_LOGI(TAG, "位置 %.3f mm（SIMULATE：未驱动引脚，脉冲核对不适用；入队 %u 个事务）",
                 s_report_pos, (unsigned)s_tx_ok);
    } else if (s_tx_ok > 0 && s_tx_ok == s_tx_done && sym_done == s_pulse_req + (int)s_tx_ok) {
        ESP_LOGI(TAG, "脉冲核对 OK：请求 %d 个脉冲 / %u 个事务，RMT 报告 %d 个符号"
                      "（= 请求脉冲 + 每事务 1 个结束符）-> 脉冲已全部出引脚",
                 s_pulse_req, (unsigned)s_tx_ok, sym_done);
    } else {
        ESP_LOGE(TAG, "★★ 脉冲核对异常：请求 %d 个脉冲 / 入队 %u 个事务 / 发完 %u 个 / "
                      "RMT 报告 %d 个符号（期望 %d）",
                 s_pulse_req, (unsigned)s_tx_ok, (unsigned)s_tx_done, sym_done,
                 s_pulse_req + (int)s_tx_ok);
    }

    /* ★ v13：本段**实际发射频率**范围 + 平均每事务脉冲数。
     *   现场"界面运动不转"时看这一行就能定性：
     *     · 只看到 500~2000Hz  → 曲线压根没升上去（软启动/加速有问题）；
     *     · 看到 8500Hz        → 频率对，那问题在驱动器/机械（与 mtest hold 的区别只剩发射节奏）；
     *     · 平均每事务很少（1~3）→ 事务太碎（攒批没生效）。 */
    if (was_moving && s_emit_f_max > 0.0f) {
        /* 期望耗时 = 脉冲数 ÷ **最高（巡航）频率**；实际耗时远大于它 ⇒ 脉冲串中间有**空档**
         * （硬件在等下一批事务），现场表现就是"一顿一顿 / 干脆不转"。
         * 注：慢速起步占的时间很短，所以用最高频率估期望值是合理的。 */
        const float expect_s = (s_emit_f_max > 1.0f) ? ((float)s_pulse_req / s_emit_f_max) : 0.0f;
        const float actual_s = (float)(esp_timer_get_time() - s_move_t0_us) / 1000000.0f;
        ESP_LOGI(TAG, "  本段实际发射频率 %.0f ~ %.0f Hz（%u 个事务，平均 %.1f 个脉冲/事务）；"
                      "脉冲 %d 个：期望耗时 %.2f s / 实际耗时 %.2f s（比值 %.2f，>1.5 说明中间有空档）",
                 s_emit_f_min, s_emit_f_max, (unsigned)s_tx_ok,
                 (s_tx_ok > 0) ? ((float)s_pulse_req / (float)s_tx_ok) : 0.0f,
                 s_pulse_req, expect_s, actual_s,
                 (expect_s > 0.01f) ? (actual_s / expect_s) : 0.0f);
    }
#endif  /* MOTION_STEP_PWM_MODE（PWM 模式专用的到位日志） */

    if (arrived) {
        s_arrived_latch = true;
        /* ★ v18：把逻辑位置对齐到目标（浮点累加会留 ±1 步残差）——
         *   下一段的方向/距离按它算，所以必须精确落在目标上。
         *   `s_last_dir` 已经在"真出过脉冲"时提交过了，这里不再动它。 */
        s_logic_steps = s_logic_target;
#if MOTION_STEP_PWM_MODE && !MOTION_SIMULATE
        /* ★ v22：PWM 模式下脉冲由硬件产生、曲线不再逐拍推进 s_pos_steps（命令域累计），
         *   这里必须把它对齐到本段终点，否则下一段的 s_cmd_start 基准会偏。 */
        s_pos_steps = s_cmd_target;
#endif
        ESP_LOGI(TAG, "Arrived at %.3f mm (%" PRId32 " steps)", s_report_pos,
                 s_cmd_target - s_cmd_start);
        /* 位置落盘走异步队列，不在运动任务里写 flash */
        app_config_save_position_async(s_report_pos);
    } else {
        s_aborted_latch = true;
        ESP_LOGW(TAG, "Motion aborted at %.3f mm", s_report_pos);
    }
    motion_push_report(true);
}

static bool motion_alarm_input_active(void)
{
#if MOTION_GPIO_ALARM >= 0
    return gpio_get_level(MOTION_GPIO_ALARM) == MOTION_ALARM_ACTIVE_LEVEL;
#else
    /* 报警输入已关闭：永远不触发。
     * 这一条同时修掉了现场那个"没接报警却一直报 ALARM"的问题 ——
     * 起因是 ALARM(IO13) 读到了 ENA 线的低电平。 */
    return false;
#endif
}

/** 报警检查：边沿中断 + 电平双重判定 */
static void motion_check_alarm(void)
{
    bool edge = s_alarm_edge;
    bool active = motion_alarm_input_active();
    if (!edge && !active) {
        return;
    }
    s_alarm_edge = false;
    if (active) {
        if (!s_alarm_latched) {
            ESP_LOGE(TAG, "ALARM input active -> latch alarm, stop motion");
        }
        s_alarm_latched = true;
        /* 真机上的急停动作：抬 ENA 切断驱动器使能 */
        motion_driver_enable(false);
    }
}

/** 把「配置里的速度(mm/s)」换算成**实际会用的巡航脉冲频率(Hz)**，上下两侧都钳制。
 *
 *  上：MOTION_STEP_FREQ_MAX_HZ —— RMT/工程上限（超了会失步）；
 *  下：MOTION_TURN_FREQ_MIN_HZ —— ★★ **起跳频率**：低于它这台机器一步都不走，
 *      只会原地嗡嗡响（现场实测：当前拨码 1600 脉冲/圈时 **8500Hz 才转**）。
 *
 *  ★ 为什么必须"上下都钳"，而不是只钳上限：
 *      "Speed 设小了"在这台机器上**不是"慢一点"的结果，而是"按了没反应"** ——
 *      轴一步都不动，界面读数却照走（位置是软件记账），最难排查。
 *      所以低于起跳频率时主动抬到下限，并把原因打到日志（只提醒一次，避免刷屏）。
 *
 *  @param speed_mm_s   配置里的速度
 *  @param clamped_low  出参：是否被"起跳频率下限"抬过（可为 NULL）
 *  @param clamped_high 出参：是否被"脉冲上限"限过（可为 NULL）
 *  @return 实际使用的巡航频率（Hz）
 */
static float motion_cruise_freq_hz(float speed_mm_s, bool *clamped_low, bool *clamped_high)
{
    float f = speed_mm_s * MOTION_STEPS_PER_UNIT;
    if (clamped_high) { *clamped_high = false; }
    if (clamped_low)  { *clamped_low  = false; }

    if (f > MOTION_STEP_FREQ_MAX_HZ) {
        if (!s_vmax_warned) {
            s_vmax_warned = true;
            ESP_LOGW(TAG, "Speed %.3f mm/s (= %.0f steps/s) 超过脉冲上限 %.0f steps/s，已限制为 %.2f mm/s",
                     speed_mm_s, f, MOTION_STEP_FREQ_MAX_HZ,
                     MOTION_STEP_FREQ_MAX_HZ / MOTION_STEPS_PER_UNIT);
        }
        f = MOTION_STEP_FREQ_MAX_HZ;
        if (clamped_high) { *clamped_high = true; }
    }

    if (f < MOTION_TURN_FREQ_MIN_HZ) {
        if (!s_vmin_warned) {
            s_vmin_warned = true;
            ESP_LOGW(TAG, "★★ Speed %.3f mm/s (= %.0f steps/s) 低于本机**起跳频率** %.0f Hz"
                          "（现场实测：%.0f Hz 以下一步都不走、只嗡嗡响）：已抬到 %.0f steps/s = %.2f mm/s。"
                          " 想改下限见 app_motion.h 的 MOTION_TURN_FREQ_MIN_HZ。",
                     speed_mm_s, f, MOTION_TURN_FREQ_MIN_HZ, MOTION_TURN_FREQ_MIN_HZ,
                     MOTION_TURN_FREQ_MIN_HZ, MOTION_TURN_FREQ_MIN_HZ / MOTION_STEPS_PER_UNIT);
        }
        f = MOTION_TURN_FREQ_MIN_HZ;
        if (clamped_low) { *clamped_low = true; }
    }
    return f;
}

/** 开始一次运动 */
static void motion_begin(float target_mm)
{
    const app_config_t cfg = app_config_get();

    /* ★ v23：既然要下发运动 ⇒ 静止计时清零、脱机标志复位
     *   （下面"使能"那一步会重新把 ENA 拉低 —— 所以自动脱机过的轴不用手动复位）*/
    s_idle_since_ms = 0;
    s_ena_released  = false;

    int32_t want = (int32_t)lroundf(target_mm * MOTION_STEPS_PER_UNIT);

    /* ★★★ v18：方向 / 距离一律按**逻辑位置 s_logic_steps** 算（不再用脉冲域 s_pos_steps）★★★
     *   `s_pos_steps` 含反向间隙的补偿脉冲，拿它算 delta 会让"再下发一次同一个目标"
     *   变成 ±160 步的**反向**短走 ⇒ 现场就是"正转后反转 / 一会正转一会反转"。
     *   （独立仿真复现：目标 40→0→40 之后再连下发三次 40，旧实现每次反向走 320 步、
     *    而读数一直显示 40.000 不变。）详见上面 s_logic_steps 的说明。 */
    int32_t delta = want - s_logic_steps;
    int32_t extra = 0;
    const int32_t dir = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
    /* 本次运动 DIR 的**期望电平**。放在这里（而不是 if 块里）是因为读回校验
     * 挪到了窗口下方"上脉冲之前的稳态"处，那里要用它比较（见那段说明）。 */
    int dir_level = gpio_get_level(MOTION_GPIO_DIR);

    if (dir != 0) {

        /* ★★★ v14：**改 DIR 之前，必须先把 RMT 队列里上一段残余的脉冲发完** ★★★
         *
         *  为什么非要有这一步（现场口径："转的时候方向老是乱跳"）：
         *    `app_motion_stop()`（界面 STOP 键 / 返回键 / Admin 退出）只把 s_moving
         *    置 false，**不会清 RMT 队列** —— 此刻最多还有 MOTION_TX_QUEUE_TARGET(3)
         *    个事务 ≈ 12ms 的脉冲在路上（12000Hz 下约 144 个脉冲）。
         *    如果这时直接改 DIR 再发新方向的脉冲，队列里那些**旧方向的脉冲会带着
         *    新 DIR 电平出来** ⇒ 轴先"反着跳"一下；连续几次就是"方向乱跳"。
         *
         *  同一条保险也覆盖"运动中又下发新目标"（队列是覆盖式，latest wins）——
         *  那时同样会改 DIR。
         *
         *  `motion_finish()` 正常到位时本来就会等队列（见那里的说明），
         *  这里只是给"STOP 后紧接着又下发 / 运动中改目标"补上同一个保险。
         *  代价：每次下发运动最多多等约 12ms（操作上感觉不到）。
         *  跑在 motion_task 上下文里，可以安全阻塞（与 motion_abort 同一套等待）。 */
        if (s_tx_pending > 0) {
            motion_wait_tx_done();
        }

        /* ★ v18：换向才补间隙，而且这次补偿脉冲在**本段开头空走**（逻辑位置不动）。
         *   旧写法把它折进目标、放在**末尾** ⇒ 脉冲域位置被推离逻辑位置 ±160 步，
         *   那正是"再下发同一个目标就反着跳"的根源。 */
        if (s_last_dir != 0 && dir != s_last_dir) {
            extra = (int32_t)lroundf(cfg.backlash * MOTION_STEPS_PER_UNIT);
            if (extra < 0) {
                extra = 0;
            }
            if (extra > 0) {
                ESP_LOGI(TAG, "Direction reversed, pre-run backlash %" PRId32
                              " steps (logic position unchanged)", extra);
            }
        }
        dir_level = (dir > 0) ? MOTION_DIR_POSITIVE_LEVEL : !MOTION_DIR_POSITIVE_LEVEL;
        motion_gpio_set(MOTION_GPIO_DIR, dir_level);
        /* 与 Arduino 版一致（motor.cpp 换向后 delayMicroseconds(MOTOR_DIR_SETUP_US)）：
         * DIR 先建立，再拉 ENA，再等稳定时间，最后才发脉冲。 */
        esp_rom_delay_us(MOTION_DIR_SETUP_US);
    }

    /* ★ v18：间隙补偿脉冲直接算进本段的**脉冲总数**（开头 s_bl_left 个只走间隙），
     *   逻辑目标单独记 —— 两者分开之后，"再下发同一个目标"必然是 0 脉冲、不动。 */
    s_dir_committed = false;
    s_bl_left       = extra;
    s_logic_target  = want;
    s_cmd_start     = s_pos_steps;
    s_cmd_target    = s_pos_steps + dir * ((delta >= 0 ? delta : -delta) + extra);
    s_report_start  = s_report_pos;
    s_target_pos    = target_mm;
    s_vel_steps = 0.0f;
    s_step_acc = 0.0f;
    s_batch_pulses = 0.0f;                  /* v12：本次运动的脉冲批次清零 */
    s_batch_ticks = 0;
    s_moving = (s_cmd_target != s_cmd_start);

    if (s_moving) {
        /* 本次运动的脉冲核对计数清零（到位时由 motion_finish 打印） */
        s_pulse_req = 0;
        s_tx_ok = 0;
        s_tx_fail = 0;
        s_tx_done = 0;
        __atomic_store_n(&s_sym_done, 0, __ATOMIC_SEQ_CST);
        s_move_no_tx_warned = false;
        s_emit_f_min = 1.0e9f;              /* v13：本段发射频率范围统计 */
        s_emit_f_max = 0.0f;
        s_move_t0_us = esp_timer_get_time();

        /* 使能：ENA 低有效，拉低（脱机/使能由 State1 的 set_enabled(false) 负责） */
        motion_driver_enable(true);

        /* ★★★ 2026-09-22：DIR / ENA 之后必须等**足够长的稳定时间**才发第一个脉冲 ★★★
         *
         * 依据：本工程里**唯一被现场证明"电机会转"的路径**是频率扫描自检，
         * 而它的顺序是「设 DIR -> ENA 拉低 -> vTaskDelay(20ms) -> 才发脉冲」
         * （见 motion_self_test 里的 20ms）。运动路径原来只等 10µs 就开脉冲，
         * 这是同一个驱动器上 DIR/ENA 这两根线**唯一的行为差别**：
         *   TB6600 这类光耦隔离输入的驱动器，ENA 由高变低后光耦导通过程 +
         *   驱动器内部输出级使能 + DIR 建立都需要时间（实测量级是 ms），
         *   10µs 之内发出去的脉冲驱动器根本还没"醒"，一个都不会被计。
         * 20ms 只在每次下发运动时等一次，操作上完全感觉不到；
         * 换来的是运动路径与"能转的"自检路径在 DIR/ENA 上完全一致 —— 少一个变量。
         * （DIR_SETUP_US 的 10µs 仍然保留在上面，那是"换向到 ENA 拉低"之间的小间隔。） */
        vTaskDelay(pdMS_TO_TICKS(MOTION_ENA_SETTLE_MS));

        /* 把**实际会发出的巡航脉冲频率**打出来（已按 MOTION_STEP_FREQ_MAX_HZ 上限
         * 与 MOTION_TURN_FREQ_MIN_HZ **起跳频率下限**双向钳制）。
         * 排查"失步"与"一步都不动"时这一行最关键：
         *   · 频率贴着上限    -> 往供电电压/扭矩方向查（超速会丢步）；
         *   · 显示 ★已抬到起跳频率 -> 你设的 Speed 落在低频禁区，软件替你抬了；
         *   · 显示 ★已按上限限制    -> 你设的 Speed 太高，被钳到 25mm/s。
         * ★ 2026-09-22 起把 DIR / ENA 的实际引脚电平也打进来 —— "马达不转"时
         *   先看这一行就能确认"使能到底有没有拉低、方向到底是不是高"。 */
        bool v_clamped_low = false, v_clamped_high = false;
        const float v_cap = motion_cruise_freq_hz(cfg.speed, &v_clamped_low, &v_clamped_high);

        /* ★ v14：**"界面里跑的频率不是 12000" 这件事必须在日志里喊出来** ★★
         *   现场最容易踩的坑：在 Admin 里把 Speed 填大（例如 40000 ⇒ 40.000mm/s），
         *   它会被钳到上限 25mm/s = **20000Hz**（12.5 转/秒）—— 电机在那一档根本跟不上，
         *   于**一次行程里**就会失步打滑 = "一会正转一会反转 / 中途倒一下"，
         *   而且听起来完全像"软件在乱换向"，极易查错方向。
         *   实测能连续平稳转的工作点是 12000Hz（= 15.000mm/s，串口 `run` 验证）。
         *   注意这里**只 WARN 不钳**：再往上（14000/16000）是允许现场试的，
         *   硬上限仍是 MOTION_STEP_FREQ_MAX_HZ。 */
        if (v_cap > MOTION_WORK_FREQ_HZ) {
            ESP_LOGW(TAG, "★★ 当前 Speed %.3f mm/s = **%.0f Hz**，高于已验证工作点 %.0f Hz"
                          "（= %.3f mm/s）%s。若出现【一次行程里一会正转一会反转 / 走不到目标】"
                          " 就是超速失步 —— 把 Admin 的 Speed 降回 %.3f（或直接烧 fac_ver=13 的固件，"
                          "上电会自动刷回）。往上探请先 `f 14000` / `f 16000` 逐档试。",
                     cfg.speed, v_cap, MOTION_WORK_FREQ_HZ,
                     MOTION_WORK_FREQ_HZ / MOTION_STEPS_PER_UNIT,
                     v_clamped_high ? "（已被上限钳过）" : "",
                     MOTION_WORK_FREQ_HZ / MOTION_STEPS_PER_UNIT);
        }
        ESP_LOGI(TAG, "Move: %.3f -> %.3f mm (%" PRId32 " steps, backlash %" PRId32
                      ", %" PRId32 " steps/s = %.1f mm/s)%s",
                 s_report_start, target_mm, s_cmd_target - s_cmd_start, extra,
                 (int32_t)v_cap, v_cap / MOTION_STEPS_PER_UNIT,
                 v_clamped_low  ? " ★已抬到起跳频率（设的速度在低频禁区）"
                                : (v_clamped_high ? " ★已按脉冲上限限制" : ""));

        /* ★ v17：本段的频率**不再有"收尾减速"** —— 起步 / 巡航 / 收尾 **同一个数**。
         *   这一行是"界面到底跑在什么频率"的**唯一凭据**，现场先看它再看电机：
         *   三个数必须相同；只要"收尾"比"巡航"小，就说明还在走老的减速曲线。 */
        const bool  ss_on    = (s_soft_start_hz >= MOTION_STEP_FREQ_MIN_HZ &&
                                s_soft_start_hz < MOTION_TURN_FREQ_MIN_HZ);
        const float f_start  = ss_on ? s_soft_start_hz : v_cap;
        ESP_LOGI(TAG, "  曲线：起步 %.0f Hz → 巡航 %.0f Hz → 收尾 %.0f Hz —— **恒定频率**%s",
                 f_start, v_cap, v_cap,
                 ss_on ? "（含软启动爬升段：按 accel 爬到巡航后就恒速；到位即停）"
                       : "（无爬升、无减速段：与串口 `run` 同构，第一拍就是它、最后一拍还是它）");

        /* ★ v17：**"刹车距离"这个概念已经取消**（原来 v15 在这里打的那条 WARN 已删）。
         *   原由：起步即巡航 ⇒ 要在末端从巡航刹回起跳频率、行程太短会滑步；
         *   现在整段恒速、到位即停（脉冲一停转子就停），与 `run` + `stop` 一样，
         *   不再需要这段距离。留这句说明是因为历史文档里"刹车距离 0.37mm"出现过很多次。 */
#if !MOTION_SIMULATE
        /* ★ v22：DIR 把**设值 + 读回**都打出来 —— 现场"正反两个方向动作一样"时，
         *   这一行就能自证"软件到底发的是哪个方向"：
         *     正转：设 1/读回 1；反转：设 0/读回 0。
         *   两者都变 ⇒ 软件在换向，问题在 DIR 那条线/驱动器；
         *   设了却读回不同 ⇒ 引脚被外部拉住（下面紧跟着一条 ERROR）；
         *   两条命令这一行完全一样 ⇒ 才是我这边的问题。 */
        ESP_LOGI(TAG, "  STEP=IO%d DIR=IO%d(设 %d / 读回 %d) ENA=IO%d(%d, 0=使能) 脉冲上限 %d 步/事务",
                 MOTION_GPIO_STEP, MOTION_GPIO_DIR, dir_level, gpio_get_level(MOTION_GPIO_DIR),
                 MOTION_GPIO_ENA, gpio_get_level(MOTION_GPIO_ENA),
                 MOTION_MAX_STEPS_PER_TX);

        /* ★★ DIR / ENA 的读回校验统一放在这里（= 脉冲发出之前的**稳态**）★★
         *
         * 为什么挪到这里：两个脚都是 INPUT_OUTPUT，读回的是引脚真实电平
         *   （旧代码用 OUTPUT 模式时 gpio_get_level() 恒返回 0，见 motion_gpio_init
         *    的说明）。而"设了没生效"只有在**稳定之后**读才可信 —— 上面等了
         *    MOTION_ENA_SETTLE_MS(20ms)，此刻的电平就是驱动器将要看到的电平。
         * 这两条 ERROR 是"电机一步都不动"最直接的分层判据：
         *   DIR 读回不符 -> 方向信号没到驱动器（这次运动方向会反或不动）；
         *   ENA 读回不符 -> 驱动器根本没用上使能，电机既不会有保持力矩也不会转。 */
        const int dir_now = gpio_get_level(MOTION_GPIO_DIR);
        if (dir_now != dir_level) {
            ESP_LOGE(TAG, "★★ DIR(IO%d) 设成 %d 但读回 %d：该引脚被外部拉住"
                          "（短路到 GND / 接错端子 / 驱动器 DIR 输入异常）",
                     MOTION_GPIO_DIR, dir_level, dir_now);
        }
        const int ena_now = gpio_get_level(MOTION_GPIO_ENA);
        if (ena_now != MOTION_ENA_ACTIVE_LEVEL) {
            ESP_LOGE(TAG, "★★ ENA(IO%d) 设成 %d(=使能) 但读回 %d：驱动器没收到使能信号 ——"
                          " 它既不会有保持力矩、也不会转发脉冲。① 查 ENA 线是否接在驱动器的 ENA/EN 端子；"
                          " ② 若该驱动器是高电平使能、或 ENA 端子是【脱机/关输出】语义，"
                          "把 app_motion.h 的 MOTION_ENA_ACTIVE_LEVEL 改成 1 再烧一次；"
                          " ③ 也可先拔掉 ENA 那根线试 —— 多数驱动器不接 ENA 就是一直使能。",
                     MOTION_GPIO_ENA, MOTION_ENA_ACTIVE_LEVEL, ena_now);
        }
#endif

#if MOTION_STEP_PWM_MODE && !MOTION_SIMULATE
        /* ★★★ v22：这一段用**硬件 PWM** 发（现场要求："你直接用PWM"）★★★
         *   本工程是"整段恒定频率"（v17 起），所以一段运动 = 频率恒定 + 脉冲数已知：
         *   直接启动 LEDC、按 (脉冲数 ÷ 实际频率) 定时停止即可 ——
         *   **中间所有脉冲都由硬件产生，没有事务/队列/边界/中断参与。** */
        {
            const int32_t dsteps = s_cmd_target - s_cmd_start;
            /* ★★★ 必须取**绝对值**（2026-09-22 现场实测的坑）★★★
             *   反向运动时 `s_cmd_target < s_cmd_start` ⇒ 差值是负数（例 -31999），
             *   直接转 uint32_t 会变成 **42 亿**个脉冲 ⇒ 定时时长 ≈ 99 小时
             *   ⇒ 现场现象就是"**反方向那一边一直转不停**"（日志里会打出
             *   `× 4294935297 个脉冲 ≈ 357970.937 s`）。RMT 那条路没这个问题，
             *   因为它是用 `s_cmd_target` 与 `s_pos_steps` 的差值判方向、逐拍推进的。 */
            const uint32_t pulses = (uint32_t)((dsteps >= 0) ? dsteps : -dsteps);
            s_move_t0_us = esp_timer_get_time();   /* 只统计 PWM 真正输出的那段时长（日志可比） */
            motion_pwm_start(v_cap, pulses);
            /* ★ PWM 模式没有曲线逐拍推进，所以"本次实际方向"要在这里提交 ——
             *   否则 `s_last_dir` 永远为 0 ⇒ **反向间隙补偿（backlash）永远不生效**
             *   （日志里 `backlash 0` 就是这个原因）。 */
            s_last_dir = dir;
            s_dir_committed = true;
            s_emit_f_min = s_emit_f_max = (float)s_pwm_freq_hz;   /* 供 motion_finish 的日志 */
            ESP_LOGW(TAG, "  STEP：LEDC 硬件 PWM **%.0f Hz**（实际）× %u 个脉冲 ≈ %.3f s "
                          "—— 整段由硬件产生：**中间没有事务边界、不可能断**",
                     (double)s_pwm_freq_hz, (unsigned)pulses,
                     (double)pulses / (double)((s_pwm_freq_hz > 0u) ? s_pwm_freq_hz : 1u));
        }
#endif
    } else {
        /* ★★★ v19：**目标 = 当前位置 ⇒ 差 0 步、一个脉冲都不发** ★★★
         *  这是现场"输入了目标却没反应"的**唯一**原因（本工程是**绝对定位**：
         *  走的是"到那个位置去"，不是"走这一段距离"），必须留一条日志：
         *  否则会被误判成"死机 / 命令丢了 / 方向没设对"，白查一圈。
         *  界面同时会闪一条 `Arrived at x.xxx`（app_motion_poll 的 arrived 标志）。 */
        ESP_LOGW(TAG, "目标 %.3f 就是当前位置（差 0 步）：**不发任何脉冲** —— "
                      "绝对定位下这是正常的。要真走一段，就把目标设成与当前位置不同的值"
                      "（或在 State1 重新标定一个不同的当前读数）。",
                 target_mm);
        motion_update_report_pos();
        motion_finish(true);
    }
}

/** 曲线积分 + 发脉冲（每拍调一次） */
static void motion_run_profile(float dt)
{
    if (!s_moving) {
        /* 已经停下来了（例如上一拍刚好走到位）。这里必须显式清标志：
         * motion_task 的队列填充循环靠 s_last_emit_n 判断"还有没有脉冲可发"，
         * 留着上一拍的旧值会让它一直往里灌空拍。 */
        s_last_emit_n = 0;
        return;
    }

    const app_config_t cfg = app_config_get();

    /* 配置换算到「步/秒」域。
     * ★ v17：本段取消收尾减速（整段恒速）⇒ **不再需要 decel**，
     *   只留加速率 a_acc（而且只在"打开了软启动"时才会真正起作用）。 */
    float a_acc = cfg.accel * MOTION_STEPS_PER_UNIT;
    float v_max;    /* 巡航频率（Hz）：由 motion_cruise_freq_hz() 双向钳制后给出 */
    if (a_acc <= 0.0f) {
        a_acc = MOTION_STEPS_PER_UNIT;
    }

    /* ★★★ 必须在这里就把 v_max 钳到 RMT 真正能发出的频带之内 ★★★
     *   否则 motion_emit_steps() 内部会再钳一次，而下面 s_pos_steps 仍按
     *   未钳制的 s_vel_steps 记账 —— 「记账走的步数」会远多于「真正发出的脉冲数」，
     *   现场表现就是**高速时严重失步**（走不到目标，速度越快差得越多）。
     *   把钳制提前到积分之前，记账与实际发射就永远一致。
     *
     *   ★ v7 起是**双向**钳制（上=脉冲上限、下=起跳频率）：下限那侧的理由见
     *     motion_cruise_freq_hz() 的说明 —— 低于起跳频率时电机一步都不走。 */
    v_max = motion_cruise_freq_hz(cfg.speed, NULL, NULL);

    int32_t remaining = s_cmd_target - s_pos_steps;
    if (remaining == 0) {
        s_last_emit_n = 0;      /* 本拍没有脉冲（原因见函数开头说明） */
        motion_finish(true);
        return;
    }
    int dir = (remaining > 0) ? 1 : -1;
    float abs_rem = (float)((remaining > 0) ? remaining : -remaining);

    /* ★★★ v17（2026-09-22 深夜）：**整段恒定频率，取消收尾减速**
     *     —— 把串口 `run` 的发射方式移植到界面运动路径上 ★★★
     *
     *  现场实测（本次改动的全部依据）：
     *    · 串口 `run`（**恒定 12000Hz**、单向连续）——现场"转得很好"；
     *    · 界面走的是**梯形曲线**，每段末尾还会从 12000Hz 减速到 8500Hz ——
     *      日志 `本段实际发射频率 8620 ~ 12000 Hz` 记的就是那一段。
     *      而 8500Hz 正是这台机器"能转"的**下沿/临界点**：转子在那里会打滑，
     *      现场现象就是"**正转后反转**"（前段 12000 正常，末段掉进临界带就弹回来）。
     *  ⇒ 结论：**界面这条路的频率不要变**：起步 = 巡航 = 收尾 = 工作频率
     *    （出厂 12000Hz = `v_max`），到位就把脉冲停掉（脉冲一停转子就停）——
     *    与 `run` + `stop` **逐项同构**：恒定频率、单向、整块事务、队列预填、50% 方波。
     *
     *  ⇒ 本函数**不再需要** MOTION_TURN_FREQ_MIN_HZ 参与曲线：它仍然作为
     *    "Speed 下限保护"用在 motion_cruise_freq_hz() 里（**别删**）。
     *    也**不再有** v_allowed / 刹车距离 这个概念 —— 到位即停、没有减速段。
     *
     *  ★ 唯一保留的分支是"软启动爬升"（运行期 `mtest ss <hz>` 打开时）：
     *    s_soft_start_hz = 0（默认关闭）⇒ v_start == v_max ⇒ 第一拍就是工作频率，
     *    下面这个 if 退化成"恒速"，与 `run` 完全一致。 */
    const bool  soft_on = (s_soft_start_hz >= MOTION_STEP_FREQ_MIN_HZ &&
                           s_soft_start_hz < MOTION_TURN_FREQ_MIN_HZ);
    const float v_start = soft_on ? s_soft_start_hz : v_max;   /* ★ 无软启动 = 工作频率起跳 */
    const float v_req   = v_max;    /* ★ v17：恒速 —— **没有减速段** */

    /* 速度逼近：**只有加速侧**（软启动关闭时它一步就到位）。没有再减速这一支。 */
    if (s_vel_steps < v_req) {
        s_vel_steps += a_acc * dt;
        if (s_vel_steps > v_req) {
            s_vel_steps = v_req;
        }
        if (s_vel_steps < v_start) {
            s_vel_steps = v_start;
        }
    }

    /* 3) 本 tick 的脉冲数（含小数累计），且不得超过剩余步数 */
    float want = s_vel_steps * dt + s_step_acc;
    int32_t n = (int32_t)want;
    s_step_acc = want - (float)n;
    if (n > (int32_t)abs_rem) {
        n = (int32_t)abs_rem;
    }

    if (n > 0) {
        /* 位置一律按**请求的** n 推进：
         *   · 模拟模式本来就不发脉冲，位置必须照常推进（这正是 SIMULATE 的用途）；
         *   · 真机上若 rmt_transmit 失败，这里也会推进（否则曲线会卡住），
         *     但"请求了却没发出去"会被运动结束时的**脉冲核对**日志以 ERROR 级别
         *     报出来，并触发上面的看门狗 —— 不会静默错位。 */
        s_pos_steps += dir * n;
        /* ★★★ v18：本拍脉冲里**前面的 s_bl_left 个是间隙补偿**（空走），
         *     不推进逻辑位置；剩下的才真正推动轴。
         *   `s_last_dir`（"上次实际输出方向"）也只在**真出过脉冲之后**才提交 ——
         *   与参考工程 motor.cpp 的 s_out_dir / s_backlash_left 语义一致。 */
        {
            int32_t adv = n;
            if (s_bl_left > 0) {
                const int32_t used = (s_bl_left < adv) ? s_bl_left : adv;
                s_bl_left -= used;
                adv -= used;
            }
            if (adv > 0) {
                s_logic_steps += dir * adv;
                if (!s_dir_committed) {
                    s_dir_committed = true;
                    s_last_dir = dir;
                }
            }
        }
        /* ★★ v12：**攒够一个批次（默认 4ms）再提交**，而不是每 1ms 交一个事务 ★★
         *   这样 RMT 队列里始终有约 3×4ms = 12ms 的存货，motion_task 被 WiFi 等
         *   高优先级任务抢占几毫秒也不会把脉冲串抽空（见 MOTION_TX_BATCH_MS 的说明）。
         *   ★ 最后一批必须立刻提交（move_end），否则到位的瞬间会丢掉这几毫秒的脉冲
         *     —— 那正是"位置短一截 / 脉冲核对不符"的来源。 */
        s_batch_pulses += (float)n;
        s_batch_ticks++;                        /* 1 拍 = MOTION_PROFILE_DT_S(1ms) */
        const bool move_end = (s_pos_steps == s_cmd_target);
        /* 三个提交条件（谁先满足用谁）：
         *   ① 攒够 MOTION_TX_BATCH_MS 拍（4ms）；
         *   ② 攒够单个事务的脉冲上限（高频时 4ms 会超过 64 个脉冲，而 RMT 一个事务装不下
         *      —— 不能让它被 motion_emit_steps 截断而丢脉冲）；
         *   ③ 本拍已经走到目标（收尾必须立刻交，否则最后几毫秒的脉冲会丢 ⇒ 位置短一截）。 */
        if (s_batch_ticks >= (uint32_t)MOTION_TX_BATCH_MS ||
            s_batch_pulses >= (float)MOTION_MAX_STEPS_PER_TX ||
            move_end) {
            /* 把攒下的整脉冲交出去；单个事务装不下的部分拆开（最多 2 个事务：
             * 攒批上限 ≈ 63+每拍脉冲数 ≤ 84 ⇒ 64+20）。小数部分（<1 个脉冲）留到下一批。
             * guard 是防御性的：万一攒批异常变大，也不让 pending 冲过 RMT 队列深度
             * （trans_queue_depth=4，超出会被 rmt_transmit 拒绝 = 真丢脉冲）。 */
            for (int guard = 0; s_batch_pulses >= 1.0f && guard < 3; guard++) {
                uint32_t nb = (uint32_t)s_batch_pulses;
                if (nb > MOTION_MAX_STEPS_PER_TX) {
                    nb = MOTION_MAX_STEPS_PER_TX;
                }
                motion_emit_steps(nb, s_vel_steps);
                s_batch_pulses -= (float)nb;
            }
            s_batch_ticks = 0;
        } else {
            s_last_emit_n = 0;      /* 本拍只是攒着，没有产生 RMT 事务 */
        }
    } else {
        /* ★ 本拍没有脉冲：必须显式把 s_last_emit_n 归零。
         *   motion_task 的队列填充循环靠它判断"还要不要继续填"，留着上一拍的旧值
         *   会让循环空转并把曲线提前推进（现象：起步/收尾的速度、落点都不对）。 */
        s_last_emit_n = 0;
    }

    /* ★ 看门狗：位置在推进，但 rmt_transmit() 从来没成功过 -> 直接把根因喊出来。
     *   这是"电机不转"里最难查的一类：界面、位置、状态机全都正常，
     *   只有脉冲其实根本没交给 RMT 硬件。 */
    if (s_tx_ok == 0 && s_tx_fail > 0 && !s_move_no_tx_warned) {
        s_move_no_tx_warned = true;
        ESP_LOGE(TAG, "★★ 位置在推进，但 rmt_transmit() 全部失败（%u 次）：脉冲没有出引脚，"
                      "电机不可能转。请先看上面第一条 rmt_transmit 报错。", (unsigned)s_tx_fail);
    }

    s_vel_mm_atomic = s_vel_steps / MOTION_STEPS_PER_UNIT;
    motion_update_report_pos();
}

/** 报警/急停：停止发脉冲（不打断已经在发的脉冲串，避免 RMT 处于非法状态） */
static void motion_abort(void)
{
    if (!s_moving) {
        return;
    }
#if MOTION_STEP_PWM_MODE && !MOTION_SIMULATE
    /* ★ v22：PWM 模式没有"在途事务"要等，直接停波形（脉冲一断转子就停） */
    motion_pwm_abort();
#else
    motion_wait_tx_done();   /* 把在途脉冲放完，最多一个 tick 的量 */
#endif
    motion_finish(false);
}

/*==============================================================================
 * motion_task
 *============================================================================*/
static void motion_task(void *arg)
{
    (void)arg;
    float target_mm;

    for (;;) {
        /* ★★★ v14：`mtest run` 一开始，界面里**还没跑完的那一段必须先停掉** ★★★
         *   下面那个 `if (s_run_active)` 只拦"新下发的界面运动"，拦不住**已经在跑**的
         *   那一段 —— 于是 cli_run_task（DIR=正向、固定频率）会和 motion_task 的
         *   motion_run_profile（另一个 DIR、梯形曲线）**同时往同一个 RMT 通道灌脉冲**，
         *   两根任务各自设 DIR ⇒ 现场就是"方向乱跳" + 频率忽高忽低。
         *   motion_abort() 会把在途脉冲放完并把这一段正常收尾（与报警中止同一套）。 */
        if (s_run_active && s_moving) {
            ESP_LOGW(TAG, "★ `mtest run` 启动：先把界面里没跑完的那一段停掉"
                          "（避免两条路径同时抢 RMT/DIR）");
            motion_abort();
        }

        /* 空闲时阻塞等命令（顺便周期性地查报警）；运动中不阻塞，保持节拍 */
        TickType_t wait = s_moving ? 0 : pdMS_TO_TICKS(MOTION_IDLE_POLL_MS);
        if (xQueueReceive(s_cmd_q, &target_mm, wait) == pdTRUE) {
            if (s_run_active) {
                /* ★ v16：`mtest run` 连续运行中 —— 界面下发的运动会和它抢 RMT/DIR/ENA，
                 *   直接忽略并说明原因（比两处互相踩、现象不可复现要好）。 */
                ESP_LOGW(TAG, "★ `mtest run` 连续运行中：忽略本次界面运动（先串口敲 stop）");
            } else if (s_alarm_latched) {
                ESP_LOGW(TAG, "Move rejected: alarm latched");
            } else {
                motion_begin(target_mm);
            }
        }

        motion_check_alarm();

        if (s_alarm_latched) {
            motion_abort();
        } else if (s_moving) {
            /* ★★★ 队列预填：让 RMT **硬件**连续执行事务 ★★★
             *
             * 老写法是「等上一拍发完 → CPU 重新 rmt_transmit 发下一拍」，两步之间
             * 必然隔着「事务完成中断 → 任务被唤醒 → 重新启动硬件」这段固定开销 ε。
             * ε 期间 STEP 完全没有脉冲 —— 脉冲串被周期性地打断，而细分驱动器内部
             * 是按输入脉冲做微步插补的：插补一断就与转子位置错开、恢复时受一次冲击，
             * 于是丢步（速度越高，同样长的空档对应的丢失角度越大）。
             * 这就是「脉冲不连续」的直接来源。
             *
             * 现在改成：始终让 RMT 队列里保持 MOTION_TX_QUEUE_TARGET 个事务。
             * 一个事务发完，硬件**立刻**开始队列里的下一个，全程不需要 CPU 介入 ——
             * 从第一个脉冲到最后一个脉冲之间没有任何空档。
             * 本任务只负责「把队列填满」，填满后睡 1ms 再回来补
             * （1ms 远小于一个事务的时长 5ms，队列不会见底）。
             *
             * 注意：这里**不再**调用 motion_wait_tx_done() 等上一拍发完 ——
             * 那种等待本身就是空档的来源。只有报警/停止才会去等（见 motion_abort）。
             *
             * ★ v12：事务是**攒批提交**的（MOTION_TX_BATCH_MS），所以"这一拍没交事务"
             *   很常见（4 拍里有 3 拍如此）。不能再像以前那样碰到 s_last_emit_n == 0 就
             *   退出填充 —— 那样每 1ms 只推进一步曲线、队列永远填不满，等于白改。
             *   现在的规则：连续 MOTION_TX_BATCH_MS 拍都没交事务才让出 CPU
             *   （真正的"低速/收尾/模拟模式"才会走到那一步）。 */
#if MOTION_STEP_PWM_MODE && !MOTION_SIMULATE
            /* ★★★ v22：PWM 模式 —— 脉冲由**硬件**产生，本任务只负责"定时到 → 收尾" ★★★
             *   不再有队列预填/攒批/曲线积分（那些都是 RMT 路径的东西）。
             *   兜底看门狗：万一 ledc 的停止定时器没触发，也**绝不能**让轴一直跑下去。 */
            if (s_pwm_done) {
                s_pwm_done = false;
                motion_finish(true);
            } else {
                const uint64_t expect_us = (s_pwm_freq_hz > 0u)
                    ? ((uint64_t)s_pwm_pulses * 1000000ull / (uint64_t)s_pwm_freq_hz) : 0ull;
                const uint64_t elapsed_us = (uint64_t)(esp_timer_get_time() - s_move_t0_us);
                if (expect_us > 0ull && elapsed_us > expect_us + 1000000ull) {
                    ESP_LOGE(TAG, "★★ PWM 停止定时器超时（预期 %.3f s，已 %.3f s）：立即停波形收尾",
                             (double)expect_us / 1000000.0, (double)elapsed_us / 1000000.0);
                    motion_pwm_abort();
                    motion_finish(true);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
#else
            int idle_ticks = 0;
            while (s_moving &&
                   __atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) < MOTION_TX_QUEUE_TARGET) {
                motion_run_profile(MOTION_PROFILE_DT_S);
                if (s_last_emit_n != 0) {
                    idle_ticks = 0;
                } else if (++idle_ticks >= MOTION_FILL_IDLE_LIMIT) {
                    break;      /* 连续这么多拍都没交事务（极低速 / 收尾 / 模拟）→ 让出 CPU */
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
#endif
        }

        /* ★★★ v23：**静止自动脱机** —— 消掉停下来之后的"嗡嗡"（保持电流）★★★
         *   现场口径："马达停了之后嗡嗡响" + "静止 2s 后自动脱机"。
         *   判据/前提/取值见 app_motion.h 的 MOTION_IDLE_RELEASE_MS：
         *     · 只有"没在运动、也没报警"并且安静地待够 MOTION_IDLE_RELEASE_MS 才抬 ENA；
         *     · 下一次下发目标时 motion_begin() 会自动重新使能（并等 100ms 稳定）⇒ 手感不变；
         *     · 报警仍然是"立即抬 ENA 切断输出"（在 motion_check_alarm 里），与本段无关。 */
#if !MOTION_SIMULATE
        if (MOTION_IDLE_RELEASE_MS > 0) {
            if (s_moving || s_alarm_latched) {
                s_idle_since_ms = 0;                     /* 运动中 / 报警中：不计时 */
            } else if (s_idle_since_ms == 0) {
                s_idle_since_ms = esp_timer_get_time() / 1000;   /* 重新起算（µs -> ms）*/
            } else if (!s_ena_released &&
                       (esp_timer_get_time() / 1000 - s_idle_since_ms) >=
                       (int64_t)MOTION_IDLE_RELEASE_MS) {
                s_ena_released = true;
                motion_driver_enable(false);             /* 抬 ENA = 脱机 = 没有保持电流 */
                ESP_LOGW(TAG, "静止 %d ms：**自动脱机**（ENA=IO%d 抬到 %d）—— 嗡嗡声/发热消失；"
                              " 下次下发目标会自动重新使能（前提：脱机期间负载不能把轴推动）",
                         (int)MOTION_IDLE_RELEASE_MS, MOTION_GPIO_ENA, !MOTION_ENA_ACTIVE_LEVEL);
            }
        }
#endif

        motion_push_report(false);
    }
}

/*==============================================================================
 * ★★ 上电固定段 / `mtest hold`：固定频率连发 N 毫秒 ★★
 *
 * 与 `examples/peripherals/rmt/stepper_motor` 的**上电 10 秒**逐项同构：
 *     设 DIR → ENA 拉低（使能）→ 等 MOTION_ENA_SETTLE_MS → **固定频率连续发脉冲**
 *     → 发满后立刻停 →（release_ena 时）脱机
 * 例程那 10 秒现场确认**能连续转**，而本工程原来"一步不动" —— 所以这一段是
 * 判定"硬件/驱动器认不认脉冲"与"本工程流程对不对"的分水岭（见 app_motion.h 的说明）。
 *
 * ★ 唯一的差别：脉冲由**本工程自己的发射路径**产生（一个事务 n 个符号、loop_count=0），
 *   而且**共用同一个脉宽设置**（s_step_pulse_us，默认见 Kconfig）。
 *   所以：
 *     · 这一段能转  ⇒ 脉冲链路没问题，去查曲线/状态机/参数；
 *     · 这一段也只嗡嗡 ⇒ 与曲线/UI 无关：先试 `STEP 高电平脉宽 = 0`（50% 方波，
 *       与能转的例程波形一致），再查电流档/相序/供电（README 9.9）。
 *
 * ★ 只在"上电固定段 / `mtest hold`"被启用时才编译：这两个功能之外没有调用者
 *   （原来的界面「连续运行自检」入口已按现场要求删除），不然会变成"定义了没用"
 *   的静态函数。
 *============================================================================*/
#if !MOTION_SIMULATE && (MOTION_BOOT_HOLD_ON_BOOT || MOTION_CLI_ON)
/** 以固定频率连发 ms 毫秒（≈ hz × ms / 1000 个脉冲），发完自动停。
 *  @param release_ena 发完是否脱机（上电固定段与 mtest hold 都置 true，避免保持电流嗡嗡响）
 *  @return 实际请求发出的脉冲数（与 RMT 报告值一起在日志里核对） */
static uint32_t motion_fixed_run(float hz, uint32_t ms, bool release_ena)
{
    /* 上下限用 app_motion.h 里的宏（MOTION_SCAN_FREQ_MIN/MAX 定义在本文件后面的
     * 诊断段里，不能在这里引用 —— 那两个还会随 CLI/SELFTEST 开关消失）。 */
    if (!(hz >= MOTION_STEP_FREQ_MIN_HZ)) {
        hz = MOTION_STEP_FREQ_MIN_HZ;
    }
    if (hz > MOTION_SCAN_FREQ_MAX_HZ) {
        hz = MOTION_SCAN_FREQ_MAX_HZ;
    }
    if (ms == 0) {
        ms = 1;
    }
    uint32_t total = (uint32_t)(hz * (float)ms / 1000.0f);
    if (total == 0) {
        total = 1;
    }
    const float travel_mm = (float)total / MOTION_STEPS_PER_UNIT;

    /* ★★ v11 软启动：本段**不再"瞬间跳到目标频率"**，而是从 MOTION_SOFT_START_HZ
     *   （默认 500Hz）按 accel 爬升到目标频率（500→8500Hz 约 67ms / 0.38mm）。
     *   依据：现场"起转那一下发顿"是步进电机的**牵入频率**限制（转子无法瞬间跟上
     *   5.3 转/秒，会滑一下再同步）；留一段爬升，转子就能跟着加速上去。
     *   爬升段用**更小的事务**（16 脉冲），频率台阶更细、更顺。 */
    const float ss_hz = (s_soft_start_hz >= MOTION_STEP_FREQ_MIN_HZ &&
                         s_soft_start_hz < hz) ? s_soft_start_hz : hz;
    float a_hz_s = app_config_get().accel * MOTION_STEPS_PER_UNIT;   /* 与运动路径同一个加速率 */
    if (a_hz_s < 1000.0f) {
        a_hz_s = 1000.0f;           /* accel 被设成 0/极小时兜底 */
    }

    ESP_LOGW(TAG, "=== 固定段开始：%.0f Hz × %" PRIu32 " ms（约 %" PRIu32 " 个脉冲 = %.2f mm"
                  " @%.0f 步/mm） 脉宽 %" PRIu32 " µs%s ===",
             hz, ms, total, travel_mm, MOTION_STEPS_PER_UNIT,
             (uint32_t)s_step_pulse_us, (s_step_pulse_us == 0) ? "（0 = 50% 方波）" : "");
    if (ss_hz < hz) {
        ESP_LOGW(TAG, "  软启动：%.0f → %.0f Hz（约 %.0f ms，加速率 %.0f Hz/s）—— "
                      "治「起转卡顿」（步进电机的牵入频率限制）",
                 ss_hz, hz, (hz - ss_hz) / a_hz_s * 1000.0f, a_hz_s);
    } else {
        ESP_LOGW(TAG, "  软启动已关闭：直接从 %.0f Hz 起跳（v10 行为）", hz);
    }
    ESP_LOGW(TAG, "★ 这一段是**真走轴**的（单向 %.2f mm）：先确认行程够；"
                  "要短一点就把 Kconfig 的 ms 改小，或在串口里 `mtest hold %.0f 2000`",
             travel_mm, hz);

    /* 脉冲核对计数清零（与运动路径同一套判据：请求数 + 事务数 = RMT 符号数） */
    s_pulse_req = 0;
    s_tx_ok = 0;
    s_tx_fail = 0;
    s_tx_done = 0;
    __atomic_store_n(&s_sym_done, 0, __ATOMIC_SEQ_CST);

    /* 与例程 same 的顺序：DIR → ENA → 稳定 MOTION_ENA_SETTLE_MS → 才发脉冲
     * ★ v10：本值默认从 20ms 提到 **100ms** —— "起转那一下发顿"十有八九是
     *   驱动器还没醒（光耦导通 + 输出级使能 + 保持力矩建立）就开始发脉冲。
     */
    motion_gpio_set(MOTION_GPIO_DIR, MOTION_DIR_POSITIVE_LEVEL);
    motion_driver_enable(true);
    ESP_LOGW(TAG, "  使能稳定时间 %d ms（驱动器「醒」过来之前发的脉冲不会被计）",
             (int)MOTION_ENA_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(MOTION_ENA_SETTLE_MS));

    /* ★ 本段跑在**调用者上下文**里（上电时是 app_main，优先级只有 1；`mtest hold` 时是
     *   控制台任务 3）。补队列的循环必须按 ms 级节拍被唤醒，否则 RMT 队列见底
     *   ⇒ 脉冲串出现空档 ⇒ 现场就是"一顿一顿/起转卡一下"。
     *   所以本段期间**临时把优先级提到与 motion_task 相同**，跑完立刻还原。 */
    const UBaseType_t prio_saved = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MOTION_TASK_PRIO);

    uint32_t sent = 0;
    /* 脉冲连续性监视：每 100ms 把"已提交脉冲数"与"按频率应该发到的脉冲数"比一次。
     * ★ 这是"卡顿到底是不是软件空档"的**直接判据**：
     *   正常时已提交数会**略领先**（队列里有存货）；落后 >10% 说明补队列被拖住过。 */
    const int64_t t_start = esp_timer_get_time();
    int64_t next_check_us = t_start + 100000;
    bool stall_warned = false;

    while (sent < total) {
        /* 本拍用哪个频率：爬升段 = ss_hz + accel×已用时；到顶后 = 目标频率。 */
        float f_now = hz;
        uint32_t n = MOTION_MAX_STEPS_PER_TX;
        if (ss_hz < hz) {
            const float elapsed_now = (float)(esp_timer_get_time() - t_start) / 1000000.0f;
            f_now = ss_hz + a_hz_s * elapsed_now;
            if (f_now >= hz) {
                f_now = hz;             /* 已升到目标频率 */
            } else {
                n = 16;                 /* 爬升段：小事务 ⇒ 频率台阶更细 */
            }
        }
        if (n > total - sent) {
            n = total - sent;
        }
        motion_emit_steps(n, f_now);
        sent += n;
        /* ★ 用**队列深度**判满，不依赖 tick 长度（vTaskDelay(1) 在 100Hz tick 下是 10ms，
         *   那样会等出空档）。这里所有队列消费者都是 RMT 硬件，1ms 一次检查足够。 */
        while (__atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) >= MOTION_TX_QUEUE_TARGET) {
            vTaskDelay(1);
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_check_us) {
            const float elapsed_s = (float)(now_us - t_start) / 1000000.0f;
            const float expect = hz * elapsed_s;
            const float behind_pct = (expect > 0.0f) ? (100.0f * (expect - (float)sent) / expect) : 0.0f;
            if (behind_pct > 10.0f && !stall_warned) {
                stall_warned = true;
                ESP_LOGW(TAG, "★★ 固定段中途检出**脉冲落后**：已提交 %" PRIu32 " 个 / 按 %.0f Hz 应该到 %.0f 个"
                              "（落后 %.0f%%）⇒ 补队列被拖住过，脉冲串有断档 —— 这就是「卡/顿」的软件侧原因；"
                              " 除 `使能稳定时间` 之外还要看 RMT 队列深度与任务优先级（见 AGENTS.md）",
                         sent, hz, expect, behind_pct);
            }
            next_check_us = now_us + 100000;
        }
    }
    vTaskPrioritySet(NULL, prio_saved);     /* 还原调用者的优先级 */
    /* 收尾：等最后几个事务发完再停（低频率档一个事务可能几百 ms，所以给足 5s）。 */
    const int64_t deadline = esp_timer_get_time() / 1000 + 5000;
    while (__atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) > 0 &&
           esp_timer_get_time() / 1000 < deadline) {
        vTaskDelay(1);
    }
    if (__atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) > 0) {
        ESP_LOGW(TAG, "★ 固定段收尾等待超时（仍有 %d 个事务在途），先继续走",
                 __atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST));
    }

    if (release_ena) {
        motion_driver_enable(false);        /* 脱机：轴可手盘，也没有保持电流的嗡嗡声 */
    }
    /* 收尾核对（与运动路径同一判据）：RMT 符号数 = 请求脉冲数 + 完成事务数，
     * 对得上 ⇒ 每一个脉冲都真的出了引脚，剩下的只能查接线/驱动器/供电。 */
    const int sym_done = __atomic_load_n(&s_sym_done, __ATOMIC_SEQ_CST);
    /* ★ 现场判据：实测平均频率 vs 目标频率（差得多 = 补队列被拖过）、有无脉冲落后。
     *   与"听/看轴顺不顺"一一对应，不需要示波器。 */
    const float elapsed_s = (float)(esp_timer_get_time() - t_start) / 1000000.0f;
    ESP_LOGW(TAG, "  实测：%.2f s 内提交 %" PRIu32 " 个脉冲 = 平均 %.0f Hz（目标 %.0f Hz）%s",
             elapsed_s, sent, (elapsed_s > 0.0f) ? ((float)sent / elapsed_s) : 0.0f, hz,
             stall_warned ? "★★ 期间检出过脉冲落后（见上文）" : "；全程连续，未检出断档");
    if (s_tx_ok > 0 && s_tx_ok == s_tx_done && sym_done == s_pulse_req + (int)s_tx_ok) {
        ESP_LOGW(TAG, "=== 固定段结束（核对 OK）：请求 %d 个脉冲 / %u 个事务，RMT 报告 %d 个符号%s ===",
                 s_pulse_req, (unsigned)s_tx_ok, sym_done,
                 release_ena ? "；已脱机（轴可手盘）" : "；仍使能");
    } else {
        ESP_LOGE(TAG, "=== 固定段结束（★★ 脉冲核对异常）：请求 %d / 入队 %u / 发完 %u / RMT %d 个符号"
                      "（期望 %d）===",
                 s_pulse_req, (unsigned)s_tx_ok, (unsigned)s_tx_done, sym_done,
                 s_pulse_req + (int)s_tx_ok);
    }
    return total;
}
#endif  /* !MOTION_SIMULATE && (BOOT_HOLD || CLI) */

/*==============================================================================
 * 参数扫描自检 + 串口命令行 `mtest`（"换挡试转"）
 *
 * ## 为什么要有这一段
 * 日志已经能证明「脉冲数 / 速率 / DIR / ENA 电平全对」，现场现象仍是"转一点 + 抖"。
 * 这时能在**软件里扫**的变量只剩两个：
 *   ① **脉宽**：TB6600 是光耦隔离输入，3.3V 直驱时光耦电流只有约 2mA、LED 上升沿慢
 *      —— 5µs 的窄脉冲可能被判成"时有时无" → 丢脉冲 → 转一点就抖。
 *   ② **频率**（= speed × 步数/mm）：这台机器**低频不走、高频也走不动**，
 *      只有中间一段能用（见 app_motion.h 的 MOTION_TURN_FREQ_MIN_HZ 长注释：
 *      当前拨码 1600 脉冲/圈，实测 **8500Hz 起才转**）。
 * 两者都能在一条命令里试完，不用改代码也不用仪器：
 *
 *      mtest                按当前设置跑一次（默认 **8500Hz** / 每方向 1600 步 = 2mm）
 *      mtest 8500           设频率 8500Hz 再跑（就是 stepper_motor 例程验证过的那档）
 *      mtest 8500 50        设频率 8500Hz、脉宽 50µs 再跑
 *      mtest 8500 0         脉宽 0 = 自动（周期的一半 = 50% 方波）
 *      mtest 8500 5 8000    第三个参数是每方向步数（8000 步 = 10mm）
 *      mtest 8500 5 1600 6000  第四个参数 = 加速率 Hz/s（隔离"加速太陡"这个变量）
 *      mtest sweep          自动跑 6 频率 × 4 脉宽 = 24 段（6000~16000Hz，骑在起跳频率两侧）
 *      mtest hold [hz] [ms] ★ **固定频率连发 N 毫秒**（默认 8500Hz × 10s）—— 与例程上电段
 *                           逐项同构，用来做"这一段到底能不能连续转"的对照
 *      mtest def            脉宽恢复出厂默认
 *      help                 列出所有命令
 *
 *  ★ v13 起默认频率 12000Hz、默认步数 1600：默认值必须落在**能正常连续转**的区间里
 *    （12000Hz 就是串口 `run` 现场验证"转得很好"、也是界面出厂工作点的那一档），
 *    否则第一次敲 `mtest` 只看到嗡嗡响，会误以为是新故障。
 *  ★ 8500Hz 是**下沿/临界点**（空载能过、带载发毛）：只用它确认下沿在哪，
 *    不要把它当工作点（界面按 8500Hz 跑就是"启动后左右转"）。
 *  ★ v9 起默认脉宽 = **0（50% 方波）**，与例程的波形一致（见 Kconfig 的说明）。
 *
 * ## 每段做什么（永远净位移 0，不会把轴开走）
 *   使能 ENA → 设 DIR → 等 MOTION_ENA_SETTLE_MS 稳定 → 正转 N 步 → **再反转 N 步**，
 *   每段结束打一条脉冲核对（与运动路径同一套判据）。
 *   ★★ 诊断串**带加减速**（起步 50Hz → 目标频率 → 按剩余距离减速）：
 *      目的是**找出低频禁区在哪**，所以这里**故意不用** MOTION_TURN_FREQ_MIN_HZ
 *      那个下限（真实运动路径是整条曲线都在起跳频率以上，见 app_motion.h）。
 *      日志里会提示"本段步数最多能升到约 X Hz"：低于目标时把第 3 个参数（每方向步数）
 *      加大再测，否则测的又是"没升上去"的假象。
 *   ★ 本段步数必须够"升到目标频率"：峰值 ≈ √(加速率 × 步数)。
 *     出厂 accel=150mm/s²(=120000Hz/s) 时，想测到 16000Hz 需要 ≥ 2133 步 ≈ 2.7mm。
 *     默认 N = 1600 步 = 2mm（可达约 13.9kHz），扫描模式用 MOTION_SCAN_SWEEP_STEPS。
 *
 * ## 判读（现场只要"看/听"，不需要任何仪器 —— 完整表见 main/README.md 9.11）
 *   · **低频全都不转、到 8500Hz 左右才开始转** = 这台机器的低速禁区（现场实测）。
 *        对策：真实运动的曲线**第一拍就是巡航频率**（软启动默认关），出厂
 *        Speed = **15.000mm/s = 12000Hz**（= 串口 `run` 现场验证"转得很好"的工作点）
 *        —— **不要**把 Speed 往低调（低了就是"不动"）。
 *   · **在 8500Hz 附近"发毛/左右来回摆"** = **贴着下沿跑、临界失步**
 *        （现场实测：界面按 8500Hz 跑就是"启动后左右转、跟震动一样"）。
 *        对策：把 Speed 抬到 12000Hz（= `f 12000` + `run` 那一档）。
 *   · 同一频率下 4 段**只有脉宽不同**：靠后（脉宽大）的明显更顺
 *        ⇒ 根因是脉宽太窄 → menuconfig 的 `STEP 高电平脉宽` 改成 50（或 0）。
 *   · 所有频率、所有脉宽**全都抖** ⇒ 与这两个软件变量无关：
 *        查拨码电流（`SW4=ON` 才是 2.0A）/ 相线分组 / 共地 / 驱动器（README 9.9）。
 *   · 低频顺、高频才抖 ⇒ 是速度上限，Admin 里把 Speed 降下来（但别降到起跳频率以下）。
 *   · 出现 `★★ 脉冲核对异常` ⇒ 有事务卡在 RMT 里（软件问题，把日志贴出来）。
 *
 * ⚠️ 上电自检（ROLLCOATER_MOTION_SELFTEST，默认关）会让电机上电就来回摆约 1 分钟；
 *    `mtest`（ROLLCOATER_MOTION_CONSOLE，默认开）只在敲命令时动。
 *============================================================================*/
#ifdef CONFIG_ROLLCOATER_MOTION_SELFTEST
#define MOTION_SELF_TEST_ON_BOOT    (1)
#endif
#ifndef MOTION_SELF_TEST_ON_BOOT
#define MOTION_SELF_TEST_ON_BOOT    (0)         /* 改成 1 = 上电自检一次 */
#endif

#if (MOTION_SELF_TEST_ON_BOOT || MOTION_CLI_ON) && !MOTION_SIMULATE
/*------------------------------------------------------------------------------
 * 共享部分：试转一趟 / 核对 / 自动扫描
 *----------------------------------------------------------------------------*/
/** 每段（每个方向）的默认步数：**1600 步 = 2mm @800 步/mm**，来回各一趟、净位移 0。
 *  ★ v7 从 800 改回 1600：本段的**峰值频率 ≈ √(加速率 × 步数)**，800 步在
 *    accel=150mm/s²(=120000Hz/s) 下只能升到约 9.8kHz —— 正好卡在起跳频率(8500Hz)
 *    边上，测 12kHz 以上的档会被判成"没升上去"而看不出真假。
 *    1600 步可升到约 13.9kHz；要测更高的档就把命令行第 3 个参数加大
 *    （`mtest 16000 5 3200`），日志会提示"最多升到约 X Hz"。 */
#define MOTION_SCAN_STEPS_PER_DIR   (1600)
/** **自动扫描（mtest sweep / 上电自检）**每段的步数：3200 步 = 4mm。
 *  比单次 `mtest` 的默认值大，是为了让 16000Hz 那一档**真的能升上去**
 *  （√(120000 × 3200) ≈ 19.6kHz ≥ 16kHz）。代价是每段摆动 ±4mm，
 *  净位移仍是 0，不会把轴开走。 */
#define MOTION_SCAN_SWEEP_STEPS     (3200)
/** 自动扫描时段与段之间的停顿（ms）：方便肉眼/耳朵分辨 */
#define MOTION_SCAN_GAP_MS          (500)
/** mtest 允许的步数上限：40000 步 = 50mm（防手滑把轴开到底） */
#define MOTION_SCAN_STEPS_MAX       (40000)
/** mtest 的频率范围：RMT 周期上限 32767µs ⇒ 约 30.5Hz 起。
 *  上限与 MOTION_SCAN_FREQ_MAX_HZ 保持一致（诊断档 20000）—— 发射上限是多少，
 *  这里就写多少，绝不出现"设了 20000 却只发 8000"的静默钳制（前几轮反复踩过）。 */
#define MOTION_SCAN_FREQ_MIN        (40.0f)
#define MOTION_SCAN_FREQ_MAX        (MOTION_SCAN_FREQ_MAX_HZ)

/** 自动扫描用的频率档 / 脉宽档
 *  ★ v13 按现场结论重排：**12000Hz 是已验证能连续平稳转的工作点**
 *    （串口 `run` 现场验证），8500Hz 只是下沿。这 6 档骑在下沿两侧，一眼就能看出
 *    "从哪一档开始转（下限）、哪一档开始抖（上限）"。
 *    低频那半段（200~4000Hz）已经确定"不转"，留着扫只是浪费时间。
 *  ★ 步数用 MOTION_SCAN_SWEEP_STEPS(3200)，出厂 accel 下各档都能升到目标。 */
static const float s_scan_freq[] = { 6000.0f, 7500.0f, 8500.0f, 10000.0f, 12000.0f, 16000.0f };
#define MOTION_SCAN_FREQ_NUM   ((int)(sizeof(s_scan_freq) / sizeof(s_scan_freq[0])))
static const uint32_t s_scan_pulse[] = { 5, 20, 50, 0 };   /* 0 = 自动 50% 方波 */
#define MOTION_SCAN_PULSE_NUM  ((int)(sizeof(s_scan_pulse) / sizeof(s_scan_pulse[0])))

/** ★ 对照实验开关：true = 本段用**软件翻转 GPIO**发脉冲（照 Arduino 的做法，绕开 RMT）。
 *  由 `mtest bb ...` 置位。见 motion_bitbang_emit / motion_bitbang_take_step_pin。 */
static bool s_scan_bitbang;

/** 本工程真实运动用的"频率上升率"（Hz/s）= accel(mm/s²) × 步数每毫米。
 *  ★★ 诊断脉冲串**必须带加减速**，否则测出来的不是机器的能力、而是测试自己的毛病：
 *     步进电机不可能从 0 瞬间跳到 1kHz 以上的转速，一定会丢步（只嗡嗡不转）。
 *     现场 2026-09-22 就踩过这个坑 —— 40Hz 能"一步一步转"，而 1000Hz 以上全抖，
 *     原因就是当时这一段是"直接按目标频率发"，把所有高频档都测成了抖动。
 *     现在与运动路径一样：起步 50Hz，按下面这个上升率升到目标频率，末尾再按剩余
 *     距离（v ≤ √(2·a·剩余)）减速收尾。 */
/** ★ 命令行第 4 个参数：本段的加速率（Hz/s）。0 = 用真实运动那个（cfg.accel×步数每毫米）。
 *  为什么需要它：加速率是"电机跟不跟得上"的**独立变量**，和"跑不到目标频率"是两回事
 *  （现象都是抖）。v7 出厂 accel=150mm/s²×800 = **120000 Hz/s**（≈0→8500Hz 只要 71ms，
 *  用来快速穿过低频禁区）；把它单独调温和（例如 1000 甚至 500 Hz/s）就能把
 *  "加速太陡"与"频率不对"两个原因分开。 */
static float s_scan_accel_override_hz_s;
static float motion_scan_accel_hz_s(void)
{
    if (s_scan_accel_override_hz_s > 0.0f) {
        return s_scan_accel_override_hz_s;
    }
    const app_config_t cfg = app_config_get();
    float a = cfg.accel * MOTION_STEPS_PER_UNIT;
    if (a < 1000.0f) {
        a = 1000.0f;                /* accel 被设成 0/极小时兜底，避免除零式的死循环 */
    }
    return a;
}

/** 本段步数能在"对称梯形"下升到的最高频率（Hz）：v = √(v0² + a·steps) */
static float motion_scan_reach_hz(int steps)
{
    const float v0 = 50.0f;
    const float a = motion_scan_accel_hz_s();
    return sqrtf(v0 * v0 + a * (float)steps);
}

/** 带加减速把 steps 个脉冲发完并等它发完（单块上限 MOTION_MAX_STEPS_PER_TX） */
static void motion_scan_emit(int steps, float freq_hz)
{
    const float a = motion_scan_accel_hz_s();
    const float v_start = 50.0f;                    /* 起步频率：任何机器都能起步 */
    const float v_peak = (freq_hz > v_start) ? freq_hz : v_start;
    float v = v_start;

    while (steps > 0) {
        const int chunk = (steps > MOTION_MAX_STEPS_PER_TX) ? MOTION_MAX_STEPS_PER_TX : steps;
        /* 与运动路径同一个判据：剩余距离内能刹住的最高速度 v ≤ √(2·a·剩余) */
        float v_cap = sqrtf(2.0f * a * (float)steps + 1.0f);
        if (v_cap > v_peak) {
            v_cap = v_peak;
        }
        if (v < v_cap) {
            v += a * ((float)chunk / ((v > 1.0f) ? v : 1.0f));
            if (v > v_cap) {
                v = v_cap;
            }
        } else {
            v -= a * ((float)chunk / ((v > 1.0f) ? v : 1.0f));
            if (v < v_cap) {
                v = v_cap;
            }
        }
        if (v < v_start) {
            v = v_start;
        }

        motion_emit_steps((uint32_t)chunk, v);
        steps -= chunk;
        while (__atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) >= MOTION_TX_QUEUE_TARGET) {
            vTaskDelay(1);
        }
    }
    motion_wait_tx_done();
}

/** 每段开头的核对计数清零 */
static void motion_scan_begin(void)
{
    s_pulse_req = 0;
    s_tx_ok = 0;
    s_tx_fail = 0;
    s_tx_done = 0;
    __atomic_store_n(&s_sym_done, 0, __ATOMIC_SEQ_CST);
}

/** 每段的脉冲核对（与运动路径同一套判据）。
 *  @note bitbang 模式不走 RMT，所以没有符号计数，只报"软件翻转发了多少个脉冲"。 */
static void motion_scan_check(const char *what, int steps)
{
    if (s_scan_bitbang) {
        ESP_LOGW(TAG, "       软件翻转发完 %d 个脉冲（%s；绕开 RMT，跳过符号核对）", steps, what);
        return;
    }
    const int sym_done = __atomic_load_n(&s_sym_done, __ATOMIC_SEQ_CST);
    if (s_tx_ok > 0 && s_tx_ok == s_tx_done && sym_done == s_pulse_req + (int)s_tx_ok) {
        ESP_LOGW(TAG, "       脉冲核对 OK（%s）：请求 %d / 入队 %u / 发完 %u，RMT 报告 %d 个符号",
                 what, s_pulse_req, (unsigned)s_tx_ok, (unsigned)s_tx_done, sym_done);
    } else {
        ESP_LOGE(TAG, "       ★★ 脉冲核对异常（%s）：请求 %d / 入队 %u / 发完 %u / RMT %d 个符号",
                 what, s_pulse_req, (unsigned)s_tx_ok, (unsigned)s_tx_done, sym_done);
    }
}

/*------------------------------------------------------------------------------
 * ★ 对照实验：软件翻转 GPIO 发脉冲（照 Arduino 的做法，绕开 RMT）
 *
 * 目的：现场怀疑"是不是 IO 配置 / RMT 这条输出路径的问题" —— 那就在**同一个引脚**上
 *       用**另一种完全不同的方式**发同一串脉冲，直接对比：
 *         · 两种方式都抖        ⇒ IO / 输出路径彻底排除（问题在驱动器/电流/电机那侧）
 *         · RMT 抖、软件翻转顺  ⇒ 是 RMT 这条路径的问题，回头改它
 *
 * 两个必须注意的点（否则实验本身不成立）：
 *   1) **RMT 占着 IO11**：只要通道还开着，GPIO 矩阵就由 RMT 驱动该脚，
 *      `gpio_set_level()` 会被**忽略**。所以先 rmt_disable + rmt_del_channel，
 *      再用 `gpio_reset_pin()` 把输出选择切回 GPIO 寄存器（关键一步）。
 *   2) 测完必须把通道重建回来（`motion_rmt_init()`），否则界面里的运动会失效
 *      （日志会明确报出来；重启即可恢复）。
 *------------------------------------------------------------------------*/
/** 软件翻转 + 忙等发脉冲（逐脉冲循环，带与运动路径同样的加减速） */
static void motion_bitbang_emit(int steps, float freq_hz)
{
    const float a = motion_scan_accel_hz_s();
    const float v_start = 50.0f;
    const float v_peak = (freq_hz > v_start) ? freq_hz : v_start;
    float v = v_start;

    for (int i = 0; i < steps; i++) {
        /* 与运动路径同一个减速判据：剩下的步数里能刹住的速度 v ≤ √(2·a·剩余) */
        float v_cap = sqrtf(2.0f * a * (float)(steps - i) + 1.0f);
        if (v_cap > v_peak) {
            v_cap = v_peak;
        }
        if (v < v_cap) {
            v += a / ((v > 1.0f) ? v : 1.0f);
            if (v > v_cap) {
                v = v_cap;
            }
        } else {
            v -= a / ((v > 1.0f) ? v : 1.0f);
            if (v < v_cap) {
                v = v_cap;
            }
        }
        if (v < v_start) {
            v = v_start;
        }

        uint32_t period_us = (uint32_t)(1000000.0f / v);
        uint32_t high = s_step_pulse_us;
        if (high == 0) {
            high = period_us / 2;
        }
        if (high > period_us / 2) {
            high = period_us / 2;
        }
        if (high < 1) {
            high = 1;
        }
        if (period_us <= high) {
            period_us = high + 1;
        }

        gpio_set_level((gpio_num_t)MOTION_GPIO_STEP, 1);
        esp_rom_delay_us(high);
        gpio_set_level((gpio_num_t)MOTION_GPIO_STEP, 0);
        esp_rom_delay_us(period_us - high);
    }
    gpio_set_level((gpio_num_t)MOTION_GPIO_STEP, 0);   /* 收尾停在低电平 */
}

#if MOTION_CLI_ON
/** 把 IO11 从 RMT 手里交还给普通 GPIO（否则 gpio_set_level 无效） */
static esp_err_t motion_bitbang_take_step_pin(void)
{
    if (s_rmt_chan != NULL) {
        ESP_RETURN_ON_ERROR(rmt_disable(s_rmt_chan), TAG, "rmt_disable failed");
        ESP_RETURN_ON_ERROR(rmt_del_channel(s_rmt_chan), TAG, "rmt_del_channel failed");
        s_rmt_chan = NULL;
    }
    if (s_rmt_copy_encoder != NULL) {
        ESP_RETURN_ON_ERROR(rmt_del_encoder(s_rmt_copy_encoder), TAG, "rmt_del_encoder failed");
        s_rmt_copy_encoder = NULL;
    }
    /* gpio_reset_pin() 会把该脚恢复成普通 GPIO（内部把输出信号切回 GPIO 寄存器）——
     * 少了这一步，gpio_set_level() 仍然被 GPIO 矩阵忽略，实验会变成"发不出任何脉冲"。 */
    ESP_RETURN_ON_ERROR(gpio_reset_pin((gpio_num_t)MOTION_GPIO_STEP), TAG, "step gpio reset failed");
    const gpio_config_t step_cfg = {
        .pin_bit_mask = 1ULL << MOTION_GPIO_STEP,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&step_cfg), TAG, "step gpio config failed");
    gpio_set_drive_capability((gpio_num_t)MOTION_GPIO_STEP, GPIO_DRIVE_CAP_3);
    gpio_set_level((gpio_num_t)MOTION_GPIO_STEP, 0);
    return ESP_OK;
}
#endif  /* MOTION_CLI_ON */

/** 按当前模式发一段：RMT 硬件，或（bb 模式）软件翻转 */
static void motion_scan_send(int steps, float freq_hz)
{
    if (s_scan_bitbang) {
        motion_bitbang_emit(steps, freq_hz);
    } else {
        motion_scan_emit(steps, freq_hz);
    }
}

/**
 * @brief 按指定「频率 + 脉宽」正反转各跑 steps_per_dir 步（净位移 0）
 *
 * 顺序与运动路径完全一致：DIR → ENA → 稳定 MOTION_ENA_SETTLE_MS → 才发脉冲。
 * 唯一改动的运行期变量是 s_step_pulse_us（脉宽）。
 */
static void motion_scan_run(float freq_hz, uint32_t width_us, int steps_per_dir, int gap_ms)
{
    const uint32_t period_us = (uint32_t)(MOTION_RMT_RESOLUTION_HZ / freq_hz);
    uint32_t hi = (width_us == 0) ? (period_us / 2) : width_us;
    if (hi > period_us / 2) {
        hi = period_us / 2;                 /* 高电平最多占一半：低电平要给光耦复位时间 */
    }
    if (hi < 1) {
        hi = 1;
    }
    const float duty = period_us ? (100.0f * (float)hi / (float)period_us) : 0.0f;

    s_step_pulse_us = width_us;             /* ★ 本段唯一的变量 */
    /* ★ 把发射上限抬到诊断档：本段的目的就是"找上限"，必须能发得比安全上限更高
     *   （否则 mtest 4000 会被静默钳到 1000，量出来的是假上限）。测完立刻还原。
     *   v7 起诊断上限也是 20000（= 安全上限），所以这一步现在只是"保持不变"；
     *   保留它是因为**正常运动路径与诊断路径的发射上限是两套语义**，
     *   以后谁把 MOTION_STEP_FREQ_MAX_HZ 调小，诊断仍不会被误伤。 */
    const float prev_freq_max = s_emit_freq_max_hz;
    s_emit_freq_max_hz = MOTION_SCAN_FREQ_MAX_HZ;

    motion_driver_enable(true);
    motion_gpio_set(MOTION_GPIO_DIR, MOTION_DIR_POSITIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(MOTION_ENA_SETTLE_MS));

    const float reach_hz = motion_scan_reach_hz(steps_per_dir);
    const float accel_now = motion_scan_accel_hz_s();
    ESP_LOGW(TAG, "【试转】目标频率 %.0f Hz（= %.3f mm/s @%.0f 步/mm） 脉宽 %u µs%s"
                  "（高 %u / 周期 %u µs，占空比 %.1f%%） 每方向 %d 步 = %.2f mm"
                  "（★ 带加减速，%s；加速率 %.0f Hz/s%s）",
             freq_hz, freq_hz / MOTION_STEPS_PER_UNIT, MOTION_STEPS_PER_UNIT,
             (unsigned)width_us, (width_us == 0) ? "【自动 = 50% 方波】" : "",
             (unsigned)hi, (unsigned)period_us, duty,
             steps_per_dir, (float)steps_per_dir / MOTION_STEPS_PER_UNIT,
             s_scan_bitbang ? "★ 软件翻转 GPIO，绕开 RMT" : "RMT 硬件",
             accel_now,
             (s_scan_accel_override_hz_s > 0.0f) ? " = 命令行指定"
                                                 : " = accel×步数每毫米（与真实运动一致）");
    /* 诊断也有上限（MOTION_SCAN_FREQ_MAX_HZ）：超了就明确报出来，
     * 不能让"实际只发了 20000Hz"和"我设的是 32000Hz"混在一起（静默钳制是前几轮
     * 反复踩过的坑）。注意：**起跳频率下限（MOTION_TURN_FREQ_MIN_HZ）在诊断期
     * 故意不生效** —— 诊断就是要从 2000Hz 这种低频段一路扫上去，看"从哪一档开始转"。 */
    if (freq_hz > MOTION_SCAN_FREQ_MAX_HZ) {
        ESP_LOGW(TAG, "       ★ 目标 %.0f Hz 超过诊断上限 %.0f Hz，实际只会发到 %.0f Hz",
                 freq_hz, MOTION_SCAN_FREQ_MAX_HZ, MOTION_SCAN_FREQ_MAX_HZ);
    }
    /* ★ 每段都把两根控制线的**实测电平**打出来：
     *   万一状态机在别处把 ENA 放开（脱机 = 驱动器输出关断 = 无论怎么发脉冲都不会转），
     *   这一行会立刻暴露 —— 只看 `【试转】` 那一行是看不出来的。 */
    ESP_LOGW(TAG, "       ENA=IO%d 实测=%d（0=使能） DIR=IO%d 实测=%d（%d=正方向）；"
                  "本段步数在加减速下最多升到约 %.0f Hz%s",
             MOTION_GPIO_ENA, gpio_get_level(MOTION_GPIO_ENA),
             MOTION_GPIO_DIR, gpio_get_level(MOTION_GPIO_DIR), MOTION_DIR_POSITIVE_LEVEL,
             reach_hz, (reach_hz < freq_hz) ? "（★ 低于目标：把第三个参数【每方向步数】加大）" : "");

    motion_scan_begin();
    motion_scan_send(steps_per_dir, freq_hz);
    motion_scan_check("正转", steps_per_dir);

    motion_scan_begin();
    motion_gpio_set(MOTION_GPIO_DIR, !MOTION_DIR_POSITIVE_LEVEL);
    motion_scan_send(steps_per_dir, freq_hz);
    motion_scan_check("反转", steps_per_dir);

    motion_gpio_set(MOTION_GPIO_DIR, MOTION_DIR_POSITIVE_LEVEL);
    if (gap_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }

    /* 还原发射上限：之后界面里的真实运动仍受 MOTION_STEP_FREQ_MAX_HZ 保护 */
    s_emit_freq_max_hz = prev_freq_max;
}

/** 自动扫描：6 频率 × 4 脉宽 = 24 段（同一频率下只有脉宽不同，便于现场对比） */
static void motion_scan_sweep(void)
{
    const int seg_total = MOTION_SCAN_FREQ_NUM * MOTION_SCAN_PULSE_NUM;

    ESP_LOGW(TAG, "=== 参数扫描开始：%d 频率（%.0f~%.0fHz）× %d 脉宽 = %d 段"
                  "（每段正反各 %d 步 = %.2f mm，净位移 0）★ 电机来回动 ===",
             MOTION_SCAN_FREQ_NUM, s_scan_freq[0], s_scan_freq[MOTION_SCAN_FREQ_NUM - 1],
             MOTION_SCAN_PULSE_NUM, seg_total,
             MOTION_SCAN_SWEEP_STEPS,
             (float)MOTION_SCAN_SWEEP_STEPS / MOTION_STEPS_PER_UNIT);
    ESP_LOGW(TAG, "★ 判读（只看不用量）："
                  "① **从哪一档开始转**（低频档只嗡嗡、到某一档才转）"
                  " ⇒ 那就是这台机器的**起跳频率**（本机现场约 8500Hz @1600 脉冲/圈，"
                  "见 stepper_motor 例程的上电固定段），按 README 9.12 把"
                  " MOTION_TURN_FREQ_MIN_HZ 与出厂 speed 设到它之上；"
                  "② **哪一档开始抖**（转得动但发毛/丢步）⇒ 那才是速度上限，"
                  "把 MOTION_STEP_FREQ_MAX_HZ 收到它下面；"
                  "③ 同一频率下 4 段只有脉宽不同 —— 靠后（脉宽大）的更顺 ⇒ 根因是脉宽太窄"
                  "（STEP 高电平脉宽改成 50 或 0）；"
                  "④ 从第一档(%.0fHz)到最后一档**全都只嗡嗡不转** ⇒ 与频率无关，"
                  "查拨码/相线/共地/机械（README 9.9，拨码以驱动器外壳印字为准）。",
             s_scan_freq[0]);

    for (int f = 0; f < MOTION_SCAN_FREQ_NUM; f++) {
        ESP_LOGW(TAG, "--- 频率 %.0f Hz（= %.3f mm/s）---",
                 s_scan_freq[f], s_scan_freq[f] / MOTION_STEPS_PER_UNIT);
        for (int p = 0; p < MOTION_SCAN_PULSE_NUM; p++) {
            motion_scan_run(s_scan_freq[f], s_scan_pulse[p],
                            MOTION_SCAN_SWEEP_STEPS, MOTION_SCAN_GAP_MS);
        }
    }

    s_step_pulse_us = MOTION_STEP_PULSE_US;     /* 恢复默认，普通运动用 Kconfig 的值 */
    ESP_LOGW(TAG, "=== 参数扫描结束（净位移 0；脉宽已恢复默认 %u µs）"
                  "把【第几段顺、第几段抖】告诉开发即可 ===",
             (unsigned)MOTION_STEP_PULSE_US);
}
#endif  /* (MOTION_SELF_TEST_ON_BOOT || MOTION_CLI_ON) && !MOTION_SIMULATE */

#if MOTION_SELF_TEST_ON_BOOT && !MOTION_SIMULATE
static void motion_self_test(void)
{
    motion_scan_sweep();
}
#endif

#if MOTION_CLI_ON && !MOTION_SIMULATE
/*------------------------------------------------------------------------------
 * 串口命令行 mtest
 *----------------------------------------------------------------------------*/
/** 命令行当前的频率（脉宽直接用运行期的 s_step_pulse_us）
 *  ★ 默认 **12000Hz**（= 15mm/s @800 步/mm）：**现场指定的默认值**，这样
 *    `f` / `run` 不带参数就能和台架（`examples/peripherals/rmt/stepper_motor`）
 *    的 `f 12000` + `run` 用**完全相同的参数**做对照。
 *  ★★ 现场结论（v13）：**`f 12000` + `run`（单向连续）转得很好** ★★
 *    这就是**界面的工作点**（出厂 `speed = 15.000mm/s = 12000Hz`，见 app_config.c
 *    的 CFG_DEF_SPEED）。反过来说：**8500Hz 只是"能转"的下沿**（临界点）——
 *    台架空载能过，界面**带载**跑就"启动后左右转、跟震动一样"（现场实测）。
 *    ⇒ 不要把界面 Speed 调到 8500Hz 附近；`f 8500` 只用来**确认下沿**还在哪。
 *  `f <hz>` 可在运行中改，下一批脉冲生效（与台架一致）。 */
static float s_cli_freq = 12000.0f;

/*------------------------------------------------------------------------------
 * ★★ `run` / `stop`：单向连续运行（v16，照 stepper_motor 台架那三条命令做的）★★
 *
 * 台架的 `f <hz>` → `run` → `stop` 是现场用起来最省事的判据：
 * **单向**连续发脉冲，不来回、不经过曲线/状态机，只看"这一段能不能连续转"。
 * 本工程原来只有 `mtest`（正反各一趟，净位移 0）和 `mtest hold`（固定时长），
 * 缺"跑起来就不停、由我喊停"这一条 —— 现场要求在**本工程自己的发射路径**上对照。
 *
 * ★ 与 `mtest hold` 的关键差别（也是必须单独做一条命令的原因）：
 *   `hold` 在**命令行任务里**跑完整段才回到提示符，所以跑起来之后**敲不了 stop**；
 *   `run` 把发射交给**独立任务**（优先级 = motion_task），命令行立刻回到提示符 ⇒
 *   `stop` 随时可用、`f <hz>` 也能**运行中改频率**（下一批脉冲生效）。
 *
 * ★ 方向**固定 = 正向**（MOTION_DIR_POSITIVE_LEVEL），全程不换向 —— 所以
 *   "左右摆/来回转"就一定是**电机失步**，不可能再被误判成软件在换向。
 *----------------------------------------------------------------------------*/
#if MOTION_CLI_ON && !MOTION_SIMULATE
static TaskHandle_t s_run_task = NULL;

/** 连续运行任务：被 `run` 拉起，被 `stop` 叫停（只在诊断命令行里用） */
static void cli_run_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_run_active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* --- 起转：DIR → ENA →（mode 0 才等稳定时间）→ 发脉冲 ---
         * ★ mode 1（台架同款）**不等**：台架 `run` 就是 `set_ena()` 之后立刻 rmt_transmit，
         *   没有 100ms 的等待。要逐项同构就必须连这个也一致（现场"起转手感不一样"
         *   有一部分就是这 100ms）。mode 0 保持本工程原来的 100ms。 */
        motion_gpio_set(MOTION_GPIO_DIR, MOTION_DIR_POSITIVE_LEVEL);
        motion_driver_enable(true);
        if (s_emit_bench) {
            vTaskDelay(pdMS_TO_TICKS(2));   /* 台架同款：几乎不等待（只留 2ms 让 ENA 建立） */
        } else {
            vTaskDelay(pdMS_TO_TICKS(MOTION_ENA_SETTLE_MS));
        }

        s_pulse_req = 0;
        s_tx_ok = 0;
        s_tx_fail = 0;
        s_tx_done = 0;
        __atomic_store_n(&s_sym_done, 0, __ATOMIC_SEQ_CST);

        const int64_t t0 = esp_timer_get_time();
        uint32_t sent = 0;
        ESP_LOGW(TAG, "=== 连续运行开始：%.0f Hz（**单向**，串口敲 stop 停止）"
                      "  脉宽 %u µs%s  发射方式 mode=%d（%s）===",
                 s_run_hz, (unsigned)s_step_pulse_us,
                 (s_step_pulse_us == 0) ? "（0 = 50% 方波）" : "",
                 s_emit_bench,
                 s_emit_bench ? "台架同款波形+节奏(64/批,等发完)"
                              : "本工程原样(队列预填,无缝)");

        while (s_run_active) {
            float f = s_run_hz;
            if (!(f >= MOTION_STEP_FREQ_MIN_HZ)) {
                f = MOTION_STEP_FREQ_MIN_HZ;
            }
            if (f > MOTION_SCAN_FREQ_MAX_HZ) {
                f = MOTION_SCAN_FREQ_MAX_HZ;
            }
            if (s_emit_bench) {
                /* ★★ 台架同款**批次节奏**（与 stepper_motor 的 pulse_task 逐行同构）★★
                 *   台架：`pulses = min(freq × BATCH_MS(50) / 1000, MAX_PULSES_PER_TX(64))`
                 *         → 然后 `rmt_transmit(..., {.loop_count = pulses})`
                 *         → **`rmt_tx_wait_all_done(chan, -1)` 等这一批发完**，再发下一批。
                 *   ⇒ 批与批之间 RMT 是**空闲**的（STEP 停在低电平一小段），
                 *     也就是"每 64 个脉冲一个固定空档"。这正是现场听到的
                 *     "都在转，但感觉不一样"的那部分（本工程原来是队列预填、无缝连续）。 */
                uint32_t pulses = (uint32_t)((uint64_t)(uint32_t)(f + 0.5f) * 50u / 1000u);
                if (pulses == 0) {
                    pulses = 1;
                }
                if (pulses > MOTION_MAX_STEPS_PER_TX) {
                    pulses = MOTION_MAX_STEPS_PER_TX;
                }
                motion_emit_steps(pulses, f);
                sent += pulses;
                motion_wait_tx_done();      /* = 台架的 rmt_tx_wait_all_done(-1) */
            } else {
                motion_emit_steps(MOTION_MAX_STEPS_PER_TX, f);
                sent += MOTION_MAX_STEPS_PER_TX;
                /* 队列深度判满（不依赖 tick 长度）：见 motion_fixed_run 的说明 */
                while (__atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) >= MOTION_TX_QUEUE_TARGET) {
                    vTaskDelay(1);
                }
            }
        }

        /* --- 收尾：等最后一个事务发完（低频率下一个事务可能几百 ms） --- */
        const int64_t deadline_ms = esp_timer_get_time() / 1000 + 5000;
        while (__atomic_load_n(&s_tx_pending, __ATOMIC_SEQ_CST) > 0 &&
               esp_timer_get_time() / 1000 < deadline_ms) {
            vTaskDelay(1);
        }
        motion_driver_enable(false);        /* 脱机：轴可手盘，也没有保持电流的嗡嗡声 */

        const float el_s = (float)(esp_timer_get_time() - t0) / 1000000.0f;
        const int sym_done = __atomic_load_n(&s_sym_done, __ATOMIC_SEQ_CST);
        ESP_LOGW(TAG, "=== 连续运行结束：%.2f s 内提交 %" PRIu32 " 个脉冲"
                      "（平均 %.0f Hz，目标 %.0f Hz）；轴已脱机，可手盘 ===",
                 el_s, sent, (el_s > 0.0f) ? ((float)sent / el_s) : 0.0f, s_run_hz);
        /* 与运动路径同一判据：RMT 符号数 = 请求脉冲数 + 完成事务数 ⇒ 每个脉冲都出了引脚。
         * ★ 平均频率明显低于目标 = 补队列被拖住过（脉冲串有断档），那才是"卡/顿"的软件原因。 */
        if (s_tx_ok > 0 && s_tx_ok == s_tx_done && sym_done == s_pulse_req + (int)s_tx_ok) {
            ESP_LOGW(TAG, "  脉冲核对 OK：请求 %d / 入队 %u / 发完 %u，RMT 报告 %d 个符号",
                     s_pulse_req, (unsigned)s_tx_ok, (unsigned)s_tx_done, sym_done);
        } else {
            ESP_LOGE(TAG, "  ★★ 脉冲核对异常：请求 %d / 入队 %u / 发完 %u，RMT %d 个符号（期望 %d）",
                     s_pulse_req, (unsigned)s_tx_ok, (unsigned)s_tx_done, sym_done,
                     s_pulse_req + (int)s_tx_ok);
        }
    }
}
#endif  /* MOTION_CLI_ON && !MOTION_SIMULATE */

static void cli_print_settings(void)
{
    const uint32_t period_us = (uint32_t)(MOTION_RMT_RESOLUTION_HZ / s_cli_freq);
    const uint32_t hi = (s_step_pulse_us == 0) ? (period_us / 2) : s_step_pulse_us;
    printf("当前设置：频率 %.0f Hz（= %.3f mm/s） 脉宽 %u µs%s（高 %u / 周期 %u µs）\n",
           s_cli_freq, s_cli_freq / MOTION_STEPS_PER_UNIT,
           (unsigned)s_step_pulse_us, (s_step_pulse_us == 0) ? " = 自动50%" : "",
           (unsigned)hi, (unsigned)period_us);
    printf("          起跳频率 %.0f Hz（低于它只嗡嗡不转），软启动起步 %.0f Hz（0=关），诊断上限 %.0f Hz\n",
           MOTION_TURN_FREQ_MIN_HZ, s_soft_start_hz, MOTION_SCAN_FREQ_MAX_HZ);
    printf("          每方向默认 %d 步（%.2f mm）；用法：mtest [频率] [脉宽µs] [步数] | sweep | def | ?\n",
           MOTION_SCAN_STEPS_PER_DIR,
           (float)MOTION_SCAN_STEPS_PER_DIR / MOTION_STEPS_PER_UNIT);
    printf("          单向连续：f <hz> 设频率 / run 开始（跑不停）/ stop 停止%s\n",
           s_run_active ? "  ← ★ 现在正在连续运行中！" : "");
    printf("          发射方式 mode = %d（%s）；`mtest mode 0|1` 切换\n", s_emit_bench,
           s_emit_bench ? "台架同款 uniform+loop_count" : "本工程原样 符号数组");
}

static void cli_print_help(void)
{
    printf("\n=== mtest —— 手动换挡试转（判断哪一档不卡/不抖）【别名 mt / m 等价】===\n");
    printf("  mtest                  用当前设置跑一次（正转再反转，净位移 0）\n");
    printf("  mtest 12000            设频率 12000Hz 再跑（= 15mm/s @800 步/mm，当前默认值）\n");
    printf("  mtest 8500             设频率 8500Hz 再跑（= 10.625mm/s，**下沿/临界**那档：\n");
    printf("                         空载能过，带载界面跑会发毛 —— 只用来确认下沿在哪）\n");
    printf("  mtest 12000 50         再设脉宽 50µs（5 = 默认 / 0 = 自动 50%% 方波）\n");
    printf("  mtest 12000 50 3200    第三个参数 = 每方向步数（3200 步 = 4mm；默认 %d）\n",
           MOTION_SCAN_STEPS_PER_DIR);
    printf("  mtest 12000 5 3200 6000 第四个参数 = 本段加速率 Hz/s（隔离【加速太陡】这个变量）\n");
    printf("                         出厂加速率 150mm/s² x 800 = 120000 Hz/s；再温和可试 6000 或 3000\n");
    printf("  mtest sweep            自动跑 %d 频率 × %d 脉宽 = %d 段（%.0f~%.0fHz，骑在起跳频率两侧）\n",
           MOTION_SCAN_FREQ_NUM, MOTION_SCAN_PULSE_NUM, MOTION_SCAN_FREQ_NUM * MOTION_SCAN_PULSE_NUM,
           s_scan_freq[0], s_scan_freq[MOTION_SCAN_FREQ_NUM - 1]);
    printf("  ★★ 单向连续（与 stepper_motor 台架的 f/run/stop 完全对应）★★\n");
    printf("  f [hz]                 设连续运行的频率（不给参数 = 打印当前 %.0fHz）；\n", s_cli_freq);
    printf("                         **运行中也有效**，下一批脉冲即生效\n");
    printf("  run                    ★ 开始**单向连续发脉冲**（不来回、不经过曲线/状态机），\n");
    printf("                         一直转到 `stop`；期间界面下发的运动会被忽略\n");
    printf("  stop                   ★ 停止连续运行（并等脉冲发完 + 脱机）；\n");
    printf("                         没在跑时等价于「把界面运动也停掉」\n");
    printf("  mode [0|1]             ★ **发射方式**：0（默认）= 本工程原样（已验证能转）；\n");
    printf("                         1 = 台架同款波形 + 台架批次节奏（64 个/批、等发完再发下一批）。\n");
    printf("                         两种都是「n 个符号 + loop_count=0」；改完下一次发射即生效\n");
    printf("                         ★ 不能像台架那样用 loop_count：驱动会关掉 TX-done 中断 → 停发\n");
    printf("  ★ 用法（现场三条命令）：`f 12000` → `run` → 看/听 → `stop`\n");
    printf("  ★ 本机现场：**%.0f Hz 以下只嗡嗡不转（低频禁区）**；\n", MOTION_TURN_FREQ_MIN_HZ);
    printf("     而 **12000Hz 连续跑得很好**（= 界面的工作点，出厂 speed 15.000mm/s）；\n");
    printf("     8500Hz 只是**下沿/临界点**（空载能过、带载发毛）—— 界面按它就是【启动后左右转】。\n");
    printf("     要往上探就先 `f 14000` / `f 16000` 一格一格看从哪档开始发毛。\n");
    printf("  mtest move [mm]        ★★ **走一段真实运动**（与界面同一入口 app_motion_move_to），\n");
    printf("                         不经过状态机；期间每 0.5s 打印 位置/速度/ENA/DIR 实测电平\n");
    printf("  mtest ss [hz]          ★ **软启动起步频率**（0 = 关闭，直接从巡航频率 12000Hz 起跳）。\n");
    printf("                         当前 %.0f Hz。起转卡壳/左右抖动就调它：`mtest ss 0` 关掉、\n",
           s_soft_start_hz);
    printf("                         或试 2000/4000/6000 找抖动最小的档；改完 `mtest move 10` 看效果\n");
    printf("  mtest hold [hz] [ms]   ★ **固定频率连发 N 毫秒**（默认 %.0fHz × %u ms）—— 就是\n",
           MOTION_BOOT_HOLD_HZ, (unsigned)MOTION_BOOT_HOLD_MS);
    printf("                         stepper_motor 例程的上电段；跑完自动停 + 脱机。真走轴，注意行程\n");
    printf("  mtest def              脉宽恢复出厂默认（%u µs）\n", (unsigned)MOTION_STEP_PULSE_US);
    printf("  mtest bb [频率] [脉宽] [步数]   ★ 对照实验：软件翻转 IO11 发脉冲（绕开 RMT，照 Arduino 的做法）\n");
    printf("  help                   列出所有命令\n");
    printf("★ mtest bb 与 mtest 的对比结论：两种都抖 ⇒ IO/输出路径彻底排除；只有 RMT 抖 ⇒ 是 RMT 路径的问题。\n");
    printf("★ 判读：同一频率下换成更宽的脉宽就不抖 ⇒ 根因是脉宽；怎么换都抖 ⇒ 与软件无关，\n");
    printf("  查拨码电流(SW4=ON 才是 2.0A)/相线分组/共地（README 9.9）；高频才抖 ⇒ 降 Speed（别低于起跳频率）。\n");
    printf("★ 诊断段**带加减速**（起步 50Hz 升到目标频率，末尾按剩余距离减速）——这是为了找出低频禁区；\n");
    printf("  真实运动的曲线**整段恒定频率**（出厂 12000Hz，起步=巡航=收尾，与串口 `run` 同构）：\n");
    printf("  **不做收尾减速**（减速会掉进 8500Hz 临界带 ⇒ 现场就是【正转后反转】），到位即停；\n");
    printf("  日志看 `曲线：起步 … → 巡航 … → 收尾 …` —— 三个数必须**相同**。\n");
    printf("  第三个参数（每方向步数）越大，本段能达到的频率越高 —— 日志会提示【最多升到约 X Hz】。\n");
    printf("★ 提示：跑完 mtest 后，界面里的运动也会用你最后设的脉宽，重启恢复默认。\n\n");
}

static int cli_mtest(int argc, char **argv)
{
    if (argc >= 2 && (strcmp(argv[1], "?") == 0 || strcmp(argv[1], "help") == 0)) {
        cli_print_help();
        cli_print_settings();
        return 0;
    }

    /*--------------------------------------------------------------------------
     * ★★ v16：`stop` / `f` / `run` —— 与 stepper_motor 台架的三条命令一一对应 ★★
     *    `f <hz>` → `run` → `stop`：**单向**连续发脉冲，不来回、不经曲线/状态机。
     *    这三条必须排在最前面：`stop` 任何时候都要能用（连续运行中其它命令会被拒绝），
     *    `f` 在运行中也要能改（现场就是靠这两条收尾/换挡）。
     *------------------------------------------------------------------------*/

    /* `mtest stop`：停连续运行；没在跑时等价于"把界面运动也停掉"（= 界面 STOP 键） */
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        const bool was_running = s_run_active;
        s_run_active = false;       /* 连续运行任务自己收尾（等脉冲发完 + 核对 + 脱机） */
        app_motion_stop();          /* 顺带把界面下发的运动停掉 */
        printf("★ 已停止%s\n",
               was_running ? "连续运行：等它把在途脉冲发完就脱机，日志会打【连续运行结束】+ 脉冲核对"
                           : "（当时没有连续运行；界面下发的运动也已停止）");
        cli_print_settings();
        return 0;
    }

    /* `mtest f [hz]`：设连续运行的频率。**运行中也有效**（下一批脉冲生效，与台架一致） */
    if (argc >= 2 && strcmp(argv[1], "f") == 0) {
        if (argc >= 3) {
            float hz = strtof(argv[2], NULL);
            if (!(hz >= MOTION_SCAN_FREQ_MIN)) {
                hz = MOTION_SCAN_FREQ_MIN;
            }
            if (hz > MOTION_SCAN_FREQ_MAX) {
                hz = MOTION_SCAN_FREQ_MAX;
            }
            s_cli_freq = hz;
            s_run_hz   = hz;
        }
        printf("频率 = %.0f Hz（= %.3f mm/s @%.0f 步/mm）%s\n", s_cli_freq,
               s_cli_freq / MOTION_STEPS_PER_UNIT, MOTION_STEPS_PER_UNIT,
               s_run_active ? "  ← 正在连续运行：下一批脉冲即生效" : "");
        cli_print_settings();
        return 0;
    }

    /* `mtest mode [0|1]`：切**发射方式**（1 = 台架同款，默认；0 = 本工程原样）
     *   现场"同频率转的不一样"就是靠这个开关排除/确认发射链路差异的。 */
    if (argc >= 2 && strcmp(argv[1], "mode") == 0) {
        if (argc >= 3) {
            s_emit_bench = (atoi(argv[2]) != 0) ? 1 : 0;
        }
        printf("发射方式 = %d（%s）\n", s_emit_bench,
               s_emit_bench
                   ? "**台架同款**：uniform 编码器 + loop_count（硬件重复同一符号），与 stepper_motor 逐行同构"
                   : "本工程原样：自己摆 n 个「先高后低」符号 + loop_count=0");
        printf("  ★ 用法：`mtest mode 1` / `mtest mode 0`；改完**下一次发射立即生效**\n"
               "  ★ A/B 方法：同一个 `f <hz>`，两种 mode 各 `run` 一次，看哪种能连续转\n");
        cli_print_settings();
        return 0;
    }

    /* 连续运行中：除 `f` / `stop` 外的命令一律先拒绝
     * （两处同时往 RMT 灌脉冲 / 同时改 DIR-ENA，现象会变得不可复现） */
    if (s_run_active) {
        printf("★ 连续运行（run）进行中：先敲 stop，再跑别的命令\n");
        return 1;
    }

    /* ★★ `mtest move [绝对位置mm] [绝对位置…]`：**走真实运动**（v13）★★
     *
     * 它调的就是界面按钮用的那个入口 `app_motion_move_to()` ⇒ 完全同一条路：
     * motion_task → motion_begin（DIR/ENA/稳定时间）→ 梯形曲线 → 攒批发射。
     * 唯一差别是**不经过状态机**（界面那套 State1/2/3 与校准流程）。
     *
     * 为什么要有它（现场"mtest hold 能转、界面运动不转"）：
     *   它把"发射链路"与"状态机/ENA 归属"分开 ——
     *     · `mtest move` 能转 ⇒ 曲线+攒批发射没问题，问题在界面/状态机那侧（例如没标定、
     *       输入被当成"标定刻度盘读数"而不是"目标位置"、ENA 被放掉）；
     *     · `mtest move` 也不转 ⇒ 就是发射链路（曲线/攒批）的问题。
     *   ★ 运动期间每 500ms 打印一次**位置/速度/ENA/DIR 实测电平** ——
     *     `ENA(IO13)=1` 就意味着"驱动器被脱机"，脉冲发得再对也不会转。 */
    if (argc >= 2 && (strcmp(argv[1], "move") == 0 || strcmp(argv[1], "go") == 0)) {
        const float from = app_motion_get_position();
        /* 没给参数就往前走 10mm（方向取"往里"）：方便现场一条命令就能看 */
        float target = (argc >= 3) ? strtof(argv[2], NULL) : (from + 10.0f);
        printf("★ 真实运动（与界面同一入口 app_motion_move_to）：%.3f -> %.3f mm\n", from, target);
        printf("  说明：本命令**不经过状态机**，所以不需要先标定；\n"
               "        但它不会替你把轴「标定」—— 界面上仍需按流程输入刻度盘读数。\n");
        esp_err_t err = app_motion_move_to(target);
        if (err != ESP_OK) {
            printf("★ 下发失败：%s\n", esp_err_to_name(err));
            return 1;
        }
        int still = 0;
        float last = from;
        for (int i = 0; i < 60; i++) {              /* 最多观察 30s */
            vTaskDelay(pdMS_TO_TICKS(500));
            const float pos = app_motion_get_position();
            const float vel = app_motion_get_velocity();
            printf("  [%2ds] 位置 %.3f mm  速度 %.3f mm/s  ENA(IO%d)=%d(0=使能)  DIR(IO%d)=%d\n",
                   (i + 1) / 2, pos, vel,
                   MOTION_GPIO_ENA, gpio_get_level((gpio_num_t)MOTION_GPIO_ENA),
                   MOTION_GPIO_DIR, gpio_get_level((gpio_num_t)MOTION_GPIO_DIR));
            if (fabsf(pos - last) < 0.001f) {
                if (++still >= 3) {
                    break;                          /* 连续 1.5s 没动 → 认为结束 */
                }
            } else {
                still = 0;
            }
            last = pos;
        }
        const float end = app_motion_get_position();
        printf("★ 结束：位置 %.3f mm（目标 %.3f，差 %.3f mm）\n", end, target, end - target);
        printf("  判读：位置**完全没动** 且 ENA=1 ⇒ 驱动器被脱机（状态机/报警把 ENA 放掉了）；\n"
               "        位置没动但 ENA=0 ⇒ 脉冲没出/曲线没升上去，看上一条【本段实际发射频率】；\n"
               "        位置动了但轴不转 ⇒ 驱动器/接线（与 mtest hold 的差别只剩发射节奏）。\n");
        return 0;
    }
    if (s_moving) {
        printf("★ 界面正在运动（或正在收尾）：先按 STOP / 等它到位，再跑 mtest\n");
        return 1;
    }

    /* ★★ `mtest run [hz]`（v16）：**单向连续运行，直到 stop** ★★
     *   与 stepper_motor 台架的 `run` 同义（那边不带参数 = RUN_CONT 连续运行）。
     *   发脉冲放在**独立任务**里（优先级 = motion_task），所以命令行立刻回到提示符：
     *   `stop` 随时能停、`f <hz>` 运行中能改频率。 */
    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        if (argc >= 3) {                    /* `run <hz>`：一步到位，不用先敲 f */
            float hz = strtof(argv[2], NULL);
            if (!(hz >= MOTION_SCAN_FREQ_MIN)) {
                hz = MOTION_SCAN_FREQ_MIN;
            }
            if (hz > MOTION_SCAN_FREQ_MAX) {
                hz = MOTION_SCAN_FREQ_MAX;
            }
            s_cli_freq = hz;
        }
        s_run_hz = s_cli_freq;
        if (s_run_task == NULL) {
            if (xTaskCreate(cli_run_task, "mtest_run", 4096, NULL, MOTION_TASK_PRIO,
                            &s_run_task) != pdPASS) {
                s_run_task = NULL;
                printf("★ 创建连续运行任务失败（内存不足）\n");
                return 1;
            }
        }
        s_run_active = true;                /* 任务下一轮就起转 */
        printf("★ 单向连续运行开始：%.0f Hz（= %.3f mm/s @%.0f 步/mm），串口敲 **stop** 停止\n",
               s_cli_freq, s_cli_freq / MOTION_STEPS_PER_UNIT, MOTION_STEPS_PER_UNIT);
        printf("  ★ 全程**只朝一个方向**（DIR 固定，不会换向）⇒ 若看到/听到**来回摆**，\n"
               "    那是电机在这个频率上**失步**，不是软件在换向；把 f 降到能连续转的档再看。\n");
        printf("  ★ 真走轴、不设长度上限（转到你敲 stop）：先确认这个方向行程够！\n");
        printf("     想按台架那条命令照抄，就是：`f 12000` → `run` → 看/听 → `stop`\n");
        cli_print_settings();
        return 0;
    }

    /* ★★ `mtest ss [hz]`：运行期改**软启动起步频率**（v14）★★
     * 0 或 ≥起跳频率(8500) = 关闭软启动（曲线直接从 8500Hz 起跳）；
     * 500~8000 = 从该频率按 accel 爬升到 8500Hz。
     * 实测背景：从 1100Hz 一级一级爬到 8500Hz 时，轴会在带不动的频段里
     * "卡壳/左右抖动，然后才转" ⇒ 这一项就是要现场一格一格试出最顺的起步频率。 */
    if (argc >= 2 && strcmp(argv[1], "ss") == 0) {
        if (argc >= 3) {
            float hz = strtof(argv[2], NULL);
            if (!(hz >= 0.0f)) {
                hz = 0.0f;
            }
            if (hz > MOTION_SCAN_FREQ_MAX_HZ) {
                hz = MOTION_SCAN_FREQ_MAX_HZ;
            }
            s_soft_start_hz = hz;
        }
        printf("软启动起步频率 = %.0f Hz%s\n", s_soft_start_hz,
               (s_soft_start_hz < MOTION_STEP_FREQ_MIN_HZ ||
                s_soft_start_hz >= MOTION_TURN_FREQ_MIN_HZ)
                   ? "（= 关闭：曲线直接从起跳频率起跳，没有低频爬升段）"
                   : "");
        printf("  用法：mtest ss 0 | 2000 | 4000 | 6000 | 8000   （0 或 ≥%.0f 都是关闭）\n",
               MOTION_TURN_FREQ_MIN_HZ);
        cli_print_settings();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "def") == 0) {
        s_step_pulse_us = MOTION_STEP_PULSE_US;
        printf("脉宽已恢复出厂默认 %u µs\n", (unsigned)MOTION_STEP_PULSE_US);
        cli_print_settings();
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "sweep") == 0 || strcmp(argv[1], "s") == 0)) {
        motion_scan_sweep();
        cli_print_settings();
        return 0;
    }
    /* ★ `mtest hold [频率] [时长ms]`：**固定频率连发 N 毫秒** —— 就是 stepper_motor 例程
     *   上电那一段（默认 8500Hz × 10000ms）。不改频率、不来回摆，纯粹"看这一段能不能连续转"，
     *   用来和例程做**逐项同构**的对照（脉冲走本工程自己的发射路径）。 */
    if (argc >= 2 && strcmp(argv[1], "hold") == 0) {
        float hz = (argc >= 3) ? strtof(argv[2], NULL) : MOTION_BOOT_HOLD_HZ;
        uint32_t ms = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : MOTION_BOOT_HOLD_MS;
        if (ms == 0) {
            ms = MOTION_BOOT_HOLD_MS;
        }
        motion_fixed_run(hz, ms, true);
        cli_print_settings();
        return 0;
    }

    /* ★ 对照实验：`mtest bb [...]` = 用**软件翻转 GPIO**发脉冲（绕开 RMT），
     *   与 `mtest [...]`（RMT 硬件）对比，用来判定"是不是 IO 配置 / RMT 这条路径的问题"。 */
    bool use_bb = false;
    if (argc >= 2 && (strcmp(argv[1], "bb") == 0 || strcmp(argv[1], "bitbang") == 0)) {
        use_bb = true;
        argv++;                     /* 参数整体前移一位，后面照常解析 */
        argc--;
    }

    float freq = s_cli_freq;
    uint32_t width = s_step_pulse_us;
    int steps = MOTION_SCAN_STEPS_PER_DIR;
    if (argc >= 2) {
        freq = strtof(argv[1], NULL);
    }
    if (argc >= 3) {
        width = (uint32_t)strtoul(argv[2], NULL, 10);
    }
    if (argc >= 4) {
        steps = (int)strtol(argv[3], NULL, 10);
    }
    /* 第 4 个参数（可选）：本段加速率 Hz/s —— 用来隔离"加速太陡"这个变量。
     * 不给 = 用真实运动的加速度（accel×步数每毫米，v7 出厂 = 150×800 = 120000 Hz/s）。 */
    float accel_hz = 0.0f;
    if (argc >= 5) {
        accel_hz = strtof(argv[4], NULL);
        if (accel_hz > 0.0f) {
            if (accel_hz < 500.0f) {
                accel_hz = 500.0f;
            }
            if (accel_hz > 2000000.0f) {
                accel_hz = 2000000.0f;
            }
        }
    }
    s_scan_accel_override_hz_s = accel_hz;      /* 0 = 默认（真实运动那个） */
    /* 参数兜底：不合法的值一律夹到合法区间，绝不因为手滑把轴开到底 */
    if (!(freq >= MOTION_SCAN_FREQ_MIN)) {
        freq = MOTION_SCAN_FREQ_MIN;
    }
    if (freq > MOTION_SCAN_FREQ_MAX) {
        freq = MOTION_SCAN_FREQ_MAX;
    }
    if (width > 1000) {
        width = 1000;
    }
    if (steps < 1) {
        steps = 1;
    }
    if (steps > MOTION_SCAN_STEPS_MAX) {
        steps = MOTION_SCAN_STEPS_MAX;
    }
    s_cli_freq = freq;

    if (use_bb) {
        /* 1) 把 IO11 从 RMT 手里抢回来（否则 gpio_set_level 会被 GPIO 矩阵忽略） */
        if (motion_bitbang_take_step_pin() != ESP_OK) {
            printf("★ 抢占 IO%d 失败（看上面日志），本次未发脉冲\n", MOTION_GPIO_STEP);
            return 1;
        }
        s_scan_bitbang = true;
        motion_scan_run(freq, width, steps, 0);
        s_scan_bitbang = false;
        /* 2) 交还给 RMT —— 失败只是"界面运动暂时不可用"，重启即可恢复，必须明确报出来 */
        if (motion_rmt_init() != ESP_OK) {
            ESP_LOGE(TAG, "★★ 恢复 RMT 失败：界面里的运动暂时不可用（重启即可恢复）");
        } else {
            ESP_LOGW(TAG, "已把 IO%d 交还给 RMT，界面运动恢复正常", MOTION_GPIO_STEP);
        }
    } else {
        motion_scan_run(freq, width, steps, 0);
    }
    cli_print_settings();
    return 0;
}

/*------------------------------------------------------------------------------
 * ★ v16：把 `f` / `run` / `stop` **注册成顶层命令**（不是只做 `mtest` 的子命令）
 *
 * 为什么必须顶层：现场在 stepper_motor 台架上就是直接敲这三条
 * （`f 12000` → `run` → `stop`），照抄过来时也会这样敲；
 * 而 ESP-IDF 的 REPL 只认**注册过的顶层命令名**，`mtest f 12000`
 * 里的 `f` 是子命令 —— `f 12000` 会被直接判成 `Unrecognized command`
 * （2026-09-22 现场就是这样撞上的）。
 *
 * 实现复用 `cli_mtest()`：把 `f 12000` 拼成 `mtest f 12000` 后再调同一个函数，
 * 保证"顶层命令"和"子命令"两条入口**永远同一份逻辑**，不会两处不一致。
 *------------------------------------------------------------------------------*/
static int cli_fwd_to_mtest(int argc, char **argv, const char *sub)
{
    char *shifted[8];
    int n = 0;
    shifted[n++] = (char *)"mtest";     /* 占位（cli_mtest 不看 argv[0]） */
    shifted[n++] = (char *)sub;         /* "f" / "run" / "stop" */
    for (int i = 1; i < argc && n < (int)(sizeof(shifted) / sizeof(shifted[0])); i++) {
        shifted[n++] = argv[i];
    }
    return cli_mtest(n, shifted);
}

static int cli_f(int argc, char **argv)    { return cli_fwd_to_mtest(argc, argv, "f"); }
static int cli_run(int argc, char **argv)  { return cli_fwd_to_mtest(argc, argv, "run"); }
static int cli_stop(int argc, char **argv) { return cli_fwd_to_mtest(argc, argv, "stop"); }

static void motion_cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "rc>";
    repl_cfg.max_cmdline_length = 96;
    repl_cfg.task_priority = 3;             /* 低于 motion(10) 与 LVGL：别抢实时性 */
    repl_cfg.task_stack_size = 4096;
    const esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart failed: %s（串口命令行不可用）", esp_err_to_name(err));
        return;
    }
    esp_console_register_help_command();   /* 内置 help：列出所有命令 */
    /* ★ 注册多个别名（主名 mtest，另加 mt / m）：
     *   现场反馈过一次"命令开头的 mtest 五个字符没进 REPL"——回显是 `rc> 2000 50`，
     *   于是首 token 变成 2000 -> `Unrecognized command`。那是串口侧丢字节
     *   （最常见的原因：**在开机日志还在刷的时候就把命令发出去了**，前面几个字节
     *   被引导阶段吃掉；也可能是工具的"发送"设置）。别名短一点，命中率更高；
     *   另外 CLI 启动时会打一条 `rc>` 提示符，看到它再敲最保险。 */
    static const char *const s_cli_names[] = { "mtest", "mt", "m" };
    for (size_t i = 0; i < sizeof(s_cli_names) / sizeof(s_cli_names[0]); i++) {
        const esp_console_cmd_t cmd = {
            .command = s_cli_names[i],
            .help    = "手动换挡试转：mtest [频率Hz] [脉宽µs] [每方向步数] | sweep | def | ?",
            .hint    = NULL,
            .func    = &cli_mtest,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }

    /* ★★ 顶层 `f` / `run` / `stop`（与 stepper_motor 台架那三条一致的敲法）★★
     *    `mtest f 12000` / `mtest run` / `mtest stop` 仍然可用，两者同一份实现。 */
    static const esp_console_cmd_t s_extra[] = {
        { .command = "f",    .help = "单向连续：设频率（运行中也能改），如 `f 12000`",
          .hint = "[hz]",    .func = &cli_f },
        { .command = "run",  .help = "单向连续运行，直到 stop（如 `run` 或 `run 12000`）",
          .hint = "[hz]",    .func = &cli_run },
        { .command = "stop", .help = "停止连续运行（并停界面运动）；等价 `mtest stop`",
          .hint = NULL,      .func = &cli_stop },
    };
    for (size_t i = 0; i < sizeof(s_extra) / sizeof(s_extra[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&s_extra[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGW(TAG, "串口命令行已就绪：在串口监视器里敲 mtest ? 看用法（UART0 @%d）",
             CONFIG_ESP_CONSOLE_UART_BAUDRATE);
}
#endif  /* MOTION_CLI_ON && !MOTION_SIMULATE */

/*==============================================================================
 * 对外接口
 *============================================================================*/
esp_err_t app_motion_init(void)
{
    s_cmd_q = xQueueCreate(MOTION_CMD_QUEUE_LEN, sizeof(float));
    ESP_RETURN_ON_FALSE(s_cmd_q != NULL, ESP_ERR_NO_MEM, TAG, "cmd queue create failed");

    s_report_q = xQueueCreate(MOTION_REPORT_QUEUE_LEN, sizeof(motion_report_t));
    ESP_RETURN_ON_FALSE(s_report_q != NULL, ESP_ERR_NO_MEM, TAG, "report queue create failed");

    /* 上电位置取 NVS 里的 pos */
    const app_config_t cfg = app_config_get();
    s_report_pos = cfg.pos;
    s_report_pos_atomic = cfg.pos;
    s_pos_steps = (int32_t)lroundf(cfg.pos * MOTION_STEPS_PER_UNIT);
    /* ★ v18：逻辑位置与脉冲域起点对齐（上电时两者相同） */
    s_logic_steps = s_pos_steps;
    s_logic_target = s_pos_steps;
    s_bl_left = 0;
    s_dir_committed = false;
    s_target_pos = cfg.pos;
    s_last_dir = 0;

    ESP_RETURN_ON_ERROR(motion_gpio_init(), TAG, "motion gpio init failed");

#if MOTION_WIRE_TEST_ON_BOOT && !MOTION_SIMULATE
    /* 只动 DIR/ENA、不发脉冲，所以放在 RMT 初始化之前、motion_task 之前跑：
     * 此刻只有本线程在操作这两个脚，读回的一定是自己设的值。 */
    motion_wire_test();
#endif

#if !MOTION_SIMULATE
#if MOTION_STEP_PWM_MODE
    /* ★ v22：STEP 由 LEDC 硬件 PWM 产生 ⇒ **不初始化 RMT**（IO11 只能有一个持有者） */
    ESP_RETURN_ON_ERROR(motion_pwm_init(), TAG, "motion pwm init failed");
#else
    ESP_RETURN_ON_ERROR(motion_rmt_init(), TAG, "motion rmt init failed");
#endif
    /* ★ 把速度换算成**步频**打出来：现场"电机只嗡嗡响不转圈"最常见的原因就是
     *   步频（≈ 转速）超出了这台机器（带载）能跟上的上限，光看 mm/s 看不出来。 */
    ESP_LOGI(TAG, "Motion ready (REAL): %d steps/unit, speed %.3f mm/s (= %.0f steps/s), "
                  "accel %.1f / decel %.1f mm/s²",
             (int)MOTION_STEPS_PER_UNIT, cfg.speed, cfg.speed * MOTION_STEPS_PER_UNIT,
             cfg.accel, cfg.decel);
    /* ★ v8：把"可用频带"直接打出来 —— 现场"一步不动"十有八九是速度落在带外。
     *   ★ v13：**工作点**是 12000Hz（出厂 speed=15.000mm/s）；8500Hz 只是"能转"的
     *     下沿/临界点（收尾减速到那里就结束）。这行要在同一句里把两件事说清楚，
     *     否则现场会把"起跳频率 8500"误当成工作点。 */
    ESP_LOGI(TAG, "  可用频带 %.0f ~ %.0f Hz（= %.3f ~ %.3f mm/s）；"
                  "工作点 %.0f Hz（= %.3f mm/s，串口 `run` 已验证），8500Hz 仅作收尾下限",
             MOTION_TURN_FREQ_MIN_HZ, MOTION_STEP_FREQ_MAX_HZ,
             MOTION_TURN_FREQ_MIN_HZ / MOTION_STEPS_PER_UNIT,
             MOTION_STEP_FREQ_MAX_HZ / MOTION_STEPS_PER_UNIT,
             MOTION_WORK_FREQ_HZ, MOTION_WORK_FREQ_HZ / MOTION_STEPS_PER_UNIT);
    /* 2026-09-22 起这只是一条"指路"日志（原来在排"只嗡嗡不转"时是 WARNING）：
     * 现在这台机器的故障已定位并解决（见 main/README.md 9.7），不需要每次上电都刷警告。 */
    ESP_LOGI(TAG, "提示：定位表见 main/README.md 9.7；TB6600 拨码/电流/步数每毫米见 9.2.1 与 9.5；"
                  "DIR/ENA 三线自检见 9.8");
#else
    ESP_LOGW(TAG, "Motion ready (SIMULATE): 不驱动 STEP/DIR/ENA，位置按曲线数值逼近");
    ESP_LOGW(TAG, "  如需真机驱动：menuconfig -> Roll Coater HMI -> 关闭 ROLLCOATER_MOTION_SIMULATE");
#endif

#if MOTION_SELF_TEST_ON_BOOT && !MOTION_SIMULATE
    /* 放在 motion_task 启动**之前**：此刻只有本线程在操作 RMT，没有并发。 */
    motion_self_test();
#endif

#if MOTION_BOOT_HOLD_ON_BOOT && !MOTION_SIMULATE
    /* ★★ 上电固定段（与 stepper_motor 例程的上电 10 秒同构）★★
     * 放在 motion_task 启动**之前**：此刻只有本线程在操作 RMT/GPIO，没有并发。
     * 现场"例程头 10 秒能转、本工程一步不动"就是靠这一段来分层的：
     *   能转 ⇒ 脉冲链路+波形 OK，问题在曲线/状态机；
     *   不能转 ⇒ 与曲线无关，先试 `STEP 高电平脉宽 = 0`（50% 方波）。 */
    ESP_LOGW(TAG, "★ 上电固定段：%.0f Hz × %" PRIu32 " ms（照抄 stepper_motor 例程的上电段）",
             MOTION_BOOT_HOLD_HZ, (uint32_t)MOTION_BOOT_HOLD_MS);
    ESP_LOGW(TAG, "  本段在 UI 建屏之前跑，所以这段时间**屏幕不会响应**（最多 %" PRIu32 " ms）；"
                  "要跳过就把 menuconfig 的 ROLLCOATER_BOOT_HOLD 关掉",
             (uint32_t)MOTION_BOOT_HOLD_MS);
    motion_fixed_run(MOTION_BOOT_HOLD_HZ, MOTION_BOOT_HOLD_MS, true);
    /* 裸脉冲不记账：位置必须由操作员在 State1 重新标定（本工程本来就要求） */
    ESP_LOGW(TAG, "  固定段是裸脉冲，位置记账未跟着走 —— 请在 State1 重新输入刻度盘读数标定");
#endif

#if MOTION_CLI_ON && !MOTION_SIMULATE
    /* 串口命令行（mtest）：起一个低优先级控制台任务，只在敲命令时才会发脉冲。
     * 放在 motion_task 之前：构造期间只有本线程在碰 RMT。 */
    motion_cli_start();
#endif

    /* ★ 2026-09-22 晚：任务优先级 5 → **10**，与同现场 Arduino 版对齐
     *   （motor.cpp：`xTaskCreatePinnedToCore(motor_task, "motor", 3072, nullptr, 10, ...)`）。
     * 为什么重要：RMT 队列只存 MOTION_TX_QUEUE_TARGET(3) 个事务 ≈ 3ms 的量，
     *   本任务被抢占太久就会把队列抽空 → **脉冲串出现空档**（示波器上看不出"周期不对"，
     *   但驱动器就是一顿一顿的）。现场 WiFi 任务的优先级是 23、LVGL 任务是 4，
     *   原来 prio 5 只要 WiFi 一忙就会被长时间抢走 CPU。
     * 本任务每轮都有 vTaskDelay（不占满 CPU），提高优先级不会饿死别的任务。 */
    BaseType_t ok = xTaskCreate(motion_task, "motion", 4096, NULL, MOTION_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "motion task create failed");
    return ESP_OK;
}

esp_err_t app_motion_set_current(float pos)
{
    if (s_cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 手动盘车/标定：清空待执行目标，停止积分 */
    xQueueReset(s_cmd_q);
    s_moving = false;
    s_vel_steps = 0.0f;
    s_step_acc = 0.0f;
    s_aborted_latch = false;
    s_arrived_latch = false;

    s_report_pos = pos;
    s_report_pos_atomic = pos;
    s_target_pos = pos;
    s_pos_steps = (int32_t)lroundf(pos * MOTION_STEPS_PER_UNIT);
    s_cmd_start = s_pos_steps;
    s_cmd_target = s_pos_steps;
    /* ★ v18：标定 = 重建坐标系 ⇒ 逻辑位置/脉冲域**一起**归到这个点上，
     *   残余的间隙补偿计数作废（否则下一次换向会凭空多走一段）。 */
    s_logic_steps = s_pos_steps;
    s_logic_target = s_pos_steps;
    s_bl_left = 0;
    s_dir_committed = false;
    /* 手动动过轴之后反向间隙状态未知，下一次运动不做间隙补偿 */
    s_last_dir = 0;

    motion_driver_enable(true);
    motion_push_report(true);
    ESP_LOGI(TAG, "Current position set to %.3f mm", pos);
    return ESP_OK;
}

esp_err_t app_motion_move_to(float target)
{
    if (s_cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_alarm_latched) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 用覆盖写保证「最新目标」一定被执行；队列满则丢弃最旧的一条 */
    while (xQueueSend(s_cmd_q, &target, 0) != pdTRUE) {
        float drop;
        if (xQueueReceive(s_cmd_q, &drop, 0) != pdTRUE) {
            break;
        }
    }
    return ESP_OK;
}

void app_motion_stop(void)
{
    if (s_cmd_q != NULL) {
        xQueueReset(s_cmd_q);
    }
    s_moving = false;
    s_vel_steps = 0.0f;
    s_step_acc = 0.0f;
    s_aborted_latch = true;
#if MOTION_STEP_PWM_MODE && !MOTION_SIMULATE
    /* ★★★ v22：**必须在这里停波形** ★★★
     *   本函数是 UI 任务调的（STOP 键 / 返回键 / Admin 退出），而 LEDC 一旦启动就由
     *   硬件**一直发下去** —— 不在这里显式停，轴会一直走到行程尽头。 */
    motion_pwm_abort();
#endif
    motion_push_report(true);
    ESP_LOGW(TAG, "STOP pressed, motion halted at %.3f mm", s_report_pos);
}

bool app_motion_is_alarm(void)
{
    return motion_alarm_input_active();
}

bool app_motion_is_alarm_latched(void)
{
    return s_alarm_latched;
}

esp_err_t app_motion_clear_alarm(void)
{
    /* 故障没排除就拒绝清除，避免误复位 */
    if (motion_alarm_input_active()) {
        ESP_LOGW(TAG, "Clear alarm refused: alarm input still active");
        return ESP_ERR_INVALID_STATE;
    }
    s_alarm_latched = false;
    s_alarm_edge = false;
    s_aborted_latch = false;
    motion_driver_enable(true);
    ESP_LOGI(TAG, "Alarm acknowledged");
    return ESP_OK;
}

void app_motion_set_enabled(bool enable)
{
    motion_driver_enable(enable);
#if !MOTION_SIMULATE
    ESP_LOGI(TAG, "Driver %s (ENA=GPIO%d, level=%d)", enable ? "ENABLED" : "RELEASED",
             MOTION_GPIO_ENA, (int)gpio_get_level(MOTION_GPIO_ENA));
#else
    ESP_LOGI(TAG, "Driver %s (SIMULATE: 引脚未驱动)", enable ? "ENABLED" : "RELEASED");
#endif
}

bool app_motion_poll_report(motion_report_t *out)
{
    if (s_report_q == NULL || out == NULL) {
        return false;
    }
    return xQueueReceive(s_report_q, out, 0) == pdTRUE;
}

float app_motion_get_position(void)
{
    return s_report_pos_atomic;
}

float app_motion_get_velocity(void)
{
    return s_vel_mm_atomic;
}
