#include "role.h"

uintptr_t FindDirectMemoryAccessAddy(game_information& game, vector<unsigned int> offsets)
{
	uintptr_t addr = game.own_base_address;
	uintptr_t tmp;
	for (unsigned int i = 0; i != offsets.size(); i++)
	{
		addr += offsets[i];
		if (i < offsets.size() - 1)//最后一次只加偏移量，不用读取了 
		{
			bool state = ReadProcessMemory(game.game_handle, (LPVOID)addr, &tmp, sizeof(tmp), 0);
			if (!state)
			{
				cout << "error in reading memory!" << endl;
			}
			addr = tmp;
		}
	}
	return addr;
}


void Money(role_offsets* offsets, game_information &game)
{

	uintptr_t addr = FindDirectMemoryAccessAddy(game, offsets->money);
	int money = 16000;
	//写入实际的money 
	WriteProcessMemory(game.game_handle, (LPVOID)addr, &money, sizeof(money), 0);
	//写入显示的money 
	WriteProcessMemory(game.game_handle, (LPVOID)(game.own_base_address + 0x61B9FC), &money, sizeof(money), 0);
}

//主武器
void MainWeapon(role_offsets* offsets, game_information& game)
{
	uintptr_t addr = FindDirectMemoryAccessAddy(game, offsets->main_weapon);
	int num = 30;
	bool state = WriteProcessMemory(game.game_handle, (LPVOID)addr, &num, sizeof(num), 0);
	if (!state)cout << "rifle change failed!" << endl;
}

void AttackInterval(role_offsets* offsets, game_information& game)
{

	uintptr_t addr = FindDirectMemoryAccessAddy(game, offsets->attack_interval);
	int num = -1;
	WriteProcessMemory(game.game_handle, (LPVOID)addr, &num, sizeof(num), 0);
}
