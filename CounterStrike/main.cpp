#include <Windows.h>
#include "gui.h"

#pragma comment(lib, "user32.lib")

// 游戏是 32 位进程，用 x64 配置编译指针长度会对不上
static_assert(sizeof(void*) == 4, "请使用 Win32(x86) 配置编译，游戏进程是 32 位的！");

// 图形界面版本：所有功能都在 gui.cpp 里，界面右侧有快捷键和功能说明。
#ifdef _UNICODE
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command)
#else
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command)
#endif
{
    return RunTrainerGui(instance, show_command);
}
