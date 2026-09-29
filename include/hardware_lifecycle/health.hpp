#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Health, readiness, availability and service eligibility are modelled here and
// nowhere else. They are separate from lifecycle state: ingesting a health
// observation never changes a lifecycle state, a revision or a generation. The
// only way a lifecycle state changes is a transition request that satisfies the
// transition table.

#include <cstdint>
#include <string>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// Health of the physical object. Unknown is the absence of a measurement, and
/// it is never reported as healthy.
enum class HealthStatus : std::uint8_t { Unknown = 0, Healthy, Impaired, Failed };

/// Whether the object would accept new work. Unknown is not "ready".
enum class ReadinessStatus : std::uint8_t { Unknown = 0, Ready, NotReady };

/// Whether the object is currently reachable and usable. Unknown is not
/// "available".
enum class AvailabilityStatus : std::uint8_t { Unknown = 0, Available, Unavailable };

/// An explicit operator or policy gate. It is separate from health and separate
/// from lifecycle: three independent facts, three independent authorities.
enum class EligibilityGate : std::uint8_t { Unknown = 0, Open, Closed };

/// The derived service eligibility of one object. This is a projection of the
/// lifecycle state and the gate, never an input to either.
enum class ServiceEligibility : std::uint8_t { Unknown = 0, Eligible, Ineligible };

/// How old an observation is relative to the authoritative logical clock.
enum class Freshness : std::uint8_t { Unknown = 0, Fresh, Stale };

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(HealthStatus status) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(ReadinessStatus status) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(AvailabilityStatus status) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(EligibilityGate gate) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(ServiceEligibility eligibility) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(Freshness freshness) noexcept;

[[nodiscard]] HARDWARE_LIFECYCLE_API Result<HealthStatus> parse_health_status(std::string_view text);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ReadinessStatus> parse_readiness_status(std::string_view text);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<AvailabilityStatus> parse_availability_status(std::string_view text);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<EligibilityGate> parse_eligibility_gate(std::string_view text);

/// One health observation as reported by an external monitoring system. This
/// runtime stores it, stamps it with an authoritative observation sequence and
/// logical time, and does nothing else with it.
struct HealthObservation {
  ObjectKey key;
  HealthStatus health = HealthStatus::Unknown;
  ReadinessStatus readiness = ReadinessStatus::Unknown;
  AvailabilityStatus availability = AvailabilityStatus::Unknown;
  Digest evidence_digest;
  std::string source;
  WallClock observed_wall_clock;
};

/// The most recent observation plus the metadata that makes it interpretable.
struct HealthSnapshot {
  /// False means no observation has ever been recorded. That is not healthy.
  bool present = false;
  HealthObservation observation;
  ObservationSequence sequence;
  LogicalTime recorded_at;
  Freshness freshness = Freshness::Unknown;
};

/// Eligible only when the gate is Open and the lifecycle state is inside the
/// service scope. Every unknown input propagates: an unknown gate yields
/// Unknown, an unknown state yields Unknown, and neither is ever Eligible.
[[nodiscard]] HARDWARE_LIFECYCLE_API ServiceEligibility service_eligibility(LifecycleState state,
                                                                           EligibilityGate gate) noexcept;

/// Freshness of an observation relative to the current authoritative logical
/// time. An observation recorded after "now" is Unknown rather than Fresh: a
/// backwards clock is not evidence of freshness.
[[nodiscard]] HARDWARE_LIFECYCLE_API Freshness assess_freshness(bool present, LogicalTime recorded_at,
                                                               LogicalTime now,
                                                               std::uint64_t max_age_records) noexcept;

}  // namespace hardware_lifecycle
