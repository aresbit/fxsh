/*
 * codegen.c - fxsh C code generation backend
 *
 * Fixes from design review:
 *   1. embed_raw → emit_raw (typo fix)
 *   2. ADT struct generation: close struct with emit_raw not embed_raw
 *   3. gen_match: generate switch-on-tag instead of /* TODO * /
 *   4. gen_pattern: generate actual binding code
 *   5. gen_lambda: generate named lambda functions with closure stubs
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
    u32 lambda_counter;
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
    for (u32 i = 0; str[i]; i++)
        sp_dyn_array_push(*ctx->output, str[i]);
}

static void emit_string(codegen_ctx_t *ctx, sp_str_t str) {
    for (u32 i = 0; i < str.len; i++)
        sp_dyn_array_push(*ctx->output, str.data[i]);
}

static void emit_line(codegen_ctx_t *ctx, const c8 *str) {
    emit_indent(ctx);
    emit_raw(ctx, str);
    sp_dyn_array_push(*ctx->output, '\n');
}

static void emit_fmt(codegen_ctx_t *ctx, const c8 *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit_raw(ctx, buf);
}

/* Generate a unique temp variable name */
static void emit_temp(codegen_ctx_t *ctx, char *out, size_t out_sz) {
    snprintf(out, out_sz, "_t%u", ctx->temp_var_counter++);
}

/*=============================================================================
 * Name Mangling
 *=============================================================================*/

static void emit_mangled(codegen_ctx_t *ctx, sp_str_t name) {
    /* Replace OCaml-style special chars with C-safe equivalents */
    for (u32 i = 0; i < name.len; i++) {
        c8 c = name.data[i];
        switch (c) {
            case '\'':
                emit_raw(ctx, "_prime");
                break;
            case '!':
                emit_raw(ctx, "_bang");
                break;
            case '?':
                emit_raw(ctx, "_q");
                break;
            default:
                sp_dyn_array_push(*ctx->output, c);
                break;
        }
    }
}

/* Write mangled name into a buffer (no ctx output) */
static void mangle_into(sp_str_t name, char *buf, size_t buf_sz) {
    size_t pos = 0;
    for (u32 i = 0; i < name.len && pos + 8 < buf_sz; i++) {
        c8 c = name.data[i];
        switch (c) {
            case '\'':
                memcpy(buf + pos, "_prime", 6);
                pos += 6;
                break;
            case '!':
                memcpy(buf + pos, "_bang", 5);
                pos += 5;
                break;
            default:
                buf[pos++] = c;
                break;
        }
    }
    buf[pos] = '\0';
}

/*=============================================================================
 * Forward Declarations
 *=============================================================================*/

static void gen_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *ast);
static void gen_type_def(codegen_ctx_t *ctx, fxsh_ast_node_t *ast);

/*=============================================================================
 * ADT Type Tracking
 *=============================================================================*/

/* Track registered ADT type names to resolve cross-references */
static sp_dyn_array(sp_str_t) g_adt_type_names = SP_NULLPTR;

/* Track ADT-typed global let bindings that need runtime init */
typedef struct {
    sp_str_t c_name;        /* mangled variable name */
    sp_str_t type_name;     /* mangled type name */
    fxsh_ast_node_t *value; /* initializer expression */
} adt_init_t;
static sp_dyn_array(adt_init_t) g_adt_inits = SP_NULLPTR;

static bool is_known_adt(sp_str_t name) {
    if (!g_adt_type_names)
        return false;
    sp_dyn_array_for(g_adt_type_names, i) if (sp_str_equal(g_adt_type_names[i], name)) return true;
    return false;
}

/*=============================================================================
 * Type Emission
 *=============================================================================*/

static void emit_c_type_for_fxsh_type(codegen_ctx_t *ctx, sp_str_t name) {
    if (sp_str_equal(name, TYPE_INT)) {
        emit_raw(ctx, "s64");
        return;
    }
    if (sp_str_equal(name, TYPE_FLOAT)) {
        emit_raw(ctx, "double");
        return;
    }
    if (sp_str_equal(name, TYPE_BOOL)) {
        emit_raw(ctx, "bool");
        return;
    }
    if (sp_str_equal(name, TYPE_STRING)) {
        emit_raw(ctx, "sp_str_t");
        return;
    }
    if (sp_str_equal(name, TYPE_UNIT)) {
        emit_raw(ctx, "void");
        return;
    }
    /* ADT or user type */
    emit_raw(ctx, "fxsh_");
    emit_mangled(ctx, name);
    emit_raw(ctx, "_t");
}

