# 默认输入法（Default Input Method）：可行性判断与实施方案

> 需求：用户提出「能否添加'更改默认输入法'的功能，每次进游戏自动切换到用户选定的输入法，可在设置中自行选择」。
> 本文先回答**是否可行**，再给**落地方案**，最后列出**需要实机确认的点**。
>
> **结论：可行。** 核心改动 = 一个配置键 + 把现有的"取枚举里第一个非英文 TIP"启发式换成"取用户配置项" + 一个下拉框。不动输入投递管线、不动焦点/启停时序、不动线程模型。

---

## 0. TL;DR

| 问题 | 结论 |
|---|---|
| 能枚举出玩家装了哪些输入法？ | ✅ 已经做了。`InputMethodManager::RefreshProfiles()` 每次启动枚举好，语言栏在用同一份列表 |
| 能切到指定输入法？ | ✅ `Ime::InputMethodManager::ActivateProfile(const LangProfile&)` 就是现成的；玩家今天手动从语言栏切换走的就是这条路 |
| 能持久化？ | ✅ 新增一个 TOML 键。但**不能只存 GUID**（键盘布局全共用 `GUID_NULL`），要存 `tip:{CLSID}/{GUID}` |
| 能在"进游戏那一刻"应用？ | ✅ 两个挂点：`ImeWnd::OnCreated`（早）+ `ImeManager::DoEnable`（准，玩家真要打字的那一刻，也是现有启发式的所在地） |
| 设置里能选？ | ✅ 现成的 `Panels::SettingsComboRow`；候选列表从 `IMEOverlay::Draw` 已有的 `langProfiles` 直接传进设置页，零新增跨线程读 |
| 会不会残留到桌面？ | ✅ 基本不会（v1 只走 TSF 的进程级激活，不写注册表、进程退出即失效），**前提是别为了兼容去调 IMM32 那条腿** —— 见 §3.4 |
| 真正的未知量 | ⚠️ 两个：跨语言激活是否当场生效（§9-A）、Windows 的"每个应用各自记输入法"会不会和我们抢（§3.9） |

---

## 1. 需求理解

1. 设置里从**已安装输入法列表**选一个作为"默认输入法"。
2. 每次**进入游戏**自动切到它。
3. 玩家重新进入文本框、输入法被重新启用时，若不是它也切过去。
4. **未配置时行为与今天完全一致**（零回归）。

第 4 条决定了出厂值必须是"空 = 不干预"，也是本方案所有取舍的前提。

---

## 2. 现状：输入法目前是怎么被选中的

三条事实构成可行性的地基。

**（1）枚举已经有了。** `Ime::InputMethodManager::RefreshProfiles()`（`src/tsf/InputMethodManager.cpp:156-240`）在 IME 线程启动时枚举全部已启用 profile，跳过 `!TF_IPP_FLAG_ENABLED`、`catid == GUID_NULL` 和 `IsEnabledLanguageProfile` 失败的项，并为每项取好显示名（`GetLangProfileDesc` / `GetKeyboardLayoutDisplayName`）、`langid`、`clsid`、`guidProfile`、`hkl`、`dwProfileType`，存进 `m_langProfiles`。

**（2）切换已经有了。** `Ime::InputMethodManager::ActivateProfile(const LangProfile&)`（`src/tsf/InputMethodManager.cpp:283-318`）就是对 `ITfInputProcessorProfileMgr::ActivateProfile` 的封装，带 `TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE`。玩家在语言栏点一项时走 `src/ui/LanguageBar.cpp:55` → `src/ime/ImeController.cpp:68-83` → `ImeWnd::ActivateLanguageProfile` → 这里。

**（3）"进游戏时自动选一个"其实已经在做了，只是判据很粗。** `Ime::InputMethodManager::ActivatePreferredImeProfile()`（`src/tsf/InputMethodManager.cpp:386-401`）：

~~~cpp
// The user's first non-English TIP (input processor) from the enumeration.
for (const auto &langProfile : m_langProfiles)
{
    if (langProfile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR
        && PRIMARYLANGID(langProfile.langid) != PRIMARYLANGID(LANGID_ENG))
    {
        logger::info("No input method remembered from the previous session; activating the user's IME '{}'", langProfile.desc);
        return ActivateProfile(langProfile);
    }
}
~~~

调用点：`src/ime/ImeManager.cpp:243-247`（默认路径：当前不是 TIP 时）、`src/ime/ImeManager.cpp:288-291`（开关路径：没有记住的 profile 时）。

**所以本需求不是"从零造一个切换器"，而是把这条**顺序仲裁**换成"取用户配置项"。** 这是改动小、风险低的根本原因：状态机、焦点、启停时序一行都不用动。

---

## 3. 可行性判断（逐条论证）

### 3.1 能拿到"用户装了哪些输入法"吗？ ✅

能，而且已经拿到了（§2.1）。`m_langProfiles` 同时包含 TIP（`TF_PROFILETYPE_INPUTPROCESSOR`）和键盘布局（`TF_PROFILETYPE_KEYBOARDLAYOUT`），带本地化显示名。语言栏 `DrawInputMethodsCombo`（`src/ui/LanguageBar.cpp:28-57`）已经把它渲染成可点的菜单 —— 设置页只是另一个消费方。

### 3.2 能"切到指定输入法"吗？ ✅（身份上有但书）

能（§2.2）。但书是身份：

`ActivateProfile(const GUID&)`（`src/tsf/InputMethodManager.cpp:263-281`）只认 GUID，而**所有键盘布局共用 `GUID_NULL`**。代码里已有注释记录过这个坑（`src/tsf/InputMethodManager.cpp:285-291`）：

~~~
// Keyboard layout profiles (TF_PROFILETYPE_KEYBOARDLAYOUT) ALL share
// guidProfile == GUID_NULL. ...
~~~

所以"记住玩家选的是哪一项"不能用 GUID 表达，必须引入能区分键盘布局的身份编码 —— §4.2。

### 3.3 能在"每次进游戏"这一刻应用吗？ ✅（两个挂点，互为保险）

- **早挂点** `ImeWnd::OnCreated(Settings&)`（`src/ImeWnd.cpp:1041-1059`）：TSF 已初始化、`m_inputMethodManager` 已构造（`src/ImeWnd.cpp:220-224`）、`settings` 可用，且**已经在 IME 线程上**（TSF 只能在这个线程调，见 `ImeApp.cpp:1028-1031` 有关游戏主线程是 MTA 的注释）。
- **准挂点** `ImeManager::DoEnable()`（`src/ime/ImeManager.cpp:217-294`）：玩家点进文本框、输入法真正启用的那一刻。

两个都要，理由是实机事实而非讲究：启动时 ImeWnd 是隐藏的 0x0 窗口，TSF 的激活事件要靠窗口消息落到焦点窗口。即使启动那次不生效（§9-B），`DoEnable` 一定会在玩家第一次打字前补一次，**所以功能不会失效，最坏只是日志里多一条 WARN**。

### 3.4 会不会残留到桌面？ ✅ 基本不会 —— 但这条"不会"很脆

先厘清机制（官方文档 + 本仓已实测）：

- `TF_IPPMF_FORPROCESS` 只是"为进程内所有线程激活该 profile"，是**运行时状态**；`ActivateProfile` 的文档里唯一写注册表的是 `TF_IPPMF_ENABLEPROFILE`（"Update the registry to enable this profile for this user"）。**进程退出，这个激活就没了。**
- 真正的"残留"来自 IMM32 那条腿：`LoadKeyboardLayout` 自 Win8 起 *"affects the layout for the entire system"*，而 `TF_IPPMF_FORPROCESS` 看起来进程级、实际生效路径会经过 IMM32 的 `ActivateKeyboardLayout(KLF_SETFORPROCESS)` + `WM_INPUTLANGCHANGEREQUEST` —— 就是任务栏语言栏走的那条路。本仓 `src/ime/ImeManager.cpp:329-342` 注释记录了实测：*"Windows keeps no 'restore when the process dies'：游戏退出后任务栏仍停在游戏里切的 ENG"*。

对本方案的意义：

