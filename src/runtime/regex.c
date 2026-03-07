#include "fxsh.h"

#include <stdlib.h>
#include <string.h>

typedef struct regex_state regex_state_t;
typedef struct regex_ptrlist regex_ptrlist_t;

struct regex_state {
    int c;
    regex_state_t *out;
    regex_state_t *out1;
    int lastlist;
};

struct regex_ptrlist {
    regex_state_t **s;
    regex_ptrlist_t *next;
};

typedef struct {
    regex_state_t *start;
    regex_ptrlist_t *out;
} regex_frag_t;

typedef struct {
    regex_state_t **s;
    int n;
} regex_list_t;

typedef struct {
    regex_state_t *start;
    int nstate;
} regex_prog_t;

#define RX_MATCH  256
#define RX_SPLIT  257
#define RX_ANY    258
#define RX_CONCAT 1

static regex_ptrlist_t *rx_list1(regex_state_t **outp) {
    regex_ptrlist_t *l = (regex_ptrlist_t *)malloc(sizeof(regex_ptrlist_t));
    if (!l)
        return NULL;
    l->s = outp;
    l->next = NULL;
    return l;
}

static regex_ptrlist_t *rx_append(regex_ptrlist_t *l1, regex_ptrlist_t *l2) {
    if (!l1)
        return l2;
    regex_ptrlist_t *old = l1;
    while (old->next)
        old = old->next;
    old->next = l2;
    return l1;
}

static void rx_patch(regex_ptrlist_t *l, regex_state_t *s) {
    while (l) {
        *(l->s) = s;
        l = l->next;
    }
}

static regex_state_t *rx_state_new(int c, regex_state_t *out, regex_state_t *out1, int *nstate) {
    regex_state_t *s = (regex_state_t *)calloc(1, sizeof(regex_state_t));
    if (!s)
        return NULL;
    s->c = c;
    s->out = out;
    s->out1 = out1;
    s->lastlist = 0;
    (*nstate)++;
    return s;
}

static sp_str_t rx_to_postfix(sp_str_t pat) {
    if (!pat.data)
        return (sp_str_t){.data = "", .len = 0};
    size_t maxn = (size_t)pat.len * 4 + 8;
    char *dst = (char *)calloc(maxn, 1);
    if (!dst)
        return (sp_str_t){.data = "", .len = 0};

    typedef struct {
        int nalt;
        int natom;
    } par_t;
    par_t par[128];
    int npar = 0;
    int nalt = 0;
    int natom = 0;
    size_t di = 0;

    for (u32 i = 0; i < pat.len; i++) {
        char ch = pat.data[i];
        bool escaped = false;
        if (ch == '\\' && i + 1 < pat.len) {
            escaped = true;
            ch = pat.data[++i];
        }

        if (!escaped && ch == '(') {
            if (natom > 1) {
                natom--;
                dst[di++] = RX_CONCAT;
            }
            if (npar >= 128)
                return (sp_str_t){.data = "", .len = 0};
            par[npar].nalt = nalt;
            par[npar].natom = natom;
            npar++;
            nalt = 0;
            natom = 0;
            continue;
        }
        if (!escaped && ch == ')') {
            if (npar <= 0 || natom == 0)
                return (sp_str_t){.data = "", .len = 0};
            while (--natom > 0)
                dst[di++] = RX_CONCAT;
            while (nalt-- > 0)
                dst[di++] = '|';
            npar--;
            nalt = par[npar].nalt;
            natom = par[npar].natom;
            natom++;
            continue;
        }
        if (!escaped && ch == '|') {
            if (natom == 0)
                return (sp_str_t){.data = "", .len = 0};
            while (--natom > 0)
                dst[di++] = RX_CONCAT;
            nalt++;
            continue;
        }
        if (!escaped && (ch == '*' || ch == '+' || ch == '?')) {
            if (natom == 0)
                return (sp_str_t){.data = "", .len = 0};
            dst[di++] = ch;
            continue;
        }

        if (natom > 1) {
            natom--;
            dst[di++] = RX_CONCAT;
        }
        if (escaped || ch == '|' || ch == '*' || ch == '+' || ch == '?' || ch == '(' || ch == ')' ||
            ch == '\\') {
            dst[di++] = '\\';
        }
        dst[di++] = ch;
        natom++;
    }

    if (npar != 0)
        return (sp_str_t){.data = "", .len = 0};
    while (--natom > 0)
        dst[di++] = RX_CONCAT;
    while (nalt-- > 0)
        dst[di++] = '|';

    dst[di] = '\0';
    return (sp_str_t){.data = dst, .len = (u32)di};
}

