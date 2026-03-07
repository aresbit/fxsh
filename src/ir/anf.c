/*
 * anf.c - AST -> ANF IR textual lowering
 */

#include "fxsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    sp_dyn_array(c8) out;
    u32 indent;
    u32 tmp;
} anf_ctx_t;

static void anf_emit_raw(anf_ctx_t *ctx, const char *s) {
    for (u32 i = 0; s[i]; i++)
        sp_dyn_array_push(ctx->out, s[i]);
}

static void anf_emit_indent(anf_ctx_t *ctx) {
    for (u32 i = 0; i < ctx->indent; i++) {
        sp_dyn_array_push(ctx->out, ' ');
        sp_dyn_array_push(ctx->out, ' ');
    }
}

static void anf_emit_line(anf_ctx_t *ctx, const char *s) {
    anf_emit_indent(ctx);
    anf_emit_raw(ctx, s);
    sp_dyn_array_push(ctx->out, '\n');
}

static void anf_emit_fmt(anf_ctx_t *ctx, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    anf_emit_raw(ctx, buf);
}

static char *anf_alloc_sprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    char *p = (char *)fxsh_alloc0(n + 1);
    memcpy(p, buf, n);
    return p;
}

static char *anf_fresh_tmp(anf_ctx_t *ctx) {
    return anf_alloc_sprintf("t%u", ctx->tmp++);
}

static char *anf_lower_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr);

static bool ast_is_atom(fxsh_ast_node_t *e) {
    if (!e)
        return false;
    switch (e->kind) {
        case AST_LIT_INT:
        case AST_LIT_FLOAT:
        case AST_LIT_STRING:
        case AST_LIT_BOOL:
        case AST_LIT_UNIT:
        case AST_IDENT:
            return true;
        case AST_CONSTR_APPL:
            return sp_dyn_array_size(e->data.constr_appl.args) == 0;
        default:
            return false;
    }
}

static char *anf_escape_string(sp_str_t s) {
    char *tmp = (char *)fxsh_alloc0((size_t)s.len * 4 + 8);
    size_t p = 0;
    tmp[p++] = '"';
    for (u32 i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        if (c == '"' || c == '\\') {
            tmp[p++] = '\\';
            tmp[p++] = (char)c;
        } else if (c == '\n') {
            tmp[p++] = '\\';
            tmp[p++] = 'n';
        } else if (c == '\t') {
            tmp[p++] = '\\';
            tmp[p++] = 't';
        } else {
            tmp[p++] = (char)c;
        }
    }
    tmp[p++] = '"';
    tmp[p] = '\0';
    return tmp;
}

static char *anf_pattern_text(fxsh_ast_node_t *pat);

static char *anf_expr_inline_text(fxsh_ast_node_t *e) {
    if (!e)
        return anf_alloc_sprintf("()");
    if (ast_is_atom(e)) {
        switch (e->kind) {
            case AST_LIT_INT:
                return anf_alloc_sprintf("%lld", (long long)e->data.lit_int);
            case AST_LIT_FLOAT:
                return anf_alloc_sprintf("%.17g", e->data.lit_float);
            case AST_LIT_BOOL:
                return anf_alloc_sprintf("%s", e->data.lit_bool ? "true" : "false");
            case AST_LIT_UNIT:
                return anf_alloc_sprintf("()");
            case AST_IDENT:
                return anf_alloc_sprintf("%.*s", e->data.ident.len, e->data.ident.data);
            case AST_LIT_STRING:
                return anf_escape_string(e->data.lit_string);
            case AST_CONSTR_APPL:
                return anf_alloc_sprintf("%.*s", e->data.constr_appl.constr_name.len,
                                         e->data.constr_appl.constr_name.data);
            default:
                break;
        }
    }

    switch (e->kind) {
        case AST_BINARY: {
            char *l = anf_expr_inline_text(e->data.binary.left);
            char *r = anf_expr_inline_text(e->data.binary.right);
            return anf_alloc_sprintf("prim(%s, %s, %s)", fxsh_token_kind_name(e->data.binary.op), l,
                                     r);
        }
        case AST_UNARY: {
            char *x = anf_expr_inline_text(e->data.unary.operand);
            return anf_alloc_sprintf("prim(%s, %s)", fxsh_token_kind_name(e->data.unary.op), x);
        }
        case AST_CALL:
            return anf_alloc_sprintf("call(...)");
        case AST_IF:
            return anf_alloc_sprintf("if(...)");
        case AST_MATCH:
            return anf_alloc_sprintf("match(...)");
        case AST_PIPE: {
            char *l = anf_expr_inline_text(e->data.pipe.left);
            char *r = anf_expr_inline_text(e->data.pipe.right);
            return anf_alloc_sprintf("pipe(%s, %s)", l, r);
        }
        case AST_TUPLE:
            return anf_alloc_sprintf("tuple(...)");
        case AST_LIST:
            return anf_alloc_sprintf("list(...)");
        case AST_RECORD:
            return anf_alloc_sprintf("record(...)");
        case AST_FIELD_ACCESS: {
            char *o = anf_expr_inline_text(e->data.field.object);
            return anf_alloc_sprintf("field(%s.%.*s)", o, e->data.field.field.len,
                                     e->data.field.field.data);
        }
        case AST_LAMBDA:
            return anf_alloc_sprintf("fun(...)");
        case AST_CONSTR_APPL:
            return anf_alloc_sprintf("%.*s(...)", e->data.constr_appl.constr_name.len,
                                     e->data.constr_appl.constr_name.data);
        default:
            return anf_alloc_sprintf("<expr kind=%d>", (int)e->kind);
    }
}

