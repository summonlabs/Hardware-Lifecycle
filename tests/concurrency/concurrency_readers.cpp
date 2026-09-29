// Hardware Lifecycle - concurrent readers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two configurations, both with real threads:
//
//  1. One writer commits a stream of transitions and gate decisions to a durable
//     store while four readers repeatedly open that store read only, verify it,
//     inspect it and export it. Every reader observation is a whole published
//     generation: it verifies, the object it sees is exactly one of the
//     generations the writer published, its revision is the length of its
//     history chain, and the document it exports describes the same generation.
//
//  2. Twelve threads hammer one shared runtime with read only queries. A Runtime
//     is single writer by design, so nothing mutates it while they run; the
//     claim under test is that concurrent readers never disagree about the state
//     and never fail.
//
// Synchronisation is by condition only: no assertion depends on a sleep, there
// is no timeout anywhere, and both configurations terminate on their own.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
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

/// Reports a result that was required to succeed, carrying the error itself.
template <class T>
[[nodiscard]] bool succeeded(hl_test::Context& hl_ctx, const Result<T>& result, const char* what) {
  if (result.has_value()) {
    return true;
  }
  hl_ctx.fail(__FILE__, __LINE__, std::string(what) + " was refused with " + describe(result.error()));
  return false;
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

struct PlannedStep {
  LifecycleState from;
  LifecycleState to;
  TransitionReason reason;
  AuthorityMask authority;
  std::vector<EvidenceKind> evidence;
};

/// The writer's stream: from Ordered to Retired through staging, installation,
/// commissioning, service, maintenance and drain. Entering and returning to
/// service needs the gate, which is a durable decision of its own, so the stream
/// interleaves two gate decisions with the transitions.
[[nodiscard]] const std::vector<PlannedStep>& planned_steps() {
  static const std::vector<PlannedStep> steps = {
      {LifecycleState::Ordered, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
       authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt}},
      {LifecycleState::Staged, LifecycleState::Installed, TransitionReason::InstallationCompleted,
       authority_bit(AuthorityScope::Installation),
       {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}},
      {LifecycleState::Installed, LifecycleState::Commissioning, TransitionReason::CommissioningStarted,
       authority_bit(AuthorityScope::Commissioning), {EvidenceKind::ServiceRecord}},
      {LifecycleState::Commissioning, LifecycleState::Active, TransitionReason::CommissioningPassed,
       authority_bit(AuthorityScope::Service), {EvidenceKind::CommissioningReport}},
      {LifecycleState::Active, LifecycleState::Maintenance, TransitionReason::MaintenanceScheduled,
       authority_bit(AuthorityScope::Maintenance), {EvidenceKind::MaintenanceRecord}},
      {LifecycleState::Maintenance, LifecycleState::Active, TransitionReason::MaintenanceCompleted,
       authority_bit(AuthorityScope::Maintenance), {EvidenceKind::MaintenanceRecord}},
      {LifecycleState::Active, LifecycleState::Retiring, TransitionReason::RetirementApproved,
       authority_bit(AuthorityScope::Retirement), {EvidenceKind::DrainRecord}},
      {LifecycleState::Retiring, LifecycleState::Retired, TransitionReason::DecommissionCompleted,
       authority_bit(AuthorityScope::Decommissioning), {EvidenceKind::DecommissioningRecord}},
  };
  return steps;
}

/// The service free walk used by the shared runtime case: it never enters the
/// service scope, so it needs no gate decision.
[[nodiscard]] const std::vector<PlannedStep>& retirement_steps() {
  static const std::vector<PlannedStep> steps = {
      {LifecycleState::Ordered, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
       authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt}},
      {LifecycleState::Staged, LifecycleState::Installed, TransitionReason::InstallationCompleted,
       authority_bit(AuthorityScope::Installation),
       {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}},
      {LifecycleState::Installed, LifecycleState::Commissioning, TransitionReason::CommissioningStarted,
       authority_bit(AuthorityScope::Commissioning), {EvidenceKind::ServiceRecord}},
      {LifecycleState::Commissioning, LifecycleState::Quarantined, TransitionReason::CommissioningFailed,
       authority_bit(AuthorityScope::Integrity), {EvidenceKind::IntegrityReport}},
      {LifecycleState::Quarantined, LifecycleState::Retiring,
       TransitionReason::QuarantineReleasedForRetirement, authority_bit(AuthorityScope::Retirement),
       {EvidenceKind::DrainRecord}},
      {LifecycleState::Retiring, LifecycleState::Retired, TransitionReason::DecommissionCompleted,
       authority_bit(AuthorityScope::Decommissioning), {EvidenceKind::DecommissioningRecord}},
  };
  return steps;
}

