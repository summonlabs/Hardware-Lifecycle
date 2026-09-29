// Hardware Lifecycle - hwlifecycle, the command line interface.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The tool is a thin, strict adapter over the runtime. It parses exactly the
// spellings the library accepts, never repairs an input, never substitutes a
// default for a value it did not understand, and reports the runtime's own
// stable error codes instead of inventing a message of its own. Two invariants
// shape the whole file:
//
//  - one invocation, one authority lifetime. A mutating command opens the store,
//    binds every expectation it was not given explicitly from the state it reads
//    in that same session, applies exactly one request and prints the receipt.
//    An expectation the caller did supply is used verbatim, so a stale-authority
//    fence is demonstrated rather than repaired.
//  - one output object per command. Every command fills exactly one JSON object
//    in a fixed field order; text mode flattens that object into "name: value"
//    lines. The two renderings therefore cannot disagree, and neither depends on
//    a clock, a hash order or a locale. An error is attached to the same object,
//    so a rejection and a success are read the same way, and a rejection never
//    carries a receipt: a receipt exists only after the durable commit point.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "json.hpp"

namespace hw = hardware_lifecycle;

namespace {

using JsonValue = hw::detail::JsonValue;

// ---------------------------------------------------------------------------
// Exit codes
// ---------------------------------------------------------------------------

inline constexpr int kExitSuccess = 0;
inline constexpr int kExitUsage = 1;
inline constexpr int kExitRejected = 2;
inline constexpr int kExitStoreFailure = 3;
inline constexpr int kExitInternal = 4;

// The value of the "usage" field: the line is rendered as "usage: <this>".
inline constexpr std::string_view kUsageLine = "hwlifecycle [global options] <command> [command options]";
inline constexpr std::string_view kHelpHint = "run \"hwlifecycle --help\" for the complete contract";
inline constexpr std::string_view kDefaultStore = "./hardware-lifecycle-store";

// ---------------------------------------------------------------------------
// Rendering primitives
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue json_text(std::string_view value) { return JsonValue::make_string(std::string(value)); }
[[nodiscard]] JsonValue json_number(std::uint64_t value) { return JsonValue::make_number(value); }
[[nodiscard]] JsonValue json_flag(bool value) { return JsonValue::make_bool(value); }

/// The one error rendering: a stable snake_case code, the human message and
/// every structured note. Nothing else is added, so two runs that fail the same
/// way print the same object.
[[nodiscard]] JsonValue error_json(const hw::Error& error) {
  JsonValue node = JsonValue::make_object();
  node.emplace("code", json_text(hw::to_string(error.code)));
  node.emplace("message", json_text(error.message));
  if (!error.notes.empty()) {
    JsonValue notes = JsonValue::make_array();
    for (const hw::FieldNote& note : error.notes) {
      JsonValue item = JsonValue::make_object();
      item.emplace("field", json_text(note.field));
      item.emplace("detail", json_text(note.detail));
      notes.push_back(std::move(item));
    }
    node.emplace("notes", std::move(notes));
  }
  return node;
}

[[nodiscard]] std::string scalar_text(const JsonValue& value) {
  switch (value.type()) {
    case JsonValue::Type::Null:
      // Absence is rendered as absence. It is never rendered as zero, false,
      // empty or a default.
      return "none";
    case JsonValue::Type::Bool:
      return value.as_bool() ? "true" : "false";
    case JsonValue::Type::Number:
      return std::to_string(value.as_number());
    case JsonValue::Type::String:
      return value.as_string();
    case JsonValue::Type::Array:
    case JsonValue::Type::Object:
      break;
  }
  return std::string();
}

void flatten(const JsonValue& value, std::string prefix, std::vector<std::string>& lines) {
  if (value.type() == JsonValue::Type::Object) {
    for (const auto& field : value.fields()) {
      flatten(field.second, prefix + field.first + ".", lines);
    }
    return;
  }
  if (value.type() == JsonValue::Type::Array) {
    for (const JsonValue& item : value.items()) {
      flatten(item, prefix, lines);
    }
    return;
  }
  if (!prefix.empty() && prefix.back() == '.') {
    prefix.pop_back();
  }
  lines.push_back(prefix + ": " + scalar_text(value));
}

[[nodiscard]] std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t end = text.find('\n', begin);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(begin));
      break;
    }
    lines.emplace_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return lines;
}

/// Collects one command's output. Text mode is a flattening of the same object
/// the JSON mode prints, except where a command documents a line oriented
/// rendering (list, history) or hands over a library rendering (diff).
class Output {
 public:
  Output(bool json_mode, bool compact) : json_(json_mode), pretty_(!compact) { root_ = JsonValue::make_object(); }

  void text(std::string_view name, std::string_view value) {
    root_.emplace(std::string(name), json_text(value));
  }
  void number(std::string_view name, std::uint64_t value) {
    root_.emplace(std::string(name), json_number(value));
  }
  void flag(std::string_view name, bool value) { root_.emplace(std::string(name), json_flag(value)); }
  void absent(std::string_view name) { root_.emplace(std::string(name), JsonValue::make_null()); }
  void member(std::string_view name, JsonValue value) { root_.emplace(std::string(name), std::move(value)); }

  void set_lines(std::vector<std::string> rendered_lines) { lines_ = std::move(rendered_lines); }
  void set_document(std::string document) { document_ = std::move(document); }

  /// Attaches a rejection. A pending line or document rendering is dropped: a
  /// failed command never prints a partial result that could be read as one.
  void attach_error(const hw::Error& error) {
    lines_.clear();
    document_.clear();
    root_.emplace("error", error_json(error));
  }

  void render(std::ostream& out) const {
    if (!document_.empty()) {
      out << document_;
      if (document_.back() != '\n') {
        out << '\n';
      }
      return;
    }
    if (json_) {
      out << hw::detail::write_json(root_, pretty_) << '\n';
      return;
    }
    if (!lines_.empty()) {
      for (const std::string& line : lines_) {
        out << line << '\n';
      }
      return;
    }
    std::vector<std::string> flattened;
    flatten(root_, std::string(), flattened);
    for (const std::string& line : flattened) {
      out << line << '\n';
    }
  }

 private:
  JsonValue root_;
  std::vector<std::string> lines_;
  std::string document_;
  bool json_ = false;
  bool pretty_ = true;
};

[[nodiscard]] hw::Error usage_error(std::string message) {
  return hw::Error::make(hw::ErrorCode::MalformedRequest, std::move(message));
}

/// Renders the structured notes of an error the same way describe() does, so a
/// rejected option value still names the offending text. The message alone would
/// leave the operator guessing which spelling was refused.
[[nodiscard]] std::string describe_notes(const hw::Error& error) {
  if (error.notes.empty()) {
    return std::string();
  }
  std::string text = " (";
  for (std::size_t index = 0; index < error.notes.size(); ++index) {
    if (index != 0) {
      text += "; ";
    }
    text += error.notes[index].field + ": " + error.notes[index].detail;
  }
  text += ")";
  return text;
}

/// Maps an error to the exit code contract. is_store_failure() is the single
/// source of truth for the durable store class, exactly as documented.
[[nodiscard]] int report_failure(Output& out, const hw::Error& error) {
  if (error.code == hw::ErrorCode::StaleAuthority) {
    out.text("hint",
             "authority is never inherited across a process boundary; re-run with --attest to re-establish "
             "authority for this object in this session");
  }
  out.attach_error(error);
  if (error.code == hw::ErrorCode::Internal) {
    return kExitInternal;
  }
  if (hw::is_store_failure(error.code)) {
    return kExitStoreFailure;
  }
  return kExitRejected;
}

