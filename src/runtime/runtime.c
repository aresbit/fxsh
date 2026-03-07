/*
 * runtime.c - shared runtime value model for interpreter/native backends
 */

#include "fxsh.h"

#include <stdio.h>
#include <string.h>

static fxsh_rt_value_t *rt_new(fxsh_rt_kind_t k) {
    fxsh_rt_value_t *v = (fxsh_rt_value_t *)fxsh_alloc0(sizeof(fxsh_rt_value_t));
    v->kind = k;
    return v;
}

fxsh_rt_value_t *fxsh_rt_unit(void) {
    return rt_new(FXSH_RT_UNIT);
}

fxsh_rt_value_t *fxsh_rt_bool(bool b) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_BOOL);
    v->as.b = b;
    return v;
}

fxsh_rt_value_t *fxsh_rt_int(s64 i) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_INT);
    v->as.i = i;
    return v;
}

fxsh_rt_value_t *fxsh_rt_float(f64 f) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_FLOAT);
    v->as.f = f;
    return v;
}

fxsh_rt_value_t *fxsh_rt_string(sp_str_t s) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_STRING);
    v->as.s = s;
    return v;
}

fxsh_rt_value_t *fxsh_rt_function(fxsh_ast_list_t params, fxsh_ast_node_t *body,
                                  fxsh_rt_env_t *env) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_FUNCTION);
    v->as.fn.params = params;
    v->as.fn.body = body;
    v->as.fn.env = env;
    v->as.fn.bound_args = SP_NULLPTR;
    v->as.fn.self_name = (sp_str_t){0};
    return v;
}

fxsh_rt_value_t *fxsh_rt_constr(sp_str_t tag, sp_dyn_array(fxsh_rt_value_t *) args) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_CONSTR);
    v->as.constr.tag = tag;
    v->as.constr.args = args;
    return v;
}

fxsh_rt_value_t *fxsh_rt_record(sp_dyn_array(sp_str_t) names,
                                sp_dyn_array(fxsh_rt_value_t *) values) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_RECORD);
    v->as.record.names = names;
    v->as.record.values = values;
    return v;
}

fxsh_rt_value_t *fxsh_rt_record_get(fxsh_rt_value_t *record, sp_str_t field_name) {
    if (!record || record->kind != FXSH_RT_RECORD)
        return NULL;
    if (sp_dyn_array_size(record->as.record.names) != sp_dyn_array_size(record->as.record.values))
        return NULL;
    sp_dyn_array_for(record->as.record.names, i) {
        if (sp_str_equal(record->as.record.names[i], field_name))
            return record->as.record.values[i];
    }
    return NULL;
}

fxsh_rt_value_t *fxsh_rt_tuple(sp_dyn_array(fxsh_rt_value_t *) items) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_TUPLE);
    v->as.tuple.items = items;
    return v;
}

fxsh_rt_value_t *fxsh_rt_tuple_get(fxsh_rt_value_t *tuple, u32 idx) {
    if (!tuple || tuple->kind != FXSH_RT_TUPLE)
        return NULL;
    if (!tuple->as.tuple.items || idx >= sp_dyn_array_size(tuple->as.tuple.items))
        return NULL;
    return tuple->as.tuple.items[idx];
}

fxsh_rt_value_t *fxsh_rt_list_nil(void) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_LIST);
    v->as.list.is_nil = true;
    v->as.list.head = NULL;
    v->as.list.tail = NULL;
    return v;
}

fxsh_rt_value_t *fxsh_rt_list_cons(fxsh_rt_value_t *head, fxsh_rt_value_t *tail) {
    fxsh_rt_value_t *v = rt_new(FXSH_RT_LIST);
    v->as.list.is_nil = false;
    v->as.list.head = head;
    v->as.list.tail = tail;
    return v;
}

bool fxsh_rt_list_is_nil(fxsh_rt_value_t *list) {
    return list && list->kind == FXSH_RT_LIST && list->as.list.is_nil;
}

fxsh_rt_value_t *fxsh_rt_list_head(fxsh_rt_value_t *list) {
    if (!list || list->kind != FXSH_RT_LIST || list->as.list.is_nil)
        return NULL;
    return list->as.list.head;
}

fxsh_rt_value_t *fxsh_rt_list_tail(fxsh_rt_value_t *list) {
    if (!list || list->kind != FXSH_RT_LIST || list->as.list.is_nil)
        return NULL;
    return list->as.list.tail;
}

