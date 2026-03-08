/*
 * comptime.c - fxsh compile-time evaluation engine
 *
 * Implements Zig-style comptime execution:
 * - Compile-time value representation
 * - Type reflection
 * - Code generation macros
 */

#include "fxsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static _Thread_local c8 g_ct_error_msg[256];

/*=============================================================================
 * Comptime Value Constructors
 *=============================================================================*/

fxsh_ct_value_t *fxsh_ct_unit(void) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_UNIT;
    return val;
}

fxsh_ct_value_t *fxsh_ct_bool(bool b) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_BOOL;
    val->data.bool_val = b;
    return val;
}

fxsh_ct_value_t *fxsh_ct_int(s64 n) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_INT;
    val->data.int_val = n;
    return val;
}

fxsh_ct_value_t *fxsh_ct_float(f64 f) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_FLOAT;
    val->data.float_val = f;
    return val;
}

fxsh_ct_value_t *fxsh_ct_string(sp_str_t s) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_STRING;
    val->data.string_val = s;
    return val;
}

fxsh_ct_value_t *fxsh_ct_type(fxsh_type_t *t) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_TYPE;
    val->data.type_val = t;
    return val;
}

fxsh_ct_value_t *fxsh_ct_ast(fxsh_ast_node_t *ast) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_AST;
    val->data.ast_val = ast;
    return val;
}

fxsh_ct_value_t *fxsh_ct_list(fxsh_ct_value_t **items, u32 len) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_LIST;
    val->data.list_val.items = items;
    val->data.list_val.len = len;
    return val;
}

/*=============================================================================
 * Comptime Context
 *=============================================================================*/

void fxsh_comptime_ctx_init(fxsh_comptime_ctx_t *ctx) {
    ctx->env = SP_NULLPTR;
    ctx->type_env = SP_NULLPTR;
    ctx->type_defs = SP_NULLPTR;
    ctx->in_comptime = true;
    g_ct_error_msg[0] = '\0';
}

/*=============================================================================
 * Value Lookup
 *=============================================================================*/

static fxsh_ct_value_t *lookup_var(fxsh_comptime_ctx_t *ctx, sp_str_t name) {
    if (!ctx->env)
        return NULL;
    sp_ht_for(*ctx->env, it) {
        sp_str_t k = *sp_ht_it_getkp(*ctx->env, it);
        if (sp_str_equal(k, name))
            return sp_ht_it_getp(*ctx->env, it);
    }
    return NULL;
}

static void bind_var(fxsh_comptime_ctx_t *ctx, sp_str_t name, fxsh_ct_value_t *val) {
    if (!ctx->env) {
        ctx->env = fxsh_alloc0(sizeof(fxsh_ct_env_t));
        *ctx->env = SP_NULLPTR;
    }
    sp_ht_for(*ctx->env, it) {
        sp_str_t k = *sp_ht_it_getkp(*ctx->env, it);
        if (sp_str_equal(k, name)) {
            *sp_ht_it_getp(*ctx->env, it) = *val;
            return;
        }
    }
    sp_ht_insert(*ctx->env, name, *val);
}

/*=============================================================================
 * Expression Evaluation
 *=============================================================================*/

static fxsh_ct_value_t *eval_expr(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx);
fxsh_ct_value_t *fxsh_ct_type_name(fxsh_ct_value_t *type_val);
static fxsh_ct_value_t *fxsh_ct_is_record(fxsh_ct_value_t *type_val);
static fxsh_ct_value_t *fxsh_ct_is_tuple(fxsh_ct_value_t *type_val);
static fxsh_ct_value_t *eval_match(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx);
static fxsh_ct_value_t *fxsh_ct_op_sqlite_sql(fxsh_ct_value_t *type_val, sp_str_t table_name);
static fxsh_ct_value_t *fxsh_ct_op_sql(fxsh_ct_value_t *dsl);

static void ct_set_error(const c8 *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ct_error_msg, sizeof(g_ct_error_msg), fmt, ap);
    va_end(ap);
}

const c8 *fxsh_ct_last_error(void) {
    return g_ct_error_msg[0] ? g_ct_error_msg : NULL;
}

static fxsh_ct_env_t *clone_env(fxsh_ct_env_t *src) {
    if (!src)
        return NULL;
    fxsh_ct_env_t *dst = fxsh_alloc0(sizeof(fxsh_ct_env_t));
    *dst = SP_NULLPTR;
    sp_ht_for(*src, it) {
        sp_str_t k = *sp_ht_it_getkp(*src, it);
        fxsh_ct_value_t v = *sp_ht_it_getp(*src, it);
        sp_ht_insert(*dst, k, v);
    }
    return dst;
}

static void ct_env_bind_local(fxsh_ct_env_t **env, sp_str_t name, fxsh_ct_value_t *val) {
    if (!env || !val)
        return;
    if (!*env) {
        *env = fxsh_alloc0(sizeof(fxsh_ct_env_t));
        **env = SP_NULLPTR;
    }
    sp_ht_for(**env, it) {
        sp_str_t k = *sp_ht_it_getkp(**env, it);
        if (sp_str_equal(k, name)) {
            *sp_ht_it_getp(**env, it) = *val;
            return;
        }
    }
    sp_ht_insert(**env, name, *val);
}

static void ct_bind_recursive_closure(sp_str_t self_name, fxsh_ct_value_t *val) {
    if (!val || val->kind != CT_FUNCTION)
        return;
    fxsh_ct_env_t *closure = (fxsh_ct_env_t *)val->data.func_val.closure;
    ct_env_bind_local(&closure, self_name, val);
    val->data.func_val.closure = closure;
}

static fxsh_ast_node_t *clone_ast(fxsh_ast_node_t *n);

static fxsh_ast_list_t clone_ast_list(fxsh_ast_list_t xs) {
    fxsh_ast_list_t out = SP_NULLPTR;
    sp_dyn_array_for(xs, i) {
        sp_dyn_array_push(out, clone_ast(xs[i]));
    }
    return out;
}

static fxsh_ast_node_t *clone_ast(fxsh_ast_node_t *n) {
    if (!n)
        return NULL;
    fxsh_ast_node_t *c = fxsh_alloc0(sizeof(fxsh_ast_node_t));
    c->kind = n->kind;
    c->loc = n->loc;
    c->data = n->data;

    switch (n->kind) {
        case AST_BINARY:
            c->data.binary.left = clone_ast(n->data.binary.left);
            c->data.binary.right = clone_ast(n->data.binary.right);
            break;
        case AST_UNARY:
            c->data.unary.operand = clone_ast(n->data.unary.operand);
            break;
        case AST_CALL:
            c->data.call.func = clone_ast(n->data.call.func);
            c->data.call.args = clone_ast_list(n->data.call.args);
            break;
        case AST_LAMBDA:
            c->data.lambda.params = clone_ast_list(n->data.lambda.params);
            c->data.lambda.body = clone_ast(n->data.lambda.body);
            break;
        case AST_LET:
        case AST_DECL_LET:
            c->data.let.pattern = clone_ast(n->data.let.pattern);
            c->data.let.type = clone_ast(n->data.let.type);
            c->data.let.value = clone_ast(n->data.let.value);
            break;
        case AST_LET_IN:
            c->data.let_in.bindings = clone_ast_list(n->data.let_in.bindings);
            c->data.let_in.body = clone_ast(n->data.let_in.body);
            break;
        case AST_IF:
            c->data.if_expr.cond = clone_ast(n->data.if_expr.cond);
            c->data.if_expr.then_branch = clone_ast(n->data.if_expr.then_branch);
            c->data.if_expr.else_branch = clone_ast(n->data.if_expr.else_branch);
            break;
        case AST_MATCH:
            c->data.match_expr.expr = clone_ast(n->data.match_expr.expr);
            c->data.match_expr.arms = clone_ast_list(n->data.match_expr.arms);
            break;
        case AST_MATCH_ARM:
            c->data.match_arm.pattern = clone_ast(n->data.match_arm.pattern);
            c->data.match_arm.guard = clone_ast(n->data.match_arm.guard);
            c->data.match_arm.body = clone_ast(n->data.match_arm.body);
            break;
        case AST_PIPE:
            c->data.pipe.left = clone_ast(n->data.pipe.left);
            c->data.pipe.right = clone_ast(n->data.pipe.right);
            break;
        case AST_TUPLE:
        case AST_LIST:
        case AST_RECORD:
        case AST_PAT_TUPLE:
        case AST_PAT_RECORD:
        case AST_TYPE_RECORD:
        case AST_PAT_CONS:
            c->data.elements = clone_ast_list(n->data.elements);
            break;
        case AST_FIELD_ACCESS:
            c->data.field.object = clone_ast(n->data.field.object);
            c->data.field.type = clone_ast(n->data.field.type);
            break;
        case AST_TYPE_APP:
            c->data.type_con.args = clone_ast_list(n->data.type_con.args);
            break;
        case AST_TYPE_ARROW:
            c->data.type_arrow.param = clone_ast(n->data.type_arrow.param);
            c->data.type_arrow.ret = clone_ast(n->data.type_arrow.ret);
            break;
        case AST_CT_TYPE_OF:
        case AST_CT_TYPE_NAME:
        case AST_CT_QUOTE:
        case AST_CT_UNQUOTE:
        case AST_CT_SPLICE:
        case AST_CT_EVAL:
        case AST_CT_SQL:
        case AST_CT_COMPILE_ERROR:
        case AST_CT_COMPILE_LOG:
        case AST_CT_PANIC:
            c->data.ct_type_of.operand = clone_ast(n->data.ct_type_of.operand);
            break;
        case AST_CT_SIZE_OF:
        case AST_CT_ALIGN_OF:
        case AST_CT_FIELDS_OF:
        case AST_CT_IS_RECORD:
        case AST_CT_IS_TUPLE:
        case AST_CT_JSON_SCHEMA:
            c->data.ct_type_op.type_expr = clone_ast(n->data.ct_type_op.type_expr);
            break;
        case AST_CT_SQLITE_SQL:
            c->data.ct_sqlite_sql.type_expr = clone_ast(n->data.ct_sqlite_sql.type_expr);
            break;
        case AST_CT_HAS_FIELD:
            c->data.ct_has_field.type_expr = clone_ast(n->data.ct_has_field.type_expr);
            break;
        case AST_CT_CTOR_APPLY:
            c->data.ct_ctor_apply.type_args = clone_ast_list(n->data.ct_ctor_apply.type_args);
            break;
        case AST_CONSTR_APPL:
            c->data.constr_appl.args = clone_ast_list(n->data.constr_appl.args);
            break;
        case AST_TYPE_DEF:
            c->data.type_def.type_params = clone_ast_list(n->data.type_def.type_params);
            c->data.type_def.constructors = clone_ast_list(n->data.type_def.constructors);
            break;
        case AST_PROGRAM:
            c->data.decls = clone_ast_list(n->data.decls);
            break;
        default:
            break;
    }
    return c;
}

static bool ct_value_to_ast_expr(fxsh_ct_value_t *v, fxsh_ast_node_t **out);

static bool ct_function_to_ast_expr(fxsh_ct_value_t *v, fxsh_ast_node_t **out) {
    if (!v || v->kind != CT_FUNCTION)
        return false;

    fxsh_ast_node_t *body = clone_ast(v->data.func_val.body);
    fxsh_ct_env_t *closure_env = (fxsh_ct_env_t *)v->data.func_val.closure;
    fxsh_ast_list_t binds = SP_NULLPTR;

    if (closure_env) {
        sp_ht_for(*closure_env, it) {
            sp_str_t name = *sp_ht_it_getkp(*closure_env, it);
            fxsh_ct_value_t *cv = sp_ht_it_getp(*closure_env, it);
            if (cv && cv->kind == CT_FUNCTION)
                continue;
            fxsh_ast_node_t *rhs = NULL;
            if (!ct_value_to_ast_expr(cv, &rhs))
                continue;
            fxsh_ast_node_t *b =
                fxsh_ast_let(name, rhs, false, false, body ? body->loc : (fxsh_loc_t){0});
            sp_dyn_array_push(binds, b);
        }
    }

    if (sp_dyn_array_size(binds) > 0) {
        fxsh_ast_node_t *letin = fxsh_alloc0(sizeof(fxsh_ast_node_t));
        letin->kind = AST_LET_IN;
        letin->loc = body ? body->loc : (fxsh_loc_t){0};
        letin->data.let_in.bindings = binds;
        letin->data.let_in.body = body;
        body = letin;
    }

    fxsh_ast_node_t *fn = body;
    for (s32 i = (s32)sp_dyn_array_size(v->data.func_val.params) - 1; i >= 0; i--) {
        fxsh_ast_list_t one = SP_NULLPTR;
        sp_dyn_array_push(one, clone_ast(v->data.func_val.params[i]));
        fn = fxsh_ast_lambda(one, fn, body ? body->loc : (fxsh_loc_t){0});
    }
    *out = fn;
    return true;
}