[[nodiscard]] int report_usage(Output& out, const hw::Error& error) {
  out.attach_error(error);
  return kExitUsage;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct OptionSpec {
  std::string_view name;
  bool takes_value = false;
  bool repeatable = false;
};

const OptionSpec kGlobalOptions[] = {
    {"--store", true, false},   {"--read-only", false, false}, {"--ephemeral", false, false},
    {"--json", false, false},   {"--quiet", false, false},     {"--compact", false, false},
    {"--help", false, false},   {"--version", false, false},
    // --attest is accepted on any command line but only the mutating commands
    // act on it: transition, gate and link-replacement re-establish authority
    // over the objects they are about to change before changing them.
    {"--attest", false, false},
};

const OptionSpec kLegalTransitionsOptions[] = {{"--state", true, false}};

const OptionSpec kExplainOptions[] = {{"--from", true, false},
                                      {"--to", true, false},
                                      {"--reason", true, false},
                                      {"--authority", true, false},
                                      {"--evidence", true, true}};
const std::string_view kExplainRequired[] = {"--from", "--to"};

const OptionSpec kCreateOptions[] = {{"--asset", true, false},
                                     {"--hardware-generation", true, false},
                                     {"--kind", true, false},
                                     {"--model", true, false},
                                     {"--plan", true, false},
                                     {"--attempt", true, false},
                                     {"--actor", true, false},
                                     {"--actor-kind", true, false},
                                     {"--authority", true, false},
                                     {"--evidence", true, true},
                                     {"--firmware-generation", true, false},
                                     {"--state", true, false},
                                     {"--location", true, false},
                                     {"--wall-clock", true, false}};
const std::string_view kCreateRequired[] = {"--asset",  "--hardware-generation", "--kind", "--model", "--plan",
                                            "--attempt", "--actor",              "--actor-kind", "--authority",
                                            "--evidence"};

const OptionSpec kTransitionOptions[] = {{"--asset", true, false},
                                         {"--hardware-generation", true, false},
                                         {"--to", true, false},
                                         {"--reason", true, false},
                                         {"--plan", true, false},
                                         {"--attempt", true, false},
                                         {"--actor", true, false},
                                         {"--actor-kind", true, false},
                                         {"--authority", true, false},
                                         {"--evidence", true, true},
                                         {"--expected-revision", true, false},
                                         {"--expected-lifecycle-generation", true, false},
                                         {"--expected-epoch", true, false},
                                         {"--expected-state", true, false},
                                         {"--successor", true, false},
                                         {"--location", true, false}};
const std::string_view kTransitionRequired[] = {"--asset",     "--hardware-generation", "--to",
                                                "--reason",    "--plan",                "--attempt",
                                                "--actor",     "--actor-kind",          "--authority",
                                                "--evidence"};

const OptionSpec kGateOptions[] = {{"--asset", true, false},
                                   {"--hardware-generation", true, false},
                                   {"--gate", true, false},
                                   {"--plan", true, false},
                                   {"--attempt", true, false},
                                   {"--actor", true, false},
                                   {"--actor-kind", true, false},
                                   {"--authority", true, false},
                                   {"--evidence", true, true},
                                   {"--expected-revision", true, false},
                                   {"--expected-lifecycle-generation", true, false},
                                   {"--expected-epoch", true, false}};
const std::string_view kGateRequired[] = {"--asset",     "--hardware-generation", "--gate", "--plan",
                                          "--attempt",   "--actor",               "--actor-kind",
                                          "--authority", "--evidence"};

const OptionSpec kObserveHealthOptions[] = {{"--asset", true, false},
                                            {"--hardware-generation", true, false},
                                            {"--health", true, false},
                                            {"--evidence-digest", true, false},
                                            {"--source", true, false},
                                            {"--readiness", true, false},
                                            {"--availability", true, false},
                                            {"--observed-wall-clock", true, false},
                                            {"--plan", true, false},
                                            {"--attempt", true, false},
                                            {"--actor", true, false},
                                            {"--actor-kind", true, false},
                                            {"--evidence", true, true}};
const std::string_view kObserveHealthRequired[] = {"--asset",           "--hardware-generation",
                                                   "--health",          "--evidence-digest",
                                                   "--source",          "--plan",
                                                   "--attempt",         "--actor",
                                                   "--actor-kind",      "--evidence"};

const OptionSpec kAttestOptions[] = {{"--asset", true, false},
                                     {"--hardware-generation", true, false},
                                     {"--plan", true, false},
                                     {"--attempt", true, false},
                                     {"--actor", true, false},
                                     {"--actor-kind", true, false},
                                     {"--authority", true, false},
                                     {"--evidence", true, true},
                                     {"--expected-revision", true, false},
                                     {"--expected-epoch", true, false}};
const std::string_view kAttestRequired[] = {"--asset",  "--hardware-generation", "--plan", "--attempt",
                                            "--actor",  "--actor-kind",          "--authority", "--evidence"};

const OptionSpec kLinkReplacementOptions[] = {{"--predecessor", true, false},
                                              {"--successor", true, false},
                                              {"--plan", true, false},
                                              {"--attempt", true, false},
                                              {"--actor", true, false},
                                              {"--actor-kind", true, false},
                                              {"--authority", true, false},
                                              {"--evidence", true, true},
                                              {"--expected-revision", true, false},
                                              {"--expected-lifecycle-generation", true, false},
                                              {"--expected-epoch", true, false}};
const std::string_view kLinkReplacementRequired[] = {"--predecessor", "--successor", "--plan",      "--attempt",
                                                     "--actor",       "--actor-kind", "--authority", "--evidence"};

const OptionSpec kInspectOptions[] = {{"--asset", true, false}, {"--hardware-generation", true, false}};
const std::string_view kInspectRequired[] = {"--asset"};

const OptionSpec kListOptions[] = {{"--state", true, false},
                                   {"--kind", true, false},
                                   {"--site", true, false},
                                   {"--eligibility", true, false}};

const OptionSpec kHistoryOptions[] = {{"--asset", true, false}, {"--hardware-generation", true, false}};
const std::string_view kHistoryRequired[] = {"--asset"};

const OptionSpec kLineageOptions[] = {{"--asset", true, false}, {"--hardware-generation", true, false}};
const std::string_view kLineageRequired[] = {"--asset"};

const OptionSpec kExportOptions[] = {{"--out", true, false}};
const std::string_view kExportRequired[] = {"--out"};

const OptionSpec kImportOptions[] = {{"--in", true, false}};
const std::string_view kImportRequired[] = {"--in"};

const OptionSpec kDiffOptions[] = {{"--before", true, false}, {"--after", true, false}};
const std::string_view kDiffRequired[] = {"--before", "--after"};

struct CommandSpec {
  std::string_view name;
  std::span<const OptionSpec> options;
  std::span<const std::string_view> required;
};

const CommandSpec kCommands[] = {
    {"version", {}, {}},
    {"limits", {}, {}},
    {"compatibility", {}, {}},
    {"diagram", {}, {}},
    {"legal-transitions", kLegalTransitionsOptions, {}},
    {"explain", kExplainOptions, kExplainRequired},
    {"create", kCreateOptions, kCreateRequired},
    {"transition", kTransitionOptions, kTransitionRequired},
    {"gate", kGateOptions, kGateRequired},
    {"observe-health", kObserveHealthOptions, kObserveHealthRequired},
    {"attest", kAttestOptions, kAttestRequired},
    {"link-replacement", kLinkReplacementOptions, kLinkReplacementRequired},
    {"inspect", kInspectOptions, kInspectRequired},
    {"list", kListOptions, {}},
    {"history", kHistoryOptions, kHistoryRequired},
    {"lineage", kLineageOptions, kLineageRequired},
    {"recover", {}, {}},
    {"verify", {}, {}},
    {"export", kExportOptions, kExportRequired},
    {"import", kImportOptions, kImportRequired},
    {"diff", kDiffOptions, kDiffRequired},
};

[[nodiscard]] const CommandSpec* find_command(std::string_view name) {
  for (const CommandSpec& spec : kCommands) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

struct Arguments {
  std::string command;
  std::map<std::string, std::vector<std::string>, std::less<>> options;
  std::string store = std::string(kDefaultStore);
  bool json = false;
  bool quiet = false;
  bool compact = false;
  bool read_only = false;
  bool ephemeral = false;
  bool help = false;
  bool version = false;
};

[[nodiscard]] bool has_option(const Arguments& args, std::string_view name) {
  return args.options.find(name) != args.options.end();
}

[[nodiscard]] const std::string* value_of(const Arguments& args, std::string_view name) {
  const auto found = args.options.find(name);
  if (found == args.options.end() || found->second.empty()) {
    return nullptr;
  }
  return &found->second.front();
}

[[nodiscard]] const std::vector<std::string>* values_of(const Arguments& args, std::string_view name) {
  const auto found = args.options.find(name);
  if (found == args.options.end() || found->second.empty()) {
    return nullptr;
  }
  return &found->second;
}

/// Parses the whole command line before anything else happens, so a malformed
/// line never opens a store and never takes writer authority.
[[nodiscard]] hw::Result<Arguments> parse_arguments(int argc, char** argv) {
  Arguments args;
  for (int index = 1; index < argc; ++index) {
    std::string token = argv[index];
    if (token == "-h") {
      token = "--help";
    } else if (token == "-V") {
      token = "--version";
    }

    if (!token.empty() && token.front() == '-') {
      const OptionSpec* spec = nullptr;
      for (const OptionSpec& candidate : kGlobalOptions) {
        if (candidate.name == token) {
          spec = &candidate;
          break;
        }
      }
      if (spec == nullptr && !args.command.empty()) {
        if (const CommandSpec* command = find_command(args.command)) {
          for (const OptionSpec& candidate : command->options) {
            if (candidate.name == token) {
              spec = &candidate;
              break;
            }
          }
        }
      }
      if (spec == nullptr) {
        return usage_error("unknown option " + token);
      }
      if (spec->takes_value) {
        if (index + 1 >= argc) {
          return usage_error("option " + token + " requires a value");
        }
        ++index;
        if (!spec->repeatable && has_option(args, token)) {
          return usage_error("option " + token + " was given more than once");
        }
        args.options[token].push_back(std::string(argv[index]));
      } else {
        if (has_option(args, token)) {
          return usage_error("option " + token + " was given more than once");
        }
        args.options.emplace(token, std::vector<std::string>());
      }
      continue;
    }

    if (!args.command.empty()) {
      return usage_error("unexpected operand " + token);
    }
    if (find_command(token) == nullptr) {
      return usage_error("unknown command " + token);
    }
    args.command = token;
  }

  args.json = has_option(args, "--json");
  args.quiet = has_option(args, "--quiet");
  args.compact = has_option(args, "--compact");
  args.read_only = has_option(args, "--read-only");
  args.ephemeral = has_option(args, "--ephemeral");
  args.help = has_option(args, "--help");
  args.version = has_option(args, "--version");
  if (const std::string* store = value_of(args, "--store")) {
    args.store = *store;
  }

  if (!args.command.empty() && !args.help && !args.version) {
    const CommandSpec* command = find_command(args.command);
    for (const std::string_view required : command->required) {
      if (!has_option(args, required)) {
        return usage_error("missing required option " + std::string(required));
      }
    }
  }
  return args;
}

// ---------------------------------------------------------------------------
// Typed option values
// ---------------------------------------------------------------------------

[[nodiscard]] hw::Result<hw::EvidenceRef> parse_evidence_ref(std::string_view text) {
  // "kind:digest_hex:source"; the source is free text and may itself contain a
  // colon, so only the first two separators are structural.
  const std::size_t first = text.find(':');
  if (first == std::string_view::npos) {
    return usage_error("evidence must be written kind:digest_hex:source");
  }
  const std::size_t second = text.find(':', first + 1);
  if (second == std::string_view::npos) {
    return usage_error("evidence must be written kind:digest_hex:source");
  }
  const std::string_view kind_text = text.substr(0, first);
  const std::string_view digest_text = text.substr(first + 1, second - first - 1);
  const std::string_view source_text = text.substr(second + 1);

  const hw::Result<hw::EvidenceKind> kind = hw::parse_evidence_kind(kind_text);
  if (!kind.has_value()) {
    return usage_error("evidence: " + kind.error().message);
  }
  const hw::Result<hw::Digest> digest = hw::Digest::from_hex(digest_text);
  if (!digest.has_value()) {
    return usage_error("evidence: " + digest.error().message);
  }
  if (source_text.empty()) {
    return usage_error("evidence must name the source it came from");
  }
  hw::EvidenceRef reference;
  reference.kind = kind.value();
  reference.digest = digest.value();
  reference.source = std::string(source_text);
  return reference;
}

/// The service eligibility spelling set has no parser in the public API because
/// nothing durable carries it; the tool renders it from to_string() and folds
/// case here.
[[nodiscard]] hw::Result<hw::ServiceEligibility> parse_service_eligibility(std::string_view text) {
  const hw::ServiceEligibility values[] = {hw::ServiceEligibility::Unknown, hw::ServiceEligibility::Eligible,
                                           hw::ServiceEligibility::Ineligible};
  for (const hw::ServiceEligibility value : values) {
    if (hw::ascii_iequals(hw::to_string(value), text)) {
      return value;
    }
  }
  return usage_error("unknown service eligibility spelling").with("service eligibility", std::string(text));
}

/// Reads one command's options. The first failure is kept and every later read
/// short circuits, so the reported problem is always the first one in the order
/// the options are declared rather than the order they were typed.
class OptionReader {
 public:
  explicit OptionReader(const Arguments& args) : args_(args) {}

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const hw::Error& error() const noexcept { return error_; }

  template <class T, class Parse>
  T read(std::string_view name, Parse parse, T fallback) {
    if (failed_) {
      return fallback;
    }
    const std::string* raw = value_of(args_, name);
    if (raw == nullptr) {
      fail("missing required option " + std::string(name));
      return fallback;
    }
    const hw::Result<T> parsed = parse(*raw);
    if (!parsed.has_value()) {
      fail("option " + std::string(name) + ": " + parsed.error().message + describe_notes(parsed.error()));
      return fallback;
    }
    return parsed.value();
  }

  template <class T, class Parse>
  std::optional<T> read_optional(std::string_view name, Parse parse) {
    if (failed_) {
      return std::nullopt;
    }
    const std::string* raw = value_of(args_, name);
    if (raw == nullptr) {
      return std::nullopt;
    }
    const hw::Result<T> parsed = parse(*raw);
    if (!parsed.has_value()) {
      fail("option " + std::string(name) + ": " + parsed.error().message + describe_notes(parsed.error()));
      return std::nullopt;
    }
    return parsed.value();
  }

  std::vector<hw::EvidenceRef> read_evidence(std::string_view name, bool required) {
    std::vector<hw::EvidenceRef> references;
    if (failed_) {
      return references;
    }
    const std::vector<std::string>* raw = values_of(args_, name);
    if (raw == nullptr) {
      if (required) {
        fail("missing required option " + std::string(name));
      }
      return references;
    }
    for (const std::string& item : *raw) {
      const hw::Result<hw::EvidenceRef> parsed = parse_evidence_ref(item);
      if (!parsed.has_value()) {
        fail("option " + std::string(name) + ": " + parsed.error().message + describe_notes(parsed.error()));
        return std::vector<hw::EvidenceRef>();
      }
      references.push_back(parsed.value());
    }
    return references;
  }

 private:
  void fail(std::string message) {
    if (failed_) {
      return;
    }
    failed_ = true;
    error_ = usage_error(std::move(message));
  }

  const Arguments& args_;
  hw::Error error_ = hw::Error::make(hw::ErrorCode::Internal, "no failure was recorded");
  bool failed_ = false;
};

[[nodiscard]] hw::ObjectKey read_object_key(OptionReader& reader, std::string_view asset_option,
                                            std::string_view generation_option) {
  hw::ObjectKey key;
  key.asset = reader.read<hw::AssetId>(asset_option, &hw::AssetId::parse, hw::AssetId());
  key.hardware_generation = reader.read<hw::HardwareGeneration>(generation_option, &hw::HardwareGeneration::parse,
                                                                hw::HardwareGeneration());
  return key;
}

[[nodiscard]] hw::Provenance read_provenance(OptionReader& reader, const hw::PlanId& plan,
                                             const hw::AttemptId& attempt, bool with_authority,
                                             bool evidence_required, bool with_wall_clock) {
  hw::Provenance provenance;
  // The plan and attempt identity lives in two places by design; the runtime
  // rebuilds every receipt from the provenance, so the two must agree.
  provenance.plan = plan;
  provenance.attempt = attempt;
  provenance.actor.id = reader.read<hw::ActorId>("--actor", &hw::ActorId::parse, hw::ActorId());
  provenance.actor.kind = reader.read<hw::ActorKind>("--actor-kind", &hw::parse_actor_kind, hw::ActorKind::Unknown);
  if (with_authority) {
    provenance.authority =
        reader.read<hw::AuthorityMask>("--authority", &hw::parse_authority_mask, hw::AuthorityMask());
  }
  provenance.evidence = reader.read_evidence("--evidence", evidence_required);
  if (with_wall_clock) {
    const std::optional<hw::WallClock> clock =
        reader.read_optional<hw::WallClock>("--wall-clock", &hw::WallClock::parse);
    if (clock.has_value()) {
      provenance.wall_clock = clock.value();
    }
  }
  return provenance;
}

[[nodiscard]] hw::Result<hw::CreateRequest> build_create_request(const Arguments& args) {
  OptionReader reader(args);
  hw::CreateRequest request;
  request.plan = reader.read<hw::PlanId>("--plan", &hw::PlanId::parse, hw::PlanId());
  request.attempt = reader.read<hw::AttemptId>("--attempt", &hw::AttemptId::parse, hw::AttemptId());
  request.key = read_object_key(reader, "--asset", "--hardware-generation");
  request.kind = reader.read<hw::HardwareKind>("--kind", &hw::parse_hardware_kind, hw::HardwareKind::Unknown);
  request.model = reader.read<hw::ModelId>("--model", &hw::ModelId::parse, hw::ModelId());
  const std::optional<hw::FirmwareGeneration> firmware =
      reader.read_optional<hw::FirmwareGeneration>("--firmware-generation", &hw::FirmwareGeneration::parse);
  if (firmware.has_value()) {
    request.firmware_generation = firmware.value();
  }
  // Registration is restricted to Ordered and Staged by the validator; the tool
  // passes whatever spelling it was given and lets the runtime reject the rest.
  const std::optional<hw::LifecycleState> state =
      reader.read_optional<hw::LifecycleState>("--state", &hw::parse_lifecycle_state_lenient);
  if (state.has_value()) {
    request.initial_state = state.value();
  }
  request.location = reader.read_optional<hw::Location>("--location", &hw::parse_location);
  request.provenance = read_provenance(reader, request.plan, request.attempt, true, true, true);
  if (reader.failed()) {
    return reader.error();
  }
  return request;
}

struct TransitionPlan {
  hw::TransitionRequest request;
  bool expected_state_given = false;
};

[[nodiscard]] hw::Result<TransitionPlan> build_transition_plan(const Arguments& args) {
  OptionReader reader(args);
  TransitionPlan plan;
  plan.request.plan = reader.read<hw::PlanId>("--plan", &hw::PlanId::parse, hw::PlanId());
  plan.request.attempt = reader.read<hw::AttemptId>("--attempt", &hw::AttemptId::parse, hw::AttemptId());
  plan.request.key = read_object_key(reader, "--asset", "--hardware-generation");
  plan.request.target_state =
      reader.read<hw::LifecycleState>("--to", &hw::parse_lifecycle_state_lenient, hw::LifecycleState::Ordered);
  plan.request.reason =
      reader.read<hw::TransitionReason>("--reason", &hw::parse_transition_reason, hw::TransitionReason::Unset);
  plan.request.successor = reader.read_optional<hw::ObjectKey>("--successor", &hw::parse_object_key);
  plan.request.location = reader.read_optional<hw::Location>("--location", &hw::parse_location);

  const std::optional<hw::Revision> revision =
      reader.read_optional<hw::Revision>("--expected-revision", &hw::Revision::parse);
  if (revision.has_value()) {
    plan.request.expected_revision = revision.value();
  }
  const std::optional<hw::LifecycleGeneration> generation = reader.read_optional<hw::LifecycleGeneration>(
      "--expected-lifecycle-generation", &hw::LifecycleGeneration::parse);
  if (generation.has_value()) {
    plan.request.expected_lifecycle_generation = generation.value();
  }
  const std::optional<hw::ControlEpoch> epoch =
      reader.read_optional<hw::ControlEpoch>("--expected-epoch", &hw::ControlEpoch::parse);
  if (epoch.has_value()) {
    plan.request.expected_control_epoch = epoch.value();
  }
  const std::optional<hw::LifecycleState> state =
      reader.read_optional<hw::LifecycleState>("--expected-state", &hw::parse_lifecycle_state_lenient);
  if (state.has_value()) {
    plan.request.expected_state = state.value();
    plan.expected_state_given = true;
  }
  plan.request.provenance = read_provenance(reader, plan.request.plan, plan.request.attempt, true, true, false);
  if (reader.failed()) {
    return reader.error();
  }
  return plan;
}

[[nodiscard]] hw::Result<hw::EligibilityGateRequest> build_gate_request(const Arguments& args) {
  OptionReader reader(args);
  hw::EligibilityGateRequest request;
  request.plan = reader.read<hw::PlanId>("--plan", &hw::PlanId::parse, hw::PlanId());
  request.attempt = reader.read<hw::AttemptId>("--attempt", &hw::AttemptId::parse, hw::AttemptId());
  request.key = read_object_key(reader, "--asset", "--hardware-generation");
  request.gate = reader.read<hw::EligibilityGate>("--gate", &hw::parse_eligibility_gate, hw::EligibilityGate::Unknown);
  const std::optional<hw::Revision> revision =
      reader.read_optional<hw::Revision>("--expected-revision", &hw::Revision::parse);
  if (revision.has_value()) {
    request.expected_revision = revision.value();
  }
  const std::optional<hw::LifecycleGeneration> generation = reader.read_optional<hw::LifecycleGeneration>(
      "--expected-lifecycle-generation", &hw::LifecycleGeneration::parse);
  if (generation.has_value()) {
    request.expected_lifecycle_generation = generation.value();
  }
  const std::optional<hw::ControlEpoch> epoch =
      reader.read_optional<hw::ControlEpoch>("--expected-epoch", &hw::ControlEpoch::parse);
  if (epoch.has_value()) {
    request.expected_control_epoch = epoch.value();
  }
  request.provenance = read_provenance(reader, request.plan, request.attempt, true, true, false);
  if (reader.failed()) {
    return reader.error();
  }
  return request;
}

[[nodiscard]] hw::Result<hw::HealthObservationRequest> build_observation_request(const Arguments& args) {
  OptionReader reader(args);
  hw::HealthObservationRequest request;
  request.plan = reader.read<hw::PlanId>("--plan", &hw::PlanId::parse, hw::PlanId());
  request.attempt = reader.read<hw::AttemptId>("--attempt", &hw::AttemptId::parse, hw::AttemptId());
  request.key = read_object_key(reader, "--asset", "--hardware-generation");
  request.observation.key = request.key;
  request.observation.health =
      reader.read<hw::HealthStatus>("--health", &hw::parse_health_status, hw::HealthStatus::Unknown);
  const std::optional<hw::ReadinessStatus> readiness =
      reader.read_optional<hw::ReadinessStatus>("--readiness", &hw::parse_readiness_status);
  if (readiness.has_value()) {
    request.observation.readiness = readiness.value();
  }
  const std::optional<hw::AvailabilityStatus> availability =
      reader.read_optional<hw::AvailabilityStatus>("--availability", &hw::parse_availability_status);
  if (availability.has_value()) {
    request.observation.availability = availability.value();
  }
  request.observation.evidence_digest =
      reader.read<hw::Digest>("--evidence-digest", &hw::Digest::from_hex, hw::Digest());
  request.observation.source = reader.read<std::string>(
      "--source", [](std::string_view text) -> hw::Result<std::string> { return std::string(text); }, std::string());
  const std::optional<hw::WallClock> observed =
      reader.read_optional<hw::WallClock>("--observed-wall-clock", &hw::WallClock::parse);
  if (observed.has_value()) {
    request.observation.observed_wall_clock = observed.value();
  }
  // An observation carries no authority scope: it is a measurement, not a
  // lifecycle decision, and the runtime never lets it become one.
  request.provenance = read_provenance(reader, request.plan, request.attempt, false, true, false);
  if (reader.failed()) {
    return reader.error();
  }
  return request;
}

[[nodiscard]] hw::Result<hw::AttestationRequest> build_attestation_request(const Arguments& args) {
  OptionReader reader(args);
  hw::AttestationRequest request;
  request.plan = reader.read<hw::PlanId>("--plan", &hw::PlanId::parse, hw::PlanId());
  request.attempt = reader.read<hw::AttemptId>("--attempt", &hw::AttemptId::parse, hw::AttemptId());
  request.key = read_object_key(reader, "--asset", "--hardware-generation");
  const std::optional<hw::Revision> revision =
      reader.read_optional<hw::Revision>("--expected-revision", &hw::Revision::parse);
  if (revision.has_value()) {
    request.expected_revision = revision.value();
  }
  const std::optional<hw::ControlEpoch> epoch =
      reader.read_optional<hw::ControlEpoch>("--expected-epoch", &hw::ControlEpoch::parse);
  if (epoch.has_value()) {
    request.expected_control_epoch = epoch.value();
  }
  request.provenance = read_provenance(reader, request.plan, request.attempt, true, true, false);
  if (reader.failed()) {
    return reader.error();
  }
  return request;
}

[[nodiscard]] hw::Result<hw::ReplacementRequest> build_replacement_request(const Arguments& args) {
  OptionReader reader(args);
  hw::ReplacementRequest request;
  request.plan = reader.read<hw::PlanId>("--plan", &hw::PlanId::parse, hw::PlanId());
  request.attempt = reader.read<hw::AttemptId>("--attempt", &hw::AttemptId::parse, hw::AttemptId());
  request.predecessor =
      reader.read<hw::ObjectKey>("--predecessor", &hw::parse_object_key, hw::ObjectKey());
  request.successor = reader.read<hw::ObjectKey>("--successor", &hw::parse_object_key, hw::ObjectKey());
  const std::optional<hw::Revision> revision =
      reader.read_optional<hw::Revision>("--expected-revision", &hw::Revision::parse);
  if (revision.has_value()) {
    request.expected_revision = revision.value();
  }
  const std::optional<hw::LifecycleGeneration> generation = reader.read_optional<hw::LifecycleGeneration>(
      "--expected-lifecycle-generation", &hw::LifecycleGeneration::parse);
  if (generation.has_value()) {
    request.expected_lifecycle_generation = generation.value();
  }
  const std::optional<hw::ControlEpoch> epoch =
      reader.read_optional<hw::ControlEpoch>("--expected-epoch", &hw::ControlEpoch::parse);
  if (epoch.has_value()) {
    request.expected_control_epoch = epoch.value();
  }
  request.provenance = read_provenance(reader, request.plan, request.attempt, true, true, false);
  if (reader.failed()) {
    return reader.error();
  }
  return request;
}

// ---------------------------------------------------------------------------
// Receipts
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue receipt_json(const hw::CreateReceipt& receipt) {
  JsonValue node = JsonValue::make_object();
  node.emplace("plan", json_text(receipt.plan.value()));
  node.emplace("attempt", json_text(receipt.attempt.value()));
  node.emplace("key", json_text(hw::to_string(receipt.key)));
  node.emplace("state", json_text(hw::to_string(receipt.state)));
  node.emplace("lifecycle_generation", json_number(receipt.lifecycle_generation.value()));
  node.emplace("revision", json_number(receipt.revision.value()));
  node.emplace("commit_sequence", json_number(receipt.commit_sequence.value()));
  node.emplace("logical_time", json_number(receipt.logical_time.value()));
  node.emplace("receipt_digest", json_text(receipt.receipt_digest.hex()));
  node.emplace("idempotent_replay", json_flag(receipt.idempotent_replay));
  return node;
}

[[nodiscard]] JsonValue receipt_json(const hw::TransitionReceipt& receipt) {
  JsonValue node = JsonValue::make_object();
  node.emplace("plan", json_text(receipt.plan.value()));
  node.emplace("attempt", json_text(receipt.attempt.value()));
  node.emplace("key", json_text(hw::to_string(receipt.key)));
  node.emplace("from", json_text(hw::to_string(receipt.from)));
  node.emplace("to", json_text(hw::to_string(receipt.to)));
  node.emplace("reason", json_text(hw::to_string(receipt.reason)));
  node.emplace("revision_before", json_number(receipt.revision_before.value()));
  node.emplace("revision_after", json_number(receipt.revision_after.value()));
  node.emplace("lifecycle_generation", json_number(receipt.lifecycle_generation.value()));
  node.emplace("gate_after", json_text(hw::to_string(receipt.gate_after)));
  node.emplace("commit_sequence", json_number(receipt.commit_sequence.value()));
  node.emplace("logical_time", json_number(receipt.logical_time.value()));
  node.emplace("request_digest", json_text(receipt.request_digest.hex()));
  node.emplace("entry_digest", json_text(receipt.entry_digest.hex()));
  node.emplace("receipt_digest", json_text(receipt.receipt_digest.hex()));
  node.emplace("idempotent_replay", json_flag(receipt.idempotent_replay));
  return node;
}

[[nodiscard]] JsonValue receipt_json(const hw::ReplacementReceipt& receipt) {
  JsonValue node = JsonValue::make_object();
  node.emplace("plan", json_text(receipt.plan.value()));
  node.emplace("attempt", json_text(receipt.attempt.value()));
  node.emplace("predecessor", json_text(hw::to_string(receipt.predecessor)));
  node.emplace("successor", json_text(hw::to_string(receipt.successor)));
  node.emplace("replacement_generation", json_number(receipt.replacement_generation.value()));
  node.emplace("commit_sequence", json_number(receipt.commit_sequence.value()));
  node.emplace("logical_time", json_number(receipt.logical_time.value()));
  node.emplace("link_digest", json_text(receipt.link_digest.hex()));
  node.emplace("request_digest", json_text(receipt.request_digest.hex()));
  node.emplace("receipt_digest", json_text(receipt.receipt_digest.hex()));
  node.emplace("idempotent_replay", json_flag(receipt.idempotent_replay));
  return node;
}

[[nodiscard]] JsonValue receipt_json(const hw::EligibilityGateReceipt& receipt) {
  JsonValue node = JsonValue::make_object();
  node.emplace("plan", json_text(receipt.plan.value()));
  node.emplace("attempt", json_text(receipt.attempt.value()));
  node.emplace("key", json_text(hw::to_string(receipt.key)));
  node.emplace("before", json_text(hw::to_string(receipt.before)));
  node.emplace("after", json_text(hw::to_string(receipt.after)));
  node.emplace("revision", json_number(receipt.revision.value()));
  node.emplace("commit_sequence", json_number(receipt.commit_sequence.value()));
  node.emplace("logical_time", json_number(receipt.logical_time.value()));
  node.emplace("request_digest", json_text(receipt.request_digest.hex()));
  node.emplace("receipt_digest", json_text(receipt.receipt_digest.hex()));
  node.emplace("idempotent_replay", json_flag(receipt.idempotent_replay));
  return node;
}

[[nodiscard]] JsonValue receipt_json(const hw::HealthReceipt& receipt) {
  JsonValue node = JsonValue::make_object();
  node.emplace("plan", json_text(receipt.plan.value()));
  node.emplace("attempt", json_text(receipt.attempt.value()));
  node.emplace("key", json_text(hw::to_string(receipt.key)));
  node.emplace("sequence", json_number(receipt.sequence.value()));
  node.emplace("commit_sequence", json_number(receipt.commit_sequence.value()));
  node.emplace("logical_time", json_number(receipt.logical_time.value()));
  node.emplace("request_digest", json_text(receipt.request_digest.hex()));
  node.emplace("receipt_digest", json_text(receipt.receipt_digest.hex()));
  node.emplace("idempotent_replay", json_flag(receipt.idempotent_replay));
  return node;
}

[[nodiscard]] JsonValue receipt_json(const hw::AttestationReceipt& receipt) {
  JsonValue node = JsonValue::make_object();
  node.emplace("plan", json_text(receipt.plan.value()));
  node.emplace("attempt", json_text(receipt.attempt.value()));
  node.emplace("key", json_text(hw::to_string(receipt.key)));
  node.emplace("revision", json_number(receipt.revision.value()));
  node.emplace("commit_sequence", json_number(receipt.commit_sequence.value()));
  node.emplace("logical_time", json_number(receipt.logical_time.value()));
  node.emplace("request_digest", json_text(receipt.request_digest.hex()));
  node.emplace("receipt_digest", json_text(receipt.receipt_digest.hex()));
  node.emplace("idempotent_replay", json_flag(receipt.idempotent_replay));
  return node;
}

// ---------------------------------------------------------------------------
// Store access
// ---------------------------------------------------------------------------

enum class Access { Query, Writer };

[[nodiscard]] hw::Result<hw::Runtime> open_runtime(const Arguments& args, Access access) {
  if (args.ephemeral) {
    return hw::Runtime::open_ephemeral();
  }
  // A query never takes the writer lock and never advances the fencing epoch;
  // --read-only asks the same of a mutating command, which the runtime then
  // refuses with read_only_authority.
  if (access == Access::Query || args.read_only) {
    return hw::Runtime::open_read_only(std::filesystem::path(args.store));
  }
  hw::OpenOptions options;
  options.root = std::filesystem::path(args.store);
  options.read_only = false;
  options.create_if_missing = true;
  return hw::Runtime::open(options);
}

[[nodiscard]] hw::Error ephemeral_refusal(std::string_view command, std::string_view reason) {
  return usage_error("--ephemeral cannot be combined with " + std::string(command) + ": " + std::string(reason));
}

/// Resolves the object an inspect, history or lineage names. An asset carries at
/// most one hardware generation on record, so an omitted generation is resolved
/// by looking the asset up rather than by inventing generation one.
[[nodiscard]] hw::Result<hw::ObjectKey> resolve_key(const hw::Runtime& runtime, const hw::AssetId& asset,
                                                    const std::optional<hw::HardwareGeneration>& generation) {
  if (generation.has_value()) {
    return hw::ObjectKey{asset, generation.value()};
  }
  const hw::Result<std::vector<hw::ObjectView>> objects = runtime.list(hw::ListQuery());
  if (!objects.has_value()) {
    return objects.error();
  }
  for (const hw::ObjectView& view : objects.value()) {
    if (view.object.key.asset == asset) {
      return view.object.key;
    }
  }
  return hw::Error::make(hw::ErrorCode::UnknownAsset, "no object with this identity is known")
      .with("asset", asset.value());
}

[[nodiscard]] hw::Result<std::string> read_text_file(const std::string& path) {
  std::error_code size_error;
  const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(path), size_error);
  if (size_error) {
    return hw::Error::make(hw::ErrorCode::IoFailure, "the file could not be read")
        .with("path", path)
        .with("detail", size_error.message());
  }
  if (size > static_cast<std::uintmax_t>(hw::limits::kMaxExportBytes)) {
    return hw::Error::make(hw::ErrorCode::LimitExceeded, "the document is above its maximum")
        .with("path", path)
        .with("bytes", std::to_string(size))
        .with("maximum", std::to_string(hw::limits::kMaxExportBytes));
  }
  std::ifstream stream(std::filesystem::path(path), std::ios::binary);
  if (!stream.is_open()) {
    return hw::Error::make(hw::ErrorCode::IoFailure, "the file could not be opened").with("path", path);
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  if (!content.empty()) {
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (stream.bad()) {
      return hw::Error::make(hw::ErrorCode::IoFailure, "the file could not be read").with("path", path);
    }
    content.resize(static_cast<std::size_t>(stream.gcount()));
  }
  return content;
}

[[nodiscard]] hw::Result<void> write_text_file(const std::string& path, std::string_view content) {
  std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return hw::Error::make(hw::ErrorCode::IoFailure, "the output file could not be opened").with("path", path);
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  stream.flush();
  if (!stream.good()) {
    return hw::Error::make(hw::ErrorCode::IoFailure, "the output file could not be written").with("path", path);
  }
  stream.close();
  if (!stream.good()) {
    return hw::Error::make(hw::ErrorCode::IoFailure, "the output file could not be closed").with("path", path);
  }
  return hw::ok();
}

// ---------------------------------------------------------------------------
// Shared renderings
// ---------------------------------------------------------------------------

[[nodiscard]] std::string binding_text(std::optional<hw::Revision> revision,
                                       std::optional<hw::LifecycleGeneration> generation,
                                       std::optional<hw::ControlEpoch> epoch) {
  std::string text;
  if (revision.has_value()) {
    text += "revision=" + std::to_string(revision->value());
  }
  if (generation.has_value()) {
    if (!text.empty()) {
      text.push_back(' ');
    }
    text += "lifecycle_generation=" + std::to_string(generation->value());
  }
  if (epoch.has_value()) {
    if (!text.empty()) {
      text.push_back(' ');
    }
    text += "epoch=" + std::to_string(epoch->value());
  }
  if (text.empty()) {
    return "none";
  }
  return text;
}

[[nodiscard]] std::string_view durability_text(hw::DurabilityClass durability) {
  return durability == hw::DurabilityClass::Durable ? "durable" : "ephemeral";
}

[[nodiscard]] JsonValue object_summary_json(const hw::ObjectView& view) {
  const hw::HardwareObject& object = view.object;
  JsonValue node = JsonValue::make_object();
  node.emplace("key", json_text(hw::to_string(object.key)));
  node.emplace("state", json_text(hw::to_string(object.state)));
  node.emplace("revision", json_number(object.revision.value()));
  node.emplace("lifecycle_generation", json_number(object.lifecycle_generation.value()));
  node.emplace("kind", json_text(hw::to_string(object.kind)));
  node.emplace("model", json_text(object.model.value()));
  if (object.location.has_value()) {
    node.emplace("location", json_text(hw::to_string(object.location.value())));
  } else {
    node.emplace("location", JsonValue::make_null());
  }
  node.emplace("eligibility_gate", json_text(hw::to_string(object.eligibility_gate)));
  node.emplace("service_eligibility", json_text(hw::to_string(view.eligibility)));
  node.emplace("authority", json_text(hw::to_string(object.authority)));
  return node;
}

[[nodiscard]] std::string object_summary_line(const hw::ObjectView& view) {
  const hw::HardwareObject& object = view.object;
  std::string line = hw::to_string(object.key);
  line += " state=" + std::string(hw::to_string(object.state));
  line += " revision=" + std::to_string(object.revision.value());
  line += " lifecycle_generation=" + std::to_string(object.lifecycle_generation.value());
  line += " kind=" + std::string(hw::to_string(object.kind));
  line += " model=" + object.model.value();
  line += " location=";
  line += object.location.has_value() ? hw::to_string(object.location.value()) : std::string("none");
  line += " gate=" + std::string(hw::to_string(object.eligibility_gate));
  line += " service_eligibility=" + std::string(hw::to_string(view.eligibility));
  line += " authority=" + std::string(hw::to_string(object.authority));
  return line;
}

// ---------------------------------------------------------------------------
// Commands that need no store
// ---------------------------------------------------------------------------

[[nodiscard]] int command_version(Output& out) {
  const hw::Version version = hw::library_version();
  out.text("name", "hwlifecycle");
  out.text("version", hw::library_version_string());
  out.number("major", version.major);
  out.number("minor", version.minor);
  out.number("patch", version.patch);
  out.number("journal_format_version", hw::kJournalFormatVersion);
  out.number("manifest_format_version", hw::kManifestFormatVersion);
  out.number("export_format_version", hw::kExportFormatVersion);
  return kExitSuccess;
}

void add_report_lines(Output& out, std::string_view name, std::string_view report) {
  JsonValue items = JsonValue::make_array();
  for (const std::string& line : split_lines(report)) {
    items.push_back(json_text(line));
  }
  out.member(name, std::move(items));
}

[[nodiscard]] int command_limits(Output& out) {
  add_report_lines(out, "limits", hw::limits::limits_report());
  return kExitSuccess;
}

[[nodiscard]] int command_compatibility(Output& out) {
  add_report_lines(out, "formats", hw::compatibility_matrix());
  return kExitSuccess;
}

[[nodiscard]] int command_diagram(Output& out) {
  add_report_lines(out, "states", hw::lifecycle_machine_diagram());
  return kExitSuccess;
}

[[nodiscard]] int command_legal_transitions(const Arguments& args, Output& out) {
  OptionReader reader(args);
  const std::optional<hw::LifecycleState> state =
      reader.read_optional<hw::LifecycleState>("--state", &hw::parse_lifecycle_state_lenient);
  if (reader.failed()) {
    return report_usage(out, reader.error());
  }
  if (state.has_value()) {
    const std::vector<hw::LifecycleState> targets = hw::legal_targets(state.value());
    out.text("state", hw::to_string(state.value()));
    out.number("target_count", static_cast<std::uint64_t>(targets.size()));
    JsonValue items = JsonValue::make_array();
    for (const hw::LifecycleState target : targets) {
      items.push_back(json_text(hw::to_string(target)));
    }
    out.member("targets", std::move(items));
    return kExitSuccess;
  }
  const std::span<const hw::TransitionRule> rules = hw::transition_rules();
  out.number("rule_count", static_cast<std::uint64_t>(rules.size()));
  JsonValue items = JsonValue::make_array();
  for (const hw::TransitionRule& rule : rules) {
    std::string edge(hw::to_string(rule.from));
    edge += " -> ";
    edge += hw::to_string(rule.to);
    items.push_back(json_text(edge));
  }
  out.member("rules", std::move(items));
  return kExitSuccess;
}

[[nodiscard]] int command_explain(const Arguments& args, Output& out) {
  OptionReader reader(args);
  const hw::LifecycleState from =
      reader.read<hw::LifecycleState>("--from", &hw::parse_lifecycle_state_lenient, hw::LifecycleState::Ordered);
  const hw::LifecycleState to =
      reader.read<hw::LifecycleState>("--to", &hw::parse_lifecycle_state_lenient, hw::LifecycleState::Ordered);
  const std::optional<hw::TransitionReason> reason =
      reader.read_optional<hw::TransitionReason>("--reason", &hw::parse_transition_reason);
  const std::optional<hw::AuthorityMask> authority =
      reader.read_optional<hw::AuthorityMask>("--authority", &hw::parse_authority_mask);
  const std::vector<hw::EvidenceRef> evidence = reader.read_evidence("--evidence", false);
  if (reader.failed()) {
    return report_usage(out, reader.error());
  }

  out.text("from", hw::to_string(from));
  out.text("to", hw::to_string(to));
  if (const hw::TransitionRule* rule = hw::find_rule(from, to)) {
    out.text("outcome", "legal");
    out.text("rule", hw::describe_rule(*rule));
    return kExitSuccess;
  }

  // The pair is not an edge of the table. The answer is whatever the one
  // validator says about a synthetic request built from the supplied reason,
  // authority and evidence; the tool never invents a rejection of its own. The
  // probe runs against an ephemeral runtime, so no store is opened.
  const hw::Result<hw::Runtime> runtime = hw::Runtime::open_ephemeral();
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::PlanId> plan = hw::PlanId::parse("explain");
  const hw::Result<hw::AttemptId> attempt = hw::AttemptId::parse("static");
  const hw::Result<hw::AssetId> asset = hw::AssetId::parse("explain-probe");
  if (!plan.has_value() || !attempt.has_value() || !asset.has_value()) {
    return report_failure(out,
                          hw::Error::make(hw::ErrorCode::Internal, "the probe identity is not a valid identifier"));
  }

  hw::TransitionRequest request;
  request.plan = plan.value();
  request.attempt = attempt.value();
  request.key.asset = asset.value();
  request.key.hardware_generation = hw::HardwareGeneration::first();
  request.expected_revision = hw::Revision::first();
  request.expected_lifecycle_generation = hw::LifecycleGeneration::first();
  request.expected_control_epoch = hw::ControlEpoch::first();
  request.expected_state = from;
  request.target_state = to;
  request.reason = reason.value_or(hw::TransitionReason::Unset);
  request.provenance.plan = request.plan;
  request.provenance.attempt = request.attempt;
  if (authority.has_value()) {
    request.provenance.authority = authority.value();
  }
  request.provenance.evidence = evidence;

  out.text("reason", hw::to_string(request.reason));
  out.text("authority", hw::to_string(request.provenance.authority));
  out.number("evidence_count", static_cast<std::uint64_t>(request.provenance.evidence.size()));

  const hw::Result<hw::PreflightReport> report = runtime.value().preflight_transition(request);
  if (!report.has_value()) {
    return report_failure(out, report.error());
  }
  const hw::PreflightReport& preflight = report.value();
  out.text("outcome", hw::to_string(preflight.outcome));
  if (preflight.outcome == hw::PreflightOutcome::Legal) {
    if (preflight.transition.rule != nullptr) {
      out.text("rule", hw::describe_rule(*preflight.transition.rule));
    }
    return kExitSuccess;
  }
  out.member("error", error_json(preflight.primary_error));
  return kExitSuccess;
}

[[nodiscard]] int command_diff(const Arguments& args, Output& out) {
  const std::string* before_path = value_of(args, "--before");
  const std::string* after_path = value_of(args, "--after");
  const hw::Result<std::string> before_text = read_text_file(*before_path);
  if (!before_text.has_value()) {
    return report_failure(out, before_text.error());
  }
  const hw::Result<std::string> after_text = read_text_file(*after_path);
  if (!after_text.has_value()) {
    return report_failure(out, after_text.error());
  }
  const hw::Result<hw::Snapshot> before = hw::from_json(before_text.value());
  if (!before.has_value()) {
    return report_failure(out, before.error());
  }
  const hw::Result<hw::Snapshot> after = hw::from_json(after_text.value());
  if (!after.has_value()) {
    return report_failure(out, after.error());
  }
  const hw::Result<hw::SnapshotDiff> diff = hw::diff_snapshots(before.value(), after.value());
  if (!diff.has_value()) {
    return report_failure(out, diff.error());
  }
  if (args.json) {
    out.set_document(hw::to_json(diff.value(), !args.compact));
  } else {
    out.set_document(hw::describe(diff.value()));
  }
  return kExitSuccess;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

[[nodiscard]] int command_create(const Arguments& args, Output& out) {
  const hw::Result<hw::CreateRequest> request = build_create_request(args);
  if (!request.has_value()) {
    return report_usage(out, request.error());
  }
  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  out.text("binding", "none");
  const hw::Result<hw::CreateReceipt> receipt = runtime.value().create_object(request.value());
  if (!receipt.has_value()) {
    return report_failure(out, receipt.error());
  }
  out.member("receipt", receipt_json(receipt.value()));
  return kExitSuccess;
}

/// Re-establishes authority over one object in this session, on request.
///
/// Every object a restart recovers is marked Recovered and the runtime refuses
/// to change it until an attestation re-establishes authority in this session.
/// A command line invocation is its own process, so a scripted workflow says so
/// explicitly, and the attestation becomes a durable record with its own
/// receipt. An object that already has live authority in this session needs
/// nothing, and then nothing is printed.
[[nodiscard]] hw::Result<void> attest_for_session(hw::Runtime& runtime, const hw::ObjectKey& key,
                                                  const hw::Provenance& provenance, Output& out) {
  const hw::Result<hw::ObjectView> view = runtime.inspect(key);
  if (!view.has_value()) {
    return view.error();
  }
  if (view.value().object.authority == hw::AuthorityState::Live) {
    return hw::ok();
  }
  // The attestation is a fact about this session, so its plan identity carries
  // the epoch this session holds. Two sessions attesting the same object under
  // the same plan are two distinct facts, and a retry in a later session cannot
  // collide with the attestation the earlier session recorded.
  const hw::Result<hw::PlanId> plan = hw::PlanId::parse(
      provenance.plan.value() + "-attest-e" + std::to_string(runtime.registry().control_epoch().value()));
  if (!plan.has_value()) {
    return plan.error();
  }
  hw::AttestationRequest request;
  request.plan = plan.value();
  request.attempt = provenance.attempt;
  request.key = key;
  request.expected_revision = view.value().object.revision;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.provenance = provenance;
  request.provenance.plan = plan.value();
  const hw::Result<hw::AttestationReceipt> receipt = runtime.attest_authority(request);
  if (!receipt.has_value()) {
    return receipt.error();
  }
  out.text("attest.plan", receipt.value().plan.value());
  out.text("attest.key", hw::to_string(receipt.value().key));
  out.number("attest.revision", receipt.value().revision.value());
  out.number("attest.commit_sequence", receipt.value().commit_sequence.value());
  out.number("attest.logical_time", receipt.value().logical_time.value());
  out.text("attest.receipt_digest", receipt.value().receipt_digest.hex());
  return hw::ok();
}

[[nodiscard]] int command_transition(const Arguments& args, Output& out) {
  const hw::Result<TransitionPlan> plan = build_transition_plan(args);
  if (!plan.has_value()) {
    return report_usage(out, plan.error());
  }
  hw::TransitionRequest request = plan.value().request;

  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::ObjectView> view = runtime.value().inspect(request.key);
  if (!view.has_value()) {
    return report_failure(out, view.error());
  }
  const hw::HardwareObject& object = view.value().object;
  if (!request.expected_revision.valid()) {
    request.expected_revision = object.revision;
  }
  if (!request.expected_lifecycle_generation.valid()) {
    request.expected_lifecycle_generation = object.lifecycle_generation;
  }
  if (!request.expected_control_epoch.valid()) {
    request.expected_control_epoch = runtime.value().registry().control_epoch();
  }
  if (!plan.value().expected_state_given) {
    request.expected_state = object.state;
  }
  out.text("binding", binding_text(request.expected_revision, request.expected_lifecycle_generation,
                                   request.expected_control_epoch));
  out.text("expected_state", hw::to_string(request.expected_state));

  if (has_option(args, "--attest")) {
    const hw::Result<void> attested = attest_for_session(runtime.value(), request.key, request.provenance, out);
    if (!attested.has_value()) {
      return report_failure(out, attested.error());
    }
  }

  const hw::Result<hw::TransitionReceipt> receipt = runtime.value().apply_transition(request);
  if (!receipt.has_value()) {
    return report_failure(out, receipt.error());
  }
  out.member("receipt", receipt_json(receipt.value()));
  return kExitSuccess;
}

[[nodiscard]] int command_gate(const Arguments& args, Output& out) {
  const hw::Result<hw::EligibilityGateRequest> request = build_gate_request(args);
  if (!request.has_value()) {
    return report_usage(out, request.error());
  }
  hw::EligibilityGateRequest gate = request.value();

  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::ObjectView> view = runtime.value().inspect(gate.key);
  if (!view.has_value()) {
    return report_failure(out, view.error());
  }
  const hw::HardwareObject& object = view.value().object;
  if (!gate.expected_revision.valid()) {
    gate.expected_revision = object.revision;
  }
  if (!gate.expected_lifecycle_generation.valid()) {
    gate.expected_lifecycle_generation = object.lifecycle_generation;
  }
  if (!gate.expected_control_epoch.valid()) {
    gate.expected_control_epoch = runtime.value().registry().control_epoch();
  }
  out.text("binding", binding_text(gate.expected_revision, gate.expected_lifecycle_generation,
                                   gate.expected_control_epoch));

  if (has_option(args, "--attest")) {
    const hw::Result<void> attested = attest_for_session(runtime.value(), gate.key, gate.provenance, out);
    if (!attested.has_value()) {
      return report_failure(out, attested.error());
    }
  }

  const hw::Result<hw::EligibilityGateReceipt> receipt = runtime.value().set_eligibility_gate(gate);
  if (!receipt.has_value()) {
    return report_failure(out, receipt.error());
  }
  out.member("receipt", receipt_json(receipt.value()));
  return kExitSuccess;
}

[[nodiscard]] int command_observe_health(const Arguments& args, Output& out) {
  const hw::Result<hw::HealthObservationRequest> request = build_observation_request(args);
  if (!request.has_value()) {
    return report_usage(out, request.error());
  }
  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  out.text("binding", "none");
  const hw::Result<hw::HealthReceipt> receipt = runtime.value().observe_health(request.value());
  if (!receipt.has_value()) {
    return report_failure(out, receipt.error());
  }
  out.member("receipt", receipt_json(receipt.value()));
  return kExitSuccess;
}

[[nodiscard]] int command_attest(const Arguments& args, Output& out) {
  const hw::Result<hw::AttestationRequest> request = build_attestation_request(args);
  if (!request.has_value()) {
    return report_usage(out, request.error());
  }
  hw::AttestationRequest attestation = request.value();

  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::ObjectView> view = runtime.value().inspect(attestation.key);
  if (!view.has_value()) {
    return report_failure(out, view.error());
  }
  const hw::HardwareObject& object = view.value().object;
  if (!attestation.expected_revision.valid()) {
    attestation.expected_revision = object.revision;
  }
  if (!attestation.expected_control_epoch.valid()) {
    attestation.expected_control_epoch = runtime.value().registry().control_epoch();
  }
  out.text("binding", binding_text(attestation.expected_revision, std::nullopt, attestation.expected_control_epoch));

  const hw::Result<hw::AttestationReceipt> receipt = runtime.value().attest_authority(attestation);
  if (!receipt.has_value()) {
    return report_failure(out, receipt.error());
  }
  out.member("receipt", receipt_json(receipt.value()));
  // Authority is session scoped by design, so the effect of an attestation can
  // only be observed inside the session that performed it: a later invocation
  // opens the store recovered again, and this field would read "recovered".
  const hw::Result<hw::ObjectView> after = runtime.value().inspect(attestation.key);
  if (after.has_value()) {
    out.text("authority_in_this_session", hw::to_string(after.value().object.authority));
  }
  return kExitSuccess;
}

[[nodiscard]] int command_link_replacement(const Arguments& args, Output& out) {
  const hw::Result<hw::ReplacementRequest> request = build_replacement_request(args);
  if (!request.has_value()) {
    return report_usage(out, request.error());
  }
  hw::ReplacementRequest replacement = request.value();

  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  // The expectations bind to the predecessor: it is the object that gains the
  // successor reference, and the validator checks that object's generation.
  const hw::Result<hw::ObjectView> view = runtime.value().inspect(replacement.predecessor);
  if (!view.has_value()) {
    return report_failure(out, view.error());
  }
  const hw::HardwareObject& object = view.value().object;
  if (!replacement.expected_revision.valid()) {
    replacement.expected_revision = object.revision;
  }
  if (!replacement.expected_lifecycle_generation.valid()) {
    replacement.expected_lifecycle_generation = object.lifecycle_generation;
  }
  if (!replacement.expected_control_epoch.valid()) {
    replacement.expected_control_epoch = runtime.value().registry().control_epoch();
  }
  out.text("binding", binding_text(replacement.expected_revision, replacement.expected_lifecycle_generation,
                                   replacement.expected_control_epoch));

  if (has_option(args, "--attest")) {
    // A replacement link mutates both objects, so both need live authority.
    const hw::Result<void> predecessor =
        attest_for_session(runtime.value(), replacement.predecessor, replacement.provenance, out);
    if (!predecessor.has_value()) {
      return report_failure(out, predecessor.error());
    }
    const hw::Result<void> successor =
        attest_for_session(runtime.value(), replacement.successor, replacement.provenance, out);
    if (!successor.has_value()) {
      return report_failure(out, successor.error());
    }
  }

  const hw::Result<hw::ReplacementReceipt> receipt = runtime.value().link_replacement(replacement);
  if (!receipt.has_value()) {
    return report_failure(out, receipt.error());
  }
  out.member("receipt", receipt_json(receipt.value()));
  return kExitSuccess;
}

// ---------------------------------------------------------------------------
// Queries against a store
// ---------------------------------------------------------------------------

[[nodiscard]] int command_inspect(const Arguments& args, Output& out) {
  OptionReader reader(args);
  const hw::AssetId asset = reader.read<hw::AssetId>("--asset", &hw::AssetId::parse, hw::AssetId());
  const std::optional<hw::HardwareGeneration> generation =
      reader.read_optional<hw::HardwareGeneration>("--hardware-generation", &hw::HardwareGeneration::parse);
  if (reader.failed()) {
    return report_usage(out, reader.error());
  }

  const hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Query);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::ObjectKey> key = resolve_key(runtime.value(), asset, generation);
  if (!key.has_value()) {
    return report_failure(out, key.error());
  }
  const hw::Result<hw::ObjectView> view = runtime.value().inspect(key.value());
  if (!view.has_value()) {
    const int code = report_failure(out, view.error());
    // A lookup that finds nothing is the common operator mistake, so the tool
    // says what to run next instead of leaving a bare key in the error note.
    if (view.error().code == hw::ErrorCode::UnknownAsset) {
      out.text("hint", "no object carries that asset id; list shows every key on record");
    } else if (view.error().code == hw::ErrorCode::StaleHardwareGeneration) {
      out.text("hint", "the asset is on record under a different hardware generation; list shows it");
    }
    return code;
  }

  const hw::ObjectView& object_view = view.value();
  const hw::HardwareObject& object = object_view.object;
  out.text("key", hw::to_string(object.key));
  out.text("asset", object.key.asset.value());
  out.number("hardware_generation", object.key.hardware_generation.value());
  out.text("state", hw::to_string(object.state));
  out.text("state_class", hw::to_string(hw::state_class(object.state)));
  out.number("revision", object.revision.value());
  out.number("lifecycle_generation", object.lifecycle_generation.value());
  out.text("kind", hw::to_string(object.kind));
  out.text("model", object.model.value());
  if (object.firmware_generation.valid()) {
    out.number("firmware_generation", object.firmware_generation.value());
  } else {
    out.absent("firmware_generation");
  }
  if (object.location.has_value()) {
    out.text("location", hw::to_string(object.location.value()));
  } else {
    out.absent("location");
  }
  out.text("eligibility_gate", hw::to_string(object.eligibility_gate));
  out.text("service_eligibility", hw::to_string(object_view.eligibility));
  out.text("authority", hw::to_string(object.authority));

  const hw::HealthSnapshot& snapshot = object_view.health;
  JsonValue health = JsonValue::make_object();
  health.emplace("present", json_flag(snapshot.present));
  health.emplace("freshness", json_text(hw::to_string(snapshot.freshness)));
  if (snapshot.present) {
    health.emplace("health", json_text(hw::to_string(snapshot.observation.health)));
    health.emplace("readiness", json_text(hw::to_string(snapshot.observation.readiness)));
    health.emplace("availability", json_text(hw::to_string(snapshot.observation.availability)));
    health.emplace("sequence", json_number(snapshot.sequence.value()));
    health.emplace("recorded_at", json_number(snapshot.recorded_at.value()));
    health.emplace("source", json_text(snapshot.observation.source));
    health.emplace("evidence_digest", json_text(snapshot.observation.evidence_digest.hex()));
    health.emplace("observed_wall_clock", snapshot.observation.observed_wall_clock.has_value()
                                              ? json_text(snapshot.observation.observed_wall_clock.value())
                                              : JsonValue::make_null());
  } else {
    health.emplace("health", JsonValue::make_null());
    health.emplace("readiness", JsonValue::make_null());
    health.emplace("availability", JsonValue::make_null());
    health.emplace("sequence", JsonValue::make_null());
    health.emplace("recorded_at", JsonValue::make_null());
    health.emplace("source", JsonValue::make_null());
    health.emplace("evidence_digest", JsonValue::make_null());
    health.emplace("observed_wall_clock", JsonValue::make_null());
  }
  out.member("health", std::move(health));

  out.number("history_entries", object.history_entries);
  if (object.predecessor.has_value()) {
    out.text("predecessor", hw::to_string(object.predecessor.value()));
  } else {
    out.absent("predecessor");
  }
  if (object.successor.has_value()) {
    out.text("successor", hw::to_string(object.successor.value()));
  } else {
    out.absent("successor");
  }
  if (object.replacement_generation.valid()) {
    out.number("replacement_generation", object.replacement_generation.value());
  } else {
    out.absent("replacement_generation");
  }
  return kExitSuccess;
}

[[nodiscard]] int command_list(const Arguments& args, Output& out) {
  OptionReader reader(args);
  hw::ListQuery query;
  query.state = reader.read_optional<hw::LifecycleState>("--state", &hw::parse_lifecycle_state_lenient);
  query.kind = reader.read_optional<hw::HardwareKind>("--kind", &hw::parse_hardware_kind);
  query.site = reader.read_optional<hw::SiteId>("--site", &hw::SiteId::parse);
  query.eligibility = reader.read_optional<hw::ServiceEligibility>("--eligibility", &parse_service_eligibility);
  if (reader.failed()) {
    return report_usage(out, reader.error());
  }

  const hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Query);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<std::vector<hw::ObjectView>> objects = runtime.value().list(query);
  if (!objects.has_value()) {
    return report_failure(out, objects.error());
  }

  JsonValue items = JsonValue::make_array();
  std::vector<std::string> lines;
  lines.push_back("command: list");
  lines.push_back("count: " + std::to_string(objects.value().size()));
  // The registry is key ordered, so the listing is key ordered without a sort.
  for (const hw::ObjectView& view : objects.value()) {
    lines.push_back("object: " + object_summary_line(view));
    items.push_back(object_summary_json(view));
  }
  out.number("count", static_cast<std::uint64_t>(objects.value().size()));
  out.member("objects", std::move(items));
  out.set_lines(std::move(lines));
  return kExitSuccess;
}

[[nodiscard]] int command_history(const Arguments& args, Output& out) {
  OptionReader reader(args);
  const hw::AssetId asset = reader.read<hw::AssetId>("--asset", &hw::AssetId::parse, hw::AssetId());
  const std::optional<hw::HardwareGeneration> generation =
      reader.read_optional<hw::HardwareGeneration>("--hardware-generation", &hw::HardwareGeneration::parse);
  if (reader.failed()) {
    return report_usage(out, reader.error());
  }

  const hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Query);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::ObjectKey> key = resolve_key(runtime.value(), asset, generation);
  if (!key.has_value()) {
    return report_failure(out, key.error());
  }
  const hw::Result<hw::HistoryView> history = runtime.value().history(key.value());
  if (!history.has_value()) {
    return report_failure(out, history.error());
  }

  JsonValue items = JsonValue::make_array();
  std::vector<std::string> lines;
  lines.push_back("command: history");
  lines.push_back("key: " + hw::to_string(history.value().key));
  lines.push_back("entry_count: " + std::to_string(history.value().entries.size()));
  for (const hw::HistoryEntry& entry : history.value().entries) {
    // One durable fact per line, exactly the fields the contract names.
    std::string line = std::to_string(entry.commit_sequence.value());
    line += " " + std::to_string(entry.logical_time.value());
    line += " " + std::string(hw::to_string(entry.from));
    line += " -> " + std::string(hw::to_string(entry.to));
    line += " " + std::string(hw::to_string(entry.reason));
    line += " " + std::to_string(entry.revision_after.value());
    line += " " + entry.chain_digest.hex();
    lines.push_back(line);

    JsonValue item = JsonValue::make_object();
    item.emplace("sequence", json_number(entry.commit_sequence.value()));
    item.emplace("logical_time", json_number(entry.logical_time.value()));
    item.emplace("from", json_text(hw::to_string(entry.from)));
    item.emplace("to", json_text(hw::to_string(entry.to)));
    item.emplace("reason", json_text(hw::to_string(entry.reason)));
    item.emplace("revision_after", json_number(entry.revision_after.value()));
    item.emplace("chain_digest", json_text(entry.chain_digest.hex()));
    items.push_back(std::move(item));
  }
  lines.push_back("chain_head: " + history.value().chain_head.hex());
  out.text("key", hw::to_string(history.value().key));
  out.number("entry_count", static_cast<std::uint64_t>(history.value().entries.size()));
  out.member("entries", std::move(items));
  out.text("chain_head", history.value().chain_head.hex());
  out.set_lines(std::move(lines));
  return kExitSuccess;
}

