/*
 * Minimal ATL COM smart-pointer / macro shim.
 *
 * VS 2022 BuildTools does not ship ATL headers (atlcomcli.h, atlbase.h, etc.).
 * SimpleIME only uses a small, well-defined subset of ATL, all of which is
 * pure-header and does not require the ATL runtime DLL. This file
 * reimplements just that subset so the project compiles under clang-cl /
 * MSVC with only the C++/Core desktop workload installed.
 *
 * Symbols provided:
 *   CComPtr<T>                 - COM refcount smart pointer
 *   CComQIPtr<T>               - QueryInterface wrapper
 *   CComVariant                - VARIANT smart wrapper
 *   CAtlException              - exception carrying HRESULT
 *   ATLENSURE_SUCCEEDED / ATLENSURE_MESSAGE_SUCCEEDED
 *   ATLENSURE_RETURN / ATLENSURE
 *   _ATL_COM_BEGIN / _ATL_COM_END
 *   AtlIsEqualGUID
 */

#pragma once

#include <unknwn.h>
#include <tchar.h>
#include <assert.h>
#include <cstddef>
#include <stdexcept>

// ---- CComPtr ---------------------------------------------------------
template <class T>
class CComPtr {
    // ATL makes m_p public; we match that so CComQIPtr and any user code
    // that reaches into m_p compiles identically.
public:
    T *m_p = nullptr;

public:
    CComPtr() noexcept = default;

    CComPtr(T *p) noexcept : m_p(p) {
        if (p) p->AddRef();
    }

    CComPtr(const CComPtr<T> &other) noexcept : m_p(other.m_p) {
        if (m_p) m_p->AddRef();
    }

    CComPtr(CComPtr<T> &&other) noexcept : m_p(other.m_p) {
        other.m_p = nullptr;
    }

    ~CComPtr() { Release(); }

    CComPtr<T> &operator=(T *p) {
        if (p) p->AddRef();
        Release();
        m_p = p;
        return *this;
    }

    CComPtr<T> &operator=(const CComPtr<T> &other) {
        if (this != &other) {
            if (other.m_p) other.m_p->AddRef();
            Release();
            m_p = other.m_p;
        }
        return *this;
    }

    CComPtr<T> &operator=(CComPtr<T> &&other) noexcept {
        if (m_p != other.m_p) {  // compare stored pointers, not &other (operator& is overloaded)
            Release();
            m_p = other.m_p;
            other.m_p = nullptr;
        }
        return *this;
    }

    bool operator!=(std::nullptr_t) const noexcept { return m_p != nullptr; }
    bool operator==(std::nullptr_t) const noexcept { return m_p == nullptr; }
    operator T *() const noexcept { return m_p; }

    // ATL-compatible: returns the raw address so COM out-parameters like
    // `mgr->GetCompartment(&ptr)` or `obj.QueryInterface(&ptr)` work.
    // Matches ATL's deprecated-but-widely-used CComPtr::operator&.
    T **operator&() noexcept { return &m_p; }

    bool operator!() const noexcept { return m_p == nullptr; }
    explicit operator bool() const noexcept { return m_p != nullptr; }

    T &operator*() const { assert(m_p != nullptr); return *m_p; }
    T *operator->() const { assert(m_p != nullptr); return m_p; }

    void Release() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }

    HRESULT CoCreateInstance(REFCLSID rclsid, IUnknown *pUnkOuter = nullptr,
                              DWORD dwClsContext = CLSCTX_ALL) {
        Release();
        HRESULT hr = ::CoCreateInstance(rclsid, pUnkOuter, dwClsContext,
                                         __uuidof(T),
                                         reinterpret_cast<void **>(&m_p));
        if (FAILED(hr)) m_p = nullptr;
        return hr;
    }

    // two-arg: QueryInterface(iid, ppv)
    HRESULT QueryInterface(REFIID iid, void **ppv) const {
        if (!m_p) {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        return m_p->QueryInterface(iid, ppv);
    }

    // one-arg: QueryInterface(ppv)  -> uses __uuidof(T)
    HRESULT QueryInterface(void **ppv) const {
        return QueryInterface(__uuidof(T), ppv);
    }

    // QueryInterface(iid, T **)
    HRESULT QueryInterface(REFIID iid, T **ppv) const {
        return QueryInterface(iid, reinterpret_cast<void **>(ppv));
    }

    // ATL-style: QueryInterface(&otherCComPtr) -- matches IUnknown's single-arg
    // template overload `QueryInterface(Q** pp)`; `&ptr` here is T** via
    // operator&, so Q deduces to the target interface type.
    template <class Q>
    HRESULT QueryInterface(Q **pp) const {
        if (!pp) {
            return E_POINTER;
        }
        *pp = nullptr;
        if (!m_p) {
            return E_NOINTERFACE;
        }
        return m_p->QueryInterface(__uuidof(Q), reinterpret_cast<void **>(pp));
    }

    HRESULT CopyFrom(IUnknown *pUnknown) {
        if (!pUnknown) {
            Release();
            return S_OK;
        }
        T *p = nullptr;
        HRESULT hr = pUnknown->QueryInterface(__uuidof(T),
                                               reinterpret_cast<void **>(&p));
        if (SUCCEEDED(hr)) {
            Release();
            m_p = p;
        }
        return hr;
    }
};

