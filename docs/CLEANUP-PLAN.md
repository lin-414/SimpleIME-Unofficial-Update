# 精简重构方案（功能不变）

> 目标：在**不改变任何可观察行为**的前提下削减重复、清除死码、拆开巨型 TU，并让改动本身可被验证。
> 状态：**L0–L3 全部完成；L4 除 clang-format 外全部完成**（`7ceebcd` 起 40 个提交）· 行号锚点为 `3048277`，剩余档位按符号名重定位
> 变更记录见 §9。**相对 9581e2c 版，本文已撤销 3 条失效条目、重锚全部行号，并给 L0 加了可机验命令。**

## 锚点规则（给执行者，包括 agent）

本文的行号是 `3048277` 的实况，**只作定位提示；符号名才是权威**。任何一次删除之前，先跑该行对应的「复核命令」，命中数必须符合期望——命令与期望本身就是验收条件。

```bash
git rev-parse --short HEAD          # 若不是 3048277，行号大概率已漂移，按符号重定位
grep -rn "<Symbol>" src include test tools extern   # 期望命中数见 §1
```

`extern/` 必须一起搜：`enumeration` 这类词在 CommonLibSSE-NG 的注释里也出现，只搜 `src include` 会漏掉真包含者，也会把无关命中当成引用。

## 0. 基线与闸门

当前基线（本机在 `3048277` 重建实测）：

```
cmake --build build/test-check --target SimpleIMETest      # EXIT 0
ctest --test-dir build/test-check                          # 49/49 通过，0.66s
```

闸门三条，每档改动合入前都要过：

1. 构建 EXIT 0 且**不新增 warning**（`CMakeLists.txt:125` 已开 `-Wpedantic -Wshadow -Wconversion`；当前 test 构建仍有 7 条既有 warning）。
2. ctest 49 项全绿。
3. 按 [VALIDATION.md](VALIDATION.md) 核对 DLL 字节数与 md5（build / dist / MO2 三处一致）；触及候选窗、覆盖层、焦点/输入路径的档另走实机清单。

**原方案的第 1 步前置（CI 不跑测试）已完成，不再需要**：`release.yml` 现在 `pull_request` 触发（`:14`）、以 `-DBUILD_TESTING=ON` 配置（`:69`）、跑 `ctest --output-on-failure`（`:75`）。唯一残留便利性问题：`CMakePresets.json` 仍无 `BUILD_TESTING` preset（`grep` 无命中），CI 是内联传的——本地手跑还要记这串参数，属可选优化，不再是阻塞项。

`imconfig.h` 的澄清保持不变：它是 Dear ImGui 用户配置，被 `CMakeLists.txt:96` 与 `tools/design-preview/build-preview.cmd` 引用，**必须保留**。

## 1. L0 · 零风险删除（≈265 行，可一次提交）

「复核命令」的期望值均为本次实测结果。

| 内容 | 位置（`3048277`） | 复核命令 | 期望命中 | 行数 |
| --- | --- | --- | --- | --- |
| `Enumeration<>` 模板整文件，无任何 `#include` | `include/enumeration.h` | `grep -rn "enumeration\.h" src include test tools` | 0（`extern/` 的 1 处是 CommonLibSSE 注释里的同词，非本头） | **122** |
| `IsDirty()` | `include/ime/ImeController.h:41` | `grep -rn "IsDirty" src include test tools` | 1，且必须是定义行 | ~3 |
| `IsInited()` | `ImeController.h:65` | `grep -rn "IsInited" src include test tools` | 1 | ~3 |
| `IsInitializeFailed()` | `include/ImeApp.h:108` | `grep -rn "IsInitializeFailed" src include test tools` | 1 | ~3 |
| `HasAll()` | `include/core/State.h:129` | `grep -rn "HasAll" src include test tools` | 1 | ~7 |
| `FrameworkFingerprint()` 无调用者 | `src/hooks/SkseMenuFrameworkBridge.cpp:365` | `grep -rn "FrameworkFingerprint" src include test tools` | 1 | ~22 |
| `s_calWasWantsText` 只写不读 | `SkseMenuFrameworkBridge.cpp:351,562,610` | `grep -n "s_calWasWantsText" ...` | 3，全为声明或赋值 | 3 |
| `TextService::ProcessImeMessage` 内注释掉的 fallback switch，引用的 `IsSupportCandidateUi` / `m_fallbackTextService` 已不存在 | `src/tsf/TextService.cpp:276-291`（`:269-275` 散文保留） | `grep -n "IsSupportCandidateUi" src include` | 1，且在注释行内 | 16 |
| 复制两遍的 "Priority 7/8/9" 注释 + 注释掉的 `inputContext.set` | `src/menu/ImeMenu.cpp:39-43`、`src/menu/ToolWindowMenu.cpp:83-86` | `grep -rn "Priority 7" src` | 2 | 8 |
| `RowTrailingCheckbox` 零调用 | `include/ui/panels/PanelWidgets.h:658-697`（`:302` 注释提到它，连带改） | `grep -rn "RowTrailingCheckbox" src include tools` | 2（定义 + 注释） | ~40 |
| 历史 FIXME 注释（已失效的崩溃描述） | `src/ImeWnd.cpp:863` 起 | 按 "Former FIXME" 文本定位 | — | ~8 |

### 1.1 已被后续提交推翻的条目（**不要照着旧方案删**）

- **`IsInitializing()` 不再是死码**：`src/ImeApp.cpp:347` 现在调用它（`if (app.m_state.IsInitializing() || app.m_state.IsInitialized())`）。9581e2c 版把它列进删除清单，这一条已作废。
- **`test/resources/SimpleIME.toml` 的陈旧键漂移已修**（`3048277`，−31 行）。
- **§6 原红线第 1 条（`StateKey` 枚举次序不可动）已作废**，见下文更新。

