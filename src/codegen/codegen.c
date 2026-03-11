/*
 * codegen.c - fxsh C code generation backend
 *
 * Fixes from design review:
 *   1. embed_raw → emit_raw (typo fix)
 *   2. ADT struct generation: close struct with emit_raw not embed_raw
 *   3. gen_match: generate switch-on-tag (replacing old TODO stub)
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

static void emit_c_escaped_str(codegen_ctx_t *ctx, sp_str_t s) {
    for (u32 i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
            case '\n':
                emit_raw(ctx, "\\n");
                break;
            case '\t':
                emit_raw(ctx, "\\t");
                break;
            case '\r':
                emit_raw(ctx, "\\r");
                break;
            case '\\':
                emit_raw(ctx, "\\\\");
                break;
            case '"':
                emit_raw(ctx, "\\\"");
                break;
            case '\0':
                emit_raw(ctx, "\\0");
                break;
            default:
                if (c < 32 || c >= 127) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\%03o", (unsigned)c);
                    emit_raw(ctx, esc);
                } else
                    sp_dyn_array_push(*ctx->output, (c8)c);
                break;
        }
    }
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
static void gen_boxed_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *expr);
static void gen_type_def(codegen_ctx_t *ctx, fxsh_ast_node_t *ast);
static void emit_c_type_for_fxsh_type(codegen_ctx_t *ctx, sp_str_t name);
static bool is_type_con_named(fxsh_type_t *t, sp_str_t name);
static fxsh_type_t *nth_param_type(fxsh_type_t *fn_t, u32 idx);
static fxsh_type_t *return_type_of_fn(fxsh_type_t *fn_t);
static fxsh_type_t *return_type_after_n_args(fxsh_type_t *fn_t, u32 nargs);
static void emit_closure_type_name(codegen_ctx_t *ctx, sp_str_t root_fn, u32 stage);
static bool closure_expr_stage_of(fxsh_ast_node_t *expr, sp_str_t *out_root, u32 *out_stage);
static void emit_c_type_guess_for_expr(codegen_ctx_t *ctx, fxsh_ast_node_t *expr);
static void collect_mono_specs(fxsh_ast_node_t *ast);
static fxsh_type_t *infer_expr_type_strict_for_codegen(fxsh_ast_node_t *expr);
static fxsh_type_t *infer_expr_type_for_codegen_any(fxsh_ast_node_t *expr);
static fxsh_type_t *if_result_type_hint(fxsh_ast_node_t *if_ast);
static void emit_zero_init_for_type(codegen_ctx_t *ctx, fxsh_type_t *t);
static fxsh_type_t *record_field_type_hint(fxsh_type_t *t, sp_str_t field);
static void emit_unbox_from_boxed(codegen_ctx_t *ctx, fxsh_type_t *hint_t, const char *boxed_expr);
static void emit_closure_wrapper(codegen_ctx_t *ctx, sp_str_t public_name,
                                 const char *target_prefix, fxsh_type_t *fn_t);
static bool tco_has_only_var_params(fxsh_ast_list_t params);
static bool tco_has_tail_self_call(fxsh_ast_node_t *expr, sp_str_t self_name, u32 arity);
static void gen_tail_dispatch(codegen_ctx_t *ctx, fxsh_ast_node_t *expr, sp_str_t self_name,
                              fxsh_type_t *fn_t, fxsh_ast_list_t params, bool ret_unit);
static bool enable_tco_for_let_lambda(fxsh_ast_node_t *ast, fxsh_ast_node_t *lam);
static void gen_decl_let_lambda_body(codegen_ctx_t *ctx, fxsh_ast_node_t *ast, fxsh_ast_node_t *lam,
                                     fxsh_type_t *fn_t);

static bool cg_name_eq(sp_str_t n, const char *lit) {
    return sp_str_equal(n, sp_str_view((char *)lit));
}

static bool str_in_list(sp_dyn_array(sp_str_t) list, sp_str_t s) {
    sp_dyn_array_for(list, i) {
        if (sp_str_equal(list[i], s))
            return true;
    }
    return false;
}

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
typedef struct {
    sp_str_t constr_name;
    sp_str_t type_name;
    fxsh_ast_list_t arg_types;
    fxsh_ast_list_t type_params;
} adt_constr_sig_t;
static sp_dyn_array(adt_constr_sig_t) g_adt_constr_sigs = SP_NULLPTR;
static sp_dyn_array(sp_str_t) g_lambda_fn_names = SP_NULLPTR;
static sp_dyn_array(sp_str_t) g_closure_fn_names = SP_NULLPTR;
static sp_dyn_array(sp_str_t) g_decl_fn_names = SP_NULLPTR;
static sp_dyn_array(sp_str_t) g_extern_fn_names = SP_NULLPTR;
static fxsh_type_env_t g_codegen_type_env = SP_NULLPTR;
static fxsh_constr_env_t g_codegen_constr_env = SP_NULLPTR;
static u32 g_top_let_counter = 0;
typedef struct {
    sp_str_t name;
    fxsh_ast_node_t *decl;
} mono_decl_entry_t;
static sp_dyn_array(mono_decl_entry_t) g_mono_decls = SP_NULLPTR;
typedef struct {
    sp_str_t fn_name;
    sp_str_t c_name;
    fxsh_type_t *ret_type;
    sp_dyn_array(fxsh_type_t *) arg_types;
} mono_spec_t;
static sp_dyn_array(mono_spec_t) g_mono_specs = SP_NULLPTR;

typedef struct {
    sp_str_t name;
    fxsh_type_t *type;
} pat_var_type_entry_t;

static sp_dyn_array(pat_var_type_entry_t) g_pat_var_types = SP_NULLPTR;
static s32 g_pat_tvar_counter = 2000000000;

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

typedef struct {
    sp_str_t name;
    fxsh_type_t *type;
} local_type_entry_t;
static sp_dyn_array(local_type_entry_t) g_local_type_stack = SP_NULLPTR;

/* Track ADT-typed global let bindings that need runtime init */
typedef struct {
    sp_str_t c_name;        /* mangled variable name */
    sp_str_t type_name;     /* mangled type name */
    fxsh_ast_node_t *value; /* initializer expression */
} adt_init_t;
static sp_dyn_array(adt_init_t) g_adt_inits = SP_NULLPTR;

