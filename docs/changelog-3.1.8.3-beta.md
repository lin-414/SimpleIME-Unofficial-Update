Fixed
- Google 日本語入力: typing kana from a half-width alphanumeric session left the
  mode at 0x1 (NATIVE without FULLSHAPE) instead of the IME's ひらがな state
  (0x9). The stamped mode then did not round-trip through the TIP on the next
  console open. Composition kana evidence now also sets FULLSHAPE.
- Japanese IMEs: reopening the console always restored かな even when the IME
  had been left in 英数/Direct Input. The session start wrote a fixed open=1;
  for these IMEs the open/close bit is the かな/英数 switch, so it is now
  captured on close and restored on open.

Notes
- Google 日本語入力 does not publish a conversion mode when it switches to
  Direct Input (or half-width alphanumerics); it only writes the open/close
  compartment. The plugin learns the mode from the first character that reaches
  WM_CHAR, so switching and closing the console without typing anything leaves
  the previous mode remembered.
