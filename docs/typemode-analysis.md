# Typing Mode（Nexus 164851）源码分析：能否在 SimpleIME 输入时自动启用

分析对象：`vinymayan/Type-Mode`（main 分支，commit `cce7737`，对应 Nexus v1.0.3）。
结论基于**公开源码**，不是反汇编。

## 0. 源码完整性

| 文件 | 大小 | 内容 |
|---|---|---|
| `src/plugin.cpp` | 10221 B | 全部核心逻辑：hook、输入分支、菜单监听、自动打字模式 |
| `src/Settings.cpp` | 12888 B | JSON 读写 + SKSE Menu Framework 设置界面 |
| `include/Settings.h` | 4061 B | 设置项、DirectInput 键码表 |
| `src/Events.cpp` / `Hooks.cpp` / `Manager.cpp` / `Utils.cpp` | **0 B** | 空模板，作者未上传 |
| `include/Events.h` / `Hooks.h` / `Manager.h` / `Utils.h` | **0 B** | 空模板 |

`0_Type-Mode` 仓库几乎全空，只有 1749 B 的早期 `plugin.cpp`。
**核心逻辑 100% 在 `src/plugin.cpp` 里，无需逆向。**

## 1. Hook 点

```cpp
REL::RelocationID hookLocation(67315, 68617);                 // RE::BSInputDeviceManager::PollInputDevices
uintptr_t hookAddress = hookLocation.address() + REL::Relocate(0x7B, 0x7B, 0x81);
g_NextHookPtr = trampoline.write_call<5>(hookAddress, hk_BlockerHook);
```

- `67315/68617` = `RE::BSInputDeviceManager::PollInputDevices`（见 `extern/CommonLibSSE-NG/src/RE/B/BSInputDeviceManager.cpp:161`）。
- 被替换的是 `PollInputDevices` 内部偏移 `+0x7B`（VR `+0x81`）处那条对 `BSTEventSource<InputEvent*>::SendEvent` 的 `call`。
- `g_NextHookPtr` = `write_call` 返回的原目标（链上的下一个 hook）。
- `g_VanillaFuncPtr` = `GetProcAddress(aaaa_TypeMode.dll, "GetVanillaFunctionPointer")` 取得的**未被 hook 的原始 SendEvent**。

**两条分支的语义**：

| 分支 | 调用 | 效果 |
|---|---|---|
| 屏蔽 | `VanillaFunc(...)` | 直接执行原始 `SendEvent`，跳过链上其它 mod 挂在 `PollInputDevices`/`SendEvent` 上的 hook |
| 放行 | `NextHookFunc(...)` | 继续走别人的 hook 链 |

两条路都会进入 `SendEvent` 的 sink 分发循环。因此它拦的是**hook 型输入处理**（Modex 之类），对纯 `RE::BSTEventSink<RE::InputEvent*>` 注册的热键 mod 无效。

## 2. 自动打字模式的判定

```cpp
bool IsTextFieldFocused()
{
    auto* ui = RE::UI::GetSingleton();
    for (const auto& menuPtr : ui->menuStack) {
        if (auto menu = menuPtr.get(); menu && menu->uiMovie) {
            RE::GFxValue result;
            if (menu->uiMovie->Invoke("Selection.getFocus", &result, nullptr, 0) && result.IsString()) {
                std::string path = result.GetString();  // 转小写
                if (path.find("textfield") != npos || path.find("textinput") != npos) return true;
            }
        }
    }
    return false;
}
```

与 `ControlMap::AllowTextInput` / `textEntryCount` **无关**。它读的是 Scaleform 的 `Selection.getFocus` 路径。
SimpleIME 让 ImeWnd 获得 Win32 焦点时，游戏侧 Scaleform 焦点不变，所以该判定在 IME 输入期间依然为 `true`。

## 3. 关键分支（`hk_BlockerHook`）

```cpp
if (g_IsBlockerToggledOn) return VanillaFunc(...);            // 手动 F4 / MessageBox 自动 → 屏蔽
if (anyMenuOpen) {
    if (Settings::AutoTypeOnTextField && IsTextFieldFocused())
        return VanillaFunc(...);                              // 屏蔽
    // 白名单：按键 ∈ MenuExceptions[当前菜单] → shouldAllowPassThrough
}
if (shouldAllowPassThrough) return NextHookFunc(...);         // 放行
if (anyMenuOpen) return VanillaFunc(...);                     // 菜单内默认屏蔽
return NextHookFunc(...);                                     // 无菜单：不干预
```

`anyMenuOpen = !g_OpenMenus.empty()`，而 `g_OpenMenus` 只在

