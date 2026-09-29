# Hardware Lifecycle

The canonical lifecycle state machine runtime for physical facility hardware in
the Summon Software Labs Data Center Control Plane (DCCP).

Hardware Lifecycle answers one question:

> What lifecycle state is authoritative for this hardware object now, which
> transitions are legal under the current generation and authority, and what
> lineage and history prove how it reached that state?

It is a C++20 library with no third-party dependencies, a command line tool, a
durable store, and a test suite whose claims are backed by real processes, real
files and real installed artifacts.

---

## Systems boundary

**This repository owns** the authoritative lifecycle state of one physical
hardware object:

* the lifecycle state machine itself, with the transition table as a single
  source of truth;
* generation-bound transition authority, evidence requirements and reasons;
* the mapping from a plan and an attempt to exactly one durable effect;
* replacement lineage between distinct object identities;
* durable, append-consistent history that replays to exactly the authoritative
  state;
* fencing of stale authority across process boundaries;
* a durable store with an explicit atomic commit point and crash consistency.

**This repository explicitly does not own**:

* physical inventory discovery — objects are registered by a caller that
  supplies the identity, the model and the location; this runtime never scans
  anything and never invents an identity;
* commissioning workflow — Commissioning is a lifecycle state, not a workflow
  engine;
* decommissioning workflow — Retiring and Retired are states, not a runbook;
* firmware policy — a firmware generation is a referenced attribute, never a
  decision made here;
* maintenance scheduling — Maintenance is a state this runtime may be told
  about, not a calendar;
* power, cooling and network control — those systems request transitions, they
  do not get their state changed by this one;
* asset health diagnosis — health, readiness and availability are observations
  recorded here, never conclusions drawn here.

Those systems reference or request lifecycle transitions through this runtime.
The boundary is enforced in the type system rather than promised in prose:
there is no API here that discovers a device, schedules work, sets firmware or
diagnoses a fault.

## Core doctrine

These are not slogans; each one is a property the code and the tests enforce.

| Statement | How it is enforced |
| --- | --- |
| Observation is not authority | `observe_health` records an observation and cannot change the lifecycle state, the revision or the lifecycle generation. A transition into Degraded requires an authority grant and evidence. |
| Acknowledgement is not effect | A receipt is issued only after the durable commit point has been passed and the in-memory state has been advanced. A rejected request produces no receipt. |
| Requested state is not observed state | A request names the state it expects and the state it wants. Both are checked separately, and `state_mismatch` is distinct from `stale_revision`. |
| Discovery is not capability | An object cannot be registered as Active. Registration is limited to Ordered and Staged; every later state is reached through the transition table. |
| Installed is not active | Installed, Commissioning and Active are separate states with separate authorities and evidence. |
| Drained is not decommissioned | Retiring (drained) and Retired (decommissioned) are separate states; `is_decommissioned(Retiring)` is false. |
| A process exit is not authoritative completion | A commit is published atomically and verified by replay; the exit code of a process is never evidence of a committed fact. |
| Missing is never a default | A counter that is not set is absent, never zero; health that was never observed is `present=false`, never healthy; an unknown gate yields `unknown` eligibility, never `eligible`. |
| Recovered state is not fresh evidence | Every object a restart recovers is marked `recovered` and cannot be changed until an attestation re-establishes authority in that session. |
| Stale authority is fenced | A request binds to the control epoch it was planned against; a new writer advances the epoch on open, so a request planned against an older epoch is refused. |
| Replay before staleness | A request whose plan and attempt were already applied with the same content returns the receipt that was issued the first time, even though every generation in it is now stale. |
| History is append-consistent | Each history entry carries a chain digest over its predecessor; tampering, reordering, dropping or duplicating an entry is detectable and replay must land on exactly the authoritative state. |

---

## Lifecycle model

Twelve states, refined so that no two of them mean the same thing:

