# SimpleIME 修复进度 (2026-08-22)

> 用户报告 bug：**有时在 mod 界面输入完继续游戏时，点击键盘还是打字状态**
> （键盘输入被 IME 吞掉，无法正常操作游戏）
> 用户已批准"全改"，正在修复 6 个已报告的 bug。

## 任务概览

| # | 严重度 | 问题 | 状态 |
|---|--------|------|------|
| 1 | 🔴 | `AbortIme()` 只还焦点、不终止组合，IN_COMPOSING/IN_CAND_CHOOSING 残留 → 键盘被吞 | ✅ 已修复，构建通过 |
| 2 | 🔴 | `EnableIme(false)` 不归还 Win32 焦点（与 EnableIme(true) 不对称） | ✅ 已修复，构建通过 |
| 3 | 🟡 | `EnableMod(false)` 时 `keepImeOpen || enable` 反转禁用请求 | ✅ 已修复，构建通过 |
| 4 | 🟡 | `g_prevTextEntryCount` 初始为 0，若加载时已有输入框会漏掉首帧 | ✅ 已修复，构建通过 |
| 5 | 🟢 | `FixInconsistentTextEntryCount` 只覆盖 CursorMenu | ✅ 已修复，构建通过 |
| 6 | 🟢 | `DoSyncImeState()` 先清 dirty 再调 delegate，失败后状态不再重试 | ✅ 已修复，构建通过 |
| 7 | — | 构建验证（clang-cl + vcvars64） | ✅ 通过（EXIT 0，16/16） |

## 已完成（任务 1：AbortIme 真正终止组合）

### 1a. CM_ABORT_IME 消息定义 ✅
- `include/configs/CustomMessage.h`：enum 末尾加 `CM_ABORT_IME`。

### 1b. ITextService::AbortIme() 接口 + Imm32 实现 ✅
- `include/ime/ITextService.h`：ITextService 加虚方法 `virtual void AbortIme() {}`（默认空实现，保证不破坏其他继承者）；
  Imm32TextService 声明 `void AbortIme() override;`。**注意：此文件曾两次缩进被打乱，最后整文件重写修复，请 diff 确认无其他改动。**
- `src/ime/Imm32TextService.cpp`：实现 AbortIme —— 先锁内清空 TextEditor + 关闭候选窗（**不调用 OnEndCompositionCallback，避免把未完成的组合文本注入游戏**），再 `ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0)` 取消系统组合，最后清 IN_COMPOSING / IN_CAND_CHOOSING。

### 1c. TextStore::TerminateComposition() + TSF TextService::AbortIme() ✅
- `include/tsf/TextStore.h`：
  - TextStore 公开方法加 `[[nodiscard]] auto TerminateComposition() const -> HRESULT;`（在 ClearFocus 声明后）；
  - Tsf::TextService 声明 `void AbortIme() override;`（OnFocus 声明后）。
- `src/tsf/TextStore.cpp`：抽出 `TerminateComposition()`（m_context 非空时通过 `ITfContextOwnerCompositionServices::TerminateComposition(nullptr)` 终止组合），`ClearFocus()` 改为复用它。
- `src/tsf/TextService.cpp`：实现 `TextService::AbortIme()` —— 先 GetWriteLock 清空 TextEditor/关闭候选窗（**OnEndComposition 会回调 SendUiString，必须先清空避免注入**），再 `m_textStore->TerminateComposition()`，最后兜底清状态位。

### 1d. ImeWnd::AbortIme() → 发消息到 IME 线程 ✅
- `src/ImeWnd.cpp`：
  - `AbortIme()`（游戏 UI 线程调用）改为：若有 IN_CAND_CHOOSING/IN_COMPOSING，`SendNotifyMessageToIme(CM_ABORT_IME, 0, 0)`（在 IME 线程的 ImeWnd WndProc 中执行真正的组合终止，**不能直接从游戏线程碰 TSF 对象**），然后 `SetFocus(m_hWndParent)` 还焦点。
  - WndProc 加 `case CM_ABORT_IME:` → `pThis->m_textService->AbortIme(); return 0;`。
  - ⚠️ **此文件 CM_EXECUTE_TASK 附近缩进被 patch 打乱过两次，最后用 python 脚本按 LF 规范化替换修复。已重新读取确认缩进正确（361-375 行）。**

## 任务 2-6（原"未开始"清单）✅ 已在 cab770e 全部修复
- 任务2 EnableIme(false) 归还焦点 ✅ / 任务3 keepImeOpen 反转 ✅ / 任务4 g_prevTextEntryCount 基线 ✅ /
  任务5 FixInconsistentTextEntryCount 通用化 ✅ / 任务6 DoSyncImeState dirty 保留 ✅ / 任务7 构建验证 ✅

## 构建/工程注意事项
- 仓库当前：main 含 cab770e → b85a786 → 7817d6b → c17fde4（修复）→ 7d4f0e5（docs），全部已推送 myfork（lin-414/SimpleIME-Patched）。
- `include/atlcomcli_shim.h`（ATL shim，TextStore.cpp 在用，**不要删**）；`build_ime.cmd`（vcvars64 + cmake --build）已提交，构建走它。
- **patch 工具陷阱**：patch 会把 C++ 缩进打乱 / 单引号 char 字面量里的 \r \n 实体化成真实 CR/LF；修复后用 python 按行尾规范化重写（deep-normalize：先 `\r\n`→`\n` 再换回 `\r\n`，避免 `\r\r\n` 双重），改后 read_file 验证 + git diff 确认最小化。

## 构建验证结果 (2026-08-23) ✅ 全部通过
- 工具链验证：LLVM clang-cl ✅ / FontForge ✅ / vcvars64 ✅
- 增量构建：`cmake --build build\RelWithDebInfo-clangcl-ninja-vcpkg --config RelWithDebInfo` → **EXIT 0，16/16 目标完成**，SimpleIME.dll 链接成功（2026-08-23 08:59）。
- 仅剩警告：`atlcomcli_shim.h` 的 `__uuidof` 语言扩展警告（预期内，shim 设计如此）。
- 新构建脚本：`build_ime.cmd`（vcvars64 + cmake --build，可复用，尚未提交）。
- 待办/待问用户：① `Settings.h` 中 `autoToggleKeyboard` 结构体默认值 `false`（line 51）vs `GetDefaultSettings()` 中 `true`（line 91）不一致，需确认是否对齐；② 是否提交（含既有未提交 baseline diff + atlcomcli_shim.h）；③ `C：tmp_batbuild_out.log` 全角冒号杂散日志是否删除；④ 5 个 python 改过的文件带 BOM（utf-8-sig）是否去 BOM。
## 残留 bug 诊断与修复 (2026-08-23 第二轮) ✅ 已修复 b85a786

> 用户实测修复后的 DLL：**"退出mod窗口后依然被识别为输入状态"**（与最初症状同源）。

### 根因（报告后用户批准修改）
**Bug A 🔴（根因）**：`FixInconsistentTextEntryCount`（EventHandler.cpp:137）存在时序缺陷——
菜单**关闭事件派发时，正在关闭的菜单仍在 `menuStack` 上**（引擎先派发事件、后出栈）。
原逻辑遍历栈时遇到该非 always-open 菜单直接 `return`，导致漏检的
`AllowTextInput(true)` 计数永远不被清理 → `EnableIme(false)` 永不触发 →
IME 保持激活、焦点留在 0×0 ImeWnd → 游戏吞键。这正是"有时"的成因：
取决于第三方 mod 是否调用 `AllowTextInput(false)`。

**修复**：遍历时用 `ui->GetMenu(event->menuName).get()`（按名字拿菜单指针，
BSFixedString 可隐式转 string_view）跳过**正在关闭的那个菜单**，其余非
always-open 菜单仍在栈上才 return；全部通过则 `EnableIme(false)` + info 日志。

**Bug B 🟡（排查后撤销）**：曾怀疑 `EnableIme(false)` 不清
`INPUT_PROCESSOR_ACTIVATED` 导致 OnCharEvent 持续吞字符。复查确认**误诊**：
吞键闸门是 `!ImeDisabled()`，`EnableIme(false)` 本来就会置 `IME_DISABLED`；
而 `INPUT_PROCESSOR_ACTIVATED` 的恢复只依赖系统 profile 激活事件
（InputMethodManager.cpp:207/310），`EnableIme(true)` 不经过它——若清除，
下次启用输入法将直接失效。**已撤销**，不做此改动。

### 日志增强
- `ImeManager::EnableIme` 两条 debug → **info**（真实状态切换才打，短路不打）。
- `ImeWnd::AbortIme` 触发时加 **info**。
- `FixInconsistentTextEntryCount` 兜底触发时加 **info**。
→ 下次再出问题，游戏 log 直接可见 EnableIme/Abort 切换轨迹。

### 验证
- 构建：python subprocess build_ime.cmd → **EXIT 0**，DLL @ 11:16（hash 3e5f6da1）。
- dist/SimpleIME 已同步（cmake --install，hash 一致）。
- 提交 `b85a786` 已推送 myfork（8b6d981..b85a786）。
## 残留 bug 诊断与修复 (2026-08-23 第三轮) ✅ 已修复 c17fde4

> 用户实测第二轮 DLL 后补充关键症状：**"游戏内 IME 状态显示一直都有，有时按下键盘无反应，有时会弹出 IME 的候选框"**。
> 这些现象映射到三个残留机制，全部在本轮修复。

### 根因（报告后用户批准修改）

**Bug C 🔴（主因）**：`EnableIme(false)` 只做了"置 IME_DISABLED + 清 TSF 焦点 + 还
Win32 焦点"，**没有把系统输入法切走**。你系统激活的输入法是中文 TIP
（微信输入法/微软拼音），它跟随 Win32 焦点回到游戏窗口后重新激活 → 
按 WASD 弹候选框 / 吞键。`DoEnableMod(true)` 对游戏窗口摘除 IMM 上下文
的防护只在 mod 开关时执行一次，且 TSF 输入法不受其约束。

**Bug D 🟡（副因）**：`ImeMenu::OnKeyEvent` 吞键只看 `IN_COMPOSING`、不检查
`IME_DISABLED`。IMM32 路径（enableTsf=false）`OnFocus(false)` 只解除关联、
不终止组合，`IN_COMPOSING` 残留 → 组合状态永吞键，表现为"按键无反应"。

**Bug E 🟢（显现）**：`INPUT_PROCESSOR_ACTIVATED` 反映"系统输入法激活"，
一直为 true → 调试窗/状态显示里输入状态灯常亮。

### 修复
1. **`ImeManager::EnableIme` 状态机 + 系统输入法联动**（ImeManager.cpp）：
   - **disable 分支**：先记住当前激活的中文输入法 profile（
     `GetActiveLangProfile()`），再 `ActivateLanguageProfile(DEFAULT_LANG_PROFILE)` 
     切到美式键盘。`TF_IPPMF_FORSESSION` 只生效于当前会话，不碰系统全局输入法；
     触发 `InputMethodManager::OnActivated` 回调后自动
     `Clear(IN_COMPOSING/IN_CAND_CHOOSING)` + `INPUT_PROCESSOR_ACTIVATED=false` ——
     **候选框不再弹、吞键解除、状态灯熄灭，一次全解**。
   - **enable 分支**：`FocusTextService(true)` 后恢复记住的中文输入法，输入框内照常中文打字。
   - `ImeManager.h` 新增 `GUID m_lastActiveProfile` 缓存；`ImeWnd.hpp` 新增
     `GetActiveLangProfile()` 访问器。
2. **`ImeMenu::OnKeyEvent` 加 `IME_DISABLED` 门控**（ImeMenu.cpp）：仅当
   `!state.ImeDisabled() && state.IsImeInputting()` 才 `kHandled`，禁用后永不吞键。

### 验证
- 构建：python subprocess build_ime.cmd → **EXIT 0**（12/12 relink），DLL @ 12:14（hash 6eb99f69）。
- dist/SimpleIME 已同步（cmake --install，hash 一致）。
- 提交 `c17fde4` 已推送 myfork（7817d6b..c17fde4），远程已验证。

### a296845 增补修复（用真实 profile 而非桩值）
- **问题**：c17fde4 用 `ActivateProfile(DEFAULT_LANG_PROFILE)` 切美式键盘，但 `DEFAULT_LANG_PROFILE` 的
  `CLSID_NULL`/`GUID_NULL` 是桩值，不匹配系统 TSF 中实际注册的美式键盘 profile。TSF API 可能静默失败，
  中文输入法（微信拼音/微软拼音）保持激活，退出 mod 窗口后仍弹候选框。
- **用户新症状**：在 mod 窗口输入框打字 → 按 ESC 退出 → IME 状态灯依然亮 + 按键盘弹候选框。
  需要先点击输入框以外区域让 IME 状态消失，再退出才能正常操作。
- **根因**：`ITfInputProcessorProfiles::ActivateProfile` 用 `CLSID_NULL`/`GUID_NULL` 激活不存在的 profile，
  返回错误但被静默忽略（之前只打 warning 日志，不影响主流程——但切输入法确实失败了）。
- **修复**：`InputMethodManager::ActivateKeyboardEng()` 遍历 `m_langProfiles`（系统 TSF 枚举加载的列表），
  按 `langid == 0x409` 找到实际美式键盘 profile（含真实 CLSID 和 GUID），调用 `ActivateProfile` 激活它。
  找不到时回退到 `DEFAULT_LANG_PROFILE` 并打 warning（兼容旧系统）。
- **文件改动**（4 文件 +27/-1）：`InputMethodManager.h` 声明 `ActivateKeyboardEng()`；
  `InputMethodManager.cpp` 实现遍历+激活逻辑；`ImeWnd.hpp` 添加转发方法；
  `ImeManager.cpp` 调用 `ActivateEnglishProfile()` 替代 `ActivateLanguageProfile(DEFAULT_LANG_PROFILE.guidProfile)`。
- **构建**：EXIT 0, [10/10], DLL `321ca4d9...`（4,290,560 B），dist 同步。
- **提交** `a296845` 已推送 myfork（d9937b7..a296845）。

### 25cec80 最终修复（用真实 profile 类型而非硬编码 INPUTPROCESSOR）
- **问题**：a296845 找到了真实美式键盘 profile（langid==0x409, 真实 CLSID/GUID），但
  `ActivateProfile(const LangProfile&)` 硬编码了 `TF_PROFILETYPE_INPUTPROCESSOR`（1）。
  美式键盘在系统 TSF 枚举中是 `TF_PROFILETYPE_KEYBOARDLAYOUT` 类型，类型不匹配导致
  `ITfInputProcessorProfileMgr::ActivateProfile` 失败，中文输入法保持激活。
- **用户症状**：ESC 退出 mod 窗口后 IME 状态灯仍亮、按键盘弹候选框——与 a296845 之前完全相同。
- **根因**：`LangProfile` 结构体没有保存 `dwProfileType` 字段，枚举时丢弃了该信息，
  `ActivateProfile` 只能猜 `INPUTPROCESSOR`。
- **修复**（2 文件 +5/-3）：`LangProfile` 加 `DWORD dwProfileType{}` 字段；
  `RefreshProfiles()` 枚举时保存 `profile.dwProfileType`；
  `ActivateProfile(const LangProfile&)` 用 `langProfile.dwProfileType` 替代硬编码。
  `DEFAULT_LANG_PROFILE` 设 `1`（TF_PROFILETYPE_INPUTPROCESSOR，兼容中文输入法通路）。
- **构建**：EXIT 0, [11/11], DLL `720699e8...`（4,290,560 B），dist 同步。
- **提交** `25cec80` 已推送 myfork（ecb7919..25cec80）。

### 42dfdc2 最终修复（传递 HKL 而非 nullptr）
- **问题**：25cec80 修复了 `dwProfileType` 硬编码，但 `ActivateProfile` 仍传 `nullptr` 给 `hkl` 参数。
  `ITfInputProcessorProfileMgr::ActivateProfile` 对 `TF_PROFILETYPE_KEYBOARDLAYOUT` 类型需要传入实际的
  **键盘布局句柄（HKL）**，而非 nullptr。传入 nullptr 让 API 静默接受请求但不实际切换输入法。
- **根因**：`LangProfile` 没有保存 `hkl` 字段。枚举时 `TF_INPUTPROCESSORPROFILE` 结构体包含 `hkl`
  （美式键盘有真实 HKL 句柄，中文 TIP 的 hkl 为 NULL），但枚举代码没保存它。
- **修复**（2 文件 +6/-3）：`LangProfile` 加 `HKL hkl{}` 字段；`RefreshProfiles()` 保存
  `profile.hkl`；`ActivateProfile(const LangProfile&)` 传 `langProfile.hkl` 替代 `nullptr`。
- **构建**：EXIT 0, [11/11], DLL `daf6a8b8...`（4,290,560 B），dist 同步。
- **提交** `42dfdc2` 已推送 myfork（361d2e7..42dfdc2）。

## 第四轮：代码审查 + "ESC 后仍在 IME 状态" 最终修复 (2026-08-28) ✅ 用户确认解决

> 42dfdc2 之后用户仍报告"退出 mod 界面后还在 IME 状态/按键盘弹候选框/语言栏面板滞留"。
> 本轮先做全面代码审查（8 项发现），再经 5 次构建/测试迭代逐层定位，最终确认**两个叠加根因**。

### 审查修复（提交于本轮 fix 提交）
- 🔴 **撤销 `INPUT_PROCESSOR_ACTIVATED` 语义收紧**：该标志门控 ImeWnd `WM_CHAR` 转发与 ImeMenu
  `OnCharEvent` 吞事件——收紧为"仅 INPUTPROCESSOR"会让英文键盘用户完全无法输入。恢复原语义，
  "状态灯常亮"改在 `DrawImeStates` 显示端按 `dwProfileType` 判断。
- `ForceEnglishKeyboardOnGameThread` 重写：`AttachThreadInput` 不会让 `ActivateKeyboardLayout`
  作用到其它线程 → 改 `PostMessage(WM_INPUTLANGCHANGEREQUEST)`（语言栏的正规做法）。
- `ActivateProfile` "已激活即短路"仅对 INPUTPROCESSOR 生效；键盘布局按 `hkl` 匹配缓存索引；
  `GUID_NULL` 激活请求路由到 `ActivateKeyboardEng()`；英文匹配改 `PRIMARYLANGID`（en-GB 等）；
  `DEFAULT_LANG_PROFILE` 降级为纯显示桩、不再是激活目标。
- `ImeWnd::AbortIme` 线程感知（仅游戏线程 `SetFocus`）；Scaleform 防抖 250ms→50ms 且
  `SyncImeStateIfDirty` 移到门控后（否则经 IME 线程绕过防抖）；错乱缩进全部 clang-format。

### 迭代 1-2：布局看门狗 + overlay 汇合点
- **布局漂移**：OS/输入法会在激活变化时重新套用窗口记住的中文输入法（日志实测 disable 22 秒后
  漂移）。新增 `WM_INPUTLANGCHANGE` 看门狗（MainWndProc）：mod 启用且 IME 禁用时布局漂移回
  非英文 → 立即重新请求英文。
- **面板滞留**：overlay 显示/隐藏请求原在 `ImeController::DoEnableIme`，而 SyncImeState 等
  路径绕过它 → 收拢到 `ImeManager::EnableIme` 唯一汇合点（任何 disable 必发隐藏请求）。
- 图钉按钮修复：原实现固定后点 PIN_OFF 图标仍设 `pinned=true`，永远无法解钉 → 改为切换。

### 迭代 3：游戏线程 TSF 激活（已撤销）
- 曾加 `ActivateKeyboardEngOnCurrentThread()` 试图在游戏线程 COM 单元激活英文 profile——
  **游戏主线程 COM 是 MTA**，`CoInitializeEx(STA)` 永远失败 `0x80010106`。且用户确认候选框是
  SimpleIME 自绘（`ImeWindow::Draw` 门控 `!ImeDisabled() && IsImeInputting()`）→ 组合发生在
  IME 线程，游戏线程 TIP 层并非可见症状根因。整体移除，保留 HKL 看门狗。

### 迭代 4-5：两个叠加根因 ✅
1. **文本框计数泄漏**（日志实锤：`No real menu left on the stack but text-entry count is
   still > 0`）：mod 菜单 `AllowTextInput(true)` 无配对 false，且泄漏可能落在菜单关闭事件
   **之后**（事件时计数为 0，旧检查跳过）→ 无后续事件纠正，IME+面板永久卡启用态。
   修复：`HealLeakedTextEntryCount()`（计数归零 + `Hooks::Scaleform::ResetTextEntryCountCache()`
   同步 hook 缓存，否则后续 0→1 检测失效）；事件版修复治愈计数；新增每帧轮询
   `PollTextEntryCountConsistency`（ImeMenu::PostDisplay 调用）：计数>0 + 稳定 2 秒 grace
   （避开 ShowMenu 排队入栈竞态）+ 无真实菜单 → 治愈 + 强制关 IME。
2. **自持循环**：面板显示期间自家 `ToolWindowMenu`（非 AlwaysOpen）常驻菜单栈，被"真实菜单"
   判定误认为计数拥有者 → 修复机制恰好被卡住状态自身蒙蔽（只有 F2 能临时藏面板）。
   修复：`HasRealMenuOnStack` 与事件版修复均排除 `ToolWindowMenuName`（它从不拥有文本框计数，
   其文本输入走 ImeWnd）。

