#include "bup_traversal.h"
#include "chunk_utils.h"
#include <git2.h>
#include <git2/odb.h>
#include <stdlib.h>
#include <string.h>

void bup_oid_list_clear(bup_oid_list *list)
{
    free(list->oids);
    list->oids = NULL;
    list->count = 0;
    list->cap = 0;
}

int bup_oid_list_add(bup_oid_list *list, const git_oid *oid)
{
    for (size_t i = 0; i < list->count; i++)
        if (git_oid_cmp(&list->oids[i], oid) == 0)
            return 0;
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 32;
        git_oid *tmp = realloc(list->oids, new_cap * sizeof(git_oid));
        if (!tmp)
            return -1;
        list->oids = tmp;
        list->cap = new_cap;
    }
    git_oid_cpy(&list->oids[list->count++], oid);
    return 0;
}

static int add_blob_chunks(git_repository *repo, const git_oid *oid,
                           bup_oid_list *list)
{
    git_odb *odb = NULL;
    if (git_repository_odb(&odb, repo) < 0)
        return -1;
    git_odb_object *obj = NULL;
    int ret = git_odb_read(&obj, odb, oid);
    git_odb_free(odb);
    if (ret < 0)
        return -1;

    const char *data = git_odb_object_data(obj);
    size_t size = git_odb_object_size(obj);
    git_oid *chunks = NULL;
    size_t *lens = NULL;
    size_t count = 0;
    if (parse_chunk_list(data, size, &chunks, &lens, &count) == 0 && count > 0) {
        for (size_t i = 0; i < count; i++) {
            if (bup_oid_list_add(list, &chunks[i]) < 0) {
                free(chunks);
                free(lens);
                git_odb_object_free(obj);
                return -1;
            }
        }
    }
    free(chunks);
    free(lens);
    git_odb_object_free(obj);
    return 0;
}

static int collect_tree_oids_bup(git_repository *repo, git_tree *tree,
                                 bup_oid_list *list)
{
    size_t count = git_tree_entrycount(tree);
    for (size_t i = 0; i < count; i++) {
        const git_tree_entry *entry = git_tree_entry_byindex(tree, i);
        const git_oid *oid = git_tree_entry_id(entry);
        if (bup_oid_list_add(list, oid) < 0)
            return -1;
        if (git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
            git_object *obj = NULL;
            if (git_tree_entry_to_object(&obj, repo, entry) < 0)
                return -1;
            int ret = collect_tree_oids_bup(repo, (git_tree *)obj, list);
            git_object_free(obj);
            if (ret < 0)
                return ret;
        } else if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
            if (add_blob_chunks(repo, oid, list) < 0)
                return -1;
        }
    }
    return 0;
}

int bup_collect_reachable_oids(git_repository *repo, bup_oid_list *list)
{
    git_revwalk *walk = NULL;
    int ret = git_revwalk_new(&walk, repo);
    if (ret < 0)
        return ret;
    git_revwalk_push_head(walk);

    git_oid oid;
    while ((ret = git_revwalk_next(&oid, walk)) == 0) {
        if (bup_oid_list_add(list, &oid) < 0)
            break;
        git_commit *commit = NULL;
        if (git_commit_lookup(&commit, repo, &oid) < 0) {
            ret = -1;
            break;
        }
        git_tree *tree = NULL;
        if (git_commit_tree(&tree, commit) < 0) {
            git_commit_free(commit);
            ret = -1;
            break;
        }
        ret = collect_tree_oids_bup(repo, tree, list);
        git_tree_free(tree);
        git_commit_free(commit);
        if (ret < 0)
            break;
    }

    git_revwalk_free(walk);
    if (ret == GIT_ITEROVER)
        ret = 0;
    return ret;
}

