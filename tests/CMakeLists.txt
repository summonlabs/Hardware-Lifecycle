# Hardware Lifecycle - test suites.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Every suite is a real program that links the real library. Tests that make a
# claim about a second process, about a durable artifact or about an installed
# package start real processes and touch real files; nothing here simulates a
# claim that can be tested directly on the host.
#
# No test declares a timeout. A hanging test is a defect to diagnose.

add_library(hl_test_support STATIC support/test_framework.cpp support/process.cpp)
target_include_directories(hl_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(hl_test_support PUBLIC HardwareLifecycle::hardware_lifecycle)
hl_configure_target(hl_test_support)

function(hl_add_test_suite suite_name)
  hl_add_executable(${suite_name} ${ARGN})
  target_link_libraries(${suite_name} PRIVATE hl_test_support)
  # A few durability proofs read the documented on-disk layout directly in order
  # to rewrite a manifest field and check that the reader refuses it. That is the
  # point of those tests, so they may see the internal headers.
  target_include_directories(${suite_name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  # The proof harnesses are passed as arguments, never compiled in: a Windows
  # path inside a macro would be read as a C++ escape sequence, and CMake quotes
  # test arguments correctly.
  add_test(NAME ${suite_name} COMMAND ${suite_name}
    --lock-child "$<TARGET_FILE:hl_lock_child>"
    --crash-child "$<TARGET_FILE:hl_crash_child>"
    --cli "$<TARGET_FILE:hwlifecycle>")
  # Zero means no timeout in CTest. Tests run to completion or they are defects.
  set_tests_properties(${suite_name} PROPERTIES TIMEOUT 0)
endfunction()

hl_add_test_suite(hl_unit_tests
  unit/unit_ids_text.cpp
  unit/unit_digest_errors.cpp
  unit/unit_lifecycle_table.cpp)

hl_add_test_suite(hl_domain_tests
  domain/domain_registry.cpp
  domain/domain_transitions.cpp
  domain/domain_replacement.cpp
  domain/domain_history.cpp)

hl_add_test_suite(hl_persistence_tests
  persistence/persistence_roundtrip.cpp
  persistence/persistence_corruption.cpp
  persistence/persistence_crash.cpp
  persistence/persistence_lock.cpp)

hl_add_test_suite(hl_property_tests property/property_state_machine.cpp)

hl_add_test_suite(hl_adversarial_tests
  adversarial/adversarial_requests.cpp
  adversarial/adversarial_documents.cpp)

hl_add_test_suite(hl_concurrency_tests concurrency/concurrency_readers.cpp)

hl_add_test_suite(hl_cli_tests cli/cli_end_to_end.cpp)

# Installed-artifact and downstream-consumer proofs need an install prefix, so
# they are enabled explicitly by the validation procedure that performs the
# install first. They are never silently skipped: when the option is on and the
# prefix is missing, the test fails.
option(HL_ENABLE_INSTALLED_TESTS "Add the installed artifact and downstream consumer tests" OFF)
set(HL_INSTALLED_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install prefix used by the installed artifact tests")

if(HL_ENABLE_INSTALLED_TESTS)
  add_test(NAME hl_installed_cli
    COMMAND ${CMAKE_COMMAND}
            -DHL_PREFIX=${HL_INSTALLED_PREFIX}
            -DHL_EXPECTED_VERSION=${PROJECT_VERSION}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/installed/installed_cli_test.cmake")
  set_tests_properties(hl_installed_cli PROPERTIES TIMEOUT 0)

  add_test(NAME hl_downstream_consumer
    COMMAND ${CMAKE_COMMAND}
            -DHL_PREFIX=${HL_INSTALLED_PREFIX}
            -DHL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/installed/downstream
            -DHL_WORK_DIR=${CMAKE_BINARY_DIR}/downstream
            -DHL_GENERATOR=${CMAKE_GENERATOR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/installed/downstream_test.cmake")
  set_tests_properties(hl_downstream_consumer PROPERTIES TIMEOUT 0)
endif()
