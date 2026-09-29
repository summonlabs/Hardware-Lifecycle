// Hardware Lifecycle - seeded randomised state machine over the real runtime.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every run drives a real Runtime through the ordinary commit path: requests are
// built from the live transition table, so the generator and the runtime can
// never disagree about which edges exist. The seeded generator is the only non
// deterministic input, and its seed is printed before the run so a failure can
// be replayed exactly.
//
// The invariants below are checked after every single operation, applied or
// rejected, because the two failure modes are different and both are serious: an
// accepted request that leaves the history chain disagreeing with the object,
// and a rejected request that leaves a trace behind anyway.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;

constexpr std::size_t kOperationsPerSeed = 400;

/// Objects are capped so that the per operation full verification stays bounded
/// while the state space stays wide: registration, staging, installation,
/// commissioning, service, maintenance, quarantine, drain, decommission,
/// replacement and removal are all reachable inside this budget.
constexpr std::size_t kMaxObjects = 16;

/// The four seeds the suite runs. More than one seed is used so that a failure
/// which depends on the operation order cannot hide behind a lucky draw, and
/// every seed is printed in the output as a reproduction.
constexpr std::uint64_t kSeeds[] = {0x5eed0001ull, 20260101ull, 424242ull, 999983ull};

[[nodiscard]] std::string seed_text(std::uint64_t seed) { return std::to_string(seed); }

/// The first reason the rule accepts, in a fixed declaration order.
[[nodiscard]] TransitionReason allowed_reason(ReasonMask allowed) {
  for (std::size_t index = 1; index < kTransitionReasonCount; ++index) {
    const TransitionReason reason = static_cast<TransitionReason>(index);
    if (reason_allowed(allowed, reason)) {
      return reason;
    }
  }
  return TransitionReason::Unset;
}

/// The first reason the rule does not accept, in the same fixed order.
[[nodiscard]] TransitionReason disallowed_reason(ReasonMask allowed) {
  for (std::size_t index = 1; index < kTransitionReasonCount; ++index) {
    const TransitionReason reason = static_cast<TransitionReason>(index);
    if (!reason_allowed(allowed, reason)) {
      return reason;
    }
  }
  return TransitionReason::Unset;
}

/// One deliberately introduced violation of an otherwise correct request.
enum class Corruption : std::uint8_t {
  StaleRevision,
  StaleLifecycleGeneration,
  StaleAuthority,
  StateMismatch,
  NoAuthority,
  NoEvidence,
  WrongReason,
  IllegalTarget,
  MissingLocation,
  MissingSuccessor,
  UnknownSuccessor,
  SurplusSuccessor,
};

[[nodiscard]] ErrorCode expected_code(Corruption corruption) noexcept {
  switch (corruption) {
    case Corruption::StaleRevision:
      return ErrorCode::StaleRevision;
    case Corruption::StaleLifecycleGeneration:
      return ErrorCode::StaleLifecycleGeneration;
    case Corruption::StaleAuthority:
      return ErrorCode::StaleAuthority;
    case Corruption::StateMismatch:
      return ErrorCode::StateMismatch;
    case Corruption::NoAuthority:
      return ErrorCode::InsufficientAuthority;
    case Corruption::NoEvidence:
      return ErrorCode::MissingEvidence;
    case Corruption::WrongReason:
      return ErrorCode::IllegalTransition;
    case Corruption::IllegalTarget:
      return ErrorCode::IllegalTransition;
    case Corruption::MissingLocation:
      return ErrorCode::MissingLocation;
    case Corruption::MissingSuccessor:
      return ErrorCode::MissingSuccessor;
    case Corruption::UnknownSuccessor:
      return ErrorCode::UnknownAsset;
    case Corruption::SurplusSuccessor:
      return ErrorCode::InvalidSuccessor;
  }
  return ErrorCode::Internal;
}

[[nodiscard]] const char* name_of(Corruption corruption) noexcept {
  switch (corruption) {
    case Corruption::StaleRevision:
      return "stale_revision";
    case Corruption::StaleLifecycleGeneration:
      return "stale_lifecycle_generation";
    case Corruption::StaleAuthority:
      return "stale_authority";
    case Corruption::StateMismatch:
      return "state_mismatch";
    case Corruption::NoAuthority:
      return "no_authority";
    case Corruption::NoEvidence:
      return "no_evidence";
    case Corruption::WrongReason:
      return "wrong_reason";
    case Corruption::IllegalTarget:
      return "illegal_target";
    case Corruption::MissingLocation:
      return "missing_location";
    case Corruption::MissingSuccessor:
      return "missing_successor";
    case Corruption::UnknownSuccessor:
      return "unknown_successor";
    case Corruption::SurplusSuccessor:
      return "surplus_successor";
  }
  return "unknown";
}

