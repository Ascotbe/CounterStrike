#pragma once
#include <Windows.h>

// 启动带界面的修改器（一直阻塞到窗口关闭，返回进程退出码）
int RunTrainerGui(HINSTANCE instance, int show_command);
