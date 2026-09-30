# 多界面 HMI 生成 SOP —— 「界面图 + 功能规格」→ 能跑的多屏工程

> **给 AI 助手**：用户给你**一张或多张界面图 + 一段功能描述**、要一个 ESP32/LVGL 多界面工程时，按本文档逐步做，不要跳步、不要凭记忆写 API、不要猜引脚。
>
> 本文档是 `examples/get-started/Mult_RollCoaterHMI` 这个工程**实际跑出来的流程与踩坑记录**：
> 表里的每条"坑"都有对应的代码位置与日志判据，都是实测过的，不是推演。
>
> 同级同类文档：`ESP32-S3_arduino_lvgl/PORTING_GUIDE.md`（Arduino 侧，LovyanGFX + LVGL v9）。

---

## 0. 一句话总览

```
抄硬件基座 → 出映射清单并问歧义 → 分层搭骨架 → 先做"公共子页框架"
→ 一页一编译 → 接线与联调 → 交付三件套（代码 + 自检清单 + 可验证判据）
```

目标形态（本工程实例，5 个入口 + 1 个二级页 + 3 类弹窗）：

```
主菜单 MAIN MENU ──┬─> LIGHT CONTROL ──┐
  (ui_menu.c)      ├─> ENVIRONMENT     │ 全部是"子页"，
                   ├─> MOTOR TEST ─────┼─ 顶栏/返回/手势完全一样
                   │    └─ 长按Enter ─> Admin 参数屏（ui_admin.c）
                   ├─> HISTORY          │
                   └─> SETTINGS ───────┤
                        └─> WIFI SETUP ──┘   ← 二级子页（返回上一层，不是回主菜单）
弹窗（报警/确认/"重置中"）挂在 lv_layer_top()，换屏不丢
```

---

## 1. 输入与输出

| | 内容 |
|---|---|
| 输入 | ① N 张界面图（主菜单 + 各子页 + 弹窗）② 功能规格（文字）③ 硬件情况（板子型号、接了哪些外设） |
| 输出 | ① 能 `idf.py build` 通过、能跑的多屏工程 ② **映射清单**（图 ↔ 界面 ↔ 控件/坐标/文案） ③ README（含自检清单与判据） |
| 不输出 | 纯"看起来对"的代码。每条结论都要给「日志关键字 + 屏幕现象」两个判据 |

---

## 2. 第 0 步：映射清单（不许跳过）

逐张图标注，写清楚：

1. 这张图 = 哪个界面 / 哪个状态（主屏？子页？弹窗？操作后？）
2. 图上每个区块 → 控件类型 + 坐标 + 文案
3. 单独列三张清单：
   - **图上没有、但功能说明提到的**
   - **图与文字冲突的**（冲突时**以图为准**，但要在清单里指出来）
   - **数值格式与输入规则**（换算、位数、上限、首位 0、缓冲区满怎么办）

然后**一次性**用 `ask_followup_question`（≤4 题）把歧义问完，不要边写边问。

### 2.1 本类项目固定要问的 5 件事

| # | 问题 | 不问会怎样（真实教训） |
|---|---|---|
| 1 | 界面之间**怎么进、怎么回**？（返回键 / 手势方向 / 是否多级） | 「现有界面就算 X 子界面 + 左滑返回」这种组合自相矛盾（没有父级可回），必须问清 |
| 2 | 每个入口的**数据源**：真实硬件还是静态占位？ | 静态占位和接真传感器的实现量差 3 倍（后者要驱动 + 采样任务 + 环形缓冲） |
| 3 | **引脚 + 极性**：SDA/SCL、灯/继电器、高电平还是低电平有效、要不要 PWM 调光 | 参考工程给的引脚在本板可能已经被占（见 §4.4） |
| 4 | **持久化**：哪些项掉电要保留？（灯状态、亮度、温标、WiFi 凭证…） | 不持久化，用户每次都抱怨"又要重设" |
| 5 | 没有某能力时怎么显示？（如本工程**完全没有联网**，但 SETTINGS 稿上有 WiFi 行） | 会做出一行永远连不上的假状态 |

