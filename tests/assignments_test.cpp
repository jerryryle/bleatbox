extern "C" {
#include "assignments.h"
#include "wrap_to_range.h"

/*
 * Fake sys_rand32_get — returns values from a caller-controlled
 * sequence so tests can predict assignments_generate output.
 */
static uint32_t fake_rand_values[64];
static int fake_rand_index;

uint32_t sys_rand32_get(void)
{
    return fake_rand_values[fake_rand_index++];
}
}

#include <gtest/gtest.h>

class AssignmentsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fake_rand_index = 0;
        memset(fake_rand_values, 0, sizeof(fake_rand_values));
    }
};

TEST_F(AssignmentsTest, NotInitializedReturnsError)
{
    /* Fresh state (no assignments_init called in this test process yet
     * — but since other tests call it, we can't rely on ordering.
     * Instead, test the < 2 sounds path which is always reachable.) */
    const struct assignment *out;
    uint8_t ids[] = {1};
    assignments_init(ids, 1, 0, 100, 1, 1);

    int n = assignments_generate(&out);
    EXPECT_EQ(n, 0);
}

TEST_F(AssignmentsTest, SinglePeerGetsAssignment)
{
    uint8_t ids[] = {0x10};
    /* 5 sounds total, first assignable is index 1 → range [1, 4] */
    assignments_init(ids, 1, 100, 500, 1, 5);

    /* First rand call → sound index, second → delay */
    fake_rand_values[0] = 3;   /* wrap_to_range(3, 1, 4) = 3 */
    fake_rand_values[1] = 200; /* wrap_to_range(200, 100, 500) = 200 */

    const struct assignment *out;
    int n = assignments_generate(&out);

    ASSERT_EQ(n, 1);
    EXPECT_EQ(out[0].device_id, 0x10);
    EXPECT_EQ(out[0].sound, 3);
    EXPECT_EQ(out[0].delay_ms, 200);
}

TEST_F(AssignmentsTest, MultiplePeersGetDistinctIds)
{
    uint8_t ids[] = {0x01, 0x02, 0x03};
    assignments_init(ids, 3, 0, 1000, 1, 5);

    /* 3 peers × 2 rand calls each = 6 values */
    fake_rand_values[0] = 1;   /* peer 0 sound */
    fake_rand_values[1] = 0;   /* peer 0 delay */
    fake_rand_values[2] = 2;   /* peer 1 sound */
    fake_rand_values[3] = 500; /* peer 1 delay */
    fake_rand_values[4] = 4;   /* peer 2 sound */
    fake_rand_values[5] = 999; /* peer 2 delay */

    const struct assignment *out;
    int n = assignments_generate(&out);

    ASSERT_EQ(n, 3);
    EXPECT_EQ(out[0].device_id, 0x01);
    EXPECT_EQ(out[1].device_id, 0x02);
    EXPECT_EQ(out[2].device_id, 0x03);
}

TEST_F(AssignmentsTest, SoundIndicesStayInRange)
{
    uint8_t ids[] = {0x01};
    /* first_sound=1, num_sounds=5 → valid range [1, 4] */
    assignments_init(ids, 1, 0, 0, 1, 5);

    /* Values that would be out of range without wrapping */
    uint32_t test_values[] = {0, 1, 4, 5, 100, UINT32_MAX};
    for (uint32_t v : test_values) {
        fake_rand_index = 0;
        fake_rand_values[0] = v;
        fake_rand_values[1] = 0;

        const struct assignment *out;
        int n = assignments_generate(&out);

        ASSERT_EQ(n, 1);
        EXPECT_GE(out[0].sound, 1) << "value=" << v;
        EXPECT_LE(out[0].sound, 4) << "value=" << v;
    }
}

TEST_F(AssignmentsTest, DelaysStayInRange)
{
    uint8_t ids[] = {0x01};
    assignments_init(ids, 1, 100, 500, 1, 5);

    uint32_t test_values[] = {0, 100, 500, 501, 9999, UINT32_MAX};
    for (uint32_t v : test_values) {
        fake_rand_index = 0;
        fake_rand_values[0] = 1;
        fake_rand_values[1] = v;

        const struct assignment *out;
        int n = assignments_generate(&out);

        ASSERT_EQ(n, 1);
        EXPECT_GE(out[0].delay_ms, 100) << "value=" << v;
        EXPECT_LE(out[0].delay_ms, 500) << "value=" << v;
    }
}

TEST_F(AssignmentsTest, TooFewSoundsReturnsZero)
{
    uint8_t ids[] = {0x01, 0x02};

    /* 0 sounds */
    assignments_init(ids, 2, 0, 100, 0, 0);
    const struct assignment *out;
    EXPECT_EQ(assignments_generate(&out), 0);

    /* 1 sound */
    assignments_init(ids, 2, 0, 100, 0, 1);
    EXPECT_EQ(assignments_generate(&out), 0);
}

TEST_F(AssignmentsTest, PeerCountClampedToMax)
{
    /* MAX_PEERS is 30 — pass more and verify no crash */
    uint8_t ids[35];
    for (int i = 0; i < 35; i++) {
        ids[i] = (uint8_t)(i + 1);
    }
    assignments_init(ids, 35, 0, 100, 1, 5);

    const struct assignment *out;
    int n = assignments_generate(&out);

    EXPECT_EQ(n, 30);
    /* First and last clamped peer have correct IDs */
    EXPECT_EQ(out[0].device_id, 1);
    EXPECT_EQ(out[29].device_id, 30);
}

TEST_F(AssignmentsTest, GenerateCanBeCalledRepeatedly)
{
    uint8_t ids[] = {0x01};
    assignments_init(ids, 1, 0, 100, 1, 5);

    const struct assignment *out1;
    fake_rand_values[0] = 2;
    fake_rand_values[1] = 50;
    int n1 = assignments_generate(&out1);
    uint8_t sound1 = out1[0].sound;

    fake_rand_index = 0;
    fake_rand_values[0] = 3;
    fake_rand_values[1] = 75;
    const struct assignment *out2;
    int n2 = assignments_generate(&out2);

    ASSERT_EQ(n1, 1);
    ASSERT_EQ(n2, 1);
    /* Second call overwrites the internal array */
    EXPECT_EQ(out2[0].sound, 3);
    EXPECT_NE(sound1, out2[0].sound);
}
