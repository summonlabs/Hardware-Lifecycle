// Hardware Lifecycle - the one validator, and the request and receipt digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Checks are staged in rank bands, and the bands are ordered exactly like the
// rank table in errors.hpp: the highest rank any check in a band can produce is
// strictly lower than the lowest rank any check in the next band can produce.
// Within a band every violation is collected and the lowest ranked one becomes
// the primary error, so the reported error never depends on the order the checks
// happen to run in.
//
// Plan identity resolution sits deliberately between the structural band and
// the staleness band. A request whose plan and attempt were already applied with
// the same semantic content returns the receipt that was issued the first time,
// even though every generation, revision and epoch it carries is now stale.
// A lost response must be replayable; only a genuinely new plan is judged
// against the state it was planned against.

#include "hardware_lifecycle/requests.hpp"

#include <string>
#include <utility>
#include <variant>

#include "hardware_lifecycle/registry.hpp"
#include "hardware_lifecycle/text.hpp"
#include "serialization.hpp"

namespace hardware_lifecycle {
namespace {

/// Collects violations and keeps the most primary one.
class IssueLog {
 public:
  void report(ErrorCode code, std::string message) {
    if (any_ && validation_rank(code) >= validation_rank(primary_.code)) {
      return;
    }
    primary_ = Error::make(code, std::move(message));
    any_ = true;
  }

  void report(ErrorCode code, std::string message, std::string field, std::string detail) {
    if (any_ && validation_rank(code) >= validation_rank(primary_.code)) {
      return;
    }
    primary_ = Error::make(code, std::move(message)).with(std::move(field), std::move(detail));
    any_ = true;
  }

  [[nodiscard]] bool any() const noexcept { return any_; }
  [[nodiscard]] const Error& primary() const noexcept { return primary_; }

