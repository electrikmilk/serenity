/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::MimeSniff {

// https://mimesniff.spec.whatwg.org/#javascript-mime-type-essence-match
bool is_javascript_mime_type_essence_match(StringView string)
{
    // NOTE: The mime type parser automatically lowercases the essence.
    auto type = MimeType::parse(string).release_value_but_fixme_should_propagate_errors();
    if (!type.has_value())
        return false;
    return type->is_javascript();
}

static bool contains_only_http_quoted_string_token_code_points(StringView string)
{
    // https://mimesniff.spec.whatwg.org/#http-quoted-string-token-code-point
    // An HTTP quoted-string token code point is U+0009 TAB, a code point in the range U+0020 SPACE to U+007E (~), inclusive,
    // or a code point in the range U+0080 through U+00FF (ÿ), inclusive.
    for (char ch : string) {
        // NOTE: This doesn't check for ch <= 0xFF, as ch is 8-bits and so that condition will always be true.
        if (!(ch == '\t' || (ch >= 0x20 && ch <= 0x7E) || (u8)ch >= 0x80))
            return false;
    }
    return true;
}

MimeType::MimeType(String type, String subtype)
    : m_type(move(type))
    , m_subtype(move(subtype))
{
    // https://mimesniff.spec.whatwg.org/#parameters
    // A MIME type’s parameters is an ordered map whose keys are ASCII strings and values are strings limited to HTTP quoted-string token code points.
    VERIFY(contains_only_http_quoted_string_token_code_points(type));
    VERIFY(contains_only_http_quoted_string_token_code_points(subtype));
}

MimeType::~MimeType() = default;

static bool contains_only_http_token_code_points(StringView string)
{
    // https://mimesniff.spec.whatwg.org/#http-token-code-point
    // An HTTP token code point is U+0021 (!), U+0023 (#), U+0024 ($), U+0025 (%), U+0026 (&), U+0027 ('), U+002A (*),
    // U+002B (+), U+002D (-), U+002E (.), U+005E (^), U+005F (_), U+0060 (`), U+007C (|), U+007E (~), or an ASCII alphanumeric.
    constexpr auto is_certain_non_ascii_alphanumeric = is_any_of("!#$%&'*+-.^_`|~"sv);
    for (char ch : string) {
        if (!is_certain_non_ascii_alphanumeric(ch) && !is_ascii_alphanumeric(ch))
            return false;
    }
    return true;
}

ErrorOr<MimeType> MimeType::create(String type, String value)
{
    auto mime_type = MimeType { move(type), move(value) };
    mime_type.m_cached_essence = TRY(String::formatted("{}/{}", mime_type.m_type, mime_type.m_subtype));
    return mime_type;
}

// https://mimesniff.spec.whatwg.org/#parse-a-mime-type
ErrorOr<Optional<MimeType>> MimeType::parse(StringView string)
{
    // 1. Remove any leading and trailing HTTP whitespace from input.
    auto trimmed_string = string.trim(Fetch::Infrastructure::HTTP_WHITESPACE, TrimMode::Both);

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    GenericLexer lexer(trimmed_string);

    // 3. Let type be the result of collecting a sequence of code points that are not U+002F (/) from input, given position.
    auto type = lexer.consume_until('/');

    // 4. If type is the empty string or does not solely contain HTTP token code points, then return failure.
    if (type.is_empty() || !contains_only_http_token_code_points(type))
        return OptionalNone {};

    // 5. If position is past the end of input, then return failure.
    if (lexer.is_eof())
        return OptionalNone {};

    // 6. Advance position by 1. (This skips past U+002F (/).)
    lexer.ignore(1);

    // 7. Let subtype be the result of collecting a sequence of code points that are not U+003B (;) from input, given position.
    auto subtype = lexer.consume_until(';');

    // 8. Remove any trailing HTTP whitespace from subtype.
    subtype = subtype.trim(Fetch::Infrastructure::HTTP_WHITESPACE, TrimMode::Right);

    // 9. If subtype is the empty string or does not solely contain HTTP token code points, then return failure.
    if (subtype.is_empty() || !contains_only_http_token_code_points(subtype))
        return OptionalNone {};

    // 10. Let mimeType be a new MIME type record whose type is type, in ASCII lowercase, and subtype is subtype, in ASCII lowercase.
    auto mime_type = TRY(MimeType::create(TRY(Infra::to_ascii_lowercase(type)), TRY(Infra::to_ascii_lowercase(subtype))));

    // 11. While position is not past the end of input:
    while (!lexer.is_eof()) {
        // 1. Advance position by 1. (This skips past U+003B (;).)
        lexer.ignore(1);

        // 2. Collect a sequence of code points that are HTTP whitespace from input given position.
        lexer.ignore_while(is_any_of(Fetch::Infrastructure::HTTP_WHITESPACE));

        // 3. Let parameterName be the result of collecting a sequence of code points that are not U+003B (;) or U+003D (=) from input, given position.
        auto parameter_name_view = lexer.consume_until([](char ch) {
            return ch == ';' || ch == '=';
        });

        // 4. Set parameterName to parameterName, in ASCII lowercase.
        auto parameter_name = TRY(Infra::to_ascii_lowercase(parameter_name_view));

        // 5. If position is not past the end of input, then:
        if (!lexer.is_eof()) {
            // 1. If the code point at position within input is U+003B (;), then continue.
            if (lexer.peek() == ';')
                continue;

            // 2. Advance position by 1. (This skips past U+003D (=).)
            lexer.ignore(1);
        }

        // 6. If position is past the end of input, then break.
        // NOTE: This is not an `else` because the ignore on step 11.5.2 could put us past the end of the input.
        if (lexer.is_eof())
            break;

        // 7. Let parameterValue be null.
        String parameter_value;

        // 8. If the code point at position within input is U+0022 ("), then:
        if (lexer.peek() == '"') {
            // 1. Set parameterValue to the result of collecting an HTTP quoted string from input, given position and the extract-value flag.
            parameter_value = TRY(Fetch::Infrastructure::collect_an_http_quoted_string(lexer, Fetch::Infrastructure::HttpQuotedStringExtractValue::Yes));

            // 2. Collect a sequence of code points that are not U+003B (;) from input, given position.
            lexer.ignore_until(';');
        }

        // 9. Otherwise:
        else {
            // 1. Set parameterValue to the result of collecting a sequence of code points that are not U+003B (;) from input, given position.
            parameter_value = TRY(String::from_utf8(lexer.consume_until(';')));

            // 2. Remove any trailing HTTP whitespace from parameterValue.
            parameter_value = TRY(parameter_value.trim(Fetch::Infrastructure::HTTP_WHITESPACE, TrimMode::Right));

            // 3. If parameterValue is the empty string, then continue.
            if (parameter_value.is_empty())
                continue;
        }

        // 10. If all of the following are true
        if (
            // - parameterName is not the empty string
            !parameter_name.is_empty()
            // - parameterName solely contains HTTP token code points
            && contains_only_http_token_code_points(parameter_name)
            // - parameterValue solely contains HTTP quoted-string token code points
            && contains_only_http_quoted_string_token_code_points(parameter_value)
            // - mimeType’s parameters[parameterName] does not exist
            && !mime_type.m_parameters.contains(parameter_name)) {
            // then set mimeType’s parameters[parameterName] to parameterValue.
            TRY(mime_type.m_parameters.try_set(move(parameter_name), move(parameter_value)));
        }
    }

    // 12. Return mimeType.
    return mime_type;
}

// https://mimesniff.spec.whatwg.org/#mime-type-essence
String const& MimeType::essence() const
{
    // The essence of a MIME type mimeType is mimeType’s type, followed by U+002F (/), followed by mimeType’s subtype.
    return m_cached_essence;
}

// https://mimesniff.spec.whatwg.org/#serialize-a-mime-type
ErrorOr<String> MimeType::serialized() const
{
    // 1. Let serialization be the concatenation of mimeType’s type, U+002F (/), and mimeType’s subtype.
    StringBuilder serialization;
    TRY(serialization.try_append(m_type));
    TRY(serialization.try_append('/'));
    TRY(serialization.try_append(m_subtype));

    // 2. For each name → value of mimeType’s parameters:
    for (auto [name, value] : m_parameters) {
        // 1. Append U+003B (;) to serialization.
        TRY(serialization.try_append(';'));

        // 2. Append name to serialization.
        TRY(serialization.try_append(name));

        // 3. Append U+003D (=) to serialization.
        TRY(serialization.try_append('='));

        // 4. If value does not solely contain HTTP token code points or value is the empty string, then:
        if (!contains_only_http_token_code_points(value) || value.is_empty()) {
            // 1. Precede each occurrence of U+0022 (") or U+005C (\) in value with U+005C (\).
            value = TRY(value.replace("\\"sv, "\\\\"sv, ReplaceMode::All));
            value = TRY(value.replace("\""sv, "\\\""sv, ReplaceMode::All));

            // 2. Prepend U+0022 (") to value.
            // 3. Append U+0022 (") to value.
            value = TRY(String::formatted("\"{}\"", value));
        }

        // 5. Append value to serialization.
        TRY(serialization.try_append(value));
    }

    // 3. Return serialization.
    return serialization.to_string();
}

ErrorOr<void> MimeType::set_parameter(String name, String value)
{
    // https://mimesniff.spec.whatwg.org/#parameters
    // A MIME type’s parameters is an ordered map whose keys are ASCII strings and values are strings limited to HTTP quoted-string token code points.
    VERIFY(contains_only_http_quoted_string_token_code_points(name));
    VERIFY(contains_only_http_quoted_string_token_code_points(value));
    TRY(m_parameters.try_set(move(name), move(value)));
    return {};
}

// https://mimesniff.spec.whatwg.org/#xml-mime-type
bool MimeType::is_xml() const
{
    return m_subtype.ends_with_bytes("+xml"sv) || essence().is_one_of("text/xml"sv, "application/xml"sv);
}

// https://mimesniff.spec.whatwg.org/#html-mime-type
bool MimeType::is_html() const
{
    return essence().is_one_of("text/html"sv);
}

// https://mimesniff.spec.whatwg.org/#javascript-mime-type
bool MimeType::is_javascript() const
{
    return essence().is_one_of(
        "application/ecmascript"sv,
        "application/javascript"sv,
        "application/x-ecmascript"sv,
        "application/x-javascript"sv,
        "text/ecmascript"sv,
        "text/javascript"sv,
        "text/javascript1.0"sv,
        "text/javascript1.1"sv,
        "text/javascript1.2"sv,
        "text/javascript1.3"sv,
        "text/javascript1.4"sv,
        "text/javascript1.5"sv,
        "text/jscript"sv,
        "text/livescript"sv,
        "text/x-ecmascript"sv,
        "text/x-javascript"sv);
}

}
