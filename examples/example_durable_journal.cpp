// Hardware Lifecycle - example: the durable journal.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Documentation that runs. It opens a real store in the working directory,
// commits real records through the composed runtime and shows what durability
// actually buys:
//
//   * every receipt is issued only after the durable commit point was passed,
//     so a receipt that exists describes an effect that happened;
//   * the store publishes a manifest that names the state digest and the record
//     count it publishes, an append only journal of framed, CRC checked records,
//     and an operating system level single writer lock;
//   * closing and reopening the store recovers the published generation, proves
//     that the replayed state hashes to the digest the manifest published, and
//     advances the fencing counters. Authority is session scoped: every
//     recovered object is Recovered, so a request planned against the epoch of
//     the previous process is refused with stale_authority until an explicit
//     attestation re-establishes authority in this process;
//   * the whole authoritative state is exported as one canonical document and
//     imported into a second, empty store, where the ordinary commit path
//     reproduces the state digest exactly.
//
// The working directory is created when it is missing and removed again, with
// everything this program created inside it, unless --keep is given.
//
// usage: example_durable_journal [<directory>] [--keep]

#include <algorithm>
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

constexpr std::string_view kExampleName = "example_durable_journal";

// ---------------------------------------------------------------------------
// Output primitives
// ---------------------------------------------------------------------------

void say(std::string_view text) { std::printf("%.*s\n", static_cast<int>(text.size()), text.data()); }

template <class T>
std::string spell(T value) {
  return std::string(to_string(value));
}

std::string number(std::uint64_t value) { return std::to_string(value); }

