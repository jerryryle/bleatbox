extern "C" {
#include "broadcast_log_core.h"
}

#include <gtest/gtest.h>

class BroadcastLogCoreTest : public ::testing::Test {
protected:
    struct broadcast_log log;

    void SetUp() override
    {
        broadcast_log_core_init(&log);
    }
};

TEST_F(BroadcastLogCoreTest, EmptyLogNoDuplicate)
{
    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, 0));
}

TEST_F(BroadcastLogCoreTest, SamePairIsDuplicate)
{
    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, 0));
    EXPECT_TRUE(broadcast_log_core_check_and_record(&log, 0x01, 0));
}

TEST_F(BroadcastLogCoreTest, DifferentOriginatorNotDuplicate)
{
    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, 0));
    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x02, 0));
}

TEST_F(BroadcastLogCoreTest, DifferentSeqNotDuplicate)
{
    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, 0));
    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, 1));
}

TEST_F(BroadcastLogCoreTest, RecordThenCheckIsDuplicate)
{
    broadcast_log_core_record(&log, 0x01, 5);
    EXPECT_TRUE(broadcast_log_core_check_and_record(&log, 0x01, 5));
}

TEST_F(BroadcastLogCoreTest, InitResetsLog)
{
    broadcast_log_core_record(&log, 0x01, 0);
    broadcast_log_core_init(&log);
    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, 0));
}

TEST_F(BroadcastLogCoreTest, FillToCapacity)
{
    for (uint8_t i = 0; i < BROADCAST_LOG_SIZE; i++) {
        EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, i));
    }
    for (uint8_t i = 0; i < BROADCAST_LOG_SIZE; i++) {
        EXPECT_TRUE(broadcast_log_core_check_and_record(&log, 0x01, i));
    }
}

TEST_F(BroadcastLogCoreTest, OldestEntryEvictedOnOverflow)
{
    for (uint8_t i = 0; i < BROADCAST_LOG_SIZE; i++) {
        broadcast_log_core_record(&log, 0x01, i);
    }

    /* One more evicts the oldest (seq 0) */
    broadcast_log_core_record(&log, 0x01, BROADCAST_LOG_SIZE);

    EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, 0));
    EXPECT_TRUE(broadcast_log_core_check_and_record(&log, 0x01, 2));
}

TEST_F(BroadcastLogCoreTest, EvictionWrapsAround)
{
    /* Fill and overflow so the write pointer wraps */
    for (uint8_t i = 0; i < BROADCAST_LOG_SIZE + 3; i++) {
        broadcast_log_core_record(&log, 0x01, i);
    }

    /* First 3 entries should have been evicted */
    for (uint8_t i = 0; i < 3; i++) {
        EXPECT_FALSE(broadcast_log_core_check_and_record(&log, 0x01, i));
    }
}
