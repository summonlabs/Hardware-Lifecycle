#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The durable identity and state of one hardware object. Physical inventory
// discovery, commissioning workflow, firmware policy, maintenance scheduling,
// power and cooling control and asset health diagnosis are owned elsewhere:
// this runtime stores the identity, the lifecycle state and the references that
// those systems supply.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// Coarse physical category. The runtime never derives capability from it:
/// knowing that something is a NetworkSwitch says nothing about what it may do.
enum class HardwareKind : std::uint8_t {
  Unknown = 0,
  Compute,
  Accelerator,
  Storage,
  NetworkSwitch,
  PowerDistribution,
  Cooling,
  RackUnit,
  Other,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(HardwareKind kind) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<HardwareKind> parse_hardware_kind(std::string_view text);

/// Where the object sits. Location is a reference to facility structure owned by
/// other systems; this runtime records and validates it, it does not discover it.
struct Location {
  SiteId site;
  RackId rack;
  SlotId slot;

  [[nodiscard]] bool valid() const noexcept { return site.valid() && rack.valid() && slot.valid(); }

  friend bool operator==(const Location& left, const Location& right) noexcept {
    return left.site == right.site && left.rack == right.rack && left.slot == right.slot;
  }
  friend bool operator!=(const Location& left, const Location& right) noexcept { return !(left == right); }
};

/// Canonical "site/rack/slot" rendering. Slash is not a valid identifier byte, so
/// the rendering is unambiguous.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_string(const Location& location);

[[nodiscard]] HARDWARE_LIFECYCLE_API Result<Location> parse_location(std::string_view text);

/// Session scoped authority over one object.
///
/// Recovered state is not live evidence. Every object that is loaded by a
/// restart is Recovered and cannot be the subject of a lifecycle transition
/// until an explicit attestation re-establishes authority in this process. The
/// value is deliberately not part of the durable state image: authority that
/// survived a restart would be authority that nobody re-established.
enum class AuthorityState : std::uint8_t { Recovered = 0, Live };

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(AuthorityState state) noexcept;

/// The authoritative record of one hardware object.
struct HardwareObject {
  ObjectKey key;
  HardwareKind kind = HardwareKind::Unknown;
  ModelId model;
  FirmwareGeneration firmware_generation;
  LifecycleGeneration lifecycle_generation;
  LifecycleState state = LifecycleState::Ordered;
  Revision revision;
  std::optional<Location> location;
  EligibilityGate eligibility_gate = EligibilityGate::Unknown;
  std::optional<ObjectKey> predecessor;
  std::optional<ObjectKey> successor;
  ReplacementGeneration replacement_generation;
  LogicalTime created_at;
  LogicalTime updated_at;
  std::uint64_t history_entries = 0;

  /// Session scoped. Excluded from the durable state image on purpose.
  AuthorityState authority = AuthorityState::Recovered;
};

}  // namespace hardware_lifecycle