static bool ct_value_to_ast_expr(fxsh_ct_value_t *v, fxsh_ast_node_t **out) {
    if (!v || !out)
        return false;
    switch (v->kind) {
        case CT_UNIT: {
            fxsh_ast_node_t *n = fxsh_alloc0(sizeof(fxsh_ast_node_t));
            n->kind = AST_LIT_UNIT;
            *out = n;
            return true;
        }
        case CT_BOOL:
            *out = fxsh_ast_lit_bool(v->data.bool_val, (fxsh_loc_t){0});
            return true;
        case CT_INT:
            *out = fxsh_ast_lit_int(v->data.int_val, (fxsh_loc_t){0});
            return true;
        case CT_FLOAT:
            *out = fxsh_ast_lit_float(v->data.float_val, (fxsh_loc_t){0});
            return true;
        case CT_STRING:
            *out = fxsh_ast_lit_string(v->data.string_val, (fxsh_loc_t){0});
            return true;
        case CT_TYPE: {
            fxsh_ast_node_t *n = fxsh_alloc0(sizeof(fxsh_ast_node_t));
            n->kind = AST_TYPE_VALUE;
            n->data.type_value = v->data.type_val;
            *out = n;
            return true;
        }
        case CT_AST:
            *out = clone_ast(v->data.ast_val);
            return true;
        case CT_LIST: {
            fxsh_ast_node_t *n = fxsh_alloc0(sizeof(fxsh_ast_node_t));
            n->kind = AST_LIST;
            n->data.elements = SP_NULLPTR;
            for (u32 i = 0; i < v->data.list_val.len; i++) {
                fxsh_ast_node_t *it = NULL;
                if (!ct_value_to_ast_expr(v->data.list_val.items[i], &it))
                    return false;
                sp_dyn_array_push(n->data.elements, it);
            }
            *out = n;
            return true;
        }
        case CT_STRUCT: {
            fxsh_ast_node_t *rec = fxsh_alloc0(sizeof(fxsh_ast_node_t));
            rec->kind = AST_RECORD;
            rec->data.elements = SP_NULLPTR;
            for (u32 i = 0; i < v->data.struct_val.num_fields; i++) {
                fxsh_ct_field_t *f = &v->data.struct_val.fields[i];
                fxsh_ast_node_t *fv = NULL;
                if (!ct_value_to_ast_expr(f->value, &fv))
                    return false;
                fxsh_ast_node_t *fn = fxsh_alloc0(sizeof(fxsh_ast_node_t));
                fn->kind = AST_FIELD_ACCESS;
                fn->data.field.field = f->name;
                fn->data.field.object = fv;
                sp_dyn_array_push(rec->data.elements, fn);
            }
            *out = rec;
            return true;
        }
        case CT_FUNCTION:
            return ct_function_to_ast_expr(v, out);
        default:
            return false;
    }
}

static sp_str_t param_name(fxsh_ast_node_t *param) {
    if (!param)
        return (sp_str_t){0};
    if (param->kind == AST_IDENT || param->kind == AST_PAT_VAR)
        return param->data.ident;
    return (sp_str_t){0};
}

static fxsh_ct_value_t *eval_binary(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *left = eval_expr(ast->data.binary.left, ctx);
    fxsh_ct_value_t *right = eval_expr(ast->data.binary.right, ctx);

    if (!left || !right)
        return NULL;

    switch (ast->data.binary.op) {
        case TOK_PLUS:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_int(left->data.int_val + right->data.int_val);
            }
            if (left->kind == CT_FLOAT && right->kind == CT_FLOAT) {
                return fxsh_ct_float(left->data.float_val + right->data.float_val);
            }
            break;
        case TOK_CONCAT:
            if (left->kind == CT_STRING && right->kind == CT_STRING) {
                u32 len = left->data.string_val.len + right->data.string_val.len;
                c8 *buf = (c8 *)fxsh_alloc0(len + 1);
                memcpy(buf, left->data.string_val.data, left->data.string_val.len);
                memcpy(buf + left->data.string_val.len, right->data.string_val.data,
                       right->data.string_val.len);
                return fxsh_ct_string((sp_str_t){.data = buf, .len = len});
            }
            break;
        case TOK_MINUS:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_int(left->data.int_val - right->data.int_val);
            }
            break;
        case TOK_STAR:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_int(left->data.int_val * right->data.int_val);
            }
            break;
        case TOK_SLASH:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_int(left->data.int_val / right->data.int_val);
            }
            break;
        case TOK_EQ:
            return fxsh_ct_bool(fxsh_ct_equal(left, right));
        case TOK_NEQ:
            return fxsh_ct_bool(!fxsh_ct_equal(left, right));
        case TOK_LT:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_bool(left->data.int_val < right->data.int_val);
            }
            break;
        case TOK_LEQ:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_bool(left->data.int_val <= right->data.int_val);
            }
            break;
        case TOK_GT:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_bool(left->data.int_val > right->data.int_val);
            }
            break;
        case TOK_GEQ:
            if (left->kind == CT_INT && right->kind == CT_INT) {
                return fxsh_ct_bool(left->data.int_val >= right->data.int_val);
            }
            break;
        case TOK_AND:
            if (left->kind == CT_BOOL && right->kind == CT_BOOL) {
                return fxsh_ct_bool(left->data.bool_val && right->data.bool_val);
            }
            break;
        case TOK_OR:
            if (left->kind == CT_BOOL && right->kind == CT_BOOL) {
                return fxsh_ct_bool(left->data.bool_val || right->data.bool_val);
            }
            break;
        default:
            break;
    }

    return NULL; /* Cannot evaluate at compile time */
}

static fxsh_ct_value_t *eval_unary(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *operand = eval_expr(ast->data.unary.operand, ctx);
    if (!operand)
        return NULL;

    switch (ast->data.unary.op) {
        case TOK_MINUS:
            if (operand->kind == CT_INT) {
                return fxsh_ct_int(-operand->data.int_val);
            }
            break;
        case TOK_NOT:
            if (operand->kind == CT_BOOL) {
                return fxsh_ct_bool(!operand->data.bool_val);
            }
            break;
        default:
            break;
    }

    return NULL;
}

static fxsh_ct_value_t *eval_if(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *cond = eval_expr(ast->data.if_expr.cond, ctx);
    if (!cond || cond->kind != CT_BOOL)
        return NULL;

    if (cond->data.bool_val) {
        return eval_expr(ast->data.if_expr.then_branch, ctx);
    } else if (ast->data.if_expr.else_branch) {
        return eval_expr(ast->data.if_expr.else_branch, ctx);
    }
    return fxsh_ct_unit();
}

static bool ct_bind_pattern(fxsh_ast_node_t *pat, fxsh_ct_value_t *val, fxsh_ct_env_t **env) {
    if (!pat)
        return false;

    switch (pat->kind) {
        case AST_PAT_WILD:
            return true;
        case AST_PAT_VAR:
            ct_env_bind_local(env, pat->data.ident, val);
            return true;
        case AST_PAT_TUPLE:
        case AST_LIST: {
            if (!val || val->kind != CT_LIST)
                return false;
            u32 n = (u32)sp_dyn_array_size(pat->data.elements);
            if (n != val->data.list_val.len)
                return false;
            for (u32 i = 0; i < n; i++) {
                if (!ct_bind_pattern(pat->data.elements[i], val->data.list_val.items[i], env))
                    return false;
            }
            return true;
        }
        case AST_PAT_CONS: {
            if (!val || val->kind != CT_LIST || val->data.list_val.len == 0)
                return false;
            if (sp_dyn_array_size(pat->data.elements) != 2)
                return false;
            fxsh_ct_value_t *head = val->data.list_val.items[0];
            fxsh_ct_value_t *tail = fxsh_ct_list(val->data.list_val.items + 1, val->data.list_val.len - 1);
            return ct_bind_pattern(pat->data.elements[0], head, env) &&
                   ct_bind_pattern(pat->data.elements[1], tail, env);
        }
        case AST_PAT_RECORD: {
            if (!val || val->kind != CT_STRUCT)
                return false;
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    return false;
                fxsh_ct_value_t *fv = fxsh_ct_record_get_field(val, f->data.field.field);
                if (!fv)
                    return false;
                if (!ct_bind_pattern(f->data.field.object, fv, env))
                    return false;
            }
            return true;
        }
        case AST_LIT_INT:
            return val && val->kind == CT_INT && pat->data.lit_int == val->data.int_val;
        case AST_LIT_FLOAT:
            return val && val->kind == CT_FLOAT && pat->data.lit_float == val->data.float_val;
        case AST_LIT_STRING:
            return val && val->kind == CT_STRING && sp_str_equal(pat->data.lit_string, val->data.string_val);
        case AST_LIT_BOOL:
            return val && val->kind == CT_BOOL && pat->data.lit_bool == val->data.bool_val;
        case AST_LIT_UNIT:
            return val && val->kind == CT_UNIT;
        case AST_PAT_CONSTR:
        default:
            return false;
    }
}

static fxsh_ct_value_t *eval_match(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *mv = eval_expr(ast->data.match_expr.expr, ctx);
    if (!mv)
        return NULL;

    sp_dyn_array_for(ast->data.match_expr.arms, i) {
        fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
        if (!arm || arm->kind != AST_MATCH_ARM)
            continue;

        fxsh_comptime_ctx_t arm_ctx = *ctx;
        arm_ctx.env = clone_env(ctx->env);
        if (!ct_bind_pattern(arm->data.match_arm.pattern, mv, &arm_ctx.env))
            continue;

        if (arm->data.match_arm.guard) {
            fxsh_ct_value_t *g = eval_expr(arm->data.match_arm.guard, &arm_ctx);
            if (!g || g->kind != CT_BOOL || !g->data.bool_val)
                continue;
        }
        return eval_expr(arm->data.match_arm.body, &arm_ctx);
    }

    ct_set_error("non-exhaustive compile-time match at %u:%u", ast->loc.line, ast->loc.column);
    return NULL;
}

static fxsh_ct_value_t *eval_let_in(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_comptime_ctx_t scoped = *ctx;
    scoped.env = clone_env(ctx->env);

    sp_dyn_array_for(ast->data.let_in.bindings, i) {
        fxsh_ast_node_t *binding = ast->data.let_in.bindings[i];
        if (binding->kind == AST_DECL_LET || binding->kind == AST_LET) {
            if (binding->data.let.is_rec) {
                fxsh_ct_value_t *slot = fxsh_ct_unit();
                ct_env_bind_local(&scoped.env, binding->data.let.name, slot);
                fxsh_ct_value_t *val = eval_expr(binding->data.let.value, &scoped);
                if (!val)
                    return NULL;
                ct_bind_recursive_closure(binding->data.let.name, val);
                ct_env_bind_local(&scoped.env, binding->data.let.name, val);
                continue;
            }

            fxsh_ct_value_t *val = eval_expr(binding->data.let.value, &scoped);
            if (!val)
                return NULL;
            ct_env_bind_local(&scoped.env, binding->data.let.name, val);
        }
    }

    return eval_expr(ast->data.let_in.body, &scoped);
}

static fxsh_ct_value_t *eval_call(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *fn = eval_expr(ast->data.call.func, ctx);
    if (!fn || fn->kind != CT_FUNCTION)
        return NULL;

    u32 nargs = (u32)sp_dyn_array_size(ast->data.call.args);
    if (nargs == 0)
        return fn;

    fxsh_ct_value_t **args = fxsh_alloc0(sizeof(fxsh_ct_value_t *) * nargs);
    for (u32 i = 0; i < nargs; i++) {
        args[i] = eval_expr(ast->data.call.args[i], ctx);
        if (!args[i])
            return NULL;
    }

    fxsh_ct_value_t *cur_fn = fn;
    u32 arg_i = 0;

    while (arg_i < nargs) {
        if (!cur_fn || cur_fn->kind != CT_FUNCTION)
            return NULL;

        u32 nparams = (u32)sp_dyn_array_size(cur_fn->data.func_val.params);
        u32 available = nargs - arg_i;
        u32 take = available < nparams ? available : nparams;

        fxsh_comptime_ctx_t new_ctx = *ctx;
        fxsh_ct_env_t *closure_env = (fxsh_ct_env_t *)cur_fn->data.func_val.closure;
        new_ctx.env = clone_env(closure_env);

        for (u32 i = 0; i < take; i++) {
            fxsh_ast_node_t *p = cur_fn->data.func_val.params[i];
            sp_str_t name = param_name(p);
            if (name.data && name.len > 0)
                bind_var(&new_ctx, name, args[arg_i + i]);
        }

        if (take < nparams) {
            fxsh_ct_value_t *partial = fxsh_alloc0(sizeof(fxsh_ct_value_t));
            partial->kind = CT_FUNCTION;
            partial->data.func_val.params = SP_NULLPTR;
            for (u32 i = take; i < nparams; i++) {
                sp_dyn_array_push(partial->data.func_val.params, cur_fn->data.func_val.params[i]);
            }
            partial->data.func_val.body = cur_fn->data.func_val.body;
            partial->data.func_val.closure = new_ctx.env;
            return partial;
        }

        fxsh_ct_value_t *applied = eval_expr(cur_fn->data.func_val.body, &new_ctx);
        if (!applied)
            return NULL;
        cur_fn = applied;
        arg_i += take;
    }

    return cur_fn;
}

static fxsh_ct_value_t *eval_lambda(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_FUNCTION;
    val->data.func_val.params = ast->data.lambda.params;
    val->data.func_val.body = ast->data.lambda.body;
    val->data.func_val.closure = clone_env(ctx->env);
    return val;
}