[[nodiscard]] int command_lineage(const Arguments& args, Output& out) {
  OptionReader reader(args);
  const hw::AssetId asset = reader.read<hw::AssetId>("--asset", &hw::AssetId::parse, hw::AssetId());
  const std::optional<hw::HardwareGeneration> generation =
      reader.read_optional<hw::HardwareGeneration>("--hardware-generation", &hw::HardwareGeneration::parse);
  if (reader.failed()) {
    return report_usage(out, reader.error());
  }

  const hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Query);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::ObjectKey> key = resolve_key(runtime.value(), asset, generation);
  if (!key.has_value()) {
    return report_failure(out, key.error());
  }
  const hw::Result<hw::LineageView> lineage = runtime.value().lineage(key.value());
  if (!lineage.has_value()) {
    return report_failure(out, lineage.error());
  }

  JsonValue ancestors = JsonValue::make_array();
  for (const hw::ObjectKey& item : lineage.value().ancestors) {
    ancestors.push_back(json_text(hw::to_string(item)));
  }
  JsonValue successors = JsonValue::make_array();
  for (const hw::ObjectKey& item : lineage.value().successors) {
    successors.push_back(json_text(hw::to_string(item)));
  }
  JsonValue links = JsonValue::make_array();
  for (const hw::ReplacementRecord& record : lineage.value().links) {
    JsonValue item = JsonValue::make_object();
    item.emplace("predecessor", json_text(hw::to_string(record.predecessor)));
    item.emplace("successor", json_text(hw::to_string(record.successor)));
    item.emplace("reason", json_text(hw::to_string(record.reason)));
    item.emplace("replacement_generation", json_number(record.replacement_generation.value()));
    item.emplace("linked_at", json_number(record.linked_at.value()));
    item.emplace("commit_sequence", json_number(record.commit_sequence.value()));
    item.emplace("link_digest", json_text(record.link_digest.hex()));
    links.push_back(std::move(item));
  }

  // The counts are printed as well as the lists: an empty list is a real answer
  // ("no ancestor on record") and must not read as a value the tool forgot.
  out.number("ancestor_count", static_cast<std::uint64_t>(lineage.value().ancestors.size()));
  out.member("ancestors", std::move(ancestors));
  out.text("self", hw::to_string(lineage.value().key));
  out.number("successor_count", static_cast<std::uint64_t>(lineage.value().successors.size()));
  out.member("successors", std::move(successors));
  out.number("link_count", static_cast<std::uint64_t>(lineage.value().links.size()));
  out.member("links", std::move(links));
  return kExitSuccess;
}