/// A counter is either a real value or absent. Zero is never printed in place of
/// an absent counter.
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
  say("usage: example_durable_journal [<directory>] [--keep]");
  say("");
  say("  <directory>  directory to work in, created when it does not exist");
  say("               (default: <system temporary directory>/example_durable_journal)");
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
      std::filesystem::remove_all(root_, error);
    } else {
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
// Requests
// ---------------------------------------------------------------------------

/// One evidence reference. The digest binds the exact artifact bytes the change
/// was planned against.
EvidenceRef make_evidence_ref(EvidenceKind kind, const ObjectKey& key, const std::string& payload) {
  EvidenceRef reference;
  reference.kind = kind;
  reference.digest = Sha256::hash(payload);
  reference.source = "example.journal/" + std::string(to_string(kind));
  (void)key;
  return reference;
}

Provenance make_provenance(const PlanId& plan, const AttemptId& attempt, const ActorId& actor, ActorKind kind,
                           AuthorityScope scope, const std::vector<EvidenceRef>& evidence) {
  Provenance provenance;
  provenance.actor.id = actor;
  provenance.actor.kind = kind;
  provenance.authority = authority_bit(scope);
  provenance.policy_generation = PolicyGeneration::first();
  provenance.plan = plan;
  provenance.attempt = attempt;
  provenance.evidence = evidence;
  return provenance;
}

// ---------------------------------------------------------------------------
// Store rendering
// ---------------------------------------------------------------------------

void print_recovery(const RecoveryReport& report) {
  std::printf("  recovered generation : existing_state=%s fence_advanced=%s\n",
              report.existing_state ? "true" : "false", report.fence_advanced ? "true" : "false");
  std::printf("  records              : count=%s segment_bytes=%s discarded_tail_bytes=%s\n",
              number(report.record_count).c_str(), number(report.segment_bytes).c_str(),
              number(report.discarded_tail_bytes).c_str());
  std::printf("  watermarks           : commit sequence %s | logical time %s | control epoch %s | incarnation %s\n",
              counter(report.commit_sequence).c_str(), counter(report.logical_time).c_str(),
              counter(report.control_epoch).c_str(), counter(report.incarnation).c_str());
  std::printf("  published digests    : log=%s state=%s\n", digest_text(report.log_digest).c_str(),
              digest_text(report.state_digest).c_str());
}

Result<void> print_store_files(const std::filesystem::path& root) {
  std::vector<std::pair<std::string, std::uintmax_t>> entries;
  std::error_code error;
  std::filesystem::directory_iterator iterator(root, error);
  if (error) {
    return Error::make(ErrorCode::IoFailure, "the store directory could not be listed")
        .with("path", root.string())
        .with("cause", error.message());
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const std::filesystem::directory_entry& entry = *iterator;
    std::error_code kind_error;
    const bool regular = entry.is_regular_file(kind_error);
    std::error_code size_error;
    const std::uintmax_t size = regular ? entry.file_size(size_error) : 0;
    if (size_error) {
      return Error::make(ErrorCode::IoFailure, "a store file could not be measured")
          .with("path", entry.path().string())
          .with("cause", size_error.message());
    }
    entries.emplace_back(entry.path().filename().string(), size);
    iterator.increment(error);
    if (error) {
      return Error::make(ErrorCode::IoFailure, "the store directory could not be listed")
          .with("path", root.string())
          .with("cause", error.message());
    }
  }
  std::sort(entries.begin(), entries.end());
  for (const std::pair<std::string, std::uintmax_t>& entry : entries) {
    std::printf("  %-16s %s bytes\n", entry.first.c_str(), number(static_cast<std::uint64_t>(entry.second)).c_str());
  }
  return ok();
}

/// The kind counts of the durable log, in record kind declaration order. A kind
/// that never occurred is printed as zero: the log is described completely.
void print_record_kind_counts(const std::vector<JournalRecord>& records) {
  const RecordKind kinds[] = {RecordKind::ObjectRegistered, RecordKind::TransitionApplied, RecordKind::SuccessorLinked,
                              RecordKind::EligibilityGateChanged, RecordKind::HealthObserved,
                              RecordKind::AuthorityAttested};
  for (RecordKind kind : kinds) {
    std::size_t count = 0;
    for (const JournalRecord& record : records) {
      if (record.kind == kind) {
        ++count;
      }
    }
    std::printf("  %-22s %s\n", spell(kind).c_str(), number(static_cast<std::uint64_t>(count)).c_str());
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

namespace {

Result<void> run(Workspace& workspace) {
  const Result<std::filesystem::path> store_root = workspace.subdirectory("store");
  if (!store_root.has_value()) {
    return store_root.error();
  }
  const Result<std::filesystem::path> import_root = workspace.subdirectory("imported-store");
  if (!import_root.has_value()) {
    return import_root.error();
  }

  const Result<AssetId> asset = AssetId::parse("hl-journal-node-07");
  const Result<ModelId> model = ModelId::parse("hl-model-j100");
  const Result<ActorId> actor = ActorId::parse("example.journal.operator");
  const Result<SiteId> site = SiteId::parse("hl-site-dc2");
  const Result<RackId> rack = RackId::parse("hl-rack-b12");
  const Result<SlotId> slot = SlotId::parse("hl-slot-u04");
  if (!asset.has_value() || !model.has_value() || !actor.has_value() || !site.has_value() || !rack.has_value() ||
      !slot.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own identifier literals are malformed");
  }

  ObjectKey key;
  key.asset = asset.value();
  key.hardware_generation = HardwareGeneration::first();
  Location location;
  location.site = site.value();
  location.rack = rack.value();
  location.slot = slot.value();

  ControlEpoch epoch_before_reopen;
  CommitSequence commit_before_reopen;
  Digest published_state_digest;
  std::string document;

  {
    say("== phase 1: a real store, real commits ==");
    say("Every mutation below follows exactly one path: validate against the identity,");
    say("generation, revision, epoch and evidence the caller planned against, copy the");
    say("authoritative state, apply the record to the copy, commit it, publish the copy,");
    say("and only then issue the receipt. A receipt that exists describes an effect that");
    say("happened.");

    OpenOptions options;
    options.root = store_root.value();
    options.create_if_missing = true;
    Result<Runtime> opened = Runtime::open(options);
    if (!opened.has_value()) {
      return opened.error();
    }
    Runtime runtime = std::move(opened).value();
    const Store* store = runtime.store();
    if (store == nullptr) {
      return Error::make(ErrorCode::Internal, "a durable runtime must own a store");
    }

    const Result<RuntimeStatus> status = runtime.status();
    if (!status.has_value()) {
      return status.error();
    }
    std::printf("  store root           : %s\n", runtime.root().string().c_str());
    std::printf("  durability           : %s | writable=%s\n", spell(status.value().durability).c_str(),
                status.value().writable ? "true" : "false");
    std::printf("  store watermarks     : commit sequence %s | logical time %s | control epoch %s | incarnation %s\n",
                counter(store->commit_sequence()).c_str(), counter(store->logical_time()).c_str(),
                counter(store->control_epoch()).c_str(), counter(store->incarnation()).c_str());
    print_recovery(status.value().recovery);

    say("");
    say("Registration is a durable fact like any other. Staged is the initial state");
    say("because the unit is on site; installed, commissioned and active are claims only");
    say("the transition table may establish.");
    const Result<PlanId> create_plan = PlanId::parse("journal.plan.create");
    const Result<AttemptId> create_attempt = AttemptId::parse("journal.attempt.0001");
    if (!create_plan.has_value() || !create_attempt.has_value()) {
      return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
    }
    CreateRequest create;
    create.plan = create_plan.value();
    create.attempt = create_attempt.value();
    create.key = key;
    create.kind = HardwareKind::Compute;
    create.model = model.value();
    create.firmware_generation = FirmwareGeneration::first();
    create.initial_state = LifecycleState::Staged;
    create.location = location;
    const std::vector<EvidenceRef> create_evidence = {
        make_evidence_ref(EvidenceKind::DeliveryReceipt, key, "example artifact: delivery receipt for " + to_string(key))};
    create.provenance =
        make_provenance(create_plan.value(), create_attempt.value(), actor.value(), ActorKind::Logistics,
                        AuthorityScope::Logistics, create_evidence);
    std::printf("\n  registration         : authority=%s evidence=%s\n",
                spell(authority_bit(AuthorityScope::Logistics)).c_str(),
                spell(evidence_bit(EvidenceKind::DeliveryReceipt)).c_str());
    for (const EvidenceRef& reference : create_evidence) {
      std::printf("  evidence             : %s digest=%s source=%s\n", spell(reference.kind).c_str(),
                  digest_text(reference.digest).c_str(), reference.source.c_str());
    }
    const Result<CreateReceipt> created = runtime.create_object(create);
    if (!created.has_value()) {
      return created.error();
    }
    std::printf("  receipt              : %s state=%s revision=%s lifecycle generation=%s\n",
                to_string(created.value().key).c_str(), spell(created.value().state).c_str(),
                counter(created.value().revision).c_str(), counter(created.value().lifecycle_generation).c_str());
    std::printf("  durable commit point : commit sequence %s | logical time %s | receipt digest %s\n",
                counter(created.value().commit_sequence).c_str(), counter(created.value().logical_time).c_str(),
                digest_text(created.value().receipt_digest).c_str());

    say("");
    say("A health observation is recorded, stamped with an authoritative observation");
    say("sequence and logical time, and changes nothing else: health is an observation,");
    say("not authority over the lifecycle state.");
    const Result<PlanId> health_plan = PlanId::parse("journal.plan.health");
    const Result<AttemptId> health_attempt = AttemptId::parse("journal.attempt.0002");
    const Result<WallClock> observed_at = WallClock::parse("2026-02-03T11:05:00Z");
    if (!health_plan.has_value() || !health_attempt.has_value() || !observed_at.has_value()) {
      return Error::make(ErrorCode::Internal, "the example's own health literals are malformed");
    }
    const std::string health_payload = "example payload: health impaired on " + to_string(key);
    HealthObservationRequest observation;
    observation.plan = health_plan.value();
    observation.attempt = health_attempt.value();
    observation.key = key;
    observation.observation.key = key;
    observation.observation.health = HealthStatus::Impaired;
    observation.observation.readiness = ReadinessStatus::NotReady;
    observation.observation.availability = AvailabilityStatus::Available;
    observation.observation.evidence_digest = Sha256::hash(health_payload);
    observation.observation.source = "example.journal/monitor";
    observation.observation.observed_wall_clock = observed_at.value();
    EvidenceRef health_evidence;
    health_evidence.kind = EvidenceKind::HealthEvidence;
    health_evidence.digest = observation.observation.evidence_digest;
    health_evidence.source = observation.observation.source;
    observation.provenance =
        make_provenance(health_plan.value(), health_attempt.value(), actor.value(), ActorKind::Automation,
                        AuthorityScope::Service, {health_evidence});
    std::printf("\n  observation          : health=%s evidence digest=%s source=%s wall clock=%s\n",
                spell(observation.observation.health).c_str(),
                digest_text(observation.observation.evidence_digest).c_str(),
                observation.observation.source.c_str(), observation.observation.observed_wall_clock.value().c_str());
    const Result<HealthReceipt> health = runtime.observe_health(observation);
    if (!health.has_value()) {
      return health.error();
    }
    std::printf("  receipt              : observation sequence %s\n", counter(health.value().sequence).c_str());
    std::printf("  durable commit point : commit sequence %s | logical time %s | receipt digest %s\n",
                counter(health.value().commit_sequence).c_str(), counter(health.value().logical_time).c_str(),
                digest_text(health.value().receipt_digest).c_str());

    say("");
    say("The durable log itself. A record frame carries a magic, the format version, the");
    say("record kind, the payload length, the commit sequence, the logical time and a");
    say("CRC-32; the manifest publishes the prefix length, the record count and the");
    say("SHA-256 of the published bytes, so uncommitted bytes are detectable and are");
    say("reported rather than believed.");
    const Result<std::vector<JournalRecord>> records = store->read_records();
    if (!records.has_value()) {
      return records.error();
    }
    for (const JournalRecord& record : records.value()) {
      std::printf("  record #%s           : kind=%s logical time=%s payload bytes=%s\n",
                  counter(record.sequence).c_str(), spell(record.kind).c_str(), counter(record.logical_time).c_str(),
                  number(static_cast<std::uint64_t>(record.payload.size())).c_str());
    }
    std::printf("  record counts        : total=%s\n", number(static_cast<std::uint64_t>(records.value().size())).c_str());
    print_record_kind_counts(records.value());
    const Result<Digest> log_digest = store->verify();
    if (!log_digest.has_value()) {
      return log_digest.error();
    }
    std::printf("  store verify         : log digest %s\n", digest_text(log_digest.value()).c_str());

    say("");
    say("What a durable store is on disk: an atomically replaced manifest, an append");
    say("only journal and a writer lock. Nothing here is edited in place.");
    const Result<void> listed = print_store_files(store_root.value());
    if (!listed.has_value()) {
      return listed.error();
    }

    const Result<RuntimeStatus> final_status = runtime.status();
    if (!final_status.has_value()) {
      return final_status.error();
    }
    std::printf("\n  watermarks           : commit sequence %s | logical time %s\n",
                counter(final_status.value().commit_sequence).c_str(),
                counter(final_status.value().logical_time).c_str());
    std::printf("  contents             : objects=%s history entries=%s applied plans=%s\n",
                number(static_cast<std::uint64_t>(final_status.value().object_count)).c_str(),
                number(static_cast<std::uint64_t>(final_status.value().history_entry_count)).c_str(),
                number(static_cast<std::uint64_t>(final_status.value().applied_plans)).c_str());

    epoch_before_reopen = runtime.registry().control_epoch();
    commit_before_reopen = runtime.registry().commit_sequence();
    published_state_digest = runtime.registry().state_digest();
    std::printf("  state digest         : %s\n", digest_text(published_state_digest).c_str());
  }

  say("");
  say("The runtime was destroyed. Its writer lock and its journal handle are released,");
  say("and nothing in this process holds authority over the store any more.");

  say("");
  say("== phase 2: recovery fences authority ==");
  say("Reopening replays the published prefix, compares the replayed state digest with");
  say("the digest the manifest published, and advances the fencing counters. Recovered");
  say("state is not live evidence: every object loaded by the restart is Recovered, and");
  say("that is fenced authority, not authority that survived a restart.");

  OpenOptions reopen_options;
  reopen_options.root = store_root.value();
  reopen_options.create_if_missing = false;
  Result<Runtime> reopened = Runtime::open(reopen_options);
  if (!reopened.has_value()) {
    return reopened.error();
  }
  Runtime runtime = std::move(reopened).value();
  const Store* store = runtime.store();
  if (store == nullptr) {
    return Error::make(ErrorCode::Internal, "a durable runtime must own a store");
  }
  const Result<RuntimeStatus> status = runtime.status();
  if (!status.has_value()) {
    return status.error();
  }
  std::printf("  epoch before close   : %s | commit sequence %s\n", counter(epoch_before_reopen).c_str(),
              counter(commit_before_reopen).c_str());
  print_recovery(status.value().recovery);
  const Digest recovered_digest = runtime.registry().state_digest();
  std::printf("  replayed state digest: %s (matches the digest published before the restart: %s)\n",
              digest_text(recovered_digest).c_str(),
              recovered_digest == published_state_digest ? "true" : "false");
  if (!(recovered_digest == published_state_digest)) {
    return Error::make(ErrorCode::IntegrityFailure, "the reopened store recovered a different state");
  }

  const Result<ObjectView> recovered_view = runtime.inspect(key);
  if (!recovered_view.has_value()) {
    return recovered_view.error();
  }
  std::printf("  object               : %s state=%s revision=%s lifecycle generation=%s authority=%s\n",
              to_string(recovered_view.value().object.key).c_str(), spell(recovered_view.value().object.state).c_str(),
              counter(recovered_view.value().object.revision).c_str(),
              counter(recovered_view.value().object.lifecycle_generation).c_str(),
              spell(recovered_view.value().object.authority).c_str());

  say("");
  say("A request planned against the writer epoch of the previous process is refused.");
  say("The revision, the generation and the state are current; only the authority is");
  say("stale, and stale authority is enough to stop the change.");
  const Result<PlanId> stale_plan = PlanId::parse("journal.plan.stale");
  const Result<AttemptId> stale_attempt = AttemptId::parse("journal.attempt.0003");
  if (!stale_plan.has_value() || !stale_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  TransitionRequest stale;
  stale.plan = stale_plan.value();
  stale.attempt = stale_attempt.value();
  stale.key = key;
  stale.expected_lifecycle_generation = recovered_view.value().object.lifecycle_generation;
  stale.expected_revision = recovered_view.value().object.revision;
  stale.expected_control_epoch = epoch_before_reopen;
  stale.expected_state = LifecycleState::Staged;
  stale.target_state = LifecycleState::Installed;
  stale.reason = TransitionReason::InstallationCompleted;
  stale.provenance = make_provenance(
      stale_plan.value(), stale_attempt.value(), actor.value(), ActorKind::Installation, AuthorityScope::Installation,
      {make_evidence_ref(EvidenceKind::InstallationRecord, key, "example artifact: installation record"),
       make_evidence_ref(EvidenceKind::LocationRecord, key, "example artifact: location record")});
  const Result<TransitionReceipt> refused = runtime.apply_transition(stale);
  if (refused.has_value()) {
    return Error::make(ErrorCode::Internal, "a transition planned against the previous epoch was applied");
  }
  std::printf("  refused              : %s\n", describe(refused.error()).c_str());
  std::printf("  primary rank         : %s is rank %u | retryable after re-planning: %s\n",
              std::string(to_string(refused.error().code)).c_str(), validation_rank(refused.error().code),
              is_retryable(refused.error().code) ? "true" : "false");

  say("");
  say("Attestation is the explicit act that re-establishes authority in this process.");
  say("It is a durable record, and it is deliberately not durable authority: the next");
  say("restart fences it again.");
  const Result<PlanId> attest_plan = PlanId::parse("journal.plan.attest");
  const Result<AttemptId> attest_attempt = AttemptId::parse("journal.attempt.0004");
  if (!attest_plan.has_value() || !attest_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  AttestationRequest attestation;
  attestation.plan = attest_plan.value();
  attestation.attempt = attest_attempt.value();
  attestation.key = key;
  attestation.expected_revision = recovered_view.value().object.revision;
  attestation.expected_control_epoch = runtime.registry().control_epoch();
  attestation.provenance = make_provenance(
      attest_plan.value(), attest_attempt.value(), actor.value(), ActorKind::Recovery, AuthorityScope::Recovery,
      {make_evidence_ref(EvidenceKind::RecoveryAttestation, key, "example artifact: recovery attestation")});
  const Result<AttestationReceipt> attested = runtime.attest_authority(attestation);
  if (!attested.has_value()) {
    return attested.error();
  }
  std::printf("  attestation          : %s revision %s\n", to_string(attested.value().key).c_str(),
              counter(attested.value().revision).c_str());
  std::printf("  durable commit point : commit sequence %s | logical time %s | receipt digest %s\n",
              counter(attested.value().commit_sequence).c_str(), counter(attested.value().logical_time).c_str(),
              digest_text(attested.value().receipt_digest).c_str());
  const Result<ObjectView> live_view = runtime.inspect(key);
  if (!live_view.has_value()) {
    return live_view.error();
  }
  std::printf("  authority now        : %s (state %s, revision %s, generation %s unchanged by the attestation)\n",
              spell(live_view.value().object.authority).c_str(), spell(live_view.value().object.state).c_str(),
              counter(live_view.value().object.revision).c_str(),
              counter(live_view.value().object.lifecycle_generation).c_str());

  say("");
  say("With authority re-established in this process, the same change is planned again");
  say("against the current epoch and passes. Installed is not commissioned and not");
  say("active: this is a physical fact, not a service fact.");
  const Result<PlanId> install_plan = PlanId::parse("journal.plan.install");
  const Result<AttemptId> install_attempt = AttemptId::parse("journal.attempt.0005");
  if (!install_plan.has_value() || !install_attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "the example's own plan literals are malformed");
  }
  TransitionRequest install;
  install.plan = install_plan.value();
  install.attempt = install_attempt.value();
  install.key = key;
  install.expected_lifecycle_generation = live_view.value().object.lifecycle_generation;
  install.expected_revision = live_view.value().object.revision;
  install.expected_control_epoch = runtime.registry().control_epoch();
  install.expected_state = LifecycleState::Staged;
  install.target_state = LifecycleState::Installed;
  install.reason = TransitionReason::InstallationCompleted;
  install.provenance = make_provenance(
      install_plan.value(), install_attempt.value(), actor.value(), ActorKind::Installation,
      AuthorityScope::Installation,
      {make_evidence_ref(EvidenceKind::InstallationRecord, key, "example artifact: installation record"),
       make_evidence_ref(EvidenceKind::LocationRecord, key, "example artifact: location record")});
  const Result<TransitionReceipt> installed = runtime.apply_transition(install);
  if (!installed.has_value()) {
    return installed.error();
  }
  std::printf("  receipt              : %s %s -> %s reason=%s\n", to_string(installed.value().key).c_str(),
              spell(installed.value().from).c_str(), spell(installed.value().to).c_str(),
              spell(installed.value().reason).c_str());
  std::printf("  revision             : %s -> %s | lifecycle generation %s\n",
              counter(installed.value().revision_before).c_str(), counter(installed.value().revision_after).c_str(),
              counter(installed.value().lifecycle_generation).c_str());
  std::printf("  durable commit point : commit sequence %s | logical time %s\n",
              counter(installed.value().commit_sequence).c_str(), counter(installed.value().logical_time).c_str());
  std::printf("  digests              : request=%s entry=%s receipt=%s\n",
              digest_text(installed.value().request_digest).c_str(), digest_text(installed.value().entry_digest).c_str(),
              digest_text(installed.value().receipt_digest).c_str());

  say("");
  say("Verify re-reads the published generation, replays the whole journal and compares");
  say("the state it produces with the state held in memory.");
  const Result<Digest> verified = runtime.verify();
  if (!verified.has_value()) {
    return verified.error();
  }
  std::printf("  store verify         : log digest %s\n", digest_text(verified.value()).c_str());
  std::printf("  store watermarks     : commit sequence %s | logical time %s | control epoch %s | incarnation %s\n",
              counter(store->commit_sequence()).c_str(), counter(store->logical_time()).c_str(),
              counter(store->control_epoch()).c_str(), counter(store->incarnation()).c_str());

  say("");
  say("== phase 3: export and import reproduce the state digest ==");
  say("The document carries the authoritative event log verbatim. Import is not a bulk");
  say("file load: every record is decoded strictly and replayed through the ordinary");
  say("commit path, in global commit order, into an empty store. A divergent import is");
  say("refused, never merged.");
  ExportOptions export_options;
  const Snapshot snapshot = runtime.snapshot(export_options);
  std::printf("  document             : format version %s | library %s | importable=%s\n",
              number(snapshot.header.format_version).c_str(), snapshot.header.library_version.c_str(),
              snapshot.header.importable ? "true" : "false");
  std::printf("  declared state digest: %s\n", digest_text(snapshot.header.state_digest).c_str());
  std::printf("  declared watermarks  : commit sequence %s | logical time %s | observation sequence %s | epoch %s | "
              "incarnation %s\n",
              counter(snapshot.header.commit_sequence).c_str(), counter(snapshot.header.logical_time).c_str(),
              counter(snapshot.header.observation_sequence).c_str(), counter(snapshot.header.control_epoch).c_str(),
              counter(snapshot.header.incarnation).c_str());
  std::printf("  declared contents    : events=%s objects=%s history=%s lineage=%s plans=%s\n",
              number(static_cast<std::uint64_t>(snapshot.header.event_count)).c_str(),
              number(static_cast<std::uint64_t>(snapshot.header.object_count)).c_str(),
              number(static_cast<std::uint64_t>(snapshot.header.history_entry_count)).c_str(),
              number(static_cast<std::uint64_t>(snapshot.header.lineage_link_count)).c_str(),
              number(static_cast<std::uint64_t>(snapshot.header.applied_plan_count)).c_str());
  const Result<std::string> exported = to_json(snapshot, export_options);
  if (!exported.has_value()) {
    return exported.error();
  }
  document = exported.value();
  std::printf("  document bytes       : %s\n", number(static_cast<std::uint64_t>(document.size())).c_str());

  OpenOptions import_options;
  import_options.root = import_root.value();
  import_options.create_if_missing = true;
  Result<Runtime> importing = Runtime::open(import_options);
  if (!importing.has_value()) {
    return importing.error();
  }
  Runtime target = std::move(importing).value();
  const Result<RuntimeStatus> target_status = target.status();
  if (!target_status.has_value()) {
    return target_status.error();
  }
  std::printf("\n  target store         : %s objects=%s commit sequence %s (a document is imported into an empty store)\n",
              target.root().string().c_str(),
              number(static_cast<std::uint64_t>(target_status.value().object_count)).c_str(),
              counter(target_status.value().commit_sequence).c_str());

  ImportOptions import_options_policy;
  const Result<ImportReport> report = target.import_document(document, import_options_policy);
  if (!report.has_value()) {
    return report.error();
  }
  std::printf("  events replayed      : %s\n",
              number(static_cast<std::uint64_t>(report.value().events)).c_str());
  std::printf("  produced contents    : objects=%s history=%s lineage=%s plans=%s health=%s\n",
              number(static_cast<std::uint64_t>(report.value().objects)).c_str(),
              number(static_cast<std::uint64_t>(report.value().history_entries)).c_str(),
              number(static_cast<std::uint64_t>(report.value().lineage_links)).c_str(),
              number(static_cast<std::uint64_t>(report.value().applied_plans)).c_str(),
              number(static_cast<std::uint64_t>(report.value().health_observations)).c_str());
  std::printf("  declared state digest: %s\n", digest_text(report.value().declared_state_digest).c_str());
  std::printf("  produced state digest: %s\n", digest_text(report.value().produced_state_digest).c_str());
  std::printf("  reproduced exactly   : declared==produced: %s | produced==source store: %s\n",
              report.value().declared_state_digest == report.value().produced_state_digest ? "true" : "false",
              report.value().produced_state_digest == runtime.registry().state_digest() ? "true" : "false");
  if (!(report.value().produced_state_digest == runtime.registry().state_digest())) {
    return Error::make(ErrorCode::IntegrityFailure, "the imported store does not hold the state the source store holds");
  }
  std::printf("  imported watermarks  : commit sequence %s | logical time %s\n",
              counter(report.value().commit_sequence).c_str(), counter(report.value().logical_time).c_str());

  const Result<ObjectView> imported_view = target.inspect(key);
  if (!imported_view.has_value()) {
    return imported_view.error();
  }
  std::printf("  imported object      : %s state=%s revision=%s lifecycle generation=%s gate=%s authority=%s\n",
              to_string(imported_view.value().object.key).c_str(), spell(imported_view.value().object.state).c_str(),
              counter(imported_view.value().object.revision).c_str(),
              counter(imported_view.value().object.lifecycle_generation).c_str(),
              spell(imported_view.value().object.eligibility_gate).c_str(),
              spell(imported_view.value().object.authority).c_str());
  std::printf("  imported health      : present=%s health=%s recorded_at=%s sequence %s\n",
              imported_view.value().health.present ? "true" : "false",
              spell(imported_view.value().health.observation.health).c_str(),
              counter(imported_view.value().health.recorded_at).c_str(),
              counter(imported_view.value().health.sequence).c_str());

  const Result<Digest> target_verified = target.verify();
  if (!target_verified.has_value()) {
    return target_verified.error();
  }
  std::printf("  target store verify  : log digest %s\n", digest_text(target_verified.value()).c_str());
  const Result<void> target_files = print_store_files(import_root.value());
  if (!target_files.has_value()) {
    return target_files.error();
  }
  say("");
  say("Both stores now hold the same state digest, reached by two different routes: a");
  say("session of live commits, and a replay of the exported log into an empty store.");
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

  Result<Workspace> workspace = Workspace::create(options.value().directory, kExampleName, options.value().keep);
  if (!workspace.has_value()) {
    return fail(workspace.error());
  }

  say("Hardware Lifecycle - durable journal");
  std::printf("working directory: %s\n", workspace.value().root().string().c_str());

  const Result<void> outcome = run(workspace.value());
  workspace.value().cleanup();
  if (!outcome.has_value()) {
    return fail(outcome.error());
  }
  say("");
  say("durable journal complete: exit code 0");
  return 0;
}