 private:
  Error primary_;
  bool any_ = false;
};

[[nodiscard]] bool enum_in_range(std::uint8_t raw, std::uint8_t count) noexcept { return raw < count; }

[[nodiscard]] std::uint8_t raw(LifecycleState state) noexcept { return static_cast<std::uint8_t>(state); }

[[nodiscard]] std::string key_text(const ObjectKey& key) { return to_string(key); }

/// Structural checks shared by every request that carries a plan identity.
void check_plan_identity(const PlanId& plan, const AttemptId& attempt, IssueLog& log) {
  if (!plan.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request carries no plan id", "plan", "missing");
  }
  if (!attempt.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request carries no attempt id", "attempt", "missing");
  }
}

/// A request carries its plan and attempt twice: once as the identity of the
/// intent and once inside the provenance that the durable record stores. They
/// must agree, or the ledger would be keyed by one identity while the caller
/// believes it holds the other, and the disagreement would only surface as an
/// integrity failure deep inside the commit path.
void check_plan_identity_agreement(const PlanId& plan, const AttemptId& attempt, const Provenance& provenance,
                                   IssueLog& log) {
  if (plan.valid() && provenance.plan.valid() && !(plan == provenance.plan)) {
    log.report(ErrorCode::MalformedRequest, "the request and its provenance name different plans", "plan",
               plan.value());
  }
  if (attempt.valid() && provenance.attempt.valid() && !(attempt == provenance.attempt)) {
    log.report(ErrorCode::MalformedRequest, "the request and its provenance name different attempts", "attempt",
               attempt.value());
  }
}

void check_authority(const Provenance& provenance, AuthorityMask required, IssueLog& log) {
  if (!provenance.authority.intersects(required)) {
    log.report(ErrorCode::InsufficientAuthority, "the request does not carry the authority this change requires",
               "required", to_string(required));
  }
}

void check_evidence(const Provenance& provenance, EvidenceMask required, IssueLog& log) {
  if (provenance.evidence.empty()) {
    log.report(ErrorCode::MissingEvidence, "every externally meaningful change must bind to evidence",
               "required", required.empty() ? std::string("any") : to_string(required));
    return;
  }
  if (!required.empty() && !has_evidence(provenance, required)) {
    log.report(ErrorCode::MissingEvidence, "the request does not carry the evidence kind this change requires",
               "required", to_string(required));
  }
}

void check_stale_authority(ControlEpoch expected, ControlEpoch current, IssueLog& log) {
  if (expected != current) {
    log.report(ErrorCode::StaleAuthority, "the request was planned against a different writer epoch",
               "expected_control_epoch", std::to_string(expected.value()));
  }
}

/// Resolves the object key, distinguishing "this generation never existed" from
/// "this generation was superseded".
[[nodiscard]] const HardwareObject* resolve(const Registry& registry, const ObjectKey& key, IssueLog& log) {
  const HardwareObject* object = registry.find(key);
  if (object != nullptr) {
    return object;
  }
  for (const auto& entry : registry.objects()) {
    if (entry.first.asset == key.asset) {
      log.report(ErrorCode::StaleHardwareGeneration, "the request names a hardware generation that is not the one on record",
                 "requested", std::to_string(key.hardware_generation.value()));
      return nullptr;
    }
  }
  log.report(ErrorCode::UnknownAsset, "no object with this identity is known", "key", key_text(key));
  return nullptr;
}

[[nodiscard]] const AppliedPlan* resolve_plan(const Registry& registry, const PlanId& plan, const AttemptId& attempt,
                                              const Digest& request_digest, IssueLog& log) {
  const AppliedPlan* applied = registry.find_plan(plan, attempt);
  if (applied == nullptr) {
    return nullptr;
  }
  if (applied->request_digest != request_digest) {
    log.report(ErrorCode::PlanConflict,
               "this plan and attempt were already applied with different content; a plan identity may not be reused",
               "plan", plan.value());
  }
  return applied;
}

}  // namespace

std::string_view to_string(PreflightOutcome outcome) noexcept {
  switch (outcome) {
    case PreflightOutcome::Legal:
      return "legal";
    case PreflightOutcome::Rejected:
      return "rejected";
    case PreflightOutcome::AlreadyApplied:
      return "already_applied";
  }
  return "rejected";
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

Digest compute_request_digest(const CreateRequest& request) {
  const std::vector<std::uint8_t> bytes = detail::canonical_request_bytes(request);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_request_digest(const TransitionRequest& request) {
  const std::vector<std::uint8_t> bytes = detail::canonical_request_bytes(request);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_request_digest(const ReplacementRequest& request) {
  const std::vector<std::uint8_t> bytes = detail::canonical_request_bytes(request);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_request_digest(const EligibilityGateRequest& request) {
  const std::vector<std::uint8_t> bytes = detail::canonical_request_bytes(request);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_request_digest(const HealthObservationRequest& request) {
  const std::vector<std::uint8_t> bytes = detail::canonical_request_bytes(request);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_request_digest(const AttestationRequest& request) {
  const std::vector<std::uint8_t> bytes = detail::canonical_request_bytes(request);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_receipt_digest(const CreateReceipt& receipt) {
  const std::vector<std::uint8_t> bytes = detail::canonical_receipt_bytes(receipt);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_receipt_digest(const TransitionReceipt& receipt) {
  const std::vector<std::uint8_t> bytes = detail::canonical_receipt_bytes(receipt);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_receipt_digest(const ReplacementReceipt& receipt) {
  const std::vector<std::uint8_t> bytes = detail::canonical_receipt_bytes(receipt);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_receipt_digest(const EligibilityGateReceipt& receipt) {
  const std::vector<std::uint8_t> bytes = detail::canonical_receipt_bytes(receipt);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_receipt_digest(const HealthReceipt& receipt) {
  const std::vector<std::uint8_t> bytes = detail::canonical_receipt_bytes(receipt);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_receipt_digest(const AttestationReceipt& receipt) {
  const std::vector<std::uint8_t> bytes = detail::canonical_receipt_bytes(receipt);
  return Sha256::hash(bytes.data(), bytes.size());
}

Digest compute_receipt_digest(const Receipt& receipt) {
  return std::visit([](const auto& typed) { return compute_receipt_digest(typed); }, receipt);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Result<void> validate_transition_shape(const TransitionRequest& request) {
  IssueLog log;
  check_plan_identity(request.plan, request.attempt, log);
  check_plan_identity_agreement(request.plan, request.attempt, request.provenance, log);
  if (!request.key.asset.valid() || !request.key.hardware_generation.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request names an incomplete object key", "key", key_text(request.key));
  }
  if (!enum_in_range(raw(request.expected_state), static_cast<std::uint8_t>(kLifecycleStateCount)) ||
      !enum_in_range(raw(request.target_state), static_cast<std::uint8_t>(kLifecycleStateCount))) {
    log.report(ErrorCode::MalformedRequest, "the request names a lifecycle state that does not exist");
  }
  if (request.reason == TransitionReason::Unset) {
    log.report(ErrorCode::MalformedRequest, "every transition request must carry a reason", "reason", "unset");
  }
  if (!request.expected_revision.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request must name the revision it was planned against",
               "expected_revision", "missing");
  }
  if (!request.expected_lifecycle_generation.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request must name the lifecycle generation it was planned against",
               "expected_lifecycle_generation", "missing");
  }
  if (!request.expected_control_epoch.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request must name the writer epoch it was planned against",
               "expected_control_epoch", "missing");
  }
  if (request.successor.has_value()) {
    const ObjectKey& successor = request.successor.value();
    if (!successor.asset.valid() || !successor.hardware_generation.valid()) {
      log.report(ErrorCode::MalformedRequest, "the successor key is incomplete", "successor", key_text(successor));
    }
  }
  if (request.location.has_value() && !request.location.value().valid()) {
    log.report(ErrorCode::MalformedRequest, "the supplied location is incomplete");
  }
  return log.any() ? Result<void>(log.primary()) : Result<void>();
}

Result<ValidatedCreate> validate_create(const Registry& registry, const CreateRequest& request) {
  IssueLog log;
  check_plan_identity(request.plan, request.attempt, log);
  check_plan_identity_agreement(request.plan, request.attempt, request.provenance, log);
  if (!request.key.asset.valid() || !request.key.hardware_generation.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request names an incomplete object key", "key", key_text(request.key));
  }
  if (request.kind == HardwareKind::Unknown) {
    log.report(ErrorCode::MalformedRequest, "an object must be registered with a hardware kind", "kind", "unknown");
  }
  if (!request.model.valid()) {
    log.report(ErrorCode::MalformedRequest, "an object must be registered with a model", "model", "missing");
  }
  if (request.initial_state != LifecycleState::Ordered && request.initial_state != LifecycleState::Staged) {
    log.report(ErrorCode::MalformedRequest,
               "an object may only be registered as ordered or staged; every later state is reached through the "
               "transition table",
               "initial_state", std::string(to_string(request.initial_state)));
  }
  if (request.location.has_value() && !request.location.value().valid()) {
    log.report(ErrorCode::MalformedRequest, "the supplied location is incomplete");
  }
  if (!log.any()) {
    const Result<void> provenance = validate_provenance(request.provenance);
    if (!provenance.has_value()) {
      log.report(provenance.error().code, provenance.error().message);
    }
  }
  if (log.any()) {
    return log.primary();
  }

  const Digest request_digest = compute_request_digest(request);
  const AppliedPlan* applied = resolve_plan(registry, request.plan, request.attempt, request_digest, log);
  if (log.any()) {
    return log.primary();
  }
  if (applied != nullptr) {
    // Handled by the runtime, which returns the recorded receipt.
    return Error::make(ErrorCode::PlanConflict, "the plan was already applied");
  }

  if (registry.contains(request.key)) {
    log.report(ErrorCode::ObjectExists, "an object with this identity is already registered", "key",
               key_text(request.key));
  }
  for (const auto& entry : registry.objects()) {
    if (entry.first.asset == request.key.asset && !(entry.first == request.key)) {
      log.report(ErrorCode::ObjectExists, "this asset already has a hardware generation on record", "asset",
                 request.key.asset.value());
    }
  }
  const bool staged = request.initial_state == LifecycleState::Staged;
  check_authority(request.provenance, authority_bit(staged ? AuthorityScope::Logistics : AuthorityScope::Procurement),
                  log);
  check_evidence(request.provenance, evidence_bit(staged ? EvidenceKind::DeliveryReceipt
                                                         : EvidenceKind::ProcurementRecord),
                 log);
  if (registry.object_count() >= registry.limits().max_objects) {
    log.report(ErrorCode::LimitExceeded, "the registry holds as many objects as it may");
  }
  if (log.any()) {
    return log.primary();
  }

  ValidatedCreate validated;
  validated.key = request.key;
  validated.initial_state = request.initial_state;
  validated.revision = Revision::first();
  validated.lifecycle_generation = LifecycleGeneration::first();
  validated.location = request.location;
  return validated;
}

Result<ValidatedTransition> validate_transition(const Registry& registry, const TransitionRequest& request) {
  const Result<void> shape = validate_transition_shape(request);
  IssueLog log;
  if (!shape.has_value()) {
    log.report(shape.error().code, shape.error().message);
  } else {
    const Result<void> provenance = validate_provenance(request.provenance);
    if (!provenance.has_value()) {
      log.report(provenance.error().code, provenance.error().message);
    }
  }
  if (log.any()) {
    return log.primary();
  }

  const Digest request_digest = compute_request_digest(request);
  const AppliedPlan* applied = resolve_plan(registry, request.plan, request.attempt, request_digest, log);
  if (log.any()) {
    return log.primary();
  }
  if (applied != nullptr) {
    return Error::make(ErrorCode::PlanConflict, "the plan was already applied");
  }

  const HardwareObject* object = resolve(registry, request.key, log);
  if (object == nullptr) {
    return log.primary();
  }

  check_stale_authority(request.expected_control_epoch, registry.control_epoch(), log);
  if (object->authority != AuthorityState::Live) {
    log.report(ErrorCode::StaleAuthority,
               "this object came back from a restart and its authority has not been re-established in this session; "
               "attest it before changing it",
               "authority", std::string(to_string(object->authority)));
  }
  if (object->lifecycle_generation != request.expected_lifecycle_generation) {
    log.report(ErrorCode::StaleLifecycleGeneration,
               "the request was planned against a different lifecycle generation", "expected",
               std::to_string(request.expected_lifecycle_generation.value()));
  }
  if (object->revision != request.expected_revision) {
    log.report(ErrorCode::StaleRevision, "the request was planned against a different revision", "expected",
               std::to_string(request.expected_revision.value()));
  }
  if (object->state != request.expected_state) {
    log.report(ErrorCode::StateMismatch, "the object is not in the state the request was planned against", "expected",
               std::string(to_string(request.expected_state)));
  }
  if (log.any()) {
    return log.primary();
  }

  const TransitionRule* rule = find_rule(object->state, request.target_state);
  if (rule == nullptr) {
    log.report(ErrorCode::IllegalTransition, "the transition table has no edge from this state to that state",
               "from", std::string(to_string(object->state)));
    log.report(ErrorCode::IllegalTransition, "the transition table has no edge from this state to that state", "to",
               std::string(to_string(request.target_state)));
    return log.primary();
  }
  if (!reason_allowed(rule->allowed_reasons, request.reason)) {
    log.report(ErrorCode::IllegalTransition, "this transition does not accept that reason", "allowed",
               to_string(rule->allowed_reasons));
  }
  check_authority(request.provenance, rule->required_authority, log);
  check_evidence(request.provenance, rule->required_evidence, log);

  const bool entering_service = occupies_service_scope(rule->to) && !occupies_service_scope(rule->from);
  if (entering_service && object->eligibility_gate != EligibilityGate::Open) {
    log.report(ErrorCode::InsufficientAuthority,
               "the service eligibility gate is not open; returning an object to service requires an explicit gate "
               "decision, which no transition may make on its own",
               "eligibility_gate", std::string(to_string(object->eligibility_gate)));
  }

  std::optional<Location> location_after = object->location;
  if (request.location.has_value()) {
    location_after = request.location;
  }
  if (rule->requires_location && !location_after.has_value()) {
    log.report(ErrorCode::MissingLocation, "the target state requires an assigned facility location", "state",
               std::string(to_string(rule->to)));
  }

  std::optional<ObjectKey> successor;
  if (rule->requires_successor_link) {
    if (!request.successor.has_value()) {
      log.report(ErrorCode::MissingSuccessor, "this transition must name the successor object that replaces it",
                 "state", std::string(to_string(rule->to)));
    } else {
      successor = request.successor;
      const Result<void> pair = validate_replacement_pair(object->key, successor.value());
      if (!pair.has_value()) {
        log.report(pair.error().code, pair.error().message);
      }
      const HardwareObject* successor_object = resolve(registry, successor.value(), log);
      if (successor_object != nullptr) {
        if (rule->requires_successor_installed && !(is_physical(successor_object->state) &&
                                                    successor_object->state != LifecycleState::Staged &&
                                                    successor_object->state != LifecycleState::Removed)) {
          log.report(ErrorCode::InvalidSuccessor,
                     "the successor must already be installed or further along before it may replace anything",
                     "successor_state", std::string(to_string(successor_object->state)));
        }
        if (object->successor.has_value() && !(object->successor.value() == successor.value())) {
          log.report(ErrorCode::InvalidSuccessor, "the object already names a different successor");
        }
      }
      const ReplacementRecord* link = registry.lineage().successor_of(object->key);
      if (link == nullptr || !(link->successor == successor.value())) {
        log.report(ErrorCode::MissingSuccessor,
                   "the replacement link has not been recorded; link the successor first, then mark the "
                   "predecessor as replaced");
      }
    }
  } else if (request.successor.has_value()) {
    log.report(ErrorCode::InvalidSuccessor, "this transition does not carry a successor link", "to",
               std::string(to_string(rule->to)));
  }

  if (log.any()) {
    return log.primary();
  }

  ValidatedTransition validated;
  validated.rule = rule;
  validated.key = object->key;
  validated.from = object->state;
  validated.to = rule->to;
  validated.reason = request.reason;
  validated.revision_before = object->revision;
  const Result<Revision> next_revision = object->revision.next();
  if (!next_revision.has_value()) {
    return next_revision.error();
  }
  validated.revision_after = next_revision.value();
  validated.lifecycle_generation_before = object->lifecycle_generation;
  const Result<LifecycleGeneration> next_generation = object->lifecycle_generation.next();
  if (!next_generation.has_value()) {
    return next_generation.error();
  }
  validated.lifecycle_generation_after = next_generation.value();
  validated.gate_before = object->eligibility_gate;
  switch (rule->eligibility_effect) {
    case EligibilityEffect::Suspend:
    case EligibilityEffect::Terminate:
      validated.gate_after = EligibilityGate::Closed;
      break;
    case EligibilityEffect::Unchanged:
    case EligibilityEffect::Resume:
      validated.gate_after = object->eligibility_gate;
      break;
  }
  validated.location_after = location_after;
  validated.successor = successor;
  return validated;
}

Result<ValidatedLink> validate_replacement(const Registry& registry, const ReplacementRequest& request) {
  IssueLog log;
  check_plan_identity(request.plan, request.attempt, log);
  check_plan_identity_agreement(request.plan, request.attempt, request.provenance, log);
  if (!request.predecessor.asset.valid() || !request.predecessor.hardware_generation.valid() ||
      !request.successor.asset.valid() || !request.successor.hardware_generation.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request names an incomplete object key");
  }
  if (!request.expected_revision.valid() || !request.expected_lifecycle_generation.valid() ||
      !request.expected_control_epoch.valid()) {
    log.report(ErrorCode::MalformedRequest,
               "the request must name the lifecycle generation, revision and writer epoch it was planned against");
  }
  if (!log.any()) {
    const Result<void> provenance = validate_provenance(request.provenance);
    if (!provenance.has_value()) {
      log.report(provenance.error().code, provenance.error().message);
    }
  }
  if (log.any()) {
    return log.primary();
  }

  const Digest request_digest = compute_request_digest(request);
  const AppliedPlan* applied = resolve_plan(registry, request.plan, request.attempt, request_digest, log);
  if (log.any()) {
    return log.primary();
  }
  if (applied != nullptr) {
    return Error::make(ErrorCode::PlanConflict, "the plan was already applied");
  }

  const HardwareObject* predecessor = resolve(registry, request.predecessor, log);
  const HardwareObject* successor = resolve(registry, request.successor, log);
  if (predecessor == nullptr || successor == nullptr) {
    return log.primary();
  }

  const Result<void> pair = validate_replacement_pair(predecessor->key, successor->key);
  if (!pair.has_value()) {
    log.report(pair.error().code, pair.error().message);
  }
  check_stale_authority(request.expected_control_epoch, registry.control_epoch(), log);
  if (predecessor->authority != AuthorityState::Live || successor->authority != AuthorityState::Live) {
    log.report(ErrorCode::StaleAuthority,
               "a replacement link mutates both objects, so both must have authority re-established in this session; "
               "attest them before linking them",
               "authority", std::string(to_string(predecessor->authority)));
  }
  if (predecessor->lifecycle_generation != request.expected_lifecycle_generation) {
    log.report(ErrorCode::StaleLifecycleGeneration, "the predecessor moved on since the request was planned");
  }
  if (predecessor->revision != request.expected_revision) {
    log.report(ErrorCode::StaleRevision, "the predecessor moved on since the request was planned");
  }
  if (predecessor->state != LifecycleState::Retired) {
    log.report(ErrorCode::IllegalTransition,
               "a successor may only be linked to an object that has been decommissioned and not yet superseded",
               "predecessor_state", std::string(to_string(predecessor->state)));
  }
  if (!(is_physical(successor->state) && successor->state != LifecycleState::Staged &&
        successor->state != LifecycleState::Removed)) {
    log.report(ErrorCode::InvalidSuccessor,
               "the successor must already be installed or further along before it may replace anything",
               "successor_state", std::string(to_string(successor->state)));
  }
  if (predecessor->successor.has_value()) {
    log.report(ErrorCode::ObjectExists, "this object already has a successor");
  }
  if (successor->predecessor.has_value()) {
    log.report(ErrorCode::ObjectExists, "this object already has a predecessor");
  }
  check_authority(request.provenance, authority_bit(AuthorityScope::Replacement), log);
  check_evidence(request.provenance,
                 evidence_bit(EvidenceKind::ReplacementAuthorization) | evidence_bit(EvidenceKind::SuccessorRecord),
                 log);
  if (log.any()) {
    return log.primary();
  }

  ValidatedLink validated;
  validated.predecessor = predecessor->key;
  validated.successor = successor->key;
  validated.reason = TransitionReason::SuccessorLinked;
  const Result<ReplacementGeneration> next = predecessor->replacement_generation.valid()
                                                 ? predecessor->replacement_generation.next()
                                                 : Result<ReplacementGeneration>(ReplacementGeneration::first());
  if (!next.has_value()) {
    return next.error();
  }
  validated.replacement_generation = next.value();
  return validated;
}

Result<ValidatedGate> validate_gate(const Registry& registry, const EligibilityGateRequest& request) {
  IssueLog log;
  check_plan_identity(request.plan, request.attempt, log);
  check_plan_identity_agreement(request.plan, request.attempt, request.provenance, log);
  if (!request.key.asset.valid() || !request.key.hardware_generation.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request names an incomplete object key");
  }
  if (request.gate == EligibilityGate::Unknown) {
    log.report(ErrorCode::MalformedRequest,
               "an eligibility decision is open or closed; unknown is the absence of a decision and cannot be set",
               "gate", "unknown");
  }
  if (!request.expected_revision.valid() || !request.expected_lifecycle_generation.valid() ||
      !request.expected_control_epoch.valid()) {
    log.report(ErrorCode::MalformedRequest,
               "the request must name the lifecycle generation, revision and writer epoch it was planned against");
  }
  if (!log.any()) {
    const Result<void> provenance = validate_provenance(request.provenance);
    if (!provenance.has_value()) {
      log.report(provenance.error().code, provenance.error().message);
    }
  }
  if (log.any()) {
    return log.primary();
  }

  const Digest request_digest = compute_request_digest(request);
  const AppliedPlan* applied = resolve_plan(registry, request.plan, request.attempt, request_digest, log);
  if (log.any()) {
    return log.primary();
  }
  if (applied != nullptr) {
    return Error::make(ErrorCode::PlanConflict, "the plan was already applied");
  }

  const HardwareObject* object = resolve(registry, request.key, log);
  if (object == nullptr) {
    return log.primary();
  }
  check_stale_authority(request.expected_control_epoch, registry.control_epoch(), log);
  if (object->authority != AuthorityState::Live) {
    log.report(ErrorCode::StaleAuthority,
               "this object came back from a restart and its authority has not been re-established in this session; "
               "attest it before changing its service eligibility",
               "authority", std::string(to_string(object->authority)));
  }
  if (object->lifecycle_generation != request.expected_lifecycle_generation) {
    log.report(ErrorCode::StaleLifecycleGeneration, "the object moved on since the request was planned");
  }
  if (object->revision != request.expected_revision) {
    log.report(ErrorCode::StaleRevision, "the object moved on since the request was planned");
  }
  check_authority(request.provenance, authority_bit(AuthorityScope::Service), log);
  check_evidence(request.provenance, EvidenceMask(), log);
  if (log.any()) {
    return log.primary();
  }

  ValidatedGate validated;
  validated.key = object->key;
  validated.before = object->eligibility_gate;
  validated.after = request.gate;
  validated.revision_before = object->revision;
  const Result<Revision> next = object->revision.next();
  if (!next.has_value()) {
    return next.error();
  }
  validated.revision_after = next.value();
  return validated;
}

Result<void> validate_observation(const Registry& registry, const HealthObservationRequest& request) {
  IssueLog log;
  check_plan_identity(request.plan, request.attempt, log);
  check_plan_identity_agreement(request.plan, request.attempt, request.provenance, log);
  if (!request.key.asset.valid() || !request.key.hardware_generation.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request names an incomplete object key");
  }
  if (!(request.observation.key == request.key)) {
    log.report(ErrorCode::MalformedRequest, "the observation must be about the object the request names");
  }
  if (!is_valid_evidence_source(request.observation.source)) {
    log.report(ErrorCode::MalformedRequest, "an observation must name the system it came from", "source",
               request.observation.source);
  }
  if (request.observation.evidence_digest.is_zero()) {
    log.report(ErrorCode::MissingEvidence,
               "an observation must bind the digest of the exact payload it reports; an unbound observation is not "
               "evidence",
               "evidence_digest", "zero");
  }
  if (!log.any()) {
    const Result<void> provenance = validate_provenance(request.provenance);
    if (!provenance.has_value()) {
      log.report(provenance.error().code, provenance.error().message);
    }
  }
  if (log.any()) {
    return log.primary();
  }

  const Digest request_digest = compute_request_digest(request);
  const AppliedPlan* applied = resolve_plan(registry, request.plan, request.attempt, request_digest, log);
  if (log.any()) {
    return log.primary();
  }
  if (applied != nullptr) {
    return Error::make(ErrorCode::PlanConflict, "the plan was already applied");
  }

  if (resolve(registry, request.key, log) == nullptr) {
    return log.primary();
  }
  const Result<ObservationSequence> next = registry.next_observation_sequence();
  if (!next.has_value()) {
    return next.error();
  }
  return ok();
}

Result<ValidatedAttestation> validate_attestation(const Registry& registry, const AttestationRequest& request) {
  IssueLog log;
  check_plan_identity(request.plan, request.attempt, log);
  check_plan_identity_agreement(request.plan, request.attempt, request.provenance, log);
  if (!request.key.asset.valid() || !request.key.hardware_generation.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request names an incomplete object key");
  }
  if (!request.expected_revision.valid() || !request.expected_control_epoch.valid()) {
    log.report(ErrorCode::MalformedRequest, "the request must name the revision and writer epoch it was planned against");
  }
  if (!log.any()) {
    const Result<void> provenance = validate_provenance(request.provenance);
    if (!provenance.has_value()) {
      log.report(provenance.error().code, provenance.error().message);
    }
  }
  if (log.any()) {
    return log.primary();
  }

  const Digest request_digest = compute_request_digest(request);
  const AppliedPlan* applied = resolve_plan(registry, request.plan, request.attempt, request_digest, log);
  if (log.any()) {
    return log.primary();
  }
  if (applied != nullptr) {
    return Error::make(ErrorCode::PlanConflict, "the plan was already applied");
  }

  const HardwareObject* object = resolve(registry, request.key, log);
  if (object == nullptr) {
    return log.primary();
  }
  check_stale_authority(request.expected_control_epoch, registry.control_epoch(), log);
  if (object->revision != request.expected_revision) {
    log.report(ErrorCode::StaleRevision, "the object moved on since the request was planned");
  }
  check_authority(request.provenance, authority_bit(AuthorityScope::Recovery), log);
  check_evidence(request.provenance, evidence_bit(EvidenceKind::RecoveryAttestation), log);
  if (log.any()) {
    return log.primary();
  }

  ValidatedAttestation validated;
  validated.key = object->key;
  validated.revision = object->revision;
  validated.before = object->authority;
  return validated;
}

}  // namespace hardware_lifecycle