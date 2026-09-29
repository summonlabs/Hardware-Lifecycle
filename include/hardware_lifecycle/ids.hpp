#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Semantically distinct strong types. AssetId, RackId and PlanId are all text
// and all validate identically, but they are different types: passing a RackId
// where an AssetId is required is a compile error, not a runtime bug. The same
// holds for every counter below, so a revision can never be mistaken for a
// generation or an epoch.

#include <compare>
#include <cstdint>
#include <type_traits>
#include <limits>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle {

// ---------------------------------------------------------------------------
// Tag types
// ---------------------------------------------------------------------------

/// Every tag carries the human name used in diagnostics.
#define HARDWARE_LIFECYCLE_TEXT_ID_TAG(tag_name, spelling) \
  struct tag_name {                                        \
    static constexpr std::string_view name = spelling;     \
  }

#define HARDWARE_LIFECYCLE_COUNTER_TAG(tag_name, spelling) \
  struct tag_name {                                        \
    static constexpr std::string_view name = spelling;     \
  }

HARDWARE_LIFECYCLE_TEXT_ID_TAG(AssetIdTag, "asset id");
HARDWARE_LIFECYCLE_TEXT_ID_TAG(RackIdTag, "rack id");
HARDWARE_LIFECYCLE_TEXT_ID_TAG(SiteIdTag, "site id");
HARDWARE_LIFECYCLE_TEXT_ID_TAG(SlotIdTag, "slot id");
HARDWARE_LIFECYCLE_TEXT_ID_TAG(ActorIdTag, "actor id");
HARDWARE_LIFECYCLE_TEXT_ID_TAG(ModelIdTag, "model id");
HARDWARE_LIFECYCLE_TEXT_ID_TAG(PlanIdTag, "plan id");
HARDWARE_LIFECYCLE_TEXT_ID_TAG(AttemptIdTag, "attempt id");

