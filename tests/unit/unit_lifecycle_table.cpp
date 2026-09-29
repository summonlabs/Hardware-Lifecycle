// Hardware Lifecycle - unit proofs for the authoritative transition table.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// transition_rules() is the single source of truth for legal lifecycle edges.
// These cases prove that the table is internally consistent and that every
// derived helper (find_rule, legal_targets, is_legal_transition, describe_rule,
// lifecycle_machine_diagram) is a faithful projection of it rather than a second
// copy of the edge list.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;

[[nodiscard]] const TransitionRule* table_rule(LifecycleState from, LifecycleState to) {
  for (const TransitionRule& rule : transition_rules()) {
    if (rule.from == from && rule.to == to) {
      return &rule;
    }
  }
  return nullptr;
}

[[nodiscard]] std::vector<LifecycleState> table_targets(LifecycleState from) {
  std::vector<LifecycleState> targets;
  for (const TransitionRule& rule : transition_rules()) {
    if (rule.from == from) {
      targets.push_back(rule.to);
    }
  }
  return targets;
}

/// The diagram line the table implies for one state.
[[nodiscard]] std::string expected_diagram_line(LifecycleState state) {
  std::string line(to_string(state));
  line += " [";
  line += to_string(state_class(state));
  line += "] -> ";
  const std::vector<LifecycleState> targets = legal_targets(state);
  if (targets.empty()) {
    line += "(terminal)";
    return line;
  }
  for (std::size_t index = 0; index < targets.size(); ++index) {
    if (index != 0) {
      line += ", ";
    }
    line += to_string(targets[index]);
  }
  return line;
}

[[nodiscard]] ReasonMask all_reason_bits() {
  ReasonMask mask;
  for (std::size_t index = 1; index < kTransitionReasonCount; ++index) {
    mask |= reason_bit(static_cast<TransitionReason>(index));
  }
  return mask;
}

[[nodiscard]] EvidenceMask all_evidence_bits() {
  EvidenceMask mask;
  for (std::size_t index = 1; index < kEvidenceKindCount; ++index) {
    mask |= evidence_bit(static_cast<EvidenceKind>(index));
  }
  return mask;
}