> 问之前**先自己查代码**：能从现有工程里查出来的（引脚、API、字体字符集）不要拿去问用户。

---

## 3. 第 1 步：抄硬件基座（绝不猜引脚）

1. 在工作区里找**同板子、已验证能跑**的参考工程（本工作区：`examples/get-started/SHT30_backled_5inch`
   —— 注意它名字里有 SHT30，**实际里面没有 SHT30 代码**，只是 RGB 屏 demo，别被名字误导）。
2. 从它抄：LCD 数据线/同步线/PCLK 时序、触摸 I2C 与地址、背光引脚与极性、分区表、`sdkconfig.defaults`。
3. 本板已验证的一套（`bsp_pins.h` 里有注释）：数据线
   `8,3,46,9,1,5,6,7,15,16,4,45,48,47,21,14`、PCLK=42、DE=40、VSYNC=41、HSYNC=39、背光=2（高亮）、
   触摸 GT911 `SDA=19/SCL=20@100k`、地址 `0x5D`（备用 `0x14`）。
4. **先跑一次 `idf.py build` 确认基座能编过**，再开始写业务代码。

---

## 4. 第 2 步：动手前必查的 5 件事

> 这 5 条每一次新工程都要重新查，**不要凭记忆**——每一条都是本工程踩过的。

### 4.1 字体字符集（最容易翻车）

粗体字库（本工程 Lato Bold）**只生成了 ASCII `0x20-0x7F`**。界面稿上的
`26.5°C` 的度符号、`ON · 80%` 的间隔点、`⌫`/`↵` 都会渲染成**空白或方框**。

| 想显示 | 正确做法 |
|---|---|
| `°` `•` | 用内置 Montserrat 的 label（`lv_font_montserrat_16.c` 头注释写明字符集是 `-r 0x20-0x7F,0xB0,0x2022`，含 U+00B0 度符号、U+2022 圆点），如本工程的 `UI_FONT_SMALL` |
| 间隔点 | 干脆**画**一个 6×6 实心圆点（`ui_menu.c` 的 `menu_dot()` / `ui_subpage_dot()`），配色随便改 |
| `⌫` `↵` | `LV_SYMBOL_*` 符号字形只能用内置 Montserrat（`UI_FONT_SYMBOL` = `lv_font_montserrat_32`） |
| 中文 | 当前 4 个字库都没有中文字形，要中文必须重新 `lv_font_conv` 生成并带 CJK 范围 |

**核实方法**（别猜）：看字库文件头部的生成参数注释，或直接 grep 字符集：
`lv_font_montserrat_16.c` 第 4 行就有 `-r 0x20-0x7F,0xB0,0x2022`。

### 4.2 手势能不能生效（LVGL 内部规则）

读 `lv_indev.c` 的 `indev_gesture()`：

- 被按下的对象**及其祖先**只要是"可滚动的"，拖拽就被判成滚动，**手势直接不检测**。
  → 所有容器/卡片/按钮都要 `lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE)`
  （本工程 `ui_theme.c` 的工厂函数已经统一做了）。
- 位移要 > 50px（`LV_INDEV_DEF_GESTURE_LIMIT`）且**横向分量大于纵向**。
- 每次采样的移动量要 > 3px（`LV_INDEV_DEF_GESTURE_MIN_VELOCITY`）→ **必须快划**，慢拖不触发。
- 方向约定：`gesture_sum.x < 0` → `LV_DIR_LEFT`（手指从右往左划）。
- 事件只发给"被按下的那个对象"，除它带 `LV_OBJ_FLAG_GESTURE_BUBBLE`（会逐级上抛，
  直到遇到**没有**该标志的祖先）。

**本工程的落地做法**（`ui_main.c` / `ui_subpage.c` 同一套）：
屏幕挂 `LV_EVENT_GESTURE` 回调；**所有子孙**加 `GESTURE_BUBBLE`；**屏幕自己不加** → 事件必定落到屏幕。
每页在**所有控件创建完之后**调用 `ui_subpage_finish()` 铺一次。

