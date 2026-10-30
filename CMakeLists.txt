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
