// Hardware Lifecycle - the composed runtime.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// One path for every mutation:
//   resolve plan identity, then validate against the exact generations,
//   revision, epoch and evidence the caller planned against
//     -> copy the authoritative state
//     -> build the durable record
//     -> apply the record to the copy, through the same replay function that
//        recovery uses
//     -> commit the record and the resulting state digest durably
//     -> publish the copy
//     -> issue the receipt, rebuilt from the record so that what the caller
//        receives is exactly what a replay would reconstruct
//
// A failure before publication leaves the previous generation authoritative and
// complete, and issues no receipt.

#include "hardware_lifecycle/runtime.hpp"

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "hardware_lifecycle/text.hpp"
#include "serialization.hpp"

namespace hardware_lifecycle {
namespace {

[[nodiscard]] Error read_only_error() {
  return Error::make(ErrorCode::ReadOnlyAuthority, "this runtime was opened without writer authority");
}

[[nodiscard]] Error internal_error(std::string message) { return Error::make(ErrorCode::Internal, std::move(message)); }

[[nodiscard]] Result<LogicalTime> predict_logical_time(const Registry& registry, const std::optional<Store>& store) {
  if (store.has_value()) {
    return store->next_logical_time();
  }
  return registry.logical_time().next();
}

[[nodiscard]] Result<CommitSequence> predict_commit_sequence(const Registry& registry,
                                                             const std::optional<Store>& store) {
  if (store.has_value()) {
    return store->commit_sequence().next();
  }
  return registry.commit_sequence().next();
}

[[nodiscard]] Result<void> check_plan_identity(const PlanId& plan, const AttemptId& attempt) {
  if (!plan.valid() || !attempt.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "the request carries no plan id or no attempt id");
  }
  return ok();
}

template <class ReceiptT>
[[nodiscard]] Result<ReceiptT> replay_receipt(const AppliedPlan& applied, std::string_view what) {
  if (!std::holds_alternative<ReceiptT>(applied.receipt)) {
    return internal_error(std::string("the applied plan ledger holds a ") + std::string(what) +
                          " plan under a receipt of a different kind");
  }
  ReceiptT receipt = std::get<ReceiptT>(applied.receipt);
  receipt.idempotent_replay = true;
  return receipt;
}

}  // namespace

struct Runtime::Impl {
  Registry registry;
  std::optional<Store> store;
  DurabilityClass durability = DurabilityClass::Durable;
  bool writable = false;
  std::uint64_t freshness_window = kDefaultHealthFreshnessWindow;
  std::filesystem::path root;
  RecoveryReport recovery;
  std::vector<JournalRecord> session_log;

  /// Applications of a record from an ephemeral runtime, kept so that an
  /// ephemeral session can still export a reimportable document.
  [[nodiscard]] std::vector<ExportedRecord> exported_events() const {
    std::vector<ExportedRecord> events;
    if (store.has_value()) {
      const Result<std::vector<JournalRecord>> records = store->read_records();
      if (!records.has_value()) {
        return events;
      }
      events.reserve(records.value().size());
      for (const JournalRecord& record : records.value()) {
        ExportedRecord exported;
        exported.kind = record.kind;
        exported.sequence = record.sequence;
        exported.logical_time = record.logical_time;
        exported.payload = record.payload;
        events.push_back(std::move(exported));
      }
      return events;
    }
    events.reserve(session_log.size());
    for (const JournalRecord& record : session_log) {
      ExportedRecord exported;
      exported.kind = record.kind;
      exported.sequence = record.sequence;
      exported.logical_time = record.logical_time;
      exported.payload = record.payload;
      events.push_back(std::move(exported));
    }
    return events;
  }

  /// Applies one record to a candidate state and makes it durable. The
  /// candidate is only published by the caller, after this returns success.
  [[nodiscard]] Result<void> publish(Registry& candidate, const detail::RecordPayload& payload,
                                     CommitSequence sequence, LogicalTime logical_time) {
    const Result<void> applied = detail::apply_payload(candidate, payload, sequence, logical_time);
    if (!applied.has_value()) {
      return applied.error();
    }
    const Result<std::vector<std::uint8_t>> encoded = detail::encode_payload(payload);
    if (!encoded.has_value()) {
      return encoded.error();
    }
    if (store.has_value()) {
      const Result<CommitOutcome> outcome =
          store->commit(detail::record_kind_of(payload), encoded.value(), logical_time, candidate.state_digest());
      if (!outcome.has_value()) {
        return outcome.error();
      }
      return ok();
    }
    JournalRecord record;
    record.kind = detail::record_kind_of(payload);
    record.sequence = sequence;
    record.logical_time = logical_time;
    record.payload = encoded.value();
    session_log.push_back(std::move(record));
    return ok();
  }

  [[nodiscard]] ObjectView view_of(const HardwareObject& object) const {
    ObjectView view;
    view.object = object;
    view.eligibility = service_eligibility(object.state, object.eligibility_gate);
    const HealthSnapshot* snapshot = registry.health(object.key);
    if (snapshot != nullptr) {
      view.health = *snapshot;
      view.health.freshness =
          assess_freshness(true, snapshot->recorded_at, registry.logical_time(), freshness_window);
    } else {
      view.health.present = false;
      view.health.freshness = Freshness::Unknown;
    }
    return view;
  }

  /// Replays a document's event log. Declared here and defined below, next to
  /// import_document, which is its only caller.
  [[nodiscard]] Result<ImportReport> replay_import(Registry& candidate, const Snapshot& snapshot_value,
                                                   const ImportOptions& options);

