// Hardware Lifecycle - the export and import boundary.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A document is untrusted input. It is decoded strictly, replayed through the
// ordinary commit path and refused unless the state it describes is reproduced
// exactly. These cases attack a real exported document - truncating it, editing
// its payloads, reordering its log, changing what it declares - and then assert
// that the refusal carried the right code and that nothing was merged into any
// store.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;

int g_sequence = 0;

[[nodiscard]] std::string unique_name(const char* prefix) {
  return std::string(prefix) + "-" + std::to_string(++g_sequence);
}

[[nodiscard]] ObjectKey key_of(const char* asset, std::uint64_t generation) {
  ObjectKey key;
  key.asset = AssetId::parse(asset).value();
  key.hardware_generation = HardwareGeneration::from_value(generation);
  return key;
}

[[nodiscard]] Location location_of(const char* site, const char* rack, const char* slot) {
  Location location;
  location.site = SiteId::parse(site).value();
  location.rack = RackId::parse(rack).value();
  location.slot = SlotId::parse(slot).value();
  return location;
}

[[nodiscard]] std::vector<EvidenceKind> required_kinds(EvidenceMask required) {
  std::vector<EvidenceKind> kinds;
  for (std::size_t index = 1; index <= kEvidenceKindCount; ++index) {
    const EvidenceKind kind = static_cast<EvidenceKind>(index);
    if (required.intersects(evidence_bit(kind))) {
      kinds.push_back(kind);
    }
  }
  if (kinds.empty()) {
    kinds.push_back(EvidenceKind::ServiceRecord);
  }
  return kinds;
}

[[nodiscard]] Provenance provenance_of(const std::string& plan, AuthorityMask authority,
                                       const std::vector<EvidenceKind>& kinds) {
  Provenance provenance;
  provenance.actor.id = ActorId::parse("actor-1").value();
  provenance.actor.kind = ActorKind::Automation;
  provenance.authority = authority;
  provenance.policy_generation = PolicyGeneration::first();
  provenance.plan = PlanId::parse(plan).value();
  provenance.attempt = AttemptId::parse("attempt-1").value();
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    EvidenceRef reference;
    reference.kind = kinds[index];
    reference.digest = Sha256::hash(plan + "#" + std::to_string(index));
    reference.source = "evidence://" + plan + "/" + std::to_string(index);
    reference.observed_sequence = ObservationSequence::from_value(static_cast<std::uint64_t>(index) + 1);
    provenance.evidence.push_back(reference);
  }
  return provenance;
}

template <class T>
[[nodiscard]] bool succeeded(hl_test::Context& hl_ctx, const Result<T>& result, const char* what) {
  if (result.has_value()) {
    return true;
  }
  hl_ctx.fail(__FILE__, __LINE__, std::string(what) + " was refused with " + describe(result.error()));
  return false;
}

[[nodiscard]] Result<Runtime> open_store(const std::filesystem::path& root) {
  OpenOptions options;
  options.root = root;
  return Runtime::open(options);
}

