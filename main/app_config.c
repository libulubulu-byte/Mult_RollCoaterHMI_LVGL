/**
 * @file app_config.c
 * @brief 机器参数的 nvs_flash 读写
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_config.h"
#include "app_text.h"

static const char *TAG = "app_config";

/* NVS 键名表：下标必须与 app_cfg_field_t 一一对应
 * （键名保持稳定，不要为了改出厂值而改名 —— 出厂值的升级走下面的
 *   APP_CFG_FACTORY_VERSION 迁移机制。） */
static const char *const s_nvs_keys[APP_CFG_FIELD_COUNT] = {
    "upper_lim",
    "lower_lim",
    "accel",
    "decel",
    "speed",
    "pos",
    "backlash",
};

/* 出厂值迁移版本号所在的 NVS 键（见 APP_CFG_FACTORY_VERSION） */
#define APP_CFG_NVS_KEY_FACTORY_VER  "fac_ver"

/*==============================================================================
 * 出厂默认值
 *
 * ★★★ 最新结论（v11，2026-09-22 深夜）：**默认仍是 12000Hz（现场指定），
 *     并新增 `f` / `run` / `stop` 三条命令做同参数对照** ★★★
 *
 *   现场要求（原话）："**f 12000 + run + stop 的命令添加上，默认还是 12000HZ**"。
 *   于是 v11 做两件事：
 *     ① 出厂 `speed` 恢复到 **15.000mm/s = 12000Hz**（`fac_ver=11` 强制迁移一次）；
 *     ② 本工程命令行新增 **`f <hz>` / `run` / `stop`**（`app_motion.c` 的 CLI 段）——
 *        与台架 `examples/peripherals/rmt/stepper_motor` 那三条**语义一一对应**：
 *        `f` 设频率（**运行中也能改**）、`run` **单向连续**发脉冲直到 `stop`。
 *     用法就是台架那三条：`f 12000` → `run` → 看/听 → `stop`。
 *
 *   ⚠️ 下面 v10 的台架结论**仍然成立，别丢**：默认用 12000Hz 可以，但**若现场看到
 *      "来回摆/发毛"，先按"超出上沿、电机失步"处理**（不是接线错、不是软件换向）：
 *        · 想稳：`f 8500` 试转对照，或把 Admin 的 Speed 降到 10.625mm/s（=8500Hz）；
 *        · 想找上沿：`f 9000` / `f 10000` … 一格一格试，**从哪档开始发毛，上一档就是上沿**。
 *
 * ★★ 上一条结论（v10，2026-09-22 深夜）：**工作点回到 8500Hz —— 12000Hz 会失步 ★★★
 *
 *   现场实测（在 `examples/peripherals/rmt/stepper_motor` 台架上，用 `f <hz>` + `run`）：
 *
 *       `f 12000` + `run`（**单向**连续运行，台架的 run 不带参数就是 RUN_CONT、DIR 不变）
 *           ⇒ "**转了，但是会左右转**" —— 即转子在该频率上**反复失步、来回摆**。
 *       `f 8500` + `run`（台架上电固定段就是它，跑 10 秒）
 *           ⇒ **连续平稳转**（现场已多次确认）。
 *
 *   ★ 换算一下就很清楚（本机细分 1600 脉冲/圈）：
 *        8500 ÷ 1600 = **5.3 转/秒**  ← 带得动（下沿）
 *       12000 ÷ 1600 = **7.5 转/秒**  ← **带不动**（超出转矩-转速能力 ⇒ 失步来回摆）
 *     ⇒ **可用窗口很窄：约 [8500, ~10000] Hz**，"越大越快"不成立，
 *       越过上沿的表现**不是"更快"，是"原地来回摆"**（很容易误判成"方向错/软件乱发脉冲"）。
 *     ★ 台架的 `run` **不会换向**（只有 `auto` 扫频到顶/到底才 `set_dir()`），
 *       所以"左右转"只可能是失步，不是软件在发反向脉冲。
 *
 *   ⇒ 该 v10 当时把默认值改成 10.625mm/s（=8500Hz）；**v11 已按现场要求改回 15.000mm/s
 *     （=12000Hz）**，但 8500Hz 依然是"连续平稳"的参考档，随时可以用 `f 8500` 对照。
 *     · 软启动仍**默认关闭**（v15 起曲线第一拍就是巡航频率）；`accel/decel` 保持 150mm/s²。
 *
 *   ★ 想确认上沿到底在哪：台架里 `f 9000` / `f 10000` / `f 11000` + `run` 各跑 10 秒，
 *     **从哪一档开始"发毛/来回摆"，上一档就是上沿**（把结果告诉开发即可调整出厂值）。
 *   ★ 往上调速度必须重新验证：`8500Hz` 是"能转"，不代表 `10000Hz` 能转。
 *
 * ★★ 上一条结论（v9，2026-09-22 深夜）：曾按现场"f 12000 直接就能起来"把出厂值设成
 *    12000Hz —— ★ **v10 据台架实测把它压回 8500Hz；v11 又按现场要求恢复 12000Hz**
 *    （"持续跑会失步"这条结论仍然成立，只是默认值按现场意见走）。留档以免再犯。
 *
 * ★★ 上一条结论（v8，2026-09-22 深夜）：照抄 stepper_motor 例程已验证的 8500Hz ★★
 *
 *   `examples/peripherals/rmt/stepper_motor` 的诊断台上电固定段 =
 *   「使能后**直接把频率推到 8500Hz、连跑 10 秒**」，现场确认**能连续正常转**；
 *   而主工程在同一拨码下"一步不动" ⇒ 差别就在**曲线的频率区间**：
 *   例程整段都待在 8500Hz，主工程从 0 慢慢升、末尾又降到 0（那段全在禁区里）。
 *
 * ★★ 更早一条结论（v7，2026-09-22 深夜）：**"低频不走、高频才转" ⇒ 巡航频率必须待在可用带内** ★★
 *
 *   现场事实（拨码 = 能跑通的那套：细分 `OFF ON OFF` = 1600 脉冲/圈、电流 `ON OFF OFF` = 2.0A）：
 *        **8500Hz 才转** —— 低于它一步都不走，只有嗡嗡响。
 *
 *   ★ 这与 v6 的"1200Hz 以上开始抖"**是同一件事的两种刻度**：
 *       v6 那次扫频用的细分是 `ON ON OFF` = 200 脉冲/圈：1200 ÷ 200 = **6.0 转/秒**；
 *       本次 1600 脉冲/圈：8500 ÷ 1600 = **5.3 转/秒**。
 *     ⇒ 这台机器只在 **≳5~6 转/秒（≈320~360 RPM）** 时才带得动，以下全是低频禁区
 *       （换算口径：`f_turn ≈ 5.3 × 驱动器脉冲每圈`）。
 *     ★ 所以 v6 的 "上限 1000Hz + 出厂 1mm/s(=800Hz)" 在这套拨码下**一步都不动** ——
 *       它们都落在禁区里。那不是"慢"，是"不动"。
 *
 *   ⇒★★★ v8 出厂值（照抄**现场唯一被直接验证过的工作点**）★★★
 *
 *   决定性证据来自同工作区的 `examples/peripherals/rmt/stepper_motor`（已改成诊断台）：
 *   它的**上电固定段**是「使能后直接把频率推到 **8500Hz**、连跑 **10 秒**」，
 *   现场确认**这一段能连续正常转**（用户原话："开头 10s 是可以正常运行的"）。
 *
 *   于是 v8 把主工程做成**与那一段同构**：
 *     · 出厂 `speed`：20mm/s(=16000Hz，未验证) → **10.625mm/s = 8500Hz**（已验证点）；
 *     · `MOTION_TURN_FREQ_MIN_HZ`：9000 → **8500**（就是例程验证的那个频率）；
 *     · `motion_run_profile()` 的曲线**整体抬到 8500Hz 以上**：
 *         起步第一拍 = 8500Hz（不再从 0/50Hz 慢慢升）；
 *         收尾减速到 8500Hz 就结束（不再降到禁区里）。
 *       ⇒ 效果上等价于例程的"上电即 8500Hz 连跑"，同时保留了梯形曲线的位置精度。
 *     · 出厂 `accel`/`decel` 仍取 **150mm/s²**：只在操作员把 Speed 调高时才起作用，
 *       用来快速跨过 8500Hz 以上那段（不再是"跨过禁区"）。
 *     · `MOTION_STEP_FREQ_MAX_HZ` = 20000（与 Arduino/例程的 FREQ_MAX 一致）：
 *       ★ 只有 8500Hz 被验证过，往上调请先用 `mtest 10000/12000/16000` 试。
 *
 *   ★ 一句话操作建议：**不要把 Speed 往低调** —— 在这台机器上"慢"就等于"不动"。
 *   ★ 行程限制**已随 v8 消失**：曲线不再从 0 起步，短行程也能转
 *     （原来"总行程 <0.85mm 到不了起跳频率"那条适用于 v7 的旧曲线）。
 *
 * ---- 以下是 v1~v6 的排查留档。注意 v6 的"上限 1200Hz"只在 200 脉冲/圈那套拨码下成立，
 *      不要直接套到 1600 脉冲/圈上；但 v5 的"照抄 Arduino 的 20mm/s"是对的方向。----
 *
 * ★★★ 上一条结论（v6，2026-09-22 深夜）：**瓶颈就是"频率 = 转速"** ★★★
 *
 *   现场用「阶梯扫频」实测量出（把官方示例 examples/peripherals/rmt/stepper_motor
 *   改成 50Hz→1500Hz、每 50Hz 一档、完整加减速、50% 方波、每档 0.5s）：
 *        ≤1200Hz    连续平稳转动
 *        1200~1500   开始嗡嗡抖 / 丢步
 *   ⇒ 这台机器（当前拨码 + 24V + 该电机 + 带载）的**真实上限约 1200Hz**。于是：
 *     · app_motion.h 的 MOTION_STEP_FREQ_MAX_HZ：20000 → **1000**（留 20% 余量）；
 *     · 出厂 speed：20mm/s → **1.0mm/s**（= 800Hz，上限的约 80%）；
 *     · 出厂 accel/decel：150 → **4mm/s²**（= 3200Hz/s，起步温和）。
 *
 *   ★ 关键换算：**20mm/s = 16000 steps/s = 上限的 13 倍**。所以"界面里一下发运动
 *     就嗡嗡抖"不是波形/脉宽/代码的问题，是**软件在逼电机超速**；这也解释了为什么
 *     官方示例(1500Hz)、Arduino 版、本工程在本机都会抖 —— 都在往上限之上要速度。
 *   ★ 想更快只能改硬件：驱动器供电 24V→36V（对高速转矩最有效）、电流档到电机额定、
 *     换更大电机/驱动器（DM542 等）；另外先确认丝杠/导轨不别劲、联轴器同心。
 *   ★ 换电机、换驱动器、改细分拨码、改供电后，必须重新扫一次频（或用 mtest），
 *     再把 MOTION_STEP_FREQ_MAX_HZ 与出厂 speed 一起改。
 *
 * ---- 以下是 v1~v5 的排查留档。其中"频率不是瓶颈"那条**已被 v6 推翻**：
 *      它是在"旧拨码 + 无加减速的扫描自检"下得到的，条件不成立。----
 *
 * ★ 2026-09-22 现场"电机只震动不转圈"的阶段性结论（过程见 main/README.md 9.7）：
 *
 *   硬件：4 线两相步进（42BL233）+ TB6600 + 22V。**频率不是瓶颈** ——
 *   现场用本工程的频率扫描自检逐档跑 200Hz→16kHz，**每一档都正常转圈**
 *   （含 16kHz = 20mm/s），所以"频率太高"的假设被排除。
 *   真正被改掉的是**脉冲波形与脉宽**：
 *     · 一个 RMT 事务从"1 个符号 + loop_count 循环"改成**摆满 n 个符号、不循环**；
 *     · STEP/DIR/ENA 驱动能力拉满（脉宽最终与现场参考波形对齐在 5µs）。
 *   这两条一起改的，事后无法单独归因，但都是更稳妥的方向，**不要再改回去**。
 *
 *   参数取值：speed 20mm/s（= 16000 steps/s = 16kHz 脉冲，与现场"参考波形"的
 *   15.5kHz 对齐 —— 那版是 20mm/s，频率方面逐项一致）；accel/decel 60mm/s²
 *   （0.33s 到满速，比最初的 150mm/s² 温和）。
 *   ★ 20mm/s 是扫频自检验证过的（16kHz 那一档正常转圈），要更慢在 Admin 里往下调；
 *     带载跑 20mm/s 若出现失步，先降到 10 或 5 再排查机械/电流。
 *
 * ★★ 2026-09-22 晚 v5：**撤销 v4 的"按厂商 0.5A 降速"，回到 20mm/s** ★★
 *
 *   现场最新事实（最有说服力的一条）：**同一台机器、同一套驱动器+电机+接线，
 *   Arduino 版固件能正常跑通**，而它那套能跑通的拨码是
 *   **「只有 SW2、SW4 是 ON」** = `SW1..SW3 = OFF ON OFF`、`SW4..SW6 = ON OFF OFF`。
 *   按驱动器外壳上的表反查：
 *     · 细分 `OFF ON OFF` ⇒ **1600 脉冲/圈**（8 细分）—— 与厂商文档一致 ✅；
 *     · 电流 `ON OFF OFF` ⇒ **2.0A**（峰值 2.2A）—— **厂商文档写的 0.5A 是错的，差 4 倍** ❌；
 *     · 也就是说我们前面几轮按 0.5A 拨的时候，**电流只有 1/4**；
 *     · 而 Arduino 出厂速度是 **20mm/s**（= 16000 脉冲/s @800 步/mm），本工程 v4 退成了
 *       2mm/s（1600 脉冲/s，慢 10 倍）→ 正好落在步进电机的**低速共振区**：
 *       电流本来就不够 + 频率又低，转子跟不上就**原地来回微抖**。
 *
 *   所以 v5 直接**照抄能跑通那版的三个运动参数**（board_config.h:432-434）：
 *     ① `MOTOR_SPEED_DEFAULT = 20`、`MOTOR_ACCEL/DECEL_DEFAULT = 150` —— 逐项一致：
 *        · 20mm/s 在**同一台机器上被 Arduino 版验证过能跑**；
 *        · accel 取 150（而不是更温和的 60/30）：0→20mm/s 只需 **0.133s**，
 *          **快速"跨过"步进电机的低速共振带**。加速越慢，电机在共振区停留越久 ——
 *          "低速 + 电流不足"正是原地来回微抖的成因之一（v4 的 2mm/s 就落在那里）。
 *        · 变速距离 = 1.33mm(加速) + 1.33mm(减速) ≈ **2.67mm**：**行程短于它就到不了满速**
 *          （三角形曲线），现场测转速请用 **≥5mm** 的行程。
 *     ② 拨码以"**能跑通的那套**"为准：**只有 SW2、SW4 是 ON**
 *        （= `OFF ON OFF | ON OFF OFF`：**1600 脉冲/圈（8 细分）+ 2.0A**，
 *          现场 Arduino 版用的就是这套）。★ 厂商文档写的 0.5A 是**错的**（要 2.0A，
 *          差 4 倍）——我们前几轮按 0.5A 拨，正是"只抖不转"的直接原因；
 *          细分那部分（1600 脉冲/圈）与厂商文档一致。
 *        （main/README.md 9.2.1 ③ 已按实测重写，以那张表为最终依据）
 *
 *   ★ 关于 v4（保留为历史，别再照它改）：
 *     厂商文档原文（现场提供）「SW1/SW2/SW3 为 1600 细分、SW4/SW5/SW6 为 0.5A 输出电流
 *     （注意设置的输出电流不能大于等于电机的额定电流），否则极容易出现电流过大导致
 *     电机运转中振动、发烫现象」——这份文档与现场实测**矛盾**，只当参考。
 *     它唯一仍成立的结论是：细分脉冲数是**脉冲/圈**，步数/mm 还要除导程（见 9.5）。
 *
 *   ★ 本条只重置 speed/accel/decel（fac_ver 迁移），**限位/位置/间隙一律不动**。
 *     现场不重编也能立刻试：在 Admin 里把 Speed 改成 20.000、accel 改成 60。
 *
 * ★ APP_CFG_FACTORY_VERSION：出厂值迁移版本
 *   NVS 里存着上一次的值时，会盖掉这里的新出厂值。所以 app_config_init()
 *   启动时会比对 NVS 里的 fac_ver，**版本更旧就把"出厂参数"（speed/accel/decel）
 *   强制写成新出厂值**，而限位 / 位置 / 反向间隙保持不动（已标定的坐标系不受影响）。
 *   这样现场不需要 erase-flash、也不需要进 Admin 手工改。
 *   以后每次调整出厂默认值，把版本号 +1 即可。
 *      v1：20mm/s,150mm/s² → 2mm/s,30mm/s²（现场"只震不转"调试期的保守值）
 *      v2：2mm/s,30mm/s²   → 10mm/s,60mm/s²（确认硬件正常后的可用值）
 *      v3：10mm/s,60mm/s²  → 20mm/s,60mm/s²（与现场参考波形 15.5kHz 对齐；脉宽也改回 5µs）
 *      v4：20mm/s,60mm/s²  → 2mm/s,30mm/s²（当时按厂商"1600 脉冲/圈 + 0.5A"口径退的，
 *                                        **已被现场实测推翻**：能跑通的是 SW4=OFF(≥2.5A)）
 *      v5：2mm/s,30mm/s²   → 20mm/s,150mm/s²（**照抄 Arduino 能跑通那版的
 *         MOTOR_SPEED/ACCEL/DECEL_DEFAULT = 20 / 150 / 150**，理由见上面 v5 那段）
 *      v6：20mm/s,150mm/s² → **1mm/s,4mm/s²**（依据是 2026-09-22 现场阶梯扫频实测：
 *         这台机器在 **1200Hz 以上开始抖**，上限约 1200Hz。脉冲上限同步从 20000
 *         收到 1000（见 app_motion.h 的 MOTION_STEP_FREQ_MAX_HZ 处的长注释）；
 *         出厂速度取 800Hz = 1.0mm/s，加速率 4mm/s² = 3200Hz/s，起步温和。）
 *         ★ 注意：这条结论是在**另一种细分拨码**（`ON ON OFF` = 200 脉冲/圈）下测的；
 *           换到 1600 脉冲/圈后它**不成立**（同一个转速对应的 Hz 数差 8 倍），
 *           于是被判为"低频禁区" —— 见本文件顶部的 v7 结论。
 *      v7：1mm/s,4mm/s²   → **20mm/s,150mm/s²**（依据：当前拨码下低频不转、
 *         **8500Hz 才转**；`MOTION_STEP_FREQ_MAX_HZ` 回到 20000、新增
 *         `MOTION_TURN_FREQ_MIN_HZ` 作巡航下限。★ 20mm/s = 16000Hz **没被验证过**，
 *         v8 已把它换成验证过的 8500Hz。）
 *      v8：20mm/s,150mm/s² → **10.625mm/s,150mm/s²**（= **8500Hz** @800 步/mm，
 *         直接取 `stepper_motor` 例程上电固定段那个**现场已验证能连续转**的点；
 *         同时把运动曲线整体抬到起跳频率以上 —— 起步第一拍就是 8500Hz、
 *         收尾也停在 8500Hz，不再进出低频禁区。**不要再往低调 Speed**。）
 *      v9：10.625mm/s → **15mm/s（= 12000Hz）**、软启动默认关闭（依据：现场实测
 *         "12000Hz 直接起跳就能起来"，且低频爬升会"卡壳/左右抖动"——
 *         见 README 9.12.5 ③）。8500Hz 仍是**可用下沿**（巡航下限不变）。
 *         ★ **该值已被 v10 推翻**：12000Hz 持续跑会失步来回摆。
 *      v10：15mm/s → **10.625mm/s（= 8500Hz）**（依据：台架 `f 12000` + `run` 单向
 *         连续运行时"会左右转" = 12000Hz 失步；而 `f 8500` + `run` 连续平稳 ——
 *         即 **12000Hz 超出这台机器的可用上沿**，8500Hz 才是唯一被验证的点。
 *         换算：12000÷1600 = **7.5 转/秒**（带不动），8500÷1600 = **5.3 转/秒**（能带）。）
 *      v11：10.625mm/s → **15mm/s（= 12000Hz）**（**现场要求默认值仍是 12000Hz**，
 *         以便用新增的 `f 12000` + `run` + `stop` 命令与台架做同参数对照；
 *         台架那条"12000 会失步"的结论保留，看到来回摆就 `f 8500` 对照或降 Speed。）
 *      v12：15mm/s → **10.625mm/s（= 8500Hz）**（一度把 8500Hz 当成"唯一验证过的
 *         交付值"—— 因为当时台架 `f 12000` + `run` 被记录成"左右转"。见 v13 的更正。）
 *      v13：10.625mm/s → **15mm/s（= 12000Hz）**（★★ **现场复测更正**：
 *         **串口 `run`（单向连续）在 12000Hz 下"转得很好"**；反倒是界面按 8500Hz 跑时
 *         **"启动后左右转、跟震动一样"**。⇒ 之前 v10/v12 把"左右转"归给 12000Hz 是
 *         **记反了**：8500Hz 恰好是这台机器"能转"的**下沿**（临界点，空载能过、带载发毛），
 *         12000Hz 才是离下沿有余量、能连续平稳转的工作点。⇒ 界面工作点对齐 `run`。）
 *============================================================================*/
