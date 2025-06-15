#include "bup_odb.h"
#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define BLOB_SIZE 100000
#define RANDOM_SEED 42
#define FLIP_MASK 0x55
#define NUM_MODS 5

static char *generate_random_blob(size_t size, unsigned int seed)
{
    char *buf = malloc(size);
    assert(buf != NULL);
    srand(seed);
    for (size_t i = 0; i < size; i++)
        buf[i] = (char)(rand() % 256);
    return buf;
}

static size_t store_blob_and_get_chunks(git_odb_backend *backend,
                                        const void *data, size_t size,
                                        git_oid *oid, const void ***chunks,
                                        size_t **lens)
{
    assert(backend->write(backend, oid, data, size, GIT_OBJECT_BLOB) == 0);
    return bup_backend_object_chunks(backend, oid, chunks, lens);
}

static int chunk_reused(const void *chunk, const void **old_chunks,
                        size_t old_count)
{
    for (size_t j = 0; j < old_count; j++)
        if (chunk == old_chunks[j])
            return 1;
    return 0;
}

static size_t count_reused_chunks(const void **new_chunks, size_t new_count,
                                  const void **old_chunks, size_t old_count)
{
    size_t reused = 0;
    for (size_t i = 0; i < new_count; i++)
        if (chunk_reused(new_chunks[i], old_chunks, old_count))
            reused++;
    return reused;
}

static size_t find_chunk_index_for_offset(size_t offset, const size_t *lens,
                                          size_t count)
{
    size_t cum = 0;
    for (size_t i = 0; i < count; i++) {
        cum += lens[i];
        if (offset < cum)
            return i;
    }
    return count;
}

static int modification_triggers_new_chunk(size_t offset,
                                           const void **new_chunks,
                                           const size_t *new_lens,
                                           size_t new_count,
                                           const void **old_chunks,
                                           size_t old_count)
{
    size_t idx = find_chunk_index_for_offset(offset, new_lens, new_count);
    if (idx >= new_count)
        return 0;
    return !chunk_reused(new_chunks[idx], old_chunks, old_count);
}

int main(void)
{
    git_libgit2_init();

    git_odb_backend *backend = NULL;
    assert(bup_odb_backend_new(&backend, NULL) == 0);

    char *data = generate_random_blob(BLOB_SIZE, RANDOM_SEED);
    git_oid oid1;
    const void **chunks1 = NULL;
    size_t *lens1 = NULL;
    size_t n1 =
        store_blob_and_get_chunks(backend, data, BLOB_SIZE, &oid1, &chunks1,
                                 &lens1);
    assert(n1 > NUM_MODS);

    char *data2 = malloc(BLOB_SIZE);
    memcpy(data2, data, BLOB_SIZE);
    size_t boundary_first = lens1[0];
    size_t boundary_last = BLOB_SIZE - lens1[n1 - 1];
    size_t mods[NUM_MODS] = {0, boundary_first, BLOB_SIZE / 2,
                             boundary_last, BLOB_SIZE - 1};
    for (size_t i = 0; i < NUM_MODS; i++)
        data2[mods[i]] ^= FLIP_MASK;

    git_oid oid2;
    const void **chunks2 = NULL;
    size_t *lens2 = NULL;
    size_t n2 =
        store_blob_and_get_chunks(backend, data2, BLOB_SIZE, &oid2, &chunks2,
                                 &lens2);
    assert(n2 >= n1);

    size_t reused = count_reused_chunks(chunks2, n2, chunks1, n1);
    assert(reused >= n1 - NUM_MODS);

    for (size_t i = 0; i < NUM_MODS; i++)
        assert(modification_triggers_new_chunk(mods[i], chunks2, lens2, n2,
                                               chunks1, n1));

    free(chunks1);
    free(lens1);
    free(chunks2);
    free(lens2);
    free(data);
    free(data2);

    backend->free(backend);
    git_libgit2_shutdown();
    return 0;
}
