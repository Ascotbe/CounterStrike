#include "gui.h"
#include "config.h"
#include "esp.h"
#include "hook_client.h"
#include "role.h"

#include <commctrl.h>
#include <TlHelp32.h>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    // ============================== 控件 ID ==============================
    enum ControlId : int
    {
        kTimerId = 1,
        kFeatureBaseId = 1000,      // 1000 ~ 1011：12 个功能勾选框
        kVersionAutoId = 1100,
        kVersion2728Id = 1101,
        kVersion3248Id = 1102,
        kProbeButtonId = 1103,
        kEspEnableId = 1200,
        kEspCrosshairId = 1201,
        kEspMatrixId = 1202,
        kEspEntitiesId = 1203,
        kEspPointerArrayId = 1204,
        kEspFlipYId = 1205,
        kSaveButtonId = 1206,
        kReloadButtonId = 1207,
        kEspColorEnemyId = 1211,    // 敌人方框颜色（RRGGBB）
        kEspColorTeamId = 1212,     // 队友方框颜色（RRGGBB）
        kLockAllId = 1208,
        kUnlockAllId = 1209,
        kResetDefaultsId = 1210,
        kValueBaseId = 2000,        // 2000 ~ 2011：每行的锁定数值输入框
        kMinusBaseId = 2100,        // 2100 ~ 2111：每行的 - 按钮
        kPlusBaseId = 2200,         // 2200 ~ 2211：每行的 + 按钮
        kNameBaseId = 2300,         // 2300 ~ 2311：每行的功能名
        kStatusId = 1300,
        kRadarId = 1301,
        kLogId = 1302,
        // ---- 注入渲染 Hook（DLL） ----
        kHookInjectId = 1400,
        kHookEjectId = 1401,
        kHookApplyId = 1402,
        kHookEnableId = 1410,
        kHookEspId = 1411,
        kHookHpTextId = 1412,
        kHookWallhackId = 1413,
        kHookAggressiveId = 1414,
        kHookFixedDamageId = 1415,
        kHookDebugMarkerId = 1416,
    kHookReleaseMouseId = 1417,
    kHookAutolocateId = 1418,
    kHookScreenshotId = 1419,
    kHookShotPathId = 1424,
    kHookLocateNowId = 1425,
    kHookLocateInfoId = 1432,
        kHookDamageValueId = 1420,
        kHookDamageMinusId = 1421,
        kHookDamagePlusId = 1422,
        kHookPatchAddrId = 1423,
        kHookStatusId = 1430,
        kHookNoteId = 1431,
    };

    const wchar_t* kMainClass = L"CSTrainerMainWindow";
    const wchar_t* kRadarClass = L"CSTrainerRadarWindow";
    const wchar_t* kWindowCaption = L"CounterStrike 离线练习修改器（按 F1~F12 开关功能）";

    const UINT kTimerIntervalMs = 15;
    const DWORD kStatusIntervalMs = 120;
    const SIZE_T kMaxLogLines = 300;
    const float kRadarScale = 0.02f;   // 1 像素 = 50 个单位

    struct Feature
    {
        int key;                                                              // 热键（F1 ~ F12）
        const wchar_t* name;                                                  // 功能名
        OffsetChain role_offsets::* chain;                                    // 对应的偏移链
        void (*apply)(const role_offsets&, const game_information&, double);   // 锁定函数
        bool is_integer;                                                      // 整数功能还是单浮点功能
        double value;                                                         // 要锁定成多少（界面上可以改）
        double step;                                                          // - / + 每次加减多少
        double min_value;                                                     // 允许范围
        double max_value;
        double default_value;                                                 // 「数值恢复默认」用的
        bool enabled;                                                         // 当前有没有锁定
        bool key_was_down;                                                    // 用来做按键边沿检测
    };

    game_information g_game;
    role_offsets g_offsets;
    std::vector<Feature> g_features;

    HWND g_main = nullptr;
    HWND g_radar = nullptr;
    HWND g_log = nullptr;
    HFONT g_font = nullptr;

    GameVersion g_version = GameVersion::CS_v2728;
    int g_version_mode = 0;              // 0 自动识别  1 强制 v2728  2 强制 v3248
    bool g_attached = false;
    std::wstring g_version_note = L"还没有连接游戏";
    std::wstring g_ini_path;

    DWORD g_last_status_tick = 0;
    DWORD g_last_attach_try = 0;
    DWORD g_last_attach_failure_tick = 0;
    std::wstring g_last_attach_failure;
    int g_last_boxes = 0;
    std::vector<std::wstring> g_log_lines;

    // 人物框框在界面上的状态（会存到 ini）
    bool g_esp_enabled = true;
    bool g_esp_crosshair = true;
    bool g_esp_pointer_array = true;
    bool g_esp_flip_y = false;
    std::wstring g_matrix_text;
    std::wstring g_entities_text;
    COLORREF g_esp_color_enemy = RGB(255, 60, 60);   // 敌人方框颜色（界面能改，存 ini）
    COLORREF g_esp_color_team = RGB(80, 220, 120);   // 队友方框颜色

    // ---- 注入渲染 Hook（把 CounterStrikeHook.dll 注进游戏）----
    bool g_hook_enabled = true;            // 总开关
    bool g_hook_esp = true;                // 游戏内画方框
    bool g_hook_hp_text = true;            // 方框上写血量
    bool g_hook_wallhack = false;          // 透视（关深度测试）
    bool g_hook_aggressive = false;        // 透视激进模式
    bool g_hook_fixed_damage = false;      // 固定伤害
    bool g_hook_debug_marker = false;      // 调试十字
    bool g_hook_release_mouse = true;        // 解锁鼠标：窗口化时游戏会把光标锁在窗口中间，勾上就不锁了
    bool g_hook_autolocate = true;           // 让 DLL 自己在游戏里找人物基址（CT 表那两条静态地址对不上这个客户端）
    std::wstring g_hook_shot_path = L"game_shot.bmp";   // 游戏内抓图存到哪儿（相对路径 = 训练器目录）
    double g_hook_damage_value = 200.0;    // 固定伤害数值（CT 表里是 200）
    std::wstring g_hook_patch_text = L"mp.dll+5ACA1";   // 补丁地址（模块名+偏移）
    bool g_hook_dirty = true;              // 设置改过了，要同步给 DLL
    bool g_hook_injected = false;          // 训练器这边记的注入状态
    bool g_hook_unload_requested = false;  // 点过卸载，等 DLL 回话
    DWORD g_hook_inject_tick = 0;          // 注入的时间
    DWORD g_hook_unload_tick = 0;          // 点卸载的时间
    int g_hook_seen_hooks = 0;             // 播报用：钩子数量（只在变化时说一次）
    int g_hook_seen_fixed_damage = 0;
    int g_hook_seen_wallhack = 0;

    // ============================== 小工具 ==============================
    std::wstring Fmt(const wchar_t* format, ...)
    {
        wchar_t buffer[512];
        va_list arguments;
        va_start(arguments, format);
        _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, arguments);
        va_end(arguments);
        return buffer;
    }

    std::wstring HexValue(uintptr_t value)
    {
        return Fmt(L"0x%08X", static_cast<unsigned int>(value));
    }

    // 把界面上填的 "RRGGBB" 变成 COLORREF；填得不对就不动
    bool ParseColorText(const std::wstring& text, COLORREF& out)
    {
        if (text.size() != 6)
        {
            return false;
        }
        for (wchar_t ch : text)
        {
            if (!iswxdigit(ch))
            {
                return false;
            }
        }
        const unsigned int value = static_cast<unsigned int>(wcstoul(text.c_str(), nullptr, 16));
        out = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
        return true;
    }

    std::wstring ColorText(COLORREF color)
    {
        return Fmt(L"%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    }

    // 「- / +」按钮上直接写清楚每点一次加多少（金钱是 100，血量是 10……）
    std::wstring FormatStep(double step)
    {
        std::wstring text = Fmt(L"%.2f", step);
        if (text.find(L'.') != std::wstring::npos)
        {
            while (!text.empty() && text.back() == L'0')
            {
                text.pop_back();
            }
            if (!text.empty() && text.back() == L'.')
            {
                text.pop_back();
            }
        }
        return text;
    }

    std::wstring FormatFloat(bool ok, float value, int decimals)
    {
        if (!ok)
        {
            return L"--";
        }
        return Fmt(L"%.*f", decimals, value);
    }

    std::wstring FormatInt(bool ok, int value)
    {
        if (!ok)
        {
            return L"--";
        }
        return std::to_wstring(value);
    }

    void Log(const std::wstring& text)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        g_log_lines.push_back(Fmt(L"[%02d:%02d:%02d] ", now.wHour, now.wMinute, now.wSecond) + text);
        while (g_log_lines.size() > kMaxLogLines)
        {
            g_log_lines.erase(g_log_lines.begin());
        }
        if (g_log != nullptr)
        {
            std::wstring all;
            for (const std::wstring& line : g_log_lines)
            {
                all += line;
                all += L"\r\n";
            }
            SetWindowTextW(g_log, all.c_str());
            SendMessageW(g_log, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
            SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
        }
    }

    std::wstring ControlText(int id)
    {
        HWND control = GetDlgItem(g_main, id);
        if (control == nullptr)
        {
            return std::wstring();
        }
        int length = GetWindowTextLengthW(control);
        if (length <= 0)
        {
            return std::wstring();
        }
        std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(control, &buffer[0], length + 1);
        buffer.resize(static_cast<size_t>(length));
        return buffer;
    }

    // 把 "cstrike.exe+11069BC+7C+88" 这种写法解析成 { 0x11069BC, 0x7C, 0x88 }
    // 逗号、短横线、空格都可以当分隔符，模块名（带点的那种）会自动忽略
    bool ParseChain(const std::wstring& text, OffsetChain& chain, std::wstring& error)
    {
        chain.clear();
        error.clear();

        std::wstring token;
        auto flush_token = [&]() -> bool
        {
            if (token.empty())
            {
                return true;
            }
            std::wstring value = token;
            token.clear();
            if (value.find(L'.') != std::wstring::npos)
            {
                return true;   // cstrike.exe 之类的模块名，跳过
            }
            if (value.size() > 2 && value[0] == L'0' && (value[1] == L'x' || value[1] == L'X'))
            {
                value = value.substr(2);
            }
            if (value.empty())
            {
                return true;
            }
            for (wchar_t character : value)
            {
                if (iswxdigit(character) == 0)
                {
                    error = L"「" + value + L"」不是十六进制数字";
                    return false;
                }
            }
            chain.push_back(static_cast<uintptr_t>(wcstoul(value.c_str(), nullptr, 16)));
            return true;
        };

        for (wchar_t character : text)
        {
            if (character == L'+' || character == L',' || character == L'-' || character == L' ' || character == L'\t')
            {
                if (!flush_token())
                {
                    return false;
                }
            }
            else
            {
                token.push_back(character);
            }
        }
        return flush_token();
    }

    std::wstring ReadIniText(const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
    {
        wchar_t buffer[512] = L"";
        GetPrivateProfileStringW(section, key, fallback, buffer, _countof(buffer), g_ini_path.c_str());
        return buffer;
    }

    int ReadIniInt(const wchar_t* section, const wchar_t* key, int fallback)
    {
        return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_ini_path.c_str()));
    }

    // 锁定数值用的小工具，定义在文件后面
    std::wstring FormatValue(double value, bool is_integer);
    bool ParseValueText(const std::wstring& text, double& value);
    double ClampValue(const Feature& feature, double value);
    bool ParsePatchAddress(const std::wstring& text, std::wstring& module, uint32_t& offset);

    void SaveSettings()
    {
        const wchar_t* version = g_version_mode == 0 ? L"auto" : (g_version_mode == 1 ? L"2728" : L"3248");
        WritePrivateProfileStringW(L"general", L"version", version, g_ini_path.c_str());

        for (size_t index = 0; index < g_features.size(); index++)
        {
            std::wstring key = L"F" + std::to_wstring(index + 1);
            WritePrivateProfileStringW(L"features", key.c_str(), g_features[index].enabled ? L"1" : L"0", g_ini_path.c_str());
        }

        WritePrivateProfileStringW(L"esp", L"enabled", g_esp_enabled ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"esp", L"crosshair", g_esp_crosshair ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"esp", L"pointer_array", g_esp_pointer_array ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"esp", L"flip_y", g_esp_flip_y ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"esp", L"color_enemy", ColorText(g_esp_color_enemy).c_str(), g_ini_path.c_str());
        WritePrivateProfileStringW(L"esp", L"color_team", ColorText(g_esp_color_team).c_str(), g_ini_path.c_str());
        for (size_t index = 0; index < g_features.size(); index++)
        {
            std::wstring value_key = L"V" + std::to_wstring(index + 1);
            WritePrivateProfileStringW(L"values", value_key.c_str(), FormatValue(g_features[index].value, false).c_str(), g_ini_path.c_str());
        }

        WritePrivateProfileStringW(L"hook", L"enable", g_hook_enabled ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"esp", g_hook_esp ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"hp_text", g_hook_hp_text ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"wallhack", g_hook_wallhack ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"aggressive", g_hook_aggressive ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"fixed_damage", g_hook_fixed_damage ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"debug_marker", g_hook_debug_marker ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"damage_value", FormatValue(g_hook_damage_value, false).c_str(), g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"patch", ControlText(kHookPatchAddrId).c_str(), g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"release_mouse", g_hook_release_mouse ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"autolocate", g_hook_autolocate ? L"1" : L"0", g_ini_path.c_str());
        WritePrivateProfileStringW(L"hook", L"shot_path", ControlText(kHookShotPathId).c_str(), g_ini_path.c_str());
        WritePrivateProfileStringW(L"esp", L"matrix", ControlText(kEspMatrixId).c_str(), g_ini_path.c_str());
        WritePrivateProfileStringW(L"esp", L"entities", ControlText(kEspEntitiesId).c_str(), g_ini_path.c_str());
    }

    void LoadSettings()
    {
        if (GetFileAttributesW(g_ini_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return;   // 没有 ini 就用代码里的默认值
        }

        std::wstring version = ReadIniText(L"general", L"version", L"auto");
        g_version_mode = version == L"2728" ? 1 : (version == L"3248" ? 2 : 0);

        for (size_t index = 0; index < g_features.size(); index++)
        {
            std::wstring key = L"F" + std::to_wstring(index + 1);
            g_features[index].enabled = ReadIniInt(L"features", key.c_str(), g_features[index].enabled ? 1 : 0) != 0;
        }

        for (size_t index = 0; index < g_features.size(); index++)
        {
            std::wstring value_key = L"V" + std::to_wstring(index + 1);
            std::wstring value_text = ReadIniText(L"values", value_key.c_str(), L"");
            double parsed = 0.0;
            if (ParseValueText(value_text, parsed))
            {
                g_features[index].value = ClampValue(g_features[index], parsed);
            }
        }

        g_esp_enabled = ReadIniInt(L"esp", L"enabled", g_esp_enabled ? 1 : 0) != 0;
        g_esp_crosshair = ReadIniInt(L"esp", L"crosshair", g_esp_crosshair ? 1 : 0) != 0;
        g_esp_pointer_array = ReadIniInt(L"esp", L"pointer_array", g_esp_pointer_array ? 1 : 0) != 0;
        g_esp_flip_y = ReadIniInt(L"esp", L"flip_y", g_esp_flip_y ? 1 : 0) != 0;
        ParseColorText(ReadIniText(L"esp", L"color_enemy", L""), g_esp_color_enemy);
        ParseColorText(ReadIniText(L"esp", L"color_team", L""), g_esp_color_team);
        g_matrix_text = ReadIniText(L"esp", L"matrix", L"");
        g_hook_enabled = ReadIniInt(L"hook", L"enable", g_hook_enabled ? 1 : 0) != 0;
        g_hook_esp = ReadIniInt(L"hook", L"esp", g_hook_esp ? 1 : 0) != 0;
        g_hook_hp_text = ReadIniInt(L"hook", L"hp_text", g_hook_hp_text ? 1 : 0) != 0;
        g_hook_wallhack = ReadIniInt(L"hook", L"wallhack", g_hook_wallhack ? 1 : 0) != 0;
        g_hook_aggressive = ReadIniInt(L"hook", L"aggressive", g_hook_aggressive ? 1 : 0) != 0;
        g_hook_fixed_damage = ReadIniInt(L"hook", L"fixed_damage", g_hook_fixed_damage ? 1 : 0) != 0;
        g_hook_debug_marker = ReadIniInt(L"hook", L"debug_marker", g_hook_debug_marker ? 1 : 0) != 0;
        g_hook_release_mouse = ReadIniInt(L"hook", L"release_mouse", g_hook_release_mouse ? 1 : 0) != 0;
        g_hook_autolocate = ReadIniInt(L"hook", L"autolocate", g_hook_autolocate ? 1 : 0) != 0;
        {
            std::wstring shot = ReadIniText(L"hook", L"shot_path", L"");
            if (!shot.empty())
            {
                g_hook_shot_path = shot;
            }
        }
        {
            double hook_damage = g_hook_damage_value;
            std::wstring hook_damage_text = ReadIniText(L"hook", L"damage_value", L"");
            if (ParseValueText(hook_damage_text, hook_damage) && hook_damage >= 0.0 && hook_damage <= 100000.0)
            {
                g_hook_damage_value = hook_damage;
            }
            std::wstring hook_patch = ReadIniText(L"hook", L"patch", L"");
            std::wstring hook_module;
            uint32_t hook_offset = 0;
            if (ParsePatchAddress(hook_patch, hook_module, hook_offset))
            {
                g_hook_patch_text = hook_patch;
            }
        }
        g_entities_text = ReadIniText(L"esp", L"entities", L"");
    }

    bool ApplyEspFromControls(bool announce)
    {
        std::wstring error;
        OffsetChain matrix;
        OffsetChain entities;
        std::wstring matrix_text = ControlText(kEspMatrixId);
        std::wstring entities_text = ControlText(kEspEntitiesId);
        if (!ParseChain(matrix_text, matrix, error))
        {
            if (announce)
            {
                Log(L"视角矩阵链填错了：" + error);
            }
            return false;
        }
        if (!ParseChain(entities_text, entities, error))
        {
            if (announce)
            {
                Log(L"实体列表链填错了：" + error);
            }
            return false;
        }

        g_matrix_text = matrix_text;
        g_entities_text = entities_text;
        g_esp_config.view_matrix = matrix;
        g_esp_config.entity_list = entities;
        g_esp_config.enabled = g_esp_enabled;
        g_esp_config.use_crosshair_entity = g_esp_crosshair;
        g_esp_config.pointer_array = g_esp_pointer_array;
        g_esp_config.flip_y = g_esp_flip_y;

        if (announce)
        {
            Log(std::wstring(L"人物框框：") + (g_esp_enabled ? L"开" : L"关")
                + L"，视角矩阵" + (matrix.empty() ? L"还没填（填了才画得出方框）" : L"已填写")
                + L"，实体列表" + (entities.empty() ? L"还没填（只画准星那个角色）" : L"已填写"));
        }
        return true;
    }
    // ===================== 注入渲染 Hook（CounterStrikeHook.dll） =====================
    // 游戏内画框 / 透视 / 固定伤害这三样，外部读写内存做不到，得把 DLL 注进游戏进程里做：
    // 界面上的开关写进共享内存（hook_shared.h），游戏里的 DLL 每 50 毫秒看一次，
    // 改一下就是即时生效，不用重开游戏。
    void ToHookChain(const OffsetChain& chain, HookChainView& view)
    {
        view.count = 0;
        for (size_t index = 0; index < chain.size() && index < CSTRAINER_HOOK_MAX_CHAIN; index++)
        {
            view.offsets[view.count] = static_cast<uint32_t>(chain[index]);
            view.count++;
        }
    }

    // 解析 "mp.dll+5ACA1" 这种补丁地址（模块名 + 偏移），失败返回 false
    bool ParsePatchAddress(const std::wstring& text, std::wstring& module, uint32_t& offset)
    {
        std::wstring trimmed = text;
        while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t'))
        {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t'))
        {
            trimmed.pop_back();
        }
        size_t plus = trimmed.find(L'+');
        if (plus == std::wstring::npos)
        {
            return false;
        }
        module = trimmed.substr(0, plus);
        std::wstring offset_text = trimmed.substr(plus + 1);
        if (module.empty() || offset_text.empty())
        {
            return false;
        }
        if (offset_text.size() > 2 && offset_text[0] == L'0' && (offset_text[1] == L'x' || offset_text[1] == L'X'))
        {
            offset_text = offset_text.substr(2);
        }
        for (wchar_t character : offset_text)
        {
            if (iswxdigit(character) == 0)
            {
                return false;
            }
        }
        offset = static_cast<uint32_t>(wcstoul(offset_text.c_str(), nullptr, 16));
        return true;
    }

    // 把界面上的 Hook 设置 + 人物框框那几条链，一起写进共享内存，让 DLL 重新读一遍
    void PushHookSettings(bool announce)
    {
        HookShared* shared = HookClientShared();
        if (shared == nullptr)
        {
            g_hook_dirty = true;
            return;
        }

        ApplyEspFromControls(false);   // 先把界面上那两条链同步到 g_esp_config
        g_esp_config.color_enemy = g_esp_color_enemy;
        g_esp_config.color_team = g_esp_color_team;

        shared->enabled = g_hook_enabled ? 1 : 0;
        shared->esp_enabled = g_hook_esp ? 1 : 0;
        shared->export_boxes = g_esp_enabled ? 1 : 0;   // 外部「人物框框」开着 → DLL 每帧送框数据
        shared->show_hp_text = g_hook_hp_text ? 1 : 0;
        shared->show_crosshair_entity = g_esp_crosshair ? 1 : 0;
        shared->flip_y = g_esp_flip_y ? 1 : 0;
        shared->box_width = g_esp_config.box_width;
        shared->box_height = g_esp_config.box_height;
        shared->max_distance = g_esp_config.max_distance;
        shared->color_enemy = static_cast<LONG>(g_esp_color_enemy);
        shared->color_team = static_cast<LONG>(g_esp_color_team);
        shared->color_unknown = static_cast<LONG>(RGB(255, 220, 60));
        shared->pointer_array = g_esp_config.pointer_array ? 1 : 0;
        shared->stride = g_esp_config.stride;
        shared->max_count = g_esp_config.max_count;
        shared->origin_offset = g_esp_config.origin_offset;
        shared->health_offset = g_esp_config.health_offset;
        shared->team_offset = g_esp_config.team_offset;
        shared->wallhack_enabled = g_hook_wallhack ? 1 : 0;
        shared->wallhack_aggressive = g_hook_aggressive ? 1 : 0;
        shared->fixed_damage_enabled = g_hook_fixed_damage ? 1 : 0;
        shared->fixed_damage_value = static_cast<FLOAT>(g_hook_damage_value);
        shared->debug_marker = g_hook_debug_marker ? 1 : 0;
        shared->release_mouse = g_hook_release_mouse ? 1 : 0;
        shared->autolocate = g_hook_autolocate ? 1 : 0;
        {
            std::wstring shot = ControlText(kHookShotPathId);
            if (shot.empty())
            {
                shot = g_hook_shot_path;
            }
            wcsncpy_s(shared->screenshot_path, shot.c_str(), _TRUNCATE);
        }
        shared->game_version = g_version == GameVersion::CS_v2728 ? 0 : 1;

        std::wstring module = L"mp.dll";
        uint32_t offset = 0x5ACA1;
        std::wstring patch_text = ControlText(kHookPatchAddrId);
        if (ParsePatchAddress(patch_text, module, offset))
        {
            g_hook_patch_text = patch_text;
        }
        else if (!ParsePatchAddress(g_hook_patch_text, module, offset))
        {
            module = L"mp.dll";
            offset = 0x5ACA1;
        }
        wcsncpy_s(shared->fixed_damage_module, module.c_str(), _TRUNCATE);
        shared->fixed_damage_offset = static_cast<LONG>(offset);

        ToHookChain(g_esp_config.entity_list, shared->entity_list);
        ToHookChain(g_esp_config.view_matrix, shared->view_matrix);
        ToHookChain(g_offsets.enemy_x, shared->crosshair_x);
        ToHookChain(g_offsets.enemy_y, shared->crosshair_y);
        ToHookChain(g_offsets.enemy_z, shared->crosshair_z);
        ToHookChain(g_offsets.enemy_hp, shared->crosshair_hp);
        ToHookChain(g_offsets.x, shared->my_x);
        ToHookChain(g_offsets.y, shared->my_y);
        ToHookChain(g_offsets.z, shared->my_z);
        ToHookChain(g_offsets.my_team, shared->my_team);

        InterlockedIncrement(&shared->settings_seq);
        g_hook_dirty = false;

        if (announce)
        {
            Log(Fmt(L"渲染 Hook 设置已同步：总开关%s，游戏内方框%s，透视%s，固定伤害%s",
                g_hook_enabled ? L"开" : L"关",
                g_hook_esp ? L"开" : L"关",
                g_hook_wallhack ? L"开" : L"关",
                g_hook_fixed_damage ? FormatValue(g_hook_damage_value, false).c_str() : L"关"));
        }
    }

    const wchar_t* HookControlName(int id)
    {
        switch (id)
        {
        case kHookEnableId: return L"总开关";
        case kHookEspId: return L"游戏内画方框";
        case kHookHpTextId: return L"方框上写血量";
        case kHookWallhackId: return L"透视（关深度测试）";
        case kHookAggressiveId: return L"透视激进模式";
        case kHookFixedDamageId: return L"固定伤害";
        case kHookDebugMarkerId: return L"调试十字";
        case kHookReleaseMouseId: return L"解锁鼠标";
        case kHookAutolocateId: return L"自动找基址";
        case kHookScreenshotId: return L"抓图";
        case kHookShotPathId: return L"图片保存路径";
        case kHookDamageMinusId:
        case kHookDamagePlusId:
        case kHookDamageValueId: return L"固定伤害数值";
        case kHookPatchAddrId: return L"补丁地址";
        default: return L"设置";
        }
    }

    void OnHookSettingChanged(int id)
    {
        g_hook_dirty = true;
        if (g_hook_injected)
        {
            Log(Fmt(L"渲染 Hook：%s 改了，已经同步给游戏里的 DLL。", HookControlName(id)));
        }
    }

    void InjectHookDll()
    {
        if (!g_attached)
        {
            Log(L"还没连上游戏，等连上了再注入（游戏和本程序都建议用管理员身份运行）。");
            return;
        }
        if (g_hook_injected)
        {
            Log(L"CounterStrikeHook.dll 已经在游戏里了，直接改设置就行。");
            return;
        }

        std::wstring error;
        if (!HookClientOpenMapping(g_game.pid, error))
        {
            Log(L"建共享内存失败：" + error);
            return;
        }
        PushHookSettings(false);   // DLL 一启动就能看到当前设置

        if (!HookClientInject(g_game.pid, error))
        {
            Log(L"注入失败：" + error);
            return;
        }

        g_hook_injected = true;
        g_hook_unload_requested = false;
        g_hook_inject_tick = GetTickCount();
        g_hook_seen_hooks = 0;
        g_hook_seen_fixed_damage = 0;
        g_hook_seen_wallhack = 0;
        Log(Fmt(L"已经把 CounterStrikeHook.dll 注进游戏（PID = %u），等它回话...", g_game.pid));
    }

    void EjectHookDll()
    {
        if (!g_hook_injected)
        {
            Log(L"现在没有注入的 DLL。");
            return;
        }
        HookClientRequestUnload();
        g_hook_unload_requested = true;
        g_hook_unload_tick = GetTickCount();
        Log(L"已经让 DLL 自己摘钩子并卸载，等它回话（最多等 4 秒）...");
    }

    void ApplyVersion(bool announce);   // 定义在下面「客户端版本」那一段

    // 把 DLL 扫到的人物静态地址搬到界面用的偏移链上。
    // 实测：这份客户端里能过指纹的链不止一条（0x102D480 指的是别的角色，0x1100ABC 才是我自己），
    // 所以只认「人物坐标就在摄像机脚下」的那一条，免得 F1 锁金钱锁到别人身上。
    void AdoptLocatedPlayerChain(HookShared* shared)
    {
        static uintptr_t adopted = 0;
        if (shared == nullptr || shared->player_found == 0 || shared->player_static_offset == 0)
        {
            return;
        }
        const bool camera_known = (shared->camera_x != 0.0f || shared->camera_y != 0.0f || shared->camera_z != 0.0f);
        if (camera_known)
        {
            const float dx = shared->player_x - shared->camera_x;
            const float dy = shared->player_y - shared->camera_y;
            const float dz = shared->player_z - shared->camera_z;
            if (dx * dx + dy * dy + dz * dz > 150.0f * 150.0f)
            {
                return;   // 这条链指的不是我自己
            }
        }

        const uintptr_t located = static_cast<uintptr_t>(shared->player_static_offset);
        if (located == adopted)
        {
            return;
        }

        static OffsetChain role_offsets::*const kPlayerChains[] =
        {
            &role_offsets::blood_bar, &role_offsets::armor, &role_offsets::money,
            &role_offsets::main_weapon, &role_offsets::no_recoil, &role_offsets::shopping_everywhere,
            &role_offsets::slowed_by_attack, &role_offsets::my_team,
            &role_offsets::x, &role_offsets::y, &role_offsets::z,
            &role_offsets::move_speed, &role_offsets::attack_interval,
            &role_offsets::install_bomb_everywhere, &role_offsets::perspective_switch,
        };
        size_t changed = 0;
        for (OffsetChain role_offsets::*member : kPlayerChains)
        {
            OffsetChain& chain = g_offsets.*member;
            if (chain.empty() || chain[0] == located)
            {
                continue;
            }
            chain[0] = located;
            changed++;
        }
        adopted = located;
        if (changed > 0)
        {
            ApplyVersion(false);
            Log(Fmt(L"DLL 扫到的人物静态地址是 cstrike.exe+%X，已经把 %u 条偏移链换到它上面（F1~F12 现在都对着我自己了）。",
                static_cast<unsigned int>(located), static_cast<unsigned int>(changed)));
        }
    }

    // 每 0.1 秒刷一次：把 DLL 那边的状态显示出来，顺便播报几个关键节点
    void RefreshHookStatus()
    {
        HookShared* shared = HookClientShared();
        if (shared == nullptr)
        {
            SetDlgItemTextW(g_main, kHookStatusId, L"DLL：还没注入（点「注入 DLL」；游戏和本程序都要管理员权限）");
            SetDlgItemTextW(g_main, kHookNoteId,
                L"游戏内方框画在游戏自己的画面上（截图、录像都带着框）；左边那个「人物框框」是另开透明窗口画的，两个各画各的。");
            return;
        }

        DWORD now = GetTickCount();

        if (g_hook_injected && shared->ready_to_unload != 0)
        {
            Log(L"注入的 DLL 已经把钩子和补丁都摘干净，自己卸载了。");
            HookClientCloseMapping();
            g_hook_injected = false;
            g_hook_unload_requested = false;
            g_hook_dirty = true;
            g_hook_seen_hooks = 0;
            SetDlgItemTextW(g_main, kHookStatusId, L"DLL：已经卸载（游戏里的钩子和补丁都还原了）");
            SetDlgItemTextW(g_main, kHookNoteId, L"再点「注入 DLL」可以重新来一次。");
            return;
        }

        if (g_hook_injected && shared->dll_loaded == 0 && now - g_hook_inject_tick > 5000)
        {
            g_hook_inject_tick = now;   // 别刷屏
            Log(L"注进去了，但 DLL 一直没起来（共享内存没打开？）：点「卸载 DLL」收尾，或者重启游戏再来。");
        }

        if (g_hook_unload_requested && now - g_hook_unload_tick > 4000)
        {
            std::wstring error;
            if (HookClientForceUnload(error))
            {
                Log(L"DLL 没自己卸载，已经用 FreeLibrary 强行卸掉（钩子和补丁也一起没了）。");
            }
            else
            {
                Log(L"DLL 没回应，强行卸载也没成功：" + error);
            }
            HookClientCloseMapping();
            g_hook_injected = false;
            g_hook_unload_requested = false;
            g_hook_dirty = true;
            g_hook_seen_hooks = 0;
            return;
        }

        if (shared->hook_installed > 0 && g_hook_seen_hooks == 0)
        {
            g_hook_seen_hooks = shared->hook_installed;
            Log(Fmt(L"游戏内渲染钩子装上了 %d 个（glBegin / glDrawArrays / glDrawElements / wglSwapBuffers）。", shared->hook_installed));
        }
        if (shared->fixed_damage_on != 0)
        {
            if (g_hook_seen_fixed_damage == 0)
            {
                g_hook_seen_fixed_damage = 1;
                Log(Fmt(L"固定伤害补丁生效了，打谁都是 %s。", FormatValue(g_hook_damage_value, false).c_str()));
            }
        }
        else
        {
            g_hook_seen_fixed_damage = 0;
        }
        if (shared->wallhack_active != 0 && g_hook_wallhack && g_hook_seen_wallhack == 0)
        {
            g_hook_seen_wallhack = 1;
            Log(L"透视生效了：游戏渲染人物的时候深度测试已经被关掉。");
        }

        std::wstring line = Fmt(L"DLL：%s    Hook：%d 个    渲染帧数 %d    上一帧方框 %d 个 / 角色 %d 个    固定伤害：%s    透视：%s",
            shared->dll_loaded != 0 ? L"已注入" : L"注进去了但还没起来",
            static_cast<int>(shared->hook_installed),
            static_cast<int>(shared->frames),
            static_cast<int>(shared->last_boxes),
            static_cast<int>(shared->last_entities),
            shared->fixed_damage_on != 0 ? L"生效中" : L"关",
            shared->wallhack_active != 0 ? L"生效过" : L"没生效");
        SetDlgItemTextW(g_main, kHookStatusId, line.c_str());
        SetDlgItemTextW(g_main, kHookNoteId, shared->status_text);

        // 自动找基址 / 自己扫角色 / 鼠标解锁 / 抓图的状态
        std::wstring locate_line;
        if (shared->player_found != 0)
        {
            locate_line = Fmt(L"人物链：cstrike.exe+%X（p1=%08X p2=%08X p3=%08X）  血 %.0f  钱 %d  甲 %.0f  子弹 %d  坐标 %.0f/%.0f/%.0f",
                static_cast<unsigned int>(shared->player_static_offset),
                static_cast<unsigned int>(shared->player_p1),
                static_cast<unsigned int>(shared->player_p2),
                static_cast<unsigned int>(shared->player_p3),
                shared->player_hp,
                static_cast<int>(shared->player_money),
                shared->player_armor,
                static_cast<int>(shared->player_ammo),
                shared->player_x, shared->player_y, shared->player_z);
        }
        else
        {
            locate_line = Fmt(L"人物链：还没找到（扫了 %d 轮；勾上「自动找基址」，进游戏出生之后 2 秒内就有）",
                static_cast<int>(shared->player_scan_count));
        }

        AdoptLocatedPlayerChain(shared);

        const wchar_t* shot_state = L"还没抓";
        if (shared->screenshot_done > 0)
        {
            shot_state = L"抓好了";
        }
        else if (shared->screenshot_done < 0)
        {
            shot_state = L"抓失败（路径写不进去？）";
        }
        std::wstring other_line = Fmt(L"自己扫到 %d 个角色（游戏内方框就用这份数据，不用填实体列表链）  相机 %.0f/%.0f/%.0f  矩阵 %d 次  解锁鼠标：%s  截图：%s  扫描 %d ms  光标复位：挡了 %d 次 / 想复位 %d 次（最近要 X=%d Y=%d，客户区中心 X=%d Y=%d，窗口中心 X=%d Y=%d）",
            static_cast<int>(shared->esp_players),
            shared->camera_x, shared->camera_y, shared->camera_z,
            static_cast<int>(shared->matrix_found),
            shared->mouse_released != 0 ? L"生效中" : L"关",
            shot_state,
            static_cast<int>(shared->scan_ms),
            static_cast<int>(shared->cursor_pin_blocked),
            static_cast<int>(shared->cursor_pin_calls),
            static_cast<int>(shared->cursor_pin_last_x),
            static_cast<int>(shared->cursor_pin_last_y),
            static_cast<int>(shared->cursor_pin_center_x),
            static_cast<int>(shared->cursor_pin_center_y),
            static_cast<int>(shared->cursor_pin_win_center_x),
            static_cast<int>(shared->cursor_pin_win_center_y));
        SetDlgItemTextW(g_main, kHookLocateInfoId, (locate_line + L"\r\n" + other_line).c_str());
    }
    void SetFeatureEnabled(size_t index, bool enabled, bool announce);   // 定义在下面功能列表那一段
    bool ParsePatchAddress(const std::wstring& text, std::wstring& module, uint32_t& offset);   // 定义在下面 Hook 那一段

    // ========================= 每行的锁定数值 =========================
    std::wstring FormatValue(double value, bool is_integer)
    {
        if (is_integer)
        {
            long long rounded = static_cast<long long>(value < 0 ? value - 0.5 : value + 0.5);
            return std::to_wstring(rounded);
        }
        std::wstring text = Fmt(L"%.4f", value);
        while (!text.empty() && text.back() == L'0')
        {
            text.pop_back();
        }
        if (!text.empty() && text.back() == L'.')
        {
            text.pop_back();
        }
        if (text.empty() || text == L"-")
        {
            text = L"0";
        }
        return text;
    }

    bool ParseValueText(const std::wstring& text, double& value)
    {
        std::wstring trimmed = text;
        while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t'))
        {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t'))
        {
            trimmed.pop_back();
        }
        if (trimmed.empty())
        {
            return false;
        }
        wchar_t* end = nullptr;
        double parsed = wcstod(trimmed.c_str(), &end);
        if (end == nullptr || end == trimmed.c_str())
        {
            return false;
        }
        while (*end == L' ' || *end == L'\t')
        {
            end++;
        }
        if (*end != L'\0')
        {
            return false;   // 后面还有别的东西，当作没写完
        }
        if (parsed != parsed)   // NaN
        {
            return false;
        }
        value = parsed;
        return true;
    }

    double ClampValue(const Feature& feature, double value)
    {
        if (value < feature.min_value)
        {
            value = feature.min_value;
        }
        if (value > feature.max_value)
        {
            value = feature.max_value;
        }
        if (feature.is_integer)
        {
            value = value < 0 ? -std::floor(-value + 0.5) : std::floor(value + 0.5);
        }
        return value;
    }

    void SetFeatureValueText(size_t index)
    {
        if (index >= g_features.size())
        {
            return;
        }
        HWND edit = GetDlgItem(g_main, kValueBaseId + static_cast<int>(index));
        if (edit == nullptr)
        {
            return;
        }
        std::wstring text = FormatValue(g_features[index].value, g_features[index].is_integer);
        if (ControlText(kValueBaseId + static_cast<int>(index)) == text)
        {
            return;
        }
        SetWindowTextW(edit, text.c_str());
    }

    void SetFeatureValue(size_t index, double value, bool announce)
    {
        if (index >= g_features.size())
        {
            return;
        }
        Feature& feature = g_features[index];
        double old_value = feature.value;
        feature.value = ClampValue(feature, value);
        SetFeatureValueText(index);
        if (announce && feature.value != old_value)
        {
            Log(Fmt(L"F%u  %s 锁定数值改成 %s", static_cast<unsigned int>(index + 1), feature.name,
                FormatValue(feature.value, feature.is_integer).c_str()));
        }
    }

    void AdjustFeatureValue(size_t index, double delta)
    {
        if (index >= g_features.size())
        {
            return;
        }
        SetFeatureValue(index, g_features[index].value + delta, true);
    }

    void SetAllLocks(bool locked)
    {
        int changed = 0;
        for (size_t index = 0; index < g_features.size(); index++)
        {
            const OffsetChain& chain = g_offsets.*(g_features[index].chain);
            if (chain.empty())
            {
                continue;   // 当前版本没收录的跳过
            }
            SetFeatureEnabled(index, locked, false);
            changed++;
        }
        Log(locked ? Fmt(L"已锁定全部功能（%d 条）", changed) : L"已取消全部锁定");
    }

    void ResetValuesToDefault()
    {
        for (size_t index = 0; index < g_features.size(); index++)
        {
            g_features[index].value = g_features[index].default_value;
            SetFeatureValueText(index);
        }
        Log(L"锁定数值已恢复默认。");
    }

    // ============================ 附加游戏进程 ============================
    bool LooksLikeGameWindow(HWND hwnd)
    {
        if (hwnd == nullptr || hwnd == g_main || !IsWindowVisible(hwnd))
        {
            return false;
        }
        wchar_t class_name[64] = L"";
        GetClassNameW(hwnd, class_name, _countof(class_name));
        if (_wcsicmp(class_name, L"Valve001") == 0)
        {
            return true;
        }
        wchar_t title[256] = L"";
        GetWindowTextW(hwnd, title, _countof(title));
        if (title[0] == L'\0')
        {
            return false;
        }
        return _wcsnicmp(title, L"Counter-Strike", 14) == 0;
    }

    BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param)
    {
        if (!LooksLikeGameWindow(hwnd))
        {
            return TRUE;
        }
        *reinterpret_cast<HWND*>(param) = hwnd;
        return FALSE;   // 找到就停
    }

    HWND FindGameWindow()
    {
        HWND result = nullptr;
        EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&result));
        return result;
    }

    uintptr_t GetProcessBase(DWORD pid)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }
        MODULEENTRY32W module_entry{};
        module_entry.dwSize = sizeof(module_entry);
        uintptr_t base = 0;
        // 快照里的第一个模块就是主程序 cstrike.exe
        if (Module32FirstW(snapshot, &module_entry))
        {
            base = reinterpret_cast<uintptr_t>(module_entry.modBaseAddr);
        }
        CloseHandle(snapshot);
        return base;
    }

    void LogAttachFailure(const std::wstring& text)
    {
        DWORD now = GetTickCount();
        if (text == g_last_attach_failure && now - g_last_attach_failure_tick < 10000)
        {
            return;   // 同样的错误 10 秒内只说一次，免得刷屏
        }
        g_last_attach_failure = text;
        g_last_attach_failure_tick = now;
        Log(text);
    }

    void DetachGame()
    {
        if (g_game.handle != nullptr)
        {
            CloseHandle(g_game.handle);
        }
        g_game = game_information();
        g_attached = false;
    }

    // 用血量、金钱、子弹三条偏移链验证客户端版本
    bool ProbeVersion(GameVersion version)
    {
        role_offsets offsets;
        IntoConfig(&offsets, version);

        float hp = 0.0f;
        int money = 0;
        int bullets = 0;
        if (!ReadFloatValue(g_game, offsets.blood_bar, hp) || hp < 1.0f || hp > 1000.0f)
        {
            return false;
        }
        if (!ReadIntValue(g_game, offsets.money, money) || money < -100 || money > 100000)
        {
            return false;
        }
        if (!ReadIntValue(g_game, offsets.main_weapon, bullets) || bullets < 0 || bullets > 999)
        {
            return false;
        }
        return true;
    }

    void ApplyVersion(bool announce)
    {
        IntoConfig(&g_offsets, g_version);

        for (size_t index = 0; index < g_features.size(); index++)
        {
            HWND box = GetDlgItem(g_main, kFeatureBaseId + static_cast<int>(index));
            if (box == nullptr)
            {
                continue;
            }
            const int id = static_cast<int>(index);
            const OffsetChain& chain = g_offsets.*(g_features[index].chain);
            const bool available = !chain.empty();

            std::wstring label = g_features[index].name;
            if (!available)
            {
                label += L"（未收录）";
            }
            SetWindowTextW(GetDlgItem(g_main, kNameBaseId + id), label.c_str());
            EnableWindow(box, available ? TRUE : FALSE);
            EnableWindow(GetDlgItem(g_main, kValueBaseId + id), available ? TRUE : FALSE);
            EnableWindow(GetDlgItem(g_main, kMinusBaseId + id), available ? TRUE : FALSE);
            EnableWindow(GetDlgItem(g_main, kPlusBaseId + id), available ? TRUE : FALSE);
            CheckDlgButton(g_main, kFeatureBaseId + id, (g_features[index].enabled && available) ? BST_CHECKED : BST_UNCHECKED);
            SetFeatureValueText(index);
        }

        if (announce)
        {
            Log(std::wstring(L"当前使用客户端版本：") + (g_version == GameVersion::CS_v2728 ? L"v2728" : L"v3248") + L"（" + g_version_note + L"）");
        }
    }

    bool TryAttach()
    {
        HWND hwnd = FindGameWindow();
        if (hwnd == nullptr)
        {
            return false;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0)
        {
            return false;
        }
        if (g_attached && g_game.pid == pid && g_game.handle != nullptr)
        {
            g_game.hwnd = hwnd;
            return true;
        }

        HANDLE handle = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (handle == nullptr)
        {
            LogAttachFailure(L"找到游戏窗口了，但 OpenProcess 失败：请用管理员身份运行本程序。");
            return false;
        }
        uintptr_t base = GetProcessBase(pid);
        if (base == 0)
        {
            CloseHandle(handle);
            LogAttachFailure(L"找不到 cstrike.exe 模块基址，等会儿再试。");
            return false;
        }

        DetachGame();
        g_game.hwnd = hwnd;
        g_game.pid = pid;
        g_game.handle = handle;
        g_game.base_address = base;
        g_attached = true;
        g_last_attach_failure.clear();
        Log(Fmt(L"已连接游戏：PID = %u，模块基址 = %s", pid, HexValue(base).c_str()));
        if (HookClientShared() != nullptr && HookClientPid() != pid)
        {
            if (g_hook_injected)
            {
                Log(L"检测到游戏进程换了：之前注入的 DLL 跟着老进程一起没了，要用的话重新点「注入 DLL」。");
            }
            HookClientCloseMapping();
            g_hook_injected = false;
            g_hook_unload_requested = false;
            g_hook_dirty = true;
            g_hook_seen_hooks = 0;
            g_hook_seen_fixed_damage = 0;
            g_hook_seen_wallhack = 0;
        }

        if (g_version_mode == 1)
        {
            g_version = GameVersion::CS_v2728;
            g_version_note = L"手动选择 v2728";
        }
        else if (g_version_mode == 2)
        {
            g_version = GameVersion::CS_v3248;
            g_version_note = L"手动选择 v3248";
        }
        else
        {
            bool old_ok = ProbeVersion(GameVersion::CS_v2728);
            bool new_ok = ProbeVersion(GameVersion::CS_v3248);
            if (old_ok && !new_ok)
            {
                g_version = GameVersion::CS_v2728;
                g_version_note = L"自动识别 v2728";
            }
            else if (!old_ok && new_ok)
            {
                g_version = GameVersion::CS_v3248;
                g_version_note = L"自动识别 v3248";
            }
            else if (old_ok && new_ok)
            {
                g_version = GameVersion::CS_v2728;
                g_version_note = L"两个版本都读通了，先按 v2728 处理，可以手动改";
            }
            else
            {
                g_version = GameVersion::CS_v2728;
                g_version_note = L"自动识别失败（人物要先进入地图出生），先按 v2728 处理";
            }
        }
        ApplyVersion(true);
        return true;
    }

    void CheckGameAlive()
    {
        if (!g_attached)
        {
            return;
        }
        DWORD exit_code = 0;
        bool dead = !IsWindow(g_game.hwnd)
            || (GetExitCodeProcess(g_game.handle, &exit_code) && exit_code != STILL_ACTIVE);
        if (dead)
        {
            DetachGame();
            if (g_hook_injected || HookClientShared() != nullptr)
            {
                Log(L"游戏进程退出了，注入的 DLL 也跟着一起没了。");
                HookClientCloseMapping();
                g_hook_injected = false;
                g_hook_unload_requested = false;
                g_hook_dirty = true;
                g_hook_seen_hooks = 0;
                g_hook_seen_fixed_damage = 0;
                g_hook_seen_wallhack = 0;
            }
            EspShutdown();
            g_last_boxes = 0;
            g_version_note = L"游戏已退出，等待重新连接";
            Log(L"游戏进程已退出，等待重新连接。");
        }
    }

    // ============================ 功能开关 ============================
    std::vector<Feature> BuildFeatures()
    {
        // 字段顺序：热键 / 功能名 / 偏移链 / 锁定函数 / 整数? / 锁定值 / 步长 / 最小 / 最大 / 默认值 / 默认锁定? / 按键状态
        return {
            { VK_F1,  L"金钱（实际+显示）",   &role_offsets::money,                   &Money,                 true,  16000.0, 100.0, 0.0,   100000.0, 16000.0, true,  false },
            { VK_F2,  L"主武器子弹",          &role_offsets::main_weapon,             &MainWeapon,            true,  30.0,    10.0,  0.0,   2000.0,   30.0,    true,  false },
            { VK_F3,  L"血量",                &role_offsets::blood_bar,               &BloodBar,              false, 200.0,   10.0,  1.0,   10000.0,  200.0,   true,  false },
            { VK_F4,  L"护甲",                &role_offsets::armor,                   &Armor,                 false, 100.0,   10.0,  0.0,   1000.0,   100.0,   true,  false },
            { VK_F5,  L"移动速度",            &role_offsets::move_speed,              &MoveSpeed,             false, 400.0,   50.0,  1.0,   3000.0,   400.0,   false, false },
            { VK_F6,  L"无限购物",            &role_offsets::shopping_everywhere,     &ShoppingEverywhere,    true,  3.0,     1.0,   0.0,   3.0,      3.0,     false, false },
            { VK_F7,  L"随地下包",            &role_offsets::install_bomb_everywhere, &InstallBombEverywhere, true,  3.0,     1.0,   0.0,   3.0,      3.0,     false, false },
            { VK_F8,  L"视角切换",            &role_offsets::perspective_switch,      &PerspectiveSwitch,     true,  3.0,     1.0,   0.0,   3.0,      3.0,     false, false },
            { VK_F9,  L"光速攻击（左键冷却）", &role_offsets::attack_interval,         &AttackInterval,        false, -1.0,    0.5,   -5.0,  50.0,     -1.0,    false, false },
            { VK_F10, L"被攻击不减速",        &role_offsets::slowed_by_attack,        &SlowedByAttack,        false, 1.0,     0.1,   0.0,   10.0,     1.0,     true,  false },
            { VK_F11, L"无后座力/手枪连发",   &role_offsets::no_recoil,               &NoRecoil,              true,  0.0,     1.0,   0.0,   1.0,      0.0,     false, false },
            { VK_F12, L"免疫伤害（是否掉血）", &role_offsets::immun_damage,            &ImmunDamage,           false, 0.0,     1.0,   0.0,   10.0,     0.0,     false, false },
        };
    }

    void SetFeatureEnabled(size_t index, bool enabled, bool announce)
    {
        if (index >= g_features.size())
        {
            return;
        }
        g_features[index].enabled = enabled;
        CheckDlgButton(g_main, kFeatureBaseId + static_cast<int>(index), enabled ? BST_CHECKED : BST_UNCHECKED);
        if (announce)
        {
            Log(Fmt(L"F%u  %s：%s", static_cast<unsigned int>(index + 1), g_features[index].name, enabled ? L"开" : L"关"));
        }
    }

    void ApplyEnabledFeatures()
    {
        for (Feature& feature : g_features)
        {
            if (!feature.enabled)
            {
                continue;
            }
            const OffsetChain& chain = g_offsets.*(feature.chain);
            if (chain.empty())
            {
                continue;
            }
            feature.apply(g_offsets, g_game, feature.value);
        }
    }

    // F1 ~ F12 用 GetAsyncKeyState 轮询，这样游戏在前台的时候也能按
    void PollHotkeys()
    {
        for (size_t index = 0; index < g_features.size(); index++)
        {
            Feature& feature = g_features[index];
            bool down = (GetAsyncKeyState(feature.key) & 0x8000) != 0;
            if (!down)
            {
                feature.key_was_down = false;
                continue;
            }
            if (feature.key_was_down)
            {
                continue;   // 按住不放只算一次
            }
            feature.key_was_down = true;

            const OffsetChain& chain = g_offsets.*(feature.chain);
            if (chain.empty())
            {
                Log(Fmt(L"F%u  %s：当前客户端版本没有收录这个偏移。", static_cast<unsigned int>(index + 1), feature.name));
                continue;
            }
            if (!g_attached)
            {
                Log(Fmt(L"F%u  %s：还没连接游戏，先记在界面上，连上以后自动生效。", static_cast<unsigned int>(index + 1), feature.name));
            }
            SetFeatureEnabled(index, !feature.enabled, g_attached);
        }
    }

    // ============================ 实时状态 ============================
    std::wstring TeamName(int team)
    {
        return team == 1 ? L"恐怖分子" : (team == 2 ? L"反恐精英" : L"未知");
    }

    std::wstring AimTypeName(int type)
    {
        return type == 0 ? L"墙/空气" : (type == 1 ? L"队友" : (type == 2 ? L"敌人" : L"未知"));
    }

    void RefreshStatus()
    {
        std::wstring text;

        // ---- 连接状态 ----
        text += L"状态：" + g_version_note + (g_version == GameVersion::CS_v2728 ? L"（v2728）" : L"（v3248）");
        if (g_attached)
        {
            text += Fmt(L"    PID = %u    基址 = %s", g_game.pid, HexValue(g_game.base_address).c_str());
        }
        else
        {
            text += L"    还没连接到「Counter-Strike」窗口，每秒自动重试";
        }
        text += L"\r\n";

        // ---- 当前角色 ----
        float hp = 0.0f, armor = 0.0f, move_speed = 0.0f, slowdown = 0.0f;
        int money = 0, team = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool has_hp = g_attached && ReadFloatValue(g_game, g_offsets.blood_bar, hp);
        bool has_armor = g_attached && ReadFloatValue(g_game, g_offsets.armor, armor);
        bool has_money = g_attached && ReadIntValue(g_game, g_offsets.money, money);
        bool has_team = g_attached && ReadIntValue(g_game, g_offsets.my_team, team);
        bool has_speed = g_attached && ReadFloatValue(g_game, g_offsets.move_speed, move_speed);
        bool has_slowdown = g_attached && ReadFloatValue(g_game, g_offsets.slowed_by_attack, slowdown);
        bool has_position = g_attached
            && ReadFloatValue(g_game, g_offsets.x, x)
            && ReadFloatValue(g_game, g_offsets.y, y)
            && ReadFloatValue(g_game, g_offsets.z, z);

        text += L"\r\n【当前角色】\r\n";
        text += Fmt(L"血量 %s    护甲 %s    金钱 %s    移动速度 %s\r\n",
            FormatFloat(has_hp, hp, 0).c_str(), FormatFloat(has_armor, armor, 0).c_str(),
            FormatInt(has_money, money).c_str(), FormatFloat(has_speed, move_speed, 0).c_str());
        text += Fmt(L"坐标 %s    阵营 %s(%s)    被攻击减速 %s\r\n",
            has_position ? Fmt(L"(%.0f, %.0f, %.0f)", x, y, z).c_str() : L"--（这个版本没收录）",
            TeamName(team).c_str(), FormatInt(has_team, team).c_str(), FormatFloat(has_slowdown, slowdown, 2).c_str());

        // ---- CT 里的其他数据 ----
        int bots = 0, global_team = 0, weapon_mark = 0, aim_type = 0, aim_index = 0, grenade = 0;
        bool has_bots = g_attached && ReadIntValue(g_game, g_offsets.bots_static, bots);
        bool has_global_team = g_attached && ReadIntValue(g_game, g_offsets.team_static, global_team);
        bool has_weapon_mark = g_attached && ReadIntValue(g_game, g_offsets.weapon_mark, weapon_mark);
        bool has_aim_type = g_attached && ReadIntValue(g_game, g_offsets.aim_target_type, aim_type);
        bool has_aim_index = g_attached && ReadIntValue(g_game, g_offsets.aim_target_index, aim_index);
        bool has_grenade = g_attached && ReadIntValue(g_game, g_offsets.grenade, grenade);

        text += L"\r\n【CT 里的其他数据】\r\n";
        text += Fmt(L"机器人个数 %s    全局阵营 %s    武器标记 %s    手雷 %s\r\n",
            FormatInt(has_bots, bots).c_str(), FormatInt(has_global_team, global_team).c_str(),
            FormatInt(has_weapon_mark, weapon_mark).c_str(),
            has_grenade ? HexValue(static_cast<uintptr_t>(static_cast<unsigned int>(grenade))).c_str() : L"--");
        text += Fmt(L"准星指向 %s(%s)    指向的角色编号 %s\r\n",
            AimTypeName(aim_type).c_str(), FormatInt(has_aim_type, aim_type).c_str(), FormatInt(has_aim_index, aim_index).c_str());

        // ---- CT「所有人物」----
        float enemy_hp = 0.0f;
        float enemy_x = 0.0f, enemy_y = 0.0f, enemy_z = 0.0f;
        bool has_enemy_hp = g_attached && ReadFloatValue(g_game, g_offsets.enemy_hp, enemy_hp);
        bool has_enemy_position = g_attached
            && ReadFloatValue(g_game, g_offsets.enemy_x, enemy_x)
            && ReadFloatValue(g_game, g_offsets.enemy_y, enemy_y)
            && ReadFloatValue(g_game, g_offsets.enemy_z, enemy_z);
        std::wstring enemy_line = Fmt(L"血量 %s", FormatFloat(has_enemy_hp, enemy_hp, 0).c_str());
        if (has_enemy_position)
        {
            enemy_line += Fmt(L"    坐标 (%.0f, %.0f, %.0f)", enemy_x, enemy_y, enemy_z);
            if (has_position)
            {
                float dx = enemy_x - x;
                float dy = enemy_y - y;
                float dz = enemy_z - z;
                enemy_line += Fmt(L"    离我 %.0f 个单位", sqrtf(dx * dx + dy * dy + dz * dz));
            }
        }
        text += L"\r\n【CT「所有人物」（一般是准星看着的那个人）】\r\n";
        text += enemy_line + L"\r\n";

        // ---- 人物框框 ----
        text += L"【人物框框】";
        if (!g_esp_enabled)
        {
            text += L"已在左边关掉。";
        }
        else if (g_esp_config.view_matrix.empty())
        {
            text += L"没数据：注入 DLL（右下角）后不用填链，会自动用 DLL 扫到的人画框；不注入才要自己填「视角矩阵链」";
        }
        else
        {
            text += Fmt(L"这一帧画了 %d 个方框。", g_last_boxes);
        }

        SetDlgItemTextW(g_main, kStatusId, text.c_str());
    }
    // ============================ 人物位置雷达 ============================
    void DrawRadar(HDC dc, const RECT& client)
    {
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old_font = nullptr;
        if (g_font != nullptr)
        {
            old_font = SelectObject(dc, g_font);
        }

        // 背景格子
        HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
        HGDIOBJ old_pen = SelectObject(dc, grid_pen);
        for (int gx = 0; gx < width; gx += 40)
        {
            MoveToEx(dc, gx, 0, nullptr);
            LineTo(dc, gx, height);
        }
        for (int gy = 0; gy < height; gy += 30)
        {
            MoveToEx(dc, 0, gy, nullptr);
            LineTo(dc, width, gy);
        }
        SelectObject(dc, old_pen);
        DeleteObject(grid_pen);

        float my_x = 0.0f, my_y = 0.0f, my_z = 0.0f;
        bool has_me = g_attached
            && ReadFloatValue(g_game, g_offsets.x, my_x)
            && ReadFloatValue(g_game, g_offsets.y, my_y)
            && ReadFloatValue(g_game, g_offsets.z, my_z);
        if (!has_me)
        {
            SetTextColor(dc, RGB(200, 200, 200));
            const wchar_t* hint = L"雷达要先连接游戏，而且当前版本要有坐标偏移（v3248 有）";
            TextOutW(dc, 8, 8, hint, static_cast<int>(wcslen(hint)));
            if (old_font != nullptr)
            {
                SelectObject(dc, old_font);
            }
            return;
        }

        int center_x = width / 2;
        int center_y = height / 2;

        for (const EspEntity& entity : EspLastEntities())
        {
            int screen_x = center_x + static_cast<int>((entity.x - my_x) * kRadarScale);
            int screen_y = center_y - static_cast<int>((entity.y - my_y) * kRadarScale);
            COLORREF color = RGB(255, 220, 60);
            if (entity.teammate_known)
            {
                color = entity.teammate ? RGB(80, 220, 120) : RGB(255, 60, 60);
            }

            HPEN line_pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
            HGDIOBJ old_line_pen = SelectObject(dc, line_pen);
            MoveToEx(dc, center_x, center_y, nullptr);
            LineTo(dc, screen_x, screen_y);
            SelectObject(dc, old_line_pen);
            DeleteObject(line_pen);

            HBRUSH dot_brush = CreateSolidBrush(color);
            HGDIOBJ old_brush = SelectObject(dc, dot_brush);
            Ellipse(dc, screen_x - 4, screen_y - 4, screen_x + 4, screen_y + 4);
            SelectObject(dc, old_brush);
            DeleteObject(dot_brush);

            SetTextColor(dc, color);
            std::wstring label = Fmt(L"HP %.0f", entity.hp);
            TextOutW(dc, screen_x + 6, screen_y - 8, label.c_str(), static_cast<int>(label.size()));
        }

        HBRUSH me_brush = CreateSolidBrush(RGB(80, 220, 120));
        HGDIOBJ old_brush = SelectObject(dc, me_brush);
        Ellipse(dc, center_x - 5, center_y - 5, center_x + 5, center_y + 5);
        SelectObject(dc, old_brush);
        DeleteObject(me_brush);
        SetTextColor(dc, RGB(80, 220, 120));
        const wchar_t* me_text = L"我";
        TextOutW(dc, center_x + 7, center_y + 2, me_text, static_cast<int>(wcslen(me_text)));

        SetTextColor(dc, RGB(170, 170, 170));
        std::wstring legend = Fmt(L"绿=自己/队友  红=敌人  黄=身份未知  1 像素≈%.0f 单位  我的坐标 (%.0f, %.0f)",
            1.0f / kRadarScale, my_x, my_y);
        TextOutW(dc, 6, height - 18, legend.c_str(), static_cast<int>(legend.size()));

        if (old_font != nullptr)
        {
            SelectObject(dc, old_font);
        }
    }

    // ============================ 界面 ============================
    const int kVersionNoteId = 1104;

    HWND MakeControl(const wchar_t* class_name, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id)
    {
        HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, g_main,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (control != nullptr && g_font != nullptr)
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
        return control;
    }

    void UpdateVersionNote()
    {
        static std::wstring last_text;
        std::wstring text = g_attached
            ? std::wstring(L"当前版本：") + (g_version == GameVersion::CS_v2728 ? L"v2728" : L"v3248")
            : std::wstring(L"当前版本：还没连接游戏");
        if (text == last_text)
        {
            return;   // 没变就别反复 SetWindowText，免得闪
        }
        last_text = text;
        SetDlgItemTextW(g_main, kVersionNoteId, text.c_str());
    }

    // ==================== 小窗口也能用：整页上下滚动 ====================
    // 控件都是按 1160x1032 的「画布」摆的；窗口比画布小的时候把控件整体上移一段，
    // 右边出现滚动条，鼠标滚轮也能翻页 —— 这样启动时就不用占满整个屏幕。
    const int kCanvasWidth = 1160;
    const int kCanvasHeight = 1036;

    struct ChildLayout
    {
        HWND hwnd;
        int x;
        int y;
    };

    std::vector<ChildLayout> g_child_layout;
    int g_scroll_y = 0;

    BOOL CALLBACK RecordChildProc(HWND child, LPARAM)
    {
        RECT rect{};
        if (GetWindowRect(child, &rect) != FALSE)
        {
            POINT point = { rect.left, rect.top };
            ScreenToClient(g_main, &point);
            ChildLayout layout = { child, point.x, point.y };
            g_child_layout.push_back(layout);
        }
        return TRUE;
    }

    void RecordChildLayout()
    {
        g_child_layout.clear();
        EnumChildWindows(g_main, RecordChildProc, 0);
    }

    int MaxScroll()
    {
        RECT client{};
        GetClientRect(g_main, &client);
        int max_scroll = kCanvasHeight - (client.bottom - client.top);
        return max_scroll > 0 ? max_scroll : 0;
    }

    void ApplyChildScroll()
    {
        for (size_t index = 0; index < g_child_layout.size(); index++)
        {
            const ChildLayout& layout = g_child_layout[index];
            SetWindowPos(layout.hwnd, nullptr, layout.x, layout.y - g_scroll_y, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(g_main, nullptr, TRUE);
    }

    void ScrollTo(int y)
    {
        if (y < 0)
        {
            y = 0;
        }
        int max_scroll = MaxScroll();
        if (y > max_scroll)
        {
            y = max_scroll;
        }
        if (y == g_scroll_y)
        {
            return;
        }
        g_scroll_y = y;
        SetScrollPos(g_main, SB_VERT, g_scroll_y, TRUE);
        ApplyChildScroll();
    }

    void UpdateScrollBar()
    {
        RECT client{};
        GetClientRect(g_main, &client);
        int page = client.bottom - client.top;

        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = kCanvasHeight;
        info.nPage = static_cast<UINT>(page > 0 ? page : 0);
        info.nPos = g_scroll_y;
        SetScrollInfo(g_main, SB_VERT, &info, TRUE);

        int max_scroll = MaxScroll();
        if (g_scroll_y > max_scroll)
        {
            g_scroll_y = max_scroll;
            SetScrollPos(g_main, SB_VERT, g_scroll_y, TRUE);
        }
        ApplyChildScroll();
    }

    void OnCreate(HWND hwnd)
    {
        g_main = hwnd;
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");

        // ---------- ① 功能列表：热键 / 功能 / 锁定数值 / - / + / 锁定 ----------
        MakeControl(L"BUTTON", L" 功能（点「锁定」或者按对应 F 键开关，数值可以自己改，- + 是微调）", WS_GROUP | BS_GROUPBOX, 10, 6, 670, 420, -1);
        MakeControl(L"STATIC", L"热键", SS_LEFT, 24, 26, 44, 18, -1);
        MakeControl(L"STATIC", L"功能", SS_LEFT, 72, 26, 246, 18, -1);
        MakeControl(L"STATIC", L"锁定数值", SS_LEFT, 322, 26, 130, 18, -1);
        MakeControl(L"STATIC", L"减 / 加（每次点多少）", SS_LEFT, 404, 26, 132, 18, -1);
        MakeControl(L"STATIC", L"锁定", SS_LEFT, 544, 26, 60, 18, -1);
        for (size_t index = 0; index < g_features.size(); index++)
        {
            const int id = static_cast<int>(index);
            const int row_y = 46 + id * 27;
            MakeControl(L"STATIC", Fmt(L"F%u", static_cast<unsigned int>(index + 1)).c_str(), SS_LEFT, 24, row_y + 2, 44, 18, -1);
            MakeControl(L"STATIC", g_features[index].name, SS_LEFT, 72, row_y + 2, 246, 18, kNameBaseId + id);
            MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_RIGHT, 322, row_y, 130, 22, kValueBaseId + id);
            MakeControl(L"BUTTON", Fmt(L"-%s", FormatStep(g_features[index].step).c_str()).c_str(), BS_PUSHBUTTON, 450, row_y, 44, 22, kMinusBaseId + id);
            MakeControl(L"BUTTON", Fmt(L"+%s", FormatStep(g_features[index].step).c_str()).c_str(), BS_PUSHBUTTON, 496, row_y, 44, 22, kPlusBaseId + id);
            MakeControl(L"BUTTON", L"锁定", BS_AUTOCHECKBOX, 544, row_y + 1, 70, 20, kFeatureBaseId + id);
            SetFeatureValueText(index);
        }
        MakeControl(L"BUTTON", L"全部锁定", BS_PUSHBUTTON, 24, 382, 110, 24, kLockAllId);
        MakeControl(L"BUTTON", L"全部取消锁定", BS_PUSHBUTTON, 148, 382, 130, 24, kUnlockAllId);
        MakeControl(L"BUTTON", L"数值恢复默认", BS_PUSHBUTTON, 292, 382, 130, 24, kResetDefaultsId);

        // ---------- ② 客户端版本 ----------
        MakeControl(L"BUTTON", L" 客户端版本（人物要先进地图出生才识别得出来）", WS_GROUP | BS_GROUPBOX, 10, 434, 670, 70, -1);
        MakeControl(L"BUTTON", L"自动识别", WS_GROUP | BS_AUTORADIOBUTTON, 24, 452, 96, 20, kVersionAutoId);
        MakeControl(L"BUTTON", L"强制 v2728", BS_AUTORADIOBUTTON, 124, 452, 104, 20, kVersion2728Id);
        MakeControl(L"BUTTON", L"强制 v3248", BS_AUTORADIOBUTTON, 232, 452, 104, 20, kVersion3248Id);
        MakeControl(L"BUTTON", L"重新检测", BS_PUSHBUTTON, 344, 450, 80, 24, kProbeButtonId);
        MakeControl(L"STATIC", L"", SS_LEFT, 24, 476, 640, 20, kVersionNoteId);
        CheckDlgButton(hwnd, g_version_mode == 0 ? kVersionAutoId : (g_version_mode == 1 ? kVersion2728Id : kVersion3248Id), BST_CHECKED);

        // ---------- ③ 人物框框 ----------
        MakeControl(L"BUTTON", L" 人物框框（方框透视，画在游戏画面上）", WS_GROUP | BS_GROUPBOX, 10, 510, 670, 238, -1);
        MakeControl(L"BUTTON", L"显示人物方框（队友绿框、敌人红框，画在游戏窗口上）", BS_AUTOCHECKBOX, 24, 530, 640, 20, kEspEnableId);
        MakeControl(L"BUTTON", L"顺便画 CT「所有人物」指针指向的角色（不用扫描列表）", BS_AUTOCHECKBOX, 24, 552, 640, 20, kEspCrosshairId);
        MakeControl(L"STATIC", L"视角矩阵链：", SS_LEFT, 24, 578, 96, 18, -1);
        MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 122, 576, 550, 22, kEspMatrixId);
        MakeControl(L"STATIC", L"实体列表链：", SS_LEFT, 24, 604, 96, 18, -1);
        MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 122, 602, 550, 22, kEspEntitiesId);
        MakeControl(L"BUTTON", L"实体列表里存的是角色指针（CE 里那种 [数组+i*4]）", BS_AUTOCHECKBOX, 24, 628, 400, 20, kEspPointerArrayId);
        MakeControl(L"BUTTON", L"方框上下颠倒（画反了就勾上）", BS_AUTOCHECKBOX, 24, 650, 400, 20, kEspFlipYId);
        MakeControl(L"STATIC", L"链可以留空：注入 DLL（右下角）后，方框直接用 DLL 扫到的人画。\r\n手动填链写法：cstrike.exe+11069BC+7C+88；地图里要有别人（bot_quota 8）才画得出框。",
            SS_LEFT, 24, 674, 400, 40, -1);
        MakeControl(L"STATIC", L"敌人框色", SS_LEFT, 430, 630, 62, 18, -1);
        MakeControl(L"EDIT", ColorText(g_esp_color_enemy).c_str(), WS_BORDER | ES_AUTOHSCROLL | ES_UPPERCASE, 494, 628, 84, 22, kEspColorEnemyId);
        MakeControl(L"STATIC", L"队友框色", SS_LEFT, 430, 654, 62, 18, -1);
        MakeControl(L"EDIT", ColorText(g_esp_color_team).c_str(), WS_BORDER | ES_AUTOHSCROLL | ES_UPPERCASE, 494, 652, 84, 22, kEspColorTeamId);
        MakeControl(L"STATIC", L"填 6 位 RRGGBB，比如 FF3C3C", SS_LEFT, 590, 654, 150, 18, -1);
        MakeControl(L"BUTTON", L"保存设置", BS_PUSHBUTTON, 440, 674, 120, 26, kSaveButtonId);
        MakeControl(L"BUTTON", L"重新连接游戏", BS_PUSHBUTTON, 566, 674, 106, 26, kReloadButtonId);

        // ---------- ⑦ 注入渲染 Hook（把 DLL 注进游戏进程里做游戏内渲染）----------
        MakeControl(L"BUTTON", L" 注入渲染 Hook（把 CounterStrikeHook.dll 注进游戏：游戏内画方框 + 透视 + 固定伤害）", WS_GROUP | BS_GROUPBOX, 10, 754, 1146, 268, -1);
        MakeControl(L"BUTTON", L"启用 Hook（总开关）", BS_AUTOCHECKBOX, 24, 774, 160, 20, kHookEnableId);
        MakeControl(L"BUTTON", L"游戏内画人物方框", BS_AUTOCHECKBOX, 190, 774, 160, 20, kHookEspId);
        MakeControl(L"BUTTON", L"方框上写血量", BS_AUTOCHECKBOX, 356, 774, 130, 20, kHookHpTextId);
        MakeControl(L"BUTTON", L"透视（关深度测试）", BS_AUTOCHECKBOX, 492, 774, 170, 20, kHookWallhackId);
        MakeControl(L"BUTTON", L"透视（激进：所有批次）", BS_AUTOCHECKBOX, 664, 774, 190, 20, kHookAggressiveId);
        MakeControl(L"BUTTON", L"画调试十字", BS_AUTOCHECKBOX, 858, 774, 120, 20, kHookDebugMarkerId);
        MakeControl(L"BUTTON", L"固定伤害", BS_AUTOCHECKBOX, 24, 800, 100, 20, kHookFixedDamageId);
        MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_RIGHT, 128, 799, 78, 22, kHookDamageValueId);
        MakeControl(L"BUTTON", L"-", BS_PUSHBUTTON, 210, 799, 28, 22, kHookDamageMinusId);
        MakeControl(L"BUTTON", L"+", BS_PUSHBUTTON, 242, 799, 28, 22, kHookDamagePlusId);
        MakeControl(L"STATIC", L"补丁地址：", SS_LEFT, 286, 803, 88, 18, -1);
        MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 376, 799, 158, 22, kHookPatchAddrId);
        MakeControl(L"STATIC", L"默认 mp.dll+5ACA1（这个客户端验证过）", SS_LEFT, 540, 803, 300, 18, -1);
        MakeControl(L"BUTTON", L"注入 DLL", BS_PUSHBUTTON, 858, 796, 88, 26, kHookInjectId);
        MakeControl(L"BUTTON", L"卸载 DLL", BS_PUSHBUTTON, 952, 796, 88, 26, kHookEjectId);
        MakeControl(L"BUTTON", L"应用设置", BS_PUSHBUTTON, 1044, 796, 100, 26, kHookApplyId);

        // 鼠标解锁 / 自动找基址 / 游戏内抓图
        MakeControl(L"BUTTON", L"解锁鼠标（窗口化时不再被锁在中间）", BS_AUTOCHECKBOX, 24, 830, 250, 20, kHookReleaseMouseId);
        MakeControl(L"BUTTON", L"自动找基址（人物链不用自己扫）", BS_AUTOCHECKBOX, 286, 830, 240, 20, kHookAutolocateId);
        MakeControl(L"BUTTON", L"立即找一次", BS_PUSHBUTTON, 534, 828, 88, 24, kHookLocateNowId);
        MakeControl(L"BUTTON", L"抓游戏内截图", BS_PUSHBUTTON, 634, 828, 108, 24, kHookScreenshotId);
        MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 750, 829, 210, 22, kHookShotPathId);
        MakeControl(L"STATIC", L"截图存这儿（BMP，带框）", SS_LEFT, 968, 833, 176, 18, -1);
        MakeControl(L"STATIC", L"", SS_LEFT, 24, 858, 1120, 40, kHookLocateInfoId);
        MakeControl(L"STATIC", L"", SS_LEFT, 24, 902, 1120, 18, kHookStatusId);
        MakeControl(L"STATIC", L"", SS_LEFT, 24, 940, 1120, 44, kHookNoteId);

        // ---------- ④ 实时状态 ----------
        MakeControl(L"BUTTON", L" 实时状态（每 0.1 秒刷新）", WS_GROUP | BS_GROUPBOX, 690, 6, 480, 344, -1);
        MakeControl(L"STATIC", L"", SS_LEFT, 702, 24, 456, 322, kStatusId);

        // ---------- ⑤ 人物位置雷达 ----------
        MakeControl(L"BUTTON", L" 人物位置雷达（不用扫描，靠坐标画）", WS_GROUP | BS_GROUPBOX, 690, 356, 480, 168, -1);
        g_radar = MakeControl(kRadarClass, L"", 0, 700, 374, 460, 136, kRadarId);

        // ---------- ⑥ 日志 ----------
        MakeControl(L"BUTTON", L" 日志", WS_GROUP | BS_GROUPBOX, 690, 530, 480, 218, -1);
        g_log = MakeControl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 700, 548, 460, 192, kLogId);

        // 界面状态和 ini 对齐
        CheckDlgButton(hwnd, kEspEnableId, g_esp_enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kEspCrosshairId, g_esp_crosshair ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kEspPointerArrayId, g_esp_pointer_array ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kEspFlipYId, g_esp_flip_y ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookEnableId, g_hook_enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookEspId, g_hook_esp ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookHpTextId, g_hook_hp_text ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookWallhackId, g_hook_wallhack ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookAggressiveId, g_hook_aggressive ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookFixedDamageId, g_hook_fixed_damage ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookDebugMarkerId, g_hook_debug_marker ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookReleaseMouseId, g_hook_release_mouse ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, kHookAutolocateId, g_hook_autolocate ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(hwnd, kHookShotPathId, g_hook_shot_path.c_str());
        SetDlgItemTextW(hwnd, kHookLocateInfoId, L"人物链：还没找（勾上「自动找基址」，进游戏出生之后 2 秒内就能找到）\r\n");
        SetDlgItemTextW(hwnd, kHookDamageValueId, FormatValue(g_hook_damage_value, false).c_str());
        SetDlgItemTextW(hwnd, kHookPatchAddrId, g_hook_patch_text.c_str());
        SetDlgItemTextW(hwnd, kHookStatusId, L"DLL：还没注入（点「注入 DLL」；游戏和本程序都要管理员权限）");
        SetDlgItemTextW(hwnd, kHookNoteId,
            L"游戏内方框画在游戏自己的画面上（截图、录像都带着框）；左边那个「人物框框」是另开透明窗口画的，两个各画各的。");
        if (!g_matrix_text.empty())
        {
            SetDlgItemTextW(hwnd, kEspMatrixId, g_matrix_text.c_str());
        }
        if (!g_entities_text.empty())
        {
            SetDlgItemTextW(hwnd, kEspEntitiesId, g_entities_text.c_str());
        }

        // 启动时如果正好按着某个热键，别让它一上来就切一次
        for (Feature& feature : g_features)
        {
            feature.key_was_down = (GetAsyncKeyState(feature.key) & 0x8000) != 0;
        }

        ApplyVersion(false);
        ApplyEspFromControls(false);
        UpdateVersionNote();

        Log(L"界面已启动，正在等游戏窗口「Counter-Strike」...");
        Log(L"每行都能自己填数值，- + 是微调（金钱 100 一档），也可以直接改完按回车。");
        Log(L"功能按 F1~F12 开关，「全部锁定」一键全开；右边是实时状态、雷达和日志。");
        Log(L"人物框框要有「视角矩阵链」才会画方框，怎么扫见 README。");
        Log(L"下面「注入渲染 Hook」是把 CounterStrikeHook.dll 注进游戏，游戏内画框 + 透视 + 固定伤害都在那里。");
        Log(L"窗口比界面小的时候，右边有滚动条，鼠标滚轮也能上下翻（窗口随便拉大拉小）。");

        RecordChildLayout();
        ScrollTo(0);
        UpdateScrollBar();
    }

    void OnCommand(int id, int notification)
    {
        if (id >= kFeatureBaseId && id < kFeatureBaseId + static_cast<int>(g_features.size()))
        {
            if (notification == BN_CLICKED)
            {
                size_t index = static_cast<size_t>(id - kFeatureBaseId);
                bool enabled = IsDlgButtonChecked(g_main, id) == BST_CHECKED;
                if (enabled && (g_offsets.*(g_features[index].chain)).empty())
                {
                    Log(Fmt(L"F%u  %s：当前客户端版本没有收录这个偏移。", static_cast<unsigned int>(index + 1), g_features[index].name));
                    CheckDlgButton(g_main, id, BST_UNCHECKED);
                    return;
                }
                SetFeatureEnabled(index, enabled, true);
            }
            return;
        }

        if (id >= kValueBaseId && id < kValueBaseId + static_cast<int>(g_features.size()))
        {
            size_t index = static_cast<size_t>(id - kValueBaseId);
            if (notification == EN_CHANGE)
            {
                double parsed = 0.0;
                if (ParseValueText(ControlText(id), parsed))
                {
                    g_features[index].value = ClampValue(g_features[index], parsed);
                }
                // 还没写完/写错了就先不动它，失去焦点的时候会把框里改回有效的数值
            }
            else if (notification == EN_KILLFOCUS)
            {
                SetFeatureValueText(index);
            }
            return;
        }
        if (id >= kMinusBaseId && id < kMinusBaseId + static_cast<int>(g_features.size()))
        {
            if (notification == BN_CLICKED)
            {
                size_t index = static_cast<size_t>(id - kMinusBaseId);
                AdjustFeatureValue(index, -g_features[index].step);
            }
            return;
        }
        if (id >= kPlusBaseId && id < kPlusBaseId + static_cast<int>(g_features.size()))
        {
            if (notification == BN_CLICKED)
            {
                size_t index = static_cast<size_t>(id - kPlusBaseId);
                AdjustFeatureValue(index, g_features[index].step);
            }
            return;
        }
        switch (id)
        {
        case kVersionAutoId:
        case kVersion2728Id:
        case kVersion3248Id:
            if (notification != BN_CLICKED)
            {
                return;
            }
            g_version_mode = IsDlgButtonChecked(g_main, kVersion2728Id) == BST_CHECKED ? 1
                : (IsDlgButtonChecked(g_main, kVersion3248Id) == BST_CHECKED ? 2 : 0);
            Log(g_version_mode == 0 ? L"版本改成：自动识别" : (g_version_mode == 1 ? L"版本改成：强制 v2728" : L"版本改成：强制 v3248"));
            if (g_attached)
            {
                DetachGame();
            if (g_hook_injected || HookClientShared() != nullptr)
            {
                Log(L"游戏进程退出了，注入的 DLL 也跟着一起没了。");
                HookClientCloseMapping();
                g_hook_injected = false;
                g_hook_unload_requested = false;
                g_hook_dirty = true;
                g_hook_seen_hooks = 0;
                g_hook_seen_fixed_damage = 0;
                g_hook_seen_wallhack = 0;
            }
                TryAttach();
            }
            UpdateVersionNote();
            return;

        case kProbeButtonId:
            if (notification == BN_CLICKED)
            {
                if (g_attached)
                {
                    DetachGame();
            if (g_hook_injected || HookClientShared() != nullptr)
            {
                Log(L"游戏进程退出了，注入的 DLL 也跟着一起没了。");
                HookClientCloseMapping();
                g_hook_injected = false;
                g_hook_unload_requested = false;
                g_hook_dirty = true;
                g_hook_seen_hooks = 0;
                g_hook_seen_fixed_damage = 0;
                g_hook_seen_wallhack = 0;
            }
                }
                if (!TryAttach())
                {
                    Log(L"还没找到「Counter-Strike」窗口，请先启动游戏（人物要先进地图出生）。");
                }
                UpdateVersionNote();
            }
            return;

        case kLockAllId:
            if (notification == BN_CLICKED)
            {
                SetAllLocks(true);
            }
            return;
        case kUnlockAllId:
            if (notification == BN_CLICKED)
            {
                SetAllLocks(false);
            }
            return;
        case kResetDefaultsId:
            if (notification == BN_CLICKED)
            {
                ResetValuesToDefault();
            }
            return;
        case kEspEnableId:
            if (notification == BN_CLICKED)
            {
                g_esp_enabled = IsDlgButtonChecked(g_main, kEspEnableId) == BST_CHECKED;
                ApplyEspFromControls(true);
            }
            return;
        case kEspCrosshairId:
            if (notification == BN_CLICKED)
            {
                g_esp_crosshair = IsDlgButtonChecked(g_main, kEspCrosshairId) == BST_CHECKED;
                ApplyEspFromControls(false);
            }
            return;
        case kEspPointerArrayId:
            if (notification == BN_CLICKED)
            {
                g_esp_pointer_array = IsDlgButtonChecked(g_main, kEspPointerArrayId) == BST_CHECKED;
                ApplyEspFromControls(false);
            }
            return;
        case kEspFlipYId:
            if (notification == BN_CLICKED)
            {
                g_esp_flip_y = IsDlgButtonChecked(g_main, kEspFlipYId) == BST_CHECKED;
                ApplyEspFromControls(false);
            }
            return;
        case kEspColorEnemyId:
        case kEspColorTeamId:
            if (notification == EN_CHANGE)
            {
                COLORREF parsed = 0;
                if (ParseColorText(ControlText(id), parsed))
                {
                    if (id == kEspColorEnemyId)
                    {
                        g_esp_color_enemy = parsed;
                    }
                    else
                    {
                        g_esp_color_team = parsed;
                    }
                    PushHookSettings(false);
                    g_hook_dirty = true;
                    Log(Fmt(L"方框颜色改了：%s = %s（DLL 下一次读设置就生效，激活窗口/切一下画面就能看到）",
                        id == kEspColorEnemyId ? L"敌人" : L"队友", ColorText(parsed).c_str()));
                }
            }
            return;
        case kEspMatrixId:
        case kEspEntitiesId:
            if (notification == EN_CHANGE)
            {
                ApplyEspFromControls(false);
            }
            return;
        case kSaveButtonId:
            if (notification == BN_CLICKED)
            {
                if (ApplyEspFromControls(true))
                {
                    SaveSettings();
                    Log(L"设置已保存到 " + g_ini_path);
                }
            }
            return;
        case kHookEnableId:
        case kHookEspId:
        case kHookHpTextId:
        case kHookWallhackId:
        case kHookAggressiveId:
        case kHookFixedDamageId:
        case kHookDebugMarkerId:
            if (notification == BN_CLICKED)
            {
                g_hook_enabled = IsDlgButtonChecked(g_main, kHookEnableId) == BST_CHECKED;
                g_hook_esp = IsDlgButtonChecked(g_main, kHookEspId) == BST_CHECKED;
                g_hook_hp_text = IsDlgButtonChecked(g_main, kHookHpTextId) == BST_CHECKED;
                g_hook_wallhack = IsDlgButtonChecked(g_main, kHookWallhackId) == BST_CHECKED;
                g_hook_aggressive = IsDlgButtonChecked(g_main, kHookAggressiveId) == BST_CHECKED;
                g_hook_fixed_damage = IsDlgButtonChecked(g_main, kHookFixedDamageId) == BST_CHECKED;
                g_hook_debug_marker = IsDlgButtonChecked(g_main, kHookDebugMarkerId) == BST_CHECKED;
                OnHookSettingChanged(id);
            }
            return;
        case kHookDamageValueId:
            if (notification == EN_CHANGE)
            {
                double parsed = 0.0;
                if (ParseValueText(ControlText(kHookDamageValueId), parsed))
                {
                    if (parsed < 0.0)
                    {
                        parsed = 0.0;
                    }
                    if (parsed > 100000.0)
                    {
                        parsed = 100000.0;
                    }
                    g_hook_damage_value = parsed;
                    g_hook_dirty = true;
                }
            }
            else if (notification == EN_KILLFOCUS)
            {
                SetDlgItemTextW(g_main, kHookDamageValueId, FormatValue(g_hook_damage_value, false).c_str());
            }
            return;
        case kHookDamageMinusId:
        case kHookDamagePlusId:
            if (notification == BN_CLICKED)
            {
                double delta = id == kHookDamageMinusId ? -10.0 : 10.0;
                g_hook_damage_value += delta;
                if (g_hook_damage_value < 0.0)
                {
                    g_hook_damage_value = 0.0;
                }
                if (g_hook_damage_value > 100000.0)
                {
                    g_hook_damage_value = 100000.0;
                }
                SetDlgItemTextW(g_main, kHookDamageValueId, FormatValue(g_hook_damage_value, false).c_str());
                OnHookSettingChanged(id);
            }
            return;
        case kHookPatchAddrId:
            if (notification == EN_CHANGE)
            {
                std::wstring module;
                uint32_t offset = 0;
                std::wstring text = ControlText(kHookPatchAddrId);
                if (ParsePatchAddress(text, module, offset))
                {
                    g_hook_patch_text = text;
                    g_hook_dirty = true;
                }
            }
            else if (notification == EN_KILLFOCUS)
            {
                SetDlgItemTextW(g_main, kHookPatchAddrId, g_hook_patch_text.c_str());
            }
            return;
        case kHookReleaseMouseId:
        case kHookAutolocateId:
            if (notification == BN_CLICKED)
            {
                g_hook_release_mouse = IsDlgButtonChecked(g_main, kHookReleaseMouseId) == BST_CHECKED;
                g_hook_autolocate = IsDlgButtonChecked(g_main, kHookAutolocateId) == BST_CHECKED;
                OnHookSettingChanged(id);
            }
            return;
        case kHookLocateNowId:
            if (notification == BN_CLICKED)
            {
                HookShared* shared = HookClientShared();
                if (shared == nullptr)
                {
                    Log(L"还没注入 DLL，先点「注入 DLL」。");
                    return;
                }
                g_hook_autolocate = true;
                g_hook_dirty = true;
                CheckDlgButton(g_main, kHookAutolocateId, BST_CHECKED);
                Log(L"已经让 DLL 重新找一次人物基址（进地图出生之后最容易找到）。");
            }
            return;
        case kHookScreenshotId:
            if (notification == BN_CLICKED)
            {
                HookShared* shared = HookClientShared();
                if (shared == nullptr)
                {
                    Log(L"还没注入 DLL，先点「注入 DLL」。");
                    return;
                }
                std::wstring shot = ControlText(kHookShotPathId);
                if (shot.empty())
                {
                    shot = g_hook_shot_path;
                }
                g_hook_shot_path = shot;
                wcsncpy_s(shared->screenshot_path, shot.c_str(), _TRUNCATE);
                InterlockedExchange(&shared->screenshot_done, 0);
                InterlockedIncrement(&shared->screenshot_request);   // 每点一次 +1，DLL 认这个号
                g_hook_dirty = true;
                Log(L"已经让游戏抓一张图（下一个渲染帧就写好，包含 DLL 画的方框）：" + shot);
            }
            return;
        case kHookShotPathId:
            if (notification == EN_CHANGE)
            {
                std::wstring shot = ControlText(kHookShotPathId);
                if (!shot.empty())
                {
                    g_hook_shot_path = shot;
                    g_hook_dirty = true;
                }
            }
            else if (notification == EN_KILLFOCUS)
            {
                SetDlgItemTextW(g_main, kHookShotPathId, g_hook_shot_path.c_str());
            }
            return;
        case kHookInjectId:
            if (notification == BN_CLICKED)
            {
                InjectHookDll();
            }
            return;
        case kHookEjectId:
            if (notification == BN_CLICKED)
            {
                EjectHookDll();
            }
            return;
        case kHookApplyId:
            if (notification == BN_CLICKED)
            {
                PushHookSettings(true);
                if (!g_hook_injected)
                {
                    Log(L"（现在还没注入 DLL，设置先记着，注入的时候会用上。）");
                }
            }
            return;        case kReloadButtonId:
            if (notification == BN_CLICKED)
            {
                DetachGame();
            if (g_hook_injected || HookClientShared() != nullptr)
            {
                Log(L"游戏进程退出了，注入的 DLL 也跟着一起没了。");
                HookClientCloseMapping();
                g_hook_injected = false;
                g_hook_unload_requested = false;
                g_hook_dirty = true;
                g_hook_seen_hooks = 0;
                g_hook_seen_fixed_damage = 0;
                g_hook_seen_wallhack = 0;
            }
                EspShutdown();
                Log(L"重新连接中...");
                TryAttach();
                UpdateVersionNote();
            }
            return;
        }
    }

    void OnTimer()
    {
        if (!g_attached)
        {
            DWORD now = GetTickCount();
            if (now - g_last_attach_try >= 1000)
            {
                g_last_attach_try = now;
                TryAttach();
            }
        }
        else
        {
            CheckGameAlive();
        }

        PollHotkeys();
        if (g_hook_dirty)
        {
            PushHookSettings(false);   // 设置改过就同步给游戏里的 DLL
        }
        if (g_attached)
        {
            ApplyEnabledFeatures();
            g_last_boxes = EspTick(g_game.hwnd, g_game, g_offsets);
        }

        DWORD now = GetTickCount();
        if (now - g_last_status_tick >= kStatusIntervalMs)
        {
            g_last_status_tick = now;
            RefreshStatus();
            RefreshHookStatus();
            UpdateVersionNote();
            if (g_radar != nullptr)
            {
                InvalidateRect(g_radar, nullptr, FALSE);
            }
        }
    }

    void Shutdown()
    {
        if (g_main != nullptr)
        {
            KillTimer(g_main, kTimerId);
        }
        EspShutdown();
        DetachGame();
    }

    LRESULT CALLBACK RadarProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            DrawRadar(dc, client);
            EndPaint(hwnd, &paint);
            return 0;
        }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;
        case WM_TIMER:
            if (wparam == kTimerId)
            {
                OnTimer();
            }
            return 0;
        case WM_SIZE:
            UpdateScrollBar();
            return 0;
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = 760;
            limits->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_VSCROLL:
        {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_PAGE;
            GetScrollInfo(hwnd, SB_VERT, &info);
            int page = static_cast<int>(info.nPage);
            int position = g_scroll_y;
            switch (LOWORD(wparam))
            {
            case SB_LINEUP: position -= 32; break;
            case SB_LINEDOWN: position += 32; break;
            case SB_PAGEUP: position -= page; break;
            case SB_PAGEDOWN: position += page; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: position = static_cast<short>(HIWORD(wparam)); break;
            case SB_TOP: position = 0; break;
            case SB_BOTTOM: position = MaxScroll(); break;
            default: break;
            }
            ScrollTo(position);
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            // 鼠标停在日志框上就让它自己滚，别抢
            POINT cursor = { static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)) };
            HWND under = WindowFromPoint(cursor);
            if (g_log == nullptr || (under != g_log && IsChild(g_log, under) == FALSE))
            {
                int delta = GET_WHEEL_DELTA_WPARAM(wparam);
                ScrollTo(g_scroll_y + (delta > 0 ? -72 : 72));
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            OnCommand(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        case WM_CLOSE:
            Shutdown();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int RunTrainerGui(HINSTANCE instance, int show_command)
{
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common_controls);

    g_features = BuildFeatures();

    wchar_t module_path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, module_path, _countof(module_path));
    std::wstring path = module_path;
    size_t slash = path.find_last_of(L'\\');
    g_ini_path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"CounterStrike.ini";
    LoadSettings();

    if (g_hook_shot_path.find(L':') == std::wstring::npos)
    {
        // 相对路径（默认 game_shot.bmp）按训练器所在目录算，免得抓的图跑到游戏目录里
        std::wstring folder = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
        g_hook_shot_path = folder + g_hook_shot_path;
    }

    // 手动指定的版本在连接之前也要先应用，这样左边的功能列表才是对的
    if (g_version_mode == 1)
    {
        g_version = GameVersion::CS_v2728;
        g_version_note = L"手动选择 v2728，还没连接游戏";
    }
    else if (g_version_mode == 2)
    {
        g_version = GameVersion::CS_v3248;
        g_version_note = L"手动选择 v3248，还没连接游戏";
    }

    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.style = CS_HREDRAW | CS_VREDRAW;
    main_class.lpfnWndProc = MainProc;
    main_class.hInstance = instance;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
    main_class.lpszClassName = kMainClass;
    if (RegisterClassExW(&main_class) == 0)
    {
        return 1;
    }

    WNDCLASSEXW radar_class{};
    radar_class.cbSize = sizeof(radar_class);
    radar_class.style = CS_HREDRAW | CS_VREDRAW;
    radar_class.lpfnWndProc = RadarProc;
    radar_class.hInstance = instance;
    radar_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    radar_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    radar_class.lpszClassName = kRadarClass;
    if (RegisterClassExW(&radar_class) == 0)
    {
        return 1;
    }

    // 启动时按屏幕工作区算一个「小窗口」大小：屏幕不够高/不够宽就别硬撑，
    // 剩下的内容靠右边的滚动条（WS_VSCROLL）翻，窗口也能随便拉。
    RECT work_area{ 0, 0, 1920, 1080 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int work_width = work_area.right - work_area.left;
    int work_height = work_area.bottom - work_area.top;
    // 右边那条竖向滚动条会占掉客户区宽度，这里先把它加上，不然最右边会差十几个像素被切掉
    int client_width = 1160 + GetSystemMetrics(SM_CXVSCROLL);
    int client_height = 1036;
    if (client_height > work_height * 70 / 100)
    {
        client_height = work_height * 70 / 100;
    }
    if (client_width > work_width - 60)
    {
        client_width = work_width - 60;
    }
    if (client_height < 420)
    {
        client_height = 420;
    }
    if (client_width < 760)
    {
        client_width = 760;
    }
    RECT window_rect{ 0, 0, client_width, client_height };
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW | WS_VSCROLL, FALSE);
    int window_width = window_rect.right - window_rect.left;
    int window_height = window_rect.bottom - window_rect.top;
    int window_x = work_area.left + (work_width - window_width) / 2;
    int window_y = work_area.top + (work_height - window_height) / 2;
    if (window_x < work_area.left)
    {
        window_x = work_area.left;
    }
    if (window_y < work_area.top)
    {
        window_y = work_area.top;
    }
    HWND hwnd = CreateWindowExW(0, kMainClass, kWindowCaption, WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        window_x, window_y, window_width, window_height,
        nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr)
    {
        return 1;
    }

    ShowWindow(hwnd, show_command == 0 ? SW_SHOWNORMAL : show_command);
    UpdateWindow(hwnd);
    SetTimer(hwnd, kTimerId, kTimerIntervalMs, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
