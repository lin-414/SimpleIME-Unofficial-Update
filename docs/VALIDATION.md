# 发布验证清单

> 每个发布版本一节,记录实测环境、
> 已验证项、**明确未验证项**与用户确认记录。发布前必须逐项核对;未验证项要在发布说明中如实声明。

## 约定

- 每节对应一个构建产物(记录 DLL 字节数与 md5,与 build / dist / MO2 三处核对一致)。
- "✅"= 已在真实游戏环境验证;"🧪"= 仅离线测试(单测/构建)通过;"⬜"= 待验证。
- 用户确认一节只有用户本人回复后才能勾选。

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

## v3.0.0-beta + NirnLab UIPlatform 后端 + 去痕改名(2026-10-05)

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