typedef struct {
    bool is_stmt;
    sp_str_t c_name;
    sp_str_t rhs_expr;
    fxsh_ast_node_t *expr;
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

static adt_constr_sig_t *constr_sig_lookup(sp_str_t constr) {
    sp_dyn_array_for(g_adt_constr_sigs, i) {
        if (sp_str_equal(g_adt_constr_sigs[i].constr_name, constr))
            return &g_adt_constr_sigs[i];
    }
    return NULL;
}

static fxsh_type_t *cg_type_from_type_ast_with_params(fxsh_ast_node_t *ast,
                                                      fxsh_ast_list_t param_names,
                                                      sp_dyn_array(fxsh_type_t *) param_types) {
    if (!ast)
        return fxsh_type_var(g_pat_tvar_counter--);
    switch (ast->kind) {
        case AST_IDENT:
            return fxsh_type_con(ast->data.ident);
        case AST_TYPE_VAR:
            sp_dyn_array_for(param_names, i) {
                fxsh_ast_node_t *pn = param_names[i];
                if (!pn)
                    continue;
                if (sp_str_equal(pn->data.ident, ast->data.ident) &&
                    i < sp_dyn_array_size(param_types) && param_types[i]) {
                    return param_types[i];
                }
            }
            return fxsh_type_var(g_pat_tvar_counter--);
        case AST_TYPE_ARROW:
            return fxsh_type_arrow(cg_type_from_type_ast_with_params(ast->data.type_arrow.param,
                                                                     param_names, param_types),
                                   cg_type_from_type_ast_with_params(ast->data.type_arrow.ret,
                                                                     param_names, param_types));
        case AST_TYPE_APP:
            if (!ast->data.type_con.args || sp_dyn_array_size(ast->data.type_con.args) != 2)
                return fxsh_type_var(g_pat_tvar_counter--);
            return fxsh_type_apply(cg_type_from_type_ast_with_params(ast->data.type_con.args[1],
                                                                     param_names, param_types),
                                   cg_type_from_type_ast_with_params(ast->data.type_con.args[0],
                                                                     param_names, param_types));
        case AST_TYPE_RECORD: {
            sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
            fxsh_type_var_t row_var = -1;
            sp_dyn_array_for(ast->data.elements, i) {
                fxsh_ast_node_t *e = ast->data.elements[i];
                if (!e)
                    continue;
                if (e->kind == AST_FIELD_ACCESS) {
                    fxsh_field_t f = {.name = e->data.field.field,
                                      .type = cg_type_from_type_ast_with_params(
                                          e->data.field.object, param_names, param_types)};
                    sp_dyn_array_push(fields, f);
                } else if (e->kind == AST_TYPE_VAR) {
                    row_var = g_pat_tvar_counter--;
                }
            }
            fxsh_type_t *t = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
            t->kind = TYPE_RECORD;
            t->data.record.fields = fields;
            t->data.record.row_var = row_var;
            return t;
        }
        default:
            return fxsh_type_var(g_pat_tvar_counter--);
    }
}

static bool ast_type_is_typevar(fxsh_ast_node_t *t) {
    if (!t)
        return false;
    if (t->kind == AST_TYPE_VAR)
        return true;
    if (t->kind == AST_IDENT) {
        return t->data.ident.len > 0 && t->data.ident.data && t->data.ident.data[0] == '\'';
    }
    return false;
}

static bool constr_arg_is_boxed_typevar(sp_str_t constr, u32 idx) {
    adt_constr_sig_t *sig = constr_sig_lookup(constr);
    if (!sig || !sig->arg_types || idx >= sp_dyn_array_size(sig->arg_types))
        return false;
    return ast_type_is_typevar(sig->arg_types[idx]);
}

static bool constr_arg_is_self_ptr(sp_str_t constr, u32 idx) {
    adt_constr_sig_t *sig = constr_sig_lookup(constr);
    if (!sig || !sig->arg_types || idx >= sp_dyn_array_size(sig->arg_types))
        return false;
    fxsh_ast_node_t *at = sig->arg_types[idx];
    if (!at || at->kind != AST_IDENT)
        return false;
    return sp_str_equal(at->data.ident, sig->type_name);
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

static bool is_extern_fn_name(sp_str_t name) {
    sp_dyn_array_for(g_extern_fn_names, i) {
        if (sp_str_equal(g_extern_fn_names[i], name))
            return true;
    }
    return false;
}

static bool lit_string_prefix(sp_str_t s, const char *prefix) {
    size_t n = strlen(prefix);
    if (!s.data || s.len < n)
        return false;
    return memcmp(s.data, prefix, n) == 0;
}

static bool let_is_ffi_decl(fxsh_ast_node_t *ast, sp_str_t *out_sym) {
    if (!ast || (ast->kind != AST_DECL_LET && ast->kind != AST_LET))
        return false;
    if (!ast->data.let.type || !ast->data.let.value || ast->data.let.value->kind != AST_LIT_STRING)
        return false;
    sp_str_t s = ast->data.let.value->data.lit_string;
    if (!lit_string_prefix(s, "c:"))
        return false;
    sp_str_t sym = {.data = s.data + 2, .len = s.len - 2};
    if (!sym.data || sym.len == 0)
        return false;
    if (out_sym)
        *out_sym = sym;
    return true;
}

static u32 fn_arity_of(fxsh_type_t *t) {
    u32 n = 0;
    while (t && t->kind == TYPE_ARROW) {
        n++;
        t = t->data.arrow.ret;
    }
    return n;
}

static bool fn_is_unit_arity(fxsh_type_t *fn_t) {
    return fn_t && fn_t->kind == TYPE_ARROW && fn_arity_of(fn_t) == 1 &&
           is_type_con_named(nth_param_type(fn_t, 0), TYPE_UNIT);
}

static bool fn_returns_unit(fxsh_type_t *fn_t) {
    return is_type_con_named(return_type_of_fn(fn_t), TYPE_UNIT);
}

static void emit_c_type_for_type(codegen_ctx_t *ctx, fxsh_type_t *t);

static bool type_is_string_con(fxsh_type_t *t) {
    return is_type_con_named(t, TYPE_STRING);
}

static bool type_is_c_abi_int_con(fxsh_type_t *t, sp_str_t *out_name) {
    while (t && t->kind == TYPE_APP)
        t = t->data.app.con;
    if (!t || t->kind != TYPE_CON)
        return false;
    sp_str_t n = t->data.con;
    if (sp_str_equal(n, TYPE_C_INT) || sp_str_equal(n, TYPE_C_UINT) ||
        sp_str_equal(n, TYPE_C_LONG) || sp_str_equal(n, TYPE_C_ULONG) ||
        sp_str_equal(n, TYPE_C_SIZE) || sp_str_equal(n, TYPE_C_SSIZE)) {
        if (out_name)
            *out_name = n;
        return true;
    }
    return false;
}

static void emit_c_abi_type_for_type(codegen_ctx_t *ctx, fxsh_type_t *t) {
    sp_str_t abi = (sp_str_t){0};
    if (type_is_c_abi_int_con(t, &abi)) {
        if (sp_str_equal(abi, TYPE_C_INT))
            emit_raw(ctx, "int");
        else if (sp_str_equal(abi, TYPE_C_UINT))
            emit_raw(ctx, "unsigned int");
        else if (sp_str_equal(abi, TYPE_C_LONG))
            emit_raw(ctx, "long");
        else if (sp_str_equal(abi, TYPE_C_ULONG))
            emit_raw(ctx, "unsigned long");
        else if (sp_str_equal(abi, TYPE_C_SIZE))
            emit_raw(ctx, "size_t");
        else
            emit_raw(ctx, "ssize_t");
        return;
    }
    if (type_is_string_con(t)) {
        emit_raw(ctx, "const char *");
        return;
    }
    emit_c_type_for_type(ctx, t);
}

static bool ast_is_unit_literal(fxsh_ast_node_t *n) {
    return n && n->kind == AST_LIT_UNIT;
}

static bool ast_type_is_unit_ident(fxsh_ast_node_t *t) {
    return t && t->kind == AST_IDENT && sp_str_equal(t->data.ident, TYPE_UNIT);
}

static fxsh_ast_node_t *ast_type_return_node(fxsh_ast_node_t *t) {
    while (t && t->kind == AST_TYPE_ARROW)
        t = t->data.type_arrow.ret;
    return t;
}

static bool ffi_decl_unit_arity_from_ast(fxsh_ast_node_t *decl) {
    if (!decl || (decl->kind != AST_DECL_LET && decl->kind != AST_LET) || !decl->data.let.type)
        return false;
    fxsh_ast_node_t *t = decl->data.let.type;
    return t->kind == AST_TYPE_ARROW && ast_type_is_unit_ident(t->data.type_arrow.param);
}

static bool ffi_decl_returns_unit_from_ast(fxsh_ast_node_t *decl) {
    if (!decl || (decl->kind != AST_DECL_LET && decl->kind != AST_LET) || !decl->data.let.type)
        return false;
    return ast_type_is_unit_ident(ast_type_return_node(decl->data.let.type));
}

static fxsh_type_t *cg_type_from_type_ast(fxsh_ast_node_t *ast) {
    return cg_type_from_type_ast_with_params(ast, SP_NULLPTR, SP_NULLPTR);
}

static fxsh_type_t *fn_type_from_let_annotation(fxsh_ast_node_t *let_ast) {
    if (!let_ast || (let_ast->kind != AST_DECL_LET && let_ast->kind != AST_LET))
        return NULL;
    if (!let_ast->data.let.type)
        return NULL;
    fxsh_type_t *t = cg_type_from_type_ast(let_ast->data.let.type);
    if (!t || t->kind != TYPE_ARROW)
        return NULL;
    return t;
}

static fxsh_type_t *let_binding_type_hint(fxsh_ast_node_t *let_ast) {
    if (!let_ast || (let_ast->kind != AST_DECL_LET && let_ast->kind != AST_LET))
        return NULL;
    if (let_ast->data.let.type)
        return cg_type_from_type_ast(let_ast->data.let.type);
    if (let_ast->data.let.value)
        return infer_expr_type_strict_for_codegen(let_ast->data.let.value);
    return NULL;
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

static u32 local_type_mark(void) {
    return (u32)sp_dyn_array_size(g_local_type_stack);
}

static void local_type_pop_to(u32 mark) {
    while (g_local_type_stack && sp_dyn_array_size(g_local_type_stack) > mark)
        sp_dyn_array_pop(g_local_type_stack);
}

static void local_type_push(sp_str_t name, fxsh_type_t *type) {
    if (!name.data || name.len == 0 || !type)
        return;
    local_type_entry_t e = {.name = name, .type = type};
    sp_dyn_array_push(g_local_type_stack, e);
}

static fxsh_type_t *local_type_lookup(sp_str_t name) {
    for (s32 i = (s32)sp_dyn_array_size(g_local_type_stack) - 1; i >= 0; i--) {
        if (sp_str_equal(g_local_type_stack[i].name, name))
            return g_local_type_stack[i].type;
    }
    return NULL;
}

static sp_str_t sym_lookup_expr(sp_str_t name) {
    for (s32 i = (s32)sp_dyn_array_size(g_sym_stack) - 1; i >= 0; i--) {
        if (sp_str_equal(g_sym_stack[i].src_name, name))
            return g_sym_stack[i].c_expr;
    }
    return (sp_str_t){0};
}

static fxsh_type_t *lookup_symbol_type(sp_str_t name) {
    fxsh_type_t *lt = local_type_lookup(name);
    if (lt)
        return lt;
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

static bool list_type_elem_hint(fxsh_type_t *t, fxsh_type_t **out_elem) {
    if (!t || t->kind != TYPE_APP)
        return false;
    fxsh_type_t *con = t->data.app.con;
    while (con && con->kind == TYPE_APP)
        con = con->data.app.con;
    if (!con || con->kind != TYPE_CON || !sp_str_equal(con->data.con, TYPE_LIST))
        return false;
    if (out_elem)
        *out_elem = t->data.app.arg;
    return true;
}

static bool ptr_type_elem_hint(fxsh_type_t *t, fxsh_type_t **out_elem) {
    if (!t || t->kind != TYPE_APP)
        return false;
    fxsh_type_t *con = t->data.app.con;
    while (con && con->kind == TYPE_APP)
        con = con->data.app.con;
    if (!con || con->kind != TYPE_CON || !sp_str_equal(con->data.con, TYPE_PTR))
        return false;
    if (out_elem)
        *out_elem = t->data.app.arg;
    return true;
}

static void emit_c_type_for_type(codegen_ctx_t *ctx, fxsh_type_t *t) {
    fxsh_type_t *list_elem = NULL;
    if (list_type_elem_hint(t, &list_elem)) {
        (void)list_elem;
        emit_raw(ctx, "fxsh_list_t*");
        return;
    }
    if (ptr_type_elem_hint(t, NULL)) {
        emit_raw(ctx, "void*");
        return;
    }
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
    if (t->kind == TYPE_TUPLE) {
        emit_raw(ctx, "fxsh_tuple_t");
        return;
    }
    if (t->kind == TYPE_APP) {
        fxsh_type_t *elem_t = NULL;
        if (list_type_elem_hint(t, &elem_t)) {
            (void)elem_t;
            emit_raw(ctx, "fxsh_list_t*");
            return;
        }
        if (ptr_type_elem_hint(t, &elem_t)) {
            (void)elem_t;
            emit_raw(ctx, "void*");
            return;
        }
    }
    if (t->kind == TYPE_ARROW) {
        emit_raw(ctx, "fxsh_closure_t");
        return;
    }
    emit_raw(ctx, "s64");
}

static void emit_c_storage_type_for_type(codegen_ctx_t *ctx, fxsh_type_t *t) {
    t = normalize_codegen_type(t);
    if (t && is_type_con_named(t, TYPE_UNIT)) {
        emit_raw(ctx, "s64");
        return;
    }
    emit_c_type_for_type(ctx, t);
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

static fxsh_type_t *return_type_after_n_args(fxsh_type_t *fn_t, u32 nargs) {
    fxsh_type_t *cur = fn_t;
    for (u32 i = 0; i < nargs; i++) {
        if (!cur || cur->kind != TYPE_ARROW)
            return NULL;
        cur = cur->data.arrow.ret;
    }
    return cur;
}

static bool is_type_con_named(fxsh_type_t *t, sp_str_t name) {
    t = normalize_codegen_type(t);
    return t && t->kind == TYPE_CON && sp_str_equal(t->data.con, name);
}

static mono_decl_entry_t *mono_decl_lookup(sp_str_t name) {
    sp_dyn_array_for(g_mono_decls, i) {
        if (sp_str_equal(g_mono_decls[i].name, name))
            return &g_mono_decls[i];
    }
    return NULL;
}

static bool type_has_var(fxsh_type_t *t) {
    if (!t)
        return true;
    switch (t->kind) {
        case TYPE_VAR:
            return true;
        case TYPE_CON:
            return false;
        case TYPE_ARROW:
            return type_has_var(t->data.arrow.param) || type_has_var(t->data.arrow.ret);
        case TYPE_APP:
            return type_has_var(t->data.app.con) || type_has_var(t->data.app.arg);
        case TYPE_TUPLE:
            sp_dyn_array_for(t->data.tuple, i) {
                if (type_has_var(t->data.tuple[i]))
                    return true;
            }
            return false;
        case TYPE_RECORD:
            sp_dyn_array_for(t->data.record.fields, i) {
                if (type_has_var(t->data.record.fields[i].type))
                    return true;
            }
            return t->data.record.row_var >= 0;
        default:
            return true;
    }
}

static fxsh_type_t *expr_mono_type_hint(fxsh_ast_node_t *e) {
    if (!e)
        return NULL;
    switch (e->kind) {
        case AST_LIT_INT:
            return fxsh_type_con(TYPE_INT);
        case AST_LIT_FLOAT:
            return fxsh_type_con(TYPE_FLOAT);
        case AST_LIT_BOOL:
            return fxsh_type_con(TYPE_BOOL);
        case AST_LIT_STRING:
            return fxsh_type_con(TYPE_STRING);
        case AST_LIT_UNIT:
            return fxsh_type_con(TYPE_UNIT);
        case AST_IDENT: {
            fxsh_type_t *t = lookup_symbol_type(e->data.ident);
            if (!t || type_has_var(t))
                return NULL;
            return t;
        }
        default:
            break;
    }
    fxsh_type_t *t = infer_expr_type_strict_for_codegen(e);
    if (!t || type_has_var(t))
        return NULL;
    return t;
}

static void type_tag_into(fxsh_type_t *t, char *buf, size_t buf_sz) {
    t = normalize_codegen_type(t);
    if (!t) {
        snprintf(buf, buf_sz, "unk");
        return;
    }
    if (t->kind == TYPE_CON) {
        if (sp_str_equal(t->data.con, TYPE_INT))
            snprintf(buf, buf_sz, "i64");
        else if (sp_str_equal(t->data.con, TYPE_FLOAT))
            snprintf(buf, buf_sz, "f64");
        else if (sp_str_equal(t->data.con, TYPE_BOOL))
            snprintf(buf, buf_sz, "bool");
        else if (sp_str_equal(t->data.con, TYPE_STRING))
            snprintf(buf, buf_sz, "str");
        else if (sp_str_equal(t->data.con, TYPE_UNIT))
            snprintf(buf, buf_sz, "unit");
        else
            snprintf(buf, buf_sz, "con");
        return;
    }
    if (t->kind == TYPE_APP) {
        fxsh_type_t *elem = NULL;
        if (list_type_elem_hint(t, &elem)) {
            char eb[32];
            type_tag_into(elem, eb, sizeof(eb));
            snprintf(buf, buf_sz, "list_%s", eb);
            return;
        }
        if (ptr_type_elem_hint(t, &elem)) {
            char eb[32];
            type_tag_into(elem, eb, sizeof(eb));
            snprintf(buf, buf_sz, "ptr_%s", eb);
            return;
        }
    }
    if (t->kind == TYPE_RECORD) {
        snprintf(buf, buf_sz, "record");
        return;
    }
    if (t->kind == TYPE_TUPLE) {
        snprintf(buf, buf_sz, "tuple");
        return;
    }
    snprintf(buf, buf_sz, "unk");
}

static bool mono_spec_same(mono_spec_t *s, sp_str_t fn, sp_dyn_array(fxsh_type_t *) arg_types) {
    if (!s || !sp_str_equal(s->fn_name, fn))
        return false;
    if (sp_dyn_array_size(s->arg_types) != sp_dyn_array_size(arg_types))
        return false;
    sp_dyn_array_for(s->arg_types, i) {
        const c8 *a = fxsh_type_to_string(s->arg_types[i]);
        const c8 *b = fxsh_type_to_string(arg_types[i]);
        if (strcmp(a ? a : "", b ? b : "") != 0)
            return false;
    }
    return true;
}

static void mono_try_register(sp_str_t fn_name, sp_dyn_array(fxsh_type_t *) call_arg_types) {
    mono_decl_entry_t *de = mono_decl_lookup(fn_name);
    if (!de || !de->decl || !de->decl->data.let.value ||
        de->decl->data.let.value->kind != AST_LAMBDA)
        return;

    fxsh_type_t *fn_t = lookup_symbol_type(fn_name);
    if (!fn_t || fn_t->kind != TYPE_ARROW || !type_has_var(fn_t))
        return;

    fxsh_type_t *ret_v = fxsh_type_var(g_pat_tvar_counter++);
    fxsh_type_t *exp = ret_v;
    for (s32 i = (s32)sp_dyn_array_size(call_arg_types) - 1; i >= 0; i--) {
        if (!call_arg_types[i] || type_has_var(call_arg_types[i]))
            return;
        exp = fxsh_type_arrow(call_arg_types[i], exp);
    }

    fxsh_subst_t subst = SP_NULLPTR;
    if (fxsh_type_unify(fn_t, exp, &subst) != ERR_OK)
        return;

    sp_dyn_array(fxsh_type_t *) mono_args = SP_NULLPTR;
    sp_dyn_array_for(call_arg_types, i) {
        fxsh_type_t *at = call_arg_types[i];
        fxsh_type_apply_subst(subst, &at);
        if (type_has_var(at))
            return;
        sp_dyn_array_push(mono_args, at);
    }
    fxsh_type_apply_subst(subst, &ret_v);
    if (type_has_var(ret_v))
        return;

    sp_dyn_array_for(g_mono_specs, i) {
        if (mono_spec_same(&g_mono_specs[i], fn_name, mono_args))
            return;
    }

    char fname[96];
    mangle_into(fn_name, fname, sizeof(fname));
    char cname[256];
    snprintf(cname, sizeof(cname), "fxsh_mono_%s", fname);
    size_t pos = strlen(cname);
    sp_dyn_array_for(mono_args, i) {
        char tb[32];
        type_tag_into(mono_args[i], tb, sizeof(tb));
        if (pos + strlen(tb) + 2 >= sizeof(cname))
            break;
        cname[pos++] = '_';
        strcpy(cname + pos, tb);
        pos += strlen(tb);
    }

    mono_spec_t spec = {
        .fn_name = fn_name,
        .c_name = make_owned_str(cname),
        .ret_type = ret_v,
        .arg_types = mono_args,
    };
    sp_dyn_array_push(g_mono_specs, spec);
}

static void collect_mono_specs_expr(fxsh_ast_node_t *ast) {
    if (!ast)
        return;
    switch (ast->kind) {
        case AST_PROGRAM:
            sp_dyn_array_for(ast->data.decls, i) collect_mono_specs_expr(ast->data.decls[i]);
            return;
        case AST_CALL: {
            fxsh_ast_list_t flat_args = SP_NULLPTR;
            fxsh_ast_node_t *func = ast;
            while (func && func->kind == AST_CALL) {
                sp_dyn_array_for(func->data.call.args, i) {
                    sp_dyn_array_push(flat_args, func->data.call.args[i]);
                }
                func = func->data.call.func;
            }
            if (func && func->kind == AST_IDENT && sp_dyn_array_size(flat_args) > 0) {
                sp_dyn_array(fxsh_type_t *) arg_types = SP_NULLPTR;
                bool ok = true;
                for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
                    fxsh_type_t *at = expr_mono_type_hint(flat_args[i]);
                    if (!at || type_has_var(at)) {
                        ok = false;
                        break;
                    }
                    sp_dyn_array_push(arg_types, at);
                }
                if (ok)
                    mono_try_register(func->data.ident, arg_types);
            }
            collect_mono_specs_expr(ast->data.call.func);
            sp_dyn_array_for(ast->data.call.args, i)
                collect_mono_specs_expr(ast->data.call.args[i]);
            return;
        }
        case AST_BINARY:
            collect_mono_specs_expr(ast->data.binary.left);
            collect_mono_specs_expr(ast->data.binary.right);
            return;
        case AST_UNARY:
            collect_mono_specs_expr(ast->data.unary.operand);
            return;
        case AST_IF:
            collect_mono_specs_expr(ast->data.if_expr.cond);
            collect_mono_specs_expr(ast->data.if_expr.then_branch);
            collect_mono_specs_expr(ast->data.if_expr.else_branch);
            return;
        case AST_LAMBDA:
            collect_mono_specs_expr(ast->data.lambda.body);
            return;
        case AST_LET_IN:
            sp_dyn_array_for(ast->data.let_in.bindings, i)
                collect_mono_specs_expr(ast->data.let_in.bindings[i]);
            collect_mono_specs_expr(ast->data.let_in.body);
            return;
        case AST_MATCH:
            collect_mono_specs_expr(ast->data.match_expr.expr);
            sp_dyn_array_for(ast->data.match_expr.arms, i)
                collect_mono_specs_expr(ast->data.match_expr.arms[i]);
            return;
        case AST_MATCH_ARM:
            collect_mono_specs_expr(ast->data.match_arm.guard);
            collect_mono_specs_expr(ast->data.match_arm.body);
            return;
        case AST_PIPE:
            collect_mono_specs_expr(ast->data.pipe.left);
            collect_mono_specs_expr(ast->data.pipe.right);
            return;
        case AST_TUPLE:
        case AST_LIST:
        case AST_RECORD:
        case AST_PAT_TUPLE:
        case AST_PAT_RECORD:
            sp_dyn_array_for(ast->data.elements, i) collect_mono_specs_expr(ast->data.elements[i]);
            return;
        case AST_RECORD_UPDATE:
            collect_mono_specs_expr(ast->data.record_update.base);
            sp_dyn_array_for(ast->data.record_update.updates, i)
                collect_mono_specs_expr(ast->data.record_update.updates[i]);
            return;
        case AST_FIELD_ACCESS:
            collect_mono_specs_expr(ast->data.field.object);
            return;
        case AST_DECL_LET:
        case AST_LET:
            collect_mono_specs_expr(ast->data.let.value);
            return;
        default:
            return;
    }
}

static void collect_mono_decl_entries(fxsh_ast_node_t *ast) {
    if (!ast)
        return;
    if (ast->kind == AST_PROGRAM) {
        sp_dyn_array_for(ast->data.decls, i) collect_mono_decl_entries(ast->data.decls[i]);
        return;
    }
    if ((ast->kind == AST_DECL_LET || ast->kind == AST_LET) && ast->data.let.value &&
        ast->data.let.value->kind == AST_LAMBDA) {
        mono_decl_entry_t e = {.name = ast->data.let.name, .decl = ast};
        sp_dyn_array_push(g_mono_decls, e);
    }
}

static void collect_mono_specs(fxsh_ast_node_t *ast) {
    g_mono_decls = SP_NULLPTR;
    g_mono_specs = SP_NULLPTR;
    if (!ast || ast->kind != AST_PROGRAM)
        return;

    collect_mono_decl_entries(ast);

    sp_dyn_array_for(ast->data.decls, i) {
        collect_mono_specs_expr(ast->data.decls[i]);
    }
}

static mono_spec_t *mono_spec_lookup_call(sp_str_t fn_name, fxsh_ast_list_t call_args_in_order) {
    sp_dyn_array(fxsh_type_t *) arg_types = SP_NULLPTR;
    sp_dyn_array_for(call_args_in_order, i) {
        fxsh_type_t *at = expr_mono_type_hint(call_args_in_order[i]);
        if (!at || type_has_var(at))
            return NULL;
        sp_dyn_array_push(arg_types, at);
    }

    sp_dyn_array_for(g_mono_specs, i) {
        if (mono_spec_same(&g_mono_specs[i], fn_name, arg_types))
            return &g_mono_specs[i];
    }
    return NULL;
}

static bool flatten_curried_lambda_groups(fxsh_ast_node_t *lam,
                                          sp_dyn_array(fxsh_ast_list_t) * out_groups,
                                          fxsh_ast_node_t **out_final_body) {
    if (!lam || lam->kind != AST_LAMBDA || !out_groups || !out_final_body)
        return false;
    fxsh_ast_node_t *cur = lam;
    fxsh_ast_node_t *final_body = NULL;
    while (cur && cur->kind == AST_LAMBDA) {
        if (!cur->data.lambda.params || sp_dyn_array_size(cur->data.lambda.params) == 0)
            return false;
        sp_dyn_array_push(*out_groups, cur->data.lambda.params);
        if (cur->data.lambda.body && cur->data.lambda.body->kind == AST_LAMBDA) {
            cur = cur->data.lambda.body;
        } else {
            final_body = cur->data.lambda.body;
            break;
        }
    }
    if (!final_body)
        return false;
    *out_final_body = final_body;
    return true;
}

static bool is_pat_var_node(fxsh_ast_node_t *p) {
    return p && p->kind == AST_PAT_VAR;
}

/*=============================================================================
 * Type Emission
 *=============================================================================*/

static void emit_c_type_for_fxsh_type(codegen_ctx_t *ctx, sp_str_t name) {
    if (sp_str_equal(name, TYPE_INT)) {
        emit_raw(ctx, "s64");
        return;
    }
    if (sp_str_equal(name, TYPE_C_INT) || sp_str_equal(name, TYPE_C_UINT) ||
        sp_str_equal(name, TYPE_C_LONG) || sp_str_equal(name, TYPE_C_ULONG) ||
        sp_str_equal(name, TYPE_C_SIZE) || sp_str_equal(name, TYPE_C_SSIZE)) {
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
    if (sp_str_equal(name, TYPE_TYPE)) {
        emit_raw(ctx, "fxsh_type_t*");
        return;
    }
    if (sp_str_equal(name, TYPE_UNIT)) {
        emit_raw(ctx, "void");
        return;
    }
    if (sp_str_equal(name, TYPE_LIST)) {
        emit_raw(ctx, "fxsh_list_t*");
        return;
    }
    if (sp_str_equal(name, TYPE_PTR)) {
        emit_raw(ctx, "void*");
        return;
    }
    if (sp_str_equal(name, TYPE_TENSOR)) {
        emit_raw(ctx, "fxsh_tensor_t*");
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
        adt_constr_sig_t sig = {.constr_name = c->data.data_constr.name,
                                .type_name = type_name,
                                .arg_types = c->data.data_constr.arg_types,
                                .type_params = ast->data.type_def.type_params};
        sp_dyn_array_push(g_adt_constr_sigs, sig);
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
            } else if (ast_type_is_typevar(arg_t)) {
                emit_raw(ctx, "fxsh_value_t ");
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
                } else if (ast_type_is_typevar(at)) {
                    emit_raw(ctx, "fxsh_value_t ");
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
            emit_c_escaped_str(ctx, ast->data.lit_string);
            emit_fmt(ctx, "\", .len = %u })", (unsigned)ast->data.lit_string.len);
            break;
        case AST_LIT_UNIT:
            emit_raw(ctx, "0 /* unit */");
            break;
        case AST_TYPE_VALUE:
            if (!ast->data.type_value) {
                emit_raw(ctx, "NULL");
                break;
            }
            switch (ast->data.type_value->kind) {
                case TYPE_VAR:
                    emit_fmt(ctx, "fxsh_type_var(%d)", ast->data.type_value->data.var);
                    break;
                case TYPE_CON:
                    emit_fmt(ctx, "fxsh_type_con((sp_str_t){ .data = \"%.*s\", .len = %u })",
                             ast->data.type_value->data.con.len,
                             ast->data.type_value->data.con.data,
                             (unsigned)ast->data.type_value->data.con.len);
                    break;
                case TYPE_ARROW: {
                    fxsh_ast_node_t lhs = {.kind = AST_TYPE_VALUE};
                    fxsh_ast_node_t rhs = {.kind = AST_TYPE_VALUE};
                    lhs.data.type_value = ast->data.type_value->data.arrow.param;
                    rhs.data.type_value = ast->data.type_value->data.arrow.ret;
                    emit_raw(ctx, "fxsh_type_arrow(");
                    gen_literal(ctx, &lhs);
                    emit_raw(ctx, ", ");
                    gen_literal(ctx, &rhs);
                    emit_raw(ctx, ")");
                    break;
                }
                case TYPE_APP: {
                    fxsh_ast_node_t con = {.kind = AST_TYPE_VALUE};
                    fxsh_ast_node_t arg = {.kind = AST_TYPE_VALUE};
                    con.data.type_value = ast->data.type_value->data.app.con;
                    arg.data.type_value = ast->data.type_value->data.app.arg;
                    emit_raw(ctx, "fxsh_type_apply(");
                    gen_literal(ctx, &con);
                    emit_raw(ctx, ", ");
                    gen_literal(ctx, &arg);
                    emit_raw(ctx, ")");
                    break;
                }
                case TYPE_TUPLE: {
                    emit_raw(ctx, "({ fxsh_type_t *_t = fxsh_type_tuple_make(");
                    emit_fmt(ctx, "%u",
                             (unsigned)sp_dyn_array_size(ast->data.type_value->data.tuple));
                    emit_raw(ctx, "); ");
                    sp_dyn_array_for(ast->data.type_value->data.tuple, i) {
                        fxsh_ast_node_t elem = {.kind = AST_TYPE_VALUE};
                        elem.data.type_value = ast->data.type_value->data.tuple[i];
                        emit_fmt(ctx, "fxsh_type_tuple_set(_t, %u, ", (unsigned)i);
                        gen_literal(ctx, &elem);
                        emit_raw(ctx, "); ");
                    }
                    emit_raw(ctx, "_t; })");
                    break;
                }
                case TYPE_RECORD: {
                    emit_raw(ctx, "({ fxsh_type_t *_t = fxsh_type_record_make(");
                    emit_fmt(ctx, "%u, %d",
                             (unsigned)sp_dyn_array_size(ast->data.type_value->data.record.fields),
                             ast->data.type_value->data.record.row_var);
                    emit_raw(ctx, "); ");
                    sp_dyn_array_for(ast->data.type_value->data.record.fields, i) {
                        fxsh_field_t *f = &ast->data.type_value->data.record.fields[i];
                        fxsh_ast_node_t ft = {.kind = AST_TYPE_VALUE};
                        ft.data.type_value = f->type;
                        emit_fmt(ctx, "fxsh_type_record_set(_t, %u, (sp_str_t){ .data = \"",
                                 (unsigned)i);
                        emit_c_escaped_str(ctx, f->name);
                        emit_fmt(ctx, "\", .len = %u }, ", (unsigned)f->name.len);
                        gen_literal(ctx, &ft);
                        emit_raw(ctx, "); ");
                    }
                    emit_raw(ctx, "_t; })");
                    break;
                }
                default:
                    emit_raw(ctx, "NULL /* unsupported type literal */");
                    break;
            }
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
    if (ast->data.binary.op == TOK_APPEND) {
        emit_raw(ctx, "fxsh_list_cons(");
        gen_boxed_expr(ctx, ast->data.binary.left);
        emit_raw(ctx, ", ");
        gen_expr(ctx, ast->data.binary.right);
        emit_raw(ctx, ")");
        return;
    }
    if (ast->data.binary.op == TOK_EQ || ast->data.binary.op == TOK_NEQ) {
        fxsh_type_t *left_t = infer_expr_type_strict_for_codegen(ast->data.binary.left);
        fxsh_type_t *right_t = infer_expr_type_strict_for_codegen(ast->data.binary.right);
        bool str_cmp = is_type_con_named(left_t, TYPE_STRING) ||
                       is_type_con_named(right_t, TYPE_STRING) ||
                       (ast->data.binary.left && ast->data.binary.left->kind == AST_LIT_STRING) ||
                       (ast->data.binary.right && ast->data.binary.right->kind == AST_LIT_STRING);
        if (str_cmp) {
            if (ast->data.binary.op == TOK_NEQ)
                emit_raw(ctx, "(!");
            emit_raw(ctx, "fxsh_str_eq(");
            gen_expr(ctx, ast->data.binary.left);
            emit_raw(ctx, ", ");
            gen_expr(ctx, ast->data.binary.right);
            emit_raw(ctx, ")");
            if (ast->data.binary.op == TOK_NEQ)
                emit_raw(ctx, ")");
            return;
        }
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
    if (!ast->data.if_expr.else_branch) {
        emit_raw(ctx, "(");
        gen_expr(ctx, ast->data.if_expr.cond);
        emit_raw(ctx, " ? ");
        gen_expr(ctx, ast->data.if_expr.then_branch);
        emit_raw(ctx, " : 0 /* no else */)");
        return;
    }
    fxsh_type_t *res_t = if_result_type_hint(ast);
    if (!res_t) {
        emit_raw(ctx, "(");
        gen_expr(ctx, ast->data.if_expr.cond);
        emit_raw(ctx, " ? ");
        gen_expr(ctx, ast->data.if_expr.then_branch);
        emit_raw(ctx, " : ");
        gen_expr(ctx, ast->data.if_expr.else_branch);
        emit_raw(ctx, ")");
        return;
    }
    if (is_type_con_named(res_t, TYPE_UNIT)) {
        emit_raw(ctx, "({ if (");
        gen_expr(ctx, ast->data.if_expr.cond);
        emit_raw(ctx, ") { ");
        gen_expr(ctx, ast->data.if_expr.then_branch);
        emit_raw(ctx, "; } else { ");
        gen_expr(ctx, ast->data.if_expr.else_branch);
        emit_raw(ctx, "; } 0; })");
        return;
    }
    emit_raw(ctx, "({ ");
    emit_c_type_for_type(ctx, res_t);
    emit_raw(ctx, " _if_res = ");
    emit_zero_init_for_type(ctx, res_t);
    emit_raw(ctx, "; if (");
    gen_expr(ctx, ast->data.if_expr.cond);
    emit_raw(ctx, ") _if_res = ");
    gen_expr(ctx, ast->data.if_expr.then_branch);
    emit_raw(ctx, "; else _if_res = ");
    gen_expr(ctx, ast->data.if_expr.else_branch);
    emit_raw(ctx, "; _if_res; })");
    return;
}

static fxsh_type_t *if_result_type_hint(fxsh_ast_node_t *if_ast) {
    if (!if_ast || if_ast->kind != AST_IF)
        return NULL;
    fxsh_type_t *tt = infer_expr_type_strict_for_codegen(if_ast->data.if_expr.then_branch);
    fxsh_type_t *et = infer_expr_type_strict_for_codegen(if_ast->data.if_expr.else_branch);
    if (tt && et) {
        fxsh_subst_t subst = SP_NULLPTR;
        fxsh_type_t *lhs = tt;
        fxsh_type_t *rhs = et;
        if (fxsh_type_unify(lhs, rhs, &subst) == ERR_OK) {
            fxsh_type_apply_subst(subst, &lhs);
            return lhs;
        }
        if (!type_has_var(tt))
            return tt;
        if (!type_has_var(et))
            return et;
    } else if (tt) {
        return tt;
    } else if (et) {
        return et;
    }
    fxsh_type_t *t = infer_expr_type_strict_for_codegen(if_ast);
    return t;
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
    if (func->kind == AST_LAMBDA) {
        u32 nargs = (u32)sp_dyn_array_size(flat_args);
        fxsh_ast_list_t params = func->data.lambda.params;
        u32 nparams = (u32)sp_dyn_array_size(params);
        bool simple_params = true;
        sp_dyn_array_for(params, i) {
            if (!is_pat_var_node(params[i])) {
                simple_params = false;
                break;
            }
        }
        if (simple_params && nargs == nparams) {
            fxsh_type_t *lam_t = infer_expr_type_for_codegen_any(func);
            u32 sym_m = sym_mark();
            u32 lty_m = local_type_mark();
            emit_raw(ctx, "({\n");
            ctx->indent_level++;
            for (u32 i = 0; i < nparams; i++) {
                fxsh_ast_node_t *p = params[i];
                fxsh_type_t *pt = nth_param_type(lam_t, i);
                char name_buf[256];
                char idx_buf[32];
                snprintf(idx_buf, sizeof(idx_buf), "__lam%u", ctx->temp_var_counter++);
                size_t pos = 0;
                for (u32 j = 0; j < p->data.ident.len && pos + 2 < sizeof(name_buf); j++) {
                    c8 c = p->data.ident.data[j];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_') {
                        name_buf[pos++] = c;
                    } else {
                        name_buf[pos++] = '_';
                    }
                }
                name_buf[pos] = '\0';
                strncat(name_buf, idx_buf, sizeof(name_buf) - strlen(name_buf) - 1);
                sp_str_t cname = make_owned_str(name_buf);
                emit_indent(ctx);
                emit_raw(ctx, "__auto_type ");
                emit_string(ctx, cname);
                emit_raw(ctx, " = ");
                gen_expr(ctx, flat_args[(s32)nargs - 1 - (s32)i]);
                emit_raw(ctx, ";\n");
                sym_push_expr(p->data.ident, cname);
                if (pt)
                    local_type_push(p->data.ident, pt);
            }
            emit_indent(ctx);
            gen_expr(ctx, func->data.lambda.body);
            emit_raw(ctx, ";\n");
            local_type_pop_to(lty_m);
            sym_pop_to(sym_m);
            ctx->indent_level--;
            emit_indent(ctx);
            emit_raw(ctx, "})");
            sp_dyn_array_free(flat_args);
            return;
        }
    }
    if (func->kind == AST_IDENT) {
        sp_str_t fname = func->data.ident;
        fxsh_type_t *ft = lookup_symbol_type(fname);
        bool extern_unit_arity = is_extern_fn_name(fname) && fn_is_unit_arity(ft);
        if (extern_unit_arity && sp_dyn_array_size(flat_args) == 1 &&
            ast_is_unit_literal(flat_args[0])) {
            gen_expr(ctx, func);
            emit_raw(ctx, "()");
            sp_dyn_array_free(flat_args);
            return;
        }
        bool allow_builtin =
            !is_lambda_fn_name(fname) && !is_decl_fn_name(fname) && !sym_lookup_expr(fname).data;
        bool force_builtin = cg_name_eq(fname, "exec_pipefail_capture") ||
                             cg_name_eq(fname, "exec_pipefail3_capture") ||
                             cg_name_eq(fname, "exec_pipefail4_capture") ||
                             cg_name_eq(fname, "capture_release");
        bool can_builtin = allow_builtin || force_builtin;
        if (can_builtin && cg_name_eq(fname, "print")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "({ sp_str_t _s = ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, "; printf(\"%.*s\\n\", (int)_s.len, _s.data); 0; })");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "argv0")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_argv0_rt()");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "argc")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_argc_rt()");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "argv_at")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_argv_at_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "getenv")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_getenv_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "cwd")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_getcwd_rt()");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "file_exists")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_file_exists_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "is_dir")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_is_dir_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "is_file")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_is_file_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "read_file")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_read_file(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "file_size")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_file_size_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "mkdir_p")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_mkdir_p_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "remove_file")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_remove_file_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "rename_path")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_rename_path_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "string_length")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_str_len_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "string_slice")) {
            if (sp_dyn_array_size(flat_args) == 3) {
                emit_raw(ctx, "fxsh_str_slice_rt(");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "string_find")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_str_find_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "string_find_from")) {
            if (sp_dyn_array_size(flat_args) == 3) {
                emit_raw(ctx, "fxsh_str_find_from_rt(");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "string_starts_with")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_str_starts_with_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "string_ends_with")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_str_ends_with_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "string_trim")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_str_trim_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "byte_at")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_byte_at_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "byte_to_string")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_byte_to_string_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "write_file")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "(fxsh_write_file(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ") == 0)");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_exec_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_code")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_exec_code_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_stdout")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_exec_stdout_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_stderr")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_exec_stderr_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_capture")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_exec_capture_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "capture_code")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_capture_code_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "capture_stdout")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_capture_stdout_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "capture_stderr")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_capture_stderr_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "capture_release")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_capture_release_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_stdin")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_stdin_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_stdin_code")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_stdin_code_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_stdin_capture")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_stdin_capture_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_stdin_stderr")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_stdin_stderr_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_pipe")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_pipe_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_pipe_code")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_pipe_code_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_pipe_capture")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_pipe_capture_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_pipefail_capture")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_pipefail_capture_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_pipefail3_capture")) {
            if (sp_dyn_array_size(flat_args) == 3) {
                emit_raw(ctx, "fxsh_exec_pipefail3_capture_rt(");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_pipefail4_capture")) {
            if (sp_dyn_array_size(flat_args) == 4) {
                emit_raw(ctx, "fxsh_exec_pipefail4_capture_rt(");
                gen_expr(ctx, flat_args[3]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "exec_pipe_stderr")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_exec_pipe_stderr_rt(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "glob")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_glob_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "list_dir")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_lines_to_list(fxsh_list_dir_text_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, "))");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "walk_dir")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_lines_to_list(fxsh_walk_dir_text_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, "))");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "split_lines")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_lines_to_list(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "split_words")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_lines_to_list(fxsh_split_words_rt(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, "))");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "grep_lines")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_grep_lines_regex(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "replace_once")) {
            if (sp_dyn_array_size(flat_args) == 3) {
                emit_raw(ctx, "fxsh_replace_once(");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_validate")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_json_validate(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_compact")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_json_compact(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_kind")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_json_kind(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_quote_string")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_json_quote_string(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_has")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_json_has(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_get")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_json_get(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_get_string")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_json_get_string(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_get_int")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_json_get_int(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ", NULL)");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_get_float")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_json_get_float(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ", NULL)");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "json_get_bool")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_json_get_bool(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ", NULL)");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "c_null")) {
            if (sp_dyn_array_size(flat_args) == 1 && ast_is_unit_literal(flat_args[0])) {
                emit_raw(ctx, "((void*)0)");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "c_malloc")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "((void*)malloc((size_t)(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")))");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "c_free")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "({ free((void*)(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")); })");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "c_cast_ptr")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "((void*)(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, "))");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "c_callback0")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                fxsh_type_t *cb_t = infer_expr_type_for_codegen_any(flat_args[0]);
                if (cb_t && cb_t->kind == TYPE_ARROW) {
                    emit_raw(ctx, "({ __auto_type _cb = ");
                    gen_expr(ctx, flat_args[0]);
                    emit_raw(ctx, "; (void*)(_cb.call); })");
                } else {
                    emit_raw(ctx, "((void*)(");
                    gen_expr(ctx, flat_args[0]);
                    emit_raw(ctx, "))");
                }
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "c_callback1_ptr")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "((void*)(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, "))");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin &&
            (cg_name_eq(fname, "int_to_c_int") || cg_name_eq(fname, "c_int_to_int") ||
             cg_name_eq(fname, "int_to_c_uint") || cg_name_eq(fname, "c_uint_to_int") ||
             cg_name_eq(fname, "int_to_c_long") || cg_name_eq(fname, "c_long_to_int") ||
             cg_name_eq(fname, "int_to_c_ulong") || cg_name_eq(fname, "c_ulong_to_int") ||
             cg_name_eq(fname, "int_to_c_size") || cg_name_eq(fname, "c_size_to_int") ||
             cg_name_eq(fname, "int_to_c_ssize") || cg_name_eq(fname, "c_ssize_to_int"))) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "((");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, "))");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "tensor_new2")) {
            if (sp_dyn_array_size(flat_args) == 3) {
                emit_raw(ctx, "fxsh_tensor_new2(");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "tensor_from_list2")) {
            if (sp_dyn_array_size(flat_args) == 3) {
                emit_raw(ctx, "fxsh_tensor_from_list2(");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "tensor_shape2")) {
            if (sp_dyn_array_size(flat_args) == 1) {
                emit_raw(ctx, "fxsh_tensor_shape2(");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "tensor_get2")) {
            if (sp_dyn_array_size(flat_args) == 3) {
                emit_raw(ctx, "fxsh_tensor_get2(");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "tensor_set2")) {
            if (sp_dyn_array_size(flat_args) == 4) {
                emit_raw(ctx, "fxsh_tensor_set2(");
                gen_expr(ctx, flat_args[3]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[2]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "tensor_add")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_tensor_add(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        if (can_builtin && cg_name_eq(fname, "tensor_dot")) {
            if (sp_dyn_array_size(flat_args) == 2) {
                emit_raw(ctx, "fxsh_tensor_dot(");
                gen_expr(ctx, flat_args[1]);
                emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[0]);
                emit_raw(ctx, ")");
                sp_dyn_array_free(flat_args);
                return;
            }
        }
        fxsh_ast_list_t args_in_order = SP_NULLPTR;
        for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
            sp_dyn_array_push(args_in_order, flat_args[i]);
        }
        mono_spec_t *ms = mono_spec_lookup_call(fname, args_in_order);
        if (ms) {
            emit_string(ctx, ms->c_name);
            emit_raw(ctx, "(");
            for (u32 i = 0; i < (u32)sp_dyn_array_size(args_in_order); i++) {
                if (i > 0)
                    emit_raw(ctx, ", ");
                gen_expr(ctx, args_in_order[i]);
            }
            emit_raw(ctx, ")");
            sp_dyn_array_free(args_in_order);
            sp_dyn_array_free(flat_args);
            return;
        }
        sp_dyn_array_free(args_in_order);

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

        if (!is_lambda_fn_name(fname) && !is_decl_fn_name(fname) && !is_extern_fn_name(fname) &&
            ft && ft->kind == TYPE_ARROW) {
            u32 nargs = (u32)sp_dyn_array_size(flat_args);
            if (nargs >= 1) {
                emit_raw(ctx, "({ __auto_type _c = ");
                gen_expr(ctx, func);
                emit_raw(ctx, "; ((");
                emit_c_type_for_type(ctx, return_type_after_n_args(ft, nargs));
                emit_raw(ctx, " (*)(void *env");
                for (u32 i = 0; i < nargs; i++) {
                    emit_raw(ctx, ", ");
                    emit_c_type_for_type(ctx, nth_param_type(ft, i));
                }
                emit_raw(ctx, "))_c.call)(_c.env");
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
    if (func->kind == AST_IDENT) {
        sp_str_t fname = func->data.ident;
        sp_str_t mapped = sym_lookup_expr(fname);
        fxsh_type_t *ft = lookup_symbol_type(fname);
        if (mapped.len > 0 && ft && ft->kind == TYPE_ARROW && !closure_value_lookup(fname)) {
            emit_string(ctx, mapped);
            sp_dyn_array_push(*ctx->output, '(');
            for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
                if (i != (s32)sp_dyn_array_size(flat_args) - 1)
                    emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[i]);
            }
            sp_dyn_array_push(*ctx->output, ')');
            sp_dyn_array_free(flat_args);
            return;
        }
        if (is_lambda_fn_name(fname) || is_decl_fn_name(fname) || is_extern_fn_name(fname)) {
            if (is_lambda_fn_name(fname)) {
                emit_raw(ctx, "fxsh_fn_");
                emit_mangled(ctx, fname);
            } else if (is_extern_fn_name(fname)) {
                if (mapped.len > 0)
                    emit_string(ctx, mapped);
                else
                    emit_mangled(ctx, fname);
            } else {
                emit_mangled(ctx, fname);
            }
            sp_dyn_array_push(*ctx->output, '(');
            for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
                if (i != (s32)sp_dyn_array_size(flat_args) - 1)
                    emit_raw(ctx, ", ");
                gen_expr(ctx, flat_args[i]);
            }
            sp_dyn_array_push(*ctx->output, ')');
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
        case AST_TYPE_VALUE:
            emit_raw(ctx, "fxsh_type_t*");
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
        case AST_RECORD_UPDATE:
            emit_raw(ctx, "fxsh_record_t");
            return;
        case AST_TUPLE:
            emit_raw(ctx, "fxsh_tuple_t");
            return;
        case AST_LIST:
            emit_raw(ctx, "fxsh_list_t*");
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
        case AST_TYPE_VALUE:
            emit_raw(ctx, "fxsh_box_ptr((void*)");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_RECORD:
        case AST_RECORD_UPDATE:
            emit_raw(ctx, "fxsh_box_ptr((void*)");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_TUPLE:
            emit_raw(ctx, "fxsh_box_tuple(");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_LIST:
            emit_raw(ctx, "fxsh_box_list(");
            gen_expr(ctx, expr);
            emit_raw(ctx, ")");
            return;
        case AST_IDENT: {
            fxsh_type_t *t = lookup_symbol_type(expr->data.ident);
            if (list_type_elem_hint(t, NULL)) {
                emit_raw(ctx, "fxsh_box_list(");
                gen_expr(ctx, expr);
                emit_raw(ctx, ")");
                return;
            }
            t = normalize_codegen_type(t);
            if (t && t->kind == TYPE_RECORD) {
                emit_raw(ctx, "fxsh_box_ptr((void*)");
                gen_expr(ctx, expr);
                emit_raw(ctx, ")");
                return;
            }
            if (t && t->kind == TYPE_TUPLE) {
                emit_raw(ctx, "fxsh_box_tuple(");
                gen_expr(ctx, expr);
                emit_raw(ctx, ")");
                return;
            }
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
        case AST_CALL:
        case AST_FIELD_ACCESS:
        case AST_BINARY:
        case AST_UNARY:
        case AST_IF:
        case AST_LET_IN:
        case AST_MATCH:
        case AST_PIPE: {
            fxsh_type_t *t = infer_expr_type_strict_for_codegen(expr);
            if (list_type_elem_hint(t, NULL)) {
                emit_raw(ctx, "fxsh_box_list(");
                gen_expr(ctx, expr);
                emit_raw(ctx, ")");
                return;
            }
            t = normalize_codegen_type(t);
            if (t && t->kind == TYPE_RECORD) {
                emit_raw(ctx, "fxsh_box_ptr((void*)");
                gen_expr(ctx, expr);
                emit_raw(ctx, ")");
                return;
            }
            if (t && t->kind == TYPE_TUPLE) {
                emit_raw(ctx, "fxsh_box_tuple(");
                gen_expr(ctx, expr);
                emit_raw(ctx, ")");
                return;
            }
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

static void gen_tuple(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "({ ");
    emit_raw(ctx, "fxsh_tuple_t _t = fxsh_tuple_make(");
    emit_fmt(ctx, "%u", (unsigned)sp_dyn_array_size(ast->data.elements));
    emit_raw(ctx, "); ");
    sp_dyn_array_for(ast->data.elements, i) {
        emit_raw(ctx, "fxsh_tuple_set(&_t, ");
        emit_fmt(ctx, "%u", (unsigned)i);
        emit_raw(ctx, ", ");
        gen_boxed_expr(ctx, ast->data.elements[i]);
        emit_raw(ctx, "); ");
    }
    emit_raw(ctx, "_t; })");
}

static void gen_list(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "({ ");
    emit_raw(ctx, "fxsh_list_t* _l = fxsh_list_nil(); ");
    for (s32 i = (s32)sp_dyn_array_size(ast->data.elements) - 1; i >= 0; i--) {
        emit_raw(ctx, "_l = fxsh_list_cons(");
        gen_boxed_expr(ctx, ast->data.elements[i]);
        emit_raw(ctx, ", _l); ");
    }
    emit_raw(ctx, "_l; })");
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

static void gen_record_update(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    emit_raw(ctx, "({ ");
    emit_raw(ctx, "fxsh_record_t _b = ");
    gen_expr(ctx, ast->data.record_update.base);
    emit_raw(ctx, "; ");
    emit_raw(ctx, "fxsh_record_t _r = fxsh_record_make(_b.len + ");
    emit_fmt(ctx, "%u", (unsigned)sp_dyn_array_size(ast->data.record_update.updates));
    emit_raw(ctx, "); ");
    emit_raw(ctx, "for (u32 _i = 0; _i < _b.len; _i++) fxsh_record_set(&_r, _i, _b.names[_i], _b.vals[_i]); ");

    sp_dyn_array_for(ast->data.record_update.updates, i) {
        fxsh_ast_node_t *u = ast->data.record_update.updates[i];
        if (!u || u->kind != AST_FIELD_ACCESS)
            continue;
        emit_raw(ctx, "({ bool _repl = false; for (u32 _j = 0; _j < _r.len; _j++) { if (_r.names[_j] && strcmp(_r.names[_j], \"");
        emit_string(ctx, u->data.field.field);
        emit_raw(ctx, "\") == 0) { _r.vals[_j] = ");
        gen_boxed_expr(ctx, u->data.field.object);
        emit_raw(ctx, "; _repl = true; break; } } if (!_repl) fxsh_record_set(&_r, _b.len + ");
        emit_fmt(ctx, "%u", (unsigned)i);
        emit_raw(ctx, ", \"");
        emit_string(ctx, u->data.field.field);
        emit_raw(ctx, "\", ");
        gen_boxed_expr(ctx, u->data.field.object);
        emit_raw(ctx, "); }); ");
    }
    emit_raw(ctx, "_r; })");
}

static void gen_field_access(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    fxsh_type_t *field_t = infer_expr_type_strict_for_codegen(ast);
    emit_raw(ctx, "({ fxsh_value_t _rf = fxsh_record_get(");
    gen_expr(ctx, ast->data.field.object);
    emit_raw(ctx, ", \"");
    emit_string(ctx, ast->data.field.field);
    emit_raw(ctx, "\"); ");
    emit_unbox_from_boxed(ctx, field_t, "_rf");
    emit_raw(ctx, "; })");
}

static void gen_let_in(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Use GCC statement expression ({ ... }) */
    emit_raw(ctx, "({\n");
    ctx->indent_level++;
    u32 sym_m = sym_mark();
    u32 lty_m = local_type_mark();
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
                fxsh_type_t *bt = let_binding_type_hint(b);
                if (bt && !type_has_var(bt) && !is_type_con_named(bt, TYPE_UNIT)) {
                    emit_c_type_for_type(ctx, bt);
                    emit_raw(ctx, " ");
                } else {
                    emit_raw(ctx, "__auto_type ");
                }
            }
            emit_string(ctx, cname);
            emit_raw(ctx, " = ");
            gen_expr(ctx, b->data.let.value);
            emit_raw(ctx, ";\n");

            sym_push_expr(b->data.let.name, cname);
            local_type_push(b->data.let.name, let_binding_type_hint(b));
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
    local_type_pop_to(lty_m);
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
        if (constr_arg_is_boxed_typevar(ast->data.constr_appl.constr_name, (u32)i)) {
            fxsh_type_t *at = infer_expr_type_strict_for_codegen(ast->data.constr_appl.args[i]);
            at = normalize_codegen_type(at);
            bool adt_ptr_box = false;
            if (at && at->kind == TYPE_CON && !sp_str_equal(at->data.con, TYPE_INT) &&
                !sp_str_equal(at->data.con, TYPE_FLOAT) && !sp_str_equal(at->data.con, TYPE_BOOL) &&
                !sp_str_equal(at->data.con, TYPE_STRING) &&
                !sp_str_equal(at->data.con, TYPE_UNIT) && is_known_adt(at->data.con)) {
                adt_ptr_box = true;
            }
            if (adt_ptr_box) {
                emit_raw(ctx, "({ __auto_type _bx_v = ");
                gen_expr(ctx, ast->data.constr_appl.args[i]);
                emit_raw(
                    ctx,
                    "; typeof(_bx_v) *_bx_p = (typeof(_bx_v)*)malloc(sizeof(typeof(_bx_v))); ");
                emit_raw(ctx, "if (_bx_p) *_bx_p = _bx_v; fxsh_box_ptr((void*)_bx_p); })");
            } else {
                emit_raw(ctx, "fxsh_autobox(");
                gen_expr(ctx, ast->data.constr_appl.args[i]);
                emit_raw(ctx, ")");
            }
        } else if (constr_arg_is_self_ptr(ast->data.constr_appl.constr_name, (u32)i)) {
            emit_raw(ctx, "({ __auto_type _self_arg = ");
            gen_expr(ctx, ast->data.constr_appl.args[i]);
            emit_raw(ctx, "; &_self_arg; })");
        } else
            gen_expr(ctx, ast->data.constr_appl.args[i]);
    }
    sp_dyn_array_push(*ctx->output, ')');
}

/*=============================================================================
 * Pattern Matching — switch on tag
 *=============================================================================*/

/* Generate code to bind pattern variables from `val_name` */
static fxsh_type_t *guess_expr_type_for_codegen(fxsh_ast_node_t *expr);
static fxsh_type_t *infer_expr_type_strict_for_codegen(fxsh_ast_node_t *expr);

static fxsh_type_t *tuple_elem_type_hint(fxsh_type_t *t, u32 idx) {
    if (!t)
        return NULL;
    t = normalize_codegen_type(t);
    if (!t || t->kind != TYPE_TUPLE)
        return NULL;
    if (idx >= sp_dyn_array_size(t->data.tuple))
        return NULL;
    return t->data.tuple[idx];
}

static fxsh_type_t *record_field_type_hint(fxsh_type_t *t, sp_str_t field) {
    if (!t)
        return NULL;
    t = normalize_codegen_type(t);
    if (!t || t->kind != TYPE_RECORD)
        return NULL;
    sp_dyn_array_for(t->data.record.fields, i) {
        if (sp_str_equal(t->data.record.fields[i].name, field))
            return t->data.record.fields[i].type;
    }
    return NULL;
}

static bool collect_type_app_args(fxsh_type_t *t, sp_dyn_array(fxsh_type_t *) * out_args,
                                  fxsh_type_t **out_head) {
    if (!out_args)
        return false;
    sp_dyn_array(fxsh_type_t *) rev = SP_NULLPTR;
    while (t && t->kind == TYPE_APP) {
        sp_dyn_array_push(rev, t->data.app.arg);
        t = t->data.app.con;
    }
    if (out_head)
        *out_head = t;
    for (s32 i = (s32)sp_dyn_array_size(rev) - 1; i >= 0; i--) {
        sp_dyn_array_push(*out_args, rev[i]);
    }
    sp_dyn_array_free(rev);
    return true;
}

static fxsh_type_t *constr_arg_type_hint(sp_str_t constr, u32 idx, fxsh_type_t *scrutinee_t) {
    adt_constr_sig_t *sig = constr_sig_lookup(constr);
    if (!sig || !sig->arg_types || idx >= sp_dyn_array_size(sig->arg_types))
        return NULL;

    sp_dyn_array(fxsh_type_t *) param_types = SP_NULLPTR;
    if (sig->type_params && scrutinee_t) {
        fxsh_type_t *head = NULL;
        collect_type_app_args(scrutinee_t, &param_types, &head);
        head = normalize_codegen_type(head);
        if (!head || head->kind != TYPE_CON || !sp_str_equal(head->data.con, sig->type_name)) {
            sp_dyn_array_free(param_types);
            param_types = SP_NULLPTR;
        }
    }
    return cg_type_from_type_ast_with_params(sig->arg_types[idx], sig->type_params, param_types);
}

static fxsh_type_t *list_elem_type_hint(fxsh_type_t *t) {
    fxsh_type_t *elem = NULL;
    return list_type_elem_hint(t, &elem) ? elem : NULL;
}

static fxsh_type_var_t pat_fresh_tvar(void) {
    return g_pat_tvar_counter--;
}

static fxsh_type_t *make_pat_tuple_type(sp_dyn_array(fxsh_type_t *) elems) {
    fxsh_type_t *tt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    tt->kind = TYPE_TUPLE;
    tt->data.tuple = elems;
    return tt;
}

static fxsh_type_t *make_pat_record_type(sp_dyn_array(fxsh_field_t) fields,
                                         fxsh_type_var_t row_var) {
    fxsh_type_t *rt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
    rt->kind = TYPE_RECORD;
    rt->data.record.fields = fields;
    rt->data.record.row_var = row_var;
    return rt;
}

static fxsh_type_t *make_pat_list_type(fxsh_type_t *elem) {
    return fxsh_type_apply(fxsh_type_con(TYPE_LIST), elem);
}

static void pat_var_types_reset(void) {
    if (g_pat_var_types)
        sp_dyn_array_free(g_pat_var_types);
    g_pat_var_types = SP_NULLPTR;
}

static fxsh_type_t *pat_var_type_lookup(sp_str_t name) {
    sp_dyn_array_for(g_pat_var_types, i) {
        if (sp_str_equal(g_pat_var_types[i].name, name))
            return g_pat_var_types[i].type;
    }
    return NULL;
}

static void pat_var_types_push_local_types(void) {
    sp_dyn_array_for(g_pat_var_types, i) {
        local_type_push(g_pat_var_types[i].name, g_pat_var_types[i].type);
    }
}

static void pat_var_type_bind_hint(sp_str_t name, fxsh_type_t *type) {
    if (!name.data || !type || type_has_var(type))
        return;
    sp_dyn_array_for(g_pat_var_types, i) {
        if (sp_str_equal(g_pat_var_types[i].name, name)) {
            g_pat_var_types[i].type = type;
            return;
        }
    }
    pat_var_type_entry_t e = {.name = name, .type = type};
    sp_dyn_array_push(g_pat_var_types, e);
}

static void seed_pattern_var_types_from_hint(fxsh_ast_node_t *pat, fxsh_type_t *hint_t) {
    if (!pat || !hint_t)
        return;
    if (is_pat_var_node(pat)) {
        pat_var_type_bind_hint(pat->data.ident, hint_t);
        return;
    }
    switch (pat->kind) {
        case AST_PAT_TUPLE:
            sp_dyn_array_for(pat->data.elements, i) seed_pattern_var_types_from_hint(
                pat->data.elements[i], tuple_elem_type_hint(hint_t, (u32)i));
            return;
        case AST_PAT_RECORD:
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    continue;
                seed_pattern_var_types_from_hint(
                    f->data.field.object, record_field_type_hint(hint_t, f->data.field.field));
            }
            return;
        case AST_PAT_CONSTR:
            sp_dyn_array_for(pat->data.constr_appl.args, i) seed_pattern_var_types_from_hint(
                pat->data.constr_appl.args[i],
                constr_arg_type_hint(pat->data.constr_appl.constr_name, (u32)i, hint_t));
            return;
        case AST_PAT_CONS:
            if (pat->data.elements && sp_dyn_array_size(pat->data.elements) == 2) {
                fxsh_type_t *elem_t = list_elem_type_hint(hint_t);
                seed_pattern_var_types_from_hint(pat->data.elements[0], elem_t);
                seed_pattern_var_types_from_hint(pat->data.elements[1], hint_t);
            }
            return;
        case AST_LIST: {
            fxsh_type_t *elem_t = list_elem_type_hint(hint_t);
            sp_dyn_array_for(pat->data.elements, i)
                seed_pattern_var_types_from_hint(pat->data.elements[i], elem_t);
            return;
        }
        default:
            return;
    }
}

