Added
- Candidate pagination in the self-drawn candidate window: `‹` / `›` buttons on
  the candidate row (and PageUp/PageDown) ask the IME for the previous/next
  page. The ImGui window mirrors the TIP's page, so the IME stays the owner of
  the paging state. The buttons are arrows only — no page counter.
- Up/Down walks the highlighted candidate inside the current page; Space or
  Enter commits the highlighted entry. Left/Right stay free for the IME's own
  caret movement inside the pinyin string.

Fixed
- Imm32 backend: PageDown on the last (or a single) page no longer strands
  `dwPageStart` on the last candidate index. The page only advances when a
  full next page exists, so the IME's page window keeps its shape.
- Candidate page arrows no longer dispatch a page request while disabled
  (first page's `‹`, last page's `›`).
- Enter now commits the highlighted candidate instead of the TIP's stale
  selection: the local highlight move is not mirrored into the TIP, so the
  commit resolves the index from the same cached list as Space and clicks.
- Also includes the 3.1.8.1–3.1.8.3 fixes: half-width alphanumeric input no
  longer disappears after a Japanese commit, 全角/半角 mode flips on console
  reopen are gone, and the Japanese IME's かな/英数 switch is remembered
  across sessions.

Notes
- Pagination relies on the IME answering the forwarded PageUp/PageDown.
  Microsoft Pinyin and the Imm32 backend handle them; a TIP that ignores the
  forwarded key simply keeps its current page.
- The candidate window's Up/Down navigation is local to the plugin: the TSF
  keyboard channel is deliberately disabled in this mod (see
  `ImeWnd::Run`), so the TIP never sees those keys.
