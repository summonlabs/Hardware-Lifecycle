# Exercises the installed command line tool rather than the in tree one.
#
# Invoked by CTest with -DHL_PREFIX and -DHL_EXPECTED_VERSION. Every check runs
# the installed binary, against a real store on disk, and asserts the documented
# exit code rather than merely "it failed".

if(NOT DEFINED HL_PREFIX OR NOT DEFINED HL_EXPECTED_VERSION)
  message(FATAL_ERROR "installed_cli_test: HL_PREFIX and HL_EXPECTED_VERSION are required")
endif()

set(tool "${HL_PREFIX}/bin/hwlifecycle")
if(WIN32)
  set(tool "${tool}.exe")
endif()
if(NOT EXISTS "${tool}")
  message(FATAL_ERROR "installed_cli_test: ${tool} was not installed")
endif()

# Beside the prefix, never inside it: the installed tree stays exactly what
# the install rules produced.
set(work "${HL_PREFIX}/../hl_installed_cli_work")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

function(hl_run label expected)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL ${expected})
    message(FATAL_ERROR "${label}: exit code ${code}, expected ${expected}\n${out}\n${err}")
  endif()
  message(STATUS "${label}: ok (exit ${code})")
endfunction()

# Evidence digests are 64 hexadecimal characters; the values are arbitrary but
# fixed, so two runs of this test compare the same documents.
set(proc_digest "1111111111111111111111111111111111111111111111111111111111111111")
set(delivery_digest "2222222222222222222222222222222222222222222222222222222222222222")
set(install_digest "3333333333333333333333333333333333333333333333333333333333333333")
set(attest_digest "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")

set(store "${work}/store")
set(second "${work}/store2")

hl_run("version" 0 "${tool}" --version)
hl_run("limits" 0 "${tool}" limits)
hl_run("compatibility" 0 "${tool}" compatibility)
hl_run("diagram" 0 "${tool}" diagram)
hl_run("legal-transitions" 0 "${tool}" legal-transitions --state Ordered)
hl_run("explain an illegal pair" 0 "${tool}" explain --from Maintenance --to Removed)

hl_run("create" 0 "${tool}" --store "${store}" create
  --asset installed-node --hardware-generation 1 --kind compute --model model-x
  --plan p1 --attempt a1 --actor operator-1 --actor-kind operator
  --authority procurement --evidence "procurement_record:${proc_digest}:erp/po-1")
hl_run("inspect" 0 "${tool}" --store "${store}" inspect --asset installed-node)

# Authority is never inherited across a process boundary, so the second
# invocation is refused until it re-establishes authority itself.
hl_run("a mutation without authority is refused" 2 "${tool}" --store "${store}" transition
  --asset installed-node --hardware-generation 1 --to Staged --reason delivery_accepted
  --plan p2 --attempt a1 --actor operator-1 --actor-kind operator
  --authority logistics --evidence "delivery_receipt:${delivery_digest}:wms/receipt-1")

hl_run("transition with an explicit attestation" 0 "${tool}" --store "${store}" transition
  --asset installed-node --hardware-generation 1 --to Staged --reason delivery_accepted --attest
  --plan p2 --attempt a1 --actor operator-1 --actor-kind operator
  --authority logistics,recovery
  --evidence "delivery_receipt:${delivery_digest}:wms/receipt-1"
  --evidence "recovery_attestation:${attest_digest}:ops/attest-1")

# Staged -> Active is not an edge of the transition table, so it is rejected as
# a domain error even though authority was re-established.
hl_run("an edge that is not in the table is rejected" 2 "${tool}" --store "${store}" transition
  --asset installed-node --hardware-generation 1 --to Active --reason commissioning_passed --attest
  --plan p3 --attempt a1 --actor operator-1 --actor-kind operator
  --authority service,recovery
  --evidence "commissioning_report:${install_digest}:cmdb/report-1"
  --evidence "recovery_attestation:${attest_digest}:ops/attest-1")

# Staged -> Installed demands an installation record and a location record.
hl_run("a missing evidence kind is rejected" 2 "${tool}" --store "${store}" transition
  --asset installed-node --hardware-generation 1 --to Installed --reason installation_completed --attest
  --location dc-1/rack-01/u12
  --plan p4 --attempt a1 --actor operator-1 --actor-kind operator
  --authority installation,recovery
  --evidence "installation_record:${install_digest}:cmdb/install-1"
  --evidence "recovery_attestation:${attest_digest}:ops/attest-1")

hl_run("transition with every required evidence kind" 0 "${tool}" --store "${store}" transition
  --asset installed-node --hardware-generation 1 --to Installed --reason installation_completed --attest
  --location dc-1/rack-01/u12
  --plan p4 --attempt a1 --actor operator-1 --actor-kind operator
  --authority installation,recovery
  --evidence "installation_record:${install_digest}:cmdb/install-1"
  --evidence "location_record:${install_digest}:cmdb/location-1"
  --evidence "recovery_attestation:${attest_digest}:ops/attest-1")

hl_run("history" 0 "${tool}" --store "${store}" history --asset installed-node)
hl_run("lineage" 0 "${tool}" --store "${store}" lineage --asset installed-node)
hl_run("list" 0 "${tool}" --store "${store}" list --state Installed)
hl_run("json inspect" 0 "${tool}" --store "${store}" --json inspect --asset installed-node)

set(document "${work}/snapshot.json")
hl_run("export" 0 "${tool}" --store "${store}" export --out "${document}")
hl_run("verify" 0 "${tool}" --store "${store}" verify)
hl_run("recover" 0 "${tool}" --store "${store}" recover)
hl_run("import" 0 "${tool}" --store "${second}" import --in "${document}")
hl_run("diff a document against itself" 0 "${tool}" diff --before "${document}" --after "${document}")

# A read only open answers queries and refuses mutations.
hl_run("read only inspect" 0 "${tool}" --store "${store}" --read-only inspect --asset installed-node)
hl_run("read only mutation is refused" 2 "${tool}" --store "${store}" --read-only gate
  --asset installed-node --hardware-generation 1 --gate open
  --plan p5 --attempt a1 --actor operator-1 --actor-kind operator
  --authority service --evidence "service_record:${install_digest}:itsm/gate-1")

hl_run("usage error" 1 "${tool}" --store "${store}" inspect)
hl_run("unknown asset" 2 "${tool}" --store "${store}" inspect --asset no-such-asset)
# A third store that was populated independently: it is a real store that simply
# never saw the exported document, so the object is not there.
hl_run("a third store is populated independently" 0 "${tool}" --store "${work}/store3" create
  --asset other-node --hardware-generation 1 --kind storage --model model-y
  --plan q1 --attempt a1 --actor operator-1 --actor-kind operator
  --authority procurement --evidence "procurement_record:${proc_digest}:erp/po-9")
hl_run("a store that never saw the document cannot see the object" 2 "${tool}" --store "${work}/store3" inspect --asset installed-node)
hl_run("but a store that does not exist yet is a store failure" 3 "${tool}" --store "${work}/store9" inspect --asset installed-node)
hl_run("but the imported store has it" 0 "${tool}" --store "${second}" inspect --asset installed-node)

message(STATUS "installed command line tool validated")
