// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Chain construction, verification and replay for the durable lifecycle
// history. The chain digest is the only link between entries: every check here
// recomputes it from the canonical encoding documented below rather than
// trusting a stored field, and no check repairs, reorders, drops or fills in an
// entry. An empty log verifies as the zero digest; every other disagreement is
// reported with the entry index and the field that failed.

#include "hardware_lifecycle/history.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

// ---------------------------------------------------------------------------
// Canonical chain encoding
// ---------------------------------------------------------------------------
//
// compute_entry_chain_digest() is the only writer of this layout. No other
// translation unit re-implements it; callers that need the binding call this
// function. Every integer is an unsigned eight byte big endian value, every
// text field is a four byte big endian length followed by exactly that many
// bytes, and every digest contributes its thirty-two raw bytes:
//
//   previous_chain                32 raw bytes; the all-zero digest opens a chain
//   commit_sequence               u64
//   logical_time                  u64
//   key.asset                     text
//   key.hardware_generation       u64
//   lifecycle_generation          u64
//   revision_before               u64, zero when the revision is not set
//   revision_after                u64
//   from                          text, canonical state spelling
//   to                            text, canonical state spelling
//   reason                        text, canonical reason spelling
//   receipt_digest                32 raw bytes
//   provenance.plan               text
//   provenance.attempt            text
//   provenance.actor.id           text
//   provenance.actor.kind         text, canonical spelling
//   provenance.authority          u64
//   provenance.policy_generation  u64
//   evidence count                u64
//   then for each evidence reference in stored order:
//     kind                        text, canonical spelling
//     digest                      32 raw bytes
//     source                      text
//     observed_sequence           u64
//
// The chain digest of the entry itself is deliberately absent from the
// encoding: it is the output of this layout, never an input to it.

namespace {

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

/// Appends the canonical bytes of one entry, excluding its own chain digest.
void append_entry(Sha256& hasher, const HistoryEntry& entry) {
  append_u64_be(hasher, entry.commit_sequence.value());
  append_u64_be(hasher, entry.logical_time.value());
  append_text(hasher, entry.key.asset.value());
  append_u64_be(hasher, entry.key.hardware_generation.value());
  append_u64_be(hasher, entry.lifecycle_generation.value());
  append_u64_be(hasher, entry.revision_before.value());
  append_u64_be(hasher, entry.revision_after.value());
  append_text(hasher, to_string(entry.from));
  append_text(hasher, to_string(entry.to));
  append_text(hasher, to_string(entry.reason));
  append_digest(hasher, entry.receipt_digest);
  append_text(hasher, entry.provenance.plan.value());
  append_text(hasher, entry.provenance.attempt.value());
  append_text(hasher, entry.provenance.actor.id.value());
  append_text(hasher, to_string(entry.provenance.actor.kind));
  append_u64_be(hasher, static_cast<std::uint64_t>(entry.provenance.authority.bits()));
  append_u64_be(hasher, entry.provenance.policy_generation.value());
  append_u64_be(hasher, static_cast<std::uint64_t>(entry.provenance.evidence.size()));
  for (const EvidenceRef& evidence : entry.provenance.evidence) {
    append_text(hasher, to_string(evidence.kind));
    append_digest(hasher, evidence.digest);
    append_text(hasher, evidence.source);
    append_u64_be(hasher, evidence.observed_sequence.value());
  }
}

/// Builds an integrity error that names the offending entry index and field, so
/// every rejection is locatable without reading the human message.
Error entry_error(ErrorCode code, const std::string& message, std::size_t index, const std::string& field) {
  return Error::make(code, message).with("entry", std::to_string(index)).with("field", field);
}

}  // namespace

Digest compute_entry_chain_digest(const Digest& previous_chain, const HistoryEntry& entry) {
  Sha256 hasher;
  append_digest(hasher, previous_chain);
  append_entry(hasher, entry);
  return hasher.finish();
}

