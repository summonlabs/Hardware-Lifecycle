// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strict text primitives. Every parser here rejects rather than repairs: no
// trimming of unexpected input, no defaulting of empty input, no overflow
// wraparound and no replacement of invalid bytes.

#include "hardware_lifecycle/text.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {
namespace {

constexpr bool is_ascii_digit(char value) noexcept { return value >= '0' && value <= '9'; }

constexpr bool is_ascii_alpha(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

constexpr bool is_ascii_alphanumeric(char value) noexcept {
  return is_ascii_digit(value) || is_ascii_alpha(value);
}

constexpr bool is_identifier_separator(char value) noexcept {
  return value == '.' || value == '_' || value == '-' || value == ':';
}

constexpr bool is_trimmed_byte(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

/// Decodes one UTF-8 scalar value starting at offset. Returns false for every
/// ill-formed sequence: invalid lead byte, missing or malformed continuation,
/// overlong form, surrogate code point, code point above U+10FFFF and truncated
/// tail. On success length receives the encoded length in bytes.
bool decode_utf8(std::string_view text, std::size_t offset, std::uint32_t& code_point, std::size_t& length) noexcept {
  code_point = 0;
  length = 0;
  if (offset >= text.size()) {
    return false;
  }
  const std::uint32_t lead = static_cast<unsigned char>(text[offset]);
  std::size_t continuations = 0;
  std::uint32_t minimum = 0;
  if (lead <= 0x7Fu) {
    code_point = lead;
    length = 1;
    return true;
  }
  if (lead >= 0xC2u && lead <= 0xDFu) {
    continuations = 1;
    code_point = lead & 0x1Fu;
    minimum = 0x80u;
  } else if (lead >= 0xE0u && lead <= 0xEFu) {
    continuations = 2;
    code_point = lead & 0x0Fu;
    minimum = 0x800u;
  } else if (lead >= 0xF0u && lead <= 0xF4u) {
    continuations = 3;
    code_point = lead & 0x07u;
    minimum = 0x10000u;
  } else {
    // 0x80..0xC1 are continuations or always overlong; 0xF5..0xFF can never
    // begin a code point at or below U+10FFFF.
    return false;
  }
  if (text.size() - offset < continuations + 1) {
    return false;
  }
  for (std::size_t index = 1; index <= continuations; ++index) {
    const std::uint32_t byte = static_cast<unsigned char>(text[offset + index]);
    if ((byte & 0xC0u) != 0x80u) {
      return false;
    }
    code_point = (code_point << 6) | (byte & 0x3Fu);
  }
  if (code_point < minimum) {
    return false;
  }
  if (code_point > 0x10FFFFu) {
    return false;
  }
  if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
    return false;
  }
  length = continuations + 1;
  return true;
}

/// Numeric value of one hexadecimal digit, or -1 when the byte is not hex.
constexpr int hex_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

/// Canonical decimal spellings of the 64 bit signed and unsigned bounds.
constexpr std::string_view kMaxU64Decimal = "18446744073709551615";
constexpr std::string_view kMaxI64Decimal = "9223372036854775807";
constexpr std::string_view kMinI64MagnitudeDecimal = "9223372036854775808";

/// True when a run of decimal digits is numerically above maximum, which is
/// supplied in its canonical spelling without leading zeroes. Leading zeroes in
/// the input are insignificant for the bound, so the bound can be decided
/// before the literal is judged for shape - that is what makes an out of range
/// literal report LimitExceeded rather than MalformedRequest.
bool digits_above(std::string_view digits, std::string_view maximum) noexcept {
  std::size_t first_significant = 0;
  while (first_significant < digits.size() && digits[first_significant] == '0') {
    ++first_significant;
  }
  const std::string_view significant = digits.substr(first_significant);
  if (significant.empty()) {
    return false;  // every digit was zero, so the literal is zero
  }
  if (significant.size() != maximum.size()) {
    return significant.size() > maximum.size();
  }
  // Equal lengths and no leading zeroes: text order is numeric order.
  return significant > maximum;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t code_point = 0;
    std::size_t length = 0;
    if (!decode_utf8(text, offset, code_point, length)) {
      return false;
    }
    offset += length;
  }
  return true;
}

bool is_printable_ascii(std::string_view text) noexcept {
  for (const char byte : text) {
    if (byte < 0x20 || byte > 0x7E) {
      return false;
    }
  }
  return true;
}

bool is_valid_identifier(std::string_view text) noexcept {
  const std::size_t size = text.size();
  if (size == 0 || size > limits::kMaxIdentifierBytes) {
    return false;
  }
  if (!is_ascii_alphanumeric(text.front())) {
    return false;
  }
  bool previous_was_dot = false;
  for (std::size_t index = 1; index < size; ++index) {
    const char byte = text[index];
    if (is_ascii_alphanumeric(byte)) {
      previous_was_dot = false;
      continue;
    }
    if (!is_identifier_separator(byte)) {
      // Separators are the only non-alphanumeric bytes accepted, which rejects
      // every non-ASCII byte and every control byte as a side effect.
      return false;
    }
    if (byte == '.' && previous_was_dot) {
      return false;
    }
    previous_was_dot = byte == '.';
  }
  // A trailing separator refers to a component that does not exist.
  return !is_identifier_separator(text.back());
}

bool is_valid_evidence_source(std::string_view text) noexcept {
  if (text.empty() || text.size() > limits::kMaxTextBytes) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t code_point = 0;
    std::size_t length = 0;
    if (!decode_utf8(text, offset, code_point, length)) {
      return false;
    }
    // C0 controls, DEL and C1 controls cannot be rendered, compared or
    // round-tripped unambiguously, so they are never evidence.
    if (code_point < 0x20u || code_point == 0x7Fu || (code_point >= 0x80u && code_point <= 0x9Fu)) {
      return false;
    }
    offset += length;
  }
  return true;
}

