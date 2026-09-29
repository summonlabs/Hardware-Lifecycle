// Hardware Lifecycle - the command line tool, driven as a real program.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case here starts the installed-style tool as a separate process, feeds
// it real arguments and asserts its real exit code and its real standard output.
// Nothing calls into the library directly: this is the end to end contract a
// script or an operator depends on.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "process.hpp"
#include "test_framework.hpp"

namespace {

using hl_test::ChildProcess;
using hl_test::TempDir;

struct Run {
  int exit_code = -1;
  std::vector<std::string> lines;

  [[nodiscard]] std::string text() const {
    std::string joined;
    for (const std::string& line : lines) {
      if (!joined.empty()) {
        joined.push_back('\n');
      }
      joined += line;
    }
    return joined;
  }

  [[nodiscard]] bool has(std::string_view needle) const { return text().find(needle) != std::string::npos; }

  [[nodiscard]] std::string value_of(std::string_view key) const {
    const std::string prefix = std::string(key) + ": ";
    for (const std::string& line : lines) {
      if (line.rfind(prefix, 0) == 0) {
        return line.substr(prefix.size());
      }
    }
    return std::string();
  }
};

[[nodiscard]] bool tool_available() { return !hl_test::cli_path().empty(); }

[[nodiscard]] Run run_cli(const std::vector<std::string>& arguments) {
  Run run;
  ChildProcess child;
  std::string error;
  if (!child.start(hl_test::cli_path(), arguments, error)) {
    return run;
  }
  run.lines = child.read_lines_until_close();
  run.exit_code = child.wait();
  return run;
}

/// Evidence references are written as "kind:64 hex:source".
[[nodiscard]] std::string evidence(const char* kind, char digit, const char* source) {
  return std::string(kind) + ":" + std::string(64, digit) + ":" + source;
}

constexpr char kAttestDigest = 'a';

/// create plus the first transition, so every later case starts from a store
/// that already exists and already has one object. Returns false when either
/// step misbehaved; the caller turns that into a failed requirement.
[[nodiscard]] bool seed_store(const std::string& store) {
  const Run created = run_cli({"--store", store, "create", "--asset", "cli-node", "--hardware-generation", "1",
                               "--kind", "compute", "--model", "model-x", "--plan", "p1", "--attempt", "a1",
                               "--actor", "operator-1", "--actor-kind", "operator", "--authority", "procurement",
                               "--evidence", evidence("procurement_record", '1', "erp/po-1")});
  if (created.exit_code != 0) {
    return false;
  }
  const Run moved = run_cli({"--store", store, "transition", "--asset", "cli-node", "--hardware-generation", "1",
                              "--to", "Staged", "--reason", "delivery_accepted", "--attest", "--plan", "p2",
                              "--attempt", "a1", "--actor", "operator-1", "--actor-kind", "operator",
                              "--authority", "logistics,recovery", "--evidence",
                              evidence("delivery_receipt", '2', "wms/receipt-1"), "--evidence",
                              evidence("recovery_attestation", kAttestDigest, "ops/attest-1")});
  return moved.exit_code == 0;
}

}  // namespace

HL_TEST(cli_end_to_end, the_runner_supplied_the_tool) {
  HL_CHECK_MSG(tool_available(),
               "the test runner must supply --cli; the command line contract is never skipped");
}