### 验证 (2026-08-28 23:26 会话) ✅
- DLL `547bfab9...`（4,306,432 B），构建 EXIT 0，dist/ 与 C:\Program Files (x86)\SimpleIME 同步一致。
- 日志签名：泄漏两次发生均被即时捕获（`healing counter and forcing IME off`）、面板自动隐藏、
  重新启用时微信输入法立即恢复（`m_lastTipProfileGuid` 跟踪，不受英文键盘缓存覆盖影响）、
  布局漂移被看门狗纠正。**用户确认问题解决。**

### 工程注意事项（新增）
- 游戏主线程 COM 为 MTA——任何"在游戏线程用 TSF/STA COM"的方案都不可行，勿重试。
- dist 同步方式：`cp build/.../SimpleIME.dll dist/SimpleIME/SKSE/Plugins/`（cmake --install
  前缀是 C:\Program Files (x86)\SimpleIME，不是 dist/）。
- 用户 MO2（E:\Skyrim AE）装有两个 SimpleIME 文件夹，启用的是 `SimpleIME`（`中文输入 SimpleIME`
  已禁用，enable_mod=false，勿混淆其配置）。

## 第五轮：焦点抢占加固 + P0 健壮性审计修复 (2026-08-29/30)

> 第四轮后两块工作：①修复遗留的 WM_KILLFOCUS 竞态崩溃 FIXME（faa7bc8）；②全面健壮性审计
> （Explore 双代理）后按用户选定范围实施 P0 崩溃/UAF/挂死类修复（97f6567 + 05b174f2）。

### 焦点抢占加固 (faa7bc8)
原 FIXME 崩溃（组合中被 Win+Shift+S 抢焦点）实为三个并发缺陷：IME 线程上调
`ImGui::ClearInputKeys` 与渲染线程 ImGui 帧竞态（改原子标志延后）；被终止组合在焦点切换
竞态窗口内注入半截文本（TSF/IMM32 注入前校验 `GetFocus()==窗口`）；组合终止权交由 TIP
在混乱中自行拆解（改为 WM_KILLFOCUS 时 IME 线程主动 AbortIme，先清编辑器使后续回调拿空串）。

### P0 审计修复 (97f6567 + 05b174f2)
1. **ErrorNotifier 无锁 deque**（子模块 fork lin-414/JamieMods@b6c74e7，.gitmodules 改指向）：
   IME 线程写入 vs 渲染线程迭代擦除 → UAF。全量互斥量保护；拷贝构造删除；
   settings_converter 改引用（拷贝单例静默吞错误）。
2. **TSF sink 无锁写入**：组合/候选 UI sink 在文档锁外触发，与渲染线程 RequestUpdate
   拷贝竞态。`GetSinkWriteLock(m_fLocked)` 条件锁——sink 在 OnLockGranted 内触发时
   RequestLock 已持锁，重入非递归锁会死锁。
3. **任务比 Shutdown 活得久**：8 个 lambda 执行时重查 IsReady；OnDestroy 排空队列；
   Shutdown 先置标志；AddTask 先拷贝 HWND。
4. **有序 IME 线程关闭**：WM_QUIT 投递到线程（原发窗口被 WndProc 忽略，循环永不退出）；
   worker 循环退出后本线程 DestroyWindow；Start 的 promise 换 shared_ptr（超时 UAF）；
   ~ImeWnd 不再跨线程拆 COM；TsfSupport 仅平衡自己初始化的 COM。
5. 快速项：shortcut 解析死循环（启动挂死）；OnStartComposition AddRef 泄漏；
   主题 RGB G/B 通道错用 R + string_view 读陈旧缓冲。

### 自查发现并修复 (05b174f2)
- DoUpdateUIElement 持锁调用 `m_currentCandidateUi->Abort()` → Abort 同步重入
  UpdateUIElement sink → 非递归锁自我死锁。两个 Abort 延迟到锁外；重构时误删的
  GetCandInfo 失败路径已恢复。
- PostThreadMessageW 使 IME 线程真正自拆后，D3DInit 失败路径上与游戏线程
  Uninitialize 并发（ImGui/TSF 非线程安全）→ worker 完成后置 m_imeTeardownDone，
  Shutdown 有界等待（2s）。
- .zcode 会话产物误入提交 → 移除 + .gitignore。

### 工程注意事项（新增）
- **发版顺序**：先升 CMakeLists VERSION 再构建产物（v2.2.2 的 zip 内 DLL 内嵌版本仍是
  2.2.1 的教训）；发布用 `gh release create <tag> --notes-file <notes> <zip>`（tag push
  不触发 workflow，原因未查明；dispatch 可手动触发但 FontForge 修复后未端到端验证过）。
- JamieMods 子模块现为用户 fork（fix/error-notifier-thread-safety 分支），上游更新时需
  在 GitHub 上同步 fork。

## 第六轮：独立审计复查 + 三批修复 (2026-09-18)

> 用户要求"通读代码，是否还有 bug、有什么需要改进"。两轮 Explore 代理全面审计后
> 逐条在源码中核实，排除误报，分三批实施。

### 核实后排除的误报（避免后续重复调查）
- `FontManager.cpp:137` `GetString(index, data(), length+1)`：DWrite 写 `length` 字符 +
  终止符，覆盖 `wstring` 自身的 NUL 是合法的，不是溢出。
- `InputMethodManager::RefreshProfiles` 的 `hresult = TRUE`：两条 throw 路径都已先赋过
  失败 HRESULT，不会误报成功。
- `HealLeakedTextEntryCount` 的 while：计数是 `uint8_t`，最多 255 次，有界。
- `GetActiveLangProfile` 跨线程读：有越界检查并返回 DEFAULT，安全。

### 第一批：确认缺陷（含一个上轮自引入的回归）
1. **`m_fWantClearInput` 无消费者**（回归）：faa7bc8 把 `ClearInputKeys()` 从 WM_KILLFOCUS
   移走改为延后标志，但消费者从未写出（注释和成员文档都声称有）。本 mod 不转发窗口
   消息给 `ImGui_ImplWin32_WndProcHandler`，改由 ImeMenu 手工喂 GFx 键事件，因此失焦时
   按住的键永远收不到 key-up —— ImGui 会一直认为 Ctrl 按着。已在 `Draw()` 消费。
   注：不能用 `AddFocusEvent(false)` 代替——本 mod 从不投递 focus-gained，ImGui 会永久
   停在 AppFocusLost 状态。
2. `ToStringFromGUID2` 恒返回空串：`reserve()` 只改 capacity 不改 size，
   `StringFromGUID2(guid, data(), size())` 的第三参数是 0 → 什么都不写。改用
   `MAX_GUID_CHAR_SIZE` 定长构造并按返回长度 resize。
3. `GetCandInfo` 用 TIP 给的 `currentPage` 直接索引页表（越界读）+ `selection - pageStart`
   无符号下溢。加边界校验（越界返回 E_FAIL 走已有 Abort 路径）与钳制。
4. `UnlockDocument` 解引用 `pTextStoreAcpSink` 前未判空（`RequestLock` 判了）——加空检查。
5. **测试目标已无法配置**：`test.cmake` 引用不存在的 `src/i18n/translator_manager.cpp`
   （translator 早已移入 JamieMods），include 目录 `${CMAKE_SOURCE_DIR}/common|imguiex`
   也不存在，`TranslatorManagerTest.cpp` 用的是子模块化前的 `Ime::i18n`/`Ime::Translate`
   API。全部修正后 16 个测试通过。CI 不跑测试，所以一直没暴露。
6. `autoToggleKeyboard` 三处默认值不一致（`configuration.h`=false、`Settings.h`=true、
   随包 TOML=false）。实际生效的是 TOML，故以 false 为准统一；README 里"aligned to true"
   的描述是错的，已改正。
7. `plugin.cpp` 的 `ErrorHandler` 返回 `EXCEPTION_CONTINUE_SEARCH` → `__except` 块不可达，
   它只是日志钩子不是崩溃保护。加注释澄清（保留原行为）。

### 第二批：并发加固
8. `ImeWnd` 拆除竞态：`m_imeOverlay`/`m_textService`/`m_inputMethodManager` 由 IME 线程
   `UnInitialize()` 释放，渲染线程 `Draw()` 解引用。`ImeApp` 状态要到 `Uninitialize()`
   末尾才离开 INITIALIZED，期间 `Draw()` 仍会进入。新增原子 `m_fTearingDown`，在释放
   任何成员前置位，`Draw()` 据此提前返回。
9. `m_uiScale`/`m_fWantUpdateUiScale` 原子化（IME 线程写、渲染线程读），标志用
   `exchange` 消费以免帧内到达的 DPI 变更丢失。
10. `RuntimeData` 的 `requestShowOverlay`/`requestHideOverlay` 原子化——这是真正跨
    IME→渲染线程的两个字段；因原子不可拷贝，补了显式拷贝构造/赋值（`Settings` 加载
    时会拷贝），并注明其余字段的线程归属。

### 第三批：死代码清理
删除：`TsfMessageLoop`（已标 deprecated 但仍在编译；其 KB4564002 论证已移到 `Run()`
的文档注释，避免丢失这段来之不易的知识）、`SendMessageToIme`、主线程任务队列 +
游戏窗口的 `CM_EXECUTE_TASK` 分支（无生产者）、`WinHooks::DisablePaste`、`m_fSShow`、
`CandWindowProp`、`MAX_COMPOSITIONS`、未使用的 `CM_CHAR`/`CM_IME_CHAR`、
`Skyrim::ToggleMenu`、残留的 `<shared_mutex>` include。
注意：`CM_IME_COMPOSITION` 实际被 `Imm32TextService` 使用，保留；枚举值显式标注为
`WM_CUSTOM + 3` 以免删除后移位。`ITextService.h`/`TextStore.h` 原本靠 `CandidateUi.h`/
`TextEditor.h` 传递包含 `<shared_mutex>`，已改为各自显式包含。

### 验证
- 插件构建 EXIT 0（clang-cl，`-Wpedantic -Wshadow -Wconversion` 全开，仅剩既有的
  `__uuidof` / vendored toml 警告）；`SimpleIMETest` 16/16 通过。
- 产物同步：`dist/SimpleIME/SKSE/Plugins/` 与 MO2 的 `E:\Skyrim AE\mods\SimpleIME\`
  （原 Program Files 安装点已不存在），三者 hash 一致 `0c22a19f...`（4,307,968 B）；
  MO2 旧 DLL 备份为 `SimpleIME.dll.bak-20260918`。

### 工程注意事项（新增）
- **测试目标需要 FontForge**：`build-lucide-icon.cmake` 里 `find_program(... REQUIRED)`，
  全新 configure 时若 fontforge 不在 PATH 会直接失败。本机路径
  `C:/Program Files/FontForgeBuilds/bin/fontforge.exe`，configure 时用
  `-DFONTFORGE_EXECUTABLE=` 显式传入即可。
- 测试单独 configure 到 `build/test-check`（`-DBUILD_TESTING=ON`），不污染主构建目录。
- 审计时务必核实再报：本轮两轮代理共报出 4 处误报（见上），直接采信会改坏正常代码。

### 发版 v2.3.1 (2026-09-18)
- 按既有教训先升 `CMakeLists.txt` VERSION 到 2.3.1，再 configure/build/package，确认
  DLL 内嵌 `FileVersion`/`ProductVersion` 均为 2.3.1（3,662,848 B）后才打包。
- 本地产物：`dist/SimpleIME-2.3.1-Release.7z`（1,245,040 B），内容已核对
  （DLL + interface/ + README + LICENSE + third_party）。
- **tag push 依然不触发 workflow**：查 `gh api .../actions/runs` 确认该仓库历史上只有
  `workflow_dispatch` 记录，从未有过 tag 触发（含 v2.3.0）。workflow 本身 `state=active`
  且 `on: push: tags: v*` 配置正确，Actions 权限 `enabled: true`。原因仍未查明；
  本次改用 `gh workflow run Release --ref v2.3.1` 手动 dispatch 发版。
- 本地已构建的 7z 作为兜底：若 workflow 再次失败，直接用 `gh release create` 上传该产物。

## 第 7 轮审查修复 (2026-09-25)

> 四路并行深审（TSF/钩子/UI/配置）+ 人工复核全部 P1，共 40+ 处。构建 EXIT 0，测试 16/16。

### P1（崩溃/卡死类，4 项）
1. **`ImeApp::Start()` 从不 `initialized.get()`**：`wait_for` ready 后直接 detach，
   worker 异常（如注册窗口类失败）静默丢失 → 状态照样置 INITIALIZED、`m_textService`
   为 null → 首帧 Draw 崩溃。修复：ready 后 `get()` 让异常走 D3DInit 的 catch；worker
   的 catch 区分 `future_error`（set_value 后才失败，如 CreateHost）改为记日志。
2. **`TextStore::GetCandidateInterface` 的 `ATLENSURE_SUCCEEDED` 抛异常穿透 COM 回调**：
   GetUIElement 拿 stale id 时抛 CAtlException，穿透 TSF 非 C++ 帧且消息泵无 catch →
   进程终止；调用点的 SUCCEEDED/FAILED 分支成死代码。修复：改 `return hresult`。
3. **`D3DInit` 早退分支跳过 `Original()`**：write_call 挂钩的替代函数在
   "Already Initialized" 路径直接 return，渲染器重建触发二次调用时游戏自身 D3D 初始化
   被吞（黑屏）且 WndProc 自环。修复：`Original()` 提到状态检查之前。
4. **设置窗口开着时固定语言栏 → `toolWindowShowing` 悬空**：`m_toolWindow.reset()` 分支
   不清标志 → ImeMenu 吞掉全部 Scaleform 输入 + MenuMode 上下文滞留（只能按 F2 解围）。
   修复：reset 时同步 `toolWindowShowing = false`。

### P1 伴随加固
- `ImeWnd::OnDestroy` 用 `m_fTearingDown.exchange` 做一次性拆毁门闩；
  `ImeApp::Uninitialize` 在 `m_imeTeardownDone==false`（超时路径）时跳过 ImGui 拆毁，
  消除双线程并发拆毁竞争（ImGui 泄漏到进程退出，可接受）。
- `OnD3DInit` 末尾补调幂等的 `InstallEventSinks()`，封死 kInputLoaded 先于 D3DInit 到达
  导致 sink 整个会话失效的启动竞态。
- `ImeApp::State::SetState` 改 CAS 循环，消除 check-then-exchange 竞争。
- `WM_NCDESTROY` 改为 Uninitialize 后直接转发给游戏原 WndProc（原先吞掉）。
- `ImeWnd::Draw` 增加成员判空兜底；消息泵 10ms 轮询改 `WaitMessage` 阻塞等待（空闲不再
  100Hz 空转唤醒）；`WM_SETTINGCHANGE` 落回 DefWindowProc。

### P2（行为缺陷）
- **`ImeController::EnableMod` TOCTOU**：去重判断移入任务体（执行时判定）；两次 enable
  不再覆盖保存的 `m_gameHIMC`（原 HIMC 永久丢失、系统输入法失效）。`m_imeWnd` 改
  `std::atomic<ImeWnd*>`。
- **Imm32 路径**：候选高亮补页偏移换算（对齐 TSF 路径）；IMN_SETCANDIDATEPOS/OPENCANDIDATE
  与 OnComposition 错误路径补 `ImmReleaseContext`；WM_DESTROY 置空 `m_hIMC`；
  OnEndComposition/AbortIme 补 MarkDirty(Composition|CandidateList)。
- **`InputMethodManager`**：`hresult=TRUE`→`E_FAIL`；catch 补 `catch(...)`；VARIANT 零初始化
  且用 `lVal`（原来 boolVal 写 2 字节、高 2 字节栈垃圾）。
- **TsfCompartment**：未初始化时 SetValue 返回 E_FAIL 而非假 S_OK；QI 显式接口 cast；
  Release 下溢保护。TsfSupport 用独立 `m_comInitialized` 平衡部分失败路径的 CoUninitialize。
- **TextStore 规格**：GetTextExt 空区间维持 E_INVALIDARG（SDK 无 TS_E_INVALIDARG）并新增
  越界 TS_E_INVALIDPOS；GetScreenExt 零初始化+判空；GetText 无缓冲时如实报 0、
  runInfoBufferCopied 判空；selection-only 且无锁时立即 MarkDirty（原来滞留到下次锁）；
  InitSinks 用嵌套 static_cast 取 COM 身份；Release 下溢保护；删 `[in]` MIDL 残留与 iostream。
- **UI**：ThemeBuilder 打开时补 `m_configuredContrastLevel` 播种（原点 Apply 静默清零对比度）；
  Zoom 下拉首次 Draw 从 settings 同步；字体预览按 DWrite 索引查找（原当 vector 下标，
  无名字体后错位）；NOT_SUPPORTED_FONTS 不再整体隐藏（先画状态栏）；搜索零命中显示空列表
  （`m_filterActive`）；AddFontFromFileTTF 失败报 NOT_SUPPORTED；语言栏 pinned 去掉
  NoInputs（图钉/设置按钮才点得到）；FontManager 字体枚举进程级缓存（原每次开窗渲染线程
  全量枚举）；CandidateUi::CandidateList 返回 const 引用 + empty() const + Close() 清索引；
  ImeWindow 删死代码 CloseCurrentPopup；ToolWindowMenu kUserEvent 判空；Fonts 表 PushID。

### 配置层（三个"静默失败"一并消除）
- log level 接受文档承诺的 `"error"` 与 spdlog 的 `"warning"` 拼写；config→settings 方向
  对 zoom/contrast 越界 clamp+告警；toml 整数形式的 `zoom = 1`/`contrast = 1` 可解析
  （findAndSetFloat）；SaveConfiguration 改临时文件+rename 原子替换；F1-F12 过期注释修正；
  `<limits>` 补 include。
- 测试：3 处用 `GetDefaultConfiguration()` 初始化（原 UB）；suite 名 typo；
  新增 error/warning 别名断言；zoom 期望改 clamp 语义。16/16 通过。

### 杂项
- FakeDirectInputDevice：Release 清空静态单例指针（防悬挂）；QI 对 IID_IUnknown/
  自身接口返回包装器（COM 身份）；WinHooks 校验 `riidltf==IID_IDirectInput8A` 否则放行；
  Hooks.hpp requires 改为真约束（`is_function_v`，原恒真）；enumeration.h/DebounceTimer
  死代码；InputFocusAnchor hScroll 同步修 right；ControlMap 补 SE/AE 偏移陷阱文档 +
  kTotal/offsetof static_assert（实测布局一致）；挂钩安装点补单线程安装不变量注释；
  CMake 补 `/utf-8`（防 cl 生成器把 BOM-less 中文按 ACP 误读）。

### 复核后有意不改
- `WM_IME_SETCONTEXT` 继续以 lParam=0 走 DefWindowProc（抑制 OS 候选窗是有意行为，
  转发给游戏 WndProc 会让 OS 候选窗复活）。
- `keepImeOpen`/`autoToggleLanguageBar` 保持普通 bool：Settings 拷贝机制为 atomic 已有
  特例化，为两个对齐 bool 引入原子化收益趋零（x86 不撕裂 + 读端容忍一帧陈旧），改为在
  Settings.h 写明线程归属与理由。
- 挂钩构造即生效 vs unique_ptr 赋值的空窗：维持现状（启动期主线程安装，不可达并发），
  已加注释说明不变量。

### 验证
- 构建 EXIT 0（clang-cl，全部警告仅剩既有 `__uuidof`/vendored toml 两类）；
  SimpleIMETest 16/16。
- 产物同步：build / `dist/SimpleIME/SKSE/Plugins/` / MO2 `E:\Skyrim AE\mods\SimpleIME\`
  三处 md5 一致 `9668b3c5...`（4,318,720 B）。待游戏内回归：
  ① IME 线程空闲 CPU 占用应下降；② 设置窗口开着按图钉/固定后 ESC、输入均应正常；
  ③ Theme Builder 改色后 Apply 对比度不丢；④ 字体预览选择与添加的字体一致；⑤ 配置
  `level="error"`、`zoom=1` 生效。

## 第 8 轮:Meridian UI 输入支持 + Prisma UI 避让模式 (2026-10-01)

> 参考 BlackMesa79/Skyrim-Text-Bridge(MIT)的实现方案,给 SimpleIME 接入 Meridian(CEF)输入支持,
> 并对 Prisma UI(自带原生输入)实现只读避让。SDK 头文件 vendored 至 extern/MeridianUI、extern/PrismaUI。

### 新增模块
- `src/hooks/MeridianBridge.cpp` + `include/hooks/MeridianBridge.h`(`Hooks::MeridianBridge`):
  - kDataLoaded 时 `GetModuleHandleW(L"MeridianUI.dll")` → 公开导出 `QueryMeridianExtension` 协商
    `Meridian.View/1`;失败仅告警,不影响其它功能。
  - 只 hook 公开接口虚表 **slot 9(TryFocus)**(Text-Bridge 同款单槽,最小化布局假设暴露面);
    Granted/AlreadyFocused 时记录 focusedView 并触发 `ImeController::SyncImeState()`。
    **不 hook Unfocus**:失焦/视图销毁由每帧 `Tick()` 的 `IsReady+HasFocus` 兜底覆盖。
  - 文字路径:IME 线程 `SendUiString` 在 `ShouldRoute()`(有 Meridian 焦点)时改入 `QueueText`
    (互斥队列,UTF-16 上限 8192);游戏线程 `Tick()`(ImeMenu::PostDisplay)先注入桥接 JS +
    `capture(id)`(仅当有待发文本,250ms 节流),captureReady 后逐块(1024)commit。
  - 会话协议:递增 sequence 作为会话 ID;CEF 线程 listener 回执 `id:status` 前缀校验;
    `inserted` 清 in-flight;stale-field/rejected 把 in-flight 块**头部回插**重投(at-least-once);
    no-field 告警一次;连续 4 次 capture 无回执 → 判定 listener 失联(view 句柄复用)强制重注册。
  - 失焦即清 pending(不残留到下一会话);JS 文本全量 `\uXXXX` 转义(不可能成为可执行代码)。
- `src/hooks/PrismaBridge.cpp` + `include/hooks/PrismaBridge.h`(`Hooks::PrismaBridge`):
  - kPostPostLoad 时探测 PrismaUI.dll → 只读协商公开 `IVPrismaUI1`,仅用 `HasAnyActiveFocus()`;
    **不在窗口过程中调用**(可能等 Ultralight),只在 PostDisplay 每帧节流(500ms)刷新缓存。
  - 失败即 fail-safe:Prisma 在场但 V1 不可用 → 永久视为 Prisma 持有键盘(IME 不启用)。
  - 注册 `PrismaUI.ImeAssociation` 消息:MainWndProc 收到 → latch + 立即 `SyncImeState()` 让位;
    之后某帧 Refresh 观察到无活动焦点且 latch 在 → 再 SyncImeState 恢复(keepImeOpen 用户无感)。

### 接线点
- `ImeManager::IsShouldEnableIme()`:`keepImeOpen || HasTextEntry() || (meridianSupport && MeridianBridge::HasFocus())`
  —— Meridian 焦点即文本输入目标,所有 SyncImeState 路径自动覆盖启用/禁用。
- `ImeManager::EnableIme()` 顶部:enable 且 `PrismaBridge::OwnsInput()` → 抑制并返回 SUCCESS
  (唯一汇合点,文本框钩子/Meridian 焦点/keepImeOpen 全覆盖;不动 m_isForceUpdate,让 Prisma
  让位后的下次 sync 重新求值)。
- `ImeMenu::PostDisplay`:`PrismaBridge::Refresh()` + `MeridianBridge::Tick()`(游戏线程每帧)。
- `ImeApp` messaging:kPostPostLoad → PrismaBridge::Install;kDataLoaded → MeridianBridge::Install;
  Uninitialize → 两桥 Uninstall(虚表 detour 不拆除,退化为直通)。

### 配置(默认全开)
- `[input] meridian_support = true` / `prisma_avoidance = true`,贯通 configuration →
  ConfigSerializer(带中文注释)→ settings_converter(双向)→ Settings;随包 SimpleIME.toml 已加。

### 已知边界(待游戏内验证)
- Meridian 焦点期间文字全部经桥提交;若 Meridian 自身把按键事件合成为文本(而非仅靠 char 事件),
  组合期间可能出现双写——届时再加 `DispatchInputEventHookData`(hook.h 已备好 67315/68617+0x7B)
  按 Text-Bridge 语义抑制。虚表布局假设与 Text-Bridge 相同(MSVC x64 View/1)。
- Esc/编辑键在组合期间不经分发钩子抑制,依赖 Meridian 自身 PauseGame 焦点语义。

### 构建环境重大坑(本机,新发现)
- **`cmake -E cmake_llvm_rc`(clang-cl + RC 的官方包装)在本机死锁**:clang-cl 预处理后向 SDK
  rc.exe 传参的管道链挂死(rc.exe 0 CPU 等待 stdin)。任何触发 `version.rc.res` 重编的构建都会
  卡住(reconfigure 改了 CMakeLists / version 变更都会触发)。临时绕过(仅存在于 build 目录,
  reconfigure 后会失效,需重做):
  1. `build/RelWithDebInfo-clangcl-ninja-vcpkg/CMakeFiles/simple_rc.bat`:本地批处理替代
     cmake_llvm_rc(clang-cl -E 预处理 → findstr 剥离 # 行标记与 clang 注入的 `extern "C" {`
     包装与裸 `}` 行(否则 rc.exe 把它们解析成 EXTERN 资源,链接时报 duplicate resource)→ rc.exe);
  2. `CMakeFiles/rules.ninja` 中 RC_COMPILER__SimpleIME_unscanned_RelWithDebInfo 规则的
     `command` 替换为 `CMakeFiles\simple_rc.bat $in $out`,并删除 `depfile`/`deps = gcc` 两行;
  3. 若 ninja 报 "stored deps info out of date"/"deps are missing",可 `rm .ninja_deps` 后重试。
- **test-check 测试目录已重建**:原目录是 VS 生成器 + VS 自带 vcpkg 工具链(路径已失效),
  缓存污染后 ninja/VS 反复冲突。已按 presets 复刻环境重新 configure(VCPKG_ROOT 指向
  C:/Users/Administrator/vcpkg,`--preset RelWithDebInfo-clangcl-ninja-vcpkg -B build/test-check
  -DBUILD_TESTING=ON -DFONTFORGE...`,首次需装 gtest/benchmark,较慢)。
- 本轮产物:SimpleIME.dll 4,347,904 B(md5 122f79c0...),内嵌版本 2.3.2,已同步 dist/ 与
  MO2 `E:\Skyrim AE\mods\SimpleIME\`;随包 toml 已加两个新开关(注意:MO2 处的 SimpleIME.toml
  被同步覆盖,若用户曾手改过缩放/主题等需重新设置)。
- 测试:test-check 重建后 SimpleIMETest **16/16 通过**(新增 meridianSupport/prismaAvoidance
  默认值 true 与双向转换断言,含 GetDefaultConfiguration/GetDefaultSettings/TOML 三处一致性)。
- 虚表 hook 与 Meridian 的实际交互、JS 桥 capture/commit 回执、Prisma 避让触发,均待游戏内实测
  (需安装 Meridian UI 框架或 Tailor / Outfit Wheeler)。

## 第 9 轮:Text-Bridge P0/P1 借鉴落地 (2026-10-01)

> 按用户选定范围实施上一轮分析的 P0 三项 + P1 三项。

### P0
1. **in-flight 提交回执超时**(借 SessionGate 期限纪律):`s_inFlight`/`s_inFlightMs` 纳入
   `s_pendingMutex` 保护(**顺带修复一处真实竞态**:CEF 线程 OnListenerPayload 与游戏线程
   FlushPending 曾无锁并发读写 std::u16string);`inserted` 回执 2 秒未达 → 丢弃该块、强制重捕获
   (Text-Bridge 同款保守语义:可能已落地的块不重投,防重复上屏),队列永不因丢回执而卡死。
2. **与 Skyrim-Text-Bridge 互斥**:`MeridianBridge::Install` 检测 `SkyrimTextBridge.dll`,
   在则 Meridian 通道保持关闭(双方 hook 同一虚表槽、会双注入;对方已有反向检查)。Prisma 避让不受影响。
3. **ABI 单测**(借 MeridianAbiCheck 思路):`include/hooks/MeridianApi.h` 抽出可测的
   `RequestMeridianView` 协商 helper;`test/MeridianAbiTest.cpp` 用按发布头文件实现的 Mock 取自身
   vtable 断言 slot 9 = TryFocus(MSVC 兼容布局),并覆盖协商接受/拒绝/空查询分支。

### P1
4. **Node 直测内嵌桥脚本**(`test/MeridianBridge.cjs`):indexOf 按 C++ 原始字符串语义提取
   BridgeScript(正则方案误截断,弃用),vm + 手搓 DOM mock 全量移植 Text-Bridge 的 MeridianTests.cjs
   断言(注入攻击样例、过期会话/字段、只读/密码、cancel/blur/iframe、execCommand 失败、幂等重注入)。
   **该测试立刻抓到发布级 bug:BridgeScript 原始字符串把 `)STBJS"` 终止符与 IIFE 收尾 `)` 合并在
   同一行,DLL 内嵌的 JS 被截断成 `(function(){...})(`,浏览器侧根本无法定义桥对象——已把终止符
   独立成行修复(与 Text-Bridge generated.h 的写法一致)。**
