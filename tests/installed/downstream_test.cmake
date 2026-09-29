# Configures, builds and runs the out of tree consumer against an installed
# prefix. Invoked by CTest with -DHL_PREFIX, -DHL_SOURCE_DIR, -DHL_WORK_DIR and
# -DHL_GENERATOR. Nothing is simulated: this is a real configure, a real build
# and a real execution of an executable that links the installed library.

foreach(required HL_PREFIX HL_SOURCE_DIR HL_WORK_DIR HL_GENERATOR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "downstream_test: ${required} was not supplied")
  endif()
endforeach()

if(NOT EXISTS "${HL_PREFIX}")
  message(FATAL_ERROR "downstream_test: install prefix ${HL_PREFIX} does not exist; install the project first")
endif()

file(REMOVE_RECURSE "${HL_WORK_DIR}")
file(MAKE_DIRECTORY "${HL_WORK_DIR}")
set(build_dir "${HL_WORK_DIR}/build")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${HL_SOURCE_DIR}" -B "${build_dir}" -G "${HL_GENERATOR}"
          "-DCMAKE_PREFIX_PATH=${HL_PREFIX}" -DCMAKE_BUILD_TYPE=Release
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed (${configure_result})
${configure_output}
${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed (${build_result})
${build_output}
${build_error}")
endif()

execute_process(
  COMMAND "${build_dir}/hl_downstream_consumer"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream consumer failed (${run_result})
${run_output}
${run_error}")
endif()

message(STATUS "downstream consumer output:
${run_output}")
