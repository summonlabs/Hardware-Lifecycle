// Hardware Lifecycle - malformed, absurd and hostile requests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case here drives the real runtime with input that is wrong on purpose
// and asserts the exact error code the taxonomy promises. The point is not that
// a rejection happens - it is that the *right* rejection happens, because a
// caller that retries, escalates or pages on a code depends on the code being
// the deterministic one.

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

/// One evidence reference per required kind; an empty requirement still needs
/// one reference of any kind, because every externally meaningful change binds
/// to the evidence it was planned against.
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

/// Reports a result that was required to succeed. The harness only renders the
/// types it knows, so the diagnostic carries the error itself.
template <class T>
[[nodiscard]] bool succeeded(hl_test::Context& hl_ctx, const Result<T>& result, const char* what) {
  if (result.has_value()) {
    return true;
  }
  hl_ctx.fail(__FILE__, __LINE__, std::string(what) + " was refused with " + describe(result.error()));
  return false;
}

[[nodiscard]] bool create_object(hl_test::Context& hl_ctx, Runtime& runtime, const char* asset,
                                 LifecycleState initial_state, bool with_location, ObjectKey& key_out) {
  const std::string plan = unique_name("create");
  key_out = key_of(asset, 1);
  CreateRequest request;
  request.plan = PlanId::parse(plan).value();
  request.attempt = AttemptId::parse("attempt-1").value();
  request.key = key_out;
  request.kind = HardwareKind::Compute;
  request.model = ModelId::parse("model-1").value();
  request.firmware_generation = FirmwareGeneration::first();
  request.initial_state = initial_state;
  if (with_location) {
    request.location = location_of("site-a", "rack-1", "slot-1");
  }
  const bool staged = initial_state == LifecycleState::Staged;
  request.provenance = provenance_of(plan, authority_bit(staged ? AuthorityScope::Logistics
                                                                : AuthorityScope::Procurement),
                                     {staged ? EvidenceKind::DeliveryReceipt : EvidenceKind::ProcurementRecord});
  const Result<CreateReceipt> created = runtime.create_object(request);
  return succeeded(hl_ctx, created, "the setup registration");
}

/// Builds a correct transition request against the object's current record.
[[nodiscard]] TransitionRequest transition_request(hl_test::Context& hl_ctx, Runtime& runtime, const ObjectKey& key,
                                                   LifecycleState target, TransitionReason reason,
                                                   AuthorityMask authority, const std::vector<EvidenceKind>& kinds) {
  const HardwareObject* object = runtime.registry().find(key);
  if (object == nullptr) {
    hl_ctx.fail(__FILE__, __LINE__, "a transition request was built against an object that is not registered");
    return TransitionRequest{};
  }
  TransitionRequest request;
  request.plan = PlanId::parse(unique_name("transition")).value();
  request.attempt = AttemptId::parse("attempt-1").value();
  request.key = key;
  request.expected_lifecycle_generation = object->lifecycle_generation;
  request.expected_revision = object->revision;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.expected_state = object->state;
  request.target_state = target;
  request.reason = reason;
  request.provenance = provenance_of(request.plan.value(), authority, kinds);
  return request;
}

[[nodiscard]] bool step(hl_test::Context& hl_ctx, Runtime& runtime, const ObjectKey& key, LifecycleState target,
                        TransitionReason reason, AuthorityMask authority, const std::vector<EvidenceKind>& kinds,
                        std::optional<Location> location) {
  TransitionRequest request = transition_request(hl_ctx, runtime, key, target, reason, authority, kinds);
  request.location = location;
  const Result<TransitionReceipt> applied = runtime.apply_transition(request);
  return succeeded(hl_ctx, applied, "the setup transition");
}

