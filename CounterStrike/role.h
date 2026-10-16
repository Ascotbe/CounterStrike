#pragma once
#include "config.h"

// 指针链解析：成功返回最终地址；中间指针为空或读取失败返回 0
uintptr_t FindDirectMemoryAccessAddy(const game_information& game, const OffsetChain& offsets);

// 读内存（用来验证偏移 / 刷新界面状态）
bool ReadIntValue(const game_information& game, const OffsetChain& offsets, int& value);
bool ReadFloatValue(const game_information& game, const OffsetChain& offsets, float& value);

// 各功能的“锁定”：界面上每隔几毫秒把值写回去，效果和 CE 里锁定地址一样。
// value 就是界面上每行的「锁定数值」，可以随时改（- / + 按钮也是改它）。
void Money(const role_offsets& offsets, const game_information& game, double value);
void MainWeapon(const role_offsets& offsets, const game_information& game, double value);
void AttackInterval(const role_offsets& offsets, const game_information& game, double value);
void NoRecoil(const role_offsets& offsets, const game_information& game, double value);
void BloodBar(const role_offsets& offsets, const game_information& game, double value);
void Armor(const role_offsets& offsets, const game_information& game, double value);
void MoveSpeed(const role_offsets& offsets, const game_information& game, double value);
void ShoppingEverywhere(const role_offsets& offsets, const game_information& game, double value);
void SlowedByAttack(const role_offsets& offsets, const game_information& game, double value);
void InstallBombEverywhere(const role_offsets& offsets, const game_information& game, double value);
void PerspectiveSwitch(const role_offsets& offsets, const game_information& game, double value);
void ImmunDamage(const role_offsets& offsets, const game_information& game, double value);
