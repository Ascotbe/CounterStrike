#include "esp.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include "role.h"
#include "hook_client.h"

namespace
{
    HWND g_overlay = nullptr;      // 透明覆盖层窗口
    HFONT g_overlay_font = nullptr;
    std::vector<EspEntity> g_entities;

    const COLORREF kColorKey = RGB(0, 0, 0);  // 黑色作为透明色键

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Box
    {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        float hp = 0.0f;
        bool teammate = false;
        bool teammate_known = false;
    };

    LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_ERASEBKGND)
        {
            return 1;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool EnsureOverlay()
    {
        if (g_overlay != nullptr)
        {
            return true;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CSTrainerOverlay";
        RegisterClassExW(&wc);

        g_overlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"CSTrainerOverlay", L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (g_overlay == nullptr)
        {
            return false;
        }

        SetLayeredWindowAttributes(g_overlay, kColorKey, 0, LWA_COLORKEY);
        ShowWindow(g_overlay, SW_SHOWNA);

        g_overlay_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        return true;
    }

    bool ReadFloats(const game_information& game, uintptr_t addr, float* values, size_t count)
    {
        return ReadProcessMemory(game.handle, reinterpret_cast<LPCVOID>(addr), values, count * sizeof(float), nullptr) != FALSE;
    }

    bool ReadIntAt(const game_information& game, uintptr_t addr, int& value)
    {
        return ReadProcessMemory(game.handle, reinterpret_cast<LPCVOID>(addr), &value, sizeof(value), nullptr) != FALSE;
    }

    // 世界坐标 -> 屏幕坐标（row-major 4x4 矩阵，和常见 CS 系引擎的用法一致）
    // 如果画出来上下颠倒，把界面里「方框上下颠倒」勾上（对应 g_esp_config.flip_y）
    bool WorldToScreen(const float matrix[16], const Vec3& world, float width, float height, bool flip_y, float& out_x, float& out_y)
    {
        float w = matrix[12] * world.x + matrix[13] * world.y + matrix[14] * world.z + matrix[15];
        if (w < 0.01f && w > -0.01f)
        {
            return false;
        }
        float clip_x = matrix[0] * world.x + matrix[1] * world.y + matrix[2] * world.z + matrix[3];
        float clip_y = matrix[4] * world.x + matrix[5] * world.y + matrix[6] * world.z + matrix[7];
        float ndc_x = clip_x / w;
        float ndc_y = clip_y / w;
        out_x = (width / 2.0f) * (ndc_x + 1.0f);
        out_y = flip_y ? (height / 2.0f) * (ndc_y + 1.0f) : (height / 2.0f) * (1.0f - ndc_y);
        return true;
    }

    void PushEntity(float x, float y, float z, float hp, bool teammate, bool teammate_known, bool from_crosshair)
    {
        EspEntity entity{};
        entity.x = x;
        entity.y = y;
        entity.z = z;
        entity.hp = hp;
        entity.teammate = teammate;
        entity.teammate_known = teammate_known;
        entity.from_crosshair = from_crosshair;
        g_entities.push_back(entity);
    }

    // 遍历 g_esp_config.entity_list 指到的数组
    void CollectFromList(const game_information& game, const role_offsets& offsets, int my_team, bool has_my_team)
    {
        uintptr_t list_addr = FindDirectMemoryAccessAddy(game, g_esp_config.entity_list);
        if (list_addr == 0)
        {
            return;
        }

        for (int i = 0; i < g_esp_config.max_count; i++)
        {
            uintptr_t entity = 0;
            if (g_esp_config.pointer_array)
            {
                uintptr_t slot = list_addr + static_cast<uintptr_t>(i) * g_esp_config.stride;
                if (!ReadProcessMemory(game.handle, reinterpret_cast<LPCVOID>(slot), &entity, sizeof(entity), nullptr) || entity == 0)
                {
                    continue;
                }
            }
            else
            {
                entity = list_addr + static_cast<uintptr_t>(i) * g_esp_config.stride;
            }

            float hp = 0.0f;
            if (!ReadFloats(game, entity + g_esp_config.health_offset, &hp, 1) || !(hp > 0.0f && hp <= 1000.0f))
            {
                continue;
            }
            float origin[3]{};
            if (!ReadFloats(game, entity + g_esp_config.origin_offset, origin, 3))
            {
                continue;
            }
            if (origin[0] == 0.0f && origin[1] == 0.0f && origin[2] == 0.0f)
            {
                continue;
            }

            int team = 0;
            bool has_team = ReadIntAt(game, entity + g_esp_config.team_offset, team) && (team == 1 || team == 2);
            PushEntity(origin[0], origin[1], origin[2], hp, has_team && has_my_team && team == my_team, has_team && has_my_team, false);
        }
    }

    // CT「所有人物」那条链：不用扫描，直接就是准星看着的角色
    void CollectCrosshairEntity(const game_information& game, const role_offsets& offsets)
    {
        if (offsets.enemy_hp.empty() || offsets.enemy_x.empty())
        {
            return;
        }
        float hp = 0.0f;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!ReadFloatValue(game, offsets.enemy_hp, hp) || !(hp > 0.0f && hp <= 1000.0f))
        {
            return;
        }
        if (!ReadFloatValue(game, offsets.enemy_x, x) || !ReadFloatValue(game, offsets.enemy_y, y) || !ReadFloatValue(game, offsets.enemy_z, z))
        {
            return;
        }
        if (x == 0.0f && y == 0.0f && z == 0.0f)
        {
            return;
        }

        // 准星指向 0 墙 / 1 友 / 2 敌，正好可以判断这个人是队友还是敌人
        int aim_type = 0;
        bool has_aim_type = ReadIntValue(game, offsets.aim_target_type, aim_type) && (aim_type == 1 || aim_type == 2);
        bool teammate = has_aim_type && aim_type == 1;
        PushEntity(x, y, z, hp, teammate, has_aim_type, true);
    }

    void CollectEntities(const game_information& game, const role_offsets& offsets)
    {
        g_entities.clear();
        if (game.handle == nullptr)
        {
            return;
        }

        int my_team = 0;
        bool has_my_team = ReadIntValue(game, offsets.my_team, my_team);

        if (!g_esp_config.entity_list.empty())
        {
            CollectFromList(game, offsets, my_team, has_my_team);
        }
        if (g_esp_config.use_crosshair_entity)
        {
            CollectCrosshairEntity(game, offsets);
        }
    }
}

