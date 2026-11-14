# NovaInsert product targets (F3). Own code: ${NI_WARNINGS} (root CMakeLists); third-party is SYSTEM.

add_library(novainsert_engine INTERFACE)
target_include_directories(novainsert_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(novainsert_engine INTERFACE cxx_std_20)

add_library(novainsert_host STATIC)
target_sources(novainsert_host PRIVATE host/host_lib.cpp host/reaper_api.cpp host/reaper_api.hpp host/reaper_types.hpp host/slot_name.cpp host/slot_name.hpp host/hook_dispatcher.cpp host/hook_dispatcher.hpp host/diagnostics.cpp host/diagnostics.hpp host/coordinator.cpp host/coordinator.hpp host/pair_registry.cpp host/pair_registry.hpp host/scheduler.cpp host/scheduler.hpp host/host_settings.cpp host/host_settings.hpp host/platform.hpp host/platform.cpp plugin/instance.cpp plugin/instance.hpp plugin/parameters.hpp engine/version.hpp engine/engine_c.cpp engine/engine_c.h engine/correlator.hpp engine/probe.hpp plugin/measurer.cpp plugin/measurer.hpp host/render_arbiter.cpp host/render_arbiter.hpp plugin/pdc_reporter.cpp plugin/pdc_reporter.hpp engine/pdc_observer.hpp engine/state_machine.hpp plugin/correlator_worker.cpp plugin/correlator_worker.hpp)
target_include_directories(novainsert_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(novainsert_host SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/external/reaper-sdk/sdk
    ${CMAKE_SOURCE_DIR}/external/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK)
target_link_libraries(novainsert_host PUBLIC novainsert_engine)
target_compile_options(novainsert_host PRIVATE ${NI_WARNINGS})
if(UNIX AND NOT APPLE)
  target_link_libraries(novainsert_host PUBLIC pthread dl rt)
endif()

# Device library read side (juce::JSON) – separate so the host library stays JUCE-free.
add_library(novainsert_library STATIC plugin/library.cpp plugin/library.hpp plugin/library_json.hpp plugin/persistence.cpp plugin/persistence.hpp plugin/cal_controller.cpp plugin/cal_controller.hpp)
target_include_directories(novainsert_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(novainsert_library PUBLIC juce::juce_core)
target_compile_definitions(novainsert_library PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_compile_options(novainsert_library PRIVATE ${NI_WARNINGS})

juce_add_plugin(NovaInsert
    COMPANY_NAME "Hillside Studios"
    BUNDLE_ID com.hillsidestudios.NovaInsert
    PLUGIN_MANUFACTURER_CODE Nvin
    PLUGIN_CODE Nvi1
    FORMATS VST3
    PRODUCT_NAME "NovaInsert"
    VST3_CATEGORIES Fx Tools
    COPY_PLUGIN_AFTER_BUILD FALSE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE)
target_sources(NovaInsert PRIVATE plugin/processor.cpp plugin/processor.hpp host/render_window.hpp host/render_window_title.hpp gui/editor.cpp gui/editor.hpp gui/main_panel.cpp gui/main_panel.hpp gui/vu_meter.cpp gui/vu_meter.hpp gui/level_bars.cpp gui/level_bars.hpp gui/bar_scale.hpp gui/controls.cpp gui/controls.hpp gui/setup_panel.cpp gui/setup_panel.hpp gui/devices_panel.cpp gui/devices_panel.hpp gui/view_model.hpp gui/vu_needle.hpp)
if(APPLE)
  target_sources(NovaInsert PRIVATE host/render_window_mac.mm)
else()
  target_sources(NovaInsert PRIVATE host/render_window_win.cpp)   # Win32, or SWELL via SWELL_dllMain on Linux
endif()
target_include_directories(NovaInsert PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(NovaInsert SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/external/reaper-sdk/sdk
    ${CMAKE_SOURCE_DIR}/external/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK)
target_compile_definitions(NovaInsert PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_options(NovaInsert PRIVATE ${NI_WARNINGS})
target_link_libraries(NovaInsert PRIVATE
    novainsert_host
    novainsert_library
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
