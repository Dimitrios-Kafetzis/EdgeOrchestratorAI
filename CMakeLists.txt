cmake_minimum_required(VERSION 3.22)
project(EdgeOrchestrator
    VERSION 1.0.0
    DESCRIPTION "Distributed adaptive resource orchestrator for edge AI workloads"
    LANGUAGES CXX
)

# ──────────────────────────────────────────────
# Global Settings
# ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ──────────────────────────────────────────────
# Options
# ──────────────────────────────────────────────
option(BUILD_TESTS       "Build unit and integration tests"       ON)
option(BUILD_TOOLS       "Install Python analysis tools"          OFF)
option(ENABLE_SANITIZERS "Enable AddressSanitizer + UBSan"        OFF)
option(CROSS_COMPILE_ARM "Cross-compile for ARM64 (Raspberry Pi)" OFF)

# ──────────────────────────────────────────────
# Compiler Warnings
# ──────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
    -Wconversion -Wsign-conversion -Wnull-dereference
    -Wformat=2 -Wimplicit-fallthrough
)

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ──────────────────────────────────────────────
# Dependencies
# ──────────────────────────────────────────────
include(FetchContent)

# Protobuf
find_package(Protobuf REQUIRED)

# nlohmann/json (header-only, for telemetry)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# toml++ (header-only, for config)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

if(BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# ──────────────────────────────────────────────
# Protobuf Code Generation
# ──────────────────────────────────────────────
set(PROTO_FILES proto/protocol.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# Generated headers go into build dir
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# ──────────────────────────────────────────────
# Library Targets
# ──────────────────────────────────────────────

# Core
add_library(eo_core STATIC
    src/core/logger.cpp
    src/core/config.cpp
)
target_include_directories(eo_core PUBLIC src)
target_link_libraries(eo_core PUBLIC nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus)

# Resource Monitor
add_library(eo_resource_monitor STATIC
    src/resource_monitor/linux_monitor.cpp
    src/resource_monitor/mock_monitor.cpp
)
target_include_directories(eo_resource_monitor PUBLIC src)
target_link_libraries(eo_resource_monitor PUBLIC eo_core)

# Workload
add_library(eo_workload STATIC
    src/workload/dag.cpp
    src/workload/generator.cpp
)
target_include_directories(eo_workload PUBLIC src)
target_link_libraries(eo_workload PUBLIC eo_core)

# Scheduler
add_library(eo_scheduler STATIC
    src/scheduler/greedy_policy.cpp
    src/scheduler/threshold_policy.cpp
    src/scheduler/optimizer_policy.cpp
    src/scheduler/cluster_view.cpp
)
target_include_directories(eo_scheduler PUBLIC src)
target_link_libraries(eo_scheduler PUBLIC eo_core eo_workload)

# Executor
add_library(eo_executor STATIC
    src/executor/thread_pool.cpp
    src/executor/task_runner.cpp
    src/executor/memory_pool.cpp
)
target_include_directories(eo_executor PUBLIC src)
target_link_libraries(eo_executor PUBLIC eo_core)

# Network
add_library(eo_network STATIC
    src/network/peer_discovery.cpp
    src/network/transport.cpp
    src/network/cluster_view.cpp
    ${PROTO_SRCS}
)
target_include_directories(eo_network PUBLIC src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(eo_network PUBLIC eo_core protobuf::libprotobuf)

# Telemetry
add_library(eo_telemetry STATIC
    src/telemetry/metrics_collector.cpp
    src/telemetry/json_sink.cpp
)
target_include_directories(eo_telemetry PUBLIC src)
target_link_libraries(eo_telemetry PUBLIC eo_core nlohmann_json::nlohmann_json)

# Orchestrator (facade — depends on all other modules)
add_library(eo_orchestrator STATIC
    src/orchestrator/offload_codec.cpp
)
target_include_directories(eo_orchestrator PUBLIC src)
target_link_libraries(eo_orchestrator PUBLIC
    eo_core eo_resource_monitor eo_workload eo_scheduler
    eo_executor eo_network eo_telemetry
)

# ──────────────────────────────────────────────
# Main Daemon Executable
# ──────────────────────────────────────────────
add_executable(edge_orchestrator src/app/main.cpp)
target_link_libraries(edge_orchestrator PRIVATE
    eo_orchestrator
    eo_core
    eo_resource_monitor
    eo_workload
    eo_scheduler
    eo_executor
    eo_network
    eo_telemetry
    pthread
)

# ──────────────────────────────────────────────
# Tests
# ──────────────────────────────────────────────
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ──────────────────────────────────────────────
# Install
# ──────────────────────────────────────────────
install(TARGETS edge_orchestrator DESTINATION bin)
install(FILES config/default.toml DESTINATION etc/edge_orchestrator)