HL_TEST(cli_end_to_end, the_commands_that_need_no_store_answer_deterministically) {
  const Run version = run_cli({"--version"});
  HL_CHECK_EQ(version.exit_code, 0);
  HL_CHECK_EQ(version.value_of("name"), std::string("hwlifecycle"));
  HL_CHECK_EQ(version.value_of("version"), std::string("1.0.0"));

  const Run limits = run_cli({"limits"});
  HL_CHECK_EQ(limits.exit_code, 0);
  HL_CHECK_MSG(limits.has("kMaxIdentifierBytes"), "limits reports the bounds it enforces");

  const Run compatibility = run_cli({"compatibility"});
  HL_CHECK_EQ(compatibility.exit_code, 0);
  HL_CHECK_MSG(compatibility.has("journal"), "the compatibility matrix names the durable formats");

  const Run legal = run_cli({"legal-transitions", "--state", "Ordered"});
  HL_CHECK_EQ(legal.exit_code, 0);
  HL_CHECK_MSG(legal.has("Staged"), "Ordered can reach Staged");
  HL_CHECK_MSG(legal.has("Removed"), "Ordered can also be cancelled into Removed");

  const Run terminal = run_cli({"legal-transitions", "--state", "Removed"});
  HL_CHECK_EQ(terminal.exit_code, 0);
  HL_CHECK_EQ(terminal.value_of("target_count"), std::string("0"));

  // The same read command twice prints the same bytes.
  const Run first = run_cli({"compatibility"});
  const Run again = run_cli({"compatibility"});
  HL_CHECK_EQ(first.text(), again.text());
}

