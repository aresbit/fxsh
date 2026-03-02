/*
 * codegen.c - fxsh C code generation backend
 */

#include "fxsh.h"

#include <stdio.h>
#include <string.h>

/*=============================================================================
 * Code Generator State
 *=============================================================================*/

typedef struct {
    sp_dyn_array(c8) * output;
    u32 indent_level;
    u32 temp_var_counter;
    sp_ht(sp_str_t, sp_str_t) * name_map; /* fxsh name -> C name */
} codegen_ctx_t;

/*=============================================================================
 * Output Helpers
 *=============================================================================*/

static void emit_indent(codegen_ctx_t *ctx) {
    for (u32 i = 0; i < ctx->indent_level; i++) {
        sp_dyn_array_push(*ctx->output, ' ');
        sp_dyn_array_push(*ctx->output, ' ');
    }
}

static void emit_raw(codegen_ctx_t *ctx, const c8 *str) {
    for (u32 i = 0; str[i]; i++) {
        sp_dyn_array_push(*ctx->output, str[i]);
    }
}

static void emit_string(codegen_ctx_t *ctx, sp_str_t str) {
    for (u32 i = 0; i < str.len; i++) {
        sp_dyn_array_push(*ctx->output, str.data[i]);
    }
}

static void emit_line(codegen_ctx_t *ctx, const c8 *str) {
    emit_indent(ctx);
    emit_raw(ctx, str);
    sp_dyn_array_push(*ctx->output, '\n');
}

static void emit_type(codegen_ctx_t *ctx, fxsh_type_t *type) {
    if (!type) {
        emit_raw(ctx, "void");
        return;
    }

    switch (type->kind) {
        case TYPE_CON: {
            if (sp_str_equal(type->data.con, TYPE_INT)) {
                emit_raw(ctx, "s64");
            } else if (sp_str_equal(type->data.con, TYPE_FLOAT)) {
                emit_raw(ctx, "f64");
            } else if (sp_str_equal(type->data.con, TYPE_BOOL)) {
                emit_raw(ctx, "bool");
            } else if (sp_str_equal(type->data.con, TYPE_STRING)) {
                emit_raw(ctx, "sp_str_t");
            } else if (sp_str_equal(type->data.con, TYPE_UNIT)) {
                emit_raw(ctx, "void");
            } else {
                emit_string(ctx, type->data.con);
            }
            break;
        }
        case TYPE_ARROW: {
            /* Function pointer type */
            emit_raw(ctx, "fxsh_fn_");
            /* Use type hash for unique name */
            break;
        }
        case TYPE_VAR: {
            emit_raw(ctx, "/* 'a */ void*");
            break;
        }
        default:
            emit_raw(ctx, "void*");
            break;
    }
}

/*=============================================================================
 * Name Mangling
 *=============================================================================*/

static sp_str_t mangle_name(sp_str_t name) {
    /* Simple mangling: replace special chars with _ */
    sp_dyn_array(c8) result = SP_NULLPTR;
    for (u32 i = 0; i < name.len; i++) {
        c8 c = name.data[i];
        if (c == '\'' || c == '+' || c == '-' || c == '*' || c == '/') {
            sp_dyn_array_push(result, '_');
        } else {
            sp_dyn_array_push(result, c);
        }
    }
    sp_dyn_array_push(result, '\0');

    sp_str_t mangled = sp_str_from_cstr(result);
    sp_dyn_array_free(result);
    return mangled;
}

/*=============================================================================
 * Forward Declarations
 *=============================================================================*/

static void gen_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *ast);

/*=============================================================================
 * Expression Code Generation
 *=============================================================================*/

static void gen_literal(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    switch (ast->kind) {
        case AST_LIT_INT: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)ast->data.lit_int);
            emit_raw(ctx, buf);
            break;
        }
        case AST_LIT_FLOAT: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%f", ast->data.lit_float);
            emit_raw(ctx, buf);
            break;
        }
        case AST_LIT_BOOL: {
            emit_raw(ctx, ast->data.lit_bool ? "true" : "false");
            break;
        }
        case AST_LIT_STRING: {
            sp_dyn_array_push(*ctx->output, '"');
            for (u32 i = 0; i < ast->data.lit_string.len; i++) {
                c8 c = ast->data.lit_string.data[i];
                if (c == '"' || c == '\\') {
                    sp_dyn_array_push(*ctx->output, '\\');
                }
                sp_dyn_array_push(*ctx->output, c);
            }
            sp_dyn_array_push(*ctx->output, '"');
            break;
        }
        case AST_LIT_UNIT:
            emit_raw(ctx, "/* unit */ 0");
            break;
        default:
            break;
    }
}

static void gen_ident(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    sp_str_t mangled = mangle_name(ast->data.ident);
    emit_string(ctx, mangled);
}