// ---- CComQIPtr -------------------------------------------------------
template <class T>
class CComQIPtr : public CComPtr<T> {
public:
    CComQIPtr() = default;

    CComQIPtr(IUnknown *p) {
        if (p)
            p->QueryInterface(__uuidof(T), this->operator&());
    }

    CComQIPtr(IDispatch *p) {
        if (p)
            p->QueryInterface(this->operator&());
    }

    template <class Q>
    CComQIPtr(const CComPtr<Q> &p) {
        if (p.m_p)
            p.m_p->QueryInterface(this->operator&());
    }

    template <class Q>
    CComQIPtr(Q *p) {
        if (p)
            p->QueryInterface(this->operator&());
    }

    operator IUnknown *() const { return CComPtr<T>::m_p; }

    HRESULT QueryInterface(REFIID iid, void **ppv) {
        return CComPtr<T>::QueryInterface(iid, ppv);
    }
};

// ---- CComBSTR --------------------------------------------------------
// Minimal ATL-compatible BSTR wrapper covering the subset SimpleIME uses:
// default ctor, out-param via &, Length(), == nullptr, wstring_view source.
class CComBSTR {
public:
    CComBSTR() noexcept : m_str(nullptr) {}
    CComBSTR(std::nullptr_t) noexcept : m_str(nullptr) {}  // non-explicit to allow `CComBSTR b = nullptr;`
    explicit CComBSTR(OLECHAR *src) : m_str(src ? SysAllocString(src) : nullptr) {}

    ~CComBSTR() { SysFreeString(m_str); }

    CComBSTR(const CComBSTR &other) = delete;
    CComBSTR &operator=(const CComBSTR &other) = delete;

    BSTR *operator&() noexcept { return &m_str; }

    [[nodiscard]] UINT Length() const noexcept { return m_str ? SysStringLen(m_str) : 0U; }

    bool operator==(std::nullptr_t) const noexcept { return m_str == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return m_str != nullptr; }
    // BSTR is `typedef OLECHAR *BSTR`, so a single conversion operator covers both.
    operator BSTR() const noexcept { return m_str; }

private:
    BSTR m_str;
};

// ---- CComVariant -----------------------------------------------------
// Inherits from VARIANT so callers can use .vt, .ulVal, .lVal, .llVal,
// .ival etc. as direct field access -- matching ATL's CComVariant.
class CComVariant : public VARIANT {
public:
    CComVariant() { VariantInit(this); }
    ~CComVariant() { Clear(); }

    CComVariant(const CComVariant &other) {
        VariantInit(this);
        (void)VariantCopy(this, &other);
    }

    CComVariant &operator=(const CComVariant &other) {
        if (this != &other) {
            VariantClear(this);
            (void)VariantCopy(this, &other);
        }
        return *this;
    }

    void Clear() { VariantClear(this); }

    VARIANT *operator&() noexcept { return this; }
    const VARIANT *operator&() const noexcept { return this; }
    operator VARIANT *() noexcept { return this; }
    operator const VARIANT *() const noexcept { return this; }
};

// ---- CAtlException ---------------------------------------------------
class CAtlException : public std::exception {
public:
    explicit CAtlException(HRESULT hr = E_FAIL) : m_hr(hr) {}
    HRESULT m_hr;
    const char *what() const noexcept override { return "CAtlException"; }
};

// ---- ATL ensure macros -----------------------------------------------
#define ATLENSURE_SUCCEEDED(hr)                                                \
    if (FAILED(hr)) throw CAtlException(hr)

#define ATLENSURE_MESSAGE_SUCCEEDED(hr, msg)                                   \
    if (FAILED(hr)) throw CAtlException(hr)

#define ATLENSURE_RETURN(cond)                                                 \
    if (!(cond)) return E_FAIL

#define ATLENSURE(cond)                                                        \
    if (!(cond)) throw CAtlException(E_FAIL)

// ---- _ATL_COM_BEGIN / _ATL_COM_END -----------------------------------
// Expands to a try/catch that turns CAtlException into a returning HRESULT.
#define _ATL_COM_BEGIN                                                         \
    try {                                                                      \
        HRESULT hrResult_ = S_OK;
#define _ATL_COM_END                                                           \
    }                                                                          \
    catch (CAtlException &atlException_) {                                    \
        return atlException_.m_hr;                                             \
    }                                                                          \
    catch (const std::exception &) {                                           \
        return E_FAIL;                                                         \
    }

// ---- GUID helpers ----------------------------------------------------
inline bool AtlIsEqualGUID(REFGUID g1, REFGUID g2) {
    return memcmp(&g1, &g2, sizeof(GUID)) == 0;
}
