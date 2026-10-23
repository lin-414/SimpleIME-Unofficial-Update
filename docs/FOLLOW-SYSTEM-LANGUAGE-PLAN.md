# 界面语言默认值改为「跟随系统」——可行性判断与实施方案

> 状态：方案 v1（**已实现**；离线验证全绿，游戏内待验，见 §9.1「实现结果」）
> 目标：`[appearance] language` 的默认值从固定的 `english` 改为「跟随系统语言」，用户仍可显式指定某个翻译文件。
> 结论：**可行**，改动集中在 3 个源文件 + 2 个默认值 + 6 个翻译文件，**不需要改动 `extern/JamieMods` 子模块**，现有 i18n API 已够用。

## 0. TL;DR

- **可行**。界面文本全部经由 `Translate()` → `i18n::Translator` 查表，语言选择只是一个字符串，运行期热换已实现（见 §2.2 两个调用点）。
- **做法**：新增哨兵值 `"auto"`。配置里**始终保留 `auto` 原文**（不回写解析结果），加载时再解析成具体语言名，然后照旧走 `i18n::UpdateTranslator()`。
- **默认值同步**：`include/ui/Settings.h:205` 与 `contrib/config/SimpleIME.toml:55` **必须同改**，否则 `ShippedConfigurationTest`（`test/settings_converter_test.cpp:375`）立刻失败——这是本改动唯一的硬约束。
- **老用户不受影响**：现有配置里写着 `language = "english"`，升级后仍是英文；只有「配置缺该键 / 全新安装 / 主动选跟随系统」才会跟随系统。
- **降级天然安全**：任何解析不出来的情况（系统语言未打包、文件被删、`GetUserPreferredUILanguages` 失败）最终都落到 `translate_english.toml`，与今天的行为一致。

## 1. 需求理解

用户诉求：「把界面语言选项从默认英文改为跟随系统语言」。

拆成三条可验收的行为：

1. **全新安装**（没有 `SimpleIME.toml`，或配置里没有 `language` 键）→ 界面语言 = 操作系统 UI 语言对应的翻译文件。
2. **系统语言有变化**（这次中文、下次英文）→ 下次启动跟着变，因为配置里存的是 `auto` 而不是解析结果。
3. **用户在设置界面显式选了某个语言** → 以用户选择为准，不再跟随系统；随时可以切回「跟随系统」。

## 2. 现状（代码事实）

### 2.1 数据侧：语言就是一个字符串

- 字段：`include/ui/Settings.h:141` `std::string language;`（`struct Appearance`）。
- 默认值：`include/ui/Settings.h:203-209` 的聚合初始化里 `language = "english"`（**第 205 行**）。
- 落盘：`Data/interface/SimpleIME/SimpleIME.toml` 的 `[appearance] language`，出厂模板在 `contrib/config/SimpleIME.toml:54-55`。
- 配置层默认是 `""`：`include/configs/configuration.h:139`。`""` 的语义是「未设置」，`src/configs/settings_converter.cpp:325-328` 只在非空时覆盖 Settings，所以**缺键 → 保留编译默认值**。这条语义正好可以直接复用。
- 序列化：`src/configs/ConfigSerializer.cpp:53`（键名）、`:200`（写表）、`:332`（读表）。读路径是「原样字符串」，不做任何合法性校验——第三方自定义语言名因此可以工作。
- 反向转换：`src/configs/settings_converter.cpp:422` `configuration.appearance.language = settings.appearance.language;`（整份配置每次保存都会重写，见 `src/ImeApp.cpp:755` → `include/ui/SettingsManager.h:36-40`）。

**要点**：`language` 是「不校验的字符串」，加一个哨兵值的成本很低；反过来看，任何把解析结果写回配置的实现都会**冻住**首次启动时的语言（见 §6 约束 C2）。

### 2.2 加载侧：只有两个调用点

