/*
 * types.c - fxsh Hindley-Milner type inference
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
    fxsh_type_t *t = sp_alloc(sizeof(fxsh_type_t));
    t->kind = TYPE_VAR;
    t->data.var = var;
    return t;
}

fxsh_type_t *fxsh_type_con(sp_str_t name) {
    fxsh_type_t *t = sp_alloc(sizeof(fxsh_type_t));
    t->kind = TYPE_CON;
    t->data.con = name;
    return t;
}

fxsh_type_t *fxsh_type_arrow(fxsh_type_t *param, fxsh_type_t *ret) {
    fxsh_type_t *t = sp_alloc(sizeof(fxsh_type_t));
    t->kind = TYPE_ARROW;
    t->data.arrow.param = param;
    t->data.arrow.ret = ret;
    return t;
}

fxsh_type_t *fxsh_type_apply(fxsh_type_t *con, fxsh_type_t *arg) {
    fxsh_type_t *t = sp_alloc(sizeof(fxsh_type_t));
    t->kind = TYPE_APP;
    t->data.app.con = con;
    t->data.app.arg = arg;
    return t;
}

/*=============================================================================
 * Type to String
 *=============================================================================*/

static void type_to_string_impl(fxsh_type_t *type, sp_dyn_array(c8) * out,
                                sp_dyn_array(s32) * bound_vars) {
    if (!type) {
        const c8 *s = "<null>";
        for (u32 i = 0; s[i]; i++) {
            sp_dyn_array_push(*out, s[i]);
        }
        return;
    }

    switch (type->kind) {
        case TYPE_VAR: {
            /* Generate name like 'a, 'b, 'c... */
            s32 var_id = type->data.var;
            sp_dyn_array_push(*out, '\'');

            /* Check if already bound */
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
                name_idx = sp_dyn_array_size(*bound_vars);
                sp_dyn_array_push(*bound_vars, var_id);
            }

            /* Generate name */
            if (name_idx < 26) {
                sp_dyn_array_push(*out, 'a' + (c8)name_idx);
            } else {
                sp_dyn_array_push(*out, 't');
                /* Append number for overflow */
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", name_idx);
                for (c8 *p = buf; *p; p++) {
                    sp_dyn_array_push(*out, *p);
                }
            }
            break;
        }
        case TYPE_CON: {
            for (u32 i = 0; i < type->data.con.len; i++) {
                sp_dyn_array_push(*out, type->data.con.data[i]);
            }
            break;
        }
        case TYPE_ARROW: {
            /* Parenthesize left if it's an arrow */
            bool needs_paren = type->data.arrow.param->kind == TYPE_ARROW;
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
        case TYPE_TUPLE: {
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
        }
        case TYPE_RECORD: {
            sp_dyn_array_push(*out, '{');
            sp_dyn_array_for(type->data.record.fields, i) {
                if (i > 0) {
                    sp_dyn_array_push(*out, ',');
                    sp_dyn_array_push(*out, ' ');
                }
                fxsh_field_t *field = &type->data.record.fields[i];
                for (u32 j = 0; j < field->name.len; j++) {
                    sp_dyn_array_push(*out, field->name.data[j]);
                }
                sp_dyn_array_push(*out, ':');
                sp_dyn_array_push(*out, ' ');
                type_to_string_impl(field->type, out, bound_vars);
            }
            if (type->data.record.row_var >= 0) {
                if (sp_dyn_array_size(type->data.record.fields) > 0) {
                    sp_dyn_array_push(*out, ',');
                    sp_dyn_array_push(*out, ' ');
                }
                sp_dyn_array_push(*out, '.');
                sp_dyn_array_push(*out, '.');
                sp_dyn_array_push(*out, '\'');
                sp_dyn_array_push(*out, 'r');
            }
            sp_dyn_array_push(*out, '}');
            break;
        }
        case TYPE_APP: {
            type_to_string_impl(type->data.app.arg, out, bound_vars);
            sp_dyn_array_push(*out, ' ');
            type_to_string_impl(type->data.app.con, out, bound_vars);
            break;
        }
    }
}