/// Walks an object that is ordered, staged, installed, commissioning,
/// quarantined or draining all the way to Retired. None of these edges enters
/// the service scope, so none of them needs a gate decision of its own.
[[nodiscard]] bool walk_to_retired(hl_test::Context& hl_ctx, Runtime& runtime, const ObjectKey& key) {
  for (int guard = 0; guard < 8; ++guard) {
    const HardwareObject* object = runtime.registry().find(key);
    if (object == nullptr) {
      hl_ctx.fail(__FILE__, __LINE__, "the object to walk to Retired is not registered");
      return false;
    }
    switch (object->state) {
      case LifecycleState::Retired:
        return true;
      case LifecycleState::Ordered:
        if (!step(hl_ctx, runtime, key, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
                  authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt}, std::nullopt)) {
          return false;
        }
        break;
      case LifecycleState::Staged:
        if (!step(hl_ctx, runtime, key, LifecycleState::Installed, TransitionReason::InstallationCompleted,
                  authority_bit(AuthorityScope::Installation),
                  {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}, std::nullopt)) {
          return false;
        }
        break;
      case LifecycleState::Installed:
        if (!step(hl_ctx, runtime, key, LifecycleState::Commissioning, TransitionReason::CommissioningStarted,
                  authority_bit(AuthorityScope::Commissioning), {EvidenceKind::ServiceRecord}, std::nullopt)) {
          return false;
        }
        break;
      case LifecycleState::Commissioning:
        if (!step(hl_ctx, runtime, key, LifecycleState::Quarantined, TransitionReason::CommissioningFailed,
                  authority_bit(AuthorityScope::Integrity), {EvidenceKind::IntegrityReport}, std::nullopt)) {
          return false;
        }
        break;
      case LifecycleState::Quarantined:
        if (!step(hl_ctx, runtime, key, LifecycleState::Retiring,
                  TransitionReason::QuarantineReleasedForRetirement, authority_bit(AuthorityScope::Retirement),
                  {EvidenceKind::DrainRecord}, std::nullopt)) {
          return false;
        }
        break;
      case LifecycleState::Retiring:
        if (!step(hl_ctx, runtime, key, LifecycleState::Retired, TransitionReason::DecommissionCompleted,
                  authority_bit(AuthorityScope::Decommissioning), {EvidenceKind::DecommissioningRecord},
                  std::nullopt)) {
          return false;
        }
        break;
      default:
        hl_ctx.fail(__FILE__, __LINE__,
                    std::string("the object is in ") + std::string(to_string(object->state)) +
                        " and cannot reach Retired without a service gate decision");
        return false;
    }
  }
  hl_ctx.fail(__FILE__, __LINE__, "the walk to Retired did not terminate");
  return false;
}

/// Walks an object to Installed, which is where it can be named as a successor.
[[nodiscard]] bool walk_to_installed(hl_test::Context& hl_ctx, Runtime& runtime, const ObjectKey& key) {
  return step(hl_ctx, runtime, key, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
              authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt}, std::nullopt) &&
         step(hl_ctx, runtime, key, LifecycleState::Installed, TransitionReason::InstallationCompleted,
              authority_bit(AuthorityScope::Installation),
              {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}, std::nullopt);
}

[[nodiscard]] Result<ReplacementReceipt> link_request(Runtime& runtime, const ObjectKey& predecessor,
                                                      const ObjectKey& successor) {
  const HardwareObject* object = runtime.registry().find(predecessor);
  const std::string plan = unique_name("link");
  ReplacementRequest request;
  request.plan = PlanId::parse(plan).value();
  request.attempt = AttemptId::parse("attempt-1").value();
  request.predecessor = predecessor;
  request.successor = successor;
  if (object != nullptr) {
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
  }
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.provenance = provenance_of(plan, authority_bit(AuthorityScope::Replacement),
                                     {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord});
  return runtime.link_replacement(request);
}

