//
// Created by jamie on 2025/2/22.
//
#include "core/State.h"
#include "ime/ImeController.h"
#include "log.h"
#include "tsf/TextStore.h"
#include "tsf/TsfSupport.h"
#include "utils/CompositionScriptLogic.h"
#include "utils/ConversionMemoryLogic.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace
{
/// How long after a session start a conversion-mode write coming from the IME
/// itself still counts as a re-initialisation (and is corrected once) rather than
/// a deliberate user choice. See TextService::MaybeReassertRememberedMode.
///
/// Deliberately short. The write this defends against is the TIP re-initialising
/// its mode on the document-focus change, which lands within milliseconds of our
/// own session-start write; a deliberate toggle needs the user to see the text
/// entry and press Shift, i.e. hundreds of milliseconds at the very earliest. A
/// window wide enough to swallow the latter (the value here used to be 1500ms —
/// never exercised, because the mechanism was disarmed by our own echo before it
/// could fire) would revert the user's own Shift right after a console open,
/// which is the exact symptom this project spent three rounds removing.
constexpr ULONG MODE_REASSERT_WINDOW_MS = 250;

/// Minimum spacing between two profile re-activations driven by mode mirrors.
///
/// Re-activating the active TIP makes it re-initialise and publish its mode
/// again; that publication is indistinguishable from a user toggle here, so it
/// mirrored and re-activated in turn. In-game 2026-10-17 Microsoft Pinyin
/// alternated 0x401/0x409 every 240-400 ms with one profile activation per
/// flip — a self-sustaining loop that only ended when the user switched modes.
/// Long enough to swallow a few echo cycles, short enough that a genuine second
/// toggle still refreshes the language bar.
constexpr ULONGLONG PROFILE_REACTIVATE_GUARD_MS = 800;

/// The only bits a conversion mode may carry.
///
/// Chinese IMEs also publish their host language id through the conversion
/// compartment: Microsoft Pinyin alternates 0x401/0x409 while it re-initialises
/// after a profile activation, and 0x409 (en-US) reads as NATIVE|FULLSHAPE in
/// its low nibble. Stamping that as the remembered mode is how a console session
/// came back in 中 全角 while the user had left it in 英 全角 (in-game report,
/// 2026-10-17). A value with any bit outside TF_CONVERSIONMODE is not a mode and
/// must not be stamped; the context compartment, which carries the real value,
/// is then used instead.
///
/// TF_CONVERSIONMODE_SYMBOL (0x0400) is deliberately absent: an LCID such as
/// 0x409 carries that bit, and accepting it would let the very value this check
/// exists to reject through.
constexpr ULONG CONVERSION_MODE_MASK =
    TF_CONVERSIONMODE_ALPHANUMERIC | TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_KATAKANA |
    0x0004U /* TF_CONVERSIONMODE_HIRAGANA, missing from some SDK headers */ |
    TF_CONVERSIONMODE_FULLSHAPE | TF_CONVERSIONMODE_ROMAN | TF_CONVERSIONMODE_CHARCODE |
    TF_CONVERSIONMODE_SOFTKEYBOARD | TF_CONVERSIONMODE_NOCONVERSION | TF_CONVERSIONMODE_EUDC |
    TF_CONVERSIONMODE_FIXED;

/// True when `value` could be a conversion mode at all.
constexpr auto IsConversionModeValue(const ULONG value) -> bool
{
    return (value & ~CONVERSION_MODE_MASK) == 0;
}


/// Publish `value` through a compartment only when it does not already hold it.
///
/// The compartment sinks fire synchronously from inside SetValue, so a redundant
/// write costs a full round of sink handling — and, through MirrorConversionMode,
/// a profile re-activation — for a value nobody changed. Session start and menu
/// close fire repeatedly (five enables in one logged session), which is what made
/// every one of them re-publish the same mode.
///
/// Returns S_FALSE when the value was already there (nothing written), the
/// SetValue result otherwise, E_POINTER for a missing compartment. A value the
/// IME overwrote is therefore still written back: only an unchanged compartment
/// skips the call.
auto SetCompartmentIfDifferent(const CComPtr<Tsf::TsfCompartment> &compartment, const ULONG value) -> HRESULT
{
    if (compartment == nullptr)
    {
        return E_POINTER;
    }
    ULONG current = 0;
    if (SUCCEEDED(compartment->GetValue(current)) && current == value)
    {
        return S_FALSE;
    }
    return compartment->SetValue(value);
}
} // namespace

