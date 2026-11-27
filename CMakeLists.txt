cmake_minimum_required(VERSION 3.22)

if(CMAKE_HOST_APPLE)
    # Universal (arm64 + x86_64). Natively on Apple silicon, Logic Pro hosts Audio Units out of process without ARA
    # (the capture mode works there); under Rosetta it can use ARA and loads the x86_64 slice.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

project(ShizukuTune VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SHIZUKU_BUILD_PLUGIN "Build the JUCE ARA plug-in (Audio Unit and VST3 on macOS, VST3 on Windows)" ON)

if(MSVC)
    # Sources are UTF-8 (Japanese UI strings); M_PI from <cmath>.
    add_compile_options(/utf-8)
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# ---------------------------------------------------------------------------
# Analysis library (no JUCE dependency, shared by the plugin and command-line tools)
add_library(shizuku_analysis STATIC
    src/analysis/Fft.cpp
    src/analysis/Json.cpp
    src/analysis/KeyDetector.cpp
    src/analysis/NoteSegmenter.cpp
    src/analysis/PitchTracker.cpp
    src/analysis/ReferenceMelody.cpp
    src/analysis/ReferenceSelection.cpp
    src/analysis/SongAnalyzer.cpp
    src/analysis/TempoDetector.cpp
    src/analysis/WavFile.cpp)
