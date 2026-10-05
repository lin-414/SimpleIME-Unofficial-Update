#ifndef TSF_TEXTSTORE_H
#define TSF_TEXTSTORE_H

#pragma once

#include "TsfCompartment.h"
#include "atlcomcli_shim.h"
#include "core/State.h"
#include "ime/ITextService.h"
#include "ime/TextEditor.h"

#include <array>
#include <msctf.h>
#include <shared_mutex>

namespace Tsf
{

/**
 * @brief RAII function call tracer for TSF debugging.
 *
 * Produces indented trace-level log output showing TSF call sequences and nesting.
 * In release builds (when SIMPLE_IME_TRACE is not defined), this entire class and
 * all its call sites are eliminated by the compiler as dead code — zero overhead.
 *
 * **Enable at compile time** by adding `-DSIMPLE_IME_TRACE` to your build flags,
 * or passing `-DSIMPLE_IME_TRACE=1` in CMake.
 */
#ifdef SIMPLE_IME_TRACE
struct FuncTracer
{
    template <typename... Args>
    explicit FuncTracer(std::format_string<Args...> fmt, Args &&...args)
    {
        if (spdlog::should_log(spdlog::level::trace))
        {
            spdlog::trace("{:>{}}{}", "", g_indent, std::format(fmt, std::forward<Args>(args)...));
            g_indent += 4;
        }
    }

    FuncTracer(const FuncTracer &other)                = delete;
    FuncTracer(FuncTracer &&other) noexcept            = delete;
    FuncTracer &operator=(const FuncTracer &other)     = delete;
    FuncTracer &operator=(FuncTracer &&other) noexcept = delete;

