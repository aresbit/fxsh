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
        case AST_CT_QUOTE:
        case AST_CT_UNQUOTE:
        case AST_CT_SPLICE:
        case AST_CT_EVAL:
        case AST_CT_COMPILE_ERROR:
        case AST_CT_COMPILE_LOG:
        case AST_CT_PANIC:
            c->data.ct_type_of.operand = clone_ast(n->data.ct_type_of.operand);
            break;
        case AST_CT_SIZE_OF:
        case AST_CT_ALIGN_OF:
        case AST_CT_FIELDS_OF:
            c->data.ct_type_op.type_expr = clone_ast(n->data.ct_type_op.type_expr);
            break;
        case AST_CT_HAS_FIELD:
            c->data.ct_has_field.type_expr = clone_ast(n->data.ct_has_field.type_expr);
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
        case CT_TYPE:
            *out = fxsh_ast_lit_string(sp_str_view(fxsh_type_to_string(v->data.type_val)),
                                       (fxsh_loc_t){0});
            return true;
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

static fxsh_ct_value_t *eval_let_in(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    /* Evaluate each binding and add to environment */
    sp_dyn_array_for(ast->data.let_in.bindings, i) {
        fxsh_ast_node_t *binding = ast->data.let_in.bindings[i];
        if (binding->kind == AST_DECL_LET || binding->kind == AST_LET) {
            fxsh_ct_value_t *val = eval_expr(binding->data.let.value, ctx);
            if (val) {
                bind_var(ctx, binding->data.let.name, val);
            }
        }
    }

    return eval_expr(ast->data.let_in.body, ctx);
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
        case AST_CT_JSON_SCHEMA: {
            fxsh_ct_value_t *type_val =
                eval_type_operand_to_ct_type(ast->data.ct_type_op.type_expr, ctx);
            return fxsh_ct_op_json_schema(type_val);
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
        fxsh_ct_value_t *val = eval_expr(ast->data.let.value, ctx);
        if (val) {
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
            if ((d->kind == AST_DECL_LET || d->kind == AST_LET) && d->data.let.is_comptime) {
                fxsh_ct_result_t r = fxsh_ct_eval(d, ctx);
                if (r.error != ERR_OK)
                    return r;
            }
        }
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
    return k == AST_CT_TYPE_OF || k == AST_CT_SIZE_OF || k == AST_CT_ALIGN_OF ||
           k == AST_CT_FIELDS_OF || k == AST_CT_HAS_FIELD || k == AST_CT_JSON_SCHEMA ||
           k == AST_CT_QUOTE || k == AST_CT_UNQUOTE || k == AST_CT_SPLICE || k == AST_CT_EVAL ||
           k == AST_CT_COMPILE_ERROR || k == AST_CT_COMPILE_LOG || k == AST_CT_PANIC;
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

        if ((d->kind == AST_DECL_LET || d->kind == AST_LET) && d->data.let.is_comptime) {
            fxsh_ct_result_t r = fxsh_ct_eval(d, &ctx);
            if (r.error != ERR_OK)
                return r.error;
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

/*=============================================================================
 * Generic Type Instantiation
 *=============================================================================*/

fxsh_type_t *fxsh_ct_instantiate_generic(fxsh_type_constructor_t *ctor,
                                         sp_dyn_array(fxsh_type_t *) type_args) {
    if (!ctor)
        return NULL;

    /* Create a new concrete type from the constructor */
    fxsh_type_t *instance = fxsh_alloc0(sizeof(fxsh_type_t));
    instance->kind = ctor->kind;

    /* TODO: Substitute type parameters with arguments */

    return instance;
}

/*=============================================================================
 * Vector Type Constructor
 *=============================================================================*/

fxsh_type_constructor_t *fxsh_ct_make_vector_ctor(void) {
    fxsh_type_constructor_t *ctor = fxsh_alloc0(sizeof(fxsh_type_constructor_t));
    ctor->name = (sp_str_t){.data = "Vector", .len = 6};
    ctor->kind = TYPE_RECORD;
    ctor->fields = SP_NULLPTR;
    ctor->target_type = NULL;
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
