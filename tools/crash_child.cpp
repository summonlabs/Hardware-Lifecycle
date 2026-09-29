// Hardware Lifecycle - the crash consistency child.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// One real process terminated at one real durable step. The parent starts this
// program, this program commits real records through the public runtime and then
// terminates itself from inside the commit observer, so the store is left in
// exactly the state a crash leaves it in. Nothing is simulated: the exit code is
// produced by TerminateProcess, not by a return from main, so no destructor
// runs, no buffer is drained and no lock is released by the process itself.
//
//   crash_child --store <dir> --stage <CommitStage> [--records <n>] [--asset <id>]
//
// The first <n> registrations (default 1) make the store non trivial. The
// registration after them is the crash candidate: the observer terminates the
// process the moment the requested stage of that one commit is reached. When the
// stage is never reached the program says so and exits 0.
//
// Exit codes: 0 the stage was not reached, 1 usage error, 2 the runtime rejected
// a request, 3 the deliberate termination the parent observes.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include <windows.h>

#include "hardware_lifecycle/hardware_lifecycle.hpp"

namespace hw = hardware_lifecycle;

namespace {

struct Arguments {
  std::string store;
  std::string stage_text;
  hw::CommitStage stage = hw::CommitStage::AfterAppend;
  std::uint64_t records = 1;
  std::string asset = "crash-child";
  bool help = false;
};

struct StageName {
  hw::CommitStage stage;
  std::string_view enumerator;
};

// The canonical spelling is the library's snake_case one. The enumerator name is
// accepted as well so a proof harness can name the step the way the header does.
const StageName kStageNames[] = {
    {hw::CommitStage::BeforeAppend, "BeforeAppend"}, {hw::CommitStage::AfterAppend, "AfterAppend"},
    {hw::CommitStage::AfterFlush, "AfterFlush"},     {hw::CommitStage::AfterVerify, "AfterVerify"},
    {hw::CommitStage::BeforePublish, "BeforePublish"}, {hw::CommitStage::AfterPublish, "AfterPublish"},
    {hw::CommitStage::AfterFencePublish, "AfterFencePublish"},
};

[[nodiscard]] hw::Result<hw::CommitStage> parse_stage(std::string_view text) {
  const hw::Result<hw::CommitStage> canonical = hw::parse_commit_stage(text);
  if (canonical.has_value()) {
    return canonical.value();
  }
  for (const StageName& candidate : kStageNames) {
    if (hw::ascii_iequals(candidate.enumerator, text)) {
      return candidate.stage;
    }
  }
  return hw::Error::make(hw::ErrorCode::MalformedRequest, "unknown commit stage spelling")
      .with("commit stage", std::string(text));
}

[[nodiscard]] hw::Error usage_error(std::string message) {
  return hw::Error::make(hw::ErrorCode::MalformedRequest, std::move(message));
}

[[nodiscard]] hw::Result<Arguments> parse_arguments(int argc, char** argv) {
  Arguments arguments;
  bool store_given = false;
  bool stage_given = false;
  bool records_given = false;
  bool asset_given = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view token = argv[index];
    if (token == "--help" || token == "-h") {
      arguments.help = true;
      return arguments;
    }
    if (token != "--store" && token != "--stage" && token != "--records" && token != "--asset") {
      return usage_error("unknown option " + std::string(token));
    }
    if (index + 1 >= argc) {
      return usage_error("option " + std::string(token) + " requires a value");
    }
    ++index;
    const std::string value = argv[index];
    if (token == "--store") {
      if (store_given) {
        return usage_error("option --store was given more than once");
      }
      store_given = true;
      arguments.store = value;
    } else if (token == "--stage") {
      if (stage_given) {
        return usage_error("option --stage was given more than once");
      }
      stage_given = true;
      arguments.stage_text = value;
      const hw::Result<hw::CommitStage> stage = parse_stage(value);
      if (!stage.has_value()) {
        return usage_error("option --stage: " + stage.error().message);
      }
      arguments.stage = stage.value();
    } else if (token == "--records") {
      if (records_given) {
        return usage_error("option --records was given more than once");
      }
      records_given = true;
      const hw::Result<std::uint64_t> records = hw::parse_u64_strict(value);
      if (!records.has_value()) {
        return usage_error("option --records: " + records.error().message);
      }
      if (records.value() == (std::numeric_limits<std::uint64_t>::max)()) {
        return usage_error("option --records is above its maximum");
      }
      arguments.records = records.value();
    } else {
      if (asset_given) {
        return usage_error("option --asset was given more than once");
      }
      asset_given = true;
      if (!hw::is_valid_identifier(value)) {
        return usage_error("option --asset is not a valid asset id");
      }
      if (value.size() + 24 > hw::limits::kMaxIdentifierBytes) {
        return usage_error("option --asset is too long to carry a per record suffix");
      }
      arguments.asset = value;
    }
  }
  if (!store_given) {
    return usage_error("missing required option --store");
  }
  if (!stage_given) {
    return usage_error("missing required option --stage");
  }
  return arguments;
}

/// Terminates the process from inside the commit it is watching. The stage text
/// is echoed exactly as it was typed, so the line the parent reads names the step
/// in the caller's own spelling.
class StageObserver final : public hw::CommitObserver {
 public:
  StageObserver(hw::CommitStage stage, std::string stage_text, std::uint64_t ignore_through)
      : stage_(stage), stage_text_(std::move(stage_text)), ignore_through_(ignore_through) {}