[[nodiscard]] int command_recover(const Arguments& args, Output& out) {
  if (args.ephemeral) {
    return report_usage(out, ephemeral_refusal("recover", "a recovery report is a statement about durable bytes"));
  }
  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::RuntimeStatus> status = runtime.value().status();
  if (!status.has_value()) {
    return report_failure(out, status.error());
  }
  const hw::RecoveryReport& report = status.value().recovery;
  JsonValue recovery = JsonValue::make_object();
  recovery.emplace("commit_sequence", json_number(report.commit_sequence.value()));
  recovery.emplace("logical_time", json_number(report.logical_time.value()));
  recovery.emplace("control_epoch", json_number(report.control_epoch.value()));
  recovery.emplace("incarnation", json_number(report.incarnation.value()));
  recovery.emplace("record_count", json_number(report.record_count));
  recovery.emplace("segment_bytes", json_number(report.segment_bytes));
  recovery.emplace("discarded_tail_bytes", json_number(report.discarded_tail_bytes));
  recovery.emplace("log_digest", json_text(report.log_digest.hex()));
  recovery.emplace("state_digest", json_text(report.state_digest.hex()));
  recovery.emplace("fence_advanced", json_flag(report.fence_advanced));
  recovery.emplace("existing_state", json_flag(report.existing_state));
  out.member("recovery", std::move(recovery));
  out.text("durability", durability_text(status.value().durability));
  out.flag("read_only", status.value().read_only);
  out.number("object_count", static_cast<std::uint64_t>(status.value().object_count));
  return kExitSuccess;
}

