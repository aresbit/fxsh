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

static sp_str_t make_owned_str(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)fxsh_alloc0(n + 1);
    memcpy(p, s, n);
    return sp_str_view(p);
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
static void emit_c_type_for_fxsh_type(codegen_ctx_t *ctx, sp_str_t name);
static void emit_closure_type_name(codegen_ctx_t *ctx, sp_str_t root_fn, u32 stage);
static bool closure_expr_stage_of(fxsh_ast_node_t *expr, sp_str_t *out_root, u32 *out_stage);
static void emit_c_type_guess_for_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *expr);

/*=============================================================================
 * ADT Type Tracking
 *=============================================================================*/

/* Track registered ADT type names to resolve cross-references */
static sp_dyn_array(sp_str_t) g_adt_type_names = SP_NULLPTR;

typedef struct {
    sp_str_t constr_name;
    sp_str_t type_name;
} adt_constr_entry_t;
static sp_dyn_array(adt_constr_entry_t) g_adt_constrs = SP_NULLPTR;
static sp_dyn_array(sp_str_t) g_lambda_fn_names = SP_NULLPTR;
static sp_dyn_array(sp_str_t) g_closure_fn_names = SP_NULLPTR;
static sp_dyn_array(sp_str_t) g_decl_fn_names = SP_NULLPTR;
static fxsh_type_env_t g_codegen_type_env = SP_NULLPTR;
static u32 g_top_let_counter = 0;

typedef struct {
    sp_str_t name;
    sp_str_t c_base;
    u32 outer_arity;
    sp_dyn_array(u32) stage_arities; /* stages after outer application */
} closure_info_t;
static sp_dyn_array(closure_info_t) g_closure_infos = SP_NULLPTR;

typedef struct {
    sp_str_t name;
    sp_str_t root_fn;
    u32 stage; /* 1..N for closure stages */
} closure_value_info_t;
static sp_dyn_array(closure_value_info_t) g_closure_values = SP_NULLPTR;

typedef struct {
    sp_str_t src_name;
    sp_str_t c_expr;
} sym_entry_t;
static sp_dyn_array(sym_entry_t) g_sym_stack = SP_NULLPTR;

/* Track ADT-typed global let bindings that need runtime init */
typedef struct {
    sp_str_t c_name;        /* mangled variable name */
    sp_str_t type_name;     /* mangled type name */
    fxsh_ast_node_t *value; /* initializer expression */
} adt_init_t;
static sp_dyn_array(adt_init_t) g_adt_inits = SP_NULLPTR;

typedef struct {
    sp_str_t c_name;
    sp_str_t rhs_expr;
} global_init_t;
static sp_dyn_array(global_init_t) g_global_inits = SP_NULLPTR;

static bool is_known_adt(sp_str_t name) {
    if (!g_adt_type_names)
        return false;
    sp_dyn_array_for(g_adt_type_names, i) if (sp_str_equal(g_adt_type_names[i], name)) return true;
    return false;
}

static sp_str_t adt_type_of_constructor(sp_str_t constr) {
    sp_dyn_array_for(g_adt_constrs, i) {
        if (sp_str_equal(g_adt_constrs[i].constr_name, constr))
            return g_adt_constrs[i].type_name;
    }
    return (sp_str_t){0};
}

static bool is_lambda_fn_name(sp_str_t name) {
    sp_dyn_array_for(g_lambda_fn_names, i) {
        if (sp_str_equal(g_lambda_fn_names[i], name))
            return true;
    }
    return false;
}

static bool is_closure_fn_name(sp_str_t name) {
    sp_dyn_array_for(g_closure_fn_names, i) {
        if (sp_str_equal(g_closure_fn_names[i], name))
            return true;
    }
    return false;
}

static closure_info_t *closure_info_lookup(sp_str_t name) {
    sp_dyn_array_for(g_closure_infos, i) {
        if (sp_str_equal(g_closure_infos[i].name, name))
            return &g_closure_infos[i];
    }
    return NULL;
}

static closure_value_info_t *closure_value_lookup(sp_str_t name) {
    for (s32 i = (s32)sp_dyn_array_size(g_closure_values) - 1; i >= 0; i--) {
        if (sp_str_equal(g_closure_values[i].name, name))
            return &g_closure_values[i];
    }
    return NULL;
}

static u32 closure_values_mark(void) {
    return (u32)sp_dyn_array_size(g_closure_values);
}

static void closure_values_pop_to(u32 mark) {
    while (g_closure_values && sp_dyn_array_size(g_closure_values) > mark)
        sp_dyn_array_pop(g_closure_values);
}

static bool is_decl_fn_name(sp_str_t name) {
    sp_dyn_array_for(g_decl_fn_names, i) {
        if (sp_str_equal(g_decl_fn_names[i], name))
            return true;
    }
    return false;
}

static u32 sym_mark(void) {
    return (u32)sp_dyn_array_size(g_sym_stack);
}

static void sym_pop_to(u32 mark) {
    while (g_sym_stack && sp_dyn_array_size(g_sym_stack) > mark)
        sp_dyn_array_pop(g_sym_stack);
}

static void sym_push_expr(sp_str_t name, sp_str_t c_expr) {
    sym_entry_t e = {.src_name = name, .c_expr = c_expr};
    sp_dyn_array_push(g_sym_stack, e);
}

static sp_str_t sym_lookup_expr(sp_str_t name) {
    for (s32 i = (s32)sp_dyn_array_size(g_sym_stack) - 1; i >= 0; i--) {
        if (sp_str_equal(g_sym_stack[i].src_name, name))
            return g_sym_stack[i].c_expr;
    }
    return (sp_str_t){0};
}

static fxsh_type_t *lookup_symbol_type(sp_str_t name) {
    fxsh_tenv_node_t *n = (fxsh_tenv_node_t *)g_codegen_type_env;
    while (n) {
        if (sp_str_equal(n->name, name))
            return n->scheme ? n->scheme->type : NULL;
        n = n->next;
    }
    return NULL;
}

static fxsh_type_t *normalize_codegen_type(fxsh_type_t *t) {
    while (t && t->kind == TYPE_APP) {
        t = t->data.app.con;
    }
    return t;
}

static void emit_c_type_for_type(codegen_ctx_t *ctx, fxsh_type_t *t) {
    t = normalize_codegen_type(t);
    if (!t) {
        emit_raw(ctx, "s64");
        return;
    }
    if (t->kind == TYPE_CON) {
        emit_c_type_for_fxsh_type(ctx, t->data.con);
        return;
    }
    if (t->kind == TYPE_VAR) {
        emit_raw(ctx, "s64");
        return;
    }
    if (t->kind == TYPE_RECORD) {
        emit_raw(ctx, "fxsh_record_t");
        return;
    }
    if (t->kind == TYPE_ARROW) {
        emit_raw(ctx, "void*");
        return;
    }
    emit_raw(ctx, "s64");
}

static fxsh_type_t *nth_param_type(fxsh_type_t *fn_t, u32 idx) {
    fxsh_type_t *cur = fn_t;
    for (u32 i = 0; i < idx; i++) {
        if (!cur || cur->kind != TYPE_ARROW)
            return NULL;
        cur = cur->data.arrow.ret;
    }
    if (!cur || cur->kind != TYPE_ARROW)
        return NULL;
    return cur->data.arrow.param;
}

static fxsh_type_t *return_type_of_fn(fxsh_type_t *fn_t) {
    fxsh_type_t *cur = fn_t;
    while (cur && cur->kind == TYPE_ARROW)
        cur = cur->data.arrow.ret;
    return cur;
}

static bool is_type_con_named(fxsh_type_t *t, sp_str_t name) {
    t = normalize_codegen_type(t);
    return t && t->kind == TYPE_CON && sp_str_equal(t->data.con, name);
}

static bool str_in_array(sp_dyn_array(sp_str_t) arr, sp_str_t s) {
    sp_dyn_array_for(arr, i) {
        if (sp_str_equal(arr[i], s))
            return true;
    }
    return false;
}