5. **桥逻辑与 SKSE 解耦**(`include/hooks/MeridianBridgeLogic.h`):纯函数
   `ParseListenerPayload`(载荷分类)+ `JsString`(\uXXXX 转义)进头文件;`MeridianBridge.cpp`
   改为消费方;`test/MeridianBridgeLogicTest.cpp` 覆盖全部状态分类、陈旧会话忽略、
   引号/反斜杠/换行/U+2028/U+2029/代理对转义。
6. **`docs/VALIDATION.md` 发布验证清单**:仿 Text-Bridge 的 VALIDATION-<ver>.md,含离线/游戏内
   两级清单、明确的未验证项与本轮产物 hash,发版前逐项核对。

### 测试与产物
- test.cmake 补 `${EXTERN_DIR}` include(ABI 测试需要 vendored MeridianUI 头)。
- SimpleIMETest **26/16→26 全过**(新增 10 项:4 ABI/协商 + 6 逻辑/转义);`node test/MeridianBridge.cjs` 通过。
- DLL md5 `bba6401e…`,build/dist/MO2 三处一致,内嵌版本 2.3.2。

## 第 10 轮:Meridian 实测诊断——"无候选框/无法中文"根因与修复 (2026-10-01)

> 用户实测反馈:Meridian 界面点输入框无 IME 候选框、无法中文;Skyrim-Text-Bridge 可以。
> 依据游戏日志(D:\Documents\My Games\...\SKSE\SimpleIME.log,61 行)逐环核对。

### 日志实锤的链路状态
- ✅ Prisma V1 协商成功;✅ Meridian View/1 协商 + slot9 观察安装(无互斥误触发);
- ✅ 10:57:24 "view 3 gained focus" → 43ms 后 "IME enabled" —— 我们的 Meridian 焦点→启用链路端到端工作;
- 🔴 但 IME 线程活动 TIP 是 **美式键盘**:启动期(10:52:20)WM_NCACTIVATE 链的 disable 发生在用户
  首次输入前,`m_lastActiveProfile`/`lastTipGuid` 均为 GUID_NULL,后续每次 enable 都走
  "无历史记录→静默保持当前(英文)"分支 → 用户打键全是英文字符,无组合、无候选框。
  日志里 微信↔美式 反复切换即用户手动 Win+Space 拉锯(10:57:32 起)。
- ℹ️ 10:57:48 disable / 10:57:55 enable = Shift+Tab Steam 覆盖层开关(ConsoleNativeUIMenu),
  非本路径缺陷;此时 lastTipGuid 已捕获微信(用户手动切过),恢复反而正常。
- ⚠️ 该次实测运行的还是第 8 轮 DLL——内嵌桥 JS 因原始字符串截断而残缺(第 9 轮已修),
  即使组合发生,提交也不会到达 DOM。两个问题叠加,症状相同。

### 修复
- `InputMethodManager::ActivatePreferredImeProfile()`:从已枚举 profile 中激活用户第一个
  非英文 INPUTPROCESSOR TIP(微信/微软拼音等);`ImeWnd` 转发;`ImeManager::EnableIme(true)`
  的"无历史记录"分支改为调用它(原先仅 debug 日志后静默保持当前)。
- 语义边界:仅当本会话从未捕获过用户 TIP 时才回退;用户显式切换过的 TIP 仍由原
  save/restore 机制优先。英文系统(无非英文 TIP)保持原行为。

### 验证与产物
- 构建 EXIT 0(主/测试两目录);SimpleIMETest 26/26;Node 桥测试通过。
- DLL md5 `e9fae88e…`,build/dist/MO2 三处一致。**待用户用新 DLL 重测**(清单见 docs/VALIDATION.md;
  若仍有问题,把生效配置 interface/SimpleIME/SimpleIME.toml 的 level 改为 "debug" 复现一次取全链路日志)。

## 第 11 轮:点击抢焦点——Meridian 无法输入的真正根因 (2026-10-01)

> 用户用第 10 轮 DLL(e9fae88e)重测仍无候选框。日志证明:TIP 回退正常
> ("activating the user's IME '微信输入法'"),IME 启用正常——但用户 11:53:59/11:54:01 仍在
> Win+Space 拉锯,11:53:44/11:54:13 两次无 gained-focus 的重复启用(WM_NCACTIVATE = Alt-Tab 往返)。

### 根因:点击把 Win32 焦点从 ImeWnd 抢到游戏窗口
- 组合只能发生在 ImeWnd(TSF 文档焦点 + Win32 焦点都在 IME 线程);游戏线程被
  DoEnableMod 故意摘除 IME 上下文(ImmAssociateContext(gameHwnd,nullptr)),在游戏窗口上永无组合。
- Meridian 是点击驱动 UI:打开菜单(IME 启用,焦点在 ImeWnd)→ 用户点击输入框 →
  WM_MOUSEACTIVATE/DefWindowProc 把焦点给游戏窗口 → 键全部走游戏线程(英文 HKL、无 HIMC)
  → 组合不可能发生。控制台流程不用点击,所以从未暴露此雷。
- 日志佐证:用户 Win+Space 的 TIP 切换作用在**前台线程(游戏)**上(系统的 profile 激活广播),
  说明用户的键入上下文一直是游戏窗口;IME 线程虽已激活微信,但它收不到任何键。

### 修复:游戏窗口获得焦点即要回(SimpleIME 侧)
- `MainWndProc` 的 `WM_IME_SETCONTEXT(wParam=TRUE)`(= 游戏窗口刚获得 Win32 焦点)在
  Meridian 会话期间投递自定义消息 `SimpleIME.ReclaimImeFocus.v1`(RegisterWindowMessageW),
  处理时经 `ImeManager::Focus`(AttachThreadInput 跨线程)把焦点还回 ImeWnd。
  鼠标事件按位置派发,不受键盘焦点影响,Meridian 的鼠标交互零损失;文本通道(WM_CHAR→ImeWnd)
  反而因此恢复。
- 守卫 `ShouldReclaimImeFocus()`:mod 启用 && IME 未禁用 && MeridianBridge::HasFocus() &&
  !Prisma::OwnsInput()——仅 Meridian 会话生效,Steam 覆盖层/Alt-Tab(焦点不落在游戏窗口)不触发。
- 去抖:`EnableIme(true)` 的回退分支仅在当前活动 profile 非 INPUTPROCESSOR 时才激活首选 TIP,
  消除每次强制 sync 重复激活微信的 churn。

### 产物
- 构建 EXIT 0;SimpleIMETest 26/26;Node 桥测试通过;DLL md5 `797f0753…` 三处一致。
- 待重测:打开 Meridian 界面 → 点输入框 → 直接打拼音(无需再手动切输入法)→ 候选框弹出 →
  上屏文字进入 Meridian 输入框。若仍失败,把生效配置(interface/SimpleIME/SimpleIME.toml)
  的 level 改 "debug" 复现一次取全链路日志。

## 第 12 轮:debug 日志实证 + 观测点补全 (2026-10-01)

> 用户用 797f0753 重测仍失败;debug 级日志(log 288 行)给出三个硬事实:
> 1. **组合从未启动**——面板出现标志行("Candidate window became visible",info 级)全程未出现,
>    即 IN_COMPOSING 从未置位(OnStartComposition 是唯一来源;原日志为 trace/编译期剔除,不可见);
> 2. **SendUiString 全程 0 次调用**——连英文直通的 WM_CHAR 都没到过;
> 3. **焦点回收从未触发**——12:17:04/12:17:13 两次 "IME window lost focus" 后无紧跟 get focus。
>    根因:回收触发器挂在游戏窗口的 WM_IME_SETCONTEXT 上,而游戏窗口的 IME 上下文已被
>    DoEnableMod 摘除(无 HIMC → 永远收不到该消息)→ **上一轮的回收是死代码**。
> 另:12:16:59 美式键盘激活可能是微信收到 Shift 后的 EN 切换(键确实到达过输入法一侧)。

### 本轮改动
- **焦点回收改挂真信号**:移入 `ImeWnd::WndProc` 的 `WM_KILLFOCUS`(每次焦点被夺必然触发),
  延迟一跳(PostMessage 自定义消息 `SimpleIME.ReclaimImeFocus.v2`)、处理时重查全部守卫
  (Meridian 持焦 + IME 未禁用 + 非 Prisma + 前台仍是游戏窗口——Alt-Tab/覆盖层不误伤),
  同线程 `SetFocus` 直接要回;附带 info 日志。删除 MainWndProc 的死触发器与 ShouldReclaimImeFocus。
- **观测点补全(全部 debug 级)**:ImeWnd `WM_KEYDOWN`(vk 值)、`WM_CHAR` 门槛三条件与放行、
  TextStore `OnStartComposition/OnUpdateComposition/OnEndComposition`(启动/提交/未提交原因)。
- 构建 EXIT 0;26/26;Node 通过;DLL md5 `e50b8677…` 三处一致。

### 待用户双实验(同一次游戏会话)
- A:Meridian 复现(开界面 → 点输入框 → 打拼音)。日志将揭示:键是否到达 ImeWnd、
  组合是否启动、回收是否触发。
