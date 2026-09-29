// Hardware Lifecycle - strict JSON codec.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This translation unit implements the bounded JSON subset used by the export
// and import document format. It sits on an adversarial boundary: every
// rejection is explicit, nothing is repaired, clamped or truncated, and the
// only authority on what a document means is what its bytes say.
//
// Parsing is recursive descent with an explicit depth bound, so a hostile
// document cannot exhaust the stack. Object member order is preserved exactly
// as the document gives it, which makes the writer a pure function of the value
// it is handed: equal values always render to equal bytes.

#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle::detail {

// ---------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------

JsonValue JsonValue::make_null() noexcept { return JsonValue(); }

JsonValue JsonValue::make_bool(bool value) noexcept {
  JsonValue result;
  result.type_ = Type::Bool;
  result.boolean_ = value;
  return result;
}

JsonValue JsonValue::make_number(std::uint64_t value) noexcept {
  JsonValue result;
  result.type_ = Type::Number;
  result.number_ = value;
  return result;
}

JsonValue JsonValue::make_string(std::string value) {
  JsonValue result;
  result.type_ = Type::String;
  result.string_ = std::move(value);
  return result;
}

JsonValue JsonValue::make_array() {
  JsonValue result;
  result.type_ = Type::Array;
  return result;
}

JsonValue JsonValue::make_object() {
  JsonValue result;
  result.type_ = Type::Object;
  return result;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (type_ != Type::Object) {
    return nullptr;
  }
  for (const std::pair<std::string, JsonValue>& field : fields_) {
    if (field.first == key) {
      return &field.second;
    }
  }
  return nullptr;
}

void JsonValue::push_back(JsonValue value) { items_.push_back(std::move(value)); }

void JsonValue::emplace(std::string key, JsonValue value) {
  fields_.emplace_back(std::move(key), std::move(value));
}

namespace {

// ---------------------------------------------------------------------------
// Byte level primitives
// ---------------------------------------------------------------------------

constexpr std::uint64_t kMaxUnsignedValue = (std::numeric_limits<std::uint64_t>::max)();

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] constexpr bool is_digit(char byte) noexcept { return byte >= '0' && byte <= '9'; }

[[nodiscard]] constexpr bool is_hex_digit(char byte) noexcept {
  return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

[[nodiscard]] constexpr std::uint32_t hex_digit_value(char byte) noexcept {
  if (byte >= '0' && byte <= '9') {
    return static_cast<std::uint32_t>(byte - '0');
  }
  if (byte >= 'a' && byte <= 'f') {
    return static_cast<std::uint32_t>(byte - 'a') + 10u;
  }
  return static_cast<std::uint32_t>(byte - 'A') + 10u;
}

/// The only bytes JSON permits between tokens.
[[nodiscard]] constexpr bool is_json_whitespace(char byte) noexcept {
  return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

/// Appends one Unicode scalar value as UTF-8. Callers guarantee the value is
/// neither a surrogate nor above U+10FFFF, so the encoding cannot fail.
void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7Fu) {
    out.push_back(static_cast<char>(code_point));
    return;
  }
  if (code_point <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    return;
  }
  if (code_point <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    return;
  }
  out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
  out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
}

// ---------------------------------------------------------------------------
// Error construction
// ---------------------------------------------------------------------------

/// Every rejection carries the byte offset it was detected at. A document is
/// adversarial input, and "somewhere inside this file" is not a diagnosis.
[[nodiscard]] Error json_error(ErrorCode code, std::string message, std::size_t offset) {
  Error error = Error::make(code, std::move(message));
  // with() is fluent and returns the note trail; the note is the effect here.
  (void)error.with("offset", std::to_string(offset));
  return error;
}

[[nodiscard]] Error malformed(std::string message, std::size_t offset) {
  return json_error(ErrorCode::MalformedRequest, std::move(message), offset);
}