static void collect_free_idents(fxsh_ast_node_t *ast, sp_dyn_array(sp_str_t) * bound,
                                sp_dyn_array(sp_str_t) * out) {
    if (!ast)
        return;

    switch (ast->kind) {
        case AST_IDENT:
            if (!str_in_array(*bound, ast->data.ident) && !str_in_array(*out, ast->data.ident))
                sp_dyn_array_push(*out, ast->data.ident);
            return;
        case AST_LAMBDA: {
            u32 mark = (u32)sp_dyn_array_size(*bound);
            if (ast->data.lambda.params) {
                sp_dyn_array_for(ast->data.lambda.params, i) {
                    fxsh_ast_node_t *p = ast->data.lambda.params[i];
                    if (p && (p->kind == AST_PAT_VAR || p->kind == AST_IDENT))
                        sp_dyn_array_push(*bound, p->data.ident);
                }
            }
            collect_free_idents(ast->data.lambda.body, bound, out);
            while (sp_dyn_array_size(*bound) > mark)
                sp_dyn_array_pop(*bound);
            return;
        }
        case AST_BINARY:
            collect_free_idents(ast->data.binary.left, bound, out);
            collect_free_idents(ast->data.binary.right, bound, out);
            return;
        case AST_UNARY:
            collect_free_idents(ast->data.unary.operand, bound, out);
            return;
        case AST_IF:
            collect_free_idents(ast->data.if_expr.cond, bound, out);
            collect_free_idents(ast->data.if_expr.then_branch, bound, out);
            collect_free_idents(ast->data.if_expr.else_branch, bound, out);
            return;
        case AST_CALL:
            collect_free_idents(ast->data.call.func, bound, out);
            if (ast->data.call.args)
                sp_dyn_array_for(ast->data.call.args, i)
                    collect_free_idents(ast->data.call.args[i], bound, out);
            return;
        case AST_PIPE:
            collect_free_idents(ast->data.pipe.left, bound, out);
            collect_free_idents(ast->data.pipe.right, bound, out);
            return;
        case AST_CONSTR_APPL:
            if (ast->data.constr_appl.args)
                sp_dyn_array_for(ast->data.constr_appl.args, i)
                    collect_free_idents(ast->data.constr_appl.args[i], bound, out);
            return;
        case AST_MATCH:
            collect_free_idents(ast->data.match_expr.expr, bound, out);
            if (ast->data.match_expr.arms) {
                sp_dyn_array_for(ast->data.match_expr.arms, i) {
                    fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
                    if (!arm || arm->kind != AST_MATCH_ARM)
                        continue;
                    collect_free_idents(arm->data.match_arm.guard, bound, out);
                    collect_free_idents(arm->data.match_arm.body, bound, out);
                }
            }
            return;
        default:
            return;
    }
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
        adt_constr_entry_t ce = {.constr_name = c->data.data_constr.name, .type_name = type_name};
        sp_dyn_array_push(g_adt_constrs, ce);
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
            emit_raw(ctx, "((sp_str_t){ .data = \"");
            for (u32 i = 0; i < ast->data.lit_string.len; i++) {
                c8 c = ast->data.lit_string.data[i];
                if (c == '"' || c == '\\')
                    sp_dyn_array_push(*ctx->output, '\\');
                sp_dyn_array_push(*ctx->output, c);
            }
            emit_fmt(ctx, "\", .len = %u })", (unsigned)ast->data.lit_string.len);
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
    if (ast->data.binary.op == TOK_CONCAT) {
        emit_raw(ctx, "fxsh_str_concat(");
        gen_expr(ctx, ast->data.binary.left);
        emit_raw(ctx, ", ");
        gen_expr(ctx, ast->data.binary.right);
        emit_raw(ctx, ")");
        return;
    }
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
    fxsh_ast_list_t flat_args = SP_NULLPTR;
    fxsh_ast_node_t *func = ast;
    while (func && func->kind == AST_CALL) {
        sp_dyn_array_for(func->data.call.args, i) {
            sp_dyn_array_push(flat_args, func->data.call.args[i]);
        }
        func = func->data.call.func;
    }
    if (!func) {
        emit_raw(ctx, "0 /* bad call */");
        sp_dyn_array_free(flat_args);
        return;
    }
    if (func->kind == AST_IDENT) {
        sp_str_t fname = func->data.ident;
        fxsh_type_t *ft = lookup_symbol_type(fname);

        if (is_closure_fn_name(fname)) {
            closure_info_t *ci = closure_info_lookup(fname);
            u32 nargs = (u32)sp_dyn_array_size(flat_args);
            if (!ci) {
                emit_raw(ctx, "0 /* missing closure info */");
                sp_dyn_array_free(flat_args);
                return;
            }
            if (nargs < ci->outer_arity) {
                emit_raw(ctx, "0 /* unsupported closure arity */");
                sp_dyn_array_free(flat_args);
                return;
            }

            u32 consumed = 0;
            emit_raw(ctx, "({ __auto_type _c1 = ");
            gen_expr(ctx, func);
            emit_raw(ctx, "(");
            for (u32 i = 0; i < ci->outer_arity; i++) {
                if (i > 0)
                    emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[(s32)nargs - 1 - (s32)i]);
            }
            consumed += ci->outer_arity;
            emit_raw(ctx, "); ");

            u32 stage = 1;
            u32 var_idx = 1;
            bool ok = true;
            bool emitted_value = false;
            while (consumed < nargs) {
                if (stage == 0 || stage > sp_dyn_array_size(ci->stage_arities)) {
                    ok = false;
                    break;
                }
                u32 ar = ci->stage_arities[stage - 1];
                if (consumed + ar > nargs) {
                    ok = false;
                    break;
                }
                if (stage == sp_dyn_array_size(ci->stage_arities) && consumed + ar == nargs) {
                    emit_fmt(ctx, "_c%u.call(_c%u.env", var_idx, var_idx);
                    for (u32 j = 0; j < ar; j++) {
                        emit_raw(ctx, ", ");
                        gen_expr(ctx, flat_args[(s32)nargs - 1 - (s32)(consumed + j)]);
                    }
                    emit_raw(ctx, "); ");
                    consumed += ar;
                    emitted_value = true;
                    break;
                }
                emit_fmt(ctx, "__auto_type _c%u = _c%u.call(_c%u.env", var_idx + 1, var_idx,
                         var_idx);
                for (u32 j = 0; j < ar; j++) {
                    emit_raw(ctx, ", ");
                    gen_expr(ctx, flat_args[(s32)nargs - 1 - (s32)(consumed + j)]);
                }
                emit_raw(ctx, "); ");
                consumed += ar;
                var_idx++;
                stage++;
            }
            if (!ok || consumed != nargs) {
                emit_raw(ctx, "0 /* unsupported closure arity */; ");
            } else if (!emitted_value) {
                emit_fmt(ctx, "_c%u; ", var_idx);
            }
            emit_raw(ctx, "})");
            sp_dyn_array_free(flat_args);
            return;
        }

        closure_value_info_t *cv = closure_value_lookup(fname);
        if (cv) {
            closure_info_t *ci = closure_info_lookup(cv->root_fn);
            u32 nargs = (u32)sp_dyn_array_size(flat_args);
            if (!ci) {
                emit_raw(ctx, "0 /* missing closure root info */");
                sp_dyn_array_free(flat_args);
                return;
            }
            emit_raw(ctx, "({ __auto_type _c1 = ");
            gen_expr(ctx, func);
            emit_raw(ctx, "; ");

            u32 consumed = 0;
            u32 stage = cv->stage;
            u32 var_idx = 1;
            bool ok = true;
            bool emitted_value = false;
            while (consumed < nargs) {
                if (stage == 0 || stage > sp_dyn_array_size(ci->stage_arities)) {
                    ok = false;
                    break;
                }
                u32 ar = ci->stage_arities[stage - 1];
                if (consumed + ar > nargs) {
                    ok = false;
                    break;
                }
                if (stage == sp_dyn_array_size(ci->stage_arities) && consumed + ar == nargs) {
                    emit_fmt(ctx, "_c%u.call(_c%u.env", var_idx, var_idx);
                    for (u32 j = 0; j < ar; j++) {
                        emit_raw(ctx, ", ");
                        gen_expr(ctx, flat_args[(s32)nargs - 1 - (s32)(consumed + j)]);
                    }
                    emit_raw(ctx, "); ");
                    consumed += ar;
                    emitted_value = true;
                    break;
                }
                emit_fmt(ctx, "__auto_type _c%u = _c%u.call(_c%u.env", var_idx + 1, var_idx,
                         var_idx);
                for (u32 j = 0; j < ar; j++) {
                    emit_raw(ctx, ", ");
                    gen_expr(ctx, flat_args[(s32)nargs - 1 - (s32)(consumed + j)]);
                }
                emit_raw(ctx, "); ");
                consumed += ar;
                var_idx++;
                stage++;
            }
            if (!ok || consumed != nargs) {
                emit_raw(ctx, "0 /* unsupported closure value arity */; ");
            } else if (!emitted_value) {
                emit_fmt(ctx, "_c%u; ", var_idx);
            }
            emit_raw(ctx, "})");
            sp_dyn_array_free(flat_args);
            return;
        }

        if (!is_lambda_fn_name(fname) && !is_decl_fn_name(fname) && ft && ft->kind == TYPE_ARROW) {
            u32 nargs = (u32)sp_dyn_array_size(flat_args);
            if (nargs >= 1) {
                emit_raw(ctx, "({ __auto_type _c = ");
                gen_expr(ctx, func);
                emit_raw(ctx, "; _c.call(_c.env");
                for (u32 i = 0; i < nargs; i++) {
                    emit_raw(ctx, ", ");
                    gen_expr(ctx, flat_args[(s32)nargs - 1 - (s32)i]);
                }
                emit_raw(ctx, "); })");
            } else {
                emit_raw(ctx, "0 /* unsupported closure call */");
            }
            sp_dyn_array_free(flat_args);
            return;
        }
    }
    gen_expr(ctx, func);
    sp_dyn_array_push(*ctx->output, '(');
    for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
        if (i != (s32)sp_dyn_array_size(flat_args) - 1)
            emit_raw(ctx, ", ");
        gen_expr(ctx, flat_args[i]);
    }
    sp_dyn_array_push(*ctx->output, ')');
    sp_dyn_array_free(flat_args);
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

