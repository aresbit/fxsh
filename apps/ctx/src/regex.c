#include "regex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ctx_ptrlist_t {
    ctx_state_t **s;
    struct ctx_ptrlist_t *next;
} ctx_ptrlist_t;

typedef struct {
    ctx_state_t *start;
    ctx_ptrlist_t *out;
} ctx_frag_t;

typedef struct {
    ctx_state_t **s;
    int n;
} ctx_list_t;

typedef struct {
    int nalt;
    int natom;
} ctx_paren_t;

static ctx_state_t *g_states = NULL;
static int g_nstate = 0;
static int g_cap = 0;
static int g_listid = 0;

static void set_err(char *err, const char *msg) {
    if (!err) return;
    snprintf(err, 128, "%s", msg);
}

static void reset_pool(void) {
    free(g_states);
    g_states = NULL;
    g_nstate = 0;
    g_cap = 0;
    g_listid = 0;
}

static ctx_state_t *new_state(int c, ctx_state_t *out, ctx_state_t *out1) {
    if (g_nstate >= g_cap) {
        int ncap = g_cap ? g_cap * 2 : 256;
        ctx_state_t *ns = (ctx_state_t *)realloc(g_states, (size_t)ncap * sizeof(ctx_state_t));
        if (!ns) return NULL;
        g_states = ns;
        g_cap = ncap;
    }
    ctx_state_t *s = &g_states[g_nstate];
    s->c = c;
    s->out = out;
    s->out1 = out1;
    s->lastlist = 0;
    s->id = g_nstate;
    g_nstate++;
    return s;
}

static ctx_ptrlist_t *list1(ctx_state_t **outp) {
    ctx_ptrlist_t *l = (ctx_ptrlist_t *)malloc(sizeof(ctx_ptrlist_t));
    if (!l) return NULL;
    l->s = outp;
    l->next = NULL;
    return l;
}

static ctx_ptrlist_t *append_ptrlist(ctx_ptrlist_t *l1, ctx_ptrlist_t *l2) {
    if (!l1) return l2;
    ctx_ptrlist_t *old = l1;
    while (old->next) old = old->next;
    old->next = l2;
    return l1;
}

static void patch(ctx_ptrlist_t *l, ctx_state_t *s) {
    while (l) {
        *(l->s) = s;
        l = l->next;
    }
}

static void free_ptrlist(ctx_ptrlist_t *l) {
    while (l) {
        ctx_ptrlist_t *n = l->next;
        free(l);
        l = n;
    }
}

static char *re2post(const char *re, char *err) {
    size_t len = strlen(re);
    size_t cap = len * 2 + 16;
    char *buf = (char *)malloc(cap);
    ctx_paren_t paren[128];
    int nparen = 0;
    int nalt = 0;
    int natom = 0;
    size_t j = 0;

    if (!buf) {
        set_err(err, "oom in re2post");
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        char c = re[i];
        switch (c) {
            case '(':
                if (natom > 1) {
                    natom--;
                    buf[j++] = '.';
                }
                if (nparen >= (int)(sizeof(paren) / sizeof(paren[0]))) {
                    free(buf);
                    set_err(err, "too many parens");
                    return NULL;
                }
                paren[nparen].nalt = nalt;
                paren[nparen].natom = natom;
                nparen++;
                nalt = 0;
                natom = 0;
                break;
            case '|':
                if (natom == 0) {
                    free(buf);
                    set_err(err, "bad alternation");
                    return NULL;
                }
                while (--natom > 0) buf[j++] = '.';
                nalt++;
                break;
            case ')':
                if (nparen == 0) {
                    free(buf);
                    set_err(err, "unmatched )");
                    return NULL;
                }
                if (natom == 0) {
                    free(buf);
                    set_err(err, "empty ()");
                    return NULL;
                }
                while (--natom > 0) buf[j++] = '.';
                while (nalt-- > 0) buf[j++] = '|';
                nparen--;
                nalt = paren[nparen].nalt;
                natom = paren[nparen].natom;
                natom++;
                break;
            case '*':
            case '+':
            case '?':
                if (natom == 0) {
                    free(buf);
                    set_err(err, "closure without atom");
                    return NULL;
                }
                buf[j++] = c;
                break;
            case '\\':
                i++;
                if (i >= len) {
                    free(buf);
                    set_err(err, "dangling escape");
                    return NULL;
                }
                if (natom > 1) {
                    natom--;
                    buf[j++] = '.';
                }
                buf[j++] = re[i];
                natom++;
                break;
            default:
                if (natom > 1) {
                    natom--;
                    buf[j++] = '.';
                }
                buf[j++] = c;
                natom++;
                break;
        }
    }

    if (nparen != 0) {
        free(buf);
        set_err(err, "unmatched (");
        return NULL;
    }

    while (--natom > 0) buf[j++] = '.';
    while (nalt-- > 0) buf[j++] = '|';
    buf[j] = '\0';
    return buf;
}

