// ============================================================================
// HookTestHost —— 给 CounterStrikeHook.dll 做自测用的小“假游戏”（必须 32 位）
//
// 手头没有 cstrike.exe 的时候，靠它把注入 / 渲染钩子 / 固定伤害补丁整条路跑通：
//   * 窗口类故意叫 Valve001、标题带 Counter-Strike，修改器才认得出它是“游戏”；
//   * 开了 OpenGL 上下文，每帧画一段 GL_TRIANGLE_STRIP 再 SwapBuffers，
//     正好走到 CT 表里 glBegin / wglSwapBuffers 这两处；
//   * 在模块里放了固定伤害那段原始字节（和 mp.dll+5ACA1 一模一样，后面跟一个 ret），
//     每帧执行一次，输入伤害 7、血量 1000 —— 补丁一打上结果就该变成 800（1000-200）；
//   * 还放了一个单位矩阵、一个角色结构、一个角色指针数组，
//     在修改器里把链填成 TestHost.exe+偏移 就能把游戏内画框也跑起来。
// 启动后会把 pid 和各个“模块偏移”写进 exe 旁边的 hooktest_status.txt。
// ============================================================================

#include <Windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <string>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")

#if !defined(_M_IX86)
#error 这个测试程序要编 32 位（和 cstrike.exe 一样）
#endif

namespace
{
    // ---- 一、固定伤害那段原始字节（= mp.dll+5ACA1 的 10 个字节，末尾补 ret） ----
    //   D8 64 24 54            fsub dword ptr [esp+54]
    //   D9 98 60 01 00 00      fstp dword ptr [eax+00000160]
    //   C3                     ret
    extern "C" __declspec(align(16)) const unsigned char g_fixed_damage_bytes[11] =
    {
        0xD8, 0x64, 0x24, 0x54, 0xD9, 0x98, 0x60, 0x01, 0x00, 0x00, 0xC3
    };

    // ---- 二、视角矩阵 / 角色结构 / 角色数组（给 DLL 读的） ----
    extern "C" __declspec(align(16)) float g_view_matrix[16] =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    struct EntityBlob
    {
        unsigned char pad_origin[0x88];          // +0x00
        float x, y, z;                           // +0x88 坐标
        unsigned char pad_health[0x160 - 0x94];  // +0x94
        float hp;                                // +0x160 血量
        unsigned char pad_team[0x1C8 - 0x164];   // +0x164
        int team;                                // +0x1C8 阵营
    };

    extern "C" EntityBlob g_entity{};
    extern "C" EntityBlob* g_entity_array[4] = { nullptr, nullptr, nullptr, nullptr };

    float g_hp_before = 1000.0f;    // fld 进 x87 的那个“当前血量”
    float g_damage_in = 7.0f;       // 每次模拟 7 点伤害（补丁打上后会被改成 200）
    float g_result_holder[128] = {};   // [eax+0x160] 写到这里
    float g_last_result = 0.0f;

    void* g_fixed_damage_address = nullptr;

    // 执行一次“固定伤害”那段代码：fld 1000 / 输入伤害 7 / 把结果存回 [eax+0x160]
    float RunFixedDamageOnce()
    {
        float result = 0.0f;
        __asm
        {
            pushad
            sub esp, 0x60
            mov ecx, dword ptr [g_damage_in]
            mov dword ptr [esp + 50], ecx        // call 之后就是 [esp+54]
            mov eax, offset g_result_holder
            fld dword ptr [g_hp_before]
            call dword ptr [g_fixed_damage_address]
            mov ecx, dword ptr [eax + 0x160]
            mov dword ptr [result], ecx
            add esp, 0x60
            popad
        }
        return result;
    }

