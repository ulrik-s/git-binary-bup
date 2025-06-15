#include "bup_odb.h"
#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define TEMP_FILE "cli_blob.tmp"
#define REPO_TEMPLATE "cli_repoXXXXXX"
#define LARGE_SIZE 50000
#define FLIP1 100
#define FLIP2 40000
#define ALPHABET_LEN 26

static const char *detect_cli(void)
{
    FILE *p = popen("which git2 2>/dev/null", "r");
    if (p) {
        int ch = fgetc(p);
        pclose(p);
        if (ch != EOF)
            return "git2";
    }
    return "git";
}

static void fill_pattern(char *buf, size_t len)
{
    srand(1234);
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)(rand() % 256);
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

int main(void)
{
    git_libgit2_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *cli = detect_cli();
    git_odb_backend *backend = NULL;
    assert(bup_odb_backend_new(&backend, NULL) == 0);

    char *data = malloc(LARGE_SIZE);
    fill_pattern(data, LARGE_SIZE);
    git_oid oid1;
    int w_before = bup_backend_write_calls();
    assert(backend->write(backend, &oid1, data, LARGE_SIZE, GIT_OBJECT_BLOB) == 0);
    assert(bup_backend_write_calls() == w_before + 1);

    size_t chunks1 = bup_backend_object_chunks(backend, &oid1, NULL, NULL);
    assert(chunks1 > 1);

    FILE *f = fopen(TEMP_FILE, "wb");
    assert(f);
    fwrite(data, 1, LARGE_SIZE, f);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s hash-object %s", cli, TEMP_FILE);
    FILE *p = popen(cmd, "r");
    assert(p);
    char out[64];
    assert(fgets(out, sizeof(out), p));
    pclose(p);
    if (out[strlen(out) - 1] == '\n')
        out[strlen(out) - 1] = '\0';
    git_oid cli_oid;
    assert(git_oid_fromstr(&cli_oid, out) == 0);
    assert(git_oid_cmp(&oid1, &cli_oid) == 0);

    char repo_tmp[] = REPO_TEMPLATE;
    char *repo = mkdtemp(repo_tmp);
    assert(repo);
    snprintf(cmd, sizeof(cmd), "%s init %s", cli, repo);
    assert(system(cmd) == 0);

    setenv("GIT_AUTHOR_NAME", "Tester", 1);
    setenv("GIT_AUTHOR_EMAIL", "tester@example.com", 1);
    setenv("GIT_COMMITTER_NAME", "Tester", 1);
    setenv("GIT_COMMITTER_EMAIL", "tester@example.com", 1);

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/file.bin", repo);
    f = fopen(filepath, "wb");
    fwrite(data, 1, LARGE_SIZE, f);
    fclose(f);

    snprintf(cmd, sizeof(cmd), "%s -C %s add file.bin", cli, repo);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "%s -C %s commit -m first", cli, repo);
    assert(system(cmd) == 0);

    long long git_first = dir_size(repo);
    size_t bup_first_size = bup_backend_total_size();
    size_t bup_first_chunks = bup_backend_chunk_count();
    printf("git_first=%lld bup_first_size=%zu chunks=%zu\n", git_first, bup_first_size, bup_first_chunks);

    char *data2 = malloc(LARGE_SIZE);
    memcpy(data2, data, LARGE_SIZE);
    data2[FLIP1] ^= 0x55;
    data2[FLIP2] ^= 0x55;
    git_oid oid2;
    w_before = bup_backend_write_calls();
    assert(backend->write(backend, &oid2, data2, LARGE_SIZE, GIT_OBJECT_BLOB) == 0);
    assert(bup_backend_write_calls() == w_before + 1);

    size_t bup_second_size = bup_backend_total_size();
    size_t bup_second_chunks = bup_backend_chunk_count();
    printf("git_second_before=%lld bup_second_size=%zu chunks=%zu\n", git_first, bup_second_size, bup_second_chunks);

    f = fopen(filepath, "wb");
    fwrite(data2, 1, LARGE_SIZE, f);
    fclose(f);

    snprintf(cmd, sizeof(cmd), "%s -C %s add file.bin", cli, repo);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "%s -C %s commit -m second", cli, repo);
    assert(system(cmd) == 0);

    long long git_second = dir_size(repo);
    printf("git_second=%lld\n", git_second);

    assert(bup_second_chunks <= bup_first_chunks + 3);
    printf("growth git=%lld backend=%zu\n", git_second - git_first,
           bup_second_size - bup_first_size);
    assert(git_second > git_first);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", repo);
    system(cmd);
    remove(TEMP_FILE);
    backend->free(backend);
    free(data);
    free(data2);
    git_libgit2_shutdown();
    return 0;
}