static void emit_c_type_guess_for_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *expr) {
    if (!expr) {
        emit_raw(ctx, "s64");
        return;
    }
    switch (expr->kind) {
        case AST_LIT_INT:
            emit_raw(ctx, "s64");
            return;
        case AST_LIT_FLOAT:
            emit_raw(ctx, "double");
            return;
        case AST_LIT_BOOL:
            emit_raw(ctx, "bool");
            return;
        case AST_LIT_STRING:
            emit_raw(ctx, "sp_str_t");
            return;
        case AST_BINARY:
            if (expr->data.binary.op == TOK_CONCAT) {
                emit_raw(ctx, "sp_str_t");
                return;
            }
            if (expr->data.binary.op == TOK_EQ || expr->data.binary.op == TOK_NEQ ||
                expr->data.binary.op == TOK_LT || expr->data.binary.op == TOK_GT ||
                expr->data.binary.op == TOK_LEQ || expr->data.binary.op == TOK_GEQ ||
                expr->data.binary.op == TOK_AND || expr->data.binary.op == TOK_OR) {
                emit_raw(ctx, "bool");
                return;
            }
            emit_raw(ctx, "s64");
            return;
        case AST_UNARY:
            if (expr->data.unary.op == TOK_NOT) {
                emit_raw(ctx, "bool");
                return;
            }
            emit_raw(ctx, "s64");
            return;
        case AST_IDENT: {
            fxsh_type_t *t = lookup_symbol_type(expr->data.ident);
            emit_c_type_for_type(ctx, t);
            return;
        }
        case AST_CALL: {
            fxsh_ast_node_t *f = expr->data.call.func;
            if (f && f->kind == AST_IDENT) {
                fxsh_type_t *ft = lookup_symbol_type(f->data.ident);
                emit_c_type_for_type(ctx, return_type_of_fn(ft));
                return;
            }
            emit_raw(ctx, "s64");
            return;
        }
        case AST_FIELD_ACCESS:
            emit_raw(ctx, "s64");
            return;
        case AST_RECORD:
            emit_raw(ctx, "fxsh_record_t");
            return;
        default:
            emit_raw(ctx, "s64");
            return;
    }
}

static void gen_boxed_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *expr) {
    if (!expr) {
        emit_raw(ctx, "fxsh_box_i64(0)");
        return;
    }
    switch (expr->kind) {
        case AST_LIT_INT:
            emit_raw(ctx, "fxsh_box_i64(");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_LIT_FLOAT:
            emit_raw(ctx, "fxsh_box_f64(");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_LIT_BOOL:
            emit_raw(ctx, "fxsh_box_bool(");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_LIT_STRING:
            emit_raw(ctx, "fxsh_box_str(");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_RECORD:
            emit_raw(ctx, "fxsh_box_ptr((void*)");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_IDENT: {
            fxsh_type_t *t = lookup_symbol_type(expr->data.ident);
            t = normalize_codegen_type(t);
            if (t && t->kind == TYPE_CON) {
                if (sp_str_equal(t->data.con, TYPE_STRING)) {
                    emit_raw(ctx, "fxsh_box_str(");
                    gen_expr(ctx, expr);
                    emit_raw(ctx, ")");
                    return;
                }
                if (sp_str_equal(t->data.con, TYPE_BOOL)) {
                    emit_raw(ctx, "fxsh_box_bool(");
                    gen_expr(ctx, expr);
                    emit_raw(ctx, ")");
                    return;
                }
                if (sp_str_equal(t->data.con, TYPE_FLOAT)) {
                    emit_raw(ctx, "fxsh_box_f64(");
                    gen_expr(ctx, expr);
                    emit_raw(ctx, ")");
                    return;
                }
            }
            emit_raw(ctx, "fxsh_box_i64(");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        }
        default:
            emit_raw(ctx, "fxsh_box_i64((s64)(");
            gen_expr(ctx, expr);
            emit_raw(ctx, "))");
            return;
    }
}

static void gen_record(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "({ ");
    emit_raw(ctx, "fxsh_record_t _r = fxsh_record_make(");
    emit_fmt(ctx, "%u", (unsigned)sp_dyn_array_size(ast->data.elements));
    emit_raw(ctx, "); ");
    u32 rec_id = ctx->temp_var_counter++;
    sp_dyn_array_for(ast->data.elements, i) {
        fxsh_ast_node_t *f = ast->data.elements[i];
        if (!f || f->kind != AST_FIELD_ACCESS)
            continue;
        emit_raw(ctx, "fxsh_record_set(&_r, ");
        emit_fmt(ctx, "%u", (unsigned)i);
        emit_raw(ctx, ", \"");
        emit_string(ctx, f->data.field.field);
        emit_raw(ctx, "\", ");
        gen_boxed_expr(ctx, f->data.field.object);
        emit_raw(ctx, "); ");
    }
    (void)rec_id;
    emit_raw(ctx, "_r; })");
}

static void gen_field_access(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "fxsh_unbox_i64(fxsh_record_get(");
    gen_expr(ctx, ast->data.field.object);
    emit_raw(ctx, ", \"");
    emit_string(ctx, ast->data.field.field);
    emit_raw(ctx, "\"))");
}