### 4.3 手势 vs 横向控件（滑条/可滚动列表）

| 冲突 | 现象 | 修法 |
|---|---|---|
| 滑条 | 在滑条上往左拖，位移一过 50px 就被判成"返回"，手指还没松就跳页 | 滑条自己捕获 `PRESSED/PRESSING`，用触点 x 算数值（不依赖滚动机制）；捕获区打 `LV_OBJ_FLAG_USER_1`，`ui_subpage_finish()` 会**跳过这棵子树** → 手势发给它自己、不传给屏幕。见 `ui_subpage.c` 的 `ui_slider_*` |
| 可滚动列表 | 滚动被清掉、列表滚不动 | 给列表容器打 `USER_1`（同上跳过）；代价是那块区域滑不动返回——**这是想要的行为**（列表区滑动=滚动列表），返回用左上角按钮 |

> `LV_OBJ_FLAG_USER_1..4` 是 LVGL 留给应用的标志位，本工程约定 **USER_1 = "不要参与手势冒泡"**。

### 4.4 引脚冲突与 PWM 通道冲突

1. 先把**本工程已占用的引脚**列全，再决定新外设接哪：
   本板 `LCD 数据 1/3/4/5/6/7/8/9/14/15/16/21/45/46/47/48`、`PCLK42/VSYNC41/HSYNC39/DE40/背光2`、
   `触摸 I2C0 19/20 + RST38`、`步进 12/13/17/18`、`PSRAM 33~37`、`串口 console 43/44`、`GPIO0 是 strapping`。
   → **真正常空的只有 GPIO10 / GPIO11**。
2. 参考工程的引脚**必须重新核对**：`lowpower_wake_openai_led_English/main/sht30.c` 用 `I2C1 SDA=12/SCL=13`，
   而本工程 **12/13 已经是步进 ENA/ALARM**；那个工程的 WS2812 灯在 `GPIO48`，本工程 **48 是 LCD 数据线 R2**。两个都不能照抄。
3. LEDC 通道要错开：屏幕背光占了 `LEDC_LOW_SPEED_MODE + TIMER_1 + CHANNEL_0`（`bsp_display.c`），
   灯就必须用 `TIMER_0 + CHANNEL_1`，否则 `ledc_channel_config` 会把背光通道抢过去（屏幕直接黑）。
4. 共用 I2C 总线是**可以**的：新 `i2c_master` 驱动内部对每条总线有 `bus_lock_mux` 信号量
   （`components/esp_driver_i2c/i2c_master.c` 的传输路径会 `xSemaphoreTake(bus_lock_mux)`），
   所以"触摸读坐标"和"传感器读温湿度"并发安全。
   **但自己那一路必须用有限超时**（本工程 `SENSOR_IO_TIMEOUT_MS=100`）：`esp_lcd` 触摸驱动用的是无限超时，
   万一总线被挂死，别把自己的采样任务也拖死。
   BSP 侧把总线句柄暴露出来即可：`bsp_display_i2c_bus()`（`bsp_display.h`）。

### 4.5 sdkconfig 里能力是否已开

要接 WiFi/SNTP 之前先 grep `sdkconfig`：本工程 `CONFIG_ESP_WIFI_ENABLED=y`、lwIP、`CONFIG_LWIP_SNTP_*` 都是开的，
所以**不需要改 sdkconfig**（改了还要提醒用户删 `sdkconfig` 才生效，能避免就避免）。
需要新的 `CONFIG_*` 时优先用 `#define`（如引脚都放 `bsp_pins.h` / 各 `app_*.h`），别动不动加 Kconfig。

---

## 5. 第 3 步：分层架构（照这张表建文件）

> 原则：**UI 不认识硬件，硬件不认识 UI**；中间靠 `app_*` 服务层。

