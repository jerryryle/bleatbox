extern "C" {
#include "sounds_match.h"
}

#include <gtest/gtest.h>
#include <cstring>

/* ------------------------------------------------------------------ */
/* sounds_try_match                                                   */
/* ------------------------------------------------------------------ */

TEST(TryMatch, ValidGoatTwoDigits)
{
    const char *name = "goat05.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), 5);
}

TEST(TryMatch, SingleDigitRejected)
{
    /* sounds_get_path() generates zero-padded names ("goat09.mp3"),
     * so non-padded files must be rejected at scan time rather than
     * failing to open at playback time. */
    const char *name = "goat9.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), -1);
}

TEST(TryMatch, ValidMisc)
{
    const char *name = "misc15.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "misc", 4), 15);
}

TEST(TryMatch, IndexZero)
{
    const char *name = "goat00.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), 0);
}

TEST(TryMatch, Index99)
{
    const char *name = "goat99.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), 99);
}

TEST(TryMatch, WrongPrefix)
{
    const char *name = "misc05.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), -1);
}

TEST(TryMatch, NoDigits)
{
    const char *name = "goat.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), -1);
}

TEST(TryMatch, ThreeDigits)
{
    const char *name = "goat000.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), -1);
}

TEST(TryMatch, NonDigitChars)
{
    const char *name = "goat0a.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), -1);
}

TEST(TryMatch, PrefixOnly)
{
    const char *name = "goat.mp3";
    EXPECT_EQ(sounds_try_match(name, strrchr(name, '.'), "goat", 4), -1);
}

/* ------------------------------------------------------------------ */
/* sounds_validate_set                                                */
/* ------------------------------------------------------------------ */

TEST(ValidateSet, ContiguousSet)
{
    bool present[100] = {};
    present[0] = true;
    present[1] = true;
    present[2] = true;
    EXPECT_EQ(sounds_validate_set(present, 2), 3);
}

TEST(ValidateSet, SingleEntry)
{
    bool present[100] = {};
    present[0] = true;
    EXPECT_EQ(sounds_validate_set(present, 0), 1);
}

TEST(ValidateSet, GapInMiddle)
{
    bool present[100] = {};
    present[0] = true;
    present[2] = true;
    EXPECT_EQ(sounds_validate_set(present, 2), 0);
}

TEST(ValidateSet, GapAtStart)
{
    bool present[100] = {};
    present[1] = true;
    present[2] = true;
    EXPECT_EQ(sounds_validate_set(present, 2), 0);
}

TEST(ValidateSet, NegativeMaxIndex)
{
    bool present[100] = {};
    EXPECT_EQ(sounds_validate_set(present, -1), 0);
}

TEST(ValidateSet, LargeContiguous)
{
    bool present[100] = {};
    for (int i = 0; i < 50; i++) {
        present[i] = true;
    }
    EXPECT_EQ(sounds_validate_set(present, 49), 50);
}
