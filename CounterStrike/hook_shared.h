#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstdio>

// ==================== 训练器 <-> 注入 DLL 的共享内存 ====================
// 训练器附加到游戏后 CreateFileMapping 建一块共享内存，名字带着游戏进程号；
// 注入进去的 CounterStrikeHook.dll 用同样的名字打开，两边读写同一个结构：
//   训练器 -> DLL：设置（画不画方框、透视开不开、固定伤害多少...）
//   DLL -> 训练器：状态（钩上没钩上、画了多少帧、报错信息、找到的人物基址）
// 这样界面上的开关一改，游戏里立刻生效，DLL 出问题也能在界面上看到原因。
//
// 注意：结构体两边是同一份头文件编出来的，字段顺序/大小必须完全一致；
// 谁改了这个文件，训练器和 DLL 都要重新编译。

#define CSTRAINER_HOOK_MAGIC   0x4B485343u   // 'CSHK'
#define CSTRAINER_HOOK_VERSION 8u
#define CSTRAINER_HOOK_PIN_LOG 32
#define CSTRAINER_HOOK_MAX_CHAIN 8
#define CSTRAINER_HOOK_MAX_BOXES 64
#define CSTRAINER_HOOK_TEXT_LENGTH 256
#define CSTRAINER_HOOK_MODULE_LENGTH 64
#define CSTRAINER_HOOK_PATH_LENGTH 260

// 一条指针链（和 CT 表写法一样，最多 8 级）
struct HookChainView
{
    int32_t  count;
    uint32_t offsets[CSTRAINER_HOOK_MAX_CHAIN];
};

// 一个已经算好屏幕位置的方框（DLL 里算，训练器的外部「人物框框」直接拿去画）
struct HookOverlayBox
{
    volatile FLOAT left;
    volatile FLOAT top;
    volatile FLOAT right;
    volatile FLOAT bottom;
    volatile FLOAT hp;
    volatile LONG teammate_known;   // 1 = 知道是不是队友（不知道就画黄框）
    volatile LONG teammate;         // 1 = 队友
};

struct HookShared
{
    uint32_t magic;      // CSTRAINER_HOOK_MAGIC
    uint32_t version;    // CSTRAINER_HOOK_VERSION
    uint32_t size;       // sizeof(HookShared)，两边对不上就不干活

    // ---------------- 训练器写，DLL 读 ----------------
    volatile LONG settings_seq;          // 设置改了 +1，DLL 看到变化就重新读一遍
    volatile LONG enabled;               // Hook 总开关：0 = 什么都不做

    // ---- 游戏内人物方框（DLL 在游戏自己的画面上用 OpenGL 画） ----
    volatile LONG esp_enabled;           // 在游戏里画人物方框
    volatile LONG show_hp_text;          // 方框上写血量数字
    volatile LONG show_crosshair_entity; // 顺带画 CT「所有人物」指向的那个角色
    volatile LONG export_boxes;          // 训练器的外部「人物框框」要数据：DLL 每帧把算好的方框写回来
    volatile LONG flip_y;                // 画反了就开这个
    volatile LONG box_width;             // 方框宽（像素，还会按距离缩放）
    volatile LONG box_height;            // 方框高（像素）
    volatile FLOAT max_distance;         // 超过这个距离不画
    volatile LONG color_enemy;           // 颜色（COLORREF，用 GetRValue 之类拆出来）
    volatile LONG color_team;
    volatile LONG color_unknown;

    // ---- 实体列表（界面上「实体列表链」那一栏，可以为空 = 只画准星那个人） ----
    HookChainView entity_list;
    volatile LONG pointer_array;         // 数组里存的是角色指针？
    volatile LONG stride;                // 相邻元素间隔
    volatile LONG max_count;             // 最多遍历几个
    volatile LONG origin_offset;         // 角色结构内坐标偏移（连续 3 个 float）
    volatile LONG health_offset;         // 血量偏移
    volatile LONG team_offset;           // 阵营偏移