std::string_view trim_ascii(std::string_view text) noexcept {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_trimmed_byte(text[begin])) {
    ++begin;
  }
  while (end > begin && is_trimmed_byte(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string to_ascii_lower(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char byte : text) {
    lowered.push_back((byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte - 'A' + 'a') : byte);
  }
  return lowered;
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const char lhs = (left[index] >= 'A' && left[index] <= 'Z') ? static_cast<char>(left[index] - 'A' + 'a') : left[index];
    const char rhs = (right[index] >= 'A' && right[index] <= 'Z') ? static_cast<char>(right[index] - 'A' + 'a') : right[index];
    if (lhs != rhs) {
      return false;
    }
  }
  return true;
}

Result<std::uint64_t> parse_u64_strict(std::string_view text) {
  if (text.empty()) {
    return Error::make(ErrorCode::MalformedRequest, "empty decimal literal").with("value", std::string(text));
  }
  for (const char byte : text) {
    if (!is_ascii_digit(byte)) {
      return Error::make(ErrorCode::MalformedRequest, "decimal literal has a non digit byte")
          .with("value", std::string(text));
    }
  }
  // Bounds before shape: a literal that cannot be considered at all is rejected
  // as out of range even when it is also badly shaped.
  if (digits_above(text, kMaxU64Decimal)) {
    return Error::make(ErrorCode::LimitExceeded, "decimal literal is above 18446744073709551615")
        .with("value", std::string(text));
  }
  if (text.size() > 1 && text.front() == '0') {
    return Error::make(ErrorCode::MalformedRequest, "decimal literal has a leading zero")
        .with("value", std::string(text));
  }
  // The bound above is already enforced, so the accumulator cannot wrap.
  std::uint64_t value = 0;
  for (const char byte : text) {
    value = value * 10u + static_cast<std::uint64_t>(byte - '0');
  }
  return value;
}

