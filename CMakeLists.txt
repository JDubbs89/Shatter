cmake_minimum_required(VERSION 3.20)

project(shatter
    VERSION 0.1.0
    DESCRIPTION "Local desktop audio mixer daemon"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SHATTER_PLATFORM "AUTO" CACHE STRING "Native platform adapter to build (AUTO, LINUX, or WINDOWS)")
set_property(CACHE SHATTER_PLATFORM PROPERTY STRINGS AUTO LINUX WINDOWS)
set(SHATTER_AUDIO_BACKEND "AUTO" CACHE STRING "Audio backend to build (AUTO, PIPEWIRE, PULSEAUDIO, or NONE)")
set_property(CACHE SHATTER_AUDIO_BACKEND PROPERTY STRINGS AUTO PIPEWIRE PULSEAUDIO NONE)

string(TOUPPER "${SHATTER_PLATFORM}" SHATTER_PLATFORM)
if(SHATTER_PLATFORM STREQUAL "AUTO")
    if(WIN32)
        set(SHATTER_PLATFORM "WINDOWS")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(SHATTER_PLATFORM "LINUX")
    else()
        message(FATAL_ERROR "Unsupported host platform '${CMAKE_SYSTEM_NAME}'. Select a supported SHATTER_PLATFORM.")
    endif()
endif()
if(SHATTER_PLATFORM STREQUAL "WINDOWS" AND NOT WIN32)
    message(FATAL_ERROR "SHATTER_PLATFORM=WINDOWS requires a Windows toolchain.")
elseif(SHATTER_PLATFORM STREQUAL "LINUX" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "SHATTER_PLATFORM=LINUX requires a Linux toolchain.")
elseif(NOT SHATTER_PLATFORM STREQUAL "WINDOWS" AND NOT SHATTER_PLATFORM STREQUAL "LINUX")
    message(FATAL_ERROR "Unsupported SHATTER_PLATFORM='${SHATTER_PLATFORM}'. Choose AUTO, LINUX, or WINDOWS.")
endif()

string(TOUPPER "${SHATTER_AUDIO_BACKEND}" SHATTER_AUDIO_BACKEND)
if(SHATTER_AUDIO_BACKEND STREQUAL "AUTO")
    if(SHATTER_PLATFORM STREQUAL "LINUX")
        set(SHATTER_AUDIO_BACKEND "PIPEWIRE")
    else()
        set(SHATTER_AUDIO_BACKEND "NONE")
    endif()
endif()
if(SHATTER_AUDIO_BACKEND STREQUAL "PIPEWIRE")
    if(NOT SHATTER_PLATFORM STREQUAL "LINUX")
        message(FATAL_ERROR "The PipeWire audio backend is supported only with SHATTER_PLATFORM=LINUX.")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
elseif(SHATTER_AUDIO_BACKEND STREQUAL "PULSEAUDIO")
    if(NOT SHATTER_PLATFORM STREQUAL "LINUX")
        message(FATAL_ERROR "The PulseAudio audio backend is supported only with SHATTER_PLATFORM=LINUX.")
    endif()
    find_program(PACTL_EXECUTABLE pactl REQUIRED)
elseif(NOT SHATTER_AUDIO_BACKEND STREQUAL "NONE")
    message(FATAL_ERROR "Unsupported SHATTER_AUDIO_BACKEND='${SHATTER_AUDIO_BACKEND}'. Choose AUTO, PIPEWIRE, PULSEAUDIO, or NONE.")
endif()
message(STATUS "Shatter platform: ${SHATTER_PLATFORM}; audio backend: ${SHATTER_AUDIO_BACKEND}")

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.7.1
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(Catch2)
    endif()
endif()

set(SHATTER_SOURCES
    src/main.cpp
    src/api/osc.cpp
    src/api/server.cpp
    src/app/supervisor.cpp
    src/control/midi.cpp
    src/engine/dsp.cpp
    src/engine/fx_host.cpp
    src/meter/meter_bus.cpp
    src/state/store.cpp
    src/state/scenes.cpp
)

if(SHATTER_AUDIO_BACKEND STREQUAL "PIPEWIRE")
    list(APPEND SHATTER_SOURCES
        src/engine/mixer_node.cpp
        src/engine/pipewire_engine.cpp
        src/engine/audio_backend.cpp
        src/pw/core.cpp
        src/pw/link_manager.cpp
        src/pw/rule_engine.cpp
        src/pw/virtual_devices.cpp
    )
elseif(SHATTER_AUDIO_BACKEND STREQUAL "PULSEAUDIO")
    list(APPEND SHATTER_SOURCES src/engine/pulseaudio_backend.cpp)
else()
    list(APPEND SHATTER_SOURCES src/engine/unavailable_audio_backend.cpp)
endif()

add_executable(shatter ${SHATTER_SOURCES})

if(SHATTER_PLATFORM STREQUAL "LINUX")
    add_library(shatter_platform STATIC src/platform/linux.cpp)
else()
    add_library(shatter_platform STATIC src/platform/windows.cpp)
endif()
target_include_directories(shatter_platform PUBLIC include)
target_compile_features(shatter_platform PUBLIC cxx_std_20)
if(SHATTER_PLATFORM STREQUAL "WINDOWS")
    target_link_libraries(shatter_platform PUBLIC advapi32)
endif()

