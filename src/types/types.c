/*
 * types.c - fxsh Hindley-Milner type inference
 *
 * Fixes from design review:
 *   1. TypeEnv is now a persistent linked list (functional style)
 *      - No more sp_ht_ensure / sp_ht_set_fns (those don't exist in sp.h)
 *      - Natural shadowing support
 *      - O(n) lookup is fine for typical script-size programs
 *   2. ConstrEnv similarly uses linked list
 *   3. free_vars_in_env implemented
 *   4. infer_pattern AST_PAT_LIT fixed (use stored lit node sub-kind)
 *   5. type_env_bind no longer mutates a passed pointer; returns new env
 */

#include "fxsh.h"

#include <stdio.h>
#include <string.h>

typedef sp_ht(s32, s32) type_var_map_t;

static s32 record_field_index(sp_dyn_array(fxsh_field_t) fields, sp_str_t name);
static fxsh_type_t *make_record_type(sp_dyn_array(fxsh_field_t) fields, fxsh_type_var_t row_var);
static const c8 *var_name(fxsh_type_var_t v);

static const c8 *var_name(fxsh_type_var_t v) {
    static _Thread_local c8 buf[16];
    s32 q = v / 26;
    s32 r = v % 26;
    if (q <= 0) {
        buf[0] = '\'';
        buf[1] = (c8)('a' + r);
        buf[2] = '\0';
    } else {
        snprintf((char *)buf, sizeof(buf), "'%c%d", (int)('a' + r), (int)q);
    }
    return buf;
}

/*=============================================================================
 * Type Variable Generation
 *=============================================================================*/

static s32 next_var_id = 0;

fxsh_type_var_t fxsh_fresh_var(void) {
    return next_var_id++;
}

void fxsh_reset_type_vars(void) {
    next_var_id = 0;
}

/*=============================================================================
 * Type Constructors
 *=============================================================================*/

fxsh_type_t *fxsh_type_var(fxsh_type_var_t var) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind = TYPE_VAR;
    t->data.var = var;
    return t;
}

fxsh_type_t *fxsh_type_con(sp_str_t name) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind = TYPE_CON;
    t->data.con = name;
    return t;
}

fxsh_type_t *fxsh_type_arrow(fxsh_type_t *param, fxsh_type_t *ret) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind = TYPE_ARROW;
    t->data.arrow.param = param;
    t->data.arrow.ret = ret;
    return t;
}

fxsh_type_t *fxsh_type_apply(fxsh_type_t *con, fxsh_type_t *arg) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind = TYPE_APP;
    t->data.app.con = con;
    t->data.app.arg = arg;
    return t;
}

/*=============================================================================
 * Type Environment — uses linked list from fxsh.h
 *=============================================================================*/

/* Create empty env */
fxsh_type_env_t fxsh_type_env_empty(void) {
    return NULL;
}

/* Extend env with a new binding (functional, no mutation) */
fxsh_type_env_t fxsh_type_env_extend(fxsh_type_env_t env, sp_str_t name, fxsh_scheme_t *scheme) {
    fxsh_tenv_node_t *node = (fxsh_tenv_node_t *)fxsh_alloc0(sizeof(fxsh_tenv_node_t));
    node->name = name;
    node->scheme = scheme;
    node->next = (fxsh_tenv_node_t *)env;
    return (fxsh_type_env_t)node;
}

/* Lookup — O(n), fine for scripts */
static fxsh_scheme_t *type_env_lookup(fxsh_type_env_t env, sp_str_t name) {
    fxsh_tenv_node_t *node = (fxsh_tenv_node_t *)env;
    while (node) {
        if (sp_str_equal(node->name, name))
            return node->scheme;
        node = node->next;
    }
    return NULL;
}

/* Mutating bind (for use when we have a pointer to env and need to update it) */
static void type_env_bind(fxsh_type_env_t *env, sp_str_t name, fxsh_scheme_t *scheme) {
    *env = fxsh_type_env_extend(*env, name, scheme);
}

/*=============================================================================
 * Constructor Environment — uses linked list from fxsh.h
 *=============================================================================*/

static fxsh_constr_env_t constr_env_extend(fxsh_constr_env_t env, sp_str_t name,
                                           fxsh_constr_info_t *info) {
    fxsh_cenv_node_t *node = (fxsh_cenv_node_t *)fxsh_alloc0(sizeof(fxsh_cenv_node_t));
    node->name = name;
    node->info = *info;
    node->next = (fxsh_cenv_node_t *)env;
    return (fxsh_constr_env_t)node;
}

static void constr_env_bind(fxsh_constr_env_t *env, sp_str_t name, fxsh_constr_info_t *info) {
    *env = constr_env_extend(*env, name, info);
}

static fxsh_constr_info_t *constr_env_lookup(fxsh_constr_env_t env, sp_str_t name) {
    fxsh_cenv_node_t *node = (fxsh_cenv_node_t *)env;
    while (node) {
        if (sp_str_equal(node->name, name))
            return &node->info;
        node = node->next;
    }
    return NULL;
}

static bool str_in_list(sp_dyn_array(sp_str_t) list, sp_str_t s) {
    sp_dyn_array_for(list, i) {
        if (sp_str_equal(list[i], s))
            return true;
    }
    return false;
}

static bool lit_string_has_prefix(sp_str_t s, const char *prefix) {
    size_t n = strlen(prefix);
    if (!s.data || s.len < n)
        return false;
    return memcmp(s.data, prefix, n) == 0;
}

/* For type applications like `'a option`, peel TYPE_APP to get the head constructor `option`. */
static sp_str_t type_head_constructor(fxsh_type_t *type) {
    if (!type)
        return (sp_str_t){0};
    while (type->kind == TYPE_APP) {
        type = type->data.app.con;
        if (!type)
            return (sp_str_t){0};
    }
    if (type->kind == TYPE_CON)
        return type->data.con;
    return (sp_str_t){0};
}

/*=============================================================================
 * Type to String
 *=============================================================================*/

static void type_to_string_impl(fxsh_type_t *type, sp_dyn_array(c8) * out,
                                sp_dyn_array(s32) * bound_vars) {
    if (!type) {
        const c8 *s = "<null>";
        for (u32 i = 0; s[i]; i++)
            sp_dyn_array_push(*out, s[i]);
        return;
    }

    switch (type->kind) {
        case TYPE_VAR: {
            s32 var_id = type->data.var;
            bool found = false;
            s32 name_idx = 0;
            sp_dyn_array_for(*bound_vars, i) {
                if ((*bound_vars)[i] == var_id) {
                    found = true;
                    name_idx = i;
                    break;
                }
            }
            if (!found) {
                name_idx = (s32)sp_dyn_array_size(*bound_vars);
                sp_dyn_array_push(*bound_vars, var_id);
            }
            sp_dyn_array_push(*out, '\'');
            if (name_idx < 26) {
                sp_dyn_array_push(*out, (c8)('a' + name_idx));
            } else {
                sp_dyn_array_push(*out, 't');
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", name_idx);
                for (c8 *p = buf; *p; p++)
                    sp_dyn_array_push(*out, *p);
            }
            break;
        }
        case TYPE_CON:
            for (u32 i = 0; i < type->data.con.len; i++)
                sp_dyn_array_push(*out, type->data.con.data[i]);
            break;
        case TYPE_ARROW: {
            bool needs_paren = (type->data.arrow.param->kind == TYPE_ARROW);
            if (needs_paren)
                sp_dyn_array_push(*out, '(');
            type_to_string_impl(type->data.arrow.param, out, bound_vars);
            if (needs_paren)
                sp_dyn_array_push(*out, ')');
            sp_dyn_array_push(*out, ' ');
            sp_dyn_array_push(*out, '-');
            sp_dyn_array_push(*out, '>');
            sp_dyn_array_push(*out, ' ');
            type_to_string_impl(type->data.arrow.ret, out, bound_vars);
            break;
        }
        case TYPE_TUPLE:
            sp_dyn_array_push(*out, '(');
            sp_dyn_array_for(type->data.tuple, i) {
                if (i > 0) {
                    sp_dyn_array_push(*out, ',');
                    sp_dyn_array_push(*out, ' ');
                }
                type_to_string_impl(type->data.tuple[i], out, bound_vars);
            }
            sp_dyn_array_push(*out, ')');
            break;
        case TYPE_RECORD:
            sp_dyn_array_push(*out, '{');
            sp_dyn_array_for(type->data.record.fields, i) {
                if (i > 0) {
                    sp_dyn_array_push(*out, ';');
                    sp_dyn_array_push(*out, ' ');
                }
                fxsh_field_t *f = &type->data.record.fields[i];
                for (u32 j = 0; j < f->name.len; j++)
                    sp_dyn_array_push(*out, f->name.data[j]);
                sp_dyn_array_push(*out, ':');
                sp_dyn_array_push(*out, ' ');
                type_to_string_impl(f->type, out, bound_vars);
            }
            if (type->data.record.row_var >= 0) {
                if (sp_dyn_array_size(type->data.record.fields) > 0)
                    sp_dyn_array_push(*out, ';');
                sp_dyn_array_push(*out, ' ');
                sp_dyn_array_push(*out, '.');
                sp_dyn_array_push(*out, '.');
                sp_dyn_array_push(*out, ' ');
                const c8 *n = var_name(type->data.record.row_var);
                while (*n)
                    sp_dyn_array_push(*out, *n++);
            }
            sp_dyn_array_push(*out, '}');
            break;
        case TYPE_APP:
            type_to_string_impl(type->data.app.arg, out, bound_vars);
            sp_dyn_array_push(*out, ' ');
            type_to_string_impl(type->data.app.con, out, bound_vars);
            break;
    }
}

const c8 *fxsh_type_to_string(fxsh_type_t *type) {
    static c8 *buf = NULL;
    /* Note: buf is arena-allocated on re-entry, safe for one-shot print */
    sp_dyn_array(c8) chars = SP_NULLPTR;
    sp_dyn_array(s32) bv = SP_NULLPTR;
    type_to_string_impl(type, &chars, &bv);
    sp_dyn_array_push(chars, '\0');
    /* Copy into arena so caller can hold it */
    buf = (c8 *)fxsh_alloc(sp_dyn_array_size(chars));
    memcpy(buf, chars, sp_dyn_array_size(chars));
    sp_dyn_array_free(chars);
    sp_dyn_array_free(bv);
    return buf;
}