**L0 的另一个例外**：`SkseMenuFrameworkBridge.cpp:251` 的 `ResolveOne(getFrameCount, "igGetFrameCount")` 结果从不被调用，但参与 `&& ok` 链——删掉会让缺该导出的旧版 SKSEMF 从"拒绝安装"变成"安装成功"。属行为变化，保留。

## 2. L1 · 机械去重（≈500 行，逐字等价）

原则：合并成一处 helper，其余站点原样搬运；每条都能用 diff 证明分支集合与执行顺序未变。

### 2.1 Hooks 层

- **`src/hooks/Hooks.cpp`：`DetourAttach`（`:13`）与 `DetourDetach`（`:62`）仍是 44 行逐字节相同**，只差 `::DetourAttach`/`::DetourDetach` 一行（`:20` / `:69`）：同样的嵌套 `if (error = …)` 事务阶梯、同样的 6 分支 `errorMsg` switch、同样的 `logger::error("Failed detour: {}", errorMsg.data())`。→ `RunTransaction(PVOID*, PVOID, bool attach)` + `DetourErrorMessage(LONG)`。**这条是本次重锚后仍然成立、收益/风险比最高的一处。**
- `SkseMenuFrameworkBridge.cpp`：`GetModuleHandleW(L"SKSEMenuFramework.dll")` 仍 6 处（`:175,241,367,416,439,477`）；`GetModuleFileNameW + GetFileAttributesExW` 的 6 行前言三次逐字复制（`:416-423, 439-446, 477-484`）；`(high<<32)|low` 打包 3 次。→ `FrameworkModule()` + `TryFrameworkIdentity(uint64&, uint64&)`。`FrameworkDllSize()`（`:475`）**有调用者**（`:932`），不是死码，只并表不删。
- `SkseMenuFrameworkBridge.cpp`：`s_resolveInFlight`（`:881`）`exchange(true)`（`:891`）后由 4 处早退各自 `store(false, release)`（`:901,910,919,971`）；`NirnLabBridge.cpp` 的 `s_internalCall`（`:117`）手工置/清 3 组（`:221/227, 241/243, 260/265`）。→ 复用 `MeridianBridge.cpp:1333` 的 `TickInFlight` RAII 形状统一成 `ScopeFlag`。既减行，也关掉"异常路径漏复位"。
- `MeridianBridge.cpp` 四个焦点回调仍两两同形：`OnViewFocused:580` / `OnViewFocusGone:606`（View/1 路）与 `OnBrowserFocused:1021` / `OnBrowserFocusGone:1042`（UIPlatform 路），共同形状为 enabled 闸门 → 同目标早退 → `exchange` 对侧后端 → `BeginSession()`（`:553`）或 `ClearSessionState()` → `ExecuteJs(…, UI_CLEAR_SCRIPT)`（`:622` / `:1055`，常量在 `:170`）→ `RequestImeSync()`（`:541`，调用点 `:603` / `:625` / `:1039` / `:1058`）。→ 一个 `HandleFocus(Backend, ptr)`。
- 安装闸门：配置关（`MeridianBridge.cpp:1072`、`SkseMenuFrameworkBridge.cpp:985`、`PrismaBridge.cpp:116`、`NirnLabBridge.cpp:440`——NirnLab 复用 `meridianSupport` 总开关）；对手 DLL 对峙（`MeridianBridge.cpp:1084-1086` 检 `SkyrimTextBridge.dll`、`SkseMenuFrameworkBridge.cpp:997-999` 检 `TMS_SIMEtoSKSEMF.dll`）；模块未加载→NotDetected（`MeridianBridge.cpp:1090-1095`、`NirnLabBridge.cpp:450`、`PrismaBridge.cpp:124-130`、`SkseMenuFrameworkBridge.cpp:908`）；NirnLab 静默闩锁（`:479-481`、`:508-510`）。→ `include/hooks/BridgeGate.h` 的 `GateConfig/GateRival/GateModule`。
- `PrismaBridge.cpp:192-246` 四处内联 `controller->IsReady()→SyncImeState()`，而 `MeridianBridge.cpp:541` 已有 `RequestImeSync()`。→ 同形 helper。
- 上屏队列重复：`MeridianBridge.cpp:129-130,152,1298`（`QueueText`）与 `SkseMenuFrameworkBridge.cpp:206,272,1079-1085` 各自声明 `MAX_PENDING_UNITS = 8192` + `std::mutex s_pendingMutex` + `std::deque<char16_t> s_pending` + 同一套溢出告警 push（前者 12 处 `lock_guard`，后者 4 处 `scoped_lock`）。剥离常量在 `SkseMenuFrameworkBridge.cpp:1079-1081` 与 `PrismaBridge.cpp:283-285` 各就地声明，而 `include/utils/Utils.cpp:22-23,102` 早有同一对。→ 共用 `ShouldStripCommittedChar`。**`MeridianBridge::QueueText` 当前不剥离，是真实行为差异，不得顺手统一。**
- `MeridianBridge.cpp`：`BeginSession():553`（内含 `:557` `s_themeRefreshPending = true`）与 `RequestCapture():1194`（`:1198`）写入同一组 atomic。→ 抽 `ArmCapture()`；反过来让 `RequestCapture` 调 `BeginSession` 会多推一次 JS，禁止。
- `.cpp` 与 `*Logic.h` 冗余：`SkseMenuFrameworkBridge.cpp:1030` 已在不活跃时早退，`:1133` 仍传 `/*sessionActive=*/true`，`SkseMenuFrameworkBridgeLogic.h:63` 的该分支在生产路径不可达。`MeridianBridgeLogic.h:25-51` 与 `:56-84` 各自拼 `expectedSeq + ":"` 并重扫载荷，而两处是背靠背调用 → 合并 `TrySplitSessionPrefix`。
- `ConsumeUiThemeRefresh(const UiThemePalette&)`（`MeridianBridge.cpp:1211`）恒 `return true`，调用方 `src/ImeWnd.cpp` 只看 `ConsumeUiThemeRefreshRequested()`（`:1206`）→ 改 `void`。