| 位置 | 时机 | 作用 |
|---|---|---|
| `src/ui/ImeOverlay.cpp:79-83`（ctor，被 `src/ImeWnd.cpp:155` 以 `settings.appearance.language` 构造） | 插件启动、语言栏首次创建 | `i18n::SetTranslator(&m_translator)` + `i18n::UpdateTranslator(language, "english", utils::GetPluginInterfaceDir())` |
| `src/ui/panels/AppearancePanel.cpp:783-786` | 用户在设置界面切换语言 | 同上，热换（ADR-0001 Option C） |

除此以外**没有任何**地方消费 `appearance.language`（已全仓确认：`src/` 内仅上述两处 + 配置读写两处）。

### 2.3 i18n 是子模块能力，已足够

- `extern/JamieMods/common/i18n/translator_manager.h:44-47`：`UpdateTranslator(language, fallbackLanguage, dir, filePattern)`——**已经带 fallback 语义**（`translator_manager.cpp:108-123`：目标文件不存在 → 尝试 fallback → 都失败则 warn 并保持原表）。
- 文件名约定：`TRANSLATE_FILE_PATTERN = "translate_{}.toml"`、扫描正则 `translate_(\w+)\.toml`（`translator_manager.h:16-19`，`ScanLanguages` 在 `.cpp:125-148`）。
- 当前打包 6 种：`english / chinese / japanese / korean / russian / german`（`contrib/Distribution/translate/`）。
- 缺键行为：`Translator::Translate`（`Translator.h:54-61`）查不到就返回**键名本身**，不会回落英文。新增 UI 文案必须同时加到 6 个文件，否则第三方旧翻译文件会显示成 `Settings.Appearance.LanguageAuto`（见 §4.5 第 4 步的 `TranslateOr`）。

### 2.4 现有测试把默认值钉死了

- `test/settings_converter_test.cpp:375-411` `ShippedConfigurationTest`：出厂 `contrib/config/SimpleIME.toml` 经真实 loader + converter 后必须**逐字段等于** `GetDefaultSettings()`，含 `:411` 的 `appearance.language`。
- `test/settings_converter_test.cpp:239-240`：`GetDefaultSettings()` 必须是 save/load 的不动点。
- `test/settings_converter_test.cpp:243-274`：`GetDefaultConfiguration()`（`language = ""`）→ Settings 必须等于默认 Settings。
- `test/ConfigSerializerTest.cpp:48-83`：随机 Configuration 往返（`test/RandomUtils.h:72` 把 language 设成随机 10 字符串）——意味着**不能给读路径加白名单校验**。
- 基线：`build/test-check/SimpleIMETest.exe` 当前 **106/106 通过**（本次实测，2026-10-23）。

## 3. 系统语言从哪取

### 3.1 API 选型

首选 `GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, ...)`：返回用户**偏好顺序**的 BCP-47 语言名列表（`zh-Hans-CN\0en-US\0\0`），取第一项。这正是 Windows 自己渲染其 UI 的依据，多语言安装下比 `GetSystemDefaultUILanguage` 准确。

回退 `GetUserDefaultUILanguage()` → `LCIDToLocaleName()`：仓内已有同族用法可抄——`src/tsf/InputMethodManager.cpp:94-98`（`GetLocaleName(LANGID)`），`src/ui/fonts/FontManager.cpp:109-123`（`GetUserDefaultLocaleName`）。两者都在 kernel32，无新链接依赖，无 SKSE 依赖。

### 3.2 映射表（locale 主语言子标签 → 翻译文件名）

| Windows locale 前缀 | 翻译名 | 打包文件 |
|---|---|---|
| `zh` | `chinese` | `translate_chinese.toml` |
| `ja` | `japanese` | `translate_japanese.toml` |
| `ko` | `korean` | `translate_korean.toml` |
| `ru` | `russian` | `translate_russian.toml` |
| `de` | `german` | `translate_german.toml` |
| `en` | `english` | `translate_english.toml` |

按主语言子标签（首个 `-` 之前）匹配，所以 `zh-CN / zh-Hans-CN / zh-SG` 都落到 `chinese`，无需枚举区域。表里没有的（`pt-BR`、`fr-FR`…）→ 回退 `english` 并记一条日志。

