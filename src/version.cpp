// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The library version and the strict "major.minor.patch" parser. Durable format
// versions live in compatibility.hpp and are deliberately separate: a build may
// change without changing the on-disk layout.

#include "hardware_lifecycle/version.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {
namespace {

/// Canonical decimal spelling of the largest component, 2^32 - 1.
constexpr std::string_view kMaxComponentDecimal = "4294967295";

bool is_ascii_digit(char value) noexcept { return value >= '0' && value <= '9'; }

/// True when a run of decimal digits is numerically above maximum, supplied in
/// its canonical spelling. Leading zeroes are insignificant for the bound, so it
/// is decided before the component is judged for shape.
bool digits_above(std::string_view digits, std::string_view maximum) noexcept {
  std::size_t first_significant = 0;
  while (first_significant < digits.size() && digits[first_significant] == '0') {
    ++first_significant;
  }
  const std::string_view significant = digits.substr(first_significant);
  if (significant.empty()) {
    return false;
  }
  if (significant.size() != maximum.size()) {
    return significant.size() > maximum.size();
  }
  return significant > maximum;
}

/// Strict parse of one dot separated component: one to ten digits, no sign, no
/// leading zero unless the component is exactly "0" and a value at most 2^32-1.
/// Whether the component can be considered at all is decided before whether it
/// is well formed, so an out of range component reports LimitExceeded even when
/// it also carries, say, a leading zero.
Result<std::uint32_t> parse_component(std::string_view component, std::string_view whole) {
  if (component.empty()) {
    return Error::make(ErrorCode::MalformedRequest, "version component is empty")
        .with("version", std::string(whole));
  }
  for (const char byte : component) {
    if (!is_ascii_digit(byte)) {
      return Error::make(ErrorCode::MalformedRequest, "version component is not a decimal number")
          .with("version", std::string(whole));
    }
  }
  if (digits_above(component, kMaxComponentDecimal)) {
    return Error::make(ErrorCode::LimitExceeded, "version component is above 4294967295")
        .with("version", std::string(whole));
  }
  if (component.size() > 1 && component.front() == '0') {
    return Error::make(ErrorCode::MalformedRequest, "version component has a leading zero")
        .with("version", std::string(whole));
  }
  // The bound above is already enforced, so the accumulator cannot overflow and
  // the narrowing conversion to 32 bits is exact.
  std::uint64_t value = 0;
  for (const char byte : component) {
    value = value * 10u + static_cast<std::uint64_t>(byte - '0');
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace

std::string_view library_version_string() noexcept {
  // Derived from the kLibraryVersion* constants so the rendering can never drift
  // from the version the library reports to its callers.
  static const std::string rendered = to_string(library_version());
  return rendered;
}

std::string to_string(Version version) {
  std::string rendered = std::to_string(version.major);
  rendered.push_back('.');
  rendered.append(std::to_string(version.minor));
  rendered.push_back('.');
  rendered.append(std::to_string(version.patch));
  return rendered;
}

Result<Version> parse_version(std::string_view text) {
  const std::size_t first_dot = text.find('.');
  if (first_dot == std::string_view::npos) {
    return Error::make(ErrorCode::MalformedRequest, "version must be major.minor.patch")
        .with("version", std::string(text));
  }
  const std::size_t second_dot = text.find('.', first_dot + 1);
  if (second_dot == std::string_view::npos) {
    return Error::make(ErrorCode::MalformedRequest, "version must be major.minor.patch")
        .with("version", std::string(text));
  }
  if (text.find('.', second_dot + 1) != std::string_view::npos) {
    return Error::make(ErrorCode::MalformedRequest, "version must have exactly three components")
        .with("version", std::string(text));
  }

  const Result<std::uint32_t> major = parse_component(text.substr(0, first_dot), text);
  if (!major.has_value()) {
    return major.error();
  }
  const Result<std::uint32_t> minor = parse_component(text.substr(first_dot + 1, second_dot - first_dot - 1), text);
  if (!minor.has_value()) {
    return minor.error();
  }
  const Result<std::uint32_t> patch = parse_component(text.substr(second_dot + 1), text);
  if (!patch.has_value()) {
    return patch.error();
  }
  return Version{major.value(), minor.value(), patch.value()};
}

int compare(Version left, Version right) noexcept {
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  return 0;
}

}  // namespace hardware_lifecycle
