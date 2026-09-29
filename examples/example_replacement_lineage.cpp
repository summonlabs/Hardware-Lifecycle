// Hardware Lifecycle - example: replacement lineage.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Documentation that runs. Two hardware objects with different asset ids are
// registered, the predecessor walks the lifecycle machine to Retired and the
// successor walks to Installed. Then the example shows what replacement is and
// what it is not:
//
//   * the link between the two objects is established on its own, with the
//     replacement authority, its own evidence and its own receipt. It binds the
//     two identities, the reason and the provenance into one link digest;
//   * only then may the predecessor move to Replaced. Replaced means superseded,
//     and the predecessor keeps its identity, its whole history, its revision
//     chain and its physical presence. Nothing is merged, renamed or reused;
//   * a second successor link for the same predecessor is refused, and so is any
//     link that would close a replacement cycle: the lineage is a set of chains,
//     never a graph with two paths to the same object;
//   * the decommissioned, superseded unit is then physically removed. Removed is
//     terminal, and it still does not erase the lineage, the history or the key.
//
// The runtime is ephemeral: no durable commit point is passed, so the receipts
// below are publication points in this process and no durable claim is made. The
// same commit path runs against a real store in example_durable_journal. Authority
// here is fenced by the session's control epoch, and every request is planned
// against it.
//
// usage: example_replacement_lineage [<directory>] [--keep]

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

constexpr std::string_view kExampleName = "example_replacement_lineage";

// ---------------------------------------------------------------------------
// Output primitives
// ---------------------------------------------------------------------------

void say(std::string_view text) { std::printf("%.*s\n", static_cast<int>(text.size()), text.data()); }

template <class T>
std::string spell(T value) {
  return std::string(to_string(value));
}

std::string number(std::uint64_t value) { return std::to_string(value); }

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
  say("usage: example_replacement_lineage [<directory>] [--keep]");
  say("");
  say("  <directory>  directory to work in, created when it does not exist");
  say("               (default: <system temporary directory>/example_replacement_lineage)");
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

  void cleanup() const {
    if (keep_) {
      std::printf("working directory left behind (--keep): %s\n", root_.string().c_str());
      return;
    }
    std::error_code error;
    if (created_root_) {
      std::filesystem::remove_all(root_, error);
    }
    std::printf("working directory removed: %s\n", root_.string().c_str());
  }

 private:
  Workspace() = default;

  std::filesystem::path root_;
  bool keep_ = false;
  bool created_root_ = false;
};

// ---------------------------------------------------------------------------
// Requests derived from the transition table
// ---------------------------------------------------------------------------

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

/// One evidence reference whose digest binds the exact synthetic artifact the
/// step is planned against.
EvidenceRef make_evidence_ref(EvidenceKind kind, const ObjectKey& key, std::uint64_t step) {
  const std::string artifact =
      "lineage artifact #" + std::to_string(step) + " " + std::string(to_string(kind)) + " for " + to_string(key);
  EvidenceRef reference;
  reference.kind = kind;
  reference.digest = Sha256::hash(artifact);
  reference.source = "example.lineage/" + std::string(to_string(kind));
  return reference;
}

/// One reference per evidence kind the rule requires. The requirement is a
/// conjunction: a request that misses any required kind is refused.
/// The evidence requirement of a rule as the library renders it in
/// describe_rule(): an empty mask means "at least one reference of any kind".
std::string evidence_requirement(EvidenceMask mask) {
  return mask.empty() ? std::string("any") : spell(mask);
}

std::vector<EvidenceRef> evidence_for(const TransitionRule& rule, const ObjectKey& key, std::uint64_t step) {
  std::vector<EvidenceRef> references;
  for (std::size_t raw = 1; raw < kEvidenceKindCount; ++raw) {
    const EvidenceKind kind = static_cast<EvidenceKind>(raw);
    if (rule.required_evidence.intersects(evidence_bit(kind))) {
      references.push_back(make_evidence_ref(kind, key, step));
    }
  }
  if (references.empty()) {
    references.push_back(make_evidence_ref(EvidenceKind::ServiceRecord, key, step));
  }
  return references;
}

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------