/*=============================================================================
 * Free Type Variables
 *=============================================================================*/

static void ftv_impl(fxsh_type_t *type, sp_dyn_array(s32) * out) {
    if (!type)
        return;
    switch (type->kind) {
        case TYPE_VAR:
            sp_dyn_array_for(*out, i) {
                if ((*out)[i] == type->data.var)
                    return;
            }
            sp_dyn_array_push(*out, type->data.var);
            break;
        case TYPE_CON:
            break;
        case TYPE_ARROW:
            ftv_impl(type->data.arrow.param, out);
            ftv_impl(type->data.arrow.ret, out);
            break;
        case TYPE_TUPLE:
            sp_dyn_array_for(type->data.tuple, i) ftv_impl(type->data.tuple[i], out);
            break;
        case TYPE_RECORD:
            sp_dyn_array_for(type->data.record.fields, i)
                ftv_impl(type->data.record.fields[i].type, out);
            if (type->data.record.row_var >= 0) {
                bool seen = false;
                sp_dyn_array_for(*out, i) {
                    if ((*out)[i] == type->data.record.row_var) {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                    sp_dyn_array_push(*out, type->data.record.row_var);
            }
            break;
        case TYPE_APP:
            ftv_impl(type->data.app.con, out);
            ftv_impl(type->data.app.arg, out);
            break;
    }
}

/* Collect all free type variables from the entire environment */
static void free_vars_in_env(fxsh_type_env_t env, sp_dyn_array(s32) * out_vars) {
    fxsh_tenv_node_t *node = (fxsh_tenv_node_t *)env;
    while (node) {
        fxsh_scheme_t *s = node->scheme;
        if (s) {
            /* Get FTVs in scheme's type, then subtract bound vars */
            sp_dyn_array(s32) type_vars = SP_NULLPTR;
            ftv_impl(s->type, &type_vars);
            sp_dyn_array_for(type_vars, i) {
                bool is_bound = false;
                sp_dyn_array_for(s->vars, j) {
                    if (s->vars[j] == type_vars[i]) {
                        is_bound = true;
                        break;
                    }
                }
                if (!is_bound) {
                    bool already = false;
                    sp_dyn_array_for(*out_vars, k) {
                        if ((*out_vars)[k] == type_vars[i]) {
                            already = true;
                            break;
                        }
                    }
                    if (!already)
                        sp_dyn_array_push(*out_vars, type_vars[i]);
                }
            }
            sp_dyn_array_free(type_vars);
        }
        node = node->next;
    }
}

/*=============================================================================
 * Substitution
 *=============================================================================*/

static fxsh_type_t *apply_subst_single(fxsh_type_var_t var, fxsh_type_t *rep, fxsh_type_t *type) {
    if (!type)
        return NULL;
    switch (type->kind) {
        case TYPE_VAR:
            return (type->data.var == var) ? rep : type;
        case TYPE_CON:
            return type;
        case TYPE_ARROW: {
            fxsh_type_t *p = apply_subst_single(var, rep, type->data.arrow.param);
            fxsh_type_t *r = apply_subst_single(var, rep, type->data.arrow.ret);
            if (p == type->data.arrow.param && r == type->data.arrow.ret)
                return type;
            return fxsh_type_arrow(p, r);
        }
        case TYPE_TUPLE: {
            /* Reuse array if nothing changed */
            bool changed = false;
            sp_dyn_array(fxsh_type_t *) elems = SP_NULLPTR;
            sp_dyn_array_for(type->data.tuple, i) {
                fxsh_type_t *e = apply_subst_single(var, rep, type->data.tuple[i]);
                if (e != type->data.tuple[i])
                    changed = true;
                sp_dyn_array_push(elems, e);
            }
            if (!changed) {
                sp_dyn_array_free(elems);
                return type;
            }
            fxsh_type_t *nt = fxsh_type_var(0);
            nt->kind = TYPE_TUPLE;
            nt->data.tuple = elems;
            return nt;
        }
        case TYPE_APP: {
            fxsh_type_t *c = apply_subst_single(var, rep, type->data.app.con);
            fxsh_type_t *a = apply_subst_single(var, rep, type->data.app.arg);
            if (c == type->data.app.con && a == type->data.app.arg)
                return type;
            return fxsh_type_apply(c, a);
        }
        case TYPE_RECORD: {
            bool changed = false;
            sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
            sp_dyn_array_for(type->data.record.fields, i) {
                fxsh_type_t *ft = apply_subst_single(var, rep, type->data.record.fields[i].type);
                if (ft != type->data.record.fields[i].type)
                    changed = true;
                fxsh_field_t nf = {.name = type->data.record.fields[i].name, .type = ft};
                sp_dyn_array_push(fields, nf);
            }

            fxsh_type_var_t row_var = type->data.record.row_var;
            if (row_var == var) {
                changed = true;
                if (rep->kind == TYPE_VAR) {
                    row_var = rep->data.var;
                } else if (rep->kind == TYPE_RECORD) {
                    sp_dyn_array_for(rep->data.record.fields, i) {
                        if (record_field_index(fields, rep->data.record.fields[i].name) < 0)
                            sp_dyn_array_push(fields, rep->data.record.fields[i]);
                    }
                    row_var = rep->data.record.row_var;
                }
            }

            if (!changed) {
                sp_dyn_array_free(fields);
                return type;
            }
            return make_record_type(fields, row_var);
        }
        default:
            return type;
    }
}

void fxsh_type_apply_subst(fxsh_subst_t subst, fxsh_type_t **type) {
    if (!*type)
        return;
    sp_dyn_array_for(subst, i) *type = apply_subst_single(subst[i].var, subst[i].type, *type);
}

static fxsh_subst_t compose(fxsh_subst_t s1, fxsh_subst_t s2) {
    fxsh_subst_t result = SP_NULLPTR;
    /* Apply s1 to all types in s2 */
    sp_dyn_array_for(s2, i) {
        fxsh_subst_entry_t e = s2[i];
        fxsh_type_apply_subst(s1, &e.type);
        sp_dyn_array_push(result, e);
    }
    /* Append s1 entries that don't conflict */
    sp_dyn_array_for(s1, i) sp_dyn_array_push(result, s1[i]);
    return result;
}

/*=============================================================================
 * Occurs Check
 *=============================================================================*/

static bool occurs_in(fxsh_type_var_t var, fxsh_type_t *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case TYPE_VAR:
            return type->data.var == var;
        case TYPE_CON:
            return false;
        case TYPE_ARROW:
            return occurs_in(var, type->data.arrow.param) || occurs_in(var, type->data.arrow.ret);
        case TYPE_APP:
            return occurs_in(var, type->data.app.con) || occurs_in(var, type->data.app.arg);
        case TYPE_TUPLE:
            sp_dyn_array_for(type->data.tuple,
                             i) if (occurs_in(var, type->data.tuple[i])) return true;
            return false;
        case TYPE_RECORD:
            sp_dyn_array_for(type->data.record.fields,
                             i) if (occurs_in(var, type->data.record.fields[i].type)) return true;
            if (type->data.record.row_var == var)
                return true;
            return false;
    }
    return false;
}

/*=============================================================================
 * Unification
 *=============================================================================*/

fxsh_error_t fxsh_type_unify(fxsh_type_t *t1, fxsh_type_t *t2, fxsh_subst_t *out_subst) {
    if (!t1 || !t2)
        return ERR_TYPE_ERROR;

    /* Both same variable */
    if (t1->kind == TYPE_VAR && t2->kind == TYPE_VAR && t1->data.var == t2->data.var) {
        *out_subst = SP_NULLPTR;
        return ERR_OK;
    }

    /* Variable on left */
    if (t1->kind == TYPE_VAR) {
        if (occurs_in(t1->data.var, t2)) {
            fprintf(stderr, "Type error: occurs check failed\n");
            return ERR_TYPE_ERROR;
        }
        fxsh_subst_entry_t e = {.var = t1->data.var, .type = t2};
        sp_dyn_array_push(*out_subst, e);
        return ERR_OK;
    }

    /* Variable on right */
    if (t2->kind == TYPE_VAR) {
        if (occurs_in(t2->data.var, t1)) {
            fprintf(stderr, "Type error: occurs check failed\n");
            return ERR_TYPE_ERROR;
        }
        fxsh_subst_entry_t e = {.var = t2->data.var, .type = t1};
        sp_dyn_array_push(*out_subst, e);
        return ERR_OK;
    }

    /* Both constructors */
    if (t1->kind == TYPE_CON && t2->kind == TYPE_CON) {
        if (sp_str_equal(t1->data.con, t2->data.con)) {
            *out_subst = SP_NULLPTR;
            return ERR_OK;
        }
        fprintf(stderr, "Type error: cannot unify '%.*s' with '%.*s'\n", (int)t1->data.con.len,
                t1->data.con.data, (int)t2->data.con.len, t2->data.con.data);
        return ERR_TYPE_ERROR;
    }

    /* Both arrows */
    if (t1->kind == TYPE_ARROW && t2->kind == TYPE_ARROW) {
        fxsh_subst_t s1 = SP_NULLPTR;
        fxsh_error_t err = fxsh_type_unify(t1->data.arrow.param, t2->data.arrow.param, &s1);
        if (err != ERR_OK)
            return err;
        fxsh_type_t *r1 = t1->data.arrow.ret, *r2 = t2->data.arrow.ret;
        fxsh_type_apply_subst(s1, &r1);
        fxsh_type_apply_subst(s1, &r2);
        fxsh_subst_t s2 = SP_NULLPTR;
        err = fxsh_type_unify(r1, r2, &s2);
        if (err != ERR_OK)
            return err;
        *out_subst = compose(s2, s1);
        return ERR_OK;
    }

    /* Both applications */
    if (t1->kind == TYPE_APP && t2->kind == TYPE_APP) {
        fxsh_subst_t s1 = SP_NULLPTR;
        fxsh_error_t err = fxsh_type_unify(t1->data.app.con, t2->data.app.con, &s1);
        if (err != ERR_OK)
            return err;
        fxsh_type_t *a1 = t1->data.app.arg, *a2 = t2->data.app.arg;
        fxsh_type_apply_subst(s1, &a1);
        fxsh_type_apply_subst(s1, &a2);
        fxsh_subst_t s2 = SP_NULLPTR;
        err = fxsh_type_unify(a1, a2, &s2);
        if (err != ERR_OK)
            return err;
        *out_subst = compose(s2, s1);
        return ERR_OK;
    }

    if (t1->kind == TYPE_TUPLE && t2->kind == TYPE_TUPLE) {
        if (sp_dyn_array_size(t1->data.tuple) != sp_dyn_array_size(t2->data.tuple)) {
            fprintf(stderr, "Type error: tuple arity mismatch (%u vs %u)\n",
                    (unsigned)sp_dyn_array_size(t1->data.tuple),
                    (unsigned)sp_dyn_array_size(t2->data.tuple));
            return ERR_TYPE_ERROR;
        }
        fxsh_subst_t s_total = SP_NULLPTR;
        sp_dyn_array_for(t1->data.tuple, i) {
            fxsh_type_t *a = t1->data.tuple[i];
            fxsh_type_t *b = t2->data.tuple[i];
            fxsh_type_apply_subst(s_total, &a);
            fxsh_type_apply_subst(s_total, &b);
            fxsh_subst_t se = SP_NULLPTR;
            fxsh_error_t err = fxsh_type_unify(a, b, &se);
            if (err != ERR_OK)
                return err;
            s_total = compose(se, s_total);
        }
        *out_subst = s_total;
        return ERR_OK;
    }

    if (t1->kind == TYPE_RECORD && t2->kind == TYPE_RECORD) {
        fxsh_subst_t s_total = SP_NULLPTR;

        sp_dyn_array(fxsh_field_t) extra1 = SP_NULLPTR; /* in t1 only */
        sp_dyn_array(fxsh_field_t) extra2 = SP_NULLPTR; /* in t2 only */

        sp_dyn_array_for(t1->data.record.fields, i) {
            fxsh_field_t f1 = t1->data.record.fields[i];
            s32 j = record_field_index(t2->data.record.fields, f1.name);
            if (j < 0) {
                sp_dyn_array_push(extra1, f1);
                continue;
            }
            fxsh_type_t *a = f1.type;
            fxsh_type_t *b = t2->data.record.fields[j].type;
            fxsh_type_apply_subst(s_total, &a);
            fxsh_type_apply_subst(s_total, &b);
            fxsh_subst_t sf = SP_NULLPTR;
            fxsh_error_t err = fxsh_type_unify(a, b, &sf);
            if (err != ERR_OK)
                return err;
            s_total = compose(sf, s_total);
        }

        sp_dyn_array_for(t2->data.record.fields, i) {
            fxsh_field_t f2 = t2->data.record.fields[i];
            if (record_field_index(t1->data.record.fields, f2.name) < 0)
                sp_dyn_array_push(extra2, f2);
        }

        if (sp_dyn_array_size(extra1) > 0) {
            if (t2->data.record.row_var < 0) {
                fprintf(stderr, "Type error: record field mismatch\n");
                return ERR_TYPE_ERROR;
            }
            fxsh_type_t *tail = make_record_type(extra1, t1->data.record.row_var);
            if (occurs_in(t2->data.record.row_var, tail)) {
                fprintf(stderr, "Type error: occurs check failed\n");
                return ERR_TYPE_ERROR;
            }
            fxsh_subst_entry_t e = {.var = t2->data.record.row_var, .type = tail};
            fxsh_subst_t sr = SP_NULLPTR;
            sp_dyn_array_push(sr, e);
            s_total = compose(sr, s_total);
        }

        if (sp_dyn_array_size(extra2) > 0) {
            if (t1->data.record.row_var < 0) {
                fprintf(stderr, "Type error: record field mismatch\n");
                return ERR_TYPE_ERROR;
            }
            fxsh_type_t *tail = make_record_type(extra2, t2->data.record.row_var);
            if (occurs_in(t1->data.record.row_var, tail)) {
                fprintf(stderr, "Type error: occurs check failed\n");
                return ERR_TYPE_ERROR;
            }
            fxsh_subst_entry_t e = {.var = t1->data.record.row_var, .type = tail};
            fxsh_subst_t sr = SP_NULLPTR;
            sp_dyn_array_push(sr, e);
            s_total = compose(sr, s_total);
        }

        if (sp_dyn_array_size(extra1) == 0 && sp_dyn_array_size(extra2) == 0 &&
            t1->data.record.row_var >= 0 && t2->data.record.row_var >= 0) {
            if (t1->data.record.row_var != t2->data.record.row_var) {
                fxsh_subst_entry_t e = {.var = t1->data.record.row_var,
                                        .type = fxsh_type_var(t2->data.record.row_var)};
                fxsh_subst_t sr = SP_NULLPTR;
                sp_dyn_array_push(sr, e);
                s_total = compose(sr, s_total);
            }
        }

        if (sp_dyn_array_size(extra1) == 0 && sp_dyn_array_size(extra2) == 0 &&
            t1->data.record.row_var >= 0 && t2->data.record.row_var < 0) {
            fxsh_subst_entry_t e = {.var = t1->data.record.row_var,
                                    .type = make_record_type(SP_NULLPTR, -1)};
            fxsh_subst_t sr = SP_NULLPTR;
            sp_dyn_array_push(sr, e);
            s_total = compose(sr, s_total);
        }

        if (sp_dyn_array_size(extra1) == 0 && sp_dyn_array_size(extra2) == 0 &&
            t2->data.record.row_var >= 0 && t1->data.record.row_var < 0) {
            fxsh_subst_entry_t e = {.var = t2->data.record.row_var,
                                    .type = make_record_type(SP_NULLPTR, -1)};
            fxsh_subst_t sr = SP_NULLPTR;
            sp_dyn_array_push(sr, e);
            s_total = compose(sr, s_total);
        }

        *out_subst = s_total;
        return ERR_OK;
    }

    fprintf(stderr, "Type error: kind mismatch (%d vs %d)\n", t1->kind, t2->kind);
    return ERR_TYPE_ERROR;
}

/*=============================================================================
 * Scheme Instantiation / Generalization
 *=============================================================================*/

static fxsh_type_t *instantiate_impl(fxsh_type_t *type, type_var_map_t *var_map) {
    if (!type)
        return NULL;
    switch (type->kind) {
        case TYPE_VAR: {
            s32 *m = sp_ht_getp(*var_map, type->data.var);
            return m ? fxsh_type_var(*m) : type;
        }
        case TYPE_CON:
            return type;
        case TYPE_ARROW: {
            fxsh_type_t *p = instantiate_impl(type->data.arrow.param, var_map);
            fxsh_type_t *r = instantiate_impl(type->data.arrow.ret, var_map);
            return (p != type->data.arrow.param || r != type->data.arrow.ret)
                       ? fxsh_type_arrow(p, r)
                       : type;
        }
        case TYPE_APP: {
            fxsh_type_t *c = instantiate_impl(type->data.app.con, var_map);
            fxsh_type_t *a = instantiate_impl(type->data.app.arg, var_map);
            return (c != type->data.app.con || a != type->data.app.arg) ? fxsh_type_apply(c, a)
                                                                        : type;
        }
        case TYPE_RECORD: {
            bool changed = false;
            sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
            sp_dyn_array_for(type->data.record.fields, i) {
                fxsh_type_t *ft = instantiate_impl(type->data.record.fields[i].type, var_map);
                if (ft != type->data.record.fields[i].type)
                    changed = true;
                fxsh_field_t nf = {.name = type->data.record.fields[i].name, .type = ft};
                sp_dyn_array_push(fields, nf);
            }
            fxsh_type_var_t row_var = type->data.record.row_var;
            if (row_var >= 0) {
                s32 *m = sp_ht_getp(*var_map, row_var);
                if (m) {
                    row_var = *m;
                    changed = true;
                }
            }
            if (!changed) {
                sp_dyn_array_free(fields);
                return type;
            }
            return make_record_type(fields, row_var);
        }
        default:
            return type;
    }
}

static fxsh_type_t *instantiate(fxsh_scheme_t *scheme) {
    if (!scheme)
        return fxsh_type_var(fxsh_fresh_var());
    type_var_map_t var_map = SP_NULLPTR;
    sp_dyn_array_for(scheme->vars, i) {
        s32 fresh = fxsh_fresh_var();
        sp_ht_insert(var_map, scheme->vars[i], fresh);
    }
    fxsh_type_t *result = instantiate_impl(scheme->type, &var_map);
    sp_ht_free(var_map);
    return result;
}

/* Instantiate all free vars in a raw type (used for constructor types). */
static fxsh_type_t *instantiate_type(fxsh_type_t *type) {
    if (!type)
        return fxsh_type_var(fxsh_fresh_var());

    type_var_map_t var_map = SP_NULLPTR;
    sp_dyn_array(s32) fvs = SP_NULLPTR;
    ftv_impl(type, &fvs);
    sp_dyn_array_for(fvs, i) {
        sp_ht_insert(var_map, fvs[i], fxsh_fresh_var());
    }

    fxsh_type_t *result = instantiate_impl(type, &var_map);
    sp_dyn_array_free(fvs);
    sp_ht_free(var_map);
    return result;
}

static fxsh_scheme_t *generalize(fxsh_type_t *type, fxsh_type_env_t *env) {
    fxsh_scheme_t *scheme = (fxsh_scheme_t *)fxsh_alloc0(sizeof(fxsh_scheme_t));
    scheme->type = type;
    scheme->vars = SP_NULLPTR;

    sp_dyn_array(s32) type_vars = SP_NULLPTR;
    ftv_impl(type, &type_vars);

    sp_dyn_array(s32) env_vars = SP_NULLPTR;
    if (env)
        free_vars_in_env(*env, &env_vars);

    sp_dyn_array_for(type_vars, i) {
        bool in_env = false;
        sp_dyn_array_for(env_vars, j) {
            if (env_vars[j] == type_vars[i]) {
                in_env = true;
                break;
            }
        }
        if (!in_env)
            sp_dyn_array_push(scheme->vars, type_vars[i]);
    }

    sp_dyn_array_free(type_vars);
    sp_dyn_array_free(env_vars);
    return scheme;
}

/*=============================================================================
 * Constructor Registration
 *=============================================================================*/

static s32 lookup_tparam_var(sp_dyn_array(sp_str_t) names, sp_dyn_array(s32) vars, sp_str_t name) {
    sp_dyn_array_for(names, i) {
        if (sp_str_equal(names[i], name))
            return vars[i];
    }
    return -1;
}

static fxsh_type_t *ast_to_type_with_params(fxsh_ast_node_t *ast,
                                            sp_dyn_array(sp_str_t) param_names,
                                            sp_dyn_array(s32) param_vars) {
    if (!ast)
        return fxsh_type_con(TYPE_UNIT);
    switch (ast->kind) {
        case AST_IDENT: {
            sp_str_t n = ast->data.ident;
            if (n.len > 0 && n.data && n.data[0] == '\'') {
                s32 v = lookup_tparam_var(param_names, param_vars, n);
                if (v >= 0)
                    return fxsh_type_var(v);
                return fxsh_type_var(fxsh_fresh_var());
            }
            if (sp_str_equal(n, TYPE_INT))
                return fxsh_type_con(TYPE_INT);
            if (sp_str_equal(n, TYPE_BOOL))
                return fxsh_type_con(TYPE_BOOL);
            if (sp_str_equal(n, TYPE_FLOAT))
                return fxsh_type_con(TYPE_FLOAT);
            if (sp_str_equal(n, TYPE_STRING))
                return fxsh_type_con(TYPE_STRING);
            if (sp_str_equal(n, TYPE_UNIT))
                return fxsh_type_con(TYPE_UNIT);
            return fxsh_type_con(n);
        }
        case AST_TYPE_VAR: {
            sp_str_t n = ast->data.ident;
            if (n.len > 0 && n.data) {
                s32 v = lookup_tparam_var(param_names, param_vars, n);
                if (v >= 0)
                    return fxsh_type_var(v);
            }
            return fxsh_type_var(fxsh_fresh_var());
        }
        case AST_TYPE_APP: {
            if (!ast->data.type_con.args || sp_dyn_array_size(ast->data.type_con.args) != 2)
                return fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *lhs =
                ast_to_type_with_params(ast->data.type_con.args[0], param_names, param_vars);
            fxsh_type_t *rhs =
                ast_to_type_with_params(ast->data.type_con.args[1], param_names, param_vars);
            return fxsh_type_apply(rhs, lhs);
        }
        case AST_TYPE_ARROW:
            return fxsh_type_arrow(
                ast_to_type_with_params(ast->data.type_arrow.param, param_names, param_vars),
                ast_to_type_with_params(ast->data.type_arrow.ret, param_names, param_vars));
        case AST_TYPE_RECORD: {
            sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
            fxsh_type_var_t row_var = -1;
            sp_dyn_array_for(ast->data.elements, i) {
                fxsh_ast_node_t *e = ast->data.elements[i];
                if (!e)
                    continue;
                if (e->kind == AST_FIELD_ACCESS) {
                    fxsh_field_t f = {.name = e->data.field.field,
                                      .type = ast_to_type_with_params(e->data.field.object,
                                                                      param_names, param_vars)};
                    sp_dyn_array_push(fields, f);
                } else if (e->kind == AST_TYPE_VAR) {
                    row_var = fxsh_fresh_var();
                }
            }
            return make_record_type(fields, row_var);
        }
        default:
            return fxsh_type_var(fxsh_fresh_var());
    }
}

static fxsh_type_t *ast_to_type(fxsh_ast_node_t *ast) {
    return ast_to_type_with_params(ast, SP_NULLPTR, SP_NULLPTR);
}

static s32 record_field_index(sp_dyn_array(fxsh_field_t) fields, sp_str_t name) {
    sp_dyn_array_for(fields, i) {
        if (sp_str_equal(fields[i].name, name))
            return (s32)i;
    }
    return -1;
}

static fxsh_type_t *make_record_type(sp_dyn_array(fxsh_field_t) fields, fxsh_type_var_t row_var) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind = TYPE_RECORD;
    t->data.record.fields = fields;
    t->data.record.row_var = row_var;
    return t;
}