#define APP_CFG_FACTORY_VERSION  (13)

#define CFG_DEF_UPPER_LIM   50.000f
#define CFG_DEF_LOWER_LIM    0.000f
#define CFG_DEF_ACCEL      150.000f
#define CFG_DEF_DECEL      150.000f
/* ★★ 12000Hz ÷ 800 步/mm = 15.000 mm/s ★★ （v13：**交付值 / 界面工作点**）
 *
 *  ★★★ 为什么是 12000Hz（现场复测结论，优先级最高）★★★
 *    · **串口 `run`（单向连续发脉冲）在 12000Hz 下"转得很好"** —— 现场原话；
 *    · 而界面（状态机 → motion_run_profile）按 **8500Hz** 跑时，现象是
 *      **"启动后左右转、跟震动一样"**。
 *    ⇒ **8500Hz 恰好卡在这台机器"能转"的下沿**（临界点：空载/台架能过，
 *      带载就发毛、失步来回摆）；**12000Hz 离下沿有余量，能连续平稳转**。
 *      所以 v10/v12 把"左右转"归罪于 12000Hz 是**记反了**，v13 更正：
 *      **界面工作点直接对齐 `run` = 12000Hz**。
 *
 *  ★ 曲线（软启动关闭，见 app_motion.h 的 MOTION_SOFT_START_HZ）：
 *      起步 **12000Hz** → 巡航 **12000Hz** → 收尾减速到 **8500Hz**（= 下沿）。
 *      起步/巡航与 `run` 完全同频；收尾停在 8500Hz 是因为低于它的脉冲电机不走，
 *      只会白丢位置（见 MOTION_TURN_FREQ_MIN_HZ）。
 *  ★ 刹车距离 = (12000² − 8500²) / (2 × 120000) ≈ **299 步 ≈ 0.37mm** ——
 *    行程比它短会在 motion_begin() 打 WARN 并可能末端滑步；要精确走更短的行程
 *    就把 Admin 的 Speed 调低（刹车距离随 v² 缩短）。
 *
 *  ★ 这个数是"频率"倒推出来的，不是拍脑袋的 mm/s：本工程的位置/速度都按
 *    步数/mm 换算，所以**改 MOTION_STEPS_PER_UNIT 或换拨码后，这里要按
 *    12000 / steps_per_unit 重算**（见 README 9.12）。 */
