# 发布验证清单

> 每个发布版本一节,记录实测环境、
> 已验证项、**明确未验证项**与用户确认记录。发布前必须逐项核对;未验证项要在发布说明中如实声明。

## 约定

- 每节对应一个构建产物(记录 DLL 字节数与 md5,与 build / dist / MO2 三处核对一致)。
- "✅"= 已在真实游戏环境验证;"🧪"= 仅离线测试(单测/构建)通过;"⬜"= 待验证。
- 用户确认一节只有用户本人回复后才能勾选。

## v3.1.7-beta + 中/英模式记忆 + 无组字直写投递 + Caps 模组兼容（2026-10-16，CI 构建）

**改动**：三连用户反馈（① Google 日本語入力半角英数打不出字 → `m_fCompositionActive` + `DeliverDirectInsert`；② Kill Caps Lock NG 失效 → `CharDelivery::IsCapsBlockingModuleName` + `NormalizeCapsCase`，两条按键直通路径共用；③ 记住"上次确认文字时的输入模式" → `State::RememberConversionMode` + 三处上屏盖章 + 会话起点写回），
以及裸 Shift 预测的 revision 判据（第八轮）与一轮审查修复（53 轮：会话起点镜像回弹、死掉的 `MaybeReassertRememberedMode`、revision 的 open/close 盲区、Imm32 后端盖章）。
**产物**：GitHub Release `v3.1.7-beta`（CI 构建，内嵌版本 3.1.7-beta；7z 含 GPL LICENSE 与 README）。

### 离线（已通过 🧪）

- ✅ 主 DLL 构建 EXIT 0（内嵌 3.1.7，`__SimpleIMEPlugin.cpp` 的 `REL::Version{3,1,7,0}`；POST_BUILD 已同步 MO2，构建树与 MO2 的 DLL md5 一致 `b664dad1…`，4996608 字节）。
- ✅ SimpleIMETest **71/71**（原 63 + `CharDeliveryLogicTest` 5 例：ASCII 字母判定 / Caps 归一化 / 非字母不动作 / 模组名匹配与不匹配；`ConversionMemoryLogicTest` 3 例：自发布值不算证据 / 只有 NATIVE 位可持久化 / 先验来源优先级）。
- ⚠️ 版本 bump 会触发自动 reconfigure，**必须带 `MO2_MODS_PATH`**（否则 POST_BUILD 同步块静默消失——本轮实测复现，用 `build/_reconfigure.cmd` 重配后恢复）。

### 游戏内（用户确认 ✅）

- ✅ Kill Caps Lock NG 兼容：日志 `Caps Lock toggle blocker detected: KillCapsLockNG.dll`；开着 Caps 打 `A A S A S D` 全部 `normalized to 0x64/0x73/0x61` 转发，Caps 关掉后不再归一化。
- ✅ 裸 Shift 预测不再抢写：首次 Shift 即翻且不闪（日志 6 次 `the IME published its own 中/英 toggle; the prediction stands down`、0 次预测翻显示）。
- ✅ 首次进游戏的语言栏先验：用户确认"问题已解决"（判据日志 `Persisted the last confirmed conversion mode …`）。
- ✅ 中英恢复侧时序（53 轮）：用户"没发现有明显问题"；日志三条判据全过——每次 `IME enabled` 前后 7 行内**无** `Mirrored conversion mode`（会话起点回弹已消除，也不再紧跟 `Re-activated the active input profile`）；8 次镜像逐一对应真实 Shift 切换；`right after the session start` 0 次（= 被测输入法照我们的写入执行，re-assert 是有待触发的保险）。

### 明确未验证（⬜）

- ⬜ **无组字直写投递**（`DeliverDirectInsert`）：整份日志 0 次 `Direct (composition-less) insert`——需要 Google 日本語入力**半角英数**模式才会走这条路径；现有实机轮次用的是微软拼音/微信，未覆盖。Caps 归一化在该路径上的表现同样未覆盖。
- ⬜ **Imm32 后端**（`enable_tsf = false`）的盖章：新增的 `Imm32TextService::StampRememberedConversionModeFromLive` 仅离线编译通过。
- ⬜ 发行版（CI Release 配置 DLL）最终复核：以上任一项在发行版上重跑。

### 已知问题（未修）