Result<Digest> verify_history_chain(const HistoryLog& log) {
  // The first entry is the registration entry: it creates the object and binds
  // itself to the all-zero digest. A log that starts anywhere else is not a
  // chain, whatever its stored digests claim.
  if (!log.entries.empty() && !log.entries.front().is_registration()) {
    return entry_error(ErrorCode::IntegrityFailure, "history chain does not open with a registration entry", 0,
                       "from")
        .with("from", std::string(to_string(log.entries.front().from)))
        .with("to", std::string(to_string(log.entries.front().to)));
  }

  // The chain opens over the all-zero digest. The stored chain head of the log
  // is a cache of what this function returns and is never an input, so a stale
  // or forged head cannot make a broken chain verify.
  Digest previous_chain;
  for (std::size_t index = 0; index < log.entries.size(); ++index) {
    const HistoryEntry& entry = log.entries[index];

    // Every counter that orders, fences or versions a durable fact must be
    // present. A zero counter is an absent counter, and an absent counter is
    // never replayed as if it were measured.
    if (!entry.commit_sequence.valid()) {
      return entry_error(ErrorCode::IntegrityFailure, "history entry carries no commit sequence", index,
                         "commit_sequence");
    }
    if (!entry.logical_time.valid()) {
      return entry_error(ErrorCode::IntegrityFailure, "history entry carries no logical time", index,
                         "logical_time");
    }
    if (!entry.lifecycle_generation.valid()) {
      return entry_error(ErrorCode::IntegrityFailure, "history entry carries no lifecycle generation", index,
                         "lifecycle_generation");
    }
    if (!entry.revision_after.valid()) {
      return entry_error(ErrorCode::IntegrityFailure, "history entry carries no resulting revision", index,
                         "revision_after");
    }
    // The entry that creates the object has no predecessor revision, and a
    // fabricated one would let a later entry claim continuity it never had.
    if (entry.is_registration() && entry.revision_before.valid()) {
      return entry_error(ErrorCode::IntegrityFailure,
                         "registration entry must not carry a previous revision", index, "revision_before");
    }

    const Digest expected = compute_entry_chain_digest(previous_chain, entry);
    if (entry.chain_digest != expected) {
      return entry_error(ErrorCode::IntegrityFailure, "history chain link does not verify", index, "chain_digest")
          .with("expected", expected.hex())
          .with("recorded", entry.chain_digest.hex());
    }
    previous_chain = expected;
  }

  // An empty log is a chain with no entries: it verifies, and its head is the
  // zero digest that the next entry will be bound to.
  return previous_chain;
}

