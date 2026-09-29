// Hardware Lifecycle - durability round trip proofs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case uses a real store on a real file system with a real close and a
// real reopen. Nothing here is simulated.

#include <filesystem>
#include <optional>
#include <string>

#include "fixture.hpp"
#include "store_layout.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;
using hl_fixture::Driver;

constexpr const char* kAsset = "rack-07-node-01";

[[nodiscard]] OpenOptions options_for(const std::filesystem::path& root) {
  OpenOptions options;
  options.root = root;
  return options;
}

[[nodiscard]] ObjectKey first_key(const Runtime& runtime) { return runtime.registry().objects().begin()->first; }

}  // namespace

HL_TEST(persistence_roundtrip, commits_survive_a_real_close_and_reopen) {
  hl_test::TempDir workspace("roundtrip_close");
  Digest before;
  std::uint64_t commits = 0;
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    Driver driver(runtime);
    const auto created = driver.register_object(kAsset);
    HL_REQUIRE(created.has_value());
    HL_REQUIRE(driver.advance_to(created.value().key, LifecycleState::Installed).has_value());
    before = runtime.registry().state_digest();
    commits = runtime.registry().commit_sequence().value();
    HL_CHECK_MSG(commits >= 3u, "a registration plus two transitions is at least three commits");
  }
  auto reopened = Runtime::open(options_for(workspace.child("store")));
  HL_REQUIRE(reopened.has_value());
  Runtime runtime = std::move(reopened.value());
  HL_CHECK_EQ(runtime.registry().state_digest().hex(), before.hex());
  HL_CHECK_EQ(runtime.registry().commit_sequence().value(), commits);
  HL_CHECK_EQ(runtime.recovery().existing_state, true);
  HL_CHECK_EQ(runtime.recovery().record_count, commits);
  HL_CHECK_EQ(runtime.recovery().discarded_tail_bytes, 0u);
  HL_CHECK_EQ(runtime.recovery().fence_advanced, true);
  const auto view = runtime.inspect(first_key(runtime));
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(std::string(to_string(view.value().object.state)), std::string("Installed"));
  HL_CHECK_EQ(view.value().object.history_entries, 3u);
}

HL_TEST(persistence_roundtrip, recovery_report_describes_the_published_generation) {
  hl_test::TempDir workspace("roundtrip_report");
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    Driver driver(runtime);
    const auto created = driver.register_object(kAsset);
    HL_REQUIRE(created.has_value());
    HL_REQUIRE(driver.advance_to(created.value().key, LifecycleState::Commissioning).has_value());
  }
  const auto manifest_bytes = hl_store::read_bytes(workspace.child("store") / hl_store::kManifestName);
  HL_REQUIRE(manifest_bytes.size() == hl_store::kManifestBytes);
  const auto journal_bytes = hl_store::read_bytes(workspace.child("store") / hl_store::kJournalName);
  HL_REQUIRE(!journal_bytes.empty());

  auto reopened = Runtime::open(options_for(workspace.child("store")));
  HL_REQUIRE(reopened.has_value());
  Runtime runtime = std::move(reopened.value());
  const RecoveryReport& report = runtime.recovery();
  HL_CHECK_EQ(report.segment_bytes, static_cast<std::uint64_t>(journal_bytes.size()));
  HL_CHECK_EQ(report.commit_sequence.value(), report.record_count);
  HL_CHECK_EQ(report.log_digest.hex(),
              Sha256::hash(journal_bytes.data(), journal_bytes.size()).hex());
  HL_CHECK_EQ(report.state_digest.hex(), runtime.registry().state_digest().hex());
  HL_CHECK_EQ(report.control_epoch.valid(), true);
  HL_CHECK_EQ(report.incarnation.valid(), true);
  HL_CHECK_EQ(runtime.status().value().object_count, 1u);
}

