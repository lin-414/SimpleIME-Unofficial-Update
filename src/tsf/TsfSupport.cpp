#include "tsf/TsfSupport.h"

#include "WCharUtils.h"
#include "log.h"

#include <comdef.h>

auto Tsf::ToErrorMessage(const HRESULT hresult) -> std::string
{
    const _com_error err(hresult);
    return WCharUtils::ToString(err.ErrorMessage());
}

auto Tsf::TsfSupport::InitializeTsf(const bool uiLessMode) -> HRESULT
{
    if (m_initialized)
    {
        return S_OK;
    }

    try
    {
        HRESULT hresult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ATLENSURE_SUCCEEDED(hresult);
        // Track from here on (S_FALSE counts as well — it still requires a
        // matching CoUninitialize): a failure in any later step must NOT skip
        // the apartment unbalance, or retries accumulate STA references.
        m_comInitialized = true;

        hresult = m_pThreadMgr.CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER);
        ATLENSURE_SUCCEEDED(hresult);

        if (uiLessMode)
        {
            hresult = m_pThreadMgr->ActivateEx(&m_tfClientId, TF_TMAE_UIELEMENTENABLEDONLY);
        }
        else
        {
            hresult = m_pThreadMgr->Activate(&m_tfClientId);
        }
        ATLENSURE_SUCCEEDED(hresult);
        ATLENSURE_SUCCEEDED(m_pThreadMgr.QueryInterface(&m_messagePump));
        ATLENSURE_SUCCEEDED(m_pThreadMgr.QueryInterface(&m_KeystrokeMgr));
        m_initialized = true;
        return S_OK;
    }
    catch (CAtlException &atlException)
    {
        logger::error("Fatal error: Initialize TSF failed: {}", ToErrorMessage(atlException.m_hr));
    }
    return E_FAIL;
}

Tsf::TsfSupport::~TsfSupport()
{
    UnInitializeTsf();
}
