#ifndef TSF_TSFCOMPARTMENT_H
    #define TSF_TSFCOMPARTMENT_H

    #pragma once

    #include "atlcomcli_shim.h"

    #include <msctf.h>

namespace Tsf
{
using CompartmentChangeCallback = std::function<HRESULT(const GUID *, ULONG)>;

class TsfCompartment : public ITfCompartmentEventSink
{
public:
    TsfCompartment() = default;

    virtual ~TsfCompartment() { UnInitialize(); }

    TsfCompartment(const TsfCompartment &other)                         = delete;
    TsfCompartment(TsfCompartment &&other) noexcept                     = delete;
    auto operator=(const TsfCompartment &other) -> TsfCompartment &     = delete;
    auto operator=(TsfCompartment &&other) noexcept -> TsfCompartment & = delete;

    /// Thread-manager-level compartment (the per-thread default values).
    auto Initialize(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, const GUID &guidCompartment, const CompartmentChangeCallback &callback = nullptr)
        -> HRESULT;
    /// Context-level compartment: the focused context's own values. IMEs keep
    /// the live conversion mode here (this is what the OS mode indicator reads),
    /// while the thread-manager-level compartment only holds per-thread defaults.
    auto Initialize(ITfContext *pContext, TfClientId tfClientId, const GUID &guidCompartment, const CompartmentChangeCallback &callback = nullptr)
        -> HRESULT;
    /// Direct compartment-manager form (e.g. the cross-process global
    /// compartment from ITfThreadMgr::GetGlobalCompartment).
    auto Initialize(ITfCompartmentMgr *pCompartmentMgr, TfClientId tfClientId, const GUID &guidCompartment, const CompartmentChangeCallback &callback = nullptr)
        -> HRESULT;
    auto UnInitialize() -> HRESULT;
    auto SetValue(ULONG value) const -> HRESULT;
    auto GetValue(__out ULONG &pValue) const -> HRESULT;

    auto AddRef() -> ULONG override;
    auto Release() -> ULONG override;

private:
    auto QueryInterface(const IID &riid, void **ppvObject) -> HRESULT override;
    auto OnChange(const GUID &rguid) -> HRESULT override;

    CComPtr<ITfCompartment>   m_tfCompartment;
    DWORD                     m_dwCookie = TF_INVALID_COOKIE;
    DWORD                     m_refCount = 0;
    GUID                      m_guidCompartment{};
    TfClientId                m_tfClientId;
    CompartmentChangeCallback m_callback = nullptr;
};
} // namespace Tsf

#endif
// namespace LIBC_NAMESPACE_DECL