| 层 | 文件（本工程实例） | 职责 | 严禁 |
|---|---|---|---|
| 显示/触摸 BSP | `bsp_display.c/h`、`bsp_pins.h` | RGB 面板、LVGL port、触摸、背光、暴露 I2C 总线 | 放业务逻辑 |
| 硬件服务 | `app_light.c/h`、`app_sensor.c/h` | 灯（GPIO+LEDC）、传感器（I2C+采样任务+统计） | 碰 LVGL |
| 配置/持久化 | `app_config.c/h`（机器参数）、`app_settings.c/h`（界面偏好） | NVS 读写、范围钳制、**异步落盘任务** | 在 LVGL 任务里写 flash |
| 网络服务 | `app_wifi.c/h` | STA 配网 + SNTP，事件队列抛给 UI | 在 LVGL 任务里扫描（阻塞 1~3s） |
| 主题/控件工厂 | `ui_theme.c/h` | 颜色/几何常量 + label/button/card/panel 工厂 | 写具体页面布局 |
| 子页框架 | `ui_subpage.c/h` | 顶栏（返回键+标题+右上状态）、手势、滑条、圆环、卡片、缓存写 | 写具体页面内容 |
| 页面 | `ui_menu/ui_main/ui_admin/ui_light/ui_env/ui_history/ui_settings/ui_wifi` | 只管布局 + 交互 + 刷新 | 直接操作 GPIO/NVS |
| 弹窗 | `ui_popup.c/h` | 二选一/单键/全屏报警，挂 `lv_layer_top()` | 各页各写一套弹窗 |

**装配顺序（`main.c`）**：

```
app_config_init()          // NVS（必须最先，其它模块的 NVS 依赖它）
bsp_display_init()         // 背光→面板→LVGL→触摸（I2C0 在这里建立）
app_motion_init()          // 运动（RMT/GPIO/报警中断）
app_state_init()           // 状态机
app_settings_init()        // 界面偏好（依赖 NVS）
app_light_init()           // 灯（依赖 app_settings 的持久化状态）
app_sensor_init()          // 传感器（依赖 bsp 的 I2C0）
app_wifi_init()            // 配网
  ── 进 LVGL 锁 ──
ui_theme_init(); ui_menu_create(); ui_main_create(); ui_admin_create();
ui_light_create(); ui_env_create(); ui_history_create(); ui_settings_create(); ui_wifi_create();
ui_menu_show();            // 最后统一决定"上电第一屏"，别让 create 顺序决定
  ── 出锁 ──
```

> 新增模块**不要**用 `fatal_hold()` 停机（除非影响安全）：没插传感器、没接天线都应该只告警、界面照常可用。

---

## 6. 第 4 步：公共子页框架（先做这个，后面每页都快）

界面稿里的子页顶栏几乎都一样，抽成 `ui_subpage.c`：

```
┌──────────────────────────────────────────────────────────────┐
│ (‹)    LIGHT CONTROL                       ON • 80%          │  y=0..57 白底
├──────────────────────────────────────────────────────────────┤
│                        ... 各页自己的内容 ...                 │
└──────────────────────────────────────────────────────────────┘
```

API（照抄即可）：

| 函数 | 用途 |
|---|---|
| `ui_subpage_create(&page, title, status)` | 建屏幕 + 顶栏 + 返回键（**page 必须是静态/全局变量**：返回键回调要用它的地址） |
| `ui_subpage_set_back_cb(&page, cb)` | 覆盖返回行为，用于二级子页（如 WiFi 密码页返回热点列表） |
| `ui_subpage_set_status(&page, text, color)` | 右上角状态（**缓存放在 page 里**，各页定时器都在跑，不能用同一个全局 static） |
| `ui_subpage_card/card_clickable/dot/rect` | 统一的卡片/装饰件 |
| `ui_slider_create(&slider, ...)` | 自绘滑条（避开手势冲突，见 §4.3） |
| `ui_subpage_arc/arc_update` | 折线逼近的圆环（仪表盘），角度自定义，不依赖 `lv_arc` 的跨版本行为 |
| `ui_subpage_set_text_cached(lbl, cache, n, text)` | 「值不变就不写」 |
| `ui_subpage_finish(&page)` | 所有控件建完后调用：铺手势冒泡 |