    std::wstring ExeDirectory()
    {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, path, _countof(path));
        std::wstring text = path;
        size_t slash = text.find_last_of(L'\\');
        return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash + 1);
    }

    uintptr_t ModuleOffset(const void* address)
    {
        return reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    }

    void WriteStatusFile(HWND hwnd)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        wchar_t module_name[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, module_name, _countof(module_name));
        std::wstring name = module_name;
        size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos)
        {
            name = name.substr(slash + 1);
        }

        FILE* file = nullptr;
        std::wstring path = ExeDirectory() + L"hooktest_status.txt";
        if (_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") != 0 || file == nullptr)
        {
            return;
        }

        fwprintf(file, L"pid=%u\n", static_cast<unsigned int>(GetCurrentProcessId()));
        fwprintf(file, L"module=%s\n", name.c_str());
        fwprintf(file, L"width=%d\n", static_cast<int>(client.right));
        fwprintf(file, L"height=%d\n", static_cast<int>(client.bottom));
        fwprintf(file, L"matrixOffset=%X\n", static_cast<unsigned int>(ModuleOffset(g_view_matrix)));
        fwprintf(file, L"entityArrayOffset=%X\n", static_cast<unsigned int>(ModuleOffset(g_entity_array)));
        fwprintf(file, L"patternOffset=%X\n", static_cast<unsigned int>(ModuleOffset(g_fixed_damage_bytes)));
        fwprintf(file, L"patternFirstByte=%02X\n", static_cast<unsigned int>(g_fixed_damage_bytes[0]));
        fwprintf(file, L"damageIn=%.0f\n", g_damage_in);
        fwprintf(file, L"hpBefore=%.0f\n", g_hp_before);
        fwprintf(file, L"patternResult=%.0f\n", g_last_result);
        fwprintf(file, L"entityHp=%.0f\n", g_entity.hp);
        fclose(file);
    }

    LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_CLOSE)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        if (message == WM_ERASEBKGND)
        {
            return 1;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = HostProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"Valve001";
    if (RegisterClassExW(&window_class) == 0)
    {
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, L"Valve001", L"Counter-Strike 1.6 (HookTestHost)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 40, 900, 700, nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr)
    {
        return 1;
    }

    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    int pixel_format = ChoosePixelFormat(dc, &format);
    if (pixel_format == 0 || SetPixelFormat(dc, pixel_format, &format) == FALSE)
    {
        MessageBoxW(hwnd, L"设置 OpenGL 像素格式失败，跑不了测试。", L"HookTestHost", MB_ICONERROR);
        return 1;
    }
    HGLRC gl_context = wglCreateContext(dc);
    if (gl_context == nullptr || wglMakeCurrent(dc, gl_context) == FALSE)
    {
        MessageBoxW(hwnd, L"创建 OpenGL 上下文失败，跑不了测试。", L"HookTestHost", MB_ICONERROR);
        return 1;
    }

    // 让固定伤害那 10 个字节可以执行（它们本来在只读段里）
    DWORD old_protect = 0;
    VirtualProtect(const_cast<unsigned char*>(g_fixed_damage_bytes), sizeof(g_fixed_damage_bytes), PAGE_EXECUTE_READ, &old_protect);
    g_fixed_damage_address = const_cast<unsigned char*>(g_fixed_damage_bytes);

    // 一个站在屏幕中间偏右的角色（单位矩阵下 world(0.1, 0, 0) 正好落在画面里）
    g_entity.x = 0.1f;
    g_entity.y = 0.0f;
    g_entity.z = 0.0f;
    g_entity.hp = 87.0f;
    g_entity.team = 1;
    g_entity_array[0] = &g_entity;

    WriteStatusFile(hwnd);

    SetForegroundWindow(hwnd);
    MSG message{};
    float phase = 0.0f;
    int frame = 0;
    for (;;)
    {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(gl_context);
                ReleaseDC(hwnd, dc);
                return 0;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        RECT client{};
        GetClientRect(hwnd, &client);
        int width = client.right > 0 ? client.right : 640;
        int height = client.bottom > 0 ? client.bottom : 480;

        glViewport(0, 0, width, height);
        glClearColor(0.09f, 0.11f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(width), static_cast<double>(height), 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_DEPTH_TEST);

        // 一段 GL_TRIANGLE_STRIP —— CT-8 的透视脚本就是盯着这个批次关深度测试的
        glBegin(GL_TRIANGLE_STRIP);
        for (int index = 0; index <= 40; index++)
        {
            float t = static_cast<float>(index) / 40.0f;
            float x = t * static_cast<float>(width);
            float y = height * 0.55f + sinf(t * 6.28318f + phase) * height * 0.22f;
            glColor3f(t, 0.45f, 1.0f - t);
            glVertex2f(x, y - 70.0f);
            glVertex2f(x, y + 70.0f);
        }
        glEnd();

        glColor3f(0.9f, 0.9f, 0.9f);
        glBegin(GL_QUADS);
        glVertex2f(20.0f, 20.0f);
        glVertex2f(220.0f, 20.0f);
        glVertex2f(220.0f, 70.0f);
        glVertex2f(20.0f, 70.0f);
        glEnd();

        SwapBuffers(dc);

        g_last_result = RunFixedDamageOnce();

        phase += 0.02f;
        if (++frame % 20 == 0)
        {
            WriteStatusFile(hwnd);
        }
        Sleep(16);
    }
}