static fxsh_type_t *make_list_type(fxsh_type_t *elem_t) {
    return fxsh_type_apply(fxsh_type_con(TYPE_LIST), elem_t);
}

static fxsh_type_t *make_constr_type(fxsh_ast_node_t *data_constr, fxsh_ast_list_t type_params,
                                     sp_str_t type_name) {
    /* Result type: TypeName applied to its type params */
    sp_dyn_array(sp_str_t) param_names = SP_NULLPTR;
    sp_dyn_array(s32) param_vars = SP_NULLPTR;
    fxsh_type_t *result = fxsh_type_con(type_name);
    sp_dyn_array_for(type_params, i) {
        fxsh_ast_node_t *p = type_params[i];
        s32 tv = fxsh_fresh_var();
        sp_str_t pname = (sp_str_t){0};
        if (p && p->kind == AST_TYPE_VAR)
            pname = p->data.ident;
        sp_dyn_array_push(param_names, pname);
        sp_dyn_array_push(param_vars, tv);
        fxsh_type_t *v = fxsh_type_var(tv);
        result = fxsh_type_apply(result, v);
    }

    /* Build arrow type right-to-left */
    fxsh_type_t *constr_type = result;
    fxsh_ast_list_t arg_types = data_constr->data.data_constr.arg_types;
    for (s32 i = (s32)sp_dyn_array_size(arg_types) - 1; i >= 0; i--) {
        fxsh_type_t *arg_t = ast_to_type_with_params(arg_types[i], param_names, param_vars);
        if (!arg_t)
            arg_t = fxsh_type_var(fxsh_fresh_var());
        constr_type = fxsh_type_arrow(arg_t, constr_type);
    }
    sp_dyn_array_free(param_names);
    sp_dyn_array_free(param_vars);
    return constr_type;
}

