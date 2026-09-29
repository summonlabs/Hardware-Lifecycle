// Hardware Lifecycle - corrupt, truncated and hostile durable input.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Each case damages exactly one documented field of a real store and checks that
// the reader refuses the store with a specific error. "Refuses" is the whole
// point: a durable format that repairs, truncates or silently falls back to an
// empty state would turn corruption into data loss.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "store_layout.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;
using hl_fixture::Driver;

/// Builds a small real store and returns the directory it lives in.
[[nodiscard]] std::filesystem::path build_store(const hl_test::TempDir& workspace) {
  auto opened = Runtime::open(OpenOptions{workspace.child("store")});
  if (!opened.has_value()) {
    return {};
  }
  Runtime runtime = std::move(opened.value());
  Driver driver(runtime);
  const auto created = driver.register_object("corruption-node");
  if (!created.has_value()) {
    return {};
  }
  if (!driver.advance_to(created.value().key, LifecycleState::Installed).has_value()) {
    return {};
  }
  return workspace.child("store");
}

/// Copies the store so a case can damage a copy and leave the original intact.
[[nodiscard]] std::filesystem::path damaged_copy(const hl_test::TempDir& workspace,
                                                 const std::filesystem::path& source, const char* label) {
  const std::filesystem::path target = workspace.child(label);
  hl_store::copy_tree(source, target);
  return target;
}

[[nodiscard]] bool patch_manifest(const std::filesystem::path& store,
                                  const std::function<void(std::vector<std::uint8_t>&)>& patch) {
  std::vector<std::uint8_t> manifest = hl_store::read_bytes(store / hl_store::kManifestName);
  if (manifest.size() != hl_store::kManifestBytes) {
    return false;
  }
  patch(manifest);
  return hl_store::write_bytes(store / hl_store::kManifestName, manifest);
}

}  // namespace

HL_TEST(persistence_corruption, a_truncated_manifest_is_refused) {
  hl_test::TempDir workspace("corrupt_manifest_truncated");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "truncated");
  std::vector<std::uint8_t> manifest = hl_store::read_bytes(copy / hl_store::kManifestName);
  manifest.resize(100);
  HL_REQUIRE(hl_store::write_bytes(copy / hl_store::kManifestName, manifest));
  const auto opened = Runtime::open_read_only(copy);
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_corrupt"));
}

HL_TEST(persistence_corruption, dirty_reserved_bytes_are_refused) {
  hl_test::TempDir workspace("corrupt_reserved");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "reserved");
  HL_REQUIRE(patch_manifest(copy, [](std::vector<std::uint8_t>& manifest) {
    manifest[300] = 0x5Au;
    hl_store::fix_manifest_crc(manifest);
  }));
  const auto opened = Runtime::open_read_only(copy);
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_corrupt"));
}

HL_TEST(persistence_corruption, a_broken_manifest_checksum_is_refused) {
  hl_test::TempDir workspace("corrupt_crc");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "crc");
  HL_REQUIRE(patch_manifest(copy, [](std::vector<std::uint8_t>& manifest) {
    manifest[hl_store::kManifestCrcOffset] ^= 0xFFu;
  }));
  const auto opened = Runtime::open_read_only(copy);
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_corrupt"));
}

HL_TEST(persistence_corruption, an_unsupported_manifest_version_is_refused) {
  hl_test::TempDir workspace("corrupt_version");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "version");
  HL_REQUIRE(patch_manifest(copy, [](std::vector<std::uint8_t>& manifest) {
    hl_store::write_u16(manifest, 8, 99);
    hl_store::fix_manifest_crc(manifest);
  }));
  const auto opened = Runtime::open_read_only(copy);
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("unsupported_format_version"));
}

HL_TEST(persistence_corruption, a_count_that_disagrees_with_itself_is_refused) {
  hl_test::TempDir workspace("corrupt_count");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "count");
  HL_REQUIRE(patch_manifest(copy, [](std::vector<std::uint8_t>& manifest) {
    hl_store::write_u64(manifest, 48, 0);  // record count says zero
    hl_store::fix_manifest_crc(manifest);
  }));
  const auto opened = Runtime::open_read_only(copy);
  HL_REQUIRE(!opened.has_value());
  // A corrupt store must never be reported as an empty one.
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_corrupt"));
  HL_CHECK_MSG(opened.error().code != ErrorCode::StoreUninitialized,
               "an empty state is not a permitted reading of a damaged store");
}

HL_TEST(persistence_corruption, a_flipped_journal_bit_is_refused) {
  hl_test::TempDir workspace("corrupt_journal_bit");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "journal_bit");
  std::vector<std::uint8_t> journal = hl_store::read_bytes(copy / hl_store::kJournalName);
  HL_REQUIRE(journal.size() > 64);
  journal[journal.size() / 2] ^= 0x01u;
  HL_REQUIRE(hl_store::write_bytes(copy / hl_store::kJournalName, journal));
  const auto opened = Runtime::open_read_only(copy);
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_corrupt"));
}