[[nodiscard]] int command_verify(const Arguments& args, Output& out) {
  if (args.ephemeral) {
    return report_usage(out, ephemeral_refusal("verify", "verification re-reads the durable store"));
  }
  // Verification never takes writer authority: it must not advance the fencing
  // epoch of the generation it is checking.
  const hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Query);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Result<hw::Digest> log_digest = runtime.value().verify();
  if (!log_digest.has_value()) {
    return report_failure(out, log_digest.error());
  }
  out.text("log_digest", log_digest.value().hex());
  out.text("state", "verified");
  return kExitSuccess;
}

[[nodiscard]] int command_export(const Arguments& args, Output& out) {
  const std::string* path = value_of(args, "--out");
  hw::ExportOptions options;
  options.pretty = !args.compact;
  const hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Query);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  const hw::Snapshot snapshot = runtime.value().snapshot(options);
  const hw::Result<std::string> document = hw::to_json(snapshot, options);
  if (!document.has_value()) {
    return report_failure(out, document.error());
  }
  const hw::Result<void> written = write_text_file(*path, document.value());
  if (!written.has_value()) {
    return report_failure(out, written.error());
  }
  out.text("out", *path);
  out.flag("compact", args.compact);
  out.text("library_version", snapshot.header.library_version);
  out.number("format_version", snapshot.header.format_version);
  out.text("state_digest", snapshot.header.state_digest.hex());
  out.number("commit_sequence", snapshot.header.commit_sequence.value());
  out.number("logical_time", snapshot.header.logical_time.value());
  out.number("observation_sequence", snapshot.header.observation_sequence.value());
  out.number("control_epoch", snapshot.header.control_epoch.value());
  out.number("incarnation", snapshot.header.incarnation.value());
  out.number("object_count", static_cast<std::uint64_t>(snapshot.header.object_count));
  out.number("history_entry_count", static_cast<std::uint64_t>(snapshot.header.history_entry_count));
  out.number("lineage_link_count", static_cast<std::uint64_t>(snapshot.header.lineage_link_count));
  out.number("applied_plan_count", static_cast<std::uint64_t>(snapshot.header.applied_plan_count));
  out.number("event_count", static_cast<std::uint64_t>(snapshot.header.event_count));
  out.flag("importable", snapshot.header.importable);
  out.number("bytes", static_cast<std::uint64_t>(document.value().size()));
  return kExitSuccess;
}

