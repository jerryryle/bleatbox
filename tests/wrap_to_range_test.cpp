extern "C" {
#include "wrap_to_range.h"
}

#include <gtest/gtest.h>

TEST(WrapToRange, WithinRange)
{
    EXPECT_EQ(wrap_to_range(5, 1, 10), 5u);
}

TEST(WrapToRange, AtMin)
{
    EXPECT_EQ(wrap_to_range(1, 1, 10), 1u);
}

TEST(WrapToRange, AtMax)
{
    EXPECT_EQ(wrap_to_range(10, 1, 10), 10u);
}

TEST(WrapToRange, OneAboveMax)
{
    EXPECT_EQ(wrap_to_range(11, 1, 10), 1u);
}

TEST(WrapToRange, WrapsAround)
{
    EXPECT_EQ(wrap_to_range(13, 1, 10), 3u);
}

TEST(WrapToRange, LargeValue)
{
    EXPECT_EQ(wrap_to_range(99, 1, 10), 9u);
}

TEST(WrapToRange, EqualMinMax)
{
    EXPECT_EQ(wrap_to_range(5, 5, 5), 5u);
}

TEST(WrapToRange, MinGreaterThanMax)
{
    EXPECT_EQ(wrap_to_range(3, 5, 2), 5u);
}

TEST(WrapToRange, ZeroMin)
{
    EXPECT_EQ(wrap_to_range(7, 0, 3), 3u);
}

TEST(WrapToRange, FullRange)
{
    EXPECT_EQ(wrap_to_range(42, 0, UINT32_MAX), 42u);
}
