# SimpleIME 修复交接单（2026-10-07 全仓库审计产物）

> 交给实施 agent 的自包含工单。所有结论来自 22 路只读审计（PR Review Toolkit）+ 主 agent 逐行复核。
> **本文件只描述"要改什么、怎么验收"，不包含任何已实施的改动。**

---

## 0. 仓库事实（不要假设别的）

| 项 | 值 |
|---|---|
| 项目 | SimpleIME 3.1.2-beta，Skyrim SE/AE 的 SKSE 插件（中文输入法注入） |
| 语言/框架 | C++20、CommonLibSSE-NG、Dear ImGui 1.92.7（FreeType + DX11 动态图集）、toml11 v4 |
| HEAD | `9581e2c`（工作区干净，仅 `docs/` 两份未跟踪文档 + 子模块指针 `m extern/CommonLibSSE-NG`） |
| 规模 | `src/`+`include/` 108 文件 / 21,536 行；`test/` 1,520 行 |
| 规范来源 | **仓库内没有 CLAUDE.md / AGENTS.md**。生效约束：`.clang-format`（Microsoft base）、`docs/VALIDATION.md` 的验收记法（✅实机 / 🧪离线 / ⬜待验证）、`cliff.toml` 的 Conventional Commits |
| 增量构建 | `cmd /c build_ime.cmd`（内部调用 vcvars64 + `cmake --build build\RelWithDebInfo-clangcl-ninja-vcpkg --config RelWithDebInfo`），必须 EXIT 0 |
| 全新配置 | `cmake --preset RelWithDebInfo-clangcl-ninja-vcpkg`（另有 `debug-` / `release-clangcl-ninja-vcpkg`） |
| 跑单测 | `cmake --preset RelWithDebInfo-clangcl-ninja-vcpkg` 时加 `-DBUILD_TESTING=ON`，再 `cmake --build --target SimpleIMETest`，`ctest --test-dir build/<preset> --output-on-failure`。当前注册 48 用例。注意 `CMakeLists.txt:7` 默认 `BUILD_TESTING=OFF` |
| CI 现状 | `.github/workflows/release.yml` **完全不跑测试**（无 ctest / 无 `BUILD_TESTING` / 无 node 步骤），只在 tag 推送时构建 `SimpleIME` + CPack 打包 |

## 1. 实施规则（硬性）

1. **一个任务 = 一个 commit**，Conventional Commits：`fix: ...` / `refactor: ...` / `ci: ...`（`ci` 是类型不是 scope）。标题 ≤ 50 字符，正文 72 换行。
2. **最小改动面**：不做顺手重排、不重命名相邻标识、不带入无关清理。发现别的问题写进 commit 正文或另开工单。
3. **禁止修改** `extern/`（含 CommonLibSSE-NG、JamieMods、imgui、PrismaUI、NirnLabUIPlatform）。它们是 vendored 参考/依赖，只读。
4. 注释规则：只写"事实 + 后果"，默认不写；1–2 行为上限。不复述代码在做什么，不叙述本次修复历史。
5. 不得跳过校验钩子（`--no-verify`）。钩子失败就修根因。
6. 不 push、不建 PR、不打 tag、不动 `CMakeLists.txt` 里版本号与 `CHANGELOG.md` / `dist/`（发布由维护者本人做）。完成后回报 diff 即可。
7. 每完成一个任务：跑一次 `build_ime.cmd` 确认 EXIT 0，并在回报里附证据（日志行、`ctest` 输出、或截图路径）。**不许用"编译通过"代替"行为正确"**。
8. 标注 `⬜` 的任务需要真实游戏环境验收，无法离线定案；实施 agent 只能做到"代码改动 + 提出可机验预测"，最终 ✅ 由维护者实机确认。

## 2. 任务索引与依赖