/// Applies one table edge and prints a compact line: the edge, what the table
/// required, and the values that came back.
Result<TransitionReceipt> move(Runtime& runtime, const ObjectKey& key, const ActorId& actor, std::uint64_t& index,
                               LifecycleState target, TransitionReason reason,
                               std::optional<Location> location = std::nullopt) {
  const Result<ObjectView> view = runtime.inspect(key);
  if (!view.has_value()) {
    return view.error();
  }
  const HardwareObject& object = view.value().object;
  const TransitionRule* rule = find_rule(object.state, target);
  if (rule == nullptr) {
    return Error::make(ErrorCode::IllegalTransition, "the transition table has no edge from this state to that state")
        .with("from", spell(object.state))
        .with("to", spell(target));
  }
  ++index;
  const Result<PlanId> plan = PlanId::parse("lineage.plan." + std::to_string(index));
  const Result<AttemptId> attempt = AttemptId::parse("lineage.attempt." + std::to_string(index));
  if (!plan.has_value() || !attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }

  TransitionRequest request;
  request.plan = plan.value();
  request.attempt = attempt.value();
  request.key = key;
  request.expected_lifecycle_generation = object.lifecycle_generation;
  request.expected_revision = object.revision;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.expected_state = object.state;
  request.target_state = target;
  request.reason = reason;
  request.location = location;
  request.provenance.actor.id = actor;
  request.provenance.actor.kind = actor_for(rule->required_authority);
  request.provenance.authority = rule->required_authority;
  request.provenance.policy_generation = PolicyGeneration::first();
  request.provenance.plan = plan.value();
  request.provenance.attempt = attempt.value();
  request.provenance.evidence = evidence_for(*rule, key, index);

  std::printf("  move  %-14s -> %-14s authority=%-14s evidence=%-34s planned for revision %s generation %s epoch %s\n",
              spell(object.state).c_str(), spell(target).c_str(), spell(rule->required_authority).c_str(),
              evidence_requirement(rule->required_evidence).c_str(), counter(object.revision).c_str(),
              counter(object.lifecycle_generation).c_str(), counter(request.expected_control_epoch).c_str());
  const Result<TransitionReceipt> receipt = runtime.apply_transition(request);
  if (!receipt.has_value()) {
    return receipt.error();
  }
  std::printf("        reason=%-24s revision %s -> %s generation %s gate=%s commit=%s logical=%s receipt=%s entry=%s\n",
              spell(receipt.value().reason).c_str(), counter(receipt.value().revision_before).c_str(),
              counter(receipt.value().revision_after).c_str(), counter(receipt.value().lifecycle_generation).c_str(),
              spell(receipt.value().gate_after).c_str(), counter(receipt.value().commit_sequence).c_str(),
              counter(receipt.value().logical_time).c_str(), digest_text(receipt.value().receipt_digest).c_str(),
              digest_text(receipt.value().entry_digest).c_str());
  return receipt;
}

