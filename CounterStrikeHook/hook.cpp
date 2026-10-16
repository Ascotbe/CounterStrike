// ============================================================================
// CounterStrikeHook.dll —— 注入进游戏里的那一半（必须编成 32 位！）
//
// 这个 DLL 被训练器用 CreateRemoteThread(LoadLibraryW) 注进 cstrike.exe，
// 然后在游戏进程里干三件事（都是从 CT 表 / 博客里搬过来的）：
//
//   1) 固定伤害：CT 表 CSGO1.6-4 和 CSGO1.6-8 里的 AutoAssembler 脚本
//        newmem:       mov [esp+54],0x43480000   (200.0f)
//        originalcode: fsub dword ptr [esp+54]
//                      fstp dword ptr [eax+00000160]
//      打在 "mp.dll"+5ACA1 上。这里用代码洞穴（code cave）原样实现，
//      数值可以在界面上改（改的是洞穴里那条 mov 的立即数）。
//
//   2) 透视：CT-8 的 LuaScript 是补 "opengl32.dll"+314A0，代码是
//        cmp [esp+4],5 / jne originalcode / push 0xB71 / call glDisable
//      也就是 glBegin(GL_TRIANGLE_STRIP) 的时候把 GL_DEPTH_TEST 关掉，
//      人物就被画到墙前面了。这里直接钩 opengl32!glBegin 的入口，效果一致，
//      而且不用自己维护字节（CT 里那个 +314A0 就是 glBegin 的入口）。
//
//   3) 游戏内画方框：钩 wglSwapBuffers，在游戏自己的画面（后台缓冲还没交换
//      之前）用 OpenGL 画方框和血量。因为是画在游戏自己的渲染里，
//      截图、录像都带着框，也不用再开一个透明窗口。
//
// 摘钩子：训练器在共享内存里置 unload_requested，本 DLL 把补丁/hook 全部还原、
// 置 ready_to_unload，然后自己 FreeLibraryAndExitThread 卸载，不会留下半截补丁。
// ============================================================================

#include "../CounterStrike/hook_shared.h"

#include <Windows.h>
#include <GL/gl.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")

#if !defined(_M_IX86)
#error 这个 DLL 必须编成 32 位（Win32 平台），因为它要注入进 32 位的 cstrike.exe
#endif

namespace
{
    // opengl32 里所有函数（glBegin / glDisable / wglSwapBuffers ...）的入口都是
    // /hotpatch 编译出来的 5 个字节：8B FF 55 8B EC
    //   mov edi,edi / push ebp / mov ebp,esp
    // 正好一条 5 字节 jmp 就能盖住，不会切断任何指令 —— 这是整件事敢做的前提。
    const unsigned char kHotPatchPrologue[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };

    // ---------------------------------------------------------------- 全局
    HMODULE g_module = nullptr;
    HANDLE g_worker = nullptr;
    volatile LONG g_stop_worker = 0;
    volatile LONG g_process_exiting = 0;

    HANDLE g_mapping = nullptr;
    HookShared* g_shared = nullptr;

    // 从 GL 里抄下来的「世界 -> 屏幕」矩阵（行主序，给 WorldToScreen 用）
    float g_gl_matrix[16] = {};
    bool g_gl_matrix_valid = false;
    void CaptureWorldMatrix();   // 定义在下面：在 3D 渲染时把世界矩阵抄下来

    HGLRC g_font_context = nullptr;
    GLuint g_font_base = 0;


    // ============================ 设置快照 ============================
    // worker 线程写、渲染线程读，用一个简单的序号锁（seqlock）保护：
    // 写的时候序号变奇数，写完变偶数；读的前后序号一样才算读稳了。
    struct Settings
    {
        LONG enabled;
        LONG esp_enabled;
        LONG show_hp_text;
        LONG show_crosshair_entity;
        LONG export_boxes;
        LONG flip_y;
        LONG box_width;
        LONG box_height;
        FLOAT max_distance;
        LONG color_enemy;
        LONG color_team;
        LONG color_unknown;
        LONG pointer_array;
        LONG stride;
        LONG max_count;
        LONG origin_offset;
        LONG health_offset;
        LONG team_offset;
        LONG wallhack_enabled;
        LONG wallhack_aggressive;
        LONG fixed_damage_enabled;
        FLOAT fixed_damage_value;
        LONG fixed_damage_offset;
        LONG debug_marker;
        LONG game_version;
        LONG release_mouse;
        LONG screenshot_request;
        LONG autolocate;
        wchar_t screenshot_path[CSTRAINER_HOOK_PATH_LENGTH];
        wchar_t fixed_damage_module[CSTRAINER_HOOK_MODULE_LENGTH];
        HookChainView entity_list;
        HookChainView view_matrix;
        HookChainView crosshair_x;
        HookChainView crosshair_y;
        HookChainView crosshair_z;
        HookChainView crosshair_hp;
        HookChainView my_x;
        HookChainView my_y;
        HookChainView my_z;
        HookChainView my_team;
    };

    volatile LONG g_settings_lock = 0;   // 偶数 = 稳定，奇数 = 正在写
    Settings g_settings;

    void CopyChain(const HookChainView& from, HookChainView& to)
    {
        to.count = from.count;
        for (int index = 0; index < CSTRAINER_HOOK_MAX_CHAIN; index++)
        {
            to.offsets[index] = from.offsets[index];
        }
    }

    void PublishSettings(const HookShared& shared)
    {
        InterlockedIncrement(&g_settings_lock);          // 变奇数：开始写
        Settings& s = g_settings;
        s.enabled = shared.enabled;
        s.esp_enabled = shared.esp_enabled;
        s.show_hp_text = shared.show_hp_text;
        s.show_crosshair_entity = shared.show_crosshair_entity;
        s.export_boxes = shared.export_boxes;
        s.flip_y = shared.flip_y;
        s.box_width = shared.box_width;
        s.box_height = shared.box_height;
        s.max_distance = shared.max_distance;
        s.color_enemy = shared.color_enemy;
        s.color_team = shared.color_team;
        s.color_unknown = shared.color_unknown;
        s.pointer_array = shared.pointer_array;
        s.stride = shared.stride;
        s.max_count = shared.max_count;
        s.origin_offset = shared.origin_offset;
        s.health_offset = shared.health_offset;
        s.team_offset = shared.team_offset;
        s.wallhack_enabled = shared.wallhack_enabled;
        s.wallhack_aggressive = shared.wallhack_aggressive;
        s.fixed_damage_enabled = shared.fixed_damage_enabled;
        s.fixed_damage_value = shared.fixed_damage_value;
        s.fixed_damage_offset = shared.fixed_damage_offset;
        s.debug_marker = shared.debug_marker;
        s.game_version = shared.game_version;
        s.release_mouse = shared.release_mouse;
        s.screenshot_request = shared.screenshot_request;
        s.autolocate = shared.autolocate;
        wcsncpy_s(s.screenshot_path, shared.screenshot_path, _TRUNCATE);
        wcsncpy_s(s.fixed_damage_module, shared.fixed_damage_module, _TRUNCATE);
        CopyChain(shared.entity_list, s.entity_list);
        CopyChain(shared.view_matrix, s.view_matrix);
        CopyChain(shared.crosshair_x, s.crosshair_x);
        CopyChain(shared.crosshair_y, s.crosshair_y);
        CopyChain(shared.crosshair_z, s.crosshair_z);
        CopyChain(shared.crosshair_hp, s.crosshair_hp);
        CopyChain(shared.my_x, s.my_x);
        CopyChain(shared.my_y, s.my_y);
        CopyChain(shared.my_z, s.my_z);
        CopyChain(shared.my_team, s.my_team);
        InterlockedIncrement(&g_settings_lock);          // 变偶数：写完了
    }

    bool ReadSettings(Settings& out)
    {
        for (int attempt = 0; attempt < 16; attempt++)
        {
            LONG before = g_settings_lock;
            if ((before & 1) != 0)
            {
                continue;   // 正在写，等下一轮
            }
            out = g_settings;
            if (g_settings_lock == before)
            {
                return true;
            }
        }
        return false;
    }

    void SetStatus(const wchar_t* text)
    {
        if (g_shared == nullptr)
        {
            return;
        }
        wcsncpy_s(g_shared->status_text, text, _TRUNCATE);
    }


    // ======================== 进程内读内存（带 SEH 保护） ========================
    bool SafeRead(const void* address, void* buffer, size_t size)
    {
        __try
        {
            memcpy(buffer, address, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t ResolveChain(const HookChainView& chain)
    {
        if (chain.count <= 0 || chain.count > CSTRAINER_HOOK_MAX_CHAIN)
        {
            return 0;
        }
        uintptr_t module_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (module_base == 0)
        {
            return 0;
        }
        uintptr_t address = module_base + chain.offsets[0];
        for (int32_t level = 1; level < chain.count; level++)
        {
            uintptr_t pointer = 0;
            if (!SafeRead(reinterpret_cast<const void*>(address), &pointer, sizeof(pointer)) || pointer == 0)
            {
                return 0;
            }
            address = pointer + chain.offsets[level];
        }
        return address;
    }

    bool ReadFloatAt(uintptr_t address, float& value)
    {
        return address != 0 && SafeRead(reinterpret_cast<const void*>(address), &value, sizeof(value));
    }

    bool ReadIntAt(uintptr_t address, int& value)
    {
        return address != 0 && SafeRead(reinterpret_cast<const void*>(address), &value, sizeof(value));
    }

    bool ReadChainFloat(const HookChainView& chain, float& value)
    {
        return ReadFloatAt(ResolveChain(chain), value);
    }

    bool ReadChainInt(const HookChainView& chain, int& value)
    {
        return ReadIntAt(ResolveChain(chain), value);
    }

    bool ReadMatrix(const HookChainView& chain, float matrix[16])
    {
        uintptr_t address = ResolveChain(chain);
        return address != 0 && SafeRead(reinterpret_cast<const void*>(address), matrix, sizeof(float) * 16);
    }

    // ============================ 内联 hook ============================
    struct HookEntry
    {
        const wchar_t* name;         // 出错时写到状态里
        const char* export_name;     // opengl32 里的导出名
        void* target;                // 解析出来的函数地址
        void* detour;                // 我们的 stub
        size_t length;               // 盖住的字节数（这些函数都是 5 字节热补丁序言）
        void** trampoline;           // 跳板指针（stub 跑完原序言跳回函数里去）
        void* cave;
        unsigned char original[16];
        bool installed;
    };

    void* g_trampoline_gl_begin = nullptr;
    void* g_trampoline_gl_draw_arrays = nullptr;
    void* g_trampoline_gl_draw_elements = nullptr;
    void* g_trampoline_swap_buffers = nullptr;

    // opengl32 里要直接调的函数（绕开我们自己的钩子）
    typedef void (APIENTRY* GlDisableProc)(GLenum);
    GlDisableProc g_gl_disable = nullptr;

    bool WriteCode(void* address, const void* bytes, size_t size)
    {
        DWORD old_protect = 0;
        if (VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect) == FALSE)
        {
            return false;
        }
        memcpy(address, bytes, size);
        DWORD ignored = 0;
        VirtualProtect(address, size, old_protect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), address, size);
        return true;
    }

    // 在目标附近（rel32 够得着的地方）申请一块可读可写可执行的内存，放跳板/代码洞穴
    void* AllocateNear(const void* target, size_t size)
    {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        uintptr_t granularity = info.dwAllocationGranularity != 0 ? info.dwAllocationGranularity : 0x10000;
        uintptr_t start = reinterpret_cast<uintptr_t>(target) & ~(granularity - 1);

        for (uintptr_t delta = 0; delta < 0x60000000u; delta += granularity)
        {
            uintptr_t candidates[2] = { start + delta, start >= delta ? start - delta : 0 };
            for (int index = 0; index < 2; index++)
            {
                uintptr_t candidate = candidates[index];
                if (candidate < 0x10000u || candidate > 0x7FFF0000u)
                {
                    continue;
                }
                void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (memory != nullptr)
                {
                    return memory;
                }
            }
        }
        return nullptr;
    }

    bool InstallHook(HookEntry& hook)
    {
        if (hook.installed)
        {
            return true;
        }
        if (hook.target == nullptr || hook.detour == nullptr || hook.trampoline == nullptr)
        {
            return false;
        }
        const unsigned char* header = static_cast<const unsigned char*>(hook.target);
        if (memcmp(hook.target, kHotPatchPrologue, sizeof(kHotPatchPrologue)) == 0)
        {
            // 常规情况：函数头正好是 5 字节热补丁序言，一条 jmp 盖住
        }
        else if (header[0] == 0xFF && header[1] == 0x25)
        {
            // Win10/11 上不少 user32 导出变成「转发桩」：FF 25 xx xx xx xx = jmp dword ptr [xx]
            // 这种要盖 6 个字节（多补一个 nop），跳板里照抄这条 jmp 就等于跳去真正的实现
            hook.length = 6;
        }
        else
        {
            return false;   // 既不是热补丁序言也不是转发桩，不敢乱盖
        }

        unsigned char* target = static_cast<unsigned char*>(hook.target);
        memcpy(hook.original, target, hook.length);

        unsigned char* cave = static_cast<unsigned char*>(AllocateNear(target, 64));
        if (cave == nullptr)
        {
            return false;
        }

        // 跳板：先执行被我们盖掉的原序言，再跳回原来的下一条指令
        memcpy(cave, target, hook.length);
        unsigned char* jump_back = cave + hook.length;
        jump_back[0] = 0xE9;
        *reinterpret_cast<int32_t*>(jump_back + 1) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(target) + hook.length - (reinterpret_cast<uintptr_t>(jump_back) + 5));

        // 目标入口：jmp 到我们的 stub（剩下的字节补 nop）
        unsigned char patch[16] = {};
        patch[0] = 0xE9;
        *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(hook.detour) - (reinterpret_cast<uintptr_t>(target) + 5));
        for (size_t index = hook.length; index < sizeof(patch); index++)
        {
            patch[index] = 0x90;
        }

