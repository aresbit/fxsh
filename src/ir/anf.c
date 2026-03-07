/*
 * anf.c - AST -> ANF IR textual lowering (MVP)
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
        default:
            return false;
    }
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

static char *anf_atom_text(fxsh_ast_node_t *e) {
    if (!e)
        return anf_alloc_sprintf("()");
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
        case AST_LIT_STRING: {
            char *tmp = (char *)fxsh_alloc0((size_t)e->data.lit_string.len * 4 + 8);
            size_t p = 0;
            tmp[p++] = '"';
            for (u32 i = 0; i < e->data.lit_string.len; i++) {
                unsigned char c = (unsigned char)e->data.lit_string.data[i];
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
        default:
            return anf_alloc_sprintf("<non-atom>");
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
    char *tmp = anf_fresh_tmp(ctx);
    anf_emit_indent(ctx);
    anf_emit_fmt(ctx, "let %s = call %s(", tmp, fn_atom);
    for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
        char *a = anf_lower_to_atom(ctx, flat_args[i]);
        if (i != (s32)sp_dyn_array_size(flat_args) - 1)
            anf_emit_raw(ctx, ", ");
        anf_emit_raw(ctx, a);
    }
    anf_emit_raw(ctx, ")\n");
    return tmp;
}

static char *anf_lower_to_atom(anf_ctx_t *ctx, fxsh_ast_node_t *expr) {
    if (!expr)
        return anf_alloc_sprintf("()");
    if (ast_is_atom(expr))
        return anf_atom_text(expr);

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
        case AST_IF: {
            char *cond = anf_lower_to_atom(ctx, expr->data.if_expr.cond);
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = if %s then {\n", tmp, cond);
            ctx->indent++;
            char *ta = anf_lower_to_atom(ctx, expr->data.if_expr.then_branch);
            anf_emit_indent(ctx);
            anf_emit_raw(ctx, ta);
            anf_emit_raw(ctx, "\n");
            ctx->indent--;
            anf_emit_indent(ctx);
            anf_emit_raw(ctx, "} else {\n");
            ctx->indent++;
            char *ea = anf_lower_to_atom(ctx, expr->data.if_expr.else_branch);
            anf_emit_indent(ctx);
            anf_emit_raw(ctx, ea);
            anf_emit_raw(ctx, "\n");
            ctx->indent--;
            anf_emit_indent(ctx);
            anf_emit_raw(ctx, "}\n");
            return tmp;
        }
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
        case AST_MATCH: {
            char *v = anf_lower_to_atom(ctx, expr->data.match_expr.expr);
            char *tmp = anf_fresh_tmp(ctx);
            anf_emit_indent(ctx);
            anf_emit_fmt(ctx, "let %s = match %s with ...\n", tmp, v);
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

    anf_emit_line(&ctx, "; ANF IR (MVP)");
    if (ast->kind == AST_PROGRAM) {
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *d = ast->data.decls[i];
            if (!d)
                continue;
            if (d->kind == AST_DECL_LET || d->kind == AST_LET) {
                char *rhs = anf_lower_to_atom(&ctx, d->data.let.value);
                anf_emit_indent(&ctx);
                anf_emit_fmt(&ctx, "let %.*s = %s\n", d->data.let.name.len, d->data.let.name.data,
                             rhs);
            } else {
                char *v = anf_lower_to_atom(&ctx, d);
                anf_emit_indent(&ctx);
                anf_emit_fmt(&ctx, "; expr %s\n", v);
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