    template <typename... Args>
    void log(std::format_string<Args...> fmt, Args &&...args)
    {
        if (spdlog::should_log(spdlog::level::trace))
        {
            spdlog::trace("{:>{}}{}", "", g_indent, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    ~FuncTracer()
    {
        g_indent -= 4;
        log("---");
    }

private:
    static inline uint32_t g_indent = 0;
};
#else
// Release / non-trace build: FuncTracer is a completely empty type.
// The compiler will eliminate every `auto tracer = FuncTracer(...)` call site,
// including argument evaluation for trivial arguments like __func__.
struct FuncTracer
{
    template <typename... Args>
    explicit constexpr FuncTracer(std::format_string<Args...> /*fmt*/, Args &&.../*args*/) noexcept
    {
    }

    FuncTracer(const FuncTracer &other)                = delete;
    FuncTracer(FuncTracer &&other) noexcept            = delete;
    FuncTracer &operator=(const FuncTracer &other)     = delete;
    FuncTracer &operator=(FuncTracer &&other) noexcept = delete;

    template <typename... Args>
    constexpr void log(std::format_string<Args...> /*fmt*/, Args &&.../*args*/) const noexcept
    {
    }
};
#endif

struct AdviseSinkCache
{
    CComPtr<IUnknown>          punkId            = nullptr;
    CComPtr<ITextStoreACPSink> pTextStoreAcpSink = nullptr;
    DWORD                      dwMask            = 0;

    void Clear()
    {
        punkId.Release();
        pTextStoreAcpSink.Release();
        dwMask = 0;
    }
};

struct CandidateInfo
{
    DWORD pageStart = 0;
    UINT  pageEnd   = 0;
};

class TextService;

class TextStore : ITextStoreACP, ITfContextOwnerCompositionSink, ITfUIElementSink, ITfTextEditSink
{
    static constexpr uint32_t EDIT_VIEW_COOKIE = 0;
    using State                                = Ime::Core::State;

public:
    explicit TextStore(TextService *pTextService) : m_pTextService(pTextService) {}

    virtual ~TextStore() { UnInitialize(); }

    TextStore(const TextStore &other)                         = delete;
    TextStore(TextStore &&other) noexcept                     = delete;
    auto operator=(const TextStore &other) -> TextStore &     = delete;
    auto operator=(TextStore &&other) noexcept -> TextStore & = delete;

    [[nodiscard]] auto Initialize(ITfThreadMgr *threadMgr, const TfClientId &tfClientId) -> HRESULT;
    auto               SetHWND(HWND hWnd) -> bool;
    void               UnInitialize();

    void SetOnEndCompositionCallback(Ime::OnEndCompositionCallback *const callback) { m_OnEndCompositionCallback = callback; }

    /// The document context created in Initialize. Valid until UnInitialize;
    /// TextService attaches its context-level compartment watchers to it.
    [[nodiscard]] auto GetContext() const -> ITfContext * { return m_context; }

    [[nodiscard]] auto Focus() -> HRESULT;
    [[nodiscard]] auto ClearFocus() const -> HRESULT;
    [[nodiscard]] auto TerminateComposition() const -> HRESULT;
    [[nodiscard]] auto CommitCandidate(UINT index) const -> bool;

    auto STDMETHODCALLTYPE AddRef() -> ULONG override;
    auto STDMETHODCALLTYPE Release() -> ULONG override;

private:
    // ITextStoreACP functions
    // NOLINTBEGIN(*-use-trailing-return-type)
    STDMETHODIMP QueryInterface(REFIID riid, void **ppvObject) override;
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown *pUnknown, DWORD dwMask) override;
    STDMETHODIMP UnadviseSink(IUnknown *pUnknown) override;
    STDMETHODIMP RequestLock(DWORD dwLockFlags, HRESULT *phrSession) override;
    STDMETHODIMP GetStatus(TS_STATUS *pStatus) override;
    /**
     * determines whether the specified start and end character positions are valid. Use this method to adjust
     * an edit to a document before executing the edit. The method must not return values outside the range of
     * the document.
     * @param acpStart Starting application character position for inserted text.
     * @param acpEnd Ending application character position for the inserted text. This value is equal to
     * acpStart if the text is inserted at a point instead of replacing selected text.
     * @param textSize Length of replacement text.
     * @param pacpResultStart Returns the new starting application character position of the inserted text. If
     * this parameter is NULL, then text cannot be inserted at the specified position. This value cannot be
     * outside the document range.
     * @param pacpResultEnd Returns the new ending application character position of the inserted text. If this
     * parameter is NULL, then pacpResultStart is set to NULL and text cannot be inserted at the specified
     * position. This value cannot be outside the document range.
     * @return
     */
    STDMETHODIMP QueryInsert(LONG acpStart, LONG acpEnd, ULONG textSize, LONG *pacpResultStart, LONG *pacpResultEnd) override;
    /**
     * returns the character position of a text selection in a document. This method supports multiple text
     * selections. The caller must have a read-only lock on the document before calling this method.
     * @param startIndex Specifies the text selections that start the process. If the TF_DEFAULT_SELECTION constant
     * is specified for this parameter, the input selection starts the process.
     * @param selectionSize Specifies the maximum number of selections to return.
     * @param pSelections Receives the style, start, and end character positions of the selected text. These
     * values are put into the TS_SELECTION_ACP structure.
     * @param pcFetched Receives the number of pSelections structures returned.
     * @return
     */
    STDMETHODIMP GetSelection(ULONG startIndex, ULONG selectionSize, TS_SELECTION_ACP *pSelections, ULONG *pcFetched) override;
    /**
     * The ITextStoreACP::SetSelection method selects text within the document.
     * The application must have a read/write lock on the document before calling this method.
     * @param selectionCount Specifies the number of text selections in pSelections.
     * @param pSelections Specifies the style, start, and end character positions of the text selected through the TS_SELECTION_ACP structure.
     * @return
     */
    STDMETHODIMP SetSelection(ULONG selectionCount, const TS_SELECTION_ACP *pSelections) override;
    STDMETHODIMP GetText(
        LONG acpStart, LONG acpEnd, WCHAR *textBuffer, ULONG textBufferSize, ULONG *textBufferCopied, TS_RUNINFO *runInfoBuffer, ULONG cRunInfoReq,
        ULONG *runInfoBufferCopied, LONG *pacpNext
    ) override;
    STDMETHODIMP SetText(DWORD dwFlags, LONG acpStart, LONG acpEnd, const WCHAR *pchText, ULONG cch, TS_TEXTCHANGE *pChange) override;
    STDMETHODIMP GetFormattedText(LONG acpStart, LONG acpEnd, IDataObject **ppDataObject) override;
    STDMETHODIMP GetEmbedded(LONG acpPos, REFGUID rguidService, REFIID riid, IUnknown **ppObject) override;
    STDMETHODIMP QueryInsertEmbedded(const GUID *pguidService, const FORMATETC *pFormatEtc, BOOL *pfInsertable) override;
    STDMETHODIMP InsertEmbedded(DWORD dwFlags, LONG acpStart, LONG acpEnd, IDataObject *pDataObject, TS_TEXTCHANGE *pChange) override;
    STDMETHODIMP InsertTextAtSelection(
        DWORD dwFlags, const WCHAR *textBuffer, ULONG textBufferSize, LONG *pacpStart, LONG *pacpEnd, TS_TEXTCHANGE *pChange
    ) override;
    STDMETHODIMP InsertEmbeddedAtSelection(DWORD dwFlags, IDataObject *pDataObject, LONG *pacpStart, LONG *pacpEnd, TS_TEXTCHANGE *pChange) override;
    STDMETHODIMP RequestSupportedAttrs(DWORD dwFlags, ULONG filterAttrCount, const TS_ATTRID *filterAttrs) override;
    STDMETHODIMP RequestAttrsAtPosition(LONG acpPos, ULONG filterAttrCount, const TS_ATTRID *filterAttrs, DWORD dwFlags) override;
    STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG acpPos, ULONG filterAttrCount, const TS_ATTRID *filterAttrs, DWORD dwFlags) override;
    STDMETHODIMP FindNextAttrTransition(
        LONG acpStart, LONG acpStop, ULONG filterAttrCount, const TS_ATTRID *filterAttrs, DWORD dwFlags, LONG *pacpNext, BOOL *pfFoundTransition,
        LONG *foundOffset
    ) override;
    STDMETHODIMP RetrieveRequestedAttrs(ULONG attrCount, TS_ATTRVAL *attrVals, ULONG *pcFetched) override;
    STDMETHODIMP GetEndACP(LONG *pacpEnd) override;
    STDMETHODIMP GetActiveView(TsViewCookie *pViewCookie) override;
    STDMETHODIMP GetACPFromPoint(TsViewCookie vcView, const POINT *ptScreenPos, DWORD dwFlags, LONG *pacpPos) override;
    STDMETHODIMP GetTextExt(TsViewCookie vcView, LONG acpStart, LONG acpEnd, RECT *pRect, BOOL *pfIsClipped) override;
    STDMETHODIMP GetScreenExt(TsViewCookie vcView, RECT *pRect) override;
    STDMETHODIMP GetWnd(TsViewCookie vcView, HWND *pHwnd) override;

    // ITfContextOwnerCompositionSink functions
    STDMETHODIMP OnStartComposition(ITfCompositionView *pComposition, BOOL *pfOk) override;
    STDMETHODIMP OnUpdateComposition(ITfCompositionView *pComposition, ITfRange *pRangeNew) override;
    STDMETHODIMP OnEndComposition(ITfCompositionView *pComposition) override;

    // ITfUIElementSink functions
    STDMETHODIMP BeginUIElement(DWORD dwUIElementId, BOOL *pbShow) override;
    STDMETHODIMP UpdateUIElement(DWORD dwUIElementId) override;
    STDMETHODIMP EndUIElement(DWORD dwUIElementId) override;

    auto InitSinks() -> HRESULT;

    // NOLINTEND(*-use-trailing-return-type)
    auto        DoUpdateUIElement() -> HRESULT;
    auto        GetCandidateInterface(DWORD dwUIElementId, ITfCandidateListUIElementBehavior **pInterface) const -> HRESULT;
    static auto GetCandInfo(ITfCandidateListUIElementBehavior *pCandidateList, CandidateInfo &candidateInfo) -> HRESULT;
    auto        OnEndEdit(ITfContext *pic, TfEditCookie ecReadOnly, ITfEditRecord *pEditRecord) -> HRESULT override;

    void               LockDocument(DWORD dwLockFlags);
    void               UnlockDocument();
    [[nodiscard]] auto IsLocked(DWORD dwLockType) const -> bool;

    std::deque<DWORD>                          m_lockQueue;
    AdviseSinkCache                            m_adviseSinkCache{};
    HWND                                       m_hWnd{nullptr};
    TextService                               *m_pTextService{nullptr};
    Ime::OnEndCompositionCallback             *m_OnEndCompositionCallback = nullptr;
    CComPtr<ITextStoreACPServices>             m_textStoreAcpServices     = nullptr;
    CComPtr<ITfThreadMgr>                      m_threadMgr                = nullptr;
    CComPtr<ITfDocumentMgr>                    m_documentMgr              = nullptr;
    CComPtr<ITfDocumentMgr>                    m_pPrevDocMgr              = nullptr;
    CComPtr<ITfUIElementMgr>                   m_uiElementMgr             = nullptr;
    CComPtr<ITfContext>                        m_context                  = nullptr;
    CComPtr<ITfCandidateListUIElementBehavior> m_currentCandidateUi       = nullptr;
    DWORD                                      m_currentUiElementId{TF_INVALID_UIELEMENTID};
    DWORD                                      m_refCount{0};
    DWORD                                      m_dwLockType{0};
    TfEditCookie                               m_editCookie{0};
    DWORD                                      m_uiElementCookie{0};
    DWORD                                      m_textEditCookie{0};
    // Pending change flags. Be Updated in an edit session and consume in OnEndEdit or at the end of RequestLock.
    Ime::ITextService::DirtyFlag               m_pendingChangeFlags{Ime::ITextService::DirtyFlag::None};
    bool                                       m_fLocked{false};
    bool                                       m_fLayoutChanged{false};
};

class TextService : public Ime::ITextService
{
    using State = Ime::Core::State;

public:
    auto Initialize(ITfThreadMgr *threadMgr, TfClientId clientId) -> HRESULT;

