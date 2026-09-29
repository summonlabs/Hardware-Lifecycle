// Hardware Lifecycle - benchmark.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// What this program measures: COMPLETED operations. Every measured call returns
// a receipt, and a receipt is issued only after the whole commit path finished,
// so the numbers below are completed work, never submission, queueing or enqueue
// latency. There is no queue in this program to measure.
//
//   Benchmark 1, in memory: completed apply_transition calls on an ephemeral
//   runtime, for a registry holding 1, 100 and 1000 objects.
//
//   Benchmark 2, durable: completed commits against a real store, one fresh
//   store per registry size, with the median operation time and the total wall
//   clock of each configuration in milliseconds.
//
//   State digest cost, measured separately: registry().state_digest() over the
//   same registry sizes. A durable commit computes exactly this value once per
//   commit; an ephemeral runtime never computes it at all, which is why it is
//   reported on its own instead of being attributed to benchmark 1.
//
// Every number is REAL execution on a SYNTHETIC generated workload. No hardware
// was involved: the objects, the identifiers and the evidence digests are all
// generated in this process from one seeded generator, and the workload is the
// same on every run of a given build. Nothing here is compared with anything
// else, and no speedup over any other implementation is claimed: there is
// nothing to compare with.
//
// usage: bench_hardware_lifecycle [--dir <directory>] [--keep]

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"

using namespace hardware_lifecycle;

namespace {

constexpr std::string_view kProgramName = "bench_hardware_lifecycle";

/// The one seed every generated identifier and every generated evidence digest
/// comes from. It is printed with the results, so the workload is reproducible.
constexpr std::uint64_t kSeed = 0x5eed5eed5eed5eedULL;

/// The registry sizes every configuration is measured at.
constexpr std::size_t kObjectCounts[] = {1, 100, 1000};

/// Completed operations per in-memory configuration.
constexpr std::size_t kInMemoryOperations = 2000;

/// Completed commits per durable configuration. A durable commit is a device
/// flush and an atomic manifest replacement, so it is measured in hundreds.
constexpr std::size_t kDurableOperations = 200;

/// Completed state_digest() calls per registry size in the separate digest
/// measurement.
constexpr std::size_t kDigestCalls = 200;

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Output primitives
// ---------------------------------------------------------------------------

void say(std::string_view text) { std::printf("%.*s\n", static_cast<int>(text.size()), text.data()); }

template <class T>
std::string spell(T value) {
  return std::string(to_string(value));
}

std::string number(std::uint64_t value) { return std::to_string(value); }

std::string size_text(std::size_t value) { return std::to_string(value); }

std::string digest_text(const Digest& digest) { return digest.is_zero() ? std::string("unset") : digest.hex(); }

int fail(const Error& error) {
  std::fprintf(stderr, "%.*s: FAILED: %s: %s\n", static_cast<int>(kProgramName.size()), kProgramName.data(),
               std::string(to_string(error.code)).c_str(), error.message.c_str());
  for (const FieldNote& note : error.notes) {
    std::fprintf(stderr, "  note: %s: %s\n", note.field.c_str(), note.detail.c_str());
  }
  return 1;
}

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string millis_text(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return std::string(buffer);
}

std::string micros_text(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return std::string(buffer);
}

/// Median of the per operation samples: the middle sample for an odd count and
/// the mean of the two middle samples for an even count.
double median_ms(std::vector<double> samples) {
  if (samples.empty()) {
    return 0.0;
  }
  const std::size_t middle = samples.size() / 2u;
  std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(middle), samples.end());
  if ((samples.size() % 2u) != 0u) {
    return samples[middle];
  }
  const double upper = samples[middle];
  std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(middle - 1u), samples.end());
  const double lower = samples[middle - 1u];
  return (lower + upper) / 2.0;
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
  say("usage: bench_hardware_lifecycle [--dir <directory>] [--keep]");
  say("");
  say("  --dir <directory>  root for the durable stores, created when it does not");
  say("                     exist (default: <system temporary directory>/bench_hardware_lifecycle).");
  say("                     One subdirectory per registry size is created inside it.");
  say("  --keep             leave the root and the stores behind instead of removing");
  say("                     them. A kept store is not reused by a later run: each run");
  say("                     measures freshly created stores.");
  say("  --help             print this text and exit");
}