  /// Applies every event of a document to a state, in order, either through the
  /// ordinary commit path or purely in memory.
  [[nodiscard]] Result<void> apply_import_events(Registry& state, const Snapshot& snapshot_value,
                                                 std::size_t& events, bool publish_events);

  /// The cross checks a reproduced import must pass: the state digest the
  /// document declares, the counts it declares, and the object states it
  /// describes. A hand edited document cannot pass by rewriting the digest
  /// alone, because the views are checked against the replayed state.
  [[nodiscard]] Result<void> verify_imported_state(const Registry& state, const Snapshot& snapshot_value,
                                                   const ImportOptions& options) const;

  [[nodiscard]] Error unknown_object_error(const ObjectKey& key) const {
    for (const auto& entry : registry.objects()) {
      if (entry.first.asset == key.asset) {
        return Error::make(ErrorCode::StaleHardwareGeneration,
                           "the request names a hardware generation that is not the one on record")
            .with("requested", std::to_string(key.hardware_generation.value()));
      }
    }
    return Error::make(ErrorCode::UnknownAsset, "no object with this identity is known")
        .with("key", to_string(key));
  }
};

std::string_view to_string(DurabilityClass durability) noexcept {
  switch (durability) {
    case DurabilityClass::Durable:
      return "durable";
    case DurabilityClass::Ephemeral:
      return "ephemeral";
  }
  return "ephemeral";
}

Runtime::Runtime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Runtime::Runtime(Runtime&& other) noexcept = default;
Runtime& Runtime::operator=(Runtime&& other) noexcept = default;
Runtime::~Runtime() = default;

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

Result<Runtime> Runtime::open(const OpenOptions& options) {
  auto impl = std::make_unique<Impl>();
  impl->freshness_window = options.health_freshness_window;
  impl->root = options.root;
  impl->registry = Registry(options.registry_limits);

  const Registry empty(options.registry_limits);

  StoreOptions store_options;
  store_options.root = options.root;
  store_options.mode = options.read_only ? StoreMode::ReadOnly
                                         : (options.create_if_missing ? StoreMode::OpenOrCreate
                                                                      : StoreMode::OpenExisting);
  store_options.observer = options.observer;
  store_options.initial_state_digest = empty.state_digest();

  Result<Store> store = Store::open(store_options);
  if (!store.has_value()) {
    return store.error();
  }
  impl->store = std::move(store.value());
  impl->recovery = impl->store->recovery();
  impl->writable = impl->store->writable();
  impl->durability = DurabilityClass::Durable;

  const Result<std::vector<JournalRecord>> records = impl->store->read_records();
  if (!records.has_value()) {
    return records.error();
  }
  for (const JournalRecord& record : records.value()) {
    const Result<detail::RecordPayload> payload = detail::decode_payload(record.kind, record.payload);
    if (!payload.has_value()) {
      return payload.error();
    }
    const Result<void> applied =
        detail::apply_payload(impl->registry, payload.value(), record.sequence, record.logical_time);
    if (!applied.has_value()) {
      return applied.error();
    }
  }

  impl->registry.set_control_epoch(impl->store->control_epoch());

  const Digest replayed = impl->registry.state_digest();
  if (!(replayed == impl->recovery.state_digest)) {
    return Error::make(ErrorCode::IntegrityFailure,
                       "the state replayed from the journal does not match the state digest the manifest published")
        .with("replayed", replayed.hex())
        .with("published", impl->recovery.state_digest.hex());
  }

  const Result<void> invariants = impl->registry.verify_invariants();
  if (!invariants.has_value()) {
    return invariants.error();
  }

  // Authority is never inherited across a process boundary.
  impl->registry.fence_all_authority();
  return Runtime(std::move(impl));
}

Result<Runtime> Runtime::open_read_only(const std::filesystem::path& root) {
  OpenOptions options;
  options.root = root;
  options.read_only = true;
  options.create_if_missing = false;
  return open(options);
}