/// The service eligibility gate is a separate authority: no transition opens it,
/// so the walk opens it explicitly before entering service.
Result<void> open_service_gate(Runtime& runtime, const ObjectKey& key, const ActorId& actor, std::uint64_t& index) {
  const Result<ObjectView> view = runtime.inspect(key);
  if (!view.has_value()) {
    return view.error();
  }
  const HardwareObject& object = view.value().object;
  ++index;
  const Result<PlanId> plan = PlanId::parse("lineage.plan.gate." + std::to_string(index));
  const Result<AttemptId> attempt = AttemptId::parse("lineage.attempt.gate." + std::to_string(index));
  if (!plan.has_value() || !attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  EligibilityGateRequest request;
  request.plan = plan.value();
  request.attempt = attempt.value();
  request.key = key;
  request.gate = EligibilityGate::Open;
  request.expected_lifecycle_generation = object.lifecycle_generation;
  request.expected_revision = object.revision;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.provenance.actor.id = actor;
  request.provenance.actor.kind = ActorKind::Operator;
  request.provenance.authority = authority_bit(AuthorityScope::Service);
  request.provenance.policy_generation = PolicyGeneration::first();
  request.provenance.plan = plan.value();
  request.provenance.attempt = attempt.value();
  request.provenance.evidence.push_back(make_evidence_ref(EvidenceKind::ServiceRecord, key, index));
  const Result<EligibilityGateReceipt> receipt = runtime.set_eligibility_gate(request);
  if (!receipt.has_value()) {
    return receipt.error();
  }
  std::printf("  gate  %-14s -> %-14s authority=%-14s revision %s commit=%s receipt=%s\n",
              spell(receipt.value().before).c_str(), spell(receipt.value().after).c_str(),
              spell(authority_bit(AuthorityScope::Service)).c_str(), counter(receipt.value().revision).c_str(),
              counter(receipt.value().commit_sequence).c_str(), digest_text(receipt.value().receipt_digest).c_str());
  return ok();
}

/// Ordered -> Staged -> Installed: the physical path, no service involved.
Result<void> walk_to_installed(Runtime& runtime, const ObjectKey& key, const ActorId& actor, std::uint64_t& index) {
  const Result<TransitionReceipt> staged =
      move(runtime, key, actor, index, LifecycleState::Staged, TransitionReason::DeliveryAccepted);
  if (!staged.has_value()) {
    return staged.error();
  }
  const Result<TransitionReceipt> installed =
      move(runtime, key, actor, index, LifecycleState::Installed, TransitionReason::InstallationCompleted);
  if (!installed.has_value()) {
    return installed.error();
  }
  return ok();
}

/// Ordered -> Staged -> Installed -> Commissioning -> (gate) -> Active ->
/// Retiring -> Retired.
Result<void> walk_to_retired(Runtime& runtime, const ObjectKey& key, const ActorId& actor, std::uint64_t& index) {
  const Result<void> installed = walk_to_installed(runtime, key, actor, index);
  if (!installed.has_value()) {
    return installed.error();
  }
  const Result<TransitionReceipt> commissioning =
      move(runtime, key, actor, index, LifecycleState::Commissioning, TransitionReason::CommissioningStarted);
  if (!commissioning.has_value()) {
    return commissioning.error();
  }
  const Result<void> gate = open_service_gate(runtime, key, actor, index);
  if (!gate.has_value()) {
    return gate.error();
  }
  const Result<TransitionReceipt> active =
      move(runtime, key, actor, index, LifecycleState::Active, TransitionReason::CommissioningPassed);
  if (!active.has_value()) {
    return active.error();
  }
  const Result<TransitionReceipt> retiring =
      move(runtime, key, actor, index, LifecycleState::Retiring, TransitionReason::RetirementApproved);
  if (!retiring.has_value()) {
    return retiring.error();
  }
  const Result<TransitionReceipt> retired =
      move(runtime, key, actor, index, LifecycleState::Retired, TransitionReason::DecommissionCompleted);
  if (!retired.has_value()) {
    return retired.error();
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Object rendering
// ---------------------------------------------------------------------------

void print_object(const char* label, const ObjectView& view) {
  const HardwareObject& object = view.object;
  std::printf("  %-12s : %s\n", label, to_string(object.key).c_str());
  std::printf("    identity   : kind=%s model=%s hardware generation=%s created_at=%s updated_at=%s\n",
              spell(object.kind).c_str(), object.model.value().c_str(), counter(object.key.hardware_generation).c_str(),
              counter(object.created_at).c_str(), counter(object.updated_at).c_str());
  std::printf("    lifecycle  : state=%s revision=%s lifecycle generation=%s gate=%s eligibility=%s\n",
              spell(object.state).c_str(), counter(object.revision).c_str(),
              counter(object.lifecycle_generation).c_str(), spell(object.eligibility_gate).c_str(),
              spell(view.eligibility).c_str());
  std::printf("    presence   : location=%s authority=%s\n",
              object.location.has_value() ? to_string(object.location.value()).c_str() : "absent",
              spell(object.authority).c_str());
  std::printf("    lineage    : predecessor=%s successor=%s replacement generation=%s\n",
              object.predecessor.has_value() ? to_string(object.predecessor.value()).c_str() : "none",
              object.successor.has_value() ? to_string(object.successor.value()).c_str() : "none",
              counter(object.replacement_generation).c_str());
  std::printf("    history    : entries=%s\n", number(object.history_entries).c_str());
}

void print_chain(const char* label, const std::vector<ObjectKey>& keys) {
  std::printf("  %-12s :", label);
  if (keys.empty()) {
    std::printf(" (none)\n");
    return;
  }
  for (const ObjectKey& key : keys) {
    std::printf(" %s", to_string(key).c_str());
  }
  std::printf("\n");
}

Result<void> print_lineage(const Runtime& runtime, const ObjectKey& key, const char* label) {
  const Result<LineageView> view = runtime.lineage(key);
  if (!view.has_value()) {
    return view.error();
  }
  std::printf("  %-12s : key=%s links=%s\n", label, to_string(view.value().key).c_str(),
              number(static_cast<std::uint64_t>(view.value().links.size())).c_str());
  print_chain("ancestry", view.value().ancestors);
  print_chain("successors", view.value().successors);
  for (const ReplacementRecord& record : view.value().links) {
    std::printf("    link       : %s -> %s reason=%s replacement generation=%s commit=%s logical=%s link digest=%s\n",
                to_string(record.predecessor).c_str(), to_string(record.successor).c_str(), spell(record.reason).c_str(),
                counter(record.replacement_generation).c_str(), counter(record.commit_sequence).c_str(),
                counter(record.linked_at).c_str(), digest_text(record.link_digest).c_str());
  }
  return ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

namespace {

Result<void> run(const Workspace& workspace) {
  const Result<ModelId> model = ModelId::parse("hl-model-r50");
  const Result<ActorId> actor = ActorId::parse("example.lineage.operator");
  const Result<AssetId> asset_a = AssetId::parse("hl-lineage-node-a");
  const Result<AssetId> asset_b = AssetId::parse("hl-lineage-node-b");
  const Result<AssetId> asset_c = AssetId::parse("hl-lineage-node-c");
  const Result<SiteId> site = SiteId::parse("hl-site-dc3");
  const Result<RackId> rack = RackId::parse("hl-rack-c01");
  if (!model.has_value() || !actor.has_value() || !asset_a.has_value() || !asset_b.has_value() || !asset_c.has_value() ||
      !site.has_value() || !rack.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own identifier literals are malformed");
  }

  ObjectKey key_a;
  key_a.asset = asset_a.value();
  key_a.hardware_generation = HardwareGeneration::first();
  ObjectKey key_b;
  key_b.asset = asset_b.value();
  key_b.hardware_generation = HardwareGeneration::first();
  ObjectKey key_c;
  key_c.asset = asset_c.value();
  key_c.hardware_generation = HardwareGeneration::first();

  Location location_a;
  const Result<SlotId> slot_a = SlotId::parse("hl-slot-u20");
  if (!slot_a.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own slot literal is malformed");
  }
  location_a.site = site.value();
  location_a.rack = rack.value();
  location_a.slot = slot_a.value();
  Location location_b = location_a;
  const Result<SlotId> slot_b = SlotId::parse("hl-slot-u21");
  if (!slot_b.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own slot literal is malformed");
  }
  location_b.slot = slot_b.value();
  Location location_c = location_a;
  const Result<SlotId> slot_c = SlotId::parse("hl-slot-u22");
  if (!slot_c.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own slot literal is malformed");
  }
  location_c.slot = slot_c.value();

  Result<Runtime> opened = Runtime::open_ephemeral();
  if (!opened.has_value()) {
    return opened.error();
  }
  Runtime runtime = std::move(opened).value();
  std::uint64_t index = 0;

  const Result<RuntimeStatus> status = runtime.status();
  if (!status.has_value()) {
    return status.error();
  }
  std::printf("  runtime            : %s | control epoch %s (authority is fenced to this session;\n",
              spell(status.value().durability).c_str(), counter(status.value().control_epoch).c_str());
  say("                       an ephemeral runtime passes no durable commit point)");
  std::printf("  working directory  : %s (an ephemeral runtime writes no store here)\n",
              workspace.root().string().c_str());

  say("");
  say("== three hardware objects, three identities ==");
  say("Registration creates identity. Nothing here discovers hardware, and nothing");
  say("infers a successor: every link below is a request with its own authority and");
  say("evidence, and every refusal leaves the lineage exactly as it was.");
  const struct {
    ObjectKey key;
    Location location;
    uint64_t step;
  } registrations[] = {{key_a, location_a, 1}, {key_b, location_b, 2}, {key_c, location_c, 3}};
  for (const auto& registration : registrations) {
    const Result<PlanId> plan = PlanId::parse("lineage.plan.register." + std::to_string(registration.step));
    const Result<AttemptId> attempt = AttemptId::parse("lineage.attempt.register." + std::to_string(registration.step));
    if (!plan.has_value() || !attempt.has_value()) {
      return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
    }
    CreateRequest create;
    create.plan = plan.value();
    create.attempt = attempt.value();
    create.key = registration.key;
    create.kind = HardwareKind::Compute;
    create.model = model.value();
    create.firmware_generation = FirmwareGeneration::first();
    create.initial_state = LifecycleState::Ordered;
    create.location = registration.location;
    create.provenance.actor.id = actor.value();
    create.provenance.actor.kind = ActorKind::Procurement;
    create.provenance.authority = authority_bit(AuthorityScope::Procurement);
    create.provenance.policy_generation = PolicyGeneration::first();
    create.provenance.plan = plan.value();
    create.provenance.attempt = attempt.value();
    create.provenance.evidence.push_back(make_evidence_ref(EvidenceKind::ProcurementRecord, registration.key,
                                                           registration.step));
    const Result<CreateReceipt> created = runtime.create_object(create);
    if (!created.has_value()) {
      return created.error();
    }
    std::printf("  registered   : %s state=%s revision=%s lifecycle generation=%s commit=%s receipt=%s\n",
                to_string(created.value().key).c_str(), spell(created.value().state).c_str(),
                counter(created.value().revision).c_str(), counter(created.value().lifecycle_generation).c_str(),
                counter(created.value().commit_sequence).c_str(), digest_text(created.value().receipt_digest).c_str());
  }

  say("");
  say("== the predecessor walks to Retired ==");
  say("The predecessor must be decommissioned before it can be superseded: a successor");
  say("never replaces a unit that is still in service.");
  const Result<void> retired = walk_to_retired(runtime, key_a, actor.value(), index);
  if (!retired.has_value()) {
    return retired.error();
  }

  say("");
  say("== the successor walks to Installed ==");
  say("The successor must already occupy its location before it may replace anything: a");
  say("link to a unit that is only ordered or staged would claim a physical fact that is");
  say("not there yet.");
  const Result<void> installed = walk_to_installed(runtime, key_b, actor.value(), index);
  if (!installed.has_value()) {
    return installed.error();
  }
  const Result<void> installed_c = walk_to_installed(runtime, key_c, actor.value(), index);
  if (!installed_c.has_value()) {
    return installed_c.error();
  }

  say("");
  say("== the lineage link is established with the replacement authority ==");
  say("The link is its own durable record: two identities, a reason and the authorising");
  say("provenance, bound into one link digest. It does not move either object: the");
  say("predecessor is still Retired and the successor is still Installed after it.");
  const Result<PlanId> link_plan = PlanId::parse("lineage.plan.link");
  const Result<AttemptId> link_attempt = AttemptId::parse("lineage.attempt.link");
  if (!link_plan.has_value() || !link_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  const Result<ObjectView> before_link = runtime.inspect(key_a);
  if (!before_link.has_value()) {
    return before_link.error();
  }
  ReplacementRequest link;
  link.plan = link_plan.value();
  link.attempt = link_attempt.value();
  link.predecessor = key_a;
  link.successor = key_b;
  link.expected_lifecycle_generation = before_link.value().object.lifecycle_generation;
  link.expected_revision = before_link.value().object.revision;
  link.expected_control_epoch = runtime.registry().control_epoch();
  link.provenance.actor.id = actor.value();
  link.provenance.actor.kind = ActorKind::Replacement;
  link.provenance.authority = authority_bit(AuthorityScope::Replacement);
  link.provenance.policy_generation = PolicyGeneration::first();
  link.provenance.plan = link_plan.value();
  link.provenance.attempt = link_attempt.value();
  link.provenance.evidence.push_back(make_evidence_ref(EvidenceKind::ReplacementAuthorization, key_a, 100));
  link.provenance.evidence.push_back(make_evidence_ref(EvidenceKind::SuccessorRecord, key_b, 101));
  std::printf("  link         : %s -> %s authority=%s evidence=%s\n", to_string(key_a).c_str(),
              to_string(key_b).c_str(), spell(authority_bit(AuthorityScope::Replacement)).c_str(),
              spell(evidence_bit(EvidenceKind::ReplacementAuthorization) |
                    evidence_bit(EvidenceKind::SuccessorRecord))
                  .c_str());
  for (const EvidenceRef& reference : link.provenance.evidence) {
    std::printf("  evidence     : %s digest=%s source=%s\n", spell(reference.kind).c_str(),
                digest_text(reference.digest).c_str(), reference.source.c_str());
  }
  const Result<ReplacementReceipt> linked = runtime.link_replacement(link);
  if (!linked.has_value()) {
    return linked.error();
  }
  std::printf("  receipt      : %s -> %s replacement generation %s\n",
              to_string(linked.value().predecessor).c_str(), to_string(linked.value().successor).c_str(),
              counter(linked.value().replacement_generation).c_str());
  std::printf("  commit point : commit sequence %s | logical time %s\n",
              counter(linked.value().commit_sequence).c_str(), counter(linked.value().logical_time).c_str());
  std::printf("  digests      : link=%s request=%s receipt=%s\n", digest_text(linked.value().link_digest).c_str(),
              digest_text(linked.value().request_digest).c_str(), digest_text(linked.value().receipt_digest).c_str());

  const Result<ObjectView> after_link_a = runtime.inspect(key_a);
  const Result<ObjectView> after_link_b = runtime.inspect(key_b);
  if (!after_link_a.has_value() || !after_link_b.has_value()) {
    return Error::make(ErrorCode::Internal, "the linked objects could not be inspected");
  }
  std::printf("  after link   : predecessor state=%s revision=%s | successor state=%s revision=%s\n",
              spell(after_link_a.value().object.state).c_str(), counter(after_link_a.value().object.revision).c_str(),
              spell(after_link_b.value().object.state).c_str(), counter(after_link_b.value().object.revision).c_str());

  say("");
  say("== a second successor link is refused ==");
  say("One object has at most one successor. The request below names a second, equally");
  say("installed candidate; the lineage already records a successor, so the request is");
  say("refused and neither object changes.");
  const Result<PlanId> second_plan = PlanId::parse("lineage.plan.second-successor");
  const Result<AttemptId> second_attempt = AttemptId::parse("lineage.attempt.second-successor");
  if (!second_plan.has_value() || !second_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  ReplacementRequest second = link;
  second.plan = second_plan.value();
  second.attempt = second_attempt.value();
  second.provenance.plan = second_plan.value();
  second.provenance.attempt = second_attempt.value();
  second.successor = key_c;
  second.expected_revision = after_link_a.value().object.revision;
  second.expected_lifecycle_generation = after_link_a.value().object.lifecycle_generation;
  const Result<ReplacementReceipt> refused_second = runtime.link_replacement(second);
  if (refused_second.has_value()) {
    return Error::make(ErrorCode::Internal, "a second successor link was accepted");
  }
  std::printf("  refused      : %s\n", describe(refused_second.error()).c_str());
  std::printf("  primary rank : %s is rank %u of the validation sequence\n",
              std::string(to_string(refused_second.error().code)).c_str(),
              validation_rank(refused_second.error().code));
  const Result<std::vector<ReplacementRecord>> links_after_refusal = runtime.all_links();
  if (!links_after_refusal.has_value()) {
    return links_after_refusal.error();
  }
  std::printf("  official links: %s (a refused link is not recorded)\n",
              number(static_cast<std::uint64_t>(links_after_refusal.value().size())).c_str());

  say("");
  say("== the predecessor moves to Replaced ==");
  say("Only now, with the link recorded, may the predecessor become Replaced. The");
  say("transition carries the successor, and the rule demands that the successor link");
  say("already exists and that the successor is installed or beyond.");
  const Result<ObjectView> before_replaced = runtime.inspect(key_a);
  if (!before_replaced.has_value()) {
    return before_replaced.error();
  }
  const TransitionRule* replaced_rule = find_rule(LifecycleState::Retired, LifecycleState::Replaced);
  if (replaced_rule == nullptr) {
    return Error::make(ErrorCode::Internal, "the table no longer declares retired -> replaced");
  }
  const Result<PlanId> replaced_plan = PlanId::parse("lineage.plan.replaced");
  const Result<AttemptId> replaced_attempt = AttemptId::parse("lineage.attempt.replaced");
  if (!replaced_plan.has_value() || !replaced_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  std::printf("  table rule   : %s\n", describe_rule(*replaced_rule).c_str());
  TransitionRequest replaced;
  replaced.plan = replaced_plan.value();
  replaced.attempt = replaced_attempt.value();
  replaced.key = key_a;
  replaced.expected_lifecycle_generation = before_replaced.value().object.lifecycle_generation;
  replaced.expected_revision = before_replaced.value().object.revision;
  replaced.expected_control_epoch = runtime.registry().control_epoch();
  replaced.expected_state = LifecycleState::Retired;
  replaced.target_state = LifecycleState::Replaced;
  replaced.reason = TransitionReason::SuccessorLinked;
  replaced.successor = key_b;
  replaced.provenance.actor.id = actor.value();
  replaced.provenance.actor.kind = ActorKind::Replacement;
  replaced.provenance.authority = replaced_rule->required_authority;
  replaced.provenance.policy_generation = PolicyGeneration::first();
  replaced.provenance.plan = replaced_plan.value();
  replaced.provenance.attempt = replaced_attempt.value();
  replaced.provenance.evidence = evidence_for(*replaced_rule, key_a, 200);
  const Result<TransitionReceipt> superseded = runtime.apply_transition(replaced);
  if (!superseded.has_value()) {
    return superseded.error();
  }
  std::printf("  receipt      : %s %s -> %s reason=%s\n", to_string(superseded.value().key).c_str(),
              spell(superseded.value().from).c_str(), spell(superseded.value().to).c_str(),
              spell(superseded.value().reason).c_str());
  std::printf("  revision     : %s -> %s | lifecycle generation %s | commit=%s logical=%s\n",
              counter(superseded.value().revision_before).c_str(), counter(superseded.value().revision_after).c_str(),
              counter(superseded.value().lifecycle_generation).c_str(),
              counter(superseded.value().commit_sequence).c_str(), counter(superseded.value().logical_time).c_str());
  std::printf("  digests      : request=%s entry=%s receipt=%s\n",
              digest_text(superseded.value().request_digest).c_str(),
              digest_text(superseded.value().entry_digest).c_str(),
              digest_text(superseded.value().receipt_digest).c_str());

  say("");
  say("== both objects, after the replacement ==");
  say("The predecessor is superseded and still physically present. Its key, its model,");
  say("its location, its revision and its whole history are the ones it always had: a");
  say("replacement adds a successor reference, it never merges two identities.");
  const Result<ObjectView> view_a = runtime.inspect(key_a);
  const Result<ObjectView> view_b = runtime.inspect(key_b);
  if (!view_a.has_value() || !view_b.has_value()) {
    return Error::make(ErrorCode::Internal, "the objects could not be inspected");
  }
  print_object("predecessor", view_a.value());
  print_object("successor", view_b.value());

  const Result<HistoryView> history_a = runtime.history(key_a);
  if (!history_a.has_value()) {
    return history_a.error();
  }
  say("");
  say("The predecessor's history is untouched by the replacement: every entry it had");
  say("before the link is the same entry, bound by the same chain digests.");
  for (const HistoryEntry& entry : history_a.value().entries) {
    std::printf("  #%-2s %-13s commit=%-3s logical=%-3s %-14s -> %-14s revision %s -> %s generation %s reason=%s\n",
                number(static_cast<std::uint64_t>(&entry - history_a.value().entries.data() + 1)).c_str(),
                entry.is_registration() ? "registration" : entry.is_gate_decision() ? "gate decision" : "state change",
                counter(entry.commit_sequence).c_str(), counter(entry.logical_time).c_str(), spell(entry.from).c_str(),
                spell(entry.to).c_str(), counter(entry.revision_before).c_str(), counter(entry.revision_after).c_str(),
                counter(entry.lifecycle_generation).c_str(), spell(entry.reason).c_str());
  }
  const Result<Digest> chain_head = runtime.verify();
  if (!chain_head.has_value()) {
    return chain_head.error();
  }
  std::printf("  verified state digest: %s\n", digest_text(chain_head.value()).c_str());

  say("");
  say("== a cycle is refused ==");
  say("The lineage is a set of chains, not a graph: a link that would close a cycle is");
  say("refused, and the authoritative lineage is not touched by the attempt. This is");
  say("checked on a detached copy of the authoritative graph, so even the attempt runs");
  say("against real state without mutating it.");
  const LineageGraph& authoritative = runtime.registry().lineage();
  const ReplacementRecord* recorded = authoritative.successor_of(key_a);
  if (recorded == nullptr) {
    return Error::make(ErrorCode::Internal, "the recorded link is missing from the authoritative lineage");
  }
  LineageGraph candidate = authoritative;
  ReplacementRecord cycle = *recorded;
  std::swap(cycle.predecessor, cycle.successor);
  cycle.link_digest = compute_link_digest(cycle);
  std::printf("  candidate    : %s -> %s (the recorded link with the two identities exchanged)\n",
              to_string(cycle.predecessor).c_str(), to_string(cycle.successor).c_str());
  const Result<void> closed = candidate.add(cycle);
  if (closed.has_value()) {
    return Error::make(ErrorCode::Internal, "the lineage graph admitted a cycle");
  }
  std::printf("  refused      : %s\n", describe(closed.error()).c_str());
  std::printf("  candidate    : links=%s after the refusal (the copy is unchanged)\n",
              number(static_cast<std::uint64_t>(candidate.size())).c_str());
  std::printf("  authoritative: links=%s (never touched by the refused candidate link)\n",
              number(static_cast<std::uint64_t>(authoritative.size())).c_str());

  say("");
  say("== the predecessor is physically removed ==");
  say("Removed is terminal. It says the unit is physically gone; it does not erase the");
  say("identity, the history or the lineage, and it does not reuse the key.");
  const Result<ObjectView> before_removed = runtime.inspect(key_a);
  if (!before_removed.has_value()) {
    return before_removed.error();
  }
  const TransitionRule* removed_rule = find_rule(LifecycleState::Replaced, LifecycleState::Removed);
  if (removed_rule == nullptr) {
    return Error::make(ErrorCode::Internal, "the table no longer declares replaced -> removed");
  }
  const Result<PlanId> removed_plan = PlanId::parse("lineage.plan.removed");
  const Result<AttemptId> removed_attempt = AttemptId::parse("lineage.attempt.removed");
  if (!removed_plan.has_value() || !removed_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  std::printf("  table rule   : %s\n", describe_rule(*removed_rule).c_str());
  TransitionRequest removed;
  removed.plan = removed_plan.value();
  removed.attempt = removed_attempt.value();
  removed.key = key_a;
  removed.expected_lifecycle_generation = before_removed.value().object.lifecycle_generation;
  removed.expected_revision = before_removed.value().object.revision;
  removed.expected_control_epoch = runtime.registry().control_epoch();
  removed.expected_state = LifecycleState::Replaced;
  removed.target_state = LifecycleState::Removed;
  removed.reason = TransitionReason::PhysicallyRemoved;
  removed.provenance.actor.id = actor.value();
  removed.provenance.actor.kind = ActorKind::Decommissioning;
  removed.provenance.authority = removed_rule->required_authority;
  removed.provenance.policy_generation = PolicyGeneration::first();
  removed.provenance.plan = removed_plan.value();
  removed.provenance.attempt = removed_attempt.value();
  removed.provenance.evidence = evidence_for(*removed_rule, key_a, 201);
  const Result<TransitionReceipt> gone = runtime.apply_transition(removed);
  if (!gone.has_value()) {
    return gone.error();
  }
  std::printf("  receipt      : %s %s -> %s reason=%s revision %s -> %s generation %s\n",
              to_string(gone.value().key).c_str(), spell(gone.value().from).c_str(), spell(gone.value().to).c_str(),
              spell(gone.value().reason).c_str(), counter(gone.value().revision_before).c_str(),
              counter(gone.value().revision_after).c_str(), counter(gone.value().lifecycle_generation).c_str());
  std::printf("  commit point : commit sequence %s | logical time %s | receipt=%s\n",
              counter(gone.value().commit_sequence).c_str(), counter(gone.value().logical_time).c_str(),
              digest_text(gone.value().receipt_digest).c_str());
  std::printf("  terminal     : is_terminal(removed)=%s | removed is superseded: %s\n",
              is_terminal(LifecycleState::Removed) ? "true" : "false",
              is_superseded(LifecycleState::Removed) ? "true" : "false");

  const Result<ObjectView> final_a = runtime.inspect(key_a);
  const Result<ObjectView> final_b = runtime.inspect(key_b);
  if (!final_a.has_value() || !final_b.has_value()) {
    return Error::make(ErrorCode::Internal, "the objects could not be inspected");
  }
  print_object("predecessor", final_a.value());
  print_object("successor", final_b.value());

  say("");
  say("== the lineage chain and the ancestry ==");
  say("The chain runs forward from the earliest object, the ancestry runs backward from");
  say("the object that is in place today. Both are derived from the same links.");
  const Result<void> lineage_a = print_lineage(runtime, key_a, "from A");
  if (!lineage_a.has_value()) {
    return lineage_a.error();
  }
  const Result<void> lineage_b = print_lineage(runtime, key_b, "from B");
  if (!lineage_b.has_value()) {
    return lineage_b.error();
  }
  const Result<std::vector<ObjectKey>> chain = runtime.registry().lineage().chain_from(key_a);
  const Result<std::vector<ObjectKey>> ancestry = runtime.registry().lineage().ancestry_of(key_b);
  if (!chain.has_value() || !ancestry.has_value()) {
    return Error::make(ErrorCode::Internal, "the lineage walks failed");
  }
  print_chain("chain", chain.value());
  print_chain("ancestry", ancestry.value());
  std::printf("  identity kept : predecessor key %s (Removed), successor key %s (Installed) - two keys, never one\n",
              to_string(key_a).c_str(), to_string(key_b).c_str());

  const Result<Digest> verified = runtime.verify();
  if (!verified.has_value()) {
    return verified.error();
  }
  const Result<RuntimeStatus> final_status = runtime.status();
  if (!final_status.has_value()) {
    return final_status.error();
  }
  std::printf("  final state  : state digest=%s | objects=%s | history entries=%s | lineage links=%s | commit=%s\n",
              digest_text(verified.value()).c_str(),
              number(static_cast<std::uint64_t>(final_status.value().object_count)).c_str(),
              number(static_cast<std::uint64_t>(final_status.value().history_entry_count)).c_str(),
              number(static_cast<std::uint64_t>(final_status.value().lineage_links)).c_str(),
              counter(final_status.value().commit_sequence).c_str());
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

  say("Hardware Lifecycle - replacement lineage");
  std::printf("working directory: %s\n", workspace.value().root().string().c_str());

  const Result<void> outcome = run(workspace.value());
  workspace.value().cleanup();
  if (!outcome.has_value()) {
    return fail(outcome.error());
  }
  say("");
  say("replacement lineage complete: exit code 0");
  return 0;
}
