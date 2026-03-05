/*
 * interp.c - fxsh tree-walking interpreter (MVP)
 */

#include "fxsh.h"

#include <stdio.h>
#include <string.h>

typedef fxsh_rt_value_t rv_value_t;
typedef fxsh_rt_env_t rv_env_t;

#define RV_UNIT     FXSH_RT_UNIT
#define RV_BOOL     FXSH_RT_BOOL
#define RV_INT      FXSH_RT_INT
#define RV_FLOAT    FXSH_RT_FLOAT
#define RV_STRING   FXSH_RT_STRING
#define RV_FUNCTION FXSH_RT_FUNCTION
#define RV_CONSTR   FXSH_RT_CONSTR

#define rv_unit      fxsh_rt_unit
#define rv_bool      fxsh_rt_bool
#define rv_int       fxsh_rt_int
#define rv_float     fxsh_rt_float
#define rv_string    fxsh_rt_string
#define rv_function  fxsh_rt_function
#define rv_constr    fxsh_rt_constr
#define env_bind     fxsh_rt_env_bind
#define env_lookup   fxsh_rt_env_lookup
#define rv_equal     fxsh_rt_equal
#define rv_to_string fxsh_rt_to_string

static rv_value_t *eval_expr(fxsh_ast_node_t *ast, rv_env_t *env, fxsh_error_t *err);

static bool bind_pattern(fxsh_ast_node_t *pat, rv_value_t *val, rv_env_t **env) {
    if (!pat)
        return false;

    switch (pat->kind) {
        case AST_PAT_WILD:
            return true;
        case AST_PAT_VAR:
        case AST_IDENT:
            *env = env_bind(*env, pat->data.ident, val);
            return true;
        case AST_PAT_CONSTR: {
            if (!val || val->kind != RV_CONSTR)
                return false;
            if (!sp_str_equal(pat->data.constr_appl.constr_name, val->as.constr.tag))
                return false;
            if (sp_dyn_array_size(pat->data.constr_appl.args) !=
                sp_dyn_array_size(val->as.constr.args))
                return false;
            sp_dyn_array_for(pat->data.constr_appl.args, i) {
                if (!bind_pattern(pat->data.constr_appl.args[i], val->as.constr.args[i], env))
                    return false;
            }
            return true;
        }
        case AST_LIT_INT:
            return val && val->kind == RV_INT && pat->data.lit_int == val->as.i;
        case AST_LIT_FLOAT:
            return val && val->kind == RV_FLOAT && pat->data.lit_float == val->as.f;
        case AST_LIT_STRING:
            return val && val->kind == RV_STRING && sp_str_equal(pat->data.lit_string, val->as.s);
        case AST_LIT_BOOL:
            return val && val->kind == RV_BOOL && pat->data.lit_bool == val->as.b;
        case AST_LIT_UNIT:
            return val && val->kind == RV_UNIT;
        default:
            return false;
    }
}

static rv_value_t *apply_one(rv_value_t *fn, rv_value_t *arg, fxsh_error_t *err) {
    if (!fn || fn->kind != RV_FUNCTION) {
        *err = ERR_INVALID_INPUT;
        fprintf(stderr, "Runtime error: trying to call a non-function value\n");
        return NULL;
    }

    sp_dyn_array(rv_value_t *) bound = SP_NULLPTR;
    sp_dyn_array_for(fn->as.fn.bound_args, i) {
        sp_dyn_array_push(bound, fn->as.fn.bound_args[i]);
    }
    sp_dyn_array_push(bound, arg);

    u32 arity = (u32)sp_dyn_array_size(fn->as.fn.params);
    if (sp_dyn_array_size(bound) < arity) {
        rv_value_t *partial = rv_function(fn->as.fn.params, fn->as.fn.body, fn->as.fn.env);
        partial->as.fn.bound_args = bound;
        return partial;
    }

    if (sp_dyn_array_size(bound) > arity) {
        *err = ERR_INVALID_INPUT;
        fprintf(stderr, "Runtime error: too many arguments\n");
        return NULL;
    }

    rv_env_t *call_env = fn->as.fn.env;
    sp_dyn_array_for(fn->as.fn.params, i) {
        fxsh_ast_node_t *p = fn->as.fn.params[i];
        if (!p)
            continue;
        if (p->kind == AST_PAT_VAR || p->kind == AST_IDENT) {
            call_env = env_bind(call_env, p->data.ident, bound[i]);
        }
    }

    return eval_expr(fn->as.fn.body, call_env, err);
}