Result<Runtime> Runtime::open_ephemeral() {
  auto impl = std::make_unique<Impl>();
  impl->durability = DurabilityClass::Ephemeral;
  impl->writable = true;
  impl->registry.set_control_epoch(ControlEpoch::first());
  return Runtime(std::move(impl));
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

Result<CreateReceipt> Runtime::create_object(const CreateRequest& request) {
  if (!impl_->writable) {
    return read_only_error();
  }
  const Result<void> identity = check_plan_identity(request.plan, request.attempt);
  if (!identity.has_value()) {
    return identity.error();
  }
  const Digest request_digest = compute_request_digest(request);
  if (const AppliedPlan* applied = impl_->registry.find_plan(request.plan, request.attempt)) {
    if (!(applied->request_digest == request_digest)) {
      return Error::make(ErrorCode::PlanConflict,
                         "this plan and attempt were already applied with different content")
          .with("plan", request.plan.value());
    }
    return replay_receipt<CreateReceipt>(*applied, "object registration");
  }

  const Result<ValidatedCreate> validated = validate_create(impl_->registry, request);
  if (!validated.has_value()) {
    return validated.error();
  }
  const ValidatedCreate& plan = validated.value();

  const Result<LogicalTime> logical_time = predict_logical_time(impl_->registry, impl_->store);
  if (!logical_time.has_value()) {
    return logical_time.error();
  }
  const Result<CommitSequence> sequence = predict_commit_sequence(impl_->registry, impl_->store);
  if (!sequence.has_value()) {
    return sequence.error();
  }

  CreateReceipt receipt;
  receipt.plan = request.plan;
  receipt.attempt = request.attempt;
  receipt.key = plan.key;
  receipt.state = plan.initial_state;
  receipt.lifecycle_generation = plan.lifecycle_generation;
  receipt.revision = plan.revision;
  receipt.commit_sequence = sequence.value();
  receipt.logical_time = logical_time.value();
  const Digest receipt_digest = compute_receipt_digest(receipt);

  HardwareObject object;
  object.key = plan.key;
  object.kind = request.kind;
  object.model = request.model;
  object.firmware_generation = request.firmware_generation;
  object.lifecycle_generation = plan.lifecycle_generation;
  object.state = plan.initial_state;
  object.revision = plan.revision;
  object.location = plan.location;
  object.eligibility_gate = EligibilityGate::Unknown;
  object.created_at = logical_time.value();
  object.updated_at = logical_time.value();
  object.authority = AuthorityState::Live;

  Provenance provenance = request.provenance;
  provenance.logical_time = logical_time.value();

  HistoryEntry entry;
  entry.commit_sequence = sequence.value();
  entry.logical_time = logical_time.value();
  entry.key = plan.key;
  entry.lifecycle_generation = plan.lifecycle_generation;
  entry.revision_after = plan.revision;
  entry.from = plan.initial_state;
  entry.to = plan.initial_state;
  entry.reason = TransitionReason::ObjectRegistered;
  entry.provenance = provenance;
  entry.receipt_digest = receipt_digest;
  entry.chain_digest = compute_entry_chain_digest(Digest(), entry);

  detail::ObjectRegisteredRecord record;
  record.object = object;
  record.provenance = provenance;
  record.request_digest = request_digest;
  record.receipt_digest = receipt_digest;
  record.entry_chain_digest = entry.chain_digest;

  Registry candidate = impl_->registry;
  const Result<void> published =
      impl_->publish(candidate, detail::RecordPayload(record), sequence.value(), logical_time.value());
  if (!published.has_value()) {
    return published.error();
  }
  impl_->registry = std::move(candidate);

  // The object was created by this session, so this session holds live authority
  // over it. Replay always starts from Recovered; only that explicit step makes
  // it Live, and it is never durable.
  if (HardwareObject* created = impl_->registry.find(plan.key)) {
    created->authority = AuthorityState::Live;
  }

  const Receipt rebuilt =
      detail::reconstruct_receipt(detail::RecordPayload(record), sequence.value(), logical_time.value());
  CreateReceipt issued = std::get<CreateReceipt>(rebuilt);
  issued.idempotent_replay = false;
  return issued;
}

Result<TransitionReceipt> Runtime::apply_transition(const TransitionRequest& request) {
  if (!impl_->writable) {
    return read_only_error();
  }
  const Result<void> identity = check_plan_identity(request.plan, request.attempt);
  if (!identity.has_value()) {
    return identity.error();
  }
  const Digest request_digest = compute_request_digest(request);
  if (const AppliedPlan* applied = impl_->registry.find_plan(request.plan, request.attempt)) {
    if (!(applied->request_digest == request_digest)) {
      return Error::make(ErrorCode::PlanConflict,
                         "this plan and attempt were already applied with different content")
          .with("plan", request.plan.value());
    }
    return replay_receipt<TransitionReceipt>(*applied, "transition");
  }

  const Result<ValidatedTransition> validated = validate_transition(impl_->registry, request);
  if (!validated.has_value()) {
    return validated.error();
  }
  const ValidatedTransition& plan = validated.value();

  const Result<LogicalTime> logical_time = predict_logical_time(impl_->registry, impl_->store);
  if (!logical_time.has_value()) {
    return logical_time.error();
  }
  const Result<CommitSequence> sequence = predict_commit_sequence(impl_->registry, impl_->store);
  if (!sequence.has_value()) {
    return sequence.error();
  }

  TransitionReceipt receipt;
  receipt.plan = request.plan;
  receipt.attempt = request.attempt;
  receipt.key = plan.key;
  receipt.from = plan.from;
  receipt.to = plan.to;
  receipt.reason = plan.reason;
  receipt.revision_before = plan.revision_before;
  receipt.revision_after = plan.revision_after;
  receipt.lifecycle_generation = plan.lifecycle_generation_after;
  receipt.gate_after = plan.gate_after;
  receipt.commit_sequence = sequence.value();
  receipt.logical_time = logical_time.value();
  receipt.request_digest = request_digest;
  const Digest receipt_digest = compute_receipt_digest(receipt);

  Provenance provenance = request.provenance;
  provenance.logical_time = logical_time.value();

  const HistoryLog* log = impl_->registry.history(plan.key);
  const Digest previous_head = log != nullptr ? log->chain_head : Digest();

  HistoryEntry entry;
  entry.commit_sequence = sequence.value();
  entry.logical_time = logical_time.value();
  entry.key = plan.key;
  entry.lifecycle_generation = plan.lifecycle_generation_after;
  entry.revision_before = plan.revision_before;
  entry.revision_after = plan.revision_after;
  entry.from = plan.from;
  entry.to = plan.to;
  entry.reason = plan.reason;
  entry.provenance = provenance;
  entry.receipt_digest = receipt_digest;
  entry.chain_digest = compute_entry_chain_digest(previous_head, entry);

  detail::TransitionAppliedRecord record;
  record.key = plan.key;
  record.from = plan.from;
  record.to = plan.to;
  record.reason = plan.reason;
  record.revision_before = plan.revision_before;
  record.revision_after = plan.revision_after;
  record.lifecycle_generation_before = plan.lifecycle_generation_before;
  record.lifecycle_generation_after = plan.lifecycle_generation_after;
  record.gate_before = plan.gate_before;
  record.gate_after = plan.gate_after;
  record.location_after = plan.location_after;
  record.successor = plan.successor;
  record.provenance = provenance;
  record.request_digest = request_digest;
  record.receipt_digest = receipt_digest;
  record.entry_chain_digest = entry.chain_digest;

  Registry candidate = impl_->registry;
  const Result<void> published =
      impl_->publish(candidate, detail::RecordPayload(record), sequence.value(), logical_time.value());
  if (!published.has_value()) {
    return published.error();
  }
  impl_->registry = std::move(candidate);

  const Receipt rebuilt =
      detail::reconstruct_receipt(detail::RecordPayload(record), sequence.value(), logical_time.value());
  TransitionReceipt issued = std::get<TransitionReceipt>(rebuilt);
  issued.idempotent_replay = false;
  return issued;
}

Result<ReplacementReceipt> Runtime::link_replacement(const ReplacementRequest& request) {
  if (!impl_->writable) {
    return read_only_error();
  }
  const Result<void> identity = check_plan_identity(request.plan, request.attempt);
  if (!identity.has_value()) {
    return identity.error();
  }
  const Digest request_digest = compute_request_digest(request);
  if (const AppliedPlan* applied = impl_->registry.find_plan(request.plan, request.attempt)) {
    if (!(applied->request_digest == request_digest)) {
      return Error::make(ErrorCode::PlanConflict,
                         "this plan and attempt were already applied with different content")
          .with("plan", request.plan.value());
    }
    return replay_receipt<ReplacementReceipt>(*applied, "replacement");
  }

  const Result<ValidatedLink> validated = validate_replacement(impl_->registry, request);
  if (!validated.has_value()) {
    return validated.error();
  }
  const ValidatedLink& plan = validated.value();

  const Result<LogicalTime> logical_time = predict_logical_time(impl_->registry, impl_->store);
  if (!logical_time.has_value()) {
    return logical_time.error();
  }
  const Result<CommitSequence> sequence = predict_commit_sequence(impl_->registry, impl_->store);
  if (!sequence.has_value()) {
    return sequence.error();
  }

  Provenance provenance = request.provenance;
  provenance.logical_time = logical_time.value();

  ReplacementRecord link;
  link.predecessor = plan.predecessor;
  link.successor = plan.successor;
  link.reason = plan.reason;
  link.provenance = provenance;
  link.linked_at = logical_time.value();
  link.commit_sequence = sequence.value();
  link.replacement_generation = plan.replacement_generation;
  link.link_digest = compute_link_digest(link);

  ReplacementReceipt receipt;
  receipt.plan = request.plan;
  receipt.attempt = request.attempt;
  receipt.predecessor = plan.predecessor;
  receipt.successor = plan.successor;
  receipt.replacement_generation = plan.replacement_generation;
  receipt.commit_sequence = sequence.value();
  receipt.logical_time = logical_time.value();
  receipt.link_digest = link.link_digest;
  receipt.request_digest = request_digest;
  const Digest receipt_digest = compute_receipt_digest(receipt);

  detail::SuccessorLinkedRecord record;
  record.link = link;
  record.provenance = provenance;
  record.request_digest = request_digest;
  record.receipt_digest = receipt_digest;

  Registry candidate = impl_->registry;
  const Result<void> published =
      impl_->publish(candidate, detail::RecordPayload(record), sequence.value(), logical_time.value());
  if (!published.has_value()) {
    return published.error();
  }
  impl_->registry = std::move(candidate);

  const Receipt rebuilt =
      detail::reconstruct_receipt(detail::RecordPayload(record), sequence.value(), logical_time.value());
  ReplacementReceipt issued = std::get<ReplacementReceipt>(rebuilt);
  issued.idempotent_replay = false;
  return issued;
}

Result<EligibilityGateReceipt> Runtime::set_eligibility_gate(const EligibilityGateRequest& request) {
  if (!impl_->writable) {
    return read_only_error();
  }
  const Result<void> identity = check_plan_identity(request.plan, request.attempt);
  if (!identity.has_value()) {
    return identity.error();
  }
  const Digest request_digest = compute_request_digest(request);
  if (const AppliedPlan* applied = impl_->registry.find_plan(request.plan, request.attempt)) {
    if (!(applied->request_digest == request_digest)) {
      return Error::make(ErrorCode::PlanConflict,
                         "this plan and attempt were already applied with different content")
          .with("plan", request.plan.value());
    }
    return replay_receipt<EligibilityGateReceipt>(*applied, "eligibility gate");
  }

  const Result<ValidatedGate> validated = validate_gate(impl_->registry, request);
  if (!validated.has_value()) {
    return validated.error();
  }
  const ValidatedGate& plan = validated.value();

  const Result<LogicalTime> logical_time = predict_logical_time(impl_->registry, impl_->store);
  if (!logical_time.has_value()) {
    return logical_time.error();
  }
  const Result<CommitSequence> sequence = predict_commit_sequence(impl_->registry, impl_->store);
  if (!sequence.has_value()) {
    return sequence.error();
  }

  Provenance provenance = request.provenance;
  provenance.logical_time = logical_time.value();

  EligibilityGateReceipt receipt;
  receipt.plan = request.plan;
  receipt.attempt = request.attempt;
  receipt.key = plan.key;
  receipt.before = plan.before;
  receipt.after = plan.after;
  receipt.revision = plan.revision_after;
  receipt.commit_sequence = sequence.value();
  receipt.logical_time = logical_time.value();
  receipt.request_digest = request_digest;
  const Digest receipt_digest = compute_receipt_digest(receipt);

  // A gate decision is a durable mutation of the object, so it appends a
  // history entry exactly like a lifecycle transition does. Its from and to are
  // the same state and its lifecycle generation does not move, because the
  // lifecycle state did not change.
  const HardwareObject* current = impl_->registry.find(plan.key);
  if (current == nullptr) {
    return Error::make(ErrorCode::UnknownAsset, "the object disappeared before the gate change was committed");
  }
  const HistoryLog* gate_log = impl_->registry.history(plan.key);
  const Digest gate_previous_head = gate_log != nullptr ? gate_log->chain_head : Digest();

  HistoryEntry gate_entry;
  gate_entry.commit_sequence = sequence.value();
  gate_entry.logical_time = logical_time.value();
  gate_entry.key = plan.key;
  gate_entry.lifecycle_generation = current->lifecycle_generation;
  gate_entry.revision_before = plan.revision_before;
  gate_entry.revision_after = plan.revision_after;
  gate_entry.from = current->state;
  gate_entry.to = current->state;
  gate_entry.reason = plan.after == EligibilityGate::Open ? TransitionReason::EligibilityGateOpened
                                                          : TransitionReason::EligibilityGateClosed;
  gate_entry.provenance = provenance;
  gate_entry.receipt_digest = receipt_digest;
  gate_entry.chain_digest = compute_entry_chain_digest(gate_previous_head, gate_entry);

  detail::EligibilityGateChangedRecord record;
  record.key = plan.key;
  record.before = plan.before;
  record.after = plan.after;
  record.revision_before = plan.revision_before;
  record.revision_after = plan.revision_after;
  record.provenance = provenance;
  record.request_digest = request_digest;
  record.receipt_digest = receipt_digest;
  record.entry_chain_digest = gate_entry.chain_digest;

  Registry candidate = impl_->registry;
  const Result<void> published =
      impl_->publish(candidate, detail::RecordPayload(record), sequence.value(), logical_time.value());
  if (!published.has_value()) {
    return published.error();
  }
  impl_->registry = std::move(candidate);

  const Receipt rebuilt =
      detail::reconstruct_receipt(detail::RecordPayload(record), sequence.value(), logical_time.value());
  EligibilityGateReceipt issued = std::get<EligibilityGateReceipt>(rebuilt);
  issued.idempotent_replay = false;
  return issued;
}

Result<HealthReceipt> Runtime::observe_health(const HealthObservationRequest& request) {
  if (!impl_->writable) {
    return read_only_error();
  }
  const Result<void> identity = check_plan_identity(request.plan, request.attempt);
  if (!identity.has_value()) {
    return identity.error();
  }
  const Digest request_digest = compute_request_digest(request);
  if (const AppliedPlan* applied = impl_->registry.find_plan(request.plan, request.attempt)) {
    if (!(applied->request_digest == request_digest)) {
      return Error::make(ErrorCode::PlanConflict,
                         "this plan and attempt were already applied with different content")
          .with("plan", request.plan.value());
    }
    return replay_receipt<HealthReceipt>(*applied, "health observation");
  }

  const Result<void> validated = validate_observation(impl_->registry, request);
  if (!validated.has_value()) {
    return validated.error();
  }

  const Result<LogicalTime> logical_time = predict_logical_time(impl_->registry, impl_->store);
  if (!logical_time.has_value()) {
    return logical_time.error();
  }
  const Result<CommitSequence> sequence = predict_commit_sequence(impl_->registry, impl_->store);
  if (!sequence.has_value()) {
    return sequence.error();
  }
  const Result<ObservationSequence> observation = impl_->registry.next_observation_sequence();
  if (!observation.has_value()) {
    return observation.error();
  }

  Provenance provenance = request.provenance;
  provenance.logical_time = logical_time.value();

  HealthReceipt receipt;
  receipt.plan = request.plan;
  receipt.attempt = request.attempt;
  receipt.key = request.key;
  receipt.sequence = observation.value();
  receipt.commit_sequence = sequence.value();
  receipt.logical_time = logical_time.value();
  receipt.request_digest = request_digest;
  const Digest receipt_digest = compute_receipt_digest(receipt);

  detail::HealthObservedRecord record;
  record.observation = request.observation;
  record.observation.key = request.key;
  record.sequence = observation.value();
  record.provenance = provenance;
  record.request_digest = request_digest;
  record.receipt_digest = receipt_digest;

  Registry candidate = impl_->registry;
  const Result<void> published =
      impl_->publish(candidate, detail::RecordPayload(record), sequence.value(), logical_time.value());
  if (!published.has_value()) {
    return published.error();
  }
  impl_->registry = std::move(candidate);

  const Receipt rebuilt =
      detail::reconstruct_receipt(detail::RecordPayload(record), sequence.value(), logical_time.value());
  HealthReceipt issued = std::get<HealthReceipt>(rebuilt);
  issued.idempotent_replay = false;
  return issued;
}

Result<AttestationReceipt> Runtime::attest_authority(const AttestationRequest& request) {
  if (!impl_->writable) {
    return read_only_error();
  }
  const Result<void> identity = check_plan_identity(request.plan, request.attempt);
  if (!identity.has_value()) {
    return identity.error();
  }
  const Digest request_digest = compute_request_digest(request);
  if (const AppliedPlan* applied = impl_->registry.find_plan(request.plan, request.attempt)) {
    if (!(applied->request_digest == request_digest)) {
      return Error::make(ErrorCode::PlanConflict,
                         "this plan and attempt were already applied with different content")
          .with("plan", request.plan.value());
    }
    return replay_receipt<AttestationReceipt>(*applied, "attestation");
  }

  const Result<ValidatedAttestation> validated = validate_attestation(impl_->registry, request);
  if (!validated.has_value()) {
    return validated.error();
  }
  const ValidatedAttestation& plan = validated.value();

  const Result<LogicalTime> logical_time = predict_logical_time(impl_->registry, impl_->store);
  if (!logical_time.has_value()) {
    return logical_time.error();
  }
  const Result<CommitSequence> sequence = predict_commit_sequence(impl_->registry, impl_->store);
  if (!sequence.has_value()) {
    return sequence.error();
  }

  Provenance provenance = request.provenance;
  provenance.logical_time = logical_time.value();

  AttestationReceipt receipt;
  receipt.plan = request.plan;
  receipt.attempt = request.attempt;
  receipt.key = plan.key;
  receipt.revision = plan.revision;
  receipt.commit_sequence = sequence.value();
  receipt.logical_time = logical_time.value();
  receipt.request_digest = request_digest;
  const Digest receipt_digest = compute_receipt_digest(receipt);

  detail::AuthorityAttestedRecord record;
  record.key = plan.key;
  record.revision = plan.revision;
  record.provenance = provenance;
  record.request_digest = request_digest;
  record.receipt_digest = receipt_digest;

  Registry candidate = impl_->registry;
  const Result<void> published =
      impl_->publish(candidate, detail::RecordPayload(record), sequence.value(), logical_time.value());
  if (!published.has_value()) {
    return published.error();
  }
  impl_->registry = std::move(candidate);
  if (HardwareObject* object = impl_->registry.find(plan.key)) {
    object->authority = AuthorityState::Live;
  }

  const Receipt rebuilt =
      detail::reconstruct_receipt(detail::RecordPayload(record), sequence.value(), logical_time.value());
  AttestationReceipt issued = std::get<AttestationReceipt>(rebuilt);
  issued.idempotent_replay = false;
  return issued;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Result<ObjectView> Runtime::inspect(const ObjectKey& key) const {
  const HardwareObject* object = impl_->registry.find(key);
  if (object == nullptr) {
    return impl_->unknown_object_error(key);
  }
  return impl_->view_of(*object);
}

Result<std::vector<ObjectView>> Runtime::list(const ListQuery& query) const {
  std::vector<ObjectView> views;
  for (const auto& entry : impl_->registry.objects()) {
    const HardwareObject& object = entry.second;
    if (query.state.has_value() && !(object.state == query.state.value())) {
      continue;
    }
    if (query.kind.has_value() && !(object.kind == query.kind.value())) {
      continue;
    }
    if (query.site.has_value()) {
      if (!object.location.has_value() || !(object.location->site == query.site.value())) {
        continue;
      }
    }
    ObjectView view = impl_->view_of(object);
    if (query.eligibility.has_value() && !(view.eligibility == query.eligibility.value())) {
      continue;
    }
    views.push_back(std::move(view));
  }
  return views;
}

Result<HistoryView> Runtime::history(const ObjectKey& key) const {
  if (impl_->registry.find(key) == nullptr) {
    return impl_->unknown_object_error(key);
  }
  const HistoryLog* log = impl_->registry.history(key);
  HistoryView view;
  view.key = key;
  if (log != nullptr) {
    view.entries = log->entries;
    view.chain_head = log->chain_head;
  }
  return view;
}

Result<LineageView> Runtime::lineage(const ObjectKey& key) const {
  if (impl_->registry.find(key) == nullptr) {
    return impl_->unknown_object_error(key);
  }
  const LineageGraph& graph = impl_->registry.lineage();
  LineageView view;
  view.key = key;

  const Result<std::vector<ObjectKey>> ancestors = graph.ancestry_of(key);
  if (!ancestors.has_value()) {
    return ancestors.error();
  }
  view.ancestors = ancestors.value();

  const Result<std::vector<ObjectKey>> successors = graph.chain_from(key);
  if (!successors.has_value()) {
    return successors.error();
  }
  view.successors = successors.value();

  std::vector<ObjectKey> chain = view.ancestors;
  chain.push_back(key);
  for (const ObjectKey& item : view.successors) {
    chain.push_back(item);
  }
  for (const ObjectKey& item : chain) {
    const ReplacementRecord* link = graph.successor_of(item);
    if (link != nullptr) {
      view.links.push_back(*link);
    }
  }
  return view;
}

Result<std::vector<ReplacementRecord>> Runtime::all_links() const {
  std::vector<ReplacementRecord> links;
  for (const auto& entry : impl_->registry.lineage().records()) {
    links.push_back(entry.second);
  }
  return links;
}

Result<PreflightReport> Runtime::preflight_transition(const TransitionRequest& request) const {
  PreflightReport report;
  report.outcome = PreflightOutcome::Rejected;
  if (!request.plan.valid() || !request.attempt.valid()) {
    report.primary_error = Error::make(ErrorCode::MalformedRequest, "the request carries no plan id or no attempt id");
    return report;
  }
  const Digest request_digest = compute_request_digest(request);
  if (const AppliedPlan* applied = impl_->registry.find_plan(request.plan, request.attempt)) {
    if (!(applied->request_digest == request_digest)) {
      report.primary_error = Error::make(ErrorCode::PlanConflict,
                                         "this plan and attempt were already applied with different content");
      return report;
    }
    report.outcome = PreflightOutcome::AlreadyApplied;
    report.applied = applied;
    return report;
  }
  const Result<ValidatedTransition> validated = validate_transition(impl_->registry, request);
  if (!validated.has_value()) {
    report.primary_error = validated.error();
    return report;
  }
  report.outcome = PreflightOutcome::Legal;
  report.transition = validated.value();
  return report;
}

Result<ValidatedCreate> Runtime::preflight_create(const CreateRequest& request) const {
  return validate_create(impl_->registry, request);
}

Result<ValidatedLink> Runtime::preflight_replacement(const ReplacementRequest& request) const {
  return validate_replacement(impl_->registry, request);
}

Result<ValidatedGate> Runtime::preflight_gate(const EligibilityGateRequest& request) const {
  return validate_gate(impl_->registry, request);
}

Result<RuntimeStatus> Runtime::status() const {
  RuntimeStatus status;
  status.durability = impl_->durability;
  status.read_only = !impl_->writable;
  status.writable = impl_->writable;
  status.commit_sequence = impl_->registry.commit_sequence();
  status.logical_time = impl_->registry.logical_time();
  status.observation_sequence = impl_->registry.observation_sequence();
  status.control_epoch = impl_->registry.control_epoch();
  status.incarnation = impl_->recovery.incarnation;
  status.object_count = impl_->registry.object_count();
  status.history_entry_count = impl_->registry.history_entry_count();
  status.lineage_links = impl_->registry.lineage().size();
  status.applied_plans = impl_->registry.plan_count();
  status.recovery = impl_->recovery;
  return status;
}

Result<Digest> Runtime::verify() const {
  const Result<void> invariants = impl_->registry.verify_invariants();
  if (!invariants.has_value()) {
    return invariants.error();
  }
  if (!impl_->store.has_value()) {
    return impl_->registry.state_digest();
  }
  const Result<Digest> log_digest = impl_->store->verify();
  if (!log_digest.has_value()) {
    return log_digest.error();
  }
  const Result<std::vector<JournalRecord>> records = impl_->store->read_records();
  if (!records.has_value()) {
    return records.error();
  }
  Registry replay(impl_->registry.limits());
  for (const JournalRecord& record : records.value()) {
    const Result<detail::RecordPayload> payload = detail::decode_payload(record.kind, record.payload);
    if (!payload.has_value()) {
      return payload.error();
    }
    const Result<void> applied = detail::apply_payload(replay, payload.value(), record.sequence, record.logical_time);
    if (!applied.has_value()) {
      return applied.error();
    }
  }
  const Digest replayed = replay.state_digest();
  if (!(replayed == impl_->registry.state_digest())) {
    return Error::make(ErrorCode::IntegrityFailure,
                       "the state held in memory does not match the state the journal replays to")
        .with("in_memory", impl_->registry.state_digest().hex())
        .with("replayed", replayed.hex());
  }
  const Result<void> replayed_invariants = replay.verify_invariants();
  if (!replayed_invariants.has_value()) {
    return replayed_invariants.error();
  }
  return log_digest.value();
}

Snapshot Runtime::snapshot(const ExportOptions& options) const {
  std::vector<ExportedRecord> events;
  if (options.include_events) {
    events = impl_->exported_events();
  }
  return snapshot_of(impl_->registry, events, impl_->registry.control_epoch(), impl_->recovery.incarnation, options);
}

Result<std::string> Runtime::export_document(const ExportOptions& options) const {
  return to_json(snapshot(options), options);
}

Result<SnapshotDiff> Runtime::diff_against(const Snapshot& other) const {
  ExportOptions options;
  return diff_snapshots(other, snapshot(options));
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

Result<ImportReport> Runtime::import_document(std::string_view document, const ImportOptions& options) {
  if (!impl_->writable) {
    return read_only_error();
  }
  if (impl_->registry.object_count() != 0 || impl_->registry.commit_sequence().valid()) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "a document is imported into an empty store; importing into a populated store would merge two "
                       "authorities");
  }

  const Result<Snapshot> decoded = from_json(document);
  if (!decoded.has_value()) {
    return decoded.error();
  }
  const Snapshot& snapshot_value = decoded.value();
  if (!snapshot_value.header.importable || snapshot_value.events.empty()) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "this document carries no durable event log, so the state it describes cannot be reproduced");
  }
  if (snapshot_value.header.event_count != snapshot_value.events.size()) {
    return Error::make(ErrorCode::MalformedRequest, "the declared event count does not match the event log");
  }

  Registry candidate = impl_->registry;
  const Result<ImportReport> result = impl_->replay_import(candidate, snapshot_value, options);

  // Whatever the outcome, the runtime adopts exactly the records that were
  // published. A refused import therefore leaves a store that is consistent
  // with its own journal and with this session's view of it, rather than a
  // durable log that has run ahead of the state in memory.
  impl_->registry = std::move(candidate);
  return result;
}