static fxsh_ct_value_t *eval_literal(fxsh_ast_node_t *ast) {
    switch (ast->kind) {
        case AST_LIT_INT:
            return fxsh_ct_int(ast->data.lit_int);
        case AST_LIT_FLOAT:
            return fxsh_ct_float(ast->data.lit_float);
        case AST_LIT_STRING:
            return fxsh_ct_string(ast->data.lit_string);
        case AST_LIT_BOOL:
            return fxsh_ct_bool(ast->data.lit_bool);
        case AST_LIT_UNIT:
            return fxsh_ct_unit();
        default:
            return NULL;
    }
}

static fxsh_ct_value_t *eval_ident(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    return lookup_var(ctx, ast->data.ident);
}

static fxsh_ct_value_t *eval_type_operand_to_ct_type(fxsh_ast_node_t *type_expr,
                                                     fxsh_comptime_ctx_t *ctx) {
    if (!type_expr)
        return NULL;
    fxsh_ct_value_t *v = eval_expr(type_expr, ctx);
    if (v && v->kind == CT_TYPE)
        return v;
    return fxsh_ct_op_type_of(type_expr, ctx);
}

typedef fxsh_type_constructor_t *(*ct_ctor_builder_fn)(void);

typedef struct {
    sp_str_t name;
    ct_ctor_builder_fn build;
} ct_ctor_registry_entry_t;

static void ct_register_type_def(fxsh_comptime_ctx_t *ctx, fxsh_ast_node_t *type_def) {
    if (!ctx || !type_def || type_def->kind != AST_TYPE_DEF)
        return;

    sp_dyn_array_for(ctx->type_defs, i) {
        fxsh_ast_node_t *existing = ctx->type_defs[i];
        if (existing && existing->kind == AST_TYPE_DEF &&
            sp_str_equal(existing->data.type_def.name, type_def->data.type_def.name)) {
            ctx->type_defs[i] = type_def;
            return;
        }
    }

    sp_dyn_array_push(ctx->type_defs, type_def);
}

static fxsh_type_t *ct_make_postfix_ctor_target_type(sp_str_t type_name,
                                                     sp_dyn_array(fxsh_type_t *) params) {
    fxsh_type_t *packed_args = NULL;

    sp_dyn_array_for(params, i) {
        if (!packed_args) {
            packed_args = params[i];
        } else {
            packed_args = fxsh_type_apply(params[i], packed_args);
        }
    }

    if (!packed_args)
        return fxsh_type_con(type_name);
    return fxsh_type_apply(fxsh_type_con(type_name), packed_args);
}

static fxsh_type_constructor_t *ct_make_type_def_ctor(fxsh_ast_node_t *type_def) {
    if (!type_def || type_def->kind != AST_TYPE_DEF)
        return NULL;

    fxsh_type_constructor_t *ctor = fxsh_alloc0(sizeof(fxsh_type_constructor_t));
    sp_dyn_array(fxsh_type_t *) params = SP_NULLPTR;

    ctor->name = type_def->data.type_def.name;
    ctor->fields = SP_NULLPTR;
    ctor->type_params = SP_NULLPTR;

    sp_dyn_array_for(type_def->data.type_def.type_params, i) {
        fxsh_type_var_t var = fxsh_fresh_var();
        sp_dyn_array_push(ctor->type_params, var);
        sp_dyn_array_push(params, fxsh_type_var(var));
    }

    ctor->target_type = ct_make_postfix_ctor_target_type(type_def->data.type_def.name, params);
    ctor->kind = ctor->target_type ? ctor->target_type->kind : TYPE_CON;

    sp_dyn_array_free(params);
    return ctor;
}

static fxsh_type_constructor_t *ct_make_unary_app_ctor(sp_str_t ctor_name, sp_str_t type_name) {
    fxsh_type_constructor_t *ctor = fxsh_alloc0(sizeof(fxsh_type_constructor_t));
    fxsh_type_var_t arg_var = fxsh_fresh_var();

    ctor->name = ctor_name;
    ctor->kind = TYPE_APP;
    ctor->type_params = SP_NULLPTR;
    sp_dyn_array_push(ctor->type_params, arg_var);
    ctor->fields = SP_NULLPTR;
    ctor->target_type = fxsh_type_apply(fxsh_type_con(type_name), fxsh_type_var(arg_var));
    return ctor;
}

static fxsh_type_constructor_t *ct_make_binary_app_ctor(sp_str_t ctor_name, sp_str_t type_name) {
    fxsh_type_constructor_t *ctor = fxsh_alloc0(sizeof(fxsh_type_constructor_t));
    fxsh_type_var_t lhs_var = fxsh_fresh_var();
    fxsh_type_var_t rhs_var = fxsh_fresh_var();
    fxsh_type_t *lhs = fxsh_type_var(lhs_var);
    fxsh_type_t *rhs = fxsh_type_var(rhs_var);

    ctor->name = ctor_name;
    ctor->kind = TYPE_APP;
    ctor->type_params = SP_NULLPTR;
    sp_dyn_array_push(ctor->type_params, lhs_var);
    sp_dyn_array_push(ctor->type_params, rhs_var);
    ctor->fields = SP_NULLPTR;

    /* Match parser/source order: `A B result` lowers to result applied to `(B A)`. */
    ctor->target_type = fxsh_type_apply(fxsh_type_con(type_name), fxsh_type_apply(rhs, lhs));
    return ctor;
}

static fxsh_type_constructor_t *ct_make_list_ctor(void) {
    return ct_make_unary_app_ctor((sp_str_t){.data = "List", .len = 4}, TYPE_LIST);
}

static fxsh_type_constructor_t *ct_make_option_ctor(void) {
    return ct_make_unary_app_ctor((sp_str_t){.data = "Option", .len = 6}, TYPE_OPTION);
}

static fxsh_type_constructor_t *ct_make_result_ctor(void) {
    return ct_make_binary_app_ctor((sp_str_t){.data = "Result", .len = 6}, TYPE_RESULT);
}

static fxsh_type_constructor_t *ct_lookup_type_constructor(fxsh_comptime_ctx_t *ctx,
                                                           sp_str_t ctor_name) {
    if (ctx) {
        sp_dyn_array_for(ctx->type_defs, i) {
            fxsh_ast_node_t *type_def = ctx->type_defs[i];
            if (type_def && type_def->kind == AST_TYPE_DEF &&
                sp_str_equal(type_def->data.type_def.name, ctor_name)) {
                return ct_make_type_def_ctor(type_def);
            }
        }
    }

    static const ct_ctor_registry_entry_t entries[] = {
        {{.data = "Vector", .len = 6}, fxsh_ct_make_vector_ctor},
        {{.data = "vectorOf", .len = 8}, fxsh_ct_make_vector_ctor},
        {{.data = "List", .len = 4}, ct_make_list_ctor},
        {{.data = "listOf", .len = 6}, ct_make_list_ctor},
        {{.data = "Option", .len = 6}, ct_make_option_ctor},
        {{.data = "optionOf", .len = 8}, ct_make_option_ctor},
        {{.data = "Result", .len = 6}, ct_make_result_ctor},
        {{.data = "resultOf", .len = 8}, ct_make_result_ctor},
    };

    for (u32 i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (sp_str_equal(ctor_name, entries[i].name))
            return entries[i].build();
    }
    return NULL;
}

static fxsh_ct_value_t *eval_expr(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    if (!ast)
        return fxsh_ct_unit();

    switch (ast->kind) {
        case AST_LIT_INT:
        case AST_LIT_FLOAT:
        case AST_LIT_STRING:
        case AST_LIT_BOOL:
        case AST_LIT_UNIT:
            return eval_literal(ast);
        case AST_TYPE_VALUE:
            return fxsh_ct_type(ast->data.type_value);
        case AST_IDENT:
            return eval_ident(ast, ctx);
        case AST_CONSTR_APPL:
            /* Allow uppercase let-bound comptime identifiers in operator args,
             * e.g. @unquote(Q), where lexer/parser yields TYPE_IDENT form. */
            if (sp_dyn_array_size(ast->data.constr_appl.args) == 0)
                return lookup_var(ctx, ast->data.constr_appl.constr_name);
            return NULL;
        case AST_BINARY:
            return eval_binary(ast, ctx);
        case AST_UNARY:
            return eval_unary(ast, ctx);
        case AST_IF:
            return eval_if(ast, ctx);
        case AST_MATCH:
            return eval_match(ast, ctx);
        case AST_LET_IN:
            return eval_let_in(ast, ctx);
        case AST_LAMBDA:
            return eval_lambda(ast, ctx);
        case AST_CALL:
            return eval_call(ast, ctx);
        case AST_TUPLE: {
            u32 n = (u32)sp_dyn_array_size(ast->data.elements);
            fxsh_ct_value_t **items = fxsh_alloc0(sizeof(fxsh_ct_value_t *) * n);
            for (u32 i = 0; i < n; i++) {
                items[i] = eval_expr(ast->data.elements[i], ctx);
                if (!items[i])
                    return NULL;
            }
            return fxsh_ct_list(items, n);
        }
        case AST_LIST: {
            u32 n = (u32)sp_dyn_array_size(ast->data.elements);
            fxsh_ct_value_t **items = fxsh_alloc0(sizeof(fxsh_ct_value_t *) * n);
            for (u32 i = 0; i < n; i++) {
                items[i] = eval_expr(ast->data.elements[i], ctx);
                if (!items[i])
                    return NULL;
            }
            return fxsh_ct_list(items, n);
        }
        case AST_RECORD: {
            fxsh_ct_value_t *rec = fxsh_ct_make_record_type((sp_str_t){.data = "Record", .len = 6});
            sp_dyn_array_for(ast->data.elements, i) {
                fxsh_ast_node_t *f = ast->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS)
                    continue;
                fxsh_ct_value_t *fv = eval_expr(f->data.field.object, ctx);
                if (!fv)
                    return NULL;
                fxsh_ct_record_add_field(rec, f->data.field.field, fv);
            }
            return rec;
        }
        case AST_FIELD_ACCESS: {
            fxsh_ct_value_t *obj = eval_expr(ast->data.field.object, ctx);
            if (!obj || obj->kind != CT_STRUCT)
                return NULL;
            return fxsh_ct_record_get_field(obj, ast->data.field.field);
        }
        case AST_PIPE: {
            /* Desugar pipe: a |> f = f a */
            fxsh_ast_node_t pipe_call = {
                .kind = AST_CALL,
                .loc = ast->loc,
                .data.call = {.func = ast->data.pipe.right, .args = SP_NULLPTR}};
            sp_dyn_array_push(pipe_call.data.call.args, ast->data.pipe.left);
            fxsh_ct_value_t *result = eval_call(&pipe_call, ctx);
            sp_dyn_array_free(pipe_call.data.call.args);
            return result;
        }
        case AST_CT_TYPE_OF: {
            return fxsh_ct_op_type_of(ast->data.ct_type_of.operand, ctx);
        }
        case AST_CT_TYPE_NAME: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_type_name(type_val);
        }
        case AST_CT_SIZE_OF: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_op_size_of(type_val);
        }
        case AST_CT_ALIGN_OF: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_op_align_of(type_val);
        }
        case AST_CT_FIELDS_OF: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_op_fields_of(type_val);
        }
        case AST_CT_IS_RECORD: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_is_record(type_val);
        }
        case AST_CT_IS_TUPLE: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_is_tuple(type_val);
        }
        case AST_CT_JSON_SCHEMA: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_op_json_schema(type_val);
        }
        case AST_CT_SQLITE_SQL: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_sqlite_sql.type_expr, ctx);
            return fxsh_ct_op_sqlite_sql(type_val, ast->data.ct_sqlite_sql.table_name);
        }
        case AST_CT_CTOR_APPLY: {
            fxsh_type_constructor_t *ctor =
                ct_lookup_type_constructor(ctx, ast->data.ct_ctor_apply.ctor_name);
            if (!ctor) {
                ct_set_error("unknown compile-time type constructor: %.*s",
                             ast->data.ct_ctor_apply.ctor_name.len,
                             ast->data.ct_ctor_apply.ctor_name.data);
                return NULL;
            }

            sp_dyn_array(fxsh_type_t *) type_args = SP_NULLPTR;
            sp_dyn_array_for(ast->data.ct_ctor_apply.type_args, i) {
                fxsh_ct_value_t *type_val =
                    eval_type_operand_to_ct_type(ast->data.ct_ctor_apply.type_args[i], ctx);
                if (!type_val || type_val->kind != CT_TYPE) {
                    ct_set_error("compile-time type constructor `%.*s` expects type arguments",
                                 ast->data.ct_ctor_apply.ctor_name.len,
                                 ast->data.ct_ctor_apply.ctor_name.data);
                    return NULL;
                }
                sp_dyn_array_push(type_args, type_val->data.type_val);
            }

            fxsh_type_t *inst = fxsh_ct_instantiate_generic(ctor, type_args);
            sp_dyn_array_free(type_args);
            if (!inst) {
                ct_set_error("compile-time type constructor `%.*s` instantiation failed",
                             ast->data.ct_ctor_apply.ctor_name.len,
                             ast->data.ct_ctor_apply.ctor_name.data);
                return NULL;
            }
            return fxsh_ct_type(inst);
        }
        case AST_CT_HAS_FIELD: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_has_field.type_expr, ctx);
            return fxsh_ct_op_has_field(type_val, ast->data.ct_has_field.field_name);
        }
        case AST_CT_QUOTE: {
            return fxsh_ct_ast(ast->data.ct_type_of.operand);
        }
        case AST_CT_UNQUOTE:
        case AST_CT_SPLICE: {
            fxsh_ct_value_t *quoted = eval_expr(ast->data.ct_type_of.operand, ctx);
            if (!quoted || quoted->kind != CT_AST)
                return NULL;
            return eval_expr(quoted->data.ast_val, ctx);
        }
        case AST_CT_EVAL:
            return eval_expr(ast->data.ct_type_of.operand, ctx);
        case AST_CT_SQL: {
            fxsh_ct_value_t *dsl = eval_expr(ast->data.ct_type_of.operand, ctx);
            return fxsh_ct_op_sql(dsl);
        }
        case AST_CT_COMPILE_LOG: {
            fxsh_ct_value_t *v = eval_expr(ast->data.ct_type_of.operand, ctx);
            if (!v)
                return NULL;
            fprintf(stderr, "[comptime] %s\n", fxsh_ct_value_to_string(v));
            return fxsh_ct_unit();
        }
        case AST_CT_COMPILE_ERROR: {
            fxsh_ct_value_t *v = eval_expr(ast->data.ct_type_of.operand, ctx);
            if (!v)
                return NULL;
            ct_set_error("compileError: %s", fxsh_ct_value_to_string(v));
            return NULL;
        }
        case AST_CT_PANIC: {
            fxsh_ct_value_t *v = eval_expr(ast->data.ct_type_of.operand, ctx);
            if (!v)
                return NULL;
            ct_set_error("panic: %s", fxsh_ct_value_to_string(v));
            return NULL;
        }
        default:
            return NULL;
    }
}