HL_TEST(cli_end_to_end, a_full_lifecycle_session_through_the_tool) {
  TempDir workspace("cli_session");
  const std::string store = workspace.child("store").string();
  const Run created = run_cli({"--store", store, "create", "--asset", "cli-node", "--hardware-generation", "1",
                               "--kind", "compute", "--model", "model-x", "--plan", "p1", "--attempt", "a1",
                               "--actor", "operator-1", "--actor-kind", "operator", "--authority", "procurement",
                               "--evidence", evidence("procurement_record", '1', "erp/po-1")});
  HL_REQUIRE(created.exit_code == 0);
  HL_CHECK_EQ(created.value_of("receipt.state"), std::string("Ordered"));
  HL_CHECK_EQ(created.value_of("receipt.revision"), std::string("1"));
  HL_CHECK_EQ(created.value_of("receipt.commit_sequence"), std::string("1"));
  HL_CHECK_MSG(created.value_of("receipt.receipt_digest").size() == 64,
               "a receipt carries a 256 bit digest");

  // A second invocation is a different process, so authority has to be
  // re-established explicitly. Without it the mutation is refused.
  const Run refused = run_cli({"--store", store, "transition", "--asset", "cli-node", "--hardware-generation", "1",
                               "--to", "Staged", "--reason", "delivery_accepted", "--plan", "p2", "--attempt", "a1",
                               "--actor", "operator-1", "--actor-kind", "operator", "--authority", "logistics",
                               "--evidence", evidence("delivery_receipt", '2', "wms/receipt-1")});
  HL_CHECK_EQ(refused.exit_code, 2);
  HL_CHECK_EQ(refused.value_of("error.code"), std::string("stale_authority"));

  const Run moved = run_cli({"--store", store, "transition", "--asset", "cli-node", "--hardware-generation", "1",
                             "--to", "Staged", "--reason", "delivery_accepted", "--attest", "--plan", "p2",
                             "--attempt", "a1", "--actor", "operator-1", "--actor-kind", "operator",
                             "--authority", "logistics,recovery", "--evidence",
                             evidence("delivery_receipt", '2', "wms/receipt-1"), "--evidence",
                             evidence("recovery_attestation", kAttestDigest, "ops/attest-1")});
  HL_REQUIRE(moved.exit_code == 0);
  HL_CHECK_MSG(moved.value_of("attest.plan").rfind("p2-attest", 0) == 0,
               "the attestation is a durable record under a plan derived from this plan and this session");
  // The epoch advances on every writer open, so the exact number depends on how
  // many invocations preceded this one; what matters is that the command bound
  // to the current revision, the current generation and the current epoch.
  HL_CHECK_MSG(moved.value_of("binding").rfind("revision=1 lifecycle_generation=1 epoch=", 0) == 0,
               "the binding names the revision, the lifecycle generation and the epoch it used");
  HL_CHECK_EQ(moved.value_of("receipt.from"), std::string("Ordered"));
  HL_CHECK_EQ(moved.value_of("receipt.to"), std::string("Staged"));
  HL_CHECK_EQ(moved.value_of("receipt.revision_after"), std::string("2"));

  // An edge that is not in the table is refused with its deterministic code.
  const Run illegal = run_cli({"--store", store, "transition", "--asset", "cli-node", "--hardware-generation", "1",
                               "--to", "Active", "--reason", "commissioning_passed", "--attest", "--plan", "p3",
                               "--attempt", "a1", "--actor", "operator-1", "--actor-kind", "operator",
                               "--authority", "service,recovery", "--evidence",
                               evidence("commissioning_report", '3', "cmdb/report-1"), "--evidence",
                               evidence("recovery_attestation", kAttestDigest, "ops/attest-1")});
  HL_CHECK_EQ(illegal.exit_code, 2);
  HL_CHECK_EQ(illegal.value_of("error.code"), std::string("illegal_transition"));

  // Pinning an expectation that no longer holds is a rejection, not a repair.
  const Run stale = run_cli({"--store", store, "transition", "--asset", "cli-node", "--hardware-generation", "1",
                             "--to", "Installed", "--reason", "installation_completed", "--expected-revision", "1",
                             "--attest", "--plan", "p4", "--attempt", "a1", "--actor", "operator-1",
                             "--actor-kind", "operator", "--authority", "installation,recovery", "--evidence",
                             evidence("installation_record", '4', "cmdb/install-1"), "--evidence",
                             evidence("location_record", '5', "cmdb/location-1"), "--evidence",
                             evidence("recovery_attestation", kAttestDigest, "ops/attest-1")});
  HL_CHECK_EQ(stale.exit_code, 2);
  HL_CHECK_EQ(stale.value_of("error.code"), std::string("stale_revision"));

  // Every required evidence kind, the location the target state demands, and a
  // location supplied on the command line.
  const Run installed =
      run_cli({"--store", store, "transition", "--asset", "cli-node", "--hardware-generation", "1", "--to",
               "Installed", "--reason", "installation_completed", "--location", "dc-1/rack-01/u12", "--attest",
               "--plan", "p5", "--attempt", "a1", "--actor", "operator-1", "--actor-kind", "operator",
               "--authority", "installation,recovery", "--evidence",
               evidence("installation_record", '4', "cmdb/install-1"), "--evidence",
               evidence("location_record", '5', "cmdb/location-1"), "--evidence",
               evidence("recovery_attestation", kAttestDigest, "ops/attest-1")});
  HL_REQUIRE(installed.exit_code == 0);
  HL_CHECK_EQ(installed.value_of("receipt.to"), std::string("Installed"));
  HL_CHECK_EQ(installed.value_of("receipt.revision_after"), std::string("3"));

  const Run inspected = run_cli({"--store", store, "inspect", "--asset", "cli-node"});
  HL_REQUIRE(inspected.exit_code == 0);
  HL_CHECK_EQ(inspected.value_of("state"), std::string("Installed"));
  HL_CHECK_EQ(inspected.value_of("location"), std::string("dc-1/rack-01/u12"));
  HL_CHECK_EQ(inspected.value_of("eligibility_gate"), std::string("unknown"));
  HL_CHECK_EQ(inspected.value_of("service_eligibility"), std::string("unknown"));
  HL_CHECK_EQ(inspected.value_of("authority"), std::string("recovered"));
  HL_CHECK_EQ(inspected.value_of("health.present"), std::string("false"));
  HL_CHECK_EQ(inspected.value_of("history_entries"), std::string("3"));

  const Run history = run_cli({"--store", store, "history", "--asset", "cli-node"});
  HL_REQUIRE(history.exit_code == 0);
  HL_CHECK_EQ(history.value_of("entry_count"), std::string("3"));
  HL_CHECK_MSG(history.has("delivery_accepted"), "the chain names the reason of each entry");
  HL_CHECK_MSG(history.value_of("chain_head").size() == 64, "the chain head is a digest");
}

