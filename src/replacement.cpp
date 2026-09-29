// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Replacement lineage: the links that bind a superseded object to its
// successor, and the forest that holds them. A link is admitted only when it
// keeps the forest a set of chains: one successor per object, one predecessor
// per object, no cycles and no chain deeper than the lineage limit. Existence
// of the linked objects is not checked here; that is the registry's job. Every
// walk in this file is depth bounded, so no structure can make it diverge.

#include "hardware_lifecycle/replacement.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

// ---------------------------------------------------------------------------
// Canonical link encoding
// ---------------------------------------------------------------------------
//
// compute_link_digest() is the only writer of this layout, and it is the only
// function a caller needs in order to bind a link. Every integer is an unsigned
// eight byte big endian value, every text field is a four byte big endian
// length followed by exactly that many bytes, and every digest contributes its
// thirty-two raw bytes:
//
//   zero digest                     32 raw bytes; a domain separator, so a link
//                                   can never share an encoding with a chain
//                                   entry, which starts with a real digest
//   predecessor.asset               text
//   predecessor.hardware_generation u64
//   successor.asset                 text
//   successor.hardware_generation   u64
//   reason                          text, canonical reason spelling
//   provenance.actor.id             text
//   provenance.actor.kind           text, canonical spelling
//   provenance.authority            u64
//   provenance.policy_generation    u64
//   provenance.plan                 text
//   provenance.attempt              text
//   linked_at                       u64
//   commit_sequence                 u64
//   replacement_generation          u64

namespace {

using LinkMap = std::map<ObjectKey, ReplacementRecord>;
using PredecessorMap = std::map<ObjectKey, ObjectKey>;

/// Appends one unsigned integer as eight big endian bytes.
void append_u64_be(Sha256& hasher, std::uint64_t value) {
  std::array<std::uint8_t, 8> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::size_t shift = 8u * (encoded.size() - 1u - index);
    encoded[index] = static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
  hasher.update(encoded.data(), encoded.size());
}

/// Appends one unsigned integer as four big endian bytes.
void append_u32_be(Sha256& hasher, std::uint32_t value) {
  std::array<std::uint8_t, 4> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::size_t shift = 8u * (encoded.size() - 1u - index);
    encoded[index] = static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
  hasher.update(encoded.data(), encoded.size());
}

/// Appends a length prefixed text field. Every text field the library admits is
/// bounded far below 2^32 bytes by limits::kMaxIdentifierBytes and
/// limits::kMaxTextBytes, so the length always fits the four byte prefix
/// exactly; nothing is truncated. An empty field contributes only its length.
void append_text(Sha256& hasher, std::string_view text) {
  append_u32_be(hasher, static_cast<std::uint32_t>(text.size()));
  if (!text.empty()) {
    hasher.update(text);
  }
}

/// Appends the raw bytes of a digest.
void append_digest(Sha256& hasher, const Digest& digest) {
  hasher.update(digest.bytes().data(), Digest::kSize);
}

/// Walks the successor links forward from start, collecting each successor in
/// link order. Returns true when the walk reached the end of its chain, and
/// false when the bound was consumed while a further link still exists: a walk
/// never runs past the lineage limit, and a structure that would make it do so
/// is reported by the caller instead of being followed or truncated.
bool collect_forward(const LinkMap& links, const ObjectKey& start, std::size_t bound,
                     std::vector<ObjectKey>& successors) {
  ObjectKey current = start;
  for (std::size_t step = 0; step < bound; ++step) {
    const auto link = links.find(current);
    if (link == links.end()) {
      return true;
    }
    successors.push_back(link->second.successor);
    current = link->second.successor;
  }
  return links.find(current) == links.end();
}

/// Walks the predecessor links backward from start, collecting each predecessor
/// nearest first. Same contract as collect_forward.
bool collect_backward(const PredecessorMap& predecessors, const ObjectKey& start, std::size_t bound,
                      std::vector<ObjectKey>& ancestors) {
  ObjectKey current = start;
  for (std::size_t step = 0; step < bound; ++step) {
    const auto link = predecessors.find(current);
    if (link == predecessors.end()) {
      return true;
    }
    ancestors.push_back(link->second);
    current = link->second;
  }
  return predecessors.find(current) == predecessors.end();
}

/// Number of links from start to the end of its chain, at most the bound.
std::size_t forward_link_count(const LinkMap& links, const ObjectKey& start) {
  std::size_t count = 0;
  ObjectKey current = start;
  while (count < limits::kMaxLineageDepth) {
    const auto link = links.find(current);
    if (link == links.end()) {
      break;
    }
    ++count;
    current = link->second.successor;
  }
  return count;
}

/// Number of links from start back to the beginning of its chain, at most the
/// bound.
std::size_t backward_link_count(const PredecessorMap& predecessors, const ObjectKey& start) {
  std::size_t count = 0;
  ObjectKey current = start;
  while (count < limits::kMaxLineageDepth) {
    const auto link = predecessors.find(current);
    if (link == predecessors.end()) {
      break;
    }
    ++count;
    current = link->second;
  }
  return count;
}

/// True when target is reachable by following successor links from start. The
/// walk is bounded, so a malformed structure cannot make this diverge.
bool reaches_forward(const LinkMap& links, const ObjectKey& start, const ObjectKey& target) {
  ObjectKey current = start;
  for (std::size_t step = 0; step < limits::kMaxLineageDepth; ++step) {
    const auto link = links.find(current);
    if (link == links.end()) {
      return false;
    }
    if (link->second.successor == target) {
      return true;
    }
    current = link->second.successor;
  }
  return false;
}

}  // namespace