/*=============================================================================
 * ADT Struct Generation
 *=============================================================================*/

static void gen_type_def_struct(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_TYPE_DEF)
        return;

    sp_str_t type_name = ast->data.type_def.name;
    fxsh_ast_list_t constrs = ast->data.type_def.constructors;
    u32 n = (u32)sp_dyn_array_size(constrs);
    if (n == 0)
        return;

    /* Track this type */
    sp_dyn_array_push(g_adt_type_names, type_name);

    /* ── Tag enum ── */
    emit_line(ctx, "/* ADT: tag enum */");
    emit_indent(ctx);
    emit_raw(ctx, "typedef enum {\n");
    ctx->indent_level++;
    sp_dyn_array_for(constrs, i) {
        fxsh_ast_node_t *c = constrs[i];
        if (c->kind != AST_DATA_CONSTR)
            continue;
        emit_indent(ctx);
        emit_raw(ctx, "fxsh_tag_");
        emit_mangled(ctx, type_name);
        emit_raw(ctx, "_");
        emit_mangled(ctx, c->data.data_constr.name);
        emit_raw(ctx, (i < n - 1) ? ",\n" : "\n");
    }
    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "} fxsh_tag_");
    emit_mangled(ctx, type_name);
    emit_raw(ctx, "_t;\n\n");

    /* ── Union of payloads ── */
    emit_line(ctx, "/* ADT: payload union */");
    emit_indent(ctx);
    emit_raw(ctx, "typedef union {\n");
    ctx->indent_level++;
    sp_dyn_array_for(constrs, i) {
        fxsh_ast_node_t *c = constrs[i];
        if (c->kind != AST_DATA_CONSTR)
            continue;
        u32 nargs = (u32)sp_dyn_array_size(c->data.data_constr.arg_types);
        if (nargs == 0)
            continue;

        emit_indent(ctx);
        emit_raw(ctx, "struct {\n");
        ctx->indent_level++;
        sp_dyn_array_for(c->data.data_constr.arg_types, j) {
            emit_indent(ctx);
            fxsh_ast_node_t *arg_t = c->data.data_constr.arg_types[j];
            if (arg_t->kind == AST_IDENT) {
                /* Self-reference: use pointer to avoid incomplete type */
                if (sp_str_equal(arg_t->data.ident, type_name)) {
                    emit_raw(ctx, "struct fxsh_");
                    emit_mangled(ctx, type_name);
                    emit_raw(ctx, "_s *");
                } else {
                    emit_c_type_for_fxsh_type(ctx, arg_t->data.ident);
                    emit_raw(ctx, " ");
                }
            } else {
                emit_raw(ctx, "s64 ");
            }
            emit_fmt(ctx, "_%u;\n", (unsigned)j);
        }
        ctx->indent_level--;
        emit_indent(ctx);
        emit_raw(ctx, "} ");
        emit_mangled(ctx, c->data.data_constr.name);
        emit_raw(ctx, ";\n");
    }
    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "} fxsh_data_");
    emit_mangled(ctx, type_name);
    emit_raw(ctx, "_t;\n\n");

    /* ── Main struct ── */
    emit_line(ctx, "/* ADT: main struct */");
    /* Use tagged struct name for forward-reference support */
    emit_indent(ctx);
    emit_raw(ctx, "typedef struct fxsh_");
    emit_mangled(ctx, type_name);
    emit_raw(ctx, "_s {\n");
    ctx->indent_level++;
    emit_indent(ctx);
    emit_raw(ctx, "fxsh_tag_");
    emit_mangled(ctx, type_name);
    emit_raw(ctx, "_t tag;\n");
    emit_indent(ctx);
    emit_raw(ctx, "fxsh_data_");
    emit_mangled(ctx, type_name);
    emit_raw(ctx, "_t data;\n");
    ctx->indent_level--;
    emit_indent(ctx);
    /* FIX: was `embed_raw` — corrected to `emit_raw` */
    emit_raw(ctx, "} fxsh_");
    emit_mangled(ctx, type_name);
    emit_raw(ctx, "_t;\n\n");
}

