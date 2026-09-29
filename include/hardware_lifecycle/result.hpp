#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

#include "hardware_lifecycle/errors.hpp"

namespace hardware_lifecycle {

/// Either a value or an Error. There is no third state: a Result that carries a
/// value carries no error, and a Result that carries an error produces no value.
/// This is what keeps "missing" from decaying into a default-constructed value.
template <class T>
class Result {
 public:
  using value_type = T;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
  [[nodiscard]] Error& error() & { return std::get<1>(storage_); }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

  template <class F>
  [[nodiscard]] auto map(F&& fn) const -> Result<std::invoke_result_t<F, const T&>> {
    using U = std::invoke_result_t<F, const T&>;
    if (!has_value()) {
      return Result<U>(std::get<1>(storage_));
    }
    return Result<U>(fn(std::get<0>(storage_)));
  }

  template <class F>
  [[nodiscard]] auto and_then(F&& fn) const -> std::invoke_result_t<F, const T&> {
    using R = std::invoke_result_t<F, const T&>;
    if (!has_value()) {
      return R(std::get<1>(storage_));
    }
    return fn(std::get<0>(storage_));
  }

  template <class F>
  [[nodiscard]] auto or_else(F&& fn) const -> Result<T> {
    if (has_value()) {
      return *this;
    }
    return fn(std::get<1>(storage_));
  }

 private:
  std::variant<T, Error> storage_;
};

/// Void specialisation: carries success or an Error, never a value.
template <>
class Result<void> {
 public:
  using value_type = void;

  Result() noexcept = default;
  Result(Error error) : error_(std::move(error)), failed_(true) {}

  [[nodiscard]] bool has_value() const noexcept { return !failed_; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const Error& error() const { return error_; }

  template <class F>
  [[nodiscard]] auto and_then(F&& fn) const -> std::invoke_result_t<F> {
    using R = std::invoke_result_t<F>;
    if (failed_) {
      return R(error_);
    }
    return fn();
  }

 private:
  Error error_;
  bool failed_ = false;
};

[[nodiscard]] inline Result<void> ok() noexcept { return Result<void>(); }

[[nodiscard]] inline Error fail(ErrorCode code, std::string message) {
  return Error::make(code, std::move(message));
}

}  // namespace hardware_lifecycle
