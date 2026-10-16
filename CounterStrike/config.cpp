#include "config.h"

// 偏移全部来自教程对应各篇的 CT 表（Ascotbe/Image 仓库 -> CSGO -> CSGO1.6-N.CT），
// 格式与 CT 表里的描述一致，例如 "cstrike.exe+11069BC+7C+5F0+CC" 就是
// { 0x11069BC, 0x7C, 0x5F0, 0xCC }。

void IntoConfig(role_offsets* offsets, GameVersion version)
{
    if (version == GameVersion::CS_v2728)
    {
        // ================= v2728（CT 表 CSGO1.6-1 ~ CSGO1.6-4） =================
        // 下面是 CT 表里作者那份客户端的原始静态地址：cstrike.exe+0xD6E63C（人物）/ +0xC7ABC0
        // （所有人物）。在我们手上这份 Esai 2738 客户端里这几个静态地址全是 0，人物链整条是空的，
        // 实测能用的静态地址是 cstrike.exe+0x1100ABC（DLL 里「自动找基址」扫出来的，坐标和摄像机
        // 对得上，确定是「我自己」）。注意 cstrike.exe+0x102D480 也能过人物指纹，但它指的是别的
        // 角色，拿它锁金钱会锁到别人身上（实测踩过这个坑），所以这里没用它。
        // 链的形状和 CT 表一模一样：静态 -> p1 ; [p1+7C] = p2 ; [p2+04] = p3 ; p3+160 = 血量。
        // 所以这里直接填实测值（CT 表原值写在各行注释里），换客户端时点界面上的「自动找基址」重扫。
        offsets->blood_bar           = { 0x1100ABC, 0x7C, 0x04, 0x160 };  // 血量（CT 表是 0xD6E63C，实测 100.0）
        offsets->armor               = { 0x1100ABC, 0x7C, 0x04, 0x1BC };  // 护甲（实测 100.0）
        offsets->money               = { 0x1100ABC, 0x7C, 0x1CC };        // 金钱（实测 800）
        offsets->main_weapon         = { 0x1100ABC, 0x7C, 0x5E8, 0xCC };  // 主武器子弹（实测 20）
        offsets->no_recoil           = { 0x1100ABC, 0x7C, 0x5E8, 0x100 }; // 无后座力 / 手枪连发
        offsets->shopping_everywhere = { 0x1100ABC, 0x7C, 0x3C4 };        // 无限购物（实测读出来 0）
        offsets->slowed_by_attack    = { 0x1100ABC, 0x7C, 0x1B0 };        // 被攻击减速（实测 1.0，锁 1 就是不被减速）
        offsets->my_team             = { 0x1100ABC, 0x7C, 0x1C8 };        // 阵营（实测 2 反恐精英 / 1 恐怖分子）
        offsets->x                   = { 0x1100ABC, 0x7C, 0x04, 0x08 };   // 坐标（单浮点三连）
        offsets->y                   = { 0x1100ABC, 0x7C, 0x04, 0x0C };
        offsets->z                   = { 0x1100ABC, 0x7C, 0x04, 0x10 };
        // 下面这几条：静态地址用实测的（见上面），偏移用 CT 表里同一个字段的形状，
        // 挨个读了一遍值都对得上（写在注释里），所以一起放出来给 F5/F7/F8/F9 用。
        offsets->move_speed          = { 0x1100ABC, 0x290 };              // 移动速度（实测 230.0，CS 默认最大速度）
        offsets->attack_interval     = { 0x1100ABC, 0x7C, 0x5E8, 0xB8 };  // 左键冷却（实测 -1.0，和 CT 表里锁的值一样）
        offsets->install_bomb_everywhere = { 0x1100ABC, 0x7C, 0x3C0 };    // 随地下包（实测 0，形状和 CT 表一致）
        offsets->perspective_switch  = { 0x1100ABC, 0x2C4 };              // 视角切换（实测 0 = 第一人称，锁 1 变第三人称）
        offsets->enemy_hp            = { 0xC7ABC0, 0x7C, 0x04, 0x160 };   // CT「所有人物」血量（这份客户端静态是 0，DLL 自己扫描）
        offsets->team_static         = { 0x601F14 };                      // 阵营（这份客户端读出来是 0）
        offsets->bots_static         = { 0x601F18 };                      // 机器人个数（这份客户端读出来是 0）
        // 免疫伤害（v3248 的 p3+16C）这份客户端读出来是 2.0，和 CT 表里「锁 0 就不掉血」对不上，
        // 所以没放出来 —— 想无敌直接用 F3 把血量锁住就是无敌了。显示金钱也是 v3248 才有的静态地址。
    }
    else
    {
        // ================= v3248（CT 表 CSGO1.6-5 ~ CSGO1.6-8） =================
        offsets->attack_interval     = { 0x11069BC, 0x7C, 0x5F0, 0xB8 };  // 左键冷却（单浮点）
        offsets->main_weapon         = { 0x11069BC, 0x7C, 0x5F0, 0xCC };  // 主武器子弹
        offsets->no_recoil           = { 0x11069BC, 0x7C, 0x5F0, 0x100 }; // 无后座力 / 手枪连发
        offsets->blood_bar           = { 0x11069BC, 0x7C, 0x04, 0x160 };
        offsets->armor               = { 0x11069BC, 0x7C, 0x04, 0x1BC };
        offsets->money               = { 0x11069BC, 0x7C, 0x1CC };
        offsets->money_display       = { 0x61B9FC };                      // 界面显示金钱（静态地址）
        offsets->move_speed          = { 0x11069BC, 0x290 };              // 移动速度（单浮点）
        offsets->shopping_everywhere = { 0x11069BC, 0x7C, 0x3C4 };
        offsets->slowed_by_attack    = { 0x11069BC, 0x7C, 0x1B0 };
        offsets->install_bomb_everywhere = { 0x11069BC, 0x7C, 0x3C0 };    // 随地下包
        offsets->perspective_switch  = { 0xD6E63C, 0x2C4 };
        offsets->immun_damage        = { 0x11069BC, 0x7C, 0x04, 0x16C };  // 锁 0 = 免疫伤害
        offsets->x                   = { 0x11069BC, 0x7C, 0x04, 0x08 };   // 坐标（单浮点）
        offsets->y                   = { 0x11069BC, 0x7C, 0x04, 0x0C };
        offsets->z                   = { 0x11069BC, 0x7C, 0x04, 0x10 };
        // 其他数据（CT-6/7/8）
        // CT 里这条的备注文字写的是 +5F0+D4，但实际用的偏移列是 5EC，以 CE 里验证过的偏移列为准
        offsets->weapon_mark         = { 0x11069BC, 0x7C, 0x5EC, 0xD4 };  // 在用武器标记
        offsets->my_team             = { 0x11069BC, 0x7C, 0x1C8 };        // 当前人物阵营
        offsets->aim_target_type     = { 0x11069BC, 0x7C, 0x710 };        // 准星指向 0墙1友2敌
        offsets->aim_target_index    = { 0x11069BC, 0x7C, 0x714 };        // 指向的角色编号
        offsets->team_static         = { 0x61B830 };                      // 全局阵营
        offsets->bots_static         = { 0x61B82C };                      // 机器人个数
        offsets->grenade             = { 0x11069BC, 0x7C, 0x628 };        // 手雷地址
        // CT「所有人物」组：指向的就是准星看着的那个角色（换个角色看数值会跟着变）
        offsets->enemy_hp            = { 0x1033240, 0x7C, 0x04, 0x160 };
        offsets->enemy_x             = { 0x1033240, 0x88 };               // 注意：坐标是直接 88/8C/90 三连单浮点
        offsets->enemy_y             = { 0x1033240, 0x8C };
        offsets->enemy_z             = { 0x1033240, 0x90 };
    }
}

// ===================== 人物框框（ESP）的参数 =====================
// 下面两条链默认是空的，程序只会画 CT「所有人物」指向的那个角色，
// 而且没有视角矩阵时算不出屏幕坐标，所以默认是画不出东西的。
//
// 这两条链可以在界面里直接填（写法和 CT 表一致，例如 cstrike.exe+11069BC+7C+88），
// 保存后写进同目录的 CounterStrike.ini，下次启动自动读回来，不用重新编译。
//
// 用 CE 扫的思路：
//   视角矩阵：转视角时整块都在变、而且是连续 16 个单浮点（4x4 矩阵）。
//             找到后右键该地址 -> 指针扫描，扫出基址链填到「视角矩阵链」。
//   实体列表：数组里是各个角色（或指向角色的指针），可以顺着 CT 里血量那条链
//             算出某个角色的真实地址，再用指针扫描找存放它的数组。
EspConfig g_esp_config;