[[nodiscard]] bool seen_before(const std::vector<std::string>& seen, std::string_view text) {
  for (const std::string& earlier : seen) {
    if (earlier == text) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Table consistency
// ---------------------------------------------------------------------------

HL_TEST(unit_lifecycle_table, transition_table_is_internally_consistent) {
  const std::span<const TransitionRule> rules = transition_rules();
  HL_REQUIRE(!rules.empty());

  const AuthorityMask declared_authority = authority_all();
  const ReasonMask declared_reasons = all_reason_bits();
  const EvidenceMask declared_evidence = all_evidence_bits();

  for (std::size_t index = 0; index < rules.size(); ++index) {
    const TransitionRule& rule = rules[index];
    const std::string label = std::string(to_string(rule.from)) + " -> " + std::string(to_string(rule.to));

    // Every rule is a real edge with real requirements.
    HL_CHECK_MSG(!rule.required_authority.empty(), label.c_str());
    HL_CHECK_MSG(!rule.allowed_reasons.empty(), label.c_str());
    HL_CHECK_MSG(rule.from != rule.to, label.c_str());
    HL_CHECK_MSG(!rule.note.empty(), label.c_str());
    HL_CHECK_MSG(declared_authority.contains(rule.required_authority), label.c_str());
    HL_CHECK_MSG((rule.allowed_reasons.bits() & ~declared_reasons.bits()) == 0u, label.c_str());
    HL_CHECK_MSG((rule.required_evidence.bits() & ~declared_evidence.bits()) == 0u, label.c_str());

    // At least one allowed reason is a real reason: Unset satisfies no rule.
    bool any_reason = false;
    for (std::size_t reason = 1; reason < kTransitionReasonCount; ++reason) {
      if (reason_allowed(rule.allowed_reasons, static_cast<TransitionReason>(reason))) {
        any_reason = true;
      }
    }
    HL_CHECK_MSG(any_reason, label.c_str());
    HL_CHECK_MSG(!reason_allowed(rule.allowed_reasons, TransitionReason::Unset), label.c_str());

    // requires_location is derived from the target state, never written by hand.
    HL_CHECK_MSG(rule.requires_location == requires_location(rule.to), label.c_str());

    // A successor that must already be installed is always a successor link, and
    // only the superseded state demands one.
    HL_CHECK_MSG(!rule.requires_successor_installed || rule.requires_successor_link, label.c_str());
    HL_CHECK_MSG(!rule.requires_successor_link || rule.to == LifecycleState::Replaced, label.c_str());

    // One rule per (from, to) pair.
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      const bool duplicate = rules[earlier].from == rule.from && rules[earlier].to == rule.to;
      HL_CHECK_MSG(!duplicate, label.c_str());
    }
  }
}

HL_TEST(unit_lifecycle_table, no_rule_opens_the_gate_and_resume_marks_a_return_to_service) {
  // Entering the service scope from outside is the crossing the runtime checks
  // the gate for. Every crossing carries Resume, and Resume means nothing else:
  // it marks a return to service, which includes returning from impaired service
  // but never includes impairing an object that was already fully in service.
  std::size_t resume_rules = 0;
  for (const TransitionRule& rule : transition_rules()) {
    const std::string label = std::string(to_string(rule.from)) + " -> " + std::string(to_string(rule.to));
    const bool crossing = occupies_service_scope(rule.to) && !occupies_service_scope(rule.from);
    const bool resumed = rule.eligibility_effect == EligibilityEffect::Resume;
    if (resumed) {
      ++resume_rules;
    }

    HL_CHECK_MSG(!crossing || resumed, label.c_str());
    HL_CHECK_MSG(!resumed || occupies_service_scope(rule.to), label.c_str());
    HL_CHECK_MSG(resumed == (occupies_service_scope(rule.to) && rule.from != LifecycleState::Active),
                 label.c_str());

    // No rule can open the gate: the effect vocabulary has no such value, and a
    // suspension or a termination never leaves the object in service.
    HL_CHECK_MSG(rule.eligibility_effect == EligibilityEffect::Unchanged ||
                     rule.eligibility_effect == EligibilityEffect::Suspend ||
                     rule.eligibility_effect == EligibilityEffect::Resume ||
                     rule.eligibility_effect == EligibilityEffect::Terminate,
                 label.c_str());
    if (rule.eligibility_effect == EligibilityEffect::Suspend ||
        rule.eligibility_effect == EligibilityEffect::Terminate) {
      HL_CHECK_MSG(!occupies_service_scope(rule.to), label.c_str());
    }
    // Leaving the service scope is always a withdrawal: a suspension or a
    // termination, never an unchanged gate.
    if (occupies_service_scope(rule.from) && !occupies_service_scope(rule.to)) {
      HL_CHECK_MSG(rule.eligibility_effect == EligibilityEffect::Suspend ||
                       rule.eligibility_effect == EligibilityEffect::Terminate,
                   label.c_str());
    }
  }
  HL_CHECK_EQ(resume_rules, static_cast<std::size_t>(5));

  HL_CHECK_EQ(to_string(EligibilityEffect::Unchanged), std::string_view("unchanged"));
  HL_CHECK_EQ(to_string(EligibilityEffect::Suspend), std::string_view("suspend"));
  HL_CHECK_EQ(to_string(EligibilityEffect::Resume), std::string_view("resume"));
  HL_CHECK_EQ(to_string(EligibilityEffect::Terminate), std::string_view("terminate"));
  HL_CHECK_EQ(to_string(static_cast<EligibilityEffect>(99)), std::string_view("unknown"));
}

HL_TEST(unit_lifecycle_table, removed_is_terminal_and_has_no_outgoing_edge) {
  HL_CHECK_EQ(is_terminal(LifecycleState::Removed), true);
  HL_CHECK_EQ(legal_targets(LifecycleState::Removed).empty(), true);

  for (const LifecycleState target : all_lifecycle_states()) {
    HL_CHECK_EQ(find_rule(LifecycleState::Removed, target), nullptr);
    HL_CHECK_EQ(is_legal_transition(LifecycleState::Removed, target), false);
  }

  // Removed is the only terminal state, and every other state has a way out.
  for (const LifecycleState state : all_lifecycle_states()) {
    HL_CHECK_EQ(is_terminal(state), state == LifecycleState::Removed);
    if (state != LifecycleState::Removed) {
      HL_CHECK_MSG(!legal_targets(state).empty(), std::string(to_string(state)).c_str());
    }
  }
}

HL_TEST(unit_lifecycle_table, every_state_is_reachable_from_ordered) {
  const std::span<const LifecycleState> states = all_lifecycle_states();
  HL_REQUIRE(states.size() == kLifecycleStateCount);

  std::vector<LifecycleState> visited;
  std::vector<LifecycleState> queue;
  queue.push_back(LifecycleState::Ordered);
  visited.push_back(LifecycleState::Ordered);
  for (std::size_t head = 0; head < queue.size(); ++head) {
    for (const LifecycleState target : legal_targets(queue[head])) {
      bool known = false;
      for (const LifecycleState seen : visited) {
        known = known || seen == target;
      }
      if (!known) {
        visited.push_back(target);
        queue.push_back(target);
      }
    }
  }

  HL_CHECK_EQ(visited.size(), kLifecycleStateCount);
  for (const LifecycleState state : states) {
    bool reachable = false;
    for (const LifecycleState seen : visited) {
      reachable = reachable || seen == state;
    }
    HL_CHECK_MSG(reachable, std::string(to_string(state)).c_str());
  }
}

HL_TEST(unit_lifecycle_table, find_rule_and_legal_targets_agree_with_the_table) {
  const std::span<const LifecycleState> states = all_lifecycle_states();

  for (const LifecycleState from : states) {
    for (const LifecycleState to : states) {
      const TransitionRule* expected = table_rule(from, to);
      const TransitionRule* actual = find_rule(from, to);
      HL_CHECK(actual == expected);
      if (expected != nullptr) {
        HL_CHECK_EQ(actual->from, from);
        HL_CHECK_EQ(actual->to, to);
      }
    }
    // Table order is the declaration order of the rules.
    HL_CHECK_EQ(legal_targets(from), table_targets(from));
  }
}

HL_TEST(unit_lifecycle_table, is_legal_transition_agrees_with_find_rule) {
  for (const LifecycleState from : all_lifecycle_states()) {
    for (const LifecycleState to : all_lifecycle_states()) {
      HL_CHECK_EQ(is_legal_transition(from, to), find_rule(from, to) != nullptr);
    }
  }

  // The declared reverse edges are legal, and the edges the domain refuses are
  // not in the table at all.
  HL_CHECK_EQ(is_legal_transition(LifecycleState::Installed, LifecycleState::Staged), true);
  HL_CHECK_EQ(is_legal_transition(LifecycleState::Retiring, LifecycleState::Active), true);
  HL_CHECK_EQ(is_legal_transition(LifecycleState::Maintenance, LifecycleState::Active), true);
  HL_CHECK_EQ(is_legal_transition(LifecycleState::Active, LifecycleState::Ordered), false);
  HL_CHECK_EQ(is_legal_transition(LifecycleState::Maintenance, LifecycleState::Removed), false);
  HL_CHECK_EQ(is_legal_transition(LifecycleState::Degraded, LifecycleState::Retired), false);
  HL_CHECK_EQ(is_legal_transition(LifecycleState::Ordered, LifecycleState::Active), false);
}

// ---------------------------------------------------------------------------
// Spellings
// ---------------------------------------------------------------------------

HL_TEST(unit_lifecycle_table, every_state_reason_and_evidence_kind_round_trips_through_its_spelling) {
  std::vector<std::string> state_spellings;
  for (std::size_t index = 0; index < kLifecycleStateCount; ++index) {
    const auto state = static_cast<LifecycleState>(index);
    const std::string spelling(to_string(state));
    HL_CHECK_MSG(!spelling.empty(), spelling.c_str());
    HL_CHECK_MSG(!seen_before(state_spellings, spelling), spelling.c_str());
    state_spellings.push_back(spelling);

    const Result<LifecycleState> parsed = parse_lifecycle_state(spelling);
    HL_CHECK_MSG(parsed.has_value(), spelling.c_str());
    if (parsed.has_value()) {
      HL_CHECK_EQ(parsed.value(), state);
    }
    const Result<LifecycleState> folded = parse_lifecycle_state_lenient(to_ascii_lower(spelling));
    HL_CHECK_MSG(folded.has_value(), spelling.c_str());
    if (folded.has_value()) {
      HL_CHECK_EQ(folded.value(), state);
    }
    HL_CHECK_MSG(!to_string(state_class(state)).empty(), spelling.c_str());
  }

  // The canonical parser is case sensitive and refuses surrounding whitespace;
  // the lenient parser folds case only.
  HL_CHECK_ERROR(parse_lifecycle_state("ordered"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_lifecycle_state("Ordered "), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_lifecycle_state(""), ErrorCode::MalformedRequest);
  const Result<LifecycleState> lenient = parse_lifecycle_state_lenient("aCtIvE");
  HL_REQUIRE(lenient.has_value());
  HL_CHECK_EQ(lenient.value(), LifecycleState::Active);
  HL_CHECK_ERROR(parse_lifecycle_state_lenient("active "), ErrorCode::MalformedRequest);

  std::vector<std::string> reason_spellings;
  for (std::size_t index = 0; index < kTransitionReasonCount; ++index) {
    const auto reason = static_cast<TransitionReason>(index);
    const std::string spelling(to_string(reason));
    HL_CHECK_MSG(!spelling.empty(), spelling.c_str());
    HL_CHECK_MSG(!seen_before(reason_spellings, spelling), spelling.c_str());
    reason_spellings.push_back(spelling);

    const Result<TransitionReason> parsed = parse_transition_reason(spelling);
    HL_CHECK_MSG(parsed.has_value(), spelling.c_str());
    if (parsed.has_value()) {
      HL_CHECK_EQ(parsed.value(), reason);
    }
  }
  HL_CHECK_ERROR(parse_transition_reason(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_transition_reason("DeliveryAccepted"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_transition_reason("delivery_accepted "), ErrorCode::MalformedRequest);

  // Every real evidence kind round trips. The unknown kind is the absence of a
  // kind: it has a spelling, and the parser deliberately refuses it as an input,
  // because absence is never a kind a request may cite.
  std::vector<std::string> evidence_spellings;
  for (std::size_t index = 1; index < kEvidenceKindCount; ++index) {
    const auto kind = static_cast<EvidenceKind>(index);
    const std::string spelling(to_string(kind));
    HL_CHECK_MSG(!spelling.empty(), spelling.c_str());
    HL_CHECK_MSG(!seen_before(evidence_spellings, spelling), spelling.c_str());
    evidence_spellings.push_back(spelling);

    const Result<EvidenceKind> parsed = parse_evidence_kind(spelling);
    HL_CHECK_MSG(parsed.has_value(), spelling.c_str());
    if (parsed.has_value()) {
      HL_CHECK_EQ(parsed.value(), kind);
    }
  }
  HL_CHECK_EQ(to_string(EvidenceKind::Unknown), std::string_view("unknown"));
  HL_CHECK_ERROR(parse_evidence_kind("unknown"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_evidence_kind(""), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_evidence_kind("delivery"), ErrorCode::MalformedRequest);
  HL_CHECK_ERROR(parse_evidence_kind("DeliveryReceipt"), ErrorCode::MalformedRequest);

  // Unset is a reason spelling, and it satisfies no rule; Unknown is an evidence
  // spelling, and it requires nothing.
  HL_CHECK_EQ(to_string(TransitionReason::Unset), std::string_view("unset"));
  HL_CHECK_EQ(reason_bit(TransitionReason::Unset).empty(), true);
  HL_CHECK_EQ(evidence_bit(EvidenceKind::Unknown).empty(), true);
  HL_CHECK_EQ(to_string(ReasonMask()), std::string("none"));
  HL_CHECK_EQ(to_string(EvidenceMask()), std::string("none"));
  HL_CHECK_EQ(to_string(reason_bit(TransitionReason::DeliveryAccepted) |
                        reason_bit(TransitionReason::PhysicallyRemoved)),
              std::string("delivery_accepted, physically_removed"));

  // Authority scopes render as a mask and parse back to the same single bit, so
  // the rendering a rule is described with is the spelling a caller can parse.
  for (std::size_t index = 0; index < kAuthorityScopeCount; ++index) {
    const auto scope = static_cast<AuthorityScope>(index);
    const AuthorityMask single = authority_bit(scope);
    const std::string rendering = to_string(single);
    HL_CHECK_MSG(!rendering.empty(), rendering.c_str());

    const Result<AuthorityMask> parsed_mask = parse_authority_mask(rendering);
    HL_CHECK_MSG(parsed_mask.has_value(), rendering.c_str());
    if (parsed_mask.has_value()) {
      HL_CHECK_EQ(parsed_mask.value().bits(), single.bits());
    }
    const Result<AuthorityScope> parsed_scope = parse_authority_scope(rendering);
    HL_CHECK_MSG(parsed_scope.has_value(), rendering.c_str());
    if (parsed_scope.has_value()) {
      HL_CHECK_EQ(parsed_scope.value(), scope);
    }
  }
  const Result<AuthorityMask> mask = parse_authority_mask(to_string(authority_bit(AuthorityScope::Service) |
                                                                  authority_bit(AuthorityScope::Integrity)));
  HL_REQUIRE(mask.has_value());
  HL_CHECK_EQ(mask.value().bits(), (authority_bit(AuthorityScope::Service) |
                                    authority_bit(AuthorityScope::Integrity)).bits());
  HL_CHECK_EQ(authority_all().bits(), static_cast<AuthorityMask::value_type>(0x7FFu));
  HL_CHECK_ERROR(parse_authority_scope("nope"), ErrorCode::MalformedRequest);
}

// ---------------------------------------------------------------------------
// Derived renderings
// ---------------------------------------------------------------------------

HL_TEST(unit_lifecycle_table, describe_rule_and_diagram_render_every_rule_and_state) {
  for (const TransitionRule& rule : transition_rules()) {
    const std::string described = describe_rule(rule);
    const std::string edge = std::string(to_string(rule.from)) + " -> " + std::string(to_string(rule.to));
    HL_CHECK_MSG(described.find(edge) != std::string::npos, edge.c_str());
    HL_CHECK_MSG(described.find(std::string(rule.note)) != std::string::npos, edge.c_str());
    HL_CHECK_MSG(described.find(std::string(to_string(rule.eligibility_effect))) != std::string::npos,
                 edge.c_str());
    HL_CHECK_MSG(described.find(std::string(to_string(rule.allowed_reasons))) != std::string::npos, edge.c_str());
    const std::string evidence_text =
        rule.required_evidence.empty() ? std::string("any") : to_string(rule.required_evidence);
    HL_CHECK_MSG(described.find(evidence_text) != std::string::npos, edge.c_str());
    HL_CHECK_MSG(described.find(to_string(rule.required_authority)) != std::string::npos, edge.c_str());
    if (rule.requires_location) {
      HL_CHECK_MSG(described.find("requires_location") != std::string::npos, edge.c_str());
    }
    if (rule.requires_successor_link) {
      HL_CHECK_MSG(described.find("requires_successor") != std::string::npos, edge.c_str());
    }
  }

  const std::string diagram = lifecycle_machine_diagram();
  for (const LifecycleState state : all_lifecycle_states()) {
    const std::string line = expected_diagram_line(state);
    HL_CHECK_MSG(diagram.find(line) != std::string::npos, line.c_str());
    HL_CHECK_MSG(diagram.find(std::string(to_string(state)) + " [") != std::string::npos,
                 std::string(to_string(state)).c_str());
  }
  HL_CHECK(diagram.find("Removed [disposed] -> (terminal)") != std::string::npos);
  HL_CHECK(diagram.find("Ordered [pre_physical] -> Staged, Removed") != std::string::npos);

  // One line per state, so the diagram cannot silently drop a state.
  std::size_t lines = 0;
  for (const char byte : diagram) {
    if (byte == '\n') {
      ++lines;
    }
  }
  HL_CHECK_EQ(lines, kLifecycleStateCount);
}
