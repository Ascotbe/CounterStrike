#include"config.h"




role_offsets IntoConfig(role_offsets* offsets)
{

	offsets->attack_interval.push_back(0x11069BC);
	offsets->attack_interval.push_back(0x7C);
	offsets->attack_interval.push_back(0x5F0);
	offsets->attack_interval.push_back(0xB8);

	offsets->main_weapon.push_back(0x11069BC);
	offsets->main_weapon.push_back(0x7C);
	offsets->main_weapon.push_back(0x5F0);
	offsets->main_weapon.push_back(0xCC);


	offsets->blood_bar.push_back(0x11069BC);
	offsets->blood_bar.push_back(0x7C);
	offsets->blood_bar.push_back(0x04);
	offsets->blood_bar.push_back(0x160);


	offsets->armor.push_back(0x11069BC);
	offsets->armor.push_back(0x7C);
	offsets->armor.push_back(0x04);
	offsets->armor.push_back(0x1BC);

	offsets->money.push_back(0x11069BC);
	offsets->money.push_back(0x7C);
	offsets->money.push_back(0x1CC);


	offsets->move_speed.push_back(0x11069BC);
	offsets->move_speed.push_back(0x290);


	offsets->shopping_everywhere.push_back(0x11069BC);
	offsets->shopping_everywhere.push_back(0x7C);
	offsets->shopping_everywhere.push_back(0x3C4);


	offsets->slowed_by_attack.push_back(0x11069BC);
	offsets->slowed_by_attack.push_back(0x7C);
	offsets->slowed_by_attack.push_back(0x1B0);


	offsets->grenade.push_back(0x11069BC);
	offsets->grenade.push_back(0x7C);
	offsets->grenade.push_back(0x628);
	

	offsets->install_bomb_everywhere.push_back(0x11069BC);
	offsets->install_bomb_everywhere.push_back(0x7C);
	offsets->install_bomb_everywhere.push_back(0x3C0);
	
	//µØÖ·²»Í¬
	offsets->perspective_switch.push_back(0xD6E63C);
	offsets->perspective_switch.push_back(0x2C4);

	return *offsets;
}