static void gen_binary(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    const c8 *op = NULL;
    switch (ast->data.binary.op) {
        case TOK_PLUS:
            op = " + ";
            break;
        case TOK_MINUS:
            op = " - ";
            break;
        case TOK_STAR:
            op = " * ";
            break;
        case TOK_SLASH:
            op = " / ";
            break;
        case TOK_PERCENT:
            op = " % ";
            break;
        case TOK_EQ:
            op = " == ";
            break;
        case TOK_NEQ:
            op = " != ";
            break;
        case TOK_LT:
            op = " < ";
            break;
        case TOK_GT:
            op = " > ";
            break;
        case TOK_LEQ:
            op = " <= ";
            break;
        case TOK_GEQ:
            op = " >= ";
            break;
        case TOK_AND:
            op = " && ";
            break;
        case TOK_OR:
            op = " || ";
            break;
        default:
            op = " /* ? */ ";
            break;
    }

    sp_dyn_array_push(*ctx->output, '(');
    gen_expr(ctx, ast->data.binary.left);
    emit_raw(ctx, op);
    gen_expr(ctx, ast->data.binary.right);
    sp_dyn_array_push(*ctx->output, ')');
}

static void gen_unary(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    switch (ast->data.unary.op) {
        case TOK_MINUS:
            sp_dyn_array_push(*ctx->output, '-');
            sp_dyn_array_push(*ctx->output, '(');
            gen_expr(ctx, ast->data.unary.operand);
            sp_dyn_array_push(*ctx->output, ')');
            break;
        case TOK_NOT:
            sp_dyn_array_push(*ctx->output, '!');
            sp_dyn_array_push(*ctx->output, '(');
            gen_expr(ctx, ast->data.unary.operand);
            sp_dyn_array_push(*ctx->output, ')');
            break;
        default:
            gen_expr(ctx, ast->data.unary.operand);
            break;
    }
}

static void gen_if(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "(");
    gen_expr(ctx, ast->data.if_expr.cond);
    emit_raw(ctx, " ? ");
    gen_expr(ctx, ast->data.if_expr.then_branch);
    emit_raw(ctx, " : ");
    if (ast->data.if_expr.else_branch) {
        gen_expr(ctx, ast->data.if_expr.else_branch);
    } else {
        emit_raw(ctx, "0 /* no else */");
    }
    emit_raw(ctx, ")");
}

static void gen_call(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    gen_expr(ctx, ast->data.call.func);
    sp_dyn_array_push(*ctx->output, '(');
    sp_dyn_array_for(ast->data.call.args, i) {
        if (i > 0)
            emit_raw(ctx, ", ");
        gen_expr(ctx, ast->data.call.args[i]);
    }
    sp_dyn_array_push(*ctx->output, ')');
}

static void gen_lambda(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Generate function pointer - requires closure conversion */
    /* For now, emit as comment */
    emit_raw(ctx, "/* lambda: ");
    emit_raw(ctx, "fn");
    emit_raw(ctx, " */ NULL");
}

static void gen_let_in(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Generate as a block that declares variables and returns the body */
    emit_raw(ctx, "({\n");
    ctx->indent_level++;

    /* Generate let bindings as variable declarations */
    sp_dyn_array_for(ast->data.let_in.bindings, i) {
        fxsh_ast_node_t *binding = ast->data.let_in.bindings[i];
        if (binding->kind == AST_LET) {
            emit_indent(ctx);
            /* Infer type - for now emit as auto or specific type */
            emit_raw(ctx, "auto ");
            sp_str_t mangled = mangle_name(binding->data.let.name);
            emit_string(ctx, mangled);
            emit_raw(ctx, " = ");
            gen_expr(ctx, binding->data.let.value);
            emit_raw(ctx, ";\n");
        }
    }

    /* Generate body */
    emit_indent(ctx);
    gen_expr(ctx, ast->data.let_in.body);
    emit_raw(ctx, ";\n");

    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "})");
}

static void gen_pipe(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* a |> f = f a */
    gen_expr(ctx, ast->data.pipe.right);
    sp_dyn_array_push(*ctx->output, '(');
    gen_expr(ctx, ast->data.pipe.left);
    sp_dyn_array_push(*ctx->output, ')');
}

static void gen_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast) {
        emit_raw(ctx, "0 /* null */");
        return;
    }

    switch (ast->kind) {
        case AST_LIT_INT:
        case AST_LIT_FLOAT:
        case AST_LIT_STRING:
        case AST_LIT_BOOL:
        case AST_LIT_UNIT:
            gen_literal(ctx, ast);
            break;
        case AST_IDENT:
            gen_ident(ctx, ast);
            break;
        case AST_BINARY:
            gen_binary(ctx, ast);
            break;
        case AST_UNARY:
            gen_unary(ctx, ast);
            break;
        case AST_IF:
            gen_if(ctx, ast);
            break;
        case AST_CALL:
            gen_call(ctx, ast);
            break;
        case AST_LAMBDA:
            gen_lambda(ctx, ast);
            break;
        case AST_LET_IN:
            gen_let_in(ctx, ast);
            break;
        case AST_PIPE:
            gen_pipe(ctx, ast);
            break;
        default:
            emit_raw(ctx, "/* unimplemented */ 0");
            break;
    }
}

/*=============================================================================
 * Declaration Code Generation
 *=============================================================================*/

