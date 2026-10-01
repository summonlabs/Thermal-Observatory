# Thermal Observatory - test suites.
#
# Every suite is registered as its own CTest test so that a failure names the suite that failed.
# The framework has no timeout and no test declares one: a hang is a defect to diagnose, not a
# result to accept.

set(THERMAL_TEST_SUPPORT test_framework.cpp test_support.cpp)

add_library(thermal_test_support STATIC ${THERMAL_TEST_SUPPORT})
target_include_directories(thermal_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(thermal_test_support PUBLIC ThermalObservatory::thermal_observatory)
target_link_libraries(thermal_test_support PRIVATE
  $<BUILD_INTERFACE:thermal_warnings>
  $<BUILD_INTERFACE:thermal_sanitizers>)
# The tool's path is resolved at run time from the test executable's own location; see cli_path()
# in tests/test_support.cpp. No path is ever baked into a compile definition.

set(THERMAL_SUITE_FILES
  suite_core.cpp
  suite_model.cpp
  suite_evidence.cpp
  suite_envelope.cpp
  suite_hotspot.cpp
  suite_coupling.cpp
  suite_derating.cpp
  suite_analysis.cpp
  suite_persistence.cpp
  suite_restart.cpp
  suite_concurrency.cpp
  suite_property.cpp
  suite_adversarial.cpp
  suite_hardening.cpp
  suite_cli.cpp)

add_executable(tobsv_tests ${THERMAL_SUITE_FILES})
target_link_libraries(tobsv_tests PRIVATE thermal_test_support)
target_link_libraries(tobsv_tests PRIVATE
  $<BUILD_INTERFACE:thermal_warnings>
  $<BUILD_INTERFACE:thermal_sanitizers>)

set(THERMAL_SUITE_NAMES
  core
  model
  evidence
  envelope
  hotspot
  coupling
  derating
  analysis
  persistence
  restart
  concurrency
  property
  adversarial
  hardening
  cli)

foreach(suite IN LISTS THERMAL_SUITE_NAMES)
  add_test(NAME suite.${suite} COMMAND tobsv_tests --suite ${suite})
endforeach()

# The package proof: install into a scratch prefix, then configure, build and run an independent
# project that resolves the package with find_package(ThermalObservatory CONFIG).
add_test(NAME package.downstream
  COMMAND ${CMAKE_COMMAND}
    -DTHERMAL_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DTHERMAL_PROJECT_BUILD_DIR=${CMAKE_BINARY_DIR}
    -DTHERMAL_BUILD_DIR=${CMAKE_BINARY_DIR}/downstream-build
    -DTHERMAL_INSTALL_DIR=${CMAKE_BINARY_DIR}/downstream-install
    -DTHERMAL_GENERATOR=${CMAKE_GENERATOR}
    -DTHERMAL_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DTHERMAL_BUILD_TYPE=$<CONFIG>
    -DTHERMAL_CONFIG=$<CONFIG>
    -P ${CMAKE_SOURCE_DIR}/cmake/DownstreamVerify.cmake)

# A downstream consumer linked through the exported interface, built inside this tree. The
# find_package proof itself lives in cmake/DownstreamVerify.cmake.
add_executable(thermal_downstream_check downstream/consumer.cpp)
target_link_libraries(thermal_downstream_check PRIVATE ThermalObservatory::thermal_observatory)
target_link_libraries(thermal_downstream_check PRIVATE
  $<BUILD_INTERFACE:thermal_warnings>
  $<BUILD_INTERFACE:thermal_sanitizers>)
# The suites invoke the command line tool through a real child process, so it must exist first.
if(TARGET thermal_observatory_tool)
  add_dependencies(tobsv_tests thermal_observatory_tool)
else()
  message(WARNING "THERMAL_BUILD_TOOLS is off: the cli suite will not find the tool to run")
endif()