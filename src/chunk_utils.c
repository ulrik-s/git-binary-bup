#include "chunk_utils.h"
#include "bup_odb.h"
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

int parse_chunk_list(const char *data, size_t size, git_oid **oids,
                     size_t **lengths, size_t *count) {
    const char *ptr = data;
    const char *end = data + size;
    size_t n = 0;
    while (ptr < end) {
        const char *nl = memchr(ptr, '\n', (size_t)(end - ptr));
        if (!nl)
            return -1;
        n++;
        ptr = nl + 1;
    }

    git_oid *tmp_oids = malloc(sizeof(git_oid) * n);
    size_t *tmp_len = malloc(sizeof(size_t) * n);
    if (!tmp_oids || !tmp_len) {
        free(tmp_oids);
        free(tmp_len);
        return -1;
    }

    ptr = data;
    for (size_t i = 0; i < n; i++) {
        const char *nl = memchr(ptr, '\n', (size_t)(end - ptr));
        const char *sp = memchr(ptr, ' ', (size_t)(nl - ptr));
        if (!nl || !sp || (size_t)(sp - ptr) != GIT_OID_HEXSZ)
            goto fail;

        char hex[GIT_OID_HEXSZ + 1];
        memcpy(hex, ptr, GIT_OID_HEXSZ);
        hex[GIT_OID_HEXSZ] = '\0';
        if (git_oid_fromstr(&tmp_oids[i], hex) < 0)
            goto fail;

        tmp_len[i] = (size_t)strtoull(sp + 1, NULL, 10);

        ptr = nl + 1;
    }

    *oids = tmp_oids;
    *lengths = tmp_len;
    *count = n;
    return 0;

fail:
    free(tmp_oids);
    free(tmp_len);
    return -1;
}

size_t bup_backend_object_chunk_count(git_odb_backend *backend,
                                      const git_oid *oid,
                                      git_oid **chunk_oids,
                                      size_t **lengths) {
    bup_odb_backend *b = (bup_odb_backend *)backend;
    git_odb_object *obj = NULL;
    if (git_odb_read(&obj, b->odb, oid) < 0)
        return 0;

    git_oid *oids = NULL;
    size_t *lens = NULL;
    size_t count = 0;
    if (parse_chunk_list(git_odb_object_data(obj),
                         git_odb_object_size(obj), &oids, &lens, &count) < 0) {
        git_odb_object_free(obj);
        return 0;
    }
    git_odb_object_free(obj);

    if (chunk_oids)
        *chunk_oids = oids;
    else
        free(oids);
    if (lengths)
        *lengths = lens;
    else
        free(lens);
    return count;
}