- v1 只对 **TIP**（`TF_PROFILETYPE_INPUTPROCESSOR`）调用 TSF 的 `ActivateProfile`，**不碰** `LoadKeyboardLayout` / `ActivateKeyboardLayout` → 不写注册表、不改系统级布局，桌面残留风险可忽略。
- 一旦为了"跨语言兜底"（§9-A）去调 `ActivateKeyboardLayout(KLF_SETFORPROCESS)`，就重新引入残留。**所以这个兜底只能当作最后手段，且必须同时做"退出还原"** —— 见 §8/§6。

### 3.5 会不会和 `switch_english_layout_on_disable` 打架？ ⚠️ 可定义优先级

| 该开关 | 禁用时行为 | 与默认输入法冲突？ |
|---|---|---|
| `false`（默认） | 只关 IME，**不动** profile（`src/ime/ImeManager.cpp:342-345`） | 不冲突：默认输入法一直留在原地 |
| `true` | 切到英文键盘布局（`src/ime/ImeManager.cpp:346-373`） | 有冲突：启用时要用 `m_lastActiveProfile` 还原（`262-269`） |

处理规则：**`DoEnable` 里先执行原有还原，再做"当前是不是配置项"的校验**；配置项非空时以其为准。于是"`switch_english_layout_on_disable = true` + 配了默认输入法"的语义自然变成"禁用切英文、启用切回我指定的输入法"，这正符合预期，不需要新增开关。

### 3.6 设置里能选吗？ ✅

现成控件：`include/ui/panels/PanelWidgets.h:1060-1089` 的 `Panels::SettingsComboRow(strId, title, supporting, preview, fillMenu, textMeasureCap)` + `Panels::ComboOption(label, selected)`，范例 `src/ui/ToolWindow.cpp:1203-1221`（日志级别）。

候选来源：`src/ui/ImeOverlay.cpp:126` 调用 `m_toolWindow->Draw(settings)`，而 `IMEOverlay::Draw` 的入参里**已经有** `langProfiles`（`src/ui/ImeOverlay.cpp:93`）。把签名改成 `ToolWindow::Draw(Settings&, const std::vector<LangProfile>&)` 即可 —— **不需要新增任何跨线程数据通道**。

落盘：设置页改动 → `MarkDirty` → 菜单关闭时 `ImeApp::SaveSettings`（链路见 §4.6）。

### 3.7 新增配置键的代价？ 低，但必须一次改全

`test/settings_converter_test.cpp:375-419` 的 `ShippedConfigurationTest` 会读真实出货的 `contrib/config/SimpleIME.toml`，逐字段断言等于 `GetDefaultSettings()`，并断言 `ignoredKeys` 为空 —— **只加 TOML 键不加 reader 会立刻把测试打红**。这是好事：保证六处改动不会漏（§5）。

### 3.8 结论

**可行。** 核心改动约 300 行（含测试与注释），不动输入投递、不动焦点/启停时序、不动线程模型。未知量集中在两个可实测点（跨语言激活、per-app 记忆竞争），都有降级路径。

### 3.9 一个容易忽略的竞争者：Windows 自己也会记住"这个程序上次用哪个输入法" ⚠️

Windows 8+ 的"**为每个应用窗口使用不同输入法**"（默认可能开可能关，随机器/版本不同，必须运行期查）由 `SystemParametersInfo(SPI_GETTHREADLOCALINPUTSETTINGS = 0x104E)` 读取：*"whether the active input settings have Local (per-thread, TRUE) or Global (session, FALSE) scope"*。

- 为 **TRUE**：Windows 按应用记住 Skyrim 上次的输入法，会**和我们的功能抢**——玩家上次手动换过，进游戏 Windows 可能先还原它，我们随后再切（表现为"闪一下才变"，但最终结果是我们的，因为我们在 `FocusTextService` 之后才切）。
- 为 **FALSE**：输入法是会话全局的，我们切了就影响会话 —— 这也解释了为什么"切到中文 TIP"在部分机器上会波及桌面观感。

处理：
1. 启动时查一次并**打日志**（一行，便于解读用户反馈）；
2. 优先级固定为 **用户显式配置 > 系统记忆 > 当前 profile**（实现上就是"配置项非空且不匹配就切"，天然满足）。

---

## 4. 方案

### 4.1 配置项

新增 `[core] default_input_method`（字符串，出厂 `""`）。放 `[core]` 根级而不是 `[input]` 段：`[input]` 段装的是"投递策略/候选项行为 + 运行期缓存"（`include/configs/configuration.h:48-96`），而"用哪个输入法"和 `switchEnglishLayoutOnDisable`（`include/configs/configuration.h:103-109`）是同一类会话级偏好，后者就在 `[core]` 根级。

| 值 | 行为 |
|---|---|
| `""`（出厂） | **不干预**，与今天的启发式完全一致 |
| `tip:{CLSID}/{GUID}` | 进游戏/重新启用输入法时切到它 |
| 其它字符串 | 解析失败 → 按显示名（`desc`）精确匹配兜底（方便手改 TOML）；仍失败则 WARN + 回退启发式 |

### 4.2 身份编码：为什么必须是 `tip:{CLSID}/{GUID}`

被排除的候选，以及排除理由：

- **枚举下标**：注册表顺序 + enable 状态会变，跨会话不稳。
- **`desc`（显示名）**：本地化字符串，随系统语言和 IME 版本变。
- **单独的 `hkl`**：HKL 是**运行时句柄**（低 16 位 LANGID + 高 16 位 USER 子系统设备号），重启/换机不保证相同；本机实测也证实它既不是 KLID 也不唯一（附录 A）。
- **`LANGID`**：`0x2000/0x2400/0x2800/0x2C00` 这类 transient LANGID 在不同机器/用户可代表不同语言，绝不能当键盘布局的持久标识。

正面依据：TIP 的持久标识就是三元组 `(clsid, guidProfile, langid)`，它同时是 TSF 的注册主键 `HKLM\SOFTWARE\Microsoft\CTF\TIP\{clsid}\LanguageProfile\0x<langid>\{<guid>}`，跨重启稳定。

所以 v1 只对 TIP 建立稳定身份：

~~~
tip:{CLSID}/{GUID}[/{langid}]
~~~

- 花括号文本用仓内既有的 `ToStringFromGUID2`（大写、带花括号）。
- `langid` 是可选的消歧后缀（同一 GUID 为多种语言注册的极端情况）。
- 匹配大小写不敏感。
- 键盘布局的身份留到 P2：唯一正路是**在玩家选择的那一刻** `ActivateKeyboardLayout(hkl, 0)` 后立刻 `GetKeyboardLayoutNameW()` 取规范 KLID（该 API 只返回调用线程当前布局的名字），再回填成 `keyboard:{KLID}`。v1 的候选列表干脆**不列**键盘布局，避免给出一个不可靠的选项。

### 4.3 纯逻辑头：`include/utils/InputMethodIdentity.h`

沿用仓内 `*Logic` 约定（`include/utils/ConversionMemoryLogic.h`、`CharDeliveryLogic.h`、`LanguagePreference.h`：header-only、零 Windows/SKSE 依赖、进无头测试套件）：

~~~cpp
namespace Ime::InputMethodIdentity
{
struct Identity
{
    enum class Kind { Unset, Unknown, Tip, Keyboard, Desc };
    Kind          kind = Kind::Unset;
    std::string   clsid;        // "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
    std::string   profile;      // 同上
    std::uint16_t langid = 0;   // 0 = 未指定
    std::string   klid;         // P2
    std::string   desc;         // 已知时总是填（Desc 兜底靠它）
};

[[nodiscard]] auto IsUnset(std::string_view raw) -> bool;   // "" / "auto" / "system" / "none"（忽略空白与大小写）
[[nodiscard]] auto FormatFromTip(std::string_view clsid, std::string_view profile, std::uint16_t langid = 0) -> std::string;
[[nodiscard]] auto FormatFromKeyboard(std::string_view klid) -> std::string;
[[nodiscard]] auto Parse(std::string_view raw) -> Identity; // 不抛：坏 tip:/keyboard: -> Unknown；非前缀 -> Desc
[[nodiscard]] auto Matches(const Identity &configured, const Identity &candidate) -> bool;
}
~~~

