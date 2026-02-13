/**
 * @file test_dag.cpp
 * @brief Comprehensive unit tests for WorkloadDAG.
 * @author Dimitris Kafetzis
 */

#include "workload/dag.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <unordered_set>

using namespace edge_orchestrator;

// ─── Helper ──────────────────────────────────

static Task make_task(const std::string& id, int64_t compute_us = 100,
                      uint64_t memory = 1024) {
    return Task{
        .id = id,
        .name = "Task " + id,
        .profile = TaskProfile{
            .compute_cost = Duration{compute_us},
            .memory_bytes = memory,
            .input_bytes = 0,
            .output_bytes = 0
        },
        .dependencies = {},
        .state = TaskState::Pending
    };
}

// ─── Construction ────────────────────────────

TEST(DAGTest, AddSingleTask) {
    WorkloadDAG dag;
    auto id = dag.add_task(make_task("t1"));
    EXPECT_EQ(id, "t1");
    EXPECT_EQ(dag.task_count(), 1u);
}

TEST(DAGTest, AddMultipleTasks) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_task(make_task("t3"));
    EXPECT_EQ(dag.task_count(), 3u);
}

TEST(DAGTest, GetExistingTask) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1", 500));
    auto task = dag.get_task("t1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->name, "Task t1");
    EXPECT_EQ(task->profile.compute_cost, Duration{500});
}

TEST(DAGTest, GetNonexistentTask) {
    WorkloadDAG dag;
    EXPECT_FALSE(dag.get_task("nonexistent").has_value());
}

// ─── Dependencies ────────────────────────────

TEST(DAGTest, AddDependency) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_dependency("t1", "t2");

    auto deps = dag.dependencies("t2");
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], "t1");
}

TEST(DAGTest, Dependents) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_task(make_task("t3"));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t1", "t3");

    auto dependents = dag.dependents("t1");
    EXPECT_EQ(dependents.size(), 2u);
}

TEST(DAGTest, NoDependencies) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    EXPECT_TRUE(dag.dependencies("t1").empty());
    EXPECT_TRUE(dag.dependents("t1").empty());
}

// ─── Topological Order ──────────────────────

TEST(DAGTest, TopologicalOrderLinearChain) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_task(make_task("t3"));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t2", "t3");

    auto order = dag.topological_order();
    ASSERT_EQ(order.size(), 3u);

    // t1 must come before t2, t2 before t3
    auto pos = [&](const TaskId& id) {
        return std::find(order.begin(), order.end(), id) - order.begin();
    };
    EXPECT_LT(pos("t1"), pos("t2"));
    EXPECT_LT(pos("t2"), pos("t3"));
}

TEST(DAGTest, TopologicalOrderDiamond) {
    // t1 → t2 → t4
    // t1 → t3 → t4
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_task(make_task("t3"));
    dag.add_task(make_task("t4"));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t1", "t3");
    dag.add_dependency("t2", "t4");
    dag.add_dependency("t3", "t4");

    auto order = dag.topological_order();
    ASSERT_EQ(order.size(), 4u);

    auto pos = [&](const TaskId& id) {
        return std::find(order.begin(), order.end(), id) - order.begin();
    };
    EXPECT_LT(pos("t1"), pos("t2"));
    EXPECT_LT(pos("t1"), pos("t3"));
    EXPECT_LT(pos("t2"), pos("t4"));
    EXPECT_LT(pos("t3"), pos("t4"));
}

TEST(DAGTest, TopologicalOrderSingleTask) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    auto order = dag.topological_order();
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "t1");
}

TEST(DAGTest, TopologicalOrderDisjointTasks) {
    WorkloadDAG dag;
    dag.add_task(make_task("a"));
    dag.add_task(make_task("b"));
    dag.add_task(make_task("c"));
    // No dependencies — all independent
    auto order = dag.topological_order();
    EXPECT_EQ(order.size(), 3u);
}

// ─── Cycle Detection ─────────────────────────

TEST(DAGTest, NoCycleInChain) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_dependency("t1", "t2");
    EXPECT_FALSE(dag.has_cycle());
    EXPECT_TRUE(dag.is_valid());
}

TEST(DAGTest, DetectSimpleCycle) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t2", "t1");  // Creates cycle
    EXPECT_TRUE(dag.has_cycle());
    EXPECT_FALSE(dag.is_valid());
}

TEST(DAGTest, DetectLongerCycle) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_task(make_task("t3"));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t2", "t3");
    dag.add_dependency("t3", "t1");  // Creates cycle
    EXPECT_TRUE(dag.has_cycle());
}

