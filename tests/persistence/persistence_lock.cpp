// Hardware Lifecycle - single writer exclusion proofs with real processes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The claim is that a store has exactly one writer, that the exclusion is an
// operating system lock rather than an in-process flag, and that the operating
// system releases it when a writer dies without running any cleanup. All three
// are proven here with real processes.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "process.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;
using hl_fixture::Driver;

[[nodiscard]] OpenOptions options_for(const std::filesystem::path& root, bool read_only = false) {
  OpenOptions options;
  options.root = root;
  options.read_only = read_only;
  return options;
}

/// Runs the lock child until it reports that it holds the lock.
[[nodiscard]] bool start_lock_holder(const std::filesystem::path& store, hl_test::ChildProcess& child,
                                     std::uint64_t& pid) {
  std::string error;
  if (!child.start(hl_test::lock_child_path(), {"--store", store.string()}, error)) {
    return false;
  }
  std::string line;
  if (!child.read_line(line)) {
    return false;
  }
  const std::string prefix = "ready ";
  if (line.rfind(prefix, 0) != 0) {
    return false;
  }
  pid = std::stoull(line.substr(prefix.size()));
  return pid != 0;
}

/// The multiprocess proofs are not skippable: a runner that forgets the path
/// must see a failure, not a quiet pass.
[[nodiscard]] bool harness_available() { return !hl_test::lock_child_path().empty(); }

}  // namespace

HL_TEST(persistence_lock, the_runner_supplied_the_proof_harness) {
  HL_CHECK_MSG(harness_available(),
               "the test runner must supply the lock proof harness; these proofs are never skipped");
}

HL_TEST(persistence_lock, a_second_writer_in_this_process_is_refused) {
  hl_test::TempDir workspace("lock_same_process");
  auto first = Runtime::open(options_for(workspace.child("store")));
  HL_REQUIRE(first.has_value());
  const auto second = Runtime::open(options_for(workspace.child("store")));
  HL_REQUIRE(!second.has_value());
  HL_CHECK_ERROR(second, ErrorCode::StoreLocked);

  // A reader is not a writer and must not be blocked by one.
  const auto reader = Runtime::open_read_only(workspace.child("store"));
  HL_CHECK_EQ(reader.has_value(), true);
}

HL_TEST(persistence_lock, the_lock_is_released_when_the_writer_is_destroyed) {
  hl_test::TempDir workspace("lock_release");
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
  }
  auto reopened = Runtime::open(options_for(workspace.child("store")));
  HL_CHECK_EQ(reopened.has_value(), true);
}

HL_TEST(persistence_lock, a_live_second_process_holds_writer_authority) {
  hl_test::TempDir workspace("lock_second_process");
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    Driver driver(runtime);
    HL_REQUIRE(driver.register_object("lock-node").has_value());
  }

  hl_test::ChildProcess holder;
  std::uint64_t pid = 0;
  HL_REQUIRE(start_lock_holder(workspace.child("store"), holder, pid));
  HL_CHECK_MSG(pid != 0, "the child reports the process id that holds the lock");

  const auto refused = Runtime::open(options_for(workspace.child("store")));
  HL_REQUIRE(!refused.has_value());
  HL_CHECK_ERROR(refused, ErrorCode::StoreLocked);

  // A reader still sees the last published generation.
  const auto reader = Runtime::open_read_only(workspace.child("store"));
  HL_REQUIRE(reader.has_value());
  HL_CHECK_EQ(reader.value().registry().object_count(), 1u);
  HL_CHECK_EQ(reader.value().verify().has_value(), true);

  holder.terminate(7);
  HL_CHECK_EQ(holder.wait(), 7);

  // The operating system released the lock even though the process ran no
  // destructor: this is the whole point of using a handle owned lock.
  const auto recovered = Runtime::open(options_for(workspace.child("store")));
  HL_CHECK_EQ(recovered.has_value(), true);
}

HL_TEST(persistence_lock, a_second_process_cannot_take_a_held_lock) {
  hl_test::TempDir workspace("lock_two_processes");
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
  }
  hl_test::ChildProcess first;
  std::uint64_t first_pid = 0;
  HL_REQUIRE(start_lock_holder(workspace.child("store"), first, first_pid));

  hl_test::ChildProcess second;
  std::string error;
  HL_REQUIRE(second.start(hl_test::lock_child_path(), {"--store", workspace.child("store").string()}, error));
  const std::vector<std::string> lines = second.read_lines_until_close();
  const int code = second.wait();
  HL_CHECK_MSG(code != 0, "the second writer must not be able to take a held lock");
  bool announced_ready = false;
  for (const std::string& line : lines) {
    if (line.rfind("ready ", 0) == 0) {
      announced_ready = true;
    }
  }
  HL_CHECK_EQ(announced_ready, false);

  first.terminate(0);
  const auto after = Runtime::open(options_for(workspace.child("store")));
  HL_CHECK_EQ(after.has_value(), true);
}

HL_TEST(persistence_lock, writers_are_not_implicitly_shared_between_commands) {
  hl_test::TempDir workspace("lock_sequential");
  for (int cycle = 0; cycle < 8; ++cycle) {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    const auto verified = runtime.verify();
    HL_CHECK_EQ(verified.has_value(), true);
  }
}
