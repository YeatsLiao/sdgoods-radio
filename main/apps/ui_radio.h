/*
 * sdgoods-radio · 圆屏 FM 表盘应用（应用层）
 *
 * 参考 leo-radio 的 dial/needle 组织思路，用谷仓平台 LVGL 重写：
 *   弧形刻度带（87~108 MHz）+ 指针 + 台名/频率 + 频谱电平条 + 播放状态。
 *   手势：点按 = 播放/暂停；左右滑动 = 上/下一台；下拉 = 控制中心（应用外壳）。
 *   开机后台拉电台目录，成功后自动播放首台；播放失败自动跳下一台（限连跳次数）。
 *
 * 与占位 flappy 一样接入「应用标准框架」（sdgoods_app_shell）。
 */

#pragma once

#include <stdbool.h>

/* 进入电台应用（建屏 + 起目录加载任务 + 接应用外壳）。boot-direct 首屏即调它。 */
void ui_radio_start(void);

/* 每帧轮询：刷新状态/指针/频谱，处理跳台。非前台立即返回。 */
void ui_radio_poll(void);

/* 电台应用是否在前台。 */
bool ui_radio_is_open(void);

/* 请求退出电台应用（走应用外壳标准退出，停播 + 释放）。 */
void ui_radio_close(void);
