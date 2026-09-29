#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strict text primitives. Every parser here rejects rather than repairs: no
// trimming of unexpected input, no defaulting of empty input, no overflow
// wraparound and no replacement of invalid bytes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// True when the bytes form a well-formed UTF-8 sequence: no overlong forms, no
/// surrogate code points, no code points above U+10FFFF and no truncated tail.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_valid_utf8(std::string_view text) noexcept;

/// True when every byte is printable ASCII in [0x20, 0x7E].
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_printable_ascii(std::string_view text) noexcept;

/// Identifier shape used by every strong textual id: 1..limits::kMaxIdentifierBytes
/// bytes, first byte alphanumeric, remaining bytes alphanumeric or one of dot,
/// underscore, dash or colon, with no doubled dot and no trailing separator.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_valid_identifier(std::string_view text) noexcept;

/// Evidence source shape: a non-empty printable ASCII or valid UTF-8 string
/// without control characters, at most limits::kMaxTextBytes bytes.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_valid_evidence_source(std::string_view text) noexcept;

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view trim_ascii(std::string_view text) noexcept;

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_ascii_lower(std::string_view text);

[[nodiscard]] HARDWARE_LIFECYCLE_API bool ascii_iequals(std::string_view left, std::string_view right) noexcept;

/// Strict unsigned decimal parse: digits only, no sign, no leading plus, no
/// leading zeroes unless the value is exactly "0", no surrounding whitespace and
/// no overflow. Deterministic and locale independent.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::uint64_t> parse_u64_strict(std::string_view text);

/// Strict signed decimal parse with the same rules plus an optional leading
/// minus sign. A negative zero is rejected.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::int64_t> parse_i64_strict(std::string_view text);

/// Accepts exactly "true", "false", "0" or "1".
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<bool> parse_bool_strict(std::string_view text);

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_hex_lower(const std::uint8_t* data, std::size_t size);

/// Strict hex decode; rejects odd length, non-hex bytes and any decoded size
/// above the caller supplied maximum.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::vector<std::uint8_t>> from_hex(std::string_view text,
                                                                                std::size_t max_bytes);

[[nodiscard]] HARDWARE_LIFECYCLE_API std::vector<std::string_view> split_ascii(std::string_view text, char delimiter);

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string join(const std::vector<std::string>& parts, std::string_view separator);

}  // namespace hardware_lifecycle
