/**
 * @file ui_subpage.h
 * @brief 子界面公共框架：统一顶栏（返回键 + 标题 + 右上状态）+ 从右向左滑动返回
 *
 * 主菜单（ui_menu.c）是父级，LIGHT / ENVIRONMENT / HISTORY / SETTINGS /
 * WIFI SETUP 都是它的子界面，四件套完全一样：
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ (‹)    LIGHT CONTROL                       ON • 80%          │ y=0..57 白底
 *   ├──────────────────────────────────────────────────────────────┤
 *   │                        ... 各页自己的内容 ...                 │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 返回方式（两种，和 MOTOR TEST 子界面一致）：
 *   ① 点左上角圆形返回键   ② 在屏幕上从右向左快划（LVGL 手势）
 *
 * ★ 复用了主菜单那套视觉常量（UI_C_MENU_* / UI_MENU_*）：这两组界面在
 *   界面稿上是同一个设计语言（浅蓝灰底、白卡片、12px 圆角、#1565C0 强调色）。
 *
 * ★ 手势生效条件（与 ui_main.c 的实现同源，见那边注释）：
 *   被按下的对象不能是可滚动的；子对象全部带 LV_OBJ_FLAG_GESTURE_BUBBLE，
 *   手势会冒泡到屏幕本身。所以每页必须在自己**所有控件创建完之后**
 *   调用 ui_subpage_finish()。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 返回键/手势的处理函数 */
typedef void (*ui_subpage_back_cb_t)(void);

/** 子页句柄（**必须是静态/全局变量**：返回键回调要用它的地址） */
typedef struct {
    lv_obj_t *scr;               /*!< 屏幕对象 */
    lv_obj_t *lbl_status;        /*!< 右上角状态文字（小字，内置 Montserrat，可用 ° •） */
    char      last_status[48];   /*!< 状态文字缓存（每个页面一份，不能共用全局） */
    ui_subpage_back_cb_t back_cb;/*!< 返回行为；NULL = 回主菜单 */
    bool      created;
} ui_subpage_t;

/**
 * @brief 创建子页骨架（屏幕 + 顶栏 + 返回键）
 * @param out    调用方提供的存储，**必须是静态/全局变量**
 * @param title  居中标题（ASCII，粗体 P1）
 * @param status 右上角状态文字，可为 NULL
 */
void ui_subpage_create(ui_subpage_t *out, const char *title, const char *status);

/**
 * @brief 覆盖"返回"行为（默认回主菜单）
 * @note 用于多级子页：例如 WiFi 配网页的密码页，返回应该回到热点列表而不是主菜单。
 *       必须在 ui_subpage_finish() 之前调用。
 */
void ui_subpage_set_back_cb(ui_subpage_t *page, ui_subpage_back_cb_t cb);

/** @brief 更新右上角状态文字 + 颜色（内容没变就不碰 LVGL） */
void ui_subpage_set_status(ui_subpage_t *page, const char *text, lv_color_t color);

/** @brief 切到该子页 */
void ui_subpage_show(ui_subpage_t *page);

/**
 * @brief 创建白色圆角卡片（子页统一的容器样式：12px 圆角 + 1px #E3E7EC 边框）
 */
lv_obj_t *ui_subpage_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * @brief 把卡片变成"整块可点"的入口（内部装饰不再吃点击）
 * @note 必须在卡片内容全部创建完之后调用
 */
void ui_subpage_card_clickable(lv_obj_t *card, lv_event_cb_t cb, void *user_data);

/**
 * @brief 收尾：给整棵子树铺手势冒泡标志（所有控件创建完之后调用）
 */
void ui_subpage_finish(ui_subpage_t *page);

/** @brief 返回主菜单（返回键与手势都走这里） */
void ui_subpage_back_to_menu(void);

/*==============================================================================
 * 小工具（四个子界面通用，避免每页各写一份）
 *============================================================================*/
/** @brief 实心圆（不可点击、不参与手势冒泡，纯装饰） */
lv_obj_t *ui_subpage_dot(lv_obj_t *parent, int32_t x, int32_t y, int32_t d, lv_color_t color);

/** @brief 圆角实心矩形（装饰） */
lv_obj_t *ui_subpage_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t radius, lv_color_t color);

/**
 * @brief 只在文字真的变化时才写 label
 * @param cache 调用方提供的缓存（长度 >= n），每个 label 一份
 * @note lv_label_set_text() 内部是无条件 invalidate；子界面每 200~500ms 刷一次
 *       数值，不做这层比较的话静止画面也会一直重绘。
 */
