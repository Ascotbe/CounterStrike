#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <Windows.h>
#include <Tlhelp32.h>
#include <stdio.h>
#include <time.h>
#include <vector>
#include "role.h"
#include "config.h"

using namespace std;



////通过PID获取基地址 
HMODULE GetProcessBase(DWORD PID)
{
	//获取进程基址
	HANDLE snap_shot;
	//通过CreateToolhelp32Snapshot和线程ID，获取进程快照
	snap_shot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, PID);
	if (snap_shot == INVALID_HANDLE_VALUE)
	{
		cout<<"can't create Snapshot!";
		return NULL;
	}
	MODULEENTRY32 module_entry32;
	module_entry32.dwSize = sizeof(module_entry32);
	if (Module32First(snap_shot, &module_entry32))
	{
		do
		{
			TCHAR tmp[5];
			strcpy(tmp, module_entry32.szExePath + strlen(module_entry32.szExePath) - 4);
			for (int i = 0; i < 4; i++)
			{
				if ((tmp[i] >= 'a') && (tmp[i] <= 'z'))
				{
					tmp[i] = tmp[i] - 0x20;
				}
			}
			if (!strcmp(tmp, ".EXE"))
			{
				CloseHandle(snap_shot);
				return module_entry32.hModule;
			}
		} while (Module32Next(snap_shot, &module_entry32));
	}
	CloseHandle(snap_shot);
	return NULL;

}



bool InIt(role_offsets * offsets, game_information &game)
{
	
	//通过cs的进程名字获取窗口句柄 
	game.hwnd = FindWindowA(NULL, "Counter-Strike");
	if (game.hwnd == NULL)
	{
		cout << "There is no such a window!" << endl;
		return 0;
	}

	//获取进程PID 
	GetWindowThreadProcessId(game.hwnd, &game.process_identity_document);

	//获取进程句柄 
	game.game_handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, game.process_identity_document);

	if (game.game_handle == NULL)
	{
		cout << "There is no such a process!" << endl;
		return 0;
	}

	HMODULE module = GetProcessBase(game.process_identity_document);
	if (module == NULL)
	{
		return 0;
	}
	game.own_base_address = (UINT_PTR)module;

	return 1;
}






int main()
{

	role_offsets offsets;
	game_information game;
	IntoConfig(&offsets);
	if (!InIt(&offsets,game))
	{
		cout << "初始化失败" << endl;
		Sleep(3000);
		return 0;
	}

	while (1)
	{
		AttackInterval(&offsets, game);//攻击间隔
		Money(&offsets, game);//修改金钱 
		MainWeapon(&offsets, game);//修改主武器子弹数 
		//changeRifleBullets();//
		
		Sleep(50);//暂停5秒，实战得提高刷新频率 
	}
	CloseHandle(game.game_handle);

	return 0;
}