- B(对照):控制台(`键)打拼音,看 SimpleIME 候选框是否出现——拆分"Meridian 特有"
  与"会话级回归"两个假设空间。

## 第 13 轮:真正的根因——Meridian 会话期间引擎停止调用 ImeMenu::PostDisplay (2026-10-01)

> 控制实验结果:控制台中文全链路正常(组合、面板 "Candidate window became visible"、
> "committing 1 characters");Meridian 会话里组合也正常启动并更新 10-13 次,但面板从未出现、
> Tick 从未运行、两次组合均以 "committing 0 characters" 结束(用户盲打后取消,编辑器被清空)。

### 根因
SimpleIME 的每帧工作全部由 `ImeMenu::PostDisplay` 驱动(ImGui NewFrame/Render、候选面板、
`MeridianBridge::Tick`、`PrismaBridge::Refresh`)。**引擎在 Meridian 视图持有焦点期间不再调用它**
(Meridian 经自己的 CEF 路径渲染画面)。于是:无 ImGui 帧 → 面板不可见;Tick 不跑 → 不捕获、
不提交;用户在完全无反馈的状态下打字,只能取消组合(0 字符提交 + 连串退格)。
铁证:IN_COMPOSING 已置位(OnStartComposition 日志)而面板出现标志行(info)一条没有。

### 修复:Present 钩子作为 Meridian 会话的备用帧驱动
- 启用 hook.h 中闲置的 `D3DPresentHookData`(75461/77226+0x9,交换链 Present——Meridian 渲染
  也走它,每帧必然执行),安装于 `ImeApp::InstallHooks`。
- `ImeApp::PresentHook`:Meridian 会话激活且 PostDisplay 帧停摆(帧令牌 >250ms)时,以
  `g_ImGuiFrameMutex` 与 PostDisplay 路径互斥,驱动完整 ImGui 帧(NewFrame → ImeWnd::Draw →
  Render)→ 面板/候选/语言栏在 Meridian 会话中恢复显示;**无论是否接管,每帧调用
  `MeridianBridge::Tick()`**(捕获+提交)。
- `ImeApp::Draw()` 同样加锁并刷新帧令牌;两驱动互斥,正常游戏路径行为不变。
- 卸载语义:Present 钩子不可安全拆卸,保留至进程结束(持有 trampoline 的 Original),退役后
  各分支自然惰化(桥 Uninstall + 状态门控);**严禁 reset g_PresentHook(丢失 Original = 黑屏)**。
- 线程说明:Present 钩子运行于引擎呈现线程;`ImeWnd::Draw` 本就为跨线程调用设计
  (m_fTearingDown 门闩 + 服务锁),与既有 UI 线程调用同级;ImGui 帧由互斥锁串行化。
- 日志:首次接管打一条 info("...present hook is now driving the frames"),不逐帧刷屏。

### 产物
- 构建 EXIT 0;26/26;Node 通过;DLL md5 `917baa62…` 三处一致。
- 待重测:Meridian 界面点输入框直接打拼音 → 候选框应出现(Present 驱动)→ 上屏文字进入输入框。

## 第 14 轮:提交链路只剩最后一环——组合文本不进编辑器 (2026-10-01)

> 用户用 917baa62 重测。日志证明此前所有修复全部生效:Present 接管触发(1 次)、面板出现(2 次)、
> 组合启动(2 次,9-15 次真实更新)、**桥捕获回执闭环**(session 1 → ready,79ms)、英文直通进
> Meridian 字段(WM_CHAR 's'/'a' 转发)。剩余断点:**两次组合均 "committing 0 characters"——
> 编辑器里没有组合文本**。
> 关键回查:上一轮控制台的 15 次更新长组合同样 "committing 0"(用户连按 9 次退格),只有一次
> 3 更新的短组合提交了 1 字符——**该问题今天在控制台同样存在,非 Meridian 特有**;用户描述的
> "控制台可以"来自短组合的成功或既往经验。
> 疑点:微信 TIP 的组合文本写入(编辑会话 → SetText/InsertTextAtSelection)被拒绝或未发生;
> TSF 规则是文档有未完成锁时同步编辑会话直接 TS_E_SYNCHRONOUS 失败——渲染/UI 侧的读锁请求
> 若在组合期间持续存在即可造成此现象,且与长短组合的成功率差异吻合。

### 本轮改动(纯观测点,debug 级)
- `TextStore::SetText`(cch,在锁检查**之前**,连 TS_E_NOLOCK 拒绝也可见)、
  `InsertTextAtSelection`(count + 首字符码点)、`OnEndComposition`(提交的实际内容,截 16 字符)、
  `RequestLock`(SYNC 被拒/异步排队深度/授予,三路全覆盖)。
- 构建 EXIT 0;26/26;Node 通过;DLL md5 `e23a39dc…` 三处一致。
- 待重测:Meridian 与控制台各打一个完整拼音词并空格上屏;同时需要用户描述面板内容
  (空白框?有拼音无候选?有候选选不了?)——区分"微信不写文本"与"写文本被拒"两条修复路线。

### 打包 v2.4.0 (2026-10-01)
- 版本升 2.3.2 → **2.4.0**(先升 CMakeLists 再构建,DLL 内嵌 FileVersion/ProductVersion 已核对)。
- reconfigure 还原了 rules.ninja → 按第 8 轮记录重新应用 simple_rc.bat 补丁,version.rc 顺利重编(2.4.0)。
- 产物:`dist/SimpleIME-2.4.0-RelWithDebInfo.7z`(1,457,612 B),CPack 全组件(DLL + interface/ + README +
  LICENSE + third_party 含 Meridian/Prisma 许可证)。包内 DLL md5 `3698e28d` 与构建产物逐字节一致、
  内嵌 2.4.0、interface toml 含 meridian_support/prisma_avoidance 两键。
- 代码与 26/26 测试通过的 e23a39dc 完全一致(仅版本资源不同);dist/ 与 MO2 已同步新 DLL。
- 未打 tag/未发 release;发布时记得 tag push 不触发 CI,用 `gh workflow run Release --ref v2.4.0`
  或本地 `gh release create v2.4.0 --notes-file <notes> <zip>`。

## 第 15 轮:拼音泄漏进 DOM + 候选框被 Meridian 压住 (2026-10-01)

> 用户用 2.4.0 实测:上屏链路已通("ailisi"+空格 → "爱丽丝" 进入字段),但
> ① 输入框同步显示拼音 "ailisi",空格后变成 "ailisi 爱丽丝"(拼音残留在字段里,CEF 拼写检查红线);
> ② SimpleIME 候选框显示在 Meridian 界面**下层**(透过其 backdrop 模糊可见、闪烁)。

### 根因
- **拼音泄漏**:ImeWnd 的普通 PeekMessage 循环里 `TranslateMessage` 先于 IME(DefWindowProc/CUAS)
  消费按键,组合期间每个拼音键(及上屏空格)仍生成 WM_CHAR;第 14 轮日志已证实
  "英文直通进 Meridian 字段(WM_CHAR 's'/'a' 转发)"。vanilla Scaleform 菜单由
  `ImeMenu::OnCharEvent`(TIP 激活时 kHandled)掩盖同一泄漏,Meridian 路径无此拦截 →
  WM_CHAR → SendUiString → QueueText → insertText 逐字母进 DOM,空格 WM_CHAR 再补一个空格。
- **候选框被压**:`D3DPresentHookData` 是游戏 present 函数内的 call hook,ImGui 候选窗口
  画进交换链;Meridian 的 CEF 纹理在更下游合成(其 CSS backdrop 模糊了候选框)——
  同一交换链内绘制顺序不可赢,与 hook 安装顺序博弈不可靠。

### 本轮改动
- **WM_CHAR 组合回声抑制**(ImeWnd.cpp):`HasAny(IN_COMPOSING, IN_CAND_CHOOSING)` 时直接丢弃
  WM_CHAR(组合中的字符属于 IME,上屏文本由 OnEndComposition 回调交付)。英文直通(无组合)不受影响。
- **Meridian DOM 内候选悬浮层**:BridgeScript 新增 `ui(comp,cands,sel)` —— 在捕获字段的文档里
  建 position:fixed 面板(z-index 最大值),显示组合串(下划线)+ 候选 chips(1./2. 编号,
  高亮选中,可点击);点击经 `<id>:pick:<n>` 回执。Commit 后推送空状态隐藏面板。
- **pick 回执协议**(MeridianBridgeLogic):`TryParsePickPayload`(≤9 位纯数字校验)+ 单测;
  OnListenerPayload 先于通用状态解析匹配 pick(否则会被当 Failed 触发无谓重捕获),
  经 `ImeController::CommitCandidate`(AddTask → IME 线程)提交。
- **推送通道**(MeridianBridge.cpp):`PushCandidateOverlay` —— 每帧 Tick 快照
  (`ITextService::SnapshotCompositionAndCandidates`,服务锁下拷贝,区别于渲染线程的
  UpdateIfDirty/GetCompositionInfo 无锁独占读);以构建的脚本文本为变更检测键,
  相同则不注入;capture Ready 时清缓存强制重推;OnViewFocusGone/Uninstall/view==0
  清缓存 + 尽力清除面板。Tick 在组合期间也触发 capture(此前仅在有 pending 文本时)。
- **ImGui 候选窗口让位**(ImeWindow.cpp):`MeridianBridge::OwnsCandidateUi()`
  (enabled && HasFocus && captureReady)时早退;无字段(captureReady=false)回退 ImGui 框。
- **ImeWnd::GetTextService()**:带 m_fTearingDown 门闩的访问器,供桥的游戏线程 Tick 使用。
- Node 桥测扩展:ui 无捕获 no-op、面板创建/组合串/候选 chips 文本、chip 点击回执 pick:1、
  空状态隐藏、字段失效隐藏。

### 产物
- 构建 EXIT 0;28/28(+2 pick);Node 通过;DLL md5 `1bf4fb6a…`,已部署 MO2(SimpleIME mod)。
- 待重测:① Meridian 字段打拼音 → 字段内**不再**出现拼音,组合/候选出现在 Meridian 页面内的
  深色悬浮面板(输入框下方),空格后仅上屏 "爱丽丝";② 点击候选 chip 应上屏对应词条;
  ③ 英文模式输入仍直通进字段;④ vanilla 菜单(控制台/改名)回归一遍组合与上屏。

## 第 16 轮:面板编号/泄漏补偿/回声协议 (2026-10-01)

> 用户用第 15 轮构建(md5 1bf4fb6a)实测:面板出现在 Meridian 页面内(链路通),
> 但 ① 候选双重编号("1. 1. 为啥");② 拼音泄漏依旧("ailisi"+空格 → "ailisi 爱丽丝");
> ③ 面板概率性不出现(16:46:13 那次组合 captureReady 持续未就绪,ImGui 回退可见)。

### 根因补充
- **双重编号**:TextStore.cpp:1089 / Imm32TextService.cpp:401 存储候选串时已带 "N. " 前缀,
  JS 面板又加 (i+1) → 双份。修复:JS 原样显示。
- **泄漏不止**:被门控拦掉的 WM_CHAR 证明字母不再走 ImeWnd 消息路径——Meridian 宿主经
  **消息无关的输入路径**(游戏线程轮询键盘,CEF 键盘合成)拿到同一批物理按键,消息层无法拦截。
  转为 DOM 侧补偿:组合期间被门控丢弃的 WM_CHAR 逐字符积累为"回声"(退格同步回退),
  上屏/中止时把回声交给 JS,在插入点前精确匹配并删除泄漏文本(含可选尾随空格)后再上屏。
- **提交尾随空格**:上屏键自己的 WM_CHAR 在组合清除后才到达(门控已开)→ 作为英文直通
  转发成单空格块;300ms 内的单空格块丢弃。
- **面板不出现**:两个嫌疑——捕获链路静默(注册绑定未回执)或第 14 轮"组合文本不进编辑器"
  (编辑器空 → 面板无内容可显)。已把线上日志开到 debug(level/flush_level),下轮日志可分辨:
  WM_CHAR dropped 行、capture requested/ready 行、SetText/InsertTextAtSelection/RequestLock 观测点。

### 本轮改动
- BridgeScript:`commit(id,text,echo)` / `ui(comp,cands,sel,echo)` 增加 stripEcho——
  在插入点前匹配 echo(+可选一个尾随空格)→ setSelectionRange + execCommand('delete')
  (发 input 事件,Meridian 状态同步);候选 chips 原样显示(去 (i+1) 前缀)。
- MeridianBridge:`s_echo`(s_pendingMutex 保护,≤256 单元)+ `QueueCompositionEchoChar`
  ('\b' 回退);FlushPending 携带 echo、失败回滚、成功记 s_lastEchoConsumeMs;
  单空格块 300ms 抑制;PushCandidateOverlay 空状态推送携带 echo(中止场景清理),失败回滚。
  锁序:只允许 s_pendingMutex → s_uiMutex(Ready 分支),PushCandidateOverlay 先取回声再进 s_uiMutex。
- ImeWnd WM_CHAR 门控:丢弃改为积累回声。
- 线上配置 level/flush_level = debug(测完记得还原 info)。

### 产物
- 构建 EXIT 0;28/28;Node 通过(新覆盖:ui 无 echo 不动字段、commit 删回声+插入、
  终止时 hide 推送删回声);DLL md5 `8b595110…`,已部署 MO2。
- 待重测:① 打拼音时输入框**仍会**显示原始拼音(内联回声,类似原生 IME),空格后应只剩
  上屏字("ailisi 爱丽丝" → "爱丽丝");② 候选编号不再双份;③ 若面板仍概率不出现,
  把 SimpleIME.log 发来——debug 级现在能看到捕获/组合/锁的全过程。

## 第 17 轮:debug 日志定案——泄漏在游戏线程,回声改为 DOM 位移计量 (2026-10-01)

> 用户用第 16 轮构建(8b595110)重测:泄漏依旧。debug 日志给出定案证据:
> 组合全程(03.748→06.688,组合串 1→13 字符)**ImeWnd 没收到任何 WM_KEYDOWN/WM_CHAR**——
> 按键在消息泵上游即被 TIP(CUAS)消费,第 16 轮的 WM_CHAR 回声积累从未触发,echo 恒空。
> 同时证实:① Meridian 会话开启 `AllowTextInput`(Scaleform_AllowTextInputHook 17:23:03.101)+
> 游戏菜单 `MeridianUI_FocusMenu`——泄漏路径=游戏线程文本管道喂 CEF(第 14 轮日志的
> "WM_CHAR 's'/'a' 转发"实为英文直通,非组合键);② 组合链路本身健康('d'→'的' 正常提交,
> 第 14 轮 "committing 0" 实为用户退格清空组合,非 bug);③ 捕获链路健康(session 1
> 58ms ready、失败自愈 re-capture)。

### 本轮改动(回声机制 v2:DOM 插入点位移计量)
- **放弃重建按键串**(微信组合串带分音符分隔符 "we's"≠原始键 "wes",不可靠),改为 JS 侧
  按插入点位移删泄漏:组合期间除泄漏外无人写字段 → 每次面板推送删除「自上次推送以来
  插入点前新增的字符」(≤32 上限防误删);组合结束的最后一次推送做收尾清理(顺带删掉
  上屏键自己泄漏的空格)。对分隔符/大小写/转换显示天然免疫。
- leakMark 生命周期:capture 时立基线(同元素已有标记则保留,防止中途 re-capture 吞掉
  已泄漏字符);commit 上屏后跟进插入点;blur/cancel/组合结束清除;元素失焦清除。
- 捕获时机:view 聚焦后立即做一次初始捕获(此前只在有 pending/组合时才捕获,首次组合
  会与捕获回执赛跑,首字符漏删)。
- 移除第 16 轮回声管道(s_echo/QueueCompositionEchoChar/echo 参数);尾随空格抑制改挂
  `s_lastCompositionEndMs`(空状态推送成功时点,300ms 窗口)。
- ImeWnd WM_CHAR 门控保持纯丢弃(防万一的消息路径双写)。
- 线上日志保持 debug 级(下轮再还原 info)。

### 产物
- 构建 EXIT 0;28/28;Node 通过(leak strip:首字符剥离、逐字符剥离、退格不误删、
  上屏插串、收尾清理后停止跟踪);DLL md5 `ca5eee34…` 已部署 MO2。
- 待重测:打 "ailisi"+空格 → 输入框在打字期间**最多短暂闪现拼音(≤1 帧)**,空格后应只剩
  "爱丽丝";若仍有残留,发 SimpleIME.log(debug 级会带 stripLeak 的完整轨迹)。

## 第 18 轮:退格使基准失准("q轻甲")+ 泄漏退格误删真实文本 (2026-10-01)

> 用户用第 17 轮构建(ca5eee34)重测:"qingjia"+空格 → "q轻甲"(恰好漏删首字符)。
> debug 日志定案:① view 聚焦时的初始捕获报 no-field(DOM 字段在 TryFocus 时还未聚焦,
> 真正聚焦=用户点进输入框,日志 18:21:36.545 AllowTextInput 触发)→ 基准直到首个组合键的
> 捕获回执才立起;② 每次组合前后用户都按退格修正(日志 vk=0x8 洪流),英文态退格经泄漏
> 路径删掉字段字符,基准标记不随之移动 → 下次组合首字符落在标记之前 → 永远漏删第一个;
> ③ 更严重的隐患:组合中泄漏的退格删的是**已剥离泄漏后的真实文本**(数据丢失)。

### 本轮改动(泄漏维护 v3:maintainField 双向维护)
- **组合开始重锚定**:首个组合推送用组合串与字段尾部精确匹配(首推组合串=原始键符,
  无分隔符)定位泄漏前插入点 → 免疫英文退格导致的标记失准(消灭 "q轻甲" 类首字符残留)。
- **泄漏退格恢复**:组合期间维护值快照,插入点后退(泄漏退格删了真实字符)→ 从快照
  恢复该字符(execCommand insertText,事件链一致)。删除方向不变(删新增的泄漏字符)。
- **点击跳变防护**:插入点位移 |δ|>16 视为点击(非泄漏),仅重锚不编辑。
- **AllowTextInput(true) 触发即时捕获**(ScaleformHook → MeridianBridge::RequestCapture,
  清 captureReady + 置 initialCapturePending):输入框聚焦的确切信号,基准/面板锚点在
  首键之前就绪(view 聚焦时刻字段未聚焦,初始捕获只能报 no-field)。
- leakMark 增加 value 快照;capture/blur/cancel 同步 leakActive 标志。

### 产物
- 构建 EXIT 0;28/28;Node 通过(重锚定、泄漏退格恢复 'b'、英文退格后真实字符存活、
  点击跳变不编辑、收尾清理);DLL md5 `e7f44e8b…` 已部署 MO2。
- 待重测:打 "qingjia"+空格 → 只剩 "轻甲";组合中按退格修正拼音,输入框文本不应丢字符;
  若仍有残留,发 SimpleIME.log。

## 第 19 轮:Meridian 候选面板对齐常态候选框的 M3 主题 (2026-10-01)

> 用户反馈 Meridian 里的候选面板与常态候选框外观不一致(此前是临时深色内联样式)。

### 本轮改动
- **主题镜像**:渲染线程(ImeWnd::Draw,唯一合法访问 M3/ImGui 样式的位置)按请求快照
  候选窗口实际使用的调色板(ImGuiCol_WindowBg+WindowRounding、onSurface 组合串、
  primary 光标、outlineVariant 分隔线/chip 描边、onSurfaceVariant chip 文字、
  secondaryContainer/onSecondaryContainer 选中 chip)+ dp→px 缩放(m_uiScale×DPI)→
  构建带 `--stb-scale`/`--stb-*` CSS 变量的样式表 → 桥接 `theme(css)` 每会话推送一次
  (变更即重推;新会话强制重推——新页面文档没有旧 style 标签)。
- 面板 DOM 改为语义 class(.stb-panel/.stb-comp/.stb-caret/.stb-divider/.stb-chips/.stb-chip
  /stb-sel)+ 注入 `<style id="stb-simpleime-style">`;样式全部走 calc(px*var(--stb-scale)),
  跟随游戏 UI 缩放;内联样式只剩定位。
- 形状对齐 M3 规范:32dp chip 高、8dp 圆角、16dp 水平内距、8dp 间距、14px/20px LabelLarge、
  光标闪烁(1.2s step-end)、无边框阴影(ImGui 候选框也没有)。
- JS 内置同构默认深色样式(theme 推送前兜底);字体 = 微软雅黑(与 ImGui 主字体一致)。
- Node 测试:样式注入/主题替换、panel/chip className、结构断言按新 DOM 更新
  (mock 的 appendChild 现在传播 textContent)。

### 产物
- 构建 EXIT 0;28/28;Node 通过;DLL md5 `ff4510b5…`已部署 MO2。
- 待重测:Meridian 面板应与常态候选框同配色/同形状/同字号(主题色变化后下次会话生效);
  若主题仍不一致,发截图+SimpleIME.log。

## 第 20 轮:面板形状对齐截图——药丸 chip + 单行撑宽 (2026-10-01)

> 用户提供常态候选框截图:chip 为全圆角药丸形(stadium),候选单行排布,面板随内容
> 撑宽(无 480px 上限)——与 M3 规格书的 8dp 圆角/换行假设不同,以实机截图为准。

### 本轮改动
- 两处样式(JS 兜底 + C++ theme 推送)同步:.stb-chip border-radius:999px(药丸形);
  .stb-chips flex-wrap:nowrap;.stb-panel width:max-content(去 max-width:480px)。
- 颜色继续走第 19 轮的实时主题镜像(截图配色自动跟随用户主题)。

### 产物
- Node 通过;构建 EXIT 0;DLL md5 `664267a7…` 已部署 MO2。
- 待重测:Meridian 面板与常态候选框并排对比(药丸 chip、单行、配色、字号)。

## 第 21 轮:面板跟随竖排设置与配置字体 (2026-10-01)

> 用户要求 Meridian 面板跟随常态候选框的颜色/缩放/横竖排/字体。颜色与缩放第 19 轮已接入
> (实时主题镜像 + --stb-scale);本轮补齐竖排与字体。

### 本轮改动
- **竖排跟随**:ui() 推送携带第 4 参数 vertical(读 settings.appearance.verticalCandidateList,
  与 ImGui 候选框同一设置);面板切 .stb-vert 类 → M3 菜单行样式:44dp 行高、未选中
  ExtraSmall(4dp) 圆角无描边、选中 tertiaryContainer/onTertiaryContainer 填充 + Medium(12dp)
  圆角、列向排布。调色板新增 rowSelectedBg/rowSelectedText。
- **字体跟随**:主题快照解析有效主字体路径(配置 fontPathList 中首个存在的文件,否则
  GetDefaultFontFilePath 系统默认,与 AddPrimaryFont 的解析一致)→ CSS 注入
  @font-face('STB Primary', file:/// 百分号编码 URL)+ 字体链 'STB Primary','Microsoft YaHei',
  'Segoe UI',sans-serif——CEF 若拒绝本地字体则优雅回退雅黑(即默认字体本尊)。
- FileUrlFromUtf8Path(UTF-8 百分号编码)/ FontFamilyChain 辅助;JS 兜底样式补竖排变体。

### 产物
- 构建 EXIT 0;28/28;Node 通过(竖排类切换、@font-face/file URL 推送);DLL md5
  `b99959c6…` 已部署 MO2。
- 待重测:① ToolWindow 里切竖排候选 → Meridian 面板变竖列菜单行样式;② 配置自定义字体
  → 面板字体跟随(若 CEF 拒本地字体则回退雅黑);③ 颜色/缩放随主题与 UI 缩放变化。

## 第 22 轮:全量代码审查——Present 兜底驱动 4Hz 自钝化 + Tick 双线程竞态 (2026-10-01)

> 对 Meridian/Prisma 桥接全量 diff(约 900 行改动 + 7 个新文件)做静态审查,交叉核对
> hook.h 虚表结构、ITextService 双实现、ImeController::AddTask 线程契约、WM_CHAR 门控
> 时序、配置编码链路(TOML UTF-8 → JsString CP_UTF8)与测试覆盖。逻辑链路
> (回声抑制、泄漏维护 v3、提交回执超时、锁序 s_pendingMutex→s_uiMutex、Prisma 失败
> 安全)全部确认无误;发现并修复 3 处并发/驱动层 bug。

### 修复
- **Present 兜底帧驱动自钝化(4Hz 抖动)**:`PresentHook` 接管时刷新帧令牌
  `g_lastUiFrameMs`,下一帧 `uiFrameStale` 即变 false → 休眠 250ms → 再跑一帧,
  兜底驱动实际退化为 ~4fps(语言栏/回退候选框卡顿,接管延迟叠加)。修复:令牌只度量
  PostDisplay 存活,仅由 `ImeApp::Draw`(PostDisplay 路径)刷新;Present 接管后逐帧驱动,
  PostDisplay 恢复时令牌自然刷新、钩子自行让位。
- **Tick 双线程竞态**:`MeridianBridge::Tick` 每帧由 PostDisplay(游戏线程)与
  PresentHook(渲染线程)双路调用(第 13 轮的既定设计),会话切换帧可能并发进入:
  `s_listenerView` 无锁写、捕获/提交节流为 check-then-act、`s_lastCompositionEndMs`
  无锁读——可致双重捕获/两块乱序提交。修复:Tick 顶部加单飞护栏
  (`s_tickInFlight` exchange + RAII 复位,异常安全),并发第二调用方整帧跳过,
  保留"每帧必 Tick"语义;相关"game thread only"注释更正为 Tick-body-only。
- **OnViewFocused 卸载后复活 + 重复聚焦churn**:vtable 虚脱钩永不移除,Uninstall 后
  焦点事件仍会重建会话状态并向停机中的控制器发 IME 同步任务(PostMessage 失败刷错);
  同一视图重复 TryFocus(AlreadyFocused)会反复重捕获/重推主题/重同步。修复:入口加
  `!s_enabled` 短路 + 同视图短路。

### 产物
- 构建 EXIT 0;28/28;Node 通过;DLL md5 `f0602c13…`,build / dist / MO2 三处一致。
- 待重测:Meridian 会话中语言栏与回退 UI 应逐帧刷新(不再 250ms 一跳);打字/提交/
  候选行为与第 21 轮一致(本轮不改任何 JS 与输入语义)。

## 第 23 轮:二次全量深审——提交分块代理对边界 + 全链路复核 (2026-10-01)

> 应用户要求再全面审查一遍。本轮把上一轮只看过 diff 的文件全部读了全文(WndProc、
> ScaleformHook、ImeMenu、MeridianBridge.cjs、PrismaUI_API.h、CandidateUi),逐行核对了
> JS 桥的 maintainField/showPanel 状态机与 C++ 推送协议的每个交叉点。

### 新修复
- **提交分块切开代理对**:FlushPending 按 MAX_COMMIT_UNITS(1024 单元)切块,边界恰好
  落在代理对两半之间时(长粘贴/超长上屏),前块以孤立高代理收尾 → DOM 里变 U+FFFD,
  低代理流入下一块。修复:边界处若块尾是高代理且队列头是低代理,退一个单元再提交。
  (QueueText 整体入队不受影响;测试路径为 >1024 单元的 unicode paste。)

### 复核确认无问题的交叉点
- **pick 索引语义**:DOM chip 索引 = CandidateList() 页内 0-based,与 ImGui 候选框点击
  (ImeWindow.cpp:112/138 传页内索引 → TextStore::CommitCandidate → ITfCandidates
  SetSelection)完全同构;Selection()/FirstIndex() 语义两侧一致。
- **JS 重锚定时序**:首推组合串=原始键符(分隔符在后续键才出现)时 landed 尾匹配成立,
  之后转 delta 维护——与第 17/18 轮实测定案一致;首推前多键落入同一帧(>1 字符且带分隔符)
  是既接受的启发式盲区。
- **maintainField 状态机**:capture 立基线→首推重锚→delta 删/恢复→点击跳变(|δ|>16)重锚
  →终态清理停跟踪;commit 后跟进插入点;blur/cancel 清空。Node 测试全覆盖。
- **Prisma V1 用法**:RequestPluginAPI(V1)/HasAnyActiveFocus 与头文件签名一致;V2 继承
  V1 不影响只读避让。
- **拆卸次序**:Uninitialize 中 WinHooks 先撤(消息停止)→ Meridian/Prisma Uninstall;
  Present 钩子永驻但 s_enabled=false 使 Tick/帧分支全部惰化;OnListenerPayload(CEF 线程)
  只触原子与 AddTask,无拆卸竞争窗口。
- **std::format 生成 CSS**:locale 无关(小数点恒为 '.'),CEF 侧解析无歧义。
- file URL 中 '\' → %5C(Chromium 在 Windows 上按路径分隔符接受)、'%'/空白均正确编码。

### 记录在案的待验证项(不改动,等游戏实测)
- **英文直通疑似双写**:Meridian 会话中英文键同时走 宿主泄漏路径(游戏文本管道喂 CEF)
  与 WM_CHAR→QueueText 提交路径,理论上英文模式会双写。组合态由回声抑制+DOM 补偿覆盖,
  英文态无证据(第 15 轮后未再显式重测)。若实测确认,修法是 Meridian 会话内停用 WM_CHAR
  英文转发、仅保留上屏文本提交。
- Prisma 拥有键盘期间 PostDisplay 停摆(Meridian 会话)时 Refresh 不刷新,Prisma 焦点缓存
  可能过期——两套 UI 同时持焦本就是病态组合,维持现状。

### 产物
- 构建 EXIT 0;28/28;Node 通过;DLL md5 `153b1592…`,build / dist / MO2 三处一致。
- build_out.log 加入 .gitignore(本地构建日志勿入库)。

## 第 24 轮:SKSE Menu Framework(ImGui)文本框原生中文输入支持 (2026-10-02)

> 应用户要求做原生支持(此前需要第三方桥 TMS_SIMEtoSKSEMF)。SKSEMF 的 InputText 是
> ImGui 字段:不进 Scaleform 菜单栈、聚焦时不加 AllowTextInput 计数,SimpleIME 的
> 激活条件(HasTextEntry)与 GFx 字符事件提交路径双双失效。本轮通过 SKSEMF 公开导出
> 补齐两个缺口,机制经 cashboxs/TMS_SIMEtoSKSEMF(MIT)在 3.7+ 实测验证。

### 新增
- **`src/hooks/SkseMenuFrameworkBridge.cpp` + `include/hooks/SkseMenuFrameworkBridge{,Logic}.h`**:
  - **API 探测**:kDataLoaded 时 GetModuleHandle(L"SKSEMenuFramework.dll") + GetProcAddress
    解析 RegisterEventPriority / GetMenuFrameworkVersion(要求 ≥3.7)/ igGetCurrentContext /
    igGetIO / igGetFrameCount / ImGuiIO_AddInputCharacter;Tick 每帧兜底重试(5s 节流)。
  - **会话租约**:kBeforeRender 回调轮询 io->WantTextInput(偏移 0xCC,按 SKSEMF 3.8 自带
    cimgui.h 的 offsetof 编译期推导,SKSEMF 升级 ImGui 时需重推),false→true 时调用
    **原生** AllowTextInput(true)——必须走 REL::RelocationID(67252,68552) 让 SimpleIME
    自己的 detour 触发 OnTextEntryCountChanged→EnableIme(Ime::ControlMap::SKSE_AllowTextInput
    直改成员不经过钩子链,IME 不会联动,这是本桥最容易踩的坑);false→true 释放。
    WantTextInput 恒定租借计数归零:租约不泄漏。
  - **文本路由**:SendUiString 在 Meridian 之后新增 SKSEMF 分支——SessionActive 时入队
    (IME 线程),框架回调线程(kBeforeRender)在会话检查前 FlushPending(提交与失焦竞态时
    最后一字不丢,ImGui 对无人消费的输入队列自清)。UTF-16→码点重组代理对,ASCII 拒绝注入
    (英文模式走原始 CharEvents 路径,与 TMS 桥一致),反引号与中点·沿用 Scaleform 路径剥离规则。
  - **ASCII 中性化**:复用 JamieMods 预置的 DispatchInputEventHookData(67315/68617+0x7B,
    与 TMS 桥同一调用点),仅框架存在时安装;会话活跃且 ShouldNeutralizeAscii 时把可打印
    ASCII 的 CharEvent.keyCode 置 0。谓词顺序:sessionActive → imeDisabled(**优先于
    composing**,对齐 ImeMenu::OnKeyEvent 的残留 IN_COMPOSING 语义)→ imeComposing →
    keyboardOpen(NATIVE 转换模式)。英文直通不受影响。谓词抽在 Logic 头,headless 单测。
  - **租约不悬空四重保障**:字段失焦(WantTextInput false)/框架停渲(PresentDisplay 侧
    Tick 检测 kBeforeRender 静默 >600ms 强制结束)/读档 ForceEndSession(ImeApp 消息监听)/
    泄漏修复器回调(EventHandler::HealLeakedTextEntryCount 通知 OnTextEntryCountHealed,
    防止计数被外部清零后租约标志残留导致下次获取变成 no-op)。
  - **互斥**:检测到 TMS_SIMEtoSKSEMF.dll 时保持关闭(双桥会双重注入),对齐 Meridian 对
    Skyrim-Text-Bridge 的处理;**不会**拒绝加载,只停用本路径。

### 接线
- ImeApp:kDataLoaded Install、kPreLoadGame ForceEndSession、Uninitialize Uninstall。
- ImeMenu::PostDisplay:Tick(停渲看门狗)。Utils.cpp SendUiString:路由分支。
- EventHandler.cpp HealLeakedTextEntryCount:通知桥。配置项 `input.skse_menu_framework_support`
  (Settings/Configuration/converter/serializer/contrib toml,默认 true,无面板项,与
  meridian_support 一致)。

### 已知边界(记录待实测)
- 中文模式下 IME 提交的半角 ASCII(如半角标点)被双路径同时丢弃(原始路径被中性化、
  提交路径拒 ASCII)——与 TMS 桥行为一致,其实测通过微软拼音/五笔;若用户反馈丢字,
  需在提交路径放行 ASCII 并在原始路径判断"该键是否已被 TIP 消费"。
- WantTextInput 偏移 0xCC 绑定 SKSEMF 3.8 的 ImGui ABI;框架升级后若失配,表现为 IME
  不激活或常开,重推偏移即可(注释内有推导方法)。
- 英文模式(WantTextInput 期间)下 DirectInput 原始 ButtonEvent(Backspace/方向键)直达
  ImGui,属正常编辑通道,不在过滤范围。

### 产物
- 构建 EXIT 0(548 目标);ctest 33/33(新增 5 个 SkseMenuFrameworkBridgeLogicTest);
  Node Meridian 回归通过;DLL md5 `b7507c12…`,build / dist 一致(MO2 待用户复制/同步后实测)。
- 本机 RC 死锁补丁(simple_rc.bat + rules.ninja)在重配置后已重新应用。

## 第 25 轮:参考 Boutique 重构暗色/亮色主题 (2026-10-02)

> 用户要求参考 lin-414/boutique(WPF,自研)的主题界面样式重构 SimpleIME 的暗色与
> 亮色主题。Boutique 的配色 = 三层暖中性背景层次 + 哑光鼠尾草绿强调色 + 暗色暖奶油
> 文字(#ECE0C8)/亮色暖纸白底,M3 tonal-spot 动态引擎从单一源色推不出这种"中性灰面 +
> 定点强调"的组合,故在主题引擎里新增静态调色板变体,与现有 Material You 自定义取色并存。

### 库侧(extern/JamieMods fork,未提交,待用户推 fork)
- `imguiex/m3/colors.h`:`SchemeConfig` 加 `ThemeVariant variant`(TonalSpot=默认,
  Boutique);新增 `GetBoutiqueSchemeConfig(darkMode)`(sourceColor 播种对应模式的
  强调色,便于切回动态取色时同族起步)。
- `imguiex/m3/palette_boutique.h`(新,header-only):暗/亮两套完整 48 角色调色板。
  锚点取自 boutique XAML:暗 #121214/#1A1B1F/#282A32 背景、边框 #3A3D48、强调
  #4D7A6D(容器 #3A5F54)、文字 #ECE0C8/#B8B2A7、Info #5B8AA6/Conflict #C8943B/
  警示红 #E67E80、错误底 #3C2A2A;亮 #FCFBF9/#FFFFFF/#F3F1ED、边框 #D8D4CC、强调
  #5B8266、文字 #1A1A1A/#333333、Info #3D6E8A/Conflict #A9711B/红 #C44D4D。其余
  角色(容器阶梯、Fixed 系、inverse 系)按锚点同族推导。
- `M3ThemeBuilder.cpp`:Build 按 variant 分发,Boutique 走静态表填充。
- `Material3.h`:`RebuildColors(bool)` 改为复制 GetSchemeConfig 再翻 darkMode——
  原来的指定初始化会把 variant 静默丢回 TonalSpot(AppBar 明暗切换必踩)。

### SimpleIME 侧
- 默认主题切 Boutique 暗色(Settings.h);旧配置无 `theme_style` 键时同样落到
  Boutique(本次重构的意图),写 `theme_style = "material"` 可回旧动态取色。
- 配置链:`configuration.h` 加 `appearance.themeStyle`(string);ConfigSerializer
  读写 `theme_style`(带注释);settings_converter 双向映射,未知值保留编译默认,
  Boutique 变体下把 sourceColor 归一为对应模式强调色(入口色块不再显示旧紫色)。
- AppearancePanel 主题构建器重排:顶部"主题样式"下拉(Boutique / 自定义 Material
  You);Boutique 分支只有明暗开关 + 表面阶梯 5 色块与 Primary/Secondary/Tertiary/
  Error 双色预览(经 ThemeBuilder::Build 实时构建,仅输入变化时重建);Material 分支
  保持原 HCT 取色器/对比度/色板预览。行标签 ThemeColor→Theme,Apply 按变体构造
  SchemeConfig。ToolWindow 的 GetSchemeConfig 结构化绑定补第 4 元素 variant。
- contrib toml 加 `theme_style = "boutique"`(theme_source_color 注明仅 material
  生效);翻译 en/zh 加 Theme/ThemeStyle/ThemeStyleBoutique/ThemeStyleMaterial,
  移除已无引用的 ThemeColor。

### 产物
- 构建 EXIT 0,SimpleIME.dll 4,441,600B;dist(SimpleIME.dll + interface/SimpleIME
  的 toml×3)已同步。RC 补丁(simple_rc.bat)仍在位,未重配置。
- 待实测:游戏内明暗切换(AppBar 日/月)、Boutique/自定义切换、旧配置升级路径、
  配置回写是否带出 theme_style;JamieMods 4 文件需在 fork 提交推送。

## 第 26 轮:取消独立 Boutique 样式选项,固化为默认主题 (2026-10-02)

> 用户决定:主题样式不作为可选项暴露,Boutique 直接就是默认主题;界面后续会彻底重构,
> 不值得为选择器继续投入。第 25 轮的样式下拉与 Boutique 预览分支整体移除。

### 变更
- **AppearancePanel**:主题构建器对话框恢复原形(HCT 取色 + 明暗 + 对比度 + 动态色板
  预览),删除样式下拉与 Boutique 色块预览;`m_configuredBoutiqueScheme` 成员移除。
- **Apply 语义**:对话框打开时播种当前 variant;只改明暗/对比度 → Apply 保持当前
  变体(Boutique 仍是 Boutique);在 HCT 取色器提交了新颜色(m_configuredColorEdited
  锁存)→ Apply 自动落为 TonalSpot 动态配色并持久化 `theme_style = "material"`。
  这是因为固定调色板忽略自定义源色,改色即等于离开默认主题。
- **回默认主题的途径**:删除 toml 里的 `theme_style` 键或改回 "boutique"(注释已写明);
  UI 无重置按钮——等界面重构时统一处理。
- 翻译:ThemeStyle/ThemeStyleBoutique/ThemeStyleMaterial 三键移除(Theme/主题保留,
  入口行标签)。
- 库侧(ThemeVariant/palette_boutique.h/RebuildColors 修复)原样保留——重构后的新
  界面可以直接复用静态调色板变体。

### 产物
- 构建 EXIT 0;dist 暂存目录与 7z 包(17:26 后重建)均已更新并验证:包内 DLL 与
  最新构建逐字节一致、翻译无残留 ThemeStyle 键、SimpleIME.toml 带 theme_style。

## 第 27 轮:设置界面重构——Boutique 设计语言 + 候选窗活体预览 (2026-10-02)

> 第 26 轮预留的"界面彻底重构"。设计语言沿用 Boutique 调色板(第 25 轮固化的默认主题),
> 重构信息架构与布局,不换肤。

### 设计要点
- **签名元素——候选窗活体预览**(外观页顶部):用与真实候选框完全相同的组件
  (ListItemPlain + 1.2s 闪烁插入符 + Divider + Filter 药丸 chips/竖排 MenuItem)复刻
  "nihao → 你好/你号/拟好/尼好",随缩放、横竖排、主题即时变化;点击候选仅切换选中环,
  不提交。竖排切换的效果在预览里即时可见。
- **侧栏展开修复**:`BeginResponsiveNavRail` 的默认展开阈值是 ExtraLarge(1600),
  超过窗口上限(Large 1200)→ 侧栏永远折叠成三个无字图标。改传 Expanded::Breakpoint(840)。
- **外观页**:废弃假三列居中表格,改为分区结构:预览 / 常规(缩放+语言)/ 候选窗口
  (竖排、语言栏)/ 主题(色块行 + 自定义 + **重置主题**——补上第 26 轮遗留的
  "回到 Boutique 默认"途径,`GetBoutiqueSchemeConfig(IsDark())` 直接应用)。
- **行为页**:①实时状态升为面板顶部**状态卡**(模组/输入法/键盘焦点三行状态点 +
  请求焦点按钮,替代原列表中段的眼睛图标行);②tooltip 里隐藏的说明改为复选框下方
  可见支撑文(缩进对齐 Checkbox::LayoutSize=48dp 槽位);③启用Mod 关闭时其余设置
  可见但禁用(原实现整块隐藏);④快捷键 chord 渲染为**键帽**(PanelWidgets::Keycap,
  凹面圆角小方块 + "+" 连接);⑤IME 位置策略三选一改为 **tonal toggle 分段按钮**
  (ButtonConfiguration.toggle + selected,2dp 间距)。
- **共享组件** `include/ui/panels/PanelWidgets.h`:SectionHeader(眉题 + 通栏细线)、
  StatusDot、Keycap,两个面板与 mock 共用。
- 翻译 en/zh:新增 Preview/PreviewCaption/General/CandidateWindow/Customize/
  ResetTheme(ToolTip)/Status/StatusMod/StatusIme/StatusFocus/StateOn/StateOff/Input;
  移除被状态卡取代的 States/ImeEnabled/Focus 行标签(其 tooltip 保留用于状态行)。

### 库侧修复(extern/JamieMods,未提交)
- **Material3.h `CachedTypeScale` 未初始化(真实 bug,预览工具揪出)**:结构体无默认
  初始化,M3Styles 构造函数的 `UpdateScaling(1.0F)` 把未初始化的 `unScaledText` 乘进
  `currText` → 游戏内任何在 role 作用域**之外**读 `GetLastText()` 的代码(典型:
  `M3::Checkbox` 标签 Y 偏移)用的是未定值——预览进程里该值是 7.2e18,标签直接画到
  屏幕外。修复:currRole=None、unScaledText/currText 默认 TEXT_LABEL_LARGE、
  currHalfLineGap 默认 HalfLineGap(_LABEL_LARGE)。游戏内表现为标签位置从"碰运气"
  变为确定(之前游戏堆布局恰好让标签可见,但位置未必居中)。

### 设计预览工具(tools/design-preview/,新)
- 独立 Win32+DX11+ImGui 宿主 + MockPanels.cpp(忠实移植两个面板的绘制代码,游戏依赖
  全部桩化),`build-preview.cmd` 用 clang-cl 手动编译,**不触碰主构建目录**(RC 补丁无恙)。
- 关键编译经验:①mini 强制包含头 preview_pch.h(<filesystem>+using std::min/max,
  等价主构建 Skyrim PCH 环境);②`/DNOMINMAX`(<filesystem> 会带进 minwindef.h 的
  min/max 宏,砸烂 std::max 调用点);③imguiex 头需在 windows.h 之前解析;
  ④ImGui 1.92.7 动态字体(无需字形 range);⑤MCU 的 .cc 源 + mcu_utils.cpp 需入链,
  `/D_USE_MATH_DEFINES`。
- 运行参数:`--tab=appearance|fontbuilder|behaviour`、`--light`、`--vertical`、
  `--scroll=N`(截图用);截图脚本 capture.ps1(PowerShell CopyFromScreen)。
- 截图存于 tools/design-preview/shots/(暗/亮 × 外观/行为/竖排/字体页 + 底部两屏),
  逐屏人工验收通过。

### 产物与验证
- 主构建 EXIT 0;SimpleIMETest **33/33**;设计预览工具编译通过。
- DLL md5 `d556cc5b…`(4,451,840 B),build / dist / MO2(E:\Skyrim AE\mods\SimpleIME)
  三处一致;dist 与 MO2 的 interface 翻译 toml 已同步(**未覆盖** SimpleIME.toml 配置)。
- 子模块 Material3.h 修改待用户在 fork 提交推送(同第 25 轮惯例)。
- 待游戏内实测:①设置窗口整体观感(明/暗);②复选框标签位置是否比之前更居中
  (未定值→确定值);③预览卡随缩放/竖排/主题实时变化;④重置主题按钮回 Boutique 默认;
  ⑤快捷键键帽显示与改键流程;⑥行为页禁用态(关闭 EnableMod)灰显。

## 第 28 轮:状态栏外形调整——略小的方框 + 微圆角 (2026-10-02)

- 用户反馈:状态栏(语言栏)从胶囊形改为**略小一点的方框形状,带略微圆角**。
- 改动(extern/JamieMods `m3/spec/tool_bar.h`,`ToolBarSizing<Floating>`——仅语言栏使用,
  Docked/FloatingFAB 变体不受影响):
  - `HorizontalContainerHeight` 64dp → **52dp**(内部 40dp SmallIconButton 上下各留 6dp,
    栏内语言 MenuItem(OuterHeightEx=48dp)留 2dp,仍可容纳);
  - `ContainerShape` 高度一半(32dp,胶囊)→ **ShapeCorner::Small(8dp)** 圆角方框。
  - 两值均走 `M3Styles::GetPixels` 预算表,随 UI 缩放正常缩放。
- 验证:主构建 EXIT 0;build / dist / MO2(E:\Skyrim AE\mods\SimpleIME)三处 DLL 已同步
  (4,495,360 B)。待游戏内实测观感(高度是否合适、圆角弧度、语言框与栏的贴合度)。

## 第 29 轮:删除外观页候选窗活体预览 (2026-10-02)

- 用户反馈:**不需要预览功能**。第 27 轮的签名元素(候选窗活体预览卡)整体移除。
- 删除范围:
  - `AppearancePanel`:`DrawCandidatePreview` / `DrawReplicaContent` / `m_previewSelection`
    / `PREVIEW_COMPOSITION` / `PREVIEW_CANDIDATES`;外观页改为直接以"常规"分区开头;
  - 翻译 en/zh/ja/ko:`Settings.Appearance` 下 `Preview` / `PreviewCaption` 两键;
  - 设计预览工具 MockPanels.cpp:同步删除移植副本(STRINGS 两键、DrawReplicaContent
    移植、预览分区绘制),工具与真实 UI 保持一致。
- 保留:FontBuilder 的字体预览(preview_panel)是选字体的既有功能,与本次删除无关。
- 验证:主构建 EXIT 0;SimpleIMETest **33/33**;设计预览工具重新编译通过;
  build / dist / MO2 三处 DLL md5 一致(9c5b2bc4…);dist 的 interface 翻译补齐为
  en/zh/ja/ko 四个(此前 dist 缺 ja/ko);MO2 的 SimpleIME.toml 配置未覆盖。

## 第 30 轮:亮色主题残留暗底修复 + 主题模式分段选择 (2026-10-03)

- 用户反馈(截图):①亮色主题下设置页仍有暗色区域;②主题区不应是"黑暗模式"开关,
  应改为亮色/暗色的方式选择。
- **根因(取用户截图逐像素定位)**:暗色区域 #1A1B1F = boutique 暗色面板背景——
  切到亮色后全局 ImGui 样式的 `ChildBg` 槽位仍是暗色值,而 `WindowBg` 已是亮色白。
  机制:`PushStyleColor` 直接改写 `g.Style.Colors[idx]`、弹栈恢复**压入时捕获的备份**。
  主题切换当帧:面板根守卫先压 ChildBg(暗),帧中点 `SetupDefaultImGuiStyles` 全量刷成亮,
  帧末守卫析构把**帧前的暗色备份**写回 → 刷新被部分回滚,此后每帧保持
  "WindowBg 亮 / ChildBg 暗"的拼接态(卡片/侧栏用 scheme 直读色不受影响)。
- **根修(ImeWnd::Draw)**:每帧帧首 `SetupDefaultImGuiStyles(ImGui::GetStyle())`,
  在任何窗口/守卫存在之前从当前 scheme 派生基础样式——无论哪条路径(模式选择、
  主题构建器、重置、缩放/DPI)何时改了配色,一帧内必然归拢。帧内的即时刷新
  (面板各处)保留,作同帧反馈;帧首同步兜底其被弹栈回滚的部分。
- **交互改版(AppearancePanel)**:`DrawDarkModeRow`(黑暗模式开关行)删除,
  新增 `DrawThemeModeRow`——"主题模式"标题 + 右侧 [亮色|暗色] tonal 分段按钮
  (与 IME 位置策略同款交互);点击当前模式无操作,点击另一模式重建配色。
  主题构建器对话框内的"黑暗模式"复选框保留不动。
- 翻译 en/zh/ja/ko:`Settings.Appearance` 新增 `ThemeMode`/`Light`/`Dark`;
  `DarkMode` 键保留(构建器对话框仍用)。
- 设计预览工具同步:MockPanels 换成分段选择行;主循环加帧首样式同步(镜像根修);
  `--scroll=` 改为作用于真正滚动的 `##SettingsContent`(页面子项是 AutoResizeY,
  之前 SetScrollY 无效)。截图 shots/fix-light-theme2.png、fix-dark-theme2.png
  两模式逐屏验收:亮色整页无暗带,主题卡两模式分段选中态正确。