| State | Meaning |
| --- | --- |
| Ordered | A commitment exists. No physical unit is at a facility yet. |
| Staged | The unit is at a facility but not installed in a target location. |
| Installed | The unit occupies its assigned location and carries no service. |
| Commissioning | Qualification is in progress. |
| Active | The unit is in service. |
| Degraded | An authority has acknowledged impaired service; the unit remains in service. |
| Maintenance | Withdrawn from service on purpose, expected to return. |
| Quarantined | Withheld from service and from maintenance pending an integrity decision. |
| Retiring | Drained: service is withdrawn, decommissioning has not happened. |
| Retired | Decommissioned, still physically present. |
| Replaced | Superseded by a successor object; still physically present. |
| Removed | Physically gone. Terminal. |

The transition table in [src/transition_table.cpp](src/transition_table.cpp) is
the single source of truth: the validator, the command line tool, the diagram
and the tests all read it, and nothing else in the repository repeats the edge
list. Each rule declares the authority scopes that may perform it, the evidence
kinds it requires, the reasons it accepts, what it does to the service
eligibility gate, and whether it needs a successor link or a location.

A few consequences worth stating, because they are deliberate:

* **Removed is terminal.** Nothing leaves it. A new physical object is a new
  identity and a new hardware generation, which is a different object key.
* **You cannot skip draining.** There is no Active to Retired edge.
* **Maintenance is not removal.** There is no Maintenance to Removed edge.
* **Replacement requires decommissioning first, and the link before the state.**
  The successor link is its own durable fact; the move to Replaced verifies that
  the link already exists.
* **Reverse edges are declared, not implied.** Installed to Staged, Retiring to
  Active, Retiring to Maintenance and the returns from Degraded and Maintenance
  exist only because they are written in the table. Anything absent from the
  table is rejected as `illegal_transition`.
* **No transition opens the service gate.** Entering the service scope requires
  the gate to be open already; opening it is a separate authority decision with
  its own receipt. Closing it may be done by a transition, because closing can
  never grant permission.

## Authority, generations and fencing

Distinct facts get distinct types, so confusing them does not compile:
`AssetId`, `RackId`, `SiteId`, `SlotId`, `ActorId`, `ModelId`,
`PlanId`, `AttemptId`, and the counters `HardwareGeneration`,
`LifecycleGeneration`, `FirmwareGeneration`, `ControlEpoch`,
`IncarnationId`, `Revision`, `CommitSequence`, `ObservationSequence`,
`LogicalTime`, `PolicyGeneration` and the rest. Masks are typed too:
an authority mask cannot be passed where an evidence mask is expected.

* **HardwareGeneration** identifies the physical object. It never changes; a
  replacement creates a new object key.
* **LifecycleGeneration** advances by exactly one for each state-changing
  history entry, and does not move for a gate decision.
* **Revision** advances by exactly one for every durable mutation of the object,
  including a gate decision.
* **ControlEpoch** identifies the writer session. Every open by a writer
  publishes a new manifest with the next epoch, after the state it fences is
  already durable. A request that binds to an older epoch is refused with
  `stale_authority`.
* **LogicalTime** is the authoritative ordering, assigned by the store, one step
  per committed record. A wall clock is carried for humans and is never used to
  order, fence or validate anything.
* **AuthorityState** is session scoped and never durable: an object recovered by
  a restart is `recovered` until an attestation re-establishes authority.

## Persistence and recovery

The store is three files under one directory:

    manifest.hlb    exactly 512 bytes, always replaced atomically
    journal.hlj     append only, one framed record after another
    writer.lock     held with an operating system lock for the writer session

A record is framed with a magic, a format version, a kind, a payload length, a
sequence, a logical time and a CRC-32. The manifest carries the format version,
the control epoch, the incarnation, the commit sequence, the logical time, the
record count, the published prefix length, the SHA-256 of that prefix, and the
SHA-256 of the authoritative state that replaying the prefix must produce.

The commit protocol, in order, with a documented seam for crash proofs at every
stage:

1. encode the record;
2. append it to the journal;
3. flush it to the device;
4. read it back and verify the framing, the sequence and the payload bytes;
5. publish a new manifest by writing a temporary file and replacing the target
   atomically, write-through;
6. fencing has already advanced, because the epoch and the incarnation travel
   inside the manifest that was just published.

