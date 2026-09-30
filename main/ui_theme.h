/**
 * @file ui_theme.h
 * @brief 全局视觉规范（浅色工业主题）+ 通用控件工厂
 *
 * 所有颜色与坐标均按参考界面稿 rollcoater_main_screen_v3.png 逐像素测量得到
 * （800x480，测量方法见 main/README.md「界面还原度」），不要凭感觉改动。
 *
 * ┌─ 测量得到的关键尺寸 ───────────────────────────────────────────────────┐
 * │ 顶栏       y=0..46，白底 #FFFFFF，下方 2px 分隔线 #CFCFCF               │
 * │ 读数卡片   x=15 w=362 h=138；卡片1 y=63，卡片2 y=215（间距 14）         │
 * │ 键盘       x=400 y=104，键 120x64，列距 8，行距 8（4 行 -> y=104..383） │
 * │ STOP       x=400 y=400 w=384 h=56                                       │
 * │ 提示行     文字居中于 x=592.5，大写高 17px                              │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * ┌─ 字体（重要）──────────────────────────────────────────────────────────┐
 * │ 界面稿里所有文字都是粗体，LVGL 内置 Montserrat 只有 Regular、也没有加粗 │
 * │ 开关，所以本工程挂 4 个**真粗体**字库（Lato Bold，见 main/fonts/ 与      │
 * │ ui_fonts.h）。它们的实测字形高度 vs 界面稿目标：                         │
 * │                                                                         │
 * │   ui_font_H1_bold  50px  字形高 36px   -> 读数大数字   （稿 46）        │
 * │   ui_font_H2_bold  44px  字形高 31px   -> 输入页大数字 （稿 —）        │
 * │   ui_font_H3_bold  35px  字形高 25px   -> 键面数字/STOP（稿 22 / 21）   │
 * │   ui_font_P1_bold  25px  字形高 18px   -> 标题/State/提示/卡片小标题    │
 * │                                            （稿 16 / 16 / 17 / 14）     │
 * │                                                                         │
 * │ 只有「读数大数字」比界面稿小（36 vs 46）：稿里那个数字约 66px，而手头  │
 * │ 最大的粗体是 50px。要完全一致就用 lv_font_conv 生成一个 64px 的，命名   │
 * │ 仍是 ui_font_H1_bold 覆盖同名文件即可，代码一行都不用改（见 README）。  │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * 现场可操作性：数字键 120x64、STOP 384x56，块间 8px，戴手套也能准确点按。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "ui_fonts.h"    /* ui_font_H1/H2/H3/P1_bold 真粗体字库声明 */

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * 颜色（测量值，勿随意改）
 *============================================================================*/
#define UI_C_PAGE_BG          lv_color_hex(0xEEEEEE)  /* 页面底色 */
#define UI_C_BAR_BG           lv_color_hex(0xFFFFFF)  /* 顶栏底（白） */
#define UI_C_BAR_LINE         lv_color_hex(0xCFCFCF)  /* 顶栏分隔线 / 卡片边框 */
#define UI_C_CARD_BG          lv_color_hex(0xFFFFFF)  /* 卡片底 */
#define UI_C_CARD_BORDER      lv_color_hex(0xCFCFCF)
#define UI_C_KEY_BG           lv_color_hex(0xE0E0E0)  /* 数字键底 */
#define UI_C_KEY_BACKSPACE    lv_color_hex(0xF9A825)  /* 退格键（橙） */
#define UI_C_KEY_ENTER        lv_color_hex(0x2E7D32)  /* Enter 键（绿） */
#define UI_C_ACCENT_GREEN     lv_color_hex(0x2E7D32)  /* 当前值 / Enter / 确定 */
#define UI_C_STOP             lv_color_hex(0xD32F2F)  /* STOP（红） */
#define UI_C_TEXT             lv_color_hex(0x212121)  /* 主文字 */
/* 「正在输入中」的数值用蓝色：和已生效的黑色(目标值)/绿色(当前值)都不一样，
 * 这样"输入缓冲区有没有内容""有没有被清空"在屏幕上一眼就能看出来。
 * 界面稿里没有这个状态（稿子是静态图），只在按键输入期间出现。 */