static void gen_let_in(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Use GCC statement expression ({ ... }) */
    emit_raw(ctx, "({\n");
    ctx->indent_level++;
    u32 sym_m = sym_mark();
    u32 clo_m = closure_values_mark();
    sp_dyn_array_for(ast->data.let_in.bindings, i) {
        fxsh_ast_node_t *b = ast->data.let_in.bindings[i];
        if (b->kind == AST_LET || b->kind == AST_DECL_LET) {
            char name_buf[256];
            char idx_buf[32];
            snprintf(idx_buf, sizeof(idx_buf), "__li%u", ctx->temp_var_counter++);
            size_t pos = 0;
            for (u32 j = 0; j < b->data.let.name.len && pos + 2 < sizeof(name_buf); j++) {
                c8 c = b->data.let.name.data[j];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_') {
                    name_buf[pos++] = c;
                } else {
                    name_buf[pos++] = '_';
                }
            }
            name_buf[pos] = '\0';
            strncat(name_buf, idx_buf, sizeof(name_buf) - strlen(name_buf) - 1);
            sp_str_t cname = make_owned_str(name_buf);

            sp_str_t clo_root = {0};
            u32 clo_stage = 0;
            bool is_closure_value = closure_expr_stage_of(b->data.let.value, &clo_root, &clo_stage);

            emit_indent(ctx);
            if (is_closure_value) {
                emit_closure_type_name(ctx, clo_root, clo_stage);
                emit_raw(ctx, " ");
            } else {
                emit_raw(ctx, "__auto_type ");
            }
            emit_string(ctx, cname);
            emit_raw(ctx, " = ");
            gen_expr(ctx, b->data.let.value);
            emit_raw(ctx, ";\n");

            sym_push_expr(b->data.let.name, cname);
            if (is_closure_value) {
                closure_value_info_t cvi = {
                    .name = b->data.let.name, .root_fn = clo_root, .stage = clo_stage};
                sp_dyn_array_push(g_closure_values, cvi);
            }
        }
    }
    emit_indent(ctx);
    gen_expr(ctx, ast->data.let_in.body);
    emit_raw(ctx, ";\n");
    closure_values_pop_to(clo_m);
    sym_pop_to(sym_m);
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
        case AST_PAT_RECORD: {
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    continue;
                fxsh_ast_node_t *sub = f->data.field.object;
                if (sub->kind == AST_PAT_VAR) {
                    emit_indent(ctx);
                    emit_raw(ctx, "__auto_type ");
                    emit_mangled(ctx, sub->data.ident);
                    emit_raw(ctx, " = fxsh_unbox_i64(fxsh_record_get(");
                    emit_string(ctx, (sp_str_t){.data = val_name, .len = (u32)strlen(val_name)});
                    emit_raw(ctx, ", \"");
                    emit_string(ctx, f->data.field.field);
                    emit_raw(ctx, "\"));\n");
                }
            }
            break;
        }
        default:
            break;
    }
}

static bool match_uses_switch_on_tag(fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_MATCH)
        return false;
    if (!ast->data.match_expr.arms || sp_dyn_array_size(ast->data.match_expr.arms) == 0)
        return false;
    sp_dyn_array_for(ast->data.match_expr.arms, i) {
        fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
        if (!arm || arm->kind != AST_MATCH_ARM)
            continue;
        fxsh_ast_node_t *pat = arm->data.match_arm.pattern;
        if (!pat)
            continue;
        if (pat->kind == AST_PAT_WILD || pat->kind == AST_PAT_VAR || pat->kind == AST_PAT_CONSTR)
            continue;
        return false;
    }
    return true;
}

static void gen_boxed_pattern_condition(codegen_ctx_t *ctx, fxsh_ast_node_t *pat,
                                        const char *boxed_expr) {
    if (!pat) {
        emit_raw(ctx, "true");
        return;
    }
    switch (pat->kind) {
        case AST_PAT_WILD:
        case AST_PAT_VAR:
            emit_raw(ctx, "true");
            return;
        case AST_LIT_INT:
            emit_fmt(ctx, "(fxsh_unbox_i64(%s) == %lldLL)", boxed_expr,
                     (long long)pat->data.lit_int);
            return;
        case AST_LIT_FLOAT:
            emit_fmt(ctx, "((%s).kind == FXSH_VAL_F64 && (%s).as.f == %.17g)", boxed_expr,
                     boxed_expr, pat->data.lit_float);
            return;
        case AST_LIT_BOOL:
            emit_fmt(ctx, "((%s).kind == FXSH_VAL_BOOL && (%s).as.b == %s)", boxed_expr, boxed_expr,
                     pat->data.lit_bool ? "true" : "false");
            return;
        case AST_LIT_STRING:
            emit_fmt(
                ctx,
                "((%s).kind == FXSH_VAL_STR && (%s).as.s.len == %u && strncmp((%s).as.s.data, \"",
                boxed_expr, boxed_expr, (unsigned)pat->data.lit_string.len, boxed_expr);
            for (u32 i = 0; i < pat->data.lit_string.len; i++) {
                c8 c = pat->data.lit_string.data[i];
                if (c == '"' || c == '\\')
                    sp_dyn_array_push(*ctx->output, '\\');
                sp_dyn_array_push(*ctx->output, c);
            }
            emit_fmt(ctx, "\", %u) == 0)", (unsigned)pat->data.lit_string.len);
            return;
        default:
            emit_raw(ctx, "false");
            return;
    }
}