static void gen_decl_fn(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Generate function prototype */
    emit_indent(ctx);
    /* Return type - for now assume s64 or infer */
    emit_raw(ctx, "s64 ");

    /* Function name */
    sp_str_t mangled = mangle_name(ast->data.decl_fn.name);
    emit_string(ctx, mangled);

    /* Parameters */
    sp_dyn_array_push(*ctx->output, '(');
    sp_dyn_array_for(ast->data.decl_fn.params, i) {
        if (i > 0)
            emit_raw(ctx, ", ");
        /* Parameter type and name */
        fxsh_ast_node_t *param = ast->data.decl_fn.params[i];
        if (param->kind == AST_PAT_VAR) {
            emit_raw(ctx, "s64 "); /* Assume int for now */
            emit_string(ctx, param->data.ident);
        } else {
            emit_raw(ctx, "s64 arg");
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", i);
            emit_raw(ctx, buf);
        }
    }
    if (sp_dyn_array_size(ast->data.decl_fn.params) == 0) {
        emit_raw(ctx, "void");
    }
    emit_raw(ctx, ") {\n");

    /* Function body */
    ctx->indent_level++;
    emit_indent(ctx);
    emit_raw(ctx, "return ");
    gen_expr(ctx, ast->data.decl_fn.body);
    emit_raw(ctx, ";\n");
    ctx->indent_level--;

    emit_line(ctx, "}");
    sp_dyn_array_push(*ctx->output, '\n');
}

static void gen_decl_let(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Global variable declaration */
    emit_indent(ctx);
    emit_raw(ctx, "static s64 "); /* Assume int for now */
    sp_str_t mangled = mangle_name(ast->data.let.name);
    emit_string(ctx, mangled);
    emit_raw(ctx, " = ");
    gen_expr(ctx, ast->data.let.value);
    emit_raw(ctx, ";\n");
}

/*============================================================================-
 * Program Code Generation
 *=============================================================================*/

static void gen_program_prelude(codegen_ctx_t *ctx) {
    emit_line(ctx, "/* Generated by fxsh */");
    emit_line(ctx, "#include \"sp.h\"");
    emit_line(ctx, "#include <stdbool.h>");
    emit_line(ctx, "");
    emit_line(ctx, "/* Forward declarations */");
    emit_line(ctx, "");
}

static void gen_program_epilogue(codegen_ctx_t *ctx) {
    emit_line(ctx, "/* Main entry point */");
    emit_line(ctx, "int main(void) {");
    ctx->indent_level++;
    emit_line(ctx, "sp_init();");
    emit_indent(ctx);
    emit_raw(ctx, "return ");
    emit_raw(ctx, "0");
    emit_raw(ctx, ";\n");
    ctx->indent_level--;
    emit_line(ctx, "}");
}

/*=============================================================================
 * Public API
 *=============================================================================*/

char *fxsh_codegen(fxsh_ast_node_t *ast) {
    codegen_ctx_t ctx = {
        .output = sp_alloc(sizeof(sp_dyn_array(c8))),
        .indent_level = 0,
        .temp_var_counter = 0,
        .name_map = NULL,
    };

    /* Initialize output array */
    *ctx.output = SP_NULLPTR;

    gen_program_prelude(&ctx);

    if (ast && ast->kind == AST_PROGRAM) {
        /* First pass: forward declarations */
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *decl = ast->data.decls[i];
            if (decl->kind == AST_DECL_FN) {
                emit_indent(&ctx);
                emit_raw(&ctx, "s64 ");
                sp_str_t mangled = mangle_name(decl->data.decl_fn.name);
                emit_string(&ctx, mangled);
                emit_raw(&ctx, "(");
                if (sp_dyn_array_size(decl->data.decl_fn.params) == 0) {
                    emit_raw(&ctx, "void");
                }
                emit_raw(&ctx, ");\n");
            }
        }
        emit_raw(&ctx, "\n");

        /* Second pass: definitions */
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *decl = ast->data.decls[i];
            switch (decl->kind) {
                case AST_DECL_FN:
                    gen_decl_fn(&ctx, decl);
                    break;
                case AST_DECL_LET:
                    gen_decl_let(&ctx, decl);
                    break;
                default:
                    /* Expression at top level */
                    emit_indent(&ctx);
                    emit_raw(&ctx, "/* expr */\n");
                    break;
            }
        }
    } else if (ast) {
        /* Single expression - wrap in main */
        gen_program_epilogue(&ctx);
    }

    /* Null terminate */
    sp_dyn_array_push(*ctx.output, '\0');

    /* Copy to return buffer */
    u32 len = sp_dyn_array_size(*ctx.output);
    char *result = sp_alloc(len);
    memcpy(result, *ctx.output, len);

    sp_dyn_array_free(*ctx.output);
    sp_free(ctx.output);

    return result;
}

fxsh_error_t fxsh_codegen_to_file(fxsh_ast_node_t *ast, sp_str_t path) {
    char *code = fxsh_codegen(ast);
    if (!code) {
        return ERR_OUT_OF_MEMORY;
    }

    sp_str_t content = {
        .data = code,
        .len = strlen(code),
    };

    fxsh_error_t err = fxsh_write_file(path, content);
    sp_free(code);

    return err;
}