HARDWARE_LIFECYCLE_COUNTER_TAG(LifecycleGenerationTag, "lifecycle generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(HardwareGenerationTag, "hardware generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(FirmwareGenerationTag, "firmware generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(ControlEpochTag, "control epoch");
HARDWARE_LIFECYCLE_COUNTER_TAG(IncarnationIdTag, "incarnation id");
HARDWARE_LIFECYCLE_COUNTER_TAG(RevisionTag, "revision");
HARDWARE_LIFECYCLE_COUNTER_TAG(CommitSequenceTag, "commit sequence");
HARDWARE_LIFECYCLE_COUNTER_TAG(ObservationSequenceTag, "observation sequence");
HARDWARE_LIFECYCLE_COUNTER_TAG(LogicalTimeTag, "logical time");
HARDWARE_LIFECYCLE_COUNTER_TAG(PolicyGenerationTag, "policy generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(DependencyGenerationTag, "dependency generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(CapacityGenerationTag, "capacity generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(TopologyGenerationTag, "topology generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(MaintenanceGenerationTag, "maintenance generation");
HARDWARE_LIFECYCLE_COUNTER_TAG(ReplacementGenerationTag, "replacement generation");

#undef HARDWARE_LIFECYCLE_TEXT_ID_TAG
#undef HARDWARE_LIFECYCLE_COUNTER_TAG

// ---------------------------------------------------------------------------
// Strong textual identifier
// ---------------------------------------------------------------------------

/// A validated, non-empty textual identifier. The default constructed value is
/// the null id: it compares and orders consistently but is not valid, and every
/// entry point rejects it.
template <class Tag>
class TextId {
 public:
  using tag_type = Tag;

  TextId() noexcept = default;

  /// Validates the shape and returns a MalformedRequest error for anything that
  /// is not an identifier.
  [[nodiscard]] static Result<TextId> parse(std::string_view text) {
    if (!is_valid_identifier(text)) {
      return Error::make(ErrorCode::MalformedRequest,
                         "invalid " + std::string(Tag::name) + " spelling")
          .with(std::string(Tag::name), std::string(text));
    }
    return TextId(std::string(text));
  }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }

  [[nodiscard]] bool valid() const noexcept { return !value_.empty(); }

  friend bool operator==(const TextId& left, const TextId& right) noexcept { return left.value_ == right.value_; }
  friend bool operator!=(const TextId& left, const TextId& right) noexcept { return !(left == right); }
  friend bool operator<(const TextId& left, const TextId& right) noexcept { return left.value_ < right.value_; }

 private:
  explicit TextId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

// ---------------------------------------------------------------------------
// Strong counter
// ---------------------------------------------------------------------------

/// A one-based counter. Zero means "not set" and is never accepted where the
/// durable format requires a real counter value: absence is encoded as an
/// omitted field, never as zero.
template <class Tag>
class CounterId {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  constexpr CounterId() noexcept = default;

  /// Explicit, documented constructor for recovered or computed values.
  [[nodiscard]] static constexpr CounterId from_value(value_type value) noexcept { return CounterId(value); }

  [[nodiscard]] static constexpr CounterId first() noexcept { return CounterId(1); }

  [[nodiscard]] constexpr value_type value() const noexcept { return value_; }

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  /// Overflow checked successor. Exhaustion is reported, never wrapped.
  [[nodiscard]] Result<CounterId> next() const {
    if (value_ == (std::numeric_limits<value_type>::max)()) {
      return Error::make(ErrorCode::CounterOverflow, std::string(Tag::name) + " counter exhausted");
    }
    return CounterId(value_ + 1);
  }

  /// Strict decimal parse. Rejects zero, leading zeroes, signs and overflow.
  [[nodiscard]] static Result<CounterId> parse(std::string_view text) {
    const Result<std::uint64_t> parsed = parse_u64_strict(text);
    if (!parsed.has_value()) {
      return Error::make(ErrorCode::MalformedRequest, "invalid " + std::string(Tag::name) + " spelling")
          .with(std::string(Tag::name), std::string(text));
    }
    if (parsed.value() == 0) {
      return Error::make(ErrorCode::MalformedRequest, std::string(Tag::name) + " must be at least 1")
          .with(std::string(Tag::name), std::string(text));
    }
    return CounterId(parsed.value());
  }

  friend constexpr bool operator==(CounterId left, CounterId right) noexcept { return left.value_ == right.value_; }
  friend constexpr bool operator!=(CounterId left, CounterId right) noexcept { return !(left == right); }
  friend constexpr bool operator<(CounterId left, CounterId right) noexcept { return left.value_ < right.value_; }
  friend constexpr bool operator<=(CounterId left, CounterId right) noexcept { return left.value_ <= right.value_; }
  friend constexpr bool operator>(CounterId left, CounterId right) noexcept { return right < left; }
  friend constexpr bool operator>=(CounterId left, CounterId right) noexcept { return right <= left; }

 private:
  explicit constexpr CounterId(value_type value) noexcept : value_(value) {}

  value_type value_ = 0;
};

// ---------------------------------------------------------------------------
// Concrete identities
// ---------------------------------------------------------------------------

using AssetId = TextId<AssetIdTag>;
using RackId = TextId<RackIdTag>;
using SiteId = TextId<SiteIdTag>;
using SlotId = TextId<SlotIdTag>;
using ActorId = TextId<ActorIdTag>;
using ModelId = TextId<ModelIdTag>;
using PlanId = TextId<PlanIdTag>;
using AttemptId = TextId<AttemptIdTag>;

using LifecycleGeneration = CounterId<LifecycleGenerationTag>;
using HardwareGeneration = CounterId<HardwareGenerationTag>;
using FirmwareGeneration = CounterId<FirmwareGenerationTag>;
using ControlEpoch = CounterId<ControlEpochTag>;
using IncarnationId = CounterId<IncarnationIdTag>;
using Revision = CounterId<RevisionTag>;
using CommitSequence = CounterId<CommitSequenceTag>;
using ObservationSequence = CounterId<ObservationSequenceTag>;
using LogicalTime = CounterId<LogicalTimeTag>;
using PolicyGeneration = CounterId<PolicyGenerationTag>;
using DependencyGeneration = CounterId<DependencyGenerationTag>;
using CapacityGeneration = CounterId<CapacityGenerationTag>;
using TopologyGeneration = CounterId<TopologyGenerationTag>;
using MaintenanceGeneration = CounterId<MaintenanceGenerationTag>;
using ReplacementGeneration = CounterId<ReplacementGenerationTag>;

// The distinctness the header claims, checked at compile time.
static_assert(!std::is_same_v<AssetId, RackId>);
static_assert(!std::is_same_v<AssetId, PlanId>);
static_assert(!std::is_same_v<Revision, LifecycleGeneration>);
static_assert(!std::is_same_v<Revision, ControlEpoch>);
static_assert(!std::is_same_v<HardwareGeneration, LifecycleGeneration>);

/// The identity of exactly one hardware object: its asset id together with the
/// hardware generation it was created under. Replacing hardware creates a new
/// object key; it never reuses the predecessor key.
struct ObjectKey {
  AssetId asset;
  HardwareGeneration hardware_generation;

  [[nodiscard]] bool valid() const noexcept { return asset.valid() && hardware_generation.valid(); }

  friend bool operator==(const ObjectKey& left, const ObjectKey& right) noexcept {
    return left.asset == right.asset && left.hardware_generation == right.hardware_generation;
  }
  friend bool operator!=(const ObjectKey& left, const ObjectKey& right) noexcept { return !(left == right); }
  friend bool operator<(const ObjectKey& left, const ObjectKey& right) noexcept {
    if (left.asset != right.asset) {
      return left.asset < right.asset;
    }
    return left.hardware_generation < right.hardware_generation;
  }
  friend bool operator>(const ObjectKey& left, const ObjectKey& right) noexcept { return right < left; }
  friend bool operator<=(const ObjectKey& left, const ObjectKey& right) noexcept { return !(right < left); }
  friend bool operator>=(const ObjectKey& left, const ObjectKey& right) noexcept { return !(left < right); }
};

/// Canonical text rendering: "asset@generation".
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_string(const ObjectKey& key);

/// Strict parse of the canonical rendering.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ObjectKey> parse_object_key(std::string_view text);

}  // namespace hardware_lifecycle
