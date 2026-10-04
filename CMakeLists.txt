cmake_minimum_required(VERSION 3.28)

project(YourotsCapture
    VERSION 0.1.0
    DESCRIPTION "Captura vertical da area Mobile do Yourots"
    LANGUAGES CXX
)

include(cmake/Dependencies.cmake)

if(NOT WIN32)
    message(FATAL_ERROR "YourotsCapture suporta apenas Windows x64.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure o projeto para x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(CaptureCore STATIC src/capture_core.cpp)
target_include_directories(CaptureCore PUBLIC src)
target_compile_options(CaptureCore PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

add_library(RecorderCore STATIC src/recorder_core.cpp)
target_include_directories(RecorderCore PUBLIC src)
target_compile_definitions(RecorderCore PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(RecorderCore PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)
target_link_libraries(RecorderCore PUBLIC CaptureCore)

add_library(Recorder STATIC src/recorder.cpp)
target_include_directories(Recorder PUBLIC src)
target_compile_definitions(Recorder PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(Recorder PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

add_library(ApplicationControls STATIC src/application_controls.cpp)
target_include_directories(ApplicationControls PUBLIC src)
target_compile_definitions(ApplicationControls PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ApplicationControls PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

add_library(UiRendering STATIC src/ui_rendering.cpp)
target_include_directories(UiRendering PUBLIC src)
target_compile_definitions(UiRendering PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(UiRendering PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)
target_link_libraries(UiRendering PUBLIC gdi32 user32)

add_executable(YourotsCapture WIN32
    src/main.cpp
    src/capture_preview.cpp
)

add_library(Calibration STATIC src/calibration.cpp)
target_include_directories(Calibration PUBLIC src)
target_compile_definitions(Calibration PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(Calibration PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)
target_link_libraries(Calibration PUBLIC CaptureCore)

target_link_libraries(Recorder PUBLIC RecorderCore windowsapp)

add_executable(YourotsCapturePoc
    src/phase1_poc.cpp
)

add_executable(YourotsCapturePhase3
    src/phase3_record.cpp
    src/capture_preview.cpp
)

target_compile_definitions(YourotsCapture PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_compile_definitions(YourotsCapturePoc PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_compile_definitions(YourotsCapturePhase3 PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_compile_options(YourotsCapture PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(YourotsCapturePoc PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(YourotsCapturePhase3 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_link_libraries(YourotsCapture PRIVATE
    ApplicationControls
    UiRendering
    Recorder
    Calibration
    gdi32
    d3d11
    dxgi
    shell32
    ole32
    windowsapp
)

target_link_libraries(YourotsCapturePoc PRIVATE
    CaptureCore
    d3d11
    dxgi
    shell32
    windowsapp
    windowscodecs
)

target_link_libraries(YourotsCapturePhase3 PRIVATE
    Recorder
    Calibration
    d3d11
    dxgi
    windowsapp
)

set_property(TARGET YourotsCapture PROPERTY
    VS_GLOBAL_WindowsTargetPlatformVersion "10.0.26100.0"
)

set_property(TARGET YourotsCapturePoc PROPERTY
    VS_GLOBAL_WindowsTargetPlatformVersion "10.0.26100.0"
)

set_property(TARGET YourotsCapturePhase3 PROPERTY
    VS_GLOBAL_WindowsTargetPlatformVersion "10.0.26100.0"
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT YourotsCapture)

include(CTest)
if(BUILD_TESTING)
    find_program(POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

    add_executable(CaptureCoreTests tests/capture_core_tests.cpp)
    target_compile_definitions(CaptureCoreTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(CaptureCoreTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(CaptureCoreTests PRIVATE CaptureCore shell32)
    add_test(NAME unit.capture_core
        COMMAND CaptureCoreTests "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/capture-core.xml"
    )
    set_tests_properties(unit.capture_core PROPERTIES LABELS unit TIMEOUT 15)

    add_executable(CalibrationTests tests/calibration_tests.cpp)
    target_compile_options(CalibrationTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(CalibrationTests PRIVATE Calibration)
    add_test(NAME unit.calibration
        COMMAND CalibrationTests "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/calibration.xml"
    )
    set_tests_properties(unit.calibration PROPERTIES LABELS unit TIMEOUT 20)

    add_executable(RecorderTests tests/recorder_tests.cpp)
    target_compile_options(RecorderTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(RecorderTests PRIVATE RecorderCore shell32)
    add_test(NAME unit.recorder
        COMMAND RecorderTests "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/recorder.xml"
    )
    set_tests_properties(unit.recorder PROPERTIES LABELS unit TIMEOUT 15)

    add_executable(ApplicationControlsTests tests/application_controls_tests.cpp)
    target_compile_options(ApplicationControlsTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(ApplicationControlsTests PRIVATE ApplicationControls RecorderCore)
    add_test(NAME unit.application_controls
        COMMAND ApplicationControlsTests "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/application-controls.xml"
    )
    set_tests_properties(unit.application_controls PROPERTIES LABELS unit TIMEOUT 20)

    add_executable(UiRenderingTests tests/ui_rendering_tests.cpp)
    target_compile_options(UiRenderingTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(UiRenderingTests PRIVATE UiRendering)
    add_test(NAME unit.ui_rendering
        COMMAND UiRenderingTests "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/ui-rendering.xml"
    )
    set_tests_properties(unit.ui_rendering PROPERTIES LABELS unit TIMEOUT 20)

    add_executable(RecorderLifecycleE2E tests/recorder_lifecycle_e2e.cpp src/capture_preview.cpp)
    target_compile_options(RecorderLifecycleE2E PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(RecorderLifecycleE2E PRIVATE Recorder d3d11 dxgi)

    add_executable(MediaToolFixture tests/media_tool_fixture.cpp)
    target_compile_definitions(MediaToolFixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(MediaToolFixture PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(MediaToolFixture PRIVATE CaptureCore)

    add_executable(CaptureFixture WIN32 tests/capture_fixture.cpp)
    target_compile_definitions(CaptureFixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(CaptureFixture PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    )
    target_link_libraries(CaptureFixture PRIVATE gdi32)

    add_test(NAME unit.ffmpeg_setup
        COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-unit.ps1"
            -ReportPath "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/unit.xml"
    )
    set_tests_properties(unit.ffmpeg_setup PROPERTIES LABELS unit TIMEOUT 30)

    add_test(NAME unit.phase5_validation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/phase5_validation_tests.py"
            "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/phase5-validation.xml"
    )
    set_tests_properties(unit.phase5_validation PROPERTIES LABELS unit TIMEOUT 15)

    add_test(NAME e2e.phase0
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_phase0.py"
            --app "$<TARGET_FILE:YourotsCapture>"
            --ffmpeg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffmpeg.exe"
            --ffprobe "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffprobe.exe"
            --powershell "${POWERSHELL_EXECUTABLE}"
            --encoder-check "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-ffmpeg.ps1"
            --expected-version "${YOUROTS_FFMPEG_VERSION}"
            --artifacts "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/e2e"
    )
    set_tests_properties(e2e.phase0 PROPERTIES LABELS e2e TIMEOUT 90 RUN_SERIAL TRUE)

    add_test(NAME e2e.phase1
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_phase1.py"
            --app "$<TARGET_FILE:YourotsCapturePoc>"
            --fixture "$<TARGET_FILE:CaptureFixture>"
            --ffmpeg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffmpeg.exe"
            --ffprobe "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffprobe.exe"
            --artifacts "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/e2e-phase1"
    )
    set_tests_properties(e2e.phase1 PROPERTIES LABELS e2e TIMEOUT 180 RUN_SERIAL TRUE)

    add_test(NAME e2e.phase2
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_phase2.py"
            --app "$<TARGET_FILE:YourotsCapture>"
            --fixture "$<TARGET_FILE:CaptureFixture>"
            --artifacts "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/e2e-phase2"
    )
    set_tests_properties(e2e.phase2 PROPERTIES LABELS e2e TIMEOUT 150 RUN_SERIAL TRUE)

    add_test(NAME e2e.phase3
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_phase3.py"
            --app "$<TARGET_FILE:YourotsCapturePhase3>"
            --lifecycle "$<TARGET_FILE:RecorderLifecycleE2E>"
            --tool-fixture "$<TARGET_FILE:MediaToolFixture>"
            --fixture "$<TARGET_FILE:CaptureFixture>"
            --ffmpeg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffmpeg.exe"
            --ffprobe "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffprobe.exe"
            --artifacts "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/e2e-phase3"
    )
    set_tests_properties(e2e.phase3 PROPERTIES LABELS e2e TIMEOUT 180 RUN_SERIAL TRUE)

    add_test(NAME e2e.phase4
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_phase4.py"
            --app "$<TARGET_FILE:YourotsCapture>"
            --tool-fixture "$<TARGET_FILE:MediaToolFixture>"
            --fixture "$<TARGET_FILE:CaptureFixture>"
            --ffmpeg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffmpeg.exe"
            --ffprobe "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffprobe.exe"
            --artifacts "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/e2e-phase4"
    )
    set_tests_properties(e2e.phase4 PROPERTIES LABELS e2e TIMEOUT 240 RUN_SERIAL TRUE)

    add_test(NAME e2e.phase5
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_phase5.py"
            --app "$<TARGET_FILE:YourotsCapture>"
            --fixture "$<TARGET_FILE:CaptureFixture>"
            --ffmpeg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffmpeg.exe"
            --ffprobe "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/ffprobe.exe"
            --artifacts "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/e2e-phase5"
    )
    set_tests_properties(e2e.phase5 PROPERTIES LABELS e2e TIMEOUT 240 RUN_SERIAL TRUE)
endif()