### 2.2 UI 层

- `ImGui::SetCursorScreenPos({row.trailingRight - W, row.centerY - H * 0.5F})` + `TextLink` 这一形状仍恰好 10 处：`ToolWindow.cpp:954, 989, 1004, 1121, 1315, 1368, 1375`（7）+ `AppearancePanel.cpp:589, 596, 765`（3）。`PanelWidgets.h` 已有 `RowTrailingSwitch:614`、`RowTrailingText:885`，独缺 `RowTrailingTextLink`。→ 补 helper，算术原样搬。
- 同一 `Translate()` key 双传：`RowTitle(row, Translate(...))` 在 `ToolWindow.cpp:952, 988, 1086, 1119, 1199, 1236`（6）+ `AppearancePanel.cpp`（3）= 9 处。`BeginSettingsRowEx`（`PanelWidgets.h:369`）本就把 title 收进 `SettingsRowScope`（`:360-364`）。→ 让 `RowTitle(row)` 复用已存标题。
- 4 个手抄 20 行 combo 行：`AppearancePanel.cpp:482-538`、`:774-823`、`ToolWindow.cpp:1130-1175`、`:1179-1214`（`MockPanels.cpp:1001,1029,1343,1368` 各再抄一遍）→ `SettingsComboRow`（`PanelWidgets.h:700 SettingsToggleRow` 已证明行级 helper 成立）。
- 卡片脚手架 `SectionHeader + BeginSettingsCard/EndSettingsCard`：`ToolWindow.cpp` 的 `Panels::BeginSettingsCard` 现有 **8** 处（`:672, 765, 855, 867, 906, 927, 936, 943`）。页首四件套 4 处（`Fonts.cpp:187-191`、`AppearancePanel.cpp:289-293`、`ToolWindow.cpp:731-735, 873-877`）。→ `SettingsSection/PageScope`。**各站点现有 `AutoResizeY` 选择保持原样，会动像素。**
- 量宽样板 11 处（`ToolWindow.cpp:534-538, 767-771, 1029-1034, 1221-1227`；`AppearancePanel.cpp:726-730` + mock 6）：已有 `MeasureButton`/`KeycapWidth`/`TextLinkWidth`，缺 `MeasureListText`。`GetTextLineHeight()*0.6F` 在 `ToolWindow.cpp:676, 771` 重新推导 `StatusDot` 直径（`:57` 用 `*0.30F`）。
- **`src/ui/ImeWindow.cpp:107 DrawCandidates` 与 `:182 DrawVerticalCandidates` 前段同形**（colors lambda、`InvisibleButton`、选中/hover 底色、`AddRectFilled`、居中偏移、提交尾），`:189` 注释自陈 "Same pill styling as the horizontal chip row (DrawCandidates)"。→ 抽 row painter，两个布局只供几何。
- 状态水洗绘制 5 处（`PanelWidgets.h:475-497, 784-788, 976-981`、`ToolWindow.cpp:138-143`）→ `DrawStateWash(...)`。
- 配置路径仍 4 处内联 `GetPluginInterfaceDir() / "SimpleIME.toml"`（`ToolWindow.cpp:957, 1031, 1104, 1164`），而 `SettingsManager.h:21` 有 `ConfigFilePath()`。`ToolWindow.cpp:1029-1030` 的理由**已核实为真**：`ConfigFilePath()` 是定义在头文件里的**非 inline** 函数（`:21-24`），目前全仓只有 `src/ImeApp.cpp` 一个 TU include 它（`grep -rln "SettingsManager.h" src include tools` 仅 1 行），所以现在不冲突；一旦第二个 TU include 就是重复定义。其 `CONFIG_FILE_NAME`（`:19`）也是 header 内 `static constexpr` → 内部链接，第二个 include 者会拿到另一份。→ 把 `ConfigFilePath()` 标 `inline`、`CONFIG_FILE_NAME` 改 `static constexpr`→`inline constexpr`，四处改调用。
- `src/ImeWnd.cpp:1055-1072` 的调试面板 18 行逐面抄写 → `{bit, label}` 表驱动。顺带修错字：`:1058` 画的是 `conversionMode.IsAlphanumeric()`，标签写成 `"CMODE Native"`。

### 2.3 IME 核心

- `src/ime/ImeController.cpp:45, 76, 91, 107, 121, 134, 147, 160`：**8 个**同形包装 `if (!IsReady()) return; AddTask([this, X]{ if (!IsReady()) return; … });` → 一个私有 `PostToImeThread(name, member-fn)`。`:292 RestoreKeyboard` 与 `:323 UnlockKeyboard` 是只差日志与 `TryRestore`/`TryUnlock` 的孪生 → `SetKeyboardCooperativeLevel(bool restore)`。`:248 DoForceFocusIme` 内两臂皆 `return result` → 折叠。
- `src/ime/Imm32TextService.cpp`：`ImmGetContext` 现有 **10 处**（`:118, 175, 191, 203, 218, 235, 306, 318, 327, 336`），`ImmReleaseContext` 手工配对，`:265` 注释明写 "error path must still pair the ImmGetContext above" → RAII HIMC 作用域，由构造保证配对。`:350 OpenCandidate` 与 `:355 ChangeCandidate` 函数体同为 `ChangeCandidateAt(hIMC);`（`:360`）——**二者不可合并**（`include/ime/ITextService.h:215/:216/:217` 是三个不同成员，`OpenCandidate` 与 `ChangeCandidate` 是不同调用语义的入口），只保留现状并加注释说明为何不合。
- `include/FakeDirectInputDevice.h:115-149`：`TryRestoreCooperativeLevel` / `TryUnlockCooperativeLevel` 18 行结构相同 → 参数化 flag 来源与日志串。
- 小重复：`Clear(IN_COMPOSING)+Clear(IN_CAND_CHOOSING)` 5 处 → `State::ClearComposing()`；"IME 持有键盘"谓词 3 处 → `State::ImeOwnsKeyboard()`；`SnapshotCompositionAndCandidates` 在 `include/tsf/TextStore.h:324-330` 与 `include/ime/ITextService.h:180-186` 字节级相同。

