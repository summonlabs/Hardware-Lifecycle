// Hardware Lifecycle - actors, authority scopes, evidence and wall clock.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Provenance is the whole justification of one externally meaningful mutation:
// who asked, under which granted authority, against which policy generation,
// backed by which evidence, at which authoritative logical time. Wall clock time
// is carried for humans and is never used for ordering or validation.
//
// Nothing here interprets an evidence artifact: the runtime binds the digest the
// caller supplied. Structural validation is deterministic. Every violation is
// collected and the one with the lowest validation_rank() is reported, so the
// primary error of a request never depends on the order in which the checks ran,
// on map ordering or on thread scheduling.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle {
namespace {

// Canonical spellings, indexed by the enumeration value. The static assert on
// every table turns a missing entry into a compile error instead of a silent gap.
constexpr std::string_view kActorKindSpellings[] = {
    "unknown",         "operator",    "automation", "procurement", "logistics",   "installation", "commissioning",
    "maintenance",     "integrity",   "retirement", "decommissioning", "replacement", "recovery"};
static_assert(std::size(kActorKindSpellings) == static_cast<std::size_t>(ActorKind::Recovery) + 1,
              "one canonical spelling per actor kind");

constexpr std::string_view kAuthorityScopeSpellings[] = {
    "procurement",        "logistics",        "installation", "commissioning", "service",      "maintenance",
    "integrity",          "retirement",       "decommissioning", "replacement", "recovery"};
static_assert(std::size(kAuthorityScopeSpellings) == kAuthorityScopeCount,
              "one canonical spelling per authority scope");

constexpr std::string_view kEvidenceKindSpellings[] = {
    "unknown",
    "procurement_record",
    "cancellation_record",
    "delivery_receipt",
    "storage_record",
    "installation_record",
    "location_record",
    "commissioning_report",
    "service_record",
    "health_evidence",
    "maintenance_record",
    "integrity_report",
    "drain_record",
    "retirement_approval",
    "decommissioning_record",
    "replacement_authorization",
    "successor_record",
    "removal_record",
    "recovery_attestation"};
static_assert(std::size(kEvidenceKindSpellings) == kEvidenceKindCount,
              "one canonical spelling per evidence kind, and one enumerator for each");

// The number of evidence kinds that carry a bit in an EvidenceMask.
constexpr std::size_t kEvidenceKindLimit = std::size(kEvidenceKindSpellings);

[[nodiscard]] Error unknown_spelling(std::string_view field, std::string_view text) {
  return Error::make(ErrorCode::MalformedRequest, "unknown " + std::string(field) + " spelling")
      .with(std::string(field), std::string(text));
}

// Renders a value that may be absent. The marker is a reporting convention only;
// nothing is stored in its place.
[[nodiscard]] std::string text_or_empty_marker(std::string_view text) {
  return text.empty() ? std::string("<empty>") : std::string(text);
}

[[nodiscard]] bool is_ascii_digit(char byte) noexcept { return byte >= '0' && byte <= '9'; }

[[nodiscard]] unsigned digit_value(char byte) noexcept { return static_cast<unsigned>(byte - '0'); }

[[nodiscard]] bool is_leap_year(unsigned year) noexcept {
  return (year % 4u == 0u) && ((year % 100u != 0u) || (year % 400u == 0u));
}

[[nodiscard]] unsigned days_in_month(unsigned year, unsigned month) noexcept {
  switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      return 31;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    case 2:
      return is_leap_year(year) ? 29u : 28u;
    default:
      return 0;
  }
}

}  // namespace

std::string_view to_string(ActorKind kind) noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  if (index >= std::size(kActorKindSpellings)) {
    return "unknown";
  }
  return kActorKindSpellings[index];
}

Result<ActorKind> parse_actor_kind(std::string_view text) {
  for (std::size_t index = 0; index < std::size(kActorKindSpellings); ++index) {
    if (kActorKindSpellings[index] == text) {
      return static_cast<ActorKind>(index);
    }
  }
  return unknown_spelling("actor kind", text);
}