| 批次 | 任务 | 严重度 | 主要文件 | 依赖 |
|---|---|---|---|---|
| A | A1 正常退出拆 IME 线程 | Blocker | `src/ImeApp.cpp` | — |
| A | A2 拆除门改资源判据 | Blocker | `src/ImeApp.cpp` | A1 |
| A | A3 重激活路径 | High | `include/ImeApp.h`+`src/ImeApp.cpp` | A1,A2 |
| A | A4 `SendNotifyMessageToIme` 三态 | Medium | `src/ImeWnd.cpp`+`include/ImeWnd.hpp` | — |
| B | B1 写失败可见化 | Blocker | `src/hooks/MeridianBridge.cpp`、`src/hooks/NirnLabBridge.cpp` | — |
| B | B2 per-backend 状态上 UI | High | `src/hooks/MeridianBridge.cpp`、`src/ui/ToolWindow.cpp` | B1 |
| B | B3 pin 死路径 | Blocker | `src/hooks/NirnLabBridge.cpp` | — |
| B | B4 hook 安装失败不再谎报 | High | `include/hooks/Hooks.hpp`、`src/hooks/WinHooks.cpp` | — |
| C | C1 提交返回值贯穿 | Blocker | `src/ime/Imm32TextService.cpp`、`include/ImeWnd.hpp`、`src/ime/ImeController.cpp` | — |
| C | C2 索引语义统一 | High | `src/ime/Imm32TextService.cpp`、`src/tsf/TextStore.cpp`、`include/ime/CandidateUi.h` | **⬜ 先机验** |
| C | C3 删影子成员 | High | `include/ime/ITextService.h` | — |
| C | C4 强制更新位泄漏 | Medium | `src/ime/ImeManager.cpp` | — |
| D | D1 compartment 半成功 | High | `src/tsf/TsfCompartment.cpp`+`include/tsf/TsfCompartment.h` | — |
| D | D2 输入法不再被丢弃 | Blocker | `src/tsf/InputMethodManager.cpp` | — |
| D | D3 配置类型错不再吞 | Blocker | `src/configs/ConfigSerializer.cpp` | — |
| D | D4 保存失败可感知 | Blocker | `src/configs/ConfigSerializer.cpp`、`src/menu/ImeMenu.cpp`、`src/menu/ToolWindowMenu.cpp`、`include/ui/SettingsManager.h` | D3 |
| D | D5 便宜项（u8path / 6 语言文案 / ClearFocus 守卫） | Medium | `src/ImeWnd.cpp`、`contrib/Distribution/translate/*.toml`、`src/tsf/TextStore.cpp` | — |
| E | E1 CI 跑测试 | High | `.github/workflows/release.yml` | — |
| E | E2 测试自身缺陷 | Medium | `test/RandomUtils.h`、`test/settings_converter_test.cpp`、`test/resources/SimpleIME.toml` | — |
| X | X1–X3 证据不足，**本轮禁止实施** | — | 见 §5 | — |

并行建议：A（ImeApp.cpp 单文件）串行做完；B/C/D 分属不同文件可并行；D5 与 E 无冲突。**A1/A2 与 A4 都会碰 `ImeApp.cpp`/`ImeWnd`，注意别互相覆盖。**

---

## 3. 任务详情

### A1 — 正常退出从不拆除 IME 线程（Blocker）

**位置**：`src/ImeApp.cpp` `Uninitialize()`（`:288-325`）、`Shutdown()`（`:543-570`）

**已核实证据**（主 agent 逐行确认，非推测）：
- 全仓 `WM_QUIT` 的唯一生产者 = `PostThreadMessageW(imeThreadId, WM_QUIT, ...)` at `src/ImeApp.cpp:552`，位于 `Shutdown()` 内部。
- `Shutdown()` 的唯一调用点 = `src/ImeApp.cpp:362`，在 `D3DInit()` 的 catch 分支里（初始化失败路径）。
- 正常退出路径 = `MainWndProc` 的 `WM_NCDESTROY`（`:744-751`）→ `app.Uninitialize()`，**不经过 `Shutdown()`**。
- 闩位 `m_imeTeardownDone` 的生产点唯一：`:511`，在 IME worker 线程体里 `ImeWnd::Run()` 返回并 `DestroyWindow` 之后。
- 因此 `Uninitialize` 的 `:291 if (m_imeTeardownDone.load())` 在正常退出时恒为 false → 恒走 `:299-308` 的 "skipping ImGui destruction" 分支；`src/ImeWnd.cpp:983-1005` `OnDestroy`（task 排空 → `ImeController::Shutdown` → `UnInitialize` → TSF/COM 反初始化）在正常退出不执行。
- 线程已 `detach()`（`:539`），退出时由 `ExitProcess` 强杀，可能正停在某个 TIP DLL 内部。

**要改成什么**：
1. 把 `Shutdown()` 中"发线程 WM_QUIT + 有界等待闩"这一段（`:548-570` 内，含 `:552` 的 Post 与 `:561` 的 200 次轮询循环）原样抽成私有成员 `bool RequestImeThreadTeardown()`（声明加进 `include/ImeApp.h`）。
2. `Shutdown()` 改为：`LogStacktrace` → `SetState(SHUTDOWN)` → `RequestImeThreadTeardown()` → `Uninitialize()`（保持 `:569` 现有顺序语义）。
3. `Uninitialize()` **首行**调用 `RequestImeThreadTeardown()`；再加一次性闩（新增成员 `std::atomic_flag m_uninitializeStarted{false}`，`test_and_set` 命中即直接 return），防止 `Shutdown→Uninitialize` 与 `WM_NCDESTROY→Uninitialize` 二次进入重复 `ImGuiEx::M3::Destroy()`/`Shutdown()`（`:296-297`）。
4. `:299-308` 的 else 分支保留（超时仍需避免与晚到的 worker 拆除赛跑），但日志级别与措辞可保持。