static regex_prog_t rx_compile(sp_str_t pat) {
    regex_prog_t prog = {.start = NULL, .nstate = 0};
    sp_str_t post = rx_to_postfix(pat);
    if (!post.data || post.len == 0)
        return prog;

    regex_frag_t st[2048];
    int nst = 0;
    for (u32 i = 0; i < post.len; i++) {
        char c = post.data[i];
        if (c == '\\') {
            if (i + 1 >= post.len)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            char lit = post.data[++i];
            regex_state_t *s = rx_state_new((unsigned char)lit, NULL, NULL, &prog.nstate);
            if (!s)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            st[nst++] = (regex_frag_t){.start = s, .out = rx_list1(&s->out)};
            continue;
        }
        if (c == RX_CONCAT) {
            if (nst < 2)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            regex_frag_t e2 = st[--nst];
            regex_frag_t e1 = st[--nst];
            rx_patch(e1.out, e2.start);
            st[nst++] = (regex_frag_t){.start = e1.start, .out = e2.out};
            continue;
        }
        if (c == '|') {
            if (nst < 2)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            regex_frag_t e2 = st[--nst];
            regex_frag_t e1 = st[--nst];
            regex_state_t *s = rx_state_new(RX_SPLIT, e1.start, e2.start, &prog.nstate);
            if (!s)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            st[nst++] = (regex_frag_t){.start = s, .out = rx_append(e1.out, e2.out)};
            continue;
        }
        if (c == '*') {
            if (nst < 1)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            regex_frag_t e = st[--nst];
            regex_state_t *s = rx_state_new(RX_SPLIT, e.start, NULL, &prog.nstate);
            if (!s)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            rx_patch(e.out, s);
            st[nst++] = (regex_frag_t){.start = s, .out = rx_list1(&s->out1)};
            continue;
        }
        if (c == '+') {
            if (nst < 1)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            regex_frag_t e = st[--nst];
            regex_state_t *s = rx_state_new(RX_SPLIT, e.start, NULL, &prog.nstate);
            if (!s)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            rx_patch(e.out, s);
            st[nst++] = (regex_frag_t){.start = e.start, .out = rx_list1(&s->out1)};
            continue;
        }
        if (c == '?') {
            if (nst < 1)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            regex_frag_t e = st[--nst];
            regex_state_t *s = rx_state_new(RX_SPLIT, e.start, NULL, &prog.nstate);
            if (!s)
                return (regex_prog_t){.start = NULL, .nstate = 0};
            st[nst++] = (regex_frag_t){.start = s, .out = rx_append(e.out, rx_list1(&s->out1))};
            continue;
        }

        int tok = (c == '.') ? RX_ANY : (unsigned char)c;
        regex_state_t *s = rx_state_new(tok, NULL, NULL, &prog.nstate);
        if (!s)
            return (regex_prog_t){.start = NULL, .nstate = 0};
        st[nst++] = (regex_frag_t){.start = s, .out = rx_list1(&s->out)};
    }

    if (nst != 1)
        return (regex_prog_t){.start = NULL, .nstate = 0};

    regex_state_t *m = rx_state_new(RX_MATCH, NULL, NULL, &prog.nstate);
    if (!m)
        return (regex_prog_t){.start = NULL, .nstate = 0};
    rx_patch(st[0].out, m);
    prog.start = st[0].start;
    return prog;
}

static void rx_addstate(regex_list_t *l, regex_state_t *s, int listid) {
    if (!s || s->lastlist == listid)
        return;
    s->lastlist = listid;
    if (s->c == RX_SPLIT) {
        rx_addstate(l, s->out, listid);
        rx_addstate(l, s->out1, listid);
        return;
    }
    l->s[l->n++] = s;
}

static void rx_startlist(regex_state_t *start, regex_list_t *l, int listid) {
    l->n = 0;
    rx_addstate(l, start, listid);
}

static void rx_step(regex_list_t *clist, int c, regex_list_t *nlist, int listid) {
    nlist->n = 0;
    for (int i = 0; i < clist->n; i++) {
        regex_state_t *s = clist->s[i];
        if (s->c == c || s->c == RX_ANY)
            rx_addstate(nlist, s->out, listid);
    }
}

