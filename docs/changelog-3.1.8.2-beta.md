Fixed
- Google 日本語入力: half-width alphanumeric input still went missing on
  3.1.8.1-beta. Clearing the composition flags at the top of OnEndComposition
  was not enough — Google keeps its candidate UI element registered after a
  commit, so BeginUIElement re-set IN_CAND_CHOOSING on its own (its interface
  lookup fails there, and the bit was claimed before the lookup), and the
  WM_CHAR gate treated either bit as a composition echo. The gate now drops
  only while IN_COMPOSING stands, and the candidate bit is claimed only after
  the candidate interface has actually been acquired.
- Diagnostics: the WM_CHAR drop line reports both flags separately, so a
  future report can tell which bit was standing.
- Chinese IMEs: the 英 全角 → 中 全角 flip on reopen is now also covered for
  sessions running with switchEnglishLayoutOnDisable on. That path restores
  the input method profile after the fact, so it now arms the remembered-mode
  restore as well instead of letting the restored profile's default mode land
  unchecked.

Notes
- Google 日本語入力 maps Caps Lock to 半角英数 (0x0) and a bare Shift press to
  全角英数 (0x8); the plugin only follows what the IME publishes. Use Caps Lock
  for half-width alphanumerics.