`Matches` 的规则（就是单测要钉住的行为）：

1. `Unset` → 永不匹配（调用方先判 `IsUnset`）。
2. 两侧 `Kind` 不同 → false（`tip:` 不能匹配键盘布局）。
3. `Tip`：`clsid` 与 `profile` 文本相等（忽略大小写）；`langid != 0` 时额外要求相等。
4. `Desc`：`candidate.desc` 与配置原文相等。

平台侧只需一个 `LangProfile -> Identity` 适配函数（放 `src/tsf/InputMethodManager.cpp`，不进纯逻辑头）：

~~~cpp
auto ToIdentity(const LangProfile &p) -> InputMethodIdentity::Identity
{
    if (p.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
        return {.kind    = InputMethodIdentity::Identity::Kind::Tip,
                .clsid   = WCharUtils::ToString(ToStringFromGUID2(p.clsid)),
                .profile = WCharUtils::ToString(ToStringFromGUID2(p.guidProfile)),
                .langid  = p.langid};
    return {.kind = InputMethodIdentity::Identity::Kind::Keyboard, .desc = p.desc /* P2: klid */};
}
~~~

### 4.4 `InputMethodManager` 新增四个方法

~~~cpp
// include/tsf/InputMethodManager.h
void               SetConfiguredProfile(std::string identity);      // IME 线程；空串 = 不干预
[[nodiscard]] auto HasConfiguredProfile() const -> bool;
[[nodiscard]] auto IsConfiguredProfileActive() const -> bool;       // 当前 active 是否匹配配置
auto               ActivateConfiguredProfile() -> HRESULT;          // 未找到返回 E_INVALIDARG（不 fallback）
~~~

要点：

- `ActivateConfiguredProfile()` 遍历 `m_langProfiles`，用 `InputMethodIdentity::Matches` 找下标，命中就 `ActivateProfile(m_langProfiles[i])`；没命中记 WARN（带配置原文）并返回 `E_INVALIDARG`，**把 fallback 的决定权留给调用方**（调用方才知道要不要退回启发式）。
- `IsConfiguredProfileActive()` 取当前激活项转成 `Identity` 再 `Matches` —— 这正是修掉"GUID 相等即视为同一个输入法"那个坑的地方，也是**幂等短路**（见 §4.8 的稳定性理由）。实现上**直接读缓存下标** `m_langProfiles[m_activatedProfile]`（越界即 false），而不是调用 `GetActiveLangProfile()`：后者在缓存失效时返回 `DEFAULT_LANG_PROFILE` 桩，其 `GUID_NULL` 会被当成一个真实 profile 参与比较。
- **不要**在这里新增任何 `settings` 访问（线程理由见 §4.7）。

`ImeWnd` 按既有风格加薄包装（`include/ImeWnd.hpp:124-143`）：

~~~cpp
void SetConfiguredInputMethod(std::string identity);
auto HasConfiguredInputMethod() const -> bool;
auto IsConfiguredInputMethodActive() const -> bool;
auto ActivateConfiguredInputMethod() -> HRESULT;
~~~

### 4.5 两个应用点

**（a）启动：`ImeWnd::OnCreated`（`src/ImeWnd.cpp:1041-1059`）末尾，`ImeController::GetInstance()->Init(...)` 之后**

~~~cpp
// 从 settings 拷一份；此后 IME 线程不再碰 settings 里的 std::string（§4.7）
m_inputMethodManager->SetConfiguredProfile(settings.defaultInputMethod);
if (m_inputMethodManager->HasConfiguredProfile()
    && SUCCEEDED(m_inputMethodManager->ActivateConfiguredProfile())
    && GetActiveLangProfile().dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
{
    // 进游戏应当是"输入法已选好但处于关闭状态"：激活只负责选 profile，
    // 不负责关。用既有的 global-compartment 通道立刻关掉它，
    // 免得启动那几帧吃掉 WASD。
    (void)SetSessionImeEnabled(false);
}
~~~

**不**在这里做回退：`DoEnable` 马上会跑一遍，那里的语义更准。

**（b）启用：`ImeManager::DoEnable()`（`src/ime/ImeManager.cpp:217-294`）两个 `return success;` 之前**

两个分支统一收口到一个私有方法，插在原有还原逻辑**之后**（§3.5 的优先级）：

~~~cpp
void ImeManager::ApplyConfiguredInputMethod()   // IME 线程
{
    if (!m_imeWnd->HasConfiguredInputMethod()) return;
    if (m_imeWnd->IsConfiguredInputMethodActive()) return;  // 已是它：幂等短路（§4.8）
    if (SUCCEEDED(m_imeWnd->ActivateConfiguredInputMethod())) return;
    logger::warn("Configured default input method is not available; falling back to the user's IME");
    (void)m_imeWnd->ActivatePreferredImeProfile();          // 保底：仍是今天的行为
}
~~~

即使配置项被卸载/改名，玩家也只是回到今天的行为，不会连字都打不了。

### 4.6 设置页

位置：Behaviour 页「输入」卡内、`enableMod` 与三个短开关的汇总行之后（`src/ui/ToolWindow.cpp:869` 之后、`ImGui::EndDisabled()` 之前）。

~~~cpp
ImGuiEx::M3::Divider();
Panels::SettingsComboRow(
    "##DefaultInputMethod",
    Translate("Settings.Behaviour.DefaultInputMethod"),
    Translate("Settings.Behaviour.DefaultInputMethodSupport"),
    previewName,                        // 空配置 -> ..System；未找到 -> ..Missing
    [&] {
        if (Panels::ComboOption(Translate("Settings.Behaviour.DefaultInputMethodSystem"), configuredIsUnset))
        {
            settings.defaultInputMethod.clear();
            ImeController::GetInstance()->SetDefaultInputMethod({});
        }
        for (const auto &p : langProfiles)  // 只列 TIP
        {
            if (p.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR) continue;
            if (Panels::ComboOption(p.localeDisplayName, /*selected*/ MatchesConfigured(p)))
            {
                auto id = Ime::InputMethodIdentity::FormatFromTip(...);
                settings.defaultInputMethod = id;
                ImeController::GetInstance()->SetDefaultInputMethod(id);  // 传值 -> IME 线程立即应用
            }
        }
    },
    SupportingMeasure());
~~~

- 候选只列 `TF_PROFILETYPE_INPUTPROCESSOR`（键盘布局身份不稳，§4.2）。
- "未找到"判定在**渲染线程**做：`Parse(settings.defaultInputMethod)` 与 `langProfiles` 逐项 `Matches` —— 纯字符串比较、只读，不新增跨线程状态。
- 立即生效：新增 `ImeController::SetDefaultInputMethod(std::string)`，照抄 `ActivateLangProfile` 的 `PostToImeThread` 传值写法（`src/ime/ImeController.cpp:72-75` 的注释已经解释了为什么必须 by value）+ `MarkDirty()`。
- 持久化：沿用既有链路 —— 菜单关闭时 `ImeApp::SaveSettings`（`src/ImeApp.cpp:706-755`）。

签名改动（两处调用点）：

- `include/ui/ToolWindow.h`：`Draw(Settings&, const std::vector<LangProfile>&)`、`DrawMenuInputStatus(Settings&, const std::vector<LangProfile>&)`
- `src/ui/ImeOverlay.cpp:126`：`m_toolWindow->Draw(settings, langProfiles);`

### 4.7 线程与数据竞争（ADR-0003 边界）

`include/ui/Settings.h:41-48` 的教条写得很清楚：

~~~
// ... they stay plain bools ON PURPOSE — on x86 an aligned bool cannot tear ...
// Don't add new cross-thread fields without at least this guarantee.
~~~

新配置值是 `std::string`，**不具备这个保证**。因此：

1. `settings.defaultInputMethod` 只在**渲染线程**读写（设置页）。
2. IME 线程只持**自己的副本** `m_configuredProfile`，由 `OnCreated`（启动一次性拷贝，此时无并发写）与 `SetConfiguredInputMethod`（`PostToImeThread` **按值**捕获）更新。
3. `DoEnable` 的判定用 `m_imeWnd->IsConfiguredInputMethodActive()`，**不读 settings**。
4. profile 列表的跨线程读取沿用现状（`src/ImeWnd.cpp:504-509` → `src/ui/ImeOverlay.cpp:93`，渲染线程持 `const&`）。本方案**不新增**任何刷新入口，因此不扩大既有窗口。若 P2 要加"刷新列表"，`m_langProfiles` 必须改成 `std::atomic<std::shared_ptr<const std::vector<LangProfile>>>` 发布式更新，而不是现在的原地 `clear() + emplace_back`（`src/tsf/InputMethodManager.cpp:153-158`）。

### 4.8 稳定性：不轮询、不忘重试、必须幂等

社区经验（TSFTool 作者对"游戏在前台时自动切输入法"的答复）是：频繁 Activate/Deactivate 因为异步 COM + 语言栏刷新 + 每线程 ThreadMgr 逐一同步而变慢、甚至挂起；建议两次切换之间留 5–6 秒。落到本方案：

- `DoEnable` 会因为焦点变化被高频调用 → 靠 `IsConfiguredInputMethodActive()` 幂等短路，**绝大多数调用是零成本的只读比较**。
- 不做定时轮询、不做"失败后立刻重试 N 次"的循环。失败就记 WARN，等下一次 `DoEnable` 自然再试。
- 只在"真的不一样"时切，避免 TSF 与 IMM32 双写。

---

## 5. 改动清单

| 文件 | 改动 | 约行数 |
|---|---|---|
| `include/configs/configuration.h` | `Configuration` 加 `std::string defaultInputMethod;`（`switchEnglishLayoutOnDisable` 之后）+ 默认 `""` | +4 |
| `include/ui/Settings.h` | `Settings` 加同名字段 + 默认值 + 线程归属注释 | +6 |
| `src/configs/ConfigSerializer.cpp` | `KEY_DEFAULT_INPUT_METHOD`、中文注释块、写表（`:191` 附近）、读表（`:315` 附近） | +12 |
| `src/configs/settings_converter.cpp` | 两个方向各一行 | +2 |
| `contrib/config/SimpleIME.toml` | `[core]` 加键 + 中文注释 | +4 |
| `include/utils/InputMethodIdentity.h` | 新建纯逻辑头 | ~90 |
| `include/tsf/InputMethodManager.h` + `src/tsf/InputMethodManager.cpp` | 4 个新方法 + `ToIdentity` | ~70 |
| `include/ImeWnd.hpp` + `src/ImeWnd.cpp` | 4 个薄包装 + `OnCreated` 里 6 行 | ~20 |
| `include/ime/ImeManager.h` + `src/ime/ImeManager.cpp` | `ApplyConfiguredInputMethod()` + `DoEnable` 两处调用 | ~20 |
| `include/ime/ImeController.h` + `src/ime/ImeController.cpp` | `SetDefaultInputMethod(std::string)` | ~12 |
| `include/ui/ToolWindow.h` + `src/ui/ToolWindow.cpp` | 签名两处 + 组合框 ~40 行 | ~45 |
| `src/ui/ImeOverlay.cpp` | 一处调用 | ~1 |
| `contrib/Distribution/translate/translate_*.toml` | 4 个键 × 6 语言 | +24 |
| `test/InputMethodIdentityTest.cpp` | 新建（12 个用例） | ~180 |
| `test/ConfigSerializerTest.cpp` | 默认值 + 非空值往返各一条 | +2 |
| `test/settings_converter_test.cpp` | `ShippedConfigurationTest` 加一行逐字段断言 | +1 |
| `test/RandomUtils.h` | 随机配置生成器补该字段 | +1 |
| `test.cmake` | **无需改动**：`file(GLOB_RECURSE TEST_SOURCES test/*.cpp)` 自动收录 | 0 |

翻译键（6 个文件都要补）：

| 键 | 中文 |
|---|---|
| `Settings.Behaviour.DefaultInputMethod` | 默认输入法 |
| `Settings.Behaviour.DefaultInputMethodSupport` | 每次进入游戏、以及重新启用输入法时切换到这个输入法。留空表示跟随系统当前选择。 |
| `Settings.Behaviour.DefaultInputMethodSystem` | 跟随系统 |
| `Settings.Behaviour.DefaultInputMethodMissing` | （未找到） |

出厂 TOML（`contrib/config/SimpleIME.toml` 的 `[core]`）：

~~~toml
# 默认输入法：每次进入游戏（以及重新启用输入法）时自动切换到这个输入法。
# 留空 = 跟随系统当前选择（保持原有行为）。
# 格式由设置界面写入，一般不需要手改：tip:{CLSID}/{GUID}
default_input_method = ""
~~~

---

## 6. 分期

### P0 — 运行期能力（只改 TOML，无 UI）

配置键 + `InputMethodIdentity` + `InputMethodManager` 四个方法 + `OnCreated` / `DoEnable` 两个应用点 + 单测。

验收：

1. TOML 填 `default_input_method = "tip:{...}"`，进游戏第一次点进文本框就是该输入法。
2. 删掉该行，行为与改动前完全一致。
3. 填一个不存在的 GUID → 日志有 WARN，游戏内输入法仍可用。
4. `ctest` 全绿（含 `ShippedConfigurationTest`）。

### P1 — 设置界面

组合框 + 立即生效 + 6 语言翻译 + "（未找到）"态。

验收：设置里选 → 当场切换 → 关闭菜单落盘 → 重启游戏仍生效；未配置的机器显示"跟随系统"。

### P2 — 可选（不在本次范围）

1. 语言栏 `DrawInputMethodsCombo`（`src/ui/LanguageBar.cpp:28-57`）改为按**索引**激活，顺带修掉"多个键盘布局不可分辨"的既有缺陷。
2. 键盘布局身份（`keyboard:{KLID}`，`GetKeyboardLayoutNameW` 在选取瞬间取）。
3. "退出时还原 / 失焦还原"（§3.4）—— **只有将来真的动用了 IMM32 兜底才需要**。
4. "仅启动应用一次"的宽容模式（避免每次启用都覆盖玩家手选）。
5. 跨语言兜底（先切目标语言默认布局再激活 TIP），并配套上面的还原。
6. 输入法列表刷新按钮（需要 §4.7-4 的发布式更新）。

---

## 7. 测试计划

**无头（进 ctest）**

- `test/InputMethodIdentityTest.cpp`：
  - `FormatFromTip` / `Parse` 往返；
  - 大小写不敏感；
  - 空串 / `"auto"` / `"system"` → `IsUnset`；
  - `"tip:"` 截断、缺 GUID、多段 → `Kind::Unknown`；
  - `Matches`：tip↔keyboard 不匹配；同 CLSID 不同 GUID 不匹配；`langid` 指定时参与、未指定时忽略；
  - `Desc` 兜底匹配。
- `test/ConfigSerializerTest.cpp`：非空值往返。
- `test/settings_converter_test.cpp`：新键自动纳入 `ShippedConfigurationTest`（逐字段相等 + `ignoredKeys` 为空）。

**手动（游戏内）**

1. 未配置：行为不变（第一次进文本框仍是玩家自己的输入法）。
2. 配置中文 TIP、桌面也是中文：启动 → 进文本框 → 打字是中文；ESC 后 WASD 正常。
3. 配置中文 TIP、桌面是英文/日文：验证跨语言（§9-A）。
4. `switch_english_layout_on_disable = true` + 配置项：禁用后任务栏 ENG，再启用回到配置的输入法。
5. 配置项指向已卸载的 IME：能正常打字（回退），日志有 WARN。
6. 反复进出文本框 / 开关设置菜单：日志不应出现重复激活刷屏（幂等短路生效）。
7. `enableTsf = false`（Imm32 后端）：配置项无效但不崩溃。
8. 开关"为每个应用窗口使用不同输入法"两种情况各跑一遍（§3.9）。

---

## 8. 风险

| 风险 | 触发条件 | 影响 | 缓解 |
|---|---|---|---|
| 跨语言激活被 TSF 延后 | 目标 TIP 语言 ≠ 当前进程输入语言（`TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE` 的语义：语言不匹配时只"登记"，等下次切语言才生效） | 配置不生效 | `DoEnable` 每次补一次；仍不行才上 IMM32 兜底（并配套还原）——**§9-A 实机先验** |
| IMM32 兜底引入桌面残留 | 走到 `ActivateKeyboardLayout(KLF_SETFORPROCESS)` | 退出后任务栏停在游戏选的语言 | 首选不做；要做就必须配 P2-3 的还原 |
| Windows per-app 记忆竞争 | `SPI_GETTHREADLOCALINPUTSETTINGS = TRUE` | 表现出"先还原成上次的、再被我们切走"的闪动 | 优先级固定为"用户配置 > 系统记忆 > 当前 profile"；启动打日志（§3.9） |
| 与 `switch_english_layout_on_disable` 的时序 | 两者都开 | 一次 enable 被还原逻辑覆盖 | 顺序固定：先还原、再校验配置项（§3.5） |
| 启动期无焦点激活不生效 | ImeWnd 隐藏、无消息泵焦点 | 启动日志 WARN | `DoEnable` 保底，功能不失效（§3.3） |
| 每次启用都强制切换 | 玩家会话内从语言栏换成别的 IME | 下次进文本框被切回配置项 | 已知取舍（"默认输入法"的字面语义）；P2-4 提供宽容模式 |
| 配置项失效 | IME 被卸载 / 兼容模式改变了 profile | 无提示地不生效 | WARN + 设置页"（未找到）"+ 回退启发式 |
| 过度切换导致 TSF 变慢/挂起 | 无幂等短路、或轮询重试 | 语言栏卡顿、输入法不响应 | §4.8：幂等短路 + 不轮询 + 不重试循环 |
| 跨线程 string | 图省事在 IME 线程直接读 settings | 未定义行为 | §4.7 强约束 |

---

## 9. 需要实机确认的三个点

**A. 跨语言激活是否当场生效？**
桌面输入法设为日文，配置 `tip:{微软拼音}`，进游戏点进文本框看组字是拼音还是罗马字。同时抓 `ActivateProfile` 的 HRESULT（`src/tsf/InputMethodManager.cpp:311` 已有日志）与随后的 `GetActiveProfile` 结果（`OnActivated` 里有现成日志）。
无效 → 优先"下一次 enable 再试"；仍无效才考虑 IMM32 兜底（同时引入还原需求）。

**B. 启动期（无焦点 + 无消息泵）激活是否生效？**
看启动日志里 `OnCreated` 那次激活的 HRESULT，以及第一次 `OnActivated` 报告的 profile 是否就是配置项。失败不影响功能（`DoEnable` 保底），但决定是否把启动点降级为"仅记录、不激活"。

**C. 桌面到底会不会被改？**
§3.4 的"v1 不残留"依赖一个假设：TSF 的进程级 profile 激活不会在内部走 IMM32 那条系统级的腿。但跨语言激活时 TSF 有可能自己去换语言/布局，这条腿未必受我们控制。做法：桌面语言与游戏内选择刻意设为不同，退出游戏后看任务栏语言指示器；再开一个记事本确认新进程的输入法有没有跟着变（同时验证 `TF_IPPMF_FORPROCESS` 的边界是否真的只影响本进程）。
若真的会残留 → 优先级上升：把"退出还原"从 P2 提到 P1。

> 另：可用 `SPI_GETTHREADLOCALINPUTSETTINGS` 在启动日志里记一行，方便解读"为什么某台机器上表现不一样"。

### 9.1 实机结果（2026-10-23 17:09，玩家本机，Skyrim SE + 微信输入法 / 微软拼音）

日志 `D:\Documents\My Games\Skyrim Special Edition\SKSE\SimpleIME.log`：

~~~
[17:09:23.838] Load installed ime: 简体中文(中国大陆) 微信输入法 (tip:{86598FB9-...}/{607FDF85-...}/0804)
[17:09:23.839] Load installed ime: 简体中文(中国大陆) 微软拼音   (tip:{81D4E9C9-...}/{FA550B04-...}/0804)
[17:09:23.839] Load installed ime: 英语(美国) 美式键盘 (<keyboard layout, not selectable yet>)
[17:09:42.805] Default input method set to 'tip:{81D4E9C9-...}/{FA550B04-...}/0804'
[17:09:42.805] Activating the configured default input method '微软拼音'
[17:09:42.807] Published the IME off state through the global compartments (open/close hr=0x1)  <- 设计如此：先选 profile 再立刻关
[17:10:28.745] Input method activated: '微软拼音' (type=1, hkl=0x0, flags=0x3)               <- OnActivated 确认
~~~

结论：

* **B 通过**：启动期（`OnCreated`，窗口尚无焦点）激活成功，随后 `OnActivated` 回报的就是配置项；`type=1` = `TF_PROFILETYPE_INPUTPROCESSOR`。**启动点不需要降级。**
* **A 未覆盖**：本机配置项与当时语言都是 `0804`，属于同语言激活，没走 `TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE` 的跨语言登记路径。跨语言仍待验证（把配置改成非 0804 的 TIP 可复验）。
* **幂等短路生效**：17:10:28 那一轮 on/off 里没有第二次 `Activating the configured default input method`，说明 `IsConfiguredProfileActive()` 提前返回，没有对 TSF 反复 Activate（正是 §4.6 想要的）。
* **C 通过（2026-10-23 17:19，玩家确认）**：进游戏前桌面是微信输入法，进游戏内切成微软拼音，**退出到桌面仍是微信输入法** —— 进程级 profile 激活没有残留到桌面。§3.4 的「不残留」假设成立，v1 不需要把「退出还原」从 P2 提前（仍留 P2 作为跨语言场景的保险）。
* 日志里唯一的 `error` 行 `No display name for input method profile (clsid {00000000-...}, langid 0x409 ...)` 是既有的键盘布局无显示名兜底，与本功能无关。

---

## 附录 A：HKL 不能当持久身份的实测（本机 Windows，2026-10-23）

~~~text
loaded layouts (current process): 1
  hkl=0x08040804  hi=0x0804  lo=0x0804
current thread klid=00000804
  00000409 -> hkl=0x04090409 hi=0x0409 lo=0x0409
  00010409 -> hkl=0xF0020409 hi=0xF002 lo=0x0409
  00000804 -> hkl=0x08040804 hi=0x0804 lo=0x0804
  e0010409 -> hkl=0x04090409 hi=0x0409 lo=0x0409
~~~

三个结论：

1. `HIWORD(hkl)` ≠ KLID（Dvorak 是 `0xF002`）。顺带说明 `src/tsf/InputMethodManager.cpp:51` 那条"用 `HandleToUlong(hkl) >> 16` 查注册表取键盘布局显示名"的路径，对非预装布局会查不到、走 `locale (guid)` 兜底 —— 既有小偏差，与本需求无关，但值得记一笔。
2. `e0010409` 与 `00000409` 返回同一个 HKL → HKL 不唯一。
3. 所以键盘布局的真实 KLID 只能在"选中那一刻"用 `GetKeyboardLayoutNameW()` 取（它只返回调用线程当前布局），这是 P2 的做法。

## 附录 B：为什么不用 `TF_IPPMF_FORSESSION`

[官方文档](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfinputprocessorprofilemgr-activateprofile) 原文：

- `TF_IPPMF_FORPROCESS` — "Activate this profile for all threads in the process."
- `TF_IPPMF_FORSESSION` — "... for all threads in the current desktop."

`FORSESSION` 才是直接改桌面的那个标志，本方案**不用**它。而 `ActivateProfile` 文档里唯一会写注册表的是 `TF_IPPMF_ENABLEPROFILE`，所以进程级的激活在进程退出后自然消失 —— 这也是 §3.4 说"v1 基本不残留"的依据。

其它相关事实（同文档）：

- 返回 `S_OK` / `S_FALSE`（profile 未启用）/ `E_FAIL` / `E_INVALIDARG`。
- `KEYBOARDLAYOUT` 时 `clsid` 必须 `CLSID_NULL`、`guidProfile` 必须 `GUID_NULL`；`INPUTPROCESSOR` 时 `hkl` 必须 `NULL`。
- `GetActiveProfile` 只支持 `catid = GUID_TFCAT_TIP_KEYBOARD`，返回"当前活动 profile"（实践上是调用线程的活动 profile，与仓内一贯的 IME 线程调用一致）。

---

## 10. P0 实现状态（2026-10-23 已落地）

P0 已按本文件实装并通过构建 + 全量单测（`SimpleIMETest` 128/128，其中新增 12 个 `InputMethodIdentityTest` 用例全部通过；插件 `SimpleIME.dll` 构建无错误）。

实际落点（与本文件上文略有差异处都记在这里）：

| 文件 | 实际改动 |
|---|---|
| `include/configs/configuration.h` | `Configuration::defaultInputMethod`（`switchEnglishLayoutOnDisable` 之后）+ 默认 `""` |
| `include/ui/Settings.h` | `Settings::defaultInputMethod` + 默认 `""` + 线程归属注释（render thread only） |
| `src/configs/ConfigSerializer.cpp` | `KEY_DEFAULT_INPUT_METHOD = "default_input_method"` + 中文注释块 + 写表 + 读表 |
| `src/configs/settings_converter.cpp` | 两个方向各一行 |
| `contrib/config/SimpleIME.toml` | `[core]` 键 + 7 行中文注释（含"日志里复制身份串"的用法） |
| `include/utils/InputMethodIdentity.h` | 新建纯逻辑头（含 `Detail::*` 校验工具、`FormatFromKeyboard` 前向兼容） |
| `include/tsf/InputMethodManager.h/.cpp` | 4 个方法 + 匿名命名空间里的 `ToIdentity` + `RefreshProfiles` 的 `Load installed ime` 日志末尾追加可复制身份串 |
| `include/ImeWnd.hpp` / `src/ImeWnd.cpp` | 4 个薄包装 + `OnCreated` 末尾的应用点 |
| `include/ime/ImeManager.h/.cpp` | `ApplyConfiguredInputMethod()` + `DoEnable` 两个 arm 的收口调用 |
| `include/ime/ImeController.h/.cpp` | `SetDefaultInputMethod(std::string)`（`PostToImeThread` 按值捕获） |
| 测试 | `test/InputMethodIdentityTest.cpp` 新建；`ConfigSerializerTest`/`settings_converter_test`/`RandomUtils.h` 各补断言与生成 |

与 §4 的设计偏差（都是实现时发现更稳的写法）：

1. **`langid` 后缀分隔符**：用路径式 `tip:{CLSID}/{GUID}/{langid 十六进制}`（`/0804`，1-4 位、可省），不用 `;langid=` —— 与主分隔符一致，`Parse` 只需一次 `find('/')` 分三段。
2. **`Identity::Kind` 增加 `Unknown`**：让"填错了"和"没配"在日志里可区分，同时保证 `Matches` 对两者都返回 false（不会误命中任何 profile）。
3. **`IsConfiguredProfileActive()` 直接读缓存下标** `m_langProfiles[m_activatedProfile]`（越界即 false），不调用 `GetActiveLangProfile()`：后者在缓存失效时返回 `DEFAULT_LANG_PROFILE` 桩（`GUID_NULL`/`CLSID_NULL`），会被当作一个真实 profile 参与比较。
4. **`Parse` 结果缓存在成员 `m_configuredIdentity`**：`DoEnable` 高频调用，避免每次重新解析字符串（幂等短路路径上零分配）。
5. **`HasConfiguredProfile()` 不认 `Keyboard`**：键盘布局身份要到 P2 才能取到 KLID（§4.2），此时返回 false 并 WARN，避免"配了但永远激活失败"地反复重试。
6. **启动点用 `SetConfiguredInputMethod(...)` 薄包装**而不是直接碰 `m_inputMethodManager`，与 `OnCreated` 里其它调用的写法一致。

仍需实机确认的三点（§9）**不变**，P0 不阻塞：A 跨语言是否当场生效、B 启动期无焦点激活、C 桌面是否残留。

## 11. 易用性补强（2026-10-23 追加，P0 范围内）

问题：P0 让人「先跑一次游戏、再从日志里抄身份串」，第一次配置体验很差。为此加了两条不依赖日志的路径。

### 11.1 显示名模糊匹配（插件侧）

`ActivateConfiguredProfile()` 现在两段匹配：

1. 精确匹配（`tip:{CLSID}/{GUID}[/{langid}]` 或显示名逐字相等）；
2. 仅当配置值是「显示名」形态时，退化为**大小写不敏感的子串匹配**（`ContainsNoCase`，只折 ASCII，中文 UTF-8 原样透传）。

命中多个时取列表里的第一个并 `warn` 列出前 8 个候选名，提示改用精确身份串；完全找不到时 `warn` 会**打印本次枚举到的全部候选**（`名称 (tip:...)`），一次运行就能看出自己填错了什么。匹配放宽只发生在这条降级路径上，`tip:` 形态仍然严格，不会误命中。

因此 `default_input_method = "微信输入法"` 或 `"拼音"` 直接可用。

### 11.2 独立枚举脚本（仓库侧，无需进游戏）

`tools/list_input_methods.ps1`（PowerShell 7 / Windows PowerShell 均可用）：

* 枚举 `HKLM/HKCU\SOFTWARE\Microsoft\CTF\TIP\{clsid}\LanguageProfile\0x{langid}\{guid}`，即插件的 `EnumProfiles` 看到的同一批 TSF 语言配置；
* 用 `SHLoadIndirectString` 解析 `Display Description` 的 MUI 资源，得到「微软拼音」「微信输入法」这类**中文显示名**（`Description` 只有英文名时作为副标题）；
* 同一输入法常在 HKLM 与 HKCU 各登记一份（其中一份只有 `Enable`、没有名称），脚本按身份串**合并**并或运算 `Enable`；
* 输出编号 + 名称 + 身份串，支持：

  ```powershell
  .\tools\list_input_methods.ps1                      # 只列出
  .\tools\list_input_methods.ps1 -TomlPath "<toml>"    # 顺便显示当前取值
  .\tools\list_input_methods.ps1 -TomlPath "<toml>" -Name 微信输入法   # 直接写入
  .\tools\list_input_methods.ps1 -TomlPath "<toml>" -Select 3         # 按编号写入
  ```

  写入用 `[System.IO.File]::ReadAllText/WriteAllText` 并保留原文件的 UTF-8 BOM，只替换 `default_input_method` 一行（没有该行时插到 `[core]` 之后）。

注意：脚本的列表来自注册表，插件用的是 `ITfInputProcessorProfileMgr::EnumProfiles`，两者可能有细微出入（例如 `Tablet PC Correction` 这类非输入法条目也会出现在注册表里）。真机上以插件日志的 `Load installed ime` 行为准；不一致时新加的候选转储会被 `warn` 打出来。

### 11.3 首次配置的最短路径

```powershell
.\tools\list_input_methods.ps1 -TomlPath "E:\Skyrim AE\mods\SimpleIME\interface\SimpleIME\SimpleIME.toml" -Name 微信输入法
# 重启游戏即可
```

---

## 12. P1 实现状态（2026-10-23 已落地）

目标：把 §11 的「先看日志抄身份串」彻底变成 **游戏内下拉框选一次**。范围仍是「选择默认输入法」，不含语言栏/键盘布局（P2）。

### 12.1 落点

| 层 | 文件 | 内容 |
| --- | --- | --- |
| 纯逻辑 | `include/ime/InputMethodChoice.h`（新） | `struct InputMethodChoice{ identity, name }`；`FindChoiceIndex(choices, configured)`（npos = 未找到；先精确 `tip:` 身份、再按显示名子串，与激活路径同规则）；`ChoiceLabel(choices, i)`（同名条目追加 langid，如 `微软输入法 (0804)`） |
| TSF 层 | `include/tsf/InputMethodManager.h` + `src/tsf/InputMethodManager.cpp` | `GetSelectableChoices() const`：只取 `TF_PROFILETYPE_INPUTPROCESSOR`，identity 用 `FormatFromTip(clsid, guidProfile, langid)`，按身份串去重、按名称稳定排序 |
| 包装 | `include/ImeWnd.hpp` | `GetSelectableInputMethods(bool refresh)`：`refresh` 为真时先 `RefreshProfiles()`（EnumProfiles 会为每个 profile 读注册表，所以只在用户显式请求时做） |
| 控制器 | `include/ime/ImeController.h` + `src/ime/ImeController.cpp` | `RequestInputMethodChoices()`（`PostToImeThread` 拉取后加锁发布）+ `GetInputMethodChoices()`（加锁拷贝）；`m_choicesMutex` / `mutable m_choices` |
| 设置界面 | `include/ui/ToolWindow.h` + `src/ui/ToolWindow.cpp` | `DrawMenuInputStatus` 的 Input card 之后新增 `Settings.Behaviour.DefaultInputMethod` 分区 → `DrawDefaultInputMethod()`；`ApplyDefaultInputMethod()` 落盘 + 双跳 |
| i18n | `contrib/Distribution/translate/translate_{chinese,english,german,japanese,korean,russian}.toml` | `[Settings.Behaviour]` 各加 6 键：`DefaultInputMethod` / `…Support` / `…FollowSystem` / `…Missing` / `…Refresh` / `…RefreshToolTip`（六份均 55 键，已用 tomllib 校验） |
| 测试 | `test/InputMethodChoiceTest.cpp`（新） | 9 个用例：身份串命中、无 langid 后缀仍命中、显示名命中（含大小写）、未配置/未知/键盘布局均不命中、空列表、重名加 langid、无名称回退身份串 |

### 12.2 关键决定

1. **异步取列表、不阻塞渲染线程**：TSF 枚举只能在 IME 线程做，界面首帧可能只有一个「跟随系统」；拿到后下一帧自动出现完整列表（`m_choicesRequested` 每开一次窗口重置一次 —— `ToolWindow` 由 `ImeOverlay` 每次打开重建）。
2. **选完不立刻切 profile**：只 `settings.defaultInputMethod = identity` + `ImeController::SetDefaultInputMethod()`（更新 IME 线程副本）+ `MarkDirty()`。理由与 §4.6 一致——设置界面里突然换输入法会把用户正在打的字打断；真正生效点仍是「进游戏」与「每次 enable」。
3. **「跟随系统」= 空串**：与出厂默认一致，写出空的 `default_input_method`，行为与加这个功能之前完全相同（回退路径零风险）。
4. **未找到态**：配置值不在列表里时，下拉框预览显示截断后的原值（长身份串不会把 chip 撑破），supporting 文案换成错误色（`TextUnformatted<BodySmall>` + `ColorRole::error`），并额外给一行「刷新列表」链接（再发一次 `RequestInputMethodChoices`）。
5. **重名消歧**：微软的 CJK 输入法在 zh/ja/ko 三份语言配置里共用同一个显示名，`ChoiceLabel` 只在真的重名时才追加 langid，避免默认情况下把名字弄脏。
6. **界面与 TOML 双通道等价**：下拉框写出的就是 §4.2 的身份串；手工改 TOML、用界面选、用 `tools/list_input_methods.ps1` 写，三者是同一套值。

### 12.3 验证

* 单测：`ctest` **137/137 通过**（原 128 + 新增 9；`ShippedConfigurationTest` 仍通过，说明出货 TOML 与默认设置一致、`ignoredKeys` 为空）。
* 编译：`build_ime.cmd` 链接成功，DLL 由 post-build 复制到 `E:\Skyrim AE\mods\SimpleIME\SKSE\Plugins\`。

### 12.4 仍需实机确认（界面部分）

1. 打开设置窗口 → Input 页能看到「默认输入法」分区，列表里出现微信输入法 / 微软拼音（首帧可能只有「跟随系统」，等一帧即出现）。
2. 选「微软拼音」→ 关窗 → 打开文本框打字，确认是拼音；重启游戏再确认仍是微软拼音（落盘生效）。
3. 选「跟随系统」→ 退出重进，确认不再干预（行为回到 P0 之前）。
4. 把 `default_input_method` 手工改成一个不存在的名字 → 设置界面应显示错误色提示 + 「刷新列表」。

### 12.5 已知限制（P2 范围内）

* 语言栏里按索引切换、键盘布局（KLID）选择仍未实现（§4.2 / §4.5 的 P2 项）。
* 列表只在打开设置窗口时刷新一次，运行期新装的输入法要重开窗口才可见（点「刷新列表」可立即重读）。
* 跨语言激活（A 点）仍未实机复验：当前机器配置项与系统语言同为 `0804`；`list_input_methods.ps1` 可挑一个非 0804 的 TIP 写入来复验。

### 12.6 实机结果（2026-10-23 17:34–17:36，玩家本机）

日志 `D:\Documents\My Games\Skyrim Special Edition\SKSE\SimpleIME.log`：

~~~
[17:34:39.820] Load installed ime: 简体中文(中国大陆) 微信输入法 (tip:{86598FB9-...}/{607FDF85-...}/0804)
[17:34:39.820] Load installed ime: 简体中文(中国大陆) 微软拼音   (tip:{81D4E9C9-...}/{FA550B04-...}/0804)
[17:34:57.509] Default input method set to 'tip:{81D4E9C9-...}/{FA550B04-...}/0804'
[17:34:57.509] Activating the configured default input method '微软拼音'
[17:34:57.511] Active profile 微软拼音 OK (type=1, hkl=0x0)
[17:36:09.737] Input method picker: enumerated 2 selectable input method(s)   <- 打开设置窗口，IME 线程的枚举
[17:36:30.940] Input method picker: enumerated 2 selectable input method(s)   <- 第二次打开窗口，再次枚举
~~~

结论：

* **P1.1 通过**（§12.4 第 1 条）：设置界面「默认输入法」正常显示为 **微软拼音**，列表来自 IME 线程的异步枚举（2 项 = 微信输入法 + 微软拼音，与 `Load installed ime` 的 TIP 数量一致）；键盘布局因此不出现在下拉框里，与 §12.1 的设计一致。
* **异步链路正常**：`RequestInputMethodChoices → PostToImeThread → GetSelectableInputMethods(refresh=true) → 加锁发布` 全程没有卡顿/异常日志，每开一次窗口重新枚举一次（`m_choicesRequested` 按窗口生命周期重置）。
* 唯一 `error` 行仍是既有的键盘布局无显示名兜底（`InputMethodManager.cpp:244`），与本功能无关。
* 仍待验：§12.4 第 2/3/4 条（切换选中项是否落盘并在下次 enable/进游戏生效、「跟随系统」是否回退为不干预、错误值是否显示红色提示）。


## 13. P2 实现状态（2026-10-23）

P2 原计划六项（§6）：① 语言栏按索引激活 ② 键盘布局 KLID ③ 退出/失焦还原 ④ 宽容模式（仅启动应用一次）⑤ 跨语言兜底 ⑥ 列表刷新。
本轮按「价值 / 成本」取舍后做了 ① 与 ④（④ 落成用户可选的开关），⑥ 在 P1 已完成，② ③ ⑤ 只记录理由不做。

### 13.1 P2-1 语言栏按索引激活（既有缺陷修复）

**问题**：语言栏的下拉框用 `guidProfile` 判断哪一项被选中。键盘布局（如「美式键盘」）的 `guidProfile` 全是 `GUID_NULL`，于是出现两个毛病：多个布局同时显示为选中；点其中任何一个都落到第一个 `GUID_NULL` 的 profile 上。

**修复**：改用**枚举下标**作为唯一句柄（TSF 自己也是这么标识 profile 的），把 active 下标一路透传到 UI。

| 落点 | 内容 |
| --- | --- |
| `include/tsf/InputMethodManager.h` | `[[nodiscard]] auto GetActiveProfileIndex() const -> std::uint32_t`（inline，越界返回 `UINT32_MAX`）；`auto ActivateProfileByIndex(std::uint32_t index) -> HRESULT` |
| `src/tsf/InputMethodManager.cpp` | `ActivateProfileByIndex`：越界 `logger::warn("ActivateProfileByIndex({}) is out of range ({} profiles)")` + `E_INVALIDARG`；否则转 `ActivateProfile(m_langProfiles[index])` |
| `include/ImeWnd.hpp` | `ActivateLanguageProfileByIndex(const std::uint32_t index) const -> HRESULT`、`GetActiveProfileIndex() const -> std::uint32_t`（空指针保护） |
| `include/ime/ImeController.h` / `src/ime/ImeController.cpp` | `void ActivateLangProfileByIndex(std::uint32_t index) const`：照 `ActivateLangProfile` 的模式（`IsReady` 门 + `PostToImeThread` **按值捕获下标** + `IsModEnabled` 判定），失败 `ErrorNotifier::Warning` |
| `include/ui/LanguageBar.h` / `src/ui/LanguageBar.cpp` | `DrawInputMethodsCombo(langProfiles, activeProfileIndex)`；`LanguageBar::Draw(pinned, toolWindowShowing, activeLangProfile, langProfiles, activeProfileIndex, interactive)` |
| `include/ui/ImeOverlay.h` / `src/ui/ImeOverlay.cpp` | `Draw(activeLangProfile, langProfiles, activeProfileIndex, settings, shortcutPressed)`，仅转调 |
| `src/ImeWnd.cpp` | 绘制前取 `m_inputMethodManager->GetActiveProfileIndex()` 并传入 |

无单测（纯 UI 行为），需实机：语言栏里出现多个键盘布局时能分别选中。

### 13.2 P2-2 「每次启用也切换」开关（原「宽容模式」）

**问题**：`DoEnable` 每次都会把输入法切回配置项，玩家在语言栏里手选的输入法下次启用就被抢回去。原 §6 的「宽容模式」把它定死成「只启动应用一次」，但那会让「重启游戏才生效」变成唯一行为；改成**用户可选**更合适。

**配置键**：`[core] reapply_default_input_method_on_enable`（bool，默认 `true` ＝ 保持已实测的现有行为）。默认输入法留空时该项无意义。

| 落点 | 内容 |
| --- | --- |
| `src/configs/ConfigSerializer.cpp` | `KEY_REAPPLY_DEFAULT_INPUT_METHOD`、注释块 `reapplyDefaultInputMethodComment`、写表 / 读表各一项 |
| `include/configs/configuration.h` | `bool reapplyDefaultInputMethodOnEnable;` + `GetDefaultConfiguration` 里 `true` |
| `include/ui/Settings.h` | 同名字段（默认 `true`）+ `GetDefaultSettings` |
| `src/configs/settings_converter.cpp` | 两个方向各一行 |
| `contrib/config/SimpleIME.toml` | 4 行中文注释 + `reapply_default_input_method_on_enable = true` |
| `include/tsf/InputMethodManager.h/.cpp` | `SetConfiguredProfile(std::string, bool reapplyOnEnable = true)`、`ShouldApplyConfiguredProfile() const`、成员 `m_reapplyOnEnable` / `m_pendingApply` |
| `include/ImeWnd.hpp` | `SetConfiguredInputMethod(std::string, bool)`、`ShouldApplyConfiguredInputMethod() const` |
| `include/ime/ImeController.h/.cpp` | `SetDefaultInputMethod(std::string, bool)`（两参都按值捕获） |
| `src/ime/ImeManager.cpp` | `ApplyConfiguredInputMethod()` 里在「未配置」之后加 `if (!m_imeWnd->ShouldApplyConfiguredInputMethod()) return;` |
| `src/ImeWnd.cpp` | 启动点 `SetConfiguredInputMethod(settings.defaultInputMethod, settings.reapplyDefaultInputMethodOnEnable)`，**启动应用无条件**（开关只管 enable 路径） |
| `src/ui/ToolWindow.cpp/.h` | 「默认输入法」卡片里加一行 `Panels::SettingsToggleRow`（未配置时 `ImGui::BeginDisabled` 置灰）+ `ApplyDefaultInputMethodReapply` |
| i18n 六份 | `DefaultInputMethodReapply` / `DefaultInputMethodReapplySupport`（各 57 键，tomllib 校验通过） |

**pendingApply 语义**（关键细节）：`SetConfiguredProfile` 只有在**文本变化**时置 `m_pendingApply`，激活成功后清掉。于是「关闭 reapply」时：
* 玩家在语言栏手选的输入法 → 下次 enable 不会被抢回；
* 玩家在设置里**换了**默认输入法 → 仍然立刻生效（欠一次应用），不需要重启游戏。

日志会把策略一起打出来，便于排查：
```
Default input method set to 'tip:{...}' (applied on every IME enable)
Default input method set to 'tip:{...}' (applied on game start only)
```

### 13.3 验证

* 单测：`ctest` **137/137 通过**（新增 `ShippedConfigurationTest` 与新键一致、`GetRandomConfiguation` 随机 bool 往返、默认值断言；仍是 137 条，因为没有新增用例文件）。
* 编译：`build_ime.cmd` 链接成功（`SimpleIME.dll` 17:59），post-build 已复制到 `E:\Skyrim AE\mods\SimpleIME\SKSE\Plugins\`；`interface/SimpleIME/translate_*.toml` 六份同步。
* 出货 TOML 与 `GetDefaultSettings()` 一致、`ignoredKeys` 为空（`ShippedConfigurationTest` 通过即证明新键被正确识别）。

### 13.4 实机确认（2026-10-23 18:13，玩家本机：四项全部通过）

原本的四项待验项，玩家实机逐条验证后确认「四项验证正常」：

1. **P2-1 通过**：语言栏下拉框里此刻有多项（微信输入法 / 微软拼音 / 美式键盘），逐项点选各自生效、且只有一项显示为选中（原来点键盘布局无效）。
2. **P2-2 默认档通过**：开关保持「开」，语言栏切到别的输入法后触发一次 enable → 被切回微软拼音。
3. **P2-2 关闭档通过**：开关关掉后，语言栏切到微信输入法再触发 enable → **保持**微信输入法；随后在设置里改选别的输入法 → 立刻生效（不必重启，即 §13.2 的 `m_pendingApply` 路径）。
4. **置灰通过**：选「跟随系统」时，「每次启用输入法时也切换」这一行不可点。

至此 §6 的 P2 清单中 ①（语言栏按索引激活）、④（宽容模式 → 用户可选开关）、⑥（列表刷新，P1 已完成）全部落地并实机验证；② ③ ⑤ 按 §13.5 的理由不做。

### 13.5 P2 未实现的三项及理由

* **② 键盘布局 KLID**：HKL → KLID 不可逆。实测 `LoadKeyboardLayout("00010409")` 得到的 HKL 是 `0xF0020409`，HIWORD 是设备/排序码；`e0010409` 与 `00000409` 返回同一个 HKL。要支持只能用「选取瞬间 `ActivateKeyboardLayout` + `GetKeyboardLayoutNameW` 取 KLID」的写法，价值低于成本，且英文布局已由 `switch_english_layout_on_disable` 覆盖。
* **③ 退出/失焦还原**：C 点已实测——退出游戏后桌面输入法没有残留变化（`TF_IPPMF_FORPROCESS` 只是进程内运行时状态，唯一写注册表的是 `TF_IPPMF_ENABLEPROFILE`）。没有可还原的东西，先不做。
* **⑤ 跨语言兜底**：本机 EnumProfiles 只有 2 个 zh TIP + 2 个键盘布局，没有非 `0804` 的 TIP 可复验；`TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE` 的跨语言行为无法证伪，先保留现状（填错只会 WARN，不影响打字）。

### 13.6 与 §4 设计的一致性

§4 的 P2 清单里「列表刷新」已提前到 P1（设置窗口的「刷新列表」链接），「宽容模式」由固定行为改为配置开关（`reapply_default_input_method_on_enable`），两者都不改变 §4.1 的核心结论：身份仍以 `(clsid, guidProfile, langid)` 为主键，键盘布局仍留待 KLID 方案。