Result<Options> parse_options(int argc, char** argv) {
  Options options;
  bool directory_set = false;
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
    if (argument == "--dir") {
      if (directory_set) {
        return Error::make(ErrorCode::MalformedRequest, "--dir may be given once");
      }
      if (index + 1 >= argc) {
        return Error::make(ErrorCode::MalformedRequest, "--dir needs a directory argument");
      }
      ++index;
      options.directory = std::filesystem::path(std::string(argv[index]));
      directory_set = true;
      continue;
    }
    return Error::make(ErrorCode::MalformedRequest, "unknown option").with("option", std::string(argument));
  }
  return options;
}

/// The directory the durable benchmark works in, removed again unless the caller
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

  /// Creates one fresh directory below the root and remembers it for cleanup.
  /// An existing, non empty directory is refused: a benchmark measures fresh
  /// stores, never a store a previous run left behind.
  Result<std::filesystem::path> fresh_subdirectory(std::string_view name) {
    const std::filesystem::path path = root_ / std::filesystem::path(std::string(name));
    std::error_code exists_error;
    if (std::filesystem::exists(path, exists_error) && !std::filesystem::is_empty(path, exists_error)) {
      return Error::make(ErrorCode::ObjectExists, "the benchmark store directory is not empty; remove it first")
          .with("path", path.string());
    }
    std::error_code create_error;
    std::filesystem::create_directories(path, create_error);
    std::error_code status_error;
    if (!std::filesystem::is_directory(path, status_error)) {
      return Error::make(ErrorCode::IoFailure, "a benchmark subdirectory could not be created")
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
// Generated workload
// ---------------------------------------------------------------------------

/// A deterministic generator. Every identifier and every evidence digest in this
/// program comes from it, so the workload is a function of the printed seed.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : engine_(seed) {}

  std::string hex16() {
    const std::uint64_t value = engine_();
    static const char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t shift = 4u * (15u - index);
      text[index] = digits[static_cast<std::size_t>((value >> shift) & 0xfu)];
    }
    return text;
  }

  Digest digest() {
    std::array<std::uint8_t, Digest::kSize> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::uint8_t>(engine_() & 0xffu);
    }
    return Digest::from_bytes(bytes);
  }

  Result<AssetId> asset_id() { return AssetId::parse("bench.hw." + hex16()); }
  Result<PlanId> plan_id() { return PlanId::parse("bench.plan." + hex16()); }
  Result<AttemptId> attempt_id() { return AttemptId::parse("bench.attempt." + hex16()); }

 private:
  std::mt19937_64 engine_;
};

struct Fixture {
  ObjectKey subject;
  Location location;
};