[[nodiscard]] int command_import(const Arguments& args, Output& out) {
  if (args.ephemeral) {
    return report_usage(out, ephemeral_refusal("import", "an import that is not durable would report a state that "
                                                          "nothing holds"));
  }
  const std::string* path = value_of(args, "--in");
  const hw::Result<std::string> document = read_text_file(*path);
  if (!document.has_value()) {
    return report_failure(out, document.error());
  }
  hw::Result<hw::Runtime> runtime = open_runtime(args, Access::Writer);
  if (!runtime.has_value()) {
    return report_failure(out, runtime.error());
  }
  hw::ImportOptions options;
  const hw::Result<hw::ImportReport> report = runtime.value().import_document(document.value(), options);
  if (!report.has_value()) {
    return report_failure(out, report.error());
  }
  out.text("in", *path);
  out.number("events", static_cast<std::uint64_t>(report.value().events));
  out.number("objects", static_cast<std::uint64_t>(report.value().objects));
  out.number("history_entries", static_cast<std::uint64_t>(report.value().history_entries));
  out.number("lineage_links", static_cast<std::uint64_t>(report.value().lineage_links));
  out.number("applied_plans", static_cast<std::uint64_t>(report.value().applied_plans));
  out.number("health_observations", static_cast<std::uint64_t>(report.value().health_observations));
  out.text("declared_state_digest", report.value().declared_state_digest.hex());
  out.text("produced_state_digest", report.value().produced_state_digest.hex());
  out.number("commit_sequence", report.value().commit_sequence.value());
  out.number("logical_time", report.value().logical_time.value());
  return kExitSuccess;
}

