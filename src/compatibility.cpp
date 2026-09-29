// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The durable format registry. A reader accepts exactly the declared inclusive
// ranges and rejects everything else with UnsupportedFormatVersion; it never
// guesses at a newer layout and never downgrades an unreadable document to an
// empty one.

#include "hardware_lifecycle/compatibility.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {
namespace {

/// Every durable format this build knows about, in declaration order of
/// compatibility.hpp. The names are part of the diagnostic contract.
constexpr FormatRange kFormatRanges[] = {
    {"journal", kJournalFormatVersion, kJournalFormatVersionMinReadable},
    {"manifest", kManifestFormatVersion, kManifestFormatVersionMinReadable},
    {"export", kExportFormatVersion, kExportFormatVersionMinReadable},
};

/// Number of decimal digits needed to render value.
std::size_t decimal_width(std::uint16_t value) noexcept {
  std::size_t width = 1;
  while (value >= 10) {
    value = static_cast<std::uint16_t>(value / 10);
    ++width;
  }
  return width;
}

/// Appends one left aligned cell padded to width.
void append_cell(std::string& line, std::string_view cell, std::size_t width) {
  line.append(cell);
  for (std::size_t index = cell.size(); index < width; ++index) {
    line.push_back(' ');
  }
}

/// Appends one table row, newline terminated. The two space gutter keeps the
/// columns readable without depending on any locale.
void append_row(std::string& out, std::string_view first, std::string_view second, std::string_view third,
                const std::size_t (&widths)[3]) {
  std::string line;
  append_cell(line, first, widths[0]);
  line.append("  ");
  append_cell(line, second, widths[1]);
  line.append("  ");
  append_cell(line, third, widths[2]);
  out.append(line);
  out.push_back('\n');
}

/// Appends the dashed rule that underlines the header row.
void append_rule(std::string& out, const std::size_t (&widths)[3]) {
  std::string line;
  for (std::size_t column = 0; column < 3; ++column) {
    if (column != 0) {
      line.append("  ");
    }
    line.append(widths[column], '-');
  }
  out.append(line);
  out.push_back('\n');
}

}  // namespace

const FormatRange* format_ranges(std::size_t& count) noexcept {
  count = std::size(kFormatRanges);
  return kFormatRanges;
}

bool is_supported(std::uint16_t current, std::uint16_t min_readable, std::uint16_t version) noexcept {
  return version >= min_readable && version <= current;
}

Result<void> check_supported(std::string_view name, std::uint16_t current, std::uint16_t min_readable,
                             std::uint16_t version) {
  if (is_supported(current, min_readable, version)) {
    return ok();
  }
  return Error::make(ErrorCode::UnsupportedFormatVersion,
                     std::string(name) + " format version " + std::to_string(version) + " is not supported")
      .with(std::string(name), "format version " + std::to_string(version) + " is outside the readable range " +
                                   std::to_string(min_readable) + ".." + std::to_string(current));
}

std::string compatibility_matrix() {
  static constexpr std::string_view kHeaders[3] = {"format", "min_readable", "current"};
  std::size_t count = 0;
  const FormatRange* ranges = format_ranges(count);
  std::size_t widths[3] = {kHeaders[0].size(), kHeaders[1].size(), kHeaders[2].size()};
  for (std::size_t index = 0; index < count; ++index) {
    if (ranges[index].name.size() > widths[0]) {
      widths[0] = ranges[index].name.size();
    }
    const std::size_t minimum_width = decimal_width(ranges[index].min_readable);
    const std::size_t current_width = decimal_width(ranges[index].current);
    if (minimum_width > widths[1]) {
      widths[1] = minimum_width;
    }
    if (current_width > widths[2]) {
      widths[2] = current_width;
    }
  }

  std::string rendered;
  append_row(rendered, kHeaders[0], kHeaders[1], kHeaders[2], widths);
  append_rule(rendered, widths);
  for (std::size_t index = 0; index < count; ++index) {
    append_row(rendered, ranges[index].name, std::to_string(ranges[index].min_readable),
               std::to_string(ranges[index].current), widths);
  }
  return rendered;
}

}  // namespace hardware_lifecycle