static char *anf_pattern_text(fxsh_ast_node_t *pat) {
    if (!pat)
        return anf_alloc_sprintf("_");
    switch (pat->kind) {
        case AST_PAT_WILD:
            return anf_alloc_sprintf("_");
        case AST_PAT_VAR:
        case AST_IDENT:
            return anf_alloc_sprintf("%.*s", pat->data.ident.len, pat->data.ident.data);
        case AST_LIT_INT:
            return anf_alloc_sprintf("%lld", (long long)pat->data.lit_int);
        case AST_LIT_FLOAT:
            return anf_alloc_sprintf("%.17g", pat->data.lit_float);
        case AST_LIT_BOOL:
            return anf_alloc_sprintf("%s", pat->data.lit_bool ? "true" : "false");
        case AST_LIT_UNIT:
            return anf_alloc_sprintf("()");
        case AST_LIT_STRING:
            return anf_escape_string(pat->data.lit_string);
        case AST_PAT_TUPLE:
        case AST_TUPLE: {
            sp_dyn_array(c8) b = SP_NULLPTR;
            sp_dyn_array_push(b, '(');
            sp_dyn_array_for(pat->data.elements, i) {
                char *p = anf_pattern_text(pat->data.elements[i]);
                if (i > 0) {
                    sp_dyn_array_push(b, ',');
                    sp_dyn_array_push(b, ' ');
                }
                for (u32 k = 0; p[k]; k++)
                    sp_dyn_array_push(b, p[k]);
            }
            sp_dyn_array_push(b, ')');
            sp_dyn_array_push(b, '\0');
            return (char *)b;
        }
        case AST_PAT_CONS: {
            if (sp_dyn_array_size(pat->data.elements) == 2) {
                char *h = anf_pattern_text(pat->data.elements[0]);
                char *t = anf_pattern_text(pat->data.elements[1]);
                return anf_alloc_sprintf("%s :: %s", h, t);
            }
            return anf_alloc_sprintf("_ :: _");
        }
        case AST_PAT_RECORD: {
            sp_dyn_array(c8) b = SP_NULLPTR;
            sp_dyn_array_push(b, '{');
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (i > 0) {
                    sp_dyn_array_push(b, ';');
                    sp_dyn_array_push(b, ' ');
                }
                if (f && f->kind == AST_FIELD_ACCESS) {
                    char *v = anf_pattern_text(f->data.field.object);
                    for (u32 k = 0; k < f->data.field.field.len; k++)
                        sp_dyn_array_push(b, f->data.field.field.data[k]);
                    sp_dyn_array_push(b, '=');
                    for (u32 k = 0; v[k]; k++)
                        sp_dyn_array_push(b, v[k]);
                } else {
                    sp_dyn_array_push(b, '_');
                }
            }
            sp_dyn_array_push(b, '}');
            sp_dyn_array_push(b, '\0');
            return (char *)b;
        }
        case AST_PAT_CONSTR:
        case AST_CONSTR_APPL: {
            sp_dyn_array(c8) b = SP_NULLPTR;
            for (u32 i = 0; i < pat->data.constr_appl.constr_name.len; i++)
                sp_dyn_array_push(b, pat->data.constr_appl.constr_name.data[i]);
            sp_dyn_array_for(pat->data.constr_appl.args, i) {
                char *a = anf_pattern_text(pat->data.constr_appl.args[i]);
                sp_dyn_array_push(b, ' ');
                for (u32 k = 0; a[k]; k++)
                    sp_dyn_array_push(b, a[k]);
            }
            sp_dyn_array_push(b, '\0');
            return (char *)b;
        }
        case AST_LIST: {
            sp_dyn_array(c8) b = SP_NULLPTR;
            sp_dyn_array_push(b, '[');
            sp_dyn_array_for(pat->data.elements, i) {
                char *e = anf_pattern_text(pat->data.elements[i]);
                if (i > 0) {
                    sp_dyn_array_push(b, ';');
                    sp_dyn_array_push(b, ' ');
                }
                for (u32 k = 0; e[k]; k++)
                    sp_dyn_array_push(b, e[k]);
            }
            sp_dyn_array_push(b, ']');
            sp_dyn_array_push(b, '\0');
            return (char *)b;
        }
        default:
            return anf_alloc_sprintf("_");
    }
}