// ---------------------------------------------------------------------------
// Help and dispatch
// ---------------------------------------------------------------------------

[[nodiscard]] std::string help_text() {
  return std::string(
      "hwlifecycle - the Hardware Lifecycle command line interface\n"
      "\n"
      "usage: hwlifecycle [global options] <command> [command options]\n"
      "\n"
      "global options:\n"
      "  --store <dir>   store root (default ./hardware-lifecycle-store)\n"
      "  --read-only     open the store without writer authority; every mutation\n"
      "                  is then refused by the runtime with read_only_authority\n"
      "  --ephemeral     keep everything in memory: no store is opened and nothing\n"
      "                  is written (refused by recover, verify and import, which\n"
      "                  make a claim about durable bytes)\n"
      "  --json          print exactly one JSON object instead of \"name: value\"\n"
      "                  lines\n"
      "  --compact       render JSON without indentation, and export the document\n"
      "                  without indentation\n"
      "  --quiet         print nothing when the command succeeds\n"
      "  --attest        accepted on any command line; only transition, gate and\n"
      "                  link-replacement act on it. They re-establish authority\n"
      "                  over every object they are about to change before changing\n"
      "                  it, because authority is never inherited across a process\n"
      "                  boundary: an object that a restart recovered is refused with\n"
      "                  stale_authority until an attestation in the current session\n"
      "                  lifts the fence. The attestation uses --authority (which must\n"
      "                  then also list recovery) and --evidence (which must then also\n"
      "                  carry a recovery_attestation reference), is recorded durably\n"
      "                  under the plan id <plan>-attest, and prints attest.* receipt\n"
      "                  lines. An object that already holds live authority in this\n"
      "                  session is left alone.\n"
      "  --help, -h      print this contract and exit 0\n"
      "  --version, -V   print the version and exit 0\n"
      "\n"
      "exit codes:\n"
      "  0  success\n"
      "  1  usage error: unknown option, missing operand, unparsable value\n"
      "  2  the runtime rejected the request; the stable snake_case error code and\n"
      "     its message are printed\n"
      "  3  durable store failure (is_store_failure)\n"
      "  4  internal error\n"
      "\n"
      "output:\n"
      "  every line is written to stdout, one fact per line, with no timestamp and\n"
      "  no ordering that depends on anything but the key; an absent optional value\n"
      "  prints as \"none\" and is never replaced by a default. --json prints exactly\n"
      "  one JSON object instead. A rejection prints \"error.code\", \"error.message\"\n"
      "  and one \"error.notes\" line per note, and prints no receipt: a receipt\n"
      "  exists only after the durable commit point.\n"
      "\n"
      "binding defaults:\n"
      "  one invocation holds writer authority for its whole duration. When\n"
      "  --expected-revision, --expected-lifecycle-generation or --expected-epoch\n"
      "  are omitted, the command reads the object's current values inside that\n"
      "  same session and binds them; when supplied they are used verbatim, which\n"
      "  is how a caller demonstrates stale-authority fencing. --expected-state\n"
      "  binds the same way. Every mutating command prints exactly what it bound to\n"
      "  as \"binding: revision=<n> lifecycle_generation=<n> epoch=<n>\" (\"binding:\n"
      "  none\" when the request carries no expectation) and then the full receipt.\n"
      "  A supplied expectation that no longer matches is a rejection, never a\n"
      "  repair.\n"
      "\n"
      "spellings:\n"
      "  an object key is written \"asset@generation\"; a lifecycle state is written\n"
      "  in canonical spelling and parsed case insensitively; a reason, kind, actor\n"
      "  kind, authority scope, health status and evidence kind use their canonical\n"
      "  spelling; evidence is written \"kind:digest_hex:source\" and may repeat;\n"
      "  --authority takes a comma separated list of scope spellings.\n"
      "\n"
      "commands:\n"
      "  version\n"
      "  limits\n"
      "  compatibility\n"
      "  diagram\n"
      "  legal-transitions [--state <State>]\n"
      "      every legal edge of the table, or the targets of one state\n"
      "  explain --from <State> --to <State> [--reason <Reason>]\n"
      "          [--authority <scopes>] [--evidence <ref>]\n"
      "      describe_rule for the pair when the edge exists; otherwise the\n"
      "      deterministic rejection Runtime::preflight_transition produces for a\n"
      "      synthetic request built from the supplied reason, authority and\n"
      "      evidence; the probe carries the unset reason when --reason is omitted,\n"
      "      so the validator answers that the probe itself is incomplete. No store\n"
      "      is opened and the exit code stays 0: the answer is the explanation,\n"
      "      not the rejection.\n"
      "  create --asset <id> --hardware-generation <n> --kind <kind> --model <id>\n"
      "         --plan <id> --attempt <id> --actor <id> --actor-kind <kind>\n"
      "         --authority <scopes> --evidence <ref>\n"
      "         [--firmware-generation <n>] [--state Ordered|Staged]\n"
      "         [--location site/rack/slot] [--wall-clock <rfc3339>]\n"
      "      --state defaults to Ordered, the only other registrable state being\n"
      "      Staged; nothing may be born installed, commissioned or active\n"
      "  transition --asset <id> --hardware-generation <n> --to <State>\n"
      "             --reason <Reason> --plan <id> --attempt <id> --actor <id>\n"
      "             --actor-kind <kind> --authority <scopes> --evidence <ref>\n"
      "             [--expected-revision <n>]\n"
      "             [--expected-lifecycle-generation <n>] [--expected-epoch <n>]\n"
      "             [--expected-state <State>] [--successor asset@gen]\n"
      "             [--location site/rack/slot]\n"
      "  gate --asset <id> --hardware-generation <n> --gate open|closed --plan <id>\n"
      "       --attempt <id> --actor <id> --actor-kind <kind> --authority <scopes>\n"
      "       --evidence <ref> [--expected-revision <n>]\n"
      "       [--expected-lifecycle-generation <n>] [--expected-epoch <n>]\n"
      "  observe-health --asset <id> --hardware-generation <n> --health <status>\n"
      "                 --evidence-digest <hex64> --source <text> --plan <id>\n"
      "                 --attempt <id> --actor <id> --actor-kind <kind>\n"
      "                 --evidence <ref> [--readiness <status>]\n"
      "                 [--availability <status>] [--observed-wall-clock <rfc3339>]\n"
      "      an observation records a measurement; it never changes a lifecycle\n"
      "      state, a revision or a generation\n"
      "  attest --asset <id> --hardware-generation <n> --plan <id> --attempt <id>\n"
      "         --actor <id> --actor-kind <kind> --authority recovery\n"
      "         --evidence recovery_attestation:<hex64>:<source>\n"
      "         [--expected-revision <n>] [--expected-epoch <n>]\n"
      "      re-establishes live authority over an object recovered by a restart\n"
      "  link-replacement --predecessor asset@gen --successor asset@gen --plan <id>\n"
      "                   --attempt <id> --actor <id> --actor-kind <kind>\n"
      "                   --authority <scopes> --evidence <ref>\n"
      "                   [--expected-revision <n>]\n"
      "                   [--expected-lifecycle-generation <n>]\n"
      "                   [--expected-epoch <n>]\n"
      "      the expectations bind to the predecessor\n"
      "  inspect --asset <id> [--hardware-generation <n>]\n"
      "      state, class, revision, generations, kind, model, location, gate,\n"
      "      derived service eligibility, authority, health snapshot, history\n"
      "      count, predecessor and successor\n"
      "  list [--state <State>] [--kind <kind>] [--site <id>]\n"
      "       [--eligibility unknown|eligible|ineligible]\n"
      "      one line per object in canonical key order\n"
      "  history --asset <id> [--hardware-generation <n>]\n"
      "      one line per entry: sequence logical_time from -> to reason\n"
      "      revision_after chain_digest, then the chain head\n"
      "  lineage --asset <id> [--hardware-generation <n>]\n"
      "      ancestors, self, successors and every link record\n"
      "  recover\n"
      "      open the store as a writer and print the whole RecoveryReport\n"
      "  verify\n"
      "      never takes writer authority; prints the log digest and \"state:\n"
      "      verified\", or fails\n"
      "  export --out <file> [--compact]\n"
      "  import --in <file>\n"
      "      the store must be empty; the document is replayed through the ordinary\n"
      "      commit path and refused unless it reproduces the state it declares\n"
      "  diff --before <file> --after <file>\n"
      "      diff_snapshots of two exported documents; no store is opened\n"
      "\n"
      "commands that only read open the store read only, so they never take the\n"
      "writer lock and never advance the fencing epoch; recover, create, transition,\n"
      "gate, observe-health, attest, link-replacement and import open it as a writer.\n");
}

