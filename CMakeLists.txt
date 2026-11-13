cmake_minimum_required(VERSION 3.20)

set(SHATTER_VERSION "0.1.0" CACHE STRING "Shatter package version (major.minor.patch)")
project(shatter
    VERSION "${SHATTER_VERSION}"
    DESCRIPTION "Local desktop audio mixer daemon"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SHATTER_PLATFORM "AUTO" CACHE STRING "Native platform adapter to build (AUTO, LINUX, or WINDOWS)")
set_property(CACHE SHATTER_PLATFORM PROPERTY STRINGS AUTO LINUX WINDOWS)
set(SHATTER_AUDIO_BACKEND "AUTO" CACHE STRING "Audio backend to build (AUTO, PIPEWIRE, PULSEAUDIO, WASAPI, or NONE)")
set_property(CACHE SHATTER_AUDIO_BACKEND PROPERTY STRINGS AUTO PIPEWIRE PULSEAUDIO WASAPI NONE)

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

# FindPkgConfig caches absolute SDK paths. Refresh discovery when a temporary
# SDK has disappeared rather than propagating its stale paths to imported targets.
foreach(_dependency PIPEWIRE PULSEAUDIO)
    foreach(_path IN LISTS ${_dependency}_INCLUDE_DIRS ${_dependency}_LINK_LIBRARIES)
        if(IS_ABSOLUTE "${_path}" AND NOT EXISTS "${_path}")
            get_cmake_property(_cache_variables CACHE_VARIABLES)
            foreach(_variable IN LISTS _cache_variables)
                if(_variable MATCHES "^${_dependency}_|^pkgcfg_lib_${_dependency}_|^__pkg_config_.*_${_dependency}$")
                    unset(${_variable} CACHE)
                endif()
            endforeach()
            message(STATUS "Refreshing ${_dependency} discovery: cached path no longer exists: ${_path}")
            break()
        endif()
    endforeach()
endforeach()

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
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSEAUDIO REQUIRED IMPORTED_TARGET libpulse)
elseif(SHATTER_AUDIO_BACKEND STREQUAL "WASAPI")
    if(NOT SHATTER_PLATFORM STREQUAL "WINDOWS")
        message(FATAL_ERROR "The WASAPI audio backend is supported only with SHATTER_PLATFORM=WINDOWS.")
    endif()
elseif(NOT SHATTER_AUDIO_BACKEND STREQUAL "NONE")
    message(FATAL_ERROR "Unsupported SHATTER_AUDIO_BACKEND='${SHATTER_AUDIO_BACKEND}'. Choose AUTO, PIPEWIRE, PULSEAUDIO, WASAPI, or NONE.")
endif()
message(STATUS "Shatter platform: ${SHATTER_PLATFORM}; audio backend: ${SHATTER_AUDIO_BACKEND}")

include(CTest)
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif()

# Each layer is a separate target. Only shatter_backend receives native audio
# SDK include paths; shatter_mixer builds without any audio SDK or control code.
add_library(shatter_mixer STATIC
    src/mixer/dsp.cpp
    src/mixer/processor.cpp
    src/mixer/channel_controls.cpp
    src/mixer/presentation.cpp
    src/mixer/fx_host.cpp
    src/mixer/routing_rules.cpp
    src/mixer/meter/meter_bus.cpp
    src/mixer/state/store.cpp
    src/mixer/state/scenes.cpp
)
target_include_directories(shatter_mixer PUBLIC src)

if(SHATTER_PLATFORM STREQUAL "LINUX")
    add_library(shatter_control_platform STATIC src/control/platform/linux/service.cpp)
else()
    add_library(shatter_control_platform STATIC src/control/platform/windows/service.cpp)
    target_link_libraries(shatter_control_platform PRIVATE advapi32)
endif()
target_include_directories(shatter_control_platform PUBLIC src)