    void UnInitialize() override
    {
        if (m_conversionModeCompartment != nullptr)
        {
            m_conversionModeCompartment->UnInitialize();
        }
        if (m_contextConversionModeCompartment != nullptr)
        {
            // Must go before m_textStore->UnInitialize(): the watcher is
            // attached to the TextStore's context.
            m_contextConversionModeCompartment->UnInitialize();
        }
        if (m_contextOpenCloseCompartment != nullptr)
        {
            m_contextOpenCloseCompartment->UnInitialize();
        }
        if (m_keyboardOpenCloseCompartment != nullptr)
        {
            m_keyboardOpenCloseCompartment->UnInitialize();
        }
        if (m_globalConversionModeCompartment != nullptr)
        {
            m_globalConversionModeCompartment->UnInitialize();
        }
        if (m_globalOpenCloseCompartment != nullptr)
        {
            m_globalOpenCloseCompartment->UnInitialize();
        }
        if (m_textStore != nullptr)
        {
            m_textStore->UnInitialize();
        }
        m_threadMgr.Release();
    }

    void RegisterCallback(Ime::OnEndCompositionCallback *callback) override { m_textStore->SetOnEndCompositionCallback(callback); }

    void OnStart(HWND hWnd) override { m_textStore->SetHWND(hWnd); }

