// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rendering and strict parsing of the canonical object key. The key binds an
// asset to the hardware generation it was created under, so a key from before a
// replacement never addresses the hardware installed after it.

#include "hardware_lifecycle/ids.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

std::string to_string(const ObjectKey& key) {
  // Rendering is total and faithful: it shows whatever the key holds. Callers
  // validate the key before relying on it.
  std::string rendered = key.asset.value();
  rendered.push_back('@');
  rendered.append(std::to_string(key.hardware_generation.value()));
  return rendered;
}

Result<ObjectKey> parse_object_key(std::string_view text) {
  const std::size_t at = text.find('@');
  if (at == std::string_view::npos) {
    return Error::make(ErrorCode::MalformedRequest, "object key must be asset@generation")
        .with("object key", std::string(text));
  }
  if (text.find('@', at + 1) != std::string_view::npos) {
    return Error::make(ErrorCode::MalformedRequest, "object key must contain exactly one '@'")
        .with("object key", std::string(text));
  }

  // Both halves are validated by their own strong type: the asset half by the
  // identifier shape, the generation half by the counter parse, which rejects
  // zero, leading zeroes, signs and overflow.
  const std::string_view asset_text = text.substr(0, at);
  const std::string_view generation_text = text.substr(at + 1);
  const Result<AssetId> asset = AssetId::parse(asset_text);
  if (!asset.has_value()) {
    return asset.error();
  }
  const Result<HardwareGeneration> generation = HardwareGeneration::parse(generation_text);
  if (!generation.has_value()) {
    return generation.error();
  }
  return ObjectKey{asset.value(), generation.value()};
}

}  // namespace hardware_lifecycle