**关键常量**（来自界面稿测量）：顶栏高 58、返回键 `x=8 y=9 d=40`（白底 + 2px `#1565C0` 描边 + `LV_SYMBOL_LEFT`）、
标题框 `(56,0,688,58)` 居中、右上状态 `(580,0,204,58)` 右对齐（用内置 Montserrat 小字，可显示 `° •`）、
页面底 `#EFF2F6`、卡片白底 + 1px `#E3E7EC` + 12px 圆角。

---

## 7. 第 5 步：一页一编译（不要全写完再编）

```
① 后端（settings/light/sensor/wifi）+ 1 个页面 → build   ← 先验证后端 API 与头文件用法
② + 1~2 个页面 → build
③ + 剩下的页面 + 主菜单接线 → build
④ 文档（README/自检清单）与代码同步
```

编译命令（Windows PowerShell，一行）：

```powershell
& 'd:/ESP32-IDF/esp-idf-v5.5.1/export.ps1' | Out-Null; cd <工程目录>; idf.py build
```

> **`build/` 目录绑定工程路径**：如果工程是从别的目录改名/复制来的，`idf.py build` 会报
> `Build directory ... configured for project '<旧路径>' not '<新路径>'. Run 'idf.py fullclean'`。
> 不想删用户的 `build/` 就用 `idf.py -B build_verify build` 另开目录编译，验证完删掉即可。

**编译期最常见的两个拦路虎**（都是 `-Werror`）：

- `-Werror=format-truncation`：`snprintf` 往**定长字段**里写 `%s`（如把 SSID 拷进 `wifi_config_t.sta.ssid`）。
  修法：写个 `memcpy` 截断助手，别用 snprintf。
- `-Werror=unused-variable / unused-function`：删掉预留但没用的变量/函数（或确认 flag 里已 `-Wno-error`）。

---

## 8. 第 6 步：交付三件套

1. **代码**：中文注释，注释里写清"为什么这么做"（尤其是反直觉的地方）。
2. **README**：屏幕结构与导航图、界面还原度表、测试清单、与原始规格的差异表。
3. **每条结论的可验证判据**：**日志关键字 + 屏幕现象**两个都要，例如：

| 动作 | 日志 | 屏幕 |
|---|---|---|
| 点 MOTOR TEST | `ui_menu: MOTOR TEST tapped -> motor test screen` | 出现辊涂机界面，顶栏 `‹ MOTOR TEST` |
| 左滑返回 | `ui_main: Back to MAIN MENU (swipe)` | 回到 5 张卡片 |
| 熄屏 | `Auto screen off: idle 30012ms >= 30s -> backlight OFF, double-tap to wake` | 屏幕变黑 |
| 双击唤醒 | `Screen woken (double tap) -> backlight 70%` | 屏幕亮回 |

> 只写"应该好了"= 没交付。反直觉的行为（比如"慢拖不触发手势"）一定要写进文档，
> 否则下次会被当成 bug 折腾。

---

## 9. ★ 踩坑库（现象 → 根因 → 修法 → 判据）