namespace Tsf
{
auto TextService::Initialize(ITfThreadMgr *threadMgr, TfClientId clientId) -> HRESULT
{
    HRESULT hr = threadMgr->QueryInterface(__uuidof(ITfThreadMgr), reinterpret_cast<void **>(&m_threadMgr));
    if (FAILED(hr))
    {
        logger::error("Can't get ITfThreadMgr: {:#X}", static_cast<uint32_t>(hr));
        return hr;
    }
    m_clientId                         = clientId;
    // The TIP's input language decides whether a composition's content may be
    // read as conversion-mode evidence at all (see CompositionScriptLogic.h).
    // Optional on purpose: a failure only costs the fallback in
    // GetInputLanguageId, never the TSF service.
    if (FAILED(m_inputProcessorProfiles.CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER)))
    {
        logger::warn("Can't create ITfInputProcessorProfiles; the input language falls back to the keyboard layout");
    }
    RefreshInputLanguage();
    m_textStore                        = new TextStore(this);
    m_conversionModeCompartment        = new TsfCompartment();
    m_contextConversionModeCompartment = new TsfCompartment();
    m_contextOpenCloseCompartment      = new TsfCompartment();
    m_keyboardOpenCloseCompartment     = new TsfCompartment();
    m_globalConversionModeCompartment  = new TsfCompartment();
    m_globalOpenCloseCompartment       = new TsfCompartment();

    hr = m_conversionModeCompartment->Initialize(
        m_threadMgr, clientId, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, [this](const GUID * /*guid*/, const ULONG ulong) {
            logger::debug("Thread conversion mode changed: {:#x}", ulong);
            UpdateConversionMode(ulong);
            return S_OK;
        }
    );

    if (SUCCEEDED(hr))
    {
        hr = m_keyboardOpenCloseCompartment->Initialize(
            m_threadMgr, clientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, [](const GUID *, const ULONG ulong) {
                logger::debug("Thread keyboard open/close changed: {}", ulong);
                State::GetInstance().Set(State::KEYBOARD_OPEN, ulong != 0);
                return S_OK;
            }
        );
    }
    if (SUCCEEDED(hr))
    {
        hr = m_textStore->Initialize(threadMgr, clientId);
    }

    if (SUCCEEDED(hr))
    {
        // The focused context's INPUTMODE_CONVERSION compartment is the live
        // source for the 中/英 conversion mode: IMEs (WeChat IME included) keep
        // the value the OS mode indicator displays here, while the
        // thread-manager-level compartment we also watch only holds the
        // per-thread default and is NOT updated on their Shift toggles.
        if (const auto contextCompartmentHr = m_contextConversionModeCompartment->Initialize(
                m_textStore->GetContext(),
                clientId,
                GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                [this](const GUID * /*guid*/, const ULONG ulong) {
                    // Fires only on real mode changes (Shift toggle etc.) — info
                    // level so an in-game test round can confirm the live sync.
                    logger::info("Context conversion mode changed: {:#x}", ulong);
                    UpdateConversionMode(ulong);
                    return S_OK;
                }
            );
            FAILED(contextCompartmentHr))
        {
            // Nice-to-have watcher: degrade to the thread-level fallback instead
            // of failing the whole TSF service over it.
            logger::warn("Can't watch the context conversion-mode compartment: {:#X}", static_cast<uint32_t>(contextCompartmentHr));
        }
    }

    if (SUCCEEDED(hr))
    {
        // Same idea for the open/close convention: per State.h, a Chinese IME's
        // Shift 中/英 toggle may be expressed as keyboard close/open instead of
        // conversion bits. When the focused context's open/close flips, mirror
        // it into the conversion mode (bit-wise, so FULLSHAPE etc. survive) —
        // IMEs that toggle conversion instead (MS Pinyin) never flip
        // open/close, so they are unaffected.
        if (const auto contextOpenCloseHr = m_contextOpenCloseCompartment->Initialize(
                m_textStore->GetContext(),
                clientId,
                GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                [this](const GUID * /*guid*/, const ULONG ulong) {
                    logger::info("Context keyboard open/close changed: {}", ulong);
                    auto &state = State::GetInstance();
                    state.Set(State::KEYBOARD_OPEN, ulong != 0);
                    // Only a Chinese TIP encodes 中/英 as open/close. Japanese
                    // uses it for かな/英数 while the conversion bits carry
                    // ひらがな vs 全角英数 (Microsoft IME publishes open=0 for
                    // English with the conversion channel already at 0x0), so
                    // deriving NATIVE from it would overwrite the mode that
                    // channel just reported — the shape that left the display on
                    // 「あ」 while the IME was typing 全角英数. An unresolved
                    // language (0) keeps the old inference: better a guess than
                    // no tracking at all for an IME that only uses this channel.
                    const LANGID languageId = GetInputLanguageId();
                    if (languageId == 0 || PRIMARYLANGID(languageId) == LANG_CHINESE)
                    {
                        if (ulong != 0)
                        {
                            state.AddConversionModeFlag(State::ConversionMode::Flags::NATIVE);
                        }
                        else
                        {
                            state.ClearConversionModeFlag(State::ConversionMode::Flags::NATIVE);
                        }
                    }
                    // This IS the IME publishing its 中/英 toggle (the shape some
                    // Chinese IMEs use instead of conversion bits) and it just
                    // drove the display above, so it has to count as a publish.
                    // Without the bump the deferred bare-Shift prediction sees an
                    // unchanged revision, concludes the IME said nothing and flips
                    // the display away from the truth it just wrote — the same
                    // race ConversionModeRevision exists to stop on the
                    // conversion-mode channel. The THREAD-level sink above stays
                    // out of this on purpose: it only tracks KEYBOARD_OPEN and
                    // leaves the display to the prediction (in-game 2026-10-16 the
                    // thread channel fired 11 times where the context one never
                    // did; bumping there would mute the prediction for exactly the
                    // IMEs that need it).
                    ++m_conversionModeRevision;
                    return S_OK;
                }
            );
            FAILED(contextOpenCloseHr))
        {
            logger::warn("Can't watch the context open/close compartment: {:#X}", static_cast<uint32_t>(contextOpenCloseHr));
        }
    }