**已知偏差（可接受，需写进文档）**：

- Windows **UI 语言** ≠ **键盘/输入语言**。英文 Windows + 中文输入法的用户，`auto` 会得到英文界面。这就是「跟随系统」的字面语义；若改成按输入法定语言是另一个需求。
- `zh-TW`（繁体）会落到唯一的中文文件（简体 `translate_chinese.toml`）。要根治需再出一个繁体翻译文件，不在本方案范围。
- `auto` 在**每次启动**解析一次；游戏运行期间改系统语言不会即时生效（没必要：改完重启才符合直觉，也避免在渲染线程反复查 API）。

## 4. 方案

### 4.1 核心决策：哨兵值 + 使用时解析（resolve-at-use）

- 配置值与 `Settings::appearance.language` 里**保存 `"auto"` 原文**。
- 只在「要交给 `i18n::UpdateTranslator`」的那一刻解析成具体语言名。
- 解析结果**不写回**任何持久状态。

理由：写回会让「跟随系统」退化成「跟随第一次启动时的系统」，恰好是需求 2 要避免的。整份配置每次保存都会被重写（`SettingsManager::Save`），所以只要内存里保留 `auto`，落盘自然还是 `auto`。

### 4.2 新增文件一：纯逻辑头（可离线单测）

`include/utils/LanguagePreference.h`——沿用仓内既有的 `*Logic` 风格约定（参考 `include/utils/CharDeliveryLogic.h:1-11`、`include/utils/ConversionMemoryLogic.h:1-11`：header-only、零 Windows/SKSE 依赖、进无头测试）。

```cpp
//
// Pure decision logic for "language = auto": map the OS UI locale onto a
// bundled translation name. Header-only and free of Windows/SKSE
// dependencies so the headless suite covers it.
//
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace Ime::LanguagePreference
{
/// Sentinel stored in appearance.language / [appearance] language. Kept in the
/// config VERBATIM: writing the resolved name back would freeze the choice at
/// the first launch of a machine whose system language later changes.
inline constexpr std::string_view kAutoLanguage = "auto";

struct LocaleEntry
{
    std::string_view subtag;   ///< primary language subtag of a Windows locale name
    std::string_view language; ///< must match translate_<language>.toml
};

inline constexpr std::array kLocaleTable{
    LocaleEntry{"zh", "chinese"}, LocaleEntry{"ja", "japanese"}, LocaleEntry{"ko", "korean"},
    LocaleEntry{"ru", "russian"}, LocaleEntry{"de", "german"},   LocaleEntry{"en", "english"},
};

constexpr auto AsciiLower(const char c) -> char
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// "zh-Hans-CN" -> "chinese". Empty when the primary subtag is not bundled.
[[nodiscard]] constexpr auto FromLocaleName(const std::string_view localeName) -> std::string_view
{
    const auto end    = localeName.find('-');
    const auto subtag = localeName.substr(0, end == std::string_view::npos ? localeName.size() : end);
    for (const auto &entry : kLocaleTable)
    {
        if (entry.subtag.size() != subtag.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < subtag.size(); ++i)
        {
            same = same && AsciiLower(subtag[i]) == entry.subtag[i];
        }
        if (same) return entry.language;
    }
    return {};
}

/// Resolve the configured value into the translation name to load.
/// Non-"auto" values pass through untouched (an unknown name keeps today's
/// behaviour: the loader's own english fallback decides).
[[nodiscard]] inline auto Resolve(
    const std::string_view configured, const std::string_view systemLocale, const std::string_view fallbackLanguage
) -> std::string
{
    if (configured != kAutoLanguage) return std::string{configured};
    const auto detected = FromLocaleName(systemLocale);
    return std::string{detected.empty() ? fallbackLanguage : detected};
}

// Platform binding (src/utils/LanguagePreference.cpp).
[[nodiscard]] auto GetSystemLocaleName() -> std::string;
/// Resolve @p configured and hot-swap the translator. Returns the name actually
/// requested (the loader's own english fallback still applies when that file is
/// missing). @p dir must be the same dir the settings combo scans.
auto LoadForConfigured(std::string_view configured, const std::filesystem::path &dir, std::string_view fallbackLanguage = "english") -> std::string;

} // namespace Ime::LanguagePreference
```

