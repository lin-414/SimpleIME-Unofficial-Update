Fixed
- Microsoft IME: Shift-toggling to half-width alphanumeric left the full-width
  bit set, so closing and reopening the console restored 全角. The raw-ASCII
  path now clears both the native and full-shape bits.
- Google 日本語入力: half-width alphanumeric keystrokes were dropped while a
  stale composition flag stood after a Japanese commit. The flag is now
  cleared at the top of OnEndComposition, before the commit callback can fail
  or re-enter.
- Japanese IME: a kana sighting in the composition preview no longer flips
  全角英数 into かな — the language bar no longer turns Ａ into あ while
  full-width alphanumerics are being typed. A real kana switch is published by
  the IME itself, so only the full-width alphanumeric state is protected.
- Chinese IMEs: closing the console no longer flips the language bar from
  英 半角 to 中 半角. The disable path no longer flattens the conversion mode
  (some IMEs answered the 0x0 write by republishing NATIVE), and conversion
  updates are ignored while the IME is disabled, so the remembered mode matches
  what the next session shows.
- Chinese IMEs: reopening the console in 英 全角 no longer sometimes restores
  中 全角. The remembered mode is published while the conversion sink is still
  guarded, and IME_DISABLED is cleared only after the one-shot re-assert is
  armed, so the IME's own default lands through the correction path.
- Chinese IMEs: starting a composition in 英 全角 no longer asserts NATIVE.
  A Chinese IME composes full-width alphanumerics too, so the first keystroke
  turned 0x8 into 0x9 and the commit stamped 0x9 as the remembered mode — the
  next console open came back in 中 全角.
- Chinese IMEs: a value carrying bits outside TF_CONVERSIONMODE is no longer
  stamped as the remembered mode. Microsoft Pinyin publishes 0x401/0x409 when
  it re-initialises, and 0x409 (en-US) reads as NATIVE|FULLSHAPE in its low
  nibble; the context compartment is used instead.