static fxsh_type_t *build_pattern_type_for_codegen(fxsh_ast_node_t *pat) {
    if (!pat)
        return fxsh_type_var(pat_fresh_tvar());
    if (is_pat_var_node(pat)) {
        fxsh_type_t *v = fxsh_type_var(pat_fresh_tvar());
        pat_var_type_entry_t e = {.name = pat->data.ident, .type = v};
        sp_dyn_array_push(g_pat_var_types, e);
        return v;
    }

    switch (pat->kind) {
        case AST_PAT_WILD:
            return fxsh_type_var(pat_fresh_tvar());
        case AST_LIT_INT:
            return fxsh_type_con(TYPE_INT);
        case AST_LIT_FLOAT:
            return fxsh_type_con(TYPE_FLOAT);
        case AST_LIT_BOOL:
            return fxsh_type_con(TYPE_BOOL);
        case AST_LIT_STRING:
            return fxsh_type_con(TYPE_STRING);
        case AST_LIT_UNIT:
            return fxsh_type_con(TYPE_UNIT);
        case AST_PAT_TUPLE: {
            sp_dyn_array(fxsh_type_t *) elems = SP_NULLPTR;
            sp_dyn_array_for(pat->data.elements, i) {
                sp_dyn_array_push(elems, build_pattern_type_for_codegen(pat->data.elements[i]));
            }
            return make_pat_tuple_type(elems);
        }
        case AST_LIST: {
            fxsh_type_t *elem_t = fxsh_type_var(pat_fresh_tvar());
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_type_t *et = build_pattern_type_for_codegen(pat->data.elements[i]);
                fxsh_subst_t s = SP_NULLPTR;
                if (fxsh_type_unify(et, elem_t, &s) == ERR_OK) {
                    fxsh_type_apply_subst(s, &elem_t);
                    sp_dyn_array_for(g_pat_var_types, k) {
                        fxsh_type_apply_subst(s, &g_pat_var_types[k].type);
                    }
                }
            }
            return make_pat_list_type(elem_t);
        }
        case AST_PAT_CONS: {
            if (!pat->data.elements || sp_dyn_array_size(pat->data.elements) != 2)
                return make_pat_list_type(fxsh_type_var(pat_fresh_tvar()));
            fxsh_type_t *head_t = build_pattern_type_for_codegen(pat->data.elements[0]);
            fxsh_type_t *tail_t = build_pattern_type_for_codegen(pat->data.elements[1]);
            fxsh_type_t *list_t = make_pat_list_type(head_t);
            fxsh_subst_t s = SP_NULLPTR;
            if (fxsh_type_unify(tail_t, list_t, &s) == ERR_OK) {
                fxsh_type_apply_subst(s, &list_t);
                sp_dyn_array_for(g_pat_var_types, k) {
                    fxsh_type_apply_subst(s, &g_pat_var_types[k].type);
                }
            }
            return list_t;
        }
        case AST_PAT_RECORD: {
            sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    continue;
                fxsh_field_t rf = {.name = f->data.field.field,
                                   .type = build_pattern_type_for_codegen(f->data.field.object)};
                sp_dyn_array_push(fields, rf);
            }
            return make_pat_record_type(fields, pat_fresh_tvar());
        }
        default:
            return fxsh_type_var(pat_fresh_tvar());
    }
}