fxsh_rt_value_t *fxsh_rt_tensor_new2(s64 rows, s64 cols, f64 fill) {
    if (rows <= 0 || cols <= 0)
        return NULL;
    fxsh_rt_value_t *v = rt_new(FXSH_RT_TENSOR);
    v->as.tensor.rows = rows;
    v->as.tensor.cols = cols;
    size_t n = (size_t)(rows * cols);
    v->as.tensor.data = (f64 *)fxsh_alloc0(sizeof(f64) * n);
    for (size_t i = 0; i < n; i++)
        v->as.tensor.data[i] = fill;
    return v;
}

fxsh_rt_value_t *fxsh_rt_tensor_from_list2(s64 rows, s64 cols, fxsh_rt_value_t *list) {
    fxsh_rt_value_t *t = fxsh_rt_tensor_new2(rows, cols, 0.0);
    if (!t)
        return NULL;
    size_t need = (size_t)(rows * cols);
    fxsh_rt_value_t *cur = list;
    for (size_t i = 0; i < need; i++) {
        if (!cur || cur->kind != FXSH_RT_LIST || cur->as.list.is_nil)
            return NULL;
        fxsh_rt_value_t *hv = cur->as.list.head;
        if (!hv)
            return NULL;
        if (hv->kind == FXSH_RT_FLOAT)
            t->as.tensor.data[i] = hv->as.f;
        else if (hv->kind == FXSH_RT_INT)
            t->as.tensor.data[i] = (f64)hv->as.i;
        else
            return NULL;
        cur = cur->as.list.tail;
    }
    if (cur && cur->kind == FXSH_RT_LIST && !cur->as.list.is_nil)
        return NULL;
    return t;
}

fxsh_rt_value_t *fxsh_rt_tensor_shape2(fxsh_rt_value_t *t) {
    if (!t || t->kind != FXSH_RT_TENSOR)
        return NULL;
    sp_dyn_array(fxsh_rt_value_t *) items = SP_NULLPTR;
    sp_dyn_array_push(items, fxsh_rt_int(t->as.tensor.rows));
    sp_dyn_array_push(items, fxsh_rt_int(t->as.tensor.cols));
    return fxsh_rt_tuple(items);
}

fxsh_rt_value_t *fxsh_rt_tensor_get2(fxsh_rt_value_t *t, s64 i, s64 j) {
    if (!t || t->kind != FXSH_RT_TENSOR)
        return NULL;
    if (i < 0 || j < 0 || i >= t->as.tensor.rows || j >= t->as.tensor.cols)
        return NULL;
    size_t idx = (size_t)(i * t->as.tensor.cols + j);
    return fxsh_rt_float(t->as.tensor.data[idx]);
}

fxsh_rt_value_t *fxsh_rt_tensor_set2(fxsh_rt_value_t *t, s64 i, s64 j, f64 value) {
    if (!t || t->kind != FXSH_RT_TENSOR)
        return NULL;
    if (i < 0 || j < 0 || i >= t->as.tensor.rows || j >= t->as.tensor.cols)
        return NULL;
    fxsh_rt_value_t *out = fxsh_rt_tensor_new2(t->as.tensor.rows, t->as.tensor.cols, 0.0);
    if (!out)
        return NULL;
    size_t n = (size_t)(t->as.tensor.rows * t->as.tensor.cols);
    memcpy(out->as.tensor.data, t->as.tensor.data, sizeof(f64) * n);
    out->as.tensor.data[(size_t)(i * out->as.tensor.cols + j)] = value;
    return out;
}

fxsh_rt_value_t *fxsh_rt_tensor_add(fxsh_rt_value_t *a, fxsh_rt_value_t *b) {
    if (!a || !b || a->kind != FXSH_RT_TENSOR || b->kind != FXSH_RT_TENSOR)
        return NULL;
    if (a->as.tensor.rows != b->as.tensor.rows || a->as.tensor.cols != b->as.tensor.cols)
        return NULL;
    fxsh_rt_value_t *out = fxsh_rt_tensor_new2(a->as.tensor.rows, a->as.tensor.cols, 0.0);
    if (!out)
        return NULL;
    size_t n = (size_t)(a->as.tensor.rows * a->as.tensor.cols);
    for (size_t i = 0; i < n; i++)
        out->as.tensor.data[i] = a->as.tensor.data[i] + b->as.tensor.data[i];
    return out;
}