    // Cross-process global compartment watchers: diagnostics only. The global
    // values are shared by every process on the machine (this is the channel
    // the taskbar mode indicator most plausibly tracks for IMEs that maintain
    // no context/thread value), but another process's IME activity must not
    // flip the in-game display, so these only log.
    if (CComPtr<ITfCompartmentMgr> globalCompartmentMgr; SUCCEEDED(threadMgr->GetGlobalCompartment(&globalCompartmentMgr)))
    {
        if (const auto globalConversionHr = m_globalConversionModeCompartment->Initialize(
                globalCompartmentMgr, clientId, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, [](const GUID * /*guid*/, const ULONG ulong) {
                    logger::info("Global conversion mode changed: {:#x}", ulong);
                    return S_OK;
                }
            );
            FAILED(globalConversionHr))
        {
            logger::debug("Can't watch the global conversion-mode compartment: {:#X}", static_cast<uint32_t>(globalConversionHr));
        }
        if (const auto globalOpenCloseHr = m_globalOpenCloseCompartment->Initialize(
                globalCompartmentMgr, clientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, [](const GUID * /*guid*/, const ULONG ulong) {
                    logger::info("Global keyboard open/close changed: {}", ulong);
                    return S_OK;
                }
            );
            FAILED(globalOpenCloseHr))
        {
            logger::debug("Can't watch the global open/close compartment: {:#X}", static_cast<uint32_t>(globalOpenCloseHr));
        }
    }
    else
    {
        logger::debug("Can't get the global compartment manager.");
    }

    return hr;
}

void TextService::UpdateConversionMode()
{
    ULONG conversionMode = 0;
    // Prefer the focused context's live value (what the active TIP actually
    // toggles); the thread-level compartment is only a per-thread default that
    // IMEs like WeChat never refresh. GetValue fails while a compartment has
    // never been written (VT_EMPTY), and on double failure State keeps its
    // current value instead of being reset to ALPHANUMERIC.
    if (m_contextConversionModeCompartment != nullptr && SUCCEEDED(m_contextConversionModeCompartment->GetValue(conversionMode)))
    {
        UpdateConversionMode(conversionMode);
        return;
    }
    if (SUCCEEDED(m_conversionModeCompartment->GetValue(conversionMode)))
    {
        UpdateConversionMode(conversionMode);
    }
}

void TextService::UpdateConversionMode(const ULONG conversionMode)
{
    // While the IME is disabled nothing may move the display. The disable path
    // used to flatten the conversion mode to 0x0 (ALPHANUMERIC) through
    // SetConversionMode, whose sink fires synchronously, and some Chinese IMEs
    // answer that write by republishing NATIVE (0x1). Letting either through
    // flipped the language bar from 「英 半角」 to 「中 半角」 right as the
    // console closed, and the persisted "last confirmed mode" then disagreed
    // with what the next session showed (in-game report, 2026-10-17).
    if (State::GetInstance().ImeDisabled())
    {
        return;
    }
    // Every observation of a published mode counts, our own mirror echo
    // included: the counter answers "did anything publish during this tap",
    // and the echo of the IME's own value is that same publication.
    // See ITextService::ConversionModeRevision.
    ++m_conversionModeRevision;
    State::GetInstance().SetConversionMode(conversionMode);
    // An observation can also be the IME overwriting its own mode right after a
    // session start — see MaybeReassertRememberedMode. When it corrected one, the
    // mirror must NOT run for this observation: it would write the value the
    // correction just rejected back into the compartments, in the same call
    // stack, undoing it (and posting a profile re-activation on top).
    if (!MaybeReassertRememberedMode(conversionMode))
    {
        MirrorConversionMode(conversionMode);
    }
}