放在 `include/utils/` 而**不**新建 `include/i18n/`：后者会和子模块的 `i18n/` 目录在 include 搜索路径上同名（`CMakeLists.txt:107-113`，子模块的 common 目录排在前面），虽然当前不会冲突，但属于没必要的隐患。

### 4.3 新增文件二：Win32 绑定

`src/utils/LanguagePreference.cpp`（主目标由 `CMakeLists.txt:56` 的 `GLOB_RECURSE sources src/*.cpp` 自动收编；测试目标在 `test.cmake:9-16` 里是显式列表，只需要纯逻辑头，无需改动）。

```cpp
#include "utils/LanguagePreference.h"

#include "i18n/translator_manager.h" // extern/JamieMods/common
#include "log.h"
#include "WCharUtils.h"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace Ime::LanguagePreference
{
namespace
{
/// The user's preferred UI language as a BCP-47 name ("zh-Hans-CN").
/// GetUserPreferredUILanguages is what Windows renders its own UI in; a
/// multi-language install can prefer a language GetSystemDefaultUILanguage
/// does not report. LCIDToLocaleName is the fallback for the rare case where
/// the MUI list is unavailable.
auto QuerySystemLocaleName() -> std::string
{
    ULONG count = 0;
    ULONG chars = 0;
    if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars) && chars > 0)
    {
        std::wstring buffer(chars, L'\0');
        if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &chars))
        {
            // NUL-separated list, double-NUL terminated; the head is preferred.
            if (const auto nul = buffer.find(L'\0'); nul != std::wstring::npos) buffer.resize(nul);
            if (!buffer.empty()) return WCharUtils::ToString(buffer);
        }
    }
    const LANGID langid = ::GetUserDefaultUILanguage();
    if (langid == 0) return {};
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (::LCIDToLocaleName(MAKELCID(langid, SORT_DEFAULT), localeName, LOCALE_NAME_MAX_LENGTH, 0) > 0)
    {
        return WCharUtils::ToString(localeName);
    }
    return {};
}
} // namespace

auto GetSystemLocaleName() -> std::string
{
    // The OS UI language cannot change under a running game in any way we
    // observe; one query per process is enough and keeps the render thread off
    // the registry.
    static const std::string cached = QuerySystemLocaleName();
    return cached;
}

auto LoadForConfigured(const std::string_view configured, const std::filesystem::path &dir, const std::string_view fallbackLanguage) -> std::string
{
    const auto resolved = Resolve(configured, GetSystemLocaleName(), fallbackLanguage);
    logger::info("i18n: UI language '{}' resolved to '{}' (system locale '{}')", configured, resolved, GetSystemLocaleName());
    i18n::UpdateTranslator(resolved, fallbackLanguage, dir);
    return resolved;
}
} // namespace Ime::LanguagePreference
```

那行日志是刻意设计的**验证锚点**（`docs/VALIDATION.md` 要求可 grep 的判据）。

### 4.4 接线（两处调用点）

`src/ui/ImeOverlay.cpp:79-83`：

```cpp
ImeOverlay::ImeOverlay(std::string_view language)
{
    i18n::SetTranslator(&m_translator);
    //! "auto" resolves against the OS UI locale HERE, at the only moment the
    //! translator is built; the config keeps the sentinel (LanguagePreference.h).
    Ime::LanguagePreference::LoadForConfigured(language, utils::GetPluginInterfaceDir());
}
```

`src/ui/panels/AppearancePanel.cpp:783-786`：把 `i18n::UpdateTranslator(appearance.language, "english", utils::GetPluginInterfaceDir())` 换成同一个 `LoadForConfigured(...)`。

注意 `ImeOverlay` 的 `language` 形参来自 `src/ImeWnd.cpp:155`，是**构造时快照**，之后不再读取——所以热切换只影响 translator，不需要动 `ImeWnd`。