#define UI_C_VALUE_EDITING    lv_color_hex(0x1565C0)
#define UI_C_TEXT_DIM         lv_color_hex(0x616161)  /* State 文本 / 卡片标题 */
#define UI_C_TEXT_ON_DARK     lv_color_hex(0xFFFFFF)  /* 深底上的白字 */
#define UI_C_DISABLED         lv_color_hex(0x9E9E9E)

/* 整屏背景（随状态变化） */
#define UI_C_BG_NEUTRAL       lv_color_hex(0xEEEEEE)  /* 位置未标定（与界面稿一致） */
#define UI_C_BG_IN_POS        lv_color_hex(0x2E7D32)  /* |cur-tgt|<=0.005：绿 */
#define UI_C_BG_MOVING        lv_color_hex(0xD32F2F)  /* 运动中：红 */
#define UI_C_BG_WARN          lv_color_hex(0xF9A825)  /* 越限待确认：橙 */
#define UI_C_SCRIM            lv_color_hex(0x000000)

/* 兼容旧命名（其它模块仍在用） */
#define UI_C_SCREEN_BG        UI_C_PAGE_BG
#define UI_C_VALUE_CURRENT    UI_C_ACCENT_GREEN
#define UI_C_VALUE_TARGET     UI_C_TEXT
#define UI_C_KEY_ORANGE       UI_C_KEY_BACKSPACE
#define UI_C_KEY_GREEN        UI_C_KEY_ENTER
#define UI_C_KEY_FG_LIGHT     UI_C_TEXT_ON_DARK
#define UI_C_STOP_FG          UI_C_TEXT_ON_DARK
#define UI_C_BTN_GREEN        UI_C_ACCENT_GREEN
#define UI_C_BTN_GREY         lv_color_hex(0xD9D9D9)
#define UI_C_BTN_GREY_FG      UI_C_TEXT
#define UI_C_TEXT_MUTED       UI_C_TEXT_DIM

/*==============================================================================
 * 主菜单专用（界面稿 ui1_main.png，实现在 ui_menu.c）
 *
 * 主菜单与辊涂机主屏是**两张不同的设计稿**（浅蓝灰底 vs 纯灰底、
 * 卡片 12px 圆角 vs 10px、顶栏 58px vs 47px），所以颜色与几何另立一套，
 * 不要和上面的 UI_C_PAGE_BG / UI_CARD_* 混用。
 *============================================================================*/
#define UI_C_MENU_BG            lv_color_hex(0xEFF2F6)  /* 页面底（浅蓝灰） */
#define UI_C_MENU_BAR_LINE      lv_color_hex(0xE3E7EC)  /* 顶栏/底栏/卡片分隔线 */
#define UI_C_MENU_CARD_BORDER   lv_color_hex(0xE3E7EC)
#define UI_C_MENU_CARD_PRESSED  lv_color_hex(0xF1F5FA)  /* 卡片按下时的反馈底色 */
#define UI_C_MENU_BLUE          lv_color_hex(0x1565C0)  /* 风扇/湿度/滑条/返回键 */
#define UI_C_MENU_ORANGE        lv_color_hex(0xF57C00)  /* 灯泡/温度/曲线 */
#define UI_C_MENU_GREEN         lv_color_hex(0x2E7D32)  /* 「ON · 80%」 */
#define UI_C_MENU_MUTED         lv_color_hex(0x9AA0A6)  /* 「READY」/灰信号格 */
#define UI_C_MENU_TRACK         lv_color_hex(0xE0E0E0)  /* 滑条轨道 */
#define UI_C_MENU_BULB          lv_color_hex(0xFDEBC8)  /* 灯泡泡体 */
#define UI_C_MENU_TOAST_BG      lv_color_hex(0x323232)  /* 「未实现」提示条 */