static char *anf_lower_call_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr) {
    fxsh_ast_list_t flat_args = SP_NULLPTR;
    fxsh_ast_node_t *f = expr;
    while (f && f->kind == AST_CALL) {
        sp_dyn_array_for(f->data.call.args, i) {
            sp_dyn_array_push(flat_args, f->data.call.args[i]);
        }
        f = f->data.call.func;
    }
    char *fn_atom = anf_lower_to_atom(ctx, f);
    sp_dyn_array(char *) args_atoms = SP_NULLPTR;
    for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
        char *a = anf_lower_to_atom(ctx, flat_args[i]);
        sp_dyn_array_push(args_atoms, a);
    }
    char *tmp = anf_fresh_tmp(ctx);
    anf_emit_indent(ctx);
    anf_emit_fmt(ctx, "let %s = call %s(", tmp, fn_atom);
    sp_dyn_array_for(args_atoms, i) {
        if (i > 0)
            anf_emit_raw(ctx, ", ");
        anf_emit_raw(ctx, args_atoms[i]);
    }
    anf_emit_raw(ctx, ")\n");
    return tmp;
}

static char *anf_lower_match_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr) {
    char *scrut = anf_lower_to_atom(ctx, expr->data.match_expr.expr);
    char *tmp = anf_fresh_tmp(ctx);
    anf_emit_indent(ctx);
    anf_emit_fmt(ctx, "let %s = match %s with {\n", tmp, scrut);
    ctx->indent++;
    sp_dyn_array_for(expr->data.match_expr.arms, i) {
        fxsh_ast_node_t *arm = expr->data.match_expr.arms[i];
        if (!arm || arm->kind != AST_MATCH_ARM)
            continue;
        char *pat = anf_pattern_text(arm->data.match_arm.pattern);
        char *g = NULL;
        if (arm->data.match_arm.guard)
            g = anf_expr_inline_text(arm->data.match_arm.guard);

        anf_emit_indent(ctx);
        anf_emit_fmt(ctx, "| %s", pat);
        if (g)
            anf_emit_fmt(ctx, " when %s", g);
        anf_emit_raw(ctx, " -> {\n");
        ctx->indent++;
        char *body = anf_lower_to_atom(ctx, arm->data.match_arm.body);
        anf_emit_indent(ctx);
        anf_emit_fmt(ctx, "%s\n", body);
        ctx->indent--;
        anf_emit_line(ctx, "}");
    }
    ctx->indent--;
    anf_emit_line(ctx, "}");
    return tmp;
}

static char *anf_lower_lambda_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr) {
    char *tmp = anf_fresh_tmp(ctx);
    anf_emit_indent(ctx);
    anf_emit_fmt(ctx, "let %s = fun (", tmp);
    sp_dyn_array_for(expr->data.lambda.params, i) {
        char *p = anf_pattern_text(expr->data.lambda.params[i]);
        if (i > 0)
            anf_emit_raw(ctx, ", ");
        anf_emit_raw(ctx, p);
    }
    anf_emit_raw(ctx, ") -> {\n");
    ctx->indent++;
    char *body = anf_lower_to_atom(ctx, expr->data.lambda.body);
    anf_emit_indent(ctx);
    anf_emit_fmt(ctx, "%s\n", body);
    ctx->indent--;
    anf_emit_line(ctx, "}");
    return tmp;
}

