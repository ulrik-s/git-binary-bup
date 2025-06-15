#ifndef BUP_ODB_H
#define BUP_ODB_H

#include <git2.h>

#ifdef __cplusplus
extern "C" {
#endif

int bup_odb_backend_new(git_odb_backend **out, const char *path);

/* Test helpers to verify backend callbacks are invoked */
int bup_backend_read_calls(void);
int bup_backend_write_calls(void);
int bup_backend_free_calls(void);

#ifdef __cplusplus
}
#endif

#endif /* BUP_ODB_H */