Digest compute_link_digest(const ReplacementRecord& record) {
  Sha256 hasher;
  append_digest(hasher, Digest());
  append_text(hasher, record.predecessor.asset.value());
  append_u64_be(hasher, record.predecessor.hardware_generation.value());
  append_text(hasher, record.successor.asset.value());
  append_u64_be(hasher, record.successor.hardware_generation.value());
  append_text(hasher, to_string(record.reason));
  append_text(hasher, record.provenance.actor.id.value());
  append_text(hasher, to_string(record.provenance.actor.kind));
  append_u64_be(hasher, static_cast<std::uint64_t>(record.provenance.authority.bits()));
  append_u64_be(hasher, record.provenance.policy_generation.value());
  append_text(hasher, record.provenance.plan.value());
  append_text(hasher, record.provenance.attempt.value());
  append_u64_be(hasher, record.linked_at.value());
  append_u64_be(hasher, record.commit_sequence.value());
  append_u64_be(hasher, record.replacement_generation.value());
  return hasher.finish();
}

Result<void> validate_replacement_pair(const ObjectKey& predecessor, const ObjectKey& successor) {
  // Structural validity is settled before identity: an unset key is not an
  // identity that may be compared, linked or stored.
  if (!predecessor.asset.valid() || !predecessor.hardware_generation.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "replacement predecessor is not a valid object key")
        .with("predecessor", to_string(predecessor))
        .with("asset", predecessor.asset.value())
        .with("hardware_generation", std::to_string(predecessor.hardware_generation.value()));
  }
  if (!successor.asset.valid() || !successor.hardware_generation.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "replacement successor is not a valid object key")
        .with("successor", to_string(successor))
        .with("asset", successor.asset.value())
        .with("hardware_generation", std::to_string(successor.hardware_generation.value()));
  }
  if (predecessor == successor) {
    return Error::make(ErrorCode::SelfReplacement, "predecessor and successor are the same object")
        .with("key", to_string(predecessor));
  }
  return ok();
}

