#include "bup_odb.h"
#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REPO_TEMPLATE "history_repoXXXXXX"
#define FILE_NAME "file.bin"
#define FILE_SIZE 20000

static const char *detect_cli(void)
{
    return "./git2";
}

static void fill_data(char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)(i % 256);
}

static void commit_file(const char *cli, const char *repo, const char *msg)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -C %s add %s", cli, repo, FILE_NAME);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "%s -C %s commit -m '%s'", cli, repo, msg);
    assert(system(cmd) == 0);
}

static int oid_in_list(const git_oid *oid, const git_oid *list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (git_oid_cmp(oid, &list[i]) == 0)
            return 1;
    return 0;
}

int main(void)
{
    git_libgit2_init();
    const char *cli = detect_cli();

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
    fill_data(data, FILE_SIZE);

    FILE *f = fopen(filepath, "wb");
    assert(f);
    fwrite(data, 1, FILE_SIZE, f);
    fclose(f);

    commit_file(cli, repo, "initial");

    /* create feature branch */
    snprintf(cmd, sizeof(cmd), "git -C %s branch feature", repo);
    assert(system(cmd) == 0);

    /* second commit on main */
    data[100] ^= 0xAA;
    f = fopen(filepath, "wb");
    assert(f);
    fwrite(data, 1, FILE_SIZE, f);
    fclose(f);
    commit_file(cli, repo, "main_change");

    /* switch to feature */
    snprintf(cmd, sizeof(cmd), "git -C %s checkout -f feature", repo);
    assert(system(cmd) == 0);

    /* commit on feature */
    data[200] ^= 0x55;
    f = fopen(filepath, "wb");
    assert(f);
    fwrite(data, 1, FILE_SIZE, f);
    fclose(f);
    commit_file(cli, repo, "feature_change");

    git_repository *gr = NULL;
    assert(git_repository_open(&gr, repo) == 0);

    git_odb_backend *backend = NULL;
    assert(bup_odb_backend_new(&backend, repo) == 0);

    git_branch_iterator *itr = NULL;
    assert(git_branch_iterator_new(&itr, gr, GIT_BRANCH_LOCAL) == 0);

    git_reference *ref = NULL;
    git_branch_t btype;
    while (git_branch_next(&ref, &btype, itr) == 0) {
        const char *bname = NULL;
        assert(git_branch_name(&bname, ref) == 0);
        printf("Branch %s\n", bname);

        git_revwalk *walk = NULL;
        assert(git_revwalk_new(&walk, gr) == 0);
        git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
        assert(git_revwalk_push_ref(walk, git_reference_name(ref)) == 0);

        git_oid *seen = NULL;
        size_t seen_count = 0;
        git_oid oid;
        while (git_revwalk_next(&oid, walk) == 0) {
            git_commit *commit = NULL;
            assert(git_commit_lookup(&commit, gr, &oid) == 0);

            git_tree *tree = NULL;
            assert(git_commit_tree(&tree, commit) == 0);

            git_tree_entry *entry = NULL;
            if (git_tree_entry_bypath(&entry, tree, FILE_NAME) == 0) {
                const git_oid *boid = git_tree_entry_id(entry);
                git_oid *chunks = NULL;
                size_t *lens = NULL;
                size_t count = bup_backend_object_chunk_count(backend, boid, &chunks, &lens);
                size_t reused = 0;
                for (size_t i = 0; i < count; i++)
                    if (oid_in_list(&chunks[i], seen, seen_count))
                        reused++;
                size_t unique = count - reused;
                char hex[GIT_OID_HEXSZ + 1];
                git_oid_tostr(hex, sizeof(hex), &oid);
                printf("  commit %.8s reused=%zu unique=%zu\n", hex, reused, unique);

                for (size_t i = 0; i < count; i++) {
                    if (!oid_in_list(&chunks[i], seen, seen_count)) {
                        seen = realloc(seen, (seen_count + 1) * sizeof(git_oid));
                        git_oid_cpy(&seen[seen_count], &chunks[i]);
                        seen_count++;
                    }
                }
                free(chunks);
                free(lens);
                git_tree_entry_free(entry);
            }
            git_tree_free(tree);
            git_commit_free(commit);
        }
        free(seen);
        git_revwalk_free(walk);
        git_reference_free(ref);
    }

    git_branch_iterator_free(itr);
    backend->free(backend);
    git_repository_free(gr);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", repo);
    system(cmd);
    free(data);

    git_libgit2_shutdown();
    return 0;
}