std::string to_string(AuthorityMask mask) {
  if (mask.empty()) {
    return "none";
  }
  std::string text;
  for (std::size_t index = 0; index < kAuthorityScopeCount; ++index) {
    const AuthorityScope scope = static_cast<AuthorityScope>(index);
    if (!mask.intersects(authority_bit(scope))) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += kAuthorityScopeSpellings[index];
  }
  return text;
}

Result<AuthorityScope> parse_authority_scope(std::string_view text) {
  for (std::size_t index = 0; index < kAuthorityScopeCount; ++index) {
    if (kAuthorityScopeSpellings[index] == text) {
      return static_cast<AuthorityScope>(index);
    }
  }
  return unknown_spelling("authority scope", text);
}

Result<AuthorityMask> parse_authority_mask(std::string_view text) {
  if (text.empty()) {
    // An empty mask is the absence of authority, and absence is never spelled.
    return Error::make(ErrorCode::MalformedRequest, "authority mask must not be empty")
        .with("authority mask", std::string(text));
  }

  AuthorityMask mask;
  std::size_t begin = 0;
  for (;;) {
    const std::size_t comma = text.find(',', begin);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view element = trim_ascii(text.substr(begin, end - begin));
    if (element.empty()) {
      return Error::make(ErrorCode::MalformedRequest, "authority mask carries an empty scope")
          .with("authority mask", std::string(text));
    }
    if (ascii_iequals(element, "none")) {
      // "none" is a rendering of the empty mask, never an accepted input.
      return Error::make(ErrorCode::MalformedRequest, "authority mask must not spell \"none\"")
          .with("authority mask", std::string(text));
    }
    const Result<AuthorityScope> scope = parse_authority_scope(element);
    if (!scope.has_value()) {
      return Error::make(ErrorCode::MalformedRequest, "unknown authority scope spelling")
          .with("authority mask", std::string(text))
          .with("authority scope", std::string(element));
    }
    const AuthorityMask bit = authority_bit(scope.value());
    if (mask.contains(bit)) {
      return Error::make(ErrorCode::MalformedRequest, "authority mask repeats a scope")
          .with("authority mask", std::string(text))
          .with("authority scope", std::string(element));
    }
    mask |= bit;
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  return mask;
}

std::string_view to_string(EvidenceKind kind) noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  if (index >= kEvidenceKindLimit) {
    return "unknown";
  }
  return kEvidenceKindSpellings[index];
}

Result<EvidenceKind> parse_evidence_kind(std::string_view text) {
  // Index 0 is the unknown kind: it is the absence of a kind and is never
  // accepted as one.
  for (std::size_t index = 1; index < kEvidenceKindLimit; ++index) {
    if (kEvidenceKindSpellings[index] == text) {
      return static_cast<EvidenceKind>(index);
    }
  }
  return unknown_spelling("evidence kind", text);
}

std::string to_string(EvidenceMask mask) {
  if (mask.empty()) {
    return "none";
  }
  std::string text;
  // Index 0 is the unknown kind, which carries no bit and is never rendered.
  for (std::size_t index = 1; index < kEvidenceKindLimit; ++index) {
    const EvidenceKind kind = static_cast<EvidenceKind>(index);
    if (!mask.intersects(evidence_bit(kind))) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += kEvidenceKindSpellings[index];
  }
  return text;
}

