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

add_executable(YourotsCapture WIN32
    src/main.cpp
)

add_executable(YourotsCapturePoc
    src/phase1_poc.cpp
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

target_compile_options(YourotsCapture PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(YourotsCapturePoc PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_link_libraries(YourotsCapture PRIVATE
    d3d11
    dxgi
    windowsapp
)

target_link_libraries(YourotsCapturePoc PRIVATE
    d3d11
    dxgi
    shell32
    windowsapp
    windowscodecs
)

set_property(TARGET YourotsCapture PROPERTY
    VS_GLOBAL_WindowsTargetPlatformVersion "10.0.26100.0"
)

set_property(TARGET YourotsCapturePoc PROPERTY
    VS_GLOBAL_WindowsTargetPlatformVersion "10.0.26100.0"
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT YourotsCapture)

include(CTest)
if(BUILD_TESTING)
    find_program(POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

    add_test(NAME unit.ffmpeg_setup
        COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-unit.ps1"
            -ReportPath "${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>/unit.xml"
    )
    set_tests_properties(unit.ffmpeg_setup PROPERTIES LABELS unit TIMEOUT 30)

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
endif()
