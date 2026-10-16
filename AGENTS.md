# AGENTS.md

给在这个仓库里干活的人（包括 AI 助手）的硬性要求。动手前先读一眼 `README.md`。

## 项目定位与边界

离线单机练手用的 CS 1.6（v2728，Esai 汉化整合包）修改器，两条技术路线：

* **外部修改器**：`CounterStrike\`（Win32 GUI），OpenProcess + 读写进程内存，不注入；
* **注入渲染 Hook**：`CounterStrikeHook\hook.cpp` 编出 `CounterStrikeHook.dll`，钩 `wglSwapBuffers` 等。

**边界（必须守）**：只面向本机离线 / 单机环境。不要往里加联网、对战平台、绕过反作弊、
自动瞄准之类的功能；也不要为了「更隐蔽」去改钩子的性质。

## 目录与 git 约定（最重要）

下面四个目录**只能在本地存在，禁止提交到 git**（`.gitignore` 里已经排好，不允许
`git add -f`，也不允许临时删忽略规则来强塞）：

| 目录 | 内容 |
| --- | --- |
| `tmp\` | 截图、抓图、调试图片、dump、一切临时产物 |
| `game\` | 游戏本体（分卷 / 解压出来的 Cs1.6） |
| `ct_tables\` | 第三方 CT 表资料 |
| `tools\` | 本地调试脚本（.ps1 / .cs）和文本 dump |

规则：

* 新生成的图片（png / bmp 等）一律放 `tmp\`，不要放在仓库根目录或源码目录
* 只提交源码和文档：`*.cpp` / `*.h` / `*.sln` / `*.vcxproj*` / `*.md` / `.gitignore`
* 提交前 `git status` 看一眼，不要 `git add .` 顺手把上面四类东西加进来
* 编译产物（`*.exe` / `*.dll` / `*.pdb` / `*.obj` / `*.tlog` …）同样不进 git；
  `.gitignore` 已覆盖，不要 `git add -f`

## 编译 / 运行

游戏是 32 位进程，**必须编 Win32（x86）**（x64 会在编译期 static_assert）：

```powershell
# MSBuild 用本机 VS / Build Tools 自带的（Developer PowerShell 里直接敲 msbuild；
# 找不到就去 VS 安装目录 MSBuild\Current\Bin\ 下找）
msbuild CounterStrike\CounterStrike.vcxproj /nologo /p:Configuration=Release /p:Platform=Win32
```

* 产物都在 `CounterStrike\Release\`：`CounterStrike.exe` + `CounterStrikeHook.dll`（同目录，别拆开）
* 运行：在仓库根目录下、以管理员身份执行 `.\CounterStrike\Release\CounterStrike.exe`
  （游戏用 `-window` 参数窗口化跑，细节见 README）
* 改 `CounterStrikeHook\hook.cpp` 前，先把 DLL 从游戏里卸载（界面「卸载 DLL」按钮），
  否则 dll 文件被占用，链接会失败

## 改代码时的要求

* **偏移必须实测**：不要照抄 CT 表地址。CT 表地址在这份客户端上踩过坑
  （`D6E63C`、`102D480` 都不是正确的人物基址，正确值见 README「偏移表」），
  改偏移要在游戏里（或用 `HookTestHost` 假游戏）实测确认，并把结论写回 README
* **文档 / 注释 / 界面文案都用中文**，语气、风格跟现有内容保持一致
* **最小改动**：只动跟任务相关的文件；不顺手重构、不动无关偏移、不乱改格式
* **文档里不写本机绝对路径**：仓库内路径一律写相对路径（如 `game\extracted\Cs1.6`），
  工具路径（VS / MSBuild 之类）写成「VS 安装目录下 …」这种说法，不要写带盘符的完整路径
* 功能的默认值 / 范围在 `gui.cpp` 的 `BuildFeatures()` 里，改了行为要同步更新
  README 的「功能一览」和「偏移表」
* 不要动 `.gitignore` 里那四条忽略规则；确需调整先跟人确认

## 验证

1. 编译通过（上面的 MSBuild 命令，Win32 配置）
2. 钩子相关改动先用 `HookTestHost`（假游戏）跑通，再进真游戏实测
3. 实测产物（截图 / 日志）丢 `tmp\`
