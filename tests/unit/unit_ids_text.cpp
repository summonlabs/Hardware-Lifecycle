// Hardware Lifecycle - unit proofs for the strong textual identities and the
// strict text primitives they are built on.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Identifiers are the durable names of hardware, so every rejection below is a
// contract: a spelling that is not an identifier is refused, never trimmed,
// folded, repaired or replaced by a default. The null id is a value that
// compares and orders, and it is never valid.

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "fs_platform.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;

/// Spellings Windows resolves to a character device rather than to a file. A
/// durable asset name must never be one of them, because an identifier that
/// reaches the file system as a path segment would open a device.
const char* const kReservedDeviceSpellings[] = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5",    "COM6",    "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",    "LPT8",    "LPT9",
    "con",  "nul",  "com1", "lpt9", "CON.txt", "NUL.dat",   "aux.log"};

/// The identifier spellings the shape rule admits, used as the positive half of
/// the boundary proofs.
const char* const kAcceptedIdentifierSpellings[] = {"a",     "A",       "0",      "Z9",      "asset-1",
                                                    "asset_1", "asset.1", "asset:1", "a.b-c_d:e", "R01.SLOT07"};

/// The spellings that are not identifiers for a structural reason: an empty
/// spelling, whitespace, a leading or trailing separator, a doubled dot, a byte
/// outside the admitted alphabet and a path separator.
const char* const kRejectedIdentifierSpellings[] = {"",
                                                    " ",
                                                    "\t",
                                                    "\n",
                                                    " a",
                                                    "a ",
                                                    "a\tb",
                                                    "a\nb",
                                                    ".a",
                                                    "-a",
                                                    "_a",
                                                    ":a",
                                                    "a.",
                                                    "a-",
                                                    "a_",
                                                    "a:",
                                                    "a..b",
                                                    "a..",
                                                    "a b",
                                                    "a/b",
                                                    "a\\b",
                                                    "a@b",
                                                    "a*b",
                                                    "a?b",
                                                    "a\"b",
                                                    "a<b",
                                                    "a>b",
                                                    "a|b",
                                                    "a+b",
                                                    "a=b",
                                                    "a!b",
                                                    "a(b)",
                                                    "a;b",
                                                    "a#b",
                                                    "a%b",
                                                    "caf\xC3\xA9",
                                                    "\xC3\xA9",
                                                    "a\x01" "b",
                                                    "a\x7F" "b"};

