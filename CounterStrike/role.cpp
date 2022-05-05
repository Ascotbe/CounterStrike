#include "role.h"

void Money(role_offsets *offsets)
{

	uintptr_t addr = FindDirectMemoryAccessAddy(offsets->own_base_address, offsets->money);
	int money = 16000;
	//写入实际的money 
	WriteProcessMemory(game_handle, (LPVOID)addr, &money, sizeof(money), 0);
	//写入显示的money 
	WriteProcessMemory(game_handle, (LPVOID)(offsets->own_base_address + 0x61B9FC), &money, sizeof(money), 0);
}

//主武器
void MainWeapon(role_offsets* offsets)
{
	uintptr_t addr = FindDirectMemoryAccessAddy(offsets->own_base_address, offsets->main_weapon);
	int num = 30;
	bool state = WriteProcessMemory(game_handle, (LPVOID)addr, &num, sizeof(num), 0);
	if (!state)cout << "rifle change failed!" << endl;
}

void AttackInterval(role_offsets* offsets)
{

	uintptr_t addr = FindDirectMemoryAccessAddy(offsets->own_base_address, offsets->attack_interval);
	int num = -1;
	WriteProcessMemory(game_handle, (LPVOID)addr, &num, sizeof(num), 0);
}
