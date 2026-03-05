/*
 * interp.c - fxsh tree-walking interpreter (MVP)
 */

#include "fxsh.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    RV_UNIT,
    RV_BOOL,
    RV_INT,
    RV_FLOAT,
    RV_STRING,
    RV_FUNCTION,
    RV_CONSTR,
} rv_kind_t;

typedef struct rv_value rv_value_t;
typedef struct rv_env rv_env_t;

typedef struct {
    fxsh_ast_list_t params;
    fxsh_ast_node_t *body;
    rv_env_t *env;
    sp_dyn_array(rv_value_t *) bound_args;
} rv_func_t;

typedef struct {
    sp_str_t tag;
    sp_dyn_array(rv_value_t *) args;
} rv_constr_t;

struct rv_value {
    rv_kind_t kind;
    union {
        bool b;
        s64 i;
        f64 f;
        sp_str_t s;
        rv_func_t fn;
        rv_constr_t constr;
    } as;
};

struct rv_env {
    sp_str_t name;
    rv_value_t *value;
    rv_env_t *next;
};

static rv_value_t *rv_new(rv_kind_t k) {
    rv_value_t *v = (rv_value_t *)fxsh_alloc0(sizeof(rv_value_t));
    v->kind = k;
    return v;
}

static rv_value_t *rv_unit(void) {
    return rv_new(RV_UNIT);
}

static rv_value_t *rv_bool(bool b) {
    rv_value_t *v = rv_new(RV_BOOL);
    v->as.b = b;
    return v;
}

static rv_value_t *rv_int(s64 i) {
    rv_value_t *v = rv_new(RV_INT);
    v->as.i = i;
    return v;
}

static rv_value_t *rv_float(f64 f) {
    rv_value_t *v = rv_new(RV_FLOAT);
    v->as.f = f;
    return v;
}

static rv_value_t *rv_string(sp_str_t s) {
    rv_value_t *v = rv_new(RV_STRING);
    v->as.s = s;
    return v;
}

static rv_value_t *rv_function(fxsh_ast_list_t params, fxsh_ast_node_t *body, rv_env_t *env) {
    rv_value_t *v = rv_new(RV_FUNCTION);
    v->as.fn.params = params;
    v->as.fn.body = body;
    v->as.fn.env = env;
    v->as.fn.bound_args = SP_NULLPTR;
    return v;
}

static rv_value_t *rv_constr(sp_str_t tag, sp_dyn_array(rv_value_t *) args) {
    rv_value_t *v = rv_new(RV_CONSTR);
    v->as.constr.tag = tag;
    v->as.constr.args = args;
    return v;
}

static rv_env_t *env_bind(rv_env_t *env, sp_str_t name, rv_value_t *value) {
    rv_env_t *n = (rv_env_t *)fxsh_alloc0(sizeof(rv_env_t));
    n->name = name;
    n->value = value;
    n->next = env;
    return n;
}

static rv_value_t *env_lookup(rv_env_t *env, sp_str_t name) {
    for (rv_env_t *n = env; n; n = n->next) {
        if (sp_str_equal(n->name, name))
            return n->value;
    }
    return NULL;
}

static bool rv_equal(rv_value_t *a, rv_value_t *b) {
    if (!a || !b)
        return a == b;
    if (a->kind != b->kind)
        return false;

    switch (a->kind) {
        case RV_UNIT:
            return true;
        case RV_BOOL:
            return a->as.b == b->as.b;
        case RV_INT:
            return a->as.i == b->as.i;
        case RV_FLOAT:
            return a->as.f == b->as.f;
        case RV_STRING:
            return sp_str_equal(a->as.s, b->as.s);
        case RV_CONSTR: {
            if (!sp_str_equal(a->as.constr.tag, b->as.constr.tag))
                return false;
            if (sp_dyn_array_size(a->as.constr.args) != sp_dyn_array_size(b->as.constr.args))
                return false;
            sp_dyn_array_for(a->as.constr.args, i) {
                if (!rv_equal(a->as.constr.args[i], b->as.constr.args[i]))
                    return false;
            }
            return true;
        }
        case RV_FUNCTION:
            return a == b;
    }
    return false;
}

static sp_str_t rv_to_string(rv_value_t *v) {
    if (!v)
        return sp_str_lit("<null>");

    switch (v->kind) {
        case RV_UNIT:
            return sp_str_lit("()");
        case RV_BOOL:
            return v->as.b ? sp_str_lit("true") : sp_str_lit("false");
        case RV_INT: {
            char *buf = (char *)fxsh_alloc0(32);
            snprintf(buf, 32, "%ld", v->as.i);
            return sp_str_view(buf);
        }
        case RV_FLOAT: {
            char *buf = (char *)fxsh_alloc0(64);
            snprintf(buf, 64, "%g", v->as.f);
            return sp_str_view(buf);
        }
        case RV_STRING:
            return v->as.s;
        case RV_FUNCTION:
            return sp_str_lit("<function>");
        case RV_CONSTR: {
            if (sp_dyn_array_size(v->as.constr.args) == 0)
                return v->as.constr.tag;
            char *buf = (char *)fxsh_alloc0(256);
            size_t off = 0;
            off += (size_t)snprintf(buf + off, 256 - off, "%.*s(", v->as.constr.tag.len,
                                    v->as.constr.tag.data);
            sp_dyn_array_for(v->as.constr.args, i) {
                sp_str_t s = rv_to_string(v->as.constr.args[i]);
                off += (size_t)snprintf(buf + off, 256 - off, "%.*s%s", s.len, s.data,
                                        i + 1 < sp_dyn_array_size(v->as.constr.args) ? ", " : "");
                if (off >= 252)
                    break;
            }
            snprintf(buf + off, 256 - off, ")");
            return sp_str_view(buf);
        }
    }
    return sp_str_lit("<unknown>");
}

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
        rv_value_t *partial = rv_new(RV_FUNCTION);
        partial->as.fn.params = fn->as.fn.params;
        partial->as.fn.body = fn->as.fn.body;
        partial->as.fn.env = fn->as.fn.env;
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