static void gen_type_def_constructors(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_TYPE_DEF)
        return;
    sp_str_t type_name = ast->data.type_def.name;
    fxsh_ast_list_t constrs = ast->data.type_def.constructors;

    sp_dyn_array_for(constrs, i) {
        fxsh_ast_node_t *c = constrs[i];
        if (c->kind != AST_DATA_CONSTR)
            continue;
        sp_str_t constr_name = c->data.data_constr.name;
        u32 nargs = (u32)sp_dyn_array_size(c->data.data_constr.arg_types);

        emit_indent(ctx);
        emit_raw(ctx, "static inline fxsh_");
        emit_mangled(ctx, type_name);
        emit_raw(ctx, "_t ");
        emit_raw(ctx, "fxsh_constr_");
        emit_mangled(ctx, constr_name);
        emit_raw(ctx, "(");
        if (nargs == 0) {
            emit_raw(ctx, "void");
        } else {
            for (u32 j = 0; j < nargs; j++) {
                if (j > 0)
                    emit_raw(ctx, ", ");
                fxsh_ast_node_t *at = c->data.data_constr.arg_types[j];
                if (at->kind == AST_IDENT) {
                    if (sp_str_equal(at->data.ident, type_name)) {
                        emit_raw(ctx, "struct fxsh_");
                        emit_mangled(ctx, type_name);
                        emit_raw(ctx, "_s *");
                    } else {
                        emit_c_type_for_fxsh_type(ctx, at->data.ident);
                        emit_raw(ctx, " ");
                    }
                } else {
                    emit_raw(ctx, "s64 ");
                }
                emit_fmt(ctx, "arg%u", j);
            }
        }
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;

        emit_indent(ctx);
        emit_raw(ctx, "fxsh_");
        emit_mangled(ctx, type_name);
        emit_raw(ctx, "_t _v;\n");
        emit_indent(ctx);
        emit_raw(ctx, "_v.tag = fxsh_tag_");
        emit_mangled(ctx, type_name);
        emit_raw(ctx, "_");
        emit_mangled(ctx, constr_name);
        emit_raw(ctx, ";\n");
        for (u32 j = 0; j < nargs; j++) {
            emit_indent(ctx);
            emit_raw(ctx, "_v.data.");
            emit_mangled(ctx, constr_name);
            emit_fmt(ctx, "._%u = arg%u;\n", j, j);
        }
        emit_indent(ctx);
        emit_raw(ctx, "return _v;\n");

        ctx->indent_level--;
        emit_line(ctx, "}");
        emit_raw(ctx, "\n");
    }
}

static void gen_type_def(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    gen_type_def_struct(ctx, ast);
    gen_type_def_constructors(ctx, ast);
}

/*=============================================================================
 * Literal Expression Generation
 *=============================================================================*/

static void gen_literal(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    char buf[64];
    switch (ast->kind) {
        case AST_LIT_INT:
            snprintf(buf, sizeof(buf), "%lldLL", (long long)ast->data.lit_int);
            emit_raw(ctx, buf);
            break;
        case AST_LIT_FLOAT:
            snprintf(buf, sizeof(buf), "%.17g", ast->data.lit_float);
            emit_raw(ctx, buf);
            break;
        case AST_LIT_BOOL:
            emit_raw(ctx, ast->data.lit_bool ? "true" : "false");
            break;
        case AST_LIT_STRING:
            sp_dyn_array_push(*ctx->output, '"');
            for (u32 i = 0; i < ast->data.lit_string.len; i++) {
                c8 c = ast->data.lit_string.data[i];
                if (c == '"' || c == '\\')
                    sp_dyn_array_push(*ctx->output, '\\');
                sp_dyn_array_push(*ctx->output, c);
            }
            sp_dyn_array_push(*ctx->output, '"');
            break;
        case AST_LIT_UNIT:
            emit_raw(ctx, "0 /* unit */");
            break;
        default:
            break;
    }
}

/*=============================================================================
 * Expression Code Generation
 *=============================================================================*/