target_include_directories(shatter PRIVATE include)
target_include_directories(shatter PRIVATE src/engine)
target_compile_features(shatter PRIVATE cxx_std_20)
target_link_libraries(shatter PRIVATE shatter_platform)
if(SHATTER_AUDIO_BACKEND STREQUAL "PIPEWIRE")
    target_link_libraries(shatter PRIVATE PkgConfig::PIPEWIRE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shatter PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS shatter RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_executable(platform_control_test tests/unit/platform-control.cpp)
    target_link_libraries(platform_control_test PRIVATE shatter_platform)
    add_test(NAME platform_control COMMAND platform_control_test)
    set_tests_properties(platform_control PROPERTIES TIMEOUT 5 RUN_SERIAL TRUE)
    if(UNIX)
        if(SHATTER_AUDIO_BACKEND STREQUAL "PIPEWIRE")
            add_test(
                NAME cli_lifecycle
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/cli-lifecycle.sh" "$<TARGET_FILE:shatter>"
            )
        elseif(SHATTER_AUDIO_BACKEND STREQUAL "NONE")
            add_test(
                NAME cli_platform_lifecycle
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/cli-no-audio.sh" "$<TARGET_FILE:shatter>"
            )
        endif()
    endif()
    if(SHATTER_AUDIO_BACKEND STREQUAL "PULSEAUDIO")
        find_program(PAPLAY_TEST_EXECUTABLE paplay)
        find_program(PYTHON3_TEST_EXECUTABLE python3)
        if(PACTL_EXECUTABLE AND PAPLAY_TEST_EXECUTABLE AND PYTHON3_TEST_EXECUTABLE)
            add_test(
                NAME pulseaudio_playback_routing
                COMMAND "${PYTHON3_TEST_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/pulseaudio-playback-routing.py"
                    "$<TARGET_FILE:shatter>"
            )
            set_tests_properties(pulseaudio_playback_routing PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        endif()
    endif()
    if(WIN32)
        find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(POWERSHELL_EXECUTABLE)
            add_test(
                NAME windows_cli_lifecycle
                COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/windows-cli-lifecycle.ps1"
                    "$<TARGET_FILE:shatter>"
            )
            set_tests_properties(windows_cli_lifecycle PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
        endif()
    endif()
    add_executable(audio_ring_test tests/unit/audio-ring.cpp)
    target_include_directories(audio_ring_test PRIVATE include)
    target_compile_features(audio_ring_test PRIVATE cxx_std_20)
    add_test(NAME audio_ring COMMAND audio_ring_test)

    add_executable(dsp_test src/engine/dsp.cpp tests/unit/dsp.cpp)
    target_include_directories(dsp_test PRIVATE include)
    target_compile_features(dsp_test PRIVATE cxx_std_20)
    target_link_libraries(dsp_test PRIVATE Catch2::Catch2WithMain)
    add_test(NAME dsp COMMAND dsp_test)

    add_executable(meter_test src/meter/meter_bus.cpp tests/unit/meter.cpp)
    target_include_directories(meter_test PRIVATE include)
    target_compile_features(meter_test PRIVATE cxx_std_20)
    target_link_libraries(meter_test PRIVATE Catch2::Catch2WithMain)
    add_test(NAME meter COMMAND meter_test)

    add_executable(engine_health_test tests/unit/engine-health.cpp)
    target_include_directories(engine_health_test PRIVATE include)
    target_compile_features(engine_health_test PRIVATE cxx_std_20)
    target_link_libraries(engine_health_test PRIVATE Catch2::Catch2WithMain)
    add_test(NAME engine_health COMMAND engine_health_test)

    add_executable(state_store_test src/state/store.cpp tests/unit/state-store.cpp)
    target_include_directories(state_store_test PRIVATE include)
    target_compile_features(state_store_test PRIVATE cxx_std_20)
    target_link_libraries(state_store_test PRIVATE Catch2::Catch2WithMain)
    add_test(NAME state_store COMMAND state_store_test)

    if(SHATTER_AUDIO_BACKEND STREQUAL "PIPEWIRE")
        find_program(PIPEWIRE_TEST_EXECUTABLE pipewire)
        find_program(WIREPLUMBER_TEST_EXECUTABLE wireplumber)
        find_program(PW_CLI_TEST_EXECUTABLE pw-cli)
        find_program(WPCTL_TEST_EXECUTABLE wpctl)
        find_program(PW_PLAY_TEST_EXECUTABLE pw-play)
        find_program(PW_RECORD_TEST_EXECUTABLE pw-record)
        find_program(PW_LINK_TEST_EXECUTABLE pw-link)
        find_program(PYTHON3_TEST_EXECUTABLE python3)
        if(PIPEWIRE_TEST_EXECUTABLE AND WIREPLUMBER_TEST_EXECUTABLE AND PW_CLI_TEST_EXECUTABLE
           AND WPCTL_TEST_EXECUTABLE AND PW_PLAY_TEST_EXECUTABLE AND PW_RECORD_TEST_EXECUTABLE
           AND PW_LINK_TEST_EXECUTABLE AND PYTHON3_TEST_EXECUTABLE)
            add_executable(pipewire_test_source tests/integration/pipewire-test-source.cpp)
            target_link_libraries(pipewire_test_source PRIVATE PkgConfig::PIPEWIRE)
            target_compile_features(pipewire_test_source PRIVATE cxx_std_20)
            add_test(
                NAME app_playback_routing
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/app-playback-routing.sh" "$<TARGET_FILE:shatter>"
            )
            add_test(
                NAME microphone_capture_routing
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/capture-routing.sh"
                    "$<TARGET_FILE:shatter>" "$<TARGET_FILE:pipewire_test_source>"
            )
            set_tests_properties(app_playback_routing microphone_capture_routing PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        endif()
    endif()
endif()