if(SHATTER_AUDIO_BACKEND STREQUAL "PIPEWIRE")
    add_library(shatter_backend STATIC
        src/backend/pipewire/factory.cpp
        src/backend/pipewire/pipewire_backend.cpp
        src/backend/pipewire/audio_callbacks.cpp
        src/backend/pipewire/registry.cpp
        src/backend/pipewire/endpoints.cpp
        src/backend/pipewire/routing.cpp
        src/backend/pipewire/virtual_devices.cpp
    )
    target_link_libraries(shatter_backend PRIVATE PkgConfig::PIPEWIRE)
elseif(SHATTER_AUDIO_BACKEND STREQUAL "PULSEAUDIO")
    add_library(shatter_backend STATIC
        src/backend/pulseaudio/pulseaudio_backend.cpp
        src/backend/pulseaudio/pulseaudio_client.cpp
    )
    target_link_libraries(shatter_backend PRIVATE PkgConfig::PULSEAUDIO)
elseif(SHATTER_AUDIO_BACKEND STREQUAL "WASAPI")
    add_library(shatter_backend STATIC src/backend/wasapi/wasapi_backend.cpp)
    target_link_libraries(shatter_backend PRIVATE ole32 propsys)
else()
    add_library(shatter_backend STATIC src/backend/unavailable_audio_backend.cpp)
endif()
target_link_libraries(shatter_backend PUBLIC shatter_mixer)

add_library(shatter_control STATIC
    src/control/cli.cpp
    src/control/service.cpp
    src/control/surfaces/osc.cpp
    src/control/surfaces/midi.cpp
)
target_link_libraries(shatter_control PUBLIC shatter_backend shatter_control_platform)

add_executable(shatter src/main.cpp)
target_link_libraries(shatter PRIVATE shatter_control)

