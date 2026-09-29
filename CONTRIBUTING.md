# Contributing to Hardware Lifecycle

Hardware Lifecycle is the canonical physical hardware lifecycle runtime of the
Data Center Control Plane. It is published by Summon Software Labs under the
Apache License 2.0.

## Licensing of contributions

Contributions are accepted under the Apache License 2.0, the same license that
covers the project. There is no contributor license agreement and no copyright
assignment: by submitting a pull request you confirm that you wrote the change,
or that you have the right to submit it, and you license it to everyone under
the same terms as the rest of the repository. Every new file carries the same
short header the existing files carry:

    Copyright 2026 Summon Software Labs.
    Licensed under the Apache License, Version 2.0.

Do not add a license header that names anyone else, and do not add an
`SPDX-License-Identifier` that disagrees with the root LICENSE file.

## Before you open a pull request

1. Build both configurations with warnings as errors:

       cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
       cmake --build build-release
       cmake -S . -B build-debug   -DCMAKE_BUILD_TYPE=Debug
       cmake --build build-debug

2. Run the whole test suite in each configuration:

       ctest --test-dir build-release --output-on-failure
       ctest --test-dir build-debug   --output-on-failure

3. If you touched the durable format, the command line interface or the
   installed package, also run the checks described in the README under
   "Validation performed", including the installed-artifact and downstream
   consumer tests.

The build treats every warning as an error in Release and in Debug. A change
that only compiles with a widened warning set is not ready.

## What the project expects of a change

- **A test that can fail.** Tests here are proof obligations, not decoration.
  If a change claims a behaviour, the suite must contain an assertion that
  would fail if that behaviour regressed. A test that passes for the wrong
  reason is worse than no test.
- **Determinism.** No unordered iteration, no reliance on addresses, locale,
  wall clock or thread scheduling. Where a random generator is used, the seed
  is fixed and printed so a failure can be reproduced.
- **No timeouts.** Nothing in the build, the tests or the tools declares a
  timeout, and no test relies on one. A hanging test is a defect to diagnose,
  not a test to bound.
- **Real proof for real claims.** A claim about a second process, a durable
  artifact, an operating system lock or an installed package is proven with a
  real process, a real file and a real installed tree. Threads never stand in
  for processes, and a mock never stands in for hardware.
- **Honest labels.** Anything that could not be validated on the host is
  labelled SYNTHETIC in the documentation, and the README's validation section
  says what was actually run.
- **Explicit failure over convenience.** Missing, unknown and unmeasured values
  are never converted to zero, false, empty, healthy, ready or permitted. The
  code returns an error instead.

## The doctrine

Hardware Lifecycle exists to answer one question: what lifecycle state is
authoritative for this hardware object now, which transitions are legal under
the current generation and authority, and what lineage and history prove how it
reached that state? Changes are judged against the doctrine that makes that
answer trustworthy:

- Observation is not authority; acknowledgement is not effect.
- Requested state is not observed state.
- Installed is not active; drained is not decommissioned.
- Health never rewrites lifecycle.
- Recovered persisted state is not fresh live evidence, and stale authority is
  fenced rather than inherited.

If a change makes one of those statements less true, it needs an extremely good
argument in the pull request.

## Commit messages

Concise, neutral and public facing. Describe what changed and why in the
imperative mood. Do not add co-author trailers of any kind, and do not mention
internal tooling, review processes or private context.

## Reporting a problem

Open an issue with the smallest reproduction you can construct: the exact
command, the exact input, the observed output and the expected output. For a
durability problem, say which store, which build configuration and whether the
store was written by an older build.