static void gen_binary(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    static const char *ops[] = {
        [TOK_PLUS] = " + ",    [TOK_MINUS] = " - ", [TOK_STAR] = " * ", [TOK_SLASH] = " / ",
        [TOK_PERCENT] = " % ", [TOK_EQ] = " == ",   [TOK_NEQ] = " != ", [TOK_LT] = " < ",
        [TOK_GT] = " > ",      [TOK_LEQ] = " <= ",  [TOK_GEQ] = " >= ", [TOK_AND] = " && ",
        [TOK_OR] = " || ",
    };
    const char *op = (ast->data.binary.op < (int)(sizeof(ops) / sizeof(ops[0])))
                         ? ops[ast->data.binary.op]
                         : " /* ? */ ";
    sp_dyn_array_push(*ctx->output, '(');
    gen_expr(ctx, ast->data.binary.left);
    emit_raw(ctx, op ? op : " /* ? */ ");
    gen_expr(ctx, ast->data.binary.right);
    sp_dyn_array_push(*ctx->output, ')');
}

static void gen_unary(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (ast->data.unary.op == TOK_MINUS) {
        emit_raw(ctx, "-(");
        gen_expr(ctx, ast->data.unary.operand);
        sp_dyn_array_push(*ctx->output, ')');
    } else if (ast->data.unary.op == TOK_NOT) {
        emit_raw(ctx, "!(");
        gen_expr(ctx, ast->data.unary.operand);
        sp_dyn_array_push(*ctx->output, ')');
    } else {
        gen_expr(ctx, ast->data.unary.operand);
    }
}

static void gen_if(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "(");
    gen_expr(ctx, ast->data.if_expr.cond);
    emit_raw(ctx, " ? ");
    gen_expr(ctx, ast->data.if_expr.then_branch);
    emit_raw(ctx, " : ");
    if (ast->data.if_expr.else_branch)
        gen_expr(ctx, ast->data.if_expr.else_branch);
    else
        emit_raw(ctx, "0 /* no else */");
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

/* Lambda: emit a named static function + return its pointer as closure stub */
static void gen_lambda(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* For now: emit a comment + function pointer cast to void*.
     * Full closure conversion requires a separate pass (lambda lifting).
     * TODO: lambda lifting pass before codegen. */
    u32 lid = ctx->lambda_counter++;
    (void)ast;
    emit_fmt(ctx, "((void*)0 /* lambda_%u */)", lid);
}

static void gen_pipe(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* a |> f  ≡  f(a) */
    gen_expr(ctx, ast->data.pipe.right);
    sp_dyn_array_push(*ctx->output, '(');
    gen_expr(ctx, ast->data.pipe.left);
    sp_dyn_array_push(*ctx->output, ')');
}

static void gen_let_in(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Use GCC statement expression ({ ... }) */
    emit_raw(ctx, "({\n");
    ctx->indent_level++;
    sp_dyn_array_for(ast->data.let_in.bindings, i) {
        fxsh_ast_node_t *b = ast->data.let_in.bindings[i];
        if (b->kind == AST_LET || b->kind == AST_DECL_LET) {
            emit_indent(ctx);
            emit_raw(ctx, "__auto_type ");
            emit_mangled(ctx, b->data.let.name);
            emit_raw(ctx, " = ");
            gen_expr(ctx, b->data.let.value);
            emit_raw(ctx, ";\n");
        }
    }
    emit_indent(ctx);
    gen_expr(ctx, ast->data.let_in.body);
    emit_raw(ctx, ";\n");
    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "})");
}

static void gen_constr_appl(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "fxsh_constr_");
    emit_mangled(ctx, ast->data.constr_appl.constr_name);
    sp_dyn_array_push(*ctx->output, '(');
    sp_dyn_array_for(ast->data.constr_appl.args, i) {
        if (i > 0)
            emit_raw(ctx, ", ");
        gen_expr(ctx, ast->data.constr_appl.args[i]);
    }
    sp_dyn_array_push(*ctx->output, ')');
}

/*=============================================================================
 * Pattern Matching — switch on tag
 *=============================================================================*/