int EspTick(HWND game_window, const game_information& game, const role_offsets& offsets)
{
    CollectEntities(game, offsets);

    if (!g_esp_config.enabled || game_window == nullptr || game.handle == nullptr)
    {
        return 0;
    }

    // ① 注入的 DLL 已经扫到人、把算好的方框写在共享内存里了？直接用那份，不用填任何链。
    HookShared* shared = HookClientShared();
    bool use_shared_boxes = shared != nullptr && shared->dll_loaded != 0 && shared->enabled != 0
        && (shared->overlay_boxes_seq & 1) == 0 && shared->overlay_boxes_count > 0;

    // ② 没有 DLL 数据就走老路：实体从实体列表链 / CT 准星角色来，屏幕坐标要自己用
    //    「视角矩阵链」算，所以这两样没填就没得画。
    if (!use_shared_boxes && (g_esp_config.view_matrix.empty() || g_entities.empty()))
    {
        return 0;
    }
    if (!EnsureOverlay())
    {
        return 0;
    }

    RECT client{};
    GetClientRect(game_window, &client);
    POINT top_left{ 0, 0 };
    ClientToScreen(game_window, &top_left);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    ShowWindow(g_overlay, SW_SHOWNA);
    SetWindowPos(g_overlay, HWND_TOPMOST, top_left.x, top_left.y, width, height, SWP_NOACTIVATE);

    HDC dc = GetDC(g_overlay);
    RECT full{ 0, 0, width, height };
    FillRect(dc, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    // 游戏不在前台或者最小化的时候不画框，免得覆盖层挡住别的窗口
    if (GetForegroundWindow() != game_window || IsIconic(game_window))
    {
        ReleaseDC(g_overlay, dc);
        return 0;
    }

    std::vector<Box> boxes;
    if (use_shared_boxes)
    {
        int count = shared->overlay_boxes_count;
        if (count > CSTRAINER_HOOK_MAX_BOXES)
        {
            count = CSTRAINER_HOOK_MAX_BOXES;
        }
        for (int index = 0; index < count; index++)
        {
            Box box{};
            box.left = shared->overlay_boxes[index].left;
            box.top = shared->overlay_boxes[index].top;
            box.right = shared->overlay_boxes[index].right;
            box.bottom = shared->overlay_boxes[index].bottom;
            box.hp = shared->overlay_boxes[index].hp;
            box.teammate_known = shared->overlay_boxes[index].teammate_known != 0;
            box.teammate = shared->overlay_boxes[index].teammate != 0;
            boxes.push_back(box);
        }
    }
    else
    {
        float matrix[16]{};
        uintptr_t matrix_addr = FindDirectMemoryAccessAddy(game, g_esp_config.view_matrix);
        if (matrix_addr == 0 || !ReadFloats(game, matrix_addr, matrix, 16))
        {
            ReleaseDC(g_overlay, dc);
            return 0;
        }

        float my_x = 0.0f, my_y = 0.0f, my_z = 0.0f;
        bool has_my_position = ReadFloatValue(game, offsets.x, my_x)
            && ReadFloatValue(game, offsets.y, my_y)
            && ReadFloatValue(game, offsets.z, my_z);

        for (const EspEntity& entity : g_entities)
        {
            if (has_my_position)
            {
                float dx = entity.x - my_x;
                float dy = entity.y - my_y;
                float dz = entity.z - my_z;
                float distance_sq = dx * dx + dy * dy + dz * dz;
                if (distance_sq < 1.0f || distance_sq > g_esp_config.max_distance * g_esp_config.max_distance)
                {
                    continue;
                }
            }

            Vec3 origin{ entity.x, entity.y, entity.z };
            float screen_x = 0.0f, screen_y = 0.0f;
            if (!WorldToScreen(matrix, origin, static_cast<float>(width), static_cast<float>(height), g_esp_config.flip_y, screen_x, screen_y))
            {
                continue;
            }
            if (screen_x < -300.0f || screen_x > width + 300.0f || screen_y < -300.0f || screen_y > height + 300.0f)
            {
                continue;
            }

            Box box{};
            box.left = screen_x - g_esp_config.box_width / 2.0f;
            box.top = screen_y - g_esp_config.box_height;
            box.right = screen_x + g_esp_config.box_width / 2.0f;
            box.bottom = screen_y;
            box.hp = entity.hp;
            box.teammate = entity.teammate;
            box.teammate_known = entity.teammate_known;
            boxes.push_back(box);
        }
    }

    if (!boxes.empty())
    {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        if (g_overlay_font != nullptr)
        {
            SelectObject(dc, g_overlay_font);
        }
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        for (const Box& box : boxes)
        {
            COLORREF color = g_esp_config.color_unknown;           // 阵营没读到：黄色
            if (box.teammate_known)
            {
                color = box.teammate ? g_esp_config.color_team : g_esp_config.color_enemy;
            }
            HPEN pen = CreatePen(PS_SOLID, 2, color);
            HGDIOBJ old_pen = SelectObject(dc, pen);
            Rectangle(dc, static_cast<int>(box.left), static_cast<int>(box.top),
                static_cast<int>(box.right), static_cast<int>(box.bottom));
            SelectObject(dc, old_pen);
            DeleteObject(pen);

            wchar_t text[32];
            swprintf_s(text, L"HP %.0f", box.hp);
            TextOutW(dc, static_cast<int>(box.left), static_cast<int>(box.top) - 18, text, static_cast<int>(wcslen(text)));
        }
        SelectObject(dc, old_brush);
    }

    ReleaseDC(g_overlay, dc);
    return static_cast<int>(boxes.size());
}

const std::vector<EspEntity>& EspLastEntities()
{
    return g_entities;
}

void EspShutdown()
{
    if (g_overlay != nullptr)
    {
        DestroyWindow(g_overlay);
        g_overlay = nullptr;
    }
    if (g_overlay_font != nullptr)
    {
        DeleteObject(g_overlay_font);
        g_overlay_font = nullptr;
    }
}