static char *anf_lower_tuple_or_list_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr,
                                             const char *tag) {
    sp_dyn_array(char *) elems = SP_NULLPTR;
    sp_dyn_array_for(expr->data.elements, i) {
        char *a = anf_lower_to_atom(ctx, expr->data.elements[i]);
        sp_dyn_array_push(elems, a);
    }
    char *tmp = anf_fresh_tmp(ctx);
    anf_emit_indent(ctx);
    anf_emit_fmt(ctx, "let %s = %s(", tmp, tag);
    sp_dyn_array_for(elems, i) {
        if (i > 0)
            anf_emit_raw(ctx, ", ");
        anf_emit_raw(ctx, elems[i]);
    }
    anf_emit_raw(ctx, ")\n");
    return tmp;
}

static char *anf_lower_record_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr) {
    sp_dyn_array(char *) names = SP_NULLPTR;
    sp_dyn_array(char *) vals = SP_NULLPTR;
    sp_dyn_array_for(expr->data.elements, i) {
        fxsh_ast_node_t *f = expr->data.elements[i];
        if (!f || f->kind != AST_FIELD_ACCESS)
            continue;
        char *n = anf_alloc_sprintf("%.*s", f->data.field.field.len, f->data.field.field.data);
        char *v = anf_lower_to_atom(ctx, f->data.field.object);
        sp_dyn_array_push(names, n);
        sp_dyn_array_push(vals, v);
    }
    char *tmp = anf_fresh_tmp(ctx);
    anf_emit_indent(ctx);
    anf_emit_fmt(ctx, "let %s = record{", tmp);
    sp_dyn_array_for(names, i) {
        if (i > 0)
            anf_emit_raw(ctx, "; ");
        anf_emit_fmt(ctx, "%s = %s", names[i], vals[i]);
    }
    anf_emit_raw(ctx, "}\n");
    return tmp;
}

static char *anf_lower_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr) {
    if (!expr)
        return anf_alloc_sprintf("()");
    if (ast_is_atom(expr))
        return anf_expr_inline_text(expr);

    switch (expr->kind) {
        case AST_BINARY: {
            char *l = anf_lower_to_atom(ctx, expr->data.binary.left);
            char *r = anf_lower_to_atom(ctx, expr->data.binary.right);
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = prim(%s, %s, %s)\n", tmp,
                         fxsh_token_kind_name(expr->data.binary.op), l, r);
            return tmp;
        }
        case AST_UNARY: {
            char *x = anf_lower_to_atom(ctx, expr->data.unary.operand);
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = prim(%s, %s)\n", tmp,
                         fxsh_token_kind_name(expr->data.unary.op), x);
            return tmp;
        }
        case AST_CALL:
            return anf_lower_call_to_atom(ctx, expr);
        case AST_PIPE: {
            char *l = anf_lower_to_atom(ctx, expr->data.pipe.left);
            char *r = anf_lower_to_atom(ctx, expr->data.pipe.right);
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = pipe(%s, %s)\n", tmp, l, r);
            return tmp;
        }
        case AST_IF: {
            char *cond = anf_lower_to_atom(ctx, expr->data.if_expr.cond);
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = if %s then {\n", tmp, cond);
            ctx->indent++;
            char *ta = anf_lower_to_atom(ctx, expr->data.if_expr.then_branch);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "%s\n", ta);
            ctx->indent--;
            anf_emit_line(ctx, "} else {");
            ctx->indent++;
            char *ea = anf_lower_to_atom(ctx, expr->data.if_expr.else_branch);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "%s\n", ea);
            ctx->indent--;
            anf_emit_line(ctx, "}");
            return tmp;
        }
        case AST_MATCH:
            return anf_lower_match_to_atom(ctx, expr);
        case AST_LAMBDA:
            return anf_lower_lambda_to_atom(ctx, expr);
        case AST_LET_IN: {
            sp_dyn_array_for(expr->data.let_in.bindings, i) {
                fxsh_ast_node_t *b = expr->data.let_in.bindings[i];
                if (!b || (b->kind != AST_LET && b->kind != AST_DECL_LET))
                    continue;
                char *rhs = anf_lower_to_atom(ctx, b->data.let.value);
                anf_emit_indent(ctx);
                anf_emit_fmt(ctx, "let %.*s = %s\n", b->data.let.name.len, b->data.let.name.data,
                             rhs);
            }
            return anf_lower_to_atom(ctx, expr->data.let_in.body);
        }
        case AST_LET:
        case AST_DECL_LET: {
            char *rhs = anf_lower_to_atom(ctx, expr->data.let.value);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %.*s = %s\n", expr->data.let.name.len, expr->data.let.name.data,
                         rhs);
            return anf_alloc_sprintf("%.*s", expr->data.let.name.len, expr->data.let.name.data);
        }
        case AST_TUPLE:
            return anf_lower_tuple_or_list_to_atom(ctx, expr, "tuple");
        case AST_LIST:
            return anf_lower_tuple_or_list_to_atom(ctx, expr, "list");
        case AST_RECORD:
            return anf_lower_record_to_atom(ctx, expr);
        case AST_FIELD_ACCESS: {
            char *o = anf_lower_to_atom(ctx, expr->data.field.object);
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = field(%s.%.*s)\n", tmp, o, expr->data.field.field.len,
                         expr->data.field.field.data);
            return tmp;
        }
        case AST_CONSTR_APPL: {
            sp_dyn_array(char *) args = SP_NULLPTR;
            sp_dyn_array_for(expr->data.constr_appl.args, i) {
                char *a = anf_lower_to_atom(ctx, expr->data.constr_appl.args[i]);
                sp_dyn_array_push(args, a);
            }
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = constr(%.*s", tmp, expr->data.constr_appl.constr_name.len,
                         expr->data.constr_appl.constr_name.data);
            if (sp_dyn_array_size(args) == 0) {
                anf_emit_raw(ctx, ")\n");
                return tmp;
            }
            anf_emit_raw(ctx, ", ");
            sp_dyn_array_for(args, i) {
                if (i > 0)
                    anf_emit_raw(ctx, ", ");
                anf_emit_raw(ctx, args[i]);
            }
            anf_emit_raw(ctx, ")\n");
            return tmp;
        }
        default: {
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = <expr kind=%d>\n", tmp, (int)expr->kind);
            return tmp;
        }
    }
}