[[nodiscard]] std::string joined(const std::vector<std::string>& parts) {
  std::string text;
  for (const std::string& part : parts) {
    if (!text.empty()) {
      text += ", ";
    }
    text += '"';
    text += part;
    text += '"';
  }
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

HL_TEST(unit_ids_text, identifier_shape_accepts_only_identifier_spellings) {
  for (const char* spelling : kAcceptedIdentifierSpellings) {
    HL_CHECK_MSG(is_valid_identifier(spelling), spelling);
  }

  std::vector<std::string> accepted;
  for (const char* spelling : kRejectedIdentifierSpellings) {
    if (is_valid_identifier(spelling)) {
      accepted.emplace_back(spelling);
    }
  }
  const std::string offenders = joined(accepted);
  HL_CHECK_MSG(accepted.empty(), ("non identifier spellings accepted as identifiers: " + offenders).c_str());

  // The length bound is inclusive at kMaxIdentifierBytes and exclusive above it.
  HL_CHECK_EQ(is_valid_identifier(std::string(limits::kMaxIdentifierBytes, 'a')), true);
  HL_CHECK_EQ(is_valid_identifier(std::string(limits::kMaxIdentifierBytes + 1, 'a')), false);
}

HL_TEST(unit_ids_text, reserved_device_spellings_are_identifiers_and_keep_their_spelling) {
  // An identifier is a name, not a file name: the shape rule deliberately admits
  // the Windows device spellings, and the path layer below is what refuses to
  // turn such a name into a segment. Acceptance is therefore the documented
  // behaviour, and it is asserted rather than assumed.
  std::vector<std::string> refused;
  for (const char* spelling : kReservedDeviceSpellings) {
    if (!is_valid_identifier(spelling)) {
      refused.emplace_back(spelling);
    }
  }
  const std::string offenders = joined(refused);
  HL_CHECK_MSG(refused.empty(), ("reserved device spellings refused as identifiers: " + offenders).c_str());

  for (const char* spelling : kReservedDeviceSpellings) {
    const Result<AssetId> parsed = AssetId::parse(spelling);
    HL_CHECK_MSG(parsed.has_value(), spelling);
    if (parsed.has_value()) {
      // The name is preserved byte for byte: no case folding, no trimming and no
      // repair of the spelling the caller presented.
      HL_CHECK_MSG(parsed.value().value() == spelling, spelling);
    }
  }

  // Two spellings that differ only in case are two identities, not one.
  const Result<AssetId> upper = AssetId::parse("CON");
  const Result<AssetId> lower = AssetId::parse("con");
  HL_REQUIRE(upper.has_value());
  HL_REQUIRE(lower.has_value());
  HL_CHECK(upper.value() != lower.value());

  // The boundary still holds for a spelling outside the shape rule.
  HL_CHECK_EQ(is_valid_identifier("CON "), false);
  HL_CHECK_ERROR(AssetId::parse("CON "), ErrorCode::MalformedRequest);
}

HL_TEST(unit_ids_text, path_segments_refuse_devices_traversal_and_alternate_streams) {
  using hardware_lifecycle::detail::validate_path_segment;

  // The store composes file names from fixed segments; a caller supplied name is
  // never used as one. This is the layer where a device name, a traversal or an
  // alternate data stream is refused, and each refusal carries its own code.
  HL_CHECK_ERROR(validate_path_segment("CON"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("con.txt"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("NUL"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("COM1"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("LPT9.dat"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("trailing."), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("trailing "), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("control\x01" "byte"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(validate_path_segment("."), ErrorCode::PathTraversal);
  HL_CHECK_ERROR(validate_path_segment(".."), ErrorCode::PathTraversal);
  HL_CHECK_ERROR(validate_path_segment("a/b"), ErrorCode::PathTraversal);
  HL_CHECK_ERROR(validate_path_segment("a\\b"), ErrorCode::PathTraversal);
  HL_CHECK_ERROR(validate_path_segment("stream:name"), ErrorCode::AlternateDataStream);

  HL_CHECK(validate_path_segment("journal-000001.hls").has_value());
  HL_CHECK(validate_path_segment("manifest.hlm").has_value());

  // The two layers agree on the boundary: every spelling this suite proves to be
  // an identifier is refused as a path segment.
  for (const char* spelling : kReservedDeviceSpellings) {
    HL_CHECK_MSG(is_valid_identifier(spelling), spelling);
    HL_CHECK_MSG(!validate_path_segment(spelling).has_value(), spelling);
  }
}

HL_TEST(unit_ids_text, text_id_parse_round_trips_and_the_null_id_is_never_valid) {
  const Result<AssetId> asset = AssetId::parse("asset-1.rack:07");
  HL_REQUIRE(asset.has_value());
  HL_CHECK_EQ(asset.value().value(), std::string("asset-1.rack:07"));
  HL_CHECK_EQ(asset.value().valid(), true);
  HL_CHECK(asset.value() == asset.value());
  HL_CHECK(asset.value() != AssetId());
  HL_CHECK(asset.value() < AssetId::parse("asset-2").value());

  const AssetId null_id;
  HL_CHECK_EQ(null_id.valid(), false);
  HL_CHECK_EQ(null_id.value(), std::string());
  HL_CHECK_EQ(null_id == AssetId(), true);

  // Every entry point rejects the null id, and the rejection names the shape.
  HL_CHECK_ERROR(AssetId::parse(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(PlanId::parse("plan 1"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(RackId::parse("rack-1/2"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(SlotId::parse(".."), ErrorCode::MalformedRequest);
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

HL_TEST(unit_ids_text, counter_id_is_one_based_and_rejects_zero) {
  const Revision null_revision;
  HL_CHECK_EQ(null_revision.value(), static_cast<std::uint64_t>(0));
  HL_CHECK_EQ(null_revision.valid(), false);
  HL_CHECK_EQ(null_revision == Revision(), true);

  const Revision first = Revision::first();
  HL_CHECK_EQ(first.value(), static_cast<std::uint64_t>(1));
  HL_CHECK_EQ(first.valid(), true);
  HL_CHECK_EQ(Revision::from_value(1), first);

  HL_CHECK_ERROR(Revision::parse("0"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(LifecycleGeneration::parse("0"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(ControlEpoch::parse("0"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(CommitSequence::parse("00"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(LogicalTime::parse("-1"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(ObservationSequence::parse(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(ObservationSequence::parse("1.0"), ErrorCode::MalformedRequest);

  const Result<Revision> parsed = Revision::parse("41");
  HL_REQUIRE(parsed.has_value());
  HL_CHECK_EQ(parsed.value().value(), static_cast<std::uint64_t>(41));
  HL_CHECK_EQ(Revision::parse("1").value(), first);
}

HL_TEST(unit_ids_text, counter_id_next_reports_overflow_instead_of_wrapping) {
  const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();

  const Result<Revision> exhausted = Revision::from_value(maximum).next();
  HL_CHECK_ERROR(exhausted, ErrorCode::CounterOverflow);

  const Result<Revision> parsed = Revision::parse("18446744073709551615");
  HL_REQUIRE(parsed.has_value());
  HL_CHECK_EQ(parsed.value().value(), maximum);
  HL_CHECK_ERROR(parsed.value().next(), ErrorCode::CounterOverflow);

  // The successor of a real counter is one more, and it stays valid.
  const Result<Revision> second = Revision::first().next();
  HL_REQUIRE(second.has_value());
  HL_CHECK_EQ(second.value().value(), static_cast<std::uint64_t>(2));
  HL_CHECK_EQ(second.value().valid(), true);
  const Result<LifecycleGeneration> generation = LifecycleGeneration::first().next();
  HL_REQUIRE(generation.has_value());
  HL_CHECK_EQ(generation.value().value(), static_cast<std::uint64_t>(2));

  // Above the counter range the parse itself is refused; it never saturates.
  HL_CHECK_ERROR(Revision::parse("18446744073709551616"), ErrorCode::MalformedRequest);
}

HL_TEST(unit_ids_text, object_key_renders_canonically_and_parses_strictly) {
  ObjectKey key;
  key.asset = AssetId::parse("rack-1").value();
  key.hardware_generation = HardwareGeneration::from_value(7);
  HL_CHECK_EQ(key.valid(), true);
  HL_CHECK_EQ(to_string(key), std::string("rack-1@7"));

  const Result<ObjectKey> parsed = parse_object_key("rack-1@7");
  HL_REQUIRE(parsed.has_value());
  HL_CHECK(parsed.value() == key);
  HL_CHECK_EQ(to_string(parsed.value()), to_string(key));

  // Round trip for a second key, including the hardware generation separator.
  const Result<ObjectKey> other = parse_object_key("asset.a@18446744073709551615");
  HL_REQUIRE(other.has_value());
  HL_CHECK_EQ(to_string(other.value()), std::string("asset.a@18446744073709551615"));

  const char* const rejected[] = {"rack-1",     "rack-1@",  "@7",       "rack-1@7@8", "rack-1@0",
                                  "rack-1@07",   "rack-1@x", "rack 1@7", " rack-1@7",  "rack-1@7 ",
                                  "rack-1@@7",   "rack-1@+7", "rack-1@-7", "rack-1@7.0", "rack-1@18446744073709551616"};
  for (const char* text : rejected) {
    HL_CHECK_ERROR(parse_object_key(text), ErrorCode::MalformedRequest);
  }

  // Ordering is by asset first, then by hardware generation: a replacement key
  // never sorts into the place of the key it replaced.
  ObjectKey higher;
  higher.asset = key.asset;
  higher.hardware_generation = HardwareGeneration::from_value(8);
  HL_CHECK(key < higher);
  HL_CHECK(higher > key);
  HL_CHECK(key <= higher);
  HL_CHECK(higher >= key);
  HL_CHECK(key != higher);
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

HL_TEST(unit_ids_text, utf8_validation_rejects_overlong_surrogate_and_truncated) {
  const char* const valid[] = {"",       "plain ascii", "caf\xC3\xA9",   "\xE2\x82\xAC",
                               "\xF0\x9F\x98\x80", "ok\xF4\x8F\xBF\xBF", "\xC2\x80"};
  for (const char* text : valid) {
    HL_CHECK_MSG(is_valid_utf8(text), text);
  }

  const char* const invalid[] = {
      // Overlong forms: a value below the minimum for its length.
      "\xC0\xAF", "\xC1\xBF", "\xE0\x80\xAF", "\xE0\x9F\xBF", "\xF0\x80\x80\xAF", "\xF0\x8F\xBF\xBF",
      // Surrogates: U+D800..U+DFFF are not scalar values.
      "\xED\xA0\x80", "\xED\xBF\xBF",
      // Above U+10FFFF, and lead bytes that can never begin a scalar value.
      "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFE", "\xFF",
      // Truncated tails and stray continuation bytes.
      "\xE2", "\xE2\x82", "\xF0\x9F\x98", "\x80", "\xBF", "ok\xE2",
      // A continuation byte where a lead byte is required.
      "\xC3\xC3\xA9", "\xE2\x28\xA1"};
  for (const char* text : invalid) {
    HL_CHECK_MSG(!is_valid_utf8(text), text);
  }
}

// ---------------------------------------------------------------------------
// Strict integer parsing
// ---------------------------------------------------------------------------

HL_TEST(unit_ids_text, u64_strict_parse_boundaries) {
  const Result<std::uint64_t> zero = parse_u64_strict("0");
  HL_REQUIRE(zero.has_value());
  HL_CHECK_EQ(zero.value(), static_cast<std::uint64_t>(0));

  const Result<std::uint64_t> one = parse_u64_strict("1");
  HL_REQUIRE(one.has_value());
  HL_CHECK_EQ(one.value(), static_cast<std::uint64_t>(1));

  const Result<std::uint64_t> maximum = parse_u64_strict("18446744073709551615");
  HL_REQUIRE(maximum.has_value());
  HL_CHECK_EQ(maximum.value(), (std::numeric_limits<std::uint64_t>::max)());

  const char* const malformed[] = {"", "00", "01", "007", "+1", "-1", " 1", "1 ", "\t1", "1\n",
                                   "1a", "a", "0x10", "1.0", "1,0", "١٢٣"};
  for (const char* text : malformed) {
    HL_CHECK_ERROR(parse_u64_strict(text), ErrorCode::MalformedRequest);
  }

  // Bounds are decided before shape, so a literal above the range is reported as
  // out of range even when it is also badly shaped.
  const char* const above[] = {"18446744073709551616", "99999999999999999999",
                               "0000000000000000000000018446744073709551616"};
  for (const char* text : above) {
    HL_CHECK_ERROR(parse_u64_strict(text), ErrorCode::LimitExceeded);
  }
}

HL_TEST(unit_ids_text, i64_strict_parse_boundaries_including_int64_min) {
  const Result<std::int64_t> zero = parse_i64_strict("0");
  HL_REQUIRE(zero.has_value());
  HL_CHECK_EQ(zero.value(), static_cast<std::int64_t>(0));

  const Result<std::int64_t> maximum = parse_i64_strict("9223372036854775807");
  HL_REQUIRE(maximum.has_value());
  HL_CHECK_EQ(maximum.value(), (std::numeric_limits<std::int64_t>::max)());

  // The most negative value has no positive counterpart, so it is the boundary a
  // sign handling bug would break.
  const Result<std::int64_t> minimum = parse_i64_strict("-9223372036854775808");
  HL_REQUIRE(minimum.has_value());
  HL_CHECK_EQ(minimum.value(), (std::numeric_limits<std::int64_t>::min)());

  const Result<std::int64_t> negative = parse_i64_strict("-42");
  HL_REQUIRE(negative.has_value());
  HL_CHECK_EQ(negative.value(), static_cast<std::int64_t>(-42));

  const char* const malformed[] = {"", "-", "--1", "-0", "+0", "+1", " 0", "0 ", "00", "-00", "-01",
                                   "1-", "1.0", "-1.0", "a", "-a"};
  for (const char* text : malformed) {
    HL_CHECK_ERROR(parse_i64_strict(text), ErrorCode::MalformedRequest);
  }

  const char* const above[] = {"9223372036854775808", "-9223372036854775809", "99999999999999999999",
                               "-99999999999999999999"};
  for (const char* text : above) {
    HL_CHECK_ERROR(parse_i64_strict(text), ErrorCode::LimitExceeded);
  }
}

HL_TEST(unit_ids_text, bool_parse_accepts_exactly_four_spellings) {
  const Result<bool> yes = parse_bool_strict("true");
  HL_REQUIRE(yes.has_value());
  HL_CHECK_EQ(yes.value(), true);
  const Result<bool> one = parse_bool_strict("1");
  HL_REQUIRE(one.has_value());
  HL_CHECK_EQ(one.value(), true);
  const Result<bool> no = parse_bool_strict("false");
  HL_REQUIRE(no.has_value());
  HL_CHECK_EQ(no.value(), false);
  const Result<bool> zero = parse_bool_strict("0");
  HL_REQUIRE(zero.has_value());
  HL_CHECK_EQ(zero.value(), false);

  const char* const rejected[] = {"", "True", "TRUE", "False", "yes", "no", "on", "off", "00", "01", " 1", "1 "};
  for (const char* text : rejected) {
    HL_CHECK_ERROR(parse_bool_strict(text), ErrorCode::MalformedRequest);
  }
}

// ---------------------------------------------------------------------------
// Hex
// ---------------------------------------------------------------------------

HL_TEST(unit_ids_text, hex_encode_and_decode_are_strict) {
  const std::uint8_t bytes[] = {0x00u, 0x0Fu, 0xA5u, 0xFFu};
  HL_CHECK_EQ(to_hex_lower(bytes, 4), std::string("000fa5ff"));
  HL_CHECK_EQ(to_hex_lower(nullptr, 0), std::string());
  HL_CHECK_EQ(to_hex_lower(bytes, 0), std::string());

  const Result<std::vector<std::uint8_t>> empty = from_hex("", 0);
  HL_REQUIRE(empty.has_value());
  HL_CHECK_EQ(empty.value().size(), static_cast<std::size_t>(0));

  const Result<std::vector<std::uint8_t>> decoded = from_hex("000fa5ff", 4);
  HL_REQUIRE(decoded.has_value());
  HL_CHECK_EQ(decoded.value().size(), static_cast<std::size_t>(4));
  HL_CHECK_EQ(decoded.value()[0], static_cast<std::uint8_t>(0x00));
  HL_CHECK_EQ(decoded.value()[1], static_cast<std::uint8_t>(0x0F));
  HL_CHECK_EQ(decoded.value()[2], static_cast<std::uint8_t>(0xA5));
  HL_CHECK_EQ(decoded.value()[3], static_cast<std::uint8_t>(0xFF));

  // Both cases are hexadecimal; the rendering is always lowercase.
  const Result<std::vector<std::uint8_t>> upper = from_hex("ABCD", 2);
  HL_REQUIRE(upper.has_value());
  HL_CHECK_EQ(upper.value()[0], static_cast<std::uint8_t>(0xAB));
  HL_CHECK_EQ(upper.value()[1], static_cast<std::uint8_t>(0xCD));
  HL_CHECK_EQ(to_hex_lower(upper.value().data(), upper.value().size()), std::string("abcd"));

  HL_CHECK_ERROR(from_hex("abc", 2), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_hex("0g", 1), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_hex(" 0", 1), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(from_hex("0x", 1), ErrorCode::MalformedRequest);

  // The byte budget follows from the length alone and is enforced before the
  // digits are interpreted: an over budget spelling that is also odd length is
  // reported as over budget, not as malformed.
  HL_CHECK_ERROR(from_hex("abcd", 1), ErrorCode::LimitExceeded);
  HL_CHECK_ERROR(from_hex("abcde", 1), ErrorCode::LimitExceeded);
  HL_CHECK_ERROR(from_hex("abcde", 2), ErrorCode::MalformedRequest);
}

// ---------------------------------------------------------------------------
// Split and join
// ---------------------------------------------------------------------------

HL_TEST(unit_ids_text, split_and_join_preserve_every_empty_piece) {
  const std::vector<std::string_view> single = split_ascii("", ',');
  HL_REQUIRE(single.size() == 1u);
  HL_CHECK_EQ(single[0], std::string_view());

  const std::vector<std::string_view> pieces = split_ascii("a,b,c", ',');
  HL_REQUIRE(pieces.size() == 3u);
  HL_CHECK_EQ(pieces[0], std::string_view("a"));
  HL_CHECK_EQ(pieces[1], std::string_view("b"));
  HL_CHECK_EQ(pieces[2], std::string_view("c"));

  const std::vector<std::string_view> gaps = split_ascii("a,,b", ',');
  HL_REQUIRE(gaps.size() == 3u);
  HL_CHECK_EQ(gaps[1], std::string_view());

  const std::vector<std::string_view> trailing = split_ascii("a,", ',');
  HL_REQUIRE(trailing.size() == 2u);
  HL_CHECK_EQ(trailing[1], std::string_view());

  const std::vector<std::string_view> leading = split_ascii(",a", ',');
  HL_REQUIRE(leading.size() == 2u);
  HL_CHECK_EQ(leading[0], std::string_view());

  const std::vector<std::string_view> only_delimiter = split_ascii(",", ',');
  HL_REQUIRE(only_delimiter.size() == 2u);
  HL_CHECK_EQ(only_delimiter[0], std::string_view());
  HL_CHECK_EQ(only_delimiter[1], std::string_view());

  const std::vector<std::string_view> absent = split_ascii("abc", ',');
  HL_REQUIRE(absent.size() == 1u);
  HL_CHECK_EQ(absent[0], std::string_view("abc"));

  HL_CHECK_EQ(join({}, "-"), std::string());
  HL_CHECK_EQ(join({"a"}, "-"), std::string("a"));
  HL_CHECK_EQ(join({"a", "b"}, ", "), std::string("a, b"));
  HL_CHECK_EQ(join({"", ""}, "-"), std::string("-"));
  HL_CHECK_EQ(join({"", "a", ""}, ","), std::string(",a,"));

  // Splitting is lossless: rejoining the pieces reproduces the input exactly,
  // including every empty piece.
  const char* const round_trip[] = {"", "a", "a,b", ",", "a,", ",a", "a,,b", ",,", "a,b,,c,"};
  for (const char* text : round_trip) {
    const std::vector<std::string_view> parts = split_ascii(text, ',');
    std::vector<std::string> owned;
    owned.reserve(parts.size());
    for (const std::string_view part : parts) {
      owned.emplace_back(part);
    }
    HL_CHECK_MSG(join(owned, ",") == text, text);
  }
}

// ---------------------------------------------------------------------------
// Remaining strict text primitives
// ---------------------------------------------------------------------------

HL_TEST(unit_ids_text, trim_and_case_helpers_touch_ascii_only) {
  HL_CHECK_EQ(trim_ascii("  \t\r\nabc \t\r\n"), std::string_view("abc"));
  HL_CHECK_EQ(trim_ascii(""), std::string_view());
  HL_CHECK_EQ(trim_ascii("   "), std::string_view());
  HL_CHECK_EQ(trim_ascii("abc"), std::string_view("abc"));

  HL_CHECK_EQ(to_ascii_lower("AbC-1"), std::string("abc-1"));
  HL_CHECK_EQ(to_ascii_lower("caf\xC3\x89"), std::string("caf\xC3\x89"));

  HL_CHECK_EQ(ascii_iequals("Asset-1", "asset-1"), true);
  HL_CHECK_EQ(ascii_iequals("asset-1", "asset-2"), false);
  HL_CHECK_EQ(ascii_iequals("asset", "asset-1"), false);
  HL_CHECK_EQ(ascii_iequals("", ""), true);

  HL_CHECK_EQ(is_printable_ascii("rack-1"), true);
  HL_CHECK_EQ(is_printable_ascii(""), true);
  HL_CHECK_EQ(is_printable_ascii("rack\t1"), false);
  HL_CHECK_EQ(is_printable_ascii("rack\xC3\xA9" "1"), false);
}

HL_TEST(unit_ids_text, evidence_source_is_non_empty_printable_text) {
  HL_CHECK_EQ(is_valid_evidence_source("wms/receipt"), true);
  HL_CHECK_EQ(is_valid_evidence_source("caf\xC3\xA9/monitor"), true);
  HL_CHECK_EQ(is_valid_evidence_source(""), false);
  HL_CHECK_EQ(is_valid_evidence_source("a\x01" "b"), false);
  HL_CHECK_EQ(is_valid_evidence_source("a\x7F" "b"), false);
  HL_CHECK_EQ(is_valid_evidence_source("a\xC2\x80" "b"), false);
  HL_CHECK_EQ(is_valid_evidence_source(std::string(limits::kMaxTextBytes, 'a')), true);
  HL_CHECK_EQ(is_valid_evidence_source(std::string(limits::kMaxTextBytes + 1, 'a')), false);
}