HL_TEST(cli_end_to_end, a_health_observation_does_not_touch_the_lifecycle) {
  TempDir workspace("cli_health");
  const std::string store = workspace.child("store").string();
  HL_REQUIRE(seed_store(store));

  const Run before = run_cli({"--store", store, "inspect", "--asset", "cli-node"});
  HL_REQUIRE(before.exit_code == 0);

  const Run observed = run_cli({"--store", store, "observe-health", "--asset", "cli-node",
                                "--hardware-generation", "1", "--health", "failed", "--readiness", "not_ready",
                                "--availability", "unavailable", "--evidence-digest", std::string(64, 'e'),
                                "--source", "monitor/rack-01", "--plan", "p9", "--attempt", "a1", "--actor",
                                "monitor-1", "--actor-kind", "automation", "--evidence",
                                evidence("health_evidence", 'e', "monitor/rack-01")});
  HL_REQUIRE(observed.exit_code == 0);
  HL_CHECK_EQ(observed.value_of("receipt.sequence"), std::string("1"));

  const Run after = run_cli({"--store", store, "inspect", "--asset", "cli-node"});
  HL_REQUIRE(after.exit_code == 0);
  HL_CHECK_EQ(after.value_of("state"), before.value_of("state"));
  HL_CHECK_EQ(after.value_of("revision"), before.value_of("revision"));
  HL_CHECK_EQ(after.value_of("lifecycle_generation"), before.value_of("lifecycle_generation"));
  HL_CHECK_EQ(after.value_of("health.present"), std::string("true"));
  HL_CHECK_EQ(after.value_of("health.health"), std::string("failed"));
  HL_CHECK_EQ(after.value_of("health.freshness"), std::string("fresh"));
  // The lifecycle state says nothing about eligibility on its own: the gate has
  // never been set on this object, so eligibility is unknown, not ineligible.
  HL_CHECK_EQ(after.value_of("service_eligibility"), std::string("unknown"));
  HL_CHECK_EQ(after.value_of("eligibility_gate"), std::string("unknown"));
}

HL_TEST(cli_end_to_end, export_import_and_diff_round_trip_through_files) {
  TempDir workspace("cli_round_trip");
  const std::string store = workspace.child("store").string();
  const std::string second = workspace.child("second").string();
  const std::string document = workspace.child("snapshot.json").string();
  HL_REQUIRE(seed_store(store));

  const Run exported = run_cli({"--store", store, "export", "--out", document});
  HL_REQUIRE(exported.exit_code == 0);
  HL_CHECK_EQ(exported.value_of("importable"), std::string("true"));
  // create, the first transition, and the attestation that made it possible.
  HL_CHECK_EQ(exported.value_of("event_count"), std::string("3"));
  const std::string digest = exported.value_of("state_digest");
  HL_CHECK_EQ(digest.size(), static_cast<std::size_t>(64));
  HL_CHECK_MSG(std::filesystem::exists(document), "the document was written where it was asked for");

  const Run imported = run_cli({"--store", second, "import", "--in", document});
  HL_REQUIRE(imported.exit_code == 0);
  HL_CHECK_EQ(imported.value_of("declared_state_digest"), digest);
  HL_CHECK_EQ(imported.value_of("produced_state_digest"), digest);

  const Run same = run_cli({"diff", "--before", document, "--after", document});
  HL_REQUIRE(same.exit_code == 0);
  HL_CHECK_MSG(same.has("identical: true"), "a document compared with itself is identical");

  const Run verified = run_cli({"--store", second, "verify"});
  HL_CHECK_EQ(verified.exit_code, 0);
  HL_CHECK_MSG(verified.has("state: verified"), "the imported store verifies");

  // Importing the same document twice is refused: two authorities would merge.
  const Run twice = run_cli({"--store", second, "import", "--in", document});
  HL_CHECK_EQ(twice.exit_code, 2);
  HL_CHECK_EQ(twice.value_of("error.code"), std::string("unsupported_operation"));
}

