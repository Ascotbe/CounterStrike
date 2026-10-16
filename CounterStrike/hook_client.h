#pragma once
#include <Windows.h>
#include <string>
#include "hook_shared.h"

// ================== 注入那一半（CounterStrikeHook.dll）的训练器侧接口 ==================
// 流程：
//   1) 附加到游戏之后调 HookClientOpenMapping 建一块共享内存（名字带游戏 PID）；
//   2) HookClientInject 用 VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryW)
//      把训练器旁边的 CounterStrikeHook.dll 注进游戏进程；
//   3) 之后界面上的开关就写共享内存，DLL 那边看到 settings_seq 变了就重新读一遍；
//   4) HookClientRequestUnload 让 DLL 自己摘掉 hook 再卸载（不会留半截补丁在游戏里）。

// 建共享内存并初始化（已经在用了就直接返回 true）
bool HookClientOpenMapping(DWORD pid, std::wstring& error);
void HookClientCloseMapping();
HookShared* HookClientShared();          // 还没建就返回 nullptr
DWORD HookClientPid();                   // 共享内存对应的游戏进程号（没建就返回 0）

std::wstring HookClientDllPath();        // 训练器 exe 旁边的 CounterStrikeHook.dll
bool HookClientDllExists();

// 把 DLL 注进指定进程；成功不等于 hook 装好了（要看共享内存里的 dll_loaded / hook_installed）
bool HookClientInject(DWORD pid, std::wstring& error);
void HookClientRequestUnload();
// 兜底：DLL 半天没反应（比如 worker 卡住了），远程 FreeLibrary 强行卸载
bool HookClientForceUnload(std::wstring& error);