/// One seeded state machine. Every check is reported through the harness
/// context, and the first violated invariant prints the seed, the operation
/// index and the operation description before the case fails.
class Machine {
 public:
  Machine(hl_test::Context& context, std::uint64_t seed)
      : hl_ctx(context), seed_(seed), rng_(seed), next_asset_(0), next_plan_(0) {}

  void run() {
    std::printf("[property_state_machine] seed=%llu operations=%zu\n", static_cast<unsigned long long>(seed_),
                kOperationsPerSeed);
    std::fflush(stdout);
    Result<Runtime> opened = Runtime::open_ephemeral();
    if (!opened.has_value()) {
      HL_FAIL("the ephemeral runtime could not be opened: " + describe(opened.error()));
      return;
    }
    runtime_.emplace(std::move(opened).value());
    for (std::size_t index = 0; index < kOperationsPerSeed && !stopped_; ++index) {
      step(index);
    }
    if (!stopped_) {
      std::printf("[property_state_machine] seed=%llu completed %zu operations with no invariant violation\n",
                  static_cast<unsigned long long>(seed_), kOperationsPerSeed);
    }
  }

 private:
  [[nodiscard]] Runtime& runtime() { return runtime_.value(); }

  // ---------------------------------------------------------------------
  // Reporting
  // ---------------------------------------------------------------------

  bool expect(bool condition, std::size_t index, const std::string& description, const std::string& detail) {
    if (condition) {
      return true;
    }
    std::printf("[property_state_machine] seed=%llu operation=%zu description=%s detail=%s\n",
                static_cast<unsigned long long>(seed_), index, description.c_str(), detail.c_str());
    std::fflush(stdout);
    HL_FAIL("seeded state machine: seed " + seed_text(seed_) + ", operation " + std::to_string(index) + ", " +
            description + ": " + detail);
    stopped_ = true;
    return false;
  }

  // ---------------------------------------------------------------------
  // Identity and provenance generators
  // ---------------------------------------------------------------------

  [[nodiscard]] std::string fresh_asset() {
    return "hl-" + seed_text(seed_) + "-asset-" + std::to_string(next_asset_++);
  }

  [[nodiscard]] PlanId fresh_plan(const char* prefix) {
    std::string text = std::string(prefix) + "-" + seed_text(seed_) + "-" + std::to_string(next_plan_++);
    return PlanId::parse(text).value();
  }

  [[nodiscard]] AttemptId attempt() const { return AttemptId::parse("attempt-1").value(); }

  [[nodiscard]] ObjectKey key_of(const std::string& asset) {
    ObjectKey key;
    key.asset = AssetId::parse(asset).value();
    key.hardware_generation = HardwareGeneration::first();
    return key;
  }

  [[nodiscard]] Location random_location() {
    Location location;
    location.site = SiteId::parse("site-" + std::to_string(rng_() % 4)).value();
    location.rack = RackId::parse("rack-" + std::to_string(rng_() % 8)).value();
    location.slot = SlotId::parse("slot-" + std::to_string(rng_() % 32)).value();
    return location;
  }

  [[nodiscard]] EvidenceKind any_evidence_kind() {
    const std::size_t kind = 1 + static_cast<std::size_t>(rng_() % (kEvidenceKindCount - 1));
    return static_cast<EvidenceKind>(kind);
  }

  [[nodiscard]] Provenance provenance_for(const PlanId& plan, AuthorityMask authority,
                                          const std::vector<EvidenceKind>& kinds) {
    Provenance provenance;
    provenance.actor.id = ActorId::parse("hl-actor").value();
    provenance.actor.kind = ActorKind::Automation;
    provenance.authority = authority;
    provenance.policy_generation = PolicyGeneration::first();
    provenance.plan = plan;
    provenance.attempt = attempt();
    for (std::size_t index = 0; index < kinds.size(); ++index) {
      EvidenceRef reference;
      reference.kind = kinds[index];
      reference.digest = Sha256::hash(plan.value() + "#" + std::to_string(index));
      reference.source = "evidence://" + plan.value() + "/" + std::to_string(index);
      reference.observed_sequence = ObservationSequence::from_value(static_cast<std::uint64_t>(index) + 1);
      provenance.evidence.push_back(reference);
    }
    return provenance;
  }

  /// One evidence reference per required kind. An empty requirement still needs
  /// one reference of any kind: every externally meaningful change binds to the
  /// evidence it was planned against.
  [[nodiscard]] std::vector<EvidenceKind> required_kinds(EvidenceMask required, bool allow_any) {
    std::vector<EvidenceKind> kinds;
    for (std::size_t index = 1; index < kEvidenceKindCount; ++index) {
      const EvidenceKind kind = static_cast<EvidenceKind>(index);
      if (required.intersects(evidence_bit(kind))) {
        kinds.push_back(kind);
      }
    }
    if (kinds.empty() && allow_any) {
      kinds.push_back(any_evidence_kind());
    }
    return kinds;
  }