## 3. L2 · 纯搬移，给巨型 TU 瘦身

- **`src/ImeWnd.cpp:119-302`（184 行）搬独立 TU。** 本次重验：该区间 `m_`/`this->` 引用数仍为 **0**；调用点为 `Draw()`（`:561` 起）的 `:650 HealStuckShortcutKeys`、`:651 HealStuckMouseButtons` 等。搬移后匿名命名空间链接性原样保留。这把"ImGui 输入态修复"从 ImeWnd 的窗口过程 / 主题调色 / 渲染 / 调试面板四合一里摘出。
- **`MeridianBridge.cpp:263` 起的 `BridgeScript`（内嵌 JS，使用点 `:813`）、4 处手拼脚本与 CSS 构造（`:1211` 附近）移到 `include/hooks/` 兄弟头。** **必须配套**：`docs/VALIDATION.md` 记录过"原始字符串终止符吞掉 IIFE 收尾括号、DLL 内 JS 残缺"的发布级事故；移动后用 Node 跑 `test/MeridianBridge.cjs`（目前全仓 0 引用，正好激活）。
- 长函数拆分（拆法即注释里已有的分段）：
  - `ImeWnd::WndProc` `src/ImeWnd.cpp:711` 起。Seam：reclaim 块、`WM_CHAR` 体、Shift-tap 三段各自自包含 → 提为 `(HWND, WPARAM, LPARAM)` 私有成员。
  - `ImeManager::EnableIme` `src/ime/ImeManager.cpp:91` 起。Seam：enable 体 / disable 体 → `DoEnable()`/`DoDisable()`。
  - `SkseMenuFrameworkBridge::UpdateFieldAnchor` `:507` 起：候选剪枝循环写两遍、`kept==1/0` 阶梯写两遍、同一条 warning 写两遍 → `PruneCandidates(pred)` + `ConfirmOrDrop(kept)`，按三种互斥模式拆。
  - `FontPreviewPanel::DrawResultCard` `src/ui/fonts/preview_panel.cpp:189` 起（注释已给六段）。
  - 次级：`PanelWidgets.h:367 BeginSettingsRowEx`、`TextStore::DoUpdateUIElement`（最大嵌套深度 7，`:1060-1147` 缩进误导 `else` 归属——**只重排缩进，不改结构**）。

## 4. L3 · 先补测试才能动

### 4.1 配置层描述表（本节相对上一版已重写）

`896948b`/`3044bbf` 把读取端换掉了：现在是**单一严格类型读取器** `findAndSetStrict<T>`（`src/configs/ConfigSerializer.cpp:191-247`，用 `if constexpr` 按 `bool`/`string`/`float`/`vector<string>` 分派），把"键存在但类型错"从 `find_or` 静默吞掉改成 warn + 收进 `ignoredKeys`；调用端统一成 `apply(表, KEY_X, config.x)`（**26 处**，`:255-303`）；`ValidateConfiguration`（`:382`）把 `ignoredKeys` 透出给 UI。文件从 340 行涨到 **406 行**。

因此：

- **撤销**旧条目"`findAndSet` / `findAndSetFloat` 是未文档化的类型分叉"——已被这个模板消掉。
- **新增前提**：任何描述表化改造必须**接在 `findAndSetStrict` 之上**（表的每个字段带类型标签，交给同一个严格读取器），不得退回 `find_or`，否则 `896948b` 修的"错键被吞"会复发。

仍然成立的重复（按 `skseMenuFrameworkSupport` 实测普查）：

| 站点 | 位置 |
| --- | --- |
| 字段声明 + 默认值 | `include/configs/configuration.h:57` + `:104` |
| UI 侧字段 + 默认值 | `include/ui/Settings.h:144` + `:173` |
| 键常量 / 写表 / 读 `apply` | `ConfigSerializer.cpp:67`、`:135-181`、`:302` |
| 双向转换 | `settings_converter.cpp:352`（→Settings）、`:427`（→Config） |
| 出厂模板 | `contrib/config/SimpleIME.toml` |
| 功能开关读取 | `src/hooks/SkseMenuFrameworkBridge.cpp:985` |
| i18n | `Settings.Behaviour.SkseMenuFrameworkSupport` + `…ToolTip` × **6** locale = 12 条字符串 |

即：一个字段 ≈ **9 个代码站点 + 12 条 locale 字符串**，跨 8 个文件（+6 locale 文件）。UI 行侧已比上一版好（`ToolWindow.cpp:886-892` 走 `DrawCompatibilityRow` 助手，6 行调用而非手写脚手架）。

默认值仍是两处编译期复制（`configuration.h:85-105`、`Settings.h:156-174`），但**现在有真测试钉住**：`test/settings_converter_test.cpp:305 ShippedConfigurationTest.shipped_toml_converts_to_default_settings` 断言出厂 TOML 转换结果 == `GetDefaultSettings()`，且由 `test.cmake` 的 PRE_BUILD 把 `contrib/config/SimpleIME.toml` 拷到测试产物目录（`+` 注释见该 diff）。这比我上一版说的"只有一句注释当守卫"强得多，描述表改造可以直接复用它当回归网。