static bool derive_pattern_var_types_from_match(fxsh_ast_node_t *pat, fxsh_type_t *scrutinee_t) {
    pat_var_types_reset();
    if (!pat || !scrutinee_t)
        return false;
    fxsh_type_t *pt = build_pattern_type_for_codegen(pat);
    fxsh_subst_t subst = SP_NULLPTR;
    if (fxsh_type_unify(pt, scrutinee_t, &subst) != ERR_OK)
        return false;
    sp_dyn_array_for(g_pat_var_types, i) {
        fxsh_type_apply_subst(subst, &g_pat_var_types[i].type);
    }
    seed_pattern_var_types_from_hint(pat, scrutinee_t);
    return true;
}

static void emit_unbox_from_boxed(codegen_ctx_t *ctx, fxsh_type_t *hint_t, const char *boxed_expr) {
    if (list_type_elem_hint(hint_t, NULL)) {
        emit_raw(ctx, "fxsh_unbox_list(");
        emit_raw(ctx, boxed_expr);
        emit_raw(ctx, ")");
        return;
    }
    hint_t = normalize_codegen_type(hint_t);
    if (hint_t && hint_t->kind == TYPE_CON) {
        if (sp_str_equal(hint_t->data.con, TYPE_FLOAT)) {
            emit_raw(ctx, "fxsh_unbox_f64(");
            emit_raw(ctx, boxed_expr);
            emit_raw(ctx, ")");
            return;
        }
        if (sp_str_equal(hint_t->data.con, TYPE_BOOL)) {
            emit_raw(ctx, "fxsh_unbox_bool(");
            emit_raw(ctx, boxed_expr);
            emit_raw(ctx, ")");
            return;
        }
        if (sp_str_equal(hint_t->data.con, TYPE_STRING)) {
            emit_raw(ctx, "fxsh_unbox_str(");
            emit_raw(ctx, boxed_expr);
            emit_raw(ctx, ")");
            return;
        }
    }
    emit_raw(ctx, "fxsh_unbox_i64(");
    emit_raw(ctx, boxed_expr);
    emit_raw(ctx, ")");
}

