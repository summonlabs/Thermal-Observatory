# Verifies that an installed Thermal Observatory package can be consumed by an independent CMake
# project through find_package(ThermalObservatory CONFIG).
#
# Required variables:
#   THERMAL_SOURCE_DIR   repository root
#   THERMAL_BUILD_DIR    scratch directory for the downstream build
#   THERMAL_INSTALL_DIR  install prefix produced by "cmake --install"

if(NOT DEFINED THERMAL_SOURCE_DIR OR NOT DEFINED THERMAL_BUILD_DIR OR NOT DEFINED THERMAL_INSTALL_DIR)
  message(FATAL_ERROR "DownstreamVerify.cmake requires THERMAL_SOURCE_DIR, THERMAL_BUILD_DIR and THERMAL_INSTALL_DIR")
endif()

# The consumer is built in the same configuration as the library it links. Building it as Release
# while installing a Debug library selects a different C runtime and fails to link, which would say
# nothing about whether the package resolves.
if(NOT DEFINED THERMAL_BUILD_TYPE OR THERMAL_BUILD_TYPE STREQUAL "")
  set(THERMAL_BUILD_TYPE "Release")
endif()
if(NOT DEFINED THERMAL_CONFIG OR THERMAL_CONFIG STREQUAL "")
  set(THERMAL_CONFIG "${THERMAL_BUILD_TYPE}")
endif()

file(REMOVE_RECURSE "${THERMAL_BUILD_DIR}")
file(MAKE_DIRECTORY "${THERMAL_BUILD_DIR}")

# Install the library into a scratch prefix exactly as a distribution would.
file(REMOVE_RECURSE "${THERMAL_INSTALL_DIR}")
set(install_command "${CMAKE_COMMAND}" --install "${THERMAL_PROJECT_BUILD_DIR}" --prefix "${THERMAL_INSTALL_DIR}")
if(DEFINED THERMAL_CONFIG AND NOT THERMAL_CONFIG STREQUAL "")
  list(APPEND install_command --config "${THERMAL_CONFIG}")
endif()
execute_process(COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install step failed:\n${install_output}\n${install_error}")
endif()

# The downstream project is configured with the same compiler the library was built with. Naming
# it explicitly keeps the proof reproducible from any shell, including one without a developer
# environment; the claim under test is that find_package resolves the installed package, and the
# compiler choice does not affect that.
set(configure_command "${CMAKE_COMMAND}"
    -S "${THERMAL_SOURCE_DIR}/tests/downstream"
    -B "${THERMAL_BUILD_DIR}"
    -DCMAKE_PREFIX_PATH=${THERMAL_INSTALL_DIR}
    -DCMAKE_BUILD_TYPE=${THERMAL_BUILD_TYPE})
if(DEFINED THERMAL_GENERATOR AND NOT THERMAL_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${THERMAL_GENERATOR}")
endif()
if(DEFINED THERMAL_CXX_COMPILER AND NOT THERMAL_CXX_COMPILER STREQUAL "")
  list(APPEND configure_command -DCMAKE_CXX_COMPILER=${THERMAL_CXX_COMPILER})
endif()

execute_process(COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed:\n${configure_output}\n${configure_error}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${THERMAL_BUILD_DIR}" --config ${THERMAL_BUILD_TYPE}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed:\n${build_output}\n${build_error}")
endif()

find_program(THERMAL_DOWNSTREAM_EXECUTABLE
  NAMES downstream_consumer
  PATHS "${THERMAL_BUILD_DIR}/Release" "${THERMAL_BUILD_DIR}" "${THERMAL_BUILD_DIR}/bin"
  NO_DEFAULT_PATH)
if(NOT THERMAL_DOWNSTREAM_EXECUTABLE)
  message(FATAL_ERROR "downstream executable was not produced in ${THERMAL_BUILD_DIR}")
endif()

execute_process(COMMAND "${THERMAL_DOWNSTREAM_EXECUTABLE}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream run failed (exit ${run_result}):\n${run_output}\n${run_error}")
endif()

message(STATUS "downstream consumer output: ${run_output}")