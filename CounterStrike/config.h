#pragma once
#include <vector>
#include <Windows.h>
#include <iostream>
using namespace std;



HWND hwnd = NULL;
//����ĵ�ַ��������һ��int
DWORD process_identity_document = NULL;//����ID
HANDLE game_handle = NULL;//��Ϸ���

typedef struct {
	uintptr_t own_base_address;//���̻���ַ
	//uintptr_t own_base_address = 0x25069bc;//��ǰ�û�����ַ��0x1400000+0x11069BC
	vector<unsigned int> attack_interval;//�������
	vector<unsigned int> main_weapon;//������
	vector<unsigned int> blood_bar;//Ѫ��
	vector<unsigned int> armor;//����
	vector<unsigned int> money;//Ǯ
	vector<unsigned int> move_speed;//����
	vector<unsigned int> shopping_everywhere;//��ع���
	vector<unsigned int> perspective_switch;//�ӽ��л�
	vector<unsigned int> slowed_by_attack;//�ܻ�����
	vector<unsigned int> x;//X����
	vector<unsigned int> y;//Y����
	vector<unsigned int> z;//Z����
	vector<unsigned int> grenade;//����
	vector<unsigned int> install_bomb_everywhere;//��ذ�װը��

}role_offsets;

role_offsets IntoConfig(role_offsets* offsets);
uintptr_t FindDirectMemoryAccessAddy(uintptr_t addr, vector<unsigned int> offsets);
