#ifndef CTX_REGEX_H
#define CTX_REGEX_H

#include <stdbool.h>
#include <stddef.h>

enum { CTX_MATCH = 256, CTX_SPLIT = 257 };

typedef struct ctx_state_t {
    int c;
    struct ctx_state_t *out;
    struct ctx_state_t *out1;
    int lastlist;
    int id;
} ctx_state_t;

typedef struct {
    char *postfix;
    ctx_state_t *start;
    ctx_state_t *match;
    int nstate;
    char err[128];
} ctx_regex_t;

bool ctx_regex_compile(const char *pattern, ctx_regex_t *out);
bool ctx_regex_match(const ctx_regex_t *re, const char *s);
void ctx_regex_dump(const ctx_regex_t *re);
void ctx_regex_free(ctx_regex_t *re);

#endif