/// Applies every event of a document to a state, in order.
///
/// The same function serves both passes of an import, so the dry run and the
/// published run cannot interpret a document differently.
Result<void> Runtime::Impl::apply_import_events(Registry& state, const Snapshot& snapshot_value,
                                                std::size_t& events, bool publish_events) {
  for (const ExportedRecord& event : snapshot_value.events) {
    if (!event.sequence.valid() || !event.logical_time.valid()) {
      return Error::make(ErrorCode::MalformedRequest, "an event carries a zero sequence or logical time");
    }
    const Result<detail::RecordPayload> payload = detail::decode_payload(event.kind, event.payload);
    if (!payload.has_value()) {
      return payload.error();
    }
    const Result<CommitSequence> expected = state.commit_sequence().next();
    if (!expected.has_value()) {
      return expected.error();
    }
    if (!(event.sequence == expected.value())) {
      return Error::make(ErrorCode::IntegrityFailure, "the event log is not contiguous")
          .with("event_sequence", std::to_string(event.sequence.value()))
          .with("expected", std::to_string(expected.value().value()));
    }
    if (publish_events) {
      // The candidate is only advanced for records that were actually
      // published, so a failure leaves it holding exactly the published prefix.
      Registry next = state;
      const Result<void> published = publish(next, payload.value(), event.sequence, event.logical_time);
      if (!published.has_value()) {
        return published.error();
      }
      state = std::move(next);
    } else {
      const Result<void> applied = detail::apply_payload(state, payload.value(), event.sequence, event.logical_time);
      if (!applied.has_value()) {
        return applied.error();
      }
    }
    ++events;
  }
  return ok();
}

