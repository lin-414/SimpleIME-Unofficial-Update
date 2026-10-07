#include "tsf/TsfCompartment.h"

#include "atlcomcli_shim.h"
#include "log.h"

auto Tsf::TsfCompartment::Initialize(
    ITfThreadMgr *pThreadMgr, TfClientId tfClientId, REFGUID guidCompartment, const CompartmentChangeCallback &callback
) -> HRESULT
{
    if (const CComQIPtr<ITfCompartmentMgr> tfCompartmentMgr(pThreadMgr); tfCompartmentMgr != nullptr)
    {
        return Initialize(tfCompartmentMgr, tfClientId, guidCompartment, callback);
    }
    return E_FAIL;
}

auto Tsf::TsfCompartment::Initialize(ITfContext *pContext, TfClientId tfClientId, REFGUID guidCompartment, const CompartmentChangeCallback &callback)
    -> HRESULT
{
    if (const CComQIPtr<ITfCompartmentMgr> tfCompartmentMgr(pContext); tfCompartmentMgr != nullptr)
    {
        return Initialize(tfCompartmentMgr, tfClientId, guidCompartment, callback);
    }
    return E_FAIL;
}

auto Tsf::TsfCompartment::Initialize(
    ITfCompartmentMgr *pCompartmentMgr, TfClientId tfClientId, REFGUID guidCompartment, const CompartmentChangeCallback &callback
) -> HRESULT
{
    if (IsEqualGUID(m_guidCompartment, GUID_NULL) == 0)
    {
        logger::warn("TsfCompartment already initialized.");
        return S_FALSE;
    }
    HRESULT hresult = E_FAIL;
    m_tfClientId      = tfClientId;
    m_guidCompartment = guidCompartment;
    m_callback        = callback;
    hresult           = pCompartmentMgr->GetCompartment(guidCompartment, &m_tfCompartment);
    if (SUCCEEDED(hresult))
    {
        if (const CComQIPtr<ITfSource> tfSource(m_tfCompartment); tfSource != nullptr)
        {
            hresult = tfSource->AdviseSink(IID_ITfCompartmentEventSink, this, &m_dwCookie);
        }
        else
        {
            // The compartment exposes no ITfSource, so it can never be advised.
            hresult = E_FAIL;
        }
    }
    if (FAILED(hresult))
    {
        // Roll back the half-initialization: the guid gate at the top would
        // otherwise report S_FALSE "already initialized" on every retry.
        m_dwCookie        = TF_INVALID_COOKIE;
        m_guidCompartment = GUID_NULL;
        m_callback        = nullptr;
        m_tfCompartment.Release();
    }
    return hresult;
}

auto Tsf::TsfCompartment::UnInitialize() -> HRESULT
{
    HRESULT hr = S_OK;
    if (m_tfCompartment != nullptr)
    {
        CComPtr<ITfSource> pSource;

        hr = m_tfCompartment.QueryInterface(&pSource);

        if (SUCCEEDED(hr))
        {
            hr = pSource->UnadviseSink(m_dwCookie);
        }

        m_tfCompartment.Release();
    }
    m_dwCookie        = TF_INVALID_COOKIE;
    m_guidCompartment = GUID_NULL;
    return hr;
}

auto Tsf::TsfCompartment::SetValue(ULONG value) const -> HRESULT
{
    if (m_tfCompartment == nullptr)
    {
        // Returning S_OK from an uninitialized compartment (the old behavior)
        // made callers believe the value was applied — e.g. ToogleKeyboard on
        // a failed TSF init reported success and no one was the wiser.
        return E_FAIL;
    }
    CComVariant var; // VariantInit: fully zeroed
    var.vt   = VT_I4;
    var.lVal = static_cast<LONG>(value);
    return m_tfCompartment->SetValue(m_tfClientId, &var);
}

auto Tsf::TsfCompartment::GetValue(__out ULONG &pValue) const -> HRESULT
{
    if (m_tfCompartment != nullptr)
    {
        CComVariant variant;
        HRESULT     hr = m_tfCompartment->GetValue(&variant);
        if (SUCCEEDED(hr) && variant.vt == VT_I4)
        {
            pValue = static_cast<ULONG>(variant.lVal);
            return S_OK;
        }
    }
    return E_FAIL;
}

auto Tsf::TsfCompartment::QueryInterface(const IID &riid, void **ppvObject) -> HRESULT
{
    *ppvObject = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfCompartmentEventSink))
    {
        // Explicit interface cast: `*ppvObject = this;` only happens to be
        // correct because ITfCompartmentEventSink is the sole COM base.
        *ppvObject = static_cast<ITfCompartmentEventSink *>(this);
    }

    if (*ppvObject != nullptr)
    {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

auto Tsf::TsfCompartment::AddRef() -> ULONG
{
    return ++m_refCount;
}

auto Tsf::TsfCompartment::Release() -> ULONG
{
    // Underflow guard: an extra Release must not wrap the counter (a wrapped
    // 0xFFFFFFFF would resurrect the object and leak it forever).
    if (m_refCount == 0)
    {
        logger::error("TsfCompartment::Release called with a zero reference count.");
        return 0;
    }
    --m_refCount;
    if (m_refCount == 0)
    {
        delete this;
        return 0;
    }
    return m_refCount;
}

auto Tsf::TsfCompartment::OnChange(const GUID &rguid) -> HRESULT
{
    HRESULT hresult = S_OK;
    if (IsEqualGUID(rguid, m_guidCompartment) != 0)
    {
        if (ULONG value = 0; SUCCEEDED(GetValue(value)))
        {
            return m_callback(&rguid, value);
        }
    }
    return hresult;
}
