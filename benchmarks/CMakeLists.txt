# Benchmarks. They measure completed work and label what they measured.

add_executable(thermal_bench bench_observatory.cpp)
target_link_libraries(thermal_bench PRIVATE ThermalObservatory::thermal_observatory)
target_link_libraries(thermal_bench PRIVATE
  $<BUILD_INTERFACE:thermal_warnings>
  $<BUILD_INTERFACE:thermal_sanitizers>)