TEST(DAGTest, EmptyDAGIsInvalid) {
    WorkloadDAG dag;
    EXPECT_FALSE(dag.is_valid());
}

// ─── Ready Tasks ─────────────────────────────

TEST(DAGTest, AllTasksReadyWhenNoDependencies) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_task(make_task("t3"));

    auto ready = dag.ready_tasks();
    EXPECT_EQ(ready.size(), 3u);
}

TEST(DAGTest, OnlyRootTaskReady) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_dependency("t1", "t2");

    auto ready = dag.ready_tasks();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0], "t1");
}

TEST(DAGTest, ReadyAfterCompletion) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.add_task(make_task("t2"));
    dag.add_task(make_task("t3"));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t1", "t3");

    // Initially only t1 is ready
    EXPECT_EQ(dag.ready_tasks().size(), 1u);

    // After completing t1, both t2 and t3 become ready
    dag.mark_completed("t1");
    auto ready = dag.ready_tasks();
    EXPECT_EQ(ready.size(), 2u);
}

TEST(DAGTest, RunningTaskNotReady) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.mark_running("t1");
    EXPECT_TRUE(dag.ready_tasks().empty());
}

// ─── State Management ────────────────────────

TEST(DAGTest, MarkStates) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));

    EXPECT_EQ(dag.get_task("t1")->state, TaskState::Pending);

    dag.mark_scheduled("t1");
    EXPECT_EQ(dag.get_task("t1")->state, TaskState::Scheduled);

    dag.mark_running("t1");
    EXPECT_EQ(dag.get_task("t1")->state, TaskState::Running);

    dag.mark_completed("t1");
    EXPECT_EQ(dag.get_task("t1")->state, TaskState::Completed);
}

TEST(DAGTest, MarkFailed) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1"));
    dag.mark_failed("t1");
    EXPECT_EQ(dag.get_task("t1")->state, TaskState::Failed);
}

// ─── Metrics ─────────────────────────────────

TEST(DAGTest, TotalComputeCost) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1", 100));
    dag.add_task(make_task("t2", 200));
    dag.add_task(make_task("t3", 300));
    EXPECT_EQ(dag.total_compute_cost(), Duration{600});
}

TEST(DAGTest, CriticalPathLinearChain) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1", 100));
    dag.add_task(make_task("t2", 200));
    dag.add_task(make_task("t3", 300));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t2", "t3");

    // Critical path = t1 + t2 + t3 = 600
    EXPECT_EQ(dag.critical_path_cost(), Duration{600});
}

TEST(DAGTest, CriticalPathDiamond) {
    // t1(100) → t2(200) → t4(50)
    // t1(100) → t3(300) → t4(50)
    // Critical path = t1 + t3 + t4 = 450 (goes through the longer branch)
    WorkloadDAG dag;
    dag.add_task(make_task("t1", 100));
    dag.add_task(make_task("t2", 200));
    dag.add_task(make_task("t3", 300));
    dag.add_task(make_task("t4", 50));
    dag.add_dependency("t1", "t2");
    dag.add_dependency("t1", "t3");
    dag.add_dependency("t2", "t4");
    dag.add_dependency("t3", "t4");

    EXPECT_EQ(dag.critical_path_cost(), Duration{450});
}

TEST(DAGTest, CriticalPathDisjointTasks) {
    WorkloadDAG dag;
    dag.add_task(make_task("t1", 100));
    dag.add_task(make_task("t2", 500));
    dag.add_task(make_task("t3", 200));
    // No dependencies — critical path = max individual cost
    EXPECT_EQ(dag.critical_path_cost(), Duration{500});
}

TEST(DAGTest, PeakMemoryEstimate) {
    // t1(mem=1024) → t3(mem=512)
    // t2(mem=2048) → t3(mem=512)
    // Level 0: t1 + t2 = 3072, Level 1: t3 = 512
    WorkloadDAG dag;
    dag.add_task(make_task("t1", 100, 1024));
    dag.add_task(make_task("t2", 100, 2048));
    dag.add_task(make_task("t3", 100, 512));
    dag.add_dependency("t1", "t3");
    dag.add_dependency("t2", "t3");

    EXPECT_EQ(dag.peak_memory_estimate(), 3072u);
}

TEST(DAGTest, CriticalPathEmptyDAG) {
    WorkloadDAG dag;
    EXPECT_EQ(dag.critical_path_cost(), Duration{0});
}