const c8 *fxsh_type_to_string(fxsh_type_t *type) {
    static c8 *buf = NULL;
    if (buf)
        sp_free(buf);

    sp_dyn_array(c8) chars = SP_NULLPTR;
    sp_dyn_array(s32) bound_vars = SP_NULLPTR;

    type_to_string_impl(type, &chars, &bound_vars);

    /* Null terminate */
    sp_dyn_array_push(chars, '\0');

    buf = sp_alloc(sp_dyn_array_size(chars));
    memcpy(buf, chars, sp_dyn_array_size(chars));

    sp_dyn_array_free(chars);
    sp_dyn_array_free(bound_vars);

    return buf;
}

/*=============================================================================
 * Free Type Variables
 *=============================================================================*/

static void ftv_impl(fxsh_type_t *type, sp_dyn_array(s32) * out);

static void ftv_scheme_impl(fxsh_scheme_t *scheme, sp_dyn_array(s32) * out) {
    /* Start with all free vars in the type */
    sp_dyn_array(s32) type_vars = SP_NULLPTR;
    ftv_impl(scheme->type, &type_vars);

    /* Remove bound vars */
    sp_dyn_array_for(type_vars, i) {
        bool is_bound = false;
        sp_dyn_array_for(scheme->vars, j) {
            if (scheme->vars[j] == type_vars[i]) {
                is_bound = true;
                break;
            }
        }
        if (!is_bound) {
            sp_dyn_array_push(*out, type_vars[i]);
        }
    }

    sp_dyn_array_free(type_vars);
}

static void ftv_impl(fxsh_type_t *type, sp_dyn_array(s32) * out) {
    if (!type)
        return;

    switch (type->kind) {
        case TYPE_VAR: {
            /* Check if already present */
            sp_dyn_array_for(*out, i) {
                if ((*out)[i] == type->data.var)
                    return;
            }
            sp_dyn_array_push(*out, type->data.var);
            break;
        }
        case TYPE_CON:
            break;
        case TYPE_ARROW:
            ftv_impl(type->data.arrow.param, out);
            ftv_impl(type->data.arrow.ret, out);
            break;
        case TYPE_TUPLE:
            sp_dyn_array_for(type->data.tuple, i) {
                ftv_impl(type->data.tuple[i], out);
            }
            break;
        case TYPE_RECORD:
            sp_dyn_array_for(type->data.record.fields, i) {
                ftv_impl(type->data.record.fields[i].type, out);
            }
            if (type->data.record.row_var >= 0) {
                sp_dyn_array_push(*out, type->data.record.row_var);
            }
            break;
        case TYPE_APP:
            ftv_impl(type->data.app.con, out);
            ftv_impl(type->data.app.arg, out);
            break;
    }
}

/*=============================================================================
 * Substitution Application
 *=============================================================================*/

static fxsh_type_t *apply_subst_single(fxsh_type_var_t var, fxsh_type_t *replacement,
                                       fxsh_type_t *type);

void fxsh_type_apply_subst(fxsh_subst_t subst, fxsh_type_t **type) {
    if (!*type)
        return;

    sp_dyn_array_for(subst, i) {
        *type = apply_subst_single(subst[i].var, subst[i].type, *type);
    }
}