  // ---------------------------------------------------------------------
  // State observation
  // ---------------------------------------------------------------------

  [[nodiscard]] std::vector<HardwareObject> objects() {
    std::vector<HardwareObject> snapshot;
    for (const std::pair<const ObjectKey, HardwareObject>& entry : runtime().registry().objects()) {
      snapshot.push_back(entry.second);
    }
    return snapshot;
  }

  [[nodiscard]] const HardwareObject* find(const ObjectKey& key) { return runtime().registry().find(key); }

  /// The successor a replacement edge must name, or null when the edge is not
  /// currently satisfiable. A correct request may only be built for an edge
  /// whose successor requirements are already recorded and still hold.
  [[nodiscard]] const ObjectKey* usable_successor(const HardwareObject& object, const TransitionRule& rule) {
    const ReplacementRecord* link = runtime().registry().lineage().successor_of(object.key);
    if (link == nullptr) {
      return nullptr;
    }
    if (object.successor.has_value() && !(object.successor.value() == link->successor)) {
      return nullptr;
    }
    const HardwareObject* successor = find(link->successor);
    if (successor == nullptr) {
      return nullptr;
    }
    if (rule.requires_successor_installed &&
        !(is_physical(successor->state) && successor->state != LifecycleState::Staged &&
          successor->state != LifecycleState::Removed)) {
      return nullptr;
    }
    return &link->successor;
  }

  [[nodiscard]] bool entering_service(const TransitionRule& rule) const noexcept {
    return occupies_service_scope(rule.to) && !occupies_service_scope(rule.from);
  }

  // ---------------------------------------------------------------------
  // Operation selection
  // ---------------------------------------------------------------------

  void step(std::size_t index) {
    const Digest digest_before = runtime().registry().state_digest();
    const CommitSequence commit_before = runtime().registry().commit_sequence();
    const LogicalTime logical_before = runtime().registry().logical_time();
    const std::size_t entries_before = runtime().registry().history_entry_count();
    const std::size_t plans_before = runtime().registry().plan_count();

    const std::uint64_t draw = rng_() % 100;
    const bool tried = draw < 12 && runtime().registry().object_count() < kMaxObjects
                           ? do_register(index)
                           : (draw < 26 ? do_gate(index) : (draw < 34 ? do_link(index) : do_transition(index)));
    const bool applied = tried && !last_was_rejected_;
    const bool rejected = tried && last_was_rejected_;

    const Digest digest_after = runtime().registry().state_digest();
    const CommitSequence commit_after = runtime().registry().commit_sequence();
    const LogicalTime logical_after = runtime().registry().logical_time();
    const std::size_t entries_after = runtime().registry().history_entry_count();
    const std::size_t plans_after = runtime().registry().plan_count();

    const std::string context = last_description_;

    // A rejected request must leave the authoritative state byte identical: no
    // history entry, no plan, no watermark move, no state digest change.
    if (rejected) {
      expect(digest_after == digest_before, index, context, "a rejected request changed the state digest");
      expect(commit_after == commit_before, index, context, "a rejected request moved the commit sequence");
      expect(logical_after == logical_before, index, context, "a rejected request moved logical time");
      expect(entries_after == entries_before, index, context, "a rejected request added a history entry");
      expect(plans_after == plans_before, index, context, "a rejected request recorded an applied plan");
    }
    if (applied) {
      expect(commit_after.value() == commit_before.value() + 1, index, context,
             "an applied request did not advance the commit sequence by exactly one");
      expect(logical_after.value() == logical_before.value() + 1, index, context,
             "an applied request did not advance logical time by exactly one");
      expect(entries_after >= entries_before, index, context, "an applied request removed a history entry");
      expect(plans_after == plans_before + 1, index, context,
             "an applied request did not record exactly one applied plan");
    }

    // Watermarks only ever move forward.
    expect(!(commit_after < commit_before), index, context, "the commit sequence moved backwards");
    expect(!(logical_after < logical_before), index, context, "logical time moved backwards");
    expect(entries_after >= entries_before, index, context, "the registry lost history entries");

    check_registry(index, context);
  }