    // ---- 视角矩阵 / 坐标（游戏内画方框要自己算屏幕位置） ----
    HookChainView view_matrix;           // 4x4 视角矩阵链（16 个 float）
    HookChainView crosshair_x;           // CT「所有人物」的坐标（3 条链分开传）
    HookChainView crosshair_y;
    HookChainView crosshair_z;
    HookChainView crosshair_hp;          // CT「所有人物」的血量
    HookChainView my_x;                  // 我的坐标（算距离用，可以留空）
    HookChainView my_y;
    HookChainView my_z;
    HookChainView my_team;               // 我方阵营（决定方框颜色，可以留空）

    // ---- 透视（CT-8 的 LuaScript：渲染人物时把深度测试关掉） ----
    volatile LONG wallhack_enabled;      // 透视：渲染人物时关掉深度测试
    volatile LONG wallhack_aggressive;   // 更激进：所有三角形/四边形批次都关

    // ---- 固定伤害（CT-4 / CT-8 里那个脚本，打在 mp.dll 上） ----
    volatile LONG fixed_damage_enabled;
    volatile FLOAT fixed_damage_value;   // 每个子弹的伤害值，默认 200（0x43480000）
    wchar_t fixed_damage_module[CSTRAINER_HOOK_MODULE_LENGTH];  // 默认 mp.dll
    volatile LONG fixed_damage_offset;   // 默认 0x5ACA1

    // ---- 鼠标解锁（游戏窗口化时会一直把光标锁在窗口中间，这里把它屏蔽掉） ----
    volatile LONG release_mouse;         // 1 = 钩住 ClipCursor，让游戏锁不住光标

    // ---- 游戏里抓一张图（glReadPixels 抓游戏自己的帧缓冲，含 DLL 画的方框） ----
    volatile LONG screenshot_request;    // 训练器置 1 = 抓一张
    wchar_t screenshot_path[CSTRAINER_HOOK_PATH_LENGTH];  // 保存到哪里（BMP）

    // ---- 让 DLL 自己在游戏里找人物基址（这个客户端的静态地址经常和 CT 表不一样） ----
    volatile LONG autolocate;            // 1 = 每 2 秒扫一次，找到了就回报
    volatile LONG start_entity_scan;     // 预留：让 DLL 顺手扫实体列表

    // ---- 杂项 ----
    volatile LONG debug_marker;          // 画个调试十字，用来确认 hook 真的在渲染
    volatile LONG game_version;          // 0 = v2728，1 = v3248
    volatile LONG unload_requested;      // 训练器要卸载 DLL：DLL 摘掉 hook 后置 ready_to_unload

    // ---------------- DLL 写，训练器读 ----------------
    volatile LONG dll_loaded;            // DLL 跑起来了
    volatile LONG hook_installed;        // 装上了几个 hook（0 = 没装上）
    volatile LONG ready_to_unload;       // hook 已摘干净，DLL 正在自己卸载
    volatile LONG frames;                // wglSwapBuffers 被叫了多少次（渲染了多少帧）
    volatile LONG last_boxes;            // 上一帧画了几个框
    volatile LONG last_entities;         // 上一帧找到几个角色
    volatile LONG fixed_damage_on;       // 固定伤害补丁生效中
    volatile LONG wallhack_active;       // 透视生效过（关过深度测试）
    volatile LONG mouse_released;        // 鼠标解锁生效中（ClipCursor 被我们挡掉了）
    volatile LONG screenshot_done;       // 0 = 还没抓，1 = 抓好了，-1 = 抓失败
    volatile LONG clip_hook_installed;   // ClipCursor 钩子装上了没
    volatile LONG cursor_pin_calls;      // 引擎想「把光标拽回窗口中心」调了多少次 SetCursorPos
    volatile LONG cursor_pin_blocked;    // 其中被我们挡掉多少次（等于「光标被钉住」的次数）
    volatile LONG cursor_pin_last_x;     // 最近一次引擎要求移到的坐标（诊断用）
    volatile LONG cursor_pin_last_y;
    volatile LONG cursor_pin_center_x;   // 我们算出来的「窗口客户区中心」屏幕坐标（诊断用）
    volatile LONG cursor_pin_center_y;
    volatile LONG cursor_pin_win_center_x;   // 整个窗口（含标题栏）的中心，引擎也可能用这个
    volatile LONG cursor_pin_win_center_y;
    volatile LONG cursor_pin_log_pos;        // 下面环形日志写到第几条了
    volatile LONG cursor_pin_log_x[CSTRAINER_HOOK_PIN_LOG];   // 最近几次 SetCursorPos 的 X
    volatile LONG cursor_pin_log_y[CSTRAINER_HOOK_PIN_LOG];   // 最近几次的 Y
    volatile LONG cursor_pin_log_ret[CSTRAINER_HOOK_PIN_LOG]; // 调用者的返回地址（看是谁在复位光标）

