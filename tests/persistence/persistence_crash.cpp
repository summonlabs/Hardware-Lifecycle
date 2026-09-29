// Hardware Lifecycle - crash consistency with real process termination.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The claim is that a store survives a process dying at any point of the commit
// protocol, that recovery yields exactly one authoritative generation and never
// a merged or partial one, and that the writer lock a dead process held is
// released by the operating system. A child process is really terminated at a
// real durable step; nothing is mocked and no timeout is used anywhere.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "process.hpp"
#include "store_layout.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;
using hl_fixture::Driver;

constexpr const char* kStageNames[] = {"BeforeAppend", "AfterAppend",   "AfterFlush",
                                       "AfterVerify",  "BeforePublish", "AfterPublish"};

[[nodiscard]] const char* stage_name(CommitStage stage) {
  switch (stage) {
    case CommitStage::BeforeAppend:
      return kStageNames[0];
    case CommitStage::AfterAppend:
      return kStageNames[1];
    case CommitStage::AfterFlush:
      return kStageNames[2];
    case CommitStage::AfterVerify:
      return kStageNames[3];
    case CommitStage::BeforePublish:
      return kStageNames[4];
    case CommitStage::AfterPublish:
      return kStageNames[5];
  }
  return kStageNames[0];
}

/// Prepares a store with two published records and returns the state digest.
[[nodiscard]] bool prepare(const std::filesystem::path& root, Digest& digest, ObjectKey& key) {
  auto opened = Runtime::open(OpenOptions{root});
  if (!opened.has_value()) {
    return false;
  }
  Runtime runtime = std::move(opened.value());
  Driver driver(runtime);
  const auto created = driver.register_object("crash-parent");
  if (!created.has_value()) {
    return false;
  }
  if (!driver.advance_to(created.value().key, LifecycleState::Staged).has_value()) {
    return false;
  }
  key = created.value().key;
  digest = runtime.registry().state_digest();
  return true;
}

/// The multiprocess proofs are not skippable: a runner that forgets the path
/// must see a failure, not a quiet pass.
[[nodiscard]] bool harness_available() { return !hl_test::crash_child_path().empty(); }

}  // namespace

HL_TEST(persistence_crash, the_runner_supplied_the_proof_harness) {
  HL_CHECK_MSG(harness_available(),
               "the test runner must supply the crash proof harness; these proofs are never skipped");
}

HL_TEST(persistence_crash, a_process_dying_inside_the_commit_protocol_never_corrupts_the_store) {
  for (const CommitStage stage : {CommitStage::BeforeAppend, CommitStage::AfterAppend, CommitStage::AfterFlush,
                                  CommitStage::AfterVerify, CommitStage::BeforePublish, CommitStage::AfterPublish}) {
    hl_test::TempDir workspace("crash_stage");
    Digest digest_before;
    ObjectKey parent_key;
    HL_REQUIRE(prepare(workspace.child("store"), digest_before, parent_key));

    hl_test::ChildProcess child;
    std::string error;
    const std::string stage_text = stage_name(stage);
    const bool started = child.start(
        hl_test::crash_child_path(),
        // The harness commits until the store holds this many records and then
        // crashes inside the next commit. The parent published two, so the child
        // publishes two more and dies during the fifth.
        {"--store", workspace.child("store").string(), "--stage", stage_text, "--records", "4", "--asset",
         "crash-child-node"},
        error);
    HL_REQUIRE(started);
    const std::vector<std::string> lines = child.read_lines_until_close();
    const int exit_code = child.wait();

    bool reached = false;
    std::size_t commits_seen = 0;
    for (const std::string& line : lines) {
      if (line == "stage " + stage_text) {
        reached = true;
      }
      if (line.rfind("committed ", 0) == 0) {
        ++commits_seen;
      }
    }
    if (!reached) {
      HL_FAIL(std::string("the child never reached stage ") + stage_text);
      continue;
    }
    HL_CHECK_EQ(exit_code, 3);
    HL_CHECK_EQ(commits_seen, 2u);

    auto reopened = Runtime::open(OpenOptions{workspace.child("store")});
    if (!reopened.has_value()) {
      HL_FAIL(std::string("the store did not reopen after a crash at ") + stage_text + ": " +
              std::string(to_string(reopened.error().code)));
      continue;
    }
    Runtime runtime = std::move(reopened.value());
    HL_CHECK_EQ(runtime.verify().has_value(), true);
    const std::uint64_t expected = stage == CommitStage::AfterPublish ? 5u : 4u;
    HL_CHECK_EQ(runtime.recovery().record_count, expected);
    HL_CHECK_EQ(runtime.registry().commit_sequence().value(), expected);
    HL_CHECK_EQ(runtime.recovery().state_digest.hex(), runtime.registry().state_digest().hex());
    if (stage == CommitStage::BeforeAppend || stage == CommitStage::AfterPublish) {
      HL_CHECK_EQ(runtime.recovery().discarded_tail_bytes, 0u);
    } else {
      HL_CHECK_MSG(runtime.recovery().discarded_tail_bytes > 0,
                   "an append that was never published must be reported as a discarded tail");
    }

    // Whatever generation survived, it is complete: the parent object still
    // carries exactly the two history entries the parent session published.
    const auto view = runtime.inspect(parent_key);
    HL_REQUIRE(view.has_value());
    HL_CHECK_EQ(view.value().object.history_entries, 2u);
    HL_CHECK_EQ(std::string(to_string(view.value().object.state)), std::string("Staged"));

    // The store is usable again: a crashed append must not poison it. The
    // recovered object is fenced, so authority is re-established first.
    Driver driver(runtime, std::string("after-") + stage_text);
    HL_REQUIRE(driver.attest(parent_key).has_value());
    const auto continued = driver.advance_to(parent_key, LifecycleState::Installed);
    HL_REQUIRE(continued.has_value());
    HL_CHECK_EQ(runtime.verify().has_value(), true);
  }
}