static void gen_pattern_bindings(codegen_ctx_t *ctx, fxsh_ast_node_t *pat, const char *val_name,
                                 bool val_is_boxed, fxsh_type_t *hint_t) {
    if (!pat)
        return;
    if (is_pat_var_node(pat)) {
        emit_indent(ctx);
        emit_raw(ctx, "__auto_type ");
        emit_mangled(ctx, pat->data.ident);
        emit_raw(ctx, " = ");
        if (val_is_boxed) {
            fxsh_type_t *bind_hint = pat_var_type_lookup(pat->data.ident);
            if (!bind_hint)
                bind_hint = hint_t;
            emit_unbox_from_boxed(ctx, bind_hint, val_name);
        } else
            emit_raw(ctx, val_name);
        emit_raw(ctx, ";\n");
        return;
    }
    switch (pat->kind) {
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
                if (val_is_boxed) {
                    sp_str_t tname = adt_type_of_constructor(cname);
                    char tbuf[128];
                    mangle_into(tname, tbuf, sizeof(tbuf));
                    snprintf(field_expr, sizeof(field_expr), "((fxsh_%s_t*)(%s).as.p)->data.%s._%u",
                             tbuf, val_name, cname_buf, (unsigned)j);
                } else {
                    snprintf(field_expr, sizeof(field_expr), "%s.data.%s._%u", val_name, cname_buf,
                             (unsigned)j);
                }
                bool field_boxed = constr_arg_is_boxed_typevar(cname, (u32)j);
                fxsh_type_t *arg_hint = constr_arg_type_hint(cname, (u32)j, hint_t);
                if (!field_boxed && constr_arg_is_self_ptr(cname, (u32)j)) {
                    char deref_expr[320];
                    snprintf(deref_expr, sizeof(deref_expr), "(*%s)", field_expr);
                    gen_pattern_bindings(ctx, pat->data.constr_appl.args[j], deref_expr, false,
                                         arg_hint);
                } else {
                    gen_pattern_bindings(ctx, pat->data.constr_appl.args[j], field_expr,
                                         field_boxed, arg_hint);
                }
            }
            break;
        }
        case AST_PAT_RECORD: {
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    continue;
                char sub_expr[256];
                if (val_is_boxed) {
                    snprintf(sub_expr, sizeof(sub_expr),
                             "fxsh_record_get(*((fxsh_record_t*)(%s).as.p), \"%.*s\")", val_name,
                             f->data.field.field.len, f->data.field.field.data);
                } else {
                    snprintf(sub_expr, sizeof(sub_expr), "fxsh_record_get(%s, \"%.*s\")", val_name,
                             f->data.field.field.len, f->data.field.field.data);
                }
                fxsh_type_t *sub_hint = record_field_type_hint(hint_t, f->data.field.field);
                gen_pattern_bindings(ctx, f->data.field.object, sub_expr, true, sub_hint);
            }
            break;
        }
        case AST_PAT_TUPLE: {
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *sub = pat->data.elements[i];
                if (!sub)
                    continue;
                char sub_expr[256];
                if (val_is_boxed) {
                    snprintf(sub_expr, sizeof(sub_expr),
                             "fxsh_tuple_get(*((fxsh_tuple_t*)(%s).as.p), %u)", val_name,
                             (unsigned)i);
                } else {
                    snprintf(sub_expr, sizeof(sub_expr), "fxsh_tuple_get(%s, %u)", val_name,
                             (unsigned)i);
                }
                fxsh_type_t *sub_hint = tuple_elem_type_hint(hint_t, (u32)i);
                gen_pattern_bindings(ctx, sub, sub_expr, true, sub_hint);
            }
            break;
        }
        case AST_PAT_CONS: {
            if (!pat->data.elements || sp_dyn_array_size(pat->data.elements) != 2)
                break;
            fxsh_type_t *elem_hint = list_elem_type_hint(hint_t);
            char head_expr[256];
            char tail_expr[256];
            if (val_is_boxed) {
                snprintf(head_expr, sizeof(head_expr), "fxsh_list_head(fxsh_unbox_list(%s))",
                         val_name);
                snprintf(tail_expr, sizeof(tail_expr), "fxsh_list_tail(fxsh_unbox_list(%s))",
                         val_name);
            } else {
                snprintf(head_expr, sizeof(head_expr), "fxsh_list_head(%s)", val_name);
                snprintf(tail_expr, sizeof(tail_expr), "fxsh_list_tail(%s)", val_name);
            }
            gen_pattern_bindings(ctx, pat->data.elements[0], head_expr, true, elem_hint);
            gen_pattern_bindings(ctx, pat->data.elements[1], tail_expr, false, hint_t);
            break;
        }
        case AST_LIST: {
            fxsh_type_t *elem_hint = list_elem_type_hint(hint_t);
            u32 lid = ctx->temp_var_counter++;
            emit_indent(ctx);
            emit_fmt(ctx, "fxsh_list_t* _lst_bind_%u = ", (unsigned)lid);
            if (val_is_boxed) {
                emit_raw(ctx, "fxsh_unbox_list(");
                emit_raw(ctx, val_name);
                emit_raw(ctx, ")");
            } else {
                emit_raw(ctx, val_name);
            }
            emit_raw(ctx, ";\n");
            emit_indent(ctx);
            emit_fmt(ctx, "(void)_lst_bind_%u;\n", (unsigned)lid);
            sp_dyn_array_for(pat->data.elements, i) {
                char head_expr[128];
                char tail_line[128];
                snprintf(head_expr, sizeof(head_expr), "fxsh_list_head(_lst_bind_%u)",
                         (unsigned)lid);
                snprintf(tail_line, sizeof(tail_line),
                         "_lst_bind_%u = fxsh_list_tail(_lst_bind_%u);", (unsigned)lid,
                         (unsigned)lid);
                gen_pattern_bindings(ctx, pat->data.elements[i], head_expr, true, elem_hint);
                emit_indent(ctx);
                emit_raw(ctx, tail_line);
                emit_raw(ctx, "\n");
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
    sp_dyn_array(sp_str_t) seen_ctors = SP_NULLPTR;
    sp_dyn_array_for(ast->data.match_expr.arms, i) {
        fxsh_ast_node_t *arm = ast->data.match_expr.arms[i];
        if (!arm || arm->kind != AST_MATCH_ARM)
            continue;
        /* Guarded arms may need fall-through to later arms on guard failure.
         * Keep condition-chain lowering for semantic correctness. */
        if (arm->data.match_arm.guard) {
            sp_dyn_array_free(seen_ctors);
            return false;
        }
        fxsh_ast_node_t *pat = arm->data.match_arm.pattern;
        if (!pat)
            continue;
        if (pat->kind == AST_PAT_WILD || is_pat_var_node(pat))
            continue;
        if (pat->kind == AST_PAT_CONSTR) {
            /* If same constructor appears multiple times or with non-trivial arg patterns,
             * use fallback condition-chain instead of tag switch for semantic correctness. */
            if (str_in_list(seen_ctors, pat->data.constr_appl.constr_name)) {
                sp_dyn_array_free(seen_ctors);
                return false;
            }
            sp_dyn_array_push(seen_ctors, pat->data.constr_appl.constr_name);
            sp_dyn_array_for(pat->data.constr_appl.args, j) {
                fxsh_ast_node_t *a = pat->data.constr_appl.args[j];
                if (!a)
                    continue;
                if (!(a->kind == AST_PAT_WILD || is_pat_var_node(a))) {
                    sp_dyn_array_free(seen_ctors);
                    return false;
                }
            }
            continue;
        }
        sp_dyn_array_free(seen_ctors);
        return false;
    }
    sp_dyn_array_free(seen_ctors);
    return true;
}

static void gen_pattern_condition(codegen_ctx_t *ctx, fxsh_ast_node_t *pat, const char *val_name);
static bool emit_constr_arg_condition(codegen_ctx_t *ctx, sp_str_t constr_name, u32 idx,
                                      fxsh_ast_node_t *arg_pat, const char *field_expr);

static fxsh_type_t *match_result_type_hint(fxsh_ast_node_t *match_ast) {
    if (!match_ast || match_ast->kind != AST_MATCH || !match_ast->data.match_expr.arms)
        return NULL;
    fxsh_type_t *acc = NULL;
    sp_dyn_array_for(match_ast->data.match_expr.arms, i) {
        fxsh_ast_node_t *arm = match_ast->data.match_expr.arms[i];
        if (!arm || arm->kind != AST_MATCH_ARM || !arm->data.match_arm.body)
            continue;
        fxsh_type_t *t = infer_expr_type_for_codegen_any(arm->data.match_arm.body);
        if (!t)
            continue;
        if (!acc) {
            acc = t;
            continue;
        }
        fxsh_subst_t subst = SP_NULLPTR;
        fxsh_type_t *lhs = acc;
        fxsh_type_t *rhs = t;
        if (fxsh_type_unify(lhs, rhs, &subst) == ERR_OK) {
            fxsh_type_apply_subst(subst, &lhs);
            fxsh_type_apply_subst(subst, &rhs);
            acc = lhs;
        } else if (!type_has_var(t)) {
            acc = t;
        }
    }
    if (acc && !type_has_var(acc))
        return acc;
    if (acc)
        return acc;
    return infer_expr_type_for_codegen_any(match_ast);
}

static void emit_zero_init_for_type(codegen_ctx_t *ctx, fxsh_type_t *t) {
    if (!t) {
        emit_raw(ctx, "0");
        return;
    }
    if (list_type_elem_hint(t, NULL) || ptr_type_elem_hint(t, NULL)) {
        emit_raw(ctx, "NULL");
        return;
    }
    t = normalize_codegen_type(t);
    if (!t) {
        emit_raw(ctx, "0");
        return;
    }
    if (t->kind == TYPE_CON) {
        if (sp_str_equal(t->data.con, TYPE_TYPE)) {
            emit_raw(ctx, "NULL");
            return;
        }
        if (sp_str_equal(t->data.con, TYPE_STRING)) {
            emit_raw(ctx, "((sp_str_t){0})");
            return;
        }
        if (sp_str_equal(t->data.con, TYPE_FLOAT)) {
            emit_raw(ctx, "0.0");
            return;
        }
        if (sp_str_equal(t->data.con, TYPE_BOOL)) {
            emit_raw(ctx, "false");
            return;
        }
        if (!sp_str_equal(t->data.con, TYPE_INT) && !sp_str_equal(t->data.con, TYPE_C_INT) &&
            !sp_str_equal(t->data.con, TYPE_C_UINT) && !sp_str_equal(t->data.con, TYPE_C_LONG) &&
            !sp_str_equal(t->data.con, TYPE_C_ULONG) && !sp_str_equal(t->data.con, TYPE_C_SIZE) &&
            !sp_str_equal(t->data.con, TYPE_C_SSIZE) && !sp_str_equal(t->data.con, TYPE_UNIT)) {
            emit_raw(ctx, "((");
            emit_c_type_for_type(ctx, t);
            emit_raw(ctx, "){0})");
            return;
        }
        emit_raw(ctx, "0");
        return;
    }
    if (t->kind == TYPE_RECORD || t->kind == TYPE_TUPLE) {
        emit_raw(ctx, "((");
        emit_c_type_for_type(ctx, t);
        emit_raw(ctx, "){0})");
        return;
    }
    emit_raw(ctx, "0");
}

static void gen_boxed_pattern_condition(codegen_ctx_t *ctx, fxsh_ast_node_t *pat,
                                        const char *boxed_expr) {
    if (!pat) {
        emit_raw(ctx, "true");
        return;
    }
    if (is_pat_var_node(pat)) {
        emit_raw(ctx, "true");
        return;
    }
    switch (pat->kind) {
        case AST_PAT_WILD:
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
            emit_c_escaped_str(ctx, pat->data.lit_string);
            emit_fmt(ctx, "\", %u) == 0)", (unsigned)pat->data.lit_string.len);
            return;
        case AST_PAT_TUPLE:
            emit_fmt(ctx,
                     "(%s.kind == FXSH_VAL_PTR && fxsh_ptr_is_tuple(%s.as.p) && "
                     "((fxsh_tuple_t*)%s.as.p)->len == %u",
                     boxed_expr, boxed_expr, boxed_expr,
                     (unsigned)sp_dyn_array_size(pat->data.elements));
            sp_dyn_array_for(pat->data.elements, i) {
                char sub_box[256];
                snprintf(sub_box, sizeof(sub_box), "fxsh_tuple_get(*((fxsh_tuple_t*)%s.as.p), %u)",
                         boxed_expr, (unsigned)i);
                emit_raw(ctx, " && ");
                gen_boxed_pattern_condition(ctx, pat->data.elements[i], sub_box);
            }
            emit_raw(ctx, ")");
            return;
        case AST_PAT_RECORD: {
            char rec_expr[320];
            snprintf(rec_expr, sizeof(rec_expr), "(*((fxsh_record_t*)(%s).as.p))", boxed_expr);
            emit_fmt(ctx, "((%s).kind == FXSH_VAL_PTR", boxed_expr);
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS)
                    continue;
                emit_raw(ctx, " && fxsh_record_has(");
                emit_raw(ctx, rec_expr);
                emit_raw(ctx, ", \"");
                emit_string(ctx, f->data.field.field);
                emit_raw(ctx, "\")");
                if (f->data.field.object) {
                    emit_raw(ctx, " && ");
                    char sub_box[384];
                    snprintf(sub_box, sizeof(sub_box), "fxsh_record_get(%s, \"%.*s\")", rec_expr,
                             f->data.field.field.len, f->data.field.field.data);
                    gen_boxed_pattern_condition(ctx, f->data.field.object, sub_box);
                }
            }
            emit_raw(ctx, ")");
            return;
        }
        case AST_PAT_CONSTR: {
            sp_str_t tname = adt_type_of_constructor(pat->data.constr_appl.constr_name);
            if (tname.len == 0) {
                emit_raw(ctx, "false");
                return;
            }
            char tbuf[128];
            char cbuf[128];
            mangle_into(tname, tbuf, sizeof(tbuf));
            mangle_into(pat->data.constr_appl.constr_name, cbuf, sizeof(cbuf));
            emit_raw(ctx, "(");
            emit_fmt(ctx, "(%s).kind == FXSH_VAL_PTR", boxed_expr);
            emit_fmt(ctx, " && ((fxsh_%s_t*)(%s).as.p)->tag == fxsh_tag_%s_%s", tbuf, boxed_expr,
                     tbuf, cbuf);
            sp_dyn_array_for(pat->data.constr_appl.args, i) {
                fxsh_ast_node_t *arg_pat = pat->data.constr_appl.args[i];
                if (!arg_pat)
                    continue;
                char field_expr[384];
                snprintf(field_expr, sizeof(field_expr), "((fxsh_%s_t*)(%s).as.p)->data.%s._%u",
                         tbuf, boxed_expr, cbuf, (unsigned)i);
                emit_raw(ctx, " && ");
                if (!emit_constr_arg_condition(ctx, pat->data.constr_appl.constr_name, (u32)i,
                                               arg_pat, field_expr))
                    emit_raw(ctx, "true");
            }
            emit_raw(ctx, ")");
            return;
        }
        case AST_PAT_CONS: {
            if (!pat->data.elements || sp_dyn_array_size(pat->data.elements) != 2) {
                emit_raw(ctx, "false");
                return;
            }
            emit_raw(ctx, "(");
            emit_fmt(ctx, "(%s).kind == FXSH_VAL_PTR && !fxsh_list_is_nil(fxsh_unbox_list(%s))",
                     boxed_expr, boxed_expr);
            emit_raw(ctx, " && ");
            char hb[256];
            snprintf(hb, sizeof(hb), "fxsh_list_head(fxsh_unbox_list(%s))", boxed_expr);
            gen_boxed_pattern_condition(ctx, pat->data.elements[0], hb);
            emit_raw(ctx, " && ");
            char tb[256];
            snprintf(tb, sizeof(tb), "fxsh_box_list(fxsh_list_tail(fxsh_unbox_list(%s)))",
                     boxed_expr);
            gen_boxed_pattern_condition(ctx, pat->data.elements[1], tb);
            emit_raw(ctx, ")");
            return;
        }
        case AST_LIST: {
            emit_raw(ctx, "(true");
            char cur[256];
            snprintf(cur, sizeof(cur), "fxsh_unbox_list(%s)", boxed_expr);
            sp_dyn_array_for(pat->data.elements, i) {
                emit_raw(ctx, " && !fxsh_list_is_nil(");
                emit_raw(ctx, cur);
                emit_raw(ctx, ")");
                emit_raw(ctx, " && ");
                char hb[256];
                snprintf(hb, sizeof(hb), "fxsh_list_head(%s)", cur);
                gen_boxed_pattern_condition(ctx, pat->data.elements[i], hb);
                char nxt[256];
                snprintf(nxt, sizeof(nxt), "fxsh_list_tail(%s)", cur);
                snprintf(cur, sizeof(cur), "%s", nxt);
            }
            emit_raw(ctx, " && fxsh_list_is_nil(");
            emit_raw(ctx, cur);
            emit_raw(ctx, "))");
            return;
        }
        default:
            emit_raw(ctx, "false");
            return;
    }
}

static bool pat_is_trivial_bind(fxsh_ast_node_t *pat) {
    return !pat || pat->kind == AST_PAT_WILD || is_pat_var_node(pat);
}

static bool emit_constr_arg_condition(codegen_ctx_t *ctx, sp_str_t constr_name, u32 idx,
                                      fxsh_ast_node_t *arg_pat, const char *field_expr) {
    if (pat_is_trivial_bind(arg_pat))
        return false;

    if (constr_arg_is_boxed_typevar(constr_name, idx)) {
        gen_boxed_pattern_condition(ctx, arg_pat, field_expr);
        return true;
    }

    if (arg_pat->kind == AST_PAT_CONSTR || arg_pat->kind == AST_PAT_TUPLE ||
        arg_pat->kind == AST_PAT_RECORD || arg_pat->kind == AST_PAT_CONS ||
        arg_pat->kind == AST_LIST) {
        if (constr_arg_is_self_ptr(constr_name, idx)) {
            char deref_expr[320];
            snprintf(deref_expr, sizeof(deref_expr), "(*%s)", field_expr);
            emit_raw(ctx, "(");
            emit_fmt(ctx, "%s != NULL && ", field_expr);
            gen_pattern_condition(ctx, arg_pat, deref_expr);
            emit_raw(ctx, ")");
        } else {
            gen_pattern_condition(ctx, arg_pat, field_expr);
        }
        return true;
    }

    if (arg_pat->kind == AST_LIT_INT) {
        emit_fmt(ctx, "(%s == %lldLL)", field_expr, (long long)arg_pat->data.lit_int);
        return true;
    }
    if (arg_pat->kind == AST_LIT_FLOAT) {
        emit_fmt(ctx, "(%s == %.17g)", field_expr, arg_pat->data.lit_float);
        return true;
    }
    if (arg_pat->kind == AST_LIT_BOOL) {
        emit_fmt(ctx, "(%s == %s)", field_expr, arg_pat->data.lit_bool ? "true" : "false");
        return true;
    }
    if (arg_pat->kind == AST_LIT_STRING) {
        emit_raw(ctx, "(");
        emit_fmt(ctx, "%s.len == %u && strncmp(%s.data, \"", field_expr,
                 (unsigned)arg_pat->data.lit_string.len, field_expr);
        emit_c_escaped_str(ctx, arg_pat->data.lit_string);
        emit_fmt(ctx, "\", %u) == 0)", (unsigned)arg_pat->data.lit_string.len);
        return true;
    }
    return false;
}

static void gen_ctor_args_condition(codegen_ctx_t *ctx, fxsh_ast_node_t *pat,
                                    const char *val_name) {
    if (!pat || pat->kind != AST_PAT_CONSTR)
        return;
    sp_str_t cname = pat->data.constr_appl.constr_name;
    char cname_buf[128];
    mangle_into(cname, cname_buf, sizeof(cname_buf));
    sp_dyn_array_for(pat->data.constr_appl.args, i) {
        fxsh_ast_node_t *arg_pat = pat->data.constr_appl.args[i];
        if (!arg_pat)
            continue;
        char field_expr[256];
        snprintf(field_expr, sizeof(field_expr), "%s.data.%s._%u", val_name, cname_buf,
                 (unsigned)i);
        emit_raw(ctx, " && ");
        if (!emit_constr_arg_condition(ctx, cname, (u32)i, arg_pat, field_expr))
            emit_raw(ctx, "true");
    }
}