Result<WallClock> WallClock::parse(std::string_view text) {
  const auto reject = [&text](std::string message) {
    return Error::make(ErrorCode::MalformedRequest, std::move(message)).with("wall clock", std::string(text));
  };

  // Exactly "YYYY-MM-DDTHH:MM:SS", an optional fraction of one to nine digits and
  // a literal "Z". Twenty bytes is the shortest accepted spelling.
  constexpr std::size_t kBaseBytes = 20;
  constexpr std::size_t kMaxFractionDigits = 9;
  if (text.size() < kBaseBytes) {
    return reject("wall clock must be \"YYYY-MM-DDTHH:MM:SSZ\" with an optional fractional second");
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!is_ascii_digit(text[index])) {
      return reject("wall clock year must be four decimal digits");
    }
  }
  if (text[4] != '-' || text[7] != '-') {
    return reject("wall clock date must separate year, month and day with '-'");
  }
  if (!is_ascii_digit(text[5]) || !is_ascii_digit(text[6])) {
    return reject("wall clock month must be two decimal digits");
  }
  if (!is_ascii_digit(text[8]) || !is_ascii_digit(text[9])) {
    return reject("wall clock day must be two decimal digits");
  }
  if (text[10] != 'T') {
    return reject("wall clock must separate date and time with an uppercase 'T'");
  }
  if (!is_ascii_digit(text[11]) || !is_ascii_digit(text[12])) {
    return reject("wall clock hour must be two decimal digits");
  }
  if (text[13] != ':') {
    return reject("wall clock time must separate hour, minute and second with ':'");
  }
  if (!is_ascii_digit(text[14]) || !is_ascii_digit(text[15])) {
    return reject("wall clock minute must be two decimal digits");
  }
  if (text[16] != ':') {
    return reject("wall clock time must separate hour, minute and second with ':'");
  }
  if (!is_ascii_digit(text[17]) || !is_ascii_digit(text[18])) {
    return reject("wall clock second must be two decimal digits");
  }

  if (text[19] == 'Z') {
    if (text.size() != kBaseBytes) {
      return reject("wall clock carries bytes after the 'Z' zone designator");
    }
  } else if (text[19] == '.') {
    if (text.size() < kBaseBytes + 2) {
      return reject("wall clock fraction must carry at least one digit");
    }
    if (text.size() - (kBaseBytes + 1) > kMaxFractionDigits) {
      return reject("wall clock fraction must carry at most nine digits");
    }
    for (std::size_t index = kBaseBytes + 1; index + 1 < text.size(); ++index) {
      if (!is_ascii_digit(text[index])) {
        return reject("wall clock fraction must be decimal digits");
      }
    }
    if (text.back() != 'Z') {
      return reject("wall clock must end with the 'Z' zone designator");
    }
  } else {
    return reject("wall clock must end with the 'Z' zone designator");
  }

  const unsigned year = digit_value(text[0]) * 1000u + digit_value(text[1]) * 100u + digit_value(text[2]) * 10u +
                        digit_value(text[3]);
  const unsigned month = digit_value(text[5]) * 10u + digit_value(text[6]);
  const unsigned day = digit_value(text[8]) * 10u + digit_value(text[9]);
  const unsigned hour = digit_value(text[11]) * 10u + digit_value(text[12]);
  const unsigned minute = digit_value(text[14]) * 10u + digit_value(text[15]);
  const unsigned second = digit_value(text[17]) * 10u + digit_value(text[18]);

  if (month < 1u || month > 12u) {
    return reject("wall clock month must be between 01 and 12");
  }
  if (day < 1u || day > days_in_month(year, month)) {
    return reject("wall clock day is outside the month");
  }
  if (hour > 23u) {
    return reject("wall clock hour must be between 00 and 23");
  }
  if (minute > 59u) {
    return reject("wall clock minute must be between 00 and 59");
  }
  // 60 is the RFC 3339 leap second spelling. It is accepted as written and is
  // never rolled over into the next minute.
  if (second > 60u) {
    return reject("wall clock second must be between 00 and 60");
  }

  // The caller's exact text is stored, so a document round trips byte for byte.
  return WallClock(std::string(text));
}

bool has_authority(const Provenance& provenance, AuthorityMask required) noexcept {
  // The transition table lists alternative scopes: holding any one of the
  // required scopes is enough, and an empty requirement grants nothing.
  return required.intersects(provenance.authority);
}

EvidenceMask evidence_mask(const Provenance& provenance) noexcept {
  EvidenceMask mask;
  for (const EvidenceRef& reference : provenance.evidence) {
    const std::size_t kind = static_cast<std::size_t>(reference.kind);
    if (kind == 0 || kind >= kEvidenceKindLimit) {
      // The unknown kind contributes nothing, and a value outside the
      // enumeration carries no bit at all; validate_provenance rejects both.
      continue;
    }
    mask |= evidence_bit(reference.kind);
  }
  return mask;
}

