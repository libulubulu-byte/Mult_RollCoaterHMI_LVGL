/**
 * @file app_motion.h
 * @brief STEP/DIR/ENA/ALARM 运动控制：RMT 生成脉冲 + 梯形速度曲线 + 位置回读
 *
 * 任务模型：
 *   motion_task（FreeRTOS 任务，优先级 5）
 *     - 消费「目标位置」队列（app_motion_move_to 非阻塞投递）
 *     - 按梯形速度曲线（accel / decel / speed 来自 app_config）逐 tick 积分出
 *       本 tick 应发的脉冲数，用 RMT TX 通道 + copy encoder 发射
 *     - 每 CONFIG_ROLLCOATER_UI_PUSH_MS 把位置通过环形队列推给 UI
 *     - ALARM 引脚（GPIO 中断 + 电平轮询）有效时立刻停止发脉冲、抬 ENA，并置报警锁存
 *
 * ★ 2026-09-22 修「马达不转」的两处硬伤（详见 app_motion.c 同名注释）：
 *   1) 一个 RMT 事务里**摆满 n 个脉冲符号、loop_count 恒为 0**，不再靠
 *      "1 个符号 + loop_count 循环"——后者与驱动自动补的 duration=0 结束符
 *      语义冲突时会让整条脉冲链静默失效（rmt_transmit 照样返回 ESP_OK）。
 *   2) 每次运动结束都会打一条**脉冲核对**日志：
 *        `脉冲核对 OK：请求 N 个脉冲 / K 个事务，RMT 报告 N+K 个符号`
 *      数字对得上 = 每个脉冲都真的出了引脚，剩下只能查接线 / ENA / 驱动器；
 *      对不上则会以 `★★ 脉冲核对异常` 的 ERROR 级别打出来。
 *      自检开关：menuconfig -> Roll Coater HMI 配置 -> ROLLCOATER_MOTION_SELFTEST。
 *
 * 位置语义：
 *   报告位置 = 机械实际位置视图（mm）。反向运动时会多走一段 backlash 补偿间隙，
 *   但报告位置按运动进度线性映射到目标，因此到点后报告值精确等于目标值。
 *
 * 无电机调试：
 *   Kconfig 里打开 ROLLCOATER_MOTION_SIMULATE（默认开）后不初始化 RMT，
 *   也不驱动 STEP/DIR/ENA，位移按同样的曲线数值逼近，HMI 全流程可验证。
 *   ALARM 默认关闭（MOTION_GPIO_ALARM = -1）。要演练报警，把它改成一个空闲脚
 *   （IO17 / IO18 / IO10）再把那根线短到 GND ——
 *   ★ 不要去短接 IO13，那已经是 ENA 输出脚了（会伤引脚）。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * 是否处于"无电机调试模式" —— 对外统一出口
 *
 * ★ 为什么要包这一层：
 *   Kconfig 的 bool 项被**关闭**时，生成的 sdkconfig.h 里是
 *   **不再定义** CONFIG_ROLLCOATER_MOTION_SIMULATE（注意是"不定义"，
 *   而不是"定义为 0"）。所以任何模块只要直接写
 *       CONFIG_ROLLCOATER_MOTION_SIMULATE ? "SIMULATE" : "REAL"
 *   一旦在 menuconfig 里关掉这一项，就会编译失败：
 *       error: 'CONFIG_ROLLCOATER_MOTION_SIMULATE' undeclared
 *   统一改用下面的宏之后，各模块不必再各自 #ifdef，这个坑一次堵死。
 *============================================================================*/
#ifdef CONFIG_ROLLCOATER_MOTION_SIMULATE
#define APP_MOTION_SIMULATE         (1)   /* 模拟：不初始化 RMT、不驱动 STEP/DIR/ENA */
#else
#define APP_MOTION_SIMULATE         (0)   /* 真机：RMT 发脉冲 */
#endif