/*=============================================================================
 * Public Evaluation Interface
 *=============================================================================*/

fxsh_ct_result_t fxsh_ct_eval(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_result_t result = {ERR_OK, NULL};

    if (!ast) {
        result.value = fxsh_ct_unit();
        return result;
    }

    /* Handle declarations */
    if ((ast->kind == AST_DECL_LET || ast->kind == AST_LET) && ast->data.let.is_comptime) {
        if (ast->data.let.is_rec) {
            fxsh_ct_value_t *slot = fxsh_ct_unit();
            bind_var(ctx, ast->data.let.name, slot);
        }

        fxsh_ct_value_t *val = eval_expr(ast->data.let.value, ctx);
        if (val) {
            if (ast->data.let.is_rec)
                ct_bind_recursive_closure(ast->data.let.name, val);
            bind_var(ctx, ast->data.let.name, val);
            result.value = val;
        } else {
            result.error = ERR_INTERNAL;
        }
        return result;
    }

    if (ast->kind == AST_PROGRAM) {
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *d = ast->data.decls[i];
            if (d && d->kind == AST_TYPE_DEF) {
                ct_register_type_def(ctx, d);
                continue;
            }
            if ((d->kind == AST_DECL_LET || d->kind == AST_LET) && d->data.let.is_comptime) {
                fxsh_ct_result_t r = fxsh_ct_eval(d, ctx);
                if (r.error != ERR_OK)
                    return r;
            }
        }
        result.value = fxsh_ct_unit();
        return result;
    }

    if (ast->kind == AST_TYPE_DEF) {
        ct_register_type_def(ctx, ast);
        result.value = fxsh_ct_unit();
        return result;
    }

    result.value = eval_expr(ast, ctx);
    if (!result.value) {
        if (!g_ct_error_msg[0])
            ct_set_error("comptime eval failed at AST kind %d", (int)ast->kind);
        result.error = ERR_INTERNAL;
    }

    return result;
}

fxsh_ct_value_t *fxsh_ct_eval_expr(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_result_t result = fxsh_ct_eval(ast, ctx);
    return result.value;
}

static bool is_ct_expr_kind(fxsh_ast_kind_t k) {
    return k == AST_CT_TYPE_OF || k == AST_CT_TYPE_NAME || k == AST_CT_SIZE_OF ||
           k == AST_CT_ALIGN_OF || k == AST_CT_FIELDS_OF || k == AST_CT_HAS_FIELD ||
           k == AST_CT_IS_RECORD || k == AST_CT_IS_TUPLE || k == AST_CT_JSON_SCHEMA ||
           k == AST_CT_SQLITE_SQL || k == AST_CT_SQL ||
           k == AST_CT_CTOR_APPLY || k == AST_CT_QUOTE || k == AST_CT_UNQUOTE ||
           k == AST_CT_SPLICE || k == AST_CT_EVAL || k == AST_CT_COMPILE_ERROR ||
           k == AST_CT_COMPILE_LOG || k == AST_CT_PANIC;
}

fxsh_error_t fxsh_ct_expand_program(fxsh_ast_node_t *ast, fxsh_type_env_t type_env) {
    if (!ast || ast->kind != AST_PROGRAM)
        return ERR_OK;

    fxsh_comptime_ctx_t ctx;
    fxsh_comptime_ctx_init(&ctx);
    ctx.type_env = type_env;

    fxsh_ast_list_t out = SP_NULLPTR;
    sp_dyn_array_for(ast->data.decls, i) {
        fxsh_ast_node_t *d = ast->data.decls[i];
        if (!d)
            continue;

        if (d->kind == AST_TYPE_DEF) {
            ct_register_type_def(&ctx, d);
            sp_dyn_array_push(out, d);
            continue;
        }

        if ((d->kind == AST_DECL_LET || d->kind == AST_LET) && d->data.let.is_comptime) {
            fxsh_ct_result_t r = fxsh_ct_eval(d, &ctx);
            if (r.error != ERR_OK)
                return r.error;
            if (r.value) {
                fxsh_ast_node_t *lowered = NULL;
                if (!ct_value_to_ast_expr(r.value, &lowered)) {
                    if (!fxsh_ct_last_error())
                        ct_set_error("cannot lower comptime value in let `%.*s`",
                                     d->data.let.name.len, d->data.let.name.data);
                    return ERR_INTERNAL;
                }
                d->data.let.value = lowered;
            }
            sp_dyn_array_push(out, d);
            continue;
        }

        if ((d->kind == AST_DECL_LET || d->kind == AST_LET) && d->data.let.value &&
            is_ct_expr_kind(d->data.let.value->kind)) {
            fxsh_ct_value_t *cv = fxsh_ct_eval_expr(d->data.let.value, &ctx);
            fxsh_ast_node_t *lowered = NULL;
            if (!cv || !ct_value_to_ast_expr(cv, &lowered)) {
                if (!fxsh_ct_last_error())
                    ct_set_error("cannot lower comptime value in let `%.*s`", d->data.let.name.len,
                                 d->data.let.name.data);
                return ERR_INTERNAL;
            }
            d->data.let.value = lowered;

            /* Expose lowered value for later comptime declarations in same file. */
            fxsh_ct_value_t *for_env = fxsh_ct_eval_expr(lowered, &ctx);
            if (for_env)
                bind_var(&ctx, d->data.let.name, for_env);

            sp_dyn_array_push(out, d);
            continue;
        }

        if (is_ct_expr_kind(d->kind)) {
            fxsh_ct_value_t *cv = fxsh_ct_eval_expr(d, &ctx);
            fxsh_ast_node_t *lowered = NULL;
            if (!cv || !ct_value_to_ast_expr(cv, &lowered)) {
                if (!fxsh_ct_last_error())
                    ct_set_error("cannot lower top-level comptime expression");
                return ERR_INTERNAL;
            }
            if (lowered && lowered->kind == AST_PROGRAM) {
                sp_dyn_array_for(lowered->data.decls, j) {
                    sp_dyn_array_push(out, lowered->data.decls[j]);
                }
            } else if (lowered) {
                sp_dyn_array_push(out, lowered);
            }
            continue;
        }

        sp_dyn_array_push(out, d);
    }

    ast->data.decls = out;
    return ERR_OK;
}

/*=============================================================================
 * Value Comparison
 *=============================================================================*/