**影响面**：仅 `ImeApp`。`m_imeThreadId` 已是 atomic（见 `include/ImeApp.h` 与 `:552` 用法）。

**可机验预测**：
- 改前：任一次正常退出游戏的 SKSE 日志尾部有 `IME thread teardown not confirmed; skipping ImGui destruction to avoid racing it.`（`:307`），且**没有** `Destroy IME Window`（`src/ImeWnd.cpp:985`）。
- 改后：应出现 `Destroy IME Window` + worker 侧的拆除日志，且不再出现 `:307` 那句（除非真的超时）。

---

### A2 — 失败/关闭态把 hook 拆除挡死（Blocker）

**位置**：`src/ImeApp.cpp:315-323`

**已核实证据**：
- 拆除块被 `if (m_state.IsInitialized())` 包住，而 `IsInitialized()` 是严格相等判断（`include/ImeApp.h:80`）。
- `Shutdown()` 在 `:546` 已把状态推进到 `SHUTDOWN`；`Uninitialize` 自己在 `:324` 推进到 `DORMANCY`；枚举序 `INITIALIZED=2 < INITIALIZE_FAILED=3 < SHUTDOWN=4 < DORMANCY=5`（`include/ImeApp.h:19-26`）。
- 于是：初始化超时（`:514`）或 `InstallHooks`/`RegisterMenu` 抛出后，`UninstallHooks()`（Scaleform present 钩子）与游戏窗口 `GWLP_WNDPROC` 还原（`:318-322`）**整块被跳过** → 本会话游戏窗口永久挂着 SimpleIME 的子类而 UI 全灭。

**要改成什么**：把状态门换成各资源自身判据 —— `UninstallHooks()` 前判它实际依赖的 hook 句柄（先读 `:588-600` 的实现，用其中的 present/scaleform hook 指针判空，不要凭猜测写符号名）；WndProc 还原保持 `if (RealWndProc != nullptr)` 并继续置空。**移除对 `IsInitialized()` 的依赖**，不要新增更宽的状态比较。

**可机验预测**：构造一次初始化失败（临时让 `ImeApp::Initialize` 抛异常）后，日志应同时出现 hook 卸载与 WndProc 还原相关记录；改前两者都不出现。

---

### A3 — 无重激活路径（High，行为面最大，可单独推迟）

**位置**：`include/ImeApp.h:19-26`、`:55-72`（`SetState` 的 CAS 循环）、`src/ImeApp.cpp:335-339`（`D3DInit` 的 `IsUnInitialized()` 门）

**问题**：状态机用枚举数值序表达"前进"，导致两个后果：(a) `INITIALIZED → INITIALIZE_FAILED` 被当作合法前进，失败可覆盖成功；(b) 一旦进入 `INITIALIZE_FAILED/SHUTDOWN/DORMANCY`，`D3DInit` 永久 bail，renderer 重建（ALT-TAB、设备重置）后没有任何恢复路径。

**要改成什么**：
1. `SetState` 改为显式合法迁移白名单（小函数或 `constexpr` 表），至少允许 `INITIALIZE_FAILED → INITIALIZING`、`SHUTDOWN → INITIALIZING`、`DORMANCY → INITIALIZING`；保留"禁止从 INITIALIZED 回退到 UNINITIALIZED"这类真实非法迁移。
2. `D3DInit` 的门从 `!IsUnInitialized()` 改为"仅 `INITIALIZING`/`INITIALIZED` 时跳过"。
3. `Uninitialize` 里 `SetState(DORMANCY)`（`:324`）与 A1 的一次性闩配合，保证二次进入不重复拆除。

**风险**：影响加载时序，**必须实机验收**（⬜）：加载失败一次 → 重新触发 D3DInit → 输入法应恢复。若时间紧，本任务可推迟到下一版，但 A1+A2 不含 A3 时"一次超时整场死亡"仍在。

---

### A4 — `SendNotifyMessageToIme` 把"送不到"伪装成成功（Medium）

**位置**：`src/ImeWnd.cpp:520-527`（`m_hWnd == nullptr` → `return true`）；声明 `include/ImeWnd.hpp:63`；调用方 `src/ImeApp.cpp:520`

**要改成什么**：返回 `false`（或引入三态枚举 `Delivered / NotDelivered / NoWindow`），并检查 `src/ImeApp.cpp:520` 超时路径对该返回值的语义依赖 —— 该处现在把 true 当"已投递，不必额外唤醒"，改后要走 warn 分支。

**另需在同一 commit 内注意（已核实但独立成条的问题）**：`src/ImeWnd.cpp:505-513`（`FocusTextService`/`ToggleKeyboard`）与 `include/ImeWnd.hpp:90,93,96`（`GetActiveLangProfile`/`GetLastTipProfileGuid`/`ActivateEnglishProfile`）无条件解引用 `m_textService` / `m_inputMethodManager`，而同文件 `:67-73`、`:109-112` 是判空的 —— 判空不一致说明"未初始化态可达"（`Draw` 的判空 `src/ImeWnd.cpp:562` 即作者承认）。**若要补齐守卫，另开一条 commit**，不要混进 A4。