```cpp
bool isTracked = (Settings::MenuExceptions.count(menuName) > 0);
```

成立时被填充。`MenuExceptions` 默认是空 `std::map`。

### 两个必须知道的事实

1. **默认配置下自动模式完全不生效**：`MenuExceptions` 空 → `g_OpenMenus` 永远空 → `anyMenuOpen == false` → 只有手按 F4 才能开关。
2. **`AutoTypeOnTextField` 对已登记菜单是冗余的**：命中时返回 `VanillaFunc`（屏蔽），不命中时也因 `anyMenuOpen` 落到同一个 `VanillaFunc`。真正的开关是「菜单是否登记在 `MenuExceptions`」。默认值 `AutoTypeOnTextField = false`，但打开它不会改变已登记菜单的行为。

## 4. 因此「输入时自动启用」的正确做法

不需要 SimpleIME 做任何事，也不需要调用该 mod（它没有对外 API，只有 `aaaa_TypeMode.dll` 导出 `GetVanillaFunctionPointer`）。要做的只是**把会用到 IME 的菜单登记进 `MenuExceptions`**：进入该菜单即自动屏蔽，离开即恢复。

### 推荐操作路径

游戏内 `ESC → SKSE Menu Framework → Typing Mode → Settings`：

1. 勾 `Auto Type Mode on Text Fields`（无害，可留作保险）。
2. 在 `Menu name` 输入菜单名 → `Add menu`。
3. 在右侧 `+` 里把**该菜单必须保留的键**加入白名单（见下）。

设置改动即时生效（`Settings::*` 是全局变量，界面直接改；保存写盘在 `Data/SKSE/Plugins/TypeMode_Settings.json`）。

### 必须放行的键（否则正常操作会失效）

被登记菜单内默认**全屏蔽**，不放行就出不去：

| 场景 | 建议白名单键码 |
|---|---|
| 控制台 `Console` | `` ` `` = 41、ESC = 1 |
| 物品栏 / 法术 / 容器 / 交易 / 制作台 | Tab = 15、ESC = 1 |
| 书籍 `Book Menu` | ESC = 1 |
| 地图 `MapMenu` | Tab = 15（或 M = 50）、ESC = 1 |
| 对话 `ConsoleDialogue Menu` | ESC = 1 |

键码是 DirectInput scan code（鼠标码 `+256`）。常用：`ESC=1`、`Tab=15`、`Enter=28`、`Spacebar=57`、`` ` ``=41、`PgUp=201`、`PgDn=209`、`Up=200`、`Down=208`、左键=256、右键=257。

### JSON 等价写法

`Data/SKSE/Plugins/TypeMode_Settings.json`：

```json
{
  "TypeMode": true,
  "ShowLogs": true,
  "AutoTypeOnMessageBox": false,
  "AutoTypeOnTextField": true,
  "TypeMode_k": 62,
  "TypeMode_m": 0,
  "TypeMode_g": 0,
  "MenuExceptions": [
    { "MenuName": "Console", "UnblockedKeys": [41, 1] },
    { "MenuName": "InventoryMenu", "UnblockedKeys": [15, 1] },
    { "MenuName": "MagicMenu", "UnblockedKeys": [15, 1] },
    { "MenuName": "Crafting Menu", "UnblockedKeys": [15, 1] },
    { "MenuName": "BarterMenu", "UnblockedKeys": [15, 1] },
    { "MenuName": "ContainerMenu", "UnblockedKeys": [15, 1] },
    { "MenuName": "Book Menu", "UnblockedKeys": [1] }
  ]
}
```

注意：手写 JSON 只在 `kDataLoaded` 读一次，改完要重启游戏；游戏内 UI 改则即时生效。

### 拿菜单名的方法

`ShowLogs = true` 后，进任意菜单看 `TypeMode.log`（`My Games/Skyrim Special Edition/SKSE/`）里的
`Event of Menu: <名字> is Opened`。文章 *How setup Advanced Mode* 讲的也是这个方法。

## 5. 局限

- **非 Scaleform 界面登记不了**：Meridian（CEF）、Prisma（Ultralight，如 PMCM/Outfit Wheeler）、SKSEMF（ImGui）不在 `UI::menuStack` 里，`MenuOpenCloseListener` 收不到它们的菜单事件，本 mod 无法覆盖。这些场景需要 SimpleIME 侧自行处理。
- **只拦 hook 型输入**：通过 `BSTEventSink<InputEvent*>` 收热键的 mod 不受影响。
- Hook 在 `kNewGame`/`kPostLoadGame` 才安装，主菜单阶段不生效。