/*==============================================================================
 * 硬件引脚
 *
 * ★ 2026-09-21 引脚调整（现场要求：电机信号**只用 IO11 / IO12 / IO13 三根**）
 *      STEP : IO17 -> IO11
 *      DIR  : IO18 -> IO12
 *      ENA  : IO12 -> IO13
 *      ALARM: IO13 -> **-1（关闭）**
 *             现场没有报警信号源，照参考工程关掉；★ 13 现在被 ENA 占用了，
 *             绝不能再把它当报警输入留着。
 *
 * 依据：同现场的 Arduino 版工程
 *   `Mult_ESP32-S3_arduino_lvgl/board_config.h:386-389`：
 *   `STEP=IO11 / DIR=IO12 / EN=IO13 / ALARM=-1`。
 *
 * 【★ 现场为什么会误报警（2026-09-21 实测记录，别再踩）】
 *   现场接线 = IO11 STEP / IO12 DIR / IO13 ENA；
 *   而本工程当时还是 STEP=17 / DIR=18 / ENA=12 / ALARM=13，于是引脚整体错位：
 *     · 代码的 ENA(IO12) 实际去驱动了现场的 **DIR** 线（把它拉低）；
 *     · 代码的 ALARM(IO13) 实际读的是现场的 **ENA** 线 —— 驱动器把 ENA 拉低使能，
 *       代码读到低电平就误判成 `ALARM input active` 并锁存，现场表现为"没接报警却报警"；
 *     · 真正的 STEP/DIR 被打到空的 IO17/IO18 上，电机自然一步都不动。
 *   引脚对齐到 11/12/13 之后，这三个症状一起消失。
 *
 * ⚠️ 三条必须记住（参考工程 board_config.h:370-377 的教训）：
 *   1) **别插 SD 卡**：IO11/IO12/IO13 正是这族板的 SD 卡 SPI 线（IO10 = SD_CS）。
 *      本工程不初始化 SD，所以能当普通 IO 用；插着卡时卡内电路会挂到这三根线上。
 *   2) **别打开 XPT2046 电阻屏**：它的 T_DIN/T_CLK/T_OUT 正是 IO11/12/13。
 *   3) **★ 绝不能再短接 IO13**：它现在是 ENA **输出脚**，短接可能损伤引脚。
 *
 * ⚠️ 不可用引脚：IO19/IO20（GT911 的 I2C SDA/SCL，同时是 USB D-/D+）、
 *    IO0（板载 BOOT 键）、IO33~37（ESP32-S3R8 的 Octal PSRAM 内部占用）、
 *    IO1（功放 I2S DIN）、以及 LCD RGB 并口那一整排（见 bsp_pins.h）。
 *============================================================================*/
#define MOTION_GPIO_STEP            (11)   /* STEP/PUL：RMT 输出，40Hz ~ 20kHz */
#define MOTION_GPIO_DIR             (12)   /* DIR：目标增大 = 高 */
#define MOTION_GPIO_ENA             (13)   /* ENA：低有效；上电先输出高（失能） */
#define MOTION_GPIO_ALARM           (-1)   /* ALARM：-1 = 关闭（现场无报警源）。
                                            * 要启用就填一个空闲脚（IO17 / IO18 / IO10），
                                            * 相关的 gpio_config / ISR / 电平轮询会自动编译进来。 */

#define MOTION_ENA_ACTIVE_LEVEL     (0)   /* 低有效，与参考工程 MOTOR_EN_ACTIVE_LOW=1 等价 */

/*------------------------------------------------------------------------------
 * ★★★ 静止自动脱机（v23，现场需求："马达停了之后嗡嗡响"）★★★
 *
 *  现象：轴停下来之后一直"嗡"——那是驱动器**保持电流**（ENA 使能着、线圈按额定电流
 *       锁住转子）发出的声音，同时也让电机发热。
 *  做法（与同现场 Arduino 版 `board_config.h` 的 `CFG_MOTOR_IDLE_RELEASE_MS` 同源）：
 *      静止持续到 **本值（ms）** 后自动**抬 ENA（脱机）** ⇒ 嗡嗡声与发热消失；
 *      **下一次下发目标时 `motion_begin()` 会自动重新使能**（并等 100ms 稳定），
 *      所以操作手感不变，不用手动复位什么。
 *  取值：0 = 关闭；默认 **2000**（现场定稿：**静止 2 秒**后脱机）。
 *
 *  ⚠️⚠️ 使用前提（**务必现场确认**）：脱机的那段时间里，**负载不能把轴推动**
 *     （丝杆自锁 / 无重力负载 / 或有机械制动）。本工程**没有编码器** ——
 *     轴一旦在脱机期间被推走，软件毫不知情，"当前位置"就与实际不符了
 *     （判据：听到"嗡"声消失后用手推/看轴，若能移动就不要开这个功能，改 0）。
 *  ★ 报警时仍然是"抬 ENA 切断输出"（那是急停动作，与本功能无关）。
 *------------------------------------------------------------------------------*/
#ifndef MOTION_IDLE_RELEASE_MS
#define MOTION_IDLE_RELEASE_MS      (2000)
#endif
#define MOTION_DIR_POSITIVE_LEVEL   (1)   /* 目标增大时 DIR 的电平 */
#define MOTION_ALARM_ACTIVE_LEVEL   (0)   /* 报警输入低有效，内部上拉 */