static bool rx_ismatch(regex_list_t *l) {
    for (int i = 0; i < l->n; i++) {
        if (l->s[i]->c == RX_MATCH)
            return true;
    }
    return false;
}

static bool rx_match_from(regex_prog_t *prog, const char *s, bool require_full) {
    if (!prog || !prog->start || prog->nstate <= 0 || !s)
        return false;

    regex_state_t **a = (regex_state_t **)calloc((size_t)prog->nstate, sizeof(regex_state_t *));
    regex_state_t **b = (regex_state_t **)calloc((size_t)prog->nstate, sizeof(regex_state_t *));
    if (!a || !b)
        return false;
    regex_list_t l1 = {.s = a, .n = 0};
    regex_list_t l2 = {.s = b, .n = 0};
    int listid = 1;
    rx_startlist(prog->start, &l1, listid++);
    if (!require_full && rx_ismatch(&l1))
        return true;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        rx_step(&l1, (int)(*p), &l2, listid++);
        regex_list_t t = l1;
        l1 = l2;
        l2 = t;
        if (!require_full && rx_ismatch(&l1))
            return true;
    }
    return rx_ismatch(&l1);
}

static void rx_parse_anchors(sp_str_t pat, bool *start_anchor, bool *end_anchor, sp_str_t *core) {
    *start_anchor = false;
    *end_anchor = false;
    *core = pat;
    if (!pat.data || pat.len == 0)
        return;

    u32 b = 0;
    u32 e = pat.len;
    if (pat.data[0] == '^') {
        *start_anchor = true;
        b = 1;
    }
    if (e > b && pat.data[e - 1] == '$') {
        int bs = 0;
        for (s32 i = (s32)e - 2; i >= (s32)b && pat.data[i] == '\\'; i--)
            bs++;
        if ((bs % 2) == 0) {
            *end_anchor = true;
            e--;
        }
    }
    if (e < b)
        e = b;
    core->data = pat.data + b;
    core->len = e - b;
}

static bool rx_line_match(sp_str_t pattern, sp_str_t line) {
    bool start_anchor = false;
    bool end_anchor = false;
    sp_str_t core = pattern;
    rx_parse_anchors(pattern, &start_anchor, &end_anchor, &core);

    if (core.len == 0) {
        if (start_anchor && end_anchor)
            return line.len == 0;
        return true;
    }

    char *line_z = (char *)calloc((size_t)line.len + 1, 1);
    if (!line_z)
        return false;
    if (line.data && line.len > 0)
        memcpy(line_z, line.data, line.len);
    line_z[line.len] = '\0';

    if (start_anchor) {
        regex_prog_t prog = rx_compile(core);
        if (!prog.start)
            return false;
        return rx_match_from(&prog, line_z, end_anchor);
    }

    const char *s = line_z;
    for (u32 i = 0; i <= line.len; i++) {
        regex_prog_t prog = rx_compile(core);
        if (!prog.start)
            return false;
        if (rx_match_from(&prog, s + i, end_anchor))
            return true;
    }
    return false;
}

sp_str_t fxsh_grep_lines_regex(sp_str_t pattern, sp_str_t text) {
    if (!pattern.data || pattern.len == 0 || !text.data || text.len == 0)
        return (sp_str_t){.data = "", .len = 0};

    size_t cap = (size_t)text.len + 1;
    char *buf = (char *)calloc(cap, 1);
    if (!buf)
        return (sp_str_t){.data = "", .len = 0};
    size_t out = 0;
    const char *cur = text.data;
    const char *end = text.data + text.len;
    while (cur < end) {
        const char *line_end = cur;
        while (line_end < end && *line_end != '\n')
            line_end++;
        size_t line_n = (size_t)(line_end - cur);
        sp_str_t line = {.data = cur, .len = (u32)line_n};

        if (rx_line_match(pattern, line)) {
            if (out + line_n + 1 > cap)
                break;
            memcpy(buf + out, cur, line_n);
            out += line_n;
            if (line_end < end)
                buf[out++] = '\n';
        }
        cur = (line_end < end) ? (line_end + 1) : end;
    }
    buf[out] = '\0';
    return (sp_str_t){.data = buf, .len = (u32)out};
}