仍需修的三项（**均已落地，2026-10-07**）：

1. ~~`RandomUtils.h:44-79 GetRandomConfiguation()` 不给 `themeStyle`、`meridianSupport`、`prismaAvoidance`、`skseMenuFrameworkSupport` 赋值~~ → 四字段已随机化（themeStyle 取两个文档拼写），随机覆盖 **26/26**；顺带补上既有测试缺失的 `skseMenuFrameworkSupport` 断言（`7c8a19d`）。
2. ~~无 settings→config→settings 往返测试~~ → 新增 `SettingsRoundTripTest.settings_config_settings_is_a_fixed_point`：首轮转换即归一化（钳制/枚举解析/默认主题钉色），此后必须是**不动点**——随机宽配置 + 编译默认两路验证（`7c8a19d`）。这也回答了"写侧无区间钳制"：s1 已钳制，写侧回写的是钳制后的值，无需对应钳制。
3. ~~`last_native_conversion` 不在出厂模板~~ → 已补进 `contrib/config/SimpleIME.toml`（连同运行时缓存注释），ShippedConfigurationTest 仍绿（`e293408`）。

### 4.1.1 描述表机制评估（结论：不做，改为清单+回归网）

对"一个字段 9 站点"逐站点核过一遍后的结论——**描述表在此代码库里不是净收益**：

- 读侧（站点 6）已是 `apply(表, KEY, config.x)` 每字段一行、全部走 `findAndSetStrict`；把这一行换成"带类型的描述符数组"需要异构成员指针（`variant`/lambda 数组），行数不减、可读性更差。
- 写侧（站点 5）每条目自带注释（`toml::value{值, Comments}`），转换器（站点 8/9）每字段都有真实逻辑（string↔enum↔chord、RGB 掩码、zoom 钳制、默认主题钉色）——描述表吸收不了这两侧，只有 X-macro/代码生成能吸收，而那是与本档"行为不变、逐条可审计"原则相抵触的大改。
- 真正能单源化的只有"键常量+写+读"三站点的重复，收益 ~26 行；代价是引入一层间接。
- **替代交付**：新增字段的 9 站点清单（上述表格即清单）+ 已强化的回归网（26/26 随机覆盖 + 往返不动点 + ShippedConfigurationTest 钉出厂模板）。将来若确有描述表需求，回归网已就位，可安全重构。

区间钳制仍只在读侧两处（`settings_converter.cpp:298-303, 330-341`），写侧无对应（往返不动点测试已证明这是稳定的）。

### 4.2 design-preview 与真实面板合流

`build-preview.cmd:25-38` 只编 `DesignPreviewMain.cpp` + `MockPanels.cpp` + imgui + imguiex，**不链任何 `src/ui/*.cpp`**，而 `PanelWidgets.h` 共享——每个面板体都是分叉副本。重叠度实测（两侧各做「折叠空白 + 去掉长度 ≤8 行与注释行」归一化，再看 mock 行是否逐行出现在真实面板）：mock 归一化 1155 行中 **517 行（44.8%）逐行相同**；来源分布 `ToolWindow.cpp` 283、`AppearancePanel.cpp` 152、`preview_panel.cpp` 72、`Fonts.cpp` 10。成对示例：侧栏 item lambda `ToolWindow.cpp:116-192` vs `MockPanels.cpp:388-464`、keycaps `ToolWindow.cpp:596-635` vs `:195-232`、候选预览 `AppearancePanel.cpp:376-419` vs `:959-997`、`DrawResultCard` `preview_panel.cpp:196-447` vs `:766-918`。

字符串表漂移（**逐 key 比对，不要整表一刀切**）：`MockPanels.cpp:55` 的 `std::array<StrEntry, 114>`（覆盖 `:55-166`）里，`SaveHint`（`:63`；TOML 0 命中、`src/` 0 对应，却在 mock `:469` 与 `:560` 两处被渲染）、`Behaviour.Policy.*`（TOML 0 / mock 5）、`FontBuilder.NotSelected`（`:164`）、`FontBuilder.PreviewingPath`（`:165`）确实不存在于出厂文案；但 `FixInconsistent*` 在 TOML 里**是存在的**。做法：绘制体上收到真实 TU、置于只含数据（settings + `std::string_view(key)` 回调）的结构后，mock 只留 stub；顺带注意 `9f1f5a2` 刚补过 `Settings.Advanced.Logging` 键，比对基准要用最新 TOML。

### 4.3 其他

未测但可测的纯逻辑接缝（补测优先）：`include/core/State.h`（约 24 个 constexpr 位掩码函数）、`include/tsf/ConversionModeUtil.h:13`、`include/ui/DebounceTimer.h`、`include/RE/ControlMap.h:64-67` + `src/RE/ControlMap.cpp:38-64`。`PrismaBridge`/`NirnLabBridge` 仍无 `*Logic.h`，纯分类逻辑内联在 `.cpp`（`NirnLabBridge.cpp:440`、`PrismaBridge.cpp:116`），而 `test.cmake:9-15` 只编 4 个 TU，够不着。`test.cmake:4` 仍 glob 不存在的 `benchmarks/`，每次走 `:43` 告警分支。

## 5. L4 · 仓库与文档卫生

> **执行状态（2026-10-08）**：PROGRESS.md → `docs/PROGRESS.md` + README 链接（`269772f`）；`docs/banner.png`+`cover.png` 去跟踪（`aee2297`，生成器 .html 保留、工作副本留盘）；`extract_i18n.py` → `tools/`（`1ae8713`）；根目录散落物 → `dev/`（已被 .gitignore 覆盖，无提交）；约定漂移 → `docs/adr/0003-accepted-conventions-and-drift.md`（`79b1ac0`）；`NirnLabBridge::s_state` 非 atomic 按"独立缺陷"修复（`62e826a`）。**clang-format 推迟**：61 文件重排必须落在安静树上的独立提交，而当前工作树载有进行中的特性开发（见 §5 末条）；ADR-0003 已记录该推迟。