void ui_subpage_set_text_cached(lv_obj_t *lbl, char *cache, size_t n, const char *text);

/*==============================================================================
 * 横向滑条（自绘图元，不用 lv_slider）
 *
 * 为什么不直接用 lv_slider：本子页有"从右向左滑动 = 返回"的手势，而 LVGL 的
 * 手势判定是**看位移**的 —— 在滑条上横向拖动，位移一超过 50px 就会被判成
 * 手势，手指还没松开就跳回主菜单了。所以滑条必须：
 *   ① 自己捕获 PRESSED/PRESSING 事件、直接把触点 x 换算成数值（不依赖滚动机制）
 *   ② 声明"不参与手势冒泡"（内部给捕获区打 LV_OBJ_FLAG_USER_1，
 *      ui_subpage_finish() 会跳过这棵子树，手势就只发给它自己、不会传给屏幕）
 * 视觉完全照界面稿：细轨道 + 蓝色填充 + 白底蓝圈圆钮（与主菜单 SETTINGS 卡片一致）。
 *============================================================================*/
/** 拖动回调：按住/拖动过程中连续触发（在 LVGL 任务里执行，禁止阻塞） */
typedef void (*ui_slider_cb_t)(uint8_t value, void *user_data);

typedef struct {
    lv_obj_t *root;        /*!< 触摸捕获区（整行，含上下左右留白） */
    lv_obj_t *fill;        /*!< 蓝色已填充部分 */
    lv_obj_t *knob;        /*!< 圆钮 */
    int32_t   track_x;     /*!< 轨道左端（屏幕坐标） */
    int32_t   track_w;     /*!< 轨道长度 */
    int32_t   knob_d;      /*!< 圆钮直径 */
    uint8_t   value;       /*!< 当前值 0..100 */
    uint8_t   shown;       /*!< 已画出来的值（缓存，避免重复设样式） */
    ui_slider_cb_t cb;     /*!< 拖动回调（可为 NULL） */
    void     *cb_user;
    bool      created;
} ui_slider_t;

/**
 * @brief 创建滑条
 * @param out   调用方提供的存储，**必须是静态/全局变量**（回调里要用它的地址）
 * @param x,y,w 轨道左上角与长度（屏幕坐标）；触摸区会在四周扩出去便于点按
 * @param value 初值 0..100
 */
void ui_slider_create(ui_slider_t *out, lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
                      uint8_t value, ui_slider_cb_t cb, void *user_data);

/** @brief 只更新视觉（外部改值后同步显示用；不会触发回调） */
void ui_slider_set_value(ui_slider_t *slider, uint8_t value);

/*==============================================================================
 * 圆环弧段（ENVIRONMENT 的两个仪表盘用）
 *
 * ★ 为什么不用 lv_arc：lv_arc 的角度约定是"0 在 3 点钟、顺时针为正"，
 *   但 start > end 时需要它内部自动 +360 归一化，跨版本行为不好保证；
 *   这里用**折线逼近**画圆环，角度自己算，行为完全确定（见 ui_subpage.c）。
 *   角度定义与 LVGL 一致：0° = 3 点钟方向，顺时针为正（屏幕 y 向下）。
 *============================================================================*/
#define UI_ARC_MAX_PTS   (49)

typedef struct {
    lv_point_t pts[UI_ARC_MAX_PTS];
    lv_obj_t  *line;
    bool       created;
} ui_arc_t;

/**
 * @brief 画一段圆环
 * @param out        调用方提供的存储，**必须是静态/全局变量**（折线点要长期有效）
 * @param cx,cy      圆心（屏幕坐标）
 * @param radius     圆环中心线半径
 * @param thickness  环宽（线宽）
 * @param start_deg  起始角（度）
 * @param sweep_deg  顺时针扫过的角度（度，正值）
 */
void ui_subpage_arc(ui_arc_t *out, lv_obj_t *parent, int32_t cx, int32_t cy, int32_t radius,
                    int32_t thickness, float start_deg, float sweep_deg, lv_color_t color);

/**
 * @brief 按新角度重画同一段弧（仪表盘指针/进度用；不重建控件）
 * @note 颜色与线宽沿用创建时的设置
 */
void ui_subpage_arc_update(ui_arc_t *arc, int32_t cx, int32_t cy, int32_t radius,
                           int32_t thickness, float start_deg, float sweep_deg);

#ifdef __cplusplus
}
#endif