Result<void> Runtime::Impl::verify_imported_state(const Registry& state, const Snapshot& snapshot_value,
                                                  const ImportOptions& options) const {
  const Digest produced = state.state_digest();
  if (options.verify_state_digest && !(produced == snapshot_value.header.state_digest)) {
    return Error::make(ErrorCode::IntegrityFailure,
                       "the state reproduced from the event log does not match the digest the document declares")
        .with("declared", snapshot_value.header.state_digest.hex())
        .with("produced", produced.hex());
  }
  if (snapshot_value.header.object_count != state.object_count()) {
    return Error::make(ErrorCode::IntegrityFailure, "the declared object count does not match the replayed state");
  }
  if (snapshot_value.header.history_entry_count != state.history_entry_count()) {
    return Error::make(ErrorCode::IntegrityFailure, "the declared history count does not match the replayed state");
  }
  if (snapshot_value.header.lineage_link_count != state.lineage().size()) {
    return Error::make(ErrorCode::IntegrityFailure, "the declared lineage count does not match the replayed state");
  }
  if (snapshot_value.header.applied_plan_count != state.plan_count()) {
    return Error::make(ErrorCode::IntegrityFailure, "the declared plan count does not match the replayed state");
  }
  for (const HardwareObject& declared : snapshot_value.objects) {
    const HardwareObject* replayed = state.find(declared.key);
    if (replayed == nullptr) {
      return Error::make(ErrorCode::IntegrityFailure, "the document declares an object the event log never created")
          .with("key", to_string(declared.key));
    }
    if (replayed->state != declared.state || replayed->revision != declared.revision ||
        replayed->lifecycle_generation != declared.lifecycle_generation ||
        replayed->eligibility_gate != declared.eligibility_gate ||
        replayed->history_entries != declared.history_entries) {
      return Error::make(ErrorCode::IntegrityFailure,
                         "the document declares an object state the event log does not reproduce")
          .with("key", to_string(declared.key));
    }
    if (replayed->location.has_value() != declared.location.has_value()) {
      return Error::make(ErrorCode::IntegrityFailure, "the document declares a location the event log does not reproduce")
          .with("key", to_string(declared.key));
    }
    if (replayed->location.has_value() && !(replayed->location.value() == declared.location.value())) {
      return Error::make(ErrorCode::IntegrityFailure, "the document declares a location the event log does not reproduce")
          .with("key", to_string(declared.key));
    }
  }
  return state.verify_invariants();
}

