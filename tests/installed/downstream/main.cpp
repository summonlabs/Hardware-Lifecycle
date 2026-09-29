// Hardware Lifecycle - downstream consumer of the installed package.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is built outside the Hardware Lifecycle source tree, against the
// installed package, through find_package(HardwareLifecycle CONFIG REQUIRED) and
// the namespaced imported target HardwareLifecycle::hardware_lifecycle. It
// exercises the installed artifact rather than the in-tree one: it registers
// hardware, walks the canonical lifecycle, checks that a health observation does
// not rewrite lifecycle state, checks that an illegal transition is refused with
// its deterministic primary error and checks that the durable store round trips
// through the installed library.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <hardware_lifecycle/hardware_lifecycle.hpp>

namespace hl = hardware_lifecycle;

namespace {

int fail(const std::string& message) {
  std::fprintf(stderr, "downstream consumer: %s\n", message.c_str());
  return 1;
}

hl::Provenance provenance(const char* plan, const char* scope, hl::EvidenceKind kind, const char* source,
                          std::uint8_t digest_seed) {
  hl::Provenance value;
  value.actor.id = hl::ActorId::parse("downstream-operator").value();
  value.actor.kind = hl::ActorKind::Operator;
  value.plan = hl::PlanId::parse(plan).value();
  value.attempt = hl::AttemptId::parse("a1").value();
  value.authority = hl::parse_authority_mask(scope).value();
  value.policy_generation = hl::PolicyGeneration::first();
  hl::EvidenceRef evidence;
  evidence.kind = kind;
  std::array<std::uint8_t, hl::Digest::kSize> bytes{};
  bytes.fill(digest_seed);
  evidence.digest = hl::Digest::from_bytes(bytes);
  evidence.source = source;
  value.evidence.push_back(evidence);
  return value;
}

}  // namespace