### 4.5 设置界面：下拉里多一项「跟随系统」

`src/ui/panels/AppearancePanel.cpp:742-787` 现状是「遍历 `m_translateLanguages`（构造时扫描的结果，见 `:273-285`）」。四处改动：

**第 1 步 — 菜单首项用固定 ID**（不能用循环下标，否则与循环项的 ImGui ID 撞车）：

```cpp
bool       clicked      = false;
const bool autoSelected = appearance.language == LanguagePreference::kAutoLanguage;
UI::Panels::SettingsComboRow("##LanguagesCombo", Translate("Settings.Appearance.Languages"), {}, preview, [&] {
    ImGui::PushID("##LanguageAuto");
    if (UI::Panels::ComboOption(TranslateOr("Settings.Appearance.LanguageAuto", "Follow system"), autoSelected))
    {
        appearance.language = std::string{LanguagePreference::kAutoLanguage};
        clicked             = true;
    }
    if (autoSelected) ImGui::SetItemDefaultFocus();
    ImGui::PopID();

    int32_t idx = 0;
    for (const auto &language : m_translateLanguages) { /* 现有循环原样保留 */ }
});
```

**第 2 步 — preview 要自解释**。现版本 `preview` 直接取 `displayName(appearance.language)`，而 `auto` 不在 `m_nativeLanguageNames` 里，会露出裸 `auto`：

```cpp
std::string preview;
if (appearance.language == LanguagePreference::kAutoLanguage)
{
    const auto resolved = LanguagePreference::Resolve(LanguagePreference::kAutoLanguage, LanguagePreference::GetSystemLocaleName(), "english");
    preview = std::format("{} ({})", TranslateOr("Settings.Appearance.LanguageAuto", "Follow system"), displayName(resolved));
}
else
{
    preview = std::string{displayName(appearance.language)};
}
```

`displayName` 保持现有实现（`m_nativeLanguageNames` 命中用原名，否则用内部名），这样「跟随系统（中文）」里的中文名与下拉其他项同一数据源。

**第 3 步 — 扫描结果剔除保留字**（防用户真的放了 `translate_auto.toml`，下拉出现两个同值项）：

```cpp
i18n::ScanLanguages(translateDir, m_translateLanguages);
std::erase(m_translateLanguages, std::string{LanguagePreference::kAutoLanguage}); // "auto" 是保留字
```

**第 4 步 — `TranslateOr` 硬化**（放在 `AppearancePanel.cpp` 匿名 namespace，紧邻 `:230-250` 的 `ReadNativeLanguageName`）：

```cpp
//! Per-key English fallback: Translator::Translate returns the KEY when the
//! active file predates the key, so a third-party translation would render new
//! UI copy as "Settings.Appearance.LanguageAuto".
auto TranslateOr(const std::string_view key, const std::string_view english) -> std::string_view
{
    const auto translated = Translate(key);
    return translated == key ? english : translated;
}
```

### 4.6 默认值（两处必须同改）

| 文件 | 行 | 现在 | 改为 |
|---|---|---|---|
| `include/ui/Settings.h` | 205 | `.language = "english",` | `.language = "auto",`（加一行注释指向 LanguagePreference.h） |
| `contrib/config/SimpleIME.toml` | 54-55 | 注释 + `language = "english"` | 注释写明 `auto` = 跟随系统、仍可写 `english/chinese/japanese/korean/russian/german`；值改 `"auto"` |

`include/configs/configuration.h:139` 的 `""` **不动**（保持「未设置」语义，让缺键的用户拿到新默认值）。

### 4.7 配置格式化器补注释

`src/configs/ConfigSerializer.cpp:200` 的 `{KEY_LANGUAGE, configuration.appearance.language}` 是**唯一没有注释**的外观键——用户只要存一次配置，出厂模板里那段说明就没了，`auto` 会变成一个没人解释的字符串。补一个 `languageComment`（照 `:94` 的 `zoomComment` 写法）挂上去：