void fxsh_register_type_constrs(fxsh_ast_node_t *type_def, fxsh_constr_env_t *constr_env) {
    if (!type_def || type_def->kind != AST_TYPE_DEF || !constr_env)
        return;
    sp_str_t type_name = type_def->data.type_def.name;
    fxsh_ast_list_t constructors = type_def->data.type_def.constructors;
    fxsh_ast_list_t type_params = type_def->data.type_def.type_params;

    sp_dyn_array_for(constructors, i) {
        fxsh_ast_node_t *constr = constructors[i];
        if (constr->kind != AST_DATA_CONSTR)
            continue;
        fxsh_constr_info_t info = {
            .constr_name = constr->data.data_constr.name,
            .type_name = type_name,
            .constr_type = make_constr_type(constr, type_params, type_name),
            .arity = (s32)sp_dyn_array_size(constr->data.data_constr.arg_types),
        };
        constr_env_bind(constr_env, constr->data.data_constr.name, &info);
    }
}

/*=============================================================================
 * Type Inference
 *=============================================================================*/

static fxsh_error_t infer_expr(fxsh_ast_node_t *ast, fxsh_type_env_t *env,
                               fxsh_constr_env_t *constr_env, fxsh_subst_t *subst,
                               fxsh_type_t **out_type);

static fxsh_scheme_t *mk_mono_scheme(fxsh_type_t *t) {
    fxsh_scheme_t *sc = (fxsh_scheme_t *)fxsh_alloc0(sizeof(fxsh_scheme_t));
    sc->type = t;
    sc->vars = SP_NULLPTR;
    return sc;
}