| # | 现象 | 根因 | 修法 | 判据 |
|---|---|---|---|---|
| 1 | 点某张卡片"完全没反应"（连日志都没有） | 手势/滚动标志设置不当，或点击被上层可点对象吃掉 | 装饰件统一 `clear_flag(CLICKABLE|SCROLLABLE)`；卡片整块可点用"递归把子孙设成不接收点击" | 每个入口点击都有 `ui_menu: <项> tapped -> ...` |
| 2 | 卡片点了没动静、但日志有 | 反馈不可见（提示条没实现/在别的屏上） | 占位入口给屏幕内可见的提示（提示条/弹窗），**不要**用另一屏的提示行 | 屏幕下方出现提示条 2.2s |
| 3 | 界面偶尔"闪一下"、静止也在刷 | `lv_label_set_text()` **无条件** `lv_obj_invalidate()`；`lv_obj_set_style_*()` 也会重绘 | 所有周期刷新做「值不变就不写」；缓存必须**每页一份**（定时器是全局的） | 静止时零重绘（可看 PCLK/带宽，或加日志计数） |
| 4 | 从右向左滑"划了没反应" | 位移 < 50px / 速度 < 3px 每次采样（慢拖）；或按下的对象可滚动 | 文档写明"必须快划"；清 SCROLLABLE | `Gesture dir 2/3 ignored`（竖划）对照 `Back to MAIN MENU (swipe)` |
| 5 | 拖滑条时突然跳页 | 横向拖拽被判成返回手势 | 滑条用 `USER_1` 退出冒泡 + 自算触点 x | 拖滑条只打亮度日志、不跳页 |
| 6 | 列表滚不动 | 手势框架把容器的 `SCROLLABLE` 清了 | 列表容器打 `USER_1` 跳过 | 列表能上下滚，左上角按钮能返回 |
| 7 | 文案显示成空白/方框 | 用的字库没有该字形（Lato 只有 ASCII） | `° • ⌫ ↵` 换内置 Montserrat 或画图元 | 屏幕上字符正常 |
| 8 | 熄屏后双击唤醒，**屏幕不亮但日志是 `Backlight ON (0%)`** | BSP 的 `bsp_display_backlight(false)` 会把"记忆亮度"冲成 0，`(true)` 恢复的是 0% | `set_backlight_percent(0)` 只熄灭**不覆盖**记忆亮度；唤醒时显式用设置里的亮度点亮 | `Screen woken (double tap) -> backlight 70%` |
| 9 | 加了灯之后**屏幕黑** | 新 LEDC 通道与背光通道撞了（会让背光 GPIO 被重配） | 通道/定时器错开（背光 TIMER_1/CH0 → 灯 TIMER_0/CH1） | 背光日志 + 屏幕亮 |
| 10 | 传感器偶发读失败/整条 I2C 卡住 | 与触摸共用总线；触摸驱动用无限超时 | 自己那路用有限超时（100ms），失败计数 + 重试；预留"热插拔"重探测 | `read failed (1 in a row): ESP_ERR_TIMEOUT` 之后能自愈 |
| 11 | 某页在后台还在刷、或状态文字串页 | 定时器是全局的；缓存写成全局 static 被多页共用 | 每个 tick 先判 `lv_scr_act() == page.scr`；缓存放进 page 结构体 | 只在当前页时才有刷新日志 |
| 12 | 二级子页返回键跳回主菜单了 | 返回行为写死 | `ui_subpage_set_back_cb()` 覆盖 | `Back to ...` 日志层级正确 |
| 13 | 输入缓冲满了"按了没反应" | 静默丢弃 | 满时**必须打日志** + 边框变红等可见判据 | `password buffer full (63), key 'x' dropped` |
| 14 | 唤醒那一下误触了界面按钮 | 唤醒的触摸穿到下层 | 熄屏时在 `lv_layer_top()` 铺全屏遮罩吃触摸，双击才唤醒 | `Screen off: wake tap 1/2` → `Screen woken (double tap)` |
| 15 | 电平/极性搞反（灯反着亮） | 没问清高/低有效 | 引脚与极性做成宏（`LIGHT_ACTIVE_LEVEL`），占空比按极性反相 | 点 ON 灯亮 |

---

## 10. 界面图测量方法（怎么把图变成常量）

1. 图片按 1:1 像素矩阵读（本工程图都是 800×480，与屏幕同尺寸）。
2. 逐元素量：底色、顶栏高度与分隔线、卡片 `x/y/w/h/圆角/边框`、控件间距、字号（用大写字母高度反推）。
3. 量到的值写进**一个**头文件（本工程 `ui_theme.h`），并在注释里写"来源：<图名>"，
   以后换稿只改一处。
4. 容差 ±2px；把"测量值 vs 实现值"列成表放进 README（界面还原度表）。
5. 字号反算文本框 y/h：`label_create_internal()` 用 `font->line_height` 把文字行垂直居中，
   所以**换字号要重算 y/h**，否则文字整体偏上/偏下。

---

## 11. 文案与刷新纪律

- **所有界面文案集中**到 `app_text.h`（宏），改文案只改一处，也便于以后做多语言。
- 字符串必须能安全放进 label（ASCII + 上面允许的符号集合）。
- 周期刷新三定律：
  1. 值不变就不写（缓存）；
  2. 缓存**每页一份**；
  3. 非当前页不刷（`lv_scr_act()` 判断）。