```cpp
const Comments languageComment = {
    " 界面语言：auto = 跟随系统语言(默认)；也可写 english / chinese / japanese / korean / russian / german",
    " 把 translate_<name>.toml 放进 interface\\SimpleIME\\ 即可扩展",
};
```

### 4.8 翻译文件（6 个都要加）

在各自 `[Settings.Appearance]` 段落、`Languages = ...` 之后加一行 `LanguageAuto`：

| 文件 | 锚点行 | 建议文案 |
|---|---|---|
| `translate_english.toml` | `[Settings.Appearance]` :54，`Languages` :59 | `LanguageAuto = "Follow system"` |
| `translate_chinese.toml` | :55 / :60 | `LanguageAuto = "跟随系统"` |
| `translate_japanese.toml` | :12 / :17 | `LanguageAuto = "システムに従う"` |
| `translate_korean.toml` | :12 / :17 | `LanguageAuto = "시스템 설정 따르기"` |
| `translate_german.toml` | :54 / :59 | `LanguageAuto = "Systemsprache"` |
| `translate_russian.toml` | :54 / :59 | `LanguageAuto = "Как в системе"` |

### 4.9 测试

新增 `test/LanguagePreferenceLogicTest.cpp`（`test.cmake:3` 的 `GLOB_RECURSE test/*.cpp` 自动收编），表驱动覆盖：

1. `FromLocaleName`：`zh-CN / zh-Hans-CN / zh-SG / zh` → `chinese`；`ja-JP`→`japanese`；`ko-KR`→`korean`；`ru-RU`→`russian`；`de-DE`→`german`；`en-US`→`english`。
2. 大小写不敏感：`ZH-Hans-CN` → `chinese`。
3. 未打包 locale：`pt-BR` / `fr-FR` / `""` → `""`。
4. `Resolve("english", "zh-CN", "english") == "english"`（显式选择永远压过系统）。
5. `Resolve("auto", "ja-JP", "english") == "japanese"`。
6. `Resolve("auto", "pt-BR", "english") == "english"`（未打包 → fallback）。
7. `Resolve("auto", "", "english") == "english"`（探测失败 → fallback）。
8. `Resolve("klingon", ...) == "klingon"`（未知显式值原样透传——保住 `ConfigSerializerTest` 的往返不变量）。
9. `Resolve("auto", "zh-TW", "english") == "chinese"`（把「繁体 → 简体文件」这条已知偏差钉成测试，将来改它必须显式改测试）。

## 5. 边界与明确不做的事

- **不改 `extern/JamieMods` 子模块**。它是 fork 出来的子模块（`.gitmodules` 里钉着分支/commit），任何改动都要连带 bump 指针；而 `UpdateTranslator(language, fallback, dir)` 已经够用。这一条是选择「在外面解析」而不是「给 i18n 加 auto 支持」的**决定性理由**。
- **不做运行期热跟随**：不挂系统语言变更通知，不每帧查询。
- **不做升级迁移**（已定）：分发的 `interface/SimpleIME/SimpleIME.toml` 出厂即为 `auto`，新安装开箱跟随系统；已有配置保持原值（3.2.x 每次保存写入的 `english` 属于用户既有状态），升级不会在用户不知情时改界面语言——要切换只需在设置里点一次「跟随系统」。
- **不改 `ImeWnd` / `LanguageBar`**：它们不消费 `appearance.language`。
- **不改读路径的合法性校验**：`ConfigSerializerTest` 的随机字符串往返依赖「原样透传」；未知值由 `UpdateTranslator` 的 fallback 兜底（今天的既有行为）。
- **不动 `dist/` 下的产物**：`dist/SimpleIME/...`、`dist/_CPack_Packages/...` 是安装/打包输出，改 `contrib/` 后重新 install 会覆盖。
- **字体**：`include/ui/Settings.h:27-32` 已把系统 CJK 字体并进 UI 字体链（Malgun Gothic / Yu Gothic / MS Gothic），默认切到中日韩不会出现豆腐块；用户仍可用 `[resources] fonts` 追加。此项无需改动，但实施后要在真实游戏里确认一次。