  /// Full verification plus the per object chain invariants the runtime promises:
  /// the chain verifies, replays to exactly the object state, and its last entry
  /// carries the object's revision and lifecycle generation.
  void check_registry(std::size_t index, const std::string& context) {
    const Result<Digest> verified = runtime().verify();
    expect(verified.has_value(), index, context, "the registry does not verify: " +
                                                     (verified.has_value() ? std::string()
                                                                           : describe(verified.error())));
    if (verified.has_value()) {
      expect(verified.value() == runtime().registry().state_digest(), index, context,
             "verify() returned a digest that is not the registry state digest");
    }

    std::size_t entries_seen = 0;
    for (const HardwareObject& object : objects()) {
      const Result<HistoryView> view = runtime().history(object.key);
      if (!expect(view.has_value(), index, context, "the object has no history view")) {
        return;
      }
      const HistoryLog log{view.value().entries, view.value().chain_head};
      const Result<Digest> chain = verify_history_chain(log);
      if (!expect(chain.has_value(), index, context,
                  "the history chain does not verify: " + (chain.has_value() ? std::string() : describe(chain.error())))) {
        return;
      }
      expect(chain.value() == view.value().chain_head, index, context,
             "the recomputed chain head is not the recorded chain head");
      const Result<LifecycleState> replayed = replay_history(log);
      if (!expect(replayed.has_value(), index, context,
                  "the history chain does not replay: " +
                      (replayed.has_value() ? std::string() : describe(replayed.error())))) {
        return;
      }
      expect(replayed.value() == object.state, index, context, "the replayed history state is not the object state");
      const Result<void> consistent = verify_history_against_object(log, object);
      expect(consistent.has_value(), index, context,
             "the history chain and the object disagree: " +
                 (consistent.has_value() ? std::string() : describe(consistent.error())));
      if (view.value().entries.empty()) {
        expect(false, index, context, "the object has an empty history chain");
        return;
      }
      expect(view.value().entries.back().revision_after == object.revision, index, context,
             "the object revision is not the last history entry revision_after");
      expect(view.value().entries.back().lifecycle_generation == object.lifecycle_generation, index, context,
             "the object lifecycle generation is not the last history entry generation");
      expect(view.value().entries.back().to == object.state, index, context,
             "the last history entry does not land on the object state");
      expect(view.value().entries.size() == object.history_entries, index, context,
             "the object entry count is not the history chain length");
      expect(view.value().entries.front().is_registration(), index, context,
             "the history chain does not open with a registration entry");
      entries_seen += view.value().entries.size();
    }
    expect(entries_seen == runtime().registry().history_entry_count(), index, context,
           "the registry entry count is not the sum of the object chains");
  }

  // ---------------------------------------------------------------------
  // Operations
  // ---------------------------------------------------------------------

  bool do_register(std::size_t index) {
    const ObjectKey key = key_of(fresh_asset());
    const bool staged = (rng_() % 2) != 0;
    const bool with_location = (rng_() % 2) != 0;
    const PlanId plan = fresh_plan("create");

    CreateRequest request;
    request.plan = plan;
    request.attempt = attempt();
    request.key = key;
    request.kind = static_cast<HardwareKind>(1 + rng_() % (static_cast<std::uint64_t>(HardwareKind::Other)));
    request.model = ModelId::parse("model-" + std::to_string(rng_() % 8)).value();
    request.firmware_generation = FirmwareGeneration::first();
    request.initial_state = staged ? LifecycleState::Staged : LifecycleState::Ordered;
    if (with_location) {
      request.location = random_location();
    }
    const AuthorityScope scope = staged ? AuthorityScope::Logistics : AuthorityScope::Procurement;
    const EvidenceKind evidence = staged ? EvidenceKind::DeliveryReceipt : EvidenceKind::ProcurementRecord;
    request.provenance = provenance_for(plan, authority_bit(scope), {evidence});

    last_description_ = "register " + to_string(key) + " as " + std::string(to_string(request.initial_state));
    const Result<CreateReceipt> created = runtime().create_object(request);
    last_was_rejected_ = !created.has_value();
    if (last_was_rejected_) {
      expect(false, index, last_description_, "a correct registration was rejected: " + describe(created.error()));
      return false;
    }
    const HardwareObject* object = find(key);
    if (!expect(object != nullptr, index, last_description_, "the registered object is not in the registry")) {
      return false;
    }
    expect(object->revision == Revision::first(), index, last_description_, "a new object is not at revision 1");
    expect(object->lifecycle_generation == LifecycleGeneration::first(), index, last_description_,
           "a new object is not at lifecycle generation 1");
    expect(created.value().state == request.initial_state, index, last_description_,
           "the receipt does not carry the registered state");
    return true;
  }