bool fxsh_ct_equal(fxsh_ct_value_t *a, fxsh_ct_value_t *b) {
    if (!a || !b)
        return a == b;
    if (a->kind != b->kind)
        return false;

    switch (a->kind) {
        case CT_UNIT:
            return true;
        case CT_BOOL:
            return a->data.bool_val == b->data.bool_val;
        case CT_INT:
            return a->data.int_val == b->data.int_val;
        case CT_FLOAT:
            return a->data.float_val == b->data.float_val;
        case CT_STRING:
            return sp_str_equal(a->data.string_val, b->data.string_val);
        case CT_TYPE:
            /* Type equality - simplified */
            return a->data.type_val == b->data.type_val;
        case CT_LIST:
            if (a->data.list_val.len != b->data.list_val.len)
                return false;
            for (u32 i = 0; i < a->data.list_val.len; i++) {
                if (!fxsh_ct_equal(a->data.list_val.items[i], b->data.list_val.items[i]))
                    return false;
            }
            return true;
        case CT_STRUCT:
            if (a->data.struct_val.num_fields != b->data.struct_val.num_fields)
                return false;
            for (u32 i = 0; i < a->data.struct_val.num_fields; i++) {
                fxsh_ct_field_t af = a->data.struct_val.fields[i];
                fxsh_ct_value_t *bv = fxsh_ct_record_get_field(b, af.name);
                if (!bv || !fxsh_ct_equal(af.value, bv))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

/*=============================================================================
 * Type Reflection
 *=============================================================================*/

fxsh_type_info_t *fxsh_ct_type_info(fxsh_type_t *type) {
    if (!type)
        return NULL;

    fxsh_type_info_t *info = fxsh_alloc0(sizeof(fxsh_type_info_t));
    info->kind = type->kind;

    switch (type->kind) {
        case TYPE_CON:
            info->name = type->data.con;
            break;
        case TYPE_ARROW:
            info->data.arrow.param = type->data.arrow.param;
            info->data.arrow.ret = type->data.arrow.ret;
            break;
        default:
            info->name = (sp_str_t){.data = "unknown", .len = 7};
            break;
    }

    return info;
}

fxsh_ct_value_t *fxsh_ct_type_name(fxsh_ct_value_t *type_val) {
    if (!type_val || type_val->kind != CT_TYPE)
        return NULL;

    fxsh_type_t *type = type_val->data.type_val;
    const c8 *name = fxsh_type_to_string(type);

    return fxsh_ct_string((sp_str_t){.data = name, .len = strlen(name)});
}

static fxsh_ct_value_t *fxsh_ct_is_record(fxsh_ct_value_t *type_val) {
    return fxsh_ct_bool(type_val && type_val->kind == CT_TYPE && type_val->data.type_val &&
                        type_val->data.type_val->kind == TYPE_RECORD);
}

static fxsh_ct_value_t *fxsh_ct_is_tuple(fxsh_ct_value_t *type_val) {
    return fxsh_ct_bool(type_val && type_val->kind == CT_TYPE && type_val->data.type_val &&
                        type_val->data.type_val->kind == TYPE_TUPLE);
}

fxsh_ct_value_t *fxsh_ct_size_of(fxsh_ct_value_t *type_val) {
    if (!type_val || type_val->kind != CT_TYPE)
        return NULL;

    /* Simplified - return mock sizes */
    fxsh_type_t *type = type_val->data.type_val;
    s64 size = 8; /* Default pointer size */

    if (type->kind == TYPE_CON) {
        if (sp_str_equal(type->data.con, TYPE_INT)) {
            size = 8;
        } else if (sp_str_equal(type->data.con, TYPE_BOOL)) {
            size = 1;
        } else if (sp_str_equal(type->data.con, TYPE_FLOAT)) {
            size = 8;
        } else if (sp_str_equal(type->data.con, TYPE_STRING)) {
            size = 16; /* ptr + len */
        }
    }

    return fxsh_ct_int(size);
}

/*=============================================================================
 * Code Generation
 *=============================================================================*/

fxsh_ast_node_t *fxsh_ct_quote(fxsh_ast_node_t *ast) {
    /* Quote returns the AST as a compile-time value */
    fxsh_ast_node_t *node = fxsh_alloc0(sizeof(fxsh_ast_node_t));
    node->kind = AST_LIT_STRING; /* Placeholder - should wrap AST */
    node->loc = ast->loc;
    node->data.lit_string = (sp_str_t){.data = "<quoted>", .len = 8};
    return node;
}

fxsh_ast_node_t *fxsh_ct_unquote(fxsh_ct_value_t *val) {
    /* Unquote converts a compile-time AST value back to AST */
    if (!val || val->kind != CT_AST)
        return NULL;
    return val->data.ast_val;
}

fxsh_ast_node_t *fxsh_ct_splice(fxsh_ct_value_t *val) {
    if (!val || val->kind != CT_AST)
        return NULL;
    return val->data.ast_val;
}

/*=============================================================================
 * Derived Macros
 *=============================================================================*/

fxsh_ast_node_t *fxsh_ct_derive_show(fxsh_type_t *type) {
    /* Generate show function for a type */
    if (!type)
        return NULL;

    /* Create: fn show(x: T) -> string = ... */
    fxsh_ast_node_t *func = fxsh_alloc0(sizeof(fxsh_ast_node_t));
    func->kind = AST_LAMBDA;
    func->loc = (fxsh_loc_t){0};

    /* Parameter: x */
    fxsh_ast_list_t params = SP_NULLPTR;
    sp_dyn_array_push(params, fxsh_ast_ident((sp_str_t){.data = "x", .len = 1}, (fxsh_loc_t){0}));
    func->data.lambda.params = params;

    /* Body: generate based on type */
    fxsh_ast_node_t *body = fxsh_alloc0(sizeof(fxsh_ast_node_t));
    body->kind = AST_LIT_STRING;
    body->loc = (fxsh_loc_t){0};
    body->data.lit_string = (sp_str_t){.data = "<derived show>", .len = 14};
    func->data.lambda.body = body;

    return func;
}

fxsh_ast_node_t *fxsh_ct_derive_eq(fxsh_type_t *type) {
    /* Generate eq function for a type */
    if (!type)
        return NULL;

    fxsh_ast_node_t *func = fxsh_alloc0(sizeof(fxsh_ast_node_t));
    func->kind = AST_LAMBDA;
    func->loc = (fxsh_loc_t){0};

    fxsh_ast_list_t params = SP_NULLPTR;
    sp_dyn_array_push(params, fxsh_ast_ident((sp_str_t){.data = "a", .len = 1}, (fxsh_loc_t){0}));
    sp_dyn_array_push(params, fxsh_ast_ident((sp_str_t){.data = "b", .len = 1}, (fxsh_loc_t){0}));
    func->data.lambda.params = params;

    fxsh_ast_node_t *body = fxsh_alloc0(sizeof(fxsh_ast_node_t));
    body->kind = AST_LIT_BOOL;
    body->loc = (fxsh_loc_t){0};
    body->data.lit_bool = true;
    func->data.lambda.body = body;

    return func;
}

/*=============================================================================
 * Value to String
 *=============================================================================*/

const c8 *fxsh_ct_value_to_string(fxsh_ct_value_t *val) {
    if (!val)
        return "<null>";

    static c8 buf[256];

    switch (val->kind) {
        case CT_UNIT:
            return "()";
        case CT_BOOL:
            return val->data.bool_val ? "true" : "false";
        case CT_INT:
            snprintf(buf, sizeof(buf), "%ld", val->data.int_val);
            return buf;
        case CT_FLOAT:
            snprintf(buf, sizeof(buf), "%f", val->data.float_val);
            return buf;
        case CT_STRING:
            snprintf(buf, sizeof(buf), "\"%.*s\"", val->data.string_val.len,
                     val->data.string_val.data);
            return buf;
        case CT_TYPE:
            return fxsh_type_to_string(val->data.type_val);
        case CT_FUNCTION:
            return "<function>";
        case CT_AST:
            return "<ast>";
        case CT_LIST: {
            static c8 list_buf[512];
            size_t off = 0;
            off += (size_t)snprintf(list_buf + off, sizeof(list_buf) - off, "[");
            for (u32 i = 0; i < val->data.list_val.len; i++) {
                const c8 *s = fxsh_ct_value_to_string(val->data.list_val.items[i]);
                off += (size_t)snprintf(list_buf + off, sizeof(list_buf) - off, "%s%s",
                                        i == 0 ? "" : ", ", s ? s : "<null>");
                if (off >= sizeof(list_buf) - 4)
                    break;
            }
            snprintf(list_buf + off, sizeof(list_buf) - off, "]");
            return list_buf;
        }
        case CT_STRUCT:
            return "<struct type>";
        default:
            return "<unknown>";
    }
}

/*=============================================================================
 * Compile-time Type Programming
 *=============================================================================*/

fxsh_ct_value_t *fxsh_ct_make_record_type(sp_str_t name) {
    fxsh_ct_value_t *val = fxsh_alloc0(sizeof(fxsh_ct_value_t));
    val->kind = CT_STRUCT;
    val->data.struct_val.fields = SP_NULLPTR;
    val->data.struct_val.num_fields = 0;
    return val;
}

fxsh_ct_value_t *fxsh_ct_record_add_field(fxsh_ct_value_t *record, sp_str_t field_name,
                                          fxsh_ct_value_t *field_type) {
    if (!record || record->kind != CT_STRUCT)
        return NULL;

    fxsh_ct_field_t field = {.name = field_name, .value = field_type};

    /* Extend fields array */
    u32 new_count = record->data.struct_val.num_fields + 1;
    fxsh_ct_field_t *new_fields = fxsh_alloc0(sizeof(fxsh_ct_field_t) * new_count);

    /* Copy old fields */
    for (u32 i = 0; i < record->data.struct_val.num_fields; i++) {
        new_fields[i] = record->data.struct_val.fields[i];
    }

    /* Add new field */
    new_fields[record->data.struct_val.num_fields] = field;

    /* Arena-backed allocations are bump-only; keep old buffer alive. */
    record->data.struct_val.fields = new_fields;
    record->data.struct_val.num_fields = new_count;

    return record;
}

fxsh_ct_value_t *fxsh_ct_record_get_field(fxsh_ct_value_t *record, sp_str_t field_name) {
    if (!record || record->kind != CT_STRUCT)
        return NULL;

    for (u32 i = 0; i < record->data.struct_val.num_fields; i++) {
        if (sp_str_equal(record->data.struct_val.fields[i].name, field_name)) {
            return record->data.struct_val.fields[i].value;
        }
    }
    return NULL;
}

/*=============================================================================
 * Compile-time Type Operators (@typeOf, @sizeOf, etc.)
 *=============================================================================*/

fxsh_ct_value_t *fxsh_ct_op_type_of(fxsh_ast_node_t *expr, fxsh_comptime_ctx_t *ctx) {
    /* Get the type of an expression at compile time */
    if (!expr)
        return NULL;

    fxsh_type_t *type = NULL;

    /* Preferred path: ask HM inference with current type env snapshot. */
    fxsh_type_env_t env = ctx ? ctx->type_env : SP_NULLPTR;
    fxsh_constr_env_t cenv = SP_NULLPTR;
    if (fxsh_type_infer(expr, &env, &cenv, &type) == ERR_OK && type)
        return fxsh_ct_type(type);

    /* Fallback path for non-typable fragments in comptime-only contexts. */
    switch (expr->kind) {
        case AST_LIT_INT:
            type = fxsh_type_con(TYPE_INT);
            break;
        case AST_LIT_FLOAT:
            type = fxsh_type_con(TYPE_FLOAT);
            break;
        case AST_LIT_STRING:
            type = fxsh_type_con(TYPE_STRING);
            break;
        case AST_LIT_BOOL:
            type = fxsh_type_con(TYPE_BOOL);
            break;
        case AST_IDENT: {
            fxsh_ct_value_t *val = lookup_var(ctx, expr->data.ident);
            if (val && val->kind == CT_TYPE)
                type = val->data.type_val;
            break;
        }
        default:
            break;
    }

    if (type) {
        return fxsh_ct_type(type);
    }
    return NULL;
}

fxsh_ct_value_t *fxsh_ct_op_size_of(fxsh_ct_value_t *type_val) {
    return fxsh_ct_size_of(type_val);
}

fxsh_ct_value_t *fxsh_ct_op_align_of(fxsh_ct_value_t *type_val) {
    if (!type_val || type_val->kind != CT_TYPE)
        return NULL;

    /* Simplified - alignment is usually same as size for primitives */
    fxsh_type_t *type = type_val->data.type_val;
    s64 align = 8;

    if (type->kind == TYPE_CON) {
        if (sp_str_equal(type->data.con, TYPE_INT)) {
            align = 8;
        } else if (sp_str_equal(type->data.con, TYPE_BOOL)) {
            align = 1;
        } else if (sp_str_equal(type->data.con, TYPE_FLOAT)) {
            align = 8;
        }
    }

    return fxsh_ct_int(align);
}

fxsh_ct_value_t *fxsh_ct_op_fields_of(fxsh_ct_value_t *type_val) {
    if (!type_val || type_val->kind != CT_TYPE)
        return NULL;

    fxsh_type_t *type = type_val->data.type_val;
    if (!type || type->kind != TYPE_RECORD)
        return fxsh_ct_list(NULL, 0);

    u32 n = (u32)sp_dyn_array_size(type->data.record.fields);
    fxsh_ct_value_t **items = fxsh_alloc0(sizeof(fxsh_ct_value_t *) * n);
    for (u32 i = 0; i < n; i++) {
        items[i] = fxsh_ct_string(type->data.record.fields[i].name);
    }
    return fxsh_ct_list(items, n);
}

fxsh_ct_value_t *fxsh_ct_op_has_field(fxsh_ct_value_t *type_val, sp_str_t field_name) {
    if (!type_val || type_val->kind != CT_TYPE)
        return fxsh_ct_bool(false);

    fxsh_type_t *type = type_val->data.type_val;
    if (type->kind == TYPE_RECORD) {
        for (u32 i = 0; i < sp_dyn_array_size(type->data.record.fields); i++) {
            if (sp_str_equal(type->data.record.fields[i].name, field_name)) {
                return fxsh_ct_bool(true);
            }
        }
    }
    return fxsh_ct_bool(false);
}

typedef struct {
    sp_dyn_array(c8) chars;
} ct_sb_t;

static void ct_sb_push_c(ct_sb_t *sb, c8 c) {
    sp_dyn_array_push(sb->chars, c);
}

static void ct_sb_push_s(ct_sb_t *sb, const c8 *s) {
    if (!s)
        return;
    for (u32 i = 0; s[i]; i++)
        sp_dyn_array_push(sb->chars, s[i]);
}

static void ct_sb_push_str(ct_sb_t *sb, sp_str_t s) {
    for (u32 i = 0; i < s.len; i++)
        sp_dyn_array_push(sb->chars, s.data[i]);
}

static void emit_json_schema_type(ct_sb_t *sb, fxsh_type_t *type);

static void emit_json_schema_primitive(ct_sb_t *sb, fxsh_type_t *type) {
    if (!type || type->kind != TYPE_CON) {
        ct_sb_push_s(sb, "{\"type\":\"string\"}");
        return;
    }
    if (sp_str_equal(type->data.con, TYPE_INT)) {
        ct_sb_push_s(sb, "{\"type\":\"integer\"}");
        return;
    }
    if (sp_str_equal(type->data.con, TYPE_FLOAT)) {
        ct_sb_push_s(sb, "{\"type\":\"number\"}");
        return;
    }
    if (sp_str_equal(type->data.con, TYPE_BOOL)) {
        ct_sb_push_s(sb, "{\"type\":\"boolean\"}");
        return;
    }
    if (sp_str_equal(type->data.con, TYPE_STRING)) {
        ct_sb_push_s(sb, "{\"type\":\"string\"}");
        return;
    }
    ct_sb_push_s(sb, "{\"type\":\"string\"}");
}

static void emit_json_schema_type(ct_sb_t *sb, fxsh_type_t *type) {
    if (!type) {
        ct_sb_push_s(sb, "{\"type\":\"string\"}");
        return;
    }
    switch (type->kind) {
        case TYPE_VAR:
            ct_sb_push_s(sb, "{\"type\":\"string\"}");
            return;
        case TYPE_CON:
            emit_json_schema_primitive(sb, type);
            return;
        case TYPE_RECORD: {
            ct_sb_push_s(sb, "{\"type\":\"object\",\"properties\":{");
            sp_dyn_array_for(type->data.record.fields, i) {
                if (i > 0)
                    ct_sb_push_c(sb, ',');
                ct_sb_push_c(sb, '\"');
                ct_sb_push_str(sb, type->data.record.fields[i].name);
                ct_sb_push_s(sb, "\":");
                emit_json_schema_type(sb, type->data.record.fields[i].type);
            }
            ct_sb_push_s(sb, "},\"required\":[");
            sp_dyn_array_for(type->data.record.fields, i) {
                if (i > 0)
                    ct_sb_push_c(sb, ',');
                ct_sb_push_c(sb, '\"');
                ct_sb_push_str(sb, type->data.record.fields[i].name);
                ct_sb_push_c(sb, '\"');
            }
            ct_sb_push_s(sb, "]}");
            return;
        }
        case TYPE_TUPLE: {
            ct_sb_push_s(sb, "{\"type\":\"array\",\"prefixItems\":[");
            sp_dyn_array_for(type->data.tuple, i) {
                if (i > 0)
                    ct_sb_push_c(sb, ',');
                emit_json_schema_type(sb, type->data.tuple[i]);
            }
            ct_sb_push_s(sb, "],\"minItems\":");
            char num[32];
            snprintf(num, sizeof(num), "%u", (u32)sp_dyn_array_size(type->data.tuple));
            ct_sb_push_s(sb, num);
            ct_sb_push_s(sb, ",\"maxItems\":");
            ct_sb_push_s(sb, num);
            ct_sb_push_c(sb, '}');
            return;
        }
        case TYPE_APP: {
            fxsh_type_t *head = type->data.app.con;
            if (head && head->kind == TYPE_CON && sp_str_equal(head->data.con, TYPE_LIST)) {
                ct_sb_push_s(sb, "{\"type\":\"array\",\"items\":");
                emit_json_schema_type(sb, type->data.app.arg);
                ct_sb_push_c(sb, '}');
                return;
            }
            ct_sb_push_s(sb, "{\"type\":\"string\"}");
            return;
        }
        case TYPE_ARROW:
            ct_sb_push_s(sb, "{\"type\":\"string\"}");
            return;
    }
}

fxsh_ct_value_t *fxsh_ct_op_json_schema(fxsh_ct_value_t *type_val) {
    if (!type_val || type_val->kind != CT_TYPE)
        return fxsh_ct_string((sp_str_t){.data = "{\"type\":\"string\"}", .len = 17});
    ct_sb_t sb = {.chars = SP_NULLPTR};
    emit_json_schema_type(&sb, type_val->data.type_val);
    sp_dyn_array_push(sb.chars, '\0');
    char *out = fxsh_alloc((size_t)sp_dyn_array_size(sb.chars));
    memcpy(out, sb.chars, (size_t)sp_dyn_array_size(sb.chars));
    sp_str_t s = sp_str_view(out);
    sp_dyn_array_free(sb.chars);
    return fxsh_ct_string(s);
}

static bool ct_type_app_head_is(fxsh_type_t *type, sp_str_t con_name) {
    if (!type || type->kind != TYPE_APP)
        return false;
    fxsh_type_t *head = type->data.app.con;
    while (head && head->kind == TYPE_APP)
        head = head->data.app.con;
    return head && head->kind == TYPE_CON && sp_str_equal(head->data.con, con_name);
}

static fxsh_type_t *ct_unwrap_option_type(fxsh_type_t *type, bool *is_nullable) {
    if (is_nullable)
        *is_nullable = false;
    if (!type)
        return type;
    if (ct_type_app_head_is(type, TYPE_OPTION)) {
        if (is_nullable)
            *is_nullable = true;
        return type->data.app.arg;
    }
    return type;
}

static const c8 *ct_sqlite_type_name(fxsh_type_t *type) {
    if (!type)
        return "TEXT";
    if (type->kind == TYPE_CON) {
        if (sp_str_equal(type->data.con, TYPE_INT) || sp_str_equal(type->data.con, TYPE_BOOL))
            return "INTEGER";
        if (sp_str_equal(type->data.con, TYPE_FLOAT))
            return "REAL";
        if (sp_str_equal(type->data.con, TYPE_STRING))
            return "TEXT";
    }
    return "TEXT";
}

static fxsh_ct_value_t *ct_sb_to_string_value(ct_sb_t *sb) {
    sp_dyn_array_push(sb->chars, '\0');
    char *out = fxsh_alloc((size_t)sp_dyn_array_size(sb->chars));
    memcpy(out, sb->chars, (size_t)sp_dyn_array_size(sb->chars));
    sp_str_t s = sp_str_view(out);
    sp_dyn_array_free(sb->chars);
    return fxsh_ct_string(s);
}

static fxsh_ct_value_t *fxsh_ct_op_sqlite_sql(fxsh_ct_value_t *type_val, sp_str_t table_name) {
    if (!type_val || type_val->kind != CT_TYPE)
        return NULL;
    fxsh_type_t *type = type_val->data.type_val;
    if (!type || type->kind != TYPE_RECORD) {
        ct_set_error("@sqliteSQL expects a record type");
        return NULL;
    }

    ct_sb_t create_sb = {.chars = SP_NULLPTR};
    ct_sb_t insert_sb = {.chars = SP_NULLPTR};
    ct_sb_t select_sb = {.chars = SP_NULLPTR};

    ct_sb_push_s(&create_sb, "CREATE TABLE IF NOT EXISTS \"");
    ct_sb_push_str(&create_sb, table_name);
    ct_sb_push_s(&create_sb, "\" (");

    ct_sb_push_s(&insert_sb, "INSERT INTO \"");
    ct_sb_push_str(&insert_sb, table_name);
    ct_sb_push_s(&insert_sb, "\" (");

    ct_sb_push_s(&select_sb, "SELECT ");

    const sp_str_t id_name = {.data = "id", .len = 2};
    sp_dyn_array_for(type->data.record.fields, i) {
        fxsh_field_t field = type->data.record.fields[i];
        if (i > 0) {
            ct_sb_push_s(&create_sb, ", ");
            ct_sb_push_s(&insert_sb, ", ");
            ct_sb_push_s(&select_sb, ", ");
        }

        bool nullable = false;
        fxsh_type_t *base_type = ct_unwrap_option_type(field.type, &nullable);
        const c8 *sql_t = ct_sqlite_type_name(base_type);

        ct_sb_push_c(&create_sb, '\"');
        ct_sb_push_str(&create_sb, field.name);
        ct_sb_push_s(&create_sb, "\" ");
        ct_sb_push_s(&create_sb, sql_t);
        if (!nullable)
            ct_sb_push_s(&create_sb, " NOT NULL");
        if (!nullable && sp_str_equal(field.name, id_name) && strcmp(sql_t, "INTEGER") == 0)
            ct_sb_push_s(&create_sb, " PRIMARY KEY");

        ct_sb_push_c(&insert_sb, '\"');
        ct_sb_push_str(&insert_sb, field.name);
        ct_sb_push_c(&insert_sb, '\"');

        ct_sb_push_c(&select_sb, '\"');
        ct_sb_push_str(&select_sb, field.name);
        ct_sb_push_c(&select_sb, '\"');
    }

    ct_sb_push_s(&create_sb, ");");

    ct_sb_push_s(&insert_sb, ") VALUES (");
    sp_dyn_array_for(type->data.record.fields, i) {
        if (i > 0)
            ct_sb_push_s(&insert_sb, ", ");
        ct_sb_push_c(&insert_sb, '?');
    }
    ct_sb_push_s(&insert_sb, ");");

    ct_sb_push_s(&select_sb, " FROM \"");
    ct_sb_push_str(&select_sb, table_name);
    ct_sb_push_s(&select_sb, "\";");

    fxsh_ct_value_t *record = fxsh_ct_make_record_type((sp_str_t){.data = "SqliteSql", .len = 9});
    fxsh_ct_record_add_field(record, (sp_str_t){.data = "create", .len = 6},
                             ct_sb_to_string_value(&create_sb));
    fxsh_ct_record_add_field(record, (sp_str_t){.data = "insert", .len = 6},
                             ct_sb_to_string_value(&insert_sb));
    fxsh_ct_record_add_field(record, (sp_str_t){.data = "select", .len = 6},
                             ct_sb_to_string_value(&select_sb));
    return record;
}

static bool ct_list_all_strings(fxsh_ct_value_t *v) {
    if (!v || v->kind != CT_LIST)
        return false;
    for (u32 i = 0; i < v->data.list_val.len; i++) {
        if (!v->data.list_val.items[i] || v->data.list_val.items[i]->kind != CT_STRING)
            return false;
    }
    return true;
}

static bool ct_list_all_list_of_strings(fxsh_ct_value_t *v) {
    if (!v || v->kind != CT_LIST)
        return false;
    for (u32 i = 0; i < v->data.list_val.len; i++) {
        fxsh_ct_value_t *row = v->data.list_val.items[i];
        if (!row || !ct_list_all_strings(row))
            return false;
    }
    return true;
}

static bool ct_sql_is_ident_char(c8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

static bool ct_sql_is_simple_ident(sp_str_t s) {
    if (!s.data || s.len == 0)
        return false;
    for (u32 i = 0; i < s.len; i++) {
        if (!ct_sql_is_ident_char(s.data[i]))
            return false;
    }
    return true;
}

static bool ct_sql_is_simple_dotted_ident(sp_str_t s) {
    if (!s.data || s.len == 0)
        return false;
    u32 seg_len = 0;
    for (u32 i = 0; i < s.len; i++) {
        c8 c = s.data[i];
        if (c == '.') {
            if (seg_len == 0)
                return false;
            seg_len = 0;
            continue;
        }
        if (!ct_sql_is_ident_char(c))
            return false;
        seg_len++;
    }
    return seg_len > 0;
}

static void ct_sql_push_ident_quoted(ct_sb_t *sb, sp_str_t ident) {
    ct_sb_push_c(sb, '"');
    ct_sb_push_str(sb, ident);
    ct_sb_push_c(sb, '"');
}

static void ct_sql_push_ident_or_expr(ct_sb_t *sb, sp_str_t text) {
    if (ct_sql_is_simple_ident(text)) {
        ct_sql_push_ident_quoted(sb, text);
        return;
    }
    if (ct_sql_is_simple_dotted_ident(text)) {
        u32 start = 0;
        for (u32 i = 0; i <= text.len; i++) {
            if (i == text.len || text.data[i] == '.') {
                if (i > start) {
                    if (start > 0)
                        ct_sb_push_c(sb, '.');
                    ct_sb_push_c(sb, '"');
                    for (u32 j = start; j < i; j++)
                        ct_sb_push_c(sb, text.data[j]);
                    ct_sb_push_c(sb, '"');
                }
                start = i + 1;
            }
        }
        return;
    }
    ct_sb_push_str(sb, text);
}

static bool ct_sql_push_string_list_join(ct_sb_t *sb, fxsh_ct_value_t *list_val, const c8 *sep,
                                         bool ident_or_expr) {
    if (!ct_list_all_strings(list_val))
        return false;
    for (u32 i = 0; i < list_val->data.list_val.len; i++) {
        if (i > 0)
            ct_sb_push_s(sb, sep);
        sp_str_t it = list_val->data.list_val.items[i]->data.string_val;
        if (ident_or_expr)
            ct_sql_push_ident_or_expr(sb, it);
        else
            ct_sb_push_str(sb, it);
    }
    return true;
}

static bool ct_sql_get_bool(fxsh_ct_value_t *dsl, const c8 *name, bool def) {
    fxsh_ct_value_t *v = fxsh_ct_record_get_field(dsl, sp_str_view((char *)name));
    if (!v || v->kind != CT_BOOL)
        return def;
    return v->data.bool_val;
}

static s64 ct_sql_get_int(fxsh_ct_value_t *dsl, const c8 *name, s64 def) {
    fxsh_ct_value_t *v = fxsh_ct_record_get_field(dsl, sp_str_view((char *)name));
    if (!v || v->kind != CT_INT)
        return def;
    return v->data.int_val;
}

static fxsh_ct_value_t *ct_sql_get(fxsh_ct_value_t *dsl, const c8 *name) {
    return fxsh_ct_record_get_field(dsl, sp_str_view((char *)name));
}

static bool ct_sql_append_conditions(ct_sb_t *sb, fxsh_ct_value_t *dsl, const c8 *field_name,
                                     const c8 *prefix, const c8 *mode_field, const c8 *def_mode,
                                     const c8 *errmsg) {
    fxsh_ct_value_t *conds = ct_sql_get(dsl, field_name);
    if (!conds || conds->kind != CT_LIST || conds->data.list_val.len == 0)
        return true;
    if (!ct_list_all_strings(conds)) {
        ct_set_error("%s", errmsg);
        return false;
    }
    const c8 *sep = def_mode;
    fxsh_ct_value_t *mode = ct_sql_get(dsl, mode_field);
    if (mode && mode->kind == CT_STRING && mode->data.string_val.len > 0)
        sep = mode->data.string_val.data;

    ct_sb_push_c(sb, ' ');
    ct_sb_push_s(sb, prefix);
    ct_sb_push_c(sb, ' ');
    for (u32 i = 0; i < conds->data.list_val.len; i++) {
        if (i > 0) {
            ct_sb_push_c(sb, ' ');
            ct_sb_push_s(sb, sep);
            ct_sb_push_c(sb, ' ');
        }
        ct_sb_push_str(sb, conds->data.list_val.items[i]->data.string_val);
    }
    return true;
}

static bool ct_sql_append_returning(ct_sb_t *sb, fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *ret = ct_sql_get(dsl, "returning");
    if (!ret)
        return true;
    if (ret->kind == CT_STRING) {
        ct_sb_push_s(sb, " RETURNING ");
        ct_sb_push_str(sb, ret->data.string_val);
        return true;
    }
    if (ret->kind != CT_LIST || !ct_list_all_strings(ret)) {
        ct_set_error("@sql returning must be string or [string]");
        return false;
    }
    ct_sb_push_s(sb, " RETURNING ");
    return ct_sql_push_string_list_join(sb, ret, ", ", true);
}

static fxsh_ct_value_t *ct_sql_emit_select(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *cols = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "columns", .len = 7});
    fxsh_ct_value_t *from = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "from", .len = 4});
    if (!cols || !from || from->kind != CT_STRING || !ct_list_all_strings(cols)) {
        ct_set_error("@sql select requires columns:[string], from:string");
        return NULL;
    }

    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "SELECT ");
    if (ct_sql_get_bool(dsl, "distinct", false))
        ct_sb_push_s(&sb, "DISTINCT ");
    if (!ct_sql_push_string_list_join(&sb, cols, ", ", true))
        return NULL;
    ct_sb_push_s(&sb, " FROM ");
    ct_sql_push_ident_or_expr(&sb, from->data.string_val);

    fxsh_ct_value_t *joins = ct_sql_get(dsl, "joins");
    if (joins) {
        if (!ct_list_all_strings(joins)) {
            ct_set_error("@sql select joins must be [string]");
            return NULL;
        }
        for (u32 i = 0; i < joins->data.list_val.len; i++) {
            ct_sb_push_c(&sb, ' ');
            ct_sb_push_str(&sb, joins->data.list_val.items[i]->data.string_val);
        }
    }

    if (!ct_sql_append_conditions(&sb, dsl, "where", "WHERE", "where_mode", "AND",
                                  "@sql select where must be [string]"))
        return NULL;

    fxsh_ct_value_t *group_by = ct_sql_get(dsl, "group_by");
    if (group_by && group_by->kind == CT_LIST && group_by->data.list_val.len > 0) {
        ct_sb_push_s(&sb, " GROUP BY ");
        if (!ct_sql_push_string_list_join(&sb, group_by, ", ", false)) {
            ct_set_error("@sql select group_by must be [string]");
            return NULL;
        }
    }

    if (!ct_sql_append_conditions(&sb, dsl, "having", "HAVING", "having_mode", "AND",
                                  "@sql select having must be [string]"))
        return NULL;

    fxsh_ct_value_t *window = ct_sql_get(dsl, "window");
    if (window && window->kind == CT_LIST && window->data.list_val.len > 0) {
        ct_sb_push_s(&sb, " WINDOW ");
        if (!ct_sql_push_string_list_join(&sb, window, ", ", false)) {
            ct_set_error("@sql select window must be [string]");
            return NULL;
        }
    }

    fxsh_ct_value_t *order_by = ct_sql_get(dsl, "order_by");
    if (order_by && order_by->kind == CT_STRING && order_by->data.string_val.len > 0) {
        ct_sb_push_s(&sb, " ORDER BY ");
        ct_sb_push_str(&sb, order_by->data.string_val);
    }

    s64 limit = ct_sql_get_int(dsl, "limit", -1);
    if (limit >= 0) {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%lld", (long long)limit);
        ct_sb_push_s(&sb, " LIMIT ");
        ct_sb_push_s(&sb, nbuf);
    }
    s64 offset = ct_sql_get_int(dsl, "offset", -1);
    if (offset >= 0) {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%lld", (long long)offset);
        ct_sb_push_s(&sb, " OFFSET ");
        ct_sb_push_s(&sb, nbuf);
    }

    fxsh_ct_value_t *union_q = ct_sql_get(dsl, "union");
    if (union_q && union_q->kind == CT_LIST && union_q->data.list_val.len > 0) {
        if (!ct_list_all_strings(union_q)) {
            ct_set_error("@sql select union must be [string]");
            return NULL;
        }
        for (u32 i = 0; i < union_q->data.list_val.len; i++) {
            ct_sb_push_s(&sb, " UNION ");
            ct_sb_push_str(&sb, union_q->data.list_val.items[i]->data.string_val);
        }
    }
    fxsh_ct_value_t *union_all_q = ct_sql_get(dsl, "union_all");
    if (union_all_q && union_all_q->kind == CT_LIST && union_all_q->data.list_val.len > 0) {
        if (!ct_list_all_strings(union_all_q)) {
            ct_set_error("@sql select union_all must be [string]");
            return NULL;
        }
        for (u32 i = 0; i < union_all_q->data.list_val.len; i++) {
            ct_sb_push_s(&sb, " UNION ALL ");
            ct_sb_push_str(&sb, union_all_q->data.list_val.items[i]->data.string_val);
        }
    }

    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_insert(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *table = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "table", .len = 5});
    fxsh_ct_value_t *cols = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "columns", .len = 7});
    if (!table || table->kind != CT_STRING || !cols || !ct_list_all_strings(cols) ||
        cols->data.list_val.len == 0) {
        ct_set_error("@sql insert requires table:string, columns:[string]");
        return NULL;
    }

    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "INSERT");
    fxsh_ct_value_t *mode = ct_sql_get(dsl, "mode");
    if (mode && mode->kind == CT_STRING && mode->data.string_val.len > 0) {
        ct_sb_push_c(&sb, ' ');
        ct_sb_push_str(&sb, mode->data.string_val);
    }
    ct_sb_push_s(&sb, " INTO ");
    ct_sql_push_ident_or_expr(&sb, table->data.string_val);
    ct_sb_push_s(&sb, " (");
    if (!ct_sql_push_string_list_join(&sb, cols, ", ", true))
        return NULL;
    ct_sb_push_s(&sb, ")");

    fxsh_ct_value_t *rows = ct_sql_get(dsl, "rows");
    fxsh_ct_value_t *values = ct_sql_get(dsl, "values");
    fxsh_ct_value_t *select_q = ct_sql_get(dsl, "select");
    if ((rows && values) || (rows && select_q) || (values && select_q)) {
        ct_set_error("@sql insert rows/values/select are mutually exclusive");
        return NULL;
    }

    if (rows) {
        if (!ct_list_all_list_of_strings(rows) || rows->data.list_val.len == 0) {
            ct_set_error("@sql insert rows must be [[string]]");
            return NULL;
        }
        ct_sb_push_s(&sb, " VALUES ");
        for (u32 r = 0; r < rows->data.list_val.len; r++) {
            fxsh_ct_value_t *row = rows->data.list_val.items[r];
            if (row->data.list_val.len != cols->data.list_val.len) {
                ct_set_error("@sql insert row size must equal columns size");
                return NULL;
            }
            if (r > 0)
                ct_sb_push_s(&sb, ", ");
            ct_sb_push_c(&sb, '(');
            if (!ct_sql_push_string_list_join(&sb, row, ", ", false))
                return NULL;
            ct_sb_push_c(&sb, ')');
        }
    } else if (values) {
        if (!ct_list_all_strings(values) || values->data.list_val.len != cols->data.list_val.len) {
            ct_set_error("@sql insert values must be [string] and match columns size");
            return NULL;
        }
        ct_sb_push_s(&sb, " VALUES (");
        if (!ct_sql_push_string_list_join(&sb, values, ", ", false))
            return NULL;
        ct_sb_push_c(&sb, ')');
    } else if (select_q) {
        if (select_q->kind != CT_STRING) {
            ct_set_error("@sql insert select must be string");
            return NULL;
        }
        ct_sb_push_c(&sb, ' ');
        ct_sb_push_str(&sb, select_q->data.string_val);
    } else {
        ct_sb_push_s(&sb, " VALUES (");
        for (u32 i = 0; i < cols->data.list_val.len; i++) {
            if (i > 0)
                ct_sb_push_s(&sb, ", ");
            ct_sb_push_c(&sb, '?');
        }
        ct_sb_push_c(&sb, ')');
    }

    fxsh_ct_value_t *on_conflict = ct_sql_get(dsl, "on_conflict");
    if (on_conflict && on_conflict->kind == CT_STRING && on_conflict->data.string_val.len > 0) {
        ct_sb_push_s(&sb, " ON CONFLICT ");
        ct_sb_push_str(&sb, on_conflict->data.string_val);
    }
    if (!ct_sql_append_returning(&sb, dsl))
        return NULL;
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_update(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *table = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "table", .len = 5});
    fxsh_ct_value_t *set = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "set", .len = 3});
    if (!table || table->kind != CT_STRING || !set || !ct_list_all_strings(set) ||
        set->data.list_val.len == 0) {
        ct_set_error("@sql update requires table:string, set:[string]");
        return NULL;
    }

    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "UPDATE");
    fxsh_ct_value_t *mode = ct_sql_get(dsl, "mode");
    if (mode && mode->kind == CT_STRING && mode->data.string_val.len > 0) {
        ct_sb_push_c(&sb, ' ');
        ct_sb_push_str(&sb, mode->data.string_val);
    }
    ct_sb_push_c(&sb, ' ');
    ct_sql_push_ident_or_expr(&sb, table->data.string_val);
    ct_sb_push_s(&sb, " SET ");
    if (!ct_sql_push_string_list_join(&sb, set, ", ", false))
        return NULL;

    fxsh_ct_value_t *from = ct_sql_get(dsl, "from");
    if (from && from->kind == CT_STRING && from->data.string_val.len > 0) {
        ct_sb_push_s(&sb, " FROM ");
        ct_sb_push_str(&sb, from->data.string_val);
    }

    if (!ct_sql_append_conditions(&sb, dsl, "where", "WHERE", "where_mode", "AND",
                                  "@sql update where must be [string]"))
        return NULL;

    fxsh_ct_value_t *order_by = ct_sql_get(dsl, "order_by");
    if (order_by && order_by->kind == CT_STRING && order_by->data.string_val.len > 0) {
        ct_sb_push_s(&sb, " ORDER BY ");
        ct_sb_push_str(&sb, order_by->data.string_val);
    }
    s64 limit = ct_sql_get_int(dsl, "limit", -1);
    if (limit >= 0) {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%lld", (long long)limit);
        ct_sb_push_s(&sb, " LIMIT ");
        ct_sb_push_s(&sb, nbuf);
    }
    if (!ct_sql_append_returning(&sb, dsl))
        return NULL;
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_delete(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *table = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "table", .len = 5});
    if (!table || table->kind != CT_STRING) {
        ct_set_error("@sql delete requires table:string");
        return NULL;
    }

    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "DELETE FROM ");
    ct_sql_push_ident_or_expr(&sb, table->data.string_val);

    if (!ct_sql_append_conditions(&sb, dsl, "where", "WHERE", "where_mode", "AND",
                                  "@sql delete where must be [string]"))
        return NULL;
    fxsh_ct_value_t *order_by = ct_sql_get(dsl, "order_by");
    if (order_by && order_by->kind == CT_STRING && order_by->data.string_val.len > 0) {
        ct_sb_push_s(&sb, " ORDER BY ");
        ct_sb_push_str(&sb, order_by->data.string_val);
    }
    s64 limit = ct_sql_get_int(dsl, "limit", -1);
    if (limit >= 0) {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%lld", (long long)limit);
        ct_sb_push_s(&sb, " LIMIT ");
        ct_sb_push_s(&sb, nbuf);
    }
    if (!ct_sql_append_returning(&sb, dsl))
        return NULL;
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_create_table(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *table = ct_sql_get(dsl, "table");
    fxsh_ct_value_t *cols = ct_sql_get(dsl, "columns");
    fxsh_ct_value_t *as_select = ct_sql_get(dsl, "as_select");
    if (!table || table->kind != CT_STRING) {
        ct_set_error("@sql create_table requires table:string");
        return NULL;
    }
    if (!as_select && (!cols || !ct_list_all_strings(cols) || cols->data.list_val.len == 0)) {
        ct_set_error("@sql create_table requires columns:[string] when as_select is absent");
        return NULL;
    }

    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "CREATE ");
    if (ct_sql_get_bool(dsl, "temporary", false))
        ct_sb_push_s(&sb, "TEMP ");
    ct_sb_push_s(&sb, "TABLE ");
    if (ct_sql_get_bool(dsl, "if_not_exists", true))
        ct_sb_push_s(&sb, "IF NOT EXISTS ");
    ct_sql_push_ident_or_expr(&sb, table->data.string_val);

    if (as_select && as_select->kind == CT_STRING && as_select->data.string_val.len > 0) {
        ct_sb_push_s(&sb, " AS ");
        ct_sb_push_str(&sb, as_select->data.string_val);
        ct_sb_push_s(&sb, ";");
        return ct_sb_to_string_value(&sb);
    }

    ct_sb_push_s(&sb, " (");
    if (!ct_sql_push_string_list_join(&sb, cols, ", ", false))
        return NULL;
    fxsh_ct_value_t *constraints = ct_sql_get(dsl, "constraints");
    if (constraints && constraints->kind == CT_LIST && constraints->data.list_val.len > 0) {
        ct_sb_push_s(&sb, ", ");
        if (!ct_sql_push_string_list_join(&sb, constraints, ", ", false)) {
            ct_set_error("@sql create_table constraints must be [string]");
            return NULL;
        }
    }
    ct_sb_push_c(&sb, ')');
    if (ct_sql_get_bool(dsl, "without_rowid", false))
        ct_sb_push_s(&sb, " WITHOUT ROWID");
    if (ct_sql_get_bool(dsl, "strict", false))
        ct_sb_push_s(&sb, " STRICT");
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_drop_table(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *table = ct_sql_get(dsl, "table");
    if (!table || table->kind != CT_STRING) {
        ct_set_error("@sql drop_table requires table:string");
        return NULL;
    }
    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "DROP TABLE ");
    if (ct_sql_get_bool(dsl, "if_exists", true))
        ct_sb_push_s(&sb, "IF EXISTS ");
    ct_sql_push_ident_or_expr(&sb, table->data.string_val);
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_create_index(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *name = ct_sql_get(dsl, "name");
    fxsh_ct_value_t *table = ct_sql_get(dsl, "table");
    fxsh_ct_value_t *cols = ct_sql_get(dsl, "columns");
    if (!name || name->kind != CT_STRING || !table || table->kind != CT_STRING || !cols ||
        !ct_list_all_strings(cols) || cols->data.list_val.len == 0) {
        ct_set_error("@sql create_index requires name:string, table:string, columns:[string]");
        return NULL;
    }
    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "CREATE ");
    if (ct_sql_get_bool(dsl, "unique", false))
        ct_sb_push_s(&sb, "UNIQUE ");
    ct_sb_push_s(&sb, "INDEX ");
    if (ct_sql_get_bool(dsl, "if_not_exists", true))
        ct_sb_push_s(&sb, "IF NOT EXISTS ");
    ct_sql_push_ident_or_expr(&sb, name->data.string_val);
    ct_sb_push_s(&sb, " ON ");
    ct_sql_push_ident_or_expr(&sb, table->data.string_val);
    ct_sb_push_s(&sb, " (");
    if (!ct_sql_push_string_list_join(&sb, cols, ", ", false))
        return NULL;
    ct_sb_push_c(&sb, ')');
    if (!ct_sql_append_conditions(&sb, dsl, "where", "WHERE", "where_mode", "AND",
                                  "@sql create_index where must be [string]"))
        return NULL;
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_pragma(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *name = ct_sql_get(dsl, "name");
    if (!name || name->kind != CT_STRING || name->data.string_val.len == 0) {
        ct_set_error("@sql pragma requires name:string");
        return NULL;
    }
    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "PRAGMA ");
    ct_sb_push_str(&sb, name->data.string_val);
    fxsh_ct_value_t *value = ct_sql_get(dsl, "value");
    if (value) {
        ct_sb_push_s(&sb, " = ");
        switch (value->kind) {
            case CT_STRING:
                ct_sb_push_str(&sb, value->data.string_val);
                break;
            case CT_INT: {
                char nbuf[32];
                snprintf(nbuf, sizeof(nbuf), "%lld", (long long)value->data.int_val);
                ct_sb_push_s(&sb, nbuf);
                break;
            }
            case CT_BOOL:
                ct_sb_push_s(&sb, value->data.bool_val ? "1" : "0");
                break;
            default:
                ct_set_error("@sql pragma value must be string/int/bool");
                return NULL;
        }
    }
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_explain(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *query = ct_sql_get(dsl, "query");
    if (!query || query->kind != CT_STRING || query->data.string_val.len == 0) {
        ct_set_error("@sql explain requires query:string");
        return NULL;
    }
    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, ct_sql_get_bool(dsl, "query_plan", false) ? "EXPLAIN QUERY PLAN "
                                                                : "EXPLAIN ");
    ct_sb_push_str(&sb, query->data.string_val);
    if (query->data.string_val.data[query->data.string_val.len - 1] != ';')
        ct_sb_push_c(&sb, ';');
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_copy(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *table = ct_sql_get(dsl, "table");
    fxsh_ct_value_t *src = ct_sql_get(dsl, "from_select");
    if (!table || table->kind != CT_STRING || !src || src->kind != CT_STRING) {
        ct_set_error("@sql copy requires table:string, from_select:string");
        return NULL;
    }
    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_s(&sb, "INSERT INTO ");
    ct_sql_push_ident_or_expr(&sb, table->data.string_val);
    ct_sb_push_c(&sb, ' ');
    ct_sb_push_str(&sb, src->data.string_val);
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *ct_sql_emit_upsert(fxsh_ct_value_t *dsl) {
    fxsh_ct_value_t *base_insert = ct_sql_get(dsl, "insert");
    fxsh_ct_value_t *on_conflict = ct_sql_get(dsl, "on_conflict");
    if (!base_insert || base_insert->kind != CT_STRING || !on_conflict ||
        on_conflict->kind != CT_STRING) {
        ct_set_error("@sql upsert requires insert:string, on_conflict:string");
        return NULL;
    }
    ct_sb_t sb = {.chars = SP_NULLPTR};
    ct_sb_push_str(&sb, base_insert->data.string_val);
    if (base_insert->data.string_val.data[base_insert->data.string_val.len - 1] == ';')
        sp_dyn_array_pop(sb.chars);
    ct_sb_push_s(&sb, " ON CONFLICT ");
    ct_sb_push_str(&sb, on_conflict->data.string_val);
    ct_sb_push_s(&sb, ";");
    return ct_sb_to_string_value(&sb);
}

static fxsh_ct_value_t *fxsh_ct_op_sql(fxsh_ct_value_t *dsl) {
    if (!dsl) {
        ct_set_error("@sql expects dsl record/string");
        return NULL;
    }
    if (dsl->kind == CT_STRING)
        return dsl; /* raw passthrough */
    if (dsl->kind != CT_STRUCT) {
        ct_set_error("@sql expects dsl record/string");
        return NULL;
    }

    fxsh_ct_value_t *op = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "op", .len = 2});
    if (!op || op->kind != CT_STRING) {
        ct_set_error("@sql dsl requires op:string");
        return NULL;
    }

    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "raw", .len = 3})) {
        fxsh_ct_value_t *text = fxsh_ct_record_get_field(dsl, (sp_str_t){.data = "text", .len = 4});
        if (!text || text->kind != CT_STRING) {
            ct_set_error("@sql raw requires text:string");
            return NULL;
        }
        return text;
    }
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "select", .len = 6}))
        return ct_sql_emit_select(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "insert", .len = 6}))
        return ct_sql_emit_insert(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "update", .len = 6}))
        return ct_sql_emit_update(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "delete", .len = 6}))
        return ct_sql_emit_delete(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "create_table", .len = 12}))
        return ct_sql_emit_create_table(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "drop_table", .len = 10}))
        return ct_sql_emit_drop_table(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "create_index", .len = 12}))
        return ct_sql_emit_create_index(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "pragma", .len = 6}))
        return ct_sql_emit_pragma(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "explain", .len = 7}))
        return ct_sql_emit_explain(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "copy", .len = 4}))
        return ct_sql_emit_copy(dsl);
    if (sp_str_equal(op->data.string_val, (sp_str_t){.data = "upsert", .len = 6}))
        return ct_sql_emit_upsert(dsl);

    ct_set_error("@sql unknown op: %s", fxsh_ct_value_to_string(op));
    return NULL;
}