        // stub 得先能跳回去，才能改目标入口
        *hook.trampoline = cave;
        if (!WriteCode(target, patch, hook.length))
        {
            *hook.trampoline = nullptr;
            VirtualFree(cave, 0, MEM_RELEASE);
            return false;
        }

        hook.cave = cave;
        hook.installed = true;
        return true;
    }

    void RemoveHook(HookEntry& hook)
    {
        if (!hook.installed)
        {
            return;
        }
        WriteCode(hook.target, hook.original, hook.length);
        hook.installed = false;
        // 先等一等：让正在跳板里跑的线程先跑完，再放掉这块内存
        Sleep(50);
        if (hook.cave != nullptr)
        {
            VirtualFree(hook.cave, 0, MEM_RELEASE);
            hook.cave = nullptr;
        }
        *hook.trampoline = nullptr;
    }


    // ============================ 各个 stub ============================
    // 说明：pushad 之后 [esp+32] 是返回地址，[esp+36] 才是第一个参数。
    // 我们的 C 函数是 cdecl，调用完 add esp,4 平栈；popad 还原所有寄存器，
    // 再跳到跳板里跑原序言 —— 游戏那边完全看不出区别。
    extern "C" void __cdecl GlBeginCallback(unsigned int mode);
    extern "C" void __cdecl GlDrawBatchCallback(unsigned int mode);
    extern "C" void __cdecl SwapBuffersCallback(HDC hdc);

    extern "C" void __declspec(naked) GlBeginStub()
    {
        __asm
        {
            pushad
            mov eax, dword ptr [esp + 36]
            push eax
            call GlBeginCallback
            add esp, 4
            popad
            jmp dword ptr [g_trampoline_gl_begin]
        }
    }

    extern "C" void __declspec(naked) GlDrawArraysStub()
    {
        __asm
        {
            pushad
            mov eax, dword ptr [esp + 36]
            push eax
            call GlDrawBatchCallback
            add esp, 4
            popad
            jmp dword ptr [g_trampoline_gl_draw_arrays]
        }
    }

    extern "C" void __declspec(naked) GlDrawElementsStub()
    {
        __asm
        {
            pushad
            mov eax, dword ptr [esp + 36]
            push eax
            call GlDrawBatchCallback
            add esp, 4
            popad
            jmp dword ptr [g_trampoline_gl_draw_elements]
        }
    }

    extern "C" void __declspec(naked) SwapBuffersStub()
    {
        __asm
        {
            pushad
            mov eax, dword ptr [esp + 36]
            push eax
            call SwapBuffersCallback
            add esp, 4
            popad
            jmp dword ptr [g_trampoline_swap_buffers]
        }
    }


    // ============================ 透视 ============================
    void TurnOffDepthTest()
    {
        if (g_gl_disable == nullptr)
        {
            return;
        }
        g_gl_disable(GL_DEPTH_TEST);
        if (g_shared != nullptr)
        {
            InterlockedExchange(&g_shared->wallhack_active, 1);
        }
    }

    // CT-8 那个脚本：glBegin 的 mode == 5（GL_TRIANGLE_STRIP）时关掉深度测试
    void __cdecl GlBeginCallback(unsigned int mode)
    {
        CaptureWorldMatrix();   // 顺手把 3D 世界的那份矩阵抄下来（相机位置、方框都用它）

        if (g_settings.wallhack_enabled == 0)
        {
            return;
        }
        if (mode == GL_TRIANGLE_STRIP)
        {
            TurnOffDepthTest();
            return;
        }
        if (g_settings.wallhack_aggressive != 0
            && (mode == GL_TRIANGLES || mode == GL_TRIANGLE_FAN || mode == GL_QUADS || mode == GL_QUAD_STRIP || mode == GL_POLYGON))
        {
            TurnOffDepthTest();
        }
    }

    // 激进模式：glDrawArrays / glDrawElements 这些批次也一起关
    void __cdecl GlDrawBatchCallback(unsigned int mode)
    {
        CaptureWorldMatrix();

        if (g_settings.wallhack_enabled == 0 || g_settings.wallhack_aggressive == 0)
        {
            return;
        }
        if (mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN
            || mode == GL_QUADS || mode == GL_QUAD_STRIP || mode == GL_POLYGON)
        {
            TurnOffDepthTest();
        }
    }


    // ============================ 游戏内画方框 ============================
    struct Entity
    {
        float x, y, z;
        float hp;
        bool teammate;
        bool teammate_known;
        bool from_crosshair;
    };

    // 世界坐标 -> 屏幕坐标（row-major 4x4，和界面里那个外部方框用的是同一套算法）
    bool WorldToScreen(const float matrix[16], const float world[3], float width, float height, bool flip_y, float& out_x, float& out_y)
    {
        float w = matrix[12] * world[0] + matrix[13] * world[1] + matrix[14] * world[2] + matrix[15];
        if (w <= 0.01f)
        {
            return false;   // 在摄像机后面（或者就在镜头上），算出来的屏幕坐标是乱的，不画
        }
        float clip_x = matrix[0] * world[0] + matrix[1] * world[1] + matrix[2] * world[2] + matrix[3];
        float clip_y = matrix[4] * world[0] + matrix[5] * world[1] + matrix[6] * world[2] + matrix[7];
        float ndc_x = clip_x / w;
        float ndc_y = clip_y / w;
        out_x = (width / 2.0f) * (ndc_x + 1.0f);
        out_y = flip_y ? (height / 2.0f) * (ndc_y + 1.0f) : (height / 2.0f) * (1.0f - ndc_y);
        return true;
    }

    void EnsureFont(HDC hdc)
    {
        HGLRC context = wglGetCurrentContext();
        if (context == nullptr || hdc == nullptr)
        {
            return;
        }
        if (context == g_font_context && g_font_base != 0)
        {
            return;
        }

        // 用一块自己的内存 DC 造字体位图，别去动游戏那个 DC
        HDC memory_dc = CreateCompatibleDC(hdc);
        if (memory_dc == nullptr)
        {
            return;
        }
        HFONT font = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
        HGDIOBJ old_font = font != nullptr ? SelectObject(memory_dc, font) : nullptr;

        GLuint base = glGenLists(128);
        if (base != 0 && wglUseFontBitmapsW(memory_dc, 0, 128, base) == FALSE)
        {
            glDeleteLists(base, 128);
            base = 0;
        }

        if (old_font != nullptr)
        {
            SelectObject(memory_dc, old_font);
        }
        if (font != nullptr)
        {
            DeleteObject(font);
        }
        DeleteDC(memory_dc);

        // 换了 GL 上下文（比如改了分辨率重建窗口）就重做一份，旧的留给它自己的上下文
        g_font_base = base;
        g_font_context = base != 0 ? context : nullptr;
    }

    void DrawText2D(float x, float y, const char* text)
    {
        if (g_font_base == 0 || text == nullptr || text[0] == '\0')
        {
            return;
        }
        glRasterPos2f(x, y);
        glListBase(g_font_base);
        glCallLists(static_cast<GLsizei>(strlen(text)), GL_UNSIGNED_BYTE, text);
    }

    void DrawBoxOutline(float left, float top, float right, float bottom)
    {
        glBegin(GL_LINE_LOOP);
        glVertex2f(left, top);
        glVertex2f(right, top);
        glVertex2f(right, bottom);
        glVertex2f(left, bottom);
        glEnd();
    }

    void SetColorFromColorRef(LONG color)
    {
        glColor3ub(GetRValue(static_cast<COLORREF>(color)),
            GetGValue(static_cast<COLORREF>(color)),
            GetBValue(static_cast<COLORREF>(color)));
    }

    int CollectEntities(const Settings& s, Entity* list, int capacity, const float* my, bool has_my, bool has_my_team, int my_team)
    {
        int count = 0;

        // ① CT「所有人物」指针指向的那个角色（不用扫描列表就能画他一个）
        if (s.show_crosshair_entity != 0 && count < capacity)
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (ReadChainFloat(s.crosshair_x, x) && ReadChainFloat(s.crosshair_y, y) && ReadChainFloat(s.crosshair_z, z))
            {
                float hp = 0.0f;
                bool has_hp = ReadChainFloat(s.crosshair_hp, hp);
                bool skip = false;
                if (has_my && s.max_distance > 1.0f)
                {
                    float dx = x - my[0], dy = y - my[1], dz = z - my[2];
                    skip = sqrtf(dx * dx + dy * dy + dz * dz) > s.max_distance;
                }
                if (!skip)
                {
                    Entity& entity = list[count++];
                    entity.x = x;
                    entity.y = y;
                    entity.z = z;
                    entity.hp = has_hp ? hp : -1.0f;
                    entity.teammate = false;
                    entity.teammate_known = false;
                    entity.from_crosshair = true;
                }
            }
        }

        // ② 界面上填的实体列表链
        uintptr_t list_address = ResolveChain(s.entity_list);
        if (list_address != 0)
        {
            int max_count = s.max_count > 0 && s.max_count < 1024 ? s.max_count : 32;
            int stride = s.stride > 0 ? s.stride : 4;
            for (int index = 0; index < max_count && count < capacity; index++)
            {
                uintptr_t entity = list_address + static_cast<uintptr_t>(index) * static_cast<uintptr_t>(stride);
                if (s.pointer_array != 0)
                {
                    uintptr_t slot = entity;
                    if (!SafeRead(reinterpret_cast<const void*>(slot), &entity, sizeof(entity)) || entity == 0)
                    {
                        continue;
                    }
                }

                float origin[3] = {};
                if (!SafeRead(reinterpret_cast<const void*>(entity + static_cast<uintptr_t>(s.origin_offset)), origin, sizeof(origin)))
                {
                    continue;
                }
                if (origin[0] == 0.0f && origin[1] == 0.0f && origin[2] == 0.0f)
                {
                    continue;   // 空位
                }
                if (has_my && s.max_distance > 1.0f)
                {
                    float dx = origin[0] - my[0], dy = origin[1] - my[1], dz = origin[2] - my[2];
                    if (sqrtf(dx * dx + dy * dy + dz * dz) > s.max_distance)
                    {
                        continue;
                    }
                }

                float hp = 0.0f;
                bool has_hp = SafeRead(reinterpret_cast<const void*>(entity + static_cast<uintptr_t>(s.health_offset)), &hp, sizeof(hp));
                int team = 0;
                bool has_team = SafeRead(reinterpret_cast<const void*>(entity + static_cast<uintptr_t>(s.team_offset)), &team, sizeof(team));

                Entity& out = list[count++];
                out.x = origin[0];
                out.y = origin[1];
                out.z = origin[2];
                out.hp = has_hp ? hp : -1.0f;
                out.teammate = has_my_team && has_team && team == my_team;
                out.teammate_known = has_my_team && has_team;
                out.from_crosshair = false;
            }
        }

        return count;
    }

    // ==================== 自己扫出游戏里所有角色（游戏内方框的数据源） ====================
    // 界面上的「实体列表链」本来要用户自己用 CE 扫，太麻烦。这个客户端里人物对象的形状
    // 非常固定，直接在进程里按指纹扫一遍更省事（自动找人物基址那段已经验证过这个形状）：
    //   B            = 人物对象（就是自动找基址扫到的 p1）
    //   [B+0x7C]     = 引擎对象 E，而且 [E+4] 正好等于 B+0x80 —— 这条是最硬的指纹
    //   B+0x1E0      = 血量（float，也就是 p3+0x160）
    //   B+0x88/8C/90 = 坐标（p3+0x08/0C/10 三个 float）
    //   E+0x1C8      = 阵营（1 恐怖分子 2 反恐精英）
    //   E+0x1CC      = 金钱
    // 扫描分片进行：每次最多花 900ms，记住扫到哪儿了，下次接着扫，不卡游戏。
    struct ScannedPlayer
    {
        uintptr_t block;
        uintptr_t engine;
        float hp;
        float x, y, z;
        int team;
        int money;
        int is_local;
    };

    const int kMaxScannedPlayers = 64;
    const size_t kScanChunk = 0x10000;
    ScannedPlayer g_scanned_players[kMaxScannedPlayers] = {};
    int g_scanned_count = 0;
    volatile LONG g_players_lock = 0;      // 和设置一样用序号锁：偶数 = 稳定，奇数 = 正在写
    uintptr_t g_scan_cursor = 0x10000u;    // 分片扫描扫到哪儿了

    bool ProbePlayerBlock(uintptr_t block, ScannedPlayer& out)
    {
        out = {};
        if (block < 0x10000u || block > 0x7FFF0000u || (block & 3u) != 0)
        {
            return false;
        }

        uint32_t engine_raw = 0, engine_ref = 0;
        if (!SafeRead(reinterpret_cast<const void*>(block + 0x7C), &engine_raw, sizeof(engine_raw)))
        {
            return false;
        }
        if (engine_raw < 0x10000u || engine_raw > 0x7FFF0000u || (engine_raw & 3u) != 0)
        {
            return false;
        }
        uintptr_t engine = engine_raw;
        if (!SafeRead(reinterpret_cast<const void*>(engine + 0x04), &engine_ref, sizeof(engine_ref)))
        {
            return false;
        }
        if (engine_ref != static_cast<uint32_t>(block + 0x80))
        {
            return false;   // 形状对不上，不是人物对象
        }

        float hp = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
        if (!SafeRead(reinterpret_cast<const void*>(block + 0x1E0), &hp, sizeof(hp)))
        {
            return false;
        }
        // 上限放宽一点：界面上血量最大能锁到 10000，锁了自己还认不出自己就白搭
        if (!(hp >= 0.0f && hp <= 100000.0f))
        {
            return false;
        }
        if (!SafeRead(reinterpret_cast<const void*>(block + 0x88), &x, sizeof(x))
            || !SafeRead(reinterpret_cast<const void*>(block + 0x8C), &y, sizeof(y))
            || !SafeRead(reinterpret_cast<const void*>(block + 0x90), &z, sizeof(z)))
        {
            return false;
        }
        if (!(x > -20000.0f && x < 20000.0f && y > -20000.0f && y < 20000.0f && z > -20000.0f && z < 20000.0f))
        {
            return false;
        }
        if (x == 0.0f && y == 0.0f && z == 0.0f)
        {
            return false;
        }

        int team = 0, money = 0;
        if (!SafeRead(reinterpret_cast<const void*>(engine + 0x1C8), &team, sizeof(team)))
        {
            return false;
        }
        if (team < 1 || team > 2)
        {
            return false;
        }
        if (!SafeRead(reinterpret_cast<const void*>(engine + 0x1CC), &money, sizeof(money)))
        {
            return false;
        }
        // 同理：金钱界面最多能锁到 100000，这里也别卡在 20000（CT 表里是 20000）
        if (money < 0 || money > 1000000)
        {
            return false;
        }

        out.block = block;
        out.engine = engine;
        out.hp = hp;
        out.x = x;
        out.y = y;
        out.z = z;
        out.team = team;
        out.money = money;
        out.is_local = 0;
        return true;
    }

    bool RefreshScannedPlayer(const ScannedPlayer& player, ScannedPlayer& fresh)
    {
        if (!ProbePlayerBlock(player.block, fresh))
        {
            return false;
        }
        fresh.is_local = player.is_local;   // 身份是我们自己标的，别被清掉
        return true;
    }

    bool ReadCamera(float camera[3])
    {
        if (g_shared == nullptr)
        {
            return false;
        }
        camera[0] = g_shared->camera_x;
        camera[1] = g_shared->camera_y;
        camera[2] = g_shared->camera_z;
        return camera[0] > -20000.0f && camera[0] < 20000.0f
            && camera[1] > -20000.0f && camera[1] < 20000.0f;
    }

    // 哪个是「自己」：优先用自动找基址找到的那个人物对象，找不到就挑离摄像机最近的那个
    void MarkLocalPlayer(ScannedPlayer* list, int count, uintptr_t known_block)
    {
        for (int index = 0; index < count; index++)
        {
            list[index].is_local = 0;
        }
        if (count <= 0)
        {
            return;
        }
        if (known_block != 0)
        {
            for (int index = 0; index < count; index++)
            {
                if (list[index].block == known_block)
                {
                    list[index].is_local = 1;
                    return;
                }
            }
        }

        float camera[3] = {};
        if (!ReadCamera(camera))
        {
            return;
        }
        int best = -1;
        float best_distance = 0.0f;
        for (int index = 0; index < count; index++)
        {
            float dx = list[index].x - camera[0], dy = list[index].y - camera[1];
            float distance = dx * dx + dy * dy;
            if (best < 0 || distance < best_distance)
            {
                best = index;
                best_distance = distance;
            }
        }
        if (best >= 0 && best_distance < 120.0f * 120.0f)
        {
            list[best].is_local = 1;   // 第一人称：离摄像机最近的就是自己
        }
    }

    void PublishScannedPlayers(const ScannedPlayer* list, int count)
    {
        if (count < 0)
        {
            count = 0;
        }
        if (count > kMaxScannedPlayers)
        {
            count = kMaxScannedPlayers;
        }
        InterlockedIncrement(&g_players_lock);       // 奇数：开始写
        for (int index = 0; index < count; index++)
        {
            g_scanned_players[index] = list[index];
        }
        g_scanned_count = count;
        InterlockedIncrement(&g_players_lock);       // 偶数：写完了
    }

    int ReadScannedPlayers(ScannedPlayer* out, int capacity)
    {
        if (capacity <= 0)
        {
            return 0;
        }
        LONG before = g_players_lock;
        if ((before & 1) != 0)
        {
            return 0;   // 正好在写，这一帧先不画
        }
        int count = g_scanned_count;
        if (count > capacity)
        {
            count = capacity;
        }
        for (int index = 0; index < count; index++)
        {
            out[index] = g_scanned_players[index];
        }
        if (g_players_lock != before)
        {
            return 0;
        }
        return count;
    }

    int ScanPlayersFromMemory(ScannedPlayer* list, int& count, int capacity, DWORD budget_ms, DWORD* elapsed_out)
    {
        DWORD started = GetTickCount();
        static unsigned char* chunk = nullptr;
        if (chunk == nullptr)
        {
            chunk = static_cast<unsigned char*>(VirtualAlloc(nullptr, kScanChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        }
        if (chunk == nullptr || list == nullptr || count < 0)
        {
            if (elapsed_out != nullptr)
            {
                *elapsed_out = 0;
            }
            return 0;
        }

        int added = 0;
        bool time_up = false;
        const uintptr_t limit = 0x7FFF0000u;
        uintptr_t cursor = g_scan_cursor;
        if (cursor < 0x10000u || cursor >= limit)
        {
            cursor = 0x10000u;
        }

        MEMORY_BASIC_INFORMATION info = {};
        while (cursor < limit && count < capacity && !time_up)
        {
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != sizeof(info))
            {
                cursor += 0x1000u;
                continue;
            }
            uintptr_t region_start = reinterpret_cast<uintptr_t>(info.BaseAddress);
            uintptr_t region_end = region_start + info.RegionSize;
            if (region_end <= cursor)
            {
                cursor += 0x1000u;
                continue;
            }

            // 只扫私有可写内存（堆），模块映射 / 显存那些跳过
            bool usable = info.State == MEM_COMMIT
                && info.Type == MEM_PRIVATE
                && (info.Protect == PAGE_READWRITE || info.Protect == PAGE_EXECUTE_READWRITE || info.Protect == PAGE_WRITECOPY)
                && info.RegionSize <= 0x8000000u;

            if (usable)
            {
                for (uintptr_t block = cursor; block < region_end; block += kScanChunk)
                {
                    size_t size = static_cast<size_t>(region_end - block);
                    if (size > kScanChunk)
                    {
                        size = kScanChunk;
                    }
                    if (SafeRead(reinterpret_cast<const void*>(block), chunk, size))
                    {
                        for (size_t offset = 0; offset + 0x240u <= size; offset += 4u)
                        {
                            // 先在本地这份拷贝里过一遍最便宜的筛子，能刷掉 99.99%
                            uint32_t engine_raw = 0;
                            memcpy(&engine_raw, chunk + offset + 0x7C, sizeof(engine_raw));
                            if (engine_raw < 0x10000u || engine_raw > 0x7FFF0000u || (engine_raw & 3u) != 0)
                            {
                                continue;
                            }

                            ScannedPlayer player = {};
                            if (!ProbePlayerBlock(block + offset, player))
                            {
                                continue;
                            }
                            bool duplicate = false;
                            for (int index = 0; index < count; index++)
                            {
                                if (list[index].block == player.block)
                                {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (duplicate)
                            {
                                continue;
                            }
                            list[count++] = player;
                            added++;
                            if (count >= capacity)
                            {
                                break;
                            }
                        }
                    }
                    if (count >= capacity)
                    {
                        cursor = block + size;
                        break;
                    }
                    if (GetTickCount() - started >= budget_ms)
                    {
                        cursor = block + size;   // 下次从这儿接着扫
                        time_up = true;
                        break;
                    }
                }
            }
            if (!time_up && count < capacity)
            {
                cursor = region_end;
            }
        }

        g_scan_cursor = cursor >= limit ? 0x10000u : cursor;
        if (elapsed_out != nullptr)
        {
            *elapsed_out = GetTickCount() - started;
        }
        return added;
    }

    // 每隔一秒把已找到的人刷新一遍（血量/坐标/阵营），走了或者换图了就把这条扔掉
    void RefreshPlayerList(uintptr_t known_block)
    {
        ScannedPlayer list[kMaxScannedPlayers] = {};
        int count = ReadScannedPlayers(list, kMaxScannedPlayers);
        int kept = 0;
        for (int index = 0; index < count; index++)
        {
            ScannedPlayer fresh = {};
            if (RefreshScannedPlayer(list[index], fresh))
            {
                list[kept++] = fresh;
            }
        }
        MarkLocalPlayer(list, kept, known_block);
        PublishScannedPlayers(list, kept);
        if (g_shared != nullptr)
        {
            InterlockedExchange(&g_shared->esp_players, kept);
        }
    }

    // 分片扫一遍内存，把新出来的人物加进列表
    void SweepPlayerMemory(uintptr_t known_block)
    {
        ScannedPlayer list[kMaxScannedPlayers] = {};
        int count = ReadScannedPlayers(list, kMaxScannedPlayers);
        DWORD elapsed = 0;
        int added = ScanPlayersFromMemory(list, count, kMaxScannedPlayers, 900, &elapsed);
        MarkLocalPlayer(list, count, known_block);
        PublishScannedPlayers(list, count);
        if (g_shared != nullptr)
        {
            InterlockedExchange(&g_shared->esp_players, count);
            int self_found = 0;
            for (int index = 0; index < count; index++)
            {
                if (list[index].is_local != 0)
                {
                    self_found = 1;
                    break;
                }
            }
            InterlockedExchange(&g_shared->esp_self_found, self_found);
            InterlockedExchange(&g_shared->scan_ms, static_cast<LONG>(elapsed));
            if (added > 0 && count > 0)
            {
                wchar_t text[CSTRAINER_HOOK_TEXT_LENGTH] = {};
                swprintf_s(text, L"自己扫到了 %d 个角色（不用填实体列表链，游戏里直接画框）", count);
                SetStatus(text);
            }
        }
    }

    void DrawFrame(HDC hdc, const Settings& s)
    {
        GLint viewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, viewport);
        float width = static_cast<float>(viewport[2]);
        float height = static_cast<float>(viewport[3]);
        if (width < 16.0f || height < 16.0f)
        {
            return;
        }

        float my[3] = {};
        bool has_my = ReadChainFloat(s.my_x, my[0]) && ReadChainFloat(s.my_y, my[1]) && ReadChainFloat(s.my_z, my[2]);
        if (!has_my && g_shared != nullptr && g_shared->player_found != 0)
        {
            // 训练器没填「我的坐标」那几条链时，用 DLL 自己定位到的人物坐标兜底
            my[0] = g_shared->player_x;
            my[1] = g_shared->player_y;
            my[2] = g_shared->player_z;
            has_my = true;
        }
        ScannedPlayer scanned[kMaxScannedPlayers] = {};
        int scanned_count = ReadScannedPlayers(scanned, kMaxScannedPlayers);
        if (!has_my && scanned_count > 0)
        {
            // 界面上的「我的坐标」没填 / 没值的时候，用自己扫到的那个人兜底
            for (int index = 0; index < scanned_count; index++)
            {
                if (scanned[index].is_local != 0)
                {
                    my[0] = scanned[index].x;
                    my[1] = scanned[index].y;
                    my[2] = scanned[index].z;
                    has_my = true;
                    break;
                }
            }
        }

        int my_team = 0;
        bool has_my_team = ReadChainInt(s.my_team, my_team);
        if (!has_my_team)
        {
            for (int index = 0; index < scanned_count; index++)
            {
                if (scanned[index].is_local != 0)
                {
                    my_team = scanned[index].team;
                    has_my_team = true;
                    break;
                }
            }
        }

        // 谁要这份方框数据：游戏内画框（esp_enabled）和训练器的外部「人物框框」（export_boxes）
        const bool want_in_game = s.esp_enabled != 0;
        const bool want_export = s.export_boxes != 0;
        const bool want_box_data = want_in_game || want_export;

        float matrix[16] = {};
        bool has_matrix = want_box_data && ReadMatrix(s.view_matrix, matrix);
        if (!has_matrix && want_box_data && g_gl_matrix_valid)
        {
            memcpy(matrix, g_gl_matrix, sizeof(matrix));   // 界面没填矩阵链就用 GL 里抄的那份
            has_matrix = true;
        }

        Entity entities[64] = {};
        int entity_count = 0;
        if (has_matrix)
        {
            // ① 优先用 DLL 自己扫出来的人（带血量/阵营，什么都不用填）
            if (scanned_count > 0)
            {
                for (int index = 0; index < scanned_count && entity_count < 64; index++)
                {
                    const ScannedPlayer& player = scanned[index];
                    if (player.is_local != 0 || player.hp <= 0.0f)
                    {
                        continue;   // 不画自己，也不画躺地上的
                    }
                    bool skip = false;
                    if (has_my && s.max_distance > 1.0f)
                    {
                        float dx = player.x - my[0], dy = player.y - my[1], dz = player.z - my[2];
                        skip = sqrtf(dx * dx + dy * dy + dz * dz) > s.max_distance;
                    }
                    if (skip)
                    {
                        continue;
                    }
                    Entity& out = entities[entity_count++];
                    out.x = player.x;
                    out.y = player.y;
                    out.z = player.z;
                    out.hp = player.hp;
                    out.teammate = has_my_team && player.team == my_team;
                    out.teammate_known = has_my_team;
                    out.from_crosshair = false;
                }
            }
            // ② 自己没扫到人（比如刚进图）就用界面上的链
            if (entity_count == 0)
            {
                entity_count = CollectEntities(s, entities, 64, my, has_my, has_my_team, my_team);
            }
        }

        GLint old_matrix_mode = GL_MODELVIEW;
        glGetIntegerv(GL_MATRIX_MODE, &old_matrix_mode);
        GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
        GLboolean texture_was_on = glIsEnabled(GL_TEXTURE_2D);
        GLboolean lighting_was_on = glIsEnabled(GL_LIGHTING);
        GLboolean cull_was_on = glIsEnabled(GL_CULL_FACE);
        GLboolean blend_was_on = glIsEnabled(GL_BLEND);

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(width), static_cast<double>(height), 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_LIGHTING);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        EnsureFont(hdc);

        int drawn = 0;
        int exported = 0;
        if (g_shared != nullptr && want_export)
        {
            InterlockedIncrement(&g_shared->overlay_boxes_seq);   // 变奇数：开始写
        }
        for (int index = 0; index < entity_count; index++)
        {
            const Entity& entity = entities[index];
            float screen_x = 0.0f, screen_y = 0.0f;
            const float world[3] = { entity.x, entity.y, entity.z };
            if (!WorldToScreen(matrix, world, width, height, s.flip_y != 0, screen_x, screen_y))
            {
                continue;
            }

            float scale = 1.0f;
            if (has_my)
            {
                float dx = entity.x - my[0], dy = entity.y - my[1], dz = entity.z - my[2];
                float distance = sqrtf(dx * dx + dy * dy + dz * dz);
                if (distance > 1.0f)
                {
                    scale = 2000.0f / distance;
                }
            }
            if (scale < 0.15f) scale = 0.15f;
            if (scale > 2.5f) scale = 2.5f;

            float box_width = s.box_width > 0 ? static_cast<float>(s.box_width) : 34.0f;
            float box_height = s.box_height > 0 ? static_cast<float>(s.box_height) : 76.0f;
            float half = box_width * scale * 0.5f;
            float bottom = screen_y;
            float top = screen_y - box_height * scale;

            if (want_export && g_shared != nullptr && exported < CSTRAINER_HOOK_MAX_BOXES)
            {
                // 把算好的框抄给训练器（外部「人物框框」用，不用手动填任何链）
                g_shared->overlay_boxes[exported].left = screen_x - half;
                g_shared->overlay_boxes[exported].top = top;
                g_shared->overlay_boxes[exported].right = screen_x + half;
                g_shared->overlay_boxes[exported].bottom = bottom;
                g_shared->overlay_boxes[exported].hp = entity.hp;
                g_shared->overlay_boxes[exported].teammate_known = entity.teammate_known ? 1 : 0;
                g_shared->overlay_boxes[exported].teammate = entity.teammate ? 1 : 0;
                exported++;
            }

            if (!want_in_game)
            {
                continue;   // 只要外部框数据的时候，不往游戏画面上画
            }

            if (entity.teammate_known)
            {
                SetColorFromColorRef(entity.teammate ? s.color_team : s.color_enemy);
            }
            else
            {
                SetColorFromColorRef(s.color_unknown);
            }

            DrawBoxOutline(screen_x - half, top, screen_x + half, bottom);
            drawn++;

            if (s.show_hp_text != 0 && entity.hp >= 0.0f && g_font_base != 0)
            {
                char text[32] = {};
                sprintf_s(text, "%.0f", entity.hp);
                DrawText2D(screen_x - half, top - 13.0f, text);
            }
        }

        if (g_shared != nullptr && want_export)
        {
            InterlockedExchange(&g_shared->overlay_boxes_count, exported);
            InterlockedIncrement(&g_shared->overlay_boxes_seq);   // 变偶数：写完了
        }

        if (s.debug_marker != 0)
        {
            // 一个小十字 + 一行英文：用来确认 hook 真的在游戏里渲染
            glColor3ub(255, 235, 64);
            float cx = width * 0.5f;
            float cy = height * 0.5f;
            glBegin(GL_LINES);
            glVertex2f(cx - 12.0f, cy);
            glVertex2f(cx + 12.0f, cy);
            glVertex2f(cx, cy - 12.0f);
            glVertex2f(cx, cy + 12.0f);
            glEnd();

            char line[128] = {};
            sprintf_s(line, "CSTRAINER HOOK ON  boxes=%d  entities=%d", drawn, entity_count);
            DrawText2D(12.0f, 22.0f, line);

            // 方框数量/角色数量已经用「上一帧方框 N 个 / 角色 M 个」写在训练器状态栏了，
            // 游戏里就只留上面那行小字，别挡着玩家的视线。
        }

        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(old_matrix_mode);

        // 把游戏原来的状态还回去（透视开着的时候不要动深度测试那一位）
        if (texture_was_on) glEnable(GL_TEXTURE_2D);
        if (lighting_was_on) glEnable(GL_LIGHTING);
        if (cull_was_on) glEnable(GL_CULL_FACE);
        if (blend_was_on) glEnable(GL_BLEND);
        if (depth_was_on && s.wallhack_enabled == 0) glEnable(GL_DEPTH_TEST);

        if (g_shared != nullptr)
        {
            InterlockedExchange(&g_shared->last_boxes, drawn);
            InterlockedExchange(&g_shared->last_entities, entity_count);
        }
    }

    // ==================== 鼠标解锁（ClipCursor 钩子） ====================
    // 这个客户端（Esai 2738）窗口化进游戏之后会把光标锁死：每帧 ClipCursor + 把光标
    // 拽回窗口中间，鼠标根本移不到训练器上 —— 界面里「解锁鼠标」就是把这个挡掉。
    // 做法：钩住进程里的 ClipCursor，勾上解锁之后游戏再调就直接返回 TRUE，
    // 光标锁不住，鼠标就能正常在窗口之间移动（游戏里的转视角照旧能用）。
    void* g_trampoline_clip_cursor = nullptr;
    volatile LONG g_clip_skip = 0;

    extern "C" LONG __cdecl ClipCursorCallback();

    extern "C" void __declspec(naked) ClipCursorStub()
    {
        __asm
        {
            pushad
            call ClipCursorCallback
            test eax, eax
            jz run_original
            popad
            mov eax, 1
            ret 4
        run_original:
            popad
            jmp dword ptr [g_trampoline_clip_cursor]
        }
    }

    LONG __cdecl ClipCursorCallback()
    {
        if (g_shared != nullptr && g_clip_skip != 0)
        {
            InterlockedExchange(&g_shared->mouse_released, 1);
            return 1;
        }
        return 0;
    }

    HookEntry g_clip_cursor_hook =
    {
        L"ClipCursor", "ClipCursor", nullptr, reinterpret_cast<void*>(&ClipCursorStub),
        5, &g_trampoline_clip_cursor, nullptr, {}, false
    };

    // ---------------- 光标复位（SetCursorPos）钩子 ----------------
    // 光挡住 ClipCursor 还不够：这个客户端窗口化之后引擎自己每帧都会把光标「拽回窗口客户区中心」
    // （Quake 那套鼠标增量做法），表现就是鼠标一直弹回中间、根本点不到训练器。
    // 所以勾了「解锁鼠标」之后，凡是「把光标移到窗口客户区中心（±4 像素）」的调用，
    // 我们直接返回 TRUE 假装移好了，实际不动它 —— 光标就能停在你放的地方。
    // 其它位置的 SetCursorPos 一律放行（控制台、菜单、窗口居中那些照旧）。
    void* g_trampoline_set_cursor_pos = nullptr;
    HWND g_game_hwnd = nullptr;              // 游戏窗口（第一次 SwapBuffers 的时候认一下）
    volatile LONG g_cursor_pin_blocked = 0;

    extern "C" int __cdecl SetCursorPosCallback(int x, int y, unsigned int caller);
    extern "C" void __declspec(naked) SetCursorPosStub()
    {
        __asm
        {
            pushad
            // 压栈顺序要和 cdecl 的取参顺序对上：先压返回地址，再压 Y，最后压 X，
            // 这样回调里 (x, y, caller) 才对得上（压错顺序 X/Y 会反过来）
            mov eax, dword ptr [esp + 32]      // 调用者的返回地址（谁在复位光标）
            push eax
            mov eax, dword ptr [esp + 44]      // 第 2 个参数 Y
            push eax
            mov eax, dword ptr [esp + 44]      // 第 1 个参数 X
            push eax
            call SetCursorPosCallback
            add esp, 12
            test eax, eax
            jz set_cursor_run_original
            popad
            mov eax, 1                         // 挡住：跟引擎说「移好了」
            ret 8
        set_cursor_run_original:
            popad
            jmp dword ptr [g_trampoline_set_cursor_pos]
        }
    }

    // 引擎复位光标的那个点：客户区中心和整个窗口中心都算出来，X/Y 反着传的也认
    // （实测这个客户端用的是「整窗口中心的 Y」和「整窗口中心的 X」反着传过来的）
    bool IsCursorRecentering(int x, int y)
    {
        if (g_game_hwnd == nullptr)
        {
            return false;
        }
        int centers[2][2] = { { 0, 0 }, { 0, 0 } };
        int count = 0;
        RECT rect = {};
        if (GetClientRect(g_game_hwnd, &rect) != FALSE)
        {
            POINT point = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
            if (ClientToScreen(g_game_hwnd, &point) != FALSE)
            {
                centers[count][0] = point.x;
                centers[count][1] = point.y;
                count++;
            }
        }
        if (count < 2 && GetWindowRect(g_game_hwnd, &rect) != FALSE)
        {
            centers[count][0] = (rect.left + rect.right) / 2;
            centers[count][1] = (rect.top + rect.bottom) / 2;
            count++;
        }
        if (g_shared != nullptr)
        {
            if (count > 0)
            {
                InterlockedExchange(&g_shared->cursor_pin_center_x, centers[0][0]);
                InterlockedExchange(&g_shared->cursor_pin_center_y, centers[0][1]);
            }
            if (count > 1)
            {
                InterlockedExchange(&g_shared->cursor_pin_win_center_x, centers[1][0]);
                InterlockedExchange(&g_shared->cursor_pin_win_center_y, centers[1][1]);
            }
        }
        for (int index = 0; index < count; index++)
        {
            if (x >= centers[index][0] - 8 && x <= centers[index][0] + 8 &&
                y >= centers[index][1] - 8 && y <= centers[index][1] + 8)
            {
                return true;
            }
            if (y >= centers[index][0] - 8 && y <= centers[index][0] + 8 &&
                x >= centers[index][1] - 8 && x <= centers[index][1] + 8)
            {
                return true;
            }
        }
        return false;
    }

    int __cdecl SetCursorPosCallback(int x, int y, unsigned int caller)
    {
        if (g_shared != nullptr)
        {
            InterlockedIncrement(&g_shared->cursor_pin_calls);
            InterlockedExchange(&g_shared->cursor_pin_last_x, x);
            InterlockedExchange(&g_shared->cursor_pin_last_y, y);
            LONG slot = InterlockedIncrement(&g_shared->cursor_pin_log_pos) % CSTRAINER_HOOK_PIN_LOG;
            if (slot < 0)
            {
                slot += CSTRAINER_HOOK_PIN_LOG;
            }
            InterlockedExchange(&g_shared->cursor_pin_log_x[slot], x);
            InterlockedExchange(&g_shared->cursor_pin_log_y[slot], y);
            InterlockedExchange(&g_shared->cursor_pin_log_ret[slot], static_cast<LONG>(caller));
        }
        if (g_clip_skip == 0 || !IsCursorRecentering(x, y))
        {
            return 0;
        }
        InterlockedIncrement(&g_cursor_pin_blocked);
        if (g_shared != nullptr)
        {
            InterlockedIncrement(&g_shared->cursor_pin_blocked);
        }
        return 1;
    }

    HookEntry g_set_cursor_pos_hook =
    {
        L"SetCursorPos", "SetCursorPos", nullptr, reinterpret_cast<void*>(&SetCursorPosStub),
        5, &g_trampoline_set_cursor_pos, nullptr, {}, false
    };


    // ==================== 抓一张游戏画面（含 DLL 画的方框） ====================
    typedef void (APIENTRY* GlReadPixelsProc)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
    typedef void (APIENTRY* GlGetIntegervProc)(GLenum, GLint*);
    typedef void (APIENTRY* GlGetFloatvProc)(GLenum, GLfloat*);

    GlReadPixelsProc g_gl_read_pixels = nullptr;
    GlGetIntegervProc g_gl_get_integerv = nullptr;
    GlGetFloatvProc g_gl_get_floatv = nullptr;

    bool WriteBmp24(const wchar_t* path, const unsigned char* bgra_bottom_up, int width, int height)
    {
        int row_raw = width * 3;
        int row_padded = (row_raw + 3) & ~3;
        DWORD image_size = static_cast<DWORD>(row_padded) * static_cast<DWORD>(height);
        DWORD file_size = 54u + image_size;

        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        unsigned char header[54] = {};
        header[0] = 'B';
        header[1] = 'M';
        *reinterpret_cast<DWORD*>(header + 2) = file_size;
        *reinterpret_cast<DWORD*>(header + 10) = 54;
        *reinterpret_cast<DWORD*>(header + 14) = 40;
        *reinterpret_cast<int*>(header + 18) = width;
        *reinterpret_cast<int*>(header + 22) = height;
        *reinterpret_cast<WORD*>(header + 26) = 1;
        *reinterpret_cast<WORD*>(header + 28) = 24;
        *reinterpret_cast<DWORD*>(header + 34) = image_size;

        DWORD written = 0;
        bool ok = WriteFile(file, header, 54, &written, nullptr) != FALSE;

        unsigned char* row = static_cast<unsigned char*>(malloc(static_cast<size_t>(row_padded)));
        if (row == nullptr)
        {
            CloseHandle(file);
            return false;
        }
        for (int y = 0; ok && y < height; y++)
        {
            const unsigned char* src = bgra_bottom_up + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
            for (int x = 0; x < width; x++)
            {
                row[x * 3 + 0] = src[x * 4 + 2];   // B
                row[x * 3 + 1] = src[x * 4 + 1];   // G
                row[x * 3 + 2] = src[x * 4 + 0];   // R
            }
            for (int pad = row_raw; pad < row_padded; pad++)
            {
                row[pad] = 0;
            }
            ok = WriteFile(file, row, static_cast<DWORD>(row_padded), &written, nullptr) != FALSE;
        }
        free(row);
        CloseHandle(file);
        return ok;
    }

    // glReadPixels 读的是后缓冲（我们是在 wglSwapBuffers 之前抓的），
    // 所以 DLL 这一帧画的方框也在图里 —— 正好用来确认游戏内渲染到底通没通。
    void CaptureFrame(const Settings& settings)
    {
        LONG done = -1;
        if (g_gl_read_pixels != nullptr && g_gl_get_integerv != nullptr && settings.screenshot_path[0] != 0)
        {
            GLint viewport[4] = {};
            g_gl_get_integerv(GL_VIEWPORT, viewport);
            int width = viewport[2];
            int height = viewport[3];
            if (width > 0 && height > 0 && width <= 4096 && height <= 4096)
            {
                size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
                unsigned char* pixels = static_cast<unsigned char*>(malloc(bytes));
                if (pixels != nullptr)
                {
                    g_gl_read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    done = WriteBmp24(settings.screenshot_path, pixels, width, height) ? 1 : -1;
                    free(pixels);
                }
            }
        }
        if (g_shared != nullptr)
        {
            InterlockedExchange(&g_shared->screenshot_done, done);
            if (done == 1)
            {
                SetStatus(L"刚抓了一张游戏画面（含 DLL 画的方框），训练器那边可以打开看");
            }
            else
            {
                SetStatus(L"抓图失败：写不进那个路径，或者没抓到 GL 的帧缓冲");
            }
        }
    }


    // ==================== 自动找人物基址 ====================
    // CT 表里 v2728 / v3248 的静态地址（cstrike.exe+D6E63C 之类）只对作者那台机器的
    // 版本有效。这个 Esai 2738 客户端里那两条静态地址全是 0，人物链整条都是空的。
    // 好在链的形状没变： 静态 -> p1 ; [p1+7C] = p2 ; [p2+04] = p3 ; p3+160 = 血量(float)
    // 所以直接在游戏主模块的可写节里按这个形状扫，扫到就把基址回报给训练器，
    // 界面上「本人血量 / 金钱 / 护甲」这些就能直接用。
    struct LocatedPlayer
    {
        uintptr_t module_base;
        uintptr_t static_address;
        uintptr_t p1;
        uintptr_t p2;
        uintptr_t p3;
        float hp;
        int money;
        float armor;
        int score;
    };

    bool ReadU32(const void* address, uint32_t& value)
    {
        return SafeRead(address, &value, sizeof(value));
    }

    bool LocatePlayerChain(LocatedPlayer& best)
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (base == 0)
        {
            return false;
        }

        uint32_t lfanew = 0;
        if (!ReadU32(reinterpret_cast<const void*>(base + 0x3C), lfanew) || lfanew == 0 || lfanew > 0x1000)
        {
            return false;
        }
        uintptr_t nt = base + lfanew;
        uint32_t file_header = 0;
        if (!ReadU32(reinterpret_cast<const void*>(nt + 4), file_header))
        {
            return false;
        }
        uint32_t section_count = (file_header >> 16) & 0xFFFF;
        uint32_t optional_size = 0;
        if (!ReadU32(reinterpret_cast<const void*>(nt + 20), optional_size))
        {
            return false;
        }
        uintptr_t section = nt + 24 + (optional_size & 0xFFFF);
        if (section_count == 0 || section_count > 64)
        {
            return false;
        }

        best = {};
        best.score = 0;
        LocatedPlayer fallback = {};

        for (uint32_t index = 0; index < section_count; index++)
        {
            uintptr_t header = section + static_cast<uintptr_t>(index) * 40u;
            uint32_t virtual_size = 0, virtual_address = 0, characteristics = 0;
            if (!ReadU32(reinterpret_cast<const void*>(header + 8), virtual_size)
                || !ReadU32(reinterpret_cast<const void*>(header + 12), virtual_address)
                || !ReadU32(reinterpret_cast<const void*>(header + 36), characteristics))
            {
                continue;
            }
            if ((characteristics & 0x80000000u) == 0 || virtual_size == 0)
            {
                continue;   // 只扫可写节（全局变量都在那）
            }
            if (virtual_size > 0x4000000u)
            {
                virtual_size = 0x4000000u;
            }

            uintptr_t start = base + virtual_address;
            uintptr_t end = start + virtual_size - 4u;
            for (uintptr_t cursor = start; cursor <= end; cursor += 4u)
            {
                uint32_t candidate = 0;
                if (!ReadU32(reinterpret_cast<const void*>(cursor), candidate))
                {
                    continue;
                }
                // 指针必须是 4 字节对齐的，不然多半是巧合的整数（之前踩过：把随机数据
                // 当成人物的静态地址报给界面，血量 67 金钱 1280 全是假的）
                if (candidate < 0x10000u || candidate > 0x7FFF0000u || (candidate & 3u) != 0)
                {
                    continue;
                }
                uintptr_t p1 = candidate;
                uint32_t p2_raw = 0, p3_raw = 0;
                if (!ReadU32(reinterpret_cast<const void*>(p1 + 0x7C), p2_raw)
                    || p2_raw < 0x10000u || p2_raw > 0x7FFF0000u || (p2_raw & 3u) != 0)
                {
                    continue;
                }
                uintptr_t p2 = p2_raw;
                if (!ReadU32(reinterpret_cast<const void*>(p2 + 0x04), p3_raw)
                    || (p3_raw & 3u) != 0)
                {
                    continue;
                }
                uintptr_t p3 = p3_raw;
                // 这条是这个客户端最硬的指纹：[p2+4] 正好指回 p1+0x80，对不上就不是人物
                if (p3 != p1 + 0x80)
                {
                    continue;
                }

                float hp = 0.0f, armor = -1.0f;
                int money = -1;
                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                if (!SafeRead(reinterpret_cast<const void*>(p3 + 0x08), &cx, sizeof(cx))
                    || !SafeRead(reinterpret_cast<const void*>(p3 + 0x0C), &cy, sizeof(cy))
                    || !SafeRead(reinterpret_cast<const void*>(p3 + 0x10), &cz, sizeof(cz)))
                {
                    continue;
                }
                if (!(cx > -20000.0f && cx < 20000.0f && cy > -20000.0f && cy < 20000.0f && cz > -20000.0f && cz < 20000.0f))
                {
                    continue;
                }
                if (cx == 0.0f && cy == 0.0f && cz == 0.0f)
                {
                    continue;
                }
                if (!SafeRead(reinterpret_cast<const void*>(p3 + 0x160), &hp, sizeof(hp)))
                {
                    continue;
                }
                if (!(hp >= 1.0f && hp <= 200.0f))
                {
                    continue;
                }
                if (!SafeRead(reinterpret_cast<const void*>(p2 + 0x1CC), &money, sizeof(money)))
                {
                    continue;
                }
                if (money < 0 || money > 20000)
                {
                    continue;
                }
                if (!SafeRead(reinterpret_cast<const void*>(p3 + 0x1BC), &armor, sizeof(armor)))
                {
                    continue;
                }
                if (!(armor >= 0.0f && armor <= 200.0f))
                {
                    continue;
                }

                // 这份客户端里能过指纹的链不止一条（实测 0x102D480 指的是别人、0x1100ABC 才是我自己），
                // 用 GL 世界矩阵抄下来的摄像机位置区别一下：我人物的坐标就在摄像机脚下，
                // 离得太远的先记下来当兜底，继续往后找。
                float camera_x = 0.0f, camera_y = 0.0f, camera_z = 0.0f;
                bool has_camera = false;
                if (g_shared != nullptr)
                {
                    camera_x = g_shared->camera_x;
                    camera_y = g_shared->camera_y;
                    camera_z = g_shared->camera_z;
                    has_camera = (camera_x != 0.0f || camera_y != 0.0f || camera_z != 0.0f);
                }
                if (has_camera)
                {
                    float dx = cx - camera_x, dy = cy - camera_y, dz = cz - camera_z;
                    if (sqrtf(dx * dx + dy * dy + dz * dz) > 150.0f)
                    {
                        if (fallback.score == 0)
                        {
                            fallback.score = 1;
                            fallback.module_base = base;
                            fallback.static_address = cursor;
                            fallback.p1 = p1;
                            fallback.p2 = p2;
                            fallback.p3 = p3;
                            fallback.hp = hp;
                            fallback.money = money;
                            fallback.armor = armor;
                        }
                        continue;   // 不是我，接着找
                    }
                }

                best.score = 2;
                best.module_base = base;
                best.static_address = cursor;
                best.p1 = p1;
                best.p2 = p2;
                best.p3 = p3;
                best.hp = hp;
                best.money = money;
                best.armor = armor;
                return true;
            }
        }
        if (best.score == 0 && fallback.score > 0)
        {
            best = fallback;   // 实在没找到「自己」就用第一条过指纹的兜底
        }
        return best.score > 0;
    }

    void ReportLocatedPlayer(const LocatedPlayer& found)
    {
        if (g_shared == nullptr)
        {
            return;
        }
        InterlockedExchange(&g_shared->player_module_base, static_cast<LONG>(found.module_base));
        InterlockedExchange(&g_shared->player_static_offset, static_cast<LONG>(found.static_address - found.module_base));
        InterlockedExchange(&g_shared->player_p1, static_cast<LONG>(found.p1));
        InterlockedExchange(&g_shared->player_p2, static_cast<LONG>(found.p2));
        InterlockedExchange(&g_shared->player_p3, static_cast<LONG>(found.p3));

        uint32_t weapon = 0, ammo = 0;
        if (ReadU32(reinterpret_cast<const void*>(found.p2 + 0x5E8), weapon))
        {
            InterlockedExchange(&g_shared->player_weapon, static_cast<LONG>(weapon));
            ReadU32(reinterpret_cast<const void*>(weapon + 0xCC), ammo);
        }
        else
        {
            InterlockedExchange(&g_shared->player_weapon, 0);
        }

        float x = 0.0f, y = 0.0f, z = 0.0f;
        SafeRead(reinterpret_cast<const void*>(found.p3 + 0x08), &x, sizeof(x));
        SafeRead(reinterpret_cast<const void*>(found.p3 + 0x0C), &y, sizeof(y));
        SafeRead(reinterpret_cast<const void*>(found.p3 + 0x10), &z, sizeof(z));

        LONG* hp_bits = reinterpret_cast<LONG*>(const_cast<float*>(&found.hp));
        LONG* armor_bits = reinterpret_cast<LONG*>(const_cast<float*>(&found.armor));
        LONG* x_bits = reinterpret_cast<LONG*>(&x);
        LONG* y_bits = reinterpret_cast<LONG*>(&y);
        LONG* z_bits = reinterpret_cast<LONG*>(&z);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->player_hp), *hp_bits);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->player_armor), *armor_bits);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->player_x), *x_bits);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->player_y), *y_bits);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->player_z), *z_bits);
        InterlockedExchange(&g_shared->player_money, found.money);
        InterlockedExchange(&g_shared->player_ammo, static_cast<LONG>(ammo));
        InterlockedExchange(&g_shared->player_found, 1);
    }

    void RefreshLocatedPlayer(const LocatedPlayer& found)
    {
        if (g_shared == nullptr)
        {
            return;
        }
        float hp = 0.0f, armor = 0.0f;
        int money = -1;
        if (SafeRead(reinterpret_cast<const void*>(found.p3 + 0x160), &hp, sizeof(hp)))
        {
            LONG bits = *reinterpret_cast<LONG*>(&hp);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->player_hp), bits);
        }
        if (SafeRead(reinterpret_cast<const void*>(found.p3 + 0x1BC), &armor, sizeof(armor)))
        {
            LONG bits = *reinterpret_cast<LONG*>(&armor);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->player_armor), bits);
        }
        if (SafeRead(reinterpret_cast<const void*>(found.p2 + 0x1CC), &money, sizeof(money)))
        {
            InterlockedExchange(&g_shared->player_money, money);
        }
        uint32_t weapon = 0, ammo = 0;
        if (ReadU32(reinterpret_cast<const void*>(found.p2 + 0x5E8), weapon))
        {
            InterlockedExchange(&g_shared->player_weapon, static_cast<LONG>(weapon));
            if (ReadU32(reinterpret_cast<const void*>(weapon + 0xCC), ammo))
            {
                InterlockedExchange(&g_shared->player_ammo, static_cast<LONG>(ammo));
            }
        }
    }


    // ==================== 从 GL 里直接拿视角矩阵 ====================
    // 训练器界面上那条「视角矩阵链」要用户自己 CE 扫，太麻烦了。
    // 我们人就住在游戏进程里，直接在 wglSwapBuffers 里问 GL 要 modelview / projection /
    // viewport，自己乘出一个「世界 -> 屏幕」矩阵 —— 方框就不用再依赖任何内存链了。
    void MultiplyMatrix(const GLfloat* a, const GLfloat* b, GLfloat* out)   // out = a * b（列主序）
    {
        for (int column = 0; column < 4; column++)
        {
            for (int row = 0; row < 4; row++)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; k++)
                {
                    sum += a[k * 4 + row] * b[column * 4 + k];
                }
                out[column * 4 + row] = sum;
            }
        }
    }

    void CaptureGlMatrix()
    {
        if (g_gl_get_floatv == nullptr || g_gl_get_integerv == nullptr)
        {
            g_gl_matrix_valid = false;
            return;
        }
        GLfloat modelview[16] = {};
        GLfloat projection[16] = {};
        GLfloat combined[16] = {};
        g_gl_get_floatv(GL_MODELVIEW_MATRIX, modelview);
        g_gl_get_floatv(GL_PROJECTION_MATRIX, projection);
        MultiplyMatrix(projection, modelview, combined);

        // OpenGL 是列主序，WorldToScreen 用的是行主序，这里转一下
        for (int row = 0; row < 4; row++)
        {
            for (int column = 0; column < 4; column++)
            {
                g_gl_matrix[row * 4 + column] = combined[column * 4 + row];
            }
        }
        g_gl_matrix_valid = true;
        if (g_shared != nullptr)
        {
            // 相机位置 = -R^T * t（modelview 的旋转是正交的）
            float tx = modelview[12], ty = modelview[13], tz = modelview[14];
            float cx = -(modelview[0] * tx + modelview[1] * ty + modelview[2] * tz);
            float cy = -(modelview[4] * tx + modelview[5] * ty + modelview[6] * tz);
            float cz = -(modelview[8] * tx + modelview[9] * ty + modelview[10] * tz);
            LONG bx = *reinterpret_cast<LONG*>(&cx), by = *reinterpret_cast<LONG*>(&cy), bz = *reinterpret_cast<LONG*>(&cz);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->camera_x), bx);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->camera_y), by);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_shared->camera_z), bz);
            InterlockedIncrement(&g_shared->matrix_found);
        }
    }


    volatile LONG g_world_matrix_captured = 0;   // 这一帧的 3D 世界矩阵抓过没

    // 在 3D 批次绘制的时候抓矩阵。SwapBuffers 那一刻 GL 里剩下的是 HUD 的 2D 矩阵
    // （实测相机位置永远是 0），所以改成在世界渲染的时候抓：世界用的是透视投影，
    // HUD / 控制台用的是 glOrtho（正交），用这个区分，只抄 3D 那一份。
    void CaptureWorldMatrix()
    {
        if (g_world_matrix_captured != 0 || g_gl_get_floatv == nullptr)
        {
            return;
        }
        GLfloat projection[16] = {};
        g_gl_get_floatv(GL_PROJECTION_MATRIX, projection);
        bool orthographic = projection[15] == 1.0f && projection[3] == 0.0f
            && projection[7] == 0.0f && projection[11] == 0.0f;
        if (orthographic)
        {
            return;   // 这是在画 HUD，不是世界
        }
        CaptureGlMatrix();
        InterlockedExchange(&g_world_matrix_captured, 1);
    }
    volatile LONG g_last_shot_request = 0;   // 上次抓图用的请求号（训练器每点一次 +1）

    // ==================== 每帧顺手做的杂活 ====================
    void ServiceJobs(const Settings& settings)
    {
        // 1) 鼠标解锁：勾上之后把 ClipCursor 挡掉，另外每秒补一次 ClipCursor(NULL)，
        //    免得游戏在我们钩子装上之前先锁了一次。
        if (settings.release_mouse != 0)
        {
            g_clip_skip = 1;
            static DWORD last_unclip = 0;
            DWORD now = GetTickCount();
            if (now - last_unclip > 1000)
            {
                last_unclip = now;
                typedef BOOL (WINAPI* ClipCursorProc)(const RECT*);
                HMODULE user32 = GetModuleHandleW(L"user32.dll");
                ClipCursorProc proc = user32 != nullptr ? reinterpret_cast<ClipCursorProc>(GetProcAddress(user32, "ClipCursor")) : nullptr;
                if (proc != nullptr)
                {
                    proc(nullptr);
                }
                while (ShowCursor(TRUE) < 0)
                {
                    // 游戏可能把光标藏了，这里补一下（多余的话 ShowCursor 会计数，无所谓）
                    if (ShowCursor(TRUE) >= 0)
                    {
                        break;
                    }
                }
                if (g_shared != nullptr)
                {
                    InterlockedExchange(&g_shared->mouse_released, 1);
                }
            }
        }
        else
        {
            g_clip_skip = 0;
            if (g_shared != nullptr)
            {
                InterlockedExchange(&g_shared->mouse_released, 0);
            }
        }

    }

    // 抓图要放在「画完方框之后」：glReadPixels 读的是后缓冲，我们在 SwapBuffers 里画的
    // 东西还留在后缓冲里，这时候抓才能把方框一起抓进去（之前放在前面，抓到的全是没框的画面）。
    //    训练器每点一次「抓图」这个请求号就 +1，只认没抓过的号 —— 不然每帧都写一次
    //    文件，游戏会卡死，BMP 也一直被占用打不开。
    void ServiceScreenshot(const Settings& settings)
    {
        if (settings.screenshot_request != 0 && settings.screenshot_request != g_last_shot_request)
        {
            g_last_shot_request = settings.screenshot_request;
            CaptureFrame(settings);
        }
    }
    void __cdecl SwapBuffersCallback(HDC hdc)
    {
        if (g_shared == nullptr)
        {
            return;
        }
        InterlockedIncrement(&g_shared->frames);
        if (g_game_hwnd == nullptr)
        {
            g_game_hwnd = WindowFromDC(hdc);   // 光标复位那条钩子要用它算「窗口中心」
        }

        Settings settings = {};
        if (!ReadSettings(settings))
        {
            return;
        }

        __try
        {
            ServiceJobs(settings);        // 鼠标解锁 / 抓图
            InterlockedExchange(&g_world_matrix_captured, 0);   // 下一帧的 3D 世界矩阵重新抓
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SetStatus(L"读 GL 状态的时候出了个异常，这一帧跳过（其他功能不受影响）");
        }

        if (settings.enabled == 0)
        {
            return;
        }
        if (settings.esp_enabled == 0 && settings.debug_marker == 0 && settings.export_boxes == 0)
        {
            return;
        }

        __try
        {
            DrawFrame(hdc, settings);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SetStatus(L"画方框的时候出了个异常，这一帧跳过了（其他功能不受影响）");
        }

        __try
        {
            ServiceScreenshot(settings);   // 画完再抓图，抓到的画面里带着方框
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SetStatus(L"抓图的时候出了个异常，这一帧跳过了");
        }
    }

    // ============================ 固定伤害 ============================
    // CT 表脚本：把 [esp+54] 先写成 200.0（0x43480000），再执行原来的 fsub / fstp。
    // 我们自己拼这段机器码放进 mp.dll 附近的代码洞穴，入口那 5 个字节改成 jmp。
    const unsigned char kFixedDamageOriginal[10] = { 0xD8, 0x64, 0x24, 0x54, 0xD9, 0x98, 0x60, 0x01, 0x00, 0x00 };

    void* g_fixed_damage_cave = nullptr;
    void* g_fixed_damage_target = nullptr;
    bool g_fixed_damage_installed = false;

    void UpdateFixedDamageValue(float value)
    {
        if (g_fixed_damage_cave == nullptr)
        {
            return;
        }
        // 洞穴布局：C7 44 24 54 <4 字节 float> ... 立即数从第 4 个字节开始
        DWORD old_protect = 0;
        unsigned char* immediate = static_cast<unsigned char*>(g_fixed_damage_cave) + 4;
        if (VirtualProtect(immediate, sizeof(float), PAGE_EXECUTE_READWRITE, &old_protect) != FALSE)
        {
            memcpy(immediate, &value, sizeof(float));
            DWORD ignored = 0;
            VirtualProtect(immediate, sizeof(float), old_protect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), immediate, sizeof(float));
        }
    }

    bool InstallFixedDamage(const Settings& s, const wchar_t*& error)
    {
        if (s.fixed_damage_module[0] == L'\0')
        {
            error = L"固定伤害：补丁模块名是空的";
            return false;
        }
        HMODULE module = GetModuleHandleW(s.fixed_damage_module);
        if (module == nullptr)
        {
            error = L"固定伤害：游戏里还没加载这个模块，等它加载出来再试";
            return false;
        }

        unsigned char* target = reinterpret_cast<unsigned char*>(module) + static_cast<uintptr_t>(s.fixed_damage_offset);
        if (target[0] == 0xE9)
        {
            return true;   // 已经是我们的 jmp（比如上一次没卸干净），当作装好了
        }
        if (memcmp(target, kFixedDamageOriginal, sizeof(kFixedDamageOriginal)) != 0)
        {
            error = L"固定伤害：目标地址的原始字节对不上（客户端版本不一样？），没有打这个补丁";
            return false;
        }

        unsigned char* cave = static_cast<unsigned char*>(AllocateNear(target, 64));
        if (cave == nullptr)
        {
            error = L"固定伤害：附近找不到能放代码洞穴的内存";
            return false;
        }

        // 洞穴里的代码（就是 CT 表那个脚本）：
        //   mov dword ptr [esp+54], <float>
        //   fsub dword ptr [esp+54]
        //   fstp dword ptr [eax+00000160]
        //   jmp 回原处 + 10
        size_t offset = 0;
        cave[offset++] = 0xC7;
        cave[offset++] = 0x44;
        cave[offset++] = 0x24;
        cave[offset++] = 0x54;
        float value = s.fixed_damage_value;
        memcpy(cave + offset, &value, sizeof(value));
        offset += sizeof(value);
        memcpy(cave + offset, kFixedDamageOriginal, sizeof(kFixedDamageOriginal));
        offset += sizeof(kFixedDamageOriginal);
        cave[offset] = 0xE9;
        *reinterpret_cast<int32_t*>(cave + offset + 1) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(target) + sizeof(kFixedDamageOriginal) - (reinterpret_cast<uintptr_t>(cave) + offset + 5));
        offset += 5;
        FlushInstructionCache(GetCurrentProcess(), cave, offset);

        // 入口：E9 + 补 nop（CT 表里写的就是 jmp newmem / nop 5）
        unsigned char patch[10] = {};
        patch[0] = 0xE9;
        *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(cave) - (reinterpret_cast<uintptr_t>(target) + 5));
        for (size_t index = 5; index < sizeof(patch); index++)
        {
            patch[index] = 0x90;
        }

        if (!WriteCode(target, patch, sizeof(patch)))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            error = L"固定伤害：写游戏代码失败（被反作弊/杀软拦了？）";
            return false;
        }

        g_fixed_damage_cave = cave;
        g_fixed_damage_target = target;
        g_fixed_damage_installed = true;
        return true;
    }

    void RemoveFixedDamage()
    {
        if (!g_fixed_damage_installed)
        {
            return;
        }
        if (g_fixed_damage_target != nullptr)
        {
            WriteCode(g_fixed_damage_target, kFixedDamageOriginal, sizeof(kFixedDamageOriginal));
        }
        g_fixed_damage_installed = false;
        Sleep(50);   // 让正在洞穴里跑的线程先出来
        if (g_fixed_damage_cave != nullptr)
        {
            VirtualFree(g_fixed_damage_cave, 0, MEM_RELEASE);
            g_fixed_damage_cave = nullptr;
        }
        g_fixed_damage_target = nullptr;
    }


    // ============================ hook 安装 / 卸载 ============================
    HookEntry g_hooks[] =
    {
        { L"glBegin",        "glBegin",        nullptr, reinterpret_cast<void*>(&GlBeginStub),       5, &g_trampoline_gl_begin,        nullptr, {}, false },
        { L"glDrawArrays",   "glDrawArrays",   nullptr, reinterpret_cast<void*>(&GlDrawArraysStub),  5, &g_trampoline_gl_draw_arrays,  nullptr, {}, false },
        { L"glDrawElements", "glDrawElements", nullptr, reinterpret_cast<void*>(&GlDrawElementsStub), 5, &g_trampoline_gl_draw_elements, nullptr, {}, false },
        { L"wglSwapBuffers", "wglSwapBuffers", nullptr, reinterpret_cast<void*>(&SwapBuffersStub),    5, &g_trampoline_swap_buffers,    nullptr, {}, false },
    };

    // 游戏一启动就加载了 opengl32（OpenGL 模式），实在没有就自己 LoadLibrary 一份
    HMODULE AcquireOpengl32()
    {
        HMODULE module = GetModuleHandleW(L"opengl32.dll");
        if (module == nullptr)
        {
            module = LoadLibraryW(L"opengl32.dll");
        }
        return module;
    }

    int InstallAllHooks()
    {
        HMODULE opengl = AcquireOpengl32();
        if (opengl == nullptr)
        {
            SetStatus(L"游戏进程里没有 opengl32.dll，透视和游戏内画框用不了（固定伤害不受影响）");
            return 0;
        }

        g_gl_disable = reinterpret_cast<GlDisableProc>(GetProcAddress(opengl, "glDisable"));
        g_gl_read_pixels = reinterpret_cast<GlReadPixelsProc>(GetProcAddress(opengl, "glReadPixels"));
        g_gl_get_integerv = reinterpret_cast<GlGetIntegervProc>(GetProcAddress(opengl, "glGetIntegerv"));
        g_gl_get_floatv = reinterpret_cast<GlGetFloatvProc>(GetProcAddress(opengl, "glGetFloatv"));

        int installed = 0;
        int missing = 0;

        // 顺手把 user32 的 ClipCursor 也钩上（解锁鼠标用，见下面 ClipCursorStub）
        if (g_clip_cursor_hook.target == nullptr)
        {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            g_clip_cursor_hook.target = user32 != nullptr ? GetProcAddress(user32, "ClipCursor") : nullptr;
        }
        if (InstallHook(g_clip_cursor_hook))
        {
            installed++;
            if (g_shared != nullptr)
            {
                InterlockedExchange(&g_shared->clip_hook_installed, 1);
            }
        }
        if (g_set_cursor_pos_hook.target == nullptr)
        {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            g_set_cursor_pos_hook.target = user32 != nullptr ? GetProcAddress(user32, "SetCursorPos") : nullptr;
        }
        if (InstallHook(g_set_cursor_pos_hook))
        {
            installed++;
        }
        for (HookEntry& hook : g_hooks)
        {
            if (hook.target == nullptr)
            {
                hook.target = GetProcAddress(opengl, hook.export_name);
            }
            if (InstallHook(hook))
            {
                installed++;
            }
            else if (hook.target == nullptr)
            {
                missing++;
            }
        }

        if (installed > 0)
        {
            wchar_t text[CSTRAINER_HOOK_TEXT_LENGTH] = {};
            swprintf_s(text, L"注入成功，装上了 %d 个渲染钩子（glBegin / glDrawArrays / glDrawElements / wglSwapBuffers）", installed);
            SetStatus(text);
        }
        else if (missing > 0)
        {
            SetStatus(L"opengl32 里找不到导出函数，钩子装不上（这个客户端可能不是 OpenGL 渲染）");
        }
        else
        {
            SetStatus(L"找到了 opengl32，但函数序言对不上，不敢钩（装个别的 opengl32 版本？）");
        }
        return installed;
    }

    void RemoveAllHooks()
    {
        for (HookEntry& hook : g_hooks)
        {
            RemoveHook(hook);
        }
        RemoveHook(g_clip_cursor_hook);
        g_clip_cursor_hook.target = nullptr;
        RemoveHook(g_set_cursor_pos_hook);
        g_set_cursor_pos_hook.target = nullptr;
        g_gl_disable = nullptr;
        if (g_shared != nullptr)
        {
            InterlockedExchange(&g_shared->clip_hook_installed, 0);
            InterlockedExchange(&g_shared->mouse_released, 0);
        }
    }


    // ============================ worker 线程 ============================
    bool OpenSharedMemory()
    {
        wchar_t name[64] = {};
        MakeHookMappingName(GetCurrentProcessId(), name, _countof(name));

        for (int attempt = 0; attempt < 50 && g_shared == nullptr; attempt++)
        {
            g_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
            if (g_mapping == nullptr)
            {
                g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HookShared), name);
            }
            if (g_mapping != nullptr)
            {
                g_shared = static_cast<HookShared*>(MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(HookShared)));
                if (g_shared != nullptr && g_shared->magic != CSTRAINER_HOOK_MAGIC)
                {
                    UnmapViewOfFile(g_shared);   // 训练器还没初始化好，再等一会儿
                    g_shared = nullptr;
                }
            }
            if (g_shared == nullptr)
            {
                Sleep(200);
            }
        }
        return g_shared != nullptr;
    }

    DWORD WINAPI WorkerThread(void*)
    {
        if (!OpenSharedMemory())
        {
            return 0;   // 共享内存等不到，什么都不用做
        }

        SetStatus(L"已经注入，正在装渲染钩子...");
        InterlockedExchange(&g_shared->dll_loaded, 1);

        int hook_count = InstallAllHooks();
        InterlockedExchange(&g_shared->hook_installed, hook_count);

        LONG seen_sequence = -1;
        bool damage_applied = false;
        float applied_damage_value = -12345.0f;
        LONG applied_offset = -1;
        wchar_t applied_module[CSTRAINER_HOOK_MODULE_LENGTH] = L"";
        DWORD last_damage_try = 0;
        DWORD last_hook_retry = GetTickCount();
        DWORD last_status_tick = GetTickCount();
        LocatedPlayer located = {};
        bool have_located = false;
        DWORD last_locate = 0;
        DWORD last_player_refresh = 0;
        DWORD last_player_scan = 0;

        while (InterlockedCompareExchange(&g_stop_worker, 0, 0) == 0)
        {
            LONG sequence = g_shared->settings_seq;
            if (sequence != seen_sequence)
            {
                PublishSettings(*g_shared);
                seen_sequence = sequence;
            }

            Settings settings = {};
            bool have_settings = ReadSettings(settings);
            DWORD now = GetTickCount();

            // ---------------- 固定伤害 ----------------
            if (have_settings)
            {
                bool want_damage = settings.enabled != 0 && settings.fixed_damage_enabled != 0;
                bool config_changed = settings.fixed_damage_offset != applied_offset
                    || wcscmp(settings.fixed_damage_module, applied_module) != 0;

                if (want_damage)
                {
                    if ((!damage_applied || config_changed) && now - last_damage_try > 1000)
                    {
                        last_damage_try = now;
                        const wchar_t* error = nullptr;
                        if (InstallFixedDamage(settings, error))
                        {
                            damage_applied = true;
                            applied_offset = settings.fixed_damage_offset;
                            wcsncpy_s(applied_module, settings.fixed_damage_module, _TRUNCATE);
                            wchar_t text[CSTRAINER_HOOK_TEXT_LENGTH] = {};
                            swprintf_s(text, L"固定伤害已经生效：%s+%X，打谁都是 %.0f",
                                settings.fixed_damage_module, static_cast<unsigned int>(settings.fixed_damage_offset), settings.fixed_damage_value);
                            SetStatus(text);
                        }
                        else if (error != nullptr)
                        {
                            SetStatus(error);
                        }
                        InterlockedExchange(&g_shared->fixed_damage_on, damage_applied ? 1 : 0);
                    }
                    if (damage_applied && settings.fixed_damage_value != applied_damage_value)
                    {
                        UpdateFixedDamageValue(settings.fixed_damage_value);
                        applied_damage_value = settings.fixed_damage_value;
                    }
                }
                else if (damage_applied)
                {
                    RemoveFixedDamage();
                    damage_applied = false;
                    applied_damage_value = -12345.0f;
                    InterlockedExchange(&g_shared->fixed_damage_on, 0);
                    SetStatus(L"固定伤害已经关掉，mp.dll 的原始字节改回去了");
                }
            }

            // ---------------- 自己找人物基址（CT 表的静态地址对不上这个客户端） ----------------
            if (have_settings && settings.enabled != 0 && settings.autolocate != 0)
            {
                if (have_located)
                {
                    uint32_t current = 0;
                    if (!ReadU32(reinterpret_cast<const void*>(located.static_address), current) || current == 0)
                    {
                        have_located = false;    // 人物没了（换图 / 退出），重新扫
                        InterlockedExchange(&g_shared->player_found, 0);
                    }
                }
                if (!have_located && now - last_locate > 2000)
                {
                    last_locate = now;
                    InterlockedIncrement(&g_shared->player_scan_count);
                    if (LocatePlayerChain(located))
                    {
                        have_located = true;
                        ReportLocatedPlayer(located);
                        wchar_t text[CSTRAINER_HOOK_TEXT_LENGTH] = {};
                        swprintf_s(text, L"自己扫到了人物基址：cstrike.exe+%X（血量 %.0f / 金钱 %d），界面上可以直接用",
                            static_cast<unsigned int>(located.static_address - located.module_base), located.hp, located.money);
                        SetStatus(text);
                    }
                }
                else if (have_located && now - last_locate > 1000)
                {
                    last_locate = now;
                    RefreshLocatedPlayer(located);
                }
            }
            else if (have_settings && settings.autolocate == 0 && have_located)
            {
                have_located = false;
                InterlockedExchange(&g_shared->player_found, 0);
            }

            // ---------------- 自己扫出游戏里所有角色（游戏内方框不用再填实体列表链） ----------------
            if (have_settings && settings.enabled != 0 && settings.esp_enabled != 0)
            {
                if (now - last_player_refresh > 1000)
                {
                    last_player_refresh = now;
                    RefreshPlayerList(have_located ? located.p1 : 0);
                }
                if (now - last_player_scan > 3000)
                {
                    last_player_scan = now;
                    SweepPlayerMemory(have_located ? located.p1 : 0);
                }
            }
            // ---------------- 钩子万一没装上，隔两秒再试一次 ----------------
            if (hook_count == 0 && now - last_hook_retry > 2000)
            {
                last_hook_retry = now;
                hook_count = InstallAllHooks();
                InterlockedExchange(&g_shared->hook_installed, hook_count);
            }
            else if (hook_count > 0 && g_shared->frames == 0 && now - last_status_tick > 5000)
            {
                last_status_tick = now;
                SetStatus(L"钩子装好了，但一直没等到 wglSwapBuffers：这个客户端可能不是 OpenGL 渲染（固定伤害照常用）");
            }

            if (g_shared->unload_requested != 0)
            {
                break;
            }
            Sleep(50);
        }

        RemoveAllHooks();
        RemoveFixedDamage();
        InterlockedExchange(&g_shared->hook_installed, 0);
        InterlockedExchange(&g_shared->fixed_damage_on, 0);
        SetStatus(L"钩子和补丁都摘干净了，DLL 正在自己卸载");

        bool self_unload = g_shared->unload_requested != 0 && InterlockedCompareExchange(&g_process_exiting, 0, 0) == 0;
        InterlockedExchange(&g_shared->ready_to_unload, 1);
        Sleep(80);

        if (self_unload)
        {
            FreeLibraryAndExitThread(g_module, 0);
        }
        return 0;
    }
}


// ============================ DLL 入口 ============================
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        g_worker = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr);
        if (g_worker == nullptr)
        {
            return FALSE;
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (reserved != nullptr)
        {
            InterlockedExchange(&g_process_exiting, 1);   // 游戏自己在退出，别做多余的事
        }
        InterlockedExchange(&g_stop_worker, 1);
        if (g_worker != nullptr && g_worker != GetCurrentThread())
        {
            WaitForSingleObject(g_worker, 1500);
            CloseHandle(g_worker);
        }
        g_worker = nullptr;
        RemoveAllHooks();
    }
    return TRUE;
}