HL_TEST(persistence_roundtrip, recovered_authority_is_fenced_until_it_is_reattested) {
  hl_test::TempDir workspace("roundtrip_fence");
  ObjectKey key;
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    Driver driver(runtime);
    const auto created = driver.register_object(kAsset);
    HL_REQUIRE(created.has_value());
    key = created.value().key;
  }
  auto reopened = Runtime::open(options_for(workspace.child("store")));
  HL_REQUIRE(reopened.has_value());
  Runtime runtime = std::move(reopened.value());
  Driver driver(runtime, "second-session");

  const auto view = runtime.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(std::string(to_string(view.value().object.authority)), std::string("recovered"));

  // The same request the table would accept must now be refused, because the
  // process that holds it never established authority over recovered state.
  const auto refused = driver.step(key, LifecycleState::Staged);
  HL_REQUIRE(!refused.has_value());
  HL_CHECK_ERROR(refused, ErrorCode::StaleAuthority);

  const auto attested = driver.attest(key);
  if (!attested.has_value()) {
    HL_FAIL(std::string("the attestation was refused: ") + describe(attested.error()));
    return;
  }
  HL_CHECK_EQ(attested.value().revision.valid(), true);
  const auto after = runtime.inspect(key);
  HL_REQUIRE(after.has_value());
  HL_CHECK_EQ(std::string(to_string(after.value().object.authority)), std::string("live"));

  const auto applied = driver.step(key, LifecycleState::Staged);
  HL_REQUIRE(applied.has_value());
  HL_CHECK_EQ(std::string(to_string(applied.value().to)), std::string("Staged"));
}

HL_TEST(persistence_roundtrip, a_read_only_runtime_cannot_mutate_but_answers_every_query) {
  hl_test::TempDir workspace("roundtrip_readonly");
  Digest digest;
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    Driver driver(runtime);
    const auto created = driver.register_object(kAsset);
    HL_REQUIRE(created.has_value());
    digest = runtime.registry().state_digest();
  }
  auto opened = Runtime::open_read_only(workspace.child("store"));
  HL_REQUIRE(opened.has_value());
  Runtime reader = std::move(opened.value());
  HL_CHECK_EQ(reader.writable(), false);
  HL_CHECK_EQ(reader.registry().state_digest().hex(), digest.hex());
  HL_CHECK_EQ(reader.verify().has_value(), true);

  Driver driver(reader);
  const auto refused_create = driver.register_object("another-node");
  HL_CHECK_ERROR(refused_create, ErrorCode::ReadOnlyAuthority);
  const auto key = first_key(reader);
  const auto refused_step = driver.step(key, LifecycleState::Staged);
  HL_CHECK_ERROR(refused_step, ErrorCode::ReadOnlyAuthority);
  const auto refused_attest = driver.attest(key);
  HL_CHECK_ERROR(refused_attest, ErrorCode::ReadOnlyAuthority);

  const auto view = reader.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.history_entries, 1u);
  HL_CHECK_EQ(reader.list(ListQuery()).value().size(), 1u);
  HL_CHECK_EQ(reader.history(key).value().entries.size(), 1u);
  HL_CHECK_EQ(reader.lineage(key).has_value(), true);
  HL_CHECK_EQ(reader.snapshot(ExportOptions()).header.importable, true);
}

