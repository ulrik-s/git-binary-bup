#include "bup_odb.h"
#include <git2/sys/odb_backend.h>
#include <git2/odb.h>
#include <git2.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>



static int read_calls = 0;
static int write_calls = 0;
static int free_calls = 0;

static int bup_backend_read(void **buffer, size_t *len, git_object_t *type,
                           git_odb_backend *backend, const git_oid *oid)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    read_calls++;

    git_odb_object *obj = NULL;
    if (git_odb_read(&obj, b->odb, oid) < 0)
        return GIT_ENOTFOUND;

    const char *data = git_odb_object_data(obj);
    size_t size = git_odb_object_size(obj);

    git_oid *oids = NULL;
    size_t *lens = NULL;
    size_t count = 0;
    if (git_odb_object_type(obj) != GIT_OBJECT_BLOB ||
        parse_chunk_list(data, size, &oids, &lens, &count) < 0 || count == 0) {
        *type = git_odb_object_type(obj);
        *len = size;
        *buffer = malloc(size);
        if (!*buffer) {
            git_odb_object_free(obj);
            return -1;
        }
        memcpy(*buffer, data, size);
        git_odb_object_free(obj);
        return 0;
    }

    git_odb_object_free(obj);

    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += lens[i];

    char *buf = malloc(total);
    if (!buf) {
        free(oids);
        free(lens);
        return -1;
    }

    size_t ofs = 0;
    for (size_t i = 0; i < count; i++) {
        git_odb_object *chunk_obj = NULL;
        if (git_odb_read(&chunk_obj, b->odb, &oids[i]) < 0) {
            free(buf);
            free(oids);
            free(lens);
            return -1;
        }
        memcpy(buf + ofs, git_odb_object_data(chunk_obj),
               git_odb_object_size(chunk_obj));
        ofs += git_odb_object_size(chunk_obj);
        git_odb_object_free(chunk_obj);
    }

    free(oids);
    free(lens);

    *type = GIT_OBJECT_BLOB;
    *len = total;
    *buffer = buf;
    return 0;
}

static int bup_backend_write(git_odb_backend *backend, const git_oid *oid,
                             const void *data, size_t len, git_object_t type)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    write_calls++;

    if (type != GIT_OBJECT_BLOB)
        return git_odb_write((git_oid *)oid, b->odb, data, len, type);

    size_t est_count = len / BUP_MIN_CHUNK + 1;
    size_t est_size = est_count * (GIT_OID_HEXSZ + 1 + 20 + 1);
    char *list = malloc(est_size);
    if (!list)
        return -1;
    size_t pos = 0;
    const unsigned char *buf = data;
    Rollsum r;
    rollsum_init(&r);

    size_t chunk_start = 0;
    size_t chunk_len = 0;

    for (size_t i = 0; i < len; i++) {
        rollsum_roll(&r, buf[i]);
        chunk_len++;

        int at_end = (i == len - 1);
        int boundary = chunk_len >= BUP_MIN_CHUNK &&
                       ((rollsum_digest(&r) & BUP_CHUNK_MASK) == 0 ||
                        chunk_len >= BUP_MAX_CHUNK);
        if (boundary || at_end) {
            bup_chunk *c = chunk_get_or_create(b->odb, &b->chunk_pool,
                                               buf + chunk_start, chunk_len);
            if (!c) {
                free(list);
                return -1;
            }
            char hex[GIT_OID_HEXSZ + 1];
            git_oid_tostr(hex, sizeof(hex), &c->oid);
            int n = snprintf(list + pos, est_size - pos, "%s %zu\n", hex, c->len);
            pos += (size_t)n;
            chunk_start = i + 1;
            chunk_len = 0;
        }
    }

    int ret = git_odb_write((git_oid *)oid, b->odb, list, pos,
                            GIT_OBJECT_BLOB);
    free(list);
    return ret;
}

static void bup_backend_free(git_odb_backend *backend)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    free_calls++;
    chunk_pool_free(&b->chunk_pool);
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
    return chunk_pool_count();
}

size_t bup_backend_total_size(void)
{
    return chunk_pool_total_size();
}

