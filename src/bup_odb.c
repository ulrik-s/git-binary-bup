#include "bup_odb.h"
#include <git2/sys/odb_backend.h>
#include <git2/odb.h>
#include <git2.h>
#include <string.h>
#include <stdlib.h>

struct bup_odb_backend {
    git_odb_backend parent;
    char *path;
    git_odb *odb;
    struct bup_object *objects;
    struct bup_chunk *chunk_pool;
};

typedef struct bup_odb_backend bup_odb_backend;

typedef struct bup_chunk {
    git_oid oid;
    size_t len;
    int refcount;
    struct bup_chunk *next_global;
} bup_chunk;

typedef struct bup_obj_chunk {
    bup_chunk *chunk;
    struct bup_obj_chunk *next;
} bup_obj_chunk;

typedef struct bup_object {
    git_oid oid;
    git_object_t type;
    size_t size;
    struct bup_obj_chunk *chunks;
    struct bup_object *next;
} bup_object;

static int read_calls = 0;
static int write_calls = 0;
static int free_calls = 0;
static int chunk_count = 0;
static size_t chunk_total_size = 0;

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

static inline void rollsum_init(Rollsum *r)
{
    r->s1 = BUP_WINDOWSIZE * BUP_ROLL_BASE;
    r->s2 = BUP_WINDOWSIZE * (BUP_WINDOWSIZE - 1) * BUP_ROLL_BASE;
    r->wofs = 0;
    memset(r->window, 0, BUP_WINDOWSIZE);
}

static inline void rollsum_roll(Rollsum *r, uint8_t c)
{
    uint8_t drop = r->window[r->wofs];
    r->s1 += c - drop;
    r->s2 += r->s1 - (BUP_WINDOWSIZE * (drop + BUP_ROLL_BASE));
    r->window[r->wofs] = c;
    r->wofs = (r->wofs + 1) % BUP_WINDOWSIZE;
}

static inline uint32_t rollsum_digest(Rollsum *r)
{
    return (r->s1 << BUP_ROLL_SHIFT) | (r->s2 & BUP_ROLL_MASK);
}

static bup_chunk *chunk_create(bup_odb_backend *b, const void *data, size_t len)
{
    git_oid oid;
    if (git_odb_write(&oid, b->odb, data, len, GIT_OBJECT_BLOB) < 0)
        return NULL;

    bup_chunk *c = malloc(sizeof(*c));
    if (!c)
        return NULL;

    git_oid_cpy(&c->oid, &oid);
    c->len = len;
    c->refcount = 0;
    c->next_global = NULL;
    chunk_total_size += len;
    return c;
}

static bup_chunk *chunk_get_or_create(bup_odb_backend *b, const void *data, size_t len)
{
    git_oid oid;
    if (git_odb_hash(&oid, data, len, GIT_OBJECT_BLOB) < 0)
        return NULL;

    for (bup_chunk *c = b->chunk_pool; c; c = c->next_global) {
        if (git_oid_cmp(&c->oid, &oid) == 0) {
            c->refcount++;
            return c;
        }
    }

    bup_chunk *c = chunk_create(b, data, len);
    if (!c)
        return NULL;
    c->refcount = 1;
    c->next_global = b->chunk_pool;
    b->chunk_pool = c;
    chunk_count++;
    return c;
}

static int object_add_chunk(bup_odb_backend *backend, struct bup_object *obj,
                            const void *data, size_t len)
{
    bup_chunk *c = chunk_get_or_create(backend, data, len);
    if (!c)
        return -1;
    bup_obj_chunk *oc = malloc(sizeof(*oc));
    if (!oc)
        return -1;
    oc->chunk = c;
    oc->next = NULL;
    if (!obj->chunks) {
        obj->chunks = oc;
    } else {
        bup_obj_chunk *tail = obj->chunks;
        while (tail->next)
            tail = tail->next;
        tail->next = oc;
    }
    obj->size += len;
    return 0;
}

static struct bup_object *object_create(git_object_t type)
{
    struct bup_object *obj = calloc(1, sizeof(*obj));
    if (!obj)
        return NULL;
    obj->type = type;
    obj->size = 0;
    obj->chunks = NULL;
    obj->next = NULL;
    return obj;
}

static int bup_backend_read(void **buffer, size_t *len, git_object_t *type,
                           git_odb_backend *backend, const git_oid *oid)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    read_calls++;

    struct bup_object *obj = b->objects;
    while (obj) {
        if (git_oid_cmp(&obj->oid, oid) == 0)
            break;
        obj = obj->next;
    }

    if (!obj)
        return GIT_ENOTFOUND;

    *type = obj->type;
    *len = obj->size;
    *buffer = malloc(obj->size);
    if (!*buffer)
        return -1;

    size_t ofs = 0;
    bup_obj_chunk *oc = obj->chunks;
    while (oc) {
        bup_chunk *c = oc->chunk;
        git_odb_object *chunk_obj = NULL;
        if (git_odb_read(&chunk_obj, b->odb, &c->oid) < 0) {
            free(*buffer);
            return -1;
        }
        memcpy((char *)(*buffer) + ofs, git_odb_object_data(chunk_obj),
               git_odb_object_size(chunk_obj));
        ofs += git_odb_object_size(chunk_obj);
        git_odb_object_free(chunk_obj);
        oc = oc->next;
    }
    return 0;
}