/// Keep the other two channels on the value the IME just published.
///
/// Microsoft Pinyin reports its 中/英 toggle ONLY on the thread-level compartment
/// (in-game log 2026-10-16: `Thread conversion mode changed: 0x401` on Shift,
/// with neither a context nor a global write), and that has two consequences:
///  - the context compartment keeps the value WE last wrote, and our read paths
///    prefer the context one — a stale value there feeds the wrong mode into
///    State::RememberConversionMode (the remembered mode is stamped from it);
///  - the taskbar / the game thread's TIP instance follow the cross-process
///    global compartment, which we only write on enable/disable — hence the
///    reported "the 中/英 indicator only catches up after the next keystroke".
/// Writing both back costs nothing (the IME is already in that mode) and makes
/// all three channels agree immediately. The last-written value is remembered so
/// the sinks' echo of our own write does not bounce back as another write.
void TextService::MirrorConversionMode(const ULONG conversionMode)
{
    if (State::GetInstance().ImeDisabled() || conversionMode == m_lastMirroredMode)
    {
        return;
    }
    m_lastMirroredMode = conversionMode;
    RecordPublishedConversionMode(conversionMode);
    if (m_contextConversionModeCompartment != nullptr)
    {
        (void)m_contextConversionModeCompartment->SetValue(conversionMode);
    }
    if (m_globalConversionModeCompartment != nullptr)
    {
        (void)m_globalConversionModeCompartment->SetValue(conversionMode);
    }
    logger::info("Mirrored conversion mode {:#x} to the context + global compartments", conversionMode);
    // Throttled: see PROFILE_REACTIVATE_GUARD_MS. The mirror itself still runs
    // on every value (the compartments must follow the IME); only the
    // activation — the thing that provokes the next publication — is rate
    // limited.
    const auto now = GetTickCount64();
    if (now >= m_nextProfileReactivateMs)
    {
        m_nextProfileReactivateMs = now + PROFILE_REACTIVATE_GUARD_MS;
        ReactivateProfileKeepingMode();
    }
}

void TextService::RecordPublishedConversionMode(const ULONG conversionMode)
{
    const auto mode = static_cast<std::uint32_t>(conversionMode);
    if (m_publishedModeCount < PUBLISHED_MODE_HISTORY)
    {
        m_publishedModes[m_publishedModeCount++] = mode;
        return;
    }
    // Full: drop the oldest and append. The history only answers "did we ever
    // write this value", so order inside it does not matter — recency does.
    for (std::size_t i = 1; i < PUBLISHED_MODE_HISTORY; ++i)
    {
        m_publishedModes[i - 1] = m_publishedModes[i];
    }
    m_publishedModes[PUBLISHED_MODE_HISTORY - 1] = mode;
}

/// Ask the system to re-evaluate the input state, so the taskbar's 中/英 chip
/// repaints with the mode we just published.
///
/// The chip only repaints on real input events or activation events, and the key
/// that toggles the mode makes it read the PRE-toggle value (in-game 2026-10-16:
/// the first Shift does not flip it, the second one does). A window message does
/// not trigger a repaint (a WM_INPUTLANGCHANGEREQUEST poke was verified to have
/// no effect) and injecting a key is off the table (it would reach the game's
/// DirectInput). What does update the chip instantly is switching input methods —
/// i.e. a TSF profile activation — so re-activate the profile that is already
/// active: a no-op switch that still broadcasts the activation event.
///
/// Deferred one hop through the IME-thread task queue: this runs from inside a
/// compartment notification (the sink fires within SetValue), and re-entering
/// TSF with an activation from there is asking for trouble.
void TextService::ReactivateProfileKeepingMode()
{
    if (const auto *controller = Ime::ImeController::GetInstance(); controller != nullptr)
    {
        controller->PostToImeThread([this] -> void { DoReactivateProfileKeepingMode(); });
    }
}

void TextService::DoReactivateProfileKeepingMode()
{
    const ULONG mode = m_lastMirroredMode;
    if (m_threadMgr == nullptr)
    {
        return;
    }
    CComQIPtr<ITfInputProcessorProfileMgr> profileMgr;
    if (profileMgr.CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER) != S_OK || profileMgr == nullptr)
    {
        logger::debug("ReactivateProfileKeepingMode: no input-processor profile manager");
        return;
    }
    TF_INPUTPROCESSORPROFILE profile{};
    if (FAILED(profileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile)))
    {
        return;
    }
    const HRESULT hr = profileMgr->ActivateProfile(
        profile.dwProfileType,
        profile.langid,
        profile.clsid,
        profile.guidProfile,
        profile.hkl,
        TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE
    );
    logger::info("Re-activated the active input profile to refresh the system language bar (hr={:#x})", static_cast<std::uint32_t>(hr));
    // An activation may put the IME back into its default mode: restore the one
    // the user was in (the value the mirror just published).
    SetConversionMode(mode);
    if (m_globalConversionModeCompartment != nullptr)
    {
        (void)m_globalConversionModeCompartment->SetValue(mode);
    }
}

