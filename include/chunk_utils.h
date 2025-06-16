#ifndef CHUNK_UTILS_H
#define CHUNK_UTILS_H

#include <git2.h>
#include <stddef.h>
#include <stdint.h>

#define BUP_WINDOWBITS 6
#define BUP_WINDOWSIZE (1 << BUP_WINDOWBITS)
#define BUP_BLOBBITS 12
#define BUP_MAX_EXTRA_BITS 0
#define BUP_CHUNK_MASK ((1 << BUP_BLOBBITS) - 1)
#define BUP_MIN_CHUNK (1 << (BUP_BLOBBITS - BUP_MAX_EXTRA_BITS))
#define BUP_MAX_CHUNK (1 << (BUP_BLOBBITS + BUP_MAX_EXTRA_BITS))
#define BUP_ROLL_BASE 31
#define BUP_ROLL_SHIFT 16
#define BUP_ROLL_MASK 0xffff

typedef struct {
    unsigned s1, s2;
    uint8_t window[BUP_WINDOWSIZE];
    int wofs;
} Rollsum;

typedef struct bup_chunk {
    git_oid oid;
    size_t len;
    struct bup_chunk *next;
} bup_chunk;

void rollsum_init(Rollsum *r);
void rollsum_roll(Rollsum *r, uint8_t c);
uint32_t rollsum_digest(const Rollsum *r);

bup_chunk *chunk_get_or_create(git_odb *odb, bup_chunk **pool,
                               const void *data, size_t len);
void chunk_pool_free(bup_chunk **pool);
int chunk_pool_count(void);
size_t chunk_pool_total_size(void);
int parse_chunk_list(const char *data, size_t size, git_oid **oids,
                     size_t **lengths, size_t *count);

#endif /* CHUNK_UTILS_H */
