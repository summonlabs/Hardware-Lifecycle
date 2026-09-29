// Hardware Lifecycle - example: the lifecycle walkthrough.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Documentation that runs. One hardware object walks the legal edges of the
// lifecycle machine on an ephemeral runtime:
//
//   Ordered -> Staged -> Installed -> Commissioning -> Active -> Maintenance
//   -> Active -> Retiring -> Retired -> Removed
//
// For every step the program reads the edge out of the transition table with
// find_rule(), builds a request from exactly the authority and evidence that
// rule demands, applies the request and prints the values the runtime returned:
// the state pair, the revision, the lifecycle generation, the commit sequence,
// the logical time and the receipt digest.
//
// Three refusals are shown as well, because what a runtime refuses is part of
// its contract:
//   * an edge the table does not declare is rejected as illegal_transition;
//   * a request that violates several rules at once reports the lowest ranking
//     violation as its primary error, not whichever check happened to run first;
//   * returning an object to service while the eligibility gate is closed is
//     refused. No transition opens the gate: opening it is a separate decision
//     with its own authority, evidence, revision and receipt.
//
// Finally it shows that a failed health observation changes nothing: the
// lifecycle state, the revision and the lifecycle generation are identical
// before and after it. Observation is not authority.
//
// The runtime is ephemeral, so no durable commit point is passed and nothing
// survives the process. The working directory is created, used to show that an
// ephemeral runtime writes no store there, and removed again unless --keep is
// given.
//
// usage: example_lifecycle_walkthrough [<directory>] [--keep]

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"

using namespace hardware_lifecycle;

namespace {

constexpr std::string_view kExampleName = "example_lifecycle_walkthrough";

// ---------------------------------------------------------------------------
// Output primitives
// ---------------------------------------------------------------------------

void say(std::string_view text) { std::printf("%.*s\n", static_cast<int>(text.size()), text.data()); }

/// Canonical library spelling of any value that has one.
template <class T>
std::string spell(T value) {
  return std::string(to_string(value));
}

std::string number(std::uint64_t value) { return std::to_string(value); }

/// A counter is either a real value or absent. Zero is never printed in place of
/// an absent counter: absence is reported as absence.
template <class Counter>
std::string counter(Counter value) {
  return value.valid() ? std::to_string(value.value()) : std::string("unset");
}

std::string digest_text(const Digest& digest) { return digest.is_zero() ? std::string("unset") : digest.hex(); }

int fail(const Error& error) {
  std::fprintf(stderr, "%.*s: FAILED: %s: %s\n", static_cast<int>(kExampleName.size()), kExampleName.data(),
               std::string(to_string(error.code)).c_str(), error.message.c_str());
  for (const FieldNote& note : error.notes) {
    std::fprintf(stderr, "  note: %s: %s\n", note.field.c_str(), note.detail.c_str());
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Command line and working directory
// ---------------------------------------------------------------------------

struct Options {
  std::filesystem::path directory;
  bool keep = false;
  bool help = false;
};

void print_usage() {
  say("usage: example_lifecycle_walkthrough [<directory>] [--keep]");
  say("");
  say("  <directory>  directory to work in, created when it does not exist");
  say("               (default: <system temporary directory>/example_lifecycle_walkthrough)");
  say("  --keep       leave the working directory behind instead of removing it");
  say("  --help       print this text and exit");
}

Result<Options> parse_options(int argc, char** argv) {
  Options options;
  bool directory_seen = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--keep") {
      options.keep = true;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument.size() > 2 && argument[0] == '-' && argument[1] == '-') {
      return Error::make(ErrorCode::MalformedRequest, "unknown option").with("option", std::string(argument));
    }
    if (directory_seen) {
      return Error::make(ErrorCode::MalformedRequest, "at most one working directory may be given")
          .with("directory", std::string(argument));
    }
    options.directory = std::filesystem::path(std::string(argument));
    directory_seen = true;
  }
  return options;
}

/// The directory this program works in. It is created when it is missing and
/// removed again, together with everything created inside it, unless the caller
/// asked for it to be kept.
class Workspace {
 public:
  static Result<Workspace> create(const std::filesystem::path& requested, std::string_view default_name, bool keep) {
    Workspace workspace;
    workspace.keep_ = keep;
    std::error_code error;
    if (requested.empty()) {
      const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
      if (error) {
        return Error::make(ErrorCode::IoFailure, "the system temporary directory could not be resolved")
            .with("cause", error.message());
      }
      workspace.root_ = temporary / std::filesystem::path(std::string(default_name));
    } else {
      workspace.root_ = requested;
    }

    std::error_code exists_error;
    const bool existed = std::filesystem::exists(workspace.root_, exists_error);
    if (exists_error) {
      return Error::make(ErrorCode::IoFailure, "the working directory could not be inspected")
          .with("path", workspace.root_.string())
          .with("cause", exists_error.message());
    }
    workspace.created_root_ = !existed;

    std::error_code create_error;
    std::filesystem::create_directories(workspace.root_, create_error);
    std::error_code status_error;
    if (!std::filesystem::is_directory(workspace.root_, status_error)) {
      return Error::make(ErrorCode::IoFailure, "the working directory could not be created")
          .with("path", workspace.root_.string())
          .with("cause", create_error ? create_error.message() : status_error.message());
    }
    return workspace;
  }

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  /// Creates a directory below the root and remembers it for cleanup.
  Result<std::filesystem::path> subdirectory(std::string_view name) {
    const std::filesystem::path path = root_ / std::filesystem::path(std::string(name));
    std::error_code create_error;
    std::filesystem::create_directories(path, create_error);
    std::error_code status_error;
    if (!std::filesystem::is_directory(path, status_error)) {
      return Error::make(ErrorCode::IoFailure, "a working subdirectory could not be created")
          .with("path", path.string())
          .with("cause", create_error ? create_error.message() : status_error.message());
    }
    created_.push_back(path);
    return path;
  }