/// The remembered mode is applied when a text-entry session starts (see
/// SetSessionImeEnabled). Some TIPs re-initialise their mode on the new document
/// focus and write their own default into the compartment right afterwards —
/// the reported shape: every console open came back in ひらがな. Put the
/// remembered mode back once. Bounded to a short window after the session start
/// and to a single correction, so a deliberate mode change by the user is never
/// fought for long. Only an observation that DIFFERS from the restore target
/// consumes the attempt (see the body): the echo of our own session-start write
/// arrives first and must not disarm the mechanism before the IME's own word.
/// Returns true when it corrected the mode, in which case the caller must not
/// mirror that same observation (see UpdateConversionMode).
auto TextService::MaybeReassertRememberedMode(const ULONG observed) -> bool
{
    if (!m_reassertArmed || State::GetInstance().ImeDisabled())
    {
        return false;
    }
    if (GetTickCount64() > m_reassertDeadlineMs)
    {
        m_reassertArmed = false;
        return false;
    }
    if (observed == m_restoreTargetMode)
    {
        // Nothing to defend — our own echo, or the user picking the remembered
        // mode again. Deliberately NOT a disarm: the first observation after
        // the session-start write is always the echo of that write (the sinks
        // fire synchronously inside SetValue), so disarming here consumed the
        // one attempt before the IME could ever write its own default — which
        // is how this stayed dead in-game (a full session logged no "right
        // after the session start" line at all). The deadline above ends the
        // window instead.
        return false;
    }
    m_reassertArmed = false; // one correction per session start
    logger::info(
        "The IME set its conversion mode to {:#x} right after the session start; re-applying the remembered {:#x}",
        observed,
        m_restoreTargetMode
    );
    SetConversionMode(m_restoreTargetMode);
    if (m_globalConversionModeCompartment != nullptr)
    {
        // Keep the cross-process channel (the game thread's TIP instance) in step.
        (void)m_globalConversionModeCompartment->SetValue(m_restoreTargetMode);
    }
    State::GetInstance().SetConversionMode(m_restoreTargetMode);
    return true;
}

/// See ITextService::StampRememberedConversionModeFromLive. The focused context's
/// compartment is the live value — but only when the IME actually published it.
/// A value equal to the one WE last published is our own session-start push (or
/// its mirror) read back, and for an IME that publishes nothing at all (WeChat)
/// the compartment never holds anything else: believing it would freeze the
/// memory at the value we guessed, which is the exact opposite of what this
/// stamp is for. In that case State — fed by behavioral inference (a composition
/// start means NATIVE, a raw letter means alphanumeric) — is the only source
/// that follows the user's typing.
void TextService::StampRememberedConversionModeFromLive() const
{
    ULONG      live          = 0;
    const auto readPublished = [this](const CComPtr<TsfCompartment> &compartment, ULONG &out) -> bool {
        return compartment != nullptr && SUCCEEDED(compartment->GetValue(out)) &&
               IsConversionModeValue(out) &&
               !Ime::ConversionMemory::IsSelfPublishedValue(static_cast<std::uint32_t>(out), m_publishedModes.data(), m_publishedModeCount);
    };
    if (readPublished(m_contextConversionModeCompartment, live))
    {
        // ok
    }
    else if (readPublished(m_conversionModeCompartment, live))
    {
        // ok
    }
    else
    {
        live = static_cast<ULONG>(State::GetInstance().GetConversionMode().RawValue());
    }
    State::GetInstance().RememberConversionMode(live);
}

auto TextService::OnFocus(bool focus) -> bool
{
    HRESULT hr = E_FAIL;
    if (focus)
    {
        hr = m_textStore->Focus();
        UpdateConversionMode();
    }
    else
    {
        hr = m_textStore->ClearFocus();
    }
    const auto succeeded = SUCCEEDED(hr);
    if (succeeded)
    {
        State::GetInstance().Set(State::TEXT_SERVICE_FOCUS, focus);
    }
    return succeeded;
}

void TextService::AbortIme()
{
    // Clear the composition/candidate content FIRST so the composition-end
    // callback (OnEndComposition -> SendUiString) receives an already-empty
    // editor: abort means "cancel without committing", not "commit what is left".
    {
        const auto lock = GetWriteLock();
        m_textEditor.Select(0, 0);
        m_textEditor.ClearText();
        m_candidateUi.Close();
    }
    if (m_textStore != nullptr)
    {
        m_textStore->TerminateComposition();
    }
    // Belt and braces: even if no active composition was terminated (e.g. TSF
    // already ended it), make sure the input state flags are cleared so the menu
    // code stops treating the game as "still inputting" (which would otherwise
    // swallow all keyboard input).
    State::GetInstance().ClearComposing();
    // Same publish as OnEndComposition's: TerminateComposition only reaches
    // OnEndComposition when a composition was active, and the candidateUi
    // cleared above would otherwise stay unreported — the render thread keeps
    // the stale list and the window survives the abort.
    MarkDirty(DirtyFlag::Composition | DirtyFlag::CandidateList);
}