int main() {
  const hl::Version version = hl::library_version();
  std::printf("downstream consumer: library version %s\n", hl::to_string(version).c_str());
  if (hl::compare(version, hl::Version{1, 0, 0}) != 0) {
    return fail("the installed library reports an unexpected version");
  }

  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) / "hl_downstream_consumer_store";
  std::filesystem::remove_all(root, error);

  hl::OpenOptions options;
  options.root = root;
  hl::Result<hl::Runtime> opened = hl::Runtime::open(options);
  if (!opened.has_value()) {
    return fail("open failed: " + hl::describe(opened.error()));
  }
  hl::Runtime runtime = std::move(opened.value());

  hl::CreateRequest create;
  create.plan = hl::PlanId::parse("p-create").value();
  create.attempt = hl::AttemptId::parse("a1").value();
  create.key.asset = hl::AssetId::parse("downstream-node").value();
  create.key.hardware_generation = hl::HardwareGeneration::first();
  create.kind = hl::HardwareKind::Compute;
  create.model = hl::ModelId::parse("model-x").value();
  create.firmware_generation = hl::FirmwareGeneration::first();
  create.initial_state = hl::LifecycleState::Ordered;
  create.provenance = provenance("p-create", "procurement", hl::EvidenceKind::ProcurementRecord, "erp/po-1", 0x11);

  const hl::Result<hl::CreateReceipt> created = runtime.create_object(create);
  if (!created.has_value()) {
    return fail("create failed: " + hl::describe(created.error()));
  }
  std::printf("downstream consumer: registered %s at revision %llu receipt %s\n",
              hl::to_string(created.value().key).c_str(),
              static_cast<unsigned long long>(created.value().revision.value()),
              created.value().receipt_digest.hex().c_str());

  // A health observation is an observation, not an authority: the lifecycle
  // state, the revision and the lifecycle generation must not move.
  hl::HealthObservationRequest observation;
  observation.plan = hl::PlanId::parse("p-health").value();
  observation.attempt = hl::AttemptId::parse("a1").value();
  observation.key = create.key;
  observation.observation.key = create.key;
  observation.observation.health = hl::HealthStatus::Failed;
  observation.observation.readiness = hl::ReadinessStatus::NotReady;
  observation.observation.availability = hl::AvailabilityStatus::Unavailable;
  observation.observation.source = "monitor/downstream";
  std::array<std::uint8_t, hl::Digest::kSize> payload{};
  payload.fill(0x5A);
  observation.observation.evidence_digest = hl::Digest::from_bytes(payload);
  observation.provenance = provenance("p-health", "service", hl::EvidenceKind::HealthEvidence, "monitor/downstream", 0x5A);
  const hl::Result<hl::HealthReceipt> observed = runtime.observe_health(observation);
  if (!observed.has_value()) {
    return fail("observe_health failed: " + hl::describe(observed.error()));
  }

  const hl::Result<hl::ObjectView> view = runtime.inspect(create.key);
  if (!view.has_value()) {
    return fail("inspect failed: " + hl::describe(view.error()));
  }
  if (view.value().object.state != hl::LifecycleState::Ordered) {
    return fail("a health observation rewrote the lifecycle state");
  }
  if (view.value().object.revision != created.value().revision) {
    return fail("a health observation rewrote the revision");
  }
  if (view.value().eligibility != hl::ServiceEligibility::Unknown) {
    return fail("service eligibility was invented from an unknown gate");
  }

  // One legal step, then one illegal step that must be refused deterministically.
  hl::TransitionRequest step;
  step.plan = hl::PlanId::parse("p-staged").value();
  step.attempt = hl::AttemptId::parse("a1").value();
  step.key = create.key;
  step.expected_lifecycle_generation = view.value().object.lifecycle_generation;
  step.expected_revision = view.value().object.revision;
  step.expected_control_epoch = runtime.registry().control_epoch();
  step.expected_state = hl::LifecycleState::Ordered;
  step.target_state = hl::LifecycleState::Staged;
  step.reason = hl::TransitionReason::DeliveryAccepted;
  step.provenance = provenance("p-staged", "logistics", hl::EvidenceKind::DeliveryReceipt, "wms/receipt-1", 0x22);
  const hl::Result<hl::TransitionReceipt> applied = runtime.apply_transition(step);
  if (!applied.has_value()) {
    return fail("transition failed: " + hl::describe(applied.error()));
  }

  hl::TransitionRequest illegal = step;
  illegal.plan = hl::PlanId::parse("p-illegal").value();
  illegal.expected_revision = applied.value().revision_after;
  illegal.expected_lifecycle_generation = applied.value().lifecycle_generation;
  illegal.expected_state = hl::LifecycleState::Staged;
  illegal.target_state = hl::LifecycleState::Active;
  illegal.reason = hl::TransitionReason::CommissioningPassed;
  illegal.provenance = provenance("p-illegal", "service", hl::EvidenceKind::CommissioningReport, "cmdb/report", 0x33);
  const hl::Result<hl::TransitionReceipt> refused = runtime.apply_transition(illegal);
  if (refused.has_value()) {
    return fail("an illegal transition was accepted");
  }
  if (refused.error().code != hl::ErrorCode::IllegalTransition && refused.error().code != hl::ErrorCode::MissingEvidence) {
    return fail("an illegal transition was refused with an unexpected code: " +
                std::string(hl::to_string(refused.error().code)));
  }
  std::printf("downstream consumer: illegal step refused with %s\n",
              std::string(hl::to_string(refused.error().code)).c_str());

  // The durable side is part of the installed artifact too.
  const hl::Result<hl::Digest> verified = runtime.verify();
  if (!verified.has_value()) {
    return fail("verify failed: " + hl::describe(verified.error()));
  }
  const hl::Result<std::string> document = runtime.export_document(hl::ExportOptions());
  if (!document.has_value()) {
    return fail("export failed: " + hl::describe(document.error()));
  }
  std::printf("downstream consumer: state digest %s, exported %llu bytes\n",
              runtime.registry().state_digest().hex().c_str(),
              static_cast<unsigned long long>(document.value().size()));

  std::printf("downstream consumer: ok\n");
  return 0;
}