[[nodiscard]] bool opens_ephemeral(hl_test::Context& hl_ctx, std::optional<Runtime>& runtime) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  if (!opened.has_value()) {
    hl_ctx.fail(__FILE__, __LINE__, "the ephemeral runtime could not be opened: " + describe(opened.error()));
    return false;
  }
  runtime.emplace(std::move(opened).value());
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, hostile_identifiers) {
  const std::string control = std::string("asset") + '\x01' + "name";
  const std::string bad_utf8 = std::string("asset") + '\xC3' + '\x28' + "name";
  const std::string too_long(129, 'a');
  HL_CHECK_ERROR(AssetId::parse(control), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse(bad_utf8), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse(too_long), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse("asset/name"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse("asset..name"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse("asset."), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse(".asset"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse("asset-"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(AssetId::parse(std::string("asset-") + '\x7F'), ErrorCode::MalformedRequest);

  // The documented shape admits a single colon and a single dot, and a reserved
  // device name is alphanumeric, so the identifier layer accepts it - as data.
  // What it must never do is silently rewrite what it was given.
  const Result<AssetId> colon = AssetId::parse("site:asset-1");
  HL_REQUIRE(colon.has_value());
  HL_CHECK_EQ(colon.value().value(), std::string("site:asset-1"));
  const Result<AssetId> dotted = AssetId::parse("asset.1");
  HL_REQUIRE(dotted.has_value());
  HL_CHECK_EQ(dotted.value().value(), std::string("asset.1"));
  const Result<AssetId> device = AssetId::parse("CON");
  HL_REQUIRE(device.has_value());
  HL_CHECK_EQ(device.value().value(), std::string("CON"));
  HL_CHECK_EQ(AssetId::parse("NUL").value().value(), std::string("NUL"));
  HL_CHECK_EQ(AssetId::parse(too_long.substr(0, 128)).value().value().size(), std::size_t{128});

  // The boundary the identifier layer does not police is the file system's: a
  // root that the platform resolves to a device rather than a directory is not a
  // store, and it is refused instead of being written through.
  hl_test::TempDir directory("hl_adversarial_requests");
  {
    OpenOptions options;
    options.root = directory.child("NUL");
    const Result<Runtime> device_root = Runtime::open(options);
    HL_CHECK_ERROR(device_root, ErrorCode::StoreCorrupt);
  }
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, counter_bounds) {
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));
  ObjectKey key;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "counter-asset", LifecycleState::Ordered, true, key));

  // A zero counter is an absent counter: never a real value, never a default.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.expected_revision = Revision();
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.expected_lifecycle_generation = LifecycleGeneration();
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.expected_control_epoch = ControlEpoch();
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  HL_CHECK_ERROR(Revision::parse("0"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(Revision::parse("01"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(Revision::parse("18446744073709551616"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(Revision::parse("+1"), ErrorCode::MalformedRequest);

  // Exhaustion is reported, never wrapped.
  const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
  HL_CHECK_ERROR(Revision::from_value(maximum).next(), ErrorCode::CounterOverflow);
  HL_CHECK_ERROR(LifecycleGeneration::from_value(maximum).next(), ErrorCode::CounterOverflow);
  HL_CHECK_ERROR(ControlEpoch::from_value(maximum).next(), ErrorCode::CounterOverflow);
  HL_CHECK_ERROR(LogicalTime::from_value(maximum).next(), ErrorCode::CounterOverflow);
  HL_CHECK_ERROR(CommitSequence::from_value(maximum).next(), ErrorCode::CounterOverflow);
  HL_CHECK_ERROR(ObservationSequence::from_value(maximum).next(), ErrorCode::CounterOverflow);

  // A request that names an exhausted counter is fenced by the same rules as any
  // other stale counter: it is never interpreted as a fresh one.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.expected_revision = Revision::from_value(maximum);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::StaleRevision);
  }
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.key.hardware_generation = HardwareGeneration::from_value(maximum);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::StaleHardwareGeneration);
  }
}

// ---------------------------------------------------------------------------
// Plan identity
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, plan_identity) {
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));

  CreateRequest request;
  const std::string plan = unique_name("plan");
  request.plan = PlanId::parse(plan).value();
  request.attempt = AttemptId::parse("attempt-1").value();
  request.key = key_of("plan-asset", 1);
  request.kind = HardwareKind::Compute;
  request.model = ModelId::parse("model-1").value();
  request.firmware_generation = FirmwareGeneration::first();
  request.initial_state = LifecycleState::Ordered;
  request.location = location_of("site-a", "rack-1", "slot-1");
  request.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                     {EvidenceKind::ProcurementRecord});

  const Result<CreateReceipt> first = runtime->create_object(request);
  HL_REQUIRE(first.has_value());

  // The same request replayed returns the receipt that was issued the first
  // time, byte for byte, and says that it is a replay.
  const Result<CreateReceipt> replay = runtime->create_object(request);
  HL_REQUIRE(replay.has_value());
  HL_CHECK(replay.value().idempotent_replay);
  HL_CHECK_EQ(replay.value().receipt_digest, first.value().receipt_digest);
  HL_CHECK_EQ(replay.value().commit_sequence.value(), first.value().commit_sequence.value());
  HL_CHECK_EQ(runtime->registry().object_count(), std::size_t{1});
  HL_CHECK_EQ(runtime->registry().plan_count(), std::size_t{1});

  // The same plan identity with different content is a conflict, not a second
  // application and not a silent overwrite.
  {
    CreateRequest conflict = request;
    conflict.model = ModelId::parse("model-2").value();
    HL_CHECK_ERROR(runtime->create_object(conflict), ErrorCode::PlanConflict);
    HL_CHECK_EQ(runtime->registry().object_count(), std::size_t{1});
    HL_CHECK_EQ(runtime->registry().plan_count(), std::size_t{1});
  }

  // A transition receipt replays the same way.
  const HardwareObject* object = runtime->registry().find(request.key);
  HL_REQUIRE(object != nullptr);
  TransitionRequest transition = transition_request(hl_ctx, *runtime, request.key, LifecycleState::Staged,
                                                    TransitionReason::DeliveryAccepted,
                                                    authority_bit(AuthorityScope::Logistics),
                                                    {EvidenceKind::DeliveryReceipt});
  const Result<TransitionReceipt> applied = runtime->apply_transition(transition);
  HL_REQUIRE(applied.has_value());
  const Result<TransitionReceipt> applied_again = runtime->apply_transition(transition);
  HL_REQUIRE(applied_again.has_value());
  HL_CHECK(applied_again.value().idempotent_replay);
  HL_CHECK_EQ(applied_again.value().receipt_digest, applied.value().receipt_digest);
  HL_CHECK_EQ(applied_again.value().revision_after.value(), applied.value().revision_after.value());
  HL_CHECK_EQ(runtime->registry().history_entry_count(), std::size_t{2});

  // A different attempt of the same plan is a different fact, and is judged on
  // its own content rather than being folded into the first attempt.
  {
    CreateRequest other = request;
    other.attempt = AttemptId::parse("attempt-2").value();
    other.provenance.attempt = other.attempt;
    HL_CHECK_ERROR(runtime->create_object(other), ErrorCode::ObjectExists);
  }
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, evidence_bounds) {
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));
  ObjectKey key;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "evidence-asset", LifecycleState::Ordered, true, key));

  const auto base = [&]() {
    return transition_request(hl_ctx, *runtime, key, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
                              authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt});
  };

  // The bound is more primary than the shape of the elements: 65 unbound
  // references are refused as a whole rather than examined one by one.
  {
    TransitionRequest request = base();
    request.provenance.evidence.clear();
    for (std::size_t index = 0; index < 65; ++index) {
      EvidenceRef reference;
      reference.kind = EvidenceKind::Unknown;
      reference.digest = Sha256::hash("unbound-" + std::to_string(index));
      reference.source = "evidence://unbound/" + std::to_string(index);
      request.provenance.evidence.push_back(reference);
    }
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::LimitExceeded);
    HL_CHECK(validation_rank(ErrorCode::LimitExceeded) < validation_rank(ErrorCode::MalformedRequest));
  }
  {
    TransitionRequest request = base();
    request.provenance.evidence.push_back(request.provenance.evidence.front());
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = base();
    request.provenance.evidence.front().digest = Digest();
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = base();
    request.provenance.evidence.front().source = std::string("evidence") + '\xC3' + '\x28' + "//0";
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = base();
    request.provenance.evidence.front().source = std::string("evidence") + '\x01' + "//0";
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = base();
    request.provenance.evidence.front().kind = EvidenceKind::Unknown;
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = base();
    request.provenance.evidence.clear();
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MissingEvidence);
  }
  {
    TransitionRequest request = base();
    request.provenance.evidence.front().kind = EvidenceKind::RemovalRecord;
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MissingEvidence);
  }
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, authority_masks) {
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));
  ObjectKey key;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "authority-asset", LifecycleState::Ordered, true, key));

  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.provenance.authority = AuthorityMask(1u << 20);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.provenance.authority = authority_bit(AuthorityScope::Logistics) | AuthorityMask(1u << 31);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  // An empty mask is the absence of authority, and absence is never authority.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted, AuthorityMask(),
                                                   {EvidenceKind::DeliveryReceipt});
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::InsufficientAuthority);
  }
  // A mask inside the declared set that simply does not cover this change is a
  // different failure from a mask that cannot be interpreted at all.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Procurement),
                                                   {EvidenceKind::DeliveryReceipt});
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::InsufficientAuthority);
  }
  HL_CHECK_ERROR(parse_authority_mask(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_authority_mask("none"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_authority_mask("service,service"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_authority_mask("teleportation"), ErrorCode::MalformedRequest);
}

// ---------------------------------------------------------------------------
// Spellings
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, unknown_spellings) {
  HL_CHECK_ERROR(parse_lifecycle_state("instaled"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_lifecycle_state("installed"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_lifecycle_state(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_transition_reason("bogus"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_actor_kind("robot"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_eligibility_gate("maybe"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_evidence_kind("receipt"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_hardware_kind("gpu"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_location("site/rack"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_location("site//slot"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_location("site/rack/slot/extra"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_object_key("asset-1"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_object_key("asset-1@0"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_object_key("asset-1@1@2"), ErrorCode::MalformedRequest);

  // The absence of a value has a spelling of its own, and parsing it produces
  // the absence - never a usable value. The evidence kind is the exception: it
  // is the one enumeration whose zero value has no spelling at all.
  const Result<TransitionReason> unset = parse_transition_reason("unset");
  HL_REQUIRE(unset.has_value());
  HL_CHECK(unset.value() == TransitionReason::Unset);
  HL_CHECK(reason_bit(unset.value()).empty());
  const Result<ActorKind> unknown_actor = parse_actor_kind("unknown");
  HL_REQUIRE(unknown_actor.has_value());
  HL_CHECK(unknown_actor.value() == ActorKind::Unknown);
  const Result<EligibilityGate> unknown_gate = parse_eligibility_gate("unknown");
  HL_REQUIRE(unknown_gate.has_value());
  HL_CHECK(unknown_gate.value() == EligibilityGate::Unknown);
  const Result<HardwareKind> unknown_kind = parse_hardware_kind("unknown");
  HL_REQUIRE(unknown_kind.has_value());
  HL_CHECK(unknown_kind.value() == HardwareKind::Unknown);
  HL_CHECK_ERROR(parse_evidence_kind("unknown"), ErrorCode::MalformedRequest);

  // Carrying the absence into a request is what is refused. A request is not a
  // place where absence may be reinterpreted as a usable value.
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));
  ObjectKey key;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "spelling-asset", LifecycleState::Ordered, true, key));
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::Unset, authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
  {
    EligibilityGateRequest request;
    request.plan = PlanId::parse(unique_name("spelling-gate")).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.gate = EligibilityGate::Unknown;
    const HardwareObject* object = runtime->registry().find(key);
    HL_REQUIRE(object != nullptr);
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
    request.expected_control_epoch = runtime->registry().control_epoch();
    request.provenance = provenance_of(request.plan.value(), authority_bit(AuthorityScope::Service),
                                       {EvidenceKind::ServiceRecord});
    HL_CHECK_ERROR(runtime->set_eligibility_gate(request), ErrorCode::MalformedRequest);
  }
  {
    CreateRequest request;
    const std::string plan = unique_name("spelling-create");
    request.plan = PlanId::parse(plan).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key_of("spelling-other", 1);
    request.kind = HardwareKind::Unknown;
    request.model = ModelId::parse("model-1").value();
    request.firmware_generation = FirmwareGeneration::first();
    request.initial_state = LifecycleState::Ordered;
    request.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                       {EvidenceKind::ProcurementRecord});
    HL_CHECK_ERROR(runtime->create_object(request), ErrorCode::MalformedRequest);
  }
  {
    CreateRequest request;
    const std::string plan = unique_name("spelling-actor");
    request.plan = PlanId::parse(plan).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key_of("spelling-actor", 1);
    request.kind = HardwareKind::Compute;
    request.model = ModelId::parse("model-1").value();
    request.firmware_generation = FirmwareGeneration::first();
    request.initial_state = LifecycleState::Ordered;
    request.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                       {EvidenceKind::ProcurementRecord});
    request.provenance.actor.kind = ActorKind::Unknown;
    HL_CHECK_ERROR(runtime->create_object(request), ErrorCode::MalformedRequest);
  }
}

// ---------------------------------------------------------------------------
// Successors
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, successor_attacks) {
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));

  ObjectKey predecessor;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-predecessor", LifecycleState::Ordered, true, predecessor));
  HL_REQUIRE(walk_to_retired(hl_ctx, *runtime, predecessor));

  ObjectKey staged;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-staged", LifecycleState::Ordered, true, staged));
  HL_REQUIRE(step(hl_ctx, *runtime, staged, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
                  authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt}, std::nullopt));

  ObjectKey installed;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-installed", LifecycleState::Ordered, true, installed));
  HL_REQUIRE(walk_to_installed(hl_ctx, *runtime, installed));

  // The object itself.
  HL_CHECK_ERROR(link_request(*runtime, predecessor, predecessor), ErrorCode::SelfReplacement);
  // An object that does not exist.
  HL_CHECK_ERROR(link_request(*runtime, predecessor, key_of("successor-ghost", 1)), ErrorCode::UnknownAsset);
  // A successor that is still ordered or staged: it is not installed, so it
  // cannot have replaced anything yet.
  {
    ObjectKey ordered;
    HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-ordered", LifecycleState::Ordered, true, ordered));
    HL_CHECK_ERROR(link_request(*runtime, predecessor, ordered), ErrorCode::InvalidSuccessor);
  }
  HL_CHECK_ERROR(link_request(*runtime, predecessor, staged), ErrorCode::InvalidSuccessor);

  // An object that has already been superseded already belongs to a chain: the
  // lineage refuses to give it a second origin.
  {
    ObjectKey middle;
    HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-middle", LifecycleState::Ordered, true, middle));
    HL_REQUIRE(walk_to_installed(hl_ctx, *runtime, middle));
    HL_REQUIRE(link_request(*runtime, predecessor, middle).has_value());
    HL_REQUIRE(walk_to_retired(hl_ctx, *runtime, middle));

    ObjectKey tail;
    HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-tail", LifecycleState::Ordered, true, tail));
    HL_REQUIRE(walk_to_installed(hl_ctx, *runtime, tail));
    HL_REQUIRE(link_request(*runtime, middle, tail).has_value());
    const HardwareObject* middle_object = runtime->registry().find(middle);
    HL_REQUIRE(middle_object != nullptr);
    TransitionRequest replaced = transition_request(hl_ctx, *runtime, middle, LifecycleState::Replaced,
                                                    TransitionReason::SuccessorLinked,
                                                    authority_bit(AuthorityScope::Replacement),
                                                    {EvidenceKind::ReplacementAuthorization,
                                                     EvidenceKind::SuccessorRecord});
    replaced.successor = tail;
    HL_REQUIRE(runtime->apply_transition(replaced).has_value());
    HL_CHECK(runtime->registry().find(middle)->state == LifecycleState::Replaced);

    ObjectKey fresh;
    HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-fresh", LifecycleState::Ordered, true, fresh));
    HL_REQUIRE(walk_to_retired(hl_ctx, *runtime, fresh));
    HL_CHECK_ERROR(link_request(*runtime, fresh, middle), ErrorCode::ObjectExists);
  }

  // A successor on an edge that does not carry one.
  {
    ObjectKey other;
    HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-surplus", LifecycleState::Ordered, true, other));
    TransitionRequest request = transition_request(hl_ctx, *runtime, other, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.successor = installed;
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::InvalidSuccessor);
  }

  // A replacement edge that names no successor at all.
  {
    ObjectKey bare;
    HL_REQUIRE(create_object(hl_ctx, *runtime, "successor-bare", LifecycleState::Ordered, true, bare));
    HL_REQUIRE(walk_to_retired(hl_ctx, *runtime, bare));
    TransitionRequest request = transition_request(hl_ctx, *runtime, bare, LifecycleState::Replaced,
                                                   TransitionReason::SuccessorLinked,
                                                   authority_bit(AuthorityScope::Replacement),
                                                   {EvidenceKind::ReplacementAuthorization,
                                                    EvidenceKind::SuccessorRecord});
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MissingSuccessor);
  }
}

// ---------------------------------------------------------------------------
// Identity and fencing
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, identity_fencing) {
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));
  ObjectKey key;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "identity-asset", LifecycleState::Ordered, true, key));

  // A hardware generation that is not the one on record is not a new object.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.key.hardware_generation = HardwareGeneration::from_value(2);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::StaleHardwareGeneration);
  }
  // An asset nobody has ever registered is unknown, not zero, not empty.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.key = key_of("identity-ghost", 1);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::UnknownAsset);
  }
  // A writer epoch from another session fences the request even when everything
  // else about it is correct.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.expected_control_epoch = ControlEpoch::from_value(runtime->registry().control_epoch().value() + 1);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::StaleAuthority);
  }
  // An object state the request was not planned against.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.expected_state = LifecycleState::Active;
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::StateMismatch);
  }
  // An edge the table does not have, and a reason the edge does not accept.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Active,
                                                   TransitionReason::CommissioningPassed,
                                                   authority_bit(AuthorityScope::Service),
                                                   {EvidenceKind::CommissioningReport});
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::IllegalTransition);
  }
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::ServiceRestored,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::IllegalTransition);
  }
  // Requesting removal of an object that does not exist is not a removal.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.key = key_of("identity-absent", 3);
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::UnknownAsset);
  }
  // The plan identity is part of the fence: a request without one cannot be
  // reasoned about at all.
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, key, LifecycleState::Staged,
                                                   TransitionReason::DeliveryAccepted,
                                                   authority_bit(AuthorityScope::Logistics),
                                                   {EvidenceKind::DeliveryReceipt});
    request.plan = PlanId();
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }
}