auto TextService::ToogleKeyboard(bool open) -> void
{
    // Skipped when the compartment already holds the value: the session start
    // publishes open/close on every enable, and an unchanged write only makes
    // the sink run again (see SetCompartmentIfDifferent).
    if (const auto hr = SetCompartmentIfDifferent(m_keyboardOpenCloseCompartment, static_cast<ULONG>(open)); FAILED(hr))
    {
        logger::debug("Can't toggle keyboard: {:#X}", hr);
    }
}

auto TextService::SetSessionImeEnabled(const bool enabled) -> bool
{
    if (m_globalOpenCloseCompartment == nullptr || m_globalConversionModeCompartment == nullptr)
    {
        return false;
    }

    // The game thread's TIP instance reads its OWN thread compartments, which no
    // other thread can write; the cross-process global compartments are the only
    // channel that reaches it. They carry the IME's open/close + 中/英 state
    // without touching the session's language profile — which is what used to
    // leave the taskbar showing ENG after the game exited.
    ULONG conversionMode = static_cast<ULONG>(State::ConversionMode::Flags::ALPHANUMERIC);
    ULONG openClose      = enabled ? 1U : 0U;
    if (enabled)
    {
        // Put the mode back verbatim. Recomposing "NATIVE" from the state we just
        // flattened would leave a Japanese IME in かな入力 (NATIVE without its
        // ROMAN bit) and drop FULLSHAPE from a Chinese one.
        //
        // Source order: the stamped "mode text was last confirmed in" (updated at
        // every commit while the IME is enabled, and seeded at startup from the
        // persisted setting), then the per-cycle capture (kept for an IME that
        // publishes nothing and where no confirmation was stamped yet), then the
        // state-with-NATIVE guess for a genuinely unobserved first entry.
        auto &state = State::GetInstance();
        if (state.HasRememberedConversionMode())
        {
            conversionMode = static_cast<ULONG>(state.GetRememberedConversionMode());
        }
        else if (m_hasSavedSessionConversionMode)
        {
            conversionMode = m_savedSessionConversionMode;
        }
        else
        {
            conversionMode = static_cast<ULONG>(state.GetConversionMode().RawValue() | static_cast<ULONG>(State::ConversionMode::Flags::NATIVE));
        }
        m_hasSavedSessionConversionMode = false;
        // Open/close is the かな/英数 switch for a Japanese IME: restoring a
        // fixed "open" turned 英数/Direct Input back into かな on every session
        // start, which is the "Google 日本語入力 does not remember the previous
        // input mode" report (2026-10-18). The TIP writes the bit itself on a
        // Shift/Caps toggle, so the captured value is where the user left it.
        if (m_hasSavedSessionOpenClose)
        {
            openClose                  = m_savedSessionOpenClose;
            m_hasSavedSessionOpenClose = false;
        }
        // Session start: freeze the target and arm the one-shot re-assert for a
        // TIP that overwrites its own mode right after this write.
        m_restoreTargetMode  = conversionMode;
        m_reassertDeadlineMs = GetTickCount64() + MODE_REASSERT_WINDOW_MS;
        m_reassertArmed      = true;
    }
    else if (!m_hasSavedSessionConversionMode)
    {
        // Capture the live mode BEFORE flattening it, once per disable/enable
        // cycle: the disable path runs on every menu close (15 in one observed
        // session), and a second pass would save the 0 we just wrote and hand
        // back English on the next enable. GetValue fails on a compartment that
        // was never written, so fall back through thread level to State.
        ULONG live = 0;
        if (m_contextConversionModeCompartment != nullptr && SUCCEEDED(m_contextConversionModeCompartment->GetValue(live)) &&
            IsConversionModeValue(live))
        {
            m_savedSessionConversionMode = live;
        }
        else if (m_conversionModeCompartment != nullptr && SUCCEEDED(m_conversionModeCompartment->GetValue(live)) &&
                 IsConversionModeValue(live))
        {
            m_savedSessionConversionMode = live;
        }
        else
        {
            m_savedSessionConversionMode = static_cast<ULONG>(State::GetInstance().GetConversionMode().RawValue());
        }
        m_hasSavedSessionConversionMode = true;
    }
    if (!enabled)
    {
        // Capture the live open/close BEFORE writing 0 (that write is the
        // "IME off" signal). For a Japanese IME this bit is the かな/英数
        // switch; the TIP writes it itself on a Shift/Caps toggle, so the
        // captured value is where the user actually left the IME.
        if (!m_hasSavedSessionOpenClose)
        {
            ULONG liveOpenClose = 1;
            if (m_contextOpenCloseCompartment != nullptr &&
                SUCCEEDED(m_contextOpenCloseCompartment->GetValue(liveOpenClose)))
            {
                m_savedSessionOpenClose = liveOpenClose;
            }
            else if (m_keyboardOpenCloseCompartment != nullptr &&
                     SUCCEEDED(m_keyboardOpenCloseCompartment->GetValue(liveOpenClose)))
            {
                m_savedSessionOpenClose = liveOpenClose;
            }
            else
            {
                m_savedSessionOpenClose = 1;
            }
            m_hasSavedSessionOpenClose = true;
        }
        // The session is over, so nothing may re-assert the remembered mode
        // behind the user's back: a fast open/close without typing would
        // otherwise put the mode straight back. The disable path no longer
        // writes the conversion compartment at all (see below), so this is
        // purely about the re-assert window.
        m_reassertArmed = false;
    }

    // Must be set BEFORE the writes below: the sinks fire synchronously from
    // inside SetValue, so a guard assigned after them still holds the previous
    // value when the echo of this very write arrives and lets it through
    // (measured in-game 2026-10-16: `Mirrored conversion mode 0x1` logged in the
    // same millisecond, immediately before this function's own "Published the
    // IME on state" line). That echo re-wrote all three channels and posted a
    // profile re-activation at EVERY session start — the exact hammer the
    // disable path avoids, and one that could land after a disable.
    //
    // Disable leaves the mirror target alone: it publishes no conversion mode,
    // and `conversionMode` still holds the ALPHANUMERIC placeholder there.
    if (enabled)
    {
        m_lastMirroredMode = conversionMode;
        RecordPublishedConversionMode(conversionMode);
    }

    // Publishing an unchanged value is not free: the sinks fire synchronously
    // from inside SetValue. A session start can run several times per session
    // (five enables in one logged session — window activations and menu
    // toggles), and each pass used to re-publish the same mode. hr=0x1 below
    // means "already there, skipped".
    const HRESULT openCloseHr = SetCompartmentIfDifferent(m_globalOpenCloseCompartment, openClose);

    // The disable path deliberately does NOT touch the conversion mode: writing
    // 0x0 (ALPHANUMERIC) to flatten it made some Chinese IMEs republish NATIVE
    // (0x1), so the system language bar flipped 「英 半角」→「中 半角」 exactly as
    // the console closed, and the mode remembered for the next session no
    // longer matched the IME. The next enable re-publishes the remembered mode
    // verbatim, so nothing has to be flattened here (in-game report, 2026-10-17).
    HRESULT conversionModeHr = S_FALSE;
    if (enabled)
    {
        conversionModeHr = SetCompartmentIfDifferent(m_globalConversionModeCompartment, conversionMode);
    }

    // Mirror into this thread's own compartments: the language bar's 中/英 state
    // reads them (their sinks refresh State), so the overlay must not go stale
    // relative to what was just published. The open/close value is the captured
    // one on a re-enable — a Japanese IME treats it as the かな/英数 switch, so
    // writing a fixed 1 turned 英数 back into かな.
    ToogleKeyboard(openClose != 0);
    if (enabled)
    {
        SetConversionMode(conversionMode);
    }

    if (enabled)
    {
        logger::info(
            "Published the IME on state through the global compartments (open/close hr={:#x}, conversion hr={:#x}, conversion={:#x})",
            static_cast<uint32_t>(openCloseHr),
            static_cast<uint32_t>(conversionModeHr),
            conversionMode
        );
    }
    else
    {
        logger::info(
            "Published the IME off state through the global compartments (open/close hr={:#x}); the conversion mode is left to the IME",
            static_cast<uint32_t>(openCloseHr)
        );
    }
    return SUCCEEDED(openCloseHr) && SUCCEEDED(conversionModeHr);
}