---

### B1 — `REL::safe_write` 写失败被抹掉还报 Active（Blocker）

**位置**：`src/hooks/MeridianBridge.cpp:1094`（随后 `:1095-1097` 置 Active 并 info）；`src/hooks/NirnLabBridge.cpp:129-132` 与 `:355-366`

**已核实证据**：`extern/CommonLibSSE-NG/src/REL/Relocation.cpp:8-24` 的 void 重载 `safe_write` 只在末尾 `assert(success)`；NDEBUG 下 `VirtualProtect` 失败即静默 no-op，调用方无从得知。

**要改成什么**：
1. 三处写全部改用**带 expected-bytes 校验的 bool 重载**（`Relocation.h` 里的 `safe_write(dst, src, size, expected...)` 家族；先确认签名再用），或写完后**回读校验**目标字节。
2. 校验失败：`s_state = SupportState::Failed` + `logger::error`（带目标地址与槽位），**不得**再赋 `Active`、不得打 "focus observer installed"。
3. `SkseMenuFrameworkBridge.cpp:851` 直接写 `io[0x7B]` 属另一路径（数据段可写，风险较低），本轮只做"写后读回确认"，不改语义。

**可机验预测**：临时把回读校验改成必然失败（改后必须还原），观察状态落 `Failed` 且设置界面显示警告色 —— 证明"谎报 Active"的通路被切断。

---

### B2 — 折叠状态掩盖单路失败（High）

**位置**：`src/hooks/MeridianBridge.cpp:1100-1129`（`State()`：`:1113` "任一 Active 即 Active"）；消费方 `src/ui/ToolWindow.cpp:497` 附近

**问题**：View/1 后端 `Failed`（DLL 在但协商/写入失败）+ UIPlatform `Active` → 合并成 `Active`，玩家和技术支持看不到 `:1086` 的 warn。`include/hooks/SupportState.h:11-19` 的 per-backend 语义本身是对的，问题只在折叠函数。

**要改成什么**：保留 `State()` 作为"总体是否有能力"的判断，但 UI 侧改为**分列显示两路** `SupportState`（Meridian View/1、UIPlatform），任一路 `Failed`/`Standoff` 单独用 warning 色与 tooltip 说明；不要靠修改折叠优先级来"修"。

---

### B3 — UIPlatform pin 死路径（Blocker）

**位置**：`src/hooks/NirnLabBridge.cpp:150-164`（`UntrackBrowser`）、`:265-283`（`HookedReleaseBrowserHandle`）、文件头承诺 `:22-28`

**已核实证据**：`:158` 的 `if (externalRefs != 0 || pinHandle != 0) return nullptr;` —— 浏览器持焦时必然已被 `PinBrowser`（`:166-214`）钉住，故 `pinHandle != 0`；于是"mod 释放最后一个外部句柄且仍持焦"这一情形返回 `nullptr`，`:273-276` 直接 return，`:277-282` 的"结束会话 + 交还 pin"与 `:26-28`、`:277-279` 的注释承诺**对已 pin 浏览器不可达**。三个独立来源（code-reviewer B5、silent-failure B5、comment-analyzer B5）+ 主 agent 复核一致。

**要改成什么**：拆开两个计数 —— 当 `externalRefs == 0` 即返回 `browser`（表示"外部引用耗尽"），让上层照常 `OnBrowserFocusGone` + `UnpinBrowser`；`pinHandle` 的归还由 `UnpinBrowser` 负责，不要在 `UntrackBrowser` 里用它当"还活着"的判据。
必须同时确认：`UnpinBrowser` 内部释放自身句柄时走 `s_internalCall` 门（`:268-271`），避免 `HookedReleaseBrowserHandle` 递归再进 `UntrackBrowser`。

**⬜ 实机验收**：在 UIPlatform 页面持焦时让宿主 mod 释放句柄 → 日志出现 `:280` 那行、键盘交还游戏、无租约滞留；反复多次不涨内存。

---

### B4 — hook 安装失败静默 + 日志谎报（High）

**位置**：`src/hooks/WinHooks.cpp:13-18`；`include/hooks/Hooks.hpp:34-121`（`FunctionHook` ctor 吞 `DetourAttach` 结果，`Detoured()` 全仓无人查询）；`src/hooks/ScaleformHook.cpp:30-38,47-50`（派生 ctor 无条件 `logger::debug("Installed ...")`）

**要改成什么**：
1. `DetourFindFunction` 返回 null → `logger::error` 带目标符号名与模块。
2. `FunctionHook` ctor 检查 `DetourAttach` 返回值，失败 `logger::error`（带目标地址）；安装成功才打印 "Installed"。
3. `FunctionHook` 删除拷贝/移动（`:34-121` 无 Rule of Three，可拷贝会导致双 `DetourDetach`）。
4. `src/hooks/Hooks.cpp:52,96` 的 "Failed detour" 日志补目标名/地址。