- ⬜ 微信输入法不读也不写任何 TSF 隔间 ⇒ 会话起点的模式只能是"我们记住的模式"这一最优猜测；在游戏外改过模式仍会不一致（无系统接口可读，架构固有）。
- ⬜ 任务栏的中/英小标滞后一拍（架构固有：小标在触发刷新的按键进入队列时就读取状态，早于输入法切换与我们的回写；poke / 二次叫醒 / 重新激活 profile 均实测无效）——以本模组自己的语言栏与打字行为为准。
- ⬜ 自绘 ImGui 宿主通道上的直通 ASCII 双打（v3.1.6 起记录，未修）。

## v3.1.6-beta + HDR 浮层 PQ 编码 + SKSEMF 直通 ASCII 投递（2026-10-13，CI 构建）

**改动**：HDR 浮层 PQ 着色器管线（`HdrOverlay` 新模块 + `ImeApp` 接线）、SKSEMF 直通 ASCII 单路径契约（`ShouldInjectCodepoint`）。
**产物**：GitHub Release `v3.1.6-beta`（CI 构建，内嵌版本 3.1.6-beta；7z 含 GPL LICENSE 与 README）。

### 离线（已通过 🧪）

- ✅ 主 DLL 构建 EXIT 0（内嵌 3.1.6-beta；POST_BUILD 已同步 MO2，构建树与 MO2 的 DLL md5 一致 `ece02445…`）。
- ✅ SimpleIMETest **63/63**（新增 SKSEMF 注入契约 2 例、HDR 逻辑 11 例——含用 D3DCompile 真实编译 PQ 着色器）。
- ✅ Node 直测内嵌桥脚本（`test/MeridianBridge.cjs`）全绿。

### 游戏内（两项均用户确认 ✅；发行版复核待做 ⬜）