[[nodiscard]] Error limit_exceeded(std::string message, std::size_t offset) {
  return json_error(ErrorCode::LimitExceeded, std::move(message), offset);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// One pass over the document bytes. The parser holds nothing but the cursor,
/// so the parse of a document is fully determined by its bytes.
class Parser {
 public:
  explicit Parser(std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] Result<JsonValue> parse_document() {
    Result<JsonValue> value = parse_value(0);
    if (!value) {
      return value.error();
    }
    skip_whitespace();
    if (!at_end()) {
      return malformed("unexpected trailing content after the top level value", pos_);
    }
    return std::move(value);
  }

 private:
  [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

  /// True when the bytes at the cursor start with literal. Uses no throwing
  /// substr operation, so a truncated tail simply does not match.
  [[nodiscard]] bool matches(std::string_view literal) const noexcept {
    if (text_.size() - pos_ < literal.size()) {
      return false;
    }
    return text_.compare(pos_, literal.size(), literal) == 0;
  }

  void skip_whitespace() noexcept {
    while (pos_ < text_.size() && is_json_whitespace(text_[pos_])) {
      ++pos_;
    }
  }

  [[nodiscard]] Result<JsonValue> parse_value(std::size_t depth) {
    skip_whitespace();
    if (at_end()) {
      return malformed("unexpected end of input, expected a value", pos_);
    }
    const char byte = text_[pos_];
    switch (byte) {
      case 'n':
        return parse_literal("null", JsonValue::make_null());
      case 't':
        return parse_literal("true", JsonValue::make_bool(true));
      case 'f':
        return parse_literal("false", JsonValue::make_bool(false));
      case '"': {
        Result<std::string> decoded = parse_string();
        if (!decoded) {
          return decoded.error();
        }
        return JsonValue::make_string(std::move(decoded).value());
      }
      case '[':
      case '{': {
        if (depth >= limits::kMaxJsonDepth) {
          Error error = limit_exceeded("JSON nesting exceeds the maximum depth", pos_);
          (void)error.with("limit", std::to_string(limits::kMaxJsonDepth));
          return error;
        }
        if (byte == '[') {
          return parse_array(depth);
        }
        return parse_object(depth);
      }
      default:
        break;
    }
    if (is_digit(byte)) {
      return parse_number();
    }
    return malformed("expected a value", pos_);
  }

  [[nodiscard]] Result<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
    if (!matches(literal)) {
      return malformed(std::string("expected '") + std::string(literal) + "'", pos_);
    }
    pos_ += literal.size();
    return value;
  }

  [[nodiscard]] Result<JsonValue> parse_number() {
    const std::size_t start = pos_;
    std::uint64_t value = 0;
    while (!at_end() && is_digit(text_[pos_])) {
      const std::uint64_t digit = static_cast<std::uint64_t>(text_[pos_] - '0');
      // Overflow safe form of "value * 10 + digit > 2^64 - 1". The subtraction
      // cannot wrap because digit is at most 9.
      if (value > (kMaxUnsignedValue - digit) / 10u) {
        return malformed("number exceeds the unsigned 64 bit range", pos_);
      }
      value = value * 10u + digit;
      ++pos_;
    }
    const std::size_t digits = pos_ - start;
    if (digits == 0) {
      return malformed("expected a value", start);
    }
    if (digits > 1 && text_[start] == '0') {
      return malformed("number has a leading zero", start);
    }
    return JsonValue::make_number(value);
  }

  [[nodiscard]] Result<JsonValue> parse_array(std::size_t depth) {
    ++pos_;  // consume '['
    JsonValue array = JsonValue::make_array();
    skip_whitespace();
    if (!at_end() && text_[pos_] == ']') {
      ++pos_;
      return array;
    }
    for (;;) {
      Result<JsonValue> element = parse_value(depth + 1);
      if (!element) {
        return element.error();
      }
      array.push_back(std::move(element).value());
      skip_whitespace();
      if (at_end()) {
        return malformed("unterminated array, expected ',' or ']'", pos_);
      }
      const char byte = text_[pos_];
      if (byte == ',') {
        ++pos_;
        skip_whitespace();
        if (!at_end() && text_[pos_] == ']') {
          return malformed("trailing comma in array", pos_);
        }
        continue;
      }
      if (byte == ']') {
        ++pos_;
        return array;
      }
      return malformed("expected ',' or ']' after an array element", pos_);
    }
  }

  [[nodiscard]] Result<JsonValue> parse_object(std::size_t depth) {
    ++pos_;  // consume '{'
    std::vector<std::pair<std::string, JsonValue>> members;
    std::vector<std::size_t> key_offsets;
    skip_whitespace();
    if (!at_end() && text_[pos_] == '}') {
      ++pos_;
      return JsonValue::make_object();
    }
    for (;;) {
      skip_whitespace();
      if (at_end()) {
        return malformed("unterminated object, expected a member name", pos_);
      }
      if (text_[pos_] != '"') {
        return malformed("object member name must be a string", pos_);
      }
      const std::size_t key_offset = pos_;
      Result<std::string> key = parse_string();
      if (!key) {
        return key.error();
      }
      skip_whitespace();
      if (at_end() || text_[pos_] != ':') {
        return malformed("expected ':' after the object member name", pos_);
      }
      ++pos_;
      Result<JsonValue> value = parse_value(depth + 1);
      if (!value) {
        return value.error();
      }
      members.emplace_back(std::move(key).value(), std::move(value).value());
      key_offsets.push_back(key_offset);
      skip_whitespace();
      if (at_end()) {
        return malformed("unterminated object, expected ',' or '}'", pos_);
      }
      const char byte = text_[pos_];
      if (byte == ',') {
        ++pos_;
        skip_whitespace();
        if (!at_end() && text_[pos_] == '}') {
          return malformed("trailing comma in object", pos_);
        }
        continue;
      }
      if (byte == '}') {
        ++pos_;
        break;
      }
      return malformed("expected ',' or '}' after an object member", pos_);
    }
    Result<void> unique = reject_duplicate_keys(members, key_offsets);
    if (!unique) {
      return unique.error();
    }
    JsonValue object = JsonValue::make_object();
    for (std::pair<std::string, JsonValue>& member : members) {
      object.emplace(std::move(member.first), std::move(member.second));
    }
    return object;
  }

  /// Duplicate member names are rejected in O(n log n): a document may legally
  /// carry an object with very many members, and a pairwise scan would let an
  /// adversary dictate quadratic work. The reported offset is the second
  /// occurrence in document order, which is the one that would have been lost.
  [[nodiscard]] static Result<void> reject_duplicate_keys(
      const std::vector<std::pair<std::string, JsonValue>>& members,
      const std::vector<std::size_t>& key_offsets) {
    if (members.size() < 2) {
      return ok();
    }
    std::vector<std::size_t> order(members.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
      order[index] = index;
    }
    std::sort(order.begin(), order.end(),
              [&members](std::size_t left, std::size_t right) {
                const int compared = members[left].first.compare(members[right].first);
                if (compared != 0) {
                  return compared < 0;
                }
                return left < right;
              });
    for (std::size_t index = 1; index < order.size(); ++index) {
      const std::size_t previous = order[index - 1];
      const std::size_t current = order[index];
      if (members[previous].first == members[current].first) {
        return malformed("duplicate object member name", key_offsets[current]);
      }
    }
    return ok();
  }

  [[nodiscard]] Result<std::string> parse_string() {
    const std::size_t open_offset = pos_;
    ++pos_;  // consume the opening quote
    std::string out;
    for (;;) {
      if (at_end()) {
        return malformed("unterminated string", open_offset);
      }
      const unsigned char byte = static_cast<unsigned char>(text_[pos_]);
      if (byte == static_cast<unsigned char>('"')) {
        ++pos_;
        break;
      }
      // Control characters must never appear raw. DEL is accepted here because
      // the writer always escapes it, so accepting it keeps the value round trip
      // closed in both directions.
      if (byte < 0x20u) {
        return malformed("raw control character in string, it must use an escape form", pos_);
      }
      if (byte != static_cast<unsigned char>('\\')) {
        out.push_back(static_cast<char>(byte));
        ++pos_;
        continue;
      }
      const std::size_t escape_offset = pos_;
      ++pos_;
      if (at_end()) {
        return malformed("unterminated string escape", escape_offset);
      }
      switch (text_[pos_]) {
        case '"':
          out.push_back('"');
          ++pos_;
          break;
        case '\\':
          out.push_back('\\');
          ++pos_;
          break;
        case '/':
          out.push_back('/');
          ++pos_;
          break;
        case 'b':
          out.push_back('\b');
          ++pos_;
          break;
        case 'f':
          out.push_back('\f');
          ++pos_;
          break;
        case 'n':
          out.push_back('\n');
          ++pos_;
          break;
        case 'r':
          out.push_back('\r');
          ++pos_;
          break;
        case 't':
          out.push_back('\t');
          ++pos_;
          break;
        case 'u': {
          Result<std::uint32_t> code_point = parse_unicode_escape();
          if (!code_point) {
            return code_point.error();
          }
          append_utf8(out, code_point.value());
          break;
        }
        default:
          return malformed("unsupported string escape", escape_offset);
      }
    }
    // Raw bytes are validated once, on the decoded string, so a malformed
    // sequence is rejected where it is found rather than carried forward.
    if (!is_valid_utf8(out)) {
      return malformed("string is not valid UTF-8", open_offset);
    }
    return out;
  }

  /// Decodes the body of a \u escape, combining a surrogate pair. The cursor
  /// points at the 'u' on entry and just past the last consumed digit on exit.
  [[nodiscard]] Result<std::uint32_t> parse_unicode_escape() {
    const std::size_t escape_offset = pos_ - 1;  // the backslash
    Result<std::uint32_t> first = parse_hex4();
    if (!first) {
      return first.error();
    }
    std::uint32_t code_point = first.value();
    if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
      if (!matches("\\u")) {
        return malformed("high surrogate escape is not followed by a low surrogate escape", escape_offset);
      }
      ++pos_;  // consume the backslash; parse_hex4 consumes the 'u'
      Result<std::uint32_t> second = parse_hex4();
      if (!second) {
        return second.error();
      }
      const std::uint32_t low = second.value();
      if (low < 0xDC00u || low > 0xDFFFu) {
        return malformed("high surrogate escape is not followed by a low surrogate escape", escape_offset);
      }
      code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
    } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
      return malformed("lone low surrogate escape", escape_offset);
    }
    return code_point;
  }

  [[nodiscard]] Result<std::uint32_t> parse_hex4() {
    const std::size_t escape_offset = pos_ - 1;  // the backslash
    ++pos_;                                      // consume 'u'
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      if (at_end()) {
        return malformed("truncated \\u escape", escape_offset);
      }
      const char byte = text_[pos_];
      if (!is_hex_digit(byte)) {
        return malformed("\\u escape needs exactly four hexadecimal digits", pos_);
      }
      value = (value << 4) | hex_digit_value(byte);
      ++pos_;
    }
    return value;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

