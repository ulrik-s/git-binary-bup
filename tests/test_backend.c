#include "bup_odb.h"
#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <assert.h>

#include <string.h>

#define LARGE_BLOB_SIZE 20000
#define LARGE_MOD_POS1 50
#define LARGE_MOD_POS2 15000
#define MAX_NEW_CHUNKS 3
#define ALPHABET_LEN 26

int main(void)
{
    git_libgit2_init();

    git_odb_backend *backend = NULL;
    int ret = bup_odb_backend_new(&backend, NULL);
    assert(ret == 0 && backend != NULL);

    /* initial call counts */
    assert(bup_backend_read_calls() == 0);
    assert(bup_backend_write_calls() == 0);
    assert(bup_backend_free_calls() == 0);

    /* direct call of read */
    void *buf = NULL;
    size_t len = 0;
    git_object_t type = 0;
    git_oid oid;
    git_oid_fromstr(&oid, "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391");
    ret = backend->read(&buf, &len, &type, backend, &oid);
    assert(ret == GIT_ENOTFOUND);
    assert(bup_backend_read_calls() == 1);

    /* integrate with libgit2 ODB */
    git_odb *odb = NULL;
    ret = git_odb_new(&odb);
    assert(ret == 0 && odb != NULL);

    ret = git_odb_add_backend(odb, backend, 1);
    assert(ret == 0);

    git_odb_object *obj = NULL;
    ret = git_odb_read(&obj, odb, &oid);
    assert(ret == GIT_ENOTFOUND && obj == NULL);
    assert(bup_backend_read_calls() == 2);

    const char data[] = "foo";
    git_oid new_oid;
    ret = backend->write(backend, &new_oid, data, sizeof(data) - 1,
                         GIT_OBJECT_BLOB);
    assert(ret == 0);
    assert(bup_backend_write_calls() == 1);

    /* read back via backend */
    void *rbuf = NULL;
    size_t rlen = 0;
    git_object_t rtype = 0;
    ret = backend->read(&rbuf, &rlen, &rtype, backend, &new_oid);
    assert(ret == 0);
    assert(rlen == sizeof(data) - 1);
    assert(rtype == GIT_OBJECT_BLOB);
    assert(memcmp(rbuf, data, rlen) == 0);
    free(rbuf);

    /* read via ODB */
    ret = git_odb_read(&obj, odb, &new_oid);
    assert(ret == 0 && obj != NULL);
    assert(git_odb_object_size(obj) == sizeof(data) - 1);
    git_odb_object_free(obj);

    /* write and read a larger blob */
    const size_t large_size = LARGE_BLOB_SIZE;
    char *large = malloc(large_size);
    for (size_t i = 0; i < large_size; i++)
        large[i] = (char)('a' + (i % ALPHABET_LEN));
    git_oid large_oid;
    ret = backend->write(backend, &large_oid, large, large_size,
                         GIT_OBJECT_BLOB);
    assert(ret == 0);

    size_t chunks_after_first = bup_backend_chunk_count();
    assert(chunks_after_first > 0);

    ret = backend->read(&rbuf, &rlen, &rtype, backend, &large_oid);
    assert(ret == 0 && rlen == large_size);
    assert(memcmp(rbuf, large, large_size) == 0);
    free(rbuf);

    /* modify a couple of bytes and store again */
    char *large2 = malloc(large_size);
    memcpy(large2, large, large_size);
    large2[LARGE_MOD_POS1] = 'x';
    large2[LARGE_MOD_POS2] = 'y';
    git_oid mod_oid;
    ret = backend->write(backend, &mod_oid, large2, large_size,
                         GIT_OBJECT_BLOB);
    assert(ret == 0);
    size_t chunks_after_second = bup_backend_chunk_count();
    assert(chunks_after_second <= chunks_after_first + MAX_NEW_CHUNKS);

    ret = backend->read(&rbuf, &rlen, &rtype, backend, &mod_oid);
    assert(ret == 0 && rlen == large_size);
    assert(memcmp(rbuf, large2, large_size) == 0);
    free(rbuf);
    free(large2);
    free(large);

    git_odb_free(odb);
    assert(bup_backend_free_calls() == 1);

    git_libgit2_shutdown();
    return 0;
}