// ---------------------------------------------------------------------------
// Locations
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, location_rules) {
  std::optional<Runtime> runtime;
  HL_REQUIRE(opens_ephemeral(hl_ctx, runtime));

  // An incomplete location is not a location.
  Location partial;
  partial.site = SiteId::parse("site-a").value();
  partial.rack = RackId::parse("rack-1").value();
  {
    CreateRequest request;
    const std::string plan = unique_name("location");
    request.plan = PlanId::parse(plan).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key_of("location-partial", 1);
    request.kind = HardwareKind::Compute;
    request.model = ModelId::parse("model-1").value();
    request.firmware_generation = FirmwareGeneration::first();
    request.initial_state = LifecycleState::Ordered;
    request.location = partial;
    request.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                       {EvidenceKind::ProcurementRecord});
    HL_CHECK_ERROR(runtime->create_object(request), ErrorCode::MalformedRequest);
  }

  // A state that requires a location with none anywhere.
  ObjectKey no_location;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "location-none", LifecycleState::Ordered, false, no_location));
  HL_REQUIRE(step(hl_ctx, *runtime, no_location, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
                  authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt}, std::nullopt));
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, no_location, LifecycleState::Installed,
                                                   TransitionReason::InstallationCompleted,
                                                   authority_bit(AuthorityScope::Installation),
                                                   {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord});
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MissingLocation);
  }
  {
    TransitionRequest request = transition_request(hl_ctx, *runtime, no_location, LifecycleState::Installed,
                                                   TransitionReason::InstallationCompleted,
                                                   authority_bit(AuthorityScope::Installation),
                                                   {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord});
    request.location = partial;
    HL_CHECK_ERROR(runtime->apply_transition(request), ErrorCode::MalformedRequest);
  }

  // A location on a state that does not require one is not silently dropped and
  // not silently invented: it is recorded exactly as supplied, and it stays
  // recorded. requires_location() is a lower bound on what a state must have,
  // never a rule that says a state may not have one.
  ObjectKey staged;
  HL_REQUIRE(create_object(hl_ctx, *runtime, "location-surplus", LifecycleState::Ordered, false, staged));
  HL_CHECK(!requires_location(LifecycleState::Staged));
  HL_CHECK(!requires_location(LifecycleState::Ordered));
  HL_CHECK(requires_location(LifecycleState::Installed));
  {
    const Location supplied = location_of("site-z", "rack-9", "slot-9");
    HL_REQUIRE(step(hl_ctx, *runtime, staged, LifecycleState::Staged, TransitionReason::DeliveryAccepted,
                    authority_bit(AuthorityScope::Logistics), {EvidenceKind::DeliveryReceipt}, supplied));
    const HardwareObject* object = runtime->registry().find(staged);
    HL_REQUIRE(object != nullptr);
    HL_CHECK(object->location.has_value());
    HL_CHECK(object->location.value() == supplied);
    // The recorded location is a real reference, so a site query finds it.
    ListQuery query;
    query.site = supplied.site;
    const Result<std::vector<ObjectView>> listed = runtime->list(query);
    HL_REQUIRE(listed.has_value());
    HL_CHECK_EQ(listed.value().size(), std::size_t{1});
  }
}