char *fxsh_anf_dump(fxsh_ast_node_t *ast) {
    anf_ctx_t ctx = {0};
    if (!ast)
        return NULL;

    anf_emit_line(&ctx, "; ANF IR");
    if (ast->kind == AST_PROGRAM) {
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *d = ast->data.decls[i];
            if (!d)
                continue;

            switch (d->kind) {
                case AST_DECL_LET:
                case AST_LET: {
                    char *rhs = anf_lower_to_atom(&ctx, d->data.let.value);
                    anf_emit_indent(&ctx);
                    anf_emit_fmt(&ctx, "let %.*s = %s\n", d->data.let.name.len,
                                 d->data.let.name.data, rhs);
                    break;
                }
                case AST_DECL_FN: {
                    anf_emit_indent(&ctx);
                    anf_emit_fmt(&ctx, "let %.*s = fun (", d->data.decl_fn.name.len,
                                 d->data.decl_fn.name.data);
                    sp_dyn_array_for(d->data.decl_fn.params, pi) {
                        char *p = anf_pattern_text(d->data.decl_fn.params[pi]);
                        if (pi > 0)
                            anf_emit_raw(&ctx, ", ");
                        anf_emit_raw(&ctx, p);
                    }
                    anf_emit_raw(&ctx, ") -> {\n");
                    ctx.indent++;
                    char *body = anf_lower_to_atom(&ctx, d->data.decl_fn.body);
                    anf_emit_indent(&ctx);
                    anf_emit_fmt(&ctx, "%s\n", body);
                    ctx.indent--;
                    anf_emit_line(&ctx, "}");
                    break;
                }
                case AST_DECL_IMPORT:
                    anf_emit_indent(&ctx);
                    anf_emit_fmt(&ctx, "; import %.*s\n", d->data.decl_import.module_name.len,
                                 d->data.decl_import.module_name.data);
                    break;
                case AST_TYPE_DEF:
                case AST_DECL_TYPE:
                    anf_emit_line(&ctx, "; type-def ...");
                    break;
                default: {
                    char *v = anf_lower_to_atom(&ctx, d);
                    anf_emit_indent(&ctx);
                    anf_emit_fmt(&ctx, "; expr %s\n", v);
                    break;
                }
            }
        }
    } else {
        char *v = anf_lower_to_atom(&ctx, ast);
        anf_emit_indent(&ctx);
        anf_emit_fmt(&ctx, "result %s\n", v);
    }

    sp_dyn_array_push(ctx.out, '\0');
    return (char *)ctx.out;
}
