#include "fxsh.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
} json_cur_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} json_sb_t;

typedef enum {
    JP_KEY,
    JP_INDEX,
} json_path_seg_kind_t;

typedef struct {
    json_path_seg_kind_t kind;
    sp_str_t key;
    s64 index;
} json_path_seg_t;

static sp_str_t json_empty(void) {
    return (sp_str_t){.data = "", .len = 0};
}

static bool json_str_eq(sp_str_t a, sp_str_t b) {
    if (a.len != b.len)
        return false;
    if (a.len == 0)
        return true;
    return memcmp(a.data, b.data, a.len) == 0;
}

static bool sb_reserve(json_sb_t *sb, size_t need) {
    if (!sb)
        return false;
    size_t req = sb->len + need;
    if (req <= sb->cap)
        return true;
    size_t ncap = sb->cap ? sb->cap : 64;
    while (ncap < req) {
        ncap *= 2;
    }
    char *nb = (char *)realloc(sb->buf, ncap);
    if (!nb)
        return false;
    sb->buf = nb;
    sb->cap = ncap;
    return true;
}

static bool sb_push_char(json_sb_t *sb, char c) {
    if (!sb_reserve(sb, 1))
        return false;
    sb->buf[sb->len++] = c;
    return true;
}

static bool sb_push_span(json_sb_t *sb, const char *s, size_t n) {
    if (!sb_reserve(sb, n))
        return false;
    if (n > 0)
        memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    return true;
}

static sp_str_t sb_take(json_sb_t *sb) {
    if (!sb)
        return json_empty();
    if (!sb_reserve(sb, 1)) {
        free(sb->buf);
        sb->buf = NULL;
        sb->len = sb->cap = 0;
        return json_empty();
    }
    sb->buf[sb->len] = '\0';
    return (sp_str_t){.data = sb->buf, .len = (u32)sb->len};
}

static void json_skip_ws(json_cur_t *c) {
    while (c && c->p < c->end) {
        char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            c->p++;
            continue;
        }
        break;
    }
}

static bool json_is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool json_parse_string_raw(json_cur_t *c, const char **out_s, const char **out_e) {
    if (!c || c->p >= c->end || *c->p != '"')
        return false;
    const char *start = c->p;
    c->p++;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch == '"') {
            if (out_s)
                *out_s = start;
            if (out_e)
                *out_e = c->p;
            return true;
        }
        if (ch < 0x20)
            return false;
        if (ch == '\\') {
            if (c->p >= c->end)
                return false;
            char esc = *c->p++;
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    break;
                case 'u':
                    for (int i = 0; i < 4; i++) {
                        if (c->p >= c->end || !json_is_hex(*c->p))
                            return false;
                        c->p++;
                    }
                    break;
                default:
                    return false;
            }
        }
    }
    return false;
}

static bool json_parse_literal(json_cur_t *c, const char *lit, const char **out_s,
                               const char **out_e) {
    if (!c || !lit)
        return false;
    const char *start = c->p;
    while (*lit) {
        if (c->p >= c->end || *c->p != *lit)
            return false;
        c->p++;
        lit++;
    }
    if (out_s)
        *out_s = start;
    if (out_e)
        *out_e = c->p;
    return true;
}

static bool json_parse_number(json_cur_t *c, const char **out_s, const char **out_e) {
    if (!c || c->p >= c->end)
        return false;
    const char *start = c->p;
    if (*c->p == '-')
        c->p++;
    if (c->p >= c->end)
        return false;
    if (*c->p == '0') {
        c->p++;
    } else {
        if (*c->p < '1' || *c->p > '9')
            return false;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
            c->p++;
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        if (c->p >= c->end || *c->p < '0' || *c->p > '9')
            return false;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
            c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-'))
            c->p++;
        if (c->p >= c->end || *c->p < '0' || *c->p > '9')
            return false;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
            c->p++;
    }
    if (out_s)
        *out_s = start;
    if (out_e)
        *out_e = c->p;
    return true;
}

static bool json_skip_value(json_cur_t *c);

static bool json_skip_array(json_cur_t *c) {
    if (!c || c->p >= c->end || *c->p != '[')
        return false;
    c->p++;
    json_skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return true;
    }
    while (c->p < c->end) {
        if (!json_skip_value(c))
            return false;
        json_skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            json_skip_ws(c);
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return true;
        }
        return false;
    }
    return false;
}