// ---------------------------------------------------------------------------
// Store lifecycle
// ---------------------------------------------------------------------------

HL_TEST(adversarial_requests, store_open_close_loop) {
  hl_test::TempDir directory("hl_adversarial_requests");
  const std::filesystem::path root = directory.child("store");
  ObjectKey key;

  // Opening and closing the same store repeatedly must not leak the writer lock
  // (the next open would fail with StoreLocked), must not lose the state, and
  // must advance the fence once per open.
  std::optional<CommitSequence> sequence;
  std::optional<Digest> digest;
  std::uint64_t previous_epoch = 0;
  for (int cycle = 0; cycle < 16; ++cycle) {
    OpenOptions options;
    options.root = root;
    Result<Runtime> store = Runtime::open(options);
    HL_REQUIRE(store.has_value());
    if (cycle == 0) {
      HL_REQUIRE(create_object(hl_ctx, store.value(), "loop-asset", LifecycleState::Ordered, true, key));
    }
    const Result<Digest> verified = store.value().verify();
    HL_CHECK(succeeded(hl_ctx, verified, "the reopened store verifies"));
    HL_CHECK_EQ(store.value().registry().object_count(), std::size_t{1});
    if (!sequence.has_value()) {
      sequence = store.value().registry().commit_sequence();
      digest = store.value().registry().state_digest();
    }
    HL_CHECK_EQ(store.value().registry().commit_sequence().value(), sequence->value());
    HL_CHECK_EQ(store.value().registry().state_digest().hex(), digest->hex());
    const std::uint64_t epoch = store.value().registry().control_epoch().value();
    if (cycle > 0) {
      HL_CHECK_EQ(epoch, previous_epoch + 1);
    }
    previous_epoch = epoch;

    // While this runtime holds the store, a second writer cannot have it. This
    // is what makes the loop a leak detector rather than a smoke test.
    OpenOptions second;
    second.root = root;
    HL_CHECK_ERROR(Runtime::open(second), ErrorCode::StoreLocked);
  }

  // The same store opened read only answers everything and mutates nothing.
  Result<Runtime> reader = Runtime::open_read_only(root);
  HL_REQUIRE(reader.has_value());
  HL_CHECK(!reader.value().writable());
  HL_CHECK(succeeded(hl_ctx, reader.value().inspect(key), "read only inspect"));
  HL_CHECK(succeeded(hl_ctx, reader.value().list(ListQuery()), "read only list"));
  HL_CHECK(succeeded(hl_ctx, reader.value().history(key), "read only history"));
  HL_CHECK(succeeded(hl_ctx, reader.value().lineage(key), "read only lineage"));
  HL_CHECK(succeeded(hl_ctx, reader.value().verify(), "read only verify"));
  HL_CHECK(succeeded(hl_ctx, reader.value().status(), "read only status"));
  HL_CHECK(succeeded(hl_ctx, reader.value().export_document(ExportOptions()), "read only export"));
  HL_CHECK(succeeded(hl_ctx, reader.value().all_links(), "read only all links"));
  HL_CHECK(succeeded(hl_ctx, reader.value().preflight_transition(TransitionRequest()), "read only preflight"));
  HL_CHECK_EQ(reader.value().registry().state_digest().hex(), digest->hex());
}

