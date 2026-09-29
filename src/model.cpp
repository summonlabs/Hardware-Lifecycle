// Hardware Lifecycle - hardware kinds, locations and session authority.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The kind of a unit and the location it occupies are references to facts owned
// elsewhere: inventory discovery owns the kind, facility structure owns the
// location. This runtime validates the canonical spelling of both and reports an
// unknown spelling instead of inventing or repairing one.
//
// The canonical location rendering is "site/rack/slot". Slash is not a valid
// identifier byte, so the rendering is unambiguous: it splits back into exactly
// three components and no identifier can contain a separator.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {
namespace {

constexpr std::string_view kKindSpellings[] = {"unknown",     "compute", "accelerator", "storage", "network_switch",
                                               "power_distribution", "cooling", "rack_unit", "other"};
static_assert(std::size(kKindSpellings) == static_cast<std::size_t>(HardwareKind::Other) + 1,
              "one canonical spelling per hardware kind");

constexpr std::string_view kAuthorityStateSpellings[] = {"recovered", "live"};
static_assert(std::size(kAuthorityStateSpellings) == static_cast<std::size_t>(AuthorityState::Live) + 1,
              "one canonical spelling per authority state");

[[nodiscard]] Error unknown_spelling(std::string_view field, std::string_view text) {
  return Error::make(ErrorCode::MalformedRequest, "unknown " + std::string(field) + " spelling")
      .with(std::string(field), std::string(text));
}

[[nodiscard]] Error malformed_location(std::string_view text, std::string message) {
  return Error::make(ErrorCode::MalformedRequest, std::move(message)).with("location", std::string(text));
}

}  // namespace

std::string_view to_string(HardwareKind kind) noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  if (index >= std::size(kKindSpellings)) {
    return "unknown";
  }
  return kKindSpellings[index];
}

Result<HardwareKind> parse_hardware_kind(std::string_view text) {
  for (std::size_t index = 0; index < std::size(kKindSpellings); ++index) {
    if (kKindSpellings[index] == text) {
      return static_cast<HardwareKind>(index);
    }
  }
  return unknown_spelling("hardware kind", text);
}

std::string to_string(const Location& location) {
  const std::string& site = location.site.value();
  const std::string& rack = location.rack.value();
  const std::string& slot = location.slot.value();
  std::string text;
  text.reserve(site.size() + rack.size() + slot.size() + 2);
  text += site;
  text += '/';
  text += rack;
  text += '/';
  text += slot;
  return text;
}

Result<Location> parse_location(std::string_view text) {
  constexpr char kSeparator = '/';
  const std::size_t first = text.find(kSeparator);
  if (first == std::string_view::npos) {
    return malformed_location(text, "location must be rendered as \"site/rack/slot\"");
  }
  const std::size_t second = text.find(kSeparator, first + 1);
  if (second == std::string_view::npos) {
    return malformed_location(text, "location must be rendered as \"site/rack/slot\"");
  }
  if (text.find(kSeparator, second + 1) != std::string_view::npos) {
    return malformed_location(text, "location must carry exactly two '/' separators");
  }

  const std::string_view site_text = text.substr(0, first);
  const std::string_view rack_text = text.substr(first + 1, second - first - 1);
  const std::string_view slot_text = text.substr(second + 1);
  if (site_text.empty() || rack_text.empty() || slot_text.empty()) {
    return malformed_location(text, "location components must not be empty");
  }

  const Result<SiteId> site = SiteId::parse(site_text);
  if (!site.has_value()) {
    return site.error();
  }
  const Result<RackId> rack = RackId::parse(rack_text);
  if (!rack.has_value()) {
    return rack.error();
  }
  const Result<SlotId> slot = SlotId::parse(slot_text);
  if (!slot.has_value()) {
    return slot.error();
  }

  Location location;
  location.site = site.value();
  location.rack = rack.value();
  location.slot = slot.value();
  return location;
}

std::string_view to_string(AuthorityState state) noexcept {
  const std::size_t index = static_cast<std::size_t>(state);
  if (index >= std::size(kAuthorityStateSpellings)) {
    return "unknown";
  }
  return kAuthorityStateSpellings[index];
}

}  // namespace hardware_lifecycle