/*=============================================================================
 * Generic Type Instantiation
 *=============================================================================*/

static bool ct_type_var_list_contains(sp_dyn_array(fxsh_type_var_t) vars, fxsh_type_var_t var) {
    sp_dyn_array_for(vars, i) {
        if (vars[i] == var)
            return true;
    }
    return false;
}

static void ct_collect_type_vars(fxsh_type_t *type, sp_dyn_array(fxsh_type_var_t) * out_vars) {
    if (!type)
        return;

    switch (type->kind) {
        case TYPE_VAR:
            if (!ct_type_var_list_contains(*out_vars, type->data.var))
                sp_dyn_array_push(*out_vars, type->data.var);
            return;
        case TYPE_CON:
            return;
        case TYPE_ARROW:
            ct_collect_type_vars(type->data.arrow.param, out_vars);
            ct_collect_type_vars(type->data.arrow.ret, out_vars);
            return;
        case TYPE_TUPLE:
            sp_dyn_array_for(type->data.tuple, i) {
                ct_collect_type_vars(type->data.tuple[i], out_vars);
            }
            return;
        case TYPE_APP:
            ct_collect_type_vars(type->data.app.con, out_vars);
            ct_collect_type_vars(type->data.app.arg, out_vars);
            return;
        case TYPE_RECORD:
            sp_dyn_array_for(type->data.record.fields, i) {
                ct_collect_type_vars(type->data.record.fields[i].type, out_vars);
            }
            if (type->data.record.row_var >= 0 &&
                !ct_type_var_list_contains(*out_vars, type->data.record.row_var)) {
                sp_dyn_array_push(*out_vars, type->data.record.row_var);
            }
            return;
    }
}

