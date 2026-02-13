/**
 * @file test_result.cpp
 * @brief Unit tests for Result<T, E> monadic error type.
 */

#include "core/result.hpp"

#include <gtest/gtest.h>

using namespace edge_orchestrator;

TEST(ResultTest, SuccessValue) {
    Result<int> r = 42;
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(ResultTest, ErrorValue) {
    Result<int> r = Error{"something went wrong"};
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message, "something went wrong");
}

TEST(ResultTest, BoolConversion) {
    Result<int> success = 1;
    Result<int> failure = Error{"fail"};
    EXPECT_TRUE(static_cast<bool>(success));
    EXPECT_FALSE(static_cast<bool>(failure));
}

TEST(ResultTest, ValueOr) {
    Result<int> success = 42;
    Result<int> failure = Error{"fail"};
    EXPECT_EQ(success.value_or(0), 42);
    EXPECT_EQ(failure.value_or(0), 0);
}

TEST(ResultTest, Map) {
    Result<int> r = 21;
    auto doubled = r.map([](int v) { return v * 2; });
    ASSERT_TRUE(doubled.has_value());
    EXPECT_EQ(*doubled, 42);
}

TEST(ResultTest, MapOnError) {
    Result<int> r = Error{"fail"};
    auto doubled = r.map([](int v) { return v * 2; });
    ASSERT_FALSE(doubled.has_value());
    EXPECT_EQ(doubled.error().message, "fail");
}