There is no window in which a fencing epoch is durable before the state it
fences. Recovery reads the manifest, verifies the published prefix against its
digest, decodes every record, replays them, and then checks that the replayed
state hashes to the digest the manifest published. If it does not, the store is
refused: it is never repaired, never truncated and never reported as empty.

Anything past the published prefix was never published. It is reported as
`discarded_tail_bytes` and dropped, because a crash between the append and the
publish is a normal outcome that must be visible rather than silent.

Readers never observe a torn generation: the manifest is replaced atomically and
readers read exactly the published prefix.

A read only open takes no lock and never writes.

## Service eligibility is not lifecycle

Three independent facts with three independent authorities:

* **lifecycle state** — owned here, changed only by the transition table;
* **health, readiness, availability** — observations recorded here from a
  monitoring system, and never an input to a transition;
* **service eligibility gate** — an explicit operator or policy decision with
  its own receipt.

`service_eligibility(state, gate)` is a projection, not an input. It is
`eligible` only when the gate is open and the state is in the service scope;
an unknown gate yields `unknown`, and never `eligible`.

## Error model

Every rejection carries a stable snake_case code, a human message and structured
notes. The full list is in
[errors.hpp](include/hardware_lifecycle/errors.hpp) and the spellings are part
of the command line contract.

Validation precedence is deterministic and documented in one table,
`validation_rank`: whether an input can be considered at all (bounds, format
version) comes before whether it is well formed, which comes before identity,
then authority and fencing, then transition legality. When one request violates
several rules the lowest ranked violation is the primary error, so the reported
error never depends on check order, map iteration order or unrelated state. A
test asserts that the enum order and the rank table agree.

## Concurrency model

One writer, many readers.

* Writer authority is an operating system lock on `writer.lock`. A second
  writer — in the same process or another one — is refused with
  `store_locked`. The lock belongs to the file handle, so the operating system
  releases it when a writer dies, which a real two-process test proves.
* A `Runtime` is single writer by design and enforces that: mutations go
  through one path with a copy, a durable commit and a publish, so a failed
  commit leaves the previous generation authoritative and complete.
* Readers open read only, take no lock, and always see a complete published
  generation.
* No callback is invoked while a lock is held, no worker is joined while holding
  state it needs, and the commit observer in the crash seam is called outside
  any state that could re-enter.

## The store is the authority, and the receipt follows it

Each mutation follows exactly one path:

    resolve plan identity
      -> validate against the exact identity, generations, revision, epoch and
         evidence the caller planned against
      -> copy the authoritative state
      -> build the durable record
      -> apply the record to the copy through the same function recovery uses
      -> commit the record and the resulting state digest durably
      -> publish the copy
      -> issue the receipt, rebuilt from the record

Applying the record through the recovery function is deliberate: the live path
and the replay path cannot disagree, because they are the same code. The receipt
is rebuilt from the record and checked against the receipt digest the record
stored, so a receipt that was never issued cannot be produced.

---

## Building

Requirements: a C++20 compiler and CMake 3.20 or newer. There are no other
dependencies.

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release
    ctest --test-dir build-release --output-on-failure

Options: `HL_BUILD_TESTS`, `HL_BUILD_TOOLS`, `HL_BUILD_EXAMPLES`,
`HL_BUILD_BENCHMARKS`, `HL_WARNINGS_AS_ERRORS` (default ON),
`HL_SANITIZERS`, `BUILD_SHARED_LIBS`, `HL_ENABLE_INSTALLED_TESTS`.

