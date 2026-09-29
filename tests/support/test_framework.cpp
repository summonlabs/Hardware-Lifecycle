// Hardware Lifecycle - the test harness implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>

namespace hl_test {
namespace {

[[nodiscard]] std::string unsigned_text(std::uint64_t value) {
  if (value == 0) {
    return "0";
  }
  std::string digits;
  while (value != 0) {
    digits.push_back(static_cast<char>('0' + static_cast<int>(value % 10u)));
    value /= 10u;
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

[[nodiscard]] std::string signed_text(long long value) {
  if (value < 0) {
    return "-" + unsigned_text(0ull - static_cast<std::uint64_t>(value));
  }
  return unsigned_text(static_cast<std::uint64_t>(value));
}

}  // namespace

std::string to_text(bool value) { return value ? "true" : "false"; }
std::string to_text(char value) { return std::string(1, value); }
std::string to_text(const std::string& value) { return "\"" + value + "\""; }
std::string to_text(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string to_text(const char* value) { return value == nullptr ? "<null>" : to_text(std::string_view(value)); }
std::string to_text(int value) { return signed_text(value); }
std::string to_text(unsigned int value) { return unsigned_text(value); }
std::string to_text(long value) { return signed_text(value); }
std::string to_text(unsigned long value) { return unsigned_text(value); }
std::string to_text(long long value) { return signed_text(value); }
std::string to_text(unsigned long long value) { return unsigned_text(value); }
std::string to_text(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6f", value);
  return buffer;
}

bool Context::check(bool ok, const char* expression, const char* file, int line) {
  if (ok) {
    return true;
  }
  fail(file, line, std::string("check failed: ") + expression);
  return false;
}

bool Context::require(bool ok, const char* expression, const char* file, int line) {
  if (ok) {
    return true;
  }
  fail(file, line, std::string("requirement failed: ") + expression);
  stopped_ = true;
  return false;
}

void Context::fail(const char* file, int line, std::string message) {
  ++failures_;
  std::cout << "    " << file << ":" << line << ": " << message << "\n";
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* suite, const char* name, Body body) {
  TestCase test_case;
  test_case.suite = suite;
  test_case.name = name;
  test_case.body = std::move(body);
  registry().push_back(std::move(test_case));
}

namespace {

std::string g_lock_child;
std::string g_crash_child;
std::string g_cli;

}  // namespace

const std::string& lock_child_path() { return g_lock_child; }
const std::string& crash_child_path() { return g_crash_child; }
const std::string& cli_path() { return g_cli; }

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--filter" && index + 1 < argc) {
      filter = argv[++index];
    } else if (argument == "--lock-child" && index + 1 < argc) {
      g_lock_child = argv[++index];
    } else if (argument == "--crash-child" && index + 1 < argc) {
      g_crash_child = argv[++index];
    } else if (argument == "--cli" && index + 1 < argc) {
      g_cli = argv[++index];
    }
  }

  std::vector<TestCase>& cases = registry();
  std::stable_sort(cases.begin(), cases.end(), [](const TestCase& left, const TestCase& right) {
    if (left.suite != right.suite) {
      return left.suite < right.suite;
    }
    return left.name < right.name;
  });

  if (list_only) {
    for (const TestCase& test_case : cases) {
      std::cout << test_case.suite << "." << test_case.name << "\n";
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed = 0;
  for (const TestCase& test_case : cases) {
    const std::string full = test_case.suite + "." + test_case.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    Context context;
    std::cout << "[ RUN  ] " << full << "\n";
    test_case.body(context);
    if (context.failures() == 0) {
      std::cout << "[ PASS ] " << full << "\n";
    } else {
      ++failed;
      std::cout << "[ FAIL ] " << full << " (" << context.failures() << " failures)\n";
    }
  }
  std::cout << "cases: " << executed << ", failures: " << failed << "\n";
  return failed == 0 ? 0 : 1;
}

TempDir::TempDir(std::string_view label) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::error_code error;
  std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    base = std::filesystem::current_path(error);
  }
  std::string name = "hl_test_";
  name += std::string(label);
  name += "_";
  name += unsigned_text(static_cast<std::uint64_t>(now));
  path_ = base / name;
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDir::child(std::string_view name) const { return path_ / std::string(name); }

}  // namespace hl_test

int main(int argc, char** argv) { return hl_test::run_all(argc, argv); }