Result<std::int64_t> parse_i64_strict(std::string_view text) {
  if (text.empty()) {
    return Error::make(ErrorCode::MalformedRequest, "empty decimal literal").with("value", std::string(text));
  }
  const bool negative = text.front() == '-';
  if (negative) {
    text.remove_prefix(1);
    if (text.empty()) {
      return Error::make(ErrorCode::MalformedRequest, "decimal literal has a sign without digits")
          .with("value", "-");
    }
  }
  for (const char byte : text) {
    if (!is_ascii_digit(byte)) {
      return Error::make(ErrorCode::MalformedRequest, "decimal literal has a non digit byte")
          .with("value", std::string(text));
    }
  }
  // Bounds before shape, with the magnitude bound selected by the sign.
  if (digits_above(text, negative ? kMinI64MagnitudeDecimal : kMaxI64Decimal)) {
    return Error::make(ErrorCode::LimitExceeded, "decimal literal is outside the 64 bit signed range")
        .with("value", std::string(text));
  }
  if (text.size() > 1 && text.front() == '0') {
    return Error::make(ErrorCode::MalformedRequest, "decimal literal has a leading zero")
        .with("value", std::string(text));
  }
  if (negative && text == "0") {
    return Error::make(ErrorCode::MalformedRequest, "negative zero is not a number")
        .with("value", std::string(text));
  }
  // The magnitude is accumulated in an unsigned type: computing it in a signed
  // one would overflow for the most negative value, whose magnitude is exactly
  // 2^63 and has no signed representation.
  std::uint64_t magnitude = 0;
  for (const char byte : text) {
    magnitude = magnitude * 10u + static_cast<std::uint64_t>(byte - '0');
  }
  if (!negative) {
    return static_cast<std::int64_t>(magnitude);
  }
  // Two's complement negation in unsigned arithmetic, then a well defined
  // conversion: the most negative value stays exact.
  return static_cast<std::int64_t>(std::uint64_t{0} - magnitude);
}

Result<bool> parse_bool_strict(std::string_view text) {
  if (text == "true" || text == "1") {
    return true;
  }
  if (text == "false" || text == "0") {
    return false;
  }
  return Error::make(ErrorCode::MalformedRequest, "boolean must be exactly true, false, 0 or 1")
      .with("value", std::string(text));
}

std::string to_hex_lower(const std::uint8_t* data, std::size_t size) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string rendered;
  // The pointer and the size are the caller's contract: a null pointer is only
  // meaningful with a zero size and then renders as the empty string.
  if (data == nullptr || size == 0) {
    return rendered;
  }
  rendered.reserve(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    const std::uint8_t byte = data[index];
    rendered.push_back(kHexDigits[static_cast<std::size_t>(byte >> 4)]);
    rendered.push_back(kHexDigits[static_cast<std::size_t>(byte & 0x0Fu)]);
  }
  return rendered;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view text, std::size_t max_bytes) {
  const std::size_t decoded_size = text.size() / 2;
  // Bounds before shape: the byte budget follows from the length alone, so it is
  // decided before a single digit is interpreted.
  if (decoded_size > max_bytes) {
    return Error::make(ErrorCode::LimitExceeded, "hexadecimal text decodes above the byte limit")
        .with("hex", "decoded size " + std::to_string(decoded_size) + " exceeds maximum " +
                         std::to_string(max_bytes));
  }
  if ((text.size() % 2) != 0) {
    return Error::make(ErrorCode::MalformedRequest, "hexadecimal text has an odd number of digits")
        .with("hex", std::string(text));
  }
  for (const char byte : text) {
    if (hex_value(byte) < 0) {
      return Error::make(ErrorCode::MalformedRequest, "hexadecimal text has a non hexadecimal digit")
          .with("hex", std::string(text));
    }
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(decoded_size);
  for (std::size_t index = 0; index < decoded_size; ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
  }
  return bytes;
}

std::vector<std::string_view> split_ascii(std::string_view text, char delimiter) {
  // Delimiter semantics: the result always holds at least one piece, empty text
  // yields exactly one empty piece, and a trailing delimiter yields a trailing
  // empty piece. Splitting is therefore lossless and reversible with join().
  std::vector<std::string_view> pieces;
  std::size_t start = 0;
  for (;;) {
    const std::size_t found = text.find(delimiter, start);
    if (found == std::string_view::npos) {
      pieces.push_back(text.substr(start));
      return pieces;
    }
    pieces.push_back(text.substr(start, found - start));
    start = found + 1;
  }
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string joined;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      joined.append(separator);
    }
    joined.append(parts[index]);
  }
  return joined;
}

}  // namespace hardware_lifecycle