- **`PROGRESS.md`** 144KB / **1903 行** / 44 个 `## 第 N 轮`，8 个轮次号重复（25–32 各 2–3 次），全仓唯一入链 `README.md:19`；`CHANGELOG.md`（520 行）由 git-cliff 自动生成。建议**归档到 `docs/` 或按版本拆分**，不要直接删——这是维护者工作日志。`docs/adr/` 两份 ADR 是真内容，保留。
- **跟踪的二进制**：`docs/banner.png` + `docs/cover.png` 1.39MB 被 git 跟踪、md/yml/bbcode/txt/cmd 里 0 引用，生成器是同级 `.html`。`docs/default-theme-{dark,light}.png` 有 `README.md:147` 引用，保留。
- **无引用脚本**：`extract_i18n.py`（CMake/CI/脚本 0 引用）；`build_ime.cmd` 硬编码本机路径——但它正是 `FIX-HANDOFF` 第 0 节指定的增量构建入口，删前要确认。
- **根目录散落物**：`ft_probe.cpp`/`.exe`、`build_out.log`、`preview_build.log`、`preview_debug.txt` 全部已被 `.gitignore` 命中且未跟踪，不是提交污染 → 挪进 `dev/`。
- **格式化**：**61** 个文件偏离 `.clang-format`（9581e2c 时是 58，新增 3 个）。格式化必须是独立 whitespace-only 提交，或与 `L1` 分开；绝不能混在一次提交里，否则 diff 不可审。
- **约定漂移**（写进一份贡献约定或 ADR，不要全局改名）：`s_`/`g_`（`ScaleformHook.cpp:39,58` 的 `"Installed {}: {}"` 构造日志 2 处）三制并存；`lock_guard`（Meridian 19 / NirnLab 5）vs `scoped_lock`（SkseMF 4）；`std::uint8_t` 与 `uint8_t` 混用；出参命名 `a_x`/`x`/`indexOut` 三制；`ScaleformHook.h` 仍 `#ifndef` 而其余 `#pragma once`；同一概念五个谓词名（`ShouldRoute`/`HasFocus`/`SessionActive`/`OwnsInput`/`OwnsCandidateUi`）。`NirnLabBridge.cpp:102` 的 `SupportState s_state` 仍**非 atomic**，而另三个桥用 `std::atomic<SupportState>`——这不是风格问题，跨线程读（`MeridianBridge.cpp` 里 `StateToken(NirnLabBridge::State())`）是数据竞争，属独立缺陷，别当去重顺手改。
- 两份 zoom 刻度并存：`include/ui/Settings.h:34-36`（0.5F/2.0F/25）vs `include/ui/panels/AppearancePanel.h:17-20`（50/200/25/100）。

## 6. 红线：看着像重复但必须原样保留

> **原红线第 1 条已作废**：`9581e2c` 时 `SetState` 靠 `while (current < stateKey)` 用枚举**序号**决定能否前进，所以次序不可动。`3048277` 已把它换成显式白名单 `ImeApp::State::IsLegalTransition`（`include/ImeApp.h:84`，注释 `:81-83` 明写 "Explicit transition whitelist instead of enum-value ordering"），`SetState`（`:55`）改为 `while(true)` + 合法性判定（`:68`）。**枚举次序不再承重，该条不再约束本方案**；但 `IsLegalTransition` 的分支集合是行为，仍然禁止"去重"。

1. `INPUT_PROCESSOR_ACTIVATED` 是跨线程快照，不可改成访问器现算——真值 `m_activatedProfile < m_langProfiles.size()` 骑在无同步 `std::vector` 上；`InputMethodManager.cpp` 已警告不要收窄。`Core::State` 所有标志同理（`State.h:211` 是 `std::atomic`）。
2. `WndProc`（`src/ImeWnd.cpp:711` 起）的 **8 个** `if (pThis == nullptr) break;`（`:747, 808, 813, 819, 831, 841, 846, 882`）不是一个早退：`CM_EXECUTE_TASK`（`:826`）与 `WM_IME_SETCONTEXT`（`:837`）故意在无实例时运行，统一 hoist 会丢掉任务队列抽干。
3. 故意的防御性重复：`Draw` 的 `m_fTearingDown` + 三重空检；`AbortIme` 测过标志仍无条件发 `CM_ABORT_IME`；`TextStore::GetCandidateInterface` 返回 hr 而非抛异常（避免穿越 TSF 非 C++ 帧展开）。
4. 时序敏感竞态块（均带注释）：提交回显窗口（`ImeWnd.hpp m_lastCommitTickMs`）、裸 Shift 预测、`m_fWantClearInput` 延迟到 `Draw()`、`GetSinkWriteLock(m_fLocked)` 重入处理、`RequestLock` 的"scope must end BEFORE `UnlockDocument()`"（`src/tsf/TextStore.cpp:313`）、`TextStore.cpp` 有意关闭的缓存。
5. **不对称的 `Release()`**：`TextStore::Release`（`src/tsf/TextStore.cpp:184`）与 `TsfCompartment::Release`（`src/tsf/TsfCompartment.cpp:140`，`:146` 有下溢 error 日志）有保护，`InputMethodManager::Release`（`src/tsf/InputMethodManager.cpp:435`）**没有**。不要用带保护那份去重——那是加日志改行为，属独立缺陷。
6. `HealStuckShortcutKeys`/`HealStuckMouseButtons` 形似神不似；`FakeDirectInputDevice` 约 25 个一行式转发是纯虚必需；`TextStore` 的 `E_NOTIMPL` stub 与 8 处 `IsLocked → TS_E_NO_LOCK` 是 COM/TSF 契约；4 处手写 `QueryInterface/AddRef/Release` 见第 5 条不合并；`FuncTracer` 32 处无 `SIMPLE_IME_TRACE` 时被编译掉（`include/tsf/TextStore.h` 的 `#else` 空类型），保留。虚表槽改写 5 处贴 ABI，最多只包 3 行机械部分。
7. `include/RE/ControlMap.h`：`:23-38` 不动；`:40-56` 的注释与 `:76-78` 三条 `static_assert`（`kTotal == 17`、`offsetof(textEntryCount) == 0x120`、`offsetof(allowTextInput) == 0x128`）钉住 SE/AE 镜像布局，统一 `IsSE()` 分支会让全游戏文本输入错乱。

