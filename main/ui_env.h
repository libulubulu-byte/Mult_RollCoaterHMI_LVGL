/**
 * @file ui_env.h
 * @brief ENVIRONMENT 子界面（界面稿 ui3_env.png）—— SHT30/31 温湿度
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ (‹) ENVIRONMENT                        SHT3x • 2s            │
 *   ├──────────────────────────────────────────────────────────────┤
 *   │      TEMPERATURE                    HUMIDITY                 │
 *   │        ╭───────╮                      ╭───────╮              │
 *   │       │  26.5   │                    │   55    │              │
 *   │       │   °C    │                    │  %RH    │              │
 *   │        ╰───────╯                      ╰───────╯              │
 *   │ ┌────────────────────────┐ ┌──────────────────────────┐      │
 *   │ │ Today Min / Max        │ │ ● Status: COMFORTABLE    │      │
 *   │ │          24.1 / 28.3°C │ │   40-60% • 18-26°C       │      │
 *   │ └────────────────────────┘ └──────────────────────────┘      │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 仪表环按量程取比例：温度 0..40°C、湿度 0..100%RH（量程写死在 ui_env.c 里，
 * 换量程只改那两个宏）。舒适度判据也写死在 ui_env.c（40-60% 且 18-26°C）。
 *
 * 温度显示单位跟随 SETTINGS 里的 Temperature Unit（°C / °F）。
 * 传感器读不到时数值显示 "--"，右上角变成 "No sensor"。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_env_create(void);
void ui_env_show(void);
bool ui_env_is_shown(void);

#ifdef __cplusplus
}
#endif
