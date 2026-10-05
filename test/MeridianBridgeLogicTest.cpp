//
// Headless tests for the Meridian bridge's pure session-protocol logic
// (extracted into MeridianBridgeLogic.h so no SKSE/Windows machinery is
// needed here).
//
#include "hooks/MeridianBridgeLogic.h"

#include <gtest/gtest.h>

using namespace Hooks::MeridianBridgeLogic;

TEST(MeridianListenerPayload, classifies_all_documented_statuses)
{
    EXPECT_EQ(ParseListenerPayload(7, "7:ready"), ListenerOutcome::Ready);
    EXPECT_EQ(ParseListenerPayload(7, "7:inserted"), ListenerOutcome::Inserted);
    EXPECT_EQ(ParseListenerPayload(7, "7:no-field"), ListenerOutcome::NoField);
    EXPECT_EQ(ParseListenerPayload(7, "7:stale-field"), ListenerOutcome::Failed);
    EXPECT_EQ(ParseListenerPayload(7, "7:insert-rejected"), ListenerOutcome::Failed);
    EXPECT_EQ(ParseListenerPayload(7, "7:capture-error"), ListenerOutcome::Failed);
    EXPECT_EQ(ParseListenerPayload(7, "7:insert-error"), ListenerOutcome::Failed);
}

TEST(MeridianListenerPayload, ignores_stale_sessions)
{
    // Older or newer session ids must never mutate the current session.
    EXPECT_EQ(ParseListenerPayload(7, "6:ready"), ListenerOutcome::Ignore);
    EXPECT_EQ(ParseListenerPayload(7, "8:inserted"), ListenerOutcome::Ignore);
    EXPECT_EQ(ParseListenerPayload(7, "7x:ready"), ListenerOutcome::Ignore); // prefix is "7:", not "7x:"
}

TEST(MeridianListenerPayload, ignores_unusable_payloads)
{
    EXPECT_EQ(ParseListenerPayload(7, nullptr), ListenerOutcome::Ignore);
    EXPECT_EQ(ParseListenerPayload(7, ""), ListenerOutcome::Ignore);
    EXPECT_EQ(ParseListenerPayload(7, "ready"), ListenerOutcome::Ignore); // missing id prefix
}

TEST(MeridianPickPayload, parses_valid_indices)
{
    std::uint32_t index = 99;
    EXPECT_TRUE(TryParsePickPayload(7, "7:pick:0", index));
    EXPECT_EQ(index, 0U);
    EXPECT_TRUE(TryParsePickPayload(7, "7:pick:9", index));
    EXPECT_EQ(index, 9U);
    EXPECT_TRUE(TryParsePickPayload(12, "12:pick:42", index));
    EXPECT_EQ(index, 42U);
}

TEST(MeridianPickPayload, rejects_foreign_or_malformed_payloads)
{
    std::uint32_t index = 99;
    EXPECT_FALSE(TryParsePickPayload(7, nullptr, index));
    EXPECT_FALSE(TryParsePickPayload(7, "6:pick:1", index));      // stale session
    EXPECT_FALSE(TryParsePickPayload(7, "8:pick:1", index));      // future session
    EXPECT_FALSE(TryParsePickPayload(7, "7:ready", index));       // different status word
    EXPECT_FALSE(TryParsePickPayload(7, "7:pick:", index));       // missing index
    EXPECT_FALSE(TryParsePickPayload(7, "7:pick:1a", index));     // non-digit tail
    EXPECT_FALSE(TryParsePickPayload(7, "7:pick:-1", index));     // sign is not a digit
    EXPECT_FALSE(TryParsePickPayload(7, "7:pick:1234567890", index)); // >9 digits
    EXPECT_EQ(index, 99U);                                        // untouched on every rejection
}

TEST(MeridianJsString, escapes_everything_to_literal_text)
{
    // Quote, backslash and newline can never terminate the literal or form code.
    EXPECT_EQ(JsString(u"'\"\\\n"), "\"\\u0027\\u0022\\u005c\\u000a\"");
    // JS line separators (U+2028/U+2029) are raw-newline-equivalents in some
    // parsers; escaping keeps the payload a one-line literal.
    EXPECT_EQ(JsString(u"\u2028\u2029"), "\"\\u2028\\u2029\"");
}

TEST(MeridianJsString, preserves_cjk_and_surrogate_pairs)
{
    EXPECT_EQ(JsString(u"\u4e2d\u6587"), "\"\\u4e2d\\u6587\"");
    // U+1F600 arrives as the surrogate pair D83D DE00 and passes through.
    EXPECT_EQ(JsString(u"\U0001F600"), "\"\\ud83d\\ude00\"");
}

TEST(MeridianJsString, wraps_empty_text_as_empty_literal)
{
    EXPECT_EQ(JsString(u""), "\"\"");
}

TEST(MeridianJsonStringArg, decodes_the_host_serialized_string_literal)
{
    // The UIPlatform host serializes each JS callback argument as a JSON
    // value: our payload arrives quoted and escaped.
    std::string out;
    EXPECT_TRUE(TryDecodeJsonStringArg("\"7:ready\"", out));
    EXPECT_EQ(out, "7:ready");
    EXPECT_TRUE(TryDecodeJsonStringArg("\"12:pick:42\"", out));
    EXPECT_EQ(out, "12:pick:42");
    EXPECT_TRUE(TryDecodeJsonStringArg("\"\"", out)); // empty string literal
    EXPECT_EQ(out, "");
}

TEST(MeridianJsonStringArg, decodes_escapes_and_unicode)
{
    std::string out;
    EXPECT_TRUE(TryDecodeJsonStringArg(R"("a\"b\\c\/d")", out));
    EXPECT_EQ(out, "a\"b\\c/d");
    EXPECT_TRUE(TryDecodeJsonStringArg("\"\\u4e2d\"", out));
    EXPECT_EQ(out, "\xE4\xB8\xAD"); // 中
    // Surrogate pair → UTF-8 (U+1F600).
    EXPECT_TRUE(TryDecodeJsonStringArg("\"\\ud83d\\ude00\"", out));
    EXPECT_EQ(out, "\xF0\x9F\x98\x80");
    // Unpaired high surrogate: decoded as-is (lossy but harmless for status
    // words, which are plain ASCII anyway).
    EXPECT_TRUE(TryDecodeJsonStringArg("\"\\ud83d\"", out));
    EXPECT_EQ(out, "\xED\xA0\xBD");
    EXPECT_TRUE(TryDecodeJsonStringArg("\"tab\\there\"", out));
    EXPECT_EQ(out, "tab\there");
}

TEST(MeridianJsonStringArg, accepts_bare_payloads_and_rejects_garbage)
{
    std::string out;
    // Not JSON-quoted: pass through verbatim (a serialization change must not
    // swallow status reports; ParseListenerPayload rejects foreign words).
    EXPECT_TRUE(TryDecodeJsonStringArg("7:ready", out));
    EXPECT_EQ(out, "7:ready");
    EXPECT_FALSE(TryDecodeJsonStringArg(nullptr, out));
    EXPECT_FALSE(TryDecodeJsonStringArg("\"unterminated", out));
    EXPECT_FALSE(TryDecodeJsonStringArg("\"bad\\xescape\"", out));
    EXPECT_FALSE(TryDecodeJsonStringArg("\"short\\u00\"", out));
}
