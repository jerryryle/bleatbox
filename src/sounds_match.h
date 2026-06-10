#ifndef SOUNDS_MATCH_H_
#define SOUNDS_MATCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Try to match a filename against a prefix and extract the numeric
 * index.  The index must be exactly two digits, zero-padded, to match
 * the names sounds_get_path() generates.
 *
 * @param filename    The filename to match (e.g. "goat05.mp3").
 * @param dot         Pointer to the last '.' in @p filename.
 * @param prefix      Prefix to match (e.g. "goat").
 * @param prefix_len  Length of @p prefix.
 * @return The numeric index on match, -1 on no match.
 */
int sounds_try_match(const char *filename, const char *dot,
                     const char *prefix, size_t prefix_len);

/**
 * Validate that a set of sound indices is contiguous from 0 to
 * @p max_index with no gaps.
 *
 * @param present    Boolean array indexed by sound number.
 * @param max_index  Highest index found (-1 if none).
 * @return Number of sounds (max_index + 1) if contiguous, 0 if
 *         there are gaps or max_index < 0.
 */
uint8_t sounds_validate_set(const bool *present, int max_index);

#endif /* SOUNDS_MATCH_H_ */