static void ensure_builtin_env(fxsh_type_env_t *env) {
    if (!env)
        return;
    if (!type_env_lookup(*env, sp_str_lit("print"))) {
        type_env_bind(
            env, sp_str_lit("print"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_UNIT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("getenv"))) {
        type_env_bind(env, sp_str_lit("getenv"),
                      mk_mono_scheme(
                          fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("file_exists"))) {
        type_env_bind(
            env, sp_str_lit("file_exists"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_BOOL))));
    }
    if (!type_env_lookup(*env, sp_str_lit("read_file"))) {
        type_env_bind(env, sp_str_lit("read_file"),
                      mk_mono_scheme(
                          fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("write_file"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_BOOL)));
        type_env_bind(env, sp_str_lit("write_file"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec"))) {
        type_env_bind(
            env, sp_str_lit("exec"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_code"))) {
        type_env_bind(
            env, sp_str_lit("exec_code"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_stdout"))) {
        type_env_bind(env, sp_str_lit("exec_stdout"),
                      mk_mono_scheme(
                          fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_stderr"))) {
        type_env_bind(env, sp_str_lit("exec_stderr"),
                      mk_mono_scheme(
                          fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_capture"))) {
        type_env_bind(
            env, sp_str_lit("exec_capture"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("capture_code"))) {
        type_env_bind(
            env, sp_str_lit("capture_code"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("capture_stdout"))) {
        type_env_bind(
            env, sp_str_lit("capture_stdout"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("capture_stderr"))) {
        type_env_bind(
            env, sp_str_lit("capture_stderr"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("capture_release"))) {
        type_env_bind(
            env, sp_str_lit("capture_release"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_BOOL))));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_stdin"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING)));
        type_env_bind(env, sp_str_lit("exec_stdin"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_stdin_code"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT)));
        type_env_bind(env, sp_str_lit("exec_stdin_code"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_stdin_capture"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT)));
        type_env_bind(env, sp_str_lit("exec_stdin_capture"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_stdin_stderr"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING)));
        type_env_bind(env, sp_str_lit("exec_stdin_stderr"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_pipe"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING)));
        type_env_bind(env, sp_str_lit("exec_pipe"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_pipe_code"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT)));
        type_env_bind(env, sp_str_lit("exec_pipe_code"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_pipe_capture"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT)));
        type_env_bind(env, sp_str_lit("exec_pipe_capture"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_pipefail_capture"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT)));
        type_env_bind(env, sp_str_lit("exec_pipefail_capture"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_pipefail3_capture"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT))));
        type_env_bind(env, sp_str_lit("exec_pipefail3_capture"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_pipefail4_capture"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                                                            fxsh_type_con(TYPE_INT)))));
        type_env_bind(env, sp_str_lit("exec_pipefail4_capture"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("exec_pipe_stderr"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING)));
        type_env_bind(env, sp_str_lit("exec_pipe_stderr"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("glob"))) {
        type_env_bind(env, sp_str_lit("glob"),
                      mk_mono_scheme(
                          fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("grep_lines"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING)));
        type_env_bind(env, sp_str_lit("grep_lines"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("replace_once"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                                                            fxsh_type_con(TYPE_STRING))));
        type_env_bind(env, sp_str_lit("replace_once"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_validate"))) {
        type_env_bind(
            env, sp_str_lit("json_validate"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_BOOL))));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_compact"))) {
        type_env_bind(env, sp_str_lit("json_compact"),
                      mk_mono_scheme(
                          fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING))));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_has"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_BOOL)));
        type_env_bind(env, sp_str_lit("json_has"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_get"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING)));
        type_env_bind(env, sp_str_lit("json_get"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_get_string"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_STRING),
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_STRING)));
        type_env_bind(env, sp_str_lit("json_get_string"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_get_int"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_INT)));
        type_env_bind(env, sp_str_lit("json_get_int"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_get_float"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_FLOAT)));
        type_env_bind(env, sp_str_lit("json_get_float"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("json_get_bool"))) {
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_STRING),
                            fxsh_type_arrow(fxsh_type_con(TYPE_STRING), fxsh_type_con(TYPE_BOOL)));
        type_env_bind(env, sp_str_lit("json_get_bool"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_null"))) {
        s32 a = fxsh_fresh_var();
        fxsh_scheme_t *sc = (fxsh_scheme_t *)fxsh_alloc0(sizeof(fxsh_scheme_t));
        sc->vars = SP_NULLPTR;
        sp_dyn_array_push(sc->vars, a);
        sc->type = fxsh_type_arrow(fxsh_type_con(TYPE_UNIT),
                                   fxsh_type_apply(fxsh_type_con(TYPE_PTR), fxsh_type_var(a)));
        type_env_bind(env, sp_str_lit("c_null"), sc);
    }
    if (!type_env_lookup(*env, sp_str_lit("c_malloc"))) {
        fxsh_type_t *ret_ptr = fxsh_type_apply(fxsh_type_con(TYPE_PTR), fxsh_type_con(TYPE_UNIT));
        type_env_bind(env, sp_str_lit("c_malloc"),
                      mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), ret_ptr)));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_free"))) {
        fxsh_type_t *arg_ptr = fxsh_type_apply(fxsh_type_con(TYPE_PTR), fxsh_type_con(TYPE_UNIT));
        type_env_bind(env, sp_str_lit("c_free"),
                      mk_mono_scheme(fxsh_type_arrow(arg_ptr, fxsh_type_con(TYPE_UNIT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_cast_ptr"))) {
        s32 a = fxsh_fresh_var();
        s32 b = fxsh_fresh_var();
        fxsh_scheme_t *sc = (fxsh_scheme_t *)fxsh_alloc0(sizeof(fxsh_scheme_t));
        sc->vars = SP_NULLPTR;
        sp_dyn_array_push(sc->vars, a);
        sp_dyn_array_push(sc->vars, b);
        fxsh_type_t *in_ptr = fxsh_type_apply(fxsh_type_con(TYPE_PTR), fxsh_type_var(a));
        fxsh_type_t *out_ptr = fxsh_type_apply(fxsh_type_con(TYPE_PTR), fxsh_type_var(b));
        sc->type = fxsh_type_arrow(in_ptr, out_ptr);
        type_env_bind(env, sp_str_lit("c_cast_ptr"), sc);
    }
    if (!type_env_lookup(*env, sp_str_lit("c_callback0"))) {
        fxsh_type_t *cb_t = fxsh_type_arrow(fxsh_type_con(TYPE_UNIT), fxsh_type_con(TYPE_UNIT));
        fxsh_type_t *ret_ptr = fxsh_type_apply(fxsh_type_con(TYPE_PTR), fxsh_type_con(TYPE_UNIT));
        type_env_bind(env, sp_str_lit("c_callback0"),
                      mk_mono_scheme(fxsh_type_arrow(cb_t, ret_ptr)));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_callback1_ptr"))) {
        fxsh_type_t *ptr_t = fxsh_type_apply(fxsh_type_con(TYPE_PTR), fxsh_type_con(TYPE_UNIT));
        fxsh_type_t *cb_t = fxsh_type_arrow(ptr_t, fxsh_type_con(TYPE_UNIT));
        type_env_bind(env, sp_str_lit("c_callback1_ptr"),
                      mk_mono_scheme(fxsh_type_arrow(cb_t, ptr_t)));
    }
    if (!type_env_lookup(*env, sp_str_lit("int_to_c_int"))) {
        type_env_bind(
            env, sp_str_lit("int_to_c_int"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_C_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_int_to_int"))) {
        type_env_bind(
            env, sp_str_lit("c_int_to_int"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_C_INT), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("int_to_c_uint"))) {
        type_env_bind(
            env, sp_str_lit("int_to_c_uint"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_C_UINT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_uint_to_int"))) {
        type_env_bind(
            env, sp_str_lit("c_uint_to_int"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_C_UINT), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("int_to_c_long"))) {
        type_env_bind(
            env, sp_str_lit("int_to_c_long"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_C_LONG))));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_long_to_int"))) {
        type_env_bind(
            env, sp_str_lit("c_long_to_int"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_C_LONG), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("int_to_c_ulong"))) {
        type_env_bind(
            env, sp_str_lit("int_to_c_ulong"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_C_ULONG))));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_ulong_to_int"))) {
        type_env_bind(
            env, sp_str_lit("c_ulong_to_int"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_C_ULONG), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("int_to_c_size"))) {
        type_env_bind(
            env, sp_str_lit("int_to_c_size"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_C_SIZE))));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_size_to_int"))) {
        type_env_bind(
            env, sp_str_lit("c_size_to_int"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_C_SIZE), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("int_to_c_ssize"))) {
        type_env_bind(
            env, sp_str_lit("int_to_c_ssize"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_C_SSIZE))));
    }
    if (!type_env_lookup(*env, sp_str_lit("c_ssize_to_int"))) {
        type_env_bind(
            env, sp_str_lit("c_ssize_to_int"),
            mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_C_SSIZE), fxsh_type_con(TYPE_INT))));
    }
    if (!type_env_lookup(*env, sp_str_lit("tensor_new2"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_INT),
            fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_arrow(fxsh_type_con(TYPE_FLOAT),
                                                                     fxsh_type_con(TYPE_TENSOR))));
        type_env_bind(env, sp_str_lit("tensor_new2"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("tensor_from_list2"))) {
        fxsh_type_t *flist = fxsh_type_apply(fxsh_type_con(TYPE_LIST), fxsh_type_con(TYPE_FLOAT));
        fxsh_type_t *t =
            fxsh_type_arrow(fxsh_type_con(TYPE_INT),
                            fxsh_type_arrow(fxsh_type_con(TYPE_INT),
                                            fxsh_type_arrow(flist, fxsh_type_con(TYPE_TENSOR))));
        type_env_bind(env, sp_str_lit("tensor_from_list2"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("tensor_shape2"))) {
        fxsh_type_t *tt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
        tt->kind = TYPE_TUPLE;
        tt->data.tuple = SP_NULLPTR;
        sp_dyn_array_push(tt->data.tuple, fxsh_type_con(TYPE_INT));
        sp_dyn_array_push(tt->data.tuple, fxsh_type_con(TYPE_INT));
        type_env_bind(env, sp_str_lit("tensor_shape2"),
                      mk_mono_scheme(fxsh_type_arrow(fxsh_type_con(TYPE_TENSOR), tt)));
    }
    if (!type_env_lookup(*env, sp_str_lit("tensor_get2"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_TENSOR),
            fxsh_type_arrow(fxsh_type_con(TYPE_INT),
                            fxsh_type_arrow(fxsh_type_con(TYPE_INT), fxsh_type_con(TYPE_FLOAT))));
        type_env_bind(env, sp_str_lit("tensor_get2"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("tensor_set2"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_TENSOR),
            fxsh_type_arrow(fxsh_type_con(TYPE_INT),
                            fxsh_type_arrow(fxsh_type_con(TYPE_INT),
                                            fxsh_type_arrow(fxsh_type_con(TYPE_FLOAT),
                                                            fxsh_type_con(TYPE_TENSOR)))));
        type_env_bind(env, sp_str_lit("tensor_set2"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("tensor_add"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_TENSOR),
            fxsh_type_arrow(fxsh_type_con(TYPE_TENSOR), fxsh_type_con(TYPE_TENSOR)));
        type_env_bind(env, sp_str_lit("tensor_add"), mk_mono_scheme(t));
    }
    if (!type_env_lookup(*env, sp_str_lit("tensor_dot"))) {
        fxsh_type_t *t = fxsh_type_arrow(
            fxsh_type_con(TYPE_TENSOR),
            fxsh_type_arrow(fxsh_type_con(TYPE_TENSOR), fxsh_type_con(TYPE_TENSOR)));
        type_env_bind(env, sp_str_lit("tensor_dot"), mk_mono_scheme(t));
    }
}

static void ensure_builtin_constr_env(fxsh_constr_env_t *constr_env) {
    if (constr_env_lookup(*constr_env, sp_str_lit("Some")) == NULL) {
        fxsh_type_var_t a = fxsh_fresh_var();
        fxsh_type_t *ta = fxsh_type_var(a);
        fxsh_type_t *opt_a = fxsh_type_apply(fxsh_type_con(sp_str_lit("option")), ta);
        fxsh_constr_info_t info = {.constr_name = sp_str_lit("Some"),
                                   .type_name = sp_str_lit("option"),
                                   .constr_type = fxsh_type_arrow(ta, opt_a),
                                   .arity = 1};
        constr_env_bind(constr_env, sp_str_lit("Some"), &info);
    }
    if (constr_env_lookup(*constr_env, sp_str_lit("None")) == NULL) {
        fxsh_type_var_t a = fxsh_fresh_var();
        fxsh_type_t *ta = fxsh_type_var(a);
        fxsh_type_t *opt_a = fxsh_type_apply(fxsh_type_con(sp_str_lit("option")), ta);
        fxsh_constr_info_t info = {.constr_name = sp_str_lit("None"),
                                   .type_name = sp_str_lit("option"),
                                   .constr_type = opt_a,
                                   .arity = 0};
        constr_env_bind(constr_env, sp_str_lit("None"), &info);
    }
    if (constr_env_lookup(*constr_env, sp_str_lit("Ok")) == NULL) {
        fxsh_type_var_t a = fxsh_fresh_var();
        fxsh_type_t *ta = fxsh_type_var(a);
        fxsh_type_t *res_a = fxsh_type_apply(fxsh_type_con(sp_str_lit("result")), ta);
        fxsh_constr_info_t info = {.constr_name = sp_str_lit("Ok"),
                                   .type_name = sp_str_lit("result"),
                                   .constr_type = fxsh_type_arrow(ta, res_a),
                                   .arity = 1};
        constr_env_bind(constr_env, sp_str_lit("Ok"), &info);
    }
    if (constr_env_lookup(*constr_env, sp_str_lit("Err")) == NULL) {
        fxsh_type_var_t a = fxsh_fresh_var();
        fxsh_type_t *ta = fxsh_type_var(a);
        fxsh_type_t *res_a = fxsh_type_apply(fxsh_type_con(sp_str_lit("result")), ta);
        fxsh_constr_info_t info = {.constr_name = sp_str_lit("Err"),
                                   .type_name = sp_str_lit("result"),
                                   .constr_type =
                                       fxsh_type_arrow(fxsh_type_con(TYPE_STRING), res_a),
                                   .arity = 1};
        constr_env_bind(constr_env, sp_str_lit("Err"), &info);
    }
}

static fxsh_error_t infer_pattern(fxsh_ast_node_t *pattern, fxsh_type_env_t *env,
                                  fxsh_constr_env_t *constr_env, fxsh_subst_t *subst,
                                  fxsh_type_t **out_type) {
    if (!pattern) {
        *out_type = fxsh_type_var(fxsh_fresh_var());
        return ERR_OK;
    }

    switch (pattern->kind) {
        case AST_PAT_WILD:
            *out_type = fxsh_type_var(fxsh_fresh_var());
            return ERR_OK;

        case AST_PAT_VAR: {
            fxsh_type_t *v = fxsh_type_var(fxsh_fresh_var());
            fxsh_scheme_t *sc = (fxsh_scheme_t *)fxsh_alloc0(sizeof(fxsh_scheme_t));
            sc->type = v;
            sc->vars = SP_NULLPTR;
            type_env_bind(env, pattern->data.ident, sc);
            *out_type = v;
            return ERR_OK;
        }

        case AST_PAT_LIT:
            /* AST_PAT_LIT is a re-tagged literal node.
             * We detect the concrete literal type by inspecting the union.
             * Since we don't store a sub-kind field, we rely on the original
             * AST kind stored before the re-tag.  For now, a safe fallback:
             * if the node has lit_int data use int, etc.
             * TODO: add pat_lit_kind field to fxsh_ast_node_t */
            *out_type = fxsh_type_con(TYPE_INT); /* conservative default */
            return ERR_OK;

        case AST_LIT_INT:
            *out_type = fxsh_type_con(TYPE_INT);
            return ERR_OK;
        case AST_LIT_FLOAT:
            *out_type = fxsh_type_con(TYPE_FLOAT);
            return ERR_OK;
        case AST_LIT_STRING:
            *out_type = fxsh_type_con(TYPE_STRING);
            return ERR_OK;
        case AST_LIT_BOOL:
            *out_type = fxsh_type_con(TYPE_BOOL);
            return ERR_OK;
        case AST_LIT_UNIT:
            *out_type = fxsh_type_con(TYPE_UNIT);
            return ERR_OK;

        case AST_PAT_TUPLE: {
            sp_dyn_array(fxsh_type_t *) elem_types = SP_NULLPTR;
            sp_dyn_array_for(pattern->data.elements, i) {
                fxsh_type_t *et = NULL;
                fxsh_error_t err =
                    infer_pattern(pattern->data.elements[i], env, constr_env, subst, &et);
                if (err != ERR_OK)
                    return err;
                sp_dyn_array_push(elem_types, et);
            }
            fxsh_type_t *tt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
            tt->kind = TYPE_TUPLE;
            tt->data.tuple = elem_types;
            *out_type = tt;
            return ERR_OK;
        }

        case AST_LIST: {
            fxsh_type_t *elem_t = fxsh_type_var(fxsh_fresh_var());
            sp_dyn_array_for(pattern->data.elements, i) {
                fxsh_type_t *et = NULL;
                fxsh_error_t err =
                    infer_pattern(pattern->data.elements[i], env, constr_env, subst, &et);
                if (err != ERR_OK)
                    return err;
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(et, elem_t, &s);
                if (err != ERR_OK)
                    return err;
                *subst = compose(s, *subst);
                fxsh_type_apply_subst(*subst, &elem_t);
            }
            *out_type = make_list_type(elem_t);
            return ERR_OK;
        }

        case AST_PAT_CONS: {
            if (!pattern->data.elements || sp_dyn_array_size(pattern->data.elements) != 2) {
                *out_type = make_list_type(fxsh_type_var(fxsh_fresh_var()));
                return ERR_OK;
            }
            fxsh_type_t *ht = NULL, *tt = NULL;
            fxsh_error_t err =
                infer_pattern(pattern->data.elements[0], env, constr_env, subst, &ht);
            if (err != ERR_OK)
                return err;
            err = infer_pattern(pattern->data.elements[1], env, constr_env, subst, &tt);
            if (err != ERR_OK)
                return err;
            fxsh_type_t *list_ht = make_list_type(ht);
            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(tt, list_ht, &s);
            if (err != ERR_OK)
                return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(*subst, &list_ht);
            *out_type = list_ht;
            return ERR_OK;
        }

        case AST_PAT_RECORD: {
            sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
            sp_dyn_array_for(pattern->data.elements, i) {
                fxsh_ast_node_t *f = pattern->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    continue;
                fxsh_type_t *ft = NULL;
                fxsh_error_t err = infer_pattern(f->data.field.object, env, constr_env, subst, &ft);
                if (err)
                    return err;
                fxsh_field_t rf = {.name = f->data.field.field, .type = ft};
                sp_dyn_array_push(fields, rf);
            }
            *out_type = make_record_type(fields, fxsh_fresh_var());
            return ERR_OK;
        }

        case AST_PAT_CONSTR: {
            fxsh_constr_info_t *ci =
                constr_env_lookup(*constr_env, pattern->data.constr_appl.constr_name);
            if (!ci) {
                *out_type = fxsh_type_var(fxsh_fresh_var());
                return ERR_OK;
            }

            fxsh_type_t *ct = instantiate_type(ci->constr_type);
            fxsh_type_t *res_t = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp_t = res_t;
            fxsh_ast_list_t args = pattern->data.constr_appl.args;
            for (s32 i = (s32)sp_dyn_array_size(args) - 1; i >= 0; i--) {
                fxsh_type_t *at = NULL;
                infer_pattern(args[i], env, constr_env, subst, &at);
                exp_t = fxsh_type_arrow(at, exp_t);
            }
            fxsh_subst_t s = SP_NULLPTR;
            fxsh_error_t err = fxsh_type_unify(ct, exp_t, &s);
            if (err != ERR_OK)
                return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res_t);
            *out_type = res_t;
            return ERR_OK;
        }

        default:
            *out_type = fxsh_type_var(fxsh_fresh_var());
            return ERR_OK;
    }
}

static fxsh_error_t infer_expr(fxsh_ast_node_t *ast, fxsh_type_env_t *env,
                               fxsh_constr_env_t *constr_env, fxsh_subst_t *subst,
                               fxsh_type_t **out_type) {
    if (!ast) {
        *out_type = fxsh_type_con(TYPE_UNIT);
        return ERR_OK;
    }

    switch (ast->kind) {
        case AST_LIT_INT:
            *out_type = fxsh_type_con(TYPE_INT);
            return ERR_OK;
        case AST_LIT_FLOAT:
            *out_type = fxsh_type_con(TYPE_FLOAT);
            return ERR_OK;
        case AST_LIT_STRING:
            *out_type = fxsh_type_con(TYPE_STRING);
            return ERR_OK;
        case AST_LIT_BOOL:
            *out_type = fxsh_type_con(TYPE_BOOL);
            return ERR_OK;
        case AST_LIT_UNIT:
            *out_type = fxsh_type_con(TYPE_UNIT);
            return ERR_OK;
        case AST_TYPE_VALUE:
            *out_type = fxsh_type_con(TYPE_TYPE);
            return ERR_OK;

        case AST_IDENT: {
            fxsh_scheme_t *sc = type_env_lookup(*env, ast->data.ident);
            if (!sc) {
                /* Unknown identifier — assign fresh type var */
                *out_type = fxsh_type_var(fxsh_fresh_var());
                return ERR_OK;
            }
            *out_type = instantiate(sc);
            return ERR_OK;
        }

        case AST_CT_TYPE_OF:
            *out_type = fxsh_type_con(TYPE_TYPE);
            return ERR_OK;
        case AST_CT_TYPE_NAME:
            *out_type = fxsh_type_con(TYPE_STRING);
            return ERR_OK;
        case AST_CT_SIZE_OF:
        case AST_CT_ALIGN_OF:
            *out_type = fxsh_type_con(TYPE_INT);
            return ERR_OK;
        case AST_CT_HAS_FIELD:
        case AST_CT_IS_RECORD:
        case AST_CT_IS_TUPLE:
            *out_type = fxsh_type_con(TYPE_BOOL);
            return ERR_OK;
        case AST_CT_FIELDS_OF:
            *out_type = make_list_type(fxsh_type_con(TYPE_STRING));
            return ERR_OK;
        case AST_CT_JSON_SCHEMA:
            *out_type = fxsh_type_con(TYPE_STRING);
            return ERR_OK;
        case AST_CT_CTOR_APPLY:
            *out_type = fxsh_type_con(TYPE_TYPE);
            return ERR_OK;
        case AST_CT_QUOTE:
            *out_type = fxsh_type_con(TYPE_STRING);
            return ERR_OK;
        case AST_CT_UNQUOTE:
        case AST_CT_SPLICE:
            *out_type = fxsh_type_var(fxsh_fresh_var());
            return ERR_OK;
        case AST_CT_EVAL:
            return infer_expr(ast->data.ct_type_of.operand, env, constr_env, subst, out_type);
        case AST_CT_COMPILE_LOG:
            *out_type = fxsh_type_con(TYPE_UNIT);
            return ERR_OK;
        case AST_CT_COMPILE_ERROR:
        case AST_CT_PANIC:
            *out_type = fxsh_type_var(fxsh_fresh_var());
            return ERR_OK;

        case AST_BINARY: {
            fxsh_type_t *lt = NULL, *rt = NULL;
            fxsh_error_t err;
            err = infer_expr(ast->data.binary.left, env, constr_env, subst, &lt);
            if (err)
                return err;
            err = infer_expr(ast->data.binary.right, env, constr_env, subst, &rt);
            if (err)
                return err;

            switch (ast->data.binary.op) {
                case TOK_APPEND: {
                    fxsh_type_t *list_lt = make_list_type(lt);
                    fxsh_subst_t s = SP_NULLPTR;
                    err = fxsh_type_unify(rt, list_lt, &s);
                    if (err)
                        return err;
                    *subst = compose(s, *subst);
                    fxsh_type_apply_subst(s, &list_lt);
                    *out_type = list_lt;
                    return ERR_OK;
                }
                case TOK_CONCAT: {
                    fxsh_type_t *st = fxsh_type_con(TYPE_STRING);
                    fxsh_subst_t s1 = SP_NULLPTR, s2 = SP_NULLPTR;
                    err = fxsh_type_unify(lt, st, &s1);
                    if (err)
                        return err;
                    fxsh_type_apply_subst(s1, &rt);
                    *subst = compose(s1, *subst);
                    err = fxsh_type_unify(rt, st, &s2);
                    if (err)
                        return err;
                    *subst = compose(s2, *subst);
                    *out_type = st;
                    return ERR_OK;
                }
                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT: {
                    fxsh_type_t *it = fxsh_type_con(TYPE_INT);
                    fxsh_subst_t s1 = SP_NULLPTR, s2 = SP_NULLPTR;
                    err = fxsh_type_unify(lt, it, &s1);
                    if (err)
                        return err;
                    fxsh_type_apply_subst(s1, &rt);
                    *subst = compose(s1, *subst);
                    err = fxsh_type_unify(rt, it, &s2);
                    if (err)
                        return err;
                    *subst = compose(s2, *subst);
                    *out_type = it;
                    return ERR_OK;
                }
                case TOK_EQ:
                case TOK_NEQ:
                case TOK_LT:
                case TOK_GT:
                case TOK_LEQ:
                case TOK_GEQ: {
                    fxsh_subst_t s = SP_NULLPTR;
                    err = fxsh_type_unify(lt, rt, &s);
                    if (err)
                        return err;
                    *subst = compose(s, *subst);
                    *out_type = fxsh_type_con(TYPE_BOOL);
                    return ERR_OK;
                }
                case TOK_AND:
                case TOK_OR: {
                    fxsh_type_t *bt = fxsh_type_con(TYPE_BOOL);
                    fxsh_subst_t s1 = SP_NULLPTR, s2 = SP_NULLPTR;
                    err = fxsh_type_unify(lt, bt, &s1);
                    if (err)
                        return err;
                    fxsh_type_apply_subst(s1, &rt);
                    *subst = compose(s1, *subst);
                    err = fxsh_type_unify(rt, bt, &s2);
                    if (err)
                        return err;
                    *subst = compose(s2, *subst);
                    *out_type = bt;
                    return ERR_OK;
                }
                default:
                    return ERR_TYPE_ERROR;
            }
        }

        case AST_UNARY: {
            fxsh_type_t *ot = NULL;
            fxsh_error_t err = infer_expr(ast->data.unary.operand, env, constr_env, subst, &ot);
            if (err)
                return err;
            if (ast->data.unary.op == TOK_MINUS) {
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(ot, fxsh_type_con(TYPE_INT), &s);
                if (err)
                    return err;
                *subst = compose(s, *subst);
                *out_type = fxsh_type_con(TYPE_INT);
            } else if (ast->data.unary.op == TOK_NOT) {
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(ot, fxsh_type_con(TYPE_BOOL), &s);
                if (err)
                    return err;
                *subst = compose(s, *subst);
                *out_type = fxsh_type_con(TYPE_BOOL);
            } else
                return ERR_TYPE_ERROR;
            return ERR_OK;
        }

        case AST_IF: {
            fxsh_type_t *ct = NULL, *tt = NULL, *et = NULL;
            fxsh_error_t err;
            err = infer_expr(ast->data.if_expr.cond, env, constr_env, subst, &ct);
            if (err)
                return err;
            err = infer_expr(ast->data.if_expr.then_branch, env, constr_env, subst, &tt);
            if (err)
                return err;
            fxsh_subst_t sc = SP_NULLPTR;
            err = fxsh_type_unify(ct, fxsh_type_con(TYPE_BOOL), &sc);
            if (err)
                return err;
            *subst = compose(sc, *subst);
            if (ast->data.if_expr.else_branch) {
                err = infer_expr(ast->data.if_expr.else_branch, env, constr_env, subst, &et);
                if (err)
                    return err;
                fxsh_subst_t se = SP_NULLPTR;
                err = fxsh_type_unify(tt, et, &se);
                if (err)
                    return err;
                *subst = compose(se, *subst);
                fxsh_type_apply_subst(se, &tt);
            }
            *out_type = tt;
            return ERR_OK;
        }

        case AST_LAMBDA: {
            fxsh_type_env_t new_env = *env;
            sp_dyn_array(fxsh_type_t *) param_types = SP_NULLPTR;
            sp_dyn_array_for(ast->data.lambda.params, i) {
                fxsh_type_t *pt = NULL;
                fxsh_error_t err =
                    infer_pattern(ast->data.lambda.params[i], &new_env, constr_env, subst, &pt);
                if (err)
                    return err;
                sp_dyn_array_push(param_types, pt);
            }
            fxsh_type_t *bt = NULL;
            fxsh_error_t err = infer_expr(ast->data.lambda.body, &new_env, constr_env, subst, &bt);
            if (err)
                return err;
            sp_dyn_array_for(param_types, i) {
                fxsh_type_apply_subst(*subst, &param_types[i]);
            }
            fxsh_type_apply_subst(*subst, &bt);
            fxsh_type_t *rt = bt;
            for (s32 i = (s32)sp_dyn_array_size(param_types) - 1; i >= 0; i--)
                rt = fxsh_type_arrow(param_types[i], rt);
            *out_type = rt;
            return ERR_OK;
        }

        case AST_CALL: {
            fxsh_type_t *ft = NULL;
            fxsh_error_t err = infer_expr(ast->data.call.func, env, constr_env, subst, &ft);
            if (err)
                return err;
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp = res;
            for (s32 i = (s32)sp_dyn_array_size(ast->data.call.args) - 1; i >= 0; i--) {
                fxsh_type_t *at = NULL;
                err = infer_expr(ast->data.call.args[i], env, constr_env, subst, &at);
                if (err)
                    return err;
                exp = fxsh_type_arrow(at, exp);
            }
            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(ft, exp, &s);
            if (err)
                return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res);
            *out_type = res;
            return ERR_OK;
        }

        case AST_TUPLE: {
            sp_dyn_array(fxsh_type_t *) elems = SP_NULLPTR;
            sp_dyn_array_for(ast->data.elements, i) {
                fxsh_type_t *et = NULL;
                fxsh_error_t err = infer_expr(ast->data.elements[i], env, constr_env, subst, &et);
                if (err)
                    return err;
                sp_dyn_array_push(elems, et);
            }
            fxsh_type_t *tt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
            tt->kind = TYPE_TUPLE;
            tt->data.tuple = elems;
            *out_type = tt;
            return ERR_OK;
        }

        case AST_LIST: {
            fxsh_type_t *elem_t = fxsh_type_var(fxsh_fresh_var());
            sp_dyn_array_for(ast->data.elements, i) {
                fxsh_type_t *et = NULL;
                fxsh_error_t err = infer_expr(ast->data.elements[i], env, constr_env, subst, &et);
                if (err)
                    return err;
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(et, elem_t, &s);
                if (err)
                    return err;
                *subst = compose(s, *subst);
                fxsh_type_apply_subst(*subst, &elem_t);
            }
            *out_type = make_list_type(elem_t);
            return ERR_OK;
        }

        case AST_PIPE: {
            fxsh_type_t *lt = NULL, *rt = NULL;
            fxsh_error_t err;
            err = infer_expr(ast->data.pipe.left, env, constr_env, subst, &lt);
            if (err)
                return err;
            err = infer_expr(ast->data.pipe.right, env, constr_env, subst, &rt);
            if (err)
                return err;
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp = fxsh_type_arrow(lt, res);
            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(rt, exp, &s);
            if (err)
                return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res);
            *out_type = res;
            return ERR_OK;
        }

        case AST_RECORD: {
            sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
            sp_dyn_array_for(ast->data.elements, i) {
                fxsh_ast_node_t *f = ast->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS)
                    continue;
                if (record_field_index(fields, f->data.field.field) >= 0) {
                    fprintf(stderr, "Type error: duplicate record field `%.*s`\n",
                            f->data.field.field.len, f->data.field.field.data);
                    return ERR_TYPE_ERROR;
                }
                fxsh_type_t *ft = NULL;
                fxsh_error_t err = infer_expr(f->data.field.object, env, constr_env, subst, &ft);
                if (err)
                    return err;
                if (f->data.field.type) {
                    fxsh_type_t *ann_t = ast_to_type(f->data.field.type);
                    fxsh_subst_t s_ann = SP_NULLPTR;
                    err = fxsh_type_unify(ft, ann_t, &s_ann);
                    if (err)
                        return err;
                    *subst = compose(s_ann, *subst);
                    fxsh_type_apply_subst(s_ann, &ft);
                }
                fxsh_field_t rf = {.name = f->data.field.field, .type = ft};
                sp_dyn_array_push(fields, rf);
            }
            *out_type = make_record_type(fields, -1);
            return ERR_OK;
        }

        case AST_FIELD_ACCESS: {
            fxsh_type_t *obj_t = NULL;
            fxsh_error_t err = infer_expr(ast->data.field.object, env, constr_env, subst, &obj_t);
            if (err)
                return err;

            fxsh_type_t *res_t = fxsh_type_var(fxsh_fresh_var());
            sp_dyn_array(fxsh_field_t) req_fields = SP_NULLPTR;
            fxsh_field_t req = {.name = ast->data.field.field, .type = res_t};
            sp_dyn_array_push(req_fields, req);
            fxsh_type_t *req_rec = make_record_type(req_fields, fxsh_fresh_var());

            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(obj_t, req_rec, &s);
            if (err)
                return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res_t);
            *out_type = res_t;
            return ERR_OK;
        }

        case AST_LET:
        case AST_DECL_LET: {
            if (ast->data.let.is_rec) {
                fxsh_type_t *rec_t = fxsh_type_var(fxsh_fresh_var());
                fxsh_scheme_t *rsc = (fxsh_scheme_t *)fxsh_alloc0(sizeof(fxsh_scheme_t));
                rsc->type = rec_t;
                rsc->vars = SP_NULLPTR;
                fxsh_type_env_t new_env = *env;
                type_env_bind(&new_env, ast->data.let.name, rsc);
                fxsh_type_t *vt = NULL;
                fxsh_error_t err =
                    infer_expr(ast->data.let.value, &new_env, constr_env, subst, &vt);
                if (err)
                    return err;
                fxsh_type_apply_subst(*subst, &vt);
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(rec_t, vt, &s);
                if (err)
                    return err;
                *subst = compose(s, *subst);
                if (ast->data.let.type) {
                    fxsh_type_t *ann_t = ast_to_type(ast->data.let.type);
                    fxsh_subst_t s_ann = SP_NULLPTR;
                    err = fxsh_type_unify(rec_t, ann_t, &s_ann);
                    if (err)
                        return err;
                    *subst = compose(s_ann, *subst);
                }
                fxsh_type_apply_subst(*subst, &rec_t);
                type_env_bind(env, ast->data.let.name, generalize(rec_t, env));
            } else {
                if (ast->data.let.type && ast->data.let.value &&
                    ast->data.let.value->kind == AST_LIT_STRING &&
                    lit_string_has_prefix(ast->data.let.value->data.lit_string, "c:")) {
                    fxsh_type_t *ann_t = ast_to_type(ast->data.let.type);
                    if (!ann_t || ann_t->kind != TYPE_ARROW) {
                        fprintf(stderr,
                                "Type error: FFI declaration `%.*s` must have function type\n",
                                ast->data.let.name.len, ast->data.let.name.data);
                        return ERR_TYPE_ERROR;
                    }
                    type_env_bind(env, ast->data.let.name, generalize(ann_t, env));
                    *out_type = fxsh_type_con(TYPE_UNIT);
                    return ERR_OK;
                }
                fxsh_type_t *vt = NULL;
                fxsh_error_t err = infer_expr(ast->data.let.value, env, constr_env, subst, &vt);
                if (err)
                    return err;
                fxsh_type_apply_subst(*subst, &vt);
                if (ast->data.let.type) {
                    fxsh_type_t *ann_t = ast_to_type(ast->data.let.type);
                    fxsh_subst_t s_ann = SP_NULLPTR;
                    err = fxsh_type_unify(vt, ann_t, &s_ann);
                    if (err)
                        return err;
                    *subst = compose(s_ann, *subst);
                }
                fxsh_type_apply_subst(*subst, &vt);
                type_env_bind(env, ast->data.let.name, generalize(vt, env));
            }
            *out_type = fxsh_type_con(TYPE_UNIT);
            return ERR_OK;
        }

        case AST_LET_IN: {
            fxsh_type_env_t new_env = *env;
            sp_dyn_array_for(ast->data.let_in.bindings, i) {
                fxsh_ast_node_t *b = ast->data.let_in.bindings[i];
                if (b->kind == AST_LET || b->kind == AST_DECL_LET) {
                    fxsh_type_t *tmp = NULL;
                    fxsh_error_t err = infer_expr(b, &new_env, constr_env, subst, &tmp);
                    if (err)
                        return err;
                }
            }
            return infer_expr(ast->data.let_in.body, &new_env, constr_env, subst, out_type);
        }

        case AST_TYPE_DEF:
            fxsh_register_type_constrs(ast, constr_env);
            *out_type = fxsh_type_con(TYPE_UNIT);
            return ERR_OK;

        case AST_CONSTR_APPL: {
            fxsh_constr_info_t *ci =
                constr_env_lookup(*constr_env, ast->data.constr_appl.constr_name);
            if (!ci) {
                *out_type = fxsh_type_var(fxsh_fresh_var());
                return ERR_OK;
            }
            fxsh_type_t *ct = instantiate_type(ci->constr_type);
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp = res;
            for (s32 i = (s32)sp_dyn_array_size(ast->data.constr_appl.args) - 1; i >= 0; i--) {
                fxsh_type_t *at = NULL;
                fxsh_error_t err =
                    infer_expr(ast->data.constr_appl.args[i], env, constr_env, subst, &at);
                if (err)
                    return err;
                exp = fxsh_type_arrow(at, exp);
            }
            fxsh_subst_t s = SP_NULLPTR;
            fxsh_error_t err = fxsh_type_unify(ct, exp, &s);
            if (err)
                return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res);
            *out_type = res;
            return ERR_OK;
        }

        case AST_MATCH: {
            fxsh_type_t *mt = NULL;
            fxsh_error_t err = infer_expr(ast->data.match_expr.expr, env, constr_env, subst, &mt);
            if (err)
                return err;
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            bool has_catch_all = false;
            sp_dyn_array(sp_str_t) seen_constrs = SP_NULLPTR;
            sp_dyn_array_for(ast->data.match_expr.arms, i) {
                fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
                if (arm->kind != AST_MATCH_ARM)
                    continue;

                if (has_catch_all) {
                    fprintf(stderr, "Warning: unreachable match arm after catch-all pattern\n");
                }

                if (arm->data.match_arm.pattern &&
                    (arm->data.match_arm.pattern->kind == AST_PAT_WILD ||
                     arm->data.match_arm.pattern->kind == AST_PAT_VAR)) {
                    has_catch_all = true;
                } else if (arm->data.match_arm.pattern &&
                           arm->data.match_arm.pattern->kind == AST_PAT_CONSTR) {
                    sp_str_t cname = arm->data.match_arm.pattern->data.constr_appl.constr_name;
                    if (!str_in_list(seen_constrs, cname)) {
                        sp_dyn_array_push(seen_constrs, cname);
                    }
                }

                fxsh_type_env_t arm_env = *env;
                fxsh_type_t *pt = NULL;
                err = infer_pattern(arm->data.match_arm.pattern, &arm_env, constr_env, subst, &pt);
                if (err)
                    return err;
                fxsh_subst_t s1 = SP_NULLPTR;
                err = fxsh_type_unify(pt, mt, &s1);
                if (err)
                    return err;
                *subst = compose(s1, *subst);
                fxsh_type_apply_subst(s1, &mt);
                fxsh_type_t *bt = NULL;
                err = infer_expr(arm->data.match_arm.body, &arm_env, constr_env, subst, &bt);
                if (err)
                    return err;
                fxsh_subst_t s2 = SP_NULLPTR;
                err = fxsh_type_unify(bt, res, &s2);
                if (err)
                    return err;
                *subst = compose(s2, *subst);
                fxsh_type_apply_subst(s2, &res);
            }

            fxsh_type_apply_subst(*subst, &mt);
            if (!has_catch_all) {
                sp_str_t tname = type_head_constructor(mt);
                if (tname.len > 0) {
                    sp_dyn_array(sp_str_t) all_constrs = SP_NULLPTR;
                    for (fxsh_cenv_node_t *n = (fxsh_cenv_node_t *)(*constr_env); n; n = n->next) {
                        if (sp_str_equal(n->info.type_name, tname) &&
                            !str_in_list(all_constrs, n->name)) {
                            sp_dyn_array_push(all_constrs, n->name);
                        }
                    }
                    if (sp_dyn_array_size(all_constrs) > 0 &&
                        sp_dyn_array_size(seen_constrs) < sp_dyn_array_size(all_constrs)) {
                        fprintf(stderr, "Warning: non-exhaustive match for type `%.*s`, missing:",
                                tname.len, tname.data);
                        sp_dyn_array_for(all_constrs, i) {
                            if (!str_in_list(seen_constrs, all_constrs[i])) {
                                fprintf(stderr, " %.*s", all_constrs[i].len, all_constrs[i].data);
                            }
                        }
                        fprintf(stderr, "\n");
                    }
                    sp_dyn_array_free(all_constrs);
                }
            }

            sp_dyn_array_free(seen_constrs);
            *out_type = res;
            return ERR_OK;
        }

        case AST_PROGRAM:
            *out_type = fxsh_type_con(TYPE_UNIT);
            sp_dyn_array_for(ast->data.decls, i) {
                fxsh_error_t err = infer_expr(ast->data.decls[i], env, constr_env, subst, out_type);
                if (err != ERR_OK)
                    return err;
            }
            return ERR_OK;

        default:
            *out_type = fxsh_type_var(fxsh_fresh_var());
            return ERR_OK;
    }
}

fxsh_error_t fxsh_type_infer(fxsh_ast_node_t *ast, fxsh_type_env_t *env,
                             fxsh_constr_env_t *constr_env, fxsh_type_t **out_type) {
    fxsh_reset_type_vars();
    ensure_builtin_env(env);
    ensure_builtin_constr_env(constr_env);
    fxsh_subst_t subst = SP_NULLPTR;
    fxsh_error_t err = infer_expr(ast, env, constr_env, &subst, out_type);
    if (err == ERR_OK)
        fxsh_type_apply_subst(subst, out_type);
    return err;
}