- ✅ HDR 浮层颜色：社区着色器 "HDR Display" 开启时候选栏不再过曝，颜色与引擎 UI 一致（用户确认"颜色没问题了"；日志锚点 `[HDR] overlay color transform active: HDR10/PQ, paperWhite=203 nits, uiBrightness=1`）。
- ✅ SKSEMF 直通 ASCII：MCM Bridge 改名框可正常输入英文、`-`、`+`、空格与符号（日志实证：转发 `+`×9、`_`×9、`#`、`@`、`` ` `` 与英文单字；中文提交与候选窗正常）。
- ⬜ 发行版（CI Release 配置 DLL）最终复核：以上两项任选重跑。
- ⬜ 回归：HDR 关闭 / 无社区着色器环境下 8 位背缓冲走原路径（sRGB 直通）无变化。

### 已知问题（未修）

- ⬜ 声明宿主通道（自绘 ImGui 模组界面）上直通 ASCII 会双打：引擎原始事件与提交队列各送一份；中文输入不受影响。

## v3.1.5-beta + SKSEMF 会话死亡收尾 + 原生 ImGui 宿主输入（2026-10-12，CI 构建）

**改动**：present 路径 backbuffer 钉定（`6e7079f`）、SKSEMF 宿主鼠标位置直写 + 会话遥测（`4506718`，io+248 运行时校验）、声明宿主通道路由 + 提交键回显/取消清理（`acda9ea`）、逐界面投递路由（`2f34a38`）、设置窗暂停计数器化（`8332be4`）。
**产物**：GitHub Release `v3.1.5-beta`（CI 构建，内嵌版本 3.1.5-beta；7z 含 GPL LICENSE 与 README）。

### 离线（已通过 🧪）

- ✅ 主 DLL 构建 EXIT 0（RelWithDebInfo-clangcl，内嵌版本 3.1.5-beta，POST_BUILD 已同步到 MO2 mod 目录）。
- ✅ SimpleIMETest **50/50**。
- ✅ Node 直测内嵌桥脚本（`test/MeridianBridge.cjs`）全绿。

### 游戏内（部分已通过，待用户确认 ⬜）

- ✅ 候选窗在 SKSE Menu Framework 菜单之上可见（`6e7079f`；用户实测确认"候选栏出来了"）。
- 🧪 SKSEMF 宿主鼠标位置直写：19:23 日志一次数据点——io+248 校验通过、会话中一次点击后字段仍是活动项（activeId 跨点击帧保持）、会话由用户 Esc 结束；用户终验 ⬜。
- ⬜ 声明宿主（自绘 ImGui、不注册菜单的模组）中文投递：中文进入宿主自己的输入框而非 SimpleIME 浮层；候选窗不被其菜单盖住。
- ⬜ 提交键回显补偿：提交后字段无多余字符（"mei" + 空格提交"没" → 字段只有"没"）。
- ⬜ Esc 取消组字：字面里不残留拼音字母（不再留下 "mei"）。
- ⬜ 设置窗暂停语义：打开设置不再让游戏暂停、不逼关宿主界面。
- ⬜ 发行版（CI Release 配置 DLL）最终复核：以上任选若干项重跑一遍。

## v2.3.2 + Meridian/Prisma(2026-10-01 构建,未发布)

**产物**:SimpleIME.dll(md5 `e9fae88e…`),build / dist / MO2 三处一致,内嵌版本 2.3.2。
**环境**:Skyrim AE 1.6.1170(SE 1.5.97 未测),MO2,E:\Skyrim AE。

> **2026-10-01 首次游戏实测结论**(日志 `D:\Documents\My Games\...\SKSE\SimpleIME.log`):
> Meridian 焦点钩子与 IME 启用链路全部正常(10:57:24 view 3 gained focus → 43ms 后 IME enabled),
> 但 **IME 线程活动 TIP 停在"美式键盘"** ——本会话启动期的 WM_NCACTIVATE 禁用在用户首次输入前发生,
> `m_lastActiveProfile` 为空,启用路径静默"保持当前"(英文),导致无组合、无候选框、只有英文字母。
> 日志中 微信输入法↔美式键盘 的反复切换即用户手动 Win+Space 拉锯;10:57:48 的 IME disabled
> 与 10:57:55 的重新启用是 Shift+Tab 触发的 Steam 覆盖层开关(非 Meridian 路径缺陷)。
> 另:该次实测运行的是上一轮 DLL(内嵌 JS 因原始字符串截断而残缺,见第 9 轮)。
> **2026-10-01 第二次实测结论**(新 DLL e9fae88e):TIP 回退生效("activating the user's IME
> '微信输入法'"),IME 启用正常,但仍无候选框。**真正根因:点击把 Win32 焦点从 ImeWnd 抢到游戏
> 窗口**——组合只发生在 ImeWnd,而游戏线程被 DoEnableMod 摘除了 IME 上下文,点击后键永远到不了
> 组合面。控制台不需要点击所以从未暴露;Meridian 是点击驱动 UI 必踩。用户 Win+Space 拉锯即症状。
> **修复**:Meridian 会话期间游戏窗口一获得焦点(WM_IME_SETCONTEXT TRUE)即经
> `SimpleIME.ReclaimImeFocus.v1` 延迟把焦点还回 ImeWnd(鼠标按位置派发,游戏/Meridian 鼠标交互
> 不受影响);`EnableIme(true)` 回退分支增加"已有 TIP 活跃则跳过"去抖。产物 md5 `797f0753…`。

### 离线(已通过 🧪)

- ✅ 插件构建 EXIT 0,内嵌版本 2.3.2, warnings 仅剩既有 `__uuidof`/vendored toml 两类。
- ✅ SimpleIMETest **26/26**(既有 16 项 + 新增 10 项:Meridian ABI/协商 4 项、
  会话协议纯逻辑 6 项;含 meridian/prisma 配置默认值与双向转换断言)。
- ✅ Meridian ABI/协商测试:`View/1` 虚表 slot 9 = TryFocus(MSVC 兼容布局哨兵断言)、
  协商 helper 的接受/拒绝/空指针分支。
- ✅ Meridian 会话协议纯逻辑:listener 载荷分类(ready/inserted/no-field/failed/陈旧会话忽略)、
  JsString 转义(引号/反斜杠/换行/U+2028/U+2029/代理对)。
- ✅ Node 直测内嵌桥脚本:Unicode 提交、注入样例落地为字面文本、过期会话/字段、只读/密码拒绝、
  cancel/blur/iframe、execCommand 失败回报、重复注入幂等(**并借此发现并修复了原始字符串
  终止符吞掉 IIFE 收尾括号、DLL 内 JS 残缺的发布级 bug**)。

### 游戏内(待验证 ⬜)

- ⬜ Meridian 焦点联动:打开 Tailor(或任一 Meridian 界面)→ 日志出现
  "Meridian view … gained focus" 与 "IME enabled";关闭 → "lost focus" + IME 关闭。
- ⬜ Meridian 中文输入:候选上屏文字逐字出现在 Meridian 网页输入框,搜索/命名框 input 事件正常。
- ⬜ Meridian 英文直输:IME 开启状态下英文经 ImeWnd WM_CHAR → 桥提交,无双重字符。
- ⬜ 焦点切换/菜单关闭竞态:组合中直接 Esc 关闭 Meridian 菜单 → 无崩溃、无残留文本队列
  (日志可见 "lost focus … cleaning up")。
- ⬜ 提交回执超时路径:极难人工触发;如有 "commit acknowledgment timed out" 日志出现即为路径生效。
- ⬜ Prisma 避让:装 Outfit Wheeler,Prisma 界面持焦时 IME 无法启用(日志 "IME enable suppressed:
  Prisma UI owns input");Prisma 释放后控制台/文本框输入恢复正常。
- ⬜ Prisma 关联握手:打开/关闭 Prisma 文本框时日志出现 "PrismaUI associated its IME context"。
- ⬜ 回归:SkyUI 控制台/Modex 等 Scaleform 输入、粘贴、语言栏、F2 设置窗口、读档后状态,全部与
  v2.3.2 基线一致。
- ⬜ 互斥:安装其他同样占用 Meridian View/1 注入路径的输入类 mod 时,SimpleIME 日志明确记录
  "Meridian input support stays off" 且无双重上屏(Prisma 避让仍工作)。

### 配置开关

- `[input] meridian_support`(默认 true)/ `[input] prisma_avoidance`(默认 true)。
- 两个开关置 false 均应完全关闭对应功能(日志确认)。

## v3.1.4-beta + 清理重构（2026-10-08，CI 构建）

**改动**：精简重构 L0–L4（`docs/CLEANUP-PLAN.md`，`7ceebcd` 起约 45 提交）：死码删除、Hooks/UI/核心层去重单源化、ImGui 输入态修复独立 TU、BridgeScript 移头 + Node 自测激活、长函数拆分、仓库卫生（PROGRESS 归档 / ADR-0003）。功能侧：disable 不再切语言 profile（`switch_english_layout_on_disable`，默认关，防 ENG 泄漏桌面；`e3449b7`）；`NirnLabBridge::s_state` 竞态修复（`62e826a`）；配置回归网强化（26/26 随机覆盖 + 往返不动点测试）。
**产物**：GitHub Release `v3.1.4-beta`（CI 构建，内嵌版本 3.1.4-beta；7z 含 GPL LICENSE 与 README）。

### 离线（已通过 🧪）

- ✅ 主 DLL 构建 EXIT 0（RelWithDebInfo-clangcl；Release 配置由 CI 构建）。
- ✅ SimpleIMETest **50/50**（新增 settings 往返不动点测试）。
- ✅ Node 直测内嵌桥脚本（`test/MeridianBridge.cjs`）全绿：Unicode、注入样例、陈旧会话/字段、只读/密码、cancel/blur/iframe、overlay ui/pick/hide。
- ✅ 重建 TU 警告与基线逐条吻合、零新增。

### 游戏内（分档验证已通过 ✅，发行版复核待做 ⬜）

- ✅ L0（死码删除）：正常输入无异常（用户确认）。
- ✅ L1 Hooks（四路输入与候选框）：控制台 / Meridian / SKSEMF / Prisma 逐项对照（用户确认）。
- ✅ L1 UI（F2 设置窗四页签、语言栏、候选窗横/竖、悬停高亮）：截图对照（用户确认）。
- ✅ L1 Core（组合中 Esc、Win+Shift+S 切窗、读档、Shift 中英切换）：用户确认。
- ✅ L2（窗口过程拆分 / 候选药丸 painter / 字体构建页结果卡 / `_DEBUG` 覆盖层在）：用户确认。
- ✅ disable 不切语言 profile：游戏内验证 ENG 不再残留任务栏（特性开发轮次）。
- ⬜ 发行版（CI Release 配置 DLL）最终复核：以上任选若干项重跑一遍。

## v3.0.0-beta + NirNLab UIPlatform 后端 + 去痕改名(2026-10-05)

**改动**:Meridian 输入支持新增 NirnLabUIPlatform 主后端(SKSE 消息 2250–2253 协商 →
AddOrGetBrowser/ReleaseBrowserHandle/SetBrowserFocused 公开槽位钩子 → AddFunctionCallback
页面回调通道),View/1 槽 9 钩子降级为老版本回退;BridgeScript 以 SimpleIME 自有实现重写
(协议形状不变,report 走 `SimpleIME.result`/`simpleIMEResult` 双通道)。聚焦期间以自有
AddOrGetBrowser 引用钉住浏览器,防止宿主析构竞态。
**去痕(用户指示)**:协议名改名(`__simpleIME`/`simpleIMEResult`/CSS
`simpleime-*`/字体 `SimpleIME Primary`),注释与 NOTICES/README 技术署名全部移除,
兼容性说明删除(运行时互斥检测保留,不点名);版本 **3.0.0-beta**。
**产物**:GitHub Release 资产 `SimpleIME-3.0.0-beta-Release.7z`(CI 构建,Release 配置);
内嵌 ProductVersion/FileVersion 均为 3.0.0-beta。

**实测虚表布局(重要,两编译器一致)**:MSVC/clang-cl 对**同名重载虚函数按声明逆序**排布 ——
API 2.0+ 的槽位为 [1]=AddOrGetBrowser(6 参 settings 重载)、[2]=AddOrGetBrowser(5 参)、
[3]=ReleaseBrowserHandle;1.x 无重载为声明序 [1]=AddOrGetBrowser、[2]=Release。槽位表按
协商到的 API 主版本选择(`NirnLabApi.h SlotsFor`),绝不可按声明序假设。

### 离线(已通过 🧪)

- ✅ 主 DLL 构建 EXIT 0(SimpleIME.dll 链接成功);SimpleIMETest **48/48**(新增:
  NirnLab ABI 槽位探针 4 项 —— IBrowser 槽 6=SetBrowserFocused、3.3 槽 1/2/3、1.1 槽 1/2;
  版本门控 3 项 —— 支持 1.x–3.x、4.x 拒绝、重载存在性;JSON 串参数解码 3 项)。
- ✅ Node 直测内嵌桥脚本(clean-room 重写版):原 20+ 断言全过(Unicode、注入样例、过期
  会话/字段、只读/密码、cancel/blur/iframe、overlay ui/pick/hide),新增双通道 report 断言
  (SimpleIME.result 优先、simpleIMEResult 回退、双通道并存无重复、无通道不抛异常)。

### 游戏内(待验证 ⬜)

- ⬜ UIPlatform 焦点联动:打开任一 UIPlatform 界面(如 SkipQuestNG)→ 日志依次出现
  "NirnLabUIPlatform x.y detected (API x.y)"、"UIPlatform focus backend installed"、
  "UIPlatform browser … gained focus" 与 IME 启用;关闭 → "lost focus" + IME 关闭。
- ⬜ UIPlatform 中文输入:组合串/候选浮层出现在页面输入框锚点,上屏文字逐字进入 DOM,
  漏字补偿(拼音回删)正常。
- ⬜ UIPlatform 提交回执:SimpleIME.result 通道载荷到达("capture session N ready"),
  静默重注册路径(4 次静默后重挂绑定)不触发即最佳。
- ⬜ 浏览器生命周期:界面关闭(最后引用释放)→ 日志 "lost focus" 或 "last external
  reference was released",无崩溃;同一界面重开输入正常。
- ⬜ 双栈共存:同时装 MeridianUI.dll(View/1 mod)与 NirnLabUIPlatform.dll 时两后端
  会话互不干扰,焦点切换日志正确("focus moved from … to …")。
- ⬜ View/1 回退回归:Tailor(View/1)输入行为与上一版一致。
- ⬜ 版本门控:若用户环境为 NirnLab 1.x,日志报出实际版本且按 1.x 槽位表钩挂("slots 1,2");
  若为 4.x(未来),日志 "API version 4.x is not supported" 且功能优雅关闭、游戏稳定。
- ⬜ 回归:诊断剪贴板新增 "NirnLab UIPlatform" 行且版本正确;SkyUI/SKSEMF/控制台输入与
  既有基线一致。

### 配置开关

- `[input] meridian_support` 同时门控两个 Meridian 后端(UIPlatform + View/1),默认 true。