The build applies `/W4 /WX /permissive- /Zc:__cplusplus /utf-8` on MSVC and
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` elsewhere, per target,
privately. There are exactly two MSVC suppressions, both documented in the root
`CMakeLists.txt` and neither able to hide a real defect: C4820, because domain
types are never memcpy'd to a durable format (every byte is encoded field by
field), and C4251, which only applies to shared library builds and concerns
standard library members crossing the boundary through the shared C runtime.

## Installing and consuming

    cmake --install build-release --prefix <prefix>

This installs the library, the public headers, the command line tool, the CMake
package files and this documentation. A downstream project consumes it with:

    find_package(HardwareLifecycle CONFIG REQUIRED)
    target_link_libraries(your_target PRIVATE HardwareLifecycle::hardware_lifecycle)

An independent, out-of-tree consumer that does exactly this lives in
[tests/installed/downstream](tests/installed/downstream) and is built and run by
the `hl_downstream_consumer` test against an installed prefix.

## Library layout

| Header | Contents |
| --- | --- |
| [hardware_lifecycle.hpp](include/hardware_lifecycle/hardware_lifecycle.hpp) | umbrella |
| [lifecycle.hpp](include/hardware_lifecycle/lifecycle.hpp) | states, classes, reasons, the transition table |
| [model.hpp](include/hardware_lifecycle/model.hpp) | hardware object, location, kinds |
| [ids.hpp](include/hardware_lifecycle/ids.hpp) | strong identities and counters |
| [provenance.hpp](include/hardware_lifecycle/provenance.hpp) | actors, authority, evidence, wall clock |
| [health.hpp](include/hardware_lifecycle/health.hpp) | health, readiness, availability, eligibility |
| [history.hpp](include/hardware_lifecycle/history.hpp) | history chains, verification, replay |
| [replacement.hpp](include/hardware_lifecycle/replacement.hpp) | replacement lineage |
| [requests.hpp](include/hardware_lifecycle/requests.hpp) | requests, receipts, validation |
| [registry.hpp](include/hardware_lifecycle/registry.hpp) | the authoritative in-memory state |
| [persistence.hpp](include/hardware_lifecycle/persistence.hpp) | the durable store |
| [runtime.hpp](include/hardware_lifecycle/runtime.hpp) | the composed runtime |
| [export.hpp](include/hardware_lifecycle/export.hpp) / [diff.hpp](include/hardware_lifecycle/diff.hpp) | documents and structural diff |
| [errors.hpp](include/hardware_lifecycle/errors.hpp) / [result.hpp](include/hardware_lifecycle/result.hpp) | error taxonomy and `Result` |
| [digest.hpp](include/hardware_lifecycle/digest.hpp) | SHA-256 and CRC-32 |
| [text.hpp](include/hardware_lifecycle/text.hpp), [limits.hpp](include/hardware_lifecycle/limits.hpp), [version.hpp](include/hardware_lifecycle/version.hpp), [compatibility.hpp](include/hardware_lifecycle/compatibility.hpp) | strict text, bounds, versions |

## Command line tool

`hwlifecycle` exposes the same authority as the library. Exit codes: 0 success,
1 usage error, 2 the runtime rejected the request, 3 a durable store failure,
4 internal error.

    # register hardware (the inventory system supplies the identity)
    hwlifecycle --store ./store create --asset rack-07-node-01 --hardware-generation 1 \
      --kind compute --model model-x --plan p1 --attempt a1 \
      --actor operator-1 --actor-kind operator --authority procurement \
      --evidence procurement_record:<64 hex>:erp/po-1

    # move it, with an explicit attestation because this is a new process
    hwlifecycle --store ./store transition --asset rack-07-node-01 --hardware-generation 1 \
      --to Staged --reason delivery_accepted --attest --plan p2 --attempt a1 \
      --actor operator-1 --actor-kind operator --authority logistics,recovery \
      --evidence delivery_receipt:<64 hex>:wms/receipt-1 \
      --evidence recovery_attestation:<64 hex>:ops/attest-1

    # what does the machine say, and what proves it?
    hwlifecycle --store ./store inspect --asset rack-07-node-01
    hwlifecycle --store ./store history --asset rack-07-node-01
    hwlifecycle legal-transitions --state Retired
    hwlifecycle explain --from Maintenance --to Removed
    hwlifecycle --store ./store recover
    hwlifecycle --store ./store verify
    hwlifecycle --store ./store export --out snapshot.json
    hwlifecycle --store ./store2 import --in snapshot.json
    hwlifecycle diff --before snapshot.json --after snapshot.json

The complete contract, including the binding defaults and the meaning of
`--attest`, is printed by `hwlifecycle --help`.

## Examples

Three programs that are documentation which runs:

* [example_lifecycle_walkthrough.cpp](examples/example_lifecycle_walkthrough.cpp)
  — the full walk, with the requirements read out of the transition table, the
  refusals, the primary error ranks, and a health observation that changes
  nothing;
* [example_durable_journal.cpp](examples/example_durable_journal.cpp) — a real
  store, recovery, fenced authority, attestation, verify, and an export that
  imports into a second store with an identical state digest;
* [example_replacement_lineage.cpp](examples/example_replacement_lineage.cpp) —
  lineage from link to supersession to physical removal, and the links that are
  refused.

## Limits

The runtime is bounded on purpose, and a bound violation is reported rather than
truncated. The bounds are listed by `hwlifecycle limits` and declared in
[limits.hpp](include/hardware_lifecycle/limits.hpp). Notably: one journal
segment per store capped at 16 GiB (there is no rotation and no compaction, so
the bound is a real ceiling rather than a hidden failure), one million objects,
one million history entries per object, one mebibyte per record payload.

A commit computes the digest of the entire authoritative state and publishes it
in the manifest, so a commit costs O(state). That is a deliberate trade: it is
what lets a restart prove that the state it replayed is the state that was
published. The cost is measured and reported in the benchmark rather than
hidden.

## Telemetry

None. The library and the tool make no network calls of any kind. There is no
telemetry, no analytics, no update check and no phone home.

## Validation performed

Everything below is a result that was actually produced on the validating host:
Windows, MSVC 19.44.35222.0 (toolset 14.44.35207), C++20, Ninja, CMake 4.3.2,
x64. Nothing in this section is projected, estimated or copied from a design
document.

### Builds

| Configuration | Flags | Result |
| --- | --- | --- |
| Release, static library | `/W4 /WX /permissive- /Zc:__cplusplus /utf-8 /EHsc /O2` | clean, zero warnings |
| Debug, static library | `/W4 /WX /permissive- /Zc:__cplusplus /utf-8 /EHsc /Od` | clean, zero warnings |
| Release, `BUILD_SHARED_LIBS=ON` | as above | clean, zero warnings |

The library, the command line tool, the two proof harnesses, three examples and
the benchmark all build in each configuration.

### Tests

126 cases across seven suites, all passing in Release, Debug and the shared
configuration:

    Release   9/9 tests passed, 0 failed  (10.80 s total)
    Debug     7/7 tests passed, 0 failed  (92.17 s total)
    Shared    7/7 tests passed, 0 failed  ( 7.41 s total)

| Suite | Cases | What it proves |
| --- | --- | --- |
| `hl_unit_tests` | 33 | identifiers, counters, strict text, SHA-256 and CRC-32 known answers, the error taxonomy, the deterministic rank table, and the transition table's internal consistency: no duplicate edge, every state reachable from Ordered, no rule opens the service gate, location requirements agree with the states, every spelling round trips. |
| `hl_domain_tests` | 36 | the registry, the full legal walk with exact revisions, generations, gates and receipts, the deterministic primary error of each rejection, idempotent replay, plan conflicts, replacement lineage, and history chain tampering, reordering, duplication and replay. |
| `hl_persistence_tests` | 29 | durability round trips through real close and reopen, corrupt and truncated stores, all six commit stages with a real process terminated inside them, and single-writer exclusion with real processes. |
| `hl_property_tests` | 5 | a seeded randomized state machine, 400 operations per seed across four printed seeds, checking after every operation that the history chain verifies and replays to the object state, that a rejected operation leaves the state digest byte identical and adds no history entry, and that the primary error is the lowest ranked violation. |
| `hl_adversarial_tests` | 14 | hostile identifiers, absurd counters, evidence bounds, mask attacks, successor attacks, identity fencing, a sixteen cycle open and close loop, read only refusal of every mutation, twelve document attacks refused with their exact codes and no durable trace, and JSON layer attacks. |
| `hl_concurrency_tests` | 2 | one writer and four readers over a durable store, where every reader observes every published generation and no generation is published while a reader is inside one, and twelve threads on one shared runtime agreeing on one digest and one byte-identical document. |
| `hl_cli_tests` | 7 | the command line contract driven as a real program: exit codes, receipts, bindings, rejections, the JSON rendering, export, import, diff, verify and recover. |

### Proofs that need a second process, a real file or an installed tree

* **Crash consistency.** `hl_crash_child` is really terminated with
  `TerminateProcess` inside the commit protocol, at each of the six stages.
  After every termination the store reopens, its published generation verifies,
  the state digest matches the manifest, the unpublished tail is reported and
  dropped, and the store accepts further commits. The record count after the
  crash is exactly what the stage implies: 4 records for every stage before the
  publish and 5 after it.
* **Lock release on process death.** A real child process holds the writer lock;
  a second writer is refused with `store_locked`; the child is terminated
  without any cleanup and the lock is immediately available again.
* **Installed artifacts.** `hl_installed_cli` runs the *installed* tool through
  30 invocations covering the exit code contract, the binding defaults, the
  refusal of a mutation without authority, evidence requirements, export,
  import, diff, verify, recover, read only mode and a store that does not exist.
* **Downstream consumption.** `hl_downstream_consumer` configures, builds and
  runs an independent project outside this source tree that uses
  `find_package(HardwareLifecycle CONFIG REQUIRED)` and
  `HardwareLifecycle::hardware_lifecycle` against the installed prefix, walks
  the lifecycle through the installed library, checks that a health observation
  does not rewrite lifecycle state, and checks that an illegal step is refused
  with its deterministic error.

### Benchmark

Methodology: every measured operation is a **completed** operation, meaning the
call returned a receipt; submission, queueing and enqueue latency are never
measured. Timing is `std::chrono::steady_clock`. The workload is generated with
`std::mt19937_64` seed 6840227782638526189, printed by the run, so it is
reproducible. The durable numbers include the whole commit: encode, append,
flush to the device, read back and verify, then publish by atomic manifest
replacement. **REAL execution on a SYNTHETIC generated workload; no hardware was
involved.** These numbers describe this build on this host and nothing else: no
before/after comparison and no speedup over any other implementation is claimed,
because there is nothing to compare with.

In-memory completed transitions on an ephemeral runtime:

| Objects | Operations | Completed | Elapsed ms | Operations/s |
| --- | --- | --- | --- | --- |
| 1 | 2000 | 2000 | 1577.148 | 1268.112 |
| 100 | 2000 | 2000 | 1479.513 | 1351.796 |
| 1000 | 2000 | 2000 | 3728.914 | 536.349 |

The state digest a durable commit computes and publishes, measured on its own:

| Objects | Calls | Elapsed ms | Microseconds per digest |
| --- | --- | --- | --- |
| 1 | 200 | 1.495 | 7.474 |
| 100 | 200 | 67.841 | 339.203 |
| 1000 | 200 | 685.866 | 3429.330 |

Durable completed commits against a real store:

| Objects | Operations | Completed | Elapsed ms | Operations/s | Median ms |
| --- | --- | --- | --- | --- | --- |
| 1 | 200 | 200 | 1560.038 | 128.202 | 7.699 |
| 100 | 200 | 200 | 1629.687 | 122.723 | 8.092 |
| 1000 | 200 | 200 | 2621.715 | 76.286 | 12.952 |

### What was not validated

* **No physical data center hardware was involved.** Every plant side behaviour
  in this repository is SYNTHETIC: there is no accelerator, switch, rack, PDU or
  cooling unit in any test, and none of the results above say anything about
  real hardware. Location, model, firmware generation and site are references
  the caller supplies, and that is what the tests exercise.
* **No second compiler was exercised.** The build rules for GCC and Clang are
  present and use the same per-target policy, but no GCC or Clang toolchain
  existed on the validating host, so no such build is claimed.
* **The POSIX branch of the platform layer was not compiled.** It is written
  (open, fcntl, flock, fsync, rename, ftruncate) and reviewed, but only the
  Windows path was exercised, because the validating host is Windows.
* **No sanitizer run is claimed.** `HL_SANITIZERS` is provided and applies
  `/fsanitize=address` on MSVC and `-fsanitize=address,undefined` elsewhere, but
  the results above come from ordinary Release, Debug and shared builds.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
