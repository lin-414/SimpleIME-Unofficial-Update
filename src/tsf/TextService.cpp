//
// Created by jamie on 2025/2/22.
//
#include "core/State.h"
#include "log.h"
#include "tsf/TextStore.h"
#include "tsf/TsfSupport.h"

#include <format>
#include <string>

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
    m_textStore                        = new TextStore(this);
    m_conversionModeCompartment        = new TsfCompartment();
    m_contextConversionModeCompartment = new TsfCompartment();
    m_contextOpenCloseCompartment      = new TsfCompartment();
    m_keyboardOpenCloseCompartment     = new TsfCompartment();
    m_globalConversionModeCompartment  = new TsfCompartment();
    m_globalOpenCloseCompartment       = new TsfCompartment();

    hr = m_conversionModeCompartment->Initialize(
        m_threadMgr, clientId, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, [](const GUID * /*guid*/, const ULONG ulong) {
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
                [](const GUID * /*guid*/, const ULONG ulong) {
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
                [](const GUID * /*guid*/, const ULONG ulong) {
                    logger::info("Context keyboard open/close changed: {}", ulong);
                    auto &state = State::GetInstance();
                    state.Set(State::KEYBOARD_OPEN, ulong != 0);
                    if (ulong != 0)
                    {
                        state.AddConversionModeFlag(State::ConversionMode::Flags::NATIVE);
                    }
                    else
                    {
                        state.ClearConversionModeFlag(State::ConversionMode::Flags::NATIVE);
                    }
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

void TextService::UpdateConversionMode() const
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
    State::GetInstance().SetConversionMode(conversionMode);
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
    State::GetInstance().Clear(State::IN_COMPOSING);
    State::GetInstance().Clear(State::IN_CAND_CHOOSING);
}

auto TextService::ToogleKeyboard(bool open) -> void
{
    if (const auto hr = m_keyboardOpenCloseCompartment->SetValue(static_cast<ULONG>(open)); FAILED(hr))
    {
        logger::debug("Can't toggle keyboard: {:#X}", hr);
    }
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
    UpdateConversionMode();
    // One diagnostic line per TIP activation: shows which of the six watched
    // compartments actually carry a value for the active IME.
    DumpConversionCompartments();
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
} // namespace Tsf