## 6. 风险与约束

- **C1（硬约束）**：`Settings.h:205` 与 `contrib/config/SimpleIME.toml:55` 必须同步，否则 `ShippedConfigurationTest` 失败。这是好事——它正好防住「只改代码不改模板」。
- **C2（设计约束）**：**永不**把解析结果写回 `Settings::appearance.language` 或配置文件，否则「跟随系统」会退化成一次性快照。
- **C3**：`ImeOverlay` ctor 里新增一次 `GetUserPreferredUILanguages` + 一行日志，属于启动期的一次性查询，量级可忽略（与已有的 TOML 解析同处一个函数）。
- **R1（低）**：用户放 `translate_auto.toml` 撞保留字 → §4.5 第 3 步剔除 + 文档写明 `auto` 是保留字。
- **R2（低）**：第三方旧翻译文件缺 `LanguageAuto` → §4.5 第 4 步 `TranslateOr` 兜底成英文 "Follow system"。
- **R3（中，需人工确认）**：系统语言为繁体（`zh-TW`）时落到简体文件，用户可能觉得不对。文档明确记录，后续可出繁体翻译文件。
- **回滚**：把 §4.6 两处默认值改回 `english` 即退回现状；哨兵值与解析逻辑留着无害（配置里不会出现 `auto`）。

## 7. 备选方案（记录为何不选）

**方案 B：新增 `bool followSystemLanguage` + 保留 `std::string language`。**

- 优点：值空间干净，`language` 永远是真实文件名，不需要保留字。
- 缺点：要同时改 `include/ui/Settings.h`（结构体 + 聚合初始化）、`include/configs/configuration.h`、`ConfigSerializer`（键名 + 写表 + 读表）、`settings_converter`（双向各一处）、`test/RandomUtils.h:72`、`test/settings_converter_test.cpp` 的三处断言、出厂 TOML——约是方案 A 的两倍面；而且 `GetDefaultSettings()` 是按成员顺序做聚合初始化的（`Settings.h:203-209`），插字段会牵动整段具名初始化。
- 结论：为了同样的可观测行为，diff 与回归面都更大。**选方案 A**。若维护者更看重「值空间纯净」，方案 B 是唯一合理的替代路径。

**方案 C：在 i18n 子模块里支持 `auto`。** 否掉——子模块 churn，见 §5 第一条。

**方案 D：按游戏语言（`Skyrim.ini` 的 `sLanguage`）解析。** 否掉——那是游戏**文本/配音**语言，与玩家想看的设置界面语言不必一致，还会引入解析游戏 INI 的新耦合。

## 8. 改动清单（按提交切分）

**commit 1 — 逻辑与绑定（无行为变化）**
1. 新增 `include/utils/LanguagePreference.h`（§4.2）。
2. 新增 `src/utils/LanguagePreference.cpp`（§4.3）。
3. 新增 `test/LanguagePreferenceLogicTest.cpp`（§4.9）。

**commit 2 — 接线与默认值（行为切换）**
4. `src/ui/ImeOverlay.cpp:79-83`（§4.4）。
5. `src/ui/panels/AppearancePanel.cpp:273-285`（剔除保留字）、`:742-787`（哨兵项 + preview + `LoadForConfigured`）、匿名 namespace（`TranslateOr`）。
6. `include/ui/Settings.h:205` + `contrib/config/SimpleIME.toml:54-55`（§4.6，必须同一 commit）。
7. `src/configs/ConfigSerializer.cpp`：新增 `languageComment`（`:94` 附近）并挂到 `:200`。

**commit 3 — 文案与文档**
8. 6 个 `contrib/Distribution/translate/translate_*.toml` 的 `LanguageAuto`（§4.8）。
9. `README.md:176-180`（Localization 段）加一句：默认跟随系统语言，可在设置界面或 `[appearance] language` 里固定。
10. 可选：`docs/adr/0004-ui-language-follows-system.md`，记录「哨兵值 + 使用时解析、不回写」以及 §6 的 C1/C2 两条约束（照 ADR-0001…0003 的惯例）。

