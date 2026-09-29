// Hardware Lifecycle - unit proofs for the binding primitives and the error
// taxonomy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SHA-256 is authoritative, CRC-32 is framing only, and the error taxonomy is
// the machine readable contract: spellings, retry and durability classes and the
// deterministic validation precedence that decides which violation a request
// reports when it breaks several rules at once.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;

/// One error code together with its stable spelling, in declaration order.
struct CodeSpelling {
  ErrorCode code;
  const char* spelling;
};

const CodeSpelling kCodeSpellings[] = {
    {ErrorCode::Ok, "ok"},
    {ErrorCode::LimitExceeded, "limit_exceeded"},
    {ErrorCode::UnsupportedFormatVersion, "unsupported_format_version"},
    {ErrorCode::MalformedRequest, "malformed_request"},
    {ErrorCode::TrailingBytes, "trailing_bytes"},
    {ErrorCode::IntegrityFailure, "integrity_failure"},
    {ErrorCode::UnknownAsset, "unknown_asset"},
    {ErrorCode::ObjectExists, "object_exists"},
    {ErrorCode::StaleHardwareGeneration, "stale_hardware_generation"},
    {ErrorCode::PlanConflict, "plan_conflict"},
    {ErrorCode::StaleAuthority, "stale_authority"},
    {ErrorCode::StaleLifecycleGeneration, "stale_lifecycle_generation"},
    {ErrorCode::StaleRevision, "stale_revision"},
    {ErrorCode::StateMismatch, "state_mismatch"},
    {ErrorCode::IllegalTransition, "illegal_transition"},
    {ErrorCode::TerminalState, "terminal_state"},
    {ErrorCode::InsufficientAuthority, "insufficient_authority"},
    {ErrorCode::MissingEvidence, "missing_evidence"},
    {ErrorCode::MissingLocation, "missing_location"},
    {ErrorCode::MissingSuccessor, "missing_successor"},
    {ErrorCode::InvalidSuccessor, "invalid_successor"},
    {ErrorCode::SelfReplacement, "self_replacement"},
    {ErrorCode::LineageCycle, "lineage_cycle"},
    {ErrorCode::StoreLocked, "store_locked"},
    {ErrorCode::StoreCorrupt, "store_corrupt"},
    {ErrorCode::StoreUninitialized, "store_uninitialized"},
    {ErrorCode::StoreClosed, "store_closed"},
    {ErrorCode::ReadOnlyAuthority, "read_only_authority"},
    {ErrorCode::ObjectNotFound, "object_not_found"},
    {ErrorCode::IoFailure, "io_failure"},
    {ErrorCode::PermissionDenied, "permission_denied"},
    {ErrorCode::PathTraversal, "path_traversal"},
    {ErrorCode::ReparsePointRejected, "reparse_point_rejected"},
    {ErrorCode::AlternateDataStream, "alternate_data_stream"},
    {ErrorCode::UnsupportedOperation, "unsupported_operation"},
    {ErrorCode::CounterOverflow, "counter_overflow"},
    {ErrorCode::Internal, "internal"},
};

/// The codes request validation can produce, in the order validation_rank()
/// declares them: the ranked prefix of the enumeration.
const ErrorCode kRankedCodes[] = {
    ErrorCode::LimitExceeded,
    ErrorCode::UnsupportedFormatVersion,
    ErrorCode::MalformedRequest,
    ErrorCode::TrailingBytes,
    ErrorCode::IntegrityFailure,
    ErrorCode::UnknownAsset,
    ErrorCode::ObjectExists,
    ErrorCode::StaleHardwareGeneration,
    ErrorCode::PlanConflict,
    ErrorCode::StaleAuthority,
    ErrorCode::StaleLifecycleGeneration,
    ErrorCode::StaleRevision,
    ErrorCode::StateMismatch,
    ErrorCode::IllegalTransition,
    ErrorCode::TerminalState,
    ErrorCode::InsufficientAuthority,
    ErrorCode::MissingEvidence,
    ErrorCode::MissingLocation,
    ErrorCode::MissingSuccessor,
    ErrorCode::InvalidSuccessor,
    ErrorCode::SelfReplacement,
    ErrorCode::LineageCycle,
};

const ErrorCode kRetryableCodes[] = {ErrorCode::StaleAuthority,         ErrorCode::StaleHardwareGeneration,
                                     ErrorCode::StaleLifecycleGeneration, ErrorCode::StaleRevision,
                                     ErrorCode::StateMismatch,          ErrorCode::PlanConflict,
                                     ErrorCode::StoreLocked};