static sp_dyn_array(fxsh_type_var_t) ct_collect_ctor_type_params(fxsh_type_constructor_t *ctor) {
    sp_dyn_array(fxsh_type_var_t) vars = SP_NULLPTR;

    if (!ctor)
        return vars;

    if (ctor->type_params) {
        sp_dyn_array_for(ctor->type_params, i) {
            sp_dyn_array_push(vars, ctor->type_params[i]);
        }
        return vars;
    }

    if (ctor->target_type)
        ct_collect_type_vars(ctor->target_type, &vars);

    sp_dyn_array_for(ctor->fields, i) {
        fxsh_ct_value_t *type_val = ctor->fields[i].type_val;
        if (type_val && type_val->kind == CT_TYPE)
            ct_collect_type_vars(type_val->data.type_val, &vars);
    }

    return vars;
}

static fxsh_type_t *ct_make_record_runtime_type(sp_dyn_array(fxsh_field_t) fields,
                                                fxsh_type_var_t row_var) {
    fxsh_type_t *record = fxsh_alloc0(sizeof(fxsh_type_t));
    record->kind = TYPE_RECORD;
    record->data.record.fields = fields;
    record->data.record.row_var = row_var;
    return record;
}

static fxsh_type_t *ct_instantiate_ctor_record(fxsh_type_constructor_t *ctor, fxsh_subst_t subst) {
    sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;

    sp_dyn_array_for(ctor->fields, i) {
        fxsh_ct_record_field_t field = ctor->fields[i];
        if (!field.type_val || field.type_val->kind != CT_TYPE)
            return NULL;

        fxsh_type_t *field_type = field.type_val->data.type_val;
        fxsh_type_apply_subst(subst, &field_type);

        fxsh_field_t runtime_field = {.name = field.name, .type = field_type};
        sp_dyn_array_push(fields, runtime_field);
    }

    return ct_make_record_runtime_type(fields, -1);
}