static void gen_pattern_condition(codegen_ctx_t *ctx, fxsh_ast_node_t *pat, const char *val_name) {
    if (!pat) {
        emit_raw(ctx, "true");
        return;
    }
    switch (pat->kind) {
        case AST_PAT_WILD:
        case AST_PAT_VAR:
            emit_raw(ctx, "true");
            return;
        case AST_LIT_INT:
            emit_fmt(ctx, "(%s == %lldLL)", val_name, (long long)pat->data.lit_int);
            return;
        case AST_LIT_FLOAT:
            emit_fmt(ctx, "(%s == %.17g)", val_name, pat->data.lit_float);
            return;
        case AST_LIT_BOOL:
            emit_fmt(ctx, "(%s == %s)", val_name, pat->data.lit_bool ? "true" : "false");
            return;
        case AST_LIT_STRING: {
            emit_raw(ctx, "(");
            emit_fmt(ctx, "%s.len == %u && strncmp(%s.data, \"", val_name,
                     (unsigned)pat->data.lit_string.len, val_name);
            for (u32 i = 0; i < pat->data.lit_string.len; i++) {
                c8 c = pat->data.lit_string.data[i];
                if (c == '"' || c == '\\')
                    sp_dyn_array_push(*ctx->output, '\\');
                sp_dyn_array_push(*ctx->output, c);
            }
            emit_fmt(ctx, "\", %u) == 0)", (unsigned)pat->data.lit_string.len);
            return;
        }
        case AST_LIT_UNIT:
            emit_raw(ctx, "true");
            return;
        case AST_PAT_CONSTR: {
            sp_str_t tname = adt_type_of_constructor(pat->data.constr_appl.constr_name);
            if (tname.len == 0) {
                emit_raw(ctx, "false");
                return;
            }
            emit_raw(ctx, "(");
            emit_string(ctx, (sp_str_t){.data = val_name, .len = (u32)strlen(val_name)});
            emit_raw(ctx, ".tag == fxsh_tag_");
            emit_mangled(ctx, tname);
            emit_raw(ctx, "_");
            emit_mangled(ctx, pat->data.constr_appl.constr_name);
            emit_raw(ctx, ")");
            return;
        }
        case AST_PAT_RECORD: {
            emit_raw(ctx, "(true");
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS)
                    continue;
                emit_raw(ctx, " && fxsh_record_has(");
                emit_string(ctx, (sp_str_t){.data = val_name, .len = (u32)strlen(val_name)});
                emit_raw(ctx, ", \"");
                emit_string(ctx, f->data.field.field);
                emit_raw(ctx, "\")");
                if (f->data.field.object) {
                    emit_raw(ctx, " && ");
                    char boxed_expr[256];
                    snprintf(boxed_expr, sizeof(boxed_expr), "fxsh_record_get(%s, \"%.*s\")",
                             val_name, f->data.field.field.len, f->data.field.field.data);
                    gen_boxed_pattern_condition(ctx, f->data.field.object, boxed_expr);
                }
            }
            emit_raw(ctx, ")");
            return;
        }
        default:
            emit_raw(ctx, "false");
            return;
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
    emit_raw(ctx, "({\n"); /* GCC statement expression */
    ctx->indent_level++;

    emit_indent(ctx);
    emit_raw(ctx, "__auto_type _match_val = ");
    gen_expr(ctx, ast->data.match_expr.expr);
    emit_raw(ctx, ";\n");

    emit_indent(ctx);
    emit_raw(ctx, "__auto_type _match_res = 0; /* placeholder */\n");
    emit_indent(ctx);
    emit_raw(ctx, "bool _match_done = false;\n");

    sp_dyn_array(sp_str_t) emitted_ctors = SP_NULLPTR;
    if (match_uses_switch_on_tag(ast)) {
        emit_indent(ctx);
        emit_raw(ctx, "switch (_match_val.tag) {\n");
        ctx->indent_level++;
        bool emitted_default = false;
        sp_dyn_array_for(ast->data.match_expr.arms, i) {
            fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
            if (arm->kind != AST_MATCH_ARM)
                continue;

            fxsh_ast_node_t *pat = arm->data.match_arm.pattern;
            emit_indent(ctx);

            if (pat->kind == AST_PAT_WILD || pat->kind == AST_PAT_VAR) {
                if (emitted_default)
                    continue;
                emit_raw(ctx, "default: {\n");
                emitted_default = true;
            } else if (pat->kind == AST_PAT_CONSTR) {
                bool seen_ctor = false;
                sp_dyn_array_for(emitted_ctors, k) {
                    if (sp_str_equal(emitted_ctors[k], pat->data.constr_appl.constr_name)) {
                        seen_ctor = true;
                        break;
                    }
                }
                if (seen_ctor)
                    continue;

                sp_str_t tname = adt_type_of_constructor(pat->data.constr_appl.constr_name);
                if (tname.len == 0) {
                    if (emitted_default)
                        continue;
                    emit_raw(ctx, "default: {\n");
                    emitted_default = true;
                } else {
                    sp_dyn_array_push(emitted_ctors, pat->data.constr_appl.constr_name);
                    emit_raw(ctx, "case fxsh_tag_");
                    emit_mangled(ctx, tname);
                    emit_raw(ctx, "_");
                    emit_mangled(ctx, pat->data.constr_appl.constr_name);
                    emit_raw(ctx, ": {\n");
                }
            } else {
                if (emitted_default)
                    continue;
                emit_raw(ctx, "default: {\n");
                emitted_default = true;
            }
            ctx->indent_level++;

            gen_pattern_bindings(ctx, pat, "_match_val");

            if (arm->data.match_arm.guard) {
                emit_indent(ctx);
                emit_raw(ctx, "if (!(");
                gen_expr(ctx, arm->data.match_arm.guard);
                emit_raw(ctx, ")) break;\n");
            }

            emit_indent(ctx);
            emit_raw(ctx, "_match_res = ");
            gen_expr(ctx, arm->data.match_arm.body);
            emit_raw(ctx, ";\n");
            emit_indent(ctx);
            emit_raw(ctx, "_match_done = true;\n");
            emit_indent(ctx);
            emit_raw(ctx, "break;\n");

            ctx->indent_level--;
            emit_indent(ctx);
            emit_raw(ctx, "}\n");
        }

        ctx->indent_level--;
        emit_indent(ctx);
        emit_raw(ctx, "}\n");
    } else {
        sp_dyn_array_for(ast->data.match_expr.arms, i) {
            fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
            if (!arm || arm->kind != AST_MATCH_ARM)
                continue;
            fxsh_ast_node_t *pat = arm->data.match_arm.pattern;

            emit_indent(ctx);
            emit_raw(ctx, "if (!_match_done && (");
            gen_pattern_condition(ctx, pat, "_match_val");
            emit_raw(ctx, ")) {\n");
            ctx->indent_level++;

            gen_pattern_bindings(ctx, pat, "_match_val");

            if (arm->data.match_arm.guard) {
                emit_indent(ctx);
                emit_raw(ctx, "if (");
                gen_expr(ctx, arm->data.match_arm.guard);
                emit_raw(ctx, ") {\n");
                ctx->indent_level++;
            }

            emit_indent(ctx);
            emit_raw(ctx, "_match_res = ");
            gen_expr(ctx, arm->data.match_arm.body);
            emit_raw(ctx, ";\n");
            emit_indent(ctx);
            emit_raw(ctx, "_match_done = true;\n");

            if (arm->data.match_arm.guard) {
                ctx->indent_level--;
                emit_indent(ctx);
                emit_raw(ctx, "}\n");
            }

            ctx->indent_level--;
            emit_indent(ctx);
            emit_raw(ctx, "}\n");
        }
    }
    emit_indent(ctx);
    emit_raw(ctx, "_match_res;\n");
    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "})");
    sp_dyn_array_free(emitted_ctors);
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
        case AST_IDENT: {
            sp_str_t mapped = sym_lookup_expr(ast->data.ident);
            if (mapped.len > 0) {
                emit_string(ctx, mapped);
                break;
            }
        }
            if (is_lambda_fn_name(ast->data.ident)) {
                emit_raw(ctx, "fxsh_fn_");
            }
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
        case AST_RECORD:
            gen_record(ctx, ast);
            break;
        case AST_FIELD_ACCESS:
            gen_field_access(ctx, ast);
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
    fxsh_type_t *fn_t = lookup_symbol_type(ast->data.decl_fn.name);
    emit_indent(ctx);
    emit_c_type_for_type(ctx, return_type_of_fn(fn_t));
    emit_raw(ctx, " ");
    emit_mangled(ctx, ast->data.decl_fn.name);
    sp_dyn_array_push(*ctx->output, '(');
    if (sp_dyn_array_size(ast->data.decl_fn.params) == 0) {
        emit_raw(ctx, "void");
    } else {
        sp_dyn_array_for(ast->data.decl_fn.params, i) {
            if (i > 0)
                emit_raw(ctx, ", ");
            fxsh_ast_node_t *p = ast->data.decl_fn.params[i];
            emit_c_type_for_type(ctx, nth_param_type(fn_t, (u32)i));
            emit_raw(ctx, " ");
            if (p->kind == AST_PAT_VAR) {
                emit_mangled(ctx, p->data.ident);
            } else {
                emit_fmt(ctx, "_arg%u", (unsigned)i);
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

static sp_str_t gen_expr_to_owned_string(codegen_ctx_t *ctx, fxsh_ast_node_t *expr) {
    sp_dyn_array(c8) buf = SP_NULLPTR;
    codegen_ctx_t tmp = {
        .output = &buf,
        .indent_level = ctx->indent_level,
        .temp_var_counter = ctx->temp_var_counter,
        .lambda_counter = ctx->lambda_counter,
    };
    gen_expr(&tmp, expr);
    sp_dyn_array_push(buf, '\0');
    sp_str_t s = make_owned_str((const char *)buf);
    ctx->temp_var_counter = tmp.temp_var_counter;
    ctx->lambda_counter = tmp.lambda_counter;
    sp_dyn_array_free(buf);
    return s;
}

static bool flatten_call_head_ident(fxsh_ast_node_t *expr, sp_str_t *out_head, u32 *out_argc) {
    if (!expr || expr->kind != AST_CALL)
        return false;
    fxsh_ast_node_t *cur = expr;
    u32 argc = 0;
    while (cur && cur->kind == AST_CALL) {
        argc += (u32)sp_dyn_array_size(cur->data.call.args);
        cur = cur->data.call.func;
    }
    if (!cur || cur->kind != AST_IDENT)
        return false;
    if (out_head)
        *out_head = cur->data.ident;
    if (out_argc)
        *out_argc = argc;
    return true;
}

static void emit_closure_type_name(codegen_ctx_t *ctx, sp_str_t root_fn, u32 stage) {
    emit_raw(ctx, "fxsh_clo_");
    emit_mangled(ctx, root_fn);
    emit_fmt(ctx, "_%u_t", (unsigned)stage);
}

static bool closure_expr_stage_of(fxsh_ast_node_t *expr, sp_str_t *out_root, u32 *out_stage) {
    if (!expr)
        return false;
    if (expr->kind == AST_IDENT) {
        closure_value_info_t *cv = closure_value_lookup(expr->data.ident);
        if (!cv)
            return false;
        if (out_root)
            *out_root = cv->root_fn;
        if (out_stage)
            *out_stage = cv->stage;
        return true;
    }
    if (expr->kind == AST_CALL) {
        sp_str_t head = {0};
        u32 argc = 0;
        if (!flatten_call_head_ident(expr, &head, &argc))
            return false;
        closure_info_t *ci = closure_info_lookup(head);
        if (ci && argc == ci->outer_arity) {
            if (out_root)
                *out_root = head;
            if (out_stage)
                *out_stage = 1;
            return true;
        }
        closure_value_info_t *cv = closure_value_lookup(head);
        if (cv) {
            closure_info_t *rci = closure_info_lookup(cv->root_fn);
            if (!rci)
                return false;
            if (cv->stage == 0 || cv->stage > sp_dyn_array_size(rci->stage_arities))
                return false;
            if (argc == rci->stage_arities[cv->stage - 1] &&
                cv->stage < sp_dyn_array_size(rci->stage_arities)) {
                if (out_root)
                    *out_root = cv->root_fn;
                if (out_stage)
                    *out_stage = cv->stage + 1;
                return true;
            }
        }
    }
    return false;
}

static bool top_let_value_is_closure_ctor(fxsh_ast_node_t *v, sp_str_t *out_fn_name) {
    if (!v || v->kind != AST_CALL)
        return false;
    fxsh_ast_node_t *func = v->data.call.func;
    if (!func || func->kind != AST_IDENT)
        return false;
    if (!is_closure_fn_name(func->data.ident))
        return false;
    closure_info_t *ci = closure_info_lookup(func->data.ident);
    if (!ci)
        return false;
    if ((u32)sp_dyn_array_size(v->data.call.args) != ci->outer_arity)
        return false;
    if (out_fn_name)
        *out_fn_name = func->data.ident;
    return true;
}

static bool gen_decl_let_closure_generic(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    fxsh_ast_node_t *lam = ast->data.let.value;
    if (!lam || lam->kind != AST_LAMBDA)
        return false;
    fxsh_type_t *fn_t = lookup_symbol_type(ast->data.let.name);
    if (!fn_t || fn_t->kind != TYPE_ARROW)
        return false;

    sp_dyn_array(fxsh_ast_list_t) groups = SP_NULLPTR;
    fxsh_ast_node_t *final_body = NULL;
    fxsh_ast_node_t *cur_lam = lam;
    while (cur_lam && cur_lam->kind == AST_LAMBDA) {
        if (!cur_lam->data.lambda.params || sp_dyn_array_size(cur_lam->data.lambda.params) == 0)
            return false;
        sp_dyn_array_push(groups, cur_lam->data.lambda.params);
        if (cur_lam->data.lambda.body && cur_lam->data.lambda.body->kind == AST_LAMBDA) {
            cur_lam = cur_lam->data.lambda.body;
        } else {
            final_body = cur_lam->data.lambda.body;
            break;
        }
    }
    if (sp_dyn_array_size(groups) < 2 || !final_body)
        return false;

    u32 group_count = (u32)sp_dyn_array_size(groups);
    u32 outer_arity = (u32)sp_dyn_array_size(groups[0]);
    u32 num_stages = group_count - 1;

    sp_dyn_array(u32) group_offsets = SP_NULLPTR;
    u32 total_params = 0;
    sp_dyn_array_for(groups, gi) {
        sp_dyn_array_push(group_offsets, total_params);
        total_params += (u32)sp_dyn_array_size(groups[gi]);
    }

    sp_dyn_array(fxsh_type_t *) param_types = SP_NULLPTR;
    fxsh_type_t *cur_t = fn_t;
    for (u32 i = 0; i < total_params; i++) {
        if (!cur_t || cur_t->kind != TYPE_ARROW)
            return false;
        sp_dyn_array_push(param_types, cur_t->data.arrow.param);
        cur_t = cur_t->data.arrow.ret;
    }
    fxsh_type_t *ret_type = cur_t;

    char base[128];
    mangle_into(ast->data.let.name, base, sizeof(base));

    for (u32 s = 1; s <= num_stages; s++) {
        emit_raw(ctx, "typedef struct fxsh_clo_");
        emit_raw(ctx, base);
        emit_fmt(ctx, "_%u_s fxsh_clo_%s_%u_t;\n", s, base, s);
    }

    for (u32 s = 1; s <= num_stages; s++) {
        emit_raw(ctx, "struct fxsh_clo_");
        emit_raw(ctx, base);
        emit_fmt(ctx, "_%u_s { ", s);
        if (s == num_stages) {
            emit_c_type_for_type(ctx, ret_type);
        } else {
            emit_raw(ctx, "fxsh_clo_");
            emit_raw(ctx, base);
            emit_fmt(ctx, "_%u_t", s + 1);
        }
        emit_raw(ctx, " (*call)(void *env");
        fxsh_ast_list_t stage_params = groups[s];
        u32 off = group_offsets[s];
        sp_dyn_array_for(stage_params, i) {
            fxsh_ast_node_t *p = stage_params[i];
            if (!p || (p->kind != AST_PAT_VAR && p->kind != AST_IDENT))
                return false;
            emit_raw(ctx, ", ");
            emit_c_type_for_type(ctx, param_types[off + (u32)i]);
            emit_raw(ctx, " ");
            emit_mangled(ctx, p->data.ident);
        }
        emit_raw(ctx, "); void *env; };\n");
    }

    for (u32 s = 1; s <= num_stages; s++) {
        emit_fmt(ctx, "typedef struct { ");
        for (u32 g = 0; g < s; g++) {
            fxsh_ast_list_t ps = groups[g];
            u32 off = group_offsets[g];
            sp_dyn_array_for(ps, pi) {
                fxsh_ast_node_t *p = ps[pi];
                emit_c_type_for_type(ctx, param_types[off + (u32)pi]);
                emit_raw(ctx, " ");
                emit_mangled(ctx, p->data.ident);
                emit_raw(ctx, "; ");
            }
        }
        emit_fmt(ctx, "} fxsh_env_%s_%u_t;\n", base, s);
    }

    for (s32 s = (s32)num_stages; s >= 1; s--) {
        emit_raw(ctx, "static ");
        if ((u32)s == num_stages) {
            emit_c_type_for_type(ctx, ret_type);
        } else {
            emit_raw(ctx, "fxsh_clo_");
            emit_raw(ctx, base);
            emit_fmt(ctx, "_%u_t", (unsigned)(s + 1));
        }
        emit_raw(ctx, " fxsh_clo_");
        emit_raw(ctx, base);
        emit_fmt(ctx, "_%u(void *env", (unsigned)s);

        fxsh_ast_list_t stage_params = groups[(u32)s];
        u32 stage_off = group_offsets[(u32)s];
        sp_dyn_array_for(stage_params, i) {
            fxsh_ast_node_t *p = stage_params[i];
            emit_raw(ctx, ", ");
            emit_c_type_for_type(ctx, param_types[stage_off + (u32)i]);
            emit_raw(ctx, " ");
            emit_mangled(ctx, p->data.ident);
        }
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;
        emit_indent(ctx);
        emit_fmt(ctx, "fxsh_env_%s_%u_t *e = (fxsh_env_%s_%u_t *)env;\n", base, (unsigned)s, base,
                 (unsigned)s);

        u32 mark = sym_mark();
        for (u32 g = 0; g < (u32)s; g++) {
            fxsh_ast_list_t ps = groups[g];
            sp_dyn_array_for(ps, pi) {
                fxsh_ast_node_t *p = ps[pi];
                char buf[256];
                char tmp[128];
                mangle_into(p->data.ident, tmp, sizeof(tmp));
                snprintf(buf, sizeof(buf), "e->%s", tmp);
                sym_push_expr(p->data.ident, make_owned_str(buf));
            }
        }

        if ((u32)s == num_stages) {
            emit_indent(ctx);
            emit_raw(ctx, "return ");
            gen_expr(ctx, final_body);
            emit_raw(ctx, ";\n");
        } else {
            emit_indent(ctx);
            emit_fmt(
                ctx,
                "fxsh_env_%s_%u_t *e2 = (fxsh_env_%s_%u_t *)malloc(sizeof(fxsh_env_%s_%u_t));\n",
                base, (unsigned)(s + 1), base, (unsigned)(s + 1), base, (unsigned)(s + 1));
            for (u32 g = 0; g < (u32)s; g++) {
                fxsh_ast_list_t ps = groups[g];
                sp_dyn_array_for(ps, pi) {
                    fxsh_ast_node_t *p = ps[pi];
                    emit_indent(ctx);
                    emit_raw(ctx, "e2->");
                    emit_mangled(ctx, p->data.ident);
                    emit_raw(ctx, " = e->");
                    emit_mangled(ctx, p->data.ident);
                    emit_raw(ctx, ";\n");
                }
            }
            sp_dyn_array_for(stage_params, i) {
                fxsh_ast_node_t *p = stage_params[i];
                emit_indent(ctx);
                emit_raw(ctx, "e2->");
                emit_mangled(ctx, p->data.ident);
                emit_raw(ctx, " = ");
                emit_mangled(ctx, p->data.ident);
                emit_raw(ctx, ";\n");
            }
            emit_indent(ctx);
            emit_raw(ctx, "return (fxsh_clo_");
            emit_raw(ctx, base);
            emit_fmt(ctx, "_%u_t){ .call = fxsh_clo_%s_%u, .env = e2 };\n", (unsigned)(s + 1), base,
                     (unsigned)(s + 1));
        }
        sym_pop_to(mark);
        ctx->indent_level--;
        emit_line(ctx, "}");
    }

    sp_dyn_array_push(g_lambda_fn_names, ast->data.let.name);
    sp_dyn_array_push(g_closure_fn_names, ast->data.let.name);
    closure_info_t ci = {
        .name = ast->data.let.name, .c_base = ast->data.let.name, .outer_arity = outer_arity};
    ci.stage_arities = SP_NULLPTR;
    for (u32 s = 1; s <= num_stages; s++) {
        sp_dyn_array_push(ci.stage_arities, (u32)sp_dyn_array_size(groups[s]));
    }
    sp_dyn_array_push(g_closure_infos, ci);

    emit_raw(ctx, "static fxsh_clo_");
    emit_raw(ctx, base);
    emit_raw(ctx, "_1_t fxsh_fn_");
    emit_mangled(ctx, ast->data.let.name);
    emit_raw(ctx, "(");
    fxsh_ast_list_t outer_params = groups[0];
    u32 outer_off = group_offsets[0];
    sp_dyn_array_for(outer_params, i) {
        fxsh_ast_node_t *p = outer_params[i];
        if (i > 0)
            emit_raw(ctx, ", ");
        emit_c_type_for_type(ctx, param_types[outer_off + (u32)i]);
        emit_raw(ctx, " ");
        emit_mangled(ctx, p->data.ident);
    }
    emit_raw(ctx, ") {\n");
    ctx->indent_level++;
    emit_indent(ctx);
    emit_fmt(ctx, "fxsh_env_%s_1_t *e = (fxsh_env_%s_1_t *)malloc(sizeof(fxsh_env_%s_1_t));\n",
             base, base, base);
    sp_dyn_array_for(outer_params, i) {
        fxsh_ast_node_t *p = outer_params[i];
        emit_indent(ctx);
        emit_raw(ctx, "e->");
        emit_mangled(ctx, p->data.ident);
        emit_raw(ctx, " = ");
        emit_mangled(ctx, p->data.ident);
        emit_raw(ctx, ";\n");
    }
    emit_indent(ctx);
    emit_raw(ctx, "return (fxsh_clo_");
    emit_raw(ctx, base);
    emit_raw(ctx, "_1_t){ .call = fxsh_clo_");
    emit_raw(ctx, base);
    emit_raw(ctx, "_1, .env = e };\n");
    ctx->indent_level--;
    emit_line(ctx, "}");
    emit_raw(ctx, "\n");
    return true;
}

static void gen_decl_let(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Lambda let-binding becomes a C function (no closure capture support yet). */
    if (ast->data.let.value && ast->data.let.value->kind == AST_LAMBDA) {
        if (gen_decl_let_closure_generic(ctx, ast))
            return;
        fxsh_ast_node_t *lam = ast->data.let.value;
        fxsh_type_t *fn_t = lookup_symbol_type(ast->data.let.name);
        sp_dyn_array_push(g_lambda_fn_names, ast->data.let.name);
        emit_indent(ctx);
        emit_raw(ctx, "static ");
        emit_c_type_for_type(ctx, return_type_of_fn(fn_t));
        emit_raw(ctx, " ");
        emit_raw(ctx, "fxsh_fn_");
        emit_mangled(ctx, ast->data.let.name);
        emit_raw(ctx, "(");
        if (!lam->data.lambda.params || sp_dyn_array_size(lam->data.lambda.params) == 0) {
            emit_raw(ctx, "void");
        } else {
            sp_dyn_array_for(lam->data.lambda.params, i) {
                if (i > 0)
                    emit_raw(ctx, ", ");
                emit_c_type_for_type(ctx, nth_param_type(fn_t, (u32)i));
                emit_raw(ctx, " ");
                fxsh_ast_node_t *p = lam->data.lambda.params[i];
                if (p->kind == AST_PAT_VAR || p->kind == AST_IDENT) {
                    emit_mangled(ctx, p->data.ident);
                } else {
                    emit_fmt(ctx, "_arg%u", (unsigned)i);
                }
            }
        }
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;
        emit_indent(ctx);
        emit_raw(ctx, "return ");
        gen_expr(ctx, lam->data.lambda.body);
        emit_raw(ctx, ";\n");
        ctx->indent_level--;
        emit_line(ctx, "}");
        emit_raw(ctx, "\n");
        return;
    }

    char name_buf[256];
    char idx_buf[32];
    snprintf(idx_buf, sizeof(idx_buf), "__%u", g_top_let_counter++);
    size_t pos = 0;
    for (u32 j = 0; j < ast->data.let.name.len && pos + 2 < sizeof(name_buf); j++) {
        c8 c = ast->data.let.name.data[j];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            name_buf[pos++] = c;
        } else {
            name_buf[pos++] = '_';
        }
    }
    name_buf[pos] = '\0';
    strncat(name_buf, idx_buf, sizeof(name_buf) - strlen(name_buf) - 1);
    sp_str_t cname = make_owned_str(name_buf);

    sp_str_t rhs = gen_expr_to_owned_string(ctx, ast->data.let.value);
    fxsh_type_t *vt = lookup_symbol_type(ast->data.let.name);
    sp_str_t clo_root = (sp_str_t){0};
    u32 clo_stage = 0;
    bool is_closure_value = closure_expr_stage_of(ast->data.let.value, &clo_root, &clo_stage);

    emit_indent(ctx);
    emit_raw(ctx, "static ");
    if (is_closure_value) {
        emit_closure_type_name(ctx, clo_root, clo_stage);
    } else {
        emit_c_type_for_type(ctx, vt);
    }
    emit_raw(ctx, " ");
    emit_string(ctx, cname);
    emit_raw(ctx, ";\n");

    global_init_t gi = {.c_name = cname, .rhs_expr = rhs};
    sp_dyn_array_push(g_global_inits, gi);
    sym_push_expr(ast->data.let.name, cname);
    if (is_closure_value) {
        closure_value_info_t cvi = {
            .name = ast->data.let.name, .root_fn = clo_root, .stage = clo_stage};
        sp_dyn_array_push(g_closure_values, cvi);
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
    emit_line(ctx, "#include <stdlib.h>");
    emit_line(ctx, "#include <string.h>");
    emit_line(ctx, "");
    emit_line(ctx, "typedef int64_t  s64;");
    emit_line(ctx, "typedef int32_t  s32;");
    emit_line(ctx, "typedef uint64_t u64;");
    emit_line(ctx, "typedef uint32_t u32;");
    emit_line(ctx, "typedef double   f64;");
    emit_line(ctx, "typedef char     c8;");
    emit_line(ctx, "typedef struct { const char *data; u32 len; } sp_str_t;");
    emit_line(ctx, "typedef enum { FXSH_VAL_I64, FXSH_VAL_F64, FXSH_VAL_BOOL, FXSH_VAL_STR, "
                   "FXSH_VAL_PTR } fxsh_val_kind_t;");
    emit_line(ctx, "typedef struct { fxsh_val_kind_t kind; union { s64 i; f64 f; bool b; sp_str_t "
                   "s; void *p; } as; } fxsh_value_t;");
    emit_line(ctx,
              "typedef struct { u32 len; const char **names; fxsh_value_t *vals; } fxsh_record_t;");
    emit_line(ctx, "");
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_i64(s64 v) { fxsh_value_t x; x.kind = "
                   "FXSH_VAL_I64; x.as.i = v; return x; }");
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_f64(f64 v) { fxsh_value_t x; x.kind = "
                   "FXSH_VAL_F64; x.as.f = v; return x; }");
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_bool(bool v) { fxsh_value_t x; x.kind = "
                   "FXSH_VAL_BOOL; x.as.b = v; return x; }");
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_str(sp_str_t v) { fxsh_value_t x; x.kind = "
                   "FXSH_VAL_STR; x.as.s = v; return x; }");
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_ptr(void *v) { fxsh_value_t x; x.kind = "
                   "FXSH_VAL_PTR; x.as.p = v; return x; }");
    emit_line(ctx, "static inline s64 fxsh_unbox_i64(fxsh_value_t v) {");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_I64) return v.as.i;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_BOOL) return v.as.b ? 1 : 0;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_F64) return (s64)v.as.f;");
    emit_line(ctx, "  return 0;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_record_t fxsh_record_make(u32 n) {");
    emit_line(ctx, "  fxsh_record_t r; r.len = n;");
    emit_line(ctx, "  r.names = (const char **)calloc((size_t)n, sizeof(const char *));");
    emit_line(ctx, "  r.vals  = (fxsh_value_t *)calloc((size_t)n, sizeof(fxsh_value_t));");
    emit_line(ctx, "  return r;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline void fxsh_record_set(fxsh_record_t *r, u32 i, const char *name, "
                   "fxsh_value_t v) {");
    emit_line(ctx, "  if (!r || i >= r->len) return;");
    emit_line(ctx, "  r->names[i] = name;");
    emit_line(ctx, "  r->vals[i] = v;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline bool fxsh_record_has(fxsh_record_t r, const char *name) {");
    emit_line(ctx, "  for (u32 i = 0; i < r.len; i++) {");
    emit_line(ctx, "    if (r.names[i] && strcmp(r.names[i], name) == 0) return true;");
    emit_line(ctx, "  }");
    emit_line(ctx, "  return false;");
    emit_line(ctx, "}");
    emit_line(ctx,
              "static inline fxsh_value_t fxsh_record_get(fxsh_record_t r, const char *name) {");
    emit_line(ctx, "  for (u32 i = 0; i < r.len; i++) {");
    emit_line(ctx, "    if (r.names[i] && strcmp(r.names[i], name) == 0) return r.vals[i];");
    emit_line(ctx, "  }");
    emit_line(ctx, "  return fxsh_box_i64(0);");
    emit_line(ctx, "}");
    emit_line(ctx, "");
    emit_line(ctx, "static inline sp_str_t fxsh_str_concat(sp_str_t a, sp_str_t b) {");
    emit_line(ctx, "  u32 n = a.len + b.len;");
    emit_line(ctx, "  char *p = (char *)malloc((size_t)n + 1u);");
    emit_line(ctx, "  if (!p) return (sp_str_t){ .data = \"\", .len = 0 };");
    emit_line(ctx, "  memcpy(p, a.data, a.len);");
    emit_line(ctx, "  memcpy(p + a.len, b.data, b.len);");
    emit_line(ctx, "  p[n] = '\\0';");
    emit_line(ctx, "  return (sp_str_t){ .data = p, .len = n };");
    emit_line(ctx, "}");
    emit_line(ctx, "");
    emit_line(ctx, "/* GNU statement-expressions are used in generated expressions. */");
    emit_line(ctx, "");
}