static fxsh_type_t *apply_subst_single(fxsh_type_var_t var, fxsh_type_t *replacement,
                                       fxsh_type_t *type) {
    if (!type)
        return NULL;

    switch (type->kind) {
        case TYPE_VAR:
            if (type->data.var == var) {
                return replacement;
            }
            return type;
        case TYPE_CON:
            return type;
        case TYPE_ARROW: {
            fxsh_type_t *new_param = apply_subst_single(var, replacement, type->data.arrow.param);
            fxsh_type_t *new_ret = apply_subst_single(var, replacement, type->data.arrow.ret);
            if (new_param != type->data.arrow.param || new_ret != type->data.arrow.ret) {
                fxsh_type_t *new_type = sp_alloc(sizeof(fxsh_type_t));
                *new_type = *type;
                new_type->data.arrow.param = new_param;
                new_type->data.arrow.ret = new_ret;
                return new_type;
            }
            return type;
        }
        case TYPE_TUPLE: {
            bool changed = false;
            sp_dyn_array(fxsh_type_t *) new_elems = SP_NULLPTR;
            sp_dyn_array_for(type->data.tuple, i) {
                fxsh_type_t *new_elem = apply_subst_single(var, replacement, type->data.tuple[i]);
                if (new_elem != type->data.tuple[i])
                    changed = true;
                sp_dyn_array_push(new_elems, new_elem);
            }
            if (changed) {
                fxsh_type_t *new_type = sp_alloc(sizeof(fxsh_type_t));
                *new_type = *type;
                new_type->data.tuple = new_elems;
                return new_type;
            }
            sp_dyn_array_free(new_elems);
            return type;
        }
        case TYPE_RECORD: {
            bool changed = false;
            sp_dyn_array(fxsh_field_t) new_fields = SP_NULLPTR;
            sp_dyn_array_for(type->data.record.fields, i) {
                fxsh_field_t field = type->data.record.fields[i];
                fxsh_type_t *new_ftype = apply_subst_single(var, replacement, field.type);
                if (new_ftype != field.type) {
                    field.type = new_ftype;
                    changed = true;
                }
                sp_dyn_array_push(new_fields, field);
            }
            if (changed) {
                fxsh_type_t *new_type = sp_alloc(sizeof(fxsh_type_t));
                *new_type = *type;
                new_type->data.record.fields = new_fields;
                return new_type;
            }
            sp_dyn_array_free(new_fields);
            return type;
        }
        case TYPE_APP: {
            fxsh_type_t *new_con = apply_subst_single(var, replacement, type->data.app.con);
            fxsh_type_t *new_arg = apply_subst_single(var, replacement, type->data.app.arg);
            if (new_con != type->data.app.con || new_arg != type->data.app.arg) {
                fxsh_type_t *new_type = sp_alloc(sizeof(fxsh_type_t));
                *new_type = *type;
                new_type->data.app.con = new_con;
                new_type->data.app.arg = new_arg;
                return new_type;
            }
            return type;
        }
    }
    return type;
}

/*=============================================================================
 * Substitution Composition
 *=============================================================================*/

static fxsh_subst_t compose(fxsh_subst_t s1, fxsh_subst_t s2) {
    /* Apply s1 to all types in s2, then append s1 */
    fxsh_subst_t result = SP_NULLPTR;

    sp_dyn_array_for(s2, i) {
        fxsh_subst_entry_t entry = s2[i];
        fxsh_type_apply_subst(s1, &entry.type);
        sp_dyn_array_push(result, entry);
    }

    sp_dyn_array_for(s1, i) {
        sp_dyn_array_push(result, s1[i]);
    }

    return result;
}

/*=============================================================================
 * Occurs Check
 *=============================================================================*/

static bool occurs_in(fxsh_type_var_t var, fxsh_type_t *type);

static bool occurs_in_fields(fxsh_type_var_t var, sp_dyn_array(fxsh_field_t) fields) {
    sp_dyn_array_for(fields, i) {
        if (occurs_in(var, fields[i].type))
            return true;
    }
    return false;
}

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
        case TYPE_TUPLE:
            sp_dyn_array_for(type->data.tuple, i) {
                if (occurs_in(var, type->data.tuple[i]))
                    return true;
            }
            return false;
        case TYPE_RECORD:
            return occurs_in_fields(var, type->data.record.fields);
        case TYPE_APP:
            return occurs_in(var, type->data.app.con) || occurs_in(var, type->data.app.arg);
    }
    return false;
}

/*=============================================================================
 * Unification
 *=============================================================================*/

