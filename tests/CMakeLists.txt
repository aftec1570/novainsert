# Unit / RT tests (Catch2 v3.7.1, external/Catch2). Registered with CTest via catch_discover_tests.
add_subdirectory(${CMAKE_SOURCE_DIR}/external/Catch2 ${CMAKE_BINARY_DIR}/external/Catch2 SYSTEM)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/external/Catch2/extras)
include(Catch)

file(GLOB NOVAINSERT_UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
# Test names must be ASCII: catch_discover_tests passes each name to the test binary as a filter, and the Windows console
# code page mangles UTF-8 there ("No test cases matched").
foreach(src ${NOVAINSERT_UNIT_SOURCES})
  file(STRINGS ${src} bad ENCODING UTF-8 REGEX "TEST_CASE\\(\"[^\"]*[^\t -~][^\"]*\"")
  if(bad)
    message(FATAL_ERROR "Non-ASCII test name in ${src}: ${bad}")
  endif()
endforeach()
add_executable(novainsert_unit_tests ${NOVAINSERT_UNIT_SOURCES} rt/alloc_guard.cpp)
target_include_directories(novainsert_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(novainsert_unit_tests PRIVATE ${NI_WARNINGS})
target_compile_definitions(novainsert_unit_tests PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(novainsert_unit_tests PRIVATE novainsert_host novainsert_library Catch2::Catch2WithMain)
catch_discover_tests(novainsert_unit_tests)

# f3_4 fixture oracle helper (tests/tools/probe_lag.cpp): product probe + correlator around F05Loop captures.
add_executable(ni_probe_lag tools/probe_lag.cpp)
target_include_directories(ni_probe_lag PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(ni_probe_lag PRIVATE ${NI_WARNINGS})

# GUI snapshot tool (VU GUI plan Task 9): renders the VU meter and the editor body with fixed ViewInputs to PNGs.
add_executable(ni_gui_snapshot tools/gui_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/main_panel.cpp ${CMAKE_SOURCE_DIR}/src/gui/vu_meter.cpp ${CMAKE_SOURCE_DIR}/src/gui/level_bars.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/controls.cpp ${CMAKE_SOURCE_DIR}/src/gui/setup_panel.cpp ${CMAKE_SOURCE_DIR}/src/gui/devices_panel.cpp)
target_include_directories(ni_gui_snapshot PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ni_gui_snapshot PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_compile_options(ni_gui_snapshot PRIVATE ${NI_WARNINGS})
target_link_libraries(ni_gui_snapshot PRIVATE novainsert_engine juce::juce_gui_basics)

# Manual screenshots (docs/manual): every editor state the user manual shows, rendered off-line like ni_gui_snapshot.
add_executable(ni_manual_shots tools/manual_shots.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/main_panel.cpp ${CMAKE_SOURCE_DIR}/src/gui/vu_meter.cpp ${CMAKE_SOURCE_DIR}/src/gui/level_bars.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/controls.cpp ${CMAKE_SOURCE_DIR}/src/gui/setup_panel.cpp ${CMAKE_SOURCE_DIR}/src/gui/devices_panel.cpp)
target_include_directories(ni_manual_shots PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ni_manual_shots PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_compile_options(ni_manual_shots PRIVATE ${NI_WARNINGS})
target_link_libraries(ni_manual_shots PRIVATE novainsert_engine juce::juce_gui_basics)