#define UI_MENU_BAR_H           (58)    /* 主菜单顶栏：白底 0..57 */
#define UI_MENU_FOOT_Y          (436)   /* 主菜单底栏：436..479 */
#define UI_MENU_CARD_RADIUS     (12)

/*==============================================================================
 * 字体映射
 *============================================================================*/
/* 内置 Montserrat 只在两处用到：符号字形（Lato 没有）和小字提示 */
#if !LV_FONT_MONTSERRAT_16 || !LV_FONT_MONTSERRAT_32
#error "请检查 sdkconfig.defaults：需要 CONFIG_LV_FONT_MONTSERRAT_16 与 _32"
#endif

#define UI_FONT_VALUE         (&ui_font_H1_bold)   /* 主屏读数（字形高 36px） */
#define UI_FONT_NUMPAD_VALUE  (&ui_font_H2_bold)   /* 输入页大数字（31px） */
#define UI_FONT_KEY           (&ui_font_H3_bold)   /* 键面数字（25px） */
#define UI_FONT_ACTION        (&ui_font_H3_bold)   /* STOP / OK / Cancel（大写 25px） */
#define UI_FONT_TITLE         (&ui_font_P1_bold)   /* 顶栏标题（大写 18px） */
#define UI_FONT_STATE         (&ui_font_P1_bold)   /* 顶栏 State（大写 18px） */
#define UI_FONT_PROMPT        (&ui_font_P1_bold)   /* 提示行（大写 18px） */
#define UI_FONT_CAPTION       (&ui_font_P1_bold)   /* 卡片小标题（大写 18px） */
#define UI_FONT_ROW           (&ui_font_P1_bold)   /* Admin 参数名 */
#define UI_FONT_ROW_VALUE     (&ui_font_H3_bold)   /* Admin 参数值（25px） */
/* ⌫ / ↵ 是 LVGL 符号字形（U+F0xx），4 个 Lato 字库都不含，必须用内置字体 */
#define UI_FONT_SYMBOL        (&lv_font_montserrat_32)
#define UI_FONT_SMALL         (&lv_font_montserrat_16)

/*==============================================================================
 * 几何（界面稿测量值 + 由字体度量反算出的文本框）
 *============================================================================*/
#define UI_SCR_W              (800)
#define UI_SCR_H              (480)

#define UI_TOP_BAR_H          (47)     /* 0..46 白底 */
#define UI_TOP_BAR_LINE_Y     (47)     /* 47..48 分隔线（2px） */
#define UI_TOP_BAR_LINE_H     (2)

#define UI_PAD                (16)

/* 读数卡片 */
#define UI_CARD_X             (15)
#define UI_CARD_Y1            (63)     /* Motor's Current Position */
#define UI_CARD_Y2            (215)    /* Target Position */
#define UI_CARD_W             (362)
#define UI_CARD_H             (138)
#define UI_CARD_RADIUS        (10)
#define UI_CARD_BORDER_W      (2)
/* 卡片内部文本框：由「界面稿文字位置 + 字体 line_height/ascent」反算
 *   小标题：大写高 18px、ascent 21、line_height 25 -> 大写中心落在卡片内 27px
 *   读数  ：字形高 36px、ascent 41、line_height 50 -> 字形中心落在卡片内 81px */
#define UI_CARD_CAP_Y         (5)
#define UI_CARD_CAP_H         (44)
#define UI_CARD_VAL_Y         (53)
#define UI_CARD_VAL_H         (60)

/* 右侧区域：键盘 400..775，提示行与 STOP 400..783 */
#define UI_RIGHT_X            (400)
#define UI_PROMPT_Y           (65)
#define UI_PROMPT_W           (385)    /* 居中于 x=592.5 */
#define UI_PROMPT_H           (32)

#define UI_KEYPAD_X           (400)
#define UI_KEYPAD_Y           (104)
#define UI_KEY_W              (120)
#define UI_KEY_H              (64)
#define UI_KEY_GAP_X          (8)
#define UI_KEY_GAP_Y          (8)
#define UI_KEY_RADIUS         (6)
/* 键面文字在按钮里要上移一点，才与界面稿的数字位置一致
 * （lv_obj_center 居中的是「行盒」，数字视觉中心比行盒中心偏低） */
