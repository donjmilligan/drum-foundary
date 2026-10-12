cmake_minimum_required(VERSION 3.22)

project(DrumFoundry VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(DRUMFOUNDRY_BUILD_TOOLS "Build the offline render / UI snapshot dev tool" ON)

# Copying into the system plug-in folders: on macOS they're per-user (~/Library/Audio/Plug-Ins),
# but on Windows the VST3 folder is C:\Program Files\Common Files\VST3, which needs an
# administrator prompt, so there it's off unless asked for.
if(APPLE)
    option(DRUMFOUNDRY_COPY_PLUGINS "Copy the built plug-ins into the system plug-in folders" ON)
else()
    option(DRUMFOUNDRY_COPY_PLUGINS "Copy the built plug-ins into the system plug-in folders" OFF)
endif()

if(MSVC)
    # Link the C++ runtime statically, so the plug-in loads on machines without the
    # Visual C++ Redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# JUCE 8.0.15: the copy in libs/JUCE, or, when that's empty (a fresh clone, or a source zip),
# a download of the same tag.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/CMakeLists.txt")
    add_subdirectory(libs/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Shared code: everything the plugin is made of. Consumed by the plugin target
# and by the dev tool so both compile exactly the same DSP and UI.
# ---------------------------------------------------------------------------
# Images the UI draws (the Theres Tears Records logo) and its fonts (Inter and Archivo,
# SIL Open Font License, see assets/fonts), compiled into the binary.
juce_add_binary_data(DrumFoundryAssets
    HEADER_NAME DrumFoundryAssets.h
    NAMESPACE DrumFoundryAssets
    SOURCES assets/Theres_Tears_Logo_Title_white_site.png
            assets/fonts/Inter-Regular.ttf assets/fonts/Inter-SemiBold.ttf
            assets/fonts/ArchivoSemiExpanded-SemiBold.ttf assets/fonts/ArchivoExpanded-Bold.ttf
            assets/fonts/ArchivoExpanded-ExtraLight.ttf)

# Cloud Seed (libs/CloudSeedCore, MIT, vendored with a few patches) isn't written for our
# warning flags: silence them for it, and for the one file that includes its headers.
set_source_files_properties(libs/CloudSeedCore/DSP/Biquad.cpp libs/CloudSeedCore/DSP/RandomBuffer.cpp src/dsp/CloudReverb.cpp
    PROPERTIES COMPILE_OPTIONS "-w")

add_library(DrumFoundryCode INTERFACE)

target_sources(DrumFoundryCode INTERFACE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/UndoHistory.cpp
    src/Parameters.cpp
    src/dsp/Envelope.cpp
    src/dsp/DrumVoice.cpp
    src/dsp/DrumEngine.cpp
    src/dsp/MasterChain.cpp
    src/dsp/CloudReverb.cpp
    libs/CloudSeedCore/DSP/Biquad.cpp
    libs/CloudSeedCore/DSP/RandomBuffer.cpp
    src/dsp/ParametricEq.cpp
    src/dsp/Clipper.cpp
    src/dsp/OutputFilter.cpp
    src/dsp/Reverb.cpp
    src/dsp/OfflineRenderer.cpp
    src/dsp/TransientTrigger.cpp
    src/presets/FactoryPresets.cpp
    src/presets/PresetManager.cpp
    src/samples/SampleBank.cpp
    src/ui/FoundryLookAndFeel.cpp
    src/ui/Controls.cpp
    src/ui/EnvelopeEditor.cpp
    src/ui/EqPanel.cpp
    src/ui/CompScope.cpp
    src/ui/HeaderBar.cpp
    src/ui/KeyMap.cpp
    src/ui/SpectrumAnalyzer.cpp
    src/ui/SampleBrowser.cpp
    src/ui/SampleSlotWidget.cpp
    src/ui/LayerPanels.cpp
    src/ui/LfoPanel.cpp
    src/ui/FxChainStrip.cpp
    src/ui/ReverbPanel.cpp)

target_include_directories(DrumFoundryCode INTERFACE src)

target_compile_definitions(DrumFoundryCode INTERFACE
    DRUMFOUNDRY_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

if(MSVC)
    # The sources are UTF-8, a few files are big, and Cloud Seed uses M_PI.
    target_compile_options(DrumFoundryCode INTERFACE /utf-8 /bigobj)
    target_compile_definitions(DrumFoundryCode INTERFACE _USE_MATH_DEFINES)
endif()

target_link_libraries(DrumFoundryCode INTERFACE
    DrumFoundryAssets
    juce::juce_audio_utils
    juce::juce_dsp)

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
if(APPLE)
    set(DRUMFOUNDRY_FORMATS VST3 AU Standalone)
else()
    set(DRUMFOUNDRY_FORMATS VST3 Standalone)
endif()

juce_add_plugin(DrumFoundry
    PRODUCT_NAME "Drum Foundry"
    COMPANY_NAME "Theres Tears Records"
    ICON_BIG assets/Theres_Tears_Icon.png
    BUNDLE_ID com.therestears.drumfoundry
    PLUGIN_MANUFACTURER_CODE Mlgn
    PLUGIN_CODE Drf1
    FORMATS ${DRUMFOUNDRY_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Drum Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD ${DRUMFOUNDRY_COPY_PLUGINS})

target_link_libraries(DrumFoundry
    PRIVATE
        DrumFoundryCode
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Dev tool: renders factory presets to WAV + PNG, snapshots the editor, and
# benchmarks the realtime path. Not shipped.
# ---------------------------------------------------------------------------
if(DRUMFOUNDRY_BUILD_TOOLS)
    juce_add_console_app(DrumFoundryTools PRODUCT_NAME "drumfoundry-tools")
    target_sources(DrumFoundryTools PRIVATE tools/Tools.cpp tools/Measure.cpp tools/ClickAnalysis.cpp tools/EqChecks.cpp tools/CompChecks.cpp tools/OrderChecks.cpp)
    target_link_libraries(DrumFoundryTools
        PRIVATE
            DrumFoundryCode
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
