#pragma once
#include "config.h"
#include <vector>

// 人物框框（ESP）：
//   1. 注入的 CounterStrikeHook.dll 扫到人之后，方框数据会经共享内存送过来，直接用（首选，
//      不用填任何链）；
//   2. 没有 DLL 数据时走老路：实体从实体列表链 / CT「所有人物」来，屏幕坐标用
//      g_esp_config.view_matrix 链读出的 4x4 视角矩阵投影；
//   3. 最后在一个透明、鼠标穿透的置顶窗口上用 GDI 画方框（队友绿框，其他人红框）。
//
// 返回这一帧画出来的方框个数；DLL 没数据又没配置视角矩阵时返回 0。
// EspLastEntities() 里是这一帧收集到的人物，界面上的雷达直接用它。
struct EspEntity
{
    float x = 0.0f;              // 世界坐标
    float y = 0.0f;
    float z = 0.0f;
    float hp = 0.0f;
    bool teammate = false;       // 是队友（决定方框颜色）
    bool teammate_known = false; // 阵营读到了才敢下结论
    bool from_crosshair = false; // 来自 CT「所有人物」的那条链
};

int EspTick(HWND game_window, const game_information& game, const role_offsets& offsets);
const std::vector<EspEntity>& EspLastEntities();
void EspShutdown();