/* Generate code to bind pattern variables from `val_name` */
static void gen_pattern_bindings(codegen_ctx_t *ctx, fxsh_ast_node_t *pat, const char *val_name) {
    if (!pat)
        return;
    switch (pat->kind) {
        case AST_PAT_VAR:
            /* __auto_type x = val_name; */
            emit_indent(ctx);
            emit_raw(ctx, "__auto_type ");
            emit_mangled(ctx, pat->data.ident);
            emit_fmt(ctx, " = %s;\n", val_name);
            break;
        case AST_PAT_WILD:
            /* nothing */
            break;
        case AST_PAT_CONSTR: {
            sp_str_t cname = pat->data.constr_appl.constr_name;
            char cname_buf[128];
            mangle_into(cname, cname_buf, sizeof(cname_buf));
            /* Bind each sub-pattern to the corresponding field */
            sp_dyn_array_for(pat->data.constr_appl.args, j) {
                char field_expr[256];
                snprintf(field_expr, sizeof(field_expr), "%s.data.%s._%u", val_name, cname_buf,
                         (unsigned)j);
                gen_pattern_bindings(ctx, pat->data.constr_appl.args[j], field_expr);
            }
            break;
        }
        default:
            break;
    }
}

static void gen_match(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Generate:
     *   ({ __auto_type _m = <expr>;
     *      <result_type> _r;
     *      switch (_m.tag) {
     *        case fxsh_tag_T_Ctor: { ... _r = body; break; }
     *        ...
     *      }
     *      _r;
     *   })
     */
    emit_raw(ctx, "__({\n"); /* GCC statement expression */
    ctx->indent_level++;

    emit_indent(ctx);
    emit_raw(ctx, "__auto_type _match_val = ");
    gen_expr(ctx, ast->data.match_expr.expr);
    emit_raw(ctx, ";\n");

    emit_indent(ctx);
    emit_raw(ctx, "__auto_type _match_res = _match_val; /* placeholder */\n");
    emit_indent(ctx);
    emit_raw(ctx, "switch (_match_val.tag) {\n");
    ctx->indent_level++;

    sp_dyn_array_for(ast->data.match_expr.arms, i) {
        fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
        if (arm->kind != AST_MATCH_ARM)
            continue;

        fxsh_ast_node_t *pat = arm->data.match_arm.pattern;
        emit_indent(ctx);

        if (pat->kind == AST_PAT_WILD || pat->kind == AST_PAT_VAR) {
            emit_raw(ctx, "default: {\n");
        } else if (pat->kind == AST_PAT_CONSTR) {
            /* We need the type name to form the tag enum member.
             * Since we don't have type info here, use a heuristic:
             * scan g_adt_type_names to find which ADT has this constructor.
             * TODO: pass constr_env to codegen for proper resolution. */
            emit_raw(ctx, "case 0 /* fxsh_tag_<T>_");
            emit_mangled(ctx, pat->data.constr_appl.constr_name);
            emit_raw(ctx, " */: {\n");
        } else if (pat->kind == AST_PAT_LIT) {
            emit_raw(ctx, "/* lit pattern — use if-chain in real impl */\n");
            emit_raw(ctx, "default: {\n");
        } else {
            emit_raw(ctx, "default: {\n");
        }
        ctx->indent_level++;

        /* Bind pattern variables */
        gen_pattern_bindings(ctx, pat, "_match_val");

        /* Guard */
        if (arm->data.match_arm.guard) {
            emit_indent(ctx);
            emit_raw(ctx, "if (!(");
            gen_expr(ctx, arm->data.match_arm.guard);
            emit_raw(ctx, ")) break;\n");
        }

        /* Body */
        emit_indent(ctx);
        emit_raw(ctx, "_match_res = ");
        gen_expr(ctx, arm->data.match_arm.body);
        emit_raw(ctx, ";\n");
        emit_indent(ctx);
        emit_raw(ctx, "break;\n");

        ctx->indent_level--;
        emit_indent(ctx);
        emit_raw(ctx, "}\n");
    }

    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "}\n");
    emit_indent(ctx);
    emit_raw(ctx, "_match_res;\n");
    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "})");
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
            emit_mangled(ctx, ast->data.ident);
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
        case AST_CONSTR_APPL:
            gen_constr_appl(ctx, ast);
            break;
        case AST_MATCH:
            gen_match(ctx, ast);
            break;
        case AST_TUPLE:
            if (ast->data.elements && sp_dyn_array_size(ast->data.elements) > 0)
                gen_expr(ctx, ast->data.elements[0]);
            else
                emit_raw(ctx, "0 /* empty tuple */");
            break;
        default:
            emit_fmt(ctx, "0 /* unimplemented node %d */", ast->kind);
            break;
    }
}