Result<Fixture> prepare(Runtime& runtime, Generator& generator, std::size_t object_count,
                        const Result<ModelId>& model, const Result<ActorId>& actor, const Result<SiteId>& site,
                        const Result<RackId>& rack) {
  if (!model.has_value() || !actor.has_value() || !site.has_value() || !rack.has_value()) {
    return Error::make(ErrorCode::Internal, "the benchmark's own identifier literals are malformed");
  }
  Fixture fixture;
  fixture.location.site = site.value();
  fixture.location.rack = rack.value();
  const Result<SlotId> slot = SlotId::parse("bench-slot-u01");
  if (!slot.has_value()) {
    return slot.error();
  }
  fixture.location.slot = slot.value();

  for (std::size_t index = 0; index < object_count; ++index) {
    const Result<AssetId> asset = generator.asset_id();
    const Result<PlanId> plan = generator.plan_id();
    const Result<AttemptId> attempt = generator.attempt_id();
    if (!asset.has_value() || !plan.has_value() || !attempt.has_value()) {
      return Error::make(ErrorCode::Internal, "a generated identifier was rejected");
    }
    ObjectKey key;
    key.asset = asset.value();
    key.hardware_generation = HardwareGeneration::first();
    if (index == 0) {
      fixture.subject = key;
    }

    CreateRequest create;
    create.plan = plan.value();
    create.attempt = attempt.value();
    create.key = key;
    create.kind = HardwareKind::Compute;
    create.model = model.value();
    create.firmware_generation = FirmwareGeneration::first();
    create.initial_state = LifecycleState::Staged;
    create.location = fixture.location;
    create.provenance.actor.id = actor.value();
    create.provenance.actor.kind = ActorKind::Logistics;
    create.provenance.authority = authority_bit(AuthorityScope::Logistics);
    create.provenance.policy_generation = PolicyGeneration::first();
    create.provenance.plan = plan.value();
    create.provenance.attempt = attempt.value();
    EvidenceRef reference;
    reference.kind = EvidenceKind::DeliveryReceipt;
    reference.digest = generator.digest();
    reference.source = "bench.generated/delivery_receipt";
    create.provenance.evidence.push_back(reference);

    const Result<CreateReceipt> created = runtime.create_object(create);
    if (!created.has_value()) {
      return Error(created.error());
    }
  }

  // The subject is moved to Installed once, outside every measurement, so the
  // measured alternation starts at Installed and only walks two service-free
  // edges: Installed <-> Commissioning.
  const Result<ObjectView> view = runtime.inspect(fixture.subject);
  if (!view.has_value()) {
    return view.error();
  }
  const Result<PlanId> plan = generator.plan_id();
  const Result<AttemptId> attempt = generator.attempt_id();
  if (!plan.has_value() || !attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "a generated identifier was rejected");
  }
  const TransitionRule* rule = find_rule(LifecycleState::Staged, LifecycleState::Installed);
  if (rule == nullptr) {
    return Error::make(ErrorCode::Internal, "the table no longer declares staged -> installed");
  }
  TransitionRequest install;
  install.plan = plan.value();
  install.attempt = attempt.value();
  install.key = fixture.subject;
  install.expected_lifecycle_generation = view.value().object.lifecycle_generation;
  install.expected_revision = view.value().object.revision;
  install.expected_control_epoch = runtime.registry().control_epoch();
  install.expected_state = LifecycleState::Staged;
  install.target_state = LifecycleState::Installed;
  install.reason = TransitionReason::InstallationCompleted;
  install.provenance.actor.id = actor.value();
  install.provenance.actor.kind = ActorKind::Installation;
  install.provenance.authority = rule->required_authority;
  install.provenance.policy_generation = PolicyGeneration::first();
  install.provenance.plan = plan.value();
  install.provenance.attempt = attempt.value();
  // The rule requires both kinds: the evidence requirement is a conjunction.
  EvidenceRef installation;
  installation.kind = EvidenceKind::InstallationRecord;
  installation.digest = generator.digest();
  installation.source = "bench.generated/installation_record";
  install.provenance.evidence.push_back(installation);
  EvidenceRef installed_location;
  installed_location.kind = EvidenceKind::LocationRecord;
  installed_location.digest = generator.digest();
  installed_location.source = "bench.generated/location_record";
  install.provenance.evidence.push_back(installed_location);
  const Result<TransitionReceipt> installed = runtime.apply_transition(install);
  if (!installed.has_value()) {
    return Error(installed.error());
  }
  return fixture;
}