- 验证:主构建 EXIT 0;SimpleIMETest **33/33**;build / dist / MO2 三处 DLL md5
  一致(c92dde10…);dist 与 MO2 的 interface 翻译已同步(MO2 的 SimpleIME.toml
  配置未覆盖)。
- 待游戏内实测:①亮色主题整页(尤其标题栏下方横带、侧栏四周、侧栏与内容间隙);
  ②亮色↔暗色来回切换后暗底是否彻底消失;③主题模式分段选择的观感与命中。

## 第 31 轮:候选窗口纵向空白压缩 (2026-10-03)

- 用户反馈(截图):横排候选窗口过高,排版行与候选行上下空白过大。
- **根因**:两行内容都包在 M3 `ListItemPlain` 里,而 M3 List 规格强制每行
  52dp(上下 paddingY 10dp × 2 + minContentHeight 32dp)——拼音串行文字实际
  仅 24dp 行高、chip 行实际仅 32dp,多余 ~28dp/行全是留白;再加窗口上下
  padding 与分隔线间距,总高 ≈137dp(用户 110% DPI 下 151px)。
- **修改(仅 src/ui/ImeWindow.cpp,不动 JamieMods 子模块)**:
  - 摘掉两处 `ListItemPlain`,拼音串与 chip 行直接绘制(原行背景填充色与
    窗口底同色,视觉无差;Chip/MenuItem 自带文字角色与高度,可独立成行);
  - 拼音串所需 List 文字角色(BodyLarge)改由窗口体作用域内的
    `UseTextRole` 提供(caret 计算与 AlignedLabel 居中依赖它);
  - 窗口 padding 改为:纵向 = 圆角安全区 8dp(原值),横向 = 8dp + List
    gutter 16dp(原 ListItem paddingX 16dp + 窗口 8dp,内容起点不变),
    分隔线与内容共享同一 gutter,且避开 16dp 圆角曲线。
  - 新总高 ≈89dp(8+24+8+1+8+32+8),约降 35%;竖排候选的拼音行同步变紧。
