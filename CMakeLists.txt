cmake_minimum_required(VERSION 3.20)

project(pwmixd
    VERSION 0.1.0
    DESCRIPTION "PipeWire-based desktop audio mixer daemon"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(pwmixd
    src/main.cpp
    src/api/osc.cpp
    src/api/server.cpp
    src/app/supervisor.cpp
    src/control/midi.cpp
    src/engine/dsp.cpp
    src/engine/fx_host.cpp
    src/engine/mixer_node.cpp
    src/meter/meter_bus.cpp
    src/pw/core.cpp
    src/pw/link_manager.cpp
    src/pw/rule_engine.cpp
    src/pw/virtual_devices.cpp
    src/state/scenes.cpp
    src/state/store.cpp
)

target_include_directories(pwmixd PRIVATE include)
target_compile_features(pwmixd PRIVATE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pwmixd PRIVATE -Wall -Wextra -Wpedantic)
endif()