/** DIR 换向后、发第一个脉冲之前必须等待的稳定时间（µs）。
 *  与同现场 Arduino 版同源（`board_config.h` 的 MOTOR_DIR_SETUP_US = 2）；
 *  这里取 10µs 留足余量 —— 只在每次下发运动时等一次，代价可忽略，
 *  但换向那一拍的第一个脉冲不会因为 DIR 还没稳定而丢掉。 */
#define MOTION_DIR_SETUP_US         (10)

/*------------------------------------------------------------------------------
 * ★★★ v22：STEP(IO11) 改用 **LEDC 硬件 PWM** 直接产生（现场要求："你直接用PWM"）★★★
 *
 *  ## ★★ 当前取值 = **0**（`MOTION_STEP_PWM_MODE = (0)`，2026-09-22 现场定稿）★★
 *    · **0（当前交付）= 走 RMT 事务路径** —— 现场把 RMT 侧三处都优化过
 *      （中断优先级 2、事务 190 符号、攒批 15ms；见 app_motion.c 的
 *      `MOTION_RMT_INTR_PRIORITY` / `MOTION_MAX_STEPS_PER_TX` / `MOTION_TX_BATCH_MS`），
 *      实测**已经能正常跑**，而且它保留"逐事务精确计数、无累积误差"的长处；
 *    · **1 = 正常运动也用硬件 PWM 发脉冲** —— 唯一能保证"波形**绝对连续**"的方案
 *      （原理见下）；现场若再出现"来回 / 一顿一顿 / 失步"，把它**改成 1** 重编即是**对照实验**。
 *  ⇒ 两条路都能跑通，"改这一个数字 + 重编"就是切换开关。**别删其中任何一条。**
 *
 *  ## 为什么当初要加 PWM 这条路（现场实测结论）
 *    IO11 的方波**不连续** = "电机来回/失步"的总根源。RMT 那条路有个天生缺陷：
 *    IDF 驱动在**每个事务结尾**都写一个 `duration0 = 0` 的 stop 符号让硬件停一下，
 *    再由"发送完成中断"启动下一个事务（`esp_driver_rmt/src/rmt_tx.c:601-648`）
 *    ⇒ **每个事务边界都留一个"中断延迟"长的空档**；CPU 一忙空档就变大。
 *    LEDC 是**纯硬件 PWM**：没有事务、没有队列、没有边界 —— 中间几万、几十万个脉冲
 *    全是硬件自己产生的，**原理上不可能断**，软件只在"开始/结束"两个时刻介入。
 *
 *  ## 代价（都已处理）
 *    · LEDC **不能数脉冲** ⇒ 用"跑够 脉冲数 ÷ 实际频率 的时间"来停（`esp_timer` 单次
 *      定时器；它的回调跑在 esp_timer 任务里、不是 ISR，所以能安全调用 ledc_* API）。
 *      落点精度 ≈ ±1 个脉冲（1 步 = 1.25µm @800 步/mm），对定位足够。
 *    · 频率用 `ledc_get_freq()` 的**实际值**换算时间（LEDC 有小数分频，实际频率可能与
 *      请求值差千分之几 —— 用实际值才不会多/少发脉冲 ⇒ 落点才准）。
 *    · 到位 / STOP / 报警都会**立即**停 PWM（脉冲一断转子就停）。
 *    · 资源：用 LEDC **TIMER_2 / CHANNEL_2**（背光是 TIMER_1/CH0、补光灯是 TIMER_0/CH1，
 *      见 app_light.h:40-41 与 bsp_display.c:250-260）—— 互不干扰。
 *    · 本模式下**不再初始化 RMT**（IO11 只能有一个持有者），脉冲核对/空档统计那几条
 *      日志自动换成"PWM 模式"的说法（没有事务，本来就没有可核对的事务）。
 *------------------------------------------------------------------------------*/
#ifndef MOTION_STEP_PWM_MODE
#define MOTION_STEP_PWM_MODE        (0)
#endif