    auto OnFocus(bool focus) -> bool override;

    void AbortIme() override;

    auto ToogleKeyboard(bool open) -> void override;

    auto CommitCandidate(DWORD index) -> bool override { return m_textStore->CommitCandidate(index); }

    auto SetConversionMode(DWORD conversionMode) -> bool override;

    /// Re-read the conversion mode from the compartments into State. Called when
    /// a TIP activates (InputMethodManager delegates via ImeWnd) so profile
    /// switches re-seed the language bar from the focused context, not from the
    /// stale thread default.
    auto RefreshConversionMode() -> void override;

    auto SnapshotCompositionAndCandidates(Ime::CompositionInfo &outComposition, Ime::CandidateUi &outCandidates) -> void override
    {
        const std::scoped_lock lock(m_mutex);
        outComposition.documentText = m_textEditor.GetText();
        outComposition.caretPos     = m_textEditor.GetStart();
        outCandidates               = m_candidateUi;
    }

    [[nodiscard]] auto GetTextEditorWrite() -> Ime::TextEditor & { return m_textEditor; }

    [[nodiscard]] auto GetCandidateUiWrite() -> Ime::CandidateUi & { return m_candidateUi; }

    [[nodiscard]] auto GetReadLock() -> std::shared_lock<std::shared_mutex> { return std::shared_lock(m_mutex); }

