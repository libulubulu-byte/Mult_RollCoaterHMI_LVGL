/**
 * @file ui_wifi.h
 * @brief WiFi 配网页（SETTINGS -> WiFi -> Configure）
 *
 * 两级页面：
 *   ① 热点列表页（WIFI SETUP）
 *      ┌────────────────────────────────────────────────────────┐
 *      │ (‹) WIFI SETUP                        Not configured    │
 *      ├────────────────────────────────────────────────────────┤
 *      │ Current: Not configured              [ FORGET ]        │
 *      │ [ SCAN ]                                               │
 *      │ ┌────────────────────────────────────────────────────┐ │  可滚动
 *      │ │ HomeNet_2.4G                              -52      │ │
 *      │ │ TP-LINK_5G                                -71      │ │
 *      │ └────────────────────────────────────────────────────┘ │
 *      └────────────────────────────────────────────────────────┘
 *   ② 密码页（PASSWORD）—— 点上面某个热点进来
 *      ┌────────────────────────────────────────────────────────┐
 *      │ (‹) PASSWORD                          HomeNet_2.4G      │
 *      ├────────────────────────────────────────────────────────┤
 *      │ Network: HomeNet_2.4G                        8/63      │
 *      │ ┌────────────────────────────────────────────────────┐ │
 *      │ │ mypass123                                          │ │
 *      │ └────────────────────────────────────────────────────┘ │
 *      │  1 2 3 4 5 6 7 8 9 0                                   │
 *      │  q w e r t y u i o p                                   │
 *      │  a s d f g h j k l -                                   │
 *      │ [⇧] z x c v b n m . [⌫]                                │
 *      │ [      SPACE      ] [ CONNECT ] [ CANCEL ]             │
 *      └────────────────────────────────────────────────────────┘
 *
 * ★ 键盘字符集（够用但有限，密码含其它符号请在别处改、或先用字母数字）：
 *     SHIFT 关：a-z 0-9 - .       SHIFT 开：A-Z 0-9 _ @
 *   密码最多 63 字符（WPA2 上限），**缓冲区满时按键会被丢弃并打日志**：
 *     `password buffer full (63), key 'x' dropped`
 *
 * ★ 返回层级：密码页的返回键/手势回到热点列表页；列表页的才回主菜单
 *   （靠 ui_subpage_set_back_cb，见 ui_subpage.h）。
 *
 * ★ 列表容器故意标了 LV_OBJ_FLAG_USER_1：它要能上下滚动，
 *   不能被"手势冒泡"框架清掉 SCROLLABLE（清了就滚不动了），
 *   所以在那块区域上滑动不会触发返回 —— 想返回请点左上角按钮。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_wifi_create(void);

/** @brief 进配网页（默认显示热点列表） */
void ui_wifi_show(void);

/** @brief 当前是否在配网页（列表页或密码页） */
bool ui_wifi_is_shown(void);

#ifdef __cplusplus
}
#endif
