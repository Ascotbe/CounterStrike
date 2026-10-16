#include "hook_client.h"

#include <cstdio>
#include <vector>

namespace
{
    HANDLE g_mapping = nullptr;
    HookShared* g_shared = nullptr;
    DWORD g_pid = 0;
    uintptr_t g_remote_module = 0;   // LoadLibraryW 在游戏里返回的模块句柄（卸载兜底用）
}

bool HookClientOpenMapping(DWORD pid, std::wstring& error)
{
    if (g_shared != nullptr && g_pid == pid)
    {
        return true;
    }
    HookClientCloseMapping();

    wchar_t name[64] = L"";
    MakeHookMappingName(pid, name, _countof(name));

    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HookShared), name);
    if (g_mapping == nullptr)
    {
        error = L"CreateFileMapping 失败，错误码 " + std::to_wstring(GetLastError());
        return false;
    }
    g_shared = static_cast<HookShared*>(MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(HookShared)));
    if (g_shared == nullptr)
    {
        error = L"MapViewOfFile 失败，错误码 " + std::to_wstring(GetLastError());
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }

    ZeroMemory(g_shared, sizeof(HookShared));
    g_shared->magic = CSTRAINER_HOOK_MAGIC;
    g_shared->version = CSTRAINER_HOOK_VERSION;
    g_shared->size = sizeof(HookShared);
    g_pid = pid;
    g_remote_module = 0;
    return true;
}

void HookClientCloseMapping()
{
    if (g_shared != nullptr)
    {
        UnmapViewOfFile(g_shared);
        g_shared = nullptr;
    }
    if (g_mapping != nullptr)
    {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
    g_pid = 0;
    g_remote_module = 0;
}

HookShared* HookClientShared()
{
    return g_shared;
}

DWORD HookClientPid()
{
    return g_pid;
}

std::wstring HookClientDllPath()
{
    wchar_t module_path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, module_path, _countof(module_path));
    std::wstring path = module_path;
    size_t slash = path.find_last_of(L'\\');
    return (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"CounterStrikeHook.dll";
}

bool HookClientDllExists()
{
    return GetFileAttributesW(HookClientDllPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool HookClientInject(DWORD pid, std::wstring& error)
{
    std::wstring dll = HookClientDllPath();
    if (!HookClientDllExists())
    {
        error = L"找不到 " + dll + L"（要先编译 CounterStrikeHook 项目，见 README）";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (process == nullptr)
    {
        error = L"OpenProcess 失败（错误码 " + std::to_wstring(GetLastError()) + L"），请用管理员身份运行本程序";
        return false;
    }

    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_path == nullptr)
    {
        error = L"VirtualAllocEx 失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
        CloseHandle(process);
        return false;
    }
    if (!WriteProcessMemory(process, remote_path, dll.c_str(), bytes, nullptr))
    {
        error = L"WriteProcessMemory 失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE load_library = kernel32 != nullptr
        ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"))
        : nullptr;
    if (load_library == nullptr)
    {
        error = L"找不到 LoadLibraryW";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote_path, 0, nullptr);
    if (thread == nullptr)
    {
        error = L"CreateRemoteThread 失败（错误码 " + std::to_wstring(GetLastError()) + L"），可能被杀软拦了";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 20000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);

    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(process);

    if (exit_code == 0)
    {
        error = L"游戏里 LoadLibraryW 返回 0，DLL 没加载成功（位数不对？杀软拦了？）";
        return false;
    }

    g_remote_module = exit_code;
    return true;
}

void HookClientRequestUnload()
{
    if (g_shared != nullptr)
    {
        InterlockedExchange(&g_shared->unload_requested, 1);
    }
}

bool HookClientForceUnload(std::wstring& error)
{
    if (g_pid == 0 || g_remote_module == 0)
    {
        error = L"没有可卸载的记录（可能是重启过训练器）";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, g_pid);
    if (process == nullptr)
    {
        error = L"OpenProcess 失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE free_library = kernel32 != nullptr
        ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "FreeLibrary"))
        : nullptr;
    if (free_library == nullptr)
    {
        error = L"找不到 FreeLibrary";
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, free_library,
        reinterpret_cast<void*>(g_remote_module), 0, nullptr);
    if (thread == nullptr)
    {
        error = L"CreateRemoteThread(FreeLibrary) 失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    CloseHandle(process);

    if (exit_code == 0)
    {
        error = L"游戏里 FreeLibrary 返回 0（DLL 还在游戏里，重启游戏就干净了）";
        return false;
    }
    g_remote_module = 0;
    return true;
}