auto TextService::SetConversionMode(DWORD conversionMode) -> bool
{
    // Write both levels: the focused context's compartment is what the active
    // TIP observes (external mode switches are supposed to go through it), the
    // thread-level one stays for IMEs that only maintain the thread default.
    bool success = SUCCEEDED(m_conversionModeCompartment->SetValue(conversionMode));
    if (m_contextConversionModeCompartment != nullptr)
    {
        success = SUCCEEDED(m_contextConversionModeCompartment->SetValue(conversionMode)) && success;
    }
    return success;
}

void TextService::RefreshConversionMode()
{
    // A TIP activation is also the only thing that changes the input language,
    // and the composition sinks gate their mode inference on it (see
    // CompositionScriptLogic.h), so re-read it here as well.
    RefreshInputLanguage();
    UpdateConversionMode();
    // One diagnostic line per TIP activation: shows which of the six watched
    // compartments actually carry a value for the active IME.
    DumpConversionCompartments();
}

auto TextService::GetInputLanguageId() const -> LANGID
{
    if (m_langId != 0)
    {
        return m_langId;
    }
    // Nothing resolved yet (no TIP activation since startup): the thread's
    // keyboard layout carries the same language id for the usual layouts.
    return LOWORD(GetKeyboardLayout(GetCurrentThreadId()));
}

