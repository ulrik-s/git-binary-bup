#include "bup_odb.h"
#include <git2.h>
#include <git2/sys/repository.h>
#include <git2/sys/odb_backend.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static int cmd_hash_object(const char *file)
{
    git_oid oid;
    int ret = git_odb_hashfile(&oid, file, GIT_OBJECT_BLOB);
    if (ret < 0)
        return ret;
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), &oid);
    printf("%s\n", hex);
    return 0;
}

static int cmd_init(const char *path)
{
    git_repository *repo = NULL;
    int ret = git_repository_init(&repo, path, 0);
    git_repository_free(repo);
    return ret;
}

static git_signature *make_signature(const char *name_env, const char *email_env)
{
    const char *name = getenv(name_env);
    const char *email = getenv(email_env);
    if (!name)
        name = "Anon";
    if (!email)
        email = "anon@example.com";
    git_signature *sig = NULL;
    if (git_signature_now(&sig, name, email) < 0)
        return NULL;
    return sig;
}

static int cmd_add(const char *repo_path, const char *pathspec)
{
    git_repository *repo = NULL;
    int ret = git_repository_open(&repo, repo_path);
    if (ret < 0)
        return ret;

    git_index *index = NULL;
    ret = git_repository_index(&index, repo);
    if (ret < 0)
        goto out_repo;

    git_odb_backend *backend = NULL;
    ret = bup_odb_backend_new(&backend, repo_path);
    if (ret < 0)
        goto out_index;

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", repo_path, pathspec);
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ret = -1;
        goto out_backend;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz);
    if (!buf) {
        fclose(f);
        ret = -1;
        goto out_backend;
    }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        ret = -1;
        goto out_backend;
    }
    fclose(f);

    git_oid oid;
    ret = backend->write(backend, &oid, buf, sz, GIT_OBJECT_BLOB);
    free(buf);
    if (ret < 0)
        goto out_backend;

    struct stat st;
    if (stat(filepath, &st) < 0) {
        ret = -1;
        goto out_backend;
    }

    git_index_entry entry = {0};
    entry.mode = GIT_FILEMODE_BLOB;
    entry.id = oid;
    entry.path = pathspec;
    entry.file_size = (git_off_t)sz;
    entry.ctime.seconds = (git_time_t)st.st_ctime;
    entry.mtime.seconds = (git_time_t)st.st_mtime;
    ret = git_index_add(index, &entry);
    if (ret == 0)
        ret = git_index_write(index);

out_backend:
    backend->free(backend);
out_index:
    git_index_free(index);
out_repo:
    git_repository_free(repo);
    return ret;
}

static int cmd_commit(const char *repo_path, const char *message)
{
    git_repository *repo = NULL;
    int ret = git_repository_open(&repo, repo_path);
    if (ret < 0)
        return ret;

    git_index *index = NULL;
    ret = git_repository_index(&index, repo);
    if (ret < 0)
        goto out;

    git_oid tree_oid;
    ret = git_index_write_tree(&tree_oid, index);
    if (ret < 0)
        goto out_index;
    ret = git_index_write(index);
    if (ret < 0)
        goto out_index;

    git_tree *tree = NULL;
    ret = git_tree_lookup(&tree, repo, &tree_oid);
    if (ret < 0)
        goto out_index;

    git_oid parent_oid;
    git_commit *parent = NULL;
    if (git_reference_name_to_id(&parent_oid, repo, "HEAD") == 0)
        git_commit_lookup(&parent, repo, &parent_oid);

    git_signature *author = make_signature("GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL");
    git_signature *committer = make_signature("GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL");
    if (!author || !committer) {
        ret = -1;
        goto out_all;
    }

    git_oid commit_oid;
    if (parent)
        ret = git_commit_create_v(&commit_oid, repo, "HEAD", author, committer,
                                  NULL, message, tree, 1, parent);
    else
        ret = git_commit_create_v(&commit_oid, repo, "HEAD", author, committer,
                                  NULL, message, tree, 0);

    if (ret < 0) {
        const git_error *e = git_error_last();
        if (e && e->message)
            fprintf(stderr, "commit error: %s\n", e->message);
    }

out_all:
    git_signature_free(author);
    git_signature_free(committer);
    if (parent)
        git_commit_free(parent);
    git_tree_free(tree);

out_index:
    git_index_free(index);
out:
    git_repository_free(repo);
    return ret;
}

int main(int argc, char **argv)
{
    git_libgit2_init();

    const char *repo_path = NULL;
    int arg = 1;
    if (arg < argc && strcmp(argv[arg], "-C") == 0) {
        repo_path = argv[arg + 1];
        arg += 2;
    }

    if (arg >= argc) {
        fprintf(stderr, "Usage: git2 [-C repo] <command> [args]\n");
        return 1;
    }

    const char *cmd = argv[arg++];
    int ret = 0;

    if (strcmp(cmd, "hash-object") == 0) {
        if (arg >= argc) {
            fprintf(stderr, "hash-object requires a file\n");
            ret = 1;
        } else {
            ret = cmd_hash_object(argv[arg]);
        }
    } else if (strcmp(cmd, "init") == 0) {
        if (arg >= argc) {
            fprintf(stderr, "init requires a path\n");
            ret = 1;
        } else {
            ret = cmd_init(argv[arg]);
        }
    } else if (strcmp(cmd, "add") == 0) {
        if (!repo_path || arg >= argc) {
            fprintf(stderr, "add requires -C <repo> and a pathspec\n");
            ret = 1;
        } else {
            ret = cmd_add(repo_path, argv[arg]);
        }
    } else if (strcmp(cmd, "commit") == 0) {
        if (!repo_path) {
            fprintf(stderr, "commit requires -C <repo>\n");
            ret = 1;
        } else {
            const char *msg = NULL;
            if (arg < argc && strcmp(argv[arg], "-m") == 0 && arg + 1 < argc) {
                msg = argv[arg + 1];
            }
            if (!msg) {
                fprintf(stderr, "commit requires -m <message>\n");
                ret = 1;
            } else {
                ret = cmd_commit(repo_path, msg);
            }
        }
    } else {
        fprintf(stderr, "Unknown command %s\n", cmd);
        ret = 1;
    }

    git_libgit2_shutdown();
    return ret;
}

