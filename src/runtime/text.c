#include "fxsh.h"

#include <ctype.h>
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
    if (a.len != b.len)
        return false;
    if (a.len == 0)
        return true;
    if (!a.data || !b.data)
        return false;
    return memcmp(a.data, b.data, a.len) == 0;
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

static s64 find_substr_at(sp_str_t haystack, sp_str_t needle) {
    if (!haystack.data || !needle.data || needle.len == 0 || haystack.len < needle.len)
        return -1;
    u32 limit = haystack.len - needle.len;
    for (u32 i = 0; i <= limit; i++) {
        if (memcmp(haystack.data + i, needle.data, needle.len) == 0)
            return (s64)i;
    }
    return -1;
}

s64 fxsh_str_len_rt(sp_str_t s) {
    return (s64)s.len;
}

sp_str_t fxsh_str_slice_rt(sp_str_t s, s64 start, s64 len) {
    if (!s.data || s.len == 0)
        return (sp_str_t){.data = "", .len = 0};
    if (start < 0)
        start = 0;
    if (len <= 0 || (u32)start >= s.len)
        return (sp_str_t){.data = "", .len = 0};
    u32 ustart = (u32)start;
    u32 available = s.len - ustart;
    u32 ulen = (u32)len > available ? available : (u32)len;
    char *buf = (char *)malloc((size_t)ulen + 1u);
    if (!buf)
        return (sp_str_t){.data = "", .len = 0};
    memcpy(buf, s.data + ustart, ulen);
    buf[ulen] = '\0';
    return (sp_str_t){.data = buf, .len = ulen};
}

s64 fxsh_str_find_rt(sp_str_t s, sp_str_t needle) {
    if (needle.len == 0)
        return 0;
    return find_substr_at(s, needle);
}

s64 fxsh_str_find_from_rt(sp_str_t s, sp_str_t needle, s64 start) {
    if (needle.len == 0)
        return start < 0 ? 0 : start;
    if (!s.data || !needle.data)
        return -1;
    if (start < 0)
        start = 0;
    if ((u32)start >= s.len)
        return -1;
    sp_str_t tail = {.data = s.data + start, .len = s.len - (u32)start};
    s64 found = find_substr_at(tail, needle);
    return found < 0 ? -1 : (start + found);
}

bool fxsh_str_starts_with_rt(sp_str_t s, sp_str_t prefix) {
    if (prefix.len > s.len)
        return false;
    if (prefix.len == 0)
        return true;
    if (!s.data || !prefix.data)
        return false;
    return memcmp(s.data, prefix.data, prefix.len) == 0;
}

bool fxsh_str_ends_with_rt(sp_str_t s, sp_str_t suffix) {
    if (suffix.len > s.len)
        return false;
    if (suffix.len == 0)
        return true;
    if (!s.data || !suffix.data)
        return false;
    return memcmp(s.data + (s.len - suffix.len), suffix.data, suffix.len) == 0;
}

sp_str_t fxsh_str_trim_rt(sp_str_t s) {
    if (!s.data || s.len == 0)
        return (sp_str_t){.data = "", .len = 0};
    u32 start = 0;
    while (start < s.len && isspace((unsigned char)s.data[start]))
        start++;
    u32 end = s.len;
    while (end > start && isspace((unsigned char)s.data[end - 1]))
        end--;
    return fxsh_str_slice_rt(s, start, (s64)(end - start));
}

s64 fxsh_byte_at_rt(sp_str_t s, s64 index) {
    if (!s.data || index < 0 || (u32)index >= s.len)
        return -1;
    return (s64)(unsigned char)s.data[index];
}

sp_str_t fxsh_byte_to_string_rt(s64 byte_value) {
    unsigned char b = (unsigned char)(byte_value & 0xFF);
    char *buf = (char *)malloc(2);
    if (!buf)
        return (sp_str_t){.data = "", .len = 0};
    buf[0] = (char)b;
    buf[1] = '\0';
    return (sp_str_t){.data = buf, .len = 1};
}

sp_str_t fxsh_split_words_rt(sp_str_t s) {
    if (!s.data || s.len == 0)
        return (sp_str_t){.data = "", .len = 0};
    char *out = (char *)malloc((size_t)s.len + 1u);
    if (!out)
        return (sp_str_t){.data = "", .len = 0};
    u32 j = 0;
    u32 i = 0;
    while (i < s.len) {
        while (i < s.len && isspace((unsigned char)s.data[i]))
            i++;
        if (i >= s.len)
            break;
        if (j > 0)
            out[j++] = '\n';
        while (i < s.len && !isspace((unsigned char)s.data[i]))
            out[j++] = s.data[i++];
    }
    out[j] = '\0';
    return (sp_str_t){.data = out, .len = j};
}

sp_str_t fxsh_replace_once(sp_str_t s, sp_str_t old_t, sp_str_t new_t) {
    if (!s.data || !old_t.data || old_t.len == 0)
        return s;
    s64 pos = find_substr_at(s, old_t);
    if (pos < 0)
        return s;
    size_t pre = (size_t)pos;
    size_t post = (size_t)s.len - pre - (size_t)old_t.len;
    size_t n = pre + (size_t)new_t.len + post;
    char *out = (char *)malloc(n + 1);
    if (!out)
        return (sp_str_t){.data = "", .len = 0};
    memcpy(out, s.data, pre);
    memcpy(out + pre, new_t.data, new_t.len);
    memcpy(out + pre + new_t.len, s.data + pre + old_t.len, post);
    out[n] = '\0';
    return (sp_str_t){.data = out, .len = (u32)n};
}
