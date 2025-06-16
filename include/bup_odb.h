#ifndef BUP_ODB_H
#define BUP_ODB_H

#include <git2.h>
#include <git2/sys/odb_backend.h>
#include "chunk_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bup_odb_backend {
    git_odb_backend parent;
    char *path;
    git_odb *odb;
    bup_chunk *chunk_pool;
} bup_odb_backend;

int bup_odb_backend_new(git_odb_backend **out, const char *path);

/* Test helpers to verify backend callbacks are invoked */
int bup_backend_read_calls(void);
int bup_backend_write_calls(void);
int bup_backend_free_calls(void);
int bup_backend_chunk_count(void);
size_t bup_backend_total_size(void);
size_t bup_backend_object_chunk_count(git_odb_backend *backend,
                                      const git_oid *oid,
                                      git_oid **chunk_oids,
                                      size_t **lengths);

#ifdef __cplusplus
}
#endif

#endif /* BUP_ODB_H */
