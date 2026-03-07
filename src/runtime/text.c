#include "fxsh.h"

#include <stdlib.h>
#include <string.h>

char *fxsh_cstr_dup(sp_str_t s) {
    char *p = (char *)malloc((size_t)s.len + 1u);
    if (!p)
        return NULL;
    if (s.len > 0 && s.data)
        memcpy(p, s.data, s.len);
    p[s.len] = '\0';
    return p;
}

sp_str_t fxsh_from_cstr(const char *p) {
    if (!p)
        return (sp_str_t){.data = "", .len = 0};
    return (sp_str_t){.data = p, .len = (u32)strlen(p)};
}

bool fxsh_str_eq(sp_str_t a, sp_str_t b) {
    return a.len == b.len && strncmp(a.data, b.data, a.len) == 0;
}

sp_str_t fxsh_str_concat(sp_str_t a, sp_str_t b) {
    size_t n = (size_t)a.len + (size_t)b.len;
    char *buf = (char *)malloc(n + 1);
    if (!buf)
        return (sp_str_t){.data = "", .len = 0};
    memcpy(buf, a.data, a.len);
    memcpy(buf + a.len, b.data, b.len);
    buf[n] = '\0';
    return (sp_str_t){.data = buf, .len = (u32)n};
}

sp_str_t fxsh_replace_once(sp_str_t s, sp_str_t old_t, sp_str_t new_t) {
    if (!s.data || !old_t.data || old_t.len == 0)
        return s;
    const char *p = strstr(s.data, old_t.data);
    if (!p)
        return s;
    size_t pre = (size_t)(p - s.data);
    size_t post = (size_t)s.len - pre - (size_t)old_t.len;
    size_t n = pre + (size_t)new_t.len + post;
    char *out = (char *)malloc(n + 1);
    if (!out)
        return (sp_str_t){.data = "", .len = 0};
    memcpy(out, s.data, pre);
    memcpy(out + pre, new_t.data, new_t.len);
    memcpy(out + pre + new_t.len, p + old_t.len, post);
    out[n] = '\0';
    return (sp_str_t){.data = out, .len = (u32)n};
}