target_include_directories(shizuku_analysis PUBLIC src/analysis)
set_target_properties(shizuku_analysis PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Pitch correction (Signalsmith Stretch, MIT, header-only; pinned submodules 1.4.0 / linear 0.6.4)
add_library(shizuku_correction STATIC src/correction/PitchCorrection.cpp src/correction/NoteEdits.cpp)
target_include_directories(shizuku_correction
    PUBLIC src/correction
    PRIVATE external/signalsmith-stretch external/signalsmith-linear/include)
target_link_libraries(shizuku_correction PUBLIC shizuku_analysis)
if(APPLE)
    target_compile_definitions(shizuku_correction PRIVATE SIGNALSMITH_USE_ACCELERATE)
    target_link_libraries(shizuku_correction PUBLIC "-framework Accelerate")
endif()
set_target_properties(shizuku_correction PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    # Stretch is ~10x slower unoptimised; keep it fast in Debug builds as well.
    target_compile_options(shizuku_correction PRIVATE -O2)
endif()

add_library(shizuku_synthetic STATIC tools/SyntheticSignals.cpp)
target_link_libraries(shizuku_synthetic PUBLIC shizuku_analysis)

add_executable(shizuku-analyze tools/AnalyzeMain.cpp)
target_link_libraries(shizuku-analyze PRIVATE shizuku_analysis)

add_library(shizuku_correction_setup STATIC tools/CorrectionSetup.cpp)
target_link_libraries(shizuku_correction_setup PUBLIC shizuku_correction)

add_executable(shizuku-correct tools/CorrectMain.cpp)
target_link_libraries(shizuku-correct PRIVATE shizuku_correction_setup)

# Resident edit session for an interactive editor front end: keeps one take's analysis and render in memory and re-renders
# only around edited notes (line protocol on stdin/stdout, see tools/EditSessionMain.cpp).
add_executable(shizuku-edit-session tools/EditSessionMain.cpp)
target_link_libraries(shizuku-edit-session PRIVATE shizuku_correction_setup)

add_executable(shizuku-reference-error tools/ReferenceErrorMain.cpp)
target_link_libraries(shizuku-reference-error PRIVATE shizuku_analysis)

add_executable(shizuku-synth tools/SynthMain.cpp)
target_link_libraries(shizuku-synth PRIVATE shizuku_synthetic)

enable_testing()
add_executable(analysis-accuracy-test tests/AnalysisAccuracyTest.cpp)
target_link_libraries(analysis-accuracy-test PRIVATE shizuku_synthetic)
add_test(NAME analysis-accuracy COMMAND analysis-accuracy-test)
add_executable(correction-accuracy-test tests/CorrectionAccuracyTest.cpp)
target_link_libraries(correction-accuracy-test PRIVATE shizuku_synthetic shizuku_correction)
add_test(NAME correction-accuracy COMMAND correction-accuracy-test)
add_executable(reference-selection-test tests/ReferenceSelectionTest.cpp)
target_link_libraries(reference-selection-test PRIVATE shizuku_analysis)
add_test(NAME reference-selection COMMAND reference-selection-test)
add_executable(note-edits-test tests/NoteEditsTest.cpp)
target_link_libraries(note-edits-test PRIVATE shizuku_synthetic shizuku_correction)
add_test(NAME note-edits COMMAND note-edits-test)

# ---------------------------------------------------------------------------
# ARA plug-in: Audio Unit and VST3 on macOS, VST3 on Windows (VST3 SDK bundled with JUCE)
if(SHIZUKU_BUILD_PLUGIN)
    if(APPLE)
        set(shizuku_plugin_formats AU VST3)
    else()
        set(shizuku_plugin_formats VST3)
    endif()

    add_subdirectory(external/JUCE)
    juce_set_ara_sdk_path("${CMAKE_CURRENT_SOURCE_DIR}/external/ARA_SDK")

    juce_add_plugin(ShizukuTune
        PRODUCT_NAME "Shizuku Tune"
        COMPANY_NAME "Kojira"
        COMPANY_WEBSITE "https://github.com/kojira"
        BUNDLE_ID "dev.kojira.shizukutune"
        PLUGIN_MANUFACTURER_CODE Kjra
        PLUGIN_CODE Shzt
        FORMATS ${shizuku_plugin_formats}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        IS_ARA_EFFECT TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        AU_MAIN_TYPE kAudioUnitType_Effect
        VERSION ${PROJECT_VERSION})

    target_sources(ShizukuTune PRIVATE
        src/plugin/ShizukuDocumentController.cpp
        src/plugin/ShizukuPlaybackRenderer.cpp
        src/plugin/ShizukuPluginProcessor.cpp
        src/plugin/ShizukuPluginEditor.cpp
        src/plugin/PitchLineView.cpp
        src/plugin/AraDiagnostics.cpp
        src/plugin/ReferenceSet.cpp
        src/plugin/CaptureSession.cpp
        src/plugin/NoteEditSession.cpp
        src/plugin/NoteEditPanel.cpp
        src/plugin/AuditionPlayer.cpp)

    target_compile_definitions(ShizukuTune PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(ShizukuTune
        PRIVATE
            shizuku_correction
            juce::juce_audio_utils
            juce::juce_audio_formats
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # Offline host simulation of the capture mode: links the plug-in's shared code (with its JUCE modules) and uses
    # its include directories and definitions, so the JUCE modules are not compiled a second time.
    add_executable(capture-session-test tests/CaptureSessionTest.cpp
        external/JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_ARA.cpp) # the ARA library (as in the AU)
    target_include_directories(capture-session-test PRIVATE $<TARGET_PROPERTY:ShizukuTune,INCLUDE_DIRECTORIES>)
    target_compile_definitions(capture-session-test PRIVATE $<TARGET_PROPERTY:ShizukuTune,COMPILE_DEFINITIONS>)
    if(APPLE)
        set_source_files_properties(tests/CaptureSessionTest.cpp PROPERTIES COMPILE_OPTIONS "-xobjective-c++")
    endif()
    target_link_libraries(capture-session-test PRIVATE ShizukuTune shizuku_synthetic shizuku_correction_setup)
    add_test(NAME capture-session COMMAND capture-session-test)

    # macOS-only tools.
    if(APPLE)
        # Hosts the built AU through AudioToolbox with a side chain and checks capture -> corrected playback.
        add_executable(shizuku-au-capture-check tools/AuCaptureCheckMain.mm)
        target_link_libraries(shizuku-au-capture-check PRIVATE shizuku_analysis "-framework AudioToolbox" "-framework CoreFoundation")
        add_dependencies(shizuku-au-capture-check ShizukuTune_AU)

        # Renders the plug-in's pitch-line view for two WAV files to a PNG (UI check without a DAW).
        juce_add_console_app(shizuku-render-view PRODUCT_NAME "shizuku-render-view")
        target_sources(shizuku-render-view PRIVATE tools/RenderViewMain.cpp src/plugin/PitchLineView.cpp)
        target_include_directories(shizuku-render-view PRIVATE src/plugin)
        target_compile_definitions(shizuku-render-view PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(shizuku-render-view
            PRIVATE
                shizuku_correction
                juce::juce_gui_basics
            PUBLIC
                juce::juce_recommended_config_flags
                juce::juce_recommended_warning_flags)
    endif()
endif()