static rv_value_t *eval_binary(fxsh_ast_node_t *ast, rv_env_t *env, fxsh_error_t *err) {
    rv_value_t *l = eval_expr(ast->data.binary.left, env, err);
    if (!l || *err != ERR_OK)
        return NULL;
    rv_value_t *r = eval_expr(ast->data.binary.right, env, err);
    if (!r || *err != ERR_OK)
        return NULL;

    switch (ast->data.binary.op) {
        case TOK_PLUS:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_int(l->as.i + r->as.i);
            if (l->kind == RV_FLOAT && r->kind == RV_FLOAT)
                return rv_float(l->as.f + r->as.f);
            break;
        case TOK_MINUS:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_int(l->as.i - r->as.i);
            break;
        case TOK_STAR:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_int(l->as.i * r->as.i);
            break;
        case TOK_SLASH:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_int(r->as.i == 0 ? 0 : l->as.i / r->as.i);
            break;
        case TOK_PERCENT:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_int(r->as.i == 0 ? 0 : l->as.i % r->as.i);
            break;
        case TOK_CONCAT:
            if (l->kind == RV_STRING && r->kind == RV_STRING) {
                u32 len = l->as.s.len + r->as.s.len;
                c8 *buf = (c8 *)fxsh_alloc0(len + 1);
                memcpy(buf, l->as.s.data, l->as.s.len);
                memcpy(buf + l->as.s.len, r->as.s.data, r->as.s.len);
                return rv_string((sp_str_t){.data = buf, .len = len});
            }
            break;
        case TOK_EQ:
            return rv_bool(rv_equal(l, r));
        case TOK_NEQ:
            return rv_bool(!rv_equal(l, r));
        case TOK_LT:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_bool(l->as.i < r->as.i);
            break;
        case TOK_GT:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_bool(l->as.i > r->as.i);
            break;
        case TOK_LEQ:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_bool(l->as.i <= r->as.i);
            break;
        case TOK_GEQ:
            if (l->kind == RV_INT && r->kind == RV_INT)
                return rv_bool(l->as.i >= r->as.i);
            break;
        case TOK_AND:
            if (l->kind == RV_BOOL && r->kind == RV_BOOL)
                return rv_bool(l->as.b && r->as.b);
            break;
        case TOK_OR:
            if (l->kind == RV_BOOL && r->kind == RV_BOOL)
                return rv_bool(l->as.b || r->as.b);
            break;
        default:
            break;
    }

    *err = ERR_INVALID_INPUT;
    fprintf(stderr, "Runtime error: invalid operands for binary op %s\n",
            fxsh_token_kind_name(ast->data.binary.op));
    return NULL;
}

