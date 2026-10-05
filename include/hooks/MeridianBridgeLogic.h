#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Pure, SKSE-free logic of the Meridian bridge, extracted so the session
// protocol can be unit-tested headless (the .cpp keeps all the Windows/CEF
// interaction).
namespace Hooks::MeridianBridgeLogic
{
/// Outcome of one `window.simpleIMEResult("<id>:<status>")` callback.
enum class ListenerOutcome
{
    Ignore,   ///< payload belongs to an older capture session (or is unusable)
    Ready,    ///< a DOM text field was captured for this session
    Inserted, ///< the pending chunk was committed into the field
    NoField,  ///< the view has no focused text field
    Failed,   ///< stale-field / insert-rejected / capture-error / insert-error
    Pick      ///< the overlay candidate at :pick:<n> was clicked
};

/// Classify a listener payload against the expected session id. Payloads are
/// status words only — typed text never crosses this boundary.
inline ListenerOutcome ParseListenerPayload(const std::uint64_t expectedSeq, const char *payload)
{
    if (payload == nullptr)
    {
        return ListenerOutcome::Ignore;
    }
    const std::string prefix = std::to_string(expectedSeq) + ":";
    std::string_view  status(payload);
    if (!status.starts_with(prefix))
    {
        return ListenerOutcome::Ignore;
    }
    status.remove_prefix(prefix.size());
    if (status == "ready")
    {
        return ListenerOutcome::Ready;
    }
    if (status == "inserted")
    {
        return ListenerOutcome::Inserted;
    }
    if (status == "no-field")
    {
        return ListenerOutcome::NoField;
    }
    return ListenerOutcome::Failed;
}

/// Extract the candidate index from a `<id>:pick:<n>` overlay payload (the
/// DOM candidate list is click-active). Returns false for every non-pick or
/// unparsable payload; the numeric part must be a plain non-negative integer.
inline bool TryParsePickPayload(const std::uint64_t expectedSeq, const char *payload, std::uint32_t &indexOut)
{
    if (payload == nullptr)
    {
        return false;
    }
    const std::string prefix = std::to_string(expectedSeq) + ":pick:";
    std::string_view  rest(payload);
    if (!rest.starts_with(prefix))
    {
        return false;
    }
    rest.remove_prefix(prefix.size());
    if (rest.empty() || rest.size() > 9)
    {
        return false;
    }
    std::uint32_t index = 0;
    for (const char c : rest)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        index = index * 10 + static_cast<std::uint32_t>(c - '0');
    }
    indexOut = index;
    return true;
}

/// Encode UTF-16 text as a JS string literal of \uXXXX escapes only: source
/// text can never become executable script, and the escape form survives
/// embedded quotes, line separators (U+2028/U+2029) and control characters.
inline std::string JsString(const std::u16string_view text)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() * 6 + 2);
    out += '"';
    for (const char16_t c : text)
    {
        out += "\\u";
        out += hex[(c >> 12) & 15];
        out += hex[(c >> 8) & 15];
        out += hex[(c >> 4) & 15];
        out += hex[c & 15];
    }
    out += '"';
    return out;
}

/// Decode one JSON-serialized string argument as the UIPlatform host passes
/// callback arguments to C++ (`SimpleIME.result("<id>:<status>")` arrives
/// with the argument serialized by the host's JSON converter, i.e. wrapped
/// in quotes with escapes). Handles the standard two-char escapes plus
/// \uXXXX with surrogate pairs (decoded to UTF-8). A payload not wrapped in
/// quotes is accepted verbatim — the protocol words are plain ASCII, so a
/// host serialization change can never silently swallow a status report.
inline bool TryDecodeJsonStringArg(const char *arg, std::string &out)
{
    out.clear();
    if (arg == nullptr)
    {
        return false;
    }
    const std::string_view input(arg);
    if (input.empty() || input.front() != '"')
    {
        out.assign(input);
        return true;
    }
    if (input.size() < 2 || input.back() != '"')
    {
        return false; // opening quote without a closing one
    }
    constexpr static char hexDigits[] = "0123456789abcdefABCDEF";
    const auto isHex = [](const char c) {
        for (const char h : hexDigits)
        {
            if (c == h)
            {
                return true;
            }
        }
        return false;
    };
    const auto hexValue = [](const char c) {
        return c <= '9' ? static_cast<unsigned>(c - '0') : (c | 0x20u) - 'a' + 10U;
    };
    std::size_t i = 1;
    const std::size_t end = input.size() - 1; // closing quote
    while (i < end)
    {
        const char c = input[i];
        if (c != '\\')
        {
            out.push_back(c);
            i += 1;
            continue;
        }
        if (i + 1 >= end)
        {
            return false; // dangling backslash
        }
        const char escape = input[i + 1];
        i += 2;
        switch (escape)
        {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
                break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                if (i + 4 > end)
                {
                    return false;
                }
                unsigned codePoint = 0;
                for (int digit = 0; digit < 4; ++digit)
                {
                    if (!isHex(input[i + digit]))
                    {
                        return false;
                    }
                    codePoint = codePoint * 16 + hexValue(input[i + digit]);
                }
                i += 4;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF && i + 6 <= end && input[i] == '\\' &&
                    input[i + 1] == 'u')
                {
                    unsigned low = 0;
                    bool lowOk   = true;
                    for (int digit = 0; digit < 4; ++digit)
                    {
                        if (!isHex(input[i + 2 + digit]))
                        {
                            lowOk = false;
                            break;
                        }
                        low = low * 16 + hexValue(input[i + 2 + digit]);
                    }
                    if (lowOk && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        codePoint    = 0x10000U + ((codePoint - 0xD800U) << 10) + (low - 0xDC00U);
                        i           += 6;
                    }
                }
                // UTF-8 encode.
                if (codePoint < 0x80)
                {
                    out.push_back(static_cast<char>(codePoint));
                }
                else if (codePoint < 0x800)
                {
                    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else if (codePoint < 0x10000)
                {
                    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                break;
            }
            default:
                return false; // unknown escape: not a payload we can trust
        }
    }
    return true;
}
} // namespace Hooks::MeridianBridgeLogic