/// Builds the small real store every case starts from: one object walked from
/// Ordered to Active through staging, installation and commissioning, with one
/// gate decision on the way. That is five durable records, one of which is a
/// gate decision that does not change the lifecycle state.
[[nodiscard]] bool build_store(hl_test::Context& hl_ctx, Runtime& runtime, ObjectKey& key_out) {
  key_out = key_of("document-asset", 1);
  const std::string plan = unique_name("document-create");
  CreateRequest create;
  create.plan = PlanId::parse(plan).value();
  create.attempt = AttemptId::parse("attempt-1").value();
  create.key = key_out;
  create.kind = HardwareKind::Compute;
  create.model = ModelId::parse("model-1").value();
  create.firmware_generation = FirmwareGeneration::first();
  create.initial_state = LifecycleState::Ordered;
  create.location = location_of("site-a", "rack-1", "slot-1");
  create.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                    {EvidenceKind::ProcurementRecord});
  if (!succeeded(hl_ctx, runtime.create_object(create), "the document store registration")) {
    return false;
  }

  const auto step = [&](LifecycleState target, TransitionReason reason, AuthorityMask authority,
                        const std::vector<EvidenceKind>& kinds) {
    const HardwareObject* object = runtime.registry().find(key_out);
    if (object == nullptr) {
      hl_ctx.fail(__FILE__, __LINE__, "the document store object disappeared");
      return false;
    }
    TransitionRequest request;
    request.plan = PlanId::parse(unique_name("document-transition")).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key_out;
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
    request.expected_control_epoch = runtime.registry().control_epoch();
    request.expected_state = object->state;
    request.target_state = target;
    request.reason = reason;
    request.provenance = provenance_of(request.plan.value(), authority, kinds);
    return succeeded(hl_ctx, runtime.apply_transition(request), "a document store transition");
  };

  if (!step(LifecycleState::Staged, TransitionReason::DeliveryAccepted, authority_bit(AuthorityScope::Logistics),
            {EvidenceKind::DeliveryReceipt})) {
    return false;
  }
  if (!step(LifecycleState::Installed, TransitionReason::InstallationCompleted,
            authority_bit(AuthorityScope::Installation),
            {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord})) {
    return false;
  }
  if (!step(LifecycleState::Commissioning, TransitionReason::CommissioningStarted,
            authority_bit(AuthorityScope::Commissioning), {EvidenceKind::ServiceRecord})) {
    return false;
  }
  {
    const HardwareObject* object = runtime.registry().find(key_out);
    const std::string gate_plan = unique_name("document-gate");
    EligibilityGateRequest request;
    request.plan = PlanId::parse(gate_plan).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key_out;
    request.gate = EligibilityGate::Open;
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
    request.expected_control_epoch = runtime.registry().control_epoch();
    request.provenance = provenance_of(gate_plan, authority_bit(AuthorityScope::Service),
                                       {EvidenceKind::ServiceRecord});
    if (!succeeded(hl_ctx, runtime.set_eligibility_gate(request), "the document store gate decision")) {
      return false;
    }
  }
  if (!step(LifecycleState::Active, TransitionReason::CommissioningPassed, authority_bit(AuthorityScope::Service),
            {EvidenceKind::CommissioningReport})) {
    return false;
  }
  return true;
}

[[nodiscard]] bool replace_first(std::string& text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  if (at == std::string::npos) {
    return false;
  }
  text.replace(at, from.size(), to);
  return true;
}