static int bup_backend_write(git_odb_backend *backend, const git_oid *oid,
                             const void *data, size_t len, git_object_t type)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    write_calls++;

    struct bup_object *obj = object_create(type);
    if (!obj)
        return -1;

    const unsigned char *buf = data;
    Rollsum r;
    rollsum_init(&r);

    size_t chunk_start = 0;
    size_t chunk_len = 0;

    for (size_t i = 0; i < len; i++) {
        rollsum_roll(&r, buf[i]);
        chunk_len++;

        if (chunk_len >= BUP_MIN_CHUNK &&
            ((rollsum_digest(&r) & BUP_CHUNK_MASK) == 0 ||
             chunk_len >= BUP_MAX_CHUNK)) {
            if (object_add_chunk(b, obj, buf + chunk_start, chunk_len) < 0)
                goto error;
            chunk_start = i + 1;
            chunk_len = 0;
        }
    }

    if (chunk_len > 0) {
        if (object_add_chunk(b, obj, buf + chunk_start, chunk_len) < 0)
            goto error;
    }

    git_oid h;
    if (git_odb_hash(&h, data, len, type) < 0)
        goto error;
    git_oid_cpy(&obj->oid, &h);

    obj->next = b->objects;
    b->objects = obj;

    if (oid)
        git_oid_cpy((git_oid *)oid, &h);

    return 0;

error:
    if (obj) {
        bup_obj_chunk *oc = obj->chunks;
        while (oc) {
            bup_obj_chunk *n = oc->next;
            bup_chunk *c = oc->chunk;
            if (--c->refcount == 0) {
                /* remove from pool */
                bup_chunk **pc = &b->chunk_pool;
                while (*pc && *pc != c)
                    pc = &(*pc)->next_global;
                if (*pc == c)
                    *pc = c->next_global;
                chunk_total_size -= c->len;
                free(c);
                chunk_count--;
            }
            free(oc);
            oc = n;
        }
        free(obj);
    }
    return -1;
}

static void bup_backend_free(git_odb_backend *backend)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    free_calls++;
    struct bup_object *obj = b->objects;
    while (obj) {
        struct bup_object *nobj = obj->next;
        bup_obj_chunk *oc = obj->chunks;
        while (oc) {
            bup_obj_chunk *noc = oc->next;
            bup_chunk *c = oc->chunk;
            if (--c->refcount == 0) {
                bup_chunk **pc = &b->chunk_pool;
                while (*pc && *pc != c)
                    pc = &(*pc)->next_global;
                if (*pc == c)
                    *pc = c->next_global;
                chunk_total_size -= c->len;
                free(c);
                chunk_count--;
            }
            free(oc);
            oc = noc;
        }
        free(obj);
        obj = nobj;
    }
    git_odb_free(b->odb);
    free(b->path);
    free(b);
}

int bup_odb_backend_new(git_odb_backend **out, const char *path)
{
    bup_odb_backend *backend = calloc(1, sizeof(*backend));
    if (!backend)
        return -1;

    backend->path = path ? strdup(path) : NULL;

    git_repository *repo = NULL;
    const char *repo_path = backend->path ? backend->path : ".";
    if (git_repository_open_ext(&repo, repo_path, 0, NULL) < 0)
        goto error;
    if (git_repository_odb(&backend->odb, repo) < 0) {
        git_repository_free(repo);
        goto error;
    }
    git_repository_free(repo);

    backend->parent.version = GIT_ODB_BACKEND_VERSION;
    backend->parent.read = bup_backend_read;
    backend->parent.write = bup_backend_write;
    backend->parent.free = bup_backend_free;
    backend->objects = NULL;
    backend->chunk_pool = NULL;

    *out = (git_odb_backend *)backend;
    return 0;

error:
    free(backend->path);
    free(backend);
    return -1;
}

int bup_backend_read_calls(void)
{
    return read_calls;
}

int bup_backend_write_calls(void)
{
    return write_calls;
}

int bup_backend_free_calls(void)
{
    return free_calls;
}

int bup_backend_chunk_count(void)
{
    return chunk_count;
}

size_t bup_backend_total_size(void)
{
    return chunk_total_size;
}

size_t bup_backend_object_chunks(git_odb_backend *backend, const git_oid *oid,
                                 git_oid **chunk_oids, size_t **lengths)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    struct bup_object *obj = b->objects;
    while (obj) {
        if (git_oid_cmp(&obj->oid, oid) == 0)
            break;
        obj = obj->next;
    }
    if (!obj)
        return 0;

    size_t count = 0;
    bup_obj_chunk *oc = obj->chunks;
    while (oc) {
        count++;
        oc = oc->next;
    }

    if (chunk_oids)
        *chunk_oids = malloc(sizeof(git_oid) * count);
    if (lengths)
        *lengths = malloc(sizeof(size_t) * count);

    oc = obj->chunks;
    size_t i = 0;
    while (oc) {
        if (chunk_oids)
            git_oid_cpy(&(*chunk_oids)[i], &oc->chunk->oid);
        if (lengths)
            (*lengths)[i] = oc->chunk->len;
        oc = oc->next;
        i++;
    }

    return count;
}