  bool do_gate(std::size_t index) {
    std::vector<HardwareObject> live = objects();
    if (live.empty()) {
      return do_register(index);
    }
    const HardwareObject& object = live[rng_() % live.size()];
    const EligibilityGate target =
        object.eligibility_gate == EligibilityGate::Open ? EligibilityGate::Closed : EligibilityGate::Open;

    const PlanId plan = fresh_plan("gate");
    EligibilityGateRequest request;
    request.plan = plan;
    request.attempt = attempt();
    request.key = object.key;
    request.gate = target;
    request.expected_lifecycle_generation = object.lifecycle_generation;
    request.expected_revision = object.revision;
    request.expected_control_epoch = runtime().registry().control_epoch();
    request.provenance = provenance_for(plan, authority_bit(AuthorityScope::Service), {any_evidence_kind()});

    last_description_ = "gate " + to_string(object.key) + " to " + std::string(to_string(target));
    const Result<EligibilityGateReceipt> receipt = runtime().set_eligibility_gate(request);
    last_was_rejected_ = !receipt.has_value();
    if (last_was_rejected_) {
      expect(false, index, last_description_, "a correct gate decision was rejected: " + describe(receipt.error()));
      return false;
    }
    expect(receipt.value().after == target, index, last_description_, "the receipt does not carry the new gate");
    return true;
  }

  bool do_link(std::size_t index) {
    std::vector<HardwareObject> live = objects();
    for (std::size_t attempt_index = 0; attempt_index < live.size() * 2; ++attempt_index) {
      const HardwareObject& predecessor = live[rng_() % live.size()];
      if (predecessor.state != LifecycleState::Retired || predecessor.successor.has_value()) {
        continue;
      }
      const HardwareObject& successor = live[rng_() % live.size()];
      if (successor.key == predecessor.key || successor.predecessor.has_value()) {
        continue;
      }
      if (!(is_physical(successor.state) && successor.state != LifecycleState::Staged &&
            successor.state != LifecycleState::Removed)) {
        continue;
      }
      const PlanId plan = fresh_plan("link");
      ReplacementRequest request;
      request.plan = plan;
      request.attempt = attempt();
      request.predecessor = predecessor.key;
      request.successor = successor.key;
      request.expected_lifecycle_generation = predecessor.lifecycle_generation;
      request.expected_revision = predecessor.revision;
      request.expected_control_epoch = runtime().registry().control_epoch();
      request.provenance = provenance_for(
          plan, authority_bit(AuthorityScope::Replacement),
          {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord});

      last_description_ = "link " + to_string(predecessor.key) + " -> " + to_string(successor.key);
      const Result<ReplacementReceipt> receipt = runtime().link_replacement(request);
      last_was_rejected_ = !receipt.has_value();
      if (last_was_rejected_) {
        expect(false, index, last_description_, "a correct replacement link was rejected: " + describe(receipt.error()));
        return false;
      }
      return true;
    }
    return do_transition(index);
  }

  bool do_transition(std::size_t index) {
    std::vector<HardwareObject> live = objects();
    if (live.empty()) {
      return do_register(index);
    }

    // Find an object and one of its legal targets whose rule can be satisfied by
    // a correct request right now. A target whose rule requires the service gate
    // to be open is only buildable while the gate is open: opening it is a
    // separate decision with its own authority, and it is a real operation of
    // this machine, not a side effect of a transition.
    for (std::size_t attempt_index = 0; attempt_index < live.size() * 8; ++attempt_index) {
      const HardwareObject& object = live[rng_() % live.size()];
      const std::vector<LifecycleState> targets = legal_targets(object.state);
      if (targets.empty()) {
        continue;
      }
      const LifecycleState target = targets[rng_() % targets.size()];
      const TransitionRule* rule = find_rule(object.state, target);
      if (rule == nullptr) {
        continue;
      }
      if (entering_service(*rule) && object.eligibility_gate != EligibilityGate::Open) {
        continue;
      }
      const ObjectKey* successor = nullptr;
      if (rule->requires_successor_link) {
        successor = usable_successor(object, *rule);
        if (successor == nullptr) {
          continue;
        }
      }

      const PlanId plan = fresh_plan("transition");
      TransitionRequest request;
      request.plan = plan;
      request.attempt = attempt();
      request.key = object.key;
      request.expected_lifecycle_generation = object.lifecycle_generation;
      request.expected_revision = object.revision;
      request.expected_control_epoch = runtime().registry().control_epoch();
      request.expected_state = object.state;
      request.target_state = target;
      request.reason = allowed_reason(rule->allowed_reasons);
      request.provenance = provenance_for(plan, rule->required_authority,
                                          required_kinds(rule->required_evidence, true));
      if (rule->requires_location && !object.location.has_value()) {
        request.location = random_location();
      }
      if (successor != nullptr) {
        request.successor = *successor;
      }
      if (request.reason == TransitionReason::Unset) {
        expect(false, index, "transition", "the transition table has an edge that allows no reason");
        return false;
      }

      std::string description = "transition " + to_string(object.key) + " " + std::string(to_string(object.state)) +
                                " -> " + std::string(to_string(target));
      if ((rng_() % 2) == 0 && corrupt(request, object, *rule, description)) {
        description += " [" + std::string(name_of(corruption_)) + "]";
        last_description_ = description;
        const Result<TransitionReceipt> rejected = runtime().apply_transition(request);
        last_was_rejected_ = true;
        if (rejected.has_value()) {
          expect(false, index, description, "a deliberately corrupted request was accepted");
          return false;
        }
        expect(rejected.error().code == expected_code(corruption_), index, description,
               std::string("the primary error is ") + std::string(to_string(rejected.error().code)) +
                   ", expected " + std::string(to_string(expected_code(corruption_))) + " (" +
                   rejected.error().message + ")");
        return true;
      }

      last_description_ = description;
      const Result<TransitionReceipt> applied = runtime().apply_transition(request);
      last_was_rejected_ = !applied.has_value();
      if (last_was_rejected_) {
        expect(false, index, description, "a correct transition was rejected: " + describe(applied.error()));
        return false;
      }
      expect(applied.value().from == object.state && applied.value().to == target, index, description,
             "the receipt does not describe the requested edge");
      return true;
    }
    return do_gate(index);
  }

