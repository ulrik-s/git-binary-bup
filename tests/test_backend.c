#include "bup_odb.h"
#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <assert.h>

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
    assert(ret == GIT_ERROR);
    assert(bup_backend_write_calls() == 1);

    git_odb_free(odb);
    assert(bup_backend_free_calls() == 1);

    git_libgit2_shutdown();
    return 0;
}