/** ★★ DIR / ENA 拉好之后、**发第一个脉冲之前**还要等的稳定时间（ms）★★
 *
 * 起初取 20ms（从"唯一被证明能转"的自检路径抄来的）；**v10 起默认 100ms**，
 * 因为现场反馈"10 秒固定段能转，但**起转那一下发顿**"—— 这正是本值要治的东西：
 *     设 DIR → ENA 拉低 → 等 MOTION_ENA_SETTLE_MS → 才发第一个脉冲
 * TB6600 这类驱动器是**光耦隔离输入**，ENA 由高变低后要经过"光耦导通 +
 * 驱动器输出级使能 + 转子保持力矩建立"，量级是 ms 甚至几十 ms；
 * 这段窗口里发出去的脉冲驱动器**根本不计** ⇒ 现场感觉就是"起转顿一下 / 滑一下"，
 * 之后才跟上（因为那时驱动器已经完全醒了）。
 *
 * ★ 可配：`menuconfig -> Roll Coater HMI 配置 -> 使能稳定时间`
 *   （`ROLLCOATER_ENA_SETTLE_MS`，默认 100，范围 5~1000）。
 *   起转仍发顿就往 200 / 300 试（只会更稳，没有副作用）。
 * 代价：每次下发运动多等这么多毫秒（100ms 在操作上几乎感觉不到）；
 *   诊断扫描的每一段也会各等一次（24 段 ≈ 多 2 秒）。 */
#ifdef CONFIG_ROLLCOATER_ENA_SETTLE_MS
#define MOTION_ENA_SETTLE_MS        (CONFIG_ROLLCOATER_ENA_SETTLE_MS)
#else
#define MOTION_ENA_SETTLE_MS        (100)
#endif

/*==============================================================================
 * ★★ 上电固定段（照抄 examples/peripherals/rmt/stepper_motor 的上电段）★★
 *
 * 为什么要有它（2026-09-22 深夜）：
 *   现场事实是「**例程上电后的头 10 秒能连续正常转**，而本工程一步不动」。
 *   例程那 10 秒做的事就是：使能 → DIR → 等 20ms → **以固定 8500Hz 连发 10 秒** → 停 + 脱机。
 *   它**绕开了本工程的一切软件层**（状态机、梯形曲线、UI、反向间隙、位置记账），
 *   所以最适合用来回答"到底是硬件/驱动器不认，还是我们这套流程有问题"。
 *
 *   本项打开后，`app_motion_init()` 会在创建 motion_task **之前**先跑同样的一段
 *   （走**本工程自己的 RMT 发射路径**，所以结论对本工程有效），上电即可用耳朵判定：
 *     · 这 10 秒能连续转   ⇒ 脉冲链路 + 波形没问题，问题只可能在曲线/状态机/参数；
 *     · 这 10 秒也只嗡嗡   ⇒ 与曲线/UI 无关，去查波形（先试 `STEP 高电平脉宽 = 0`）、
 *                            电流档、相序、供电（README 9.9）。
 *
 * ⚠️ 它是**真走轴**的：`8500Hz × 10s = 85000 脉冲 = 106mm @800 步/mm`（单向）。
 *    上电前先确认轴两侧行程够，否则把时长改短（Kconfig 的 ms 项，或 `boot 0 0` 等价物
 *    = 把 `ROLLCOATER_BOOT_HOLD` 关掉），或在串口里用 `mtest hold 8500 2000` 只跑 2 秒。
 * ⚠️ 这一段是"裸脉冲"，**位置记账不会跟着走** —— 上电后本来就必须在 State1
 *    重新输入刻度盘读数来标定（本工程的设计如此），所以不影响正常使用。
 *============================================================================*/
#ifdef CONFIG_ROLLCOATER_BOOT_HOLD
#define MOTION_BOOT_HOLD_ON_BOOT    (1)
#else
#define MOTION_BOOT_HOLD_ON_BOOT    (0)
#endif
#ifdef CONFIG_ROLLCOATER_BOOT_HOLD_HZ
#define MOTION_BOOT_HOLD_HZ         ((float)CONFIG_ROLLCOATER_BOOT_HOLD_HZ)
#else
#define MOTION_BOOT_HOLD_HZ         (12000.0f)  /* = 出厂默认（现场指定的那档） */
#endif
#ifdef CONFIG_ROLLCOATER_BOOT_HOLD_MS
#define MOTION_BOOT_HOLD_MS         ((uint32_t)CONFIG_ROLLCOATER_BOOT_HOLD_MS)
#else
#define MOTION_BOOT_HOLD_MS         (10000U)    /* 与例程同时长 */
#endif

/*==============================================================================
 * 机械与脉冲参数
 *============================================================================*/