---

### C1 — 候选提交假成功 + 返回值全程被丢（Blocker）

**位置**：`src/ime/Imm32TextService.cpp:199-213`；`include/ImeWnd.hpp:65-73`；`src/ime/ImeController.cpp:87-99`；UI 消费点 `src/ui/ImeWindow.cpp:173,249`、`src/hooks/MeridianBridge.cpp:643`

**已核实证据**（读过实现）：`:199` 注释自陈 "This method does not work as expected"；`:205-209` 在 `hImc == nullptr` 时保持 `result = true`；`:211` 对可能为 null 的 `hImc` 无条件 `ImmReleaseContext`（与 `ImmGetContext` 不配对）；返回值在 `include/ImeWnd.hpp:67-73` 被丢弃（函数返回 void），`ImeController` 亦为 void；失败仅一行 `logger::debug`，而默认日志级别是 info。

**要改成什么**：
1. `hImc == nullptr` → `logger::error` + `return false`；`ImmReleaseContext` 只在取得上下文成功后调用。
2. `include/ImeWnd.hpp:67-73` 的 `CommitCandidate` 改为 `[[nodiscard]] bool`；`ImeController::CommitCandidate`（`src/ime/ImeController.cpp:87-99`）返回 `Result`/`bool` 并在 `IImeModule::Result` 语义下区分失败。
3. UI/桥接侧对 false 给 `ErrorNotifier` 或至少 `logger::error`，不再静默。

---

### C2 — 页内索引 vs 全局索引（High，**先做机验再改码**）

**位置**：`src/ui/ImeWindow.cpp:171-174,247-250`（`clicked` 为页内下标）→ `src/ime/ImeController.cpp:91-98` → TSF `src/tsf/TextStore.cpp:960-967`（`SetSelection(index)`）/ IMM32 `src/ime/Imm32TextService.cpp:208`（`NI_SELECTCANDIDATESTR` 要全局索引）

**已核实证据**：`src/tsf/TextStore.cpp:1080-1084` 注释自证 "Selection is a global candidate index; the UI list is page-relative"；`grep FirstIndex` 全仓唯一读者是 `:1087` 的翻页判定，**提交路径无人使用**；`SetFirstIndex` 只在 TSF 侧 `:1125` 调用，`src/ime/Imm32TextService.cpp` 从不设置（`include/ime/CandidateUi.h:62` 的 `m_dwFirstIndex` 在 IMM32 恒为 0）。三个独立批次（code-reviewer B1/B2、type-design B2）指向同一处。

**实施前必须做的机验**（不许跳过）：游戏内把候选翻到第 2 页，点击第 1 个候选，记录上屏字；同时记录 `dwPageStart`/`dwCount` 与页 1 第 `pageStart+1` 项。据此判定：
- IMM32 侧：确认 `NI_SELECTCANDIDATESTR` 需要全局索引 → 在 `DoUpdateCandidateList`（`src/ime/Imm32TextService.cpp:388-414`）保存 `dwPageStart` 到 `CandidateUi`，提交时加偏移。
- TSF 侧：确认 `ITfCandidateListUIElementBehavior::SetSelection` 的索引语义（微软文档为全局），若实测一致则在 `TextStore::CommitCandidate` 前加 `FirstIndex()` 偏移。

**类型层面顺手收口**（同一 commit）：`include/ime/ITextService.h:126` 的 `CommitCandidate(DWORD)` 参数改为具名类型（`PageRelativeIndex` 或带 `enum class CandidateIndexKind` 的包装），避免第三处再混用。

---

### C3 — `Imm32TextService` 候选缓冲影子成员（High）

**位置**：`include/ime/ITextService.h:145`（基类 private `m_candidateUi`）与 `:218`（派生类同名 private 成员）

**已核实机制**：基类 `UpdateIfDirty:91` 把**基类**成员作为 `uiForRead` 传给 `RequestUpdate`；派生 `RequestUpdate:201` 的 `uiForRead.swap(m_candidateUi)` 中非限定名解析到**派生**成员。于是：渲染线程 swap 后基类成员拿到最新列表，派生成员拿到上一代；而 `SnapshotCompositionAndCandidates:185` 给游戏线程（`src/hooks/MeridianBridge.cpp` 的 Tick）读的正是派生成员 → **永远落后一代**。TSF 侧（`include/tsf/TextStore.h:371`）用的是赋值，两后端语义还不同。

**要改成什么**：删除 `:218` 的派生类重复声明，让派生实现使用基类成员（swap 目标写清楚），或彻底去掉"传引用进去 swap"的模式改为在锁内直接更新基类成员。**注意**：`Snapshot` 与 `RequestUpdate` 都在 `m_mutex` 下，改动不得放宽锁。