/// Plain decimal digits, no locale, no sign, no padding.
void append_decimal(std::string& out, std::uint64_t value) {
  char digits[20];  // 18446744073709551615 is twenty digits
  std::size_t length = 0;
  do {
    digits[length] = static_cast<char>('0' + (value % 10u));
    ++length;
    value /= 10u;
  } while (value != 0);
  while (length != 0) {
    --length;
    out.push_back(digits[length]);
  }
}

void append_indent(std::string& out, std::size_t level) { out.append(level * 2, ' '); }

void append_escaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20u || byte == 0x7Fu) {
          out.append("\\u00");
          out.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
          out.push_back(kHexDigits[byte & 0x0Fu]);
        } else {
          // Valid UTF-8 passes through unchanged, byte for byte.
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

void write_value(const JsonValue& value, bool pretty, std::size_t level, std::string& out) {
  switch (value.type()) {
    case JsonValue::Type::Null:
      out.append("null");
      return;
    case JsonValue::Type::Bool:
      out.append(value.as_bool() ? "true" : "false");
      return;
    case JsonValue::Type::Number:
      append_decimal(out, value.as_number());
      return;
    case JsonValue::Type::String:
      append_escaped(out, value.as_string());
      return;
    case JsonValue::Type::Array: {
      const std::vector<JsonValue>& elements = value.items();
      if (elements.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      for (std::size_t index = 0; index < elements.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        if (pretty) {
          out.push_back('\n');
          append_indent(out, level + 1);
        }
        write_value(elements[index], pretty, level + 1, out);
      }
      if (pretty) {
        out.push_back('\n');
        append_indent(out, level);
      }
      out.push_back(']');
      return;
    }
    case JsonValue::Type::Object: {
      const std::vector<std::pair<std::string, JsonValue>>& members = value.fields();
      if (members.empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      for (std::size_t index = 0; index < members.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        if (pretty) {
          out.push_back('\n');
          append_indent(out, level + 1);
        }
        append_escaped(out, members[index].first);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        write_value(members[index].second, pretty, level + 1, out);
      }
      if (pretty) {
        out.push_back('\n');
        append_indent(out, level);
      }
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

Result<JsonValue> parse_json(std::string_view text) {
  if (text.size() > limits::kMaxExportBytes) {
    Error error = limit_exceeded("document exceeds the maximum accepted size", 0);
    (void)error.with("size", std::to_string(text.size()));
    (void)error.with("limit", std::to_string(limits::kMaxExportBytes));
    return error;
  }
  Parser parser(text);
  return parser.parse_document();
}

std::string write_json(const JsonValue& value, bool pretty) {
  std::string out;
  write_value(value, pretty, 0, out);
  return out;
}

std::string escape_json_string(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  append_escaped(out, text);
  return out;
}

}  // namespace hardware_lifecycle::detail
