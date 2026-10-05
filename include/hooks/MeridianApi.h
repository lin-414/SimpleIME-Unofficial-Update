#pragma once

#include "MeridianUI/ViewAPI.h"

// Negotiation helper for the published Meridian.View/1 extension, kept
// header-only so it can be exercised by unit tests with a mock query.
namespace Hooks::MeridianBridge
{
inline ::Meridian::UI::View::IViewAPI *RequestMeridianView(::Meridian::UI::View::QueryMeridianExtensionFn query)
{
    using namespace ::Meridian::UI::View;
    void *value = nullptr;
    return query != nullptr && query(EXTENSION_NAME, INTERFACE_VERSION, &value, nullptr, "SimpleIME") && value != nullptr
               ? static_cast<::Meridian::UI::View::IViewAPI *>(value)
               : nullptr;
}
} // namespace Hooks::MeridianBridge
