#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <Windows.h>
#include <Tlhelp32.h>
#include <stdio.h>
#include <time.h>
#include <vector>
#include"config.h"
#include"role.h"
using namespace std;



////ͨ��PID��ȡ����ַ 
HMODULE GetProcessBase(DWORD PID)
{
	//��ȡ���̻�ַ
	HANDLE snap_shot;
	//ͨ��CreateToolhelp32Snapshot���߳�ID����ȡ���̿���
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



bool InIt(role_offsets * offsets)
{
	
	//ͨ��cs�Ľ������ֻ�ȡ���ھ�� 
	hwnd = FindWindowA(NULL, "Counter-Strike");
	if (hwnd == NULL)
	{
		cout << "There is no such a window!" << endl;
		return 0;
	}

	//��ȡ����PID 
	GetWindowThreadProcessId(hwnd,&process_identity_document);

	//��ȡ���̾�� 
	game_handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, process_identity_document);

	if (game_handle == NULL)
	{
		cout << "There is no such a process!" << endl;
		return 0;
	}

	HMODULE module = GetProcessBase(process_identity_document);
	if (module == NULL)
	{
		return 0;
	}
	offsets->own_base_address = (UINT_PTR)module;

	return 1;
}






int main()
{

	role_offsets offsets;
	
	IntoConfig(&offsets);
	if (!InIt(&offsets))
	{
		cout << "��ʼ��ʧ��" << endl;
		Sleep(3000);
		return 0;
	}

	while (1)
	{
		AttackInterval(&offsets);//�������
		Money(&offsets);//�޸Ľ�Ǯ 
		MainWeapon(&offsets);//�޸��������ӵ��� 
		//changeRifleBullets();//
		
		Sleep(50);//��ͣ5�룬ʵս�����ˢ��Ƶ�� 
	}
	CloseHandle(game_handle);

	return 0;
}
