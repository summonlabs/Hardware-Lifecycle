#pragma once

// Hardware Lifecycle - internal canonical binary codec.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every durable byte this project produces goes through this writer and reader.
// The layout rules are fixed and endianness independent:
//   u8/u16/u32/u64   big endian fixed width integers
//   raw(n)           n bytes verbatim
//   text             u32 byte length followed by the bytes, no terminator
//   digest           32 raw bytes
//   optional         one presence byte (0 absent, 1 present) then the payload
//
// A reader is sticky: the first failed read latches the error and every later
// read returns a default. Callers must therefore check ok() before using
// anything they decoded, and every decode path in this project does.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle::detail {

class Writer {
 public:
  Writer() = default;

  void u8(std::uint8_t value) { data_.push_back(value); }

  void u16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    data_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  }

  void u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void raw(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    data_.insert(data_.end(), bytes, bytes + size);
  }

  void raw(std::span<const std::uint8_t> data) { data_.insert(data_.end(), data.begin(), data.end()); }

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value.data(), value.size());
  }

  void digest(const Digest& value) { raw(value.bytes().data(), Digest::kSize); }

  void optional_u64(bool present, std::uint64_t value) {
    u8(present ? 1u : 0u);
    if (present) {
      u64(value);
    }
  }

  void optional_text(bool present, std::string_view value) {
    u8(present ? 1u : 0u);
    if (present) {
      text(value);
    }
  }

  void optional_digest(bool present, const Digest& value) {
    u8(present ? 1u : 0u);
    if (present) {
      digest(value);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(data_); }

 private:
  std::vector<std::uint8_t> data_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data,
                  ErrorCode truncation_code = ErrorCode::MalformedRequest) noexcept
      : data_(data), truncation_code_(truncation_code) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode truncation_code() const noexcept { return truncation_code_; }

  /// Returns false so that callers can write "return reader.fail(...)".
  bool fail(ErrorCode code, std::string message) {
    if (ok_) {
      ok_ = false;
      error_ = Error::make(code, std::move(message)).with("offset", std::to_string(offset_));
    }
    return false;
  }

  std::uint8_t u8() {
    if (remaining() < 1) {
      fail(truncation_code_, "unexpected end of encoded data");
      return 0;
    }
    return data_[offset_++];
  }

  std::uint16_t u16() {
    if (remaining() < 2) {
      fail(truncation_code_, "unexpected end of encoded data");
      return 0;
    }
    std::uint16_t value = 0;
    for (int index = 0; index < 2; ++index) {
      value = static_cast<std::uint16_t>((value << 8) | data_[offset_++]);
    }
    return value;
  }

  std::uint32_t u32() {
    if (remaining() < 4) {
      fail(truncation_code_, "unexpected end of encoded data");
      return 0;
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      value = (value << 8) | data_[offset_++];
    }
    return value;
  }

  std::uint64_t u64() {
    if (remaining() < 8) {
      fail(truncation_code_, "unexpected end of encoded data");
      return 0;
    }
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
      value = (value << 8) | data_[offset_++];
    }
    return value;
  }

  std::span<const std::uint8_t> raw(std::size_t size) {
    if (remaining() < size) {
      fail(truncation_code_, "unexpected end of encoded data");
      return {};
    }
    const std::span<const std::uint8_t> slice = data_.subspan(offset_, size);
    offset_ += size;
    return slice;
  }

  std::string text(std::size_t max_bytes = limits::kMaxTextBytes) {
    const std::uint32_t length = u32();
    if (!ok_) {
      return {};
    }
    if (length > max_bytes) {
      fail(ErrorCode::LimitExceeded, "encoded text exceeds the accepted length");
      return {};
    }
    const std::span<const std::uint8_t> slice = raw(length);
    if (!ok_) {
      return {};
    }
    return std::string(reinterpret_cast<const char*>(slice.data()), slice.size());
  }

  Digest digest() {
    const std::span<const std::uint8_t> slice = raw(Digest::kSize);
    if (!ok_) {
      return Digest();
    }
    std::array<std::uint8_t, Digest::kSize> bytes{};
    std::memcpy(bytes.data(), slice.data(), Digest::kSize);
    return Digest::from_bytes(bytes);
  }

  bool presence() {
    const std::uint8_t flag = u8();
    if (!ok_) {
      return false;
    }
    if (flag > 1u) {
      fail(ErrorCode::MalformedRequest, "invalid optional presence byte");
      return false;
    }
    return flag == 1u;
  }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t offset_ = 0;
  bool ok_ = true;
  ErrorCode truncation_code_ = ErrorCode::MalformedRequest;
  Error error_;
};

}  // namespace hardware_lifecycle::detail