fxsh_error_t fxsh_type_unify(fxsh_type_t *t1, fxsh_type_t *t2, fxsh_subst_t *out_subst) {
    if (!t1 || !t2) {
        return ERR_TYPE_ERROR;
    }

    /* Both variables */
    if (t1->kind == TYPE_VAR && t2->kind == TYPE_VAR && t1->data.var == t2->data.var) {
        *out_subst = SP_NULLPTR;
        return ERR_OK;
    }

    /* Variable on left */
    if (t1->kind == TYPE_VAR) {
        if (occurs_in(t1->data.var, t2)) {
            return ERR_TYPE_ERROR; /* Occurs check fails */
        }
        fxsh_subst_entry_t entry = {.var = t1->data.var, .type = t2};
        sp_dyn_array_push(*out_subst, entry);
        return ERR_OK;
    }

    /* Variable on right */
    if (t2->kind == TYPE_VAR) {
        if (occurs_in(t2->data.var, t1)) {
            return ERR_TYPE_ERROR; /* Occurs check fails */
        }
        fxsh_subst_entry_t entry = {.var = t2->data.var, .type = t1};
        sp_dyn_array_push(*out_subst, entry);
        return ERR_OK;
    }

    /* Both constructors */
    if (t1->kind == TYPE_CON && t2->kind == TYPE_CON) {
        if (sp_str_equal(t1->data.con, t2->data.con)) {
            *out_subst = SP_NULLPTR;
            return ERR_OK;
        }
        return ERR_TYPE_ERROR;
    }

    /* Both arrows */
    if (t1->kind == TYPE_ARROW && t2->kind == TYPE_ARROW) {
        fxsh_subst_t s1 = SP_NULLPTR;
        fxsh_error_t err = fxsh_type_unify(t1->data.arrow.param, t2->data.arrow.param, &s1);
        if (err != ERR_OK)
            return err;

        fxsh_type_t *ret1 = t1->data.arrow.ret;
        fxsh_type_t *ret2 = t2->data.arrow.ret;
        fxsh_type_apply_subst(s1, &ret1);
        fxsh_type_apply_subst(s1, &ret2);

        fxsh_subst_t s2 = SP_NULLPTR;
        err = fxsh_type_unify(ret1, ret2, &s2);
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

        fxsh_type_t *arg1 = t1->data.app.arg;
        fxsh_type_t *arg2 = t2->data.app.arg;
        fxsh_type_apply_subst(s1, &arg1);
        fxsh_type_apply_subst(s1, &arg2);

        fxsh_subst_t s2 = SP_NULLPTR;
        err = fxsh_type_unify(arg1, arg2, &s2);
        if (err != ERR_OK)
            return err;

        *out_subst = compose(s2, s1);
        return ERR_OK;
    }

    /* Mismatched constructors */
    return ERR_TYPE_ERROR;
}

/*=============================================================================
 * Type Scheme Instantiation (replace bound vars with fresh vars)
 *=============================================================================*/

static fxsh_type_t *instantiate(fxsh_scheme_t *scheme);

static fxsh_type_t *instantiate_impl(fxsh_type_t *type, sp_ht(s32, s32) * var_map) {
    if (!type)
        return NULL;

    switch (type->kind) {
        case TYPE_VAR: {
            s32 var_id = type->data.var;
            s32 *mapped = sp_ht_getp(*var_map, var_id);
            if (mapped) {
                return fxsh_type_var(*mapped);
            }
            return type;
        }
        case TYPE_CON:
            return type;
        case TYPE_ARROW: {
            fxsh_type_t *new_param = instantiate_impl(type->data.arrow.param, var_map);
            fxsh_type_t *new_ret = instantiate_impl(type->data.arrow.ret, var_map);
            if (new_param != type->data.arrow.param || new_ret != type->data.arrow.ret) {
                return fxsh_type_arrow(new_param, new_ret);
            }
            return type;
        }
        case TYPE_APP: {
            fxsh_type_t *new_con = instantiate_impl(type->data.app.con, var_map);
            fxsh_type_t *new_arg = instantiate_impl(type->data.app.arg, var_map);
            if (new_con != type->data.app.con || new_arg != type->data.app.arg) {
                return fxsh_type_apply(new_con, new_arg);
            }
            return type;
        }
        default:
            return type;
    }
}

