#include "chunk_utils.h"
#include <string.h>
#include <stdlib.h>

static int chunk_count = 0;
static size_t chunk_total_size = 0;

int chunk_pool_count(void) {
    return chunk_count;
}

size_t chunk_pool_total_size(void) {
    return chunk_total_size;
}

static bup_chunk *find_chunk(bup_chunk *pool, const git_oid *oid) {
    for (bup_chunk *c = pool; c; c = c->next)
        if (git_oid_cmp(&c->oid, oid) == 0)
            return c;
    return NULL;
}

void rollsum_init(Rollsum *r) {
    r->s1 = BUP_WINDOWSIZE * BUP_ROLL_BASE;
    r->s2 = BUP_WINDOWSIZE * (BUP_WINDOWSIZE - 1) * BUP_ROLL_BASE;
    r->wofs = 0;
    memset(r->window, 0, BUP_WINDOWSIZE);
}

void rollsum_roll(Rollsum *r, uint8_t c) {
    uint8_t drop = r->window[r->wofs];
    r->s1 += c - drop;
    r->s2 += r->s1 - (BUP_WINDOWSIZE * (drop + BUP_ROLL_BASE));
    r->window[r->wofs] = c;
    r->wofs = (r->wofs + 1) % BUP_WINDOWSIZE;
}

uint32_t rollsum_digest(const Rollsum *r) {
    return (r->s1 << BUP_ROLL_SHIFT) | (r->s2 & BUP_ROLL_MASK);
}

static bup_chunk *chunk_create(git_odb *odb, const void *data, size_t len) {
    git_oid oid;
    if (git_odb_write(&oid, odb, data, len, GIT_OBJECT_BLOB) < 0)
        return NULL;

    bup_chunk *c = malloc(sizeof(*c));
    if (!c)
        return NULL;

    git_oid_cpy(&c->oid, &oid);
    c->len = len;
    c->next = NULL;
    chunk_total_size += len;
    chunk_count++;
    return c;
}

bup_chunk *chunk_get_or_create(git_odb *odb, bup_chunk **pool,
                               const void *data, size_t len) {
    git_oid oid;
    if (git_odb_hash(&oid, data, len, GIT_OBJECT_BLOB) < 0)
        return NULL;
    bup_chunk *c = find_chunk(*pool, &oid);
    if (c)
        return c;

    c = chunk_create(odb, data, len);
    if (!c)
        return NULL;
    c->next = *pool;
    *pool = c;
    return c;
}

void chunk_pool_free(bup_chunk **pool) {
    bup_chunk *c = *pool;
    while (c) {
        bup_chunk *next = c->next;
        chunk_total_size -= c->len;
        chunk_count--;
        free(c);
        c = next;
    }
    *pool = NULL;
}