/** 每单位（mm）对应多少步 = 驱动器脉冲/圈 ÷ 丝杠导程。
 *
 * ★★ 2026-09-22 晚：从"写死的 800"改成 **menuconfig 可配**
 *     （`Roll Coater HMI 配置 -> 每毫米脉冲数 Steps per unit`，默认 800）。
 *
 * 为什么必须可配：厂商拨码口径（现场提供）是
 *     「SW1/SW2/SW3 = 1600 细分、SW4/SW5/SW6 = 0.5A」
 * 而 1600 是"**脉冲/圈**"，不是"步/mm" —— 后者的换算要除丝杠导程：
 *     1600 ÷ 2mm 导程 = **800**  （与本工程默认自洽，不用改）
 *     1600 ÷ 4mm 导程 = **400**  （此时必须把本项改成 400！）
 *     3200（16 细分）÷ 4mm = 800 （这条也与默认自洽）
 * ★ 填错的后果不是"不转"，而是**位置与速度都差一个倍数**：
 *     本值填大了（实际 400 却填 800）-> 只走一半，而指令的 20mm/s 会按
 *     40mm/s 去发脉冲；配合 0.5A 这种小电流 → 起步就堵转、只嗡嗡不转。
 *     本值填小了（实际 800 却填 400）-> 走过头，报位置比实际小一半。
 *
 * ★ 不用猜，量出来最快（见 main/README.md 9.5）：
 *     ROLLCOATER_MOTION_SELFTEST 每档固定 200 步，量实测位移 d(mm)
 *     -> 真实 steps/mm = 200 / d   （d=0.25 -> 800；d=0.5 -> 400；d=0.125 -> 1600）
 *
 * 注：本工程 Admin 界面里**没有** "Steps per unit" 这一项（与 Arduino 版不同），
 *     所以只能在这里（Kconfig）改，改完需要重新编译烧录。 */
#ifdef CONFIG_ROLLCOATER_STEPS_PER_UNIT
#define MOTION_STEPS_PER_UNIT       ((float)CONFIG_ROLLCOATER_STEPS_PER_UNIT)
#else
#define MOTION_STEPS_PER_UNIT       (800.0f)
#endif
/** RMT 计数分辨率：1MHz -> 1 个计数 = 1us */
#define MOTION_RMT_RESOLUTION_HZ    (1000000)
/** 发射用的最低频率（RMT 周期上限 32767µs ⇒ 约 15.3Hz；40Hz 留足余量）。
 *  ★ 它**不是**"电机会转的最低频率" —— 电机能不能转取决于下面的
 *    MOTION_TURN_FREQ_MIN_HZ。本值只用来兜底：加速段起点、每拍脉冲数不为 0。 */
#define MOTION_STEP_FREQ_MIN_HZ     (40.0f)

/** 最高脉冲频率（= 最大可用速度的硬上限）。
 *        max_speed(mm/s) = MOTION_STEP_FREQ_MAX_HZ / MOTION_STEPS_PER_UNIT
 *      当前 = 20000 / 800 = **25 mm/s**。Admin 里把 Speed 设得比这更大时，
 *      软件会把它钳到这里（并 WARN 一次）。
 *
 *==============================================================================
 * ★★★ v7（2026-09-22 深夜，现场实测）：瓶颈是"**低频不走、高频才转**" ★★★
 *==============================================================================
 * 现场反馈（当前拨码 = `SW1..3 OFF ON OFF` 细分 1600 脉冲/圈、`SW4..6 ON OFF OFF`
 * 电流 2.0A，即"能跑通的那套"）：
 *
 *     「**8500Hz 才转**」—— 低于它一步都不走，只有嗡嗡响。
 *
 * ★ 这条和 v6「1200Hz 以上开始抖」**并不矛盾，它们是同一件事的两个刻度**：
 *     v6 那次扫频的拨码是另一种细分（`ON ON OFF` = **200** 脉冲/圈）:
 *         1200 Hz ÷ 200 = 6.0 转/秒
 *     v7 这次拨码 1600 脉冲/圈：
 *         8500 Hz ÷ 1600 = **5.3 转/秒**
 *   ⇒ 两次都指向同一个物理事实：**这台机器（该电机 + 该驱动器 + 该电压/负载）
 *     只在转速 ≳ 5~6 转/秒（≈320~360 RPM）时才带得动**，以下无论怎么调
 *     脉宽/频率/代码都只嗡嗡响 —— 那是电机的转矩-转速特性 + 低速共振，
 *     不是脉冲生成的问题（波形/脉宽/RMT 早在 9.9 就被逐项排除了）。
 *
 * ★★★ 换算口径（改拨码后照这个算，不用重扫）★★★
 *        起跳频率 Hz ≈ 5.3 × 脉冲每圈
 *        1600 脉冲/圈 -> 8500Hz   200 脉冲/圈 -> 约 1060Hz
 *     也就是"低频段是禁区"。软件要做的就是**让巡航频率待在禁区之上**，
 *     并**用足够陡的加速度快速穿过禁区**（见 MOTION_TURN_FREQ_MIN_HZ 与出厂 accel）。
 *
 * ★ 为什么 v6 的 1000Hz 上限 + 1mm/s(=800Hz) 出厂速度在这套拨码下"一步都不动"：
 *     1000Hz 和 800Hz 都落在禁区里 —— 不是"太慢不好看"，是**低于起跳频率**。
 *     所以 v7 把上限放回 20000（> 8500 才有余量）；
 *     v8 再把出厂 speed 定在 **8500Hz 那个已验证的点**（10.625mm/s），
 *     并把运动曲线整体抬到起跳频率以上（见 MOTION_TURN_FREQ_MIN_HZ）；
 *     v9 进一步把工作点定到 **12000Hz（= 15mm/s）**：现场实测"直接就能起来"，
 *     离下沿留了余量，起转更利索（软启动默认关闭，见 app_config.c 的 CFG_DEF_SPEED）。
 *
 * ★ 上限为什么是 20000：与同现场 Arduino 版（board_config.h 的
 *   MOTOR_STEP_FREQ_MAX_HZ = 20000.0f）以及 stepper_motor 例程的 FREQ_MAX 一致。
 *   ★ 但要清楚：**当前拨码下只有 8500Hz 被现场验证过**，20000 只是"给自己留余量
 *   的工程上限"。要往上调（Admin 里改 Speed，或把这里改大）之前，
 *   先用 `mtest 10000` / `12000` / `16000` 逐档确认那一档真的能连续转。
 *   TB6600 的 PUL 输入本身能到几十 kHz，限制不在驱动器输入端，而在电机能跟上的转速。
 *
 * ★ 什么情况下必须重新量 / 重算：换电机 / 换驱动器 / **改细分拨码** / 改供电电压。
 *   量法：串口 `mtest <Hz> 5 <步数>` 从低往高试（它**不受本值约束**，
 *   见 MOTION_SCAN_FREQ_MAX_HZ），或打开 ROLLCOATER_MOTION_SELFTEST 跑上电扫描。
 */
