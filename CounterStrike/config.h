#pragma once
#include <vector>
#include <Windows.h>
#include <iostream>
using namespace std;



HWND hwnd = NULL;
//程序的地址，类似于一个int
DWORD process_identity_document = NULL;//进程ID
HANDLE game_handle = NULL;//游戏句柄

typedef struct {
	uintptr_t own_base_address;//进程基地址
	//uintptr_t own_base_address = 0x25069bc;//当前用户基地址，0x1400000+0x11069BC
	vector<unsigned int> attack_interval;//攻击间隔
	vector<unsigned int> main_weapon;//主武器
	vector<unsigned int> blood_bar;//血条
	vector<unsigned int> armor;//护甲
	vector<unsigned int> money;//钱
	vector<unsigned int> move_speed;//移速
	vector<unsigned int> shopping_everywhere;//随地购物
	vector<unsigned int> perspective_switch;//视角切换
	vector<unsigned int> slowed_by_attack;//受击减速
	vector<unsigned int> x;//X坐标
	vector<unsigned int> y;//Y坐标
	vector<unsigned int> z;//Z坐标
	vector<unsigned int> grenade;//手雷
	vector<unsigned int> install_bomb_everywhere;//随地安装炸弹

}role_offsets;

role_offsets IntoConfig(role_offsets* offsets);
uintptr_t FindDirectMemoryAccessAddy(uintptr_t addr, vector<unsigned int> offsets);