Result<LifecycleState> replay_history(const HistoryLog& log) {
  const Result<Digest> verified = verify_history_chain(log);
  if (!verified.has_value()) {
    return verified.error();
  }
  if (log.entries.empty()) {
    return Error::make(ErrorCode::IntegrityFailure, "an empty history log proves no state")
        .with("field", "entries");
  }
  if (!log.entries.front().is_registration()) {
    return entry_error(ErrorCode::IntegrityFailure, "history log does not open with a registration entry", 0,
                       "from");
  }

  for (std::size_t index = 1; index < log.entries.size(); ++index) {
    const HistoryEntry& previous = log.entries[index - 1];
    const HistoryEntry& entry = log.entries[index];

    // Continuity: the chain is a single path through the state machine.
    if (entry.from != previous.to) {
      return entry_error(ErrorCode::IntegrityFailure,
                         "history entry does not continue from its predecessor", index, "from")
          .with("from", std::string(to_string(entry.from)))
          .with("previous_to", std::string(to_string(previous.to)));
    }

    // Revisions and lifecycle generations advance by exactly one per entry.
    // Exhaustion is reported rather than wrapped.
    const Result<Revision> next_revision = previous.revision_after.next();
    if (!next_revision.has_value()) {
      Error exhausted = next_revision.error();
      return exhausted.with("entry", std::to_string(index)).with("field", "revision_after");
    }
    if (entry.revision_after != next_revision.value()) {
      return entry_error(ErrorCode::IntegrityFailure, "history revision does not advance by exactly one", index,
                         "revision_after")
          .with("expected", std::to_string(next_revision.value().value()))
          .with("recorded", std::to_string(entry.revision_after.value()));
    }

    // A gate decision carries from equal to to and does not move the lifecycle
    // generation, because the lifecycle state did not change. Every other entry
    // advances it by exactly one. Exhaustion is reported rather than wrapped.
    LifecycleGeneration expected_generation = previous.lifecycle_generation;
    if (entry.changes_state()) {
      const Result<LifecycleGeneration> next_generation = previous.lifecycle_generation.next();
      if (!next_generation.has_value()) {
        Error exhausted = next_generation.error();
        return exhausted.with("entry", std::to_string(index)).with("field", "lifecycle_generation");
      }
      expected_generation = next_generation.value();
    }
    if (entry.lifecycle_generation != expected_generation) {
      return entry_error(ErrorCode::IntegrityFailure,
                         "history lifecycle generation does not follow the entry's own kind", index,
                         "lifecycle_generation")
          .with("expected", std::to_string(expected_generation.value()))
          .with("recorded", std::to_string(entry.lifecycle_generation.value()));
    }

    // Commit sequences and logical times order the durable log strictly.
    if (!(previous.commit_sequence < entry.commit_sequence)) {
      return entry_error(ErrorCode::IntegrityFailure, "history commit sequence is not strictly increasing",
                         index, "commit_sequence")
          .with("previous", std::to_string(previous.commit_sequence.value()))
          .with("recorded", std::to_string(entry.commit_sequence.value()));
    }
    if (!(previous.logical_time < entry.logical_time)) {
      return entry_error(ErrorCode::IntegrityFailure, "history logical time is not strictly increasing", index,
                         "logical_time")
          .with("previous", std::to_string(previous.logical_time.value()))
          .with("recorded", std::to_string(entry.logical_time.value()));
    }
  }

  return log.entries.back().to;
}

Result<void> verify_history_against_object(const HistoryLog& log, const HardwareObject& object) {
  // Replay subsumes chain verification, and it proves that the log has at least
  // one entry before the last entry is read below.
  const Result<LifecycleState> replayed = replay_history(log);
  if (!replayed.has_value()) {
    return replayed.error();
  }
  if (replayed.value() != object.state) {
    return Error::make(ErrorCode::StateMismatch, "replayed history does not land on the object state")
        .with("replayed", std::string(to_string(replayed.value())))
        .with("state", std::string(to_string(object.state)));
  }

  const HistoryEntry& last = log.entries.back();
  if (last.revision_after != object.revision) {
    return Error::make(ErrorCode::IntegrityFailure, "history chain does not end at the object revision")
        .with("field", "revision")
        .with("history", std::to_string(last.revision_after.value()))
        .with("object", std::to_string(object.revision.value()));
  }
  if (last.lifecycle_generation != object.lifecycle_generation) {
    return Error::make(ErrorCode::IntegrityFailure,
                       "history chain does not end at the object lifecycle generation")
        .with("field", "lifecycle_generation")
        .with("history", std::to_string(last.lifecycle_generation.value()))
        .with("object", std::to_string(object.lifecycle_generation.value()));
  }
  if (last.key != object.key) {
    return Error::make(ErrorCode::IntegrityFailure, "history chain belongs to a different object")
        .with("field", "key")
        .with("history", to_string(last.key))
        .with("object", to_string(object.key));
  }
  if (static_cast<std::uint64_t>(log.size()) != object.history_entries) {
    return Error::make(ErrorCode::IntegrityFailure, "history chain length does not match the object entry count")
        .with("field", "history_entries")
        .with("history", std::to_string(static_cast<std::uint64_t>(log.size())))
        .with("object", std::to_string(object.history_entries));
  }

  return ok();
}

}  // namespace hardware_lifecycle