static bool post2nfa(const char *post, ctx_state_t **start, ctx_state_t **match, char *err) {
    ctx_frag_t st[2048];
    int nst = 0;

    for (size_t i = 0; post[i] != '\0'; i++) {
        char c = post[i];
        ctx_frag_t e1, e2, e;
        ctx_state_t *s;

        switch (c) {
            case '.':
                if (nst < 2) {
                    set_err(err, "bad postfix concat");
                    return false;
                }
                e2 = st[--nst];
                e1 = st[--nst];
                patch(e1.out, e2.start);
                e.start = e1.start;
                e.out = e2.out;
                st[nst++] = e;
                break;
            case '|':
                if (nst < 2) {
                    set_err(err, "bad postfix alternation");
                    return false;
                }
                e2 = st[--nst];
                e1 = st[--nst];
                s = new_state(CTX_SPLIT, e1.start, e2.start);
                if (!s) {
                    set_err(err, "oom state");
                    return false;
                }
                e.start = s;
                e.out = append_ptrlist(e1.out, e2.out);
                st[nst++] = e;
                break;
            case '?':
                if (nst < 1) {
                    set_err(err, "bad postfix ?");
                    return false;
                }
                e = st[--nst];
                s = new_state(CTX_SPLIT, e.start, NULL);
                if (!s) {
                    set_err(err, "oom state");
                    return false;
                }
                e.start = s;
                e.out = append_ptrlist(e.out, list1(&s->out1));
                st[nst++] = e;
                break;
            case '*':
                if (nst < 1) {
                    set_err(err, "bad postfix *");
                    return false;
                }
                e = st[--nst];
                s = new_state(CTX_SPLIT, e.start, NULL);
                if (!s) {
                    set_err(err, "oom state");
                    return false;
                }
                patch(e.out, s);
                e.start = s;
                e.out = list1(&s->out1);
                st[nst++] = e;
                break;
            case '+':
                if (nst < 1) {
                    set_err(err, "bad postfix +");
                    return false;
                }
                e = st[--nst];
                s = new_state(CTX_SPLIT, e.start, NULL);
                if (!s) {
                    set_err(err, "oom state");
                    return false;
                }
                patch(e.out, s);
                e.out = list1(&s->out1);
                st[nst++] = e;
                break;
            default:
                s = new_state((unsigned char)c, NULL, NULL);
                if (!s) {
                    set_err(err, "oom state");
                    return false;
                }
                e.start = s;
                e.out = list1(&s->out);
                st[nst++] = e;
                break;
        }
    }

    if (nst != 1) {
        set_err(err, "bad postfix terminal stack");
        return false;
    }

    ctx_state_t *m = new_state(CTX_MATCH, NULL, NULL);
    if (!m) {
        set_err(err, "oom match state");
        return false;
    }
    patch(st[0].out, m);
    free_ptrlist(st[0].out);
    *start = st[0].start;
    *match = m;
    return true;
}

static void addstate(ctx_list_t *l, ctx_state_t *s) {
    if (!s || s->lastlist == g_listid) return;
    s->lastlist = g_listid;
    if (s->c == CTX_SPLIT) {
        addstate(l, s->out);
        addstate(l, s->out1);
        return;
    }
    l->s[l->n++] = s;
}

static void startlist(ctx_state_t *start, ctx_list_t *l) {
    l->n = 0;
    g_listid++;
    addstate(l, start);
}

static void step(ctx_list_t *clist, int c, ctx_list_t *nlist) {
    nlist->n = 0;
    g_listid++;
    for (int i = 0; i < clist->n; i++) {
        ctx_state_t *s = clist->s[i];
        if (s->c == c || s->c == '.') addstate(nlist, s->out);
    }
}

static bool ismatch(ctx_list_t *l) {
    for (int i = 0; i < l->n; i++) {
        if (l->s[i]->c == CTX_MATCH) return true;
    }
    return false;
}

bool ctx_regex_compile(const char *pattern, ctx_regex_t *out) {
    if (!pattern || !out) return false;
    memset(out, 0, sizeof(*out));
    reset_pool();

    out->postfix = re2post(pattern, out->err);
    if (!out->postfix) return false;

    if (!post2nfa(out->postfix, &out->start, &out->match, out->err)) {
        free(out->postfix);
        out->postfix = NULL;
        reset_pool();
        return false;
    }

    out->nstate = g_nstate;
    return true;
}

bool ctx_regex_match(const ctx_regex_t *re, const char *s) {
    if (!re || !re->start || !s) return false;
    ctx_state_t *buf1[4096];
    ctx_state_t *buf2[4096];
    ctx_list_t l1 = {buf1, 0};
    ctx_list_t l2 = {buf2, 0};
    ctx_list_t *clist = &l1;
    ctx_list_t *nlist = &l2;

    startlist(re->start, clist);
    for (size_t i = 0; s[i] != '\0'; i++) {
        step(clist, (unsigned char)s[i], nlist);
        ctx_list_t *tmp = clist;
        clist = nlist;
        nlist = tmp;
    }
    return ismatch(clist);
}

void ctx_regex_dump(const ctx_regex_t *re) {
    if (!re || !g_states) return;
    printf("postfix: %s\n", re->postfix ? re->postfix : "");
    printf("states: %d\n", re->nstate);
    for (int i = 0; i < re->nstate; i++) {
        ctx_state_t *s = &g_states[i];
        const char *name;
        char lit[8];
        if (s->c == CTX_MATCH) {
            name = "MATCH";
        } else if (s->c == CTX_SPLIT) {
            name = "SPLIT";
        } else if (s->c == '.') {
            name = "DOT";
        } else {
            snprintf(lit, sizeof(lit), "%c", (char)s->c);
            name = lit;
        }
        printf("[%d] %-5s out=%d out1=%d\n", s->id, name,
               s->out ? s->out->id : -1,
               s->out1 ? s->out1->id : -1);
    }
}

void ctx_regex_free(ctx_regex_t *re) {
    if (!re) return;
    free(re->postfix);
    re->postfix = NULL;
    re->start = NULL;
    re->match = NULL;
    re->nstate = 0;
    re->err[0] = '\0';
    reset_pool();
}