    // ---- DLL 自己找到的人物链（训练器可以直接拿来当「本人」基址用） ----
    volatile LONG player_found;          // 0 = 还没找到
    volatile LONG player_scan_count;     // 扫过几轮
    volatile LONG player_static_offset;  // 静态地址（模块内偏移，和 CT 表一样的写法）
    volatile LONG player_module_base;    // 那个模块的基址（一般是 cstrike.exe）
    volatile LONG player_p1;             // 链上第一级指针
    volatile LONG player_p2;             // 第二级（+0x7C）
    volatile LONG player_p3;             // 第三级（+0x04）
    volatile FLOAT player_hp;            // p3+0x160
    volatile LONG player_money;          // p2+0x1CC
    volatile FLOAT player_armor;         // p3+0x1BC
    volatile LONG player_weapon;         // p2+0x5E8
    volatile LONG player_ammo;           // weapon+0xCC
    volatile FLOAT player_x;             // p3+0x08
    volatile FLOAT player_y;             // p3+0x0C
    volatile FLOAT player_z;             // p3+0x10
    volatile FLOAT camera_x;             // 从 GL 矩阵里算出来的相机位置
    volatile FLOAT camera_y;
    volatile FLOAT camera_z;
    volatile LONG matrix_found;          // 抓到过几次 GL 矩阵

    // ---- DLL 自己扫出来的角色（游戏内方框直接用这份数据，不用填实体列表链） ----
    volatile LONG esp_players;           // 一共扫到几个角色
    volatile LONG esp_self_found;        // 认出哪个是自己了没
    volatile LONG scan_ms;               // 最近一次分片扫描花了多少毫秒

    // ---- DLL 写：给训练器「人物框框」（外部透明覆盖层）用的方框，屏幕坐标已经算好 ----
    volatile LONG overlay_boxes_seq;     // 偶数 = 稳定，奇数 = 正在写
    volatile LONG overlay_boxes_count;   // 这一帧有几个框（没扫到人 / 没矩阵就是 0）
    HookOverlayBox overlay_boxes[CSTRAINER_HOOK_MAX_BOXES];

    wchar_t status_text[CSTRAINER_HOOK_TEXT_LENGTH];   // DLL 写给界面看的一句话
};

// 共享内存名字（按游戏进程号区分，训练器先建，DLL 打开）
inline void MakeHookMappingName(DWORD pid, wchar_t* buffer, size_t count)
{
    swprintf_s(buffer, count, L"Local\\CSTrainerHook_%u", static_cast<unsigned int>(pid));
}

// 固定伤害那个补丁打在 mp.dll 的这个偏移上（CT 表 CSGO1.6-4 / CSGO1.6-8 里验证过的地址）
// 原指令：fsub dword ptr [esp+54] / fstp dword ptr [eax+00000160]
// 字节：D8 64 24 54 D9 98 60 01 00 00
#define CSTRAINER_FIXED_DAMAGE_MODULE L"mp.dll"
#define CSTRAINER_FIXED_DAMAGE_OFFSET 0x5ACA1u
#define CSTRAINER_FIXED_DAMAGE_DEFAULT 200.0f
