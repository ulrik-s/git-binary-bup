#include "bup_odb.h"
#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define FILE_SIZE 100000
#define CHANGE_BLOCK 100
#define NUM_CHANGES 100
#define REPO_TEMPLATE "many_repoXXXXXX"
#define FILE_NAME "file.bin"
#define MAX_NEW_CHUNKS 3

static const char *detect_cli(void)
{
    return "./git2";
}

static void fill_random(char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)(rand() % 256);
}

static size_t store_blob_get_chunks(git_odb_backend *backend, const void *data,
                                    size_t len, git_oid *oid, git_oid **chunks,
                                    size_t **lens)
{
    assert(backend->write(backend, oid, data, len, GIT_OBJECT_BLOB) == 0);
    return bup_backend_object_chunk_count(backend, oid, chunks, lens);
}

static size_t count_reused(const git_oid *new_chunks, size_t new_count,
                           const git_oid *old_chunks, size_t old_count)
{
    size_t reused = 0;
    for (size_t i = 0; i < new_count; i++) {
        for (size_t j = 0; j < old_count; j++) {
            if (git_oid_cmp(&new_chunks[i], &old_chunks[j]) == 0) {
                reused++;
                break;
            }
        }
    }
    return reused;
}

static long long dir_size(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;
    long long sum = S_ISDIR(st.st_mode) ? 0 : st.st_size;
    if (!S_ISDIR(st.st_mode))
        return sum;
    DIR *d = opendir(path);
    if (!d)
        return sum;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "%s/%s", path, ent->d_name);
        sum += dir_size(buf);
    }
    closedir(d);
    return sum;
}

static void verify_head_blob(const char *cli, const char *repo, const char *path,
                             const char *data, size_t len)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -C %s show HEAD:%s", cli, repo, path);
    FILE *p = popen(cmd, "r");
    assert(p);
    char *buf = malloc(len);
    size_t r = fread(buf, 1, len, p);
    assert(r == len);
    int c = fgetc(p);
    assert(c == EOF);
    pclose(p);
    assert(memcmp(buf, data, len) == 0);
    free(buf);
}

static void commit_file(const char *cli, const char *repo, const char *msg)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -C %s add %s", cli, repo, FILE_NAME);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "%s -C %s commit -m '%s'", cli, repo, msg);
    assert(system(cmd) == 0);
}

int main(void)
{
    git_libgit2_init();
    srand(1234);

    const char *cli = detect_cli();
    git_odb_backend *backend = NULL;
    assert(bup_odb_backend_new(&backend, NULL) == 0);

    char repo_tmp[] = REPO_TEMPLATE;
    char *repo = mkdtemp(repo_tmp);
    assert(repo);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s init %s", cli, repo);
    assert(system(cmd) == 0);

    setenv("GIT_AUTHOR_NAME", "Tester", 1);
    setenv("GIT_AUTHOR_EMAIL", "tester@example.com", 1);
    setenv("GIT_COMMITTER_NAME", "Tester", 1);
    setenv("GIT_COMMITTER_EMAIL", "tester@example.com", 1);

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", repo, FILE_NAME);

    char *data = malloc(FILE_SIZE);
    fill_random(data, FILE_SIZE);

    FILE *f = fopen(filepath, "wb");
    assert(f);
    fwrite(data, 1, FILE_SIZE, f);
    fclose(f);

    commit_file(cli, repo, "initial");
    verify_head_blob(cli, repo, FILE_NAME, data, FILE_SIZE);

    git_oid *chunks = NULL;
    size_t *lens = NULL;
    git_oid oid;
    size_t chunk_count = store_blob_get_chunks(backend, data, FILE_SIZE, &oid,
                                              &chunks, &lens);
    size_t prev_total_chunks = bup_backend_chunk_count();
    long long size_git = dir_size(repo);
    printf("initial reused=%zu unique=%zu git_size=%lld\n", chunk_count, 0UL,
           size_git);

    for (int i = 0; i < NUM_CHANGES; i++) {
        size_t off = rand() % (FILE_SIZE - CHANGE_BLOCK + 1);
        fill_random(data + off, CHANGE_BLOCK);

        f = fopen(filepath, "wb");
        assert(f);
        fwrite(data, 1, FILE_SIZE, f);
        fclose(f);

        char msg[64];
        snprintf(msg, sizeof(msg), "change %d", i + 1);
        commit_file(cli, repo, msg);
        verify_head_blob(cli, repo, FILE_NAME, data, FILE_SIZE);

        git_oid *new_chunks = NULL;
        size_t *new_lens = NULL;
        git_oid new_oid;
        size_t new_count =
            store_blob_get_chunks(backend, data, FILE_SIZE, &new_oid,
                                  &new_chunks, &new_lens);
        size_t reused = count_reused(new_chunks, new_count, chunks, chunk_count);
        size_t unique = new_count - reused;
        size_git = dir_size(repo);
        printf("iter=%d reused=%zu unique=%zu git_size=%lld\n", i + 1, reused,
               unique, size_git);

        assert(unique <= MAX_NEW_CHUNKS);
        assert(bup_backend_chunk_count() <= prev_total_chunks + MAX_NEW_CHUNKS);
        prev_total_chunks = bup_backend_chunk_count();

        free(chunks);
        free(lens);
        chunks = new_chunks;
        lens = new_lens;
        chunk_count = new_count;
    }

    free(chunks);
    free(lens);
    free(data);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", repo);
    system(cmd);

    backend->free(backend);
    git_libgit2_shutdown();
    return 0;
}
