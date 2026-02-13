# EdgeOrchestrator Test Suite

include(GoogleTest)

# ──────────────────────────────────────────────
# Unit Tests — Core
# ──────────────────────────────────────────────

add_executable(test_core
    unit/test_result.cpp
    unit/test_types.cpp
    unit/test_config.cpp
)
target_link_libraries(test_core PRIVATE eo_core GTest::gtest_main)
gtest_discover_tests(test_core)

# ──────────────────────────────────────────────
# Unit Tests — Workload
# ──────────────────────────────────────────────

add_executable(test_workload
    unit/test_dag.cpp
    unit/test_generator.cpp
)
target_link_libraries(test_workload PRIVATE eo_workload GTest::gtest_main)
gtest_discover_tests(test_workload)

# ──────────────────────────────────────────────
# Unit Tests — Executor
# ──────────────────────────────────────────────

add_executable(test_executor
    unit/test_thread_pool.cpp
    unit/test_memory_pool.cpp
)
target_link_libraries(test_executor PRIVATE eo_executor GTest::gtest_main)
gtest_discover_tests(test_executor)

# ──────────────────────────────────────────────
# Unit Tests — Resource Monitor
# ──────────────────────────────────────────────

add_executable(test_monitor
    unit/test_monitor.cpp
)
target_link_libraries(test_monitor PRIVATE eo_resource_monitor eo_core GTest::gtest_main)
gtest_discover_tests(test_monitor)

# ──────────────────────────────────────────────
# Unit Tests — Scheduler
# ──────────────────────────────────────────────

add_executable(test_scheduler
    unit/test_scheduler.cpp
)
target_link_libraries(test_scheduler PRIVATE eo_scheduler eo_workload eo_core GTest::gtest_main)
gtest_discover_tests(test_scheduler)

# ──────────────────────────────────────────────
# Unit Tests — Network
# ──────────────────────────────────────────────

add_executable(test_network
    unit/test_network.cpp
)
target_link_libraries(test_network PRIVATE eo_network eo_scheduler eo_core GTest::gtest_main)
gtest_discover_tests(test_network)

# ──────────────────────────────────────────────
# Unit Tests — Orchestrator
# ──────────────────────────────────────────────

add_executable(test_orchestrator
    unit/test_orchestrator.cpp
)
target_link_libraries(test_orchestrator PRIVATE eo_orchestrator GTest::gtest_main)
gtest_discover_tests(test_orchestrator)

# ──────────────────────────────────────────────
# Integration Tests
# ──────────────────────────────────────────────

add_executable(test_integration
    integration/test_single_node.cpp
)
target_link_libraries(test_integration PRIVATE
    eo_orchestrator
    GTest::gtest_main
)
gtest_discover_tests(test_integration)

# ──────────────────────────────────────────────
# Benchmarks (not part of ctest, run manually)
# ──────────────────────────────────────────────

add_executable(bench_scheduler
    benchmark/bench_scheduler.cpp
)
target_link_libraries(bench_scheduler PRIVATE eo_orchestrator)