  /// Introduces exactly one violation into an otherwise correct request and
  /// records which one it was. Returns false when no violation applies to this
  /// edge, in which case the request stays correct.
  bool corrupt(TransitionRequest& request, const HardwareObject& object, const TransitionRule& rule,
               const std::string& description) {
    std::vector<Corruption> applicable;
    const std::uint64_t max_counter = (std::numeric_limits<std::uint64_t>::max)();
    if (object.revision.value() < max_counter) {
      applicable.push_back(Corruption::StaleRevision);
    }
    if (object.lifecycle_generation.value() < max_counter) {
      applicable.push_back(Corruption::StaleLifecycleGeneration);
    }
    if (runtime().registry().control_epoch().value() < max_counter) {
      applicable.push_back(Corruption::StaleAuthority);
    }
    applicable.push_back(Corruption::StateMismatch);
    applicable.push_back(Corruption::NoAuthority);
    applicable.push_back(Corruption::NoEvidence);
    if (disallowed_reason(rule.allowed_reasons) != TransitionReason::Unset) {
      applicable.push_back(Corruption::WrongReason);
    }
    applicable.push_back(Corruption::IllegalTarget);
    if (rule.requires_location && !object.location.has_value() && !request.location.has_value()) {
      applicable.push_back(Corruption::MissingLocation);
    }
    if (rule.requires_successor_link) {
      applicable.push_back(Corruption::MissingSuccessor);
      applicable.push_back(Corruption::UnknownSuccessor);
    } else {
      applicable.push_back(Corruption::SurplusSuccessor);
    }
    if (applicable.empty()) {
      return false;
    }
    corruption_ = applicable[rng_() % applicable.size()];
    switch (corruption_) {
      case Corruption::StaleRevision:
        request.expected_revision = Revision::from_value(object.revision.value() + 1);
        break;
      case Corruption::StaleLifecycleGeneration:
        request.expected_lifecycle_generation = LifecycleGeneration::from_value(object.lifecycle_generation.value() + 1);
        break;
      case Corruption::StaleAuthority:
        request.expected_control_epoch = ControlEpoch::from_value(runtime().registry().control_epoch().value() + 1);
        break;
      case Corruption::StateMismatch:
        request.expected_state = request.expected_state == LifecycleState::Ordered ? LifecycleState::Removed
                                                                                   : LifecycleState::Ordered;
        break;
      case Corruption::NoAuthority:
        request.provenance.authority = authority_all() & ~rule.required_authority;
        break;
      case Corruption::NoEvidence:
        request.provenance.evidence.clear();
        break;
      case Corruption::WrongReason:
        request.reason = disallowed_reason(rule.allowed_reasons);
        break;
      case Corruption::IllegalTarget:
        for (std::size_t index = 0; index < kLifecycleStateCount; ++index) {
          const LifecycleState candidate = static_cast<LifecycleState>(index);
          if (find_rule(object.state, candidate) == nullptr) {
            request.target_state = candidate;
            break;
          }
        }
        break;
      case Corruption::MissingLocation:
        request.location.reset();
        break;
      case Corruption::MissingSuccessor:
        request.successor.reset();
        break;
      case Corruption::UnknownSuccessor:
        request.successor = key_of("hl-" + seed_text(seed_) + "-ghost-" + std::to_string(next_asset_++));
        break;
      case Corruption::SurplusSuccessor:
        request.successor = key_of("hl-" + seed_text(seed_) + "-surplus-" + std::to_string(next_asset_++));
        break;
    }
    (void)description;
    return true;
  }

