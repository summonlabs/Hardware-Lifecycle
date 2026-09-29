#pragma once

// Hardware Lifecycle - internal JSON codec.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Not installed. The exported document format is JSON, so this parser is on an
// adversarial boundary: it is strict, bounded and deterministic.
//
// Restrictions that make the format unambiguous:
//  - numbers are unsigned 64 bit integers only; no sign, no fraction, no
//    exponent, no leading zero and no overflow,
//  - duplicate object keys are rejected,
//  - trailing content after the top level value is rejected,
//  - nesting deeper than limits::kMaxJsonDepth is rejected,
//  - strings must be valid UTF-8; lone surrogates and control characters must
//    use the escape forms the writer emits.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle::detail {

class HARDWARE_LIFECYCLE_API JsonValue {
 public:
  enum class Type : std::uint8_t { Null = 0, Bool, Number, String, Array, Object };

  JsonValue() noexcept = default;

  [[nodiscard]] static JsonValue make_null() noexcept;
  [[nodiscard]] static JsonValue make_bool(bool value) noexcept;
  [[nodiscard]] static JsonValue make_number(std::uint64_t value) noexcept;
  [[nodiscard]] static JsonValue make_string(std::string value);
  [[nodiscard]] static JsonValue make_array();
  [[nodiscard]] static JsonValue make_object();

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

  [[nodiscard]] bool as_bool() const noexcept { return boolean_; }
  [[nodiscard]] std::uint64_t as_number() const noexcept { return number_; }
  [[nodiscard]] const std::string& as_string() const noexcept { return string_; }

  [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return items_; }
  [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& fields() const noexcept { return fields_; }

  /// Looks up an object member. Returns null when absent or when this is not an
  /// object; callers must distinguish absence from a null value explicitly.
  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

  /// Array append.
  void push_back(JsonValue value);

  /// Object member append. The writer uses insertion order; a repeated key is a
  /// programming error and replaces nothing, it appends, so the parser is the
  /// only place duplicates can appear and it rejects them.
  void emplace(std::string key, JsonValue value);

 private:
  Type type_ = Type::Null;
  bool boolean_ = false;
  std::uint64_t number_ = 0;
  std::string string_;
  std::vector<JsonValue> items_;
  std::vector<std::pair<std::string, JsonValue>> fields_;
};

/// Strict parse. Never throws, never repairs, never guesses.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<JsonValue> parse_json(std::string_view text);

/// Canonical rendering: fixed indentation of two spaces when pretty, member
/// order as inserted, escaped control characters, no trailing whitespace.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string write_json(const JsonValue& value, bool pretty);

/// Escapes one string as a JSON string literal including both quotes.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string escape_json_string(std::string_view text);

}  // namespace hardware_lifecycle::detail