- 验证:主构建 EXIT 0;SimpleIMETest **33/33**;build / dist / MO2 三处 DLL
  md5 一致(582027df…)。
- 待游戏内实测:横排候选窗口观感(文字↔分隔线↔chip 间距是否均匀)、竖排
  候选行缩进( gutter 由 8dp 改为 24dp)与圆角处是否相切得当。

## 第 32 轮:设置分段按钮胶囊圆角修复 (2026-10-03)

- 用户反馈(截图):部分按钮组件圆角过大呈胶囊形,要求统一为矩形小圆角;
  涉及"IME 窗口位置更新策略"与主题模式的分段按钮。
- **根因(设计预览工具 + 临时插桩定位)**:imguiex `ButtonConfiguration` 的流式
  糖方法(Elevated/Filled/Tonal/Outlined/Text/Round/Square,及 BaseConfiguration
  的 XSmall/Small/Medium/Large/XLarge)写成 `constexpr auto X() { return inner(); }`
  ——`auto` 推导把内层返回的 `ButtonConfiguration&` **decay 成按值拷贝**。
  `config.Tonal().Square()` 中:`Tonal()` 先在 config 上改对 colors,再返回
  config 的 32 字节栈拷贝;`.Square()` 改在拷贝上、临时随即丢弃 → `shape`
  永远停在默认 Round(胶囊,圆角=h/2),colors 却生效。插桩实锤:
  Shape() 的 this=…3F0 vs 调用点 &config=…410,相差 0x20=sizeof(副本)。
  `Size()` 因显式 `-> Derived &` 幸免,故 size=XSMALL 生效而 shape 失效;
  源码里的 `.Square()` 一直没生效,游戏与预览一致渲染胶囊。
- **修改(仅 extern/JamieMods/imguiex/imguiex/imguiex_m3.h)**:12 个糖方法
  全部补显式尾置返回类型 `-> Derived &` / `-> ButtonConfiguration &`,
  链式调用不再产生拷贝;应用侧 4 处 `config.Tonal().Square()`(策略行、
  主题模式行,及预览镜像 ×2)无需改动即恢复。
- 验证:设计预览 capture 实测 rounding 16→4(ExtraSmall 4dp),分段按钮呈
  矩形小圆角、"无"不再近圆(shots/policy-fixed.png);主构建 EXIT 0;
  SimpleIMETest **33/33**;build / dist / MO2 三处 DLL md5 一致(c3aa762c…)。
- 待游戏内实测:外观页两处分段按钮(IME 位置策略、主题模式)圆角观感。

## 第 32 轮:候选窗口左右留白压缩 (2026-10-03)

- 用户反馈(第 31 轮后截图):高度已收紧,但左右两侧仍有空白,再略微缩小。
- **根因**:①窗口水平 padding 沿用了第 31 轮的"圆角安全区 8dp + List gutter
  16dp = 24dp"(原 ListItem 的水平缩进),比纵向的 8dp 宽;②chip 循环在最后
  一个 chip 之后仍调用 `SameLine()`,光标多前进一个 ItemSpacing(8dp),
  AlwaysAutoResize 把它计入了窗口宽度 → 右侧 32dp / 左侧 24dp,不对称且偏宽。
- **修改(仅 src/ui/ImeWindow.cpp)**:
  - 窗口 padding 四边统一为圆角安全区 8dp(圆角曲线仅在贴到窗口边缘处达到
    16dp,行背景距边缘 8dp 不会盖到圆角);
  - `DrawCandidates` 的 `SameLine` 只在 chip 之间调用(跳过首个),行尾不再
    多出一个间距。
- 验证:主构建 EXIT 0;SimpleIMETest **33/33**;build / dist / MO2 三处 DLL
  md5 一致(5f28d1f8…)。
- 待游戏内实测:左右留白是否到位;首个/末个 chip 与圆角边缘的观感。

## 第 25 轮:SKSEMF 首测失败修复——WantTextInput 偏移按 DLL 实际内嵌 ImGui 版本重推 (2026-10-03)

> 用户实测"不能在 SKSE 框架中中文输入"。日志:bridge ready (framework 3.80) 正常,
> 但全程无一条 "text session begin" —— 会话从未建立,提交文本走了旧 Scaleform
> 路径被吞。IME enable 刷屏为用户操作 SimpleIME 自身设置窗所致,与本失败无关。

### 根因
- WantTextInput 偏移 0xCC 是从 QTR SKSEMF3 仓库(ImGui 1.92,666 键,IO=0xBD0)推导;
  用户安装的 SKSEMenuFramework.dll 3.8 正式版内嵌 **Dear ImGui 1.90.8 (19080)**
  (从 DLL 二进制版本字符串确认),其 ImGuiIO 布局不同:
  offsetof(WantTextInput)=0xC4、sizeof(ImGuiIO)=0x38E0(官方 v1.90.8 imgui.h
  offsetof 探针实测)。0xCC 处是相邻标志字节,恒 false → 租约/路由/过滤全部不启动。

### 修复
- 偏移 0xCC → **0xC4**;注释更新为"按 DLL 内嵌 ImGui 版本推导 + 两版数值对照"。
- 新增首帧诊断日志:回调首次触发时 info 一次 "SKSEMF render callback alive
  (first event, type=N)" —— 下次若再无会话,可区分"回调未触发"与"偏移又失配"。

### 教训
- 框架 GitHub 仓库(两个)都落后于发行 DLL,ABI 证据必须取自用户机器上的二进制
  (导出表 llvm-objdump + 内嵌版本字符串),偏移用对应官方版本头文件编译 offsetof 探针。

### 产物
- 构建 EXIT 0;DLL md5 `a0e07922…`,build / dist / MO2 三处一致。待用户复测。

## 第 33 轮:删除设置窗"更改立即生效"提示 (2026-10-03)

- 用户反馈侧边栏底部常驻的 SaveHint 提示文字影响美观,整段删除:
  - 桌面布局:导航卡片底部 pinned 提示块(含防溢出高度判断)整体移除;
  - 紧凑模式:导航下拉框下方的提示行移除,保留 Divider + dp4 间距。
- `Settings.SaveHint` 键从 6 个语言 toml 全部移除(代码已无引用,避免死键)。
- 构建 EXIT 0(ToolWindow.cpp 重编+链接);DLL(22:04)+ 翻译 toml 已同步 MO2。
- 待游戏内实测:侧边栏底部留白观感(原提示区域现为纯空白)。

## 第 26 轮:SKSEMF 二次排查——偏移锚定到发行 DLL 真实 ABI + 运行时遥测 (2026-10-03)

> 用户复测仍无会话。日志:bridge ready (3.80) + 回调首帧确认(type=3),仍无 session begin。

### 关键反查(全部基于用户机器上的 SKSEMenuFramework.dll 3.8)
- **反汇编导出函数锚点**:ImGuiIO_AddInputCharacter(_UTF16) 实测访问 io->Ctx@0xF0、
  AppAcceptingEvents@0x2BB9、InputQueueSurrogate@0x2BBC。
- 官方 imgui.h 各版本(1.90.8/1.90.9/1.91.0-1.91.9)逐一 offsetof 探针,无一匹配 →
  发行 DLL 内嵌定制构建(字符串自称 1.90.8)。
- **拿到框架自己的合并头(fatalCMD/walk-with-me vendored 的 SKSEMenuFramework.h,
  即 TMS 桥编译所用镜像)**:ImGuiMCP::ImGuiIO 给出 Ctx=0xF0 ✔、AppAccepting=0xBB9、
  Surrogate=0xBBC —— 与 DLL 锚点差恒为 0x2000,且 0x2000 恰为 KeysData 数组从
  [NamedKey_COUNT=154] 扩成 [ImGuiKey_COUNT=666] 的增量(512 键×16B)。KeysData 位于
  Ctx 之后,WantTextInput(0xCC)在 Ctx 之前不受影响。
- **结论:WantTextInput=0xCC(最初值)本来就对,第 25 轮改 0xC4 反而改错了**。
  两次失败同表象的真因是:该标志在用户测试期间从未变 true(会话从未建立)。
  0xC4 读的是邻近标志字节,恒 false,与 0xCC"正确但标志恒 false"无法从日志区分。

### 本轮改动
- 偏移改回 0xCC,并锚定 WantCaptureMouse=0xCA/WantCaptureKeyboard=0xCB 一并监控;
  注释完整记录锚定方法(反汇编锚点+镜像头+KeysData 0x2000 位移模型)。
- 回调遥测:[SKSEMF-io] 按变化+5s 心跳记录 ctx 指针/帧号/三标志位
  (mouse=1,kbd=2,text=4);新增各事件类型(0-4)首见日志(kOpenMenu/kCloseMenu 缺席
  =用户没打开过框架窗口)。
- 下次日志判读:flags 全 0 = 输入没进 ImGui(交付问题);kbd/text 有置位 = 字段
  已激活而会话仍未建立(桥侧问题);kOpenMenu/kCloseMenu 缺席 = 窗口未开。

### 产物
- 构建 EXIT 0;DLL md5 `f5d8f8ac…`,build / dist / MO2 一致。待用户按新流程复测。

## 第 27 轮:SKSEMF 三次修复——泄漏修复器误杀桥接租约(真因) (2026-10-04)

> 用户复测仍无法输入。遥测终于给出决定性证据:flags=7(鼠标+键盘+文本全亮)→
> session begin(lease acquired)成功 → 30ms 后 "Text-entry count 1 survived with
> no menu to own it — healing counter and forcing IME off" → 下一帧重新租借 →
> 再被杀。35 秒内 begin/heal 循环 **1137 次**,IME 在开关间振荡,永远无法组字。

### 根因
- PollTextEntryCountConsistency / FixInconsistentTextEntryCount 的启发式假设:
  "持有文本计数的必然是菜单栈上的真实(非 always-open)IMenu"。
- **SKSEMF 不注册 IMenu** —— 它通过 D3D/MenuManager 渲染调用点钩子直接画界面,
  栈上永远没有它的菜单 → "no real menu on stack" 恒成立 → 我们的租约被当作泄漏。
- 30ms 就触发(而非 2s 宽限)的原因:轮询的 static 稳定计时器被此前"计数==1 但
  计数归零未被轮询观察到"的周期污染(lastSeen 残留为 1,lastChange 已超 2s),
  租借建立后首个轮询周期直接满足宽限。后续 flags 7→6→1 抖动是治疗引起的:
  EnableIme(false)→Focus(gameHwnd) 焦点回掷 → ImGui 收到失焦/清输入 →
  InputText 失活 → WantTextInput 掉 → 会话结束 → 重租 → 循环。

### 修复
- 两个修复器(每帧轮询版 + MenuOpenClose 事件版)在 `SkseMenuFrameworkBridge::
  SessionActive()` 时直接豁免:会话活跃期间租约就是计数的合法持有者(SKSEMF
  无 IMenu 在栈上是它的常态,不是泄漏)。会话结束后泄漏检测照常工作。

### 产物
- 构建 EXIT 0;DLL md5 `23a11d36…`,build / dist / MO2 一致。
- 遥测([SKSEMF-io] 按变化+5s 心跳)本轮立功,判读规则见第 26 轮;待复测确认后
  可考虑降频或移除。

## 第 28 轮:SKSEMF 四修——激活竞态泄漏首字母 (2026-10-04)

> 复测截图:输入 dans+空格后框内是 "d但是" —— 提交链路已完全打通(但是进框),
> 但会话刚建立、IME 异步启用(~20ms)未完成、模式标志未落定的窗口里,首字母 'd'
> 的 DirectInput 回声通过了模式谓词(imeDisabled=true → pass),落进输入框。

### 修复:全新启用过渡窗口
- BeginTextInput 快照 s_imeDisabledAtBegin(进入时 IME 是否禁用,即 keepImeOpen
  关闭时的常见首击场景)。
- NeutralizeRawAscii 计算 freshEnableTransition = 会话开始时 IME 禁用 且
  now-sessionBegin < 250ms;谓词新增该参数:窗口内一律拦掉可打印回声(这些回声
  对拼音用户本来就是垃圾;纯英文键盘用户最多损失进入字段后 250ms 内的首击)。
- 会话建立时 IME 已启用(keepImeOpen 用户/字段间切换)不设窗口,英文模式打字
  完全不受影响。

### 产物
- 构建 EXIT 0;测试 35/35(新增过渡窗口 4 例);DLL md5 `a2ec2ba4…`,
  build / dist / MO2 一致。遥测保留待复测确认。

## 第 29 轮:SKSEMF 五修——冻结时间下语言栏闪现即灭:焦点丢失清输入 (2026-10-04)

> 用户:仅"时间暂停"关闭时可用;开启后点输入框,IME 状态栏出现即消失。
> 新日志(暂停开启):flags=7 → session begin → IME enable(+18ms,焦点移 ImeWnd)
> → 2 帧后 text 位掉 → 随后 mouse 位掉(flags=0,= ClearInputKeys 式 MousePos
> 失效)→ 会话结束。每次点击同样 2 帧死亡,高度确定。

### 根因链
- EnableIme(true) 必须把 Win32 焦点移到 SimpleIME 的 ImeWnd(TSF 组字必需,
  游戏线程是 MTA 无法承载 TSF)→ 游戏窗口收到 WM_KILLFOCUS → SKSEMF 的
  ImGui_ImplWin32 → io.AddFocusEvent(false) → 其 ImGui 清理输入状态
  (发行 DLL 为定制构建,处理比原版激进;原版 1.90.8 只置 AppFocusLost)→
  激活中的 InputText 被失活 → WantTextInput 掉 → 会话结束。
- "时间暂停关闭能用"的原因待定(同一焦点变更照发),但 GameLock Resume 路径下
  后续清理由 GameLock::Unlocked 的 io.ClearInputKeys 承担,时序不同。

### 修复
- 就绪后一次性把框架 io 的 **ConfigDebugIgnoreFocusLoss**(ImGui 1.89+ 官方
  开关,偏移 0x7B,pre-KeysData 不受定制布局影响)置 1:AddFocusEvent(false)
  直接短路。游戏嵌入式 overlay 无视宿主焦点丢失无害(alt-tab 时框架根本不渲染)。

### 产物
- 构建 EXIT 0;DLL md5 `aa5d6df5…`,build / dist / MO2 一致。待复测;若仍闪灭,
  下一刀是 BlurBackgroundOnMenu vs FreezeTimeOnMenu 二分 + ActiveId 遥测。

## 第 30 轮:SKSEMF 六修未中——升级上下文内部遥测 (2026-10-04)

> ConfigDebugIgnoreFocusLoss 置位生效(本轮日志 mouse 位不再掉 0,上轮的
> ClearInputKeys 路径确证存在并被抑制),但 text 位仍在会话后 2 帧掉落——
> 控件失活走的是另一条通路(发行 DLL 为定制 imgui,处理不明)。

### 改动
- 遥测 v2:每次变化行追加 ImGuiContext 内部状态——active(ActiveId@ctx+0x346C,
  镜像 0x146C+0x2000)、wantNext(WantTextInputNextFrame@ctx+0x571C)、
  accepting(AppAcceptingEvents@io+0x2BB9)。
- 判读:active=0 → 控件被框架/引擎侧清除;active=1 且 wantNext=0 → 定制
  InputText 不再置位(定制构建特有);accepting=0 → 焦点丢失清输入仍在发生
  (DLL 无视该开关)→ 下一刀改在 WndProc 链拦截 WM_KILLFOCUS。

### 产物
- 构建 EXIT 0;DLL md5 `7a162809…`,build / dist / MO2 一致。待用户复测取数。

## 第 29 轮:SKSEMF 五修——TIP 激活期 NATIVE 滞后 + 字符级追踪 (2026-10-04)

> 父会话(本会话分叉前)已诊断"焦点丢失清空 ImGui 输入态导致字段两帧失活"并加
> ConfigDebugIgnoreFocusLoss(0x7B,aa5d6df5,11:19 部署);用户 11:26 复测仍报
> "问题依旧存在"。该轮日志显示会话机制健康(8 秒稳定会话、两次组字、干净结束),
> 但无法从 info 级日志判断字段内容与提交落点。

### 本轮(接父会话未构建的升级遥测之上)
- **谓词修复**:TIP 已激活(INPUT_PROCESSOR_ACTIVATED)+ 键盘开启 → 一律拦回声,
  不再依赖 NATIVE——转换模式舱在 TIP 激活时只读一次,常滞后于首次按键
  (首字母 "d" 竞态的另一个来源)。英文模式靠 Shift 关闭键盘舱放行,不受影响;
  纯英文键盘用户(无 TIP)行为不变。
- **字符级追踪**(临时,确认后移除):
  - [SKSEMF-trace] echo 'd' x1 -> NEUTRALIZE/pass (composing/disabled/kbdOpen/
    native/transition/tipActive 全量谓词输入)——每次可打印回声批一条;
  - [SKSEMF-trace] commit '但是' (N unit(s)) queued -> injected——提交链路逐块。
- 构建包含父会话未部署的升级遥测(active/wantNext/accepting)。

### 判读表(下一轮日志)
- echo 行 NEUTRALIZE 但字段仍见字母 → 泄漏不在原始路径(WM_CHAR/其他),看 commit 行。
- echo 行 pass + tipActive=0 → TIP 未激活即打字(启用竞态,过渡窗已覆盖 250ms 内)。
- commit 行存在但字段无字 → 注入被 ImGui 丢弃,看 accepting= 与 active= 遥测位。