/// Replays a document's event log onto a candidate state.
///
/// Import is all or nothing at the document level. The first pass reproduces
/// the whole state in memory and runs every cross check without publishing
/// anything, so a document that is refused leaves the store exactly as it was:
/// a half imported store would be a merged authority, which is the one thing an
/// import must never produce. Only a document that has already been reproduced
/// exactly is then published, record by record, through the ordinary commit
/// path.
Result<ImportReport> Runtime::Impl::replay_import(Registry& candidate, const Snapshot& snapshot_value,
                                                  const ImportOptions& options) {
  std::size_t events = 0;
  {
    Registry dry = candidate;
    const Result<void> reproduced = apply_import_events(dry, snapshot_value, events, false);
    if (!reproduced.has_value()) {
      return reproduced.error();
    }
    const Result<void> checked = verify_imported_state(dry, snapshot_value, options);
    if (!checked.has_value()) {
      return checked.error();
    }
  }

  events = 0;
  const Result<void> published = apply_import_events(candidate, snapshot_value, events, true);
  if (!published.has_value()) {
    return published.error();
  }

  ImportReport report;
  report.events = events;
  report.declared_state_digest = snapshot_value.header.state_digest;
  report.produced_state_digest = candidate.state_digest();
  report.objects = candidate.object_count();
  report.history_entries = candidate.history_entry_count();
  report.lineage_links = candidate.lineage().size();
  report.applied_plans = candidate.plan_count();
  report.health_observations = candidate.health_snapshots().size();
  report.commit_sequence = candidate.commit_sequence();
  report.logical_time = candidate.logical_time();
  return report;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const RecoveryReport& Runtime::recovery() const noexcept { return impl_->recovery; }

DurabilityClass Runtime::durability() const noexcept { return impl_->durability; }

bool Runtime::writable() const noexcept { return impl_->writable; }

const Registry& Runtime::registry() const noexcept { return impl_->registry; }

const Store* Runtime::store() const noexcept { return impl_->store.has_value() ? &impl_->store.value() : nullptr; }

const std::filesystem::path& Runtime::root() const noexcept { return impl_->root; }

}  // namespace hardware_lifecycle