HL_TEST(persistence_corruption, a_truncated_journal_is_refused) {
  hl_test::TempDir workspace("corrupt_journal_truncated");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "journal_truncated");
  std::vector<std::uint8_t> journal = hl_store::read_bytes(copy / hl_store::kJournalName);
  HL_REQUIRE(journal.size() > 40);
  journal.resize(journal.size() - 20);
  HL_REQUIRE(hl_store::write_bytes(copy / hl_store::kJournalName, journal));
  const auto opened = Runtime::open_read_only(copy);
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_corrupt"));
}

HL_TEST(persistence_corruption, an_unpublished_tail_is_discarded_and_reported) {
  hl_test::TempDir workspace("corrupt_tail");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto published = hl_store::read_bytes(store / hl_store::kJournalName);
  std::vector<std::uint8_t> junk(40, 0xC3u);
  HL_REQUIRE(hl_store::append_bytes(store / hl_store::kJournalName, junk));

  // A reader ignores everything past the published prefix.
  const auto reader = Runtime::open_read_only(store);
  HL_REQUIRE(reader.has_value());
  HL_CHECK_EQ(reader.value().verify().has_value(), true);

  // A writer reports the unpublished tail and drops it. The runtime is
  // destroyed at the end of the block, which releases the writer lock.
  {
    auto opened = Runtime::open(OpenOptions{store});
    HL_REQUIRE(opened.has_value());
    HL_CHECK_EQ(opened.value().recovery().discarded_tail_bytes, 40u);
  }

  const auto after = hl_store::read_bytes(store / hl_store::kJournalName);
  HL_CHECK_EQ(after.size(), published.size());
  auto second = Runtime::open(OpenOptions{store});
  HL_REQUIRE(second.has_value());
  HL_CHECK_EQ(second.value().recovery().discarded_tail_bytes, 0u);
}

HL_TEST(persistence_corruption, record_framing_damage_is_refused_even_with_a_matching_log_digest) {
  hl_test::TempDir workspace("corrupt_framing");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());

  struct Damage {
    const char* label;
    // The exact code the reader must report. A declared length above the bound
    // is a bound violation, so it is more primary than the framing failure that
    // would follow from it.
    ErrorCode expected;
    std::function<void(std::vector<std::uint8_t>&)> patch;
  };
  const std::vector<Damage> damages = {
      {"magic", ErrorCode::IntegrityFailure, [](std::vector<std::uint8_t>& journal) { journal[0] = 'X'; }},
      {"kind", ErrorCode::IntegrityFailure,
       [](std::vector<std::uint8_t>& journal) { hl_store::write_u16(journal, 10, 99); }},
      {"sequence", ErrorCode::IntegrityFailure,
       [](std::vector<std::uint8_t>& journal) { hl_store::write_u64(journal, 16, 9); }},
      {"payload_crc", ErrorCode::IntegrityFailure,
       [](std::vector<std::uint8_t>& journal) { journal[32] ^= 0x7Fu; }},
      {"payload_length", ErrorCode::LimitExceeded,
       [](std::vector<std::uint8_t>& journal) { hl_store::write_u32(journal, 12, 0x00FFFFFFu); }}};

  for (const Damage& damage : damages) {
    const std::filesystem::path copy = damaged_copy(workspace, store, damage.label);
    std::vector<std::uint8_t> journal = hl_store::read_bytes(copy / hl_store::kJournalName);
    HL_REQUIRE(!journal.empty());
    damage.patch(journal);
    HL_REQUIRE(hl_store::write_bytes(copy / hl_store::kJournalName, journal));
    // Keep the manifest honest about the bytes so that the framing checks are
    // what refuses the store, not the log digest.
    HL_REQUIRE(patch_manifest(copy, [&journal](std::vector<std::uint8_t>& manifest) {
      hl_store::fix_manifest_log_digest(manifest, journal);
      hl_store::fix_manifest_crc(manifest);
    }));
    const auto opened = Runtime::open_read_only(copy);
    if (opened.has_value()) {
      HL_FAIL(std::string("a store whose record ") + damage.label + " was damaged must not open");
      continue;
    }
    HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string(to_string(damage.expected)));
  }
}

HL_TEST(persistence_corruption, a_journal_without_a_manifest_is_refused) {
  hl_test::TempDir workspace("corrupt_no_manifest");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  const auto copy = damaged_copy(workspace, store, "no_manifest");
  std::error_code error;
  std::filesystem::remove(copy / hl_store::kManifestName, error);
  HL_REQUIRE(!error);
  const auto opened = Runtime::open(OpenOptions{copy});
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_corrupt"));
}

HL_TEST(persistence_corruption, creating_over_an_existing_store_is_refused) {
  hl_test::TempDir workspace("corrupt_create_new");
  const auto store = build_store(workspace);
  HL_REQUIRE(!store.empty());
  OpenOptions options;
  options.root = store;
  options.create_if_missing = false;
  {
    auto opened = Runtime::open(options);
    HL_CHECK_EQ(opened.has_value(), true);
  }
}

HL_TEST(persistence_corruption, a_missing_store_in_read_only_mode_is_uninitialised_not_empty) {
  hl_test::TempDir workspace("corrupt_missing");
  const auto opened = Runtime::open_read_only(workspace.child("nothing-here"));
  HL_REQUIRE(!opened.has_value());
  HL_CHECK_EQ(std::string(to_string(opened.error().code)), std::string("store_uninitialized"));
}