const ErrorCode kStoreFailureCodes[] = {ErrorCode::StoreLocked,      ErrorCode::StoreCorrupt,
                                        ErrorCode::StoreUninitialized, ErrorCode::StoreClosed,
                                        ErrorCode::IoFailure,        ErrorCode::PermissionDenied,
                                        ErrorCode::PathTraversal,    ErrorCode::ReparsePointRejected,
                                        ErrorCode::AlternateDataStream};

const ErrorCode kIntegrityCodes[] = {ErrorCode::IntegrityFailure, ErrorCode::TrailingBytes,
                                     ErrorCode::StoreCorrupt, ErrorCode::UnsupportedFormatVersion};

template <std::size_t N>
[[nodiscard]] bool listed(const ErrorCode (&codes)[N], ErrorCode code) {
  for (const ErrorCode candidate : codes) {
    if (candidate == code) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool snake_case(std::string_view text) {
  if (text.empty() || text.front() == '_' || text.back() == '_') {
    return false;
  }
  bool previous_underscore = false;
  for (const char byte : text) {
    const bool underscore = byte == '_';
    if (!underscore && !(byte >= 'a' && byte <= 'z') && !(byte >= '0' && byte <= '9')) {
      return false;
    }
    if (underscore && previous_underscore) {
      return false;
    }
    previous_underscore = underscore;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

HL_TEST(unit_digest_errors, sha256_known_answers) {
  // FIPS 180-4 and the standard test vectors, including the two block padding
  // case and the million byte case that exercises long input handling.
  HL_CHECK_EQ(Sha256::hash("").hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  HL_CHECK_EQ(Sha256::hash("abc").hex(),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  HL_CHECK_EQ(two_block.size(), static_cast<std::size_t>(56));
  HL_CHECK_EQ(Sha256::hash(two_block).hex(),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  const std::string million(1000000, 'a');
  HL_CHECK_EQ(Sha256::hash(million).hex(),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  // A digest of the wrong bytes is a different digest: the known answers above
  // are not satisfied by any zero or truncated value.
  HL_CHECK(Sha256::hash("abc") != Sha256::hash("abd"));
  HL_CHECK(Sha256::hash("abc") != Digest());
  HL_CHECK_EQ(Sha256::hash("abc").is_zero(), false);
  HL_CHECK_EQ(Digest().is_zero(), true);
}

HL_TEST(unit_digest_errors, sha256_incremental_equals_one_shot_and_finalise_keeps_state) {
  const std::string message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

  Sha256 hasher;
  hasher.update("abcdbcdecdefdefgefghfghighijhijk");
  const Digest prefix = hasher.finish();
  HL_CHECK_EQ(prefix.hex(), Sha256::hash("abcdbcdecdefdefgefghfghighijhijk").hex());

  // Finalising a copy leaves the running state alone: the same hasher continues
  // from where it was and still reaches the one shot answer.
  hasher.update("ijkljklmklmnlmnomnopnopq");
  HL_CHECK_EQ(hasher.finish().hex(), Sha256::hash(message).hex());

  // Feeding the same bytes one at a time crosses every buffer boundary and must
  // not change the result.
  Sha256 byte_by_byte;
  for (const char byte : message) {
    byte_by_byte.update(&byte, 1);
  }
  HL_CHECK_EQ(byte_by_byte.finish().hex(), Sha256::hash(message).hex());

  // An update with no bytes is a no-op, not a reset.
  Sha256 untouched;
  untouched.update(static_cast<const void*>(nullptr), 0);
  untouched.update("");
  HL_CHECK_EQ(untouched.finish().hex(), Sha256::hash("").hex());
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

HL_TEST(unit_digest_errors, crc32_known_answer_and_incremental_continuation) {
  const std::string check = "123456789";
  const std::uint32_t value = crc32_ieee(check.data(), check.size());
  HL_CHECK_EQ(value, static_cast<std::uint32_t>(0xCBF43926u));

  // Continuing from a finalised CRC is the same as hashing the concatenation.
  const std::string head = "1234";
  const std::string tail = "56789";
  const std::uint32_t prefix = crc32_ieee(head.data(), head.size());
  HL_CHECK_EQ(crc32_ieee_update(prefix, tail.data(), tail.size()), value);

  const std::string part_a = "12";
  const std::string part_b = "34";
  const std::string part_c = "56789";
  const std::uint32_t chained = crc32_ieee_update(
      crc32_ieee_update(crc32_ieee(part_a.data(), part_a.size()), part_b.data(), part_b.size()), part_c.data(),
      part_c.size());
  HL_CHECK_EQ(chained, value);

  // An empty update returns the seed unchanged and the empty message is zero.
  HL_CHECK_EQ(crc32_ieee(nullptr, 0), static_cast<std::uint32_t>(0));
  HL_CHECK_EQ(crc32_ieee_update(0x12345678u, nullptr, 0), static_cast<std::uint32_t>(0x12345678u));
  HL_CHECK_EQ(crc32_ieee_update(value, nullptr, 0), value);

  // CRC-32 is framing, not authority: two different messages can share it, and
  // it is not a digest.
  HL_CHECK_EQ(sizeof(crc32_ieee(check.data(), check.size())), sizeof(std::uint32_t));
}

// ---------------------------------------------------------------------------
// Digest
// ---------------------------------------------------------------------------

HL_TEST(unit_digest_errors, digest_from_hex_is_strict_and_round_trips) {
  const Digest digest = Sha256::hash("abc");
  const std::string rendered = digest.hex();
  HL_CHECK_EQ(rendered.size(), static_cast<std::size_t>(64));
  HL_CHECK_EQ(to_ascii_lower(rendered), rendered);

  const Result<Digest> parsed = Digest::from_hex(rendered);
  HL_REQUIRE(parsed.has_value());
  HL_CHECK(parsed.value() == digest);

  // Uppercase is hexadecimal too, and it decodes to the same bytes.
  const Result<Digest> upper = Digest::from_hex(to_ascii_lower(rendered).empty()
                                                    ? std::string()
                                                    : std::string(rendered).replace(0, 1, "B"));
  HL_REQUIRE(upper.has_value());
  const Result<Digest> lower = Digest::from_hex(std::string(rendered).replace(0, 1, "b"));
  HL_REQUIRE(lower.has_value());
  HL_CHECK(upper.value() == lower.value());

  HL_CHECK_EQ(Digest::from_hex(std::string(63, 'a')).has_value(), false);
  HL_CHECK_EQ(Digest::from_hex(std::string(65, 'a')).has_value(), false);
  HL_CHECK_EQ(Digest::from_hex("").has_value(), false);
  HL_CHECK_EQ(Digest::from_hex(std::string(64, 'g')).has_value(), false);
  HL_CHECK_EQ(Digest::from_hex(std::string(63, 'a') + " ").has_value(), false);

  // The all zero digest means "not computed" and is distinguishable from every
  // real digest.
  const Digest zero;
  HL_CHECK_EQ(zero.is_zero(), true);
  HL_CHECK_EQ(zero == digest, false);
  HL_CHECK_EQ(Digest::from_hex(zero.hex()).value().is_zero(), true);

  // Bytes survive a round trip through the value type.
  const std::array<std::uint8_t, Digest::kSize> bytes = [] {
    std::array<std::uint8_t, Digest::kSize> filled{};
    for (std::size_t index = 0; index < filled.size(); ++index) {
      filled[index] = static_cast<std::uint8_t>(index + 1);
    }
    return filled;
  }();
  const Digest rebuilt = Digest::from_bytes(bytes);
  HL_CHECK_EQ(rebuilt.bytes(), bytes);
  HL_CHECK_EQ(Digest::from_hex(rebuilt.hex()).value().bytes(), bytes);
}

// ---------------------------------------------------------------------------
// Error taxonomy
// ---------------------------------------------------------------------------

HL_TEST(unit_digest_errors, error_code_spellings_are_stable_and_unique) {
  HL_CHECK_EQ(sizeof(kCodeSpellings) / sizeof(kCodeSpellings[0]), static_cast<std::size_t>(37));

  std::vector<std::string> seen;
  for (const CodeSpelling& entry : kCodeSpellings) {
    const std::string_view spelling = to_string(entry.code);
    HL_CHECK_EQ(spelling, std::string_view(entry.spelling));
    HL_CHECK_MSG(snake_case(spelling), std::string(spelling).c_str());
    for (const std::string& earlier : seen) {
      HL_CHECK_MSG(earlier != spelling, std::string(spelling).c_str());
    }
    seen.emplace_back(spelling);
  }

  // A value outside the declared enumerator set is reported with the catch-all
  // spelling rather than an empty or invented one.
  HL_CHECK_EQ(to_string(static_cast<ErrorCode>(9999)), std::string_view("internal"));
}

HL_TEST(unit_digest_errors, validation_rank_is_strictly_increasing_over_the_ranked_prefix) {
  HL_CHECK_EQ(validation_rank(ErrorCode::Ok), static_cast<std::uint32_t>(0));
  HL_CHECK_EQ(is_validation_code(ErrorCode::Ok), false);

  std::uint32_t previous = 0;
  for (const ErrorCode code : kRankedCodes) {
    const std::uint32_t rank = validation_rank(code);
    HL_CHECK_MSG(rank > previous, std::string(to_string(code)).c_str());
    previous = rank;
  }

  // Every code agrees with is_validation_code, and the codes after the ranked
  // prefix never take part in request validation.
  for (const CodeSpelling& entry : kCodeSpellings) {
    HL_CHECK_EQ(is_validation_code(entry.code), validation_rank(entry.code) != 0);
    if (!listed(kRankedCodes, entry.code)) {
      // Codes outside the prefix never take part in request validation: a
      // non-zero rank here would mean the taxonomy and the table disagree.
      HL_CHECK_MSG(validation_rank(entry.code) == 0,
                   (std::string(to_string(entry.code)) + " is ranked but not in the ranked prefix").c_str());
      HL_CHECK_EQ(is_validation_code(entry.code), false);
    }
  }
}

HL_TEST(unit_digest_errors, primary_error_picks_the_lower_rank) {
  HL_CHECK_EQ(primary_error(ErrorCode::MissingEvidence, ErrorCode::IllegalTransition), ErrorCode::IllegalTransition);
  HL_CHECK_EQ(primary_error(ErrorCode::IllegalTransition, ErrorCode::MissingEvidence), ErrorCode::IllegalTransition);
  HL_CHECK_EQ(primary_error(ErrorCode::StaleRevision, ErrorCode::MalformedRequest), ErrorCode::MalformedRequest);
  HL_CHECK_EQ(primary_error(ErrorCode::MalformedRequest, ErrorCode::StaleRevision), ErrorCode::MalformedRequest);
  HL_CHECK_EQ(primary_error(ErrorCode::SelfReplacement, ErrorCode::LineageCycle), ErrorCode::SelfReplacement);

  // An unranked code never wins against a ranked one, and two unranked codes
  // resolve to the left operand so the result is a pure function of the pair.
  HL_CHECK_EQ(primary_error(ErrorCode::Internal, ErrorCode::MalformedRequest), ErrorCode::MalformedRequest);
  HL_CHECK_EQ(primary_error(ErrorCode::MalformedRequest, ErrorCode::Internal), ErrorCode::MalformedRequest);
  HL_CHECK_EQ(primary_error(ErrorCode::StoreLocked, ErrorCode::Internal), ErrorCode::StoreLocked);
  HL_CHECK_EQ(primary_error(ErrorCode::Internal, ErrorCode::StoreLocked), ErrorCode::Internal);
  HL_CHECK_EQ(primary_error(ErrorCode::SelfReplacement, ErrorCode::SelfReplacement), ErrorCode::SelfReplacement);

  // Folding the ranked prefix in reverse order lands on the most primary code.
  ErrorCode winner = kRankedCodes[sizeof(kRankedCodes) / sizeof(kRankedCodes[0]) - 1];
  for (std::size_t index = sizeof(kRankedCodes) / sizeof(kRankedCodes[0]); index > 0; --index) {
    winner = primary_error(winner, kRankedCodes[index - 1]);
  }
  HL_CHECK_EQ(winner, ErrorCode::LimitExceeded);
}

HL_TEST(unit_digest_errors, retryable_store_and_integrity_classification_is_exact) {
  for (const CodeSpelling& entry : kCodeSpellings) {
    const std::string spelling(to_string(entry.code));
    HL_CHECK_MSG(is_retryable(entry.code) == listed(kRetryableCodes, entry.code), spelling.c_str());
    HL_CHECK_MSG(is_store_failure(entry.code) == listed(kStoreFailureCodes, entry.code), spelling.c_str());
    HL_CHECK_MSG(is_integrity_failure(entry.code) == listed(kIntegrityCodes, entry.code), spelling.c_str());
  }

  // The classes are not the same class: a store failure is not automatically an
  // integrity failure, and Internal is in neither.
  HL_CHECK_EQ(is_store_failure(ErrorCode::IoFailure), true);
  HL_CHECK_EQ(is_integrity_failure(ErrorCode::IoFailure), false);
  HL_CHECK_EQ(is_store_failure(ErrorCode::IntegrityFailure), false);
  HL_CHECK_EQ(is_retryable(ErrorCode::MalformedRequest), false);
  HL_CHECK_EQ(is_retryable(ErrorCode::StoreCorrupt), false);
}

HL_TEST(unit_digest_errors, describe_renders_the_code_the_message_and_every_note) {
  const Error plain = Error::make(ErrorCode::MalformedRequest, "the request is not well formed");
  HL_CHECK_EQ(describe(plain), std::string("malformed_request: the request is not well formed"));

  // with() returns the error it annotated, so notes chain and the result is
  // consumed rather than discarded.
  const Error noted = Error::make(ErrorCode::MissingEvidence, "every change must bind to evidence")
                          .with("required", "drain_record")
                          .with("field", "evidence");
  HL_CHECK_EQ(describe(noted),
              std::string("missing_evidence: every change must bind to evidence (required: drain_record; field: "
                          "evidence)"));

  HL_CHECK_EQ(noted.notes.size(), static_cast<std::size_t>(2));
  HL_CHECK_EQ(noted.notes[0].field, std::string("required"));
  HL_CHECK_EQ(noted.notes[0].detail, std::string("drain_record"));

  // The constructors and the convenience helper agree, and a default constructed
  // error is Internal rather than Ok: an unset error is never a success.
  const Error convenience = make_error(ErrorCode::MissingEvidence, "every change must bind to evidence");
  HL_CHECK_EQ(convenience.code, noted.code);
  HL_CHECK_EQ(convenience.message, noted.message);
  const Error unset;
  HL_CHECK_EQ(unset.code, ErrorCode::Internal);
  HL_CHECK_EQ(unset.message, std::string());
  HL_CHECK_EQ(unset.notes.size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Durable format compatibility
// ---------------------------------------------------------------------------

HL_TEST(unit_digest_errors, compatibility_accepts_each_current_format_and_refuses_zero_and_two) {
  std::size_t count = 0;
  const FormatRange* ranges = format_ranges(count);
  HL_REQUIRE(ranges != nullptr);
  HL_CHECK_EQ(count, static_cast<std::size_t>(3));
  HL_CHECK_EQ(std::string_view(ranges[0].name), std::string_view("journal"));
  HL_CHECK_EQ(std::string_view(ranges[1].name), std::string_view("manifest"));
  HL_CHECK_EQ(std::string_view(ranges[2].name), std::string_view("export"));

  for (std::size_t index = 0; index < count; ++index) {
    const FormatRange& range = ranges[index];
    HL_CHECK_MSG(range.min_readable <= range.current, std::string(range.name).c_str());

    // Version 0 is below every readable range, the current version is accepted
    // and one version above it is refused with the version error.
    HL_CHECK_ERROR(check_supported(range.name, range.current, range.min_readable, 0),
                   ErrorCode::UnsupportedFormatVersion);
    HL_CHECK(check_supported(range.name, range.current, range.min_readable, range.current).has_value());
    HL_CHECK(check_supported(range.name, range.current, range.min_readable, range.min_readable).has_value());
    HL_CHECK_ERROR(check_supported(range.name, range.current, range.min_readable,
                                   static_cast<std::uint16_t>(range.current + 1)),
                   ErrorCode::UnsupportedFormatVersion);
    HL_CHECK_ERROR(check_supported(range.name, range.current, range.min_readable, 2),
                   ErrorCode::UnsupportedFormatVersion);
  }

  // The declared constants and the reported matrix agree.
  HL_CHECK_EQ(kJournalFormatVersion, static_cast<std::uint16_t>(1));
  HL_CHECK_EQ(kManifestFormatVersion, static_cast<std::uint16_t>(1));
  HL_CHECK_EQ(kExportFormatVersion, static_cast<std::uint16_t>(1));
  HL_CHECK_EQ(is_supported(1, 1, 0), false);
  HL_CHECK_EQ(is_supported(1, 1, 1), true);
  HL_CHECK_EQ(is_supported(1, 1, 2), false);

  const std::string matrix = compatibility_matrix();
  HL_CHECK(matrix.find("journal") != std::string::npos);
  HL_CHECK(matrix.find("manifest") != std::string::npos);
  HL_CHECK(matrix.find("export") != std::string::npos);
  HL_CHECK(matrix.find("min_readable") != std::string::npos);
  HL_CHECK(matrix.find("current") != std::string::npos);
}