fxsh_rt_value_t *fxsh_rt_tensor_dot(fxsh_rt_value_t *a, fxsh_rt_value_t *b) {
    if (!a || !b || a->kind != FXSH_RT_TENSOR || b->kind != FXSH_RT_TENSOR)
        return NULL;
    if (a->as.tensor.cols != b->as.tensor.rows)
        return NULL;
    fxsh_rt_value_t *out = fxsh_rt_tensor_new2(a->as.tensor.rows, b->as.tensor.cols, 0.0);
    if (!out)
        return NULL;
    for (s64 i = 0; i < a->as.tensor.rows; i++) {
        for (s64 j = 0; j < b->as.tensor.cols; j++) {
            f64 acc = 0.0;
            for (s64 k = 0; k < a->as.tensor.cols; k++) {
                f64 lv = a->as.tensor.data[(size_t)(i * a->as.tensor.cols + k)];
                f64 rv = b->as.tensor.data[(size_t)(k * b->as.tensor.cols + j)];
                acc += lv * rv;
            }
            out->as.tensor.data[(size_t)(i * out->as.tensor.cols + j)] = acc;
        }
    }
    return out;
}

fxsh_rt_env_t *fxsh_rt_env_bind(fxsh_rt_env_t *env, sp_str_t name, fxsh_rt_value_t *value) {
    fxsh_rt_env_t *n = (fxsh_rt_env_t *)fxsh_alloc0(sizeof(fxsh_rt_env_t));
    n->name = name;
    n->value = value;
    n->next = env;
    return n;
}

fxsh_rt_value_t *fxsh_rt_env_lookup(fxsh_rt_env_t *env, sp_str_t name) {
    for (fxsh_rt_env_t *n = env; n; n = n->next) {
        if (sp_str_equal(n->name, name))
            return n->value;
    }
    return NULL;
}

bool fxsh_rt_equal(fxsh_rt_value_t *a, fxsh_rt_value_t *b) {
    if (!a || !b)
        return a == b;
    if (a->kind != b->kind)
        return false;

    switch (a->kind) {
        case FXSH_RT_UNIT:
            return true;
        case FXSH_RT_BOOL:
            return a->as.b == b->as.b;
        case FXSH_RT_INT:
            return a->as.i == b->as.i;
        case FXSH_RT_FLOAT:
            return a->as.f == b->as.f;
        case FXSH_RT_STRING:
            return sp_str_equal(a->as.s, b->as.s);
        case FXSH_RT_CONSTR: {
            if (!sp_str_equal(a->as.constr.tag, b->as.constr.tag))
                return false;
            if (sp_dyn_array_size(a->as.constr.args) != sp_dyn_array_size(b->as.constr.args))
                return false;
            sp_dyn_array_for(a->as.constr.args, i) {
                if (!fxsh_rt_equal(a->as.constr.args[i], b->as.constr.args[i]))
                    return false;
            }
            return true;
        }
        case FXSH_RT_RECORD: {
            if (sp_dyn_array_size(a->as.record.names) != sp_dyn_array_size(b->as.record.names))
                return false;
            sp_dyn_array_for(a->as.record.names, i) {
                fxsh_rt_value_t *av = fxsh_rt_record_get(a, a->as.record.names[i]);
                fxsh_rt_value_t *bv = fxsh_rt_record_get(b, a->as.record.names[i]);
                if (!av || !bv || !fxsh_rt_equal(av, bv))
                    return false;
            }
            return true;
        }
        case FXSH_RT_TUPLE: {
            if (sp_dyn_array_size(a->as.tuple.items) != sp_dyn_array_size(b->as.tuple.items))
                return false;
            sp_dyn_array_for(a->as.tuple.items, i) {
                if (!fxsh_rt_equal(a->as.tuple.items[i], b->as.tuple.items[i]))
                    return false;
            }
            return true;
        }
        case FXSH_RT_LIST: {
            if (a->as.list.is_nil != b->as.list.is_nil)
                return false;
            if (a->as.list.is_nil)
                return true;
            return fxsh_rt_equal(a->as.list.head, b->as.list.head) &&
                   fxsh_rt_equal(a->as.list.tail, b->as.list.tail);
        }
        case FXSH_RT_TENSOR: {
            if (a->as.tensor.rows != b->as.tensor.rows || a->as.tensor.cols != b->as.tensor.cols)
                return false;
            size_t n = (size_t)(a->as.tensor.rows * a->as.tensor.cols);
            for (size_t i = 0; i < n; i++) {
                if (a->as.tensor.data[i] != b->as.tensor.data[i])
                    return false;
            }
            return true;
        }
        case FXSH_RT_FUNCTION:
            return a == b;
    }
    return false;
}