**⬜ 实机验收**：在 Meridian/CEF 输入框内选词或按 ESC 后，旧候选行不应继续显示。

---

### C4 — `m_isForceUpdate` 残留（Medium）

**位置**：`src/ime/ImeManager.cpp:107-111`（Prisma 抑制启用）、`:116-120`（`TryFocusIme` 失败）—— 两者都在 `:124-126` 消费该位之前 return。

**后果**：残留位让下一次任意状态同步无视当前状态强跑主体，包括 disable 分支的 `AbortIme` + `ActivateEnglishProfile` + `Focus(gameHwnd)`（`:196-244`）→ 模式莫名重置、组合串被清、焦点抖动。

**要改成什么**：两条提前 return 前 `m_isForceUpdate = false;`，或把消费点移到函数入口（进入即 `auto force = std::exchange(m_isForceUpdate, false)`，后续基于 `force` 判断）。选后者更不易漏。

---

### D1 — `TsfCompartment` 半初始化不可重试（High）

**位置**：`src/tsf/TsfCompartment.cpp:31-49`、`:68`；`include/tsf/TsfCompartment.h:27-37`

**已核实证据**：`:40` `GetCompartment` 成功后，若 `:43` 的 `CComQIPtr<ITfSource>` 为空，函数落到 `:49 return hresult` —— 而此刻 `hresult` 是 `GetCompartment` 的 `S_OK`，**返回成功却从未 Advise**；`m_guidCompartment` 已在 `:38` 置位，`:31-35` 于是把任何重试挡成 `S_FALSE "already initialized"`；`UnInitialize` 的 `:68` 只复位 guid，不清 `m_dwCookie`。另 `include/tsf/TsfCompartment.h:27-37` 的 callback 默认 `nullptr`，而 `OnChange:151` 无条件调用 → `bad_function_call` 穿越 COM 回调即 terminate（当前 6 个调用点都传了 lambda，属潜伏）。

**要改成什么**：Advise 未成功时回滚（`m_guidCompartment = GUID_NULL`、`m_tfCompartment.Release()`）并返回真实失败 hr；callback 参数去掉默认值改必传；`UnInitialize` 复位 `m_dwCookie`。

**后果对照**：不修则 Shift 中/英切换事件永不到达且无法自愈 —— 正是注释里反复出现的那一类"语言栏冻结"故障。

---

### D2 — 输入法被静默踢出列表（Blocker）

**位置**：`src/tsf/InputMethodManager.cpp:54-56`、`:68-71`、`:86-91`；丢弃点 `:195 if (!desc.empty())`

**已核实证据**：三处失败（`RegOpenKeyExW` 非 ERROR_SUCCESS、`SHLoadIndirectString` 失败、`GetLanguageProfileDescription` 失败）一律 `return {}` 且零日志；消费点只把非空 desc 的 profile 放进 `m_langProfiles`。

**要改成什么**：(a) 取显示名失败时**保留该 profile**，用回退名（`GetLocaleName(langid)` + clsid/guid 短形式），`logger::error` 记录 clsid/langid/guidProfile 与失败的 API；(b) 不要在 `:195` 用"显示名为空"当"不可用"的判据。

**后果对照**：不修则某输入法在游戏里凭空消失、切过去完全打不出字，且日志无痕。

---

### D3 — 配置键类型错被 `find_or` 吞掉（Blocker）

**位置**：`src/configs/ConfigSerializer.cpp:186-205`（`findAndSet` / `findAndSetFloat`）、`:206-255` 全部键读取

**已核实证据**：`extern/JamieMods/extern/toml/toml.hpp:15622-15650` 的 `find_or` 全部重载是 `noexcept` + `catch (...) { return opt; }` —— "键缺失"与"键存在但类型不符"结果相同。仓库已为 `zoom`/`theme_contrast_level` 打过 `findAndSetFloat` 补丁（`:191-195` 注释自认此坑），其余键裸奔。`ValidateConfiguration`（`:318-337`）只做整文件语法校验，对类型错返回 Ok，Advanced 面板的 `ConfigStatusRow`（`src/ui/ToolWindow.cpp:1004`）因此**误导**。

**要改成什么**：`findAndSet` 改为——键存在时按期望类型严格取（bool/int/float/string/array 各自 `is_xxx()` 判定），取不动就 `logger::warn("config key {} has unexpected type, ignoring user value")`，并把键名收集进"被忽略键"列表；键不存在才静默用默认。列表可在 Advanced 面板显示一行。**逐键改造量大，允许用一个小 helper 统一（不要写 20 份重复分支）**。

---

### D4 — 保存失败不可感知 + 违背自身契约（Blocker）

**位置**：`src/configs/ConfigSerializer.cpp:260-302`；`include/configs/ConfigSerializer.h:35`；调用方 `src/menu/ImeMenu.cpp:340`、`src/menu/ToolWindowMenu.cpp:56`、`include/ui/SettingsManager.h:32-36`