/// One generation the writer can actually publish, in commit order. A reader
/// compares every observation against this table, so a state that is merely
/// plausible is not enough: the observed revision, state and gate must be the
/// exact triple one of the writer's commits produced.
struct ReachableGeneration {
  std::uint64_t revision;
  LifecycleState state;
  EligibilityGate gate;
};

[[nodiscard]] const std::vector<ReachableGeneration>& reachable_generations() {
  static const std::vector<ReachableGeneration> generations = {
      {1, LifecycleState::Ordered, EligibilityGate::Unknown},
      {2, LifecycleState::Staged, EligibilityGate::Unknown},
      {3, LifecycleState::Installed, EligibilityGate::Unknown},
      {4, LifecycleState::Commissioning, EligibilityGate::Unknown},
      {5, LifecycleState::Commissioning, EligibilityGate::Open},
      {6, LifecycleState::Active, EligibilityGate::Open},
      {7, LifecycleState::Maintenance, EligibilityGate::Closed},
      {8, LifecycleState::Maintenance, EligibilityGate::Open},
      {9, LifecycleState::Active, EligibilityGate::Open},
      {10, LifecycleState::Retiring, EligibilityGate::Closed},
      {11, LifecycleState::Retired, EligibilityGate::Closed},
  };
  return generations;
}

[[nodiscard]] bool committed(hl_test::Context& hl_ctx, Runtime& runtime, const ObjectKey& key, LifecycleState target,
                             TransitionReason reason, AuthorityMask authority,
                             const std::vector<EvidenceKind>& kinds) {
  const HardwareObject* object = runtime.registry().find(key);
  if (object == nullptr) {
    hl_ctx.fail(__FILE__, __LINE__, "the writer lost its object");
    return false;
  }
  TransitionRequest request;
  request.plan = PlanId::parse(unique_name("writer-transition")).value();
  request.attempt = AttemptId::parse("attempt-1").value();
  request.key = key;
  request.expected_lifecycle_generation = object->lifecycle_generation;
  request.expected_revision = object->revision;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.expected_state = object->state;
  request.target_state = target;
  request.reason = reason;
  request.provenance = provenance_of(request.plan.value(), authority, kinds);
  const Result<TransitionReceipt> applied = runtime.apply_transition(request);
  if (applied.has_value()) {
    return true;
  }
  hl_ctx.fail(__FILE__, __LINE__, "a writer transition was refused: " + describe(applied.error()));
  return false;
}

[[nodiscard]] bool set_gate(hl_test::Context& hl_ctx, Runtime& runtime, const ObjectKey& key, EligibilityGate gate) {
  const HardwareObject* object = runtime.registry().find(key);
  if (object == nullptr) {
    hl_ctx.fail(__FILE__, __LINE__, "the writer lost its object");
    return false;
  }
  const std::string plan = unique_name("writer-gate");
  EligibilityGateRequest request;
  request.plan = PlanId::parse(plan).value();
  request.attempt = AttemptId::parse("attempt-1").value();
  request.key = key;
  request.gate = gate;
  request.expected_lifecycle_generation = object->lifecycle_generation;
  request.expected_revision = object->revision;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.provenance = provenance_of(plan, authority_bit(AuthorityScope::Service), {EvidenceKind::ServiceRecord});
  const Result<EligibilityGateReceipt> applied = runtime.set_eligibility_gate(request);
  if (applied.has_value()) {
    return true;
  }
  hl_ctx.fail(__FILE__, __LINE__, "a writer gate decision was refused: " + describe(applied.error()));
  return false;
}