- 禁止在 LVGL 任务里：写 flash（NVS）、`vTaskDelay` 大值、扫描 WiFi、等 RMT 发完。
  这些一律丢给独立任务/队列（本工程：`cfg_save`/`set_save`/`wifi`/`sensor` 任务 + `xQueueOverwrite` 覆盖式入队）。

---

## 12. 新建一个多屏工程：复刻清单（文件级）

| 文件 | 从哪来 | 说明 |
|---|---|---|
| `bsp_pins.h` / `bsp_display.c/h` | 抄同板参考工程 | 只改 LEDC 开关（`BSP_LCD_BL_USE_LEDC`）与暴露 I2C 总线 |
| `ui_theme.c/h` | 抄本工程 | 颜色/几何常量 + 控件工厂 |
| `ui_subpage.c/h` | 抄本工程 | **子页框架，最有复用价值**（顶栏/手势/滑条/圆环/缓存写） |
| `ui_menu.c/h` | 抄本工程 | 主菜单（卡片入口 + 顶栏时钟/信号格 + 底栏） |
| `ui_popup.c/h` | 抄本工程 | 三类弹窗 |
| `app_settings.c/h` | 抄本工程 | 界面偏好 NVS + 异步落盘 + 恢复出厂 |
| `app_light.c/h`、`app_sensor.c/h`、`app_wifi.c/h` | 抄本工程 | 按需保留；传感器驱动参数（`0x44`/`0x2400`/CRC-8 0x31）直接可用 |
| `main.c` | 抄本工程 | 装配顺序（§5）+ **不用 `ESP_ERROR_CHECK`**（失败别进重启循环） |
| `main/CMakeLists.txt` | 抄本工程 | `REQUIRES` 里按需加 `esp_wifi/esp_netif/esp_event/lwip` |
| 各 `ui_<页>.c/h` | 按新图新写 | 只做布局 + 交互 + 刷新 |

**可以直接搬运的"成品件"**：`ui_subpage.c` 的滑条与圆环、`ui_settings.c` 的自动熄屏+双击唤醒、
`ui_wifi.c` 的全键盘与配网流程、`app_sensor.c` 的 SHT30 驱动。

---

## 13. 新窗口开场提示词（直接复制粘贴）

```
我要在 examples/get-started/<新工程名>/ 做一个多界面 ESP32-S3 HMI（板子 JC8048W550 / ESP32-8048S050）。

【界面图】共 N 张（按 800x480 画）：
  ui1_main.png    = 主菜单（5 个入口卡片）
  ui2_xxx.png     = 子页 …（每张图属于哪个界面/状态写清楚）
  …
【功能说明】<把要做什么、每个入口的数据源、返回方式写清楚>

【要求】
1. 先按根目录 AGENTS.md §1 出「映射清单」，并用一次选择题把歧义问完（不要边写边问）。
2. 引脚/时序/分区表从 examples/get-started/SHT30_backled_5inch 抄，**不要猜**；
   新外设的引脚先核对本工程已占用引脚（可能只剩 GPIO10/11）。
3. 多界面照 examples/get-started/Mult_RollCoaterHMI/MULTI_SCREEN_UI_GUIDE.md 的流程做：
   先抄/建 ui_subpage.c（顶栏+返回键+手势+滑条），再一页一编译。
4. 交付：完整代码（中文注释）+ README（屏幕结构、自检清单、与规格的差异表）+
   每条结论的可验证判据（日志关键字 + 屏幕现象）。
5. 每个入口都要有反馈（还没做的入口要有可见提示），不要"点了没反应"。
6. 我说"你改代码、我来编译"时，不要执行 build。
```

---

## 14. 维护

- 代码改动后**同步更新本文档**（尤其踩坑库与常量）。
- 本文档与 `main/README.md` 的分工：README 讲**这个工程怎么用/怎么查故障**，
  本文档讲**怎么再生成一个同类工程**。
