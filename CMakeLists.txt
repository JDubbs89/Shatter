cmake_minimum_required(VERSION 3.20)

project(shatter
    VERSION 0.1.0
    DESCRIPTION "Local desktop audio mixer daemon"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

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

add_executable(shatter
    src/main.cpp
    src/api/osc.cpp
    src/api/server.cpp
    src/app/supervisor.cpp
    src/control/midi.cpp
    src/engine/dsp.cpp
    src/engine/fx_host.cpp
    src/engine/mixer_node.cpp
    src/engine/pipewire_engine.cpp
    src/meter/meter_bus.cpp
    src/pw/core.cpp
    src/pw/link_manager.cpp
    src/pw/rule_engine.cpp
    src/pw/virtual_devices.cpp
    src/state/scenes.cpp
    src/state/store.cpp
)

target_include_directories(shatter PRIVATE include)
target_compile_features(shatter PRIVATE cxx_std_20)
target_link_libraries(shatter PRIVATE PkgConfig::PIPEWIRE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shatter PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS shatter RUNTIME DESTINATION bin)

enable_testing()
add_test(
    NAME cli_lifecycle
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/cli-lifecycle.sh" "$<TARGET_FILE:shatter>"
)
add_executable(audio_ring_test tests/unit/audio-ring.cpp)
target_include_directories(audio_ring_test PRIVATE include)
target_compile_features(audio_ring_test PRIVATE cxx_std_20)
add_test(NAME audio_ring COMMAND audio_ring_test)

add_executable(dsp_test src/engine/dsp.cpp tests/unit/dsp.cpp)
target_include_directories(dsp_test PRIVATE include)
target_compile_features(dsp_test PRIVATE cxx_std_20)
target_link_libraries(dsp_test PRIVATE Catch2::Catch2WithMain)
add_test(NAME dsp COMMAND dsp_test)

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