    [[nodiscard]] auto GetWriteLock() -> std::scoped_lock<std::shared_mutex> { return std::scoped_lock(m_mutex); }

    /// Exclusive lock for TSF sinks (composition / UIElement) that can fire BOTH
    /// inside and outside the TSF document lock:
    ///  - Inside OnLockGranted, RequestLock already holds this mutex for the
    ///    whole grant scope (it is not recursive — locking again would
    ///    deadlock), so pass documentLockHeld=true and rely on it.
    ///  - Outside, the sink must lock itself: the render thread's
    ///    RequestUpdate copies the same state under this mutex.
    [[nodiscard]] auto GetSinkWriteLock(bool documentLockHeld) const -> std::unique_lock<std::shared_mutex>
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex, std::defer_lock);
        if (!documentLockHeld)
        {
            lock.lock();
        }
        return lock;
    }

    auto ProcessImeMessage(HWND /*hWnd*/, UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) -> bool override;

protected:
    void RequestUpdate(Ime::CompositionInfo &compositionInfo, Ime::CandidateUi &uiForRead, DirtyFlag flag) override
    {
        const std::scoped_lock lock(m_mutex);
        if (HasDirtyFlag(flag, DirtyFlag::Composition))
        {
            compositionInfo.documentText = m_textEditor.GetText();
            compositionInfo.caretPos     = m_textEditor.GetStart();
        }

        if (HasDirtyFlag(flag, DirtyFlag::CandidateList))
        {
            uiForRead = m_candidateUi;
        }
        else if (HasDirtyFlag(flag, DirtyFlag::CandidateSelection))
        {
            uiForRead.SetSelection(m_candidateUi.Selection());
        }
    }

private:
    void        UpdateConversionMode() const;
    static void UpdateConversionMode(ULONG conversionMode);
    void        DumpConversionCompartments() const;

    Ime::TextEditor           m_textEditor;
    Ime::CandidateUi          m_candidateUi;
    CComPtr<ITfThreadMgr>     m_threadMgr{nullptr};
    CComPtr<TsfCompartment>   m_conversionModeCompartment        = nullptr;
    /// Focused-context-level INPUTMODE_CONVERSION watcher. IMEs like WeChat keep
    /// the live 中/英 conversion mode ONLY here (the OS mode indicator reads the
    /// focused context); the thread-level compartment above never changes on
    /// their Shift toggles, which froze the language bar on "英".
    CComPtr<TsfCompartment>   m_contextConversionModeCompartment = nullptr;
    /// Focused-context-level OPEN/CLOSE watcher: per the State.h convention the
    /// Shift 中/英 toggle of IMEs that express mode through open/close (rather
    /// than conversion bits) lands here.
    CComPtr<TsfCompartment>   m_contextOpenCloseCompartment      = nullptr;
    CComPtr<TsfCompartment>   m_keyboardOpenCloseCompartment     = nullptr;
    /// Cross-process global compartment watchers. Diagnostics only for now:
    /// they LOG changes (the value every app on the machine sees, i.e. what the
    /// taskbar tracks) but do not write State, so another process's IME
    /// activity cannot flip the in-game mode display.
    CComPtr<TsfCompartment>   m_globalConversionModeCompartment  = nullptr;
    CComPtr<TsfCompartment>   m_globalOpenCloseCompartment       = nullptr;
    CComPtr<TextStore>        m_textStore                        = nullptr;
    mutable std::shared_mutex m_mutex;
    TfClientId                m_clientId;
};
} // namespace Tsf

#endif
