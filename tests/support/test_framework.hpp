#pragma once

// Hardware Lifecycle - the test harness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small harness: the project is dependency light, so the tests
// are too. It supports registration, filtering, listing, per case reporting and
// a non zero exit status when anything fails.
//
// A failing check records the source location, the expression and, where the
// harness knows how to render them, both operands. A failed requirement stops
// the case; a failed check continues, so one run reports every problem in a case.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hl_test {

/// Renders a value for failure messages. Overloads exist for the shapes the
/// suites compare most often; anything else prints as a placeholder rather than
/// pretending to describe the value.
[[nodiscard]] std::string to_text(bool value);
[[nodiscard]] std::string to_text(char value);
[[nodiscard]] std::string to_text(const std::string& value);
[[nodiscard]] std::string to_text(std::string_view value);
[[nodiscard]] std::string to_text(const char* value);
// Only fundamental types are overloaded. The fixed width aliases are aliases of
// these on every supported platform (int32_t is int and uint64_t is unsigned
// long long on MSVC), so overloading both would declare the same function twice.
[[nodiscard]] std::string to_text(int value);
[[nodiscard]] std::string to_text(unsigned int value);
[[nodiscard]] std::string to_text(long value);
[[nodiscard]] std::string to_text(unsigned long value);
[[nodiscard]] std::string to_text(long long value);
[[nodiscard]] std::string to_text(unsigned long long value);
[[nodiscard]] std::string to_text(double value);

template <class T>
[[nodiscard]] std::string to_text(const T&) {
  return "<value>";
}

template <class T>
[[nodiscard]] std::string to_text(const std::vector<T>& values) {
  std::string text = "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      text += ", ";
    }
    text += to_text(values[index]);
  }
  text += "]";
  return text;
}

/// One test case body.
using Body = std::function<void(class Context&)>;

struct TestCase {
  std::string suite;
  std::string name;
  Body body;
};

class Context {
 public:
  Context() = default;

  [[nodiscard]] bool check(bool ok, const char* expression, const char* file, int line);
  [[nodiscard]] bool require(bool ok, const char* expression, const char* file, int line);

  template <class T, class U>
  [[nodiscard]] bool check_eq(const T& left, const U& right, const char* left_expression,
                              const char* right_expression, const char* file, int line) {
    if (left == right) {
      return true;
    }
    fail(file, line, std::string("expected ") + left_expression + " == " + right_expression + ", saw " +
                          to_text(left) + " and " + to_text(right));
    return false;
  }

  template <class T, class U>
  [[nodiscard]] bool check_ne(const T& left, const U& right, const char* left_expression,
                              const char* right_expression, const char* file, int line) {
    if (!(left == right)) {
      return true;
    }
    fail(file, line, std::string("expected ") + left_expression + " != " + right_expression + ", both are " +
                          to_text(left));
    return false;
  }

  /// Asserts that a Result carries an error with exactly the expected code, and
  /// reports the actual code (and message) otherwise.
  template <class T>
  [[nodiscard]] bool check_error(const hardware_lifecycle::Result<T>& result,
                                 hardware_lifecycle::ErrorCode expected, const char* expression, const char* file,
                                 int line) {
    if (result.has_value()) {
      fail(file, line, std::string(expression) + " was expected to fail with " +
                            std::string(hardware_lifecycle::to_string(expected)) + " but succeeded");
      return false;
    }
    if (result.error().code != expected) {
      fail(file, line, std::string(expression) + " failed with " +
                            std::string(hardware_lifecycle::to_string(result.error().code)) + " (" +
                            result.error().message + "), expected " +
                            std::string(hardware_lifecycle::to_string(expected)));
      return false;
    }
    return true;
  }

  [[nodiscard]] bool check_error_code(hardware_lifecycle::ErrorCode actual,
                                      hardware_lifecycle::ErrorCode expected, const char* expression,
                                      const char* file, int line) {
    if (actual == expected) {
      return true;
    }
    fail(file, line, std::string(expression) + " reported " + std::string(hardware_lifecycle::to_string(actual)) +
                          ", expected " + std::string(hardware_lifecycle::to_string(expected)));
    return false;
  }

  void fail(const char* file, int line, std::string message);
  [[nodiscard]] std::size_t failures() const noexcept { return failures_; }
  [[nodiscard]] bool stopped() const noexcept { return stopped_; }

 private:
  std::size_t failures_ = 0;
  bool stopped_ = false;
};

/// The single process wide registry.
[[nodiscard]] std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, Body body);
};

/// Runs every registered case whose "suite.name" contains the filter.
/// Args: --filter <substring> | --list | --verbose.
[[nodiscard]] int run_all(int argc, char** argv);

/// Paths supplied on the command line by the test runner:
///   --lock-child <path>   the writer lock proof harness
///   --crash-child <path>  the crash proof harness
///   --cli <path>          the command line tool
///
/// They are arguments rather than compile time constants on purpose: a Windows
/// path inside a macro would be read by the compiler as a C++ escape sequence
/// (C:\Users contains \U), and the build system quotes arguments correctly.
/// Empty means the runner did not supply them, which a suite must treat as a
/// failure rather than as a reason to skip its proofs.
[[nodiscard]] const std::string& lock_child_path();
[[nodiscard]] const std::string& crash_child_path();
[[nodiscard]] const std::string& cli_path();

/// A unique directory under the system temporary directory, removed when this
/// object dies. Tests that need real files use it and leave nothing behind.
class TempDir {
 public:
  explicit TempDir(std::string_view label);
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path child(std::string_view name) const;

 private:
  std::filesystem::path path_;
};

}  // namespace hl_test

#define HL_TEST(suite_name, case_name)                                                                    \
  static void hl_test_body_##suite_name##_##case_name(::hl_test::Context& hl_ctx);                        \
  namespace {                                                                                             \
  const ::hl_test::Registrar hl_test_registrar_##suite_name##_##case_name(                                \
      #suite_name, #case_name, &hl_test_body_##suite_name##_##case_name);                                 \
  }                                                                                                       \
  static void hl_test_body_##suite_name##_##case_name([[maybe_unused]] ::hl_test::Context& hl_ctx)

#define HL_CHECK(expression) \
  (void)hl_ctx.check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define HL_REQUIRE(expression)                                                       \
  do {                                                                               \
    if (!hl_ctx.require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)) { \
      return;                                                                        \
    }                                                                                \
  } while (false)

#define HL_CHECK_EQ(left, right) \
  (void)hl_ctx.check_eq((left), (right), #left, #right, __FILE__, __LINE__)

#define HL_CHECK_NE(left, right) \
  (void)hl_ctx.check_ne((left), (right), #left, #right, __FILE__, __LINE__)

#define HL_CHECK_MSG(expression, message) \
  (void)hl_ctx.check(static_cast<bool>(expression), message, __FILE__, __LINE__)

#define HL_CHECK_ERROR(expression, code) \
  (void)hl_ctx.check_error((expression), (code), #expression, __FILE__, __LINE__)

#define HL_FAIL(message) hl_ctx.fail(__FILE__, __LINE__, (message))
