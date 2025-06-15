#include "bup_odb.h"
#include <git2/sys/odb_backend.h>
#include <string.h>
#include <stdlib.h>

struct bup_odb_backend {
    git_odb_backend parent;
    char *path;
};

typedef struct bup_odb_backend bup_odb_backend;

static int read_calls = 0;
static int write_calls = 0;
static int free_calls = 0;

static int bup_backend_read(void **buffer, size_t *len, git_object_t *type,
                           git_odb_backend *backend, const git_oid *oid)
{
    (void)buffer; (void)len; (void)type; (void)backend; (void)oid;
    read_calls++;
    return GIT_ENOTFOUND;
}

static int bup_backend_write(git_odb_backend *backend, const git_oid *oid,
                             const void *data, size_t len, git_object_t type)
{
    (void)backend; (void)oid; (void)data; (void)len; (void)type;
    write_calls++;
    return GIT_ERROR;
}

static void bup_backend_free(git_odb_backend *backend)
{
    bup_odb_backend *b = (bup_odb_backend *)backend;
    free_calls++;
    free(b->path);
    free(b);
}

int bup_odb_backend_new(git_odb_backend **out, const char *path)
{
    bup_odb_backend *backend = calloc(1, sizeof(*backend));
    if (!backend)
        return -1;

    backend->path = path ? strdup(path) : NULL;

    backend->parent.version = GIT_ODB_BACKEND_VERSION;
    backend->parent.read = bup_backend_read;
    backend->parent.write = bup_backend_write;
    backend->parent.free = bup_backend_free;

    *out = (git_odb_backend *)backend;
    return 0;
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