[[nodiscard]] std::string render(const Snapshot& snapshot) {
  const Result<std::string> document = to_json(snapshot, ExportOptions());
  return document.has_value() ? document.value() : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// The happy path
// ---------------------------------------------------------------------------

HL_TEST(adversarial_documents, export_import_round_trip) {
  hl_test::TempDir directory("hl_adversarial_documents");
  Result<Runtime> source = open_store(directory.child("source"));
  HL_REQUIRE(source.has_value());
  ObjectKey key;
  HL_REQUIRE(build_store(hl_ctx, source.value(), key));
  HL_REQUIRE(succeeded(hl_ctx, source.value().verify(), "the source store verifies"));

  const ExportOptions options;
  const Result<std::string> exported = source.value().export_document(options);
  HL_REQUIRE(succeeded(hl_ctx, exported, "the source store exports"));
  const std::string document = exported.value();
  HL_CHECK(!document.empty());

  // Session scoped authority is not a durable fact: the document never carries
  // it, so an imported object cannot inherit the exporter's authority.
  HL_CHECK(document.find("\"live\"") == std::string::npos);
  HL_CHECK(document.find("\"recovered\"") == std::string::npos);

  Result<Snapshot> decoded = from_json(document);
  HL_REQUIRE(succeeded(hl_ctx, decoded, "the exported document decodes"));
  const Snapshot snapshot = decoded.value();
  HL_CHECK_EQ(snapshot.header.state_digest.hex(), source.value().registry().state_digest().hex());
  HL_CHECK_EQ(snapshot.header.event_count, snapshot.events.size());
  // Registration, staging, installation, commissioning, the gate decision and
  // entering service: six durable records, and six history entries, because a
  // gate decision is a durable mutation of the object like any other.
  HL_CHECK_EQ(snapshot.events.size(), std::size_t{6});
  HL_CHECK_EQ(snapshot.objects.size(), std::size_t{1});
  HL_CHECK_EQ(snapshot.history.size(), std::size_t{6});
  HL_CHECK_EQ(snapshot.header.importable, true);

  // Encoding a decoded document is byte identical: the format is canonical.
  HL_CHECK_EQ(render(snapshot), document);

  // Import into a second, empty store.
  Result<Runtime> target = open_store(directory.child("target"));
  HL_REQUIRE(target.has_value());
  const Digest empty_state = target.value().registry().state_digest();
  const Result<ImportReport> imported = target.value().import_document(document, ImportOptions());
  HL_REQUIRE(succeeded(hl_ctx, imported, "the document imports into an empty store"));
  HL_CHECK_EQ(imported.value().declared_state_digest.hex(), snapshot.header.state_digest.hex());
  HL_CHECK_EQ(imported.value().produced_state_digest.hex(), snapshot.header.state_digest.hex());
  HL_CHECK_EQ(imported.value().events, std::size_t{6});
  HL_CHECK_EQ(imported.value().objects, std::size_t{1});
  HL_CHECK_EQ(target.value().registry().state_digest().hex(), snapshot.header.state_digest.hex());
  HL_CHECK(target.value().registry().state_digest() != empty_state);
  HL_REQUIRE(succeeded(hl_ctx, target.value().verify(), "the imported store verifies"));

  // And exporting the imported store again produces the same bytes.
  const Result<std::string> re_exported = target.value().export_document(options);
  HL_REQUIRE(succeeded(hl_ctx, re_exported, "the imported store re-exports"));
  HL_CHECK_EQ(re_exported.value(), document);

  // The imported objects come back without live authority: authority is never
  // inherited across a process or a store boundary.
  const Result<ObjectView> inspected = target.value().inspect(key);
  HL_REQUIRE(succeeded(hl_ctx, inspected, "the imported object can be inspected"));
  HL_CHECK(inspected.value().object.authority == AuthorityState::Recovered);
}

// ---------------------------------------------------------------------------
// Attacks on a real document
// ---------------------------------------------------------------------------

HL_TEST(adversarial_documents, document_attacks) {
  hl_test::TempDir directory("hl_adversarial_documents");
  Result<Runtime> source = open_store(directory.child("source"));
  HL_REQUIRE(source.has_value());
  ObjectKey key;
  HL_REQUIRE(build_store(hl_ctx, source.value(), key));
  const Result<std::string> exported = source.value().export_document(ExportOptions());
  HL_REQUIRE(succeeded(hl_ctx, exported, "the source store exports"));
  const std::string document = exported.value();
  const Digest source_state = source.value().registry().state_digest();

  Result<Snapshot> decoded = from_json(document);
  HL_REQUIRE(succeeded(hl_ctx, decoded, "the exported document decodes"));
  const Snapshot snapshot = decoded.value();

  struct Attack {
    std::string name;
    std::string document;
    ErrorCode expected;
  };
  std::vector<Attack> attacks;

  attacks.push_back({"truncated document", document.substr(0, document.size() / 2),
                     ErrorCode::MalformedRequest});
  {
    std::string flipped = document;
    const std::size_t at = flipped.find("\"payload\": \"");
    HL_REQUIRE(at != std::string::npos);
    const std::size_t digit = at + 12;
    HL_REQUIRE(digit < flipped.size());
    flipped[digit] = flipped[digit] == 'a' ? 'b' : 'a';
    attacks.push_back({"flipped payload hex digit", std::move(flipped), ErrorCode::IntegrityFailure});
  }
  {
    Snapshot changed = snapshot;
    changed.events[0].sequence = CommitSequence::from_value(changed.events[0].sequence.value() + 1);
    attacks.push_back({"changed event sequence", render(changed), ErrorCode::IntegrityFailure});
  }
  {
    Snapshot changed = snapshot;
    changed.events.erase(changed.events.begin());
    attacks.push_back({"dropped the first event", render(changed), ErrorCode::IntegrityFailure});
  }
  {
    Snapshot changed = snapshot;
    std::swap(changed.events[0], changed.events[1]);
    attacks.push_back({"reordered two events", render(changed), ErrorCode::IntegrityFailure});
  }
  {
    std::string changed = document;
    HL_REQUIRE(replace_first(changed, "\"objects\": 1,", "\"objects\": 2,"));
    attacks.push_back({"changed a declared count", std::move(changed), ErrorCode::MalformedRequest});
  }
  {
    Snapshot changed = snapshot;
    changed.header.state_digest = Sha256::hash("not the state");
    attacks.push_back({"changed the declared state digest", render(changed), ErrorCode::IntegrityFailure});
  }
  {
    Snapshot changed = snapshot;
    changed.header.state_digest = Digest();
    attacks.push_back({"declared the zero state digest", render(changed), ErrorCode::IntegrityFailure});
  }
  {
    std::string changed = document;
    const std::size_t at = changed.find('{');
    HL_REQUIRE(at != std::string::npos);
    changed.insert(at + 1, "\n  \"unexpected_member\": 1,");
    attacks.push_back({"added an unknown member", std::move(changed), ErrorCode::MalformedRequest});
  }
  {
    std::string changed = document;
    const std::size_t at = changed.find("\"state_digest\"");
    HL_REQUIRE(at != std::string::npos);
    const std::size_t line_start = changed.rfind('\n', at);
    const std::size_t line_end = changed.find('\n', at);
    HL_REQUIRE(line_start != std::string::npos && line_end != std::string::npos);
    changed.erase(line_start, line_end - line_start);
    attacks.push_back({"removed a required member", std::move(changed), ErrorCode::MalformedRequest});
  }
  {
    std::string changed = document;
    HL_REQUIRE(replace_first(changed, "\"object_registered\"", "\"object_registerd\""));
    attacks.push_back({"changed a kind spelling", std::move(changed), ErrorCode::MalformedRequest});
  }
  attacks.push_back({"empty document", std::string(), ErrorCode::MalformedRequest});

  HL_REQUIRE(attacks.size() >= 12);
  for (std::size_t index = 0; index < attacks.size(); ++index) {
    const Attack& attack = attacks[index];
    Result<Runtime> target = open_store(directory.child("target-" + std::to_string(index)));
    HL_REQUIRE(target.has_value());
    const Digest empty_state = target.value().registry().state_digest();

    const Result<ImportReport> imported = target.value().import_document(attack.document, ImportOptions());
    HL_CHECK_ERROR(imported, attack.expected);

    // The authoritative in-memory state of the importing runtime is untouched:
    // a refused document is never merged, not even partially.
    HL_CHECK_EQ(target.value().registry().state_digest().hex(), empty_state.hex());
    HL_CHECK_EQ(target.value().registry().object_count(), std::size_t{0});
    HL_CHECK_EQ(target.value().registry().plan_count(), std::size_t{0});
    HL_CHECK(!target.value().registry().commit_sequence().valid());

    // And the durable store the import refused to accept is untouched too: it
    // still verifies and still replays to the state it held before the attack.
    // A refusal that leaves records behind would make this the only place the
    // divergence shows up.
    const Result<Digest> verified = target.value().verify();
    if (!verified.has_value()) {
      std::printf("[adversarial_documents] attack '%s' left the target store unverifiable: %s\n",
                  attack.name.c_str(), describe(verified.error()).c_str());
      std::fflush(stdout);
    }
    HL_CHECK(succeeded(hl_ctx, verified, "the target store still verifies after the attack"));

    // The source store is untouched by every attack on a copy of its document.
    HL_CHECK(succeeded(hl_ctx, source.value().verify(), "the source store still verifies"));
    HL_CHECK_EQ(source.value().registry().state_digest().hex(), source_state.hex());
  }
}

// ---------------------------------------------------------------------------
// The JSON layer, attacked directly
// ---------------------------------------------------------------------------

HL_TEST(adversarial_documents, json_layer_attacks) {
  // The document layer is bounded before it is interpreted: an over sized
  // document is refused as a whole rather than parsed and then judged.
  {
    const std::string oversized(limits::kMaxExportBytes + 1, ' ');
    HL_CHECK_ERROR(from_json(oversized), ErrorCode::LimitExceeded);
  }
  // Nesting is bounded as well, so no document can exhaust the parser stack.
  {
    std::string deep;
    for (int index = 0; index < 40; ++index) {
      deep.push_back('[');
    }
    for (int index = 0; index < 40; ++index) {
      deep.push_back(']');
    }
    HL_CHECK_ERROR(from_json(deep), ErrorCode::LimitExceeded);
  }
  HL_CHECK_ERROR(from_json("{\"format\": \"a\", \"format\": \"b\"}"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("{} {}"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("{\"n\": 01}"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("{\"s\": \"\\ud800\"}"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("{\"s\": \"\\udc00\"}"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("   "), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("[]"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("null"), ErrorCode::MalformedRequest);
  // A well formed document that is not this document: every required member is
  // missing, and the format tag is not this build's.
  HL_CHECK_ERROR(from_json("{}"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("{\"format\": \"hardware-lifecycle-snapshot\"}"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_json("{\"format\": \"something-else\"}"), ErrorCode::MalformedRequest);
}