## 7. 执行顺序与验收

| 步 | 内容 | 合入条件 |
| --- | --- | --- |
| 1 | L0 删除 | 每行「复核命令」先跑；构建 EXIT 0 无新 warning；ctest 49/49；`git diff --stat` 只含删除 |
| 2 | L1 Hooks | 同上 + 实机：控制台、Meridian、SKSEMF、Prisma 四路输入与候选框逐项对照基线 |
| 3 | L1 UI | 同上 + 实机截图对照（F2 设置窗、语言栏、候选窗横/竖） |
| 4 | L1 核心 | 同上 + 实机：组合中 Esc 关闭、Win+Shift+S 切窗、读档、Shift 中英切换 |
| 5 | L2 搬移 | JS 搬移需过 Node 自测；`_DEBUG` 覆盖层仍在 |
| 6 | L3 配置表 | 先补 §4.1 的 1（4 字段随机化）与 2（往返测试）并**单独绿一次**，再动表；出厂键 `last_native_conversion` 补进模板后 ShippedConfigurationTest 须仍绿 |
| 7 | L4 卫生 | 逐条独立提交，不与代码混 |

预计净减 **≈700–800 行**（L0 + L1），另有 **≈600 行**从两个 1.3k 行 TU 搬出，L3 再减 250–300 行；`include/enumeration.h` 整个消失。

**与 [FIX-HANDOFF-2026-10-07.md](FIX-HANDOFF-2026-10-07.md) 的协调**：该工单的 E1（CI 跑测试）、E2（测试自身缺陷）、D3（错键被吞）、D4（保存失败不可感知）**已经落地**，对应条目已从本方案撤销或改写。剩余仍会撞车的文件：C3（`Imm32TextService` 影子成员 ↔ 本文 §2.3 的 RAII HIMC）、B1（`REL::safe_write` 谎报 ↔ §2.1 NirnLab 闸门）、C1（候选提交假成功 ↔ §3 `TextStore`/`ImeWindow`）。规则不变：**工单改行为、本方案要求行为不变，两者绝不混在同一次提交**；撞车文件按工单优先，本文相关条目退后一轮。

## 8. 待拍板