  void cleanup() const {
    if (keep_) {
      std::printf("working directory left behind (--keep): %s\n", root_.string().c_str());
      return;
    }
    std::error_code error;
    if (created_root_) {
      // The directory did not exist before this run, so it is ours to remove
      // whole.
      std::filesystem::remove_all(root_, error);
    } else {
      // The caller supplied an existing directory: only what this run created
      // inside it is removed.
      for (const std::filesystem::path& path : created_) {
        std::filesystem::remove_all(path, error);
      }
    }
    std::printf("working directory removed: %s\n", root_.string().c_str());
  }

 private:
  Workspace() = default;

  std::filesystem::path root_;
  std::vector<std::filesystem::path> created_;
  bool keep_ = false;
  bool created_root_ = false;
};

// ---------------------------------------------------------------------------
// Requests derived from the transition table
// ---------------------------------------------------------------------------

/// The actor kind that owns the first scope of a requirement. The actor records
/// who asked; the scope records what they were allowed to ask for.
ActorKind actor_for(AuthorityMask mask) {
  for (std::size_t raw = 0; raw < kAuthorityScopeCount; ++raw) {
    const AuthorityScope scope = static_cast<AuthorityScope>(raw);
    if (!mask.intersects(authority_bit(scope))) {
      continue;
    }
    switch (scope) {
      case AuthorityScope::Procurement:
        return ActorKind::Procurement;
      case AuthorityScope::Logistics:
        return ActorKind::Logistics;
      case AuthorityScope::Installation:
        return ActorKind::Installation;
      case AuthorityScope::Commissioning:
        return ActorKind::Commissioning;
      case AuthorityScope::Service:
        // ActorKind describes who asked; it has no service kind, because the
        // service authority is exercised by an operator or by automation.
        return ActorKind::Automation;
      case AuthorityScope::Maintenance:
        return ActorKind::Maintenance;
      case AuthorityScope::Integrity:
        return ActorKind::Integrity;
      case AuthorityScope::Retirement:
        return ActorKind::Retirement;
      case AuthorityScope::Decommissioning:
        return ActorKind::Decommissioning;
      case AuthorityScope::Replacement:
        return ActorKind::Replacement;
      case AuthorityScope::Recovery:
        return ActorKind::Recovery;
    }
  }
  return ActorKind::Automation;
}

/// One evidence reference. The digest binds the exact artifact the step is
/// planned against; the runtime never interprets the artifact, it only records
/// which bytes were claimed.
EvidenceRef make_evidence_ref(EvidenceKind kind, const ObjectKey& key, LifecycleState from, LifecycleState to,
                              std::uint64_t step) {
  const std::string artifact = "walkthrough artifact #" + std::to_string(step) + " " + std::string(to_string(kind)) +
                               " for " + to_string(key) + " " + spell(from) + " -> " + spell(to);
  EvidenceRef reference;
  reference.kind = kind;
  reference.digest = Sha256::hash(artifact);
  reference.source = "example.walkthrough/" + std::string(to_string(kind));
  return reference;
}

/// Every kind the rule requires. An empty requirement means "at least one
/// reference of any kind", because every externally meaningful change binds to
/// evidence it was planned against.
std::vector<EvidenceRef> evidence_for(const TransitionRule& rule, const ObjectKey& key, LifecycleState from,
                                      LifecycleState to, std::uint64_t step) {
  std::vector<EvidenceRef> references;
  for (std::size_t raw = 1; raw < kEvidenceKindCount; ++raw) {
    const EvidenceKind kind = static_cast<EvidenceKind>(raw);
    if (rule.required_evidence.intersects(evidence_bit(kind))) {
      references.push_back(make_evidence_ref(kind, key, from, to, step));
    }
  }
  if (references.empty()) {
    references.push_back(make_evidence_ref(EvidenceKind::ServiceRecord, key, from, to, step));
  }
  return references;
}

// ---------------------------------------------------------------------------
// Receipt rendering
// ---------------------------------------------------------------------------

void print_evidence(const std::vector<EvidenceRef>& references) {
  for (const EvidenceRef& reference : references) {
    std::printf("  evidence     : %s digest=%s source=%s observed_sequence=%s\n", spell(reference.kind).c_str(),
                digest_text(reference.digest).c_str(), reference.source.c_str(),
                counter(reference.observed_sequence).c_str());
  }
}

void print_transition_receipt(const TransitionReceipt& receipt) {
  std::printf("  receipt      : %s %s -> %s reason=%s\n", to_string(receipt.key).c_str(), spell(receipt.from).c_str(),
              spell(receipt.to).c_str(), spell(receipt.reason).c_str());
  std::printf("  revision     : %s -> %s | lifecycle generation %s | gate after %s\n",
              counter(receipt.revision_before).c_str(), counter(receipt.revision_after).c_str(),
              counter(receipt.lifecycle_generation).c_str(), spell(receipt.gate_after).c_str());
  std::printf("  commit point : commit sequence %s | logical time %s\n", counter(receipt.commit_sequence).c_str(),
              counter(receipt.logical_time).c_str());
  std::printf("  digests      : request=%s entry=%s receipt=%s\n", digest_text(receipt.request_digest).c_str(),
              digest_text(receipt.entry_digest).c_str(), digest_text(receipt.receipt_digest).c_str());
  std::printf("  replay       : %s\n", receipt.idempotent_replay ? "true" : "false");
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------

/// Drives one object along the edges the transition table declares. Every step
/// is planned against the authoritative state the runtime reports at that
/// moment; nothing is carried over from the previous step except the identity.
class Walk {
 public:
  Walk(Runtime& runtime, ObjectKey key, ActorId actor)
      : runtime_(runtime), key_(std::move(key)), actor_(std::move(actor)) {}

  [[nodiscard]] const ObjectKey& key() const noexcept { return key_; }

  Result<TransitionReceipt> step(std::string_view narrative, LifecycleState target, TransitionReason reason,
                                 std::optional<Location> location = std::nullopt) {
    const Result<ObjectView> view = runtime_.inspect(key_);
    if (!view.has_value()) {
      return view.error();
    }
    const HardwareObject& object = view.value().object;

    const TransitionRule* rule = find_rule(object.state, target);
    if (rule == nullptr) {
      return Error::make(ErrorCode::IllegalTransition,
                         "the transition table has no edge from this state to that state")
          .with("from", spell(object.state))
          .with("to", spell(target));
    }

    ++index_;
    std::printf("\nstep %s: %s -> %s\n", number(index_).c_str(), spell(object.state).c_str(), spell(target).c_str());
    std::printf("  why          : %.*s\n", static_cast<int>(narrative.size()), narrative.data());
    std::printf("  table rule   : %s\n", describe_rule(*rule).c_str());

    const Result<PlanId> plan = PlanId::parse("walkthrough.plan." + std::to_string(index_));
    if (!plan.has_value()) {
      return plan.error();
    }
    const Result<AttemptId> attempt = AttemptId::parse("walkthrough.attempt." + std::to_string(index_));
    if (!attempt.has_value()) {
      return attempt.error();
    }
    const std::vector<EvidenceRef> evidence =
        evidence_for(*rule, key_, object.state, target, index_);

    TransitionRequest request;
    request.plan = plan.value();
    request.attempt = attempt.value();
    request.key = key_;
    request.expected_lifecycle_generation = object.lifecycle_generation;
    request.expected_revision = object.revision;
    request.expected_control_epoch = runtime_.registry().control_epoch();
    request.expected_state = object.state;
    request.target_state = target;
    request.reason = reason;
    request.location = location;
    request.provenance.actor.id = actor_;
    request.provenance.actor.kind = actor_for(rule->required_authority);
    request.provenance.authority = rule->required_authority;
    request.provenance.policy_generation = PolicyGeneration::first();
    request.provenance.plan = plan.value();
    request.provenance.attempt = attempt.value();
    request.provenance.evidence = evidence;

    std::printf("  planned for  : state=%s revision=%s lifecycle generation=%s control epoch=%s\n",
                spell(object.state).c_str(), counter(object.revision).c_str(),
                counter(object.lifecycle_generation).c_str(), counter(runtime_.registry().control_epoch()).c_str());
    std::printf("  authority    : %s (actor %s as %s)\n", spell(rule->required_authority).c_str(),
                actor_.value().c_str(), spell(actor_for(rule->required_authority)).c_str());
    print_evidence(evidence);

    const Result<TransitionReceipt> receipt = runtime_.apply_transition(request);
    if (!receipt.has_value()) {
      return receipt.error();
    }
    print_transition_receipt(receipt.value());
    return receipt;
  }

  /// Opens the service eligibility gate. This is not a lifecycle transition: it
  /// is a separate decision with its own authority and its own receipt.
  Result<EligibilityGateReceipt> open_service_gate(std::string_view narrative) {
    const Result<ObjectView> view = runtime_.inspect(key_);
    if (!view.has_value()) {
      return view.error();
    }
    const HardwareObject& object = view.value().object;

    ++index_;
    const Result<PlanId> plan = PlanId::parse("walkthrough.plan.gate." + std::to_string(index_));
    if (!plan.has_value()) {
      return plan.error();
    }
    const Result<AttemptId> attempt = AttemptId::parse("walkthrough.attempt.gate." + std::to_string(index_));
    if (!attempt.has_value()) {
      return attempt.error();
    }

    EligibilityGateRequest request;
    request.plan = plan.value();
    request.attempt = attempt.value();
    request.key = key_;
    request.gate = EligibilityGate::Open;
    request.expected_lifecycle_generation = object.lifecycle_generation;
    request.expected_revision = object.revision;
    request.expected_control_epoch = runtime_.registry().control_epoch();
    request.provenance.actor.id = actor_;
    request.provenance.actor.kind = ActorKind::Operator;
    request.provenance.authority = authority_bit(AuthorityScope::Service);
    request.provenance.policy_generation = PolicyGeneration::first();
    request.provenance.plan = plan.value();
    request.provenance.attempt = attempt.value();
    request.provenance.evidence.push_back(
        make_evidence_ref(EvidenceKind::ServiceRecord, key_, object.state, object.state, index_));

    std::printf("\ngate decision %s\n", number(index_).c_str());
    std::printf("  why          : %.*s\n", static_cast<int>(narrative.size()), narrative.data());
    std::printf("  gate rule    : separate authority: %s | evidence: any bound reference\n",
                spell(authority_bit(AuthorityScope::Service)).c_str());
    std::printf("  planned for  : gate=%s revision=%s lifecycle generation=%s control epoch=%s\n",
                spell(object.eligibility_gate).c_str(), counter(object.revision).c_str(),
                counter(object.lifecycle_generation).c_str(), counter(runtime_.registry().control_epoch()).c_str());
    print_evidence(request.provenance.evidence);

    const Result<EligibilityGateReceipt> receipt = runtime_.set_eligibility_gate(request);
    if (!receipt.has_value()) {
      return receipt.error();
    }
    const EligibilityGateReceipt& value = receipt.value();
    std::printf("  receipt      : %s gate %s -> %s revision %s\n", to_string(value.key).c_str(),
                spell(value.before).c_str(), spell(value.after).c_str(), counter(value.revision).c_str());
    std::printf("  commit point : commit sequence %s | logical time %s\n", counter(value.commit_sequence).c_str(),
                counter(value.logical_time).c_str());
    std::printf("  digests      : request=%s receipt=%s\n", digest_text(value.request_digest).c_str(),
                digest_text(value.receipt_digest).c_str());
    std::printf("  history      : the decision is a durable mutation: the revision advances by one and the\n");
    std::printf("                 lifecycle generation holds, because the lifecycle state did not move\n");
    return receipt;
  }

 private:
  Runtime& runtime_;
  ObjectKey key_;
  ActorId actor_;
  std::uint64_t index_ = 0;
};

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

/// Applies a request that is expected to be refused and prints what came back.
/// The receipt is never issued, nothing is committed and the object keeps the
/// state, revision and generation it had.
Result<void> show_refusal(Runtime& runtime, const TransitionRequest& request, std::string_view label) {
  const Result<PreflightReport> preflight = runtime.preflight_transition(request);
  if (!preflight.has_value()) {
    return preflight.error();
  }
  std::printf("\nrefusal: %.*s\n", static_cast<int>(label.size()), label.data());
  std::printf("  preflight    : outcome=%s\n", spell(preflight.value().outcome).c_str());
  std::printf("  primary error: %s\n", describe(preflight.value().primary_error).c_str());
  std::printf("  primary rank : %s is rank %u of the validation sequence\n",
              std::string(to_string(preflight.value().primary_error.code)).c_str(),
              validation_rank(preflight.value().primary_error.code));

  const Result<TransitionReceipt> applied = runtime.apply_transition(request);
  if (applied.has_value()) {
    return Error::make(ErrorCode::Internal, "a refusal was expected and the runtime applied the request instead")
        .with("label", std::string(label));
  }
  std::printf("  applied      : no receipt: %s\n", describe(applied.error()).c_str());
  return ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

namespace {

Result<void> walk(const Workspace& workspace) {
  const Result<AssetId> asset = AssetId::parse("hl-walkthrough-node-01");
  if (!asset.has_value()) {
    return asset.error();
  }
  const Result<ModelId> model = ModelId::parse("hl-model-x200");
  if (!model.has_value()) {
    return model.error();
  }
  const Result<ActorId> actor = ActorId::parse("example.walkthrough.operator");
  if (!actor.has_value()) {
    return actor.error();
  }

  ObjectKey key;
  key.asset = asset.value();
  key.hardware_generation = HardwareGeneration::first();

  Result<Runtime> opened = Runtime::open_ephemeral();
  if (!opened.has_value()) {
    return opened.error();
  }
  Runtime runtime = std::move(opened).value();

  const Result<RuntimeStatus> status = runtime.status();
  if (!status.has_value()) {
    return status.error();
  }

  say("");
  say("== the machine ==");
  say("The lifecycle machine is data, not code paths: this listing is the whole");
  say("transition table, and the walk below only ever uses edges that appear here.");
  for (LifecycleState state : all_lifecycle_states()) {
    std::vector<std::string> targets;
    for (LifecycleState target : legal_targets(state)) {
      targets.push_back(spell(target));
    }
    std::printf("  %-14s -> %s\n", spell(state).c_str(), targets.empty() ? "(terminal)" : join(targets, ", ").c_str());
  }

  say("");
  say("== the runtime ==");
  say("Authority is session scoped and fenced by the control epoch: every request below");
  say("is planned against the epoch this runtime holds. A process that recovered this");
  say("state would hold fenced authority instead, and would have to attest explicitly");
  say("before it could change anything.");
  std::printf("  library              : %s\n", std::string(library_version_string()).c_str());
  std::printf("  durability           : %s\n", spell(status.value().durability).c_str());
  std::printf("  writable             : %s\n", status.value().writable ? "true" : "false");
  std::printf("  working directory    : %s (holds no store: this runtime is ephemeral)\n",
              workspace.root().string().c_str());
  std::printf("  commit sequence      : %s\n", counter(status.value().commit_sequence).c_str());
  std::printf("  logical time         : %s\n", counter(status.value().logical_time).c_str());
  std::printf("  control epoch        : %s\n", counter(status.value().control_epoch).c_str());
  std::printf("  incarnation          : %s (an ephemeral runtime has none)\n", counter(status.value().incarnation).c_str());

  say("");
  say("== registration ==");
  say("An object is born ordered or staged and nothing else: no later state can be");
  say("asserted at registration, because every later state claims a physical fact");
  say("that only a transition with evidence may establish.");
  const Result<PlanId> create_plan = PlanId::parse("walkthrough.plan.create");
  if (!create_plan.has_value()) {
    return create_plan.error();
  }
  const Result<AttemptId> create_attempt = AttemptId::parse("walkthrough.attempt.create");
  if (!create_attempt.has_value()) {
    return create_attempt.error();
  }

  CreateRequest create;
  create.plan = create_plan.value();
  create.attempt = create_attempt.value();
  create.key = key;
  create.kind = HardwareKind::Compute;
  create.model = model.value();
  create.firmware_generation = FirmwareGeneration::first();
  create.initial_state = LifecycleState::Ordered;
  create.provenance.actor.id = actor.value();
  create.provenance.actor.kind = ActorKind::Procurement;
  create.provenance.authority = authority_bit(AuthorityScope::Procurement);
  create.provenance.policy_generation = PolicyGeneration::first();
  create.provenance.plan = create_plan.value();
  create.provenance.attempt = create_attempt.value();
  create.provenance.evidence.push_back(
      make_evidence_ref(EvidenceKind::ProcurementRecord, key, LifecycleState::Ordered, LifecycleState::Ordered, 0));

  std::printf("  registration rule: state ordered requires authority %s and evidence %s\n",
              spell(authority_bit(AuthorityScope::Procurement)).c_str(),
              spell(evidence_bit(EvidenceKind::ProcurementRecord)).c_str());
  print_evidence(create.provenance.evidence);
  const Result<CreateReceipt> created = runtime.create_object(create);
  if (!created.has_value()) {
    return created.error();
  }
  std::printf("  receipt      : %s registered as %s revision %s lifecycle generation %s\n",
              to_string(created.value().key).c_str(), spell(created.value().state).c_str(),
              counter(created.value().revision).c_str(), counter(created.value().lifecycle_generation).c_str());
  std::printf("  commit point : commit sequence %s | logical time %s\n",
              counter(created.value().commit_sequence).c_str(), counter(created.value().logical_time).c_str());
  std::printf("  digests      : receipt=%s\n", digest_text(created.value().receipt_digest).c_str());

  Walk walk_step(runtime, key, actor.value());

  // --- physical arrival -----------------------------------------------------
  const Result<TransitionReceipt> staged = walk_step.step(
      "The unit is physically at a facility. Staged means on site, not in place, and "
      "it carries no service: installed is not staged, and staged is not active.",
      LifecycleState::Staged, TransitionReason::DeliveryAccepted);
  if (!staged.has_value()) {
    return staged.error();
  }

  // --- installation ---------------------------------------------------------
  Location location;
  const Result<SiteId> site = SiteId::parse("hl-site-dc1");
  const Result<RackId> rack = RackId::parse("hl-rack-a07");
  const Result<SlotId> slot = SlotId::parse("hl-slot-u18");
  if (!site.has_value() || !rack.has_value() || !slot.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own location literals are malformed");
  }
  location.site = site.value();
  location.rack = rack.value();
  location.slot = slot.value();
  const Result<TransitionReceipt> installed = walk_step.step(
      "The unit now occupies an assigned location, which the rule demands: a state that "
      "claims a place cannot be reached by a request that names no place. Installed is "
      "not commissioned and not active.",
      LifecycleState::Installed, TransitionReason::InstallationCompleted, location);
  if (!installed.has_value()) {
    return installed.error();
  }

  // --- commissioning --------------------------------------------------------
  const Result<TransitionReceipt> commissioning = walk_step.step(
      "Verification and qualification start. This rule requires no particular evidence "
      "kind, only that the change binds to some evidence.",
      LifecycleState::Commissioning, TransitionReason::CommissioningStarted);
  if (!commissioning.has_value()) {
    return commissioning.error();
  }

  // --- the gate is separate authority ---------------------------------------
  say("");
  say("== the service gate ==");
  say("Entering service is not something a transition may decide. The next request");
  say("names the right state, the right reason and the right evidence, and it is still");
  say("refused, because the gate is closed. Only an explicit gate decision opens it.");

  const Result<PlanId> blocked_plan = PlanId::parse("walkthrough.plan.blocked-entry");
  if (!blocked_plan.has_value()) {
    return blocked_plan.error();
  }
  const Result<AttemptId> blocked_attempt = AttemptId::parse("walkthrough.attempt.blocked-entry");
  if (!blocked_attempt.has_value()) {
    return blocked_attempt.error();
  }
  const Result<ObjectView> before_gate = runtime.inspect(key);
  if (!before_gate.has_value()) {
    return before_gate.error();
  }
  const TransitionRule* entry_rule = find_rule(LifecycleState::Commissioning, LifecycleState::Active);
  if (entry_rule == nullptr) {
    return Error::make(ErrorCode::Internal, "the table no longer declares commissioning -> active");
  }
  TransitionRequest blocked;
  blocked.plan = blocked_plan.value();
  blocked.attempt = blocked_attempt.value();
  blocked.key = key;
  blocked.expected_lifecycle_generation = before_gate.value().object.lifecycle_generation;
  blocked.expected_revision = before_gate.value().object.revision;
  blocked.expected_control_epoch = runtime.registry().control_epoch();
  blocked.expected_state = LifecycleState::Commissioning;
  blocked.target_state = LifecycleState::Active;
  blocked.reason = TransitionReason::CommissioningPassed;
  blocked.provenance.actor.id = actor.value();
  blocked.provenance.actor.kind = actor_for(entry_rule->required_authority);
  blocked.provenance.authority = entry_rule->required_authority;
  blocked.provenance.policy_generation = PolicyGeneration::first();
  blocked.provenance.plan = blocked_plan.value();
  blocked.provenance.attempt = blocked_attempt.value();
  blocked.provenance.evidence =
      evidence_for(*entry_rule, key, LifecycleState::Commissioning, LifecycleState::Active, 0);
  const Result<void> refused = show_refusal(runtime, blocked, "commissioning -> active with the gate closed");
  if (!refused.has_value()) {
    return refused.error();
  }

  const Result<EligibilityGateReceipt> gate =
      walk_step.open_service_gate("The service authority decides, separately and explicitly, that this object may "
                                  "return to service. The decision is authorised, evidenced and receipted on its own.");
  if (!gate.has_value()) {
    return gate.error();
  }

  const Result<TransitionReceipt> active = walk_step.step(
      "Service begins. Requested state was active before this call; observed state is "
      "active only now, and only because a commit point was passed.",
      LifecycleState::Active, TransitionReason::CommissioningPassed);
  if (!active.has_value()) {
    return active.error();
  }

  // --- a failed health observation changes nothing ---------------------------
  say("");
  say("== observation is not authority ==");
  say("A monitoring system reports the unit as failed. The runtime records the");
  say("observation, stamps it with an authoritative observation sequence and logical");
  say("time, and changes nothing else: health, readiness and availability are");
  say("observations, while lifecycle state is authority.");

  const Result<ObjectView> before_health = runtime.inspect(key);
  if (!before_health.has_value()) {
    return before_health.error();
  }
  std::printf("  before       : state=%s revision=%s lifecycle generation=%s gate=%s\n",
              spell(before_health.value().object.state).c_str(), counter(before_health.value().object.revision).c_str(),
              counter(before_health.value().object.lifecycle_generation).c_str(),
              spell(before_health.value().object.eligibility_gate).c_str());

  const Result<PlanId> health_plan = PlanId::parse("walkthrough.plan.health");
  const Result<AttemptId> health_attempt = AttemptId::parse("walkthrough.attempt.health");
  const Result<WallClock> observed_at = WallClock::parse("2026-01-15T08:30:00Z");
  if (!health_plan.has_value() || !health_attempt.has_value() || !observed_at.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own health observation literals are malformed");
  }
  const std::string health_payload = "example payload: health failed on " + to_string(key);
  HealthObservationRequest observation;
  observation.plan = health_plan.value();
  observation.attempt = health_attempt.value();
  observation.key = key;
  observation.observation.key = key;
  observation.observation.health = HealthStatus::Failed;
  observation.observation.readiness = ReadinessStatus::NotReady;
  observation.observation.availability = AvailabilityStatus::Unavailable;
  observation.observation.evidence_digest = Sha256::hash(health_payload);
  observation.observation.source = "example.walkthrough/monitor";
  observation.observation.observed_wall_clock = observed_at.value();
  observation.provenance.actor.id = actor.value();
  observation.provenance.actor.kind = ActorKind::Automation;
  observation.provenance.authority = authority_bit(AuthorityScope::Service);
  observation.provenance.policy_generation = PolicyGeneration::first();
  observation.provenance.plan = health_plan.value();
  observation.provenance.attempt = health_attempt.value();
  EvidenceRef health_evidence;
  health_evidence.kind = EvidenceKind::HealthEvidence;
  health_evidence.digest = observation.observation.evidence_digest;
  health_evidence.source = observation.observation.source;
  observation.provenance.evidence.push_back(health_evidence);

  std::printf("  observation  : health=%s readiness=%s availability=%s source=%s wall clock=%s digest=%s\n",
              spell(observation.observation.health).c_str(), spell(observation.observation.readiness).c_str(),
              spell(observation.observation.availability).c_str(), observation.observation.source.c_str(),
              observation.observation.observed_wall_clock.value().c_str(),
              digest_text(observation.observation.evidence_digest).c_str());
  const Result<HealthReceipt> health = runtime.observe_health(observation);
  if (!health.has_value()) {
    return health.error();
  }
  std::printf("  receipt      : %s observation sequence %s\n", to_string(health.value().key).c_str(),
              counter(health.value().sequence).c_str());
  std::printf("  commit point : commit sequence %s | logical time %s\n",
              counter(health.value().commit_sequence).c_str(), counter(health.value().logical_time).c_str());
  std::printf("  digests      : request=%s receipt=%s\n", digest_text(health.value().request_digest).c_str(),
              digest_text(health.value().receipt_digest).c_str());

  const Result<ObjectView> after_health = runtime.inspect(key);
  if (!after_health.has_value()) {
    return after_health.error();
  }
  std::printf("  after        : state=%s revision=%s lifecycle generation=%s gate=%s\n",
              spell(after_health.value().object.state).c_str(), counter(after_health.value().object.revision).c_str(),
              counter(after_health.value().object.lifecycle_generation).c_str(),
              spell(after_health.value().object.eligibility_gate).c_str());
  std::printf("  stored health: present=%s health=%s recorded_at=%s freshness=%s\n",
              after_health.value().health.present ? "true" : "false",
              spell(after_health.value().health.observation.health).c_str(),
              counter(after_health.value().health.recorded_at).c_str(),
              spell(after_health.value().health.freshness).c_str());
  std::printf("  eligibility  : %s (derived from lifecycle state %s and gate %s only)\n",
              spell(after_health.value().eligibility).c_str(), spell(after_health.value().object.state).c_str(),
              spell(after_health.value().object.eligibility_gate).c_str());
  std::printf("  unchanged    : state=%s revision=%s generation=%s\n",
              before_health.value().object.state == after_health.value().object.state ? "true" : "false",
              before_health.value().object.revision == after_health.value().object.revision ? "true" : "false",
              before_health.value().object.lifecycle_generation == after_health.value().object.lifecycle_generation
                  ? "true"
                  : "false");

  // --- maintenance and back -------------------------------------------------
  const Result<TransitionReceipt> maintenance = walk_step.step(
      "Withdrawn from service on purpose, expected to return. The rule suspends the "
      "gate: maintenance is not removal, and drained is not decommissioned.",
      LifecycleState::Maintenance, TransitionReason::MaintenanceScheduled);
  if (!maintenance.has_value()) {
    return maintenance.error();
  }
  std::printf("  gate after   : %s (the suspend effect of the rule, not a second request)\n",
              spell(maintenance.value().gate_after).c_str());

  say("");
  say("Returning to service needs the gate open again, so the request that names");
  say("maintenance -> active is refused until the service authority decides again.");
  const Result<ObjectView> before_return = runtime.inspect(key);
  if (!before_return.has_value()) {
    return before_return.error();
  }
  const TransitionRule* return_rule = find_rule(LifecycleState::Maintenance, LifecycleState::Active);
  if (return_rule == nullptr) {
    return Error::make(ErrorCode::Internal, "the table no longer declares maintenance -> active");
  }
  const Result<PlanId> blocked_return_plan = PlanId::parse("walkthrough.plan.blocked-return");
  const Result<AttemptId> blocked_return_attempt = AttemptId::parse("walkthrough.attempt.blocked-return");
  if (!blocked_return_plan.has_value() || !blocked_return_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  TransitionRequest blocked_return;
  blocked_return.plan = blocked_return_plan.value();
  blocked_return.attempt = blocked_return_attempt.value();
  blocked_return.key = key;
  blocked_return.expected_lifecycle_generation = before_return.value().object.lifecycle_generation;
  blocked_return.expected_revision = before_return.value().object.revision;
  blocked_return.expected_control_epoch = runtime.registry().control_epoch();
  blocked_return.expected_state = LifecycleState::Maintenance;
  blocked_return.target_state = LifecycleState::Active;
  blocked_return.reason = TransitionReason::MaintenanceCompleted;
  blocked_return.provenance.actor.id = actor.value();
  blocked_return.provenance.actor.kind = actor_for(return_rule->required_authority);
  blocked_return.provenance.authority = return_rule->required_authority;
  blocked_return.provenance.policy_generation = PolicyGeneration::first();
  blocked_return.provenance.plan = blocked_return_plan.value();
  blocked_return.provenance.attempt = blocked_return_attempt.value();
  blocked_return.provenance.evidence =
      evidence_for(*return_rule, key, LifecycleState::Maintenance, LifecycleState::Active, 0);
  const Result<void> refused_return = show_refusal(runtime, blocked_return, "maintenance -> active with the gate closed");
  if (!refused_return.has_value()) {
    return refused_return.error();
  }

  const Result<EligibilityGateReceipt> reopened =
      walk_step.open_service_gate("The service authority decides a second time. Authority is re-established, not "
                                  "inherited from the previous decision.");
  if (!reopened.has_value()) {
    return reopened.error();
  }
  const Result<TransitionReceipt> returned = walk_step.step(
      "Maintenance completed and the object returns to service. Two independent facts "
      "made this possible: the maintenance authority completed the work, and the "
      "service authority opened the gate.",
      LifecycleState::Active, TransitionReason::MaintenanceCompleted);
  if (!returned.has_value()) {
    return returned.error();
  }

  // --- an edge the table does not declare -----------------------------------
  say("");
  say("== what the table refuses ==");
  say("The table is the only source of legal edges. An edge that is absent from it");
  say("is rejected with the same primary error every time, whatever route the caller");
  say("takes to ask for it.");
  const Result<ObjectView> before_illegal = runtime.inspect(key);
  if (!before_illegal.has_value()) {
    return before_illegal.error();
  }
  const Result<PlanId> illegal_plan = PlanId::parse("walkthrough.plan.illegal");
  const Result<AttemptId> illegal_attempt = AttemptId::parse("walkthrough.attempt.illegal");
  if (!illegal_plan.has_value() || !illegal_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  TransitionRequest illegal;
  illegal.plan = illegal_plan.value();
  illegal.attempt = illegal_attempt.value();
  illegal.key = key;
  illegal.expected_lifecycle_generation = before_illegal.value().object.lifecycle_generation;
  illegal.expected_revision = before_illegal.value().object.revision;
  illegal.expected_control_epoch = runtime.registry().control_epoch();
  illegal.expected_state = LifecycleState::Active;
  illegal.target_state = LifecycleState::Retired;
  illegal.reason = TransitionReason::DecommissionCompleted;
  illegal.provenance.actor.id = actor.value();
  illegal.provenance.actor.kind = ActorKind::Decommissioning;
  illegal.provenance.authority = authority_all();
  illegal.provenance.policy_generation = PolicyGeneration::first();
  illegal.provenance.plan = illegal_plan.value();
  illegal.provenance.attempt = illegal_attempt.value();
  illegal.provenance.evidence.push_back(make_evidence_ref(EvidenceKind::DecommissioningRecord, key,
                                                           LifecycleState::Active, LifecycleState::Retired, 0));
  std::printf("\n  active -> retired is not in the table: %s\n",
              is_legal_transition(LifecycleState::Active, LifecycleState::Retired) ? "legal" : "illegal");
  const Result<void> refused_illegal =
      show_refusal(runtime, illegal, "active -> retired, an edge the table does not declare");
  if (!refused_illegal.has_value()) {
    return refused_illegal.error();
  }

  say("");
  say("A request that violates several rules at once reports the lowest ranking");
  say("violation as its primary error. Here the edge is still absent, and the revision");
  say("is stale as well; the stale revision outranks the illegal edge and is reported.");
  TransitionRequest compound = illegal;
  const Result<PlanId> compound_plan = PlanId::parse("walkthrough.plan.compound");
  const Result<AttemptId> compound_attempt = AttemptId::parse("walkthrough.attempt.compound");
  if (!compound_plan.has_value() || !compound_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  compound.plan = compound_plan.value();
  compound.attempt = compound_attempt.value();
  compound.provenance.plan = compound_plan.value();
  compound.provenance.attempt = compound_attempt.value();
  compound.expected_revision = Revision::from_value(before_illegal.value().object.revision.value() + 1u);
  std::printf("\n  ranks        : %s=%u  %s=%u\n", std::string(to_string(ErrorCode::StaleRevision)).c_str(),
              validation_rank(ErrorCode::StaleRevision), std::string(to_string(ErrorCode::IllegalTransition)).c_str(),
              validation_rank(ErrorCode::IllegalTransition));
  const Result<void> refused_compound = show_refusal(runtime, compound, "active -> retired with a stale revision");
  if (!refused_compound.has_value()) {
    return refused_compound.error();
  }

  // --- drain, decommission, remove ------------------------------------------
  const Result<TransitionReceipt> retiring = walk_step.step(
      "Draining: service is withdrawn, decommissioning has not happened. Retiring is "
      "not retired, and drained is not decommissioned.",
      LifecycleState::Retiring, TransitionReason::RetirementApproved);
  if (!retiring.has_value()) {
    return retiring.error();
  }
  const Result<TransitionReceipt> retired = walk_step.step(
      "Decommissioned, and still physically present. The unit keeps its identity and "
      "its whole history.",
      LifecycleState::Retired, TransitionReason::DecommissionCompleted);
  if (!retired.has_value()) {
    return retired.error();
  }
  const Result<TransitionReceipt> removed = walk_step.step(
      "Physically gone. Removed is terminal; returning to service would need a new "
      "hardware identity and generation, which is a different object key.",
      LifecycleState::Removed, TransitionReason::PhysicallyRemoved);
  if (!removed.has_value()) {
    return removed.error();
  }
  std::printf("  terminal     : is_terminal(removed)=%s is_physical(removed)=%s is_decommissioned(removed)=%s\n",
              is_terminal(LifecycleState::Removed) ? "true" : "false",
              is_physical(LifecycleState::Removed) ? "true" : "false",
              is_decommissioned(LifecycleState::Removed) ? "true" : "false");

  // --- the resulting state --------------------------------------------------
  say("");
  say("== what the walk left behind ==");
  const Result<ObjectView> final_view = runtime.inspect(key);
  if (!final_view.has_value()) {
    return final_view.error();
  }
  const HardwareObject& object = final_view.value().object;
  std::printf("  object       : %s state=%s revision=%s lifecycle generation=%s hardware generation=%s\n",
              to_string(object.key).c_str(), spell(object.state).c_str(), counter(object.revision).c_str(),
              counter(object.lifecycle_generation).c_str(), counter(object.key.hardware_generation).c_str());
  std::printf("  identity     : kind=%s model=%s history entries=%s created_at=%s updated_at=%s\n",
              spell(object.kind).c_str(), object.model.value().c_str(), number(object.history_entries).c_str(),
              counter(object.created_at).c_str(), counter(object.updated_at).c_str());
  std::printf("  location     : %s\n", object.location.has_value() ? to_string(object.location.value()).c_str() : "absent");
  std::printf("  gate         : %s | eligibility %s | authority %s\n", spell(object.eligibility_gate).c_str(),
              spell(final_view.value().eligibility).c_str(), spell(object.authority).c_str());

  const Result<HistoryView> history = runtime.history(key);
  if (!history.has_value()) {
    return history.error();
  }
  say("");
  say("The history chain: every entry is bound to its predecessor by a chain digest,");
  say("so a rewritten, reordered or removed entry is detectable without trusting the");
  say("storage layer. A gate decision is a durable mutation of the object and appears");
  say("here like any other, with from == to and reason eligibility_gate_opened: the");
  say("revision advanced by one while the lifecycle generation did not move, because no");
  say("lifecycle step happened.");
  std::size_t gate_entries = 0;
  for (const HistoryEntry& entry : history.value().entries) {
    const char* marker = entry.is_registration() ? "registration"
                         : entry.is_gate_decision() ? "gate decision"
                                                    : "state change";
    if (entry.is_gate_decision()) {
      ++gate_entries;
    }
    std::printf("  #%-2s %-13s commit=%-3s logical=%-3s %-14s -> %-14s revision=%-3s generation=%-3s reason=%-26s "
                "chain=%s\n",
                number(static_cast<std::uint64_t>(&entry - history.value().entries.data() + 1)).c_str(), marker,
                counter(entry.commit_sequence).c_str(), counter(entry.logical_time).c_str(), spell(entry.from).c_str(),
                spell(entry.to).c_str(), counter(entry.revision_after).c_str(),
                counter(entry.lifecycle_generation).c_str(), spell(entry.reason).c_str(),
                digest_text(entry.chain_digest).c_str());
  }
  std::printf("  gate decisions in the chain: %s (from == to, revision advanced by one, lifecycle generation held)\n",
              number(static_cast<std::uint64_t>(gate_entries)).c_str());
  const Registry& registry = runtime.registry();
  const HistoryLog* log = registry.history(key);
  if (log == nullptr) {
    return Error::make(ErrorCode::Internal, "the object has no history log");
  }
  const Result<Digest> verified_chain = verify_history_chain(*log);
  if (!verified_chain.has_value()) {
    return verified_chain.error();
  }
  const Result<LifecycleState> replayed = replay_history(*log);
  if (!replayed.has_value()) {
    return replayed.error();
  }
  std::printf("  chain head   : %s\n", digest_text(verified_chain.value()).c_str());
  std::printf("  replay proves: %s (authoritative state %s, equal=%s)\n", spell(replayed.value()).c_str(),
              spell(object.state).c_str(), replayed.value() == object.state ? "true" : "false");
  std::printf("  history head : %s\n", digest_text(history.value().chain_head).c_str());

  const Result<Digest> verified = runtime.verify();
  if (!verified.has_value()) {
    return verified.error();
  }
  const Result<RuntimeStatus> final_status = runtime.status();
  if (!final_status.has_value()) {
    return final_status.error();
  }
  say("");
  say("Verify re-checks every invariant of the authoritative state and, for a durable");
  say("runtime, replays the whole journal. For this ephemeral runtime it proves the");
  say("in-memory state alone.");
  std::printf("  state digest : %s\n", digest_text(verified.value()).c_str());
  std::printf("  watermarks   : commit sequence %s | logical time %s | observation sequence %s | control epoch %s\n",
              counter(final_status.value().commit_sequence).c_str(), counter(final_status.value().logical_time).c_str(),
              counter(final_status.value().observation_sequence).c_str(),
              counter(final_status.value().control_epoch).c_str());
  std::printf("  contents     : objects=%s history entries=%s applied plans=%s lineage links=%s\n",
              number(static_cast<std::uint64_t>(final_status.value().object_count)).c_str(),
              number(static_cast<std::uint64_t>(final_status.value().history_entry_count)).c_str(),
              number(static_cast<std::uint64_t>(final_status.value().applied_plans)).c_str(),
              number(static_cast<std::uint64_t>(final_status.value().lineage_links)).c_str());
  std::printf("  incarnation  : %s\n", counter(final_status.value().incarnation).c_str());

  say("");
  say("Nothing in this walk was asserted twice: every state was reached by a request");
  say("whose authority, evidence, generation, revision and epoch matched the state it");
  say("was planned against, and every refusal above left the state untouched.");
  return ok();
}

}  // namespace

int main(int argc, char** argv) {
  const Result<Options> options = parse_options(argc, argv);
  if (!options.has_value()) {
    return fail(options.error());
  }
  if (options.value().help) {
    print_usage();
    return 0;
  }

  const Result<Workspace> workspace = Workspace::create(options.value().directory, kExampleName, options.value().keep);
  if (!workspace.has_value()) {
    return fail(workspace.error());
  }

  say("Hardware Lifecycle - lifecycle walkthrough");
  std::printf("working directory: %s\n", workspace.value().root().string().c_str());

  const Result<void> outcome = walk(workspace.value());
  workspace.value().cleanup();
  if (!outcome.has_value()) {
    return fail(outcome.error());
  }
  say("");
  say("walkthrough complete: exit code 0");
  return 0;
}