#define UI_KEY_LABEL_DY       (-3)
/* STOP 文字同样上移，对齐界面稿 */
#define UI_STOP_LABEL_DY      (-2)

#define UI_STOP_X             (400)
#define UI_STOP_Y             (400)
#define UI_STOP_W             (384)
#define UI_STOP_H             (56)
#define UI_STOP_RADIUS        (6)

/*==============================================================================
 * 通用控件工厂
 *============================================================================*/
/** @brief 初始化主题（只创建共享 style，必须在任何 UI 创建之前调用一次） */
void ui_theme_init(void);

/**
 * @brief 创建单行文本（超出宽度直接裁剪，不换行）
 * @param x,y,w,h 文本框；文字在 [x, x+w] 内按 align 水平对齐，并在高度 h 内垂直居中
 */
lv_obj_t *ui_label_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          const lv_font_t *font, lv_color_t color, lv_text_align_t align,
                          const char *text);

/**
 * @brief 创建「粗体」文本
 * @note 现在用的是真粗体字库（UI_FONT_* 指向 main/fonts 下的 Lato Bold），
 *       所以本函数等价于 ui_label_create()，只是保留 API 让调用点不用动。
 *       若哪天换回内置 Regular 字体，把 ui_theme.c 里的 UI_BOLD_EMULATE 打开即可
 *       用「1px 偏移幽灵副本」模拟粗体。
 */
lv_obj_t *ui_label_create_bold(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                               const lv_font_t *font, lv_color_t color, lv_text_align_t align,
                               const char *text);

/** @brief 更新「粗体」文本（开了 UI_BOLD_EMULATE 时主+幽灵一起改） */
void ui_label_set_text_bold(lv_obj_t *lbl, const char *text);

/** @brief 更新「粗体」文本为 "%.3f" */
void ui_label_set_value_bold(lv_obj_t *lbl, float value);

/** @brief 创建可换行的段落文本（弹窗正文用） */
lv_obj_t *ui_label_create_para(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                               const lv_font_t *font, lv_color_t color, lv_text_align_t align,
                               const char *text);

/**
 * @brief 创建按钮（含居中文字 label，按下时底色加深）
 * @note 点击回调挂在按钮本体上（LV_EVENT_CLICKED）
 */
lv_obj_t *ui_button_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           lv_color_t bg, lv_color_t fg, const lv_font_t *font,
                           const char *text, lv_event_cb_t cb, void *user_data);

/** @brief 取按钮内的文字 label（需要动态改文案时用） */
lv_obj_t *ui_button_get_label(lv_obj_t *btn);

/** @brief 把按钮内的文字垂直偏移 dy 像素（对齐界面稿基线用） */
void ui_button_move_label(lv_obj_t *btn, int32_t dy);

/** @brief 把按钮文字按“粗体”处理并整体上移 dy（真粗体下等价于 move_label） */
void ui_button_make_bold(lv_obj_t *btn, int32_t dy);

/** @brief 更新按钮文字（自动同步幽灵副本） */
void ui_button_set_text(lv_obj_t *btn, const char *text);

/** @brief 创建读数卡片：白底 + 2px 灰边 + 10px 圆角 + 无内边距 */
lv_obj_t *ui_card_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

/** @brief 创建纯色扁平容器（顶栏 / 遮罩 / 行容器） */
lv_obj_t *ui_panel_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          lv_color_t bg);

/** @brief 设置屏幕底色（状态色切换用） */
void ui_screen_set_bg(lv_obj_t *scr, lv_color_t color);

/** @brief 把浮点数格式化成 "%.3f" */
void ui_fmt_value(char *buf, size_t n, float value);

/** @brief 直接把浮点数写进普通 label */
void ui_label_set_value(lv_obj_t *lbl, float value);

#ifdef __cplusplus
}
#endif