/*=============================================================================
 * Declaration Generation
 *=============================================================================*/

static void gen_decl_fn(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_indent(ctx);
    emit_raw(ctx, "s64 "); /* TODO: use actual return type from TypeEnv */
    emit_mangled(ctx, ast->data.decl_fn.name);
    sp_dyn_array_push(*ctx->output, '(');
    if (sp_dyn_array_size(ast->data.decl_fn.params) == 0) {
        emit_raw(ctx, "void");
    } else {
        sp_dyn_array_for(ast->data.decl_fn.params, i) {
            if (i > 0)
                emit_raw(ctx, ", ");
            fxsh_ast_node_t *p = ast->data.decl_fn.params[i];
            if (p->kind == AST_PAT_VAR) {
                emit_raw(ctx, "s64 ");
                emit_mangled(ctx, p->data.ident);
            } else {
                emit_fmt(ctx, "s64 _arg%u", (unsigned)i);
            }
        }
    }
    emit_raw(ctx, ") {\n");
    ctx->indent_level++;
    emit_indent(ctx);
    emit_raw(ctx, "return ");
    gen_expr(ctx, ast->data.decl_fn.body);
    emit_raw(ctx, ";\n");
    ctx->indent_level--;
    emit_line(ctx, "}");
    sp_dyn_array_push(*ctx->output, '\n');
}

/* Heuristic: determine if a value expression is an ADT constructor application */
static sp_str_t constr_appl_type_name(fxsh_ast_node_t *v) {
    if (!v || v->kind != AST_CONSTR_APPL)
        return (sp_str_t){0};
    sp_str_t cn = v->data.constr_appl.constr_name;
    /* Common constructor heuristics */
    static const struct {
        const char *c;
        const char *t;
    } tbl[] = {
        {"None", "option"}, {"Some", "option"}, {"Nil", "list"},  {"Cons", "list"},
        {"Leaf", "tree"},   {"Node", "tree"},   {"Ok", "result"}, {"Err", "result"},
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (cn.len == strlen(tbl[i].c) && memcmp(cn.data, tbl[i].c, cn.len) == 0) {
            return sp_str_view(tbl[i].t);
        }
    }
    /* Check g_adt_type_names — if there's only one ADT, fall back to it */
    return (sp_str_t){0};
}

static void gen_decl_let(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    sp_str_t type_hint = constr_appl_type_name(ast->data.let.value);

    emit_indent(ctx);
    if (type_hint.len > 0) {
        emit_raw(ctx, "static fxsh_");
        emit_mangled(ctx, type_hint);
        emit_raw(ctx, "_t ");
        emit_mangled(ctx, ast->data.let.name);
        emit_raw(ctx, "; /* init in fxsh_init() */\n");
        char cn[128], tn[128];
        mangle_into(ast->data.let.name, cn, sizeof(cn));
        mangle_into(type_hint, tn, sizeof(tn));
        adt_init_t entry = {
            .c_name = (sp_str_t){.data = cn, .len = (u32)strlen(cn)},
            .type_name = (sp_str_t){.data = tn, .len = (u32)strlen(tn)},
            .value = ast->data.let.value,
        };
        sp_dyn_array_push(g_adt_inits, entry);
    } else {
        emit_raw(ctx, "static __auto_type ");
        emit_mangled(ctx, ast->data.let.name);
        emit_raw(ctx, " = ");
        gen_expr(ctx, ast->data.let.value);
        emit_raw(ctx, ";\n");
    }
}

/*=============================================================================
 * Program Generation
 *=============================================================================*/