static bool json_skip_object(json_cur_t *c) {
    if (!c || c->p >= c->end || *c->p != '{')
        return false;
    c->p++;
    json_skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return true;
    }
    while (c->p < c->end) {
        if (!json_parse_string_raw(c, NULL, NULL))
            return false;
        json_skip_ws(c);
        if (c->p >= c->end || *c->p != ':')
            return false;
        c->p++;
        json_skip_ws(c);
        if (!json_skip_value(c))
            return false;
        json_skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            json_skip_ws(c);
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return true;
        }
        return false;
    }
    return false;
}

static bool json_skip_value(json_cur_t *c) {
    if (!c)
        return false;
    json_skip_ws(c);
    if (c->p >= c->end)
        return false;
    char ch = *c->p;
    if (ch == '"')
        return json_parse_string_raw(c, NULL, NULL);
    if (ch == '{')
        return json_skip_object(c);
    if (ch == '[')
        return json_skip_array(c);
    if (ch == 't')
        return json_parse_literal(c, "true", NULL, NULL);
    if (ch == 'f')
        return json_parse_literal(c, "false", NULL, NULL);
    if (ch == 'n')
        return json_parse_literal(c, "null", NULL, NULL);
    return json_parse_number(c, NULL, NULL);
}

static bool json_compact_value(json_cur_t *c, json_sb_t *out);

static bool json_compact_array(json_cur_t *c, json_sb_t *out) {
    if (!sb_push_char(out, '['))
        return false;
    c->p++;
    json_skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return sb_push_char(out, ']');
    }
    bool first = true;
    while (c->p < c->end) {
        if (!first && !sb_push_char(out, ','))
            return false;
        first = false;
        if (!json_compact_value(c, out))
            return false;
        json_skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            json_skip_ws(c);
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return sb_push_char(out, ']');
        }
        return false;
    }
    return false;
}

static bool json_compact_object(json_cur_t *c, json_sb_t *out) {
    if (!sb_push_char(out, '{'))
        return false;
    c->p++;
    json_skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return sb_push_char(out, '}');
    }
    bool first = true;
    while (c->p < c->end) {
        const char *ks = NULL;
        const char *ke = NULL;
        if (!json_parse_string_raw(c, &ks, &ke))
            return false;
        if (!first && !sb_push_char(out, ','))
            return false;
        first = false;
        if (!sb_push_span(out, ks, (size_t)(ke - ks)))
            return false;
        json_skip_ws(c);
        if (c->p >= c->end || *c->p != ':')
            return false;
        c->p++;
        if (!sb_push_char(out, ':'))
            return false;
        json_skip_ws(c);
        if (!json_compact_value(c, out))
            return false;
        json_skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            json_skip_ws(c);
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return sb_push_char(out, '}');
        }
        return false;
    }
    return false;
}

static bool json_compact_value(json_cur_t *c, json_sb_t *out) {
    if (!c || !out)
        return false;
    json_skip_ws(c);
    if (c->p >= c->end)
        return false;
    char ch = *c->p;
    if (ch == '{')
        return json_compact_object(c, out);
    if (ch == '[')
        return json_compact_array(c, out);
    if (ch == '"') {
        const char *s = NULL;
        const char *e = NULL;
        if (!json_parse_string_raw(c, &s, &e))
            return false;
        return sb_push_span(out, s, (size_t)(e - s));
    }
    if (ch == 't' || ch == 'f' || ch == 'n') {
        const char *s = NULL;
        const char *e = NULL;
        bool ok = false;
        if (ch == 't')
            ok = json_parse_literal(c, "true", &s, &e);
        else if (ch == 'f')
            ok = json_parse_literal(c, "false", &s, &e);
        else
            ok = json_parse_literal(c, "null", &s, &e);
        if (!ok)
            return false;
        return sb_push_span(out, s, (size_t)(e - s));
    }
    {
        const char *s = NULL;
        const char *e = NULL;
        if (!json_parse_number(c, &s, &e))
            return false;
        return sb_push_span(out, s, (size_t)(e - s));
    }
}