[[nodiscard]] int dispatch(const Arguments& args, Output& out) {
  const std::string& command = args.command;
  if (command == "version") {
    return command_version(out);
  }
  if (command == "limits") {
    return command_limits(out);
  }
  if (command == "compatibility") {
    return command_compatibility(out);
  }
  if (command == "diagram") {
    return command_diagram(out);
  }
  if (command == "legal-transitions") {
    return command_legal_transitions(args, out);
  }
  if (command == "explain") {
    return command_explain(args, out);
  }
  if (command == "diff") {
    return command_diff(args, out);
  }
  if (command == "create") {
    return command_create(args, out);
  }
  if (command == "transition") {
    return command_transition(args, out);
  }
  if (command == "gate") {
    return command_gate(args, out);
  }
  if (command == "observe-health") {
    return command_observe_health(args, out);
  }
  if (command == "attest") {
    return command_attest(args, out);
  }
  if (command == "link-replacement") {
    return command_link_replacement(args, out);
  }
  if (command == "inspect") {
    return command_inspect(args, out);
  }
  if (command == "list") {
    return command_list(args, out);
  }
  if (command == "history") {
    return command_history(args, out);
  }
  if (command == "lineage") {
    return command_lineage(args, out);
  }
  if (command == "recover") {
    return command_recover(args, out);
  }
  if (command == "verify") {
    return command_verify(args, out);
  }
  if (command == "export") {
    return command_export(args, out);
  }
  if (command == "import") {
    return command_import(args, out);
  }
  out.attach_error(hw::Error::make(hw::ErrorCode::Internal, "unhandled command " + command));
  return kExitInternal;
}

/// Prints one failure and returns its exit code. The usage summary is part of a
/// usage failure, so a caller that mistyped a line is told what a line looks
/// like without having to run --help.
[[nodiscard]] int print_usage_failure(const hw::Error& error) {
  Output out(false, false);
  out.text("usage", kUsageLine);
  out.attach_error(error);
  out.text("help", kHelpHint);
  out.render(std::cout);
  return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
  const hw::Result<Arguments> parsed = parse_arguments(argc, argv);
  if (!parsed.has_value()) {
    const int code = print_usage_failure(parsed.error());
    std::cout.flush();
    return code;
  }
  const Arguments& args = parsed.value();
  if (args.help) {
    std::cout << help_text();
    std::cout.flush();
    return kExitSuccess;
  }
  if (args.version || args.command == "version") {
    Output out(args.json, args.compact);
    out.text("command", "version");
    const int code = command_version(out);
    if (!(args.quiet && code == kExitSuccess)) {
      out.render(std::cout);
    }
    std::cout.flush();
    return code;
  }
  if (args.command.empty()) {
    Output out(args.json, args.compact);
    out.text("usage", kUsageLine);
    out.attach_error(usage_error("no command was given"));
    out.text("help", kHelpHint);
    if (!args.quiet) {
      out.render(std::cout);
    }
    std::cout.flush();
    return kExitUsage;
  }

  Output out(args.json, args.compact);
  out.text("command", args.command);
  const int code = dispatch(args, out);
  if (!(args.quiet && code == kExitSuccess)) {
    out.render(std::cout);
  }
  std::cout.flush();
  return code;
}