bool has_evidence(const Provenance& provenance, EvidenceMask required) noexcept {
  if (required.empty()) {
    // The transition table uses an empty evidence mask for "at least one evidence
    // reference of any kind": every externally meaningful mutation binds to
    // evidence it was planned against.
    return !provenance.evidence.empty();
  }
  return evidence_mask(provenance).contains(required);
}

Result<void> validate_provenance(const Provenance& provenance) {
  Error primary;
  bool has_primary = false;
  // Keeps the lowest ranking violation. Equal ranks keep the first one found, so
  // the reported code is a function of the input alone.
  const auto record = [&primary, &has_primary](Error error) {
    if (!has_primary || validation_rank(error.code) < validation_rank(primary.code)) {
      primary = std::move(error);
      has_primary = true;
    }
  };

  if (!provenance.actor.id.valid()) {
    record(Error::make(ErrorCode::MalformedRequest, "actor id is not a valid identifier")
               .with("actor id", text_or_empty_marker(provenance.actor.id.value())));
  }
  const std::size_t actor_kind = static_cast<std::size_t>(provenance.actor.kind);
  if (actor_kind == 0) {
    record(Error::make(ErrorCode::MalformedRequest, "actor kind must not be unknown").with("actor kind", "unknown"));
  } else if (actor_kind >= std::size(kActorKindSpellings)) {
    record(Error::make(ErrorCode::MalformedRequest, "actor kind is not a declared kind")
               .with("actor kind", std::to_string(actor_kind)));
  }

  if (!provenance.plan.valid()) {
    record(Error::make(ErrorCode::MalformedRequest, "plan id is not a valid identifier")
               .with("plan id", text_or_empty_marker(provenance.plan.value())));
  }
  if (!provenance.attempt.valid()) {
    record(Error::make(ErrorCode::MalformedRequest, "attempt id is not a valid identifier")
               .with("attempt id", text_or_empty_marker(provenance.attempt.value())));
  }

  if (!authority_all().contains(provenance.authority)) {
    record(Error::make(ErrorCode::MalformedRequest, "authority mask carries scopes outside the declared set")
               .with("authority mask", std::to_string(provenance.authority.bits())));
  }

  if (provenance.evidence.size() > limits::kMaxEvidencePerRequest) {
    record(Error::make(ErrorCode::LimitExceeded, "evidence list exceeds the accepted number of references")
               .with("evidence count", std::to_string(provenance.evidence.size()))
               .with("maximum", std::to_string(limits::kMaxEvidencePerRequest)));
  }

  for (std::size_t index = 0; index < provenance.evidence.size(); ++index) {
    const EvidenceRef& reference = provenance.evidence[index];
    const std::string position = "#" + std::to_string(index);

    const std::size_t kind = static_cast<std::size_t>(reference.kind);
    if (kind == 0) {
      record(Error::make(ErrorCode::MalformedRequest, "evidence kind must not be unknown")
                 .with("evidence kind", position));
    } else if (kind >= kEvidenceKindLimit) {
      record(Error::make(ErrorCode::MalformedRequest, "evidence kind is not a declared kind")
                 .with("evidence kind", position + " is " + std::to_string(kind)));
    }

    if (reference.digest.is_zero()) {
      record(Error::make(ErrorCode::MalformedRequest, "evidence digest must not be all zero")
                 .with("evidence digest", position));
    }
    if (!is_valid_evidence_source(reference.source)) {
      record(Error::make(ErrorCode::MalformedRequest, "evidence source is not a valid source")
                 .with("evidence source", position + ": " + reference.source));
    }

    // The observed sequence may be zero, meaning it was not supplied. The pair of
    // kind and source must still be unique: two references that claim the same
    // artifact from the same place disagree about something.
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      const EvidenceRef& previous = provenance.evidence[earlier];
      if (previous.kind == reference.kind && previous.source == reference.source) {
        record(Error::make(ErrorCode::MalformedRequest, "evidence repeats a kind and source pair")
                   .with("evidence", position + " repeats #" + std::to_string(earlier)));
        break;
      }
    }
  }

  if (!has_primary) {
    return ok();
  }
  return primary;
}

}  // namespace hardware_lifecycle