#define MOTION_STEP_FREQ_MAX_HZ     (20000.0f)

/** ★★ 起跳频率（= 可用**下沿**）：低于它电机一步都不走，只会嗡嗡响 ★★
 *
 *  = **8500 Hz**（= 10.625 mm/s @800 步/mm）。
 *  ★ 出厂**工作点**也是 **8500Hz**（v10 起，见 app_config.c 的 CFG_DEF_SPEED）——
 *    它是这台机器**唯一被现场验证能连续平稳转**的点：
 *      · `f 8500` + `run`（台架固定段）⇒ 连续平稳 ✓
 *      · `f 12000` + `run`（单向连续）⇒ **"左右转"** = 7.5 转/秒失步、来回摆 ✗
 *    ⇒ **可用窗口很窄（约 8500 ~ 10000Hz）**：低于下沿"一步不走"，高于上沿
 *      "原地来回摆"（很容易被误判成方向错/软件乱发脉冲）。
 *    本值既是最低可用频率，也是当前工作点；往上调必须先台架验证。
 *
 *  直接取现场实测值：
 *     `examples/peripherals/rmt/stepper_motor` 的**上电固定段**就是
 *     「使能后**直接把频率推到 8500Hz、连跑 10 秒**」，现场确认这一段
 *     **能连续正常转** —— 这是当前拨码（1600 脉冲/圈）下最早被验证的工作点。
 *
 *  ★★ 本值有**三个**作用：
 *
 *  ① **巡航频率下限**：`motion_cruise_freq_hz()` 把配置速度换算出的频率钳到本值
 *     （低于它就把 Speed 抬上来，并 WARN 一次）。理由：低于起跳频率时轴**根本不动**，
 *     安静地照做等于"按了没反应"，比抬高更难排查。
 *
 *  ② ★★ **曲线本身**（v17 起）：`motion_run_profile()` 走的是**整段恒定频率** ——
 *     起步 = 巡航 = 收尾 = 工作频率（`MOTION_WORK_FREQ_HZ`），**没有减速段**，
 *     到位即停（脉冲一停转子就停）。与串口 `run` + `stop` 逐项同构。
 *     ⇒ 本值（8500Hz）**不再出现在曲线里**，只用于上面 ① 那条下限保护。
 *     ★ 为什么取消减速：现场实测 —— `run`（恒定 12000Hz）连续平稳；而原来每段末尾
 *       会从 12000Hz 减速到 8500Hz（日志 `本段实际发射频率 8620 ~ 12000 Hz` 就是它），
 *       8500Hz 是"能转"的**下沿/临界点**，转子在那里打滑 ⇒ 现场"**正转后反转**"。
 *
 *  ③ 起步（v15/v17 起）：曲线的**第一拍直接用工作频率**（出厂 12000Hz），
 *     本值**不参与起步** —— 详见 MOTION_SOFT_START_HZ 的说明。
 *     v8~v14 那种"起步先顶到 8500Hz 再爬到巡航"的写法已经被现场否掉：
 *     日志里 `曲线：起步 8500 Hz → 巡航 12000 Hz` 说明**起步那一下不是 12000**。
 *
 *  ★ **"刹车距离"已不存在**（v15 引入、v17 取消）：整段恒速、到位即停，
 *    不再需要"末端从巡航刹回起跳频率"的那段距离（历史文档里的 0.37mm 已作废）。
 *
 *  ★ 采样/验证：`mtest 8500 5 3200`（可用下沿）；`mtest sweep` 会扫
 *    6000~16000Hz，看"从哪一档开始转"。
 *
 *  换算口径：f_turn ≈ 5.3 × 脉冲每圈（见 MOTION_STEP_FREQ_MAX_HZ 处的说明）。 */