  void on_commit_stage(hw::CommitStage stage, hw::CommitSequence sequence) override {
    if (stage != stage_ || sequence.value() <= ignore_through_) {
      return;
    }
    std::cout << "stage " << stage_text_ << '\n';
    std::cout.flush();
    // A real termination at a real durable step: no unwinding, no orderly
    // shutdown, no lock release by this process.
    TerminateProcess(GetCurrentProcess(), 3);
  }

 private:
  hw::CommitStage stage_;
  std::string stage_text_;
  std::uint64_t ignore_through_ = 0;
};

[[nodiscard]] hw::Result<hw::CreateRequest> registration(const std::string& asset_base, std::uint64_t index) {
  const std::string asset_text = asset_base + "-" + std::to_string(index);
  const hw::Result<hw::AssetId> asset = hw::AssetId::parse(asset_text);
  if (!asset.has_value()) {
    return asset.error();
  }
  const hw::Result<hw::ModelId> model = hw::ModelId::parse("crash-model");
  if (!model.has_value()) {
    return model.error();
  }
  const hw::Result<hw::PlanId> plan = hw::PlanId::parse("crash-plan-" + std::to_string(index));
  if (!plan.has_value()) {
    return plan.error();
  }
  const hw::Result<hw::AttemptId> attempt = hw::AttemptId::parse("crash-attempt");
  if (!attempt.has_value()) {
    return attempt.error();
  }
  const hw::Result<hw::ActorId> actor = hw::ActorId::parse("crash-child");
  if (!actor.has_value()) {
    return actor.error();
  }

  hw::CreateRequest request;
  request.plan = plan.value();
  request.attempt = attempt.value();
  request.key.asset = asset.value();
  request.key.hardware_generation = hw::HardwareGeneration::from_value(index);
  request.kind = hw::HardwareKind::Compute;
  request.model = model.value();
  request.initial_state = hw::LifecycleState::Ordered;
  // The receipt is rebuilt from the provenance, so the two identities must agree.
  request.provenance.plan = request.plan;
  request.provenance.attempt = request.attempt;
  request.provenance.actor.id = actor.value();
  request.provenance.actor.kind = hw::ActorKind::Recovery;
  request.provenance.authority = hw::authority_bit(hw::AuthorityScope::Procurement);
  hw::EvidenceRef evidence;
  evidence.kind = hw::EvidenceKind::ProcurementRecord;
  // A real digest over the identity of this proof record, never the all zero
  // digest: an unbound observation is not evidence.
  evidence.digest = hw::Sha256::hash(asset_text);
  evidence.source = "crash-child/proof";
  request.provenance.evidence.push_back(evidence);
  return request;
}

/// Prints one failure and returns its exit code. Every line this program writes
/// goes to stdout, and stdout is flushed before each line, so the parent reads
/// exactly the lines that were produced before the termination.
[[nodiscard]] int fail(const hw::Error& error, int code) {
  std::cout << "error: " << hw::describe(error) << '\n';
  std::cout.flush();
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  const hw::Result<Arguments> parsed = parse_arguments(argc, argv);
  if (!parsed.has_value()) {
    return fail(parsed.error(), 1);
  }
  const Arguments& arguments = parsed.value();
  if (arguments.help) {
    std::cout << "usage: crash_child --store <dir> --stage <CommitStage> [--records <n>] [--asset <id>]\n";
    std::cout.flush();
    return 0;
  }

  StageObserver observer(arguments.stage, arguments.stage_text, arguments.records);
  hw::OpenOptions options;
  options.root = std::filesystem::path(arguments.store);
  options.read_only = false;
  options.create_if_missing = true;
  options.observer = &observer;

  hw::Result<hw::Runtime> runtime = hw::Runtime::open(options);
  if (!runtime.has_value()) {
    return fail(runtime.error(), 2);
  }

  // A registration that was already applied is replayed rather than committed
  // again; the line then names the sequence of the commit that first produced
  // the record, because that is the commit the receipt describes.
  for (std::uint64_t index = 1; index <= arguments.records; ++index) {
    const hw::Result<hw::CreateRequest> request = registration(arguments.asset, index);
    if (!request.has_value()) {
      return fail(request.error(), 2);
    }
    const hw::Result<hw::CreateReceipt> receipt = runtime.value().create_object(request.value());
    if (!receipt.has_value()) {
      return fail(receipt.error(), 2);
    }
    std::cout << "committed " << receipt.value().commit_sequence.value() << '\n';
    std::cout.flush();
  }

  // The crash candidate. Reaching the requested stage terminates the process
  // here, so the line below is only written when the stage was never reached.
  const hw::Result<hw::CreateRequest> request = registration(arguments.asset, arguments.records + 1);
  if (!request.has_value()) {
    return fail(request.error(), 2);
  }
  const hw::Result<hw::CreateReceipt> receipt = runtime.value().create_object(request.value());
  if (!receipt.has_value()) {
    return fail(receipt.error(), 2);
  }
  std::cout << "committed " << receipt.value().commit_sequence.value() << '\n';
  std::cout.flush();
  std::cout << "stage " << arguments.stage_text << " not reached\n";
  std::cout.flush();
  return 0;
}