static rv_value_t *eval_expr(fxsh_ast_node_t *ast, rv_env_t *env, fxsh_error_t *err) {
    if (!ast)
        return rv_unit();

    switch (ast->kind) {
        case AST_LIT_INT:
            return rv_int(ast->data.lit_int);
        case AST_LIT_FLOAT:
            return rv_float(ast->data.lit_float);
        case AST_LIT_STRING:
            return rv_string(ast->data.lit_string);
        case AST_LIT_BOOL:
            return rv_bool(ast->data.lit_bool);
        case AST_LIT_UNIT:
            return rv_unit();
        case AST_IDENT: {
            rv_value_t *v = env_lookup(env, ast->data.ident);
            if (!v) {
                *err = ERR_INVALID_INPUT;
                fprintf(stderr, "Runtime error: unbound variable `%.*s`\n", ast->data.ident.len,
                        ast->data.ident.data);
                return NULL;
            }
            return v;
        }
        case AST_BINARY:
            return eval_binary(ast, env, err);
        case AST_UNARY: {
            rv_value_t *v = eval_expr(ast->data.unary.operand, env, err);
            if (!v || *err != ERR_OK)
                return NULL;
            if (ast->data.unary.op == TOK_MINUS && v->kind == RV_INT)
                return rv_int(-v->as.i);
            if (ast->data.unary.op == TOK_NOT && v->kind == RV_BOOL)
                return rv_bool(!v->as.b);
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: invalid unary op\n");
            return NULL;
        }
        case AST_IF: {
            rv_value_t *c = eval_expr(ast->data.if_expr.cond, env, err);
            if (!c || *err != ERR_OK)
                return NULL;
            if (c->kind != RV_BOOL) {
                *err = ERR_INVALID_INPUT;
                fprintf(stderr, "Runtime error: if condition must be bool\n");
                return NULL;
            }
            if (c->as.b)
                return eval_expr(ast->data.if_expr.then_branch, env, err);
            if (ast->data.if_expr.else_branch)
                return eval_expr(ast->data.if_expr.else_branch, env, err);
            return rv_unit();
        }
        case AST_LAMBDA:
            return rv_function(ast->data.lambda.params, ast->data.lambda.body, env);
        case AST_CALL: {
            rv_value_t *fn = eval_expr(ast->data.call.func, env, err);
            if (!fn || *err != ERR_OK)
                return NULL;
            rv_value_t *res = fn;
            sp_dyn_array_for(ast->data.call.args, i) {
                rv_value_t *arg = eval_expr(ast->data.call.args[i], env, err);
                if (!arg || *err != ERR_OK)
                    return NULL;
                res = apply_one(res, arg, err);
                if (!res || *err != ERR_OK)
                    return NULL;
            }
            return res;
        }
        case AST_PIPE: {
            rv_value_t *lhs = eval_expr(ast->data.pipe.left, env, err);
            if (!lhs || *err != ERR_OK)
                return NULL;
            rv_value_t *rhs = eval_expr(ast->data.pipe.right, env, err);
            if (!rhs || *err != ERR_OK)
                return NULL;
            return apply_one(rhs, lhs, err);
        }
        case AST_CONSTR_APPL: {
            sp_dyn_array(rv_value_t *) args = SP_NULLPTR;
            sp_dyn_array_for(ast->data.constr_appl.args, i) {
                rv_value_t *arg = eval_expr(ast->data.constr_appl.args[i], env, err);
                if (!arg || *err != ERR_OK)
                    return NULL;
                sp_dyn_array_push(args, arg);
            }
            return rv_constr(ast->data.constr_appl.constr_name, args);
        }
        case AST_MATCH: {
            rv_value_t *mv = eval_expr(ast->data.match_expr.expr, env, err);
            if (!mv || *err != ERR_OK)
                return NULL;

            sp_dyn_array_for(ast->data.match_expr.arms, i) {
                fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
                if (!arm || arm->kind != AST_MATCH_ARM)
                    continue;

                rv_env_t *arm_env = env;
                if (!bind_pattern(arm->data.match_arm.pattern, mv, &arm_env))
                    continue;

                if (arm->data.match_arm.guard) {
                    rv_value_t *g = eval_expr(arm->data.match_arm.guard, arm_env, err);
                    if (!g || *err != ERR_OK)
                        return NULL;
                    if (g->kind != RV_BOOL || !g->as.b)
                        continue;
                }

                return eval_expr(arm->data.match_arm.body, arm_env, err);
            }

            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: non-exhaustive match at %u:%u\n", ast->loc.line,
                    ast->loc.column);
            return NULL;
        }
        case AST_LET: {
            rv_value_t *v = eval_expr(ast->data.let.value, env, err);
            if (!v || *err != ERR_OK)
                return NULL;
            env = env_bind(env, ast->data.let.name, v);
            (void)env;
            return rv_unit();
        }
        case AST_DECL_LET: {
            if (ast->data.let.is_rec) {
                rv_value_t *slot = rv_unit();
                rv_env_t *rec_env = env_bind(env, ast->data.let.name, slot);
                rv_value_t *v = eval_expr(ast->data.let.value, rec_env, err);
                if (!v || *err != ERR_OK)
                    return NULL;
                *slot = *v;
                return rv_unit();
            }
            rv_value_t *v = eval_expr(ast->data.let.value, env, err);
            if (!v || *err != ERR_OK)
                return NULL;
            env = env_bind(env, ast->data.let.name, v);
            (void)env;
            return rv_unit();
        }
        case AST_LET_IN: {
            rv_env_t *new_env = env;
            sp_dyn_array_for(ast->data.let_in.bindings, i) {
                fxsh_ast_node_t *b = ast->data.let_in.bindings[i];
                if (!b || (b->kind != AST_LET && b->kind != AST_DECL_LET))
                    continue;

                if (b->data.let.is_rec) {
                    rv_value_t *slot = rv_unit();
                    new_env = env_bind(new_env, b->data.let.name, slot);
                    rv_value_t *v = eval_expr(b->data.let.value, new_env, err);
                    if (!v || *err != ERR_OK)
                        return NULL;
                    *slot = *v;
                } else {
                    rv_value_t *v = eval_expr(b->data.let.value, new_env, err);
                    if (!v || *err != ERR_OK)
                        return NULL;
                    new_env = env_bind(new_env, b->data.let.name, v);
                }
            }
            return eval_expr(ast->data.let_in.body, new_env, err);
        }
        case AST_PROGRAM: {
            rv_env_t *prog_env = env;
            rv_value_t *last = rv_unit();
            sp_dyn_array_for(ast->data.decls, i) {
                fxsh_ast_node_t *d = ast->data.decls[i];
                if (!d)
                    continue;

                if (d->kind == AST_DECL_LET || d->kind == AST_LET) {
                    if (d->data.let.is_rec) {
                        rv_value_t *slot = rv_unit();
                        prog_env = env_bind(prog_env, d->data.let.name, slot);
                        rv_value_t *v = eval_expr(d->data.let.value, prog_env, err);
                        if (!v || *err != ERR_OK)
                            return NULL;
                        *slot = *v;
                        last = rv_unit();
                    } else {
                        rv_value_t *v = eval_expr(d->data.let.value, prog_env, err);
                        if (!v || *err != ERR_OK)
                            return NULL;
                        prog_env = env_bind(prog_env, d->data.let.name, v);
                        last = rv_unit();
                    }
                } else if (d->kind == AST_TYPE_DEF) {
                    last = rv_unit();
                } else {
                    last = eval_expr(d, prog_env, err);
                    if (!last || *err != ERR_OK)
                        return NULL;
                }
            }
            return last;
        }
        default:
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: unsupported AST node %d\n", ast->kind);
            return NULL;
    }
}

fxsh_error_t fxsh_interp_eval(fxsh_ast_node_t *ast, sp_str_t *out_value_str) {
    fxsh_error_t err = ERR_OK;
    rv_value_t *v = eval_expr(ast, NULL, &err);
    if (err != ERR_OK)
        return err;
    if (out_value_str)
        *out_value_str = rv_to_string(v);
    return ERR_OK;
}
