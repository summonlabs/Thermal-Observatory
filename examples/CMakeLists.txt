# Worked examples. Each one is a small program that a reader can run and modify.

set(THERMAL_EXAMPLES
  minimal_analysis.cpp
  headroom_and_transitions.cpp
  persist_and_recover.cpp)

foreach(example IN LISTS THERMAL_EXAMPLES)
  get_filename_component(example_name ${example} NAME_WE)
  add_executable(thermal_example_${example_name} ${example})
  target_link_libraries(thermal_example_${example_name} PRIVATE ThermalObservatory::thermal_observatory)
  target_link_libraries(thermal_example_${example_name} PRIVATE
    $<BUILD_INTERFACE:thermal_warnings>
    $<BUILD_INTERFACE:thermal_sanitizers>)
endforeach()