static fxsh_type_t *instantiate(fxsh_scheme_t *scheme) {
    if (!scheme)
        return fxsh_type_var(fxsh_fresh_var());

    /* Create mapping from bound vars to fresh vars */
    sp_ht(s32, s32) var_map = SP_NULLPTR;
    sp_dyn_array_for(scheme->vars, i) {
        s32 fresh_var = fxsh_fresh_var();
        sp_ht_insert(var_map, scheme->vars[i], fresh_var);
    }

    fxsh_type_t *result = instantiate_impl(scheme->type, &var_map);

    sp_ht_free(var_map);
    return result;
}

/*=============================================================================
 * Generalization (convert type to scheme by abstracting over free vars)
 *=============================================================================*/

static fxsh_scheme_t *generalize(fxsh_type_t *type, fxsh_type_env_t *env);

static void free_vars_in_env(fxsh_type_env_t *env, sp_dyn_array(s32) * out_vars);

static fxsh_scheme_t *generalize(fxsh_type_t *type, fxsh_type_env_t *env) {
    if (!type)
        return NULL;

    fxsh_scheme_t *scheme = sp_alloc(sizeof(fxsh_scheme_t));
    scheme->vars = SP_NULLPTR;
    scheme->type = type;

    /* Get free vars in type */
    sp_dyn_array(s32) type_vars = SP_NULLPTR;
    ftv_impl(type, &type_vars);

    /* Get free vars in environment */
    sp_dyn_array(s32) env_vars = SP_NULLPTR;
    free_vars_in_env(env, &env_vars);

    /* Generalize vars that are free in type but not in env */
    sp_dyn_array_for(type_vars, i) {
        bool in_env = false;
        sp_dyn_array_for(env_vars, j) {
            if (env_vars[j] == type_vars[i]) {
                in_env = true;
                break;
            }
        }
        if (!in_env) {
            sp_dyn_array_push(scheme->vars, type_vars[i]);
        }
    }

    sp_dyn_array_free(type_vars);
    sp_dyn_array_free(env_vars);

    return scheme;
}

static void free_vars_in_scheme(fxsh_scheme_t *scheme, sp_dyn_array(s32) * out_vars);

static void free_vars_in_env(fxsh_type_env_t *env, sp_dyn_array(s32) * out_vars) {
    if (!env)
        return;
    /* Iterate through all entries in env */
    /* Note: This is simplified - we'd need to iterate the hash table */
}

static void free_vars_in_scheme(fxsh_scheme_t *scheme, sp_dyn_array(s32) * out_vars) {
    if (!scheme)
        return;

    sp_dyn_array(s32) type_vars = SP_NULLPTR;
    ftv_impl(scheme->type, &type_vars);

    /* Remove bound vars */
    sp_dyn_array_for(type_vars, i) {
        bool is_bound = false;
        sp_dyn_array_for(scheme->vars, j) {
            if (scheme->vars[j] == type_vars[i]) {
                is_bound = true;
                break;
            }
        }
        if (!is_bound) {
            /* Check if already in out_vars */
            bool already_present = false;
            sp_dyn_array_for(*out_vars, k) {
                if ((*out_vars)[k] == type_vars[i]) {
                    already_present = true;
                    break;
                }
            }
            if (!already_present) {
                sp_dyn_array_push(*out_vars, type_vars[i]);
            }
        }
    }

    sp_dyn_array_free(type_vars);
}

/*=============================================================================
 * Type Environment Operations
 *=============================================================================*/

static void type_env_bind(fxsh_type_env_t *env, sp_str_t name, fxsh_scheme_t *scheme) {
    if (!env)
        return;
    sp_ht_insert(*env, name, *scheme);
}

