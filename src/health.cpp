// Hardware Lifecycle - health, readiness, availability, gate and eligibility.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Health, readiness, availability and the explicit eligibility gate are four
// independent facts, and none of them is lifecycle state. This file owns their
// spellings and the two derived projections: service eligibility and observation
// freshness.
//
// Both projections are deliberately conservative. Eligibility is Eligible only
// for a state inside the service scope behind an explicitly Open gate; an unknown
// gate, a state for which eligibility is not defined and a value that is not a
// state at all project to Unknown, never to Eligible. Freshness is Fresh only for
// an observation that exists and does not lie in the future of the authoritative
// logical clock.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {
namespace {

constexpr std::string_view kHealthSpellings[] = {"unknown", "healthy", "impaired", "failed"};
static_assert(std::size(kHealthSpellings) == static_cast<std::size_t>(HealthStatus::Failed) + 1,
              "one canonical spelling per health status");

constexpr std::string_view kReadinessSpellings[] = {"unknown", "ready", "not_ready"};
static_assert(std::size(kReadinessSpellings) == static_cast<std::size_t>(ReadinessStatus::NotReady) + 1,
              "one canonical spelling per readiness status");

constexpr std::string_view kAvailabilitySpellings[] = {"unknown", "available", "unavailable"};
static_assert(std::size(kAvailabilitySpellings) == static_cast<std::size_t>(AvailabilityStatus::Unavailable) + 1,
              "one canonical spelling per availability status");

constexpr std::string_view kGateSpellings[] = {"unknown", "open", "closed"};
static_assert(std::size(kGateSpellings) == static_cast<std::size_t>(EligibilityGate::Closed) + 1,
              "one canonical spelling per eligibility gate");

constexpr std::string_view kEligibilitySpellings[] = {"unknown", "eligible", "ineligible"};
static_assert(std::size(kEligibilitySpellings) == static_cast<std::size_t>(ServiceEligibility::Ineligible) + 1,
              "one canonical spelling per service eligibility");

constexpr std::string_view kFreshnessSpellings[] = {"unknown", "fresh", "stale"};
static_assert(std::size(kFreshnessSpellings) == static_cast<std::size_t>(Freshness::Stale) + 1,
              "one canonical spelling per freshness");

template <std::size_t N>
[[nodiscard]] Result<std::size_t> find_spelling(const std::string_view (&spellings)[N], std::string_view field,
                                                std::string_view text) {
  for (std::size_t index = 0; index < N; ++index) {
    if (spellings[index] == text) {
      return index;
    }
  }
  return Error::make(ErrorCode::MalformedRequest, "unknown " + std::string(field) + " spelling")
      .with(std::string(field), std::string(text));
}

template <std::size_t N>
[[nodiscard]] std::string_view spelling_at(const std::string_view (&spellings)[N], std::size_t index) noexcept {
  if (index >= N) {
    return "unknown";
  }
  return spellings[index];
}

}  // namespace

std::string_view to_string(HealthStatus status) noexcept {
  return spelling_at(kHealthSpellings, static_cast<std::size_t>(status));
}

std::string_view to_string(ReadinessStatus status) noexcept {
  return spelling_at(kReadinessSpellings, static_cast<std::size_t>(status));
}

std::string_view to_string(AvailabilityStatus status) noexcept {
  return spelling_at(kAvailabilitySpellings, static_cast<std::size_t>(status));
}

std::string_view to_string(EligibilityGate gate) noexcept {
  return spelling_at(kGateSpellings, static_cast<std::size_t>(gate));
}

std::string_view to_string(ServiceEligibility eligibility) noexcept {
  return spelling_at(kEligibilitySpellings, static_cast<std::size_t>(eligibility));
}

std::string_view to_string(Freshness freshness) noexcept {
  return spelling_at(kFreshnessSpellings, static_cast<std::size_t>(freshness));
}

Result<HealthStatus> parse_health_status(std::string_view text) {
  const Result<std::size_t> index = find_spelling(kHealthSpellings, "health status", text);
  if (!index.has_value()) {
    return index.error();
  }
  return static_cast<HealthStatus>(index.value());
}

Result<ReadinessStatus> parse_readiness_status(std::string_view text) {
  const Result<std::size_t> index = find_spelling(kReadinessSpellings, "readiness status", text);
  if (!index.has_value()) {
    return index.error();
  }
  return static_cast<ReadinessStatus>(index.value());
}

Result<AvailabilityStatus> parse_availability_status(std::string_view text) {
  const Result<std::size_t> index = find_spelling(kAvailabilitySpellings, "availability status", text);
  if (!index.has_value()) {
    return index.error();
  }
  return static_cast<AvailabilityStatus>(index.value());
}

Result<EligibilityGate> parse_eligibility_gate(std::string_view text) {
  const Result<std::size_t> index = find_spelling(kGateSpellings, "eligibility gate", text);
  if (!index.has_value()) {
    return index.error();
  }
  return static_cast<EligibilityGate>(index.value());
}

ServiceEligibility service_eligibility(LifecycleState state, EligibilityGate gate) noexcept {
  if (gate != EligibilityGate::Open && gate != EligibilityGate::Closed) {
    // An unknown gate, or a value that is not a gate at all, permits nothing.
    return ServiceEligibility::Unknown;
  }
  switch (state) {
    case LifecycleState::Ordered:
      // Nothing physical exists yet, so service eligibility is not defined: it is
      // neither eligible nor ineligible.
      return ServiceEligibility::Unknown;
    case LifecycleState::Active:
    case LifecycleState::Degraded:
      return gate == EligibilityGate::Open ? ServiceEligibility::Eligible : ServiceEligibility::Ineligible;
    case LifecycleState::Staged:
    case LifecycleState::Installed:
    case LifecycleState::Commissioning:
    case LifecycleState::Maintenance:
    case LifecycleState::Quarantined:
    case LifecycleState::Retiring:
    case LifecycleState::Retired:
    case LifecycleState::Replaced:
    case LifecycleState::Removed:
      return ServiceEligibility::Ineligible;
  }
  // Not a declared state: an unknown state propagates as Unknown.
  return ServiceEligibility::Unknown;
}

Freshness assess_freshness(bool present, LogicalTime recorded_at, LogicalTime now,
                           std::uint64_t max_age_records) noexcept {
  if (!present) {
    // No observation has ever been recorded. That is not fresh.
    return Freshness::Unknown;
  }
  if (now.value() < recorded_at.value()) {
    // A backwards clock is not evidence of freshness.
    return Freshness::Unknown;
  }
  return (now.value() - recorded_at.value()) <= max_age_records ? Freshness::Fresh : Freshness::Stale;
}

}  // namespace hardware_lifecycle