/// One reader observation of one stable published generation. A reader never
/// mutates anything: it opens the store read only, verifies what it opened,
/// inspects the object, exports the document and reconciles all three views.
[[nodiscard]] std::string observe(const std::filesystem::path& root, const ObjectKey& key) {
  Result<Runtime> reader = Runtime::open_read_only(root);
  if (!reader.has_value()) {
    return "open read only: " + describe(reader.error());
  }
  const Result<Digest> verified = reader.value().verify();
  if (!verified.has_value()) {
    return "verify: " + describe(verified.error());
  }
  const Result<ObjectView> inspected = reader.value().inspect(key);
  if (!inspected.has_value()) {
    return "inspect: " + describe(inspected.error());
  }
  const HardwareObject& object = inspected.value().object;
  if (object.authority != AuthorityState::Recovered) {
    return "a read only generation claimed live authority";
  }
  const std::vector<ReachableGeneration>& generations = reachable_generations();
  if (!object.revision.valid() || object.revision.value() > generations.size()) {
    return "revision " + std::to_string(object.revision.value()) + " is not one the writer can have published";
  }
  const ReachableGeneration& expected = generations[static_cast<std::size_t>(object.revision.value() - 1)];
  if (object.state != expected.state || object.eligibility_gate != expected.gate) {
    return "revision " + std::to_string(object.revision.value()) + " observed as " +
           std::string(to_string(object.state)) + "/" + std::string(to_string(object.eligibility_gate)) +
           ", which that revision never was";
  }
  const Result<HistoryView> history = reader.value().history(key);
  if (!history.has_value()) {
    return "history: " + describe(history.error());
  }
  if (object.history_entries != history.value().entries.size()) {
    return "the object entry count is not the history chain length";
  }
  if (object.revision.value() != history.value().entries.size()) {
    return "the object revision is not the history chain length";
  }
  const Result<void> consistent = verify_history_against_object(
      HistoryLog{history.value().entries, history.value().chain_head}, object);
  if (!consistent.has_value()) {
    return "history against object: " + describe(consistent.error());
  }
  const Result<std::string> document = reader.value().export_document(ExportOptions());
  if (!document.has_value()) {
    return "export: " + describe(document.error());
  }
  Result<Snapshot> snapshot = from_json(document.value());
  if (!snapshot.has_value()) {
    return "the exported document does not decode: " + describe(snapshot.error());
  }
  if (snapshot.value().header.state_digest != reader.value().registry().state_digest()) {
    return "the document declares a different state digest than the runtime holds";
  }
  // The document describes exactly the generation the reader just verified: as
  // many events as the published commit sequence, contiguous from one.
  if (snapshot.value().events.size() != reader.value().registry().commit_sequence().value()) {
    return "the document carries " + std::to_string(snapshot.value().events.size()) + " events while the published " +
           "commit sequence is " + std::to_string(reader.value().registry().commit_sequence().value());
  }
  for (std::size_t index = 0; index < snapshot.value().events.size(); ++index) {
    if (snapshot.value().events[index].sequence.value() != index + 1) {
      return "the exported event log is not contiguous";
    }
  }
  return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// One writer, four readers, one durable store
// ---------------------------------------------------------------------------

/// What the writer does next: one durable decision, one published generation.
struct WriterAction {
  bool gate;
  std::size_t step;
};

HL_TEST(concurrency_readers, writer_and_four_readers) {
  hl_test::TempDir directory("hl_concurrency_readers");
  const std::filesystem::path root = directory.child("store");

  Result<Runtime> writer = Runtime::open(OpenOptions{root});
  HL_REQUIRE(writer.has_value());
  const ObjectKey key = key_of("concurrent-asset", 1);
  {
    const std::string plan = unique_name("writer-create");
    CreateRequest create;
    create.plan = PlanId::parse(plan).value();
    create.attempt = AttemptId::parse("attempt-1").value();
    create.key = key;
    create.kind = HardwareKind::Compute;
    create.model = ModelId::parse("model-1").value();
    create.firmware_generation = FirmwareGeneration::first();
    create.initial_state = LifecycleState::Ordered;
    create.location = location_of("site-a", "rack-1", "slot-1");
    create.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                      {EvidenceKind::ProcurementRecord});
    HL_REQUIRE(writer.value().create_object(create).has_value());
  }

  // The actions that publish generations 2..11, in order: two gate decisions
  // interleaved with the eight transitions of the stream.
  const std::vector<WriterAction> actions = {
      {false, 0}, {false, 1}, {false, 2}, {true, 0}, {false, 3},
      {false, 4}, {true, 0}, {false, 5}, {false, 6}, {false, 7},
  };
  HL_CHECK_EQ(actions.size() + 1, reachable_generations().size());

  constexpr std::size_t kReaderCount = 4;
  const std::size_t generation_count = reachable_generations().size();

  // Every reader observes every published generation exactly once, and the
  // writer publishes the next generation only after all four readers have
  // finished the current one. That is what makes "a whole generation, never a
  // torn one" an assertion rather than a hope: no manifest is ever published
  // while a reader is inside an observation, and no reader is ever starved
  // because the work per generation is bounded. Synchronisation is by these
  // counters alone - there is no sleep and no timeout.
  std::atomic<std::size_t> generation{1};
  std::atomic<bool> stop{false};
  std::vector<std::atomic<std::size_t>> arrived(generation_count);
  for (std::atomic<std::size_t>& counter : arrived) {
    counter.store(0, std::memory_order_relaxed);
  }
  std::vector<std::string> reader_errors(kReaderCount);
  std::vector<std::size_t> reader_rounds(kReaderCount, 0);
  std::size_t published_generations = 1;

  // A bounded handshake rather than continuous observation: generation N+1 is
  // never published until every reader has observed generation N. That is what
  // makes "a whole generation, never a torn one" an assertion instead of a hope,
  // and it keeps the cost of the proof proportional to the number of
  // generations rather than to how fast the machine spins.
  // Generation one, the object the writer registered before the readers start,
  // is a published generation like any other, so the readers observe it first.
  std::atomic<std::size_t> published_turns{1};
  std::atomic<bool> done{false};
  const auto observed_all = [&]() {
    for (;;) {
      bool everyone = true;
      for (std::size_t index = 0; index < kReaderCount; ++index) {
        if (reader_rounds[index] < published_turns.load(std::memory_order_acquire)) {
          everyone = false;
          break;
        }
      }
      if (everyone) {
        return;
      }
      std::this_thread::yield();
    }
  };

  std::vector<std::thread> readers;
  readers.reserve(kReaderCount);
  for (std::size_t index = 0; index < kReaderCount; ++index) {
    readers.emplace_back([&, index]() {
      std::size_t observed = 0;
      for (;;) {
        const std::size_t wanted = published_turns.load(std::memory_order_acquire);
        if (observed >= wanted) {
          if (done.load(std::memory_order_acquire)) {
            return;
          }
          std::this_thread::yield();
          continue;
        }
        const std::string error = observe(root, key);
        ++observed;
        reader_rounds[index] = observed;
        if (!error.empty() && reader_errors[index].empty()) {
          reader_errors[index] = error;
        }
      }
    });
  }

  bool writer_ok = true;
  for (const WriterAction& action : actions) {
    // Every reader has seen the generation that is currently published.
    observed_all();
    if (writer_ok) {
      const PlannedStep& step = planned_steps()[action.step];
      writer_ok = action.gate ? set_gate(hl_ctx, writer.value(), key, EligibilityGate::Open)
                              : committed(hl_ctx, writer.value(), key, step.to, step.reason, step.authority,
                                          step.evidence);
      if (writer_ok) {
        ++published_generations;
      }
    }
    published_turns.fetch_add(1, std::memory_order_acq_rel);
  }

  // One last round of observations of the final generation.
  observed_all();
  done.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  HL_CHECK(writer_ok);
  HL_CHECK_EQ(published_generations, generation_count);
  for (std::size_t index = 0; index < kReaderCount; ++index) {
    if (!reader_errors[index].empty()) {
      std::printf("[concurrency_readers] reader %zu failed: %s\n", index, reader_errors[index].c_str());
      std::fflush(stdout);
    }
    HL_CHECK_MSG(reader_errors[index].empty(), "a reader observed a torn or partial generation");
    HL_CHECK_MSG(reader_rounds[index] >= reachable_generations().size(),
                 "every reader observed every generation the writer published, including the first");
  }

  // The final durable generation is complete, and reading it twice gives the
  // same answer: the digest is reproducible, not incidental.
  HL_REQUIRE(succeeded(hl_ctx, writer.value().verify(), "the final store verifies"));
  const Digest final_state = writer.value().registry().state_digest();
  HL_CHECK_EQ(writer.value().registry().commit_sequence().value(), published_generations);
  Result<Runtime> first = Runtime::open_read_only(root);
  HL_REQUIRE(first.has_value());
  Result<Runtime> second = Runtime::open_read_only(root);
  HL_REQUIRE(second.has_value());
  HL_CHECK_EQ(first.value().registry().state_digest().hex(), final_state.hex());
  HL_CHECK_EQ(second.value().registry().state_digest().hex(), final_state.hex());
  const HardwareObject* final_object = first.value().registry().find(key);
  HL_REQUIRE(final_object != nullptr);
  HL_CHECK(final_object->state == LifecycleState::Retired);
  HL_CHECK_EQ(final_object->revision.value(), reachable_generations().size());
}

