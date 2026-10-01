# The thermal-observatory command line tool.

add_executable(thermal_observatory_tool tobsv_main.cpp)
set_target_properties(thermal_observatory_tool PROPERTIES OUTPUT_NAME thermal-observatory)

target_link_libraries(thermal_observatory_tool PRIVATE ThermalObservatory::thermal_observatory)
target_link_libraries(thermal_observatory_tool PRIVATE
  $<BUILD_INTERFACE:thermal_warnings>
  $<BUILD_INTERFACE:thermal_sanitizers>)

install(TARGETS thermal_observatory_tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