  hl_test::Context& hl_ctx;
  std::uint64_t seed_;
  std::mt19937_64 rng_;
  std::optional<Runtime> runtime_;
  std::size_t next_asset_;
  std::size_t next_plan_;
  std::string last_description_;
  bool last_was_rejected_ = false;
  bool stopped_ = false;
  Corruption corruption_ = Corruption::StaleRevision;
};

void run_seed(hl_test::Context& hl_ctx, std::uint64_t seed) {
  Machine machine(hl_ctx, seed);
  machine.run();
}

}  // namespace

HL_TEST(property_state_machine, seed_5eed0001) { run_seed(hl_ctx, kSeeds[0]); }
HL_TEST(property_state_machine, seed_20260101) { run_seed(hl_ctx, kSeeds[1]); }
HL_TEST(property_state_machine, seed_000424242) { run_seed(hl_ctx, kSeeds[2]); }
HL_TEST(property_state_machine, seed_000999983) { run_seed(hl_ctx, kSeeds[3]); }

// The deterministic precedence claim, asserted directly: when one request
// violates several rules at once, the reported primary error is the lowest
// ranking violation, not whichever check happened to run first. Each block below
// introduces several violations that the same validation band really evaluates,
// and the expected code is the fold of primary_error() over the introduced set -
// the same rule the runtime documents. The folded value is additionally pinned
// to a concrete code so the test proves the taxonomy, not just the fold.
HL_TEST(property_state_machine, primary_error_precedence) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened).value();

  CreateRequest create;
  create.plan = PlanId::parse("precedence-create").value();
  create.attempt = AttemptId::parse("attempt-1").value();
  create.key = ObjectKey{AssetId::parse("precedence-asset").value(), HardwareGeneration::first()};
  create.kind = HardwareKind::Compute;
  create.model = ModelId::parse("model-1").value();
  create.firmware_generation = FirmwareGeneration::first();
  create.initial_state = LifecycleState::Ordered;
  {
    Provenance provenance;
    provenance.actor.id = ActorId::parse("actor-1").value();
    provenance.actor.kind = ActorKind::Operator;
    provenance.authority = authority_bit(AuthorityScope::Procurement);
    provenance.policy_generation = PolicyGeneration::first();
    provenance.plan = create.plan;
    provenance.attempt = create.attempt;
    EvidenceRef reference;
    reference.kind = EvidenceKind::ProcurementRecord;
    reference.digest = Sha256::hash("precedence-evidence");
    reference.source = "evidence://precedence/0";
    provenance.evidence.push_back(reference);
    create.provenance = provenance;
  }
  HL_REQUIRE(runtime.create_object(create).has_value());

  const ObjectKey key = create.key;
  const HardwareObject* object = runtime.registry().find(key);
  HL_REQUIRE(object != nullptr);
  const TransitionRule* rule = find_rule(object->state, LifecycleState::Staged);
  HL_REQUIRE(rule != nullptr);

  const auto transition = [&]() {
    TransitionRequest request;
    request.plan = PlanId::parse("precedence-transition").value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
    request.expected_control_epoch = runtime.registry().control_epoch();
    request.expected_state = object->state;
    request.target_state = LifecycleState::Staged;
    request.reason = allowed_reason(rule->allowed_reasons);
    request.provenance.actor.id = ActorId::parse("actor-1").value();
    request.provenance.actor.kind = ActorKind::Operator;
    request.provenance.authority = rule->required_authority;
    request.provenance.policy_generation = PolicyGeneration::first();
    request.provenance.plan = request.plan;
    request.provenance.attempt = request.attempt;
    EvidenceRef reference;
    reference.kind = EvidenceKind::DeliveryReceipt;
    reference.digest = Sha256::hash("precedence-transition");
    reference.source = "evidence://precedence/1";
    request.provenance.evidence.push_back(reference);
    return request;
  };

  // Band: transition legality. Four violations are collected by the same stage,
  // so the primary error is the lowest ranked one: the reason the edge does not
  // accept, rank 300.
  {
    TransitionRequest request = transition();
    request.reason = TransitionReason::ServiceRestored;  // IllegalTransition, rank 300
    request.provenance.authority = AuthorityMask();       // InsufficientAuthority, rank 310
    request.provenance.evidence.clear();                  // MissingEvidence, rank 320
    request.successor = key;                              // InvalidSuccessor, rank 335
    const ErrorCode expected = primary_error(ErrorCode::IllegalTransition,
                                             primary_error(ErrorCode::InsufficientAuthority,
                                                           primary_error(ErrorCode::MissingEvidence,
                                                                         ErrorCode::InvalidSuccessor)));
    const Result<TransitionReceipt> refused = runtime.apply_transition(request);
    HL_REQUIRE(!refused.has_value());
    HL_CHECK(hl_ctx.check_error_code(expected, ErrorCode::IllegalTransition, "folded precedence",
                                     __FILE__, __LINE__));
    HL_CHECK(hl_ctx.check_error_code(refused.error().code, expected, "reported primary error",
                                     __FILE__, __LINE__));
  }
  {
    TransitionRequest request = transition();
    request.provenance.authority = AuthorityMask();  // InsufficientAuthority, rank 310
    request.provenance.evidence.clear();             // MissingEvidence, rank 320
    request.successor = key;                         // InvalidSuccessor, rank 335
    const ErrorCode expected = primary_error(ErrorCode::InsufficientAuthority,
                                             primary_error(ErrorCode::MissingEvidence, ErrorCode::InvalidSuccessor));
    const Result<TransitionReceipt> refused = runtime.apply_transition(request);
    HL_REQUIRE(!refused.has_value());
    HL_CHECK(hl_ctx.check_error_code(expected, ErrorCode::InsufficientAuthority, "folded precedence",
                                     __FILE__, __LINE__));
    HL_CHECK(hl_ctx.check_error_code(refused.error().code, expected, "reported primary error",
                                     __FILE__, __LINE__));
  }
  {
    TransitionRequest request = transition();
    request.provenance.evidence.clear();  // MissingEvidence, rank 320
    request.successor = key;              // InvalidSuccessor, rank 335
    const ErrorCode expected = primary_error(ErrorCode::MissingEvidence, ErrorCode::InvalidSuccessor);
    const Result<TransitionReceipt> refused = runtime.apply_transition(request);
    HL_REQUIRE(!refused.has_value());
    HL_CHECK(hl_ctx.check_error_code(expected, ErrorCode::MissingEvidence, "folded precedence", __FILE__,
                                     __LINE__));
    HL_CHECK(hl_ctx.check_error_code(refused.error().code, expected, "reported primary error",
                                     __FILE__, __LINE__));
  }

  // Band: identity. A request that names a hardware generation which is not the
  // one on record is refused before any legality check runs, so its rank (110)
  // wins over every later violation the same request also carries.
  {
    TransitionRequest request = transition();
    request.key.hardware_generation = HardwareGeneration::from_value(7);
    request.reason = TransitionReason::ServiceRestored;  // IllegalTransition, rank 300
    request.provenance.evidence.clear();                 // MissingEvidence, rank 320
    const Result<TransitionReceipt> refused = runtime.apply_transition(request);
    HL_REQUIRE(!refused.has_value());
    HL_CHECK_ERROR(refused, ErrorCode::StaleHardwareGeneration);
    HL_CHECK(validation_rank(ErrorCode::StaleHardwareGeneration) < validation_rank(ErrorCode::IllegalTransition));
    HL_CHECK(validation_rank(ErrorCode::StaleHardwareGeneration) < validation_rank(ErrorCode::MissingEvidence));
  }

  // Band: fencing. A stale lifecycle generation (rank 210) is more primary than
  // a stale revision (220), which is more primary than a state mismatch (230),
  // and all three are more primary than any legality violation. The later band
  // is never even reached, and the reported code is still the lowest ranked
  // violation of the whole request.
  {
    TransitionRequest request = transition();
    request.expected_revision = Revision::from_value(object->revision.value() + 1);
    request.expected_lifecycle_generation = LifecycleGeneration::from_value(object->lifecycle_generation.value() + 1);
    request.expected_state = LifecycleState::Removed;
    request.provenance.evidence.clear();
    const ErrorCode expected =
        primary_error(ErrorCode::StaleRevision,
                      primary_error(ErrorCode::StaleLifecycleGeneration,
                                    primary_error(ErrorCode::StateMismatch, ErrorCode::MissingEvidence)));
    const Result<TransitionReceipt> refused = runtime.apply_transition(request);
    HL_REQUIRE(!refused.has_value());
    HL_CHECK(hl_ctx.check_error_code(expected, ErrorCode::StaleLifecycleGeneration, "folded precedence",
                                     __FILE__, __LINE__));
    HL_CHECK(hl_ctx.check_error_code(refused.error().code, expected, "reported primary error",
                                     __FILE__, __LINE__));
  }
  {
    TransitionRequest request = transition();
    request.expected_revision = Revision::from_value(object->revision.value() + 1);
    const Result<TransitionReceipt> refused = runtime.apply_transition(request);
    HL_REQUIRE(!refused.has_value());
    HL_CHECK_ERROR(refused, ErrorCode::StaleRevision);
    HL_CHECK(validation_rank(ErrorCode::StaleRevision) < validation_rank(ErrorCode::StateMismatch));
  }
}