**已核实证据**：`SaveConfiguration` 返回 void，所有异常只落 `logger::error`，调用方无从感知；`:280-287` 在 rename 失败后回退 `open(filePath, trunc)` —— 若这步再失败，头文件承诺的"原文件保持完整"不成立，且 `SimpleIME.toml.tmp` 永不清理。

**要改成什么**：`SaveConfiguration` 返回 `bool`（或 `Result`）；回退直写失败 → `ErrorNotifier` 用户可见提示；失败路径清理 `.tmp`；三个调用方处理返回值（失败时不要把 UI 停在"已保存"的观感）。同步修正 `ConfigSerializer.h:35` 的注释使其与实际保证一致。

---

### D5 — 三项便宜修复（Medium，可拆成 3 个 commit）

1. **u8path**：`src/ImeWnd.cpp:616` 附近 `std::filesystem::path(fontPath)` → `std::filesystem::u8path(fontPath)`。理由：字体路径按 UTF-8 存（`src/configs/ConfigSerializer.cpp:147-149` + `extern/JamieMods/common/WCharUtils.h:11` 默认 CP_UTF8），而 MSVC 下 `path(std::string)` 按 ANSI 码页解码；同时 ImGui 侧 `extern/imgui/imgui.cpp:2499-2517` 把同一路径按 UTF-8→wide 打开 —— 两条管线对同一含中文路径判定相反，表现为 Meridian 候选面板字体与 ImGui 侧不一致。`translation_dir` 的消费方需同类检查（该键当前疑似无消费方，见 §5 X3）。
2. **6 语言补 `Settings.Advanced.Logging`**：`src/ui/ToolWindow.cpp:899` 取用该键，`grep -c Logging contrib/Distribution/translate/*.toml` 六个文件全为 0，缺失时按 `extern/JamieMods/common/i18n/translator_manager.h:62-70` 的回退原样显示裸键 → 任何语言打开 Advanced 页都看到 `Settings.Advanced.Logging`。
3. **de/ja/ko/ru 的 Prisma tooltip 与实现相反**：`translate_german.toml:111`（"tritt SimpleIME zurück"）、`translate_japanese.toml:72`（「SimpleIME が自動的に譲り」）、`translate_korean.toml:72`（"물러나"）、`translate_russian.toml:111`（«уступает клавиатуру»）仍在描述"让位"旧行为；`translate_english.toml:111` 与 `translate_chinese.toml:130` 已更新为"SimpleIME 接管文字输入，仅当 PrismaUI 公开 V1 接口不可用时才完全让位"。按 en/zh 语义重写这四种语言，并保留各自的"重启游戏生效"尾句。
4. （同批可选）`src/tsf/TextStore.cpp:119-137` 的 `ClearFocus` 补 `Focus()`（`:95-101`）同款 `m_threadMgr/m_hWnd/m_documentMgr` 判空守卫。

---

### E1 — CI 从不跑测试（High）

**已核实**：`.github/workflows/release.yml` 全文无 `ctest` / `BUILD_TESTING` / `node` 步骤（`grep` 仅命中 `windows-latest` 与 `make_latest`）；`CMakeLists.txt:7` `option(BUILD_TESTING "Build tests" OFF)`；`CMakePresets.json` 无 test preset。

**要改成什么**：加一个 push/PR 触发的 job：configure 带 `-DBUILD_TESTING=ON`，build `SimpleIMETest`，`ctest --output-on-failure`；可选再加 `node test/MeridianBridge.cjs`（该脚本目前只能手工跑，见 README）。不要把它塞进 release job 里，独立 job 失败不阻塞产物上传。**注意**：`run:` 步骤里任何 workflow-dispatch 来源的值必须经 `env:` 间接引用，不得直接 `${{ }}` 内插进 shell。

### E2 — 测试自身缺陷（Medium）

1. `test/RandomUtils.h:18` 用 `std::random_device` 且不记录种子 → 随机往返失败不可复现。改为可注入种子、失败时打印种子。
2. `test/settings_converter_test.cpp:210-215` 注释声称默认值在"三处"一致，实际只断言 `GetDefaultConfiguration` 与 `GetDefaultSettings` 两处；`contrib/config/SimpleIME.toml` 从未被解析校验（改 toml 忘改代码 = 新装用户拿到与代码不同的默认行为，无人拦截）。补一条用例：加载该 shipped toml 并与 `GetDefaultConfiguration()` 逐字段对比 —— 完全离线可行。
3. `test/resources/SimpleIME.toml` 是死夹具：`test.cmake:74-79` 把它拷到二进制旁，但无任何测试读取，且键名（`shortcut_key`/`fonts`）与现行 schema 不符。要么真正用于解析用例，要么删掉（连同 `test.cmake` 的拷贝规则）。
4. `test/TranslatorManagerTest.cpp` 依赖测试间全局态（前一用例尾部 `SetTranslator(nullptr)` 决定后一用例语义）且写 CWD：顺序敏感。至少加注释声明依赖，理想是各自 setup/teardown。