## 9. 验证

### 9.1 离线（必须全绿）

```cmd
REM 新增了 src/*.cpp —— CMake 用 GLOB（CMakeLists.txt:55-56），必须重新 configure。
REM 注意 docs/VALIDATION.md:22：reconfigure 必须带 MO2_MODS_PATH，否则 POST_BUILD 的 MO2 同步块静默消失。
build\_reconfigure_tests.cmd
cmake --build build\test-check
build\test-check\SimpleIMETest.exe --gtest_brief=1
```

判据：

- `ShippedConfigurationTest`（钉子）通过 —— 证明 `Settings.h` 与出厂 TOML 同步。
- 新增 `LanguagePreferenceLogicTest` 全绿；总数应从基线 **106** 增加到 106 + 新用例数。
- `ConfigSerializerTest` / `settings_converter_test` 的往返与默认值不动点仍通过（证明哨兵值没有破坏 `language` 的透传语义）。

主 DLL：`build\_reconfigure.cmd` 后 `cmake --build build\RelWithDebInfo-clangcl-ninja-vcpkg`，确认 EXIT 0（`-Wconversion` 已开，注意 `ULONG` → `size_t` 之类的窄化告警）。

#### 实现结果（2026-10-23，分支 `feat/follow-system-language`）

| 项 | 结果 |
|---|---|
| 测试 reconfigure | `build\_reconfigure_tests.cmd` EXIT 0 |
| `cmake --build build\test-check` | EXIT 0（仅新增文件与既有文件的既有告警） |
| `SimpleIMETest.exe --gtest_brief=1` | **116 tests / 42 suites 全 PASS**（基线 106 → +10，全部来自 `LanguagePreferenceLogicTest`） |
| `ShippedConfigurationTest` | 通过 —— `Settings.h:207` 与出厂 TOML 已同步（C1 未被破坏） |
| 主 DLL | reconfigure + build EXIT 0；`src/utils/LanguagePreference.cpp`、`src/ui/ImeOverlay.cpp`、`src/ui/panels/AppearancePanel.cpp` 三个 TU **零新增告警**（`-Wconversion`/`-Wshadow`/`-Wpedantic` 下） |
| 游戏内 | ⬜ 未验证（§9.2 六项待人工） |

### 9.2 游戏内（只能人工）

| # | 操作 | 期望 |
|---|---|---|
| 1 | 删掉/改名配置，或删掉 `language` 键；系统 UI 语言 = 中文 | 启动后设置界面为中文；日志 `i18n: UI language 'auto' resolved to 'chinese' (system locale 'zh-Hans-CN')` |
| 2 | 同上，系统 UI 语言 = 英文 | 英文界面；日志 `... resolved to 'english' ...` |
| 3 | 打开设置看语言下拉 | 第一项是「跟随系统（中文）」且当前选中，其余 6 项照旧 |
| 4 | 切到 English 并保存，重进游戏 | 英文界面，配置写 `language = "english"`（不再跟随系统） |
| 5 | 切回「跟随系统」并保存，重进游戏 | 配置写 `language = "auto"`，界面回到系统语言 |
| 6 | 语言栏的设置按钮 tooltip（`Translate("Settings.Settings")`） | 与界面语言一致（证明启动期解析生效，而非仅在设置窗打开后） |

按惯例把结论补进 `docs/VALIDATION.md` 新版本一节（✅ 已实测 / 🧪 仅离线 / ⬜ 未验证）。

## 10. 一句话总结

界面文本本来就只是一个可热换的查表，语言选择也只是一个不校验的字符串——所以「跟随系统」不需要任何架构改动：**加一个哨兵值 `auto`、在加载时用 `GetUserPreferredUILanguages` 解析一次、解析结果只用于这一次加载**，即可满足全部三条验收行为；风险集中在「两处默认值必须同改」和「解析结果绝不回写」这两个约束上，后者可直接落成测试。