fxsh_type_t *fxsh_ct_instantiate_generic(fxsh_type_constructor_t *ctor,
                                         sp_dyn_array(fxsh_type_t *) type_args) {
    if (!ctor)
        return NULL;

    sp_dyn_array(fxsh_type_var_t) params = ct_collect_ctor_type_params(ctor);
    if (sp_dyn_array_size(params) != sp_dyn_array_size(type_args)) {
        sp_dyn_array_free(params);
        return NULL;
    }

    fxsh_subst_t subst = SP_NULLPTR;
    sp_dyn_array_for(params, i) {
        fxsh_subst_entry_t entry = {.var = params[i], .type = type_args[i]};
        sp_dyn_array_push(subst, entry);
    }

    fxsh_type_t *instance = NULL;
    if (ctor->target_type) {
        instance = ctor->target_type;
        fxsh_type_apply_subst(subst, &instance);
    } else if (ctor->kind == TYPE_RECORD) {
        instance = ct_instantiate_ctor_record(ctor, subst);
    }

    sp_dyn_array_free(subst);
    sp_dyn_array_free(params);

    return instance;
}

/*=============================================================================
 * Vector Type Constructor
 *=============================================================================*/

fxsh_type_constructor_t *fxsh_ct_make_vector_ctor(void) {
    fxsh_type_constructor_t *ctor = fxsh_alloc0(sizeof(fxsh_type_constructor_t));
    fxsh_type_var_t elem_var = fxsh_fresh_var();
    fxsh_type_t *elem_type = fxsh_type_var(elem_var);
    sp_dyn_array(fxsh_field_t) target_fields = SP_NULLPTR;

    ctor->name = (sp_str_t){.data = "Vector", .len = 6};
    ctor->kind = TYPE_RECORD;
    ctor->type_params = SP_NULLPTR;
    sp_dyn_array_push(ctor->type_params, elem_var);
    ctor->fields = SP_NULLPTR;

    fxsh_ct_record_field_t data_field = {
        .name = (sp_str_t){.data = "data", .len = 4},
        .type_val = fxsh_ct_type(elem_type),
        .default_val = NULL,
    };
    fxsh_ct_record_field_t len_field = {
        .name = (sp_str_t){.data = "len", .len = 3},
        .type_val = fxsh_ct_type(fxsh_type_con(TYPE_INT)),
        .default_val = NULL,
    };
    fxsh_ct_record_field_t cap_field = {
        .name = (sp_str_t){.data = "cap", .len = 3},
        .type_val = fxsh_ct_type(fxsh_type_con(TYPE_INT)),
        .default_val = NULL,
    };

    sp_dyn_array_push(ctor->fields, data_field);
    sp_dyn_array_push(ctor->fields, len_field);
    sp_dyn_array_push(ctor->fields, cap_field);

    sp_dyn_array_push(target_fields, ((fxsh_field_t){.name = data_field.name, .type = elem_type}));
    sp_dyn_array_push(target_fields, ((fxsh_field_t){.name = len_field.name,
                                                     .type = len_field.type_val->data.type_val}));
    sp_dyn_array_push(target_fields, ((fxsh_field_t){.name = cap_field.name,
                                                     .type = cap_field.type_val->data.type_val}));
    ctor->target_type = ct_make_record_runtime_type(target_fields, -1);
    return ctor;
}

fxsh_ct_value_t *fxsh_ct_make_vector(fxsh_ct_value_t *elem_type) {
    if (!elem_type || elem_type->kind != CT_TYPE)
        return NULL;

    /* Create a Vector(T) type at compile time */
    fxsh_ct_value_t *vec_type = fxsh_ct_make_record_type((sp_str_t){.data = "Vector", .len = 6});

    /* Add fields: data, len, cap */
    fxsh_ct_record_add_field(vec_type, (sp_str_t){.data = "data", .len = 4}, elem_type);
    fxsh_ct_record_add_field(vec_type, (sp_str_t){.data = "len", .len = 3},
                             fxsh_ct_type(fxsh_type_con(TYPE_INT)));
    fxsh_ct_record_add_field(vec_type, (sp_str_t){.data = "cap", .len = 3},
                             fxsh_ct_type(fxsh_type_con(TYPE_INT)));

    return vec_type;
}
