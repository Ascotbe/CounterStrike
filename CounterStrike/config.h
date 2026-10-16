#pragma once
#include <Windows.h>
#include <vector>
#include <cstdint>

// ============================== 说明 ==============================
// 本项目跟随 ascotbe 的《CSGO破解学习》系列（目标是 cstrike.exe 离线整合包），
// 用外部读写进程内存的方式实现一个练习用的修改器，带图形界面。
//
// 仓库 Ascotbe/Image 的 CSGO 目录下有两个客户端版本的 CT 表：
//   CSGO1.6-1 ~ CSGO1.6-4  ->  CS_v2728（仓库里 CSGO_v2728.7z 那份客户端）
//   CSGO1.6-5 ~ CSGO1.6-8  ->  CS_v3248（博客第五篇起更换的版本）
// 界面里可以自动识别版本，也可以手动指定。
//
// 仅用于离线客户端的学习与练手，不要拿到联机服务器上使用。
// ==================================================================

enum class GameVersion { CS_v2728, CS_v3248 };

// 一条偏移链，写法和 Cheat Engine 的指针一致：
//   "cstrike.exe+11069BC+7C+5F0+CC"  =>  { 0x11069BC, 0x7C, 0x5F0, 0xCC }
// 解析过程：模块基址加静态偏移 -> 读指针 -> 加偏移 -> 读指针 ... 最后一级加完偏移后不再读。
using OffsetChain = std::vector<uintptr_t>;

struct game_information
{
    HWND hwnd = nullptr;         // 游戏窗口句柄
    DWORD pid = 0;               // 进程 ID
    HANDLE handle = nullptr;     // 进程句柄
    uintptr_t base_address = 0;  // cstrike.exe 模块基址
};

// 链为空表示当前客户端版本没有收录这个偏移，程序会自动跳过对应功能
struct role_offsets
{
    // ---- 可写功能 ----
    OffsetChain attack_interval;          // 左键冷却（锁 -1 => 光速攻击）
    OffsetChain main_weapon;              // 主武器子弹
    OffsetChain no_recoil;                // 机枪无后座力 / 手枪连发
    OffsetChain blood_bar;                // 血量（单浮点）
    OffsetChain armor;                    // 护甲（单浮点）
    OffsetChain money;                    // 实际金钱
    OffsetChain money_display;            // 界面显示金钱
    OffsetChain move_speed;               // 移动速度（单浮点）
    OffsetChain shopping_everywhere;      // 无限购物
    OffsetChain slowed_by_attack;         // 被攻击减速（单浮点）
    OffsetChain install_bomb_everywhere;  // 随地下包
    OffsetChain perspective_switch;       // 视角切换
    OffsetChain immun_damage;             // 是否掉血，锁 0 = 免疫伤害（单浮点）

    // ---- 坐标（画雷达/方框用） ----
    OffsetChain x;
    OffsetChain y;
    OffsetChain z;

    // ---- 只读显示用的偏移（CT 表里的其他数据） ----
    OffsetChain weapon_mark;              // 正在使用武器标记（在用武器对象会变成 1）
    OffsetChain my_team;                  // 当前人物阵营：1 恐怖分子 2 反恐精英
    OffsetChain aim_target_type;          // 准星指向：0 墙 1 友 2 敌
    OffsetChain aim_target_index;         // 准星指向的角色编号
    OffsetChain team_static;              // 全局阵营地址
    OffsetChain bots_static;              // 机器人个数地址
    OffsetChain grenade;                  // 手雷地址（CT 表里是个指针，只做显示）
    OffsetChain enemy_hp;                 // CT「所有人物」指针的血量
    OffsetChain enemy_x;                  // CT「所有人物」指针的坐标
    OffsetChain enemy_y;
    OffsetChain enemy_z;
};

// 各功能锁定的值，想调整效果直接改这里
namespace trainer_values
{
    constexpr int   kMoney          = 16000;
    constexpr int   kBullets        = 30;
    constexpr float kHealth         = 200.0f;   // CT-2 里记录的锁定量
    constexpr float kArmor          = 100.0f;
    constexpr float kMoveSpeed      = 400.0f;   // 游戏默认是 250
    constexpr float kSlowdownFactor = 1.0f;     // 1 = 被攻击不减速
    constexpr int   kShopAnywhere   = 3;        // CT-5 里记录的锁定值
    constexpr int   kBombAnywhere   = 3;        // CT 表备注：改成 3 或者 2
    constexpr int   kPerspective    = 3;        // CT-4 里记录的锁定值
    constexpr float kFireCooldown   = -1.0f;    // 不开枪时该值就是 -1，锁住即可快速射击
    constexpr int   kNoRecoil       = 0;
    constexpr float kNoDamage       = 0.0f;     // 是否掉血 = 0 => 免疫伤害
}

// =========================== 人物框框（ESP） ===========================
// 方框透视要两样 CT 表里没有收录的东西，界面里可以直接填（不用重新编译）：
//   1. view_matrix —— 世界坐标转屏幕坐标的 4x4 视角矩阵（16 个 float）
//   2. entity_list —— 指向角色数组（或角色指针数组）的链，用来遍历所有人
// 两个都留空时，程序只画 CT「所有人物」指针指向的那个角色（见 use_crosshair_entity），
// 但依然需要 view_matrix 才能算出屏幕位置。
struct EspConfig
{
    bool enabled = true;               // 总开关（界面「显示人物方框」勾选框）
    bool use_crosshair_entity = true;  // 顺带画 CT「所有人物」指针指向的角色（不用扫描列表）
    OffsetChain entity_list;       // 实体列表链；为空 = 不遍历列表
    bool pointer_array = true;     // true：数组里存的是角色指针；false：数组里直接是角色结构
    int stride = 4;                // 相邻两个角色的字节间隔
    int max_count = 32;            // 最多遍历多少个
    OffsetChain view_matrix;       // 视角矩阵链（16 个 float，共 64 字节）
    int health_offset = 0x160;     // 角色结构内血量偏移（单浮点）
    int team_offset = 0x1C8;       // 角色结构内阵营偏移
    int origin_offset = 0x88;      // 角色结构内坐标偏移（连续 3 个 float）
    bool flip_y = false;           // 方框上下颠倒时改为 true
    int box_width = 34;            // 方框宽（像素）
    int box_height = 76;           // 方框高（像素）
    float max_distance = 6000.0f;  // 超过这个距离不画（de_dust2 这种大图别调太小，不然远处的人不画）

    // 外部「人物框框」画框用的颜色（和 DLL 游戏内方框共用界面上那两块框色）
    COLORREF color_enemy = RGB(255, 60, 60);
    COLORREF color_team = RGB(80, 220, 120);
    COLORREF color_unknown = RGB(255, 220, 60);
};

extern EspConfig g_esp_config;

// 按客户端版本填充偏移链
void IntoConfig(role_offsets* offsets, GameVersion version);