foreach(_target shatter shatter_control shatter_control_platform shatter_mixer shatter_backend)
    target_compile_features(${_target} PUBLIC cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

install(TARGETS shatter RUNTIME DESTINATION bin)
if(SHATTER_PLATFORM STREQUAL "LINUX")
    install(PROGRAMS packaging/systemd/configure-startup.sh DESTINATION share/shatter)
endif()

if(WIN32)
    include(InstallRequiredSystemLibraries)
endif()

option(SHATTER_BUILD_MSI "Enable the Windows MSI package target (requires CMake 3.30+ and WiX 4)" ${WIN32})
if(SHATTER_BUILD_MSI)
    if(NOT WIN32)
        message(FATAL_ERROR "SHATTER_BUILD_MSI requires a Windows toolchain.")
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.30)
        message(FATAL_ERROR "MSI packaging requires CMake 3.30 or newer. Use -DSHATTER_BUILD_MSI=OFF to build without packaging.")
    endif()
    set(CPACK_GENERATOR WIX)
    set(CPACK_PACKAGE_NAME Shatter)
    set(CPACK_PACKAGE_VENDOR "Shatter")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY Shatter)
    string(TOLOWER "${SHATTER_AUDIO_BACKEND}" _package_backend)
    set(CPACK_PACKAGE_FILE_NAME "shatter-${PROJECT_VERSION}-windows-${CMAKE_SYSTEM_PROCESSOR}-${_package_backend}")
    set(CPACK_WIX_VERSION 4)
    set(CPACK_WIX_INSTALL_SCOPE perMachine)
    set(CPACK_WIX_UPGRADE_GUID "A46E1892-362F-4974-A321-3C958B4CFC75")
    set(CPACK_WIX_SKIP_PROGRAM_FOLDER TRUE)
    set(CPACK_WIX_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/installer.wxs.in")
    set(CPACK_WIX_EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/path.wxs")
    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/feature.xml")
    include(CPack)
    add_custom_target(msi
        COMMAND "${CMAKE_CPACK_COMMAND}" --config "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake" -C "$<CONFIG>"
        DEPENDS shatter
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Building the Shatter Windows MSI installer"
        VERBATIM
    )
endif()

if(BUILD_TESTING)
    add_executable(backend_contract_test src/backend/contract_test.cpp)
    target_link_libraries(backend_contract_test PRIVATE shatter_backend GTest::gtest_main)
    add_test(NAME backend_contract COMMAND backend_contract_test)

    add_executable(platform_control_test tests/unit/platform-control.cpp)
    target_link_libraries(platform_control_test PRIVATE shatter_control_platform GTest::gtest_main)
    add_test(NAME platform_control COMMAND platform_control_test)
    set_tests_properties(platform_control PROPERTIES TIMEOUT 5 RUN_SERIAL TRUE)
    add_executable(wasapi_selection_test tests/unit/wasapi-selection.cpp)
    target_include_directories(wasapi_selection_test PRIVATE src)
    target_compile_features(wasapi_selection_test PRIVATE cxx_std_20)
    target_link_libraries(wasapi_selection_test PRIVATE GTest::gtest_main)
    add_test(NAME wasapi_selection COMMAND wasapi_selection_test)
    if(UNIX)
        add_test(NAME install_startup COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/install-startup.sh")
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
        add_executable(pulseaudio_selection_test tests/unit/pulseaudio-selection.cpp)
        target_include_directories(pulseaudio_selection_test PRIVATE src)
        target_compile_features(pulseaudio_selection_test PRIVATE cxx_std_20)
        target_link_libraries(pulseaudio_selection_test PRIVATE GTest::gtest_main)
        add_test(NAME pulseaudio_selection COMMAND pulseaudio_selection_test)

        find_program(PACTL_TEST_EXECUTABLE pactl)
        find_program(PAPLAY_TEST_EXECUTABLE paplay)
        find_program(PYTHON3_TEST_EXECUTABLE python3)
        if(PACTL_TEST_EXECUTABLE AND PAPLAY_TEST_EXECUTABLE AND PYTHON3_TEST_EXECUTABLE)
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
                    "$<TARGET_FILE:shatter>" "${SHATTER_AUDIO_BACKEND}"
            )
            set_tests_properties(windows_cli_lifecycle PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
        endif()
    endif()
    add_executable(audio_engine_test src/mixer/audio_engine_test.cpp)
    target_link_libraries(audio_engine_test PRIVATE shatter_mixer GTest::gtest_main)
    add_test(NAME audio_engine COMMAND audio_engine_test)
    add_executable(mixer_engine_test src/mixer/engine_test.cpp)
    target_link_libraries(mixer_engine_test PRIVATE shatter_mixer GTest::gtest_main)
    add_test(NAME mixer_engine COMMAND mixer_engine_test)

    add_executable(mixer_processor_test src/mixer/processor_test.cpp)
    target_link_libraries(mixer_processor_test PRIVATE shatter_mixer GTest::gtest_main)
    add_test(NAME mixer_processor COMMAND mixer_processor_test)

    add_executable(audio_ring_test tests/unit/audio-ring.cpp)
    target_include_directories(audio_ring_test PRIVATE src)
    target_compile_features(audio_ring_test PRIVATE cxx_std_20)
    target_link_libraries(audio_ring_test PRIVATE GTest::gtest_main)
    add_test(NAME audio_ring COMMAND audio_ring_test)

    add_executable(dsp_test tests/unit/dsp.cpp)
    target_include_directories(dsp_test PRIVATE src)
    target_compile_features(dsp_test PRIVATE cxx_std_20)
    target_link_libraries(dsp_test PRIVATE shatter_mixer GTest::gtest_main)
    add_test(NAME dsp COMMAND dsp_test)

    add_executable(meter_test tests/unit/meter.cpp)
    target_include_directories(meter_test PRIVATE src)
    target_compile_features(meter_test PRIVATE cxx_std_20)
    target_link_libraries(meter_test PRIVATE shatter_mixer GTest::gtest_main)
    add_test(NAME meter COMMAND meter_test)

    add_executable(engine_health_test tests/unit/engine-health.cpp)
    target_include_directories(engine_health_test PRIVATE src)
    target_compile_features(engine_health_test PRIVATE cxx_std_20)
    target_link_libraries(engine_health_test PRIVATE GTest::gtest_main)
    add_test(NAME engine_health COMMAND engine_health_test)

    add_executable(state_store_test tests/unit/state-store.cpp)
    target_include_directories(state_store_test PRIVATE src)
    target_compile_features(state_store_test PRIVATE cxx_std_20)
    target_link_libraries(state_store_test PRIVATE shatter_mixer GTest::gtest_main)
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
        set(_pipewire_test_tools
            PIPEWIRE_TEST_EXECUTABLE
            WIREPLUMBER_TEST_EXECUTABLE
            PW_CLI_TEST_EXECUTABLE
            WPCTL_TEST_EXECUTABLE
            PW_PLAY_TEST_EXECUTABLE
            PW_RECORD_TEST_EXECUTABLE
            PW_LINK_TEST_EXECUTABLE
            PYTHON3_TEST_EXECUTABLE
        )
        set(_missing_pipewire_test_tools)
        foreach(_tool_var IN LISTS _pipewire_test_tools)
            if(NOT ${_tool_var})
                string(REPLACE "_TEST_EXECUTABLE" "" _tool_name "${_tool_var}")
                string(TOLOWER "${_tool_name}" _tool_name)
                string(REPLACE "_" "-" _tool_name "${_tool_name}")
                list(APPEND _missing_pipewire_test_tools "${_tool_name}")
            endif()
        endforeach()
        if(_missing_pipewire_test_tools)
            list(JOIN _missing_pipewire_test_tools ", " _missing_tools)
            message(FATAL_ERROR
                "PipeWire integration tests are required when BUILD_TESTING is enabled; missing tools: ${_missing_tools}")
        endif()

        execute_process(
            COMMAND "${WIREPLUMBER_TEST_EXECUTABLE}" --version
            RESULT_VARIABLE _wireplumber_version_result
            OUTPUT_VARIABLE _wireplumber_version_output
            ERROR_VARIABLE _wireplumber_version_error
            TIMEOUT 10
        )
        if(NOT _wireplumber_version_result STREQUAL "0" OR
           NOT _wireplumber_version_output MATCHES "Compiled with libwireplumber ([0-9]+\\.[0-9]+\\.[0-9]+)")
            message(FATAL_ERROR
                "PipeWire integration tests require WirePlumber 0.5.0 or newer; could not determine its version from '${WIREPLUMBER_TEST_EXECUTABLE} --version': ${_wireplumber_version_output}${_wireplumber_version_error}")
        endif()
        set(_wireplumber_version "${CMAKE_MATCH_1}")
        if(_wireplumber_version VERSION_LESS "0.5.0")
            message(FATAL_ERROR
                "PipeWire integration tests require WirePlumber 0.5.0 or newer for --profile policy; found ${_wireplumber_version}. Upgrade WirePlumber or configure with -DBUILD_TESTING=OFF.")
        endif()
        message(STATUS "PipeWire integration tests: WirePlumber ${_wireplumber_version}")

        add_executable(pipewire_test_source tests/integration/pipewire-test-source.cpp)
        target_link_libraries(pipewire_test_source PRIVATE PkgConfig::PIPEWIRE)
        target_compile_features(pipewire_test_source PRIVATE cxx_std_20)
        add_executable(pipewire_test_input tests/integration/pipewire-test-input.cpp)
        target_link_libraries(pipewire_test_input PRIVATE PkgConfig::PIPEWIRE)
        target_compile_features(pipewire_test_input PRIVATE cxx_std_20)
        add_test(
            NAME app_playback_routing
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/app-playback-routing.sh"
                "$<TARGET_FILE:shatter>" "$<TARGET_FILE:pipewire_test_input>"
        )
        add_test(
            NAME microphone_capture_routing
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/capture-routing.sh"
                "$<TARGET_FILE:shatter>" "$<TARGET_FILE:pipewire_test_source>"
        )
        set_tests_properties(app_playback_routing microphone_capture_routing PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        message(STATUS "PipeWire integration tests: enabled")
    endif()
endif()
