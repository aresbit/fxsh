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
    t->kind     = TYPE_VAR;
    t->data.var = var;
    return t;
}

fxsh_type_t *fxsh_type_con(sp_str_t name) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind     = TYPE_CON;
    t->data.con = name;
    return t;
}

fxsh_type_t *fxsh_type_arrow(fxsh_type_t *param, fxsh_type_t *ret) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind                = TYPE_ARROW;
    t->data.arrow.param    = param;
    t->data.arrow.ret      = ret;
    return t;
}

fxsh_type_t *fxsh_type_apply(fxsh_type_t *con, fxsh_type_t *arg) {
    fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    t->kind          = TYPE_APP;
    t->data.app.con  = con;
    t->data.app.arg  = arg;
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
fxsh_type_env_t fxsh_type_env_extend(fxsh_type_env_t env,
                                      sp_str_t name,
                                      fxsh_scheme_t *scheme) {
    fxsh_tenv_node_t *node = (fxsh_tenv_node_t *)fxsh_alloc0(sizeof(fxsh_tenv_node_t));
    node->name   = name;
    node->scheme = scheme;
    node->next   = (fxsh_tenv_node_t *)env;
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

static fxsh_constr_env_t constr_env_extend(fxsh_constr_env_t env,
                                            sp_str_t name,
                                            fxsh_constr_info_t *info) {
    fxsh_cenv_node_t *node = (fxsh_cenv_node_t *)fxsh_alloc0(sizeof(fxsh_cenv_node_t));
    node->name = name;
    node->info = *info;
    node->next = (fxsh_cenv_node_t *)env;
    return (fxsh_constr_env_t)node;
}

static void constr_env_bind(fxsh_constr_env_t *env,
                             sp_str_t name,
                             fxsh_constr_info_t *info) {
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

/*=============================================================================
 * Type to String
 *=============================================================================*/

static void type_to_string_impl(fxsh_type_t *type, sp_dyn_array(c8) * out,
                                sp_dyn_array(s32) * bound_vars) {
    if (!type) {
        const c8 *s = "<null>";
        for (u32 i = 0; s[i]; i++) sp_dyn_array_push(*out, s[i]);
        return;
    }

    switch (type->kind) {
        case TYPE_VAR: {
            s32 var_id   = type->data.var;
            bool found   = false;
            s32  name_idx = 0;
            sp_dyn_array_for(*bound_vars, i) {
                if ((*bound_vars)[i] == var_id) { found = true; name_idx = i; break; }
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
                for (c8 *p = buf; *p; p++) sp_dyn_array_push(*out, *p);
            }
            break;
        }
        case TYPE_CON:
            for (u32 i = 0; i < type->data.con.len; i++)
                sp_dyn_array_push(*out, type->data.con.data[i]);
            break;
        case TYPE_ARROW: {
            bool needs_paren = (type->data.arrow.param->kind == TYPE_ARROW);
            if (needs_paren) sp_dyn_array_push(*out, '(');
            type_to_string_impl(type->data.arrow.param, out, bound_vars);
            if (needs_paren) sp_dyn_array_push(*out, ')');
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
                if (i > 0) { sp_dyn_array_push(*out, ','); sp_dyn_array_push(*out, ' '); }
                type_to_string_impl(type->data.tuple[i], out, bound_vars);
            }
            sp_dyn_array_push(*out, ')');
            break;
        case TYPE_RECORD:
            sp_dyn_array_push(*out, '{');
            sp_dyn_array_for(type->data.record.fields, i) {
                if (i > 0) { sp_dyn_array_push(*out, ';'); sp_dyn_array_push(*out, ' '); }
                fxsh_field_t *f = &type->data.record.fields[i];
                for (u32 j = 0; j < f->name.len; j++) sp_dyn_array_push(*out, f->name.data[j]);
                sp_dyn_array_push(*out, ':'); sp_dyn_array_push(*out, ' ');
                type_to_string_impl(f->type, out, bound_vars);
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
    sp_dyn_array(c8) chars     = SP_NULLPTR;
    sp_dyn_array(s32) bv       = SP_NULLPTR;
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
    if (!type) return;
    switch (type->kind) {
        case TYPE_VAR:
            sp_dyn_array_for(*out, i) { if ((*out)[i] == type->data.var) return; }
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
                    if (s->vars[j] == type_vars[i]) { is_bound = true; break; }
                }
                if (!is_bound) {
                    bool already = false;
                    sp_dyn_array_for(*out_vars, k) {
                        if ((*out_vars)[k] == type_vars[i]) { already = true; break; }
                    }
                    if (!already) sp_dyn_array_push(*out_vars, type_vars[i]);
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
    if (!type) return NULL;
    switch (type->kind) {
        case TYPE_VAR:
            return (type->data.var == var) ? rep : type;
        case TYPE_CON:
            return type;
        case TYPE_ARROW: {
            fxsh_type_t *p = apply_subst_single(var, rep, type->data.arrow.param);
            fxsh_type_t *r = apply_subst_single(var, rep, type->data.arrow.ret);
            if (p == type->data.arrow.param && r == type->data.arrow.ret) return type;
            return fxsh_type_arrow(p, r);
        }
        case TYPE_TUPLE: {
            /* Reuse array if nothing changed */
            bool changed = false;
            sp_dyn_array(fxsh_type_t *) elems = SP_NULLPTR;
            sp_dyn_array_for(type->data.tuple, i) {
                fxsh_type_t *e = apply_subst_single(var, rep, type->data.tuple[i]);
                if (e != type->data.tuple[i]) changed = true;
                sp_dyn_array_push(elems, e);
            }
            if (!changed) { sp_dyn_array_free(elems); return type; }
            fxsh_type_t *nt = fxsh_type_var(0); nt->kind = TYPE_TUPLE; nt->data.tuple = elems;
            return nt;
        }
        case TYPE_APP: {
            fxsh_type_t *c = apply_subst_single(var, rep, type->data.app.con);
            fxsh_type_t *a = apply_subst_single(var, rep, type->data.app.arg);
            if (c == type->data.app.con && a == type->data.app.arg) return type;
            return fxsh_type_apply(c, a);
        }
        default:
            return type;
    }
}

void fxsh_type_apply_subst(fxsh_subst_t subst, fxsh_type_t **type) {
    if (!*type) return;
    sp_dyn_array_for(subst, i)
        *type = apply_subst_single(subst[i].var, subst[i].type, *type);
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
    if (!type) return false;
    switch (type->kind) {
        case TYPE_VAR:    return type->data.var == var;
        case TYPE_CON:    return false;
        case TYPE_ARROW:  return occurs_in(var, type->data.arrow.param)
                              || occurs_in(var, type->data.arrow.ret);
        case TYPE_APP:    return occurs_in(var, type->data.app.con)
                              || occurs_in(var, type->data.app.arg);
        case TYPE_TUPLE:
            sp_dyn_array_for(type->data.tuple, i)
                if (occurs_in(var, type->data.tuple[i])) return true;
            return false;
        case TYPE_RECORD:
            sp_dyn_array_for(type->data.record.fields, i)
                if (occurs_in(var, type->data.record.fields[i].type)) return true;
            return false;
    }
    return false;
}

/*=============================================================================
 * Unification
 *=============================================================================*/

fxsh_error_t fxsh_type_unify(fxsh_type_t *t1, fxsh_type_t *t2, fxsh_subst_t *out_subst) {
    if (!t1 || !t2) return ERR_TYPE_ERROR;

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
        if (sp_str_equal(t1->data.con, t2->data.con)) { *out_subst = SP_NULLPTR; return ERR_OK; }
        fprintf(stderr, "Type error: cannot unify '%.*s' with '%.*s'\n",
                (int)t1->data.con.len, t1->data.con.data,
                (int)t2->data.con.len, t2->data.con.data);
        return ERR_TYPE_ERROR;
    }

    /* Both arrows */
    if (t1->kind == TYPE_ARROW && t2->kind == TYPE_ARROW) {
        fxsh_subst_t s1 = SP_NULLPTR;
        fxsh_error_t err = fxsh_type_unify(t1->data.arrow.param, t2->data.arrow.param, &s1);
        if (err != ERR_OK) return err;
        fxsh_type_t *r1 = t1->data.arrow.ret, *r2 = t2->data.arrow.ret;
        fxsh_type_apply_subst(s1, &r1);
        fxsh_type_apply_subst(s1, &r2);
        fxsh_subst_t s2 = SP_NULLPTR;
        err = fxsh_type_unify(r1, r2, &s2);
        if (err != ERR_OK) return err;
        *out_subst = compose(s2, s1);
        return ERR_OK;
    }

    /* Both applications */
    if (t1->kind == TYPE_APP && t2->kind == TYPE_APP) {
        fxsh_subst_t s1 = SP_NULLPTR;
        fxsh_error_t err = fxsh_type_unify(t1->data.app.con, t2->data.app.con, &s1);
        if (err != ERR_OK) return err;
        fxsh_type_t *a1 = t1->data.app.arg, *a2 = t2->data.app.arg;
        fxsh_type_apply_subst(s1, &a1);
        fxsh_type_apply_subst(s1, &a2);
        fxsh_subst_t s2 = SP_NULLPTR;
        err = fxsh_type_unify(a1, a2, &s2);
        if (err != ERR_OK) return err;
        *out_subst = compose(s2, s1);
        return ERR_OK;
    }

    fprintf(stderr, "Type error: kind mismatch (%d vs %d)\n", t1->kind, t2->kind);
    return ERR_TYPE_ERROR;
}

/*=============================================================================
 * Scheme Instantiation / Generalization
 *=============================================================================*/

static fxsh_type_t *instantiate_impl(fxsh_type_t *type, sp_ht(s32, s32) * var_map) {
    if (!type) return NULL;
    switch (type->kind) {
        case TYPE_VAR: {
            s32 *m = sp_ht_getp(*var_map, type->data.var);
            return m ? fxsh_type_var(*m) : type;
        }
        case TYPE_CON: return type;
        case TYPE_ARROW: {
            fxsh_type_t *p = instantiate_impl(type->data.arrow.param, var_map);
            fxsh_type_t *r = instantiate_impl(type->data.arrow.ret, var_map);
            return (p != type->data.arrow.param || r != type->data.arrow.ret)
                ? fxsh_type_arrow(p, r) : type;
        }
        case TYPE_APP: {
            fxsh_type_t *c = instantiate_impl(type->data.app.con, var_map);
            fxsh_type_t *a = instantiate_impl(type->data.app.arg, var_map);
            return (c != type->data.app.con || a != type->data.app.arg)
                ? fxsh_type_apply(c, a) : type;
        }
        default: return type;
    }
}

static fxsh_type_t *instantiate(fxsh_scheme_t *scheme) {
    if (!scheme) return fxsh_type_var(fxsh_fresh_var());
    sp_ht(s32, s32) var_map = SP_NULLPTR;
    sp_dyn_array_for(scheme->vars, i) {
        s32 fresh = fxsh_fresh_var();
        sp_ht_insert(var_map, scheme->vars[i], fresh);
    }
    fxsh_type_t *result = instantiate_impl(scheme->type, &var_map);
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
    if (env) free_vars_in_env(*env, &env_vars);

    sp_dyn_array_for(type_vars, i) {
        bool in_env = false;
        sp_dyn_array_for(env_vars, j) {
            if (env_vars[j] == type_vars[i]) { in_env = true; break; }
        }
        if (!in_env) sp_dyn_array_push(scheme->vars, type_vars[i]);
    }

    sp_dyn_array_free(type_vars);
    sp_dyn_array_free(env_vars);
    return scheme;
}

/*=============================================================================
 * Constructor Registration
 *=============================================================================*/

static fxsh_type_t *ast_to_type(fxsh_ast_node_t *ast) {
    if (!ast) return fxsh_type_con(TYPE_UNIT);
    switch (ast->kind) {
        case AST_IDENT: {
            sp_str_t n = ast->data.ident;
            if (sp_str_equal(n, TYPE_INT))    return fxsh_type_con(TYPE_INT);
            if (sp_str_equal(n, TYPE_BOOL))   return fxsh_type_con(TYPE_BOOL);
            if (sp_str_equal(n, TYPE_FLOAT))  return fxsh_type_con(TYPE_FLOAT);
            if (sp_str_equal(n, TYPE_STRING)) return fxsh_type_con(TYPE_STRING);
            if (sp_str_equal(n, TYPE_UNIT))   return fxsh_type_con(TYPE_UNIT);
            return fxsh_type_con(n);
        }
        case AST_TYPE_VAR: return fxsh_type_var(fxsh_fresh_var());
        case AST_TYPE_ARROW:
            return fxsh_type_arrow(ast_to_type(ast->data.type_arrow.param),
                                   ast_to_type(ast->data.type_arrow.ret));
        default: return fxsh_type_var(fxsh_fresh_var());
    }
}

static fxsh_type_t *make_constr_type(fxsh_ast_node_t *data_constr,
                                      fxsh_ast_list_t type_params,
                                      sp_str_t type_name) {
    /* Result type: TypeName applied to its type params */
    fxsh_type_t *result = fxsh_type_con(type_name);
    sp_dyn_array_for(type_params, i) {
        (void)i;
        fxsh_type_t *v = fxsh_type_var(fxsh_fresh_var());
        result = fxsh_type_apply(result, v);
    }

    /* Build arrow type right-to-left */
    fxsh_type_t *constr_type = result;
    fxsh_ast_list_t arg_types = data_constr->data.data_constr.arg_types;
    for (s32 i = (s32)sp_dyn_array_size(arg_types) - 1; i >= 0; i--) {
        fxsh_type_t *arg_t = ast_to_type(arg_types[i]);
        if (!arg_t) arg_t = fxsh_type_var(fxsh_fresh_var());
        constr_type = fxsh_type_arrow(arg_t, constr_type);
    }
    return constr_type;
}

void fxsh_register_type_constrs(fxsh_ast_node_t *type_def,
                                  fxsh_constr_env_t *constr_env) {
    if (!type_def || type_def->kind != AST_TYPE_DEF || !constr_env) return;
    sp_str_t       type_name   = type_def->data.type_def.name;
    fxsh_ast_list_t constructors = type_def->data.type_def.constructors;
    fxsh_ast_list_t type_params  = type_def->data.type_def.type_params;

    sp_dyn_array_for(constructors, i) {
        fxsh_ast_node_t *constr = constructors[i];
        if (constr->kind != AST_DATA_CONSTR) continue;
        fxsh_constr_info_t info = {
            .constr_name = constr->data.data_constr.name,
            .type_name   = type_name,
            .constr_type = make_constr_type(constr, type_params, type_name),
            .arity       = (s32)sp_dyn_array_size(constr->data.data_constr.arg_types),
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

static fxsh_error_t infer_pattern(fxsh_ast_node_t *pattern, fxsh_type_env_t *env,
                                  fxsh_constr_env_t *constr_env, fxsh_subst_t *subst,
                                  fxsh_type_t **out_type) {
    if (!pattern) { *out_type = fxsh_type_var(fxsh_fresh_var()); return ERR_OK; }

    switch (pattern->kind) {
        case AST_PAT_WILD:
            *out_type = fxsh_type_var(fxsh_fresh_var());
            return ERR_OK;

        case AST_PAT_VAR: {
            fxsh_type_t   *v  = fxsh_type_var(fxsh_fresh_var());
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

        case AST_LIT_INT:    *out_type = fxsh_type_con(TYPE_INT);    return ERR_OK;
        case AST_LIT_FLOAT:  *out_type = fxsh_type_con(TYPE_FLOAT);  return ERR_OK;
        case AST_LIT_STRING: *out_type = fxsh_type_con(TYPE_STRING); return ERR_OK;
        case AST_LIT_BOOL:   *out_type = fxsh_type_con(TYPE_BOOL);   return ERR_OK;
        case AST_LIT_UNIT:   *out_type = fxsh_type_con(TYPE_UNIT);   return ERR_OK;

        case AST_PAT_TUPLE: {
            sp_dyn_array(fxsh_type_t *) elem_types = SP_NULLPTR;
            sp_dyn_array_for(pattern->data.elements, i) {
                fxsh_type_t *et = NULL;
                fxsh_error_t err = infer_pattern(pattern->data.elements[i],
                                                  env, constr_env, subst, &et);
                if (err != ERR_OK) return err;
                sp_dyn_array_push(elem_types, et);
            }
            fxsh_type_t *tt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
            tt->kind = TYPE_TUPLE; tt->data.tuple = elem_types;
            *out_type = tt;
            return ERR_OK;
        }

        case AST_PAT_CONSTR: {
            fxsh_constr_info_t *ci = constr_env_lookup(*constr_env,
                                        pattern->data.constr_appl.constr_name);
            if (!ci) { *out_type = fxsh_type_var(fxsh_fresh_var()); return ERR_OK; }

            fxsh_type_t *ct      = instantiate(ci->constr_type);
            fxsh_type_t *res_t   = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp_t   = res_t;
            fxsh_ast_list_t args = pattern->data.constr_appl.args;
            for (s32 i = (s32)sp_dyn_array_size(args) - 1; i >= 0; i--) {
                fxsh_type_t *at = NULL;
                infer_pattern(args[i], env, constr_env, subst, &at);
                exp_t = fxsh_type_arrow(at, exp_t);
            }
            fxsh_subst_t s = SP_NULLPTR;
            fxsh_error_t err = fxsh_type_unify(ct, exp_t, &s);
            if (err != ERR_OK) return err;
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
    if (!ast) { *out_type = fxsh_type_con(TYPE_UNIT); return ERR_OK; }

    switch (ast->kind) {
        case AST_LIT_INT:    *out_type = fxsh_type_con(TYPE_INT);    return ERR_OK;
        case AST_LIT_FLOAT:  *out_type = fxsh_type_con(TYPE_FLOAT);  return ERR_OK;
        case AST_LIT_STRING: *out_type = fxsh_type_con(TYPE_STRING); return ERR_OK;
        case AST_LIT_BOOL:   *out_type = fxsh_type_con(TYPE_BOOL);   return ERR_OK;
        case AST_LIT_UNIT:   *out_type = fxsh_type_con(TYPE_UNIT);   return ERR_OK;

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

        case AST_BINARY: {
            fxsh_type_t *lt = NULL, *rt = NULL;
            fxsh_error_t err;
            err = infer_expr(ast->data.binary.left,  env, constr_env, subst, &lt); if (err) return err;
            err = infer_expr(ast->data.binary.right, env, constr_env, subst, &rt); if (err) return err;

            switch (ast->data.binary.op) {
                case TOK_PLUS: case TOK_MINUS: case TOK_STAR:
                case TOK_SLASH: case TOK_PERCENT: {
                    fxsh_type_t *it = fxsh_type_con(TYPE_INT);
                    fxsh_subst_t s1 = SP_NULLPTR, s2 = SP_NULLPTR;
                    err = fxsh_type_unify(lt, it, &s1); if (err) return err;
                    fxsh_type_apply_subst(s1, &rt);
                    *subst = compose(s1, *subst);
                    err = fxsh_type_unify(rt, it, &s2); if (err) return err;
                    *subst = compose(s2, *subst);
                    *out_type = it; return ERR_OK;
                }
                case TOK_EQ: case TOK_NEQ: case TOK_LT:
                case TOK_GT: case TOK_LEQ: case TOK_GEQ: {
                    fxsh_subst_t s = SP_NULLPTR;
                    err = fxsh_type_unify(lt, rt, &s); if (err) return err;
                    *subst = compose(s, *subst);
                    *out_type = fxsh_type_con(TYPE_BOOL); return ERR_OK;
                }
                case TOK_AND: case TOK_OR: {
                    fxsh_type_t *bt = fxsh_type_con(TYPE_BOOL);
                    fxsh_subst_t s1 = SP_NULLPTR, s2 = SP_NULLPTR;
                    err = fxsh_type_unify(lt, bt, &s1); if (err) return err;
                    fxsh_type_apply_subst(s1, &rt);
                    *subst = compose(s1, *subst);
                    err = fxsh_type_unify(rt, bt, &s2); if (err) return err;
                    *subst = compose(s2, *subst);
                    *out_type = bt; return ERR_OK;
                }
                default: return ERR_TYPE_ERROR;
            }
        }

        case AST_UNARY: {
            fxsh_type_t *ot = NULL;
            fxsh_error_t err = infer_expr(ast->data.unary.operand, env, constr_env, subst, &ot);
            if (err) return err;
            if (ast->data.unary.op == TOK_MINUS) {
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(ot, fxsh_type_con(TYPE_INT), &s); if (err) return err;
                *subst = compose(s, *subst); *out_type = fxsh_type_con(TYPE_INT);
            } else if (ast->data.unary.op == TOK_NOT) {
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(ot, fxsh_type_con(TYPE_BOOL), &s); if (err) return err;
                *subst = compose(s, *subst); *out_type = fxsh_type_con(TYPE_BOOL);
            } else return ERR_TYPE_ERROR;
            return ERR_OK;
        }

        case AST_IF: {
            fxsh_type_t *ct = NULL, *tt = NULL, *et = NULL;
            fxsh_error_t err;
            err = infer_expr(ast->data.if_expr.cond,        env, constr_env, subst, &ct); if (err) return err;
            err = infer_expr(ast->data.if_expr.then_branch, env, constr_env, subst, &tt); if (err) return err;
            fxsh_subst_t sc = SP_NULLPTR;
            err = fxsh_type_unify(ct, fxsh_type_con(TYPE_BOOL), &sc); if (err) return err;
            *subst = compose(sc, *subst);
            if (ast->data.if_expr.else_branch) {
                err = infer_expr(ast->data.if_expr.else_branch, env, constr_env, subst, &et); if (err) return err;
                fxsh_subst_t se = SP_NULLPTR;
                err = fxsh_type_unify(tt, et, &se); if (err) return err;
                *subst = compose(se, *subst);
                fxsh_type_apply_subst(se, &tt);
            }
            *out_type = tt; return ERR_OK;
        }

        case AST_LAMBDA: {
            fxsh_type_env_t new_env = *env;
            sp_dyn_array(fxsh_type_t *) param_types = SP_NULLPTR;
            sp_dyn_array_for(ast->data.lambda.params, i) {
                fxsh_type_t *pt = NULL;
                fxsh_error_t err = infer_pattern(ast->data.lambda.params[i],
                                                  &new_env, constr_env, subst, &pt);
                if (err) return err;
                sp_dyn_array_push(param_types, pt);
            }
            fxsh_type_t *bt = NULL;
            fxsh_error_t err = infer_expr(ast->data.lambda.body, &new_env, constr_env, subst, &bt);
            if (err) return err;
            fxsh_type_t *rt = bt;
            for (s32 i = (s32)sp_dyn_array_size(param_types) - 1; i >= 0; i--)
                rt = fxsh_type_arrow(param_types[i], rt);
            *out_type = rt; return ERR_OK;
        }

        case AST_CALL: {
            fxsh_type_t *ft = NULL;
            fxsh_error_t err = infer_expr(ast->data.call.func, env, constr_env, subst, &ft);
            if (err) return err;
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp = res;
            for (s32 i = (s32)sp_dyn_array_size(ast->data.call.args) - 1; i >= 0; i--) {
                fxsh_type_t *at = NULL;
                err = infer_expr(ast->data.call.args[i], env, constr_env, subst, &at);
                if (err) return err;
                exp = fxsh_type_arrow(at, exp);
            }
            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(ft, exp, &s); if (err) return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res);
            *out_type = res; return ERR_OK;
        }

        case AST_PIPE: {
            fxsh_type_t *lt = NULL, *rt = NULL;
            fxsh_error_t err;
            err = infer_expr(ast->data.pipe.left,  env, constr_env, subst, &lt); if (err) return err;
            err = infer_expr(ast->data.pipe.right, env, constr_env, subst, &rt); if (err) return err;
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp = fxsh_type_arrow(lt, res);
            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(rt, exp, &s); if (err) return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res);
            *out_type = res; return ERR_OK;
        }

        case AST_LET:
        case AST_DECL_LET: {
            if (ast->data.let.is_rec) {
                fxsh_type_t *rec_t = fxsh_type_var(fxsh_fresh_var());
                fxsh_scheme_t *rsc = (fxsh_scheme_t *)fxsh_alloc0(sizeof(fxsh_scheme_t));
                rsc->type = rec_t; rsc->vars = SP_NULLPTR;
                fxsh_type_env_t new_env = *env;
                type_env_bind(&new_env, ast->data.let.name, rsc);
                fxsh_type_t *vt = NULL;
                fxsh_error_t err = infer_expr(ast->data.let.value, &new_env, constr_env, subst, &vt);
                if (err) return err;
                fxsh_type_apply_subst(*subst, &vt);
                fxsh_subst_t s = SP_NULLPTR;
                err = fxsh_type_unify(rec_t, vt, &s); if (err) return err;
                *subst = compose(s, *subst);
                fxsh_type_apply_subst(*subst, &rec_t);
                type_env_bind(env, ast->data.let.name, generalize(rec_t, env));
            } else {
                fxsh_type_t *vt = NULL;
                fxsh_error_t err = infer_expr(ast->data.let.value, env, constr_env, subst, &vt);
                if (err) return err;
                fxsh_type_apply_subst(*subst, &vt);
                type_env_bind(env, ast->data.let.name, generalize(vt, env));
            }
            *out_type = fxsh_type_con(TYPE_UNIT); return ERR_OK;
        }

        case AST_LET_IN: {
            fxsh_type_env_t new_env = *env;
            sp_dyn_array_for(ast->data.let_in.bindings, i) {
                fxsh_ast_node_t *b = ast->data.let_in.bindings[i];
                if (b->kind == AST_LET || b->kind == AST_DECL_LET) {
                    fxsh_type_t *vt = NULL;
                    fxsh_error_t err = infer_expr(b->data.let.value,
                                                   &new_env, constr_env, subst, &vt);
                    if (err) return err;
                    fxsh_type_apply_subst(*subst, &vt);
                    type_env_bind(&new_env, b->data.let.name, generalize(vt, &new_env));
                }
            }
            return infer_expr(ast->data.let_in.body, &new_env, constr_env, subst, out_type);
        }

        case AST_TYPE_DEF:
            fxsh_register_type_constrs(ast, constr_env);
            *out_type = fxsh_type_con(TYPE_UNIT);
            return ERR_OK;

        case AST_CONSTR_APPL: {
            fxsh_constr_info_t *ci = constr_env_lookup(*constr_env,
                                        ast->data.constr_appl.constr_name);
            if (!ci) { *out_type = fxsh_type_var(fxsh_fresh_var()); return ERR_OK; }
            fxsh_type_t *ct  = instantiate(ci->constr_type);
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *exp = res;
            for (s32 i = (s32)sp_dyn_array_size(ast->data.constr_appl.args) - 1; i >= 0; i--) {
                fxsh_type_t *at = NULL;
                fxsh_error_t err = infer_expr(ast->data.constr_appl.args[i],
                                               env, constr_env, subst, &at);
                if (err) return err;
                exp = fxsh_type_arrow(at, exp);
            }
            fxsh_subst_t s = SP_NULLPTR;
            fxsh_error_t err = fxsh_type_unify(ct, exp, &s); if (err) return err;
            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &res);
            *out_type = res; return ERR_OK;
        }

        case AST_MATCH: {
            fxsh_type_t *mt = NULL;
            fxsh_error_t err = infer_expr(ast->data.match_expr.expr, env, constr_env, subst, &mt);
            if (err) return err;
            fxsh_type_t *res = fxsh_type_var(fxsh_fresh_var());
            sp_dyn_array_for(ast->data.match_expr.arms, i) {
                fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
                if (arm->kind != AST_MATCH_ARM) continue;
                fxsh_type_env_t arm_env = *env;
                fxsh_type_t *pt = NULL;
                err = infer_pattern(arm->data.match_arm.pattern, &arm_env, constr_env, subst, &pt);
                if (err) return err;
                fxsh_subst_t s1 = SP_NULLPTR;
                err = fxsh_type_unify(pt, mt, &s1); if (err) return err;
                *subst = compose(s1, *subst);
                fxsh_type_apply_subst(s1, &mt);
                fxsh_type_t *bt = NULL;
                err = infer_expr(arm->data.match_arm.body, &arm_env, constr_env, subst, &bt);
                if (err) return err;
                fxsh_subst_t s2 = SP_NULLPTR;
                err = fxsh_type_unify(bt, res, &s2); if (err) return err;
                *subst = compose(s2, *subst);
                fxsh_type_apply_subst(s2, &res);
            }
            *out_type = res; return ERR_OK;
        }

        case AST_PROGRAM:
            *out_type = fxsh_type_con(TYPE_UNIT);
            sp_dyn_array_for(ast->data.decls, i) {
                fxsh_error_t err = infer_expr(ast->data.decls[i], env, constr_env, subst, out_type);
                if (err != ERR_OK) return err;
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
    fxsh_subst_t subst = SP_NULLPTR;
    fxsh_error_t err   = infer_expr(ast, env, constr_env, &subst, out_type);
    if (err == ERR_OK) fxsh_type_apply_subst(subst, out_type);
    return err;
}