### 产物
- 构建 EXIT 0;测试 36/36;DLL md5 `936625b2…`,build / dist / MO2 一致。

## 第 31 轮:SKSEMF 输入支持验证通过——收尾 (2026-10-04)

> 用户确认"这次没问题了":冻结时间开启,点入 SKSEMF 文本框,语言栏稳定,
> 中文正常输入。日志:29s 稳定会话,begin/end 各一次,accepting 全程 true
> (焦点丢失清理被 ConfigDebugIgnoreFocusLoss 抑制),中途一次瞬时
> WantTextInput 抖动自行恢复未影响会话。

### 确认生效的完整链路(第 24-30 轮累计)
WantTextInput 轮询(0xCC)→ 租约(原生 AllowTextInput 过自家 detour)→ IME 启用
→ 会话期间输入分发层中性化组字回声(全新启用 250ms 过渡窗 + 模式谓词)→
SendUiString 路由入队 → 框架回调线程 ImGuiIO_AddInputCharacter 注入。
三处关键免疫:泄漏修复器豁免(无 IMenu 在栈)、ConfigDebugIgnoreFocusLoss
(焦点清输入)、过渡窗口(首字母竞态)。

### 收尾
- 拆除逐帧/心跳遥测(问题解决后是噪音):[SKSEMF-io] 行、ActiveId/wantNext/
  accepting 读取、捕获鼠标/键盘偏移常量、上下文偏移常量全部移除;保留一次性
  ConfigDebugIgnoreFocusLoss 设置日志、事件类型首见日志、会话 begin/end。
- 构建 EXIT 0;测试 35/35;DLL md5 `6ea33998…`,build / dist / MO2 一致。

## 第 30 轮:SKSEMF 支持实测确认 + 诊断清理 (2026-10-04)

> 用户确认:SKSEMF 文本框中文输入正常("这次没问题了")。dans+空格 → 但是,
> 无首字母泄漏、无残留。

### 清理
- 移除临时字符级追踪([SKSEMF-trace] echo/commit 与 TraceUtf8 助手)。
- 移除 [SKSEMF-io] 遥测块(与父会话并行清理汇合;ConfigDebugIgnoreFocusLoss
  的一次性设置日志与事件类型首见日志保留在 info)。
- 保留的行为修复:修复器会话豁免、250ms 全新启用过渡窗、tipActive 拦截
  (键盘开+TIP 活跃即拦回声,不依赖激活期滞后的 NATIVE)。

### 产物
- 构建 EXIT 0;测试 36/36;DLL md5 `6ea33998…`,build / dist / MO2 一致。
- SKSEMF 原生中文输入支持至此完结。后续待办:版本号随下次发布 bump(README
  已有 Unreleased 段),游戏内完整回归(SKSEMF 各类窗口 + 与控制台/原版菜单
  共存)交用户日常验证。

## 第 34 轮:设置界面六项功能补全 + 2.5.0 打包 (2026-10-04)

> 用户提案六项全部采纳("六项都做吧"),完成后指示"打包"。

### 设置界面新增
- **输入与状态页「兼容性」卡**:Meridian/Prisma/SKSEMF 三开关进 UI + 尾随状态
  caption。新增 `Hooks::SupportState`(Pending/Off/Standoff/NotDetected/Failed/
  Active),三桥 Install(及 SKSEMF TryResolve 重试)埋点 `s_state` atomic 并暴露
  `State()`。caption 语义:已生效(绿)/重启后生效/重启后停用/未检测到/存在冲突
  (红,互斥插件)/不可用(红)。桥配置门安装期锁存,UI 翻转只写 TOML。
- **高级页**:诊断信息行(一键复制英文摘要:版本/SKSE 版本/DPI/TSF/三桥状态/
  配置校验/路径;SKSE 版本经 `Ime::Global::g_skseVersion` 于 PluginLoad 解码)+
  日志与错误提示卡(日志级别下拉 spdlog::set_level 热改;错误提示时长预设下拉,
  即时 SetMessageDuration)+ 运行环境卡(DPI/TSF 只读状态行)。
- 日志行/配置行新增「打开」链接(explorer /select 高亮配置文件);CMake 补链 shell32。
- 翻译六语各补 30+ 键(新节 Settings.Compat、Settings.Copy/Open 等);
  ConfigDescription/Page.Advanced.Support 文案更新;MockPanels 同步(100 键)。

### 产物
- 版本 2.4.0 → **2.5.0**;构建 EXIT 0;测试 36/36;DLL 内嵌 FileVersion/ProductVersion
  均为 2.5.0,md5 `c7dc603f…`,build / dist / MO2 三处一致。
- 发行包 `dist/SimpleIME-2.5.0-RelWithDebInfo.7z`(1.5 MB,含全部 6 翻译)。
- MO2 已部署 DLL + 6 翻译;用户 live TOML 未动(本轮无新增配置键)。
- 注意:CMakeLists 变更触发 reconfigure 会冲掉 build 目录 rules.ninja 的
  simple_rc 补丁,本轮已重打两次;下次 reconfigure 后构建前需再查。
- 待游戏内回归:兼容性 caption 与实际桥状态一致性、诊断复制内容、
  日志级别热改、打开链接(游戏进程 ShellExecute)。

## 第 35 轮:NirnLab UIPlatform 主后端 + Meridian 双后端化 (2026-10-05)

> 用户问"可以不用 Text-Bridge 的代码、有没有更好的方法"。结论:焦点感知有严格更优路径
> (NirnLabUIPlatform 公开 API),文本注入 execCommand 仍是 CEF 下最优原语。指示"动手实施
> 1,2,3"(UIPlatform 主路径 / View/1 降回退 / BridgeScript clean-room)。

### 调研结论(全部经源码/实测验证)
- "Meridian 1.5 UIPlatform" = **NirnLabUIPlatform**(kkEngine,MIT,当前 3.3;tags
  1.0→3.3)。其 `IUIPlatformAPI` 经 SKSE 消息 2250(RequestVersion)→2251(Response)→
  2252(RequestAPI)→2253(ResponseAPI) 协商;宿主自带控制器即用 `RegisterListener(nullptr)`
  通配监听 + 把响应按请求方 sender 名回发 —— 消费者以 "SimpleIME" 为 label 派发即可。
- **虚表重载逆序(本机两编译器实测,最重要的坑)**:MSVC cl 与 clang-cl 布局一致,但
  **同名重载虚函数在 vtable 中按声明逆序**排布:3.3 头 [1]=AddOrGetBrowser(6 参 settings)、
  [2]=AddOrGetBrowser(5 参)、[3]=ReleaseBrowserHandle、[4]=RegisterOnShutdown;1.1 无重载
  声明序 [1]=AddOrGet、[2]=Release。按声明序直接钩 slot 1 会钩到 6 参重载 → 签名错位崩溃。
  槽位表按协商主版本选择(`NirnLabApi.h::SlotsFor`);IBrowser 无重载,槽 6=SetBrowserFocused
  全版本一致。ABI 探针脚本(MSVC cl 逐槽行为探测)完成使命后已删。
- `ReleaseBrowserHandle` 释出最后引用 → MultiLayerMenu 移除子菜单 → CEFMenu/DefaultBrowser
  **同步析构**;故聚焦期间以自有 AddOrGetBrowser 引用(同名 get 路径)钉住浏览器,外部引用
  计数归零时先结束会话再解钉。
- `AddFunctionCallback` 即时经 IPC 注入当前页并在后续导航重放(executeInGameThread=false →
  宿主 CEF 线程回调,与 View/1 listener 同线程模型);回调参数为宿主 JSON 序列化串,需解码
  (`TryDecodeJsonStringArg`,含 \uXXXX/代理对 → UTF-8)。
- `AddOrGetBrowser` get 路径忽略 url/绑定参数;hook 槽在 kInputLoaded 后安装,理论上漏掉
  "他插件在自身 kInputLoaded 同步建浏览器" 的窗口(无枚举 API,接受并文档化)。

### 实现
- **vendor `extern/NirnLabUIPlatform/`**(API.h/IBrowser.h/JSTypes.h/Settings.h/Version.h +
  MIT LICENSE;JSTypes.h 依赖宿主 `<string>` 前置包含,NirnLabApi.h 负责)。
- **`include/hooks/NirnLabApi.h`**:槽位表(SlotsFor 按主版本)、版本门控(1.x–3.x 支持,
  4.x 拒绝)、SimpleIME.result 通道常量、消息号常量。
- **`src/hooks/NirnLabBridge.cpp`**(新后端):kPostPostLoad 注册通配监听+RequestVersion、
  kInputLoaded RequestAPI(2.0+ 带 Settings 载荷,1.x 无载荷);ResponseAPI 后按版本槽表钩
  IUIPlatformAPI(AddOrGetBrowser×2 + Release),首浏览器出现时钩 IBrowser 槽 6;浏览器注册表
  (handle↔browser、外部引用计数、pin);SetBrowserFocused(true)= pin+会话开始,false=解钉+
  会话结束;最后外部引用释放也强制结束会话+解钉。
- **MeridianBridge 双后端化**:会话状态增加 `s_focusedBrowser`;Target 抽象(view/browser)+ 
  ExecuteJs/TargetAlive 分派;焦点事件跨后端互清;Tick backstop 对浏览器走 `IsBrowserFocused()`
  轮询;监听注册分双路(View/1 RegisterListener / 浏览器 AddFunctionCallback,静默重挂机制共用);
  State() 合并两后端(Off→Active→Standoff→Failed→Pending→NotDetected);Text-Bridge 互斥仅限
  View/1 路径(UIPlatform 不受影响)。HasFocus/OwnsCandidateUi/ShouldRoute 对调用方不变。
- **BridgeScript clean-room 重写**:对外契约不变(`__stbSimpleIME`、capture/commit/cancel/
  theme/ui、状态字、stb-* CSS 类);内部重组(report 双通道优先 `SimpleIME.result`、泄漏守卫
  tendField、面板 showPanel/hidePanel);默认暗色 CSS 与主题推送保持逐字节一致。
- 集成:ImeApp kPostPostLoad/kInputLoaded 接 NirnLabBridge;ToolWindow 诊断加
  "NirnLab UIPlatform: 版本 (API x.y) 状态" 行。

### 产物与遗留
- 构建 EXIT 0;SimpleIMETest **48/48**(新增 ABI 槽位 4 + 版本门控 3 + JSON 解码 3);
  node 桥脚本测试全过(新增双通道断言)。
- reconfigure 已发生 → rules.ninja simple_rc 补丁两目录均已重打(main + test-check)。
- VALIDATION.md 新增 v3.0.0-beta + UIPlatform 节,游戏内验证项待用户实测(重点:UIPlatform
  mod 焦点联动、双栈共存、浏览器生命周期)。
- 疑点上报:`extern/MeridianUI/LICENSE` 为 **GPL-3**,但 vendored 头文件标 SPDX MIT、
  NOTICES 亦写 MIT —— 上游 MeridianUI(Nexus 141552)分发物自相矛盾,未擅动,待核实。

## 第 36 轮:彻底去痕 + 3.1.0-beta 打包 (2026-10-05)

> 用户确认后指示:"改名为 simpleIMEResult 之类彻底去痕,不保留技术署名,把注释里的技术署名
> 也清掉,给协议名改名",随后指示"打包"。

### 去痕(协议名 + 署名)
- 协议名全部去 stb 前缀:`window.__stbSimpleIME`→`__simpleIME`、View/1 listener 名
  `stbSimpleIMEResult`→`simpleIMEResult`、CSS 类/变量 `stb-*`/`--stb-*`→`simpleime-*`/
  `--simpleime-*`、字体名 `'STB Primary'`→`'SimpleIME Primary'`、样式表 id→`simpleime-style`、
  原始串定界符 `STBJS`→`IMEJS`。桥脚本/宿主推送脚本/主题 CSS 构建器/node 测试同步。
- 技术署名全部移除:MeridianBridge.cpp 文件头与脚本注释、MeridianApi.h、MeridianBridgeLogic.h、
  PrismaBridge.cpp、SkseMenuFrameworkBridge.cpp、MeridianAbiTest.cpp、MeridianBridgeLogicTest.cpp、
  MeridianBridge.cjs 头、THIRD-PARTY-NOTICES 整节、README 致谢行、VALIDATION.md 头部措辞。
- **保留(功能性,非署名)**:`GetModuleHandleW(L"SkyrimTextBridge.dll")` 运行时互斥检测 +
  日志 + README 兼容性章节(用户装了它时必须停用 View/1 路径防双重注入)。
- 未清洗:PROGRESS.md 开发日志与 git 历史(内部记录,如实保留)。

### 打包(3.1.0-beta)
- 版本 bump 3.0.0-beta → **3.1.0-beta**(功能轮惯例,保留 beta 后缀);reconfigure×2 目录,
  simple_rc 补丁重打(本轮共两次,version bump 与 install 规则各触发一次)。
- **install 规则修复**:NirnLabUIPlatform LICENSE 加入 third_party 时与 MeridianUI LICENSE
  同名冲突(CMake install(FILES) 静默覆盖,包内丢失 GPL-3 文本)——拆分为独立 install 调用并
  RENAME 为 `MeridianUI-LICENSE`/`NirnLabUIPlatform-LICENSE`。
- 发行包 `dist/SimpleIME-3.1.0-beta-Release.7z`(1.57 MB,md5 `ffc9c489…`):DLL+README+LICENSE+
  界面 toml×7+lucide 字体+四许可;DLL md5 `2ec8c626…`(含 15:31 并行改动的 ImeWnd 裸 Shift
  点按 中/英 切换推断),内嵌版本均 3.1.0-beta;build/dist/MO2 三处一致;解包镜像同步(含 PDB)。
- README 更新(Current 版本行、PROGRESS 指针澄清 CHANGELOG 为上游史、测试 36→48、
  兼容性列表加 NirnLabUIPlatform 与 API 主版本门控说明)并重打包。
- `dist/release-notes-3.1.0-beta.md` 新增(UIPlatform 后端/双后端化/版本门控/回调通道/去痕改名)。
- 测试:SimpleIMETest 48/48、node 桥测试全过(改名后全量重跑)。
- GitHub Release 未建(需用户指示;注意 gh 建 tag 会触发 release.yml CI 覆盖资产的老坑)。

## 第 37 轮:删库重发 SimpleIME-Unofficial-Update + v3.0.0-beta 发布 (2026-10-05)

> 用户指示:"删除现有仓库,重新发布,仓库名为 SimpleIME-Unofficial-Update,发版 3.0.0beta"。
> 闭环第 36 轮遗留的"git 历史未清洗 + GitHub Release 未建"两项。

### 历史重建
- 已推送历史(fcf36df 及更早)含 stb 前缀协议名与技术署名(10 文件命中),无法只追加提交去痕,
  按指示删除整个 GitHub 仓库后以全新历史重发。
- 旧史备份:`SimpleIME-history-backup/pre-clean-republish-20261005.bundle`(完整)+ 本地分支
  `backup/pre-clean-republish-20261005`;旧 release 资产已下载存档于同目录。
- 新历史 = 单个初始提交(orphan),树内容 = 当前工作区全量(第 35/36 轮 NirnLab 后端 + 去痕 +
  文档),不再保留含痕迹的中间版本。

### 版本归位 3.0.0-beta
- 按用户本次指示,GitHub Release 定为 **v3.0.0-beta**(覆盖第 36 轮内部打包用的 3.1.0-beta 号):
  CMakeLists VERSION 3.1.0→3.0.0(保留 beta 后缀)、README Current 行同步。第 36 轮 dist 包
  (SimpleIME-3.1.0-beta-Release.7z)为本地包,与本次线上发布号不同,以 CI 产出的
  SimpleIME-3.0.0-beta-Release.7z 为准。
- cliff.toml [commit.remote] 从上游 cyfewlp/SimpleIME 修正为本仓库(否则 release changelog
  的 commit/compare 链接指错仓库)。
- 用户追加指示"不需要说明对 Skyrim-Text-Bridge 的兼容性":README Compatibility 节 Text-Bridge
  条目删除;VALIDATION 互斥项改不点名措辞且构建记录节改写为 v3.0.0-beta;nexus/main.bbcode 删
  兼容条目、中文摘要去名、BlackMesa79 致谢行删除;MeridianBridge 注释去 rival 名。保留:检测
  DLL 字面量与 warn 日志(第 36 轮功能性决定)、PROGRESS 开发日志(如实记录)。amend 单提交后删
  release+tag 重推,CI 重跑覆盖发布。

### 打包(3.0.0-beta 本地包)
- 重配置×2 目录(RelWithDebInfo + test-check,cache 直接 `cmake -S -B` 即可)→ simple_rc 补丁
  重打(version.rc 重生成为 3.0.0-beta)→ 构建 → SimpleIMETest 48/48 + node 桥测试全过。
- 发行包 `dist/SimpleIME-3.0.0-beta-Release.7z`(1.58 MB,md5 `39e31d12…`,15 文件: DLL+README+
  LICENSE+界面 toml×7+lucide 字体+四许可);DLL md5 `542b9653…`,内嵌 ProductVersion/FileVersion
  均 3.0.0-beta;build / dist 镜像(含 PDB) / MO2 / 包内四处哈希一致。MO2 同步跳过
  interface\SimpleIME\SimpleIME.toml(用户活配置未动)。与 GitHub Release 资产同名但为本地
  RelWithDebInfo 构建(CI 资产为 Release 构建,哈希不同属预期)。

## 第 38 轮:单字符候选列表空缺修复 (2026-10-05)

### 症状与定位
- 游戏内偶发:只输入一个字母后停手,候选窗只剩组字字符,候选区持续空白
  (截图 2026-10-05 12:47,stall 快照日志证实 12:46:57 起组字挂着 8 秒+候选未出现);
  输入第二个字符即恢复。微信输入法。
- 根因(TextStore.cpp UIElement 管线,三处叠加):
  1. **锁外刷新丢失(主因)**:DoUpdateUIElement 的列表刷新只设
     m_pendingChangeFlags |= CandidateList,该标志仅由 RequestLock 尾部或
     OnEndEdit 消费;TIP 异步刷新候选列表常在文档锁外到达,单字符后无后续
     编辑会话 → 标志永远无人消费 → 渲染线程 base 副本保持空列表。
     selection-only 路径此前已修过锁外直发,列表路径漏了(不对称)。
  2. **注册不拉初值**:BeginUIElement 只缓存接口不读内容;TIP 注册时内容已
     就绪且后续不发内容更新的话首屏列表永远不来。
  3. **空页被接受**:GetCandInfo 对 pageCount==0 返回 S_FALSE 被当成功,
     接受成"开着但空"的列表,后续 selection-only 更新提前返回永不填补。

### 修复(均在 src/tsf/TextStore.cpp)
- DoUpdateUIElement 列表刷新按 m_fLocked 分流:锁内保持 pending 标志交
  post-lock 尾部,锁外直接 MarkDirty(CandidateList)(与 selection-only
  路径对称;MarkDirty 先于填列表,渲染线程 RequestUpdate 会阻塞在服务锁上,
  拷到的一定是填完的列表)。
- BeginUIElement 拿到 ITfCandidateListUIElementBehavior 后立即
  DoUpdateUIElement 拉初始列表(debug 日志记 fill HRESULT)。
- GetCandInfo S_FALSE(零页)不再当成功:Abort 元素让 TIP 带真数据重建
  (重建即触发 Begin 再拉取),info 级日志。
- selection-only 提前返回加 !empty() 守卫,空列表时落入完整重填。

### 产物
- 构建通过(警告全为 FuncTracer 既有噪音),DLL 已部署 MO2
  (E:\Skyrim AE\mods\SimpleIME\SKSE\Plugins\SimpleIME.dll),游戏内待实测。

## 第 39 轮:Prisma UI 接管 + SKSEMF 会话修复 + 候选窗层级 (2026-10-06,3.1.1)

> Nexus 用户报告(Hero Avatar HUD 172644):SKSE 菜单输入栏候选窗"闪一下就没动静"、
> 多点几次才恢复;改用 TMS 桥接只出英文。随后本机实测扩展出 PMCM 无候选框、候选窗
> 层级两个问题。用户确认四项全部修复。

### 症状与根因(逐项)
1. **SKSEMF 候选栏闪一下就死 / TMS 只出英文**:stall 看门狗 600ms 过激(真实游戏卡顿
   0.6-2.4s+,本地 10-05 日志 15:13:51/53 实证)→ force-end 释放租约 → 计数器 1→0 →
   EnableIme(false) 候选窗消失;框架回调 1ms 内重建会话 → 0→1 被 OnTextEntryCountChanged
   的 50ms 重启用去抖直接丢弃 → 计数器恒 1 再无 0→1,IME 死在"会话活跃但 IME 关闭"态。
   原版 SimpleIME 无此去抖,故原版+TMS 正常。SKSEMF 3.14 偏移复测未漂(反汇编
   ImGuiIO_AddFocusEvent:ConfigDebugIgnoreFocusLoss=0x7B、Ctx=0xF0 与 3.8 一致)。
2. **PMCM(Prisma Mod Configuration Manager,168551)无候选框**:PMCM 是纯 PrismaUI/
   Ultralight 菜单(非 SKSEMF)。PrismaUI 自带 IME 管线:游戏窗口 subclass 收 WM_CHAR→
   view、HIMC 按需关联、**WM_IME_SETCONTEXT lParam=0 故意抑制系统候选窗**、组词/候选经
   prismaIME_state 推给网页自渲染——PMCM 的 shell.html 没实现该监听 ⇒ 哪边都无候选框,
   中文只能盲打。V1 API 无枚举焦点 view 接口 ⇒ JS 注入路线不可行,改为 SimpleIME 接管。