HL_TEST(cli_end_to_end, the_reporting_contract_holds) {
  TempDir workspace("cli_contract");
  const std::string store = workspace.child("store").string();
  HL_REQUIRE(seed_store(store));

  // --json prints exactly one JSON object.
  const Run json = run_cli({"--store", store, "--json", "inspect", "--asset", "cli-node"});
  HL_REQUIRE(json.exit_code == 0);
  const std::string text = json.text();
  HL_CHECK_EQ(text.front(), '{');
  HL_CHECK_EQ(text.back(), '}');
  HL_CHECK_MSG(text.find("\"state\": \"Staged\"") != std::string::npos, "the JSON carries the real state");

  // Usage errors and rejections are different exit codes.
  const Run usage = run_cli({"--store", store, "inspect"});
  HL_CHECK_EQ(usage.exit_code, 1);
  const Run unknown_option = run_cli({"--store", store, "--not-an-option", "inspect"});
  HL_CHECK_EQ(unknown_option.exit_code, 1);
  const Run unknown_asset = run_cli({"--store", store, "inspect", "--asset", "no-such-asset"});
  HL_CHECK_EQ(unknown_asset.exit_code, 2);
  HL_CHECK_EQ(unknown_asset.value_of("error.code"), std::string("unknown_asset"));

  // A store failure has its own code: verify with nothing to verify.
  const Run missing = run_cli({"--store", workspace.child("nothing").string(), "verify"});
  HL_CHECK_EQ(missing.exit_code, 3);
  HL_CHECK_EQ(missing.value_of("error.code"), std::string("store_uninitialized"));

  // A read only open answers queries and refuses mutations.
  const Run read_only = run_cli({"--store", store, "--read-only", "inspect", "--asset", "cli-node"});
  HL_CHECK_EQ(read_only.exit_code, 0);
  const Run refused = run_cli({"--store", store, "--read-only", "attest", "--asset", "cli-node",
                               "--hardware-generation", "1", "--plan", "pz", "--attempt", "a1", "--actor",
                               "operator-1", "--actor-kind", "operator", "--authority", "recovery", "--evidence",
                               evidence("recovery_attestation", kAttestDigest, "ops/attest-1")});
  HL_CHECK_EQ(refused.exit_code, 2);
  HL_CHECK_EQ(refused.value_of("error.code"), std::string("read_only_authority"));
}

HL_TEST(cli_end_to_end, the_recovery_report_names_the_published_generation) {
  TempDir workspace("cli_recover");
  const std::string store = workspace.child("store").string();
  HL_REQUIRE(seed_store(store));

  const Run recovered = run_cli({"--store", store, "recover"});
  HL_REQUIRE(recovered.exit_code == 0);
  HL_CHECK_EQ(recovered.value_of("recovery.record_count"), std::string("3"));
  HL_CHECK_EQ(recovered.value_of("recovery.commit_sequence"), std::string("3"));
  HL_CHECK_EQ(recovered.value_of("recovery.discarded_tail_bytes"), std::string("0"));
  HL_CHECK_EQ(recovered.value_of("recovery.existing_state"), std::string("true"));
  HL_CHECK_EQ(recovered.value_of("recovery.fence_advanced"), std::string("true"));
  HL_CHECK_EQ(recovered.value_of("durability"), std::string("durable"));
  HL_CHECK_MSG(recovered.value_of("recovery.state_digest").size() == 64, "the report names the state digest");
  HL_CHECK_MSG(recovered.value_of("recovery.log_digest").size() == 64, "the report names the log digest");
}