// ---------------------------------------------------------------------------
// Twelve readers on one shared runtime
// ---------------------------------------------------------------------------

HL_TEST(concurrency_readers, twelve_readers_one_runtime) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened).value();

  // A non trivial state: two objects, one walked to a superseded predecessor and
  // one installed successor, plus the replacement link between them, so list,
  // history, lineage and export all carry real content.
  const ObjectKey predecessor = key_of("shared-predecessor", 1);
  const ObjectKey successor = key_of("shared-successor", 1);
  const auto register_object = [&](const ObjectKey& key, HardwareKind kind) {
    const std::string plan = unique_name("shared-create");
    CreateRequest create;
    create.plan = PlanId::parse(plan).value();
    create.attempt = AttemptId::parse("attempt-1").value();
    create.key = key;
    create.kind = kind;
    create.model = ModelId::parse("model-1").value();
    create.firmware_generation = FirmwareGeneration::first();
    create.initial_state = LifecycleState::Ordered;
    create.location = location_of("site-a", "rack-1", "slot-1");
    create.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                      {EvidenceKind::ProcurementRecord});
    return runtime.create_object(create).has_value();
  };
  HL_REQUIRE(register_object(predecessor, HardwareKind::Compute));
  HL_REQUIRE(register_object(successor, HardwareKind::Storage));

  const auto walk = [&](const ObjectKey& key, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
      const PlannedStep& step = retirement_steps()[index];
      HL_REQUIRE(committed(hl_ctx, runtime, key, step.to, step.reason, step.authority, step.evidence));
    }
  };
  walk(predecessor, retirement_steps().size());
  walk(successor, 2);
  {
    const HardwareObject* object = runtime.registry().find(predecessor);
    HL_REQUIRE(object != nullptr);
    const std::string plan = unique_name("shared-link");
    ReplacementRequest link;
    link.plan = PlanId::parse(plan).value();
    link.attempt = AttemptId::parse("attempt-1").value();
    link.predecessor = predecessor;
    link.successor = successor;
    link.expected_lifecycle_generation = object->lifecycle_generation;
    link.expected_revision = object->revision;
    link.expected_control_epoch = runtime.registry().control_epoch();
    link.provenance = provenance_of(plan, authority_bit(AuthorityScope::Replacement),
                                    {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord});
    HL_REQUIRE(runtime.link_replacement(link).has_value());

    // The link republished the registry, so the pointer captured before it is
    // no longer a view of the authoritative state: the object is read again.
    const HardwareObject* after_link = runtime.registry().find(predecessor);
    HL_REQUIRE(after_link != nullptr);

    const std::string replaced_plan = unique_name("shared-replaced");
    TransitionRequest replaced;
    replaced.plan = PlanId::parse(replaced_plan).value();
    replaced.attempt = AttemptId::parse("attempt-1").value();
    replaced.key = predecessor;
    replaced.expected_lifecycle_generation = after_link->lifecycle_generation;
    replaced.expected_revision = after_link->revision;
    replaced.expected_control_epoch = runtime.registry().control_epoch();
    replaced.expected_state = after_link->state;
    replaced.target_state = LifecycleState::Replaced;
    replaced.reason = TransitionReason::SuccessorLinked;
    replaced.provenance = provenance_of(replaced_plan, authority_bit(AuthorityScope::Replacement),
                                        {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord});
    replaced.successor = successor;
    HL_REQUIRE(runtime.apply_transition(replaced).has_value());
  }

  const Result<Digest> verified = runtime.verify();
  HL_REQUIRE(succeeded(hl_ctx, verified, "the shared runtime verifies"));
  const Digest expected = verified.value();
  const std::size_t object_count = runtime.registry().object_count();
  const std::size_t event_count = runtime.registry().commit_sequence().value();
  const Result<std::string> exported = runtime.export_document(ExportOptions());
  HL_REQUIRE(succeeded(hl_ctx, exported, "the shared runtime exports"));
  const std::string expected_document = exported.value();

  constexpr std::size_t kThreads = 12;
  // Every round verifies the whole journal and exports the whole state, so the
  // round count is what makes this suite expensive. Fewer, heavier rounds prove
  // the same property: concurrent readers always agree.
  constexpr std::size_t kRounds = 4;
  std::atomic<std::size_t> started{0};
  std::vector<std::string> errors(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      started.fetch_add(1, std::memory_order_acq_rel);
      for (std::size_t round = 0; round < kRounds; ++round) {
        const Result<Digest> check = runtime.verify();
        if (!check.has_value()) {
          errors[index] = "verify: " + describe(check.error());
          return;
        }
        if (check.value() != expected) {
          errors[index] = "verify returned a different state digest";
          return;
        }
        const Result<std::vector<ObjectView>> listed = runtime.list(ListQuery());
        if (!listed.has_value() || listed.value().size() != object_count) {
          errors[index] = "list disagreed about the number of objects";
          return;
        }
        for (const ObjectView& view : listed.value()) {
          const Result<HistoryView> history = runtime.history(view.object.key);
          if (!history.has_value()) {
            errors[index] = "history: " + describe(history.error());
            return;
          }
          if (view.object.revision.value() != history.value().entries.size()) {
            errors[index] = "the history chain length disagrees with the object revision";
            return;
          }
          const Result<LineageView> lineage = runtime.lineage(view.object.key);
          if (!lineage.has_value()) {
            errors[index] = "lineage: " + describe(lineage.error());
            return;
          }
          const Result<ObjectView> inspected = runtime.inspect(view.object.key);
          if (!inspected.has_value() || inspected.value().object.revision != view.object.revision) {
            errors[index] = "inspect disagreed with list";
            return;
          }
        }
        const Result<LineageView> predecessor_lineage = runtime.lineage(predecessor);
        if (!predecessor_lineage.has_value() || predecessor_lineage.value().successors.size() != 1) {
          errors[index] = "the predecessor lineage lost its single successor";
          return;
        }
        const Result<std::string> document = runtime.export_document(ExportOptions());
        if (!document.has_value()) {
          errors[index] = "export: " + describe(document.error());
          return;
        }
        if (document.value() != expected_document) {
          errors[index] = "export produced different bytes for the same state";
          return;
        }
        Result<Snapshot> decoded = from_json(document.value());
        if (!decoded.has_value() || decoded.value().header.state_digest != expected) {
          errors[index] = "the exported document does not carry the state digest of the runtime";
          return;
        }
        if (decoded.value().events.size() != event_count) {
          errors[index] = "the exported document changed its event count";
          return;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  HL_CHECK_EQ(started.load(), kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    if (!errors[index].empty()) {
      std::printf("[concurrency_readers] thread %zu failed: %s\n", index, errors[index].c_str());
      std::fflush(stdout);
    }
    HL_CHECK_MSG(errors[index].empty(), "a concurrent reader disagreed with the shared runtime");
  }

  // The state never moved while the readers ran, and it is reproducible.
  const Result<Digest> final_digest = runtime.verify();
  HL_REQUIRE(final_digest.has_value());
  HL_CHECK_EQ(final_digest.value().hex(), expected.hex());
  const Result<std::string> final_document = runtime.export_document(ExportOptions());
  HL_REQUIRE(final_document.has_value());
  HL_CHECK_EQ(final_document.value(), expected_document);
}