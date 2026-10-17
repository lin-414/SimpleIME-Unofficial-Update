Fixed
- Japanese IME: the language bar showed あ while full-width alphanumeric was
  being typed. Composition content now corrects the mode on every update.
- The bare-Shift 中/英 prediction no longer runs on Japanese IMEs.
- Profile re-activation is throttled to once per 800 ms, so repeated 中/英
  toggles no longer re-arm the IME every few hundred milliseconds.
- The remembered mode no longer treats an earlier mirror as the IME's own word.
- Chinese: 全角/半角 is its own language-bar label instead of replacing 中/英.
- Persists the full conversion mode instead of a native/alphanumeric bool.