HL_TEST(adversarial_requests, read_only_rejects_every_mutation) {
  hl_test::TempDir directory("hl_adversarial_requests");
  const std::filesystem::path root = directory.child("store");
  ObjectKey key;
  {
    OpenOptions options;
    options.root = root;
    Result<Runtime> writer = Runtime::open(options);
    HL_REQUIRE(writer.has_value());
    HL_REQUIRE(create_object(hl_ctx, writer.value(), "readonly-asset", LifecycleState::Ordered, true, key));
  }

  Result<Runtime> reader = Runtime::open_read_only(root);
  HL_REQUIRE(reader.has_value());
  const std::size_t objects_before = reader.value().registry().object_count();
  const std::string digest_before = reader.value().registry().state_digest().hex();

  const HardwareObject* object = reader.value().registry().find(key);
  HL_REQUIRE(object != nullptr);

  {
    const std::string plan = unique_name("readonly-create");
    CreateRequest request;
    request.plan = PlanId::parse(plan).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key_of("readonly-other", 1);
    request.kind = HardwareKind::Compute;
    request.model = ModelId::parse("model-1").value();
    request.firmware_generation = FirmwareGeneration::first();
    request.initial_state = LifecycleState::Ordered;
    request.provenance = provenance_of(plan, authority_bit(AuthorityScope::Procurement),
                                       {EvidenceKind::ProcurementRecord});
    HL_CHECK_ERROR(reader.value().create_object(request), ErrorCode::ReadOnlyAuthority);
  }
  {
    TransitionRequest request;
    request.plan = PlanId::parse(unique_name("readonly-transition")).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
    request.expected_control_epoch = reader.value().registry().control_epoch();
    request.expected_state = object->state;
    request.target_state = LifecycleState::Staged;
    request.reason = TransitionReason::DeliveryAccepted;
    request.provenance = provenance_of(request.plan.value(), authority_bit(AuthorityScope::Logistics),
                                       {EvidenceKind::DeliveryReceipt});
    HL_CHECK_ERROR(reader.value().apply_transition(request), ErrorCode::ReadOnlyAuthority);
  }
  {
    ReplacementRequest request;
    request.plan = PlanId::parse(unique_name("readonly-link")).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.predecessor = key;
    request.successor = key;
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
    request.expected_control_epoch = reader.value().registry().control_epoch();
    request.provenance = provenance_of(request.plan.value(), authority_bit(AuthorityScope::Replacement),
                                       {EvidenceKind::ReplacementAuthorization});
    HL_CHECK_ERROR(reader.value().link_replacement(request), ErrorCode::ReadOnlyAuthority);
  }
  {
    EligibilityGateRequest request;
    request.plan = PlanId::parse(unique_name("readonly-gate")).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.gate = EligibilityGate::Open;
    request.expected_lifecycle_generation = object->lifecycle_generation;
    request.expected_revision = object->revision;
    request.expected_control_epoch = reader.value().registry().control_epoch();
    request.provenance = provenance_of(request.plan.value(), authority_bit(AuthorityScope::Service),
                                       {EvidenceKind::ServiceRecord});
    HL_CHECK_ERROR(reader.value().set_eligibility_gate(request), ErrorCode::ReadOnlyAuthority);
  }
  {
    HealthObservationRequest request;
    request.plan = PlanId::parse(unique_name("readonly-health")).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.observation.key = key;
    request.observation.evidence_digest = Sha256::hash("readonly-observation");
    request.observation.source = "evidence://readonly/0";
    request.provenance = provenance_of(request.plan.value(), authority_bit(AuthorityScope::Service),
                                       {EvidenceKind::HealthEvidence});
    HL_CHECK_ERROR(reader.value().observe_health(request), ErrorCode::ReadOnlyAuthority);
  }
  {
    AttestationRequest request;
    request.plan = PlanId::parse(unique_name("readonly-attest")).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.expected_revision = object->revision;
    request.expected_control_epoch = reader.value().registry().control_epoch();
    request.provenance = provenance_of(request.plan.value(), authority_bit(AuthorityScope::Recovery),
                                       {EvidenceKind::RecoveryAttestation});
    HL_CHECK_ERROR(reader.value().attest_authority(request), ErrorCode::ReadOnlyAuthority);
  }
  HL_CHECK_ERROR(reader.value().import_document("{}", ImportOptions()), ErrorCode::ReadOnlyAuthority);

  // After every rejection the reader still answers, and the store it opened is
  // exactly the store it opened.
  HL_CHECK_EQ(reader.value().registry().object_count(), objects_before);
  HL_CHECK_EQ(reader.value().registry().state_digest().hex(), digest_before);
  HL_CHECK(succeeded(hl_ctx, reader.value().verify(), "the reader still verifies after every rejection"));
}