static void gen_pattern_condition(codegen_ctx_t *ctx, fxsh_ast_node_t *pat, const char *val_name) {
    if (!pat) {
        emit_raw(ctx, "true");
        return;
    }
    if (is_pat_var_node(pat)) {
        emit_raw(ctx, "true");
        return;
    }
    switch (pat->kind) {
        case AST_PAT_WILD:
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
            emit_c_escaped_str(ctx, pat->data.lit_string);
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
            gen_ctor_args_condition(ctx, pat, val_name);
            emit_raw(ctx, ")");
            return;
        }
        case AST_PAT_TUPLE:
            emit_fmt(ctx, "(%s.len == %u", val_name,
                     (unsigned)sp_dyn_array_size(pat->data.elements));
            sp_dyn_array_for(pat->data.elements, i) {
                char sub_box[256];
                snprintf(sub_box, sizeof(sub_box), "fxsh_tuple_get(%s, %u)", val_name, (unsigned)i);
                emit_raw(ctx, " && ");
                gen_boxed_pattern_condition(ctx, pat->data.elements[i], sub_box);
            }
            emit_raw(ctx, ")");
            return;
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
        case AST_PAT_CONS: {
            if (!pat->data.elements || sp_dyn_array_size(pat->data.elements) != 2) {
                emit_raw(ctx, "false");
                return;
            }
            emit_raw(ctx, "(!fxsh_list_is_nil(");
            emit_raw(ctx, val_name);
            emit_raw(ctx, ")");
            emit_raw(ctx, " && ");
            char head_expr[256];
            snprintf(head_expr, sizeof(head_expr), "fxsh_list_head(%s)", val_name);
            gen_boxed_pattern_condition(ctx, pat->data.elements[0], head_expr);
            emit_raw(ctx, " && ");
            char tail_expr[256];
            snprintf(tail_expr, sizeof(tail_expr), "fxsh_list_tail(%s)", val_name);
            gen_pattern_condition(ctx, pat->data.elements[1], tail_expr);
            emit_raw(ctx, ")");
            return;
        }
        case AST_LIST: {
            emit_raw(ctx, "(true");
            char cur[256];
            snprintf(cur, sizeof(cur), "%s", val_name);
            sp_dyn_array_for(pat->data.elements, i) {
                emit_raw(ctx, " && !fxsh_list_is_nil(");
                emit_raw(ctx, cur);
                emit_raw(ctx, ")");
                emit_raw(ctx, " && ");
                char head_expr[256];
                snprintf(head_expr, sizeof(head_expr), "fxsh_list_head(%s)", cur);
                gen_boxed_pattern_condition(ctx, pat->data.elements[i], head_expr);
                char nxt[256];
                snprintf(nxt, sizeof(nxt), "fxsh_list_tail(%s)", cur);
                snprintf(cur, sizeof(cur), "%s", nxt);
            }
            emit_raw(ctx, " && fxsh_list_is_nil(");
            emit_raw(ctx, cur);
            emit_raw(ctx, "))");
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
    fxsh_type_t *match_t_hint = infer_expr_type_strict_for_codegen(ast->data.match_expr.expr);
    fxsh_type_t *match_res_t = match_result_type_hint(ast);

    emit_indent(ctx);
    if (match_res_t) {
        emit_c_type_for_type(ctx, match_res_t);
        emit_raw(ctx, " _match_res = ");
        emit_zero_init_for_type(ctx, match_res_t);
        emit_raw(ctx, ";\n");
    } else {
        emit_raw(ctx, "__auto_type _match_res = 0; /* placeholder */\n");
    }

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

            if (pat->kind == AST_PAT_WILD || is_pat_var_node(pat)) {
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

            if (pat && pat->kind == AST_PAT_CONSTR) {
                emit_indent(ctx);
                emit_raw(ctx, "if (!(");
                gen_pattern_condition(ctx, pat, "_match_val");
                emit_raw(ctx, ")) break;\n");
            }

            derive_pattern_var_types_from_match(pat, match_t_hint);
            u32 lty_m = local_type_mark();
            pat_var_types_push_local_types();
            gen_pattern_bindings(ctx, pat, "_match_val", false, match_t_hint);

            if (arm->data.match_arm.guard) {
                emit_indent(ctx);
                emit_raw(ctx, "if (!");
                gen_expr(ctx, arm->data.match_arm.guard);
                emit_raw(ctx, ") break;\n");
            }

            emit_indent(ctx);
            emit_raw(ctx, "_match_res = ");
            gen_expr(ctx, arm->data.match_arm.body);
            emit_raw(ctx, ";\n");
            emit_indent(ctx);
            emit_raw(ctx, "break;\n");

            local_type_pop_to(lty_m);
            pat_var_types_reset();
            ctx->indent_level--;
            emit_indent(ctx);
            emit_raw(ctx, "}\n");
        }

        ctx->indent_level--;
        emit_indent(ctx);
        emit_raw(ctx, "}\n");
    } else {
        emit_indent(ctx);
        emit_raw(ctx, "bool _match_done = false;\n");
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

            derive_pattern_var_types_from_match(pat, match_t_hint);
            u32 lty_m = local_type_mark();
            pat_var_types_push_local_types();
            gen_pattern_bindings(ctx, pat, "_match_val", false, match_t_hint);

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

            local_type_pop_to(lty_m);
            pat_var_types_reset();
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
    pat_var_types_reset();
}

static fxsh_type_t *type_from_literal_expr(fxsh_ast_node_t *expr) {
    if (!expr)
        return NULL;
    switch (expr->kind) {
        case AST_LIT_INT:
            return fxsh_type_con(TYPE_INT);
        case AST_LIT_FLOAT:
            return fxsh_type_con(TYPE_FLOAT);
        case AST_LIT_BOOL:
            return fxsh_type_con(TYPE_BOOL);
        case AST_LIT_STRING:
            return fxsh_type_con(TYPE_STRING);
        case AST_TYPE_VALUE:
            return fxsh_type_con(TYPE_TYPE);
        case AST_LIT_UNIT:
            return fxsh_type_con(TYPE_UNIT);
        default:
            return NULL;
    }
}

static fxsh_type_t *guess_expr_type_for_codegen(fxsh_ast_node_t *expr) {
    if (!expr)
        return NULL;
    if (expr->kind == AST_IDENT)
        return lookup_symbol_type(expr->data.ident);
    if (expr->kind == AST_TUPLE) {
        sp_dyn_array(fxsh_type_t *) elems = SP_NULLPTR;
        sp_dyn_array_for(expr->data.elements, i) {
            fxsh_type_t *et = guess_expr_type_for_codegen(expr->data.elements[i]);
            if (!et)
                et = type_from_literal_expr(expr->data.elements[i]);
            if (!et)
                et = fxsh_type_con(TYPE_INT);
            sp_dyn_array_push(elems, et);
        }
        fxsh_type_t *tt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
        tt->kind = TYPE_TUPLE;
        tt->data.tuple = elems;
        return tt;
    }
    if (expr->kind == AST_RECORD) {
        sp_dyn_array(fxsh_field_t) fields = SP_NULLPTR;
        sp_dyn_array_for(expr->data.elements, i) {
            fxsh_ast_node_t *f = expr->data.elements[i];
            if (!f || f->kind != AST_FIELD_ACCESS)
                continue;
            fxsh_type_t *ft = guess_expr_type_for_codegen(f->data.field.object);
            if (!ft)
                ft = type_from_literal_expr(f->data.field.object);
            if (!ft)
                ft = fxsh_type_con(TYPE_INT);
            fxsh_field_t rf = {.name = f->data.field.field, .type = ft};
            sp_dyn_array_push(fields, rf);
        }
        fxsh_type_t *rt = (fxsh_type_t *)fxsh_alloc0(sizeof(fxsh_type_t));
        rt->kind = TYPE_RECORD;
        rt->data.record.fields = fields;
        rt->data.record.row_var = -1;
        return rt;
    }
    if (expr->kind == AST_RECORD_UPDATE) {
        return guess_expr_type_for_codegen(expr->data.record_update.base);
    }
    if (expr->kind == AST_FIELD_ACCESS) {
        fxsh_type_t *obj_t = guess_expr_type_for_codegen(expr->data.field.object);
        fxsh_type_t *field_t = record_field_type_hint(obj_t, expr->data.field.field);
        if (field_t)
            return field_t;
    }
    if (expr->kind == AST_LIST) {
        fxsh_type_t *elem_t = fxsh_type_con(TYPE_INT);
        if (expr->data.elements && sp_dyn_array_size(expr->data.elements) > 0) {
            fxsh_type_t *et = guess_expr_type_for_codegen(expr->data.elements[0]);
            if (!et)
                et = type_from_literal_expr(expr->data.elements[0]);
            if (et)
                elem_t = et;
        }
        return fxsh_type_apply(fxsh_type_con(TYPE_LIST), elem_t);
    }
    return type_from_literal_expr(expr);
}

static fxsh_type_t *infer_expr_type_strict_for_codegen(fxsh_ast_node_t *expr) {
    if (!expr)
        return NULL;
    if (expr->kind == AST_IDENT) {
        fxsh_type_t *t = lookup_symbol_type(expr->data.ident);
        if (t)
            return t;
    }
    fxsh_type_env_t env = g_codegen_type_env;
    fxsh_constr_env_t cenv = g_codegen_constr_env;
    fxsh_type_t *out_t = NULL;
    if (fxsh_type_infer(expr, &env, &cenv, &out_t) == ERR_OK && out_t && !type_has_var(out_t))
        return out_t;
    return guess_expr_type_for_codegen(expr);
}

static fxsh_type_t *infer_expr_type_for_codegen_any(fxsh_ast_node_t *expr) {
    if (!expr)
        return NULL;
    if (expr->kind == AST_IDENT) {
        fxsh_type_t *t = lookup_symbol_type(expr->data.ident);
        if (t)
            return t;
    }
    fxsh_type_env_t env = g_codegen_type_env;
    fxsh_constr_env_t cenv = g_codegen_constr_env;
    fxsh_type_t *out_t = NULL;
    if (fxsh_type_infer(expr, &env, &cenv, &out_t) == ERR_OK && out_t)
        return out_t;
    return guess_expr_type_for_codegen(expr);
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
        case AST_TYPE_VALUE:
            gen_literal(ctx, ast);
            break;
        case AST_IDENT: {
            sp_str_t mapped = sym_lookup_expr(ast->data.ident);
            fxsh_type_t *ident_t = lookup_symbol_type(ast->data.ident);
            if (mapped.len > 0 && !(ident_t && ident_t->kind == TYPE_ARROW)) {
                emit_string(ctx, mapped);
                break;
            }
            if (ident_t && ident_t->kind == TYPE_ARROW) {
                if (mapped.len > 0) {
                    if (closure_value_lookup(ast->data.ident)) {
                        emit_string(ctx, mapped);
                        break;
                    }
                    if (is_lambda_fn_name(ast->data.ident) || is_decl_fn_name(ast->data.ident) ||
                        is_extern_fn_name(ast->data.ident)) {
                        emit_raw(ctx, "((fxsh_closure_t){ .call = (void*)");
                        emit_string(ctx, mapped);
                        emit_raw(ctx, ", .env = NULL })");
                        break;
                    }
                    emit_raw(ctx, "((fxsh_closure_t){ .call = (void*)(");
                    emit_string(ctx, mapped);
                    emit_raw(ctx, ").call, .env = (");
                    emit_string(ctx, mapped);
                    emit_raw(ctx, ").env })");
                    break;
                }
                {
                    bool has_module_sep = false;
                    for (u32 mi = 1; mi < ast->data.ident.len; mi++) {
                        if (ast->data.ident.data[mi - 1] == '_' && ast->data.ident.data[mi] == '_') {
                            has_module_sep = true;
                            break;
                        }
                    }
                    if (has_module_sep) {
                        emit_raw(ctx, "fxsh_fn_");
                        emit_mangled(ctx, ast->data.ident);
                        break;
                    }
                }
                if (is_closure_fn_name(ast->data.ident)) {
                    emit_raw(ctx, "fxsh_fn_");
                    emit_mangled(ctx, ast->data.ident);
                    break;
                }
                if (is_lambda_fn_name(ast->data.ident)) {
                    emit_raw(ctx, "((fxsh_closure_t){ .call = (void*)fxsh_wrap_");
                    emit_mangled(ctx, ast->data.ident);
                    emit_raw(ctx, ", .env = NULL })");
                    break;
                }
                if (is_decl_fn_name(ast->data.ident) || is_extern_fn_name(ast->data.ident)) {
                    emit_raw(ctx, "((fxsh_closure_t){ .call = (void*)fxsh_wrap_");
                    emit_mangled(ctx, ast->data.ident);
                    emit_raw(ctx, ", .env = NULL })");
                    break;
                }
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
        case AST_RECORD_UPDATE:
            gen_record_update(ctx, ast);
            break;
        case AST_TUPLE:
            gen_tuple(ctx, ast);
            break;
        case AST_LIST:
            gen_list(ctx, ast);
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
    bool ret_unit = is_type_con_named(return_type_of_fn(fn_t), TYPE_UNIT);
    bool enable_tco = tco_has_only_var_params(ast->data.decl_fn.params) &&
                      tco_has_tail_self_call(ast->data.decl_fn.body, ast->data.decl_fn.name,
                                             (u32)sp_dyn_array_size(ast->data.decl_fn.params));
    u32 lty_m = local_type_mark();
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
            if (is_pat_var_node(p)) {
                emit_mangled(ctx, p->data.ident);
                local_type_push(p->data.ident, nth_param_type(fn_t, (u32)i));
            } else {
                emit_fmt(ctx, "_arg%u", (unsigned)i);
            }
        }
    }
    emit_raw(ctx, ") {\n");
    ctx->indent_level++;
    if (enable_tco) {
        emit_line(ctx, "for (;;) {");
        ctx->indent_level++;
        gen_tail_dispatch(ctx, ast->data.decl_fn.body, ast->data.decl_fn.name, fn_t,
                          ast->data.decl_fn.params, ret_unit);
        ctx->indent_level--;
        emit_line(ctx, "}");
    } else {
        emit_indent(ctx);
        if (ret_unit) {
            gen_expr(ctx, ast->data.decl_fn.body);
            emit_raw(ctx, ";\n");
            emit_indent(ctx);
            emit_raw(ctx, "return;\n");
        } else {
            emit_raw(ctx, "return ");
            gen_expr(ctx, ast->data.decl_fn.body);
            emit_raw(ctx, ";\n");
        }
    }
    ctx->indent_level--;
    emit_line(ctx, "}");
    emit_closure_wrapper(ctx, ast->data.decl_fn.name, "", fn_t);
    local_type_pop_to(lty_m);
    sp_dyn_array_push(*ctx->output, '\n');
}

static bool enable_tco_for_let_lambda(fxsh_ast_node_t *ast, fxsh_ast_node_t *lam) {
    if (!ast || !lam || lam->kind != AST_LAMBDA)
        return false;
    if (!tco_has_only_var_params(lam->data.lambda.params))
        return false;
    if (!ast->data.let.name.data || ast->data.let.name.len == 0)
        return false;
    return tco_has_tail_self_call(lam->data.lambda.body, ast->data.let.name,
                                  (u32)sp_dyn_array_size(lam->data.lambda.params));
}

static void gen_decl_let_lambda_body(codegen_ctx_t *ctx, fxsh_ast_node_t *ast, fxsh_ast_node_t *lam,
                                     fxsh_type_t *fn_t) {
    bool ret_unit = is_type_con_named(return_type_of_fn(fn_t), TYPE_UNIT);
    if (enable_tco_for_let_lambda(ast, lam)) {
        emit_line(ctx, "for (;;) {");
        ctx->indent_level++;
        gen_tail_dispatch(ctx, lam->data.lambda.body, ast->data.let.name, fn_t,
                          lam->data.lambda.params, ret_unit);
        ctx->indent_level--;
        emit_line(ctx, "}");
    } else {
        emit_indent(ctx);
        if (ret_unit) {
            gen_expr(ctx, lam->data.lambda.body);
            emit_raw(ctx, ";\n");
            emit_indent(ctx);
            emit_raw(ctx, "return;\n");
        } else {
            emit_raw(ctx, "return ");
            gen_expr(ctx, lam->data.lambda.body);
            emit_raw(ctx, ";\n");
        }
    }
}

static void emit_closure_wrapper(codegen_ctx_t *ctx, sp_str_t public_name,
                                 const char *target_prefix, fxsh_type_t *fn_t) {
    u32 arity = fn_arity_of(fn_t);
    emit_indent(ctx);
    emit_raw(ctx, "static ");
    emit_c_type_for_type(ctx, return_type_of_fn(fn_t));
    emit_raw(ctx, " fxsh_wrap_");
    emit_mangled(ctx, public_name);
    emit_raw(ctx, "(void *env");
    for (u32 i = 0; i < arity; i++) {
        emit_raw(ctx, ", ");
        emit_c_type_for_type(ctx, nth_param_type(fn_t, i));
        emit_fmt(ctx, " _a%u", (unsigned)i);
    }
    emit_raw(ctx, ") {\n");
    ctx->indent_level++;
    emit_indent(ctx);
    emit_raw(ctx, "(void)env;\n");
    emit_indent(ctx);
    if (is_type_con_named(return_type_of_fn(fn_t), TYPE_UNIT)) {
        emit_raw(ctx, target_prefix);
        emit_mangled(ctx, public_name);
        emit_raw(ctx, "(");
        for (u32 i = 0; i < arity; i++) {
            if (i > 0)
                emit_raw(ctx, ", ");
            emit_fmt(ctx, "_a%u", (unsigned)i);
        }
        emit_raw(ctx, ");\n");
        emit_indent(ctx);
        emit_raw(ctx, "return;\n");
    } else {
        emit_raw(ctx, "return ");
        emit_raw(ctx, target_prefix);
        emit_mangled(ctx, public_name);
        emit_raw(ctx, "(");
        for (u32 i = 0; i < arity; i++) {
            if (i > 0)
                emit_raw(ctx, ", ");
            emit_fmt(ctx, "_a%u", (unsigned)i);
        }
        emit_raw(ctx, ");\n");
    }
    ctx->indent_level--;
    emit_line(ctx, "}");
    emit_raw(ctx, "\n");
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

static void flatten_call_args_in_order_rec(fxsh_ast_node_t *expr, fxsh_ast_list_t *out_args) {
    if (!expr || expr->kind != AST_CALL || !out_args)
        return;
    flatten_call_args_in_order_rec(expr->data.call.func, out_args);
    sp_dyn_array_for(expr->data.call.args, i) {
        sp_dyn_array_push(*out_args, expr->data.call.args[i]);
    }
}

static bool flatten_call_args_in_order(fxsh_ast_node_t *expr, fxsh_ast_list_t *out_args) {
    if (!expr || expr->kind != AST_CALL || !out_args)
        return false;
    *out_args = SP_NULLPTR;
    flatten_call_args_in_order_rec(expr, out_args);
    return true;
}

static bool tco_has_only_var_params(fxsh_ast_list_t params) {
    sp_dyn_array_for(params, i) {
        if (!is_pat_var_node(params[i]))
            return false;
    }
    return true;
}

static bool tco_is_self_tail_call(fxsh_ast_node_t *expr, sp_str_t self_name, u32 arity,
                                  fxsh_ast_list_t *out_args) {
    if (!expr || expr->kind != AST_CALL)
        return false;
    sp_str_t head = {0};
    u32 argc = 0;
    if (!flatten_call_head_ident(expr, &head, &argc))
        return false;
    if (!sp_str_equal(head, self_name) || argc != arity)
        return false;
    if (!flatten_call_args_in_order(expr, out_args))
        return false;
    return sp_dyn_array_size(*out_args) == argc;
}

static bool tco_has_tail_self_call(fxsh_ast_node_t *expr, sp_str_t self_name, u32 arity) {
    if (!expr)
        return false;

    fxsh_ast_list_t dummy = SP_NULLPTR;
    if (tco_is_self_tail_call(expr, self_name, arity, &dummy))
        return true;

    switch (expr->kind) {
        case AST_IF:
            return tco_has_tail_self_call(expr->data.if_expr.then_branch, self_name, arity) ||
                   tco_has_tail_self_call(expr->data.if_expr.else_branch, self_name, arity);
        case AST_LET_IN:
            return tco_has_tail_self_call(expr->data.let_in.body, self_name, arity);
        case AST_MATCH:
            sp_dyn_array_for(expr->data.match_expr.arms, i) {
                fxsh_ast_node_t *arm = expr->data.match_expr.arms[i];
                if (!arm || arm->kind != AST_MATCH_ARM)
                    continue;
                if (tco_has_tail_self_call(arm->data.match_arm.body, self_name, arity))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static void gen_tail_dispatch(codegen_ctx_t *ctx, fxsh_ast_node_t *expr, sp_str_t self_name,
                              fxsh_type_t *fn_t, fxsh_ast_list_t params, bool ret_unit) {
    if (!expr) {
        emit_indent(ctx);
        if (ret_unit)
            emit_raw(ctx, "return;\n");
        else
            emit_raw(ctx, "return 0;\n");
        return;
    }

    fxsh_ast_list_t self_args = SP_NULLPTR;
    if (tco_is_self_tail_call(expr, self_name, (u32)sp_dyn_array_size(params), &self_args)) {
        sp_dyn_array(char *) tmp_names = SP_NULLPTR;
        sp_dyn_array_for(self_args, i) {
            char tname[32];
            emit_temp(ctx, tname, sizeof(tname));
            size_t tn = strlen(tname);
            char *owned_tname = (char *)fxsh_alloc0(tn + 1);
            memcpy(owned_tname, tname, tn);
            sp_dyn_array_push(tmp_names, owned_tname);

            emit_indent(ctx);
            fxsh_type_t *pt = nth_param_type(fn_t, (u32)i);
            if (pt)
                emit_c_type_for_type(ctx, pt);
            else
                emit_raw(ctx, "__auto_type");
            emit_raw(ctx, " ");
            emit_raw(ctx, tmp_names[i]);
            emit_raw(ctx, " = ");
            gen_expr(ctx, self_args[i]);
            emit_raw(ctx, ";\n");
        }
        sp_dyn_array_for(params, i) {
            fxsh_ast_node_t *p = params[i];
            emit_indent(ctx);
            emit_mangled(ctx, p->data.ident);
            emit_raw(ctx, " = ");
            emit_raw(ctx, tmp_names[i]);
            emit_raw(ctx, ";\n");
        }
        emit_indent(ctx);
        emit_raw(ctx, "continue;\n");
        return;
    }

    if (expr->kind == AST_IF) {
        emit_indent(ctx);
        emit_raw(ctx, "if (");
        gen_expr(ctx, expr->data.if_expr.cond);
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;
        gen_tail_dispatch(ctx, expr->data.if_expr.then_branch, self_name, fn_t, params, ret_unit);
        ctx->indent_level--;
        emit_line(ctx, "} else {");
        ctx->indent_level++;
        gen_tail_dispatch(ctx, expr->data.if_expr.else_branch, self_name, fn_t, params, ret_unit);
        ctx->indent_level--;
        emit_line(ctx, "}");
        return;
    }

    if (expr->kind == AST_LET_IN) {
        emit_line(ctx, "{");
        ctx->indent_level++;
        sp_dyn_array_for(expr->data.let_in.bindings, i) {
            fxsh_ast_node_t *b = expr->data.let_in.bindings[i];
            if (!b || (b->kind != AST_LET && b->kind != AST_DECL_LET))
                continue;
            if (!b->data.let.pattern || b->data.let.pattern->kind != AST_PAT_VAR)
                continue;
            emit_indent(ctx);
            emit_raw(ctx, "__auto_type ");
            emit_mangled(ctx, b->data.let.name);
            emit_raw(ctx, " = ");
            gen_expr(ctx, b->data.let.value);
            emit_raw(ctx, ";\n");
        }
        gen_tail_dispatch(ctx, expr->data.let_in.body, self_name, fn_t, params, ret_unit);
        ctx->indent_level--;
        emit_line(ctx, "}");
        return;
    }

    if (expr->kind == AST_MATCH) {
        char mval[32];
        char mdone[32];
        emit_temp(ctx, mval, sizeof(mval));
        emit_temp(ctx, mdone, sizeof(mdone));

        fxsh_type_t *match_t_hint = infer_expr_type_strict_for_codegen(expr->data.match_expr.expr);

        emit_indent(ctx);
        emit_raw(ctx, "__auto_type ");
        emit_raw(ctx, mval);
        emit_raw(ctx, " = ");
        gen_expr(ctx, expr->data.match_expr.expr);
        emit_raw(ctx, ";\n");

        emit_indent(ctx);
        emit_raw(ctx, "bool ");
        emit_raw(ctx, mdone);
        emit_raw(ctx, " = false;\n");

        if (match_uses_switch_on_tag(expr)) {
            sp_dyn_array(sp_str_t) emitted_ctors = SP_NULLPTR;
            bool emitted_default = false;
            emit_indent(ctx);
            emit_raw(ctx, "switch (");
            emit_raw(ctx, mval);
            emit_raw(ctx, ".tag) {\n");
            ctx->indent_level++;
            sp_dyn_array_for(expr->data.match_expr.arms, i) {
                fxsh_ast_node_t *arm = expr->data.match_expr.arms[i];
                if (!arm || arm->kind != AST_MATCH_ARM)
                    continue;
                fxsh_ast_node_t *pat = arm->data.match_arm.pattern;
                emit_indent(ctx);
                if (pat && pat->kind == AST_PAT_CONSTR) {
                    if (str_in_list(emitted_ctors, pat->data.constr_appl.constr_name))
                        continue;
                    sp_dyn_array_push(emitted_ctors, pat->data.constr_appl.constr_name);
                    sp_str_t tname = adt_type_of_constructor(pat->data.constr_appl.constr_name);
                    if (tname.len == 0)
                        continue;
                    emit_raw(ctx, "case fxsh_tag_");
                    emit_mangled(ctx, tname);
                    emit_raw(ctx, "_");
                    emit_mangled(ctx, pat->data.constr_appl.constr_name);
                    emit_raw(ctx, ": {\n");
                } else if (pat && (pat->kind == AST_PAT_WILD || is_pat_var_node(pat))) {
                    if (emitted_default)
                        continue;
                    emitted_default = true;
                    emit_raw(ctx, "default: {\n");
                } else {
                    continue;
                }
                ctx->indent_level++;
                derive_pattern_var_types_from_match(pat, match_t_hint);
                u32 lty_m = local_type_mark();
                pat_var_types_push_local_types();
                gen_pattern_bindings(ctx, pat, mval, false, match_t_hint);
                emit_indent(ctx);
                emit_raw(ctx, mdone);
                emit_raw(ctx, " = true;\n");
                gen_tail_dispatch(ctx, arm->data.match_arm.body, self_name, fn_t, params, ret_unit);
                emit_indent(ctx);
                emit_raw(ctx, "break;\n");
                local_type_pop_to(lty_m);
                pat_var_types_reset();
                ctx->indent_level--;
                emit_line(ctx, "}");
            }
            ctx->indent_level--;
            emit_line(ctx, "}");
            sp_dyn_array_free(emitted_ctors);
        } else {
            sp_dyn_array_for(expr->data.match_expr.arms, i) {
                fxsh_ast_node_t *arm = expr->data.match_expr.arms[i];
                if (!arm || arm->kind != AST_MATCH_ARM)
                    continue;
                fxsh_ast_node_t *pat = arm->data.match_arm.pattern;

                emit_indent(ctx);
                emit_raw(ctx, "if (!");
                emit_raw(ctx, mdone);
                emit_raw(ctx, " && (");
                gen_pattern_condition(ctx, pat, mval);
                emit_raw(ctx, ")) {\n");
                ctx->indent_level++;

                derive_pattern_var_types_from_match(pat, match_t_hint);
                u32 lty_m = local_type_mark();
                pat_var_types_push_local_types();
                gen_pattern_bindings(ctx, pat, mval, false, match_t_hint);

                if (arm->data.match_arm.guard) {
                    emit_indent(ctx);
                    emit_raw(ctx, "if (");
                    gen_expr(ctx, arm->data.match_arm.guard);
                    emit_raw(ctx, ") {\n");
                    ctx->indent_level++;
                }

                emit_indent(ctx);
                emit_raw(ctx, mdone);
                emit_raw(ctx, " = true;\n");
                gen_tail_dispatch(ctx, arm->data.match_arm.body, self_name, fn_t, params, ret_unit);

                if (arm->data.match_arm.guard) {
                    ctx->indent_level--;
                    emit_line(ctx, "}");
                }

                local_type_pop_to(lty_m);
                pat_var_types_reset();
                ctx->indent_level--;
                emit_line(ctx, "}");
            }
        }
        emit_indent(ctx);
        emit_raw(ctx, "if (!");
        emit_raw(ctx, mdone);
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;
        emit_indent(ctx);
        if (ret_unit)
            emit_raw(ctx, "return;\n");
        else
            emit_raw(ctx, "return 0;\n");
        ctx->indent_level--;
        emit_line(ctx, "}");
        return;
    }

    emit_indent(ctx);
    if (ret_unit) {
        gen_expr(ctx, expr);
        emit_raw(ctx, ";\n");
        emit_indent(ctx);
        emit_raw(ctx, "return;\n");
    } else {
        emit_raw(ctx, "return ");
        gen_expr(ctx, expr);
        emit_raw(ctx, ";\n");
    }
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
    fxsh_type_t *ann_fn = fn_type_from_let_annotation(ast);
    if (ann_fn)
        fn_t = ann_fn;
    if (!fn_t || fn_t->kind != TYPE_ARROW)
        return false;

    sp_dyn_array(fxsh_ast_list_t) groups = SP_NULLPTR;
    fxsh_ast_node_t *final_body = NULL;
    if (!flatten_curried_lambda_groups(lam, &groups, &final_body))
        return false;
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
            if (!is_pat_var_node(p))
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
        u32 lty_m = local_type_mark();
        for (u32 g = 0; g < (u32)s; g++) {
            fxsh_ast_list_t ps = groups[g];
            u32 off = group_offsets[g];
            sp_dyn_array_for(ps, pi) {
                fxsh_ast_node_t *p = ps[pi];
                char buf[256];
                char tmp[128];
                mangle_into(p->data.ident, tmp, sizeof(tmp));
                snprintf(buf, sizeof(buf), "e->%s", tmp);
                sym_push_expr(p->data.ident, make_owned_str(buf));
                local_type_push(p->data.ident, param_types[off + (u32)pi]);
            }
        }
        sp_dyn_array_for(stage_params, i) {
            fxsh_ast_node_t *p = stage_params[i];
            if (!is_pat_var_node(p))
                continue;
            local_type_push(p->data.ident, param_types[stage_off + (u32)i]);
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
        local_type_pop_to(lty_m);
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

static void gen_decl_let_mono_specs(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast || (ast->kind != AST_DECL_LET && ast->kind != AST_LET))
        return;
    if (!ast->data.let.value || ast->data.let.value->kind != AST_LAMBDA)
        return;

    fxsh_ast_node_t *lam = ast->data.let.value;
    sp_dyn_array(fxsh_ast_list_t) groups = SP_NULLPTR;
    fxsh_ast_node_t *final_body = NULL;
    if (!flatten_curried_lambda_groups(lam, &groups, &final_body))
        return;

    u32 total_params = 0;
    sp_dyn_array_for(groups, gi) total_params += (u32)sp_dyn_array_size(groups[gi]);

    sp_dyn_array_for(g_mono_specs, i) {
        mono_spec_t *ms = &g_mono_specs[i];
        if (!sp_str_equal(ms->fn_name, ast->data.let.name))
            continue;
        if ((u32)sp_dyn_array_size(ms->arg_types) != total_params)
            continue;

        emit_indent(ctx);
        emit_raw(ctx, "static ");
        emit_c_type_for_type(ctx, ms->ret_type);
        emit_raw(ctx, " ");
        emit_string(ctx, ms->c_name);
        emit_raw(ctx, "(");
        u32 lty_m = local_type_mark();
        u32 arg_idx = 0;
        sp_dyn_array_for(groups, gi) {
            fxsh_ast_list_t params = groups[gi];
            sp_dyn_array_for(params, pi) {
                if (arg_idx > 0)
                    emit_raw(ctx, ", ");
                emit_c_type_for_type(ctx, ms->arg_types[arg_idx]);
                emit_raw(ctx, " ");
                fxsh_ast_node_t *p = params[pi];
                if (is_pat_var_node(p)) {
                    emit_mangled(ctx, p->data.ident);
                    local_type_push(p->data.ident, ms->arg_types[arg_idx]);
                } else {
                    emit_fmt(ctx, "_arg%u", (unsigned)arg_idx);
                }
                arg_idx++;
            }
        }
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;
        emit_indent(ctx);
        if (is_type_con_named(ms->ret_type, TYPE_UNIT)) {
            gen_expr(ctx, final_body);
            emit_raw(ctx, ";\n");
            emit_indent(ctx);
            emit_raw(ctx, "return;\n");
        } else {
            emit_raw(ctx, "return ");
            gen_expr(ctx, final_body);
            emit_raw(ctx, ";\n");
        }
        ctx->indent_level--;
        emit_line(ctx, "}");
        local_type_pop_to(lty_m);
        emit_raw(ctx, "\n");
    }
}

static void gen_decl_let(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    sp_str_t ffi_sym = (sp_str_t){0};
    if (let_is_ffi_decl(ast, &ffi_sym)) {
        fxsh_type_t *fn_t = lookup_symbol_type(ast->data.let.name);
        if (!fn_t || fn_t->kind != TYPE_ARROW)
            return;

        char wrap_buf[256];
        snprintf(wrap_buf, sizeof(wrap_buf), "fxsh_ffi_");
        size_t pos = strlen(wrap_buf);
        for (u32 j = 0; j < ast->data.let.name.len && pos + 8 < sizeof(wrap_buf); j++) {
            c8 c = ast->data.let.name.data[j];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_') {
                wrap_buf[pos++] = c;
            } else {
                wrap_buf[pos++] = '_';
            }
        }
        wrap_buf[pos] = '\0';
        sp_str_t wrap_name = make_owned_str(wrap_buf);
        char csym_buf[256];
        snprintf(csym_buf, sizeof(csym_buf), "fxsh_csym_");
        pos = strlen(csym_buf);
        for (u32 j = 0; j < ast->data.let.name.len && pos + 8 < sizeof(csym_buf); j++) {
            c8 c = ast->data.let.name.data[j];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_') {
                csym_buf[pos++] = c;
            } else {
                csym_buf[pos++] = '_';
            }
        }
        csym_buf[pos] = '\0';
        sp_str_t c_sym_alias = make_owned_str(csym_buf);

        u32 arity = fn_arity_of(fn_t);
        bool unit_arity = ffi_decl_unit_arity_from_ast(ast) || fn_is_unit_arity(fn_t);
        u32 c_arity = unit_arity ? 0 : arity;
        bool ret_unit = ffi_decl_returns_unit_from_ast(ast) || fn_returns_unit(fn_t);
        bool ret_string = type_is_string_con(return_type_of_fn(fn_t));
        emit_line(ctx, "#if defined(__APPLE__)");
        emit_indent(ctx);
        emit_raw(ctx, "extern ");
        emit_c_abi_type_for_type(ctx, return_type_of_fn(fn_t));
        emit_raw(ctx, " ");
        emit_string(ctx, c_sym_alias);
        emit_raw(ctx, "(");
        if (c_arity == 0) {
            emit_raw(ctx, "void");
        }
        for (u32 i = 0; i < c_arity; i++) {
            if (i > 0)
                emit_raw(ctx, ", ");
            emit_c_abi_type_for_type(ctx, nth_param_type(fn_t, i));
            emit_fmt(ctx, " _a%u", (unsigned)i);
        }
        emit_raw(ctx, ") __asm__(\"_");
        emit_string(ctx, ffi_sym);
        emit_raw(ctx, "\");\n");
        emit_line(ctx, "#else");
        emit_indent(ctx);
        emit_raw(ctx, "extern ");
        emit_c_abi_type_for_type(ctx, return_type_of_fn(fn_t));
        emit_raw(ctx, " ");
        emit_string(ctx, c_sym_alias);
        emit_raw(ctx, "(");
        if (c_arity == 0) {
            emit_raw(ctx, "void");
        }
        for (u32 i = 0; i < c_arity; i++) {
            if (i > 0)
                emit_raw(ctx, ", ");
            emit_c_abi_type_for_type(ctx, nth_param_type(fn_t, i));
            emit_fmt(ctx, " _a%u", (unsigned)i);
        }
        emit_raw(ctx, ") __asm__(\"");
        emit_string(ctx, ffi_sym);
        emit_raw(ctx, "\");\n");
        emit_line(ctx, "#endif");

        emit_indent(ctx);
        emit_raw(ctx, "static ");
        emit_c_type_for_type(ctx, return_type_of_fn(fn_t));
        emit_raw(ctx, " ");
        emit_string(ctx, wrap_name);
        emit_raw(ctx, "(");
        if (c_arity == 0) {
            emit_raw(ctx, "void");
        }
        for (u32 i = 0; i < c_arity; i++) {
            if (i > 0)
                emit_raw(ctx, ", ");
            emit_c_type_for_type(ctx, nth_param_type(fn_t, i));
            emit_fmt(ctx, " _a%u", (unsigned)i);
        }
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;
        for (u32 i = 0; i < c_arity; i++) {
            if (type_is_string_con(nth_param_type(fn_t, i))) {
                emit_indent(ctx);
                emit_fmt(ctx, "char *_cstr_%u = fxsh_cstr_dup(_a%u);\n", (unsigned)i, (unsigned)i);
            }
        }
        if (ret_unit) {
            emit_indent(ctx);
            emit_string(ctx, c_sym_alias);
            emit_raw(ctx, "(");
            for (u32 i = 0; i < c_arity; i++) {
                if (i > 0)
                    emit_raw(ctx, ", ");
                if (type_is_string_con(nth_param_type(fn_t, i)))
                    emit_fmt(ctx, "_cstr_%u", (unsigned)i);
                else
                    emit_fmt(ctx, "_a%u", (unsigned)i);
            }
            emit_raw(ctx, ");\n");
            for (u32 i = 0; i < c_arity; i++) {
                if (type_is_string_con(nth_param_type(fn_t, i))) {
                    emit_indent(ctx);
                    emit_fmt(ctx, "free(_cstr_%u);\n", (unsigned)i);
                }
            }
            emit_indent(ctx);
            emit_raw(ctx, "return;\n");
            ctx->indent_level--;
            emit_line(ctx, "}");
            emit_raw(ctx, "\n");
            sym_push_expr(ast->data.let.name, wrap_name);
            sp_dyn_array_push(g_extern_fn_names, ast->data.let.name);
            return;
        }
        if (ret_string) {
            emit_indent(ctx);
            emit_raw(ctx, "const char *_ret = ");
            emit_string(ctx, c_sym_alias);
            emit_raw(ctx, "(");
            for (u32 i = 0; i < c_arity; i++) {
                if (i > 0)
                    emit_raw(ctx, ", ");
                if (type_is_string_con(nth_param_type(fn_t, i)))
                    emit_fmt(ctx, "_cstr_%u", (unsigned)i);
                else
                    emit_fmt(ctx, "_a%u", (unsigned)i);
            }
            emit_raw(ctx, ");\n");
            for (u32 i = 0; i < c_arity; i++) {
                if (type_is_string_con(nth_param_type(fn_t, i))) {
                    emit_indent(ctx);
                    emit_fmt(ctx, "free(_cstr_%u);\n", (unsigned)i);
                }
            }
            emit_indent(ctx);
            emit_raw(ctx, "return fxsh_from_cstr(_ret);\n");
            ctx->indent_level--;
            emit_line(ctx, "}");
            emit_raw(ctx, "\n");
            sym_push_expr(ast->data.let.name, wrap_name);
            sp_dyn_array_push(g_extern_fn_names, ast->data.let.name);
            return;
        }
        emit_indent(ctx);
        emit_c_type_for_type(ctx, return_type_of_fn(fn_t));
        emit_raw(ctx, " _ret = ");
        emit_string(ctx, c_sym_alias);
        emit_raw(ctx, "(");
        for (u32 i = 0; i < c_arity; i++) {
            if (i > 0)
                emit_raw(ctx, ", ");
            if (type_is_string_con(nth_param_type(fn_t, i)))
                emit_fmt(ctx, "_cstr_%u", (unsigned)i);
            else
                emit_fmt(ctx, "_a%u", (unsigned)i);
        }
        emit_raw(ctx, ");\n");
        for (u32 i = 0; i < c_arity; i++) {
            if (type_is_string_con(nth_param_type(fn_t, i))) {
                emit_indent(ctx);
                emit_fmt(ctx, "free(_cstr_%u);\n", (unsigned)i);
            }
        }
        emit_indent(ctx);
        emit_raw(ctx, "return _ret;\n");
        ctx->indent_level--;
        emit_line(ctx, "}");
        emit_raw(ctx, "\n");

        sym_push_expr(ast->data.let.name, wrap_name);
        sp_dyn_array_push(g_extern_fn_names, ast->data.let.name);
        return;
    }

    /* Lambda let-binding becomes a C function (no closure capture support yet). */
    if (ast->data.let.value && ast->data.let.value->kind == AST_LAMBDA) {
        gen_decl_let_mono_specs(ctx, ast);
        if (gen_decl_let_closure_generic(ctx, ast))
            return;
        fxsh_ast_node_t *lam = ast->data.let.value;
        fxsh_type_t *fn_t = lookup_symbol_type(ast->data.let.name);
        fxsh_type_t *ann_fn = fn_type_from_let_annotation(ast);
        if (ann_fn)
            fn_t = ann_fn;
        sp_dyn_array_push(g_lambda_fn_names, ast->data.let.name);
        emit_indent(ctx);
        emit_raw(ctx, "static ");
        emit_c_type_for_type(ctx, return_type_of_fn(fn_t));
        emit_raw(ctx, " ");
        emit_raw(ctx, "fxsh_fn_");
        emit_mangled(ctx, ast->data.let.name);
        emit_raw(ctx, "(");
        u32 lty_m = local_type_mark();
        if (!lam->data.lambda.params || sp_dyn_array_size(lam->data.lambda.params) == 0) {
            emit_raw(ctx, "void");
        } else {
            sp_dyn_array_for(lam->data.lambda.params, i) {
                if (i > 0)
                    emit_raw(ctx, ", ");
                emit_c_type_for_type(ctx, nth_param_type(fn_t, (u32)i));
                emit_raw(ctx, " ");
                fxsh_ast_node_t *p = lam->data.lambda.params[i];
                if (is_pat_var_node(p)) {
                    emit_mangled(ctx, p->data.ident);
                    local_type_push(p->data.ident, nth_param_type(fn_t, (u32)i));
                } else {
                    emit_fmt(ctx, "_arg%u", (unsigned)i);
                }
            }
        }
        emit_raw(ctx, ") {\n");
        ctx->indent_level++;
        gen_decl_let_lambda_body(ctx, ast, lam, fn_t);
        ctx->indent_level--;
        emit_line(ctx, "}");
        emit_closure_wrapper(ctx, ast->data.let.name, "fxsh_fn_", fn_t);
        local_type_pop_to(lty_m);
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
        emit_c_storage_type_for_type(ctx, vt);
    }
    emit_raw(ctx, " ");
    emit_string(ctx, cname);
    emit_raw(ctx, ";\n");

    global_init_t gi = {.is_stmt = false, .c_name = cname, .rhs_expr = rhs, .expr = NULL};
    sp_dyn_array_push(g_global_inits, gi);
    sym_push_expr(ast->data.let.name, cname);
    if (is_closure_value) {
        closure_value_info_t cvi = {
            .name = ast->data.let.name, .root_fn = clo_root, .stage = clo_stage};
        sp_dyn_array_push(g_closure_values, cvi);
    }
}

static void gen_top_level_expr_init(fxsh_ast_node_t *ast) {
    if (!ast)
        return;
    global_init_t gi = {
        .is_stmt = true, .c_name = (sp_str_t){0}, .rhs_expr = (sp_str_t){0}, .expr = ast};
    sp_dyn_array_push(g_global_inits, gi);
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
    emit_line(ctx, "#include <sys/stat.h>");
    emit_line(ctx, "#include <sys/wait.h>");
    emit_line(ctx, "#include <unistd.h>");
    emit_line(ctx, "#include <glob.h>");
    emit_line(ctx, "");
    emit_line(ctx, "typedef int64_t  s64;");
    emit_line(ctx, "typedef int32_t  s32;");
    emit_line(ctx, "typedef uint64_t u64;");
    emit_line(ctx, "typedef uint32_t u32;");
    emit_line(ctx, "typedef double   f64;");
    emit_line(ctx, "typedef char     c8;");
    emit_line(ctx, "typedef s32 fxsh_type_var_t;");
    emit_line(ctx, "typedef struct { const char *data; u32 len; } sp_str_t;");
    emit_line(ctx, "typedef enum { TYPE_VAR, TYPE_CON, TYPE_ARROW, TYPE_TUPLE, TYPE_RECORD, "
                   "TYPE_APP } fxsh_type_kind_t;");
    emit_line(ctx, "typedef struct fxsh_type fxsh_type_t;");
    emit_line(ctx, "typedef struct { sp_str_t name; fxsh_type_t *type; } fxsh_field_t;");
    emit_line(ctx, "struct fxsh_type { fxsh_type_kind_t kind; union { fxsh_type_var_t var; "
                   "sp_str_t con; struct { fxsh_type_t *param; fxsh_type_t *ret; } arrow; "
                   "struct { u32 len; fxsh_type_t **items; } tuple; "
                   "struct { u32 len; fxsh_field_t *fields; fxsh_type_var_t row_var; } record; "
                   "struct { fxsh_type_t *con; fxsh_type_t *arg; } app; } data; };");
    emit_line(ctx, "typedef enum { FXSH_VAL_I64, FXSH_VAL_F64, FXSH_VAL_BOOL, FXSH_VAL_STR, "
                   "FXSH_VAL_PTR } fxsh_val_kind_t;");
    emit_line(ctx, "typedef struct { fxsh_val_kind_t kind; union { s64 i; f64 f; bool b; sp_str_t "
                   "s; void *p; } as; } fxsh_value_t;");
    emit_line(ctx, "typedef struct { void *call; void *env; } fxsh_closure_t;");
    emit_line(ctx, "typedef struct { u32 len; fxsh_value_t *items; } fxsh_tuple_t;");
    emit_line(ctx,
              "typedef struct { u32 len; const char **names; fxsh_value_t *vals; } fxsh_record_t;");
    emit_line(ctx, "typedef struct fxsh_list_s { bool is_nil; fxsh_value_t head; struct "
                   "fxsh_list_s *tail; } fxsh_list_t;");
    emit_line(ctx, "typedef struct { s64 rows; s64 cols; double *data; } fxsh_tensor_t;");
    emit_line(ctx, "extern bool fxsh_json_validate(sp_str_t json);");
    emit_line(ctx, "extern sp_str_t fxsh_json_compact(sp_str_t json);");
    emit_line(ctx, "extern sp_str_t fxsh_json_kind(sp_str_t json);");
    emit_line(ctx, "extern sp_str_t fxsh_json_quote_string(sp_str_t s);");
    emit_line(ctx, "extern bool fxsh_json_has(sp_str_t json, sp_str_t path);");
    emit_line(ctx, "extern sp_str_t fxsh_json_get(sp_str_t json, sp_str_t path);");
    emit_line(ctx, "extern sp_str_t fxsh_json_get_string(sp_str_t json, sp_str_t path);");
    emit_line(ctx, "extern s64 fxsh_json_get_int(sp_str_t json, sp_str_t path, bool *ok);");
    emit_line(ctx, "extern f64 fxsh_json_get_float(sp_str_t json, sp_str_t path, bool *ok);");
    emit_line(ctx, "extern bool fxsh_json_get_bool(sp_str_t json, sp_str_t path, bool *ok);");
    emit_line(ctx, "");
    emit_line(ctx, "extern void fxsh_set_argv_rt(sp_str_t argv0, s64 argc, char **argv);");
    emit_line(ctx, "extern sp_str_t fxsh_argv0_rt(void);");
    emit_line(ctx, "extern s64 fxsh_argc_rt(void);");
    emit_line(ctx, "extern sp_str_t fxsh_argv_at_rt(s64 index);");
    emit_line(ctx, "");
    emit_line(ctx, "extern sp_str_t fxsh_getenv_rt(sp_str_t key);");
    emit_line(ctx, "extern bool fxsh_file_exists_rt(sp_str_t path);");
    emit_line(ctx, "extern bool fxsh_is_dir_rt(sp_str_t path);");
    emit_line(ctx, "extern bool fxsh_is_file_rt(sp_str_t path);");
    emit_line(ctx, "extern sp_str_t fxsh_list_dir_text_rt(sp_str_t path);");
    emit_line(ctx, "extern sp_str_t fxsh_walk_dir_text_rt(sp_str_t path);");
    emit_line(ctx, "extern sp_str_t fxsh_getcwd_rt(void);");
    emit_line(ctx, "extern bool fxsh_mkdir_p_rt(sp_str_t path);");
    emit_line(ctx, "extern s64 fxsh_file_size_rt(sp_str_t path);");
    emit_line(ctx, "extern bool fxsh_remove_file_rt(sp_str_t path);");
    emit_line(ctx, "extern bool fxsh_rename_path_rt(sp_str_t src, sp_str_t dst);");
    emit_line(ctx, "extern s64 fxsh_str_len_rt(sp_str_t s);");
    emit_line(ctx, "extern sp_str_t fxsh_str_slice_rt(sp_str_t s, s64 start, s64 len);");
    emit_line(ctx, "extern s64 fxsh_str_find_rt(sp_str_t s, sp_str_t needle);");
    emit_line(ctx, "extern s64 fxsh_str_find_from_rt(sp_str_t s, sp_str_t needle, s64 start);");
    emit_line(ctx, "extern bool fxsh_str_starts_with_rt(sp_str_t s, sp_str_t prefix);");
    emit_line(ctx, "extern bool fxsh_str_ends_with_rt(sp_str_t s, sp_str_t suffix);");
    emit_line(ctx, "extern sp_str_t fxsh_str_trim_rt(sp_str_t s);");
    emit_line(ctx, "extern s64 fxsh_byte_at_rt(sp_str_t s, s64 index);");
    emit_line(ctx, "extern sp_str_t fxsh_byte_to_string_rt(s64 byte_value);");
    emit_line(ctx, "extern sp_str_t fxsh_split_words_rt(sp_str_t s);");
    emit_line(ctx, "extern sp_str_t fxsh_read_file(sp_str_t path);");
    emit_line(ctx, "extern int fxsh_write_file(sp_str_t path, sp_str_t content);");
    emit_line(ctx, "extern s64 fxsh_exec_rt(sp_str_t cmd);");
    emit_line(ctx, "extern s64 fxsh_exec_code_rt(sp_str_t cmd);");
    emit_line(ctx, "extern sp_str_t fxsh_exec_stdout_rt(sp_str_t cmd);");
    emit_line(ctx, "extern sp_str_t fxsh_exec_stderr_rt(sp_str_t cmd);");
    emit_line(ctx, "extern s64 fxsh_exec_capture_rt(sp_str_t cmd);");
    emit_line(ctx, "extern s64 fxsh_capture_code_rt(s64 capture_id);");
    emit_line(ctx, "extern sp_str_t fxsh_capture_stdout_rt(s64 capture_id);");
    emit_line(ctx, "extern sp_str_t fxsh_capture_stderr_rt(s64 capture_id);");
    emit_line(ctx, "extern bool fxsh_capture_release_rt(s64 capture_id);");
    emit_line(ctx, "extern sp_str_t fxsh_exec_stdin_rt(sp_str_t cmd, sp_str_t input);");
    emit_line(ctx, "extern s64 fxsh_exec_stdin_code_rt(sp_str_t cmd, sp_str_t input);");
    emit_line(ctx, "extern s64 fxsh_exec_stdin_capture_rt(sp_str_t cmd, sp_str_t input);");
    emit_line(ctx, "extern sp_str_t fxsh_exec_stdin_stderr_rt(sp_str_t cmd, sp_str_t input);");
    emit_line(ctx, "extern sp_str_t fxsh_exec_pipe_rt(sp_str_t left, sp_str_t right);");
    emit_line(ctx, "extern s64 fxsh_exec_pipe_code_rt(sp_str_t left, sp_str_t right);");
    emit_line(ctx, "extern s64 fxsh_exec_pipe_capture_rt(sp_str_t left, sp_str_t right);");
    emit_line(ctx, "extern s64 fxsh_exec_pipefail_capture_rt(sp_str_t left, sp_str_t right);");
    emit_line(ctx, "extern s64 fxsh_exec_pipefail3_capture_rt(sp_str_t c1, sp_str_t c2, sp_str_t "
                   "c3);");
    emit_line(ctx, "extern s64 fxsh_exec_pipefail4_capture_rt(sp_str_t c1, sp_str_t c2, sp_str_t "
                   "c3, sp_str_t c4);");
    emit_line(ctx, "extern sp_str_t fxsh_exec_pipe_stderr_rt(sp_str_t left, sp_str_t right);");
    emit_line(ctx, "extern sp_str_t fxsh_glob_rt(sp_str_t pattern);");
    emit_line(ctx, "extern sp_str_t fxsh_grep_lines_regex(sp_str_t pattern, sp_str_t text);");
    emit_line(ctx,
              "extern sp_str_t fxsh_replace_once(sp_str_t s, sp_str_t old_t, sp_str_t new_t);");
    emit_line(ctx, "extern char *fxsh_cstr_dup(sp_str_t s);");
    emit_line(ctx, "extern sp_str_t fxsh_from_cstr(const char *p);");
    emit_line(ctx, "extern bool fxsh_str_eq(sp_str_t a, sp_str_t b);");
    emit_line(ctx, "extern sp_str_t fxsh_str_concat(sp_str_t a, sp_str_t b);");
    emit_line(ctx, "");
    emit_line(ctx, "static inline fxsh_type_t *fxsh_codegen_type_new(void) {");
    emit_line(ctx, "  return (fxsh_type_t*)calloc(1, sizeof(fxsh_type_t));");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_type_t *fxsh_type_var(fxsh_type_var_t v) {");
    emit_line(ctx, "  fxsh_type_t *t = fxsh_codegen_type_new(); if (!t) return NULL; t->kind = "
                   "TYPE_VAR; t->data.var = v; return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_type_t *fxsh_type_con(sp_str_t name) {");
    emit_line(ctx, "  fxsh_type_t *t = fxsh_codegen_type_new(); if (!t) return NULL; t->kind = "
                   "TYPE_CON; t->data.con = name; return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_type_t *fxsh_type_arrow(fxsh_type_t *param, fxsh_type_t "
                   "*ret) {");
    emit_line(ctx, "  fxsh_type_t *t = fxsh_codegen_type_new(); if (!t) return NULL; t->kind = "
                   "TYPE_ARROW; t->data.arrow.param = param; t->data.arrow.ret = ret; return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_type_t *fxsh_type_apply(fxsh_type_t *con, fxsh_type_t "
                   "*arg) {");
    emit_line(ctx, "  fxsh_type_t *t = fxsh_codegen_type_new(); if (!t) return NULL; t->kind = "
                   "TYPE_APP; t->data.app.con = con; t->data.app.arg = arg; return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_type_t *fxsh_type_tuple_make(u32 len) {");
    emit_line(ctx, "  fxsh_type_t *t = fxsh_codegen_type_new(); if (!t) return NULL; t->kind = "
                   "TYPE_TUPLE; t->data.tuple.len = len; t->data.tuple.items = len ? "
                   "(fxsh_type_t**)calloc(len, sizeof(fxsh_type_t*)) : NULL; return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline void fxsh_type_tuple_set(fxsh_type_t *t, u32 idx, fxsh_type_t "
                   "*elem) {");
    emit_line(ctx, "  if (!t || t->kind != TYPE_TUPLE || idx >= t->data.tuple.len) return; "
                   "t->data.tuple.items[idx] = elem;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_type_t *fxsh_type_record_make(u32 len, fxsh_type_var_t "
                   "row_var) {");
    emit_line(ctx, "  fxsh_type_t *t = fxsh_codegen_type_new(); if (!t) return NULL; t->kind = "
                   "TYPE_RECORD; t->data.record.len = len; t->data.record.row_var = row_var; "
                   "t->data.record.fields = len ? (fxsh_field_t*)calloc(len, sizeof(fxsh_field_t)) "
                   ": NULL; return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline void fxsh_type_record_set(fxsh_type_t *t, u32 idx, sp_str_t "
                   "name, fxsh_type_t *field_t) {");
    emit_line(ctx, "  if (!t || t->kind != TYPE_RECORD || idx >= t->data.record.len) return; "
                   "t->data.record.fields[idx].name = name; "
                   "t->data.record.fields[idx].type = field_t;");
    emit_line(ctx, "}");
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
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_tuple(fxsh_tuple_t v) {");
    emit_line(ctx, "  fxsh_tuple_t *p = (fxsh_tuple_t*)malloc(sizeof(fxsh_tuple_t));");
    emit_line(ctx, "  if (!p) return fxsh_box_i64(0);");
    emit_line(ctx, "  *p = v;");
    emit_line(ctx, "  return fxsh_box_ptr((void*)p);");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_list(fxsh_list_t *v) {");
    emit_line(ctx, "  return fxsh_box_ptr((void*)v);");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_value_t fxsh_box_record(fxsh_record_t v) {");
    emit_line(ctx, "  fxsh_record_t *p = (fxsh_record_t*)malloc(sizeof(fxsh_record_t));");
    emit_line(ctx, "  if (!p) return fxsh_box_i64(0);");
    emit_line(ctx, "  *p = v;");
    emit_line(ctx, "  return fxsh_box_ptr((void*)p);");
    emit_line(ctx, "}");
    emit_line(ctx, "#define fxsh_autobox(v) _Generic((v), \\");
    emit_line(ctx, "  s64: fxsh_box_i64, \\");
    emit_line(ctx, "  int: fxsh_box_i64, \\");
    emit_line(ctx, "  f64: fxsh_box_f64, \\");
    emit_line(ctx, "  bool: fxsh_box_bool, \\");
    emit_line(ctx, "  sp_str_t: fxsh_box_str, \\");
    emit_line(ctx, "  fxsh_tuple_t: fxsh_box_tuple, \\");
    emit_line(ctx, "  fxsh_list_t*: fxsh_box_list, \\");
    emit_line(ctx, "  fxsh_record_t: fxsh_box_record, \\");
    emit_line(ctx, "  default: fxsh_box_i64)(v)");
    emit_line(ctx, "static inline bool fxsh_ptr_is_tuple(void *p) { (void)p; return true; }");
    emit_line(ctx, "static inline s64 fxsh_unbox_i64(fxsh_value_t v) {");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_I64) return v.as.i;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_BOOL) return v.as.b ? 1 : 0;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_F64) return (s64)v.as.f;");
    emit_line(ctx, "  return 0;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline f64 fxsh_unbox_f64(fxsh_value_t v) {");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_F64) return v.as.f;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_I64) return (f64)v.as.i;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_BOOL) return v.as.b ? 1.0 : 0.0;");
    emit_line(ctx, "  return 0.0;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline bool fxsh_unbox_bool(fxsh_value_t v) {");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_BOOL) return v.as.b;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_I64) return v.as.i != 0;");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_F64) return v.as.f != 0.0;");
    emit_line(ctx, "  return false;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline sp_str_t fxsh_unbox_str(fxsh_value_t v) {");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_STR) return v.as.s;");
    emit_line(ctx, "  return (sp_str_t){ .data = \"\", .len = 0 };");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_list_t *fxsh_unbox_list(fxsh_value_t v) {");
    emit_line(ctx, "  if (v.kind == FXSH_VAL_PTR) return (fxsh_list_t*)v.as.p;");
    emit_line(ctx, "  return NULL;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_tuple_t fxsh_tuple_make(u32 n) {");
    emit_line(ctx, "  fxsh_tuple_t t; t.len = n;");
    emit_line(ctx, "  t.items = (fxsh_value_t *)calloc((size_t)n, sizeof(fxsh_value_t));");
    emit_line(ctx, "  return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline void fxsh_tuple_set(fxsh_tuple_t *t, u32 i, fxsh_value_t v) {");
    emit_line(ctx, "  if (!t || i >= t->len) return;");
    emit_line(ctx, "  t->items[i] = v;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_value_t fxsh_tuple_get(fxsh_tuple_t t, u32 i) {");
    emit_line(ctx, "  if (i >= t.len) return fxsh_box_i64(0);");
    emit_line(ctx, "  return t.items[i];");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_list_t* fxsh_list_nil(void) {");
    emit_line(ctx, "  fxsh_list_t *n = (fxsh_list_t*)malloc(sizeof(fxsh_list_t));");
    emit_line(ctx, "  if (!n) return NULL;");
    emit_line(ctx, "  n->is_nil = true;");
    emit_line(ctx, "  n->tail = NULL;");
    emit_line(ctx, "  n->head = fxsh_box_i64(0);");
    emit_line(ctx, "  return n;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_list_t* fxsh_list_cons(fxsh_value_t head, fxsh_list_t "
                   "*tail) {");
    emit_line(ctx, "  fxsh_list_t *n = (fxsh_list_t*)malloc(sizeof(fxsh_list_t));");
    emit_line(ctx, "  if (!n) return NULL;");
    emit_line(ctx, "  n->is_nil = false;");
    emit_line(ctx, "  n->head = head;");
    emit_line(ctx, "  n->tail = tail;");
    emit_line(ctx, "  return n;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline bool fxsh_list_is_nil(fxsh_list_t *l) {");
    emit_line(ctx, "  return !l || l->is_nil;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_value_t fxsh_list_head(fxsh_list_t *l) {");
    emit_line(ctx, "  if (!l || l->is_nil) return fxsh_box_i64(0);");
    emit_line(ctx, "  return l->head;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_list_t *fxsh_list_tail(fxsh_list_t *l) {");
    emit_line(ctx, "  if (!l || l->is_nil) return fxsh_list_nil();");
    emit_line(ctx, "  return l->tail;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_list_t *fxsh_lines_to_list(sp_str_t text) {");
    emit_line(ctx, "  if (!text.data || text.len == 0) return fxsh_list_nil();");
    emit_line(ctx, "  fxsh_list_t *out = fxsh_list_nil();");
    emit_line(ctx, "  u32 end = text.len;");
    emit_line(ctx, "  while (end > 0) {");
    emit_line(ctx, "    u32 start = end;");
    emit_line(ctx, "    while (start > 0 && text.data[start - 1] != '\\n') start--;");
    emit_line(ctx, "    if (end > start) {");
    emit_line(ctx, "      sp_str_t line = { .data = text.data + start, .len = end - start };");
    emit_line(ctx, "      out = fxsh_list_cons(fxsh_box_str(line), out);");
    emit_line(ctx, "    }");
    emit_line(ctx, "    if (start == 0) break;");
    emit_line(ctx, "    end = start - 1;");
    emit_line(ctx, "  }");
    emit_line(ctx, "  return out;");
    emit_line(ctx, "}");
    emit_line(ctx,
              "static inline fxsh_tensor_t *fxsh_tensor_new2(s64 rows, s64 cols, double fill) {");
    emit_line(ctx, "  if (rows <= 0 || cols <= 0) return NULL;");
    emit_line(ctx, "  fxsh_tensor_t *t = (fxsh_tensor_t*)malloc(sizeof(fxsh_tensor_t));");
    emit_line(ctx, "  if (!t) return NULL;");
    emit_line(ctx, "  t->rows = rows; t->cols = cols;");
    emit_line(ctx, "  size_t n = (size_t)(rows * cols);");
    emit_line(ctx, "  t->data = (double*)calloc(n, sizeof(double));");
    emit_line(ctx, "  if (!t->data) return NULL;");
    emit_line(ctx, "  for (size_t i = 0; i < n; i++) t->data[i] = fill;");
    emit_line(ctx, "  return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_tensor_t *fxsh_tensor_from_list2(s64 rows, s64 cols, "
                   "fxsh_list_t *l) {");
    emit_line(ctx, "  fxsh_tensor_t *t = fxsh_tensor_new2(rows, cols, 0.0);");
    emit_line(ctx, "  if (!t) return NULL;");
    emit_line(ctx, "  size_t need = (size_t)(rows * cols);");
    emit_line(ctx, "  fxsh_list_t *cur = l;");
    emit_line(ctx, "  for (size_t i = 0; i < need; i++) {");
    emit_line(ctx, "    if (!cur || cur->is_nil) return NULL;");
    emit_line(ctx, "    t->data[i] = fxsh_unbox_f64(cur->head);");
    emit_line(ctx, "    cur = cur->tail;");
    emit_line(ctx, "  }");
    emit_line(ctx, "  if (cur && !cur->is_nil) return NULL;");
    emit_line(ctx, "  return t;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_tuple_t fxsh_tensor_shape2(fxsh_tensor_t *t) {");
    emit_line(ctx, "  fxsh_tuple_t out = fxsh_tuple_make(2);");
    emit_line(ctx, "  if (!t) return out;");
    emit_line(ctx, "  fxsh_tuple_set(&out, 0, fxsh_box_i64(t->rows));");
    emit_line(ctx, "  fxsh_tuple_set(&out, 1, fxsh_box_i64(t->cols));");
    emit_line(ctx, "  return out;");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline double fxsh_tensor_get2(fxsh_tensor_t *t, s64 i, s64 j) {");
    emit_line(ctx, "  if (!t || i < 0 || j < 0 || i >= t->rows || j >= t->cols) return 0.0;");
    emit_line(ctx, "  return t->data[(size_t)(i * t->cols + j)];");
    emit_line(ctx, "}");
    emit_line(ctx, "static inline fxsh_tensor_t *fxsh_tensor_set2(fxsh_tensor_t *t, s64 i, s64 j, "
                   "double v) {");
    emit_line(ctx, "  if (!t || i < 0 || j < 0 || i >= t->rows || j >= t->cols) return NULL;");
    emit_line(ctx, "  fxsh_tensor_t *out = fxsh_tensor_new2(t->rows, t->cols, 0.0);");
    emit_line(ctx, "  if (!out) return NULL;");
    emit_line(ctx, "  size_t n = (size_t)(t->rows * t->cols);");
    emit_line(ctx, "  memcpy(out->data, t->data, n * sizeof(double));");
    emit_line(ctx, "  out->data[(size_t)(i * t->cols + j)] = v;");
    emit_line(ctx, "  return out;");
    emit_line(ctx, "}");
    emit_line(ctx,
              "static inline fxsh_tensor_t *fxsh_tensor_add(fxsh_tensor_t *a, fxsh_tensor_t *b) {");
    emit_line(ctx, "  if (!a || !b || a->rows != b->rows || a->cols != b->cols) return NULL;");
    emit_line(ctx, "  fxsh_tensor_t *out = fxsh_tensor_new2(a->rows, a->cols, 0.0);");
    emit_line(ctx, "  if (!out) return NULL;");
    emit_line(ctx, "  size_t n = (size_t)(a->rows * a->cols);");
    emit_line(ctx, "  for (size_t i = 0; i < n; i++) out->data[i] = a->data[i] + b->data[i];");
    emit_line(ctx, "  return out;");
    emit_line(ctx, "}");
    emit_line(ctx,
              "static inline fxsh_tensor_t *fxsh_tensor_dot(fxsh_tensor_t *a, fxsh_tensor_t *b) {");
    emit_line(ctx, "  if (!a || !b || a->cols != b->rows) return NULL;");
    emit_line(ctx, "  fxsh_tensor_t *out = fxsh_tensor_new2(a->rows, b->cols, 0.0);");
    emit_line(ctx, "  if (!out) return NULL;");
    emit_line(ctx, "  for (s64 i = 0; i < a->rows; i++) {");
    emit_line(ctx, "    for (s64 j = 0; j < b->cols; j++) {");
    emit_line(ctx, "      double acc = 0.0;");
    emit_line(ctx, "      for (s64 k = 0; k < a->cols; k++) acc += a->data[(size_t)(i*a->cols+k)] "
                   "* b->data[(size_t)(k*b->cols+j)];");
    emit_line(ctx, "      out->data[(size_t)(i*out->cols+j)] = acc;");
    emit_line(ctx, "    }");
    emit_line(ctx, "  }");
    emit_line(ctx, "  return out;");
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
        if (g_global_inits[i].is_stmt) {
            emit_indent(ctx);
            gen_expr(ctx, g_global_inits[i].expr);
            emit_raw(ctx, ";\n");
        } else {
            emit_indent(ctx);
            emit_string(ctx, g_global_inits[i].c_name);
            emit_raw(ctx, " = ");
            emit_string(ctx, g_global_inits[i].rhs_expr);
            emit_raw(ctx, ";\n");
        }
    }
    ctx->indent_level--;
    emit_line(ctx, "}");
    emit_raw(ctx, "\n");
}

static void gen_epilogue(codegen_ctx_t *ctx) {
    gen_adt_init_fn(ctx);
    emit_line(ctx, "int main(int argc, char **argv) {");
    ctx->indent_level++;
    emit_line(
        ctx,
        "fxsh_set_argv_rt(argc > 1 ? fxsh_from_cstr(argv[1]) : (argc > 0 ? fxsh_from_cstr(argv[0]) "
        ": fxsh_from_cstr(\"\")), argc > 2 ? (s64)(argc - 2) : 0, argc > 2 ? argv + 2 : NULL);");
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
    g_adt_constr_sigs = SP_NULLPTR;
    g_adt_inits = SP_NULLPTR;
    g_global_inits = SP_NULLPTR;
    g_lambda_fn_names = SP_NULLPTR;
    g_closure_fn_names = SP_NULLPTR;
    g_closure_infos = SP_NULLPTR;
    g_closure_values = SP_NULLPTR;
    g_decl_fn_names = SP_NULLPTR;
    g_extern_fn_names = SP_NULLPTR;
    g_sym_stack = SP_NULLPTR;
    g_local_type_stack = SP_NULLPTR;
    g_top_let_counter = 0;
    g_codegen_type_env = SP_NULLPTR;
    g_codegen_constr_env = SP_NULLPTR;
    g_mono_decls = SP_NULLPTR;
    g_mono_specs = SP_NULLPTR;
    g_pat_tvar_counter = 2000000000;
    pat_var_types_reset();

    /* Build a type environment so signatures are generated with concrete types. */
    if (ast) {
        fxsh_type_env_t tenv = SP_NULLPTR;
        fxsh_constr_env_t cenv = SP_NULLPTR;
        fxsh_type_t *program_t = NULL;
        if (fxsh_type_infer(ast, &tenv, &cenv, &program_t) == ERR_OK) {
            g_codegen_type_env = tenv;
            g_codegen_constr_env = cenv;
            collect_mono_specs(ast);
        }
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
                        if (is_pat_var_node(p)) {
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
            else if (d->kind != AST_TYPE_DEF && d->kind != AST_DECL_IMPORT)
                gen_top_level_expr_init(d);
            /* TYPE_DEF already handled */
        }
    }

    gen_epilogue(&ctx);
    sp_dyn_array_push(out_arr, '\0');

    u32 len = (u32)sp_dyn_array_size(out_arr);
    char *result = (char *)sp_alloc(len);
    memcpy(result, out_arr, len);
    sp_dyn_array_free(out_arr);
    pat_var_types_reset();
    if (g_local_type_stack)
        sp_dyn_array_free(g_local_type_stack);
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