sp_str_t fxsh_rt_to_string(fxsh_rt_value_t *v) {
    if (!v)
        return sp_str_lit("<null>");

    switch (v->kind) {
        case FXSH_RT_UNIT:
            return sp_str_lit("()");
        case FXSH_RT_BOOL:
            return v->as.b ? sp_str_lit("true") : sp_str_lit("false");
        case FXSH_RT_INT: {
            char *buf = (char *)fxsh_alloc0(32);
            snprintf(buf, 32, "%ld", v->as.i);
            return sp_str_view(buf);
        }
        case FXSH_RT_FLOAT: {
            char *buf = (char *)fxsh_alloc0(64);
            snprintf(buf, 64, "%g", v->as.f);
            return sp_str_view(buf);
        }
        case FXSH_RT_STRING:
            return v->as.s;
        case FXSH_RT_FUNCTION:
            return sp_str_lit("<function>");
        case FXSH_RT_CONSTR: {
            if (sp_dyn_array_size(v->as.constr.args) == 0)
                return v->as.constr.tag;
            char *buf = (char *)fxsh_alloc0(256);
            size_t off = 0;
            off += (size_t)snprintf(buf + off, 256 - off, "%.*s(", v->as.constr.tag.len,
                                    v->as.constr.tag.data);
            sp_dyn_array_for(v->as.constr.args, i) {
                sp_str_t s = fxsh_rt_to_string(v->as.constr.args[i]);
                off += (size_t)snprintf(buf + off, 256 - off, "%.*s%s", s.len, s.data,
                                        i + 1 < sp_dyn_array_size(v->as.constr.args) ? ", " : "");
                if (off >= 252)
                    break;
            }
            snprintf(buf + off, 256 - off, ")");
            return sp_str_view(buf);
        }
        case FXSH_RT_RECORD: {
            char *buf = (char *)fxsh_alloc0(512);
            size_t off = 0;
            off += (size_t)snprintf(buf + off, 512 - off, "{");
            sp_dyn_array_for(v->as.record.names, i) {
                fxsh_rt_value_t *fv = v->as.record.values[i];
                sp_str_t s = fxsh_rt_to_string(fv);
                off += (size_t)snprintf(buf + off, 512 - off, "%.*s = %.*s%s",
                                        v->as.record.names[i].len, v->as.record.names[i].data,
                                        s.len, s.data,
                                        i + 1 < sp_dyn_array_size(v->as.record.names) ? ", " : "");
                if (off >= 508)
                    break;
            }
            snprintf(buf + off, 512 - off, "}");
            return sp_str_view(buf);
        }
        case FXSH_RT_TUPLE: {
            char *buf = (char *)fxsh_alloc0(512);
            size_t off = 0;
            off += (size_t)snprintf(buf + off, 512 - off, "(");
            sp_dyn_array_for(v->as.tuple.items, i) {
                sp_str_t s = fxsh_rt_to_string(v->as.tuple.items[i]);
                off += (size_t)snprintf(buf + off, 512 - off, "%.*s%s", s.len, s.data,
                                        i + 1 < sp_dyn_array_size(v->as.tuple.items) ? ", " : "");
                if (off >= 508)
                    break;
            }
            snprintf(buf + off, 512 - off, ")");
            return sp_str_view(buf);
        }
        case FXSH_RT_LIST: {
            char *buf = (char *)fxsh_alloc0(512);
            size_t off = 0;
            fxsh_rt_value_t *cur = v;
            off += (size_t)snprintf(buf + off, 512 - off, "[");
            bool first = true;
            while (cur && cur->kind == FXSH_RT_LIST && !cur->as.list.is_nil) {
                sp_str_t hs = fxsh_rt_to_string(cur->as.list.head);
                off += (size_t)snprintf(buf + off, 512 - off, "%s%.*s", first ? "" : "; ", hs.len,
                                        hs.data);
                if (off >= 508)
                    break;
                first = false;
                cur = cur->as.list.tail;
            }
            snprintf(buf + off, 512 - off, "]");
            return sp_str_view(buf);
        }
        case FXSH_RT_TENSOR: {
            char *buf = (char *)fxsh_alloc0(96);
            snprintf(buf, 96, "<tensor %ldx%ld>", v->as.tensor.rows, v->as.tensor.cols);
            return sp_str_view(buf);
        }
    }
    return sp_str_lit("<unknown>");
}