HL_TEST(persistence_crash, the_writer_lock_is_released_by_process_death) {
  hl_test::TempDir workspace("crash_lock");
  Digest digest_before;
  ObjectKey parent_key;
  HL_REQUIRE(prepare(workspace.child("store"), digest_before, parent_key));

  hl_test::ChildProcess child;
  std::string error;
  HL_REQUIRE(child.start(hl_test::crash_child_path(),
                         {"--store", workspace.child("store").string(), "--stage", "AfterAppend", "--records", "3",
                          "--asset", "crash-lock-node"},
                         error));
  const std::vector<std::string> lines = child.read_lines_until_close();
  HL_CHECK_MSG(!lines.empty(), "the child reported its progress before dying");
  HL_CHECK_EQ(child.wait(), 3);

  // The child held the writer lock when it died. No cleanup ran.
  auto opened = Runtime::open(OpenOptions{workspace.child("store")});
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  HL_CHECK_EQ(runtime.verify().has_value(), true);
  HL_CHECK_EQ(runtime.recovery().fence_advanced, true);
}

HL_TEST(persistence_crash, a_crashed_append_leaves_no_partial_generation) {
  // The proof harness uses fixed plan identities, so one store supports one
  // crashed run. Repeating the whole experiment on a fresh store shows that the
  // outcome does not depend on the previous crash having been cleaned up.
  for (int round = 0; round < 3; ++round) {
    hl_test::TempDir workspace("crash_partial");
    const std::filesystem::path store = workspace.child("store");
    Digest digest_before;
    ObjectKey parent_key;
    HL_REQUIRE(prepare(store, digest_before, parent_key));

    hl_test::ChildProcess child;
    std::string error;
    HL_REQUIRE(child.start(hl_test::crash_child_path(),
                           {"--store", store.string(), "--stage", "AfterFlush", "--records", "4", "--asset",
                            "crash-round-" + std::to_string(round)},
                           error));
    const std::vector<std::string> lines = child.read_lines_until_close();
    HL_CHECK_EQ(child.wait(), 3);
    bool reached = false;
    std::size_t commits_seen = 0;
    for (const std::string& line : lines) {
      if (line == "stage AfterFlush") {
        reached = true;
      }
      if (line.rfind("committed ", 0) == 0) {
        ++commits_seen;
      }
    }
    HL_CHECK_EQ(reached, true);
    HL_CHECK_EQ(commits_seen, 2u);

    auto reopened = Runtime::open(OpenOptions{store});
    HL_REQUIRE(reopened.has_value());
    Runtime runtime = std::move(reopened.value());
    HL_CHECK_EQ(runtime.verify().has_value(), true);
    HL_CHECK_EQ(runtime.recovery().state_digest.hex(), runtime.registry().state_digest().hex());
    HL_CHECK_EQ(runtime.registry().commit_sequence().value(), runtime.recovery().record_count);
    // The two records the child did publish are durable; the one it died inside
    // is not, and its bytes are reported as a discarded tail.
    HL_CHECK_EQ(runtime.recovery().record_count, 4u);
    HL_CHECK_MSG(runtime.recovery().discarded_tail_bytes > 0,
                 "the crashed append left unpublished bytes that recovery must report");
    const auto view = runtime.inspect(parent_key);
    HL_REQUIRE(view.has_value());
    HL_CHECK_EQ(view.value().object.history_entries, 2u);
    Driver attester(runtime, "round-" + std::to_string(round));
    HL_REQUIRE(attester.attest(parent_key).has_value());
    const auto continued = attester.advance_to(parent_key, LifecycleState::Installed);
    HL_REQUIRE(continued.has_value());
    HL_CHECK_EQ(runtime.verify().has_value(), true);
  }
}