HL_TEST(persistence_roundtrip, an_export_reimports_to_exactly_the_same_state_digest) {
  hl_test::TempDir workspace("roundtrip_export");
  std::string first_document;
  Digest first_digest;
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    Driver driver(runtime);
    const auto created = driver.register_object(kAsset);
    HL_REQUIRE(created.has_value());
    HL_REQUIRE(driver.advance_to(created.value().key, LifecycleState::Commissioning).has_value());
    const auto second = driver.register_object("rack-07-node-02", LifecycleState::Staged,
                                              HardwareGeneration::first(), "model-y", HardwareKind::Storage);
    HL_REQUIRE(second.has_value());
    const auto document = runtime.export_document(ExportOptions());
    HL_REQUIRE(document.has_value());
    first_document = document.value();
    first_digest = runtime.registry().state_digest();
  }

  auto opened = Runtime::open(options_for(workspace.child("second")));
  HL_REQUIRE(opened.has_value());
  Runtime imported = std::move(opened.value());
  const auto report = imported.import_document(first_document, ImportOptions());
  HL_REQUIRE(report.has_value());
  HL_CHECK_EQ(report.value().declared_state_digest.hex(), first_digest.hex());
  HL_CHECK_EQ(report.value().produced_state_digest.hex(), first_digest.hex());
  HL_CHECK_EQ(imported.registry().state_digest().hex(), first_digest.hex());
  HL_CHECK_EQ(report.value().events, imported.registry().commit_sequence().value());
  HL_CHECK_EQ(imported.verify().has_value(), true);

  const auto second_document = imported.export_document(ExportOptions());
  HL_REQUIRE(second_document.has_value());
  HL_CHECK_EQ(second_document.value(), first_document);
}

HL_TEST(persistence_roundtrip, repeated_open_and_close_is_stable) {
  hl_test::TempDir workspace("roundtrip_repeat");
  Digest digest;
  std::uint64_t records = 0;
  for (int cycle = 0; cycle < 6; ++cycle) {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    if (cycle == 0) {
      Driver driver(runtime);
      const auto created = driver.register_object(kAsset);
      HL_REQUIRE(created.has_value());
      HL_REQUIRE(driver.advance_to(created.value().key, LifecycleState::Installed).has_value());
      digest = runtime.registry().state_digest();
      records = runtime.registry().commit_sequence().value();
      HL_CHECK_EQ(runtime.recovery().existing_state, false);
      continue;
    }
    HL_CHECK_EQ(runtime.registry().state_digest().hex(), digest.hex());
    HL_CHECK_EQ(runtime.registry().commit_sequence().value(), records);
    const RecoveryReport& report = runtime.recovery();
    HL_CHECK_EQ(report.existing_state, true);
    HL_CHECK_EQ(report.record_count, records);
    HL_CHECK_EQ(report.commit_sequence.value(), records);
    HL_CHECK_EQ(report.discarded_tail_bytes, 0u);
    HL_CHECK_EQ(report.fence_advanced, true);
  }
}

HL_TEST(persistence_roundtrip, the_manifest_has_the_documented_shape) {
  hl_test::TempDir workspace("roundtrip_manifest");
  {
    auto opened = Runtime::open(options_for(workspace.child("store")));
    HL_REQUIRE(opened.has_value());
    Runtime runtime = std::move(opened.value());
    Driver driver(runtime);
    HL_REQUIRE(driver.register_object(kAsset).has_value());
  }
  const auto manifest = hl_store::read_bytes(workspace.child("store") / hl_store::kManifestName);
  HL_REQUIRE(manifest.size() == hl_store::kManifestBytes);
  HL_CHECK_EQ(hl_store::magic_of(manifest, 0, 8), std::string(hl_store::kManifestMagic));
  HL_CHECK_EQ(hl_store::read_u16(manifest, 8), static_cast<std::uint16_t>(1));
  HL_CHECK_EQ(hl_store::read_u16(manifest, 10), static_cast<std::uint16_t>(512));
  HL_CHECK_EQ(hl_store::read_u32(manifest, 12), 0u);
  HL_CHECK_EQ(hl_store::read_u32(manifest, hl_store::kManifestCrcOffset),
              crc32_ieee(manifest.data(), hl_store::kManifestCrcOffset));
  for (std::size_t index = 132; index < manifest.size(); ++index) {
    if (manifest[index] != 0u) {
      HL_FAIL("the manifest reserved area must be zero");
      break;
    }
  }
  HL_CHECK_EQ(hl_store::read_u64(manifest, 32), hl_store::read_u64(manifest, 48));
  HL_CHECK_MSG(hl_store::read_u64(manifest, 16) >= 1u, "the control epoch is at least one");
  HL_CHECK_MSG(hl_store::read_u64(manifest, 24) >= 1u, "the incarnation is at least one");
}
