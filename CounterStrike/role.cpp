#include "role.h"

namespace
{
    bool WriteValue(const game_information& game, const OffsetChain& offsets, const void* data, SIZE_T size)
    {
        uintptr_t addr = FindDirectMemoryAccessAddy(game, offsets);
        if (addr == 0)
        {
            return false;
        }
        return WriteProcessMemory(game.handle, reinterpret_cast<LPVOID>(addr), data, size, nullptr) != FALSE;
    }

    bool WriteInt(const game_information& game, const OffsetChain& offsets, int value)
    {
        return WriteValue(game, offsets, &value, sizeof(value));
    }

    bool WriteFloat(const game_information& game, const OffsetChain& offsets, float value)
    {
        return WriteValue(game, offsets, &value, sizeof(value));
    }
}

uintptr_t FindDirectMemoryAccessAddy(const game_information& game, const OffsetChain& offsets)
{
    if (offsets.empty() || game.handle == nullptr)
    {
        return 0;
    }

    uintptr_t addr = game.base_address;
    for (size_t i = 0; i < offsets.size(); i++)
    {
        addr += offsets[i];
        if (i + 1 < offsets.size())
        {
            uintptr_t next = 0;
            if (!ReadProcessMemory(game.handle, reinterpret_cast<LPCVOID>(addr), &next, sizeof(next), nullptr) || next == 0)
            {
                // 没进地图、人物没出生或者客户端版本不对时都会走到这里
                return 0;
            }
            addr = next;
        }
    }
    return addr;
}

bool ReadIntValue(const game_information& game, const OffsetChain& offsets, int& value)
{
    uintptr_t addr = FindDirectMemoryAccessAddy(game, offsets);
    if (addr == 0)
    {
        return false;
    }
    return ReadProcessMemory(game.handle, reinterpret_cast<LPCVOID>(addr), &value, sizeof(value), nullptr) != FALSE;
}

bool ReadFloatValue(const game_information& game, const OffsetChain& offsets, float& value)
{
    uintptr_t addr = FindDirectMemoryAccessAddy(game, offsets);
    if (addr == 0)
    {
        return false;
    }
    return ReadProcessMemory(game.handle, reinterpret_cast<LPCVOID>(addr), &value, sizeof(value), nullptr) != FALSE;
}

// ---------------- 以下每个功能对应 CT 表里的一条地址 ----------------
// value 是界面里每行的「锁定数值」，整数功能会做一次取整。

void Money(const role_offsets& offsets, const game_information& game, double value)
{
    // 实际金钱和界面显示的金钱是两个地址，都要写
    WriteInt(game, offsets.money, static_cast<int>(value));
    WriteInt(game, offsets.money_display, static_cast<int>(value));
}

void MainWeapon(const role_offsets& offsets, const game_information& game, double value)
{
    WriteInt(game, offsets.main_weapon, static_cast<int>(value));
}

void AttackInterval(const role_offsets& offsets, const game_information& game, double value)
{
    WriteFloat(game, offsets.attack_interval, static_cast<float>(value));
}

void NoRecoil(const role_offsets& offsets, const game_information& game, double value)
{
    WriteInt(game, offsets.no_recoil, static_cast<int>(value));
}

void BloodBar(const role_offsets& offsets, const game_information& game, double value)
{
    WriteFloat(game, offsets.blood_bar, static_cast<float>(value));
}

void Armor(const role_offsets& offsets, const game_information& game, double value)
{
    WriteFloat(game, offsets.armor, static_cast<float>(value));
}

void MoveSpeed(const role_offsets& offsets, const game_information& game, double value)
{
    WriteFloat(game, offsets.move_speed, static_cast<float>(value));
}

void ShoppingEverywhere(const role_offsets& offsets, const game_information& game, double value)
{
    WriteInt(game, offsets.shopping_everywhere, static_cast<int>(value));
}

void SlowedByAttack(const role_offsets& offsets, const game_information& game, double value)
{
    WriteFloat(game, offsets.slowed_by_attack, static_cast<float>(value));
}

void InstallBombEverywhere(const role_offsets& offsets, const game_information& game, double value)
{
    WriteInt(game, offsets.install_bomb_everywhere, static_cast<int>(value));
}

void PerspectiveSwitch(const role_offsets& offsets, const game_information& game, double value)
{
    WriteInt(game, offsets.perspective_switch, static_cast<int>(value));
}

void ImmunDamage(const role_offsets& offsets, const game_information& game, double value)
{
    WriteFloat(game, offsets.immun_damage, static_cast<float>(value));
}