#define MOTION_TURN_FREQ_MIN_HZ     (8500.0f)

/** ★★ 现场**已验证能连续平稳转**的工作频率（Hz）= **12000**（= 15.000mm/s @800 步/mm）★★
 *
 *  依据：串口 `f 12000` + `run`（**单向连续**）现场实测"转得很好" ——
 *  它同时是出厂 `speed` 对应的频率（见 app_config.c 的 CFG_DEF_SPEED）。
 *
 *  ★ 它**不是硬上限**（硬上限是 MOTION_STEP_FREQ_MAX_HZ = 20000），
 *    只用于"超过就打一条 WARN"：现场最容易踩的坑就是在 Admin 里把 Speed 填大
 *    （例如填 40000 → 40.000mm/s，会被钳到 25mm/s = **20000Hz**），
 *    而电机在 20000Hz 上根本跟不上 ⇒ **一次行程里就"一会正转一会反转"**（失步打滑），
 *    极容易被误判成"方向乱跳/软件毛病"。见 `motion_begin()` 里的 WARN。
 *  ★ 想往上探（14000/16000…）必须先现场逐档确认：`f 14000` → `run` 看/听。 */
#define MOTION_WORK_FREQ_HZ         (12000.0f)

/** ★★ 软启动起步频率（**默认 0 = 关闭**）：v11 新增，v15 起默认关 ★★
 *
 *  =0（默认，关闭）：曲线的**第一拍直接就是巡航频率**（出厂 **12000Hz**）——
 *      波形与串口 `f 12000` + `run`（现场验证"转得很好"）同构。
 *      ★ v8~v14 的写法是"起步先顶到起跳频率、再按 accel 爬到巡航"，
 *        日志 `曲线：起步 8500 → 巡航 12000` —— 现场据此指出起步频率不对，
 *        于是 v15 改成"第一拍就是巡航频率"。
 *
 *  >0（打开，允许低于起跳频率的爬升）：从本值开始按 accel 爬到巡航频率，
 *      用来给转子留"牵入"时间。**但现场实测这条路更差**：爬升段会把轴拖着
 *      走过它**带不动的频段**，现象是"**卡壳、左右抖动，然后才转**"，
 *      爬升段越长抖得越久 ⇒ 所以默认关闭。
 *
 *  ★ 与 MOTION_TURN_FREQ_MIN_HZ 的分工（v17 起简化）：
 *      · **起步**：本值（0 = 直接用工作频率；>0 = 从低频爬到工作频率后恒速）；
 *      · **收尾**：**不再减速** —— 整段恒速、到位即停（与串口 `run` 同构）。
 *
 *  ★ 现场可运行期改，不用重编：串口 `mtest ss <hz>`（0 = 关；9000/10000 = 短爬升）。
 *    改完 `mtest move <mm>` 立刻看起转。
 *  ★ 换算：爬升时间 = (巡航频率 - 本值) / (accel × 步数每毫米)。 */
#ifdef CONFIG_ROLLCOATER_SOFT_START_HZ
#define MOTION_SOFT_START_HZ        ((float)CONFIG_ROLLCOATER_SOFT_START_HZ)
#else
#define MOTION_SOFT_START_HZ        (0.0f)     /* 0 = 关闭：第一拍就是巡航频率 */
#endif

/** 扫频自检 / 串口 `mtest` 允许的最高频率。
 *  诊断工具的目的就是"越过安全线去找那条线"，所以它必须能发得比
 *  MOTION_STEP_FREQ_MAX_HZ 更高 —— 否则 `mtest 4000` 会被静默钳到 1000，
 *  量出来的"上限"就是假的（这正是当初 20000→1000 的教训）。
 *  v7 起与 MOTION_STEP_FREQ_MAX_HZ 同为 20000：现场要在 8500Hz 以上找"哪一档转、
 *  哪一档抖"，8000 的上限正好卡在禁区里面，`mtest 8500` 会被钳成 8000 —— 量不出东西。 */
