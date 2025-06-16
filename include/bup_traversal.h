#ifndef BUP_TRAVERSAL_H
#define BUP_TRAVERSAL_H

#include <git2.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    git_oid *oids;
    size_t count;
    size_t cap;
} bup_oid_list;

void bup_oid_list_clear(bup_oid_list *list);
int bup_oid_list_add(bup_oid_list *list, const git_oid *oid);
int bup_collect_reachable_oids(git_repository *repo, bup_oid_list *list);

#ifdef __cplusplus
}
#endif

#endif /* BUP_TRAVERSAL_H */