#define CFG_DEF_SPEED       15.000f
#define CFG_DEF_POS          0.000f
#define CFG_DEF_BACKLASH     0.200f

static app_config_t s_cfg;
static QueueHandle_t s_save_q = NULL;

/**
 * @brief 落盘任务：把队列里最新的一份参数写进 NVS
 * @note 独立任务、优先级低于 LVGL 任务，避免在 UI 线程里做几十毫秒的 flash 写
 */
static void cfg_save_task(void *arg)
{
    (void)arg;
    app_config_t cfg;

    for (;;) {
        if (xQueueReceive(s_save_q, &cfg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        nvs_handle_t nh;
        if (nvs_open(APP_CFG_NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open for save failed");
            continue;
        }
        for (app_cfg_field_t f = 0; f < APP_CFG_FIELD_COUNT; f++) {
            float v = app_config_get_field(&cfg, f);
            nvs_set_blob(nh, s_nvs_keys[f], &v, sizeof(float));
        }
        esp_err_t werr = nvs_commit(nh);
        nvs_close(nh);
        ESP_LOGI(TAG, "Config committed to NVS (%s)", esp_err_to_name(werr));
    }
}

/*==============================================================================
 * 基础访问
 *============================================================================*/
app_config_t app_config_defaults(void)
{
    app_config_t cfg = {
        .upper_lim = CFG_DEF_UPPER_LIM,
        .lower_lim = CFG_DEF_LOWER_LIM,
        .accel = CFG_DEF_ACCEL,
        .decel = CFG_DEF_DECEL,
        .speed = CFG_DEF_SPEED,
        .pos = CFG_DEF_POS,
        .backlash = CFG_DEF_BACKLASH,
    };
    return cfg;
}

app_config_t app_config_get(void)
{
    return s_cfg;
}

float app_config_get_field(const app_config_t *cfg, app_cfg_field_t field)
{
    if (cfg == NULL || field >= APP_CFG_FIELD_COUNT) {
        return 0.0f;
    }
    switch (field) {
    case APP_CFG_UPPER_LIMIT:      return cfg->upper_lim;
    case APP_CFG_LOWER_LIMIT:      return cfg->lower_lim;
    case APP_CFG_ACCELERATION:     return cfg->accel;
    case APP_CFG_DECELERATION:     return cfg->decel;
    case APP_CFG_SPEED:            return cfg->speed;
    case APP_CFG_CURRENT_POSITION: return cfg->pos;
    case APP_CFG_BACKLASH:         return cfg->backlash;
    default:                       return 0.0f;
    }
}

void app_config_set_field(app_config_t *cfg, app_cfg_field_t field, float value)
{
    if (cfg == NULL || field >= APP_CFG_FIELD_COUNT) {
        return;
    }
    switch (field) {
    case APP_CFG_UPPER_LIMIT:      cfg->upper_lim = value; break;
    case APP_CFG_LOWER_LIMIT:      cfg->lower_lim = value; break;
    case APP_CFG_ACCELERATION:     cfg->accel = value; break;
    case APP_CFG_DECELERATION:     cfg->decel = value; break;
    case APP_CFG_SPEED:            cfg->speed = value; break;
    case APP_CFG_CURRENT_POSITION: cfg->pos = value; break;
    case APP_CFG_BACKLASH:         cfg->backlash = value; break;
    default: break;
    }
}

const char *app_config_field_name(app_cfg_field_t field)
{
    switch (field) {
    case APP_CFG_UPPER_LIMIT:      return APP_TXT_CFG_UPPER_LIMIT;
    case APP_CFG_LOWER_LIMIT:      return APP_TXT_CFG_LOWER_LIMIT;
    case APP_CFG_ACCELERATION:     return APP_TXT_CFG_ACCELERATION;
    case APP_CFG_DECELERATION:     return APP_TXT_CFG_DECELERATION;
    case APP_CFG_SPEED:            return APP_TXT_CFG_SPEED;
    case APP_CFG_CURRENT_POSITION: return APP_TXT_CFG_CURRENT_POS;
    case APP_CFG_BACKLASH:         return APP_TXT_CFG_BACKLASH;
    default:                       return "?";
    }
}

/*==============================================================================
 * 校验（在 UI 任务里同步执行，只做比较，绝不阻塞）
 *============================================================================*/
const char *app_config_validate(const app_config_t *cfg, app_cfg_field_t field, float value)
{
    if (cfg == NULL) {
        return APP_TXT_ERR_EMPTY;
    }

    switch (field) {
    case APP_CFG_UPPER_LIMIT:
        if (value < cfg->lower_lim) {
            return APP_TXT_ERR_UPPER_BELOW_LOWER;
        }
        break;

    case APP_CFG_LOWER_LIMIT:
        if (value > cfg->upper_lim) {
            return APP_TXT_ERR_LOWER_ABOVE_UPPER;
        }
        break;

    case APP_CFG_ACCELERATION:
    case APP_CFG_DECELERATION:
    case APP_CFG_SPEED:
        if (value <= 0.0f) {
            return APP_TXT_ERR_GREATER_ZERO;
        }
        break;

    case APP_CFG_BACKLASH:
        if (value < 0.0f) {
            return APP_TXT_ERR_NOT_NEGATIVE;
        }
        break;

    case APP_CFG_CURRENT_POSITION:
        if (value < cfg->lower_lim || value > cfg->upper_lim) {
            static char s_err[64];
            snprintf(s_err, sizeof(s_err), APP_TXT_ERR_RANGE_FMT, cfg->lower_lim, cfg->upper_lim);
            return s_err;
        }
        break;

    default:
        break;
    }
    return NULL;
}

/*==============================================================================
 * NVS 读写
 *============================================================================*/
static esp_err_t nvs_load_or_default(nvs_handle_t h, app_cfg_field_t field, float *out)
{
    size_t len = sizeof(float);
    float v = 0.0f;
    esp_err_t err = nvs_get_blob(h, s_nvs_keys[field], &v, &len);
    if (err == ESP_OK && len == sizeof(float)) {
        *out = v;
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* 首次上电：把默认值写进去，之后就不会再走这里 */
        nvs_set_blob(h, s_nvs_keys[field], out, sizeof(float));
        ESP_LOGI(TAG, "NVS key '%s' missing -> default %.3f", s_nvs_keys[field], *out);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "read '%s' failed: %s", s_nvs_keys[field], esp_err_to_name(err));
    return err;
}

/**
 * @brief 出厂参数迁移：NVS 里的出厂版本比固件新出厂值旧时，强制刷成新出厂值
 *
 * 只重置 speed / accel / decel —— **限位 / 位置 / 反向间隙一律不动**，
 * 所以不会破坏现场已经标定好的坐标系。
 * 目的：现场换固件后不必 erase-flash、也不必进 Admin 逐项改
 *       （换固件后 NVS 里还是上一次的 20mm/s，会盖掉新出厂值）。
 *
 * @return true = 发生了迁移（需要 commit）
 */
static bool nvs_migrate_factory_params(nvs_handle_t h)
{
    int32_t ver = 0;
    if (nvs_get_i32(h, APP_CFG_NVS_KEY_FACTORY_VER, &ver) != ESP_OK) {
        ver = 0;    /* 老固件没写过这个键 */
    }
    if (ver >= APP_CFG_FACTORY_VERSION) {
        return false;
    }

    static const app_cfg_field_t reset_fields[] = {
        APP_CFG_ACCELERATION, APP_CFG_DECELERATION, APP_CFG_SPEED,
    };
    const app_config_t def = app_config_defaults();

    ESP_LOGW(TAG, "出厂参数升级 v%d -> v%d：Speed/Accel/Decel 重置为出厂值"
                  "（限位/位置/反向间隙保持不动）",
             (int)ver, (int)APP_CFG_FACTORY_VERSION);
    for (size_t i = 0; i < sizeof(reset_fields) / sizeof(reset_fields[0]); i++) {
        const float v = app_config_get_field(&def, reset_fields[i]);
        nvs_set_blob(h, s_nvs_keys[reset_fields[i]], &v, sizeof(float));
        app_config_set_field(&s_cfg, reset_fields[i], v);
        ESP_LOGW(TAG, "  %s = %.3f", s_nvs_keys[reset_fields[i]], v);
    }
    nvs_set_i32(h, APP_CFG_NVS_KEY_FACTORY_VER, APP_CFG_FACTORY_VERSION);
    return true;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), erasing...", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    s_cfg = app_config_defaults();

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(APP_CFG_NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open failed");

    bool need_commit = false;
    for (app_cfg_field_t f = 0; f < APP_CFG_FIELD_COUNT; f++) {
        float v = app_config_get_field(&s_cfg, f);
        if (nvs_load_or_default(h, f, &v) == ESP_ERR_NOT_FOUND) {
            need_commit = true;
        }
        app_config_set_field(&s_cfg, f, v);
    }

    /* ★ 出厂参数迁移：换固件后 NVS 里的旧速度会盖掉新出厂值，这里按版本号纠一次。
     *   只动 speed/accel/decel，限位/位置/反向间隙不受影响。 */
    if (nvs_migrate_factory_params(h)) {
        need_commit = true;
    }
    if (need_commit) {
        nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: upper=%.3f lower=%.3f accel=%.3f decel=%.3f speed=%.3f pos=%.3f backlash=%.3f",
             s_cfg.upper_lim, s_cfg.lower_lim, s_cfg.accel, s_cfg.decel,
             s_cfg.speed, s_cfg.pos, s_cfg.backlash);

    /* 落盘任务：低优先级，NVS 写不阻塞 UI */
    if (s_save_q == NULL) {
        s_save_q = xQueueCreate(1, sizeof(app_config_t));
        ESP_RETURN_ON_FALSE(s_save_q != NULL, ESP_ERR_NO_MEM, TAG, "save queue create failed");
        BaseType_t ok = xTaskCreate(cfg_save_task, "cfg_save", 4096, NULL, 3, NULL);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "save task create failed");
    }

    return ESP_OK;
}

esp_err_t app_config_commit_async(const app_config_t *cfg)
{
    if (cfg == NULL || s_save_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 先把内存值切过去，UI 立刻按新参数工作；落盘由任务异步完成 */
    s_cfg = *cfg;
    if (xQueueOverwrite(s_save_q, cfg) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_config_save_position_async(float pos)
{
    s_cfg.pos = pos;
    if (s_save_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueOverwrite(s_save_q, &s_cfg) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