3. **第一版接管失败("无法输入中文")**:触发器选错——(a) PMCM 根本不租借 AllowTextInput
   计数器(全程零 0→1);(b) PrismaUI subclass 吞掉自家 ImeAssociation 注册消息
   (HandleControlMessage 返回 true 直接 return) ⇒ MainWndProc 收不到;(c) IME 被压制
   期间布局看门狗把用户手动切的中文布局反复强制回英文,连原生盲打都被封死。
4. **候选窗不在最上层**:PrismaUI 在自己的 present 调用点钩子里先调原函数再绘制 view,
   调用点链上永远盖住菜单阶段绘制的 ImGui 叠加层;其钩子惰性安装(首个 view 创建时),
   调用点无稳定"排它之后"位置。

### 修复
- **去抖改延迟启用锁存**(ScaleformHook.cpp):g_pendingTextEntryEnable(连同
  g_lastDisableTime 移出类私有),被去抖的 0→1 置锁存不丢弃,由
  Hooks::Scaleform::CommitPendingTextEntryEnable()(ImeMenu 每帧轮询旁调用)在 50ms
  窗口过后提交:计数器仍开且 IME_DISABLED 才 EnableIme(true);1→0 清锁存(ESC 关窗
  抖动语义不变)。
- **看门狗 600→3000ms**(SkseMenuFrameworkBridge.cpp);>3s 极端情况由锁存自动恢复。
- **Prisma 接管**:门控反转(EnableIme 仅 IsUnavailable 时压制);触发器=
  PrismaBridge::Refresh 的 hasActiveFocus 转换(≤500ms 轮询)→ SyncImeState;
  **IsShouldEnableIme 纳入 prismaAvoidance && ShouldRoute()**(防 WM_NCACTIVATE 同步
  中途杀掉会话);OnAssociationMessage 区分 wParam(关联/解除;实际收不到,死代码防御)。
- **提交路由**:SendUiString 在 Meridian 后 SKSEMF 前插 PrismaBridge::ShouldRoute →
  QueueText=逐 UTF-16 单元 PostMessageW(gameHwnd, WM_CHAR)(Prisma subclass 自带代理
  对重组,ShouldQueueChar 不滤 CJK,lParam=0 仅影响 repeat 位);s_gameHwnd 由
  ImeWnd::OnCreated SetGameHwnd 提供。
- **编辑键转发**:ImeWnd WM_KEYDOWN/WM_KEYUP 对称转发 VK_BACK/RETURN/DELETE/TAB/
  方向/HOME/END(ShouldRoute 且非组词时;组词中方向键归候选导航不转发)。
- **ToolWindow 防抢**:SendUiString 顶部 toolWindowShowing 为真跳过全部桥接路由走
  Scaleform 回退(同类冲突 Meridian/SKSEMF 一并修掉)。
- **候选窗层级**:新增 IDXGISwapChain::Present 虚表钩子(ImeApp::SwapChainPresentHook,
  slot 8,FunctionHook 原始地址 ctor):先绘制后调原始 Present ⇒ 必然在所有调用点钩子
  (Prisma/SKSEMF)之上;仅 ShouldRoute 时驱动,其余场景层级不变;OMGetRenderTargets
  空时钉后台缓冲 RTV(GetBuffer(0) 用 REX::W32::IID_ID3D11Texture2D,IID_PPV_ARGS 的
  MSVC GUID 与 REX GUID 类型不同不能隐转)。
- 设置文案:"Prisma UI 避让模式"→"Prisma UI 输入支持"(zh/en;键名 prisma_avoidance
  不动);contrib config 模板注释同步。

### 产物
- 构建 EXIT 0;SimpleIMETest 48/48。用户游戏内确认:PMCM 中文输入+候选框层级全正常,
  SKSEMF 菜单(01:49 会话)正常。
- 遗留:de/ko/ja/ru 翻译文案未同步(仍旧避让措辞);组词串贴屏幕顶缘可能被裁(搜索框
  本身在屏幕顶时),用户未再报,暂不处理。
- 版本 3.0.0-beta → **3.1.1-beta**(用户指示保留 beta 后缀;3.1.1 tag/Release 已删重发)。

## 第 40 轮:候选窗偶发出生在屏幕左上角修复 (2026-10-06)

> 用户报告:posUpdatePolicy=BASED_ON_CARET 时,候选/组词窗口有时不出现在插入符处,
> 而是出生在屏幕左上角 (0,0)。

### 根因
- `ImeWindow::Draw` 只在窗口**重新出现那一帧**(`currentFrame > m_lastShowFrame + 1`)调一次
  `UpdateImeWindowPos`;后续帧只做 Clamp 不重查。
- 该帧 `InputFocusAnchor::ComputeScreenMetrics()` **先 Reset 成 (0,0) 再查询**;Scaleform
  查询链(`Selection.getFocus` → 焦点对象 → `Selection.getCaretIndex` →
  `getExactCharBoundaries` → `TranslateLocalToScreen`×2)任一环失败(焦点尚未传播、
  caretIndex=-1、字段未布局、或压根无 Scaleform 字段=ImGui/SKSEMF/Console)都把 (0,0)
  当成插入符位置 → 窗口出生左上角并**在整个组词会话期间滞留**。
- 次要:`GetBoundsRectFrom` 逐成员局部赋值(缺 width/height 时产出半截矩形);
  `TranslateLocalToScreen` 只成功一半时混合局部/屏幕坐标;`FindActiveInputMovie`
  全扫描失败时返回 menuCount-1 把未聚焦菜单写进缓存索引。

### 修复
- **InputFocusAnchor**:`ComputeScreenMetrics()` 改返回 bool(本次是否新查得);
  失败**保留上次有效边界**绝不清零;caretIndex≥0、char 边界四成员齐全、两点
  LocalToScreen 全成功才提交缓存;全扫描失败返回 RE_ARRAY_SIZE_MAX 不再毒化缓存
  (顺带删了已无调用点的 `Reset()`)。
- **ImeWindow**:`UpdateImeWindowPos` 返回锚点是否落定;CARET 查询失败回退
  **MenuCursor**(用户刚点击处)代替 (0,0);新增 `m_caretAnchorLocked` +
  每 5 帧重试直到锁定( appearance 帧输掉焦点竞态时 ≤80ms 自愈)。
  外观帧日志追加 `caretAnchorLocked=` 字段。
- Meridian 接管路径(OwnsCandidateUi)与 BASED_ON_CURSOR/NONE 策略行为不变。

### 产物
- 构建 EXIT 0(16/16);SimpleIMETest 48/48;DLL 已部署 E:\Skyrim AE\mods\SimpleIME,
  待用户游戏内验证。

### 第 40 轮追加:左上角→左下角(光标回退被 ImGui 定位策略无视)
- 用户复测:窗口从左上角变**左下角**。日志实证:15:59:23/15:59:35 两次 SKSEMF 文本会话
  `caretAnchorLocked=false`,而 15:59:11 会话 `=true` 写入了边界缓存。
- **根因**:`FindBestWindowPosForPopupEx`(ComboBox 策略)**完全无视传入 pos**,直接用
  `avoidRect` 摆位(Down=(Min.x, Max.y) 左对齐贴下缘)→ 解锁会话拿"上次成功查询"的
  陈旧边界当 avoidRect → 窗口贴到旧字段左下方;修复前的 (0,0) 左上角其实是同一机制
  (Reset 后的退化 avoidRect),`windowPos` 赋值从来就不是最终位置。
- **修复**:
  - `ClampWindowToViewport` 增加 `caretAnchorLocked` 参数:CARET 未锁定会话用**点状
    avoidRect**(=回退位置),让 ComboBox 策略原样返回回退点;锁定会话才用真实字段边界。
  - `UpdateImeWindowPosByCaret` 在 **ImGui 系表面持有输入时直接跳过 Scaleform 查询**
    (SkseMenuFrameworkBridge::SessionActive / MeridianBridge::HasFocus /
    runtimeData.overlayShowing)——防 HUD 隐藏文本框 caret=0 造成假锁定写垃圾边界。
  - 解锁期间每帧跟随光标(MenuCursor 为空时回退 ImGui::GetMousePos,IsMousePosValid
    过滤 -FLT_MAX),每 5 帧仍重试锚点直到锁定。
- 重新构建部署(16:16),待游戏内验证:SKSEMF/设置界面输入时窗口应跟随鼠标,真实
  Scaleform 字段时仍按插入符定位。

### 第 40 轮再追加:取消鼠标跟随 + 修正 overlayShowing 误用
- 用户复测:窗口跟随鼠标移动;SKSE 菜单里候选框不显示。
- **根因**:`overlayShowing` 实为**语言栏**显示标志(所有菜单里都为 true,见
  ImeOverlay.cpp "Language bar shown"),上一轮把它当"设置界面打开"纳入压制条件,
  等于全局禁用 Scaleform 锚点 → caretAnchorLocked 永远 false → 全部走光标回退;
  加上"解锁期间每帧跟随光标"的设计 → 窗口贴着鼠标走。
- **修复**:
  - 压制条件只留 SkseMenuFrameworkBridge::SessionActive + MeridianBridge::HasFocus
    (真正无 Scaleform 插入符的表面);注释注明 overlayShowing 陷阱。
  - 取消逐帧鼠标跟随:光标位置只在**出现帧一次性捕获**,会话内位置稳定;解锁期间
    仍每 5 帧重试锚点,锁到即吸附到插入符。
  - 外观帧日志追加 imePos=(x,y),位置类问题可直接从日志定位。
- 重新构建部署,待游戏内验证:游戏内 Scaleform 字段应锁插入符;SKSEMF 字段应停在
  出现时的鼠标点(即点击的输入框处)不动。

### 第 40 轮三追加:SKSE 菜单遮挡候选框(Present 钩子门控扩展)
- 用户截图:SKSEMF 菜单(模组控制面板)输入时,候选框被框架菜单面板盖住下缘。
- **根因**:第 39 轮的 IDXGISwapChain::Present 层级钩子门控只写了
  `PrismaBridge::ShouldRoute()`,注释明确把 SKSEMF 排除在外("keeps its layering")。
  SKSEMF 框架菜单的渲染回调在 ImeMenu::PostDisplay(游戏线程画框点)之后执行 →
  框被盖。此前框定位 (0,0)/旧字段与面板不重叠,层级缺陷一直没显形;本轮框定位到
  点击处才暴露。
- **修复**:门控改为 `PrismaBridge::ShouldRoute() || SkseMenuFrameworkBridge::
  SessionActive()`——SKSEMF 文本会话期间翻转前最后重画一遍,框在框架菜单之上
  (与 Prisma 会话同机制,双绘制/互斥锁/后台缓冲 RTV 钉住全部沿用)。
- 重新构建部署,待游戏内验证:SKSEMF 输入时候选框应完整浮在面板之上。

### 第 40 轮四追加:SKSEMF 输入框锚点(候选框定位到输入框)
- 用户截图:层级已修复(候选框浮在面板上),但框出现在屏幕中部——回退用的是组词
  开始时的鼠标位置,用户点完输入框后鼠标已移开。
- **修复**:SKSEMF 桥在 WantTextInput false→true 转换(BeginTextInput,即框架输入框
  获得焦点瞬间,引擎光标必然停在刚点击的输入框上)捕获**字段锚点**
  (MenuCursor 坐标,原子存储,公开 HasFieldAnchor/GetFieldAnchor)。
  ImeWindow 的 CARET 路径:SKSEMF 会话→有锚点则锚定锚点+24px(让出输入行高度)并
  直接锁定;Meridian→仍走光标回退;其余→Scaleform 插入符查询不变。
- **同时简化 ClampWindowToViewport**:CARET/CURSOR 一律用点状 avoidRect——锚定成功
  时 pos 本来就等于 Scaleform 边界的 (left,bottom),点状完全等价;陈旧边界从此彻底
  退出定位链路(左下角类 bug 无法再发生)。
- 构建 EXIT 0;测试 48/48;已部署,待游戏内验证。

### 第 40 轮五追加:同类问题排查
- **查实并修复 2 处同类缺陷**:
  1. `UpdateWindowPosByCursor` 信任 `MenuCursor::GetSingleton() != nullptr`——SDM 单例
     恒非空,CursorMenu 关闭时坐标陈旧(与 overlayShowing 同类的"标志位/存在性误当
     有效性"陷阱)。改用 `ImGui::GetMousePos()`(imgui_manager 每帧按 CursorMenu 开合
     选好 MenuCursor/GetCursorPos-client 来源喂 io,UpdateCursorPos 在帧路径上)。
  2. Prisma 会话未压制 Scaleform 锚点查询——杂散 HUD 文本字段可假锁定垃圾边界。
     补 `PrismaBridge::OwnsInput()` 压制;已核实其语义:PrismaUI 未安装时为 false
     (Install 提前返回,unavailable 仅指"已加载但 V1 不支持"),不会误伤纯 Scaleform 场景。
  3. 桥内字段锚点捕获加与 imgui_manager 相同的 CursorMenu-open 信任闸,关闭时跳过
     (ImeWindow 落 io.MousePos 回退);补 include RE/C/CursorMenu.h。
- **排查确认无问题**:GetLastBounds/ComputeScreenMetrics 仅 ImeWindow 消费且仅新鲜值;
  g_ImGuiFrameMutex 串行 PostDisplay 与 Present 钩子帧(Scaleform Invoke 无并发);
  Meridian 网页面板自带 getBoundingClientRect 定位、无字段回退无更优锚点;Console 非
  组词目标;语言栏/设置窗在 SKSEMF/Prisma 会话由 Present 钩子整帧重画在顶层;
  ImeWnd MouseDrawCursor 已正确用 IsMenuOpen 闸。
- 构建 EXIT 0;测试 48/48;部署 17:18。

### 第 40 轮六追加:控制台飘移根因 + 框架字段真实插入符锚点
- 用户复测:控制台打字时候选框从初始位置持续垂直飘到左上角;SKSEMF 框仍未贴输入框。
- **飘移根因(日志+代码联合定位)**:上轮点状锚点改用 FindBestWindowPosForPopupEx
  (ComboBox 策略)后,其**方向记忆对点锚是致命的**——框底部出视口时翻转到 Right
  (=点上方一个框高),而 last_dir=Right 有粘性、Right 的参照点就是当前 pos 本身,
  于是每帧 pos.y -= size.y,一路爬到左上角卡住。控制台查询其实成功
  (17:26:21/25/29、17:27:09/15 locked=true, imePos=(74,138x)=控制台输入行真实位置),
  恰好触发底部翻转→飘移。
- **修复 1**:ClampWindowToViewport 弃用 FindBestWindowPosForPopupEx,改简单视口钳制
  +一次性底部翻转(翻转条件保证下一帧不再触发,方向记忆成员 m_lastAutoPosDir 删除)。
- **SKSEMF 锚点失效根因**:MCM 面板打开时**自动聚焦**搜索框(无点击),WantTextInput
  上升瞬间鼠标在别处,"点击时刻鼠标位置"启发式天然拿不到字段位置。
- **修复 2**:发现 SKSEMenuFramework.dll 导出完整 cimgui 表面(1427 个导出),其内嵌
  imgui 为 **1.90.8**(DLL 内 "Dear ImGui 1.90.8" 版本串实证)。InputText 每帧把
  **屏幕空间插入符行位置**写入 ImGuiContext::PlatformImeData(系统 IME 靠它定位)。
  用 v1.90.8 头文件编译 offsetof 探针算得偏移 **24384**(PlatformImeData 16 字节:
  WantVisible@0/InputPos@4/InputLineHeight@12,Prev=24400 互证)。桥每框架帧读取:
  WantVisible>1 视为偏移失配(一次性 warn)并回退点击启发式,永不信任垃圾值;
  WantVisible=1 时锚点=InputPos 底边(+行高);=0 时锚点失效(不越过所属字段)。
  BeginTextInput 的 MenuCursor 点击捕获仅作偏移失配后的回退。
- kFieldAnchorOffsetY 24→12(锚点现在是插入符行底边,非字段中部)。
- 构建 EXIT 0;测试 48/48;部署 17:53。

### 第 40 轮七追加:锚点读取时机修正(kAfterRender)
- 用户复测:SKSEMF 仍 locked=false 且无偏移失配警告;控制台已正常锁定 (74,1382)。
- 分析:WantVisible 恒读 0 → kBeforeRender(3) 在框架 ImGui NewFrame 之后、widget
  提交之前派发,PlatformImeData.WantVisible 刚被 NewFrame 重置。框架有 kAfterRender
  (4) 事件(桥的首见遥测早已证实其存在),在帧循环之后派发——此时 PlatformImeData
  必为当帧 InputText 写入的值。
- **修复**:UpdateFieldAnchor 改挂 kAfterRender;加 lineHeight∈[4,200] 合理性校验
  (防偏移半对读错字段);首次读到有效锚点时打一条 "PlatformImeData caret anchor
  live" 探针日志(含 pos/lineHeight),下次运行即可确认路径是否真正打通。
- 构建 EXIT 0;测试 48/48;已部署,待验证。

### 第 40 轮八追加:PlatformImeData 运行时自校准
- 复测:探针日志与失配警告都未出现,WantVisible 在 type 3/4 两个事件点恒读 0
  (kAfterRender 帧末读仍 0)→ 排除读取时机问题,唯一解释=框架的 imgui 为改动过
  ImGuiContext 布局的 fork,固定偏移 24384 落在恒 0 字节上(1.90.8 源码实证
  InputText 每帧必写 PlatformImeData,活动字段不可能恒 0)。
- **修复:运行时自校准**——会话活跃期在 ImGuiContext [4096,25000) 每 4 字节扫描
  WantVisible 模式(bool=1 + 有限屏幕坐标 + lineHeight∈[4,200],≤64 候选);字段
  失活帧剪枝(必须回落 0);两次会话往返后唯一幸存者即真实偏移,打
  "PlatformImeData calibrated at ImGuiContext+N" 日志;3 轮无候选则判失配,永久
  回退点击启发式。校准期间锚点无效走旧回退,不影响其它策略。
- 构建 EXIT 0;测试 48/48;已部署,待验证。

### 第 40 轮九追加:校准偏移持久化(消除每次启动的校准延迟)
- 用户反馈:每次冷启动后第一次输入都未校准,延迟不可接受。
- **修复**:校准结果按框架 DLL 指纹(size+mtime)持久化到
  `Data/interface/SimpleIME/skse_menu_framework_anchor.cache`;
  TryResolve 成功(框架身份确定)时指纹匹配则直接恢复偏移——**首个会话即锚定**;
  指纹不匹配(框架更新)自动重新校准并覆写缓存。缓存错位的自愈:字段活跃但
  WantVisible 连续 ~180 帧为 0 → 弃缓存重新校准。校准收敛后 FinishCalibration
  统一写缓存(两处成功点合并)。
- 构建 EXIT 0;测试 48/48;已部署。

### 第 40 轮十追加:内置已知框架偏移表(分发即用)
- 用户问:分发给别人,别人第一次输入也偏吗?——会(缓存是本机运行期产物)。
- **修复**:内置已知框架构建的偏移表 kKnownImeOffsets(键=DLL 字节数+框架版本×100):
  {4583936, 380, 24864}(SKSE-Menu-Framework 3.80 内嵌 cimgui 1.90.8 漂移布局)。
  TryResolve 时恢复顺序=本机缓存(精确指纹)→ 内置表 → 运行时校准。同框架构建的
  新用户首次输入即锚定;未知构建走运行时校准一次并入缓存。错表自愈:活跃期
  WantVisible 连续 ~180 帧为 0 自动重校准。
- 构建 EXIT 0;测试 48/48;已部署。

### 第 40 轮十一追加:SetPlatformImeDataFn 钩子——确定性锚点,免扫描免校准免缓存
- 用户:缓存方案实用性太低。根因升级发现:**框架的 ImGuiIO/ImGuiContext 布局整体
  相对官方 1.90.8 漂移**——探针 offsetof(ImGuiIO,WantTextInput)=196 vs 运行时实证
  0xCC(204)、ConfigDebugIgnoreFocusLoss 同 +8,一切按官方头文件算的偏移必然错位
  (这正是扫描校准被迫存在的原因)。
- **确定性方案:钩 io.SetPlatformImeDataFn**。ImGui 帧末在数据变化时以
  `&g.PlatformImeData` 为参调用该指针(1.90.8 imgui.cpp:5116 实证;框架 DLL 内嵌
  imgui_impl_win32,槽位指针指向框架模块)。官方 vanilla offsetof=184,夹在两个
  实证锚点(0x73/0xC4)之间 → 框架槽位=184+8=**192**;安装前校验槽内指针必须属于
  SKSEMenuFramework.dll 模块(证明偏移正确),失败则不碰、走校准/点击回退。
- 钩子事件式维护锚点生命周期:字段激活→锚定,插入符移动→更新,失活→失效。
  UpdateFieldAnchor(kAfterRender 轮询读)与 BeginTextInput 点击捕获降级为钩子
  未装时的回退;缓存/内置表保留但仅服务校准回退路径。
- 构建 EXIT 0;测试 48/48;已部署。预期日志:"Hooked ImGuiIO::SetPlatformImeDataFn
  at io+192"。
- 版本 3.1.1-beta → **3.1.2-beta**（10-07 修复：Prisma 候选窗锚点点击钉定+逐帧跟踪；中/英模式记忆与跨会话持久化 last_native_conversion；Explorer 日志定位走物理路径）。