Result<void> LineageGraph::add(const ReplacementRecord& record) {
  // Checks run in a fixed order so that a link violating several rules always
  // reports the same primary error.
  if (record.predecessor == record.successor) {
    return Error::make(ErrorCode::SelfReplacement, "an object cannot be its own successor")
        .with("key", to_string(record.successor));
  }
  const Result<void> pair = validate_replacement_pair(record.predecessor, record.successor);
  if (!pair.has_value()) {
    return pair.error();
  }
  // A stored link is only meaningful when it is bound by a real digest; the zero
  // digest means "not computed" and is never accepted in its place.
  if (record.link_digest.is_zero()) {
    return Error::make(ErrorCode::IntegrityFailure, "replacement link carries no link digest")
        .with("field", "link_digest")
        .with("predecessor", to_string(record.predecessor))
        .with("successor", to_string(record.successor));
  }

  const ReplacementRecord* const existing_successor = successor_of(record.predecessor);

  // Relinking an object to the successor it already has is a duplicate, not a
  // new fact.
  if (existing_successor != nullptr && existing_successor->successor == record.successor) {
    return Error::make(ErrorCode::ObjectExists, "predecessor already has this successor")
        .with("predecessor", to_string(record.predecessor))
        .with("successor", to_string(existing_successor->successor));
  }
  // The successor already follows the predecessor. The link would give the
  // predecessor two paths to the same object, so the structure would stop being
  // a set of chains, and a walk that followed it would never be well defined.
  if (reaches_forward(by_predecessor_, record.predecessor, record.successor)) {
    return Error::make(ErrorCode::LineageCycle,
                       "successor already follows the predecessor in the lineage")
        .with("predecessor", to_string(record.predecessor))
        .with("successor", to_string(record.successor));
  }
  // The predecessor already follows the successor: the link would close a cycle.
  if (reaches_forward(by_predecessor_, record.successor, record.predecessor)) {
    return Error::make(ErrorCode::LineageCycle, "link would close a replacement cycle")
        .with("predecessor", to_string(record.predecessor))
        .with("successor", to_string(record.successor));
  }
  // One successor per object.
  if (existing_successor != nullptr) {
    return Error::make(ErrorCode::ObjectExists, "predecessor already has a successor")
        .with("predecessor", to_string(record.predecessor))
        .with("successor", to_string(existing_successor->successor));
  }
  // One predecessor per object: identities are never merged and a successor
  // never has two origins.
  const auto existing_predecessor = predecessor_of_.find(record.successor);
  if (existing_predecessor != predecessor_of_.end()) {
    return Error::make(ErrorCode::ObjectExists, "successor already has a predecessor")
        .with("successor", to_string(record.successor))
        .with("predecessor", to_string(existing_predecessor->second));
  }

  // The chain the link would create: everything above the predecessor, the link
  // itself, and everything below the successor. A chain deeper than the limit is
  // refused rather than truncated, because a bounded walk could no longer reach
  // the whole of it.
  const std::size_t links_above = backward_link_count(predecessor_of_, record.predecessor);
  const std::size_t links_below = forward_link_count(by_predecessor_, record.successor);
  const Result<void> depth =
      limits::check_limit("lineage depth", links_above + 1u + links_below, limits::kMaxLineageDepth);
  if (!depth.has_value()) {
    Error exceeded = depth.error();
    return exceeded.with("predecessor", to_string(record.predecessor))
        .with("successor", to_string(record.successor));
  }

  // The two maps only mean anything together, so they are updated together.
  if (!by_predecessor_.emplace(record.predecessor, record).second) {
    return Error::make(ErrorCode::ObjectExists, "replacement link already recorded")
        .with("predecessor", to_string(record.predecessor));
  }
  predecessor_of_.emplace(record.successor, record.predecessor);
  return ok();
}

const ReplacementRecord* LineageGraph::successor_of(const ObjectKey& key) const {
  const auto link = by_predecessor_.find(key);
  if (link == by_predecessor_.end()) {
    return nullptr;
  }
  return &link->second;
}

const ReplacementRecord* LineageGraph::predecessor_of(const ObjectKey& key) const {
  const auto origin = predecessor_of_.find(key);
  if (origin == predecessor_of_.end()) {
    return nullptr;
  }
  const auto link = by_predecessor_.find(origin->second);
  if (link == by_predecessor_.end()) {
    return nullptr;
  }
  return &link->second;
}

Result<std::vector<ObjectKey>> LineageGraph::chain_from(const ObjectKey& root) const {
  std::vector<ObjectKey> successors;
  if (!collect_forward(by_predecessor_, root, limits::kMaxLineageDepth, successors)) {
    return Error::make(ErrorCode::LineageCycle, "replacement lineage exceeds the maximum depth")
        .with("root", to_string(root))
        .with("limit", std::to_string(limits::kMaxLineageDepth));
  }
  return successors;
}

Result<std::vector<ObjectKey>> LineageGraph::ancestry_of(const ObjectKey& key) const {
  std::vector<ObjectKey> ancestors;
  if (!collect_backward(predecessor_of_, key, limits::kMaxLineageDepth, ancestors)) {
    return Error::make(ErrorCode::LineageCycle, "replacement ancestry exceeds the maximum depth")
        .with("key", to_string(key))
        .with("limit", std::to_string(limits::kMaxLineageDepth));
  }
  // The walk collects nearest first; the answer starts at the earliest known
  // ancestor and ends at the immediate predecessor.
  std::reverse(ancestors.begin(), ancestors.end());
  return ancestors;
}

}  // namespace hardware_lifecycle