void TextService::RefreshInputLanguage()
{
    LANGID           langId = 0;
    std::string_view source = "keyboard layout";
    if (m_inputProcessorProfiles != nullptr)
    {
        // The active profile is the authoritative answer: it is the IME the user
        // actually selected, and its langid is the language the composition
        // sinks need (in-game 2026-10-17: GetCurrentLanguage reported 0x0804 —
        // the system default — while the active TIP was Microsoft Pinyin; the
        // two only agree by coincidence).
        CComQIPtr<ITfInputProcessorProfileMgr> profileMgr(m_inputProcessorProfiles);
        TF_INPUTPROCESSORPROFILE               profile{};
        if (profileMgr != nullptr && SUCCEEDED(profileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile)) && profile.langid != 0)
        {
            langId = profile.langid;
            source = "active profile";
        }
        else if (SUCCEEDED(m_inputProcessorProfiles->GetCurrentLanguage(&langId)) && langId != 0)
        {
            source = "TSF current language";
        }
        else
        {
            langId = LOWORD(GetKeyboardLayout(GetCurrentThreadId()));
        }
    }
    else
    {
        langId = LOWORD(GetKeyboardLayout(GetCurrentThreadId()));
    }
    if (langId != m_langId)
    {
        // The composition sinks gate their script inference on this language
        // (CompositionScriptLogic.h). A value that resolves to the US layout
        // while a Japanese IME is active would silently disable the whole
        // correction path, so make the decision visible in the log.
        logger::info(
            "Input language resolved: langId={:#06x} (primary={:#04x}, Japanese inference {}) via {}",
            static_cast<std::uint32_t>(langId),
            static_cast<std::uint32_t>(PRIMARYLANGID(langId)),
            PRIMARYLANGID(langId) == Ime::CompositionScript::LANG_JAPANESE_PRIMARY ? "enabled" : "disabled",
            source
        );
    }
    m_langId = langId;
}

void TextService::DumpConversionCompartments() const
{
    const auto describe = [](const CComPtr<TsfCompartment> &compartment) -> std::string {
        ULONG value = 0;
        if (compartment != nullptr && SUCCEEDED(compartment->GetValue(value)))
        {
            return std::format("{:#x}", value);
        }
        return "n/a"; // never written by anyone
    };
    logger::info(
        "Conversion compartments: thread={} context={} global={} | open/close: thread={} context={} global={}",
        describe(m_conversionModeCompartment),
        describe(m_contextConversionModeCompartment),
        describe(m_globalConversionModeCompartment),
        describe(m_keyboardOpenCloseCompartment),
        describe(m_contextOpenCloseCompartment),
        describe(m_globalOpenCloseCompartment)
    );
}

auto TextService::ProcessImeMessage(HWND /*hWnd*/, UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) -> bool
{
    // This fallback is practically unnecessary.
    // If the current environment supports TSF APIs but fails to provide a Candidate UI (UIElement),
    // it is likely a severe system-level anomaly rather than a supported configuration.
    //
    // Logic: Our architecture already separates concerns by initializing a legacy Imm32
    // TextService if TSF is disabled or fails to initialize. Therefore, within the
    // TSF-enabled service, we assume UIElement support is guaranteed.
    return false;
}

auto TextService::MoveCandidateSelection(const int delta) -> bool
{
    if (delta == 0)
    {
        return false;
    }
    const std::scoped_lock lock(m_mutex);
    const auto             count = m_candidateUi.CandidateList().size();
    if (count == 0)
    {
        return false;
    }
    const auto current = static_cast<long>(std::min<DWORD>(m_candidateUi.Selection(), static_cast<DWORD>(count - 1)));
    const auto clamped = std::clamp(current + delta, 0L, static_cast<long>(count - 1));
    if (clamped == current)
    {
        return false;
    }
    m_candidateUi.SetSelection(static_cast<DWORD>(clamped));
    MarkDirty(DirtyFlag::CandidateSelection);
    return true;
}

auto TextService::CommitSelectedCandidate() -> bool
{
    DWORD index = 0;
    {
        const std::scoped_lock lock(m_mutex);
        const auto             count = m_candidateUi.CandidateList().size();
        if (count == 0)
        {
            return false;
        }
        index = std::min<DWORD>(m_candidateUi.Selection(), static_cast<DWORD>(count - 1));
    }
    return m_textStore->CommitCandidate(index);
}

auto TextService::ForwardKeyToTip(const UINT vk, const LPARAM lParam) -> bool
{
    auto keystrokeMgr = TsfSupport::GetSingleton().GetKeystrokeMgr();
    if (keystrokeMgr == nullptr)
    {
        return false;
    }
    BOOL eaten = FALSE;
    if (SUCCEEDED(keystrokeMgr->TestKeyDown(vk, lParam, &eaten)) && eaten != FALSE)
    {
        return SUCCEEDED(keystrokeMgr->KeyDown(vk, lParam, &eaten)) && eaten != FALSE;
    }
    return false;
}
} // namespace Tsf