static int json_hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static bool sb_push_utf8(json_sb_t *sb, unsigned cp) {
    if (cp <= 0x7F) {
        return sb_push_char(sb, (char)cp);
    }
    if (cp <= 0x7FF) {
        return sb_push_char(sb, (char)(0xC0 | (cp >> 6))) &&
               sb_push_char(sb, (char)(0x80 | (cp & 0x3F)));
    }
    if (cp <= 0xFFFF) {
        return sb_push_char(sb, (char)(0xE0 | (cp >> 12))) &&
               sb_push_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
               sb_push_char(sb, (char)(0x80 | (cp & 0x3F)));
    }
    if (cp <= 0x10FFFF) {
        return sb_push_char(sb, (char)(0xF0 | (cp >> 18))) &&
               sb_push_char(sb, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
               sb_push_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
               sb_push_char(sb, (char)(0x80 | (cp & 0x3F)));
    }
    return false;
}

static bool json_decode_string_content(const char *s, const char *e, json_sb_t *out) {
    const char *p = s;
    while (p < e) {
        char ch = *p++;
        if (ch != '\\') {
            if (!sb_push_char(out, ch))
                return false;
            continue;
        }
        if (p >= e)
            return false;
        char esc = *p++;
        switch (esc) {
            case '"':
                if (!sb_push_char(out, '"'))
                    return false;
                break;
            case '\\':
                if (!sb_push_char(out, '\\'))
                    return false;
                break;
            case '/':
                if (!sb_push_char(out, '/'))
                    return false;
                break;
            case 'b':
                if (!sb_push_char(out, '\b'))
                    return false;
                break;
            case 'f':
                if (!sb_push_char(out, '\f'))
                    return false;
                break;
            case 'n':
                if (!sb_push_char(out, '\n'))
                    return false;
                break;
            case 'r':
                if (!sb_push_char(out, '\r'))
                    return false;
                break;
            case 't':
                if (!sb_push_char(out, '\t'))
                    return false;
                break;
            case 'u': {
                if (p + 4 > e)
                    return false;
                int h0 = json_hex_val(p[0]);
                int h1 = json_hex_val(p[1]);
                int h2 = json_hex_val(p[2]);
                int h3 = json_hex_val(p[3]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
                    return false;
                unsigned cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    if (!sb_push_char(out, '?'))
                        return false;
                } else if (!sb_push_utf8(out, cp)) {
                    return false;
                }
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

static bool json_decode_string_raw(const char *raw_s, const char *raw_e, sp_str_t *out) {
    if (!raw_s || !raw_e || raw_e - raw_s < 2)
        return false;
    if (*raw_s != '"' || *(raw_e - 1) != '"')
        return false;
    json_sb_t sb = {0};
    if (!json_decode_string_content(raw_s + 1, raw_e - 1, &sb)) {
        free(sb.buf);
        return false;
    }
    *out = sb_take(&sb);
    if (!out->data)
        return false;
    return true;
}

static bool json_path_parse(sp_str_t path, json_path_seg_t **out_segs, u32 *out_n) {
    if (!out_segs || !out_n)
        return false;
    *out_segs = NULL;
    *out_n = 0;
    const char *p = path.data;
    const char *end = path.data + path.len;
    bool need_seg = true;

    while (p < end) {
        if (*p == '.') {
            p++;
            need_seg = true;
            continue;
        }
        if (*p == '[') {
            p++;
            if (p >= end || !isdigit((unsigned char)*p))
                return false;
            s64 idx = 0;
            while (p < end && isdigit((unsigned char)*p)) {
                idx = idx * 10 + (*p - '0');
                p++;
            }
            if (p >= end || *p != ']')
                return false;
            p++;
            json_path_seg_t seg = {.kind = JP_INDEX, .key = {0}, .index = idx};
            json_path_seg_t *tmp =
                (json_path_seg_t *)realloc(*out_segs, sizeof(json_path_seg_t) * (*out_n + 1));
            if (!tmp)
                return false;
            *out_segs = tmp;
            (*out_segs)[*out_n] = seg;
            (*out_n)++;
            need_seg = false;
            continue;
        }
        if (!need_seg)
            return false;

        const char *ks = p;
        while (p < end && *p != '.' && *p != '[') {
            p++;
        }
        if (p == ks)
            return false;
        json_path_seg_t seg = {
            .kind = JP_KEY,
            .key = {.data = ks, .len = (u32)(p - ks)},
            .index = 0,
        };
        json_path_seg_t *tmp =
            (json_path_seg_t *)realloc(*out_segs, sizeof(json_path_seg_t) * (*out_n + 1));
        if (!tmp)
            return false;
        *out_segs = tmp;
        (*out_segs)[*out_n] = seg;
        (*out_n)++;
        need_seg = false;
    }

    return *out_n > 0;
}

static bool json_key_match(const char *raw_s, const char *raw_e, sp_str_t key) {
    sp_str_t dec = json_empty();
    if (!json_decode_string_raw(raw_s, raw_e, &dec))
        return false;
    bool eq = json_str_eq(dec, key);
    free((void *)dec.data);
    return eq;
}

static bool json_find_value(json_cur_t *c, const json_path_seg_t *segs, u32 nsegs, u32 depth,
                            const char **out_s, const char **out_e) {
    json_skip_ws(c);
    if (c->p >= c->end)
        return false;

    if (depth == nsegs) {
        const char *s = c->p;
        if (!json_skip_value(c))
            return false;
        if (out_s)
            *out_s = s;
        if (out_e)
            *out_e = c->p;
        return true;
    }

    json_path_seg_t seg = segs[depth];
    if (*c->p == '{') {
        if (seg.kind != JP_KEY) {
            (void)json_skip_value(c);
            return false;
        }
        c->p++;
        json_skip_ws(c);
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return false;
        }
        while (c->p < c->end) {
            const char *ks = NULL;
            const char *ke = NULL;
            if (!json_parse_string_raw(c, &ks, &ke))
                return false;
            json_skip_ws(c);
            if (c->p >= c->end || *c->p != ':')
                return false;
            c->p++;
            json_skip_ws(c);

            if (json_key_match(ks, ke, seg.key)) {
                if (json_find_value(c, segs, nsegs, depth + 1, out_s, out_e))
                    return true;
            } else {
                if (!json_skip_value(c))
                    return false;
            }

            json_skip_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                json_skip_ws(c);
                continue;
            }
            if (c->p < c->end && *c->p == '}') {
                c->p++;
                return false;
            }
            return false;
        }
        return false;
    }

    if (*c->p == '[') {
        if (seg.kind != JP_INDEX) {
            (void)json_skip_value(c);
            return false;
        }
        c->p++;
        json_skip_ws(c);
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return false;
        }
        s64 idx = 0;
        while (c->p < c->end) {
            if (idx == seg.index) {
                if (json_find_value(c, segs, nsegs, depth + 1, out_s, out_e))
                    return true;
            } else {
                if (!json_skip_value(c))
                    return false;
            }
            idx++;
            json_skip_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                json_skip_ws(c);
                continue;
            }
            if (c->p < c->end && *c->p == ']') {
                c->p++;
                return false;
            }
            return false;
        }
        return false;
    }

    (void)json_skip_value(c);
    return false;
}

bool fxsh_json_validate(sp_str_t json) {
    json_cur_t c = {.p = json.data, .end = json.data + json.len};
    if (!json_skip_value(&c))
        return false;
    json_skip_ws(&c);
    return c.p == c.end;
}

sp_str_t fxsh_json_compact(sp_str_t json) {
    json_cur_t c = {.p = json.data, .end = json.data + json.len};
    json_sb_t out = {0};
    if (!json_compact_value(&c, &out)) {
        free(out.buf);
        return json_empty();
    }
    json_skip_ws(&c);
    if (c.p != c.end) {
        free(out.buf);
        return json_empty();
    }
    return sb_take(&out);
}

sp_str_t fxsh_json_kind(sp_str_t json) {
    json_cur_t c = {.p = json.data, .end = json.data + json.len};
    json_skip_ws(&c);
    if (c.p >= c.end)
        return json_empty();

    const char *kind = NULL;
    switch (*c.p) {
        case '{':
            kind = "object";
            break;
        case '[':
            kind = "array";
            break;
        case '"':
            kind = "string";
            break;
        case 't':
        case 'f':
            kind = "bool";
            break;
        case 'n':
            kind = "null";
            break;
        default:
            kind = "number";
            break;
    }
    if (!json_skip_value(&c))
        return json_empty();
    json_skip_ws(&c);
    if (c.p != c.end)
        return json_empty();
    return fxsh_from_cstr(kind);
}

sp_str_t fxsh_json_quote_string(sp_str_t s) {
    static const char hex[] = "0123456789abcdef";
    json_sb_t out = {0};
    if (!sb_push_char(&out, '"')) {
        free(out.buf);
        return json_empty();
    }
    for (u32 i = 0; i < s.len; i++) {
        unsigned char ch = (unsigned char)s.data[i];
        switch (ch) {
            case '"':
                if (!sb_push_span(&out, "\\\"", 2)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
            case '\\':
                if (!sb_push_span(&out, "\\\\", 2)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
            case '\b':
                if (!sb_push_span(&out, "\\b", 2)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
            case '\f':
                if (!sb_push_span(&out, "\\f", 2)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
            case '\n':
                if (!sb_push_span(&out, "\\n", 2)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
            case '\r':
                if (!sb_push_span(&out, "\\r", 2)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
            case '\t':
                if (!sb_push_span(&out, "\\t", 2)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
            default:
                if (ch < 0x20) {
                    char esc[6] = {'\\', 'u', '0', '0', hex[(ch >> 4) & 0xF], hex[ch & 0xF]};
                    if (!sb_push_span(&out, esc, 6)) {
                        free(out.buf);
                        return json_empty();
                    }
                } else if (!sb_push_char(&out, (char)ch)) {
                    free(out.buf);
                    return json_empty();
                }
                break;
        }
    }
    if (!sb_push_char(&out, '"')) {
        free(out.buf);
        return json_empty();
    }
    return sb_take(&out);
}

bool fxsh_json_has(sp_str_t json, sp_str_t path) {
    json_path_seg_t *segs = NULL;
    u32 nsegs = 0;
    if (!json_path_parse(path, &segs, &nsegs)) {
        free(segs);
        return false;
    }
    json_cur_t c = {.p = json.data, .end = json.data + json.len};
    const char *s = NULL;
    const char *e = NULL;
    bool ok = json_find_value(&c, segs, nsegs, 0, &s, &e);
    free(segs);
    return ok && s && e && e >= s;
}

sp_str_t fxsh_json_get(sp_str_t json, sp_str_t path) {
    json_path_seg_t *segs = NULL;
    u32 nsegs = 0;
    if (!json_path_parse(path, &segs, &nsegs)) {
        free(segs);
        return json_empty();
    }
    json_cur_t c = {.p = json.data, .end = json.data + json.len};
    const char *s = NULL;
    const char *e = NULL;
    bool ok = json_find_value(&c, segs, nsegs, 0, &s, &e);
    free(segs);
    if (!ok || !s || !e || e < s)
        return json_empty();
    return fxsh_json_compact((sp_str_t){.data = s, .len = (u32)(e - s)});
}

sp_str_t fxsh_json_get_string(sp_str_t json, sp_str_t path) {
    json_path_seg_t *segs = NULL;
    u32 nsegs = 0;
    if (!json_path_parse(path, &segs, &nsegs)) {
        free(segs);
        return json_empty();
    }
    json_cur_t c = {.p = json.data, .end = json.data + json.len};
    const char *s = NULL;
    const char *e = NULL;
    bool ok = json_find_value(&c, segs, nsegs, 0, &s, &e);
    free(segs);
    if (!ok || !s || !e || e - s < 2 || *s != '"' || *(e - 1) != '"')
        return json_empty();
    sp_str_t out = json_empty();
    if (!json_decode_string_raw(s, e, &out))
        return json_empty();
    return out;
}

s64 fxsh_json_get_int(sp_str_t json, sp_str_t path, bool *ok) {
    if (ok)
        *ok = false;
    sp_str_t raw = fxsh_json_get(json, path);
    if (!raw.data || raw.len == 0)
        return 0;
    char *endp = NULL;
    long long v = strtoll(raw.data, &endp, 10);
    if (!endp || *endp != '\0')
        return 0;
    if (ok)
        *ok = true;
    return (s64)v;
}

f64 fxsh_json_get_float(sp_str_t json, sp_str_t path, bool *ok) {
    if (ok)
        *ok = false;
    sp_str_t raw = fxsh_json_get(json, path);
    if (!raw.data || raw.len == 0)
        return 0.0;
    char *endp = NULL;
    double v = strtod(raw.data, &endp);
    if (!endp || *endp != '\0')
        return 0.0;
    if (ok)
        *ok = true;
    return v;
}

bool fxsh_json_get_bool(sp_str_t json, sp_str_t path, bool *ok) {
    if (ok)
        *ok = false;
    sp_str_t raw = fxsh_json_get(json, path);
    if (raw.len == 4 && strncmp(raw.data, "true", 4) == 0) {
        if (ok)
            *ok = true;
        return true;
    }
    if (raw.len == 5 && strncmp(raw.data, "false", 5) == 0) {
        if (ok)
            *ok = true;
        return false;
    }
    return false;
}