---

## 4. 复核状态标记

| 任务 | 主 agent 是否读过代码自证 |
|---|---|
| A1、A2、A4、B1、B3、C1、C3、C4、D1、D2、D3、D5(2,3)、E1 | ✅ 已逐行核实（含 grep 交叉验证） |
| A3、B2、B4、D4、E2 | 🧪 证据来自审计路的 file:line 摘录 + 我的局部核对，实施时请先读函数全貌 |
| C2 | ⬜ 机制核实（`FirstIndex` 无提交侧消费者已 grep 证明），但**索引语义需实机确认后才动手** |

## 5. 本轮禁止实施（证据不足，动了会引入新错）

- **X1 `ScaleformHook.cpp:43,167-169` 把 `Original()` 返回值当文本入口计数**：`extern/CommonLibSSE-NG/include/RE/C/ControlMap.h:86` 声明 `void AllowTextInput(bool)`，但项目镜像 `include/RE/ControlMap.h:60` 声明 `-> uint8_t`，且 `:40-56` 明确记录 SE/AE 计数器"end to end 实测一致"。反编译首遍常丢返回值，故上游 `void` 不足以定案。**正确动作**：加一次性对照日志（同处打 `result` 与 `GetTextEntryCount()`），拿到数据再决定。若日志显示不等，才成为独立工单。
- **X2 `SkseMenuFrameworkBridge.cpp:708,851` 的 `io+0xCC` 盲读 / `io[0x7B]` 盲写**：现工作正常，缺的是版本上界与值域校验（`:906` 门只设 `>=3.7`）。属加固，不属修 bug；实施时需同时处理 `include/hooks/SkseMenuFrameworkBridge.h` 的"detour 不移除"承诺。
- **X3 `translation_dir` 键是否死配置**：`ConfigSerializer.cpp:227` → `settings_converter.cpp:281` 读入后疑似无消费方（`src/ui/ImeOverlay.cpp:79`、`src/ui/panels/AppearancePanel.cpp:826` 疑似硬编码插件目录）。需先确认，再决定"接上消费方"或"删除该键并在 toml 注释里说明"。

## 6. 已撤回的误报（**不要按这些改**，防止别的 agent 重新发现又去修）

| 曾报缺陷 | 撤回依据 |
|---|---|
| "UTF-8 字体路径交给 ANSI `fopen`，中文用户名下打不开"（曾标 High） | `extern/imgui/imgui.cpp:2499-2517`：`ImFileOpen` 在 Win32 先 `MultiByteToWideChar(CP_UTF8)` 再 `_wfopen` |
| "渲染帧内 AddFont/RemoveFont 非法" | 本仓 imgui 1.92.7 已启用 `ImGuiBackendFlags_RendererHasTextures`（`extern/imgui/backends/imgui_impl_dx11.cpp:628`），`imgui.cpp:8936-8942` 仅在无该 flag 时锁图集 |
| "MergeFont 造成整份 TTF 缓冲永久驻留"（曾标 High） | `src/ui/fonts/Fonts.cpp:41` 的 `FontDataOwnedByAtlas = false` 是所有权移交，合并副本仍为 true，无双释/悬垂 |
| "`include/RE/ControlMap.h` 的 SE/AE 偏移是未知缺陷" | `:40-56` 已把差异写成显式注释并 `static_assert` 钉住；残留风险仅是 `IsSE()` 与上游版本门在 1.6.x 边界构建可能不同 → 降为 Medium 观察项 |
| "候选窗在高 DPI/副屏偏一半" | 三源坐标同为物理像素、钳制数学（`src/ui/ImeWindow.cpp:394-405`）本身正确，未发现确证 |
| "`FreeType` 句柄/`FT_Face` 泄漏" | 库级 `FT_New_Library/FT_Done_Library` 随 atlas LoaderInit/Shutdown 成对（`extern/imgui/misc/freetype/imgui_freetype.cpp:355,383-390`） |

## 7. 交付要求（每个实施 agent 回报时给全）

1. `git diff` 范围 + 每个 commit 的 hash 与信息。
2. `build_ime.cmd` 的 EXIT 码与新增 warning（仓库基线仅剩既有 `__uuidof` 与 vendored toml 两类 warning，不得新增）。
3. 若任务带可机验预测：给出日志行原文（改前/改后对照）。
4. 若任务标 ⬜：写明需要维护者实机确认的具体操作项（照 `docs/VALIDATION.md` 的 ✅/🧪/⬜ 记法列条目），不要自行标 ✅。
5. 不要修改本文件；如需增删工单，另开 `docs/` 新文件并说明分歧。
