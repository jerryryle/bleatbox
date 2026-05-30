extern "C" {
#include "clamp.h"
}

#include <gtest/gtest.h>

TEST(Clamp, WithinRange) {
	EXPECT_EQ(clamp(5, 1, 10), 5u);
}

TEST(Clamp, BelowMin) {
	EXPECT_EQ(clamp(0, 1, 10), 1u);
}

TEST(Clamp, AboveMax) {
	EXPECT_EQ(clamp(99, 1, 10), 10u);
}

TEST(Clamp, EqualMinMax) {
	EXPECT_EQ(clamp(5, 5, 5), 5u);
}

TEST(Clamp, AllZeros) {
	EXPECT_EQ(clamp(0, 0, 0), 0u);
}

TEST(Clamp, BelowEqualMinMax) {
	EXPECT_EQ(clamp(3, 5, 5), 5u);
}
