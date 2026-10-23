# ADR-0004: The UI language can follow the system locale

**Status:** Accepted
**Date:** 2026-10-23
**Context files:**

- `include/utils/LanguagePreference.h`
- `src/utils/LanguagePreference.cpp`
- `include/ui/Settings.h` (`Appearance::language` default)
- `contrib/config/SimpleIME.toml` (`[appearance] language`)
- `src/ui/panels/AppearancePanel.cpp` (`DrawLanguagesCombo`, constructor)

---

## Background

`appearance.language` is a plain, unvalidated string that names the file
`Data/interface/SimpleIME/translate_<language>.toml`; it defaulted to `"english"`
and the only consumers are the two `i18n::UpdateTranslator()` call sites
(`ImeOverlay` constructor, `AppearancePanel::DrawLanguagesCombo`).

A fresh install on a Chinese / Japanese / Korean / Russian / German system
therefore opened its settings in English until the user found the Languages
combo. The requirement was to follow the OS UI language **by default**, while
still letting the user pin a language and while letting a machine whose system
language changes later pick the new language up.

That last clause is the whole problem: any implementation that resolves the
locale once and persists the result degrades "follow the system" into "follow
the system as it was at first launch".

---

## Decision

1. Store a **sentinel** `"auto"` in the config and in `Settings::appearance.language`
   (`Ime::LanguagePreference::kAutoLanguage`).
2. **Resolve at use**: only the moment a translator is (re)built maps the sentinel
   onto a bundled translation name, via `GetUserPreferredUILanguages(MUI_LANGUAGE_NAME)`
   with `GetUserDefaultUILanguage` + `LCIDToLocaleName` as fallback
   (`LanguagePreference::LoadForConfigured`).
3. **Never write the resolved value back** into `Settings` or the config.
4. Bind the two existing call sites to `LoadForConfigured`; `ImeOverlay` stays the
   lifecycle owner (ADR-0001).
5. `DrawLanguagesCombo` draws one extra first entry ("Follow system"), reserved:
   the scanned language list has `auto` erased from it.

The locale -> file name table is `zh -> chinese, ja -> japanese, ko -> korean,
ru -> russian, de -> german, en -> english`; an unbundled locale falls back to the
caller's fallback language (today `english`), which is exactly what the loader
does for a missing file.

---

## Alternatives considered

- **A separate `bool followSystemLanguage` field.** Rejected: needs a structural
  change to `Settings`, `Configuration`, the serializer and the converter in both
  directions, plus `test/RandomUtils.h` — roughly twice the diff for the same
  observable behaviour, and it puts two fields that can disagree in the config.
- **Teach the i18n submodule about `auto`.** Rejected: `extern/JamieMods` is a
  pinned fork; a submodule bump for a feature the existing
  `UpdateTranslator(language, fallback, dir)` API already supports is churn.
- **Parse `Skyrim.ini`'s `sLanguage`.** Rejected: that is the game's text/voice
  language, which need not match the language the player wants the mod UI in.
- **Resolve once at load and write the result back.** Rejected: freezes the choice
  (see Background); this is constraint C2 below.

---

## Consequences

- **C1 (hard):** `include/ui/Settings.h`'s `.language` default and
  `contrib/config/SimpleIME.toml`'s `language` must change together —
  `ShippedConfigurationTest` (`test/settings_converter_test.cpp`) asserts that the
  shipped file converts field-by-field to the compiled default.
- **C2 (design):** the resolved name must never be persisted. `Configuration`
  therefore keeps `""` as "unset" so an upgraded config that lacks the key lands on
  the new default; an existing `language = "english"` keeps English, i.e. the
  upgrade does not silently switch anyone's UI.
- Third-party translation files predating the new `Settings.Appearance.LanguageAuto`
  key would render the raw key (`Translator::Translate` returns the key when it
  misses), so the panel uses a local `TranslateOr()` giving a per-key English
  fallback. Bundled files all ship the key.
- **No migration, by decision.** The packaged `interface/SimpleIME/SimpleIME.toml`
  (installed from `contrib/config/SimpleIME.toml`) ships `language = "auto"`, so a new
  install follows the system out of the box. An existing config keeps whatever value it
  has — including the `english` that every 3.2.x save wrote — so upgrading never changes
  anyone's UI language behind their back; switching is one click on *Follow system*.
- `zh-TW` resolves to `chinese` (only one Chinese file ships). Documented and
  pinned by `test/LanguagePreferenceLogicTest.cpp`; fixing it means adding a
  traditional-Chinese translation and updating that test.
- Resolution happens once per translator rebuild (startup, plus a combo click).
  No OS locale-change notifications and no per-frame queries: `GetSystemLocaleName`
  caches its answer for the process lifetime.