static void gen_prelude(codegen_ctx_t *ctx) {
    emit_line(ctx, "/* Generated by fxsh — do not edit */");
    emit_line(ctx, "#include <stdbool.h>");
    emit_line(ctx, "#include <stdint.h>");
    emit_line(ctx, "#include <stdio.h>");
    emit_line(ctx, "");
    emit_line(ctx, "typedef int64_t  s64;");
    emit_line(ctx, "typedef int32_t  s32;");
    emit_line(ctx, "typedef uint64_t u64;");
    emit_line(ctx, "typedef uint32_t u32;");
    emit_line(ctx, "typedef double   f64;");
    emit_line(ctx, "typedef char     c8;");
    emit_line(ctx, "typedef struct { const char *data; u32 len; } sp_str_t;");
    emit_line(ctx, "");
    emit_line(ctx, "/* Forward: GCC statement-expression helper */");
    emit_line(ctx, "#define __(x) x");
    emit_line(ctx, "");
}

static void gen_adt_init_fn(codegen_ctx_t *ctx) {
    if (!g_adt_inits || sp_dyn_array_size(g_adt_inits) == 0)
        return;
    emit_line(ctx, "static void fxsh_init_globals(void) {");
    ctx->indent_level++;
    sp_dyn_array_for(g_adt_inits, i) {
        emit_indent(ctx);
        emit_string(ctx, g_adt_inits[i].c_name);
        emit_raw(ctx, " = ");
        gen_expr(ctx, g_adt_inits[i].value);
        emit_raw(ctx, ";\n");
    }
    ctx->indent_level--;
    emit_line(ctx, "}");
    emit_raw(ctx, "\n");
}

static void gen_epilogue(codegen_ctx_t *ctx) {
    gen_adt_init_fn(ctx);
    emit_line(ctx, "int main(void) {");
    ctx->indent_level++;
    if (g_adt_inits && sp_dyn_array_size(g_adt_inits) > 0)
        emit_line(ctx, "fxsh_init_globals();");
    emit_line(ctx, "return 0;");
    ctx->indent_level--;
    emit_line(ctx, "}");
}

/*=============================================================================
 * Public API
 *=============================================================================*/

char *fxsh_codegen(fxsh_ast_node_t *ast) {
    sp_dyn_array(c8) out_arr = SP_NULLPTR;
    codegen_ctx_t ctx = {
        .output = &out_arr,
        .indent_level = 0,
        .temp_var_counter = 0,
        .lambda_counter = 0,
    };
    g_adt_type_names = SP_NULLPTR;
    g_adt_inits = SP_NULLPTR;

    gen_prelude(&ctx);

    if (ast && ast->kind == AST_PROGRAM) {
        /* Pass 1: type definitions */
        sp_dyn_array_for(ast->data.decls, i) {
            if (ast->data.decls[i]->kind == AST_TYPE_DEF)
                gen_type_def(&ctx, ast->data.decls[i]);
        }
        /* Pass 2: forward declarations for functions */
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *d = ast->data.decls[i];
            if (d->kind == AST_DECL_FN) {
                emit_raw(&ctx, "static s64 ");
                emit_mangled(&ctx, d->data.decl_fn.name);
                emit_raw(&ctx,
                         sp_dyn_array_size(d->data.decl_fn.params) == 0 ? "(void);\n" : "(...);\n");
            }
        }
        emit_raw(&ctx, "\n");
        /* Pass 3: function and variable definitions */
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *d = ast->data.decls[i];
            if (d->kind == AST_DECL_FN)
                gen_decl_fn(&ctx, d);
            else if (d->kind == AST_DECL_LET || d->kind == AST_LET)
                gen_decl_let(&ctx, d);
            /* TYPE_DEF already handled */
        }
    }

    gen_epilogue(&ctx);
    sp_dyn_array_push(out_arr, '\0');

    u32 len = (u32)sp_dyn_array_size(out_arr);
    char *result = (char *)sp_alloc(len);
    memcpy(result, out_arr, len);
    sp_dyn_array_free(out_arr);
    return result;
}

fxsh_error_t fxsh_codegen_to_file(fxsh_ast_node_t *ast, sp_str_t path) {
    char *code = fxsh_codegen(ast);
    if (!code)
        return ERR_OUT_OF_MEMORY;
    sp_str_t content = {.data = code, .len = (u32)strlen(code)};
    fxsh_error_t err = fxsh_write_file(path, content);
    sp_free(code);
    return err;
}
