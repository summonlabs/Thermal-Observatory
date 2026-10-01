cmake_minimum_required(VERSION 3.22)

# Thermal Observatory - thermal observation, explanation and attribution.
#
# The library observes, explains and attributes. It does not actuate cooling, govern thermal policy,
# own thermal zones, place workloads or perform recovery.

project(ThermalObservatory
  VERSION 1.0.0
  LANGUAGES CXX
  DESCRIPTION "Thermal observation, headroom, hotspot, coupling and derating evidence with explicit attribution limits"
  HOMEPAGE_URL "https://github.com/summonlabs/Thermal-Observatory")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(THERMAL_IS_TOP_LEVEL ON)
else()
  set(THERMAL_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(THERMAL_BUILD_TOOLS "Build the thermal-observatory command line tool" ${THERMAL_IS_TOP_LEVEL})
option(THERMAL_BUILD_TESTS "Build the test suites" ${THERMAL_IS_TOP_LEVEL})
option(THERMAL_BUILD_EXAMPLES "Build the examples" ${THERMAL_IS_TOP_LEVEL})
option(THERMAL_BUILD_BENCHMARKS "Build the benchmarks" ${THERMAL_IS_TOP_LEVEL})
option(THERMAL_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(THERMAL_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning configuration. The first-party warning count is zero in every
# configuration; /W4 /WX on MSVC and -Werror elsewhere enforce it rather than
# leaving it aspirational.
# ---------------------------------------------------------------------------
add_library(thermal_warnings INTERFACE)
if(MSVC)
  target_compile_options(thermal_warnings INTERFACE
    /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /MP)
else()
  target_compile_options(thermal_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2
    -Wimplicit-fallthrough -Werror)
endif()

# AddressSanitizer is only offered where the toolchain can actually link it. On MSVC it needs the
# optional "C++ AddressSanitizer" component; without it the link fails with a missing
# clang_rt.asan_dynamic_runtime_thunk library, which says nothing useful. Detect it here so the
# answer is a clear configuration error instead.
if(THERMAL_ENABLE_ASAN AND MSVC)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "/fsanitize=address")
  check_cxx_source_compiles("int main() { return 0; }" THERMAL_MSVC_ASAN_AVAILABLE)
  unset(CMAKE_REQUIRED_FLAGS)
  if(NOT THERMAL_MSVC_ASAN_AVAILABLE)
    message(FATAL_ERROR
      "THERMAL_ENABLE_ASAN is on but this MSVC installation cannot link /fsanitize=address. "
      "Install the 'C++ AddressSanitizer' component of Visual Studio, or configure with "
      "-DTHERMAL_ENABLE_ASAN=OFF. The Debug configuration still runs the checked debug CRT.")
  endif()
endif()

add_library(thermal_sanitizers INTERFACE)
if(THERMAL_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(thermal_sanitizers INTERFACE /fsanitize=address /Zi)
    target_link_options(thermal_sanitizers INTERFACE /INCREMENTAL:NO)
  else()
    target_compile_options(thermal_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(thermal_sanitizers INTERFACE -fsanitize=address)
  endif()
  target_compile_definitions(thermal_sanitizers INTERFACE THERMAL_ASAN_ENABLED=1)
endif()

# ---------------------------------------------------------------------------
# The runtime library.
# ---------------------------------------------------------------------------
set(THERMAL_PUBLIC_HEADERS
  include/tobsv/tobsv.hpp
  include/tobsv/version.hpp
  include/tobsv/core/checked.hpp
  include/tobsv/core/crc32c.hpp
  include/tobsv/core/format.hpp
  include/tobsv/core/hash.hpp
  include/tobsv/core/json.hpp
  include/tobsv/core/limits.hpp
  include/tobsv/core/result.hpp
  include/tobsv/core/strong_id.hpp
  include/tobsv/core/time.hpp
  include/tobsv/model/generation.hpp
  include/tobsv/model/inventory.hpp
  include/tobsv/model/topology.hpp
  include/tobsv/model/vocabulary.hpp
  include/tobsv/evidence/evidence.hpp
  include/tobsv/evidence/provenance.hpp
  include/tobsv/evidence/quality.hpp
  include/tobsv/evidence/store.hpp
  include/tobsv/envelope/envelope.hpp
  include/tobsv/envelope/headroom.hpp
  include/tobsv/hotspot/hotspot.hpp
  include/tobsv/coupling/coupling.hpp
  include/tobsv/derating/derating.hpp
  include/tobsv/attribution/attribution.hpp
  include/tobsv/analysis/analysis.hpp
  include/tobsv/persistence/codec.hpp
  include/tobsv/persistence/files.hpp
  include/tobsv/persistence/lock.hpp
  include/tobsv/persistence/log.hpp
  include/tobsv/persistence/worker.hpp
  include/tobsv/runtime/observatory.hpp)

set(THERMAL_SOURCES
  src/version.cpp
  src/core/result.cpp
  src/core/strong_id.cpp
  src/core/hash.cpp
  src/core/time.cpp
  src/core/limits.cpp
  src/core/json.cpp
  src/core/format.cpp
  src/model/vocabulary.cpp
  src/model/generation.cpp
  src/model/topology.cpp
  src/model/inventory.cpp
  src/evidence/quality.cpp
  src/evidence/provenance.cpp
  src/evidence/evidence.cpp
  src/evidence/store.cpp
  src/envelope/envelope.cpp
  src/envelope/headroom.cpp
  src/hotspot/hotspot.cpp
  src/coupling/coupling.cpp
  src/derating/derating.cpp
  src/attribution/attribution.cpp
  src/analysis/analysis.cpp
  src/persistence/files.cpp
  src/persistence/lock.cpp
  src/persistence/codec.cpp
  src/persistence/log.cpp
  src/persistence/worker.cpp
  src/runtime/observatory.cpp)

add_library(thermal_observatory STATIC ${THERMAL_SOURCES} ${THERMAL_PUBLIC_HEADERS})
add_library(ThermalObservatory::thermal_observatory ALIAS thermal_observatory)

target_include_directories(thermal_observatory
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(thermal_observatory PUBLIC Threads::Threads)
# Warning and sanitizer settings are build-time only and must not leak into the exported package.
target_link_libraries(thermal_observatory PRIVATE
  $<BUILD_INTERFACE:thermal_warnings>
  $<BUILD_INTERFACE:thermal_sanitizers>)

set_target_properties(thermal_observatory PROPERTIES
  OUTPUT_NAME thermal_observatory
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(THERMAL_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(THERMAL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(THERMAL_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(THERMAL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation and package export.
# ---------------------------------------------------------------------------
install(TARGETS thermal_observatory
  EXPORT ThermalObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ThermalObservatoryTargets
  FILE ThermalObservatoryTargets.cmake
  NAMESPACE ThermalObservatory::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThermalObservatory)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThermalObservatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalObservatoryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThermalObservatory)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalObservatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalObservatoryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalObservatoryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThermalObservatory)

install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/ThermalObservatory)

# ---------------------------------------------------------------------------
# Packaging.
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "ThermalObservatory")
set(CPACK_PACKAGE_VENDOR "Summon Software Labs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Thermal observation, explanation and attribution runtime")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Summon Software Labs")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_FILE_NAME "ThermalObservatory-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

message(STATUS "Thermal Observatory ${PROJECT_VERSION}: tests=${THERMAL_BUILD_TESTS} tools=${THERMAL_BUILD_TOOLS} examples=${THERMAL_BUILD_EXAMPLES} benchmarks=${THERMAL_BUILD_BENCHMARKS} asan=${THERMAL_ENABLE_ASAN}")