1. `PROGRESS.md` 归档还是原样保留？
2. 6 份 `translate_*.toml` 是否纳入配置描述表？纳入则 L3 收益翻倍，但要改出厂文件。
3. 只服务 `_DEBUG` 覆盖层的谓词，以及 `GAME_LOADING`（写：`src/ImeApp.cpp`；读：仅 `src/ImeWnd.cpp:1071` 调试面板）要不要一并收掉？收了调试面板少两行，属 debug 构建可见变化。
4. `MeridianBridge::QueueText`（`:1298`）不剥离 `` ` `` / `·`，SKSEMF 侧剥离——历史遗留还是有意为之？决定 §2.1 能否共用 `ShouldStripCommittedChar`。

## 9. 变更记录（相对 9581e2c 版）

| 变更 | 依据 |
| --- | --- |
| 撤销「CI 从不跑测试」前置 | `6a0a9a1`；`release.yml:14,69,75` |
| 撤销「`test/resources` 陈旧键」漂移项 | `3048277`；`test/resources/SimpleIME.toml` −31 行 |
| 撤销红线「`StateKey` 枚举次序不可动」 | `include/ImeApp.h:81-100` 引入 `IsLegalTransition` 白名单 |
| **`IsInitializing()` 移出 L0 删除清单** | 新调用点 `src/ImeApp.cpp:347` |
| §4.1 依新读取器重写；`findAndSetFloat` 项撤销 | `ConfigSerializer.cpp:191-247` 的 `findAndSetStrict<T>` |
| 默认值三处复制的风险下调 | `settings_converter_test.cpp:305` + `test.cmake` PRE_BUILD 拷贝出厂 TOML |
| 基线由 48/48 更新为 49/49 | 本机在 `3048277` 重建实测 |
| L0 加「复核命令 + 期望命中」列；全文行号重锚 | `IsDirty`/`IsInited`/`IsInitializeFailed`/`HasAll`/`FrameworkFingerprint`/`RowTrailingCheckbox`/`s_calWasWantsText` 命中数逐条实测 |
| clang-format 偏离文件数 58 → 61 | `clang-format --dry-run` 重跑 |
| **L0 执行完毕**：11 项全删（commit `7ceebcd`，10 文件 −243/+2，净 241 行） | 复核命令逐条先验后清零；ctest 49/49；全量构建 EXIT 0 且**无新增 warning**（TU 级基线对比实证，顺带消掉基线既有的 `FrameworkFingerprint` unused-function 警告）；DLL `adea97bd` 已同步 build/dist/MO2。两处注释行改为改写而非删除（避免悬空引用），其余全为纯删除 |
| **L1 Hooks 执行完毕**：§2.1 十项全落（`b1ffc65`→`3073200` 共 10 提交，12 文件 +361/−241，含新头 ScopeFlag.h/BridgeGate.h） | 每项独立提交、独立构建；ctest 49/49（含 Logic 头重编）；最终重建 TU 警告清单与基线逐条吻合、零新增（SkseMF 既有 unused-variable 随项 2 清零）；DLL `b5d07c2e` 三处同步。**计划内保留**：GateModule 闸与 NirnLab 静默闩锁（四处日志句式各异，硬合并只是搬方差）；`ShouldNeutralizeAscii` 的 sessionActive 参数（生产不可达但被测试决策表钉住）；`MeridianBridge::QueueText` 不剥离、NirnLab `s_state` 非 atomic 均原样。实机四路对照待用户 |
| **L1 UI 执行完毕**：§2.2 九项全落（`6cd9634`→`1ce4009` 共 9 提交，主构建+design-preview 构建双验证） | 项 2 前提修正：SettingsRowScope 原本**不**存 title，已补字段+重载（12 处收敛，带 offsetX 的主题行保留显式重载）；项 1 精确同形仅 4 处（方案枚举的复制对/按钮/分段器站点不能并入，否则动像素）；项 5 含 mock 5 处同步+StatusDotDiameter（0.6F 与 StatusDot 内 0.30F 半径的 1-ULP 差故不动 StatusDot 本体）；项 3 收编 4+2 组合行；项 4 收编 8 卡片+1 页（StatusCard/四个大卡片体/两个 ToolWindow 页体/Fonts 页按形状保留）；项 6 抽 DrawCandidatePill+CommitClickedCandidate（SameLine 时序逐项保持）。SettingsPage 依赖后置符号须定义在文件尾。ctest 49/49；重建 TU 警告零新增；DLL `f32dc4b8` 三处同步。实机截图对照待用户 |
| **L1 Core 执行完毕**：§2.3 四项落地（`afadb1f`→`56f3203` 共 4 提交），**L1 全部完成** | 项 1：`PostToImeThread(body)` 收编 8 个 AddTask 包装的就绪复检不变量（方案的"8 个同形"实为 8 同形外壳+4 同体 Do\*，4 个异构体保留）；Restore/UnlockKeyboard→`SetKeyboardCooperativeLevel(restore)`（日志逐字保留，含 "Failed lock keyboard." 原文）；DoForceFocusIme 双臂折叠。项 2：`AcquiredHimc` RAII 收编全部 10 处 HIMC 配对（OnComposition 的错误路径专用 release 与注释随之消失）；OpenCandidate/ChangeCandidate 加不合并注释。项 3：`TrySetCooperativeLevel(restore)` 参数化 DI 孪生。项 4a：`State::ClearComposing()` 收敛 5 处配对（含一处逆序——纯位存储无观察差）。**按证据保留**：`ImeOwnsKeyboard`（谓词现仅 2 处且参数不同——WantTextInput vs 常量，搬进 State 反跨层，第三处已被 FIX-HANDOFF 吸收）；Snapshot 双胞胎（锁类型不同族 shared_mutex vs 未证实，2×6 行收益不抵间接层）。ctest 49/49；重建 TU 警告零新增；DLL `bf686274` 三处同步。实机清单：组合中 Esc 关闭、Win+Shift+S 切窗、读档、Shift 中英切换 |
| **L2 执行完毕**：§3 全部落地（`83592e3`→`9f2a6f0` 共 8 提交），**L0–L2 完成** | 项 1：ImeWnd 输入态修复块（190 行）→ `ImGuiInputState.h/.cpp`（3 条警告随搬移 1:1 转移）；项 2：BridgeScript → `MeridianBridgeScript.h`（脚本化逐字节搬移）+ **Node 自测首次激活全绿**；长函数拆分：`UpdateFieldAnchor` 三模式+`PruneCandidates`+`MarkAnchorPathBroken`、`EnableIme`→`DoEnable/DoDisable`、**WndProc 四个消息体成员化**（8 处 `pThis==nullptr` 红线原位保留）、`DrawResultCard`→`ResultCardLayout`+四段 painter、`DoUpdateUIElement` 仅缩进（`git diff -w` 为空实证）。**环境坑**：GLOB 无 CONFIGURE_DEPENDS，加新 TU 须重跑 configure，而 configure 清 RC 补丁——已固化为 **`simple_rc.py`（configure 后必跑）**。**拆分引入的 1 条新警告已修**（HandleCharMessage 的 unused lParam，`9f2a6f0`）；preview_panel 两条为首次重编才暴露的既有警告。ctest 49/49；Node 复跑绿；DLL `32d2c6a4` 三处同步 |
| **L3 前置与评估完成**：§4.1 三项待修全部落地（`7c8a19d`、`e293408`），描述表机制经逐站点核验**评估为不做**（见 §4.1.1） | 随机配置 26/26 覆盖（含此前恒真的 4 字段）；新增 settings→config→settings **不动点**测试（随机宽配置+默认两路）；出厂 TOML 补 `last_native_conversion`（ShippedConfigurationTest 仍绿）。ctest **50/50**。替代交付=9 站点清单 + 强化回归网 |
| **L4 执行（除 clang-format）**：`269772f`→`79b1ac0` 共 5 提交 | PROGRESS 归档 docs/、PNG 去跟踪、脚本归 tools/、散落物归 dev/、ADR-0003 记录约定漂移、`s_state` 竞态修复。**clang-format 推迟**（安静树独立提交原则 + 工作树有在途特性）。**注**：`7c8a19d` 提交的测试文件里含当时工作树中未提交特性的 5 条断言（`switchEnglishLayoutOnDisable`）——HEAD 单独构建测试目标会失败，待该特性源码落地后自愈；工作树本身一致且全绿 |