#define MOTION_SCAN_FREQ_MAX_HZ     (20000.0f)
/** 到位死区（mm）。UI 背景色判定也用这个值 */
#define MOTION_DEADBAND_MM          (0.005f)

/* 目标位置命令队列深度（覆盖式使用，只保留最新一条） */
#define MOTION_CMD_QUEUE_LEN        (4)
/* 位置上报环形队列深度 */
#define MOTION_REPORT_QUEUE_LEN     (8)
/* 空闲时等待命令的超时（ms），用于在没有运动时也能发现报警 */
#define MOTION_IDLE_POLL_MS         (50)

/** 运动状态上报（motion_task -> UI，通过环形队列单向传递） */
typedef struct {
    float pos;        /*!< 当前位置（mm） */
    bool  moving;     /*!< 是否正在运动 */
    bool  arrived;    /*!< 本次运动到位（一次性，被读走即清） */
    bool  aborted;    /*!< 本次运动被中止（报警 / STOP，一次性） */
} motion_report_t;

/**
 * @brief 初始化运动控制（GPIO、RMT、报警中断、创建 motion_task）
 */
esp_err_t app_motion_init(void);

/**
 * @brief 把当前位置标定为 pos（操作员读刻度盘后输入 / Admin 改 Current Position）
 * @note 会立即停止当前运动，并清除反向间隙记忆（手动盘车后间隙状态未知）
 */
esp_err_t app_motion_set_current(float pos);

/**
 * @brief 下发目标位置（非阻塞，只入队）
 * @note 队列满时会丢弃最旧的一条，保证最新的目标一定被执行
 */
esp_err_t app_motion_move_to(float target);

/** @brief 立即停止（STOP 按钮）。停止后位置保持在当前值 */
void app_motion_stop(void);

/**
 * @brief 使能 / 脱机步进驱动器（ENA 低有效；`MOTION_ENA_ACTIVE_LEVEL = 0`）
 *
 * ★★★ 使能策略（2026-09-22 现场定稿：**"运行时一直低电平"**）★★★
 *
 * 与同现场 Arduino 版 `Mult_ESP32-S3_arduino_lvgl` **逐状态对齐**：
 *   | 状态 | 参考工程 | 本工程 |
 *   |---|---|---|
 *   | 上电 | `write_enable(false)`（高=失能） | `motion_gpio_init()` 先输出高 |
 *   | State1（输入刻度盘读数） | `case ST_CALIB: motor_set_enabled(false)` | 同 |
 *   | **State2（输入目标/运动）** | `case ST_TARGET: motor_set_enabled(true)` | **同（v19 新增）** |
 *   | State3（越限，要用手轮摇回） | —（沿用之前状态） | `set_enabled(false)`（要手盘） |
 *   | 报警 | `motor_set_enabled(false)` 抬 ENA | 同 |
 *
 * ★ 为什么 State2 要**一进去就使能、并且整个停留期间保持低**（而不是"发脉冲时才拉低"）：
 *   现场实测"把 IO13 强制拉低，电机就正常了" —— 说明 ENA 这一路**必须是一个稳定的低**。
 *   参考工程在 `ST_TARGET` 一进入就使能并一直保持，本工程原来只在 `motion_begin()`
 *   那一瞬间才拉低（运动结束也不抬，但**进入 State2 到第一次运动之间**、以及
 *   `run`/固定段结束（会主动抬 ENA 脱机）之后，驱动器都是失能状态），
 *   与参考版的行为并不一致 —— 现在对齐。
 */
void app_motion_set_enabled(bool enable);

/** @brief 报警输入当前是否处于有效电平 */
bool app_motion_is_alarm(void);

/** @brief 报警是否已锁存（需人工确认） */
bool app_motion_is_alarm_latched(void);

/**
 * @brief 清除报警锁存
 * @return ESP_ERR_INVALID_STATE 表示报警输入仍是有效电平（故障未排除，拒绝清）
 */
esp_err_t app_motion_clear_alarm(void);

/** @brief 取最新一条状态上报（非阻塞） */
bool app_motion_poll_report(motion_report_t *out);

/** @brief 取当前报告位置（mm） */
float app_motion_get_position(void);

/** @brief 取当前速度（mm/s），UI 只用于显示 */
float app_motion_get_velocity(void);

#ifdef __cplusplus
}
#endif