/// Builds the next measured request for the subject, outside every timed region.
/// The workload alternates the two edges between Installed and Commissioning:
/// both are legal without service, so the object never enters or leaves the
/// service scope while it is being measured.
Result<TransitionRequest> next_request(Runtime& runtime, const ObjectKey& subject, Generator& generator,
                                       const Result<ActorId>& actor, std::size_t step) {
  if (!actor.has_value()) {
    return Error::make(ErrorCode::Internal, "the benchmark's own actor literal is malformed");
  }
  const HardwareObject* object = runtime.registry().find(subject);
  if (object == nullptr) {
    return Error::make(ErrorCode::UnknownAsset, "the measured subject is gone");
  }
  const bool forward = (step % 2u) == 0u;
  const LifecycleState target = forward ? LifecycleState::Commissioning : LifecycleState::Installed;
  const TransitionReason reason =
      forward ? TransitionReason::CommissioningStarted : TransitionReason::CommissioningReworkRequired;
  const TransitionRule* rule = find_rule(object->state, target);
  if (rule == nullptr) {
    return Error::make(ErrorCode::IllegalTransition, "the table no longer declares the measured edge")
        .with("from", spell(object->state))
        .with("to", spell(target));
  }
  const Result<PlanId> plan = generator.plan_id();
  const Result<AttemptId> attempt = generator.attempt_id();
  if (!plan.has_value() || !attempt.has_value()) {
    return Error::make(ErrorCode::Internal, "a generated identifier was rejected");
  }

  TransitionRequest request;
  request.plan = plan.value();
  request.attempt = attempt.value();
  request.key = subject;
  request.expected_lifecycle_generation = object->lifecycle_generation;
  request.expected_revision = object->revision;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.expected_state = object->state;
  request.target_state = target;
  request.reason = reason;
  request.provenance.actor.id = actor.value();
  request.provenance.actor.kind = ActorKind::Commissioning;
  request.provenance.authority = rule->required_authority;
  request.provenance.policy_generation = PolicyGeneration::first();
  request.provenance.plan = plan.value();
  request.provenance.attempt = attempt.value();
  // One generated reference per required evidence kind, because the requirement
  // is a conjunction; an empty requirement accepts any bound reference.
  for (std::size_t raw = 1; raw < kEvidenceKindCount; ++raw) {
    const EvidenceKind kind = static_cast<EvidenceKind>(raw);
    if (!rule->required_evidence.intersects(evidence_bit(kind))) {
      continue;
    }
    EvidenceRef reference;
    reference.kind = kind;
    reference.digest = generator.digest();
    reference.source = "bench.generated/" + std::string(to_string(kind));
    request.provenance.evidence.push_back(reference);
  }
  if (request.provenance.evidence.empty()) {
    EvidenceRef reference;
    reference.kind = EvidenceKind::ServiceRecord;
    reference.digest = generator.digest();
    reference.source = "bench.generated/service_record";
    request.provenance.evidence.push_back(reference);
  }
  return request;
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

void print_in_memory_header() {
  std::printf("  %9s %12s %11s %12s %15s\n", "objects", "operations", "completed", "elapsed_ms", "ops_per_second");
}

Result<void> run_in_memory(Generator& generator, const Result<ModelId>& model, const Result<ActorId>& actor,
                           const Result<SiteId>& site, const Result<RackId>& rack) {
  say("");
  say("== Benchmark 1: in-memory completed apply_transition calls (ephemeral runtime) ==");
  say("REAL execution on a SYNTHETIC generated workload. No hardware was involved.");
  say("Every measured call returned a receipt: the operation completed. The workload is");
  say("the Installed <-> Commissioning alternation of one subject object inside a");
  say("registry that holds the stated number of objects. The plan ledger and the");
  say("subject's own history grow with the operations, exactly as they do in a real");
  say("session, so each number describes the whole validated, committed operation.");
  print_in_memory_header();
  for (std::size_t object_count : kObjectCounts) {
    Result<Runtime> opened = Runtime::open_ephemeral();
    if (!opened.has_value()) {
      return opened.error();
    }
    Runtime runtime = std::move(opened).value();
    const Result<Fixture> fixture = prepare(runtime, generator, object_count, model, actor, site, rack);
    if (!fixture.has_value()) {
      return fixture.error();
    }

    std::size_t completed = 0;
    const Clock::time_point start = Clock::now();
    for (std::size_t step = 0; step < kInMemoryOperations; ++step) {
      const Result<TransitionRequest> request =
          next_request(runtime, fixture.value().subject, generator, actor, step);
      if (!request.has_value()) {
        return request.error();
      }
      const Result<TransitionReceipt> receipt = runtime.apply_transition(request.value());
      if (!receipt.has_value()) {
        return receipt.error();
      }
      if (!(receipt.value().to == request.value().target_state)) {
        return Error::make(ErrorCode::Internal, "a completed receipt does not name the requested target state");
      }
      ++completed;
    }
    const Clock::time_point end = Clock::now();
    const double elapsed = elapsed_ms(start, end);
    const double per_second = static_cast<double>(completed) * 1000.0 / elapsed;
    std::printf("  %9s %12s %11s %12s %15s\n", size_text(object_count).c_str(),
                size_text(kInMemoryOperations).c_str(), size_text(completed).c_str(), millis_text(elapsed).c_str(),
                millis_text(per_second).c_str());
    if (completed != kInMemoryOperations) {
      return Error::make(ErrorCode::Internal, "not every in-memory operation completed");
    }
  }
  return ok();
}

Result<void> run_state_digest(Generator& generator, const Result<ModelId>& model, const Result<ActorId>& actor,
                              const Result<SiteId>& site, const Result<RackId>& rack) {
  say("");
  say("== State digest cost, measured separately ==");
  say("REAL execution on a SYNTHETIC generated workload. No hardware was involved.");
  say("registry().state_digest() is exactly the value a durable commit computes once per");
  say("commit and publishes in its manifest. An ephemeral runtime never computes it, so");
  say("the numbers below are reported on their own rather than attributed to benchmark 1.");
  std::printf("  %9s %12s %11s %15s\n", "objects", "digest_calls", "elapsed_ms", "us_per_digest");
  for (std::size_t object_count : kObjectCounts) {
    Result<Runtime> opened = Runtime::open_ephemeral();
    if (!opened.has_value()) {
      return opened.error();
    }
    Runtime runtime = std::move(opened).value();
    const Result<Fixture> fixture = prepare(runtime, generator, object_count, model, actor, site, rack);
    if (!fixture.has_value()) {
      return fixture.error();
    }
    std::size_t completed = 0;
    const Clock::time_point start = Clock::now();
    for (std::size_t call = 0; call < kDigestCalls; ++call) {
      const Digest digest = runtime.registry().state_digest();
      if (digest.is_zero()) {
        return Error::make(ErrorCode::Internal, "a state digest was reported as the zero digest");
      }
      ++completed;
    }
    const Clock::time_point end = Clock::now();
    const double elapsed = elapsed_ms(start, end);
    const double per_call_micros = elapsed * 1000.0 / static_cast<double>(completed);
    std::printf("  %9s %12s %11s %15s\n", size_text(object_count).c_str(), size_text(kDigestCalls).c_str(),
                millis_text(elapsed).c_str(), micros_text(per_call_micros).c_str());
  }
  return ok();
}

Result<void> run_durable(Workspace& workspace, Generator& generator, const Result<ModelId>& model,
                         const Result<ActorId>& actor, const Result<SiteId>& site, const Result<RackId>& rack) {
  say("");
  say("== Benchmark 2: durable completed commits (a real store) ==");
  say("REAL execution on a SYNTHETIC generated workload. No hardware was involved.");
  say("Every measured call returned a receipt after the record was appended, flushed to");
  say("the device, read back, verified and published by an atomic manifest replacement:");
  say("the durable commit point of one commit.");
  say("elapsed_ms is the measured operation phase only; total_wall_ms is the whole");
  say("configuration, including creating the store and populating it with objects.");
  std::printf("  %9s %12s %11s %12s %15s %11s %14s\n", "objects", "operations", "completed", "elapsed_ms",
              "ops_per_second", "median_ms", "total_wall_ms");
  for (std::size_t object_count : kObjectCounts) {
    const std::string name = "durable-objects-" + std::to_string(object_count);
    const Result<std::filesystem::path> root = workspace.fresh_subdirectory(name);
    if (!root.has_value()) {
      return root.error();
    }
    const Clock::time_point configuration_start = Clock::now();

    OpenOptions options;
    options.root = root.value();
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
    const Result<Fixture> fixture = prepare(runtime, generator, object_count, model, actor, site, rack);
    if (!fixture.has_value()) {
      return fixture.error();
    }

    std::vector<double> samples;
    samples.reserve(kDurableOperations);
    std::size_t completed = 0;
    const Clock::time_point start = Clock::now();
    for (std::size_t step = 0; step < kDurableOperations; ++step) {
      const Result<TransitionRequest> request =
          next_request(runtime, fixture.value().subject, generator, actor, step);
      if (!request.has_value()) {
        return request.error();
      }
      const CommitSequence expected = store->commit_sequence().next().value();
      const Clock::time_point operation_start = Clock::now();
      const Result<TransitionReceipt> receipt = runtime.apply_transition(request.value());
      const Clock::time_point operation_end = Clock::now();
      if (!receipt.has_value()) {
        return receipt.error();
      }
      if (!(receipt.value().commit_sequence == expected)) {
        return Error::make(ErrorCode::Internal, "the receipt does not name the commit sequence it published")
            .with("expected", std::to_string(expected.value()))
            .with("receipt", std::to_string(receipt.value().commit_sequence.value()));
      }
      samples.push_back(elapsed_ms(operation_start, operation_end));
      ++completed;
    }
    const Clock::time_point end = Clock::now();
    const double elapsed = elapsed_ms(start, end);
    const double per_second = static_cast<double>(completed) * 1000.0 / elapsed;
    const double median = median_ms(samples);
    const double total_wall = elapsed_ms(configuration_start, end);
    std::printf("  %9s %12s %11s %12s %15s %11s %14s\n", size_text(object_count).c_str(),
                size_text(kDurableOperations).c_str(), size_text(completed).c_str(), millis_text(elapsed).c_str(),
                millis_text(per_second).c_str(), millis_text(median).c_str(), millis_text(total_wall).c_str());
    if (completed != kDurableOperations) {
      return Error::make(ErrorCode::Internal, "not every durable operation completed");
    }
    const Result<Digest> verified = runtime.verify();
    if (!verified.has_value()) {
      return verified.error();
    }
    std::printf("            store %s verified, log digest %s, %s records, %s bytes\n", root.value().string().c_str(),
                digest_text(verified.value()).c_str(), size_text(static_cast<std::size_t>(store->recovery().record_count)).c_str(),
                number(store->recovery().segment_bytes).c_str());
  }
  return ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  const Result<Options> options = parse_options(argc, argv);
  if (!options.has_value()) {
    return fail(options.error());
  }
  if (options.value().help) {
    print_usage();
    return 0;
  }

  Result<Workspace> workspace = Workspace::create(options.value().directory, kProgramName, options.value().keep);
  if (!workspace.has_value()) {
    return fail(workspace.error());
  }

  const Result<ModelId> model = ModelId::parse("bench-model-1");
  const Result<ActorId> actor = ActorId::parse("bench.generated.operator");
  const Result<SiteId> site = SiteId::parse("bench-site");
  const Result<RackId> rack = RackId::parse("bench-rack");
  if (!model.has_value() || !actor.has_value() || !site.has_value() || !rack.has_value()) {
    return fail(Error::make(ErrorCode::Internal, "the benchmark's own identifier literals are malformed"));
  }

  say("Hardware Lifecycle - benchmark");
  std::printf("working directory : %s\n", workspace.value().root().string().c_str());
  std::printf("library           : %s\n", std::string(library_version_string()).c_str());
  std::printf("generator seed    : %s (0x%llx) - every generated identifier and evidence digest\n",
              number(kSeed).c_str(), static_cast<unsigned long long>(kSeed));
  say("                    comes from std::mt19937_64 with this seed, so the workload is");
  say("                    reproducible. Every measured operation is a COMPLETED operation:");
  say("                    a receipt exists only after the commit path finished. Submission,");
  say("                    queueing and enqueue latency are never measured.");
  say("REAL execution on a SYNTHETIC generated workload. No hardware was involved.");

  Generator generator(kSeed);
  const Result<void> in_memory = run_in_memory(generator, model, actor, site, rack);
  if (!in_memory.has_value()) {
    return fail(in_memory.error());
  }
  const Result<void> digest = run_state_digest(generator, model, actor, site, rack);
  if (!digest.has_value()) {
    return fail(digest.error());
  }
  const Result<void> durable = run_durable(workspace.value(), generator, model, actor, site, rack);
  if (!durable.has_value()) {
    return fail(durable.error());
  }

  workspace.value().cleanup();
  say("");
  say("REAL execution on a SYNTHETIC generated workload. No hardware was involved. The");
  say("tables above describe this build on this host and nothing else: no before/after");
  say("comparison and no speedup over any other implementation is claimed, because there");
  say("is nothing to compare with.");
  return 0;
}