static void gen_adt_init_fn(codegen_ctx_t *ctx) {
    if ((!g_adt_inits || sp_dyn_array_size(g_adt_inits) == 0) &&
        (!g_global_inits || sp_dyn_array_size(g_global_inits) == 0))
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
    sp_dyn_array_for(g_global_inits, i) {
        emit_indent(ctx);
        emit_string(ctx, g_global_inits[i].c_name);
        emit_raw(ctx, " = ");
        emit_string(ctx, g_global_inits[i].rhs_expr);
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
    if ((g_adt_inits && sp_dyn_array_size(g_adt_inits) > 0) ||
        (g_global_inits && sp_dyn_array_size(g_global_inits) > 0))
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
    g_global_inits = SP_NULLPTR;
    g_lambda_fn_names = SP_NULLPTR;
    g_closure_fn_names = SP_NULLPTR;
    g_closure_infos = SP_NULLPTR;
    g_closure_values = SP_NULLPTR;
    g_decl_fn_names = SP_NULLPTR;
    g_sym_stack = SP_NULLPTR;
    g_top_let_counter = 0;
    g_codegen_type_env = SP_NULLPTR;

    /* Build a type environment so signatures are generated with concrete types. */
    if (ast) {
        fxsh_type_env_t tenv = SP_NULLPTR;
        fxsh_constr_env_t cenv = SP_NULLPTR;
        fxsh_type_t *program_t = NULL;
        if (fxsh_type_infer(ast, &tenv, &cenv, &program_t) == ERR_OK)
            g_codegen_type_env = tenv;
        (void)cenv;
        (void)program_t;
    }

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
                sp_dyn_array_push(g_decl_fn_names, d->data.decl_fn.name);
                fxsh_type_t *fn_t = lookup_symbol_type(d->data.decl_fn.name);
                emit_raw(&ctx, "static ");
                emit_c_type_for_type(&ctx, return_type_of_fn(fn_t));
                emit_raw(&ctx, " ");
                emit_mangled(&ctx, d->data.decl_fn.name);
                emit_raw(&ctx, "(");
                if (sp_dyn_array_size(d->data.decl_fn.params) == 0) {
                    emit_raw(&ctx, "void");
                } else {
                    sp_dyn_array_for(d->data.decl_fn.params, pi) {
                        if (pi > 0)
                            emit_raw(&ctx, ", ");
                        emit_c_type_for_type(&ctx, nth_param_type(fn_t, (u32)pi));
                        emit_raw(&ctx, " ");
                        fxsh_ast_node_t *p = d->data.decl_fn.params[pi];
                        if (p->kind == AST_PAT_VAR) {
                            emit_mangled(&ctx, p->data.ident);
                        } else {
                            emit_fmt(&ctx, "_arg%u", (unsigned)pi);
                        }
                    }
                }
                emit_raw(&ctx, ");\n");
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