static fxsh_scheme_t *type_env_lookup(fxsh_type_env_t *env, sp_str_t name) {
    if (!env)
        return NULL;
    return sp_ht_getp(*env, name);
}

/*=============================================================================
 * Type Inference with Let-polymorphism
 *=============================================================================*/

static fxsh_error_t infer_expr(fxsh_ast_node_t *ast, fxsh_type_env_t *env, fxsh_subst_t *subst,
                               fxsh_type_t **out_type);

fxsh_error_t fxsh_type_infer(fxsh_ast_node_t *ast, fxsh_type_env_t *env, fxsh_type_t **out_type) {
    fxsh_reset_type_vars();
    fxsh_subst_t subst = SP_NULLPTR;
    fxsh_error_t err = infer_expr(ast, env, &subst, out_type);

    if (err == ERR_OK) {
        fxsh_type_apply_subst(subst, out_type);
    }

    return err;
}

static fxsh_error_t infer_expr(fxsh_ast_node_t *ast, fxsh_type_env_t *env, fxsh_subst_t *subst,
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

        case AST_IDENT: {
            /* Look up in environment */
            fxsh_scheme_t *scheme = type_env_lookup(env, ast->data.ident);
            if (!scheme) {
                /* Unknown identifier - create fresh variable for now */
                *out_type = fxsh_type_var(fxsh_fresh_var());
                return ERR_OK;
            }
            /* Instantiate scheme with fresh vars (Let-polymorphism) */
            *out_type = instantiate(scheme);
            return ERR_OK;
        }

        case AST_BINARY: {
            fxsh_type_t *left_type = NULL;
            fxsh_type_t *right_type = NULL;

            fxsh_error_t err = infer_expr(ast->data.binary.left, env, subst, &left_type);
            if (err != ERR_OK)
                return err;

            err = infer_expr(ast->data.binary.right, env, subst, &right_type);
            if (err != ERR_OK)
                return err;

            switch (ast->data.binary.op) {
                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT: {
                    /* Arithmetic: both operands must be numeric */
                    fxsh_type_t *int_type = fxsh_type_con(TYPE_INT);
                    fxsh_subst_t s1 = SP_NULLPTR;
                    err = fxsh_type_unify(left_type, int_type, &s1);
                    if (err != ERR_OK)
                        return err;

                    fxsh_type_apply_subst(s1, &right_type);
                    *subst = compose(s1, *subst);

                    fxsh_subst_t s2 = SP_NULLPTR;
                    err = fxsh_type_unify(right_type, int_type, &s2);
                    if (err != ERR_OK)
                        return err;

                    *subst = compose(s2, *subst);
                    *out_type = int_type;
                    return ERR_OK;
                }
                case TOK_EQ:
                case TOK_NEQ:
                case TOK_LT:
                case TOK_GT:
                case TOK_LEQ:
                case TOK_GEQ: {
                    /* Comparison: both operands same type, returns bool */
                    fxsh_subst_t s = SP_NULLPTR;
                    err = fxsh_type_unify(left_type, right_type, &s);
                    if (err != ERR_OK)
                        return err;

                    *subst = compose(s, *subst);
                    *out_type = fxsh_type_con(TYPE_BOOL);
                    return ERR_OK;
                }
                case TOK_AND:
                case TOK_OR: {
                    /* Logical: both operands bool, returns bool */
                    fxsh_type_t *bool_type = fxsh_type_con(TYPE_BOOL);
                    fxsh_subst_t s1 = SP_NULLPTR;
                    err = fxsh_type_unify(left_type, bool_type, &s1);
                    if (err != ERR_OK)
                        return err;

                    fxsh_type_apply_subst(s1, &right_type);
                    *subst = compose(s1, *subst);

                    fxsh_subst_t s2 = SP_NULLPTR;
                    err = fxsh_type_unify(right_type, bool_type, &s2);
                    if (err != ERR_OK)
                        return err;

                    *subst = compose(s2, *subst);
                    *out_type = bool_type;
                    return ERR_OK;
                }
                default:
                    return ERR_TYPE_ERROR;
            }
        }

        case AST_UNARY: {
            fxsh_type_t *operand_type = NULL;
            fxsh_error_t err = infer_expr(ast->data.unary.operand, env, subst, &operand_type);
            if (err != ERR_OK)
                return err;

            switch (ast->data.unary.op) {
                case TOK_MINUS: {
                    fxsh_type_t *int_type = fxsh_type_con(TYPE_INT);
                    fxsh_subst_t s = SP_NULLPTR;
                    err = fxsh_type_unify(operand_type, int_type, &s);
                    if (err != ERR_OK)
                        return err;
                    *subst = compose(s, *subst);
                    *out_type = int_type;
                    return ERR_OK;
                }
                case TOK_NOT: {
                    fxsh_type_t *bool_type = fxsh_type_con(TYPE_BOOL);
                    fxsh_subst_t s = SP_NULLPTR;
                    err = fxsh_type_unify(operand_type, bool_type, &s);
                    if (err != ERR_OK)
                        return err;
                    *subst = compose(s, *subst);
                    *out_type = bool_type;
                    return ERR_OK;
                }
                default:
                    return ERR_TYPE_ERROR;
            }
        }

        case AST_IF: {
            fxsh_type_t *cond_type = NULL;
            fxsh_error_t err = infer_expr(ast->data.if_expr.cond, env, subst, &cond_type);
            if (err != ERR_OK)
                return err;

            fxsh_type_t *bool_type = fxsh_type_con(TYPE_BOOL);
            fxsh_subst_t s1 = SP_NULLPTR;
            err = fxsh_type_unify(cond_type, bool_type, &s1);
            if (err != ERR_OK)
                return err;

            *subst = compose(s1, *subst);

            fxsh_type_t *then_type = NULL;
            err = infer_expr(ast->data.if_expr.then_branch, env, subst, &then_type);
            if (err != ERR_OK)
                return err;

            if (ast->data.if_expr.else_branch) {
                fxsh_type_t *else_type = NULL;
                err = infer_expr(ast->data.if_expr.else_branch, env, subst, &else_type);
                if (err != ERR_OK)
                    return err;

                fxsh_subst_t s2 = SP_NULLPTR;
                err = fxsh_type_unify(then_type, else_type, &s2);
                if (err != ERR_OK)
                    return err;

                *subst = compose(s2, *subst);
                fxsh_type_apply_subst(s2, &then_type);
            }

            *out_type = then_type;
            return ERR_OK;
        }

        case AST_LAMBDA: {
            /* Create fresh type vars for parameters */
            sp_dyn_array(fxsh_type_t *) param_types = SP_NULLPTR;
            sp_dyn_array_for(ast->data.lambda.params, i) {
                fxsh_type_t *param_type = fxsh_type_var(fxsh_fresh_var());
                sp_dyn_array_push(param_types, param_type);
            }

            /* Extend environment with parameter types */
            /* TODO: Actually extend env */

            fxsh_type_t *body_type = NULL;
            fxsh_error_t err = infer_expr(ast->data.lambda.body, env, subst, &body_type);
            if (err != ERR_OK)
                return err;

            /* Build arrow type */
            fxsh_type_t *result_type = body_type;
            /* Fold right: a -> b -> c = a -> (b -> c) */
            for (s32 i = (s32)sp_dyn_array_size(param_types) - 1; i >= 0; i--) {
                result_type = fxsh_type_arrow(param_types[i], result_type);
            }

            *out_type = result_type;
            return ERR_OK;
        }

        case AST_CALL: {
            fxsh_type_t *func_type = NULL;
            fxsh_error_t err = infer_expr(ast->data.call.func, env, subst, &func_type);
            if (err != ERR_OK)
                return err;

            fxsh_type_t *result_type = fxsh_type_var(fxsh_fresh_var());

            /* Build expected function type */
            fxsh_type_t *expected_func_type = result_type;
            for (s32 i = (s32)sp_dyn_array_size(ast->data.call.args) - 1; i >= 0; i--) {
                fxsh_type_t *arg_type = NULL;
                err = infer_expr(ast->data.call.args[i], env, subst, &arg_type);
                if (err != ERR_OK)
                    return err;

                expected_func_type = fxsh_type_arrow(arg_type, expected_func_type);
            }

            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(func_type, expected_func_type, &s);
            if (err != ERR_OK)
                return err;

            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &result_type);
            *out_type = result_type;
            return ERR_OK;
        }

        case AST_PIPE: {
            /* a |> f = f a */
            fxsh_type_t *left_type = NULL;
            fxsh_error_t err = infer_expr(ast->data.pipe.left, env, subst, &left_type);
            if (err != ERR_OK)
                return err;

            fxsh_type_t *right_type = NULL;
            err = infer_expr(ast->data.pipe.right, env, subst, &right_type);
            if (err != ERR_OK)
                return err;

            fxsh_type_t *result_type = fxsh_type_var(fxsh_fresh_var());
            fxsh_type_t *expected = fxsh_type_arrow(left_type, result_type);

            fxsh_subst_t s = SP_NULLPTR;
            err = fxsh_type_unify(right_type, expected, &s);
            if (err != ERR_OK)
                return err;

            *subst = compose(s, *subst);
            fxsh_type_apply_subst(s, &result_type);
            *out_type = result_type;
            return ERR_OK;
        }

        case AST_LET: {
            /* let x = value (non-recursive) */
            fxsh_type_t *value_type = NULL;
            fxsh_error_t err = infer_expr(ast->data.let.value, env, subst, &value_type);
            if (err != ERR_OK)
                return err;

            /* Apply current substitution to value_type before generalizing */
            fxsh_type_apply_subst(*subst, &value_type);

            /* Generalize to create polymorphic scheme (Let-polymorphism) */
            fxsh_scheme_t *scheme = generalize(value_type, env);

            /* Bind in environment */
            type_env_bind(env, ast->data.let.name, scheme);

            *out_type = fxsh_type_con(TYPE_UNIT);
            return ERR_OK;
        }

        case AST_LET_IN: {
            /* let x = value in body */
            fxsh_type_env_t new_env = env ? *env : SP_NULLPTR;

            sp_dyn_array_for(ast->data.let_in.bindings, i) {
                fxsh_ast_node_t *binding = ast->data.let_in.bindings[i];
                if (binding->kind == AST_LET) {
                    fxsh_type_t *value_type = NULL;
                    fxsh_error_t err =
                        infer_expr(binding->data.let.value, &new_env, subst, &value_type);
                    if (err != ERR_OK)
                        return err;

                    /* Apply current substitution */
                    fxsh_type_apply_subst(*subst, &value_type);

                    /* Generalize for Let-polymorphism */
                    fxsh_scheme_t *scheme = generalize(value_type, &new_env);
                    type_env_bind(&new_env, binding->data.let.name, scheme);
                }
            }

            /* Infer body type with extended environment */
            fxsh_type_t *body_type = NULL;
            fxsh_error_t err = infer_expr(ast->data.let_in.body, &new_env, subst, &body_type);
            if (err != ERR_OK)
                return err;

            *out_type = body_type;
            return ERR_OK;
        }

        case AST_PROGRAM:
            /* Infer last expression's type */
            *out_type = fxsh_type_con(TYPE_UNIT);
            sp_dyn_array_for(ast->data.decls, i) {
                fxsh_error_t err = infer_expr(ast->data.decls[i], env, subst, out_type);
                if (err != ERR_OK)
                    return err;
            }
            return ERR_OK;

        default:
            /* For unimplemented nodes, return a fresh variable */
            *out_type = fxsh_type_var(fxsh_fresh_var());
            return ERR_OK;
    }
}
