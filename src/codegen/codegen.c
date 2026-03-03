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
static void gen_pattern(codegen_ctx_t *ctx, fxsh_ast_node_t *pattern, sp_str_t value_var);
static void gen_match(codegen_ctx_t *ctx, fxsh_ast_node_t *ast);

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

/*=============================================================================
 * ADT Code Generation
 *===========================================================================*/

/* Track generated type definitions to avoid duplicates */
static sp_ht(sp_str_t, bool) generated_types = SP_NULLPTR;

/* Track ADT type names for proper type resolution */
static sp_dyn_array(sp_str_t) adt_type_names = SP_NULLPTR;

/* Convert a type identifier to C type string */
static void emit_type_for_ident(codegen_ctx_t *ctx, sp_str_t type_name, sp_str_t current_type) {
    /* Check if it's a primitive type */
    if (sp_str_equal(type_name, sp_str_lit("int"))) {
        emit_raw(ctx, "s64");
    } else if (sp_str_equal(type_name, sp_str_lit("bool"))) {
        emit_raw(ctx, "bool");
    } else if (sp_str_equal(type_name, sp_str_lit("float"))) {
        emit_raw(ctx, "f64");
    } else if (sp_str_equal(type_name, sp_str_lit("string"))) {
        emit_raw(ctx, "sp_str_t");
    } else if (sp_str_equal(type_name, sp_str_lit("unit"))) {
        emit_raw(ctx, "void");
    } else {
        /* Check if it's a known ADT type (including self-reference) */
        bool is_adt = false;
        if (adt_type_names) {
            sp_dyn_array_for(adt_type_names, i) {
                if (sp_str_equal(adt_type_names[i], type_name)) {
                    is_adt = true;
                    break;
                }
            }
        }

        if (is_adt || (current_type.len > 0 && sp_str_equal(type_name, current_type))) {
            /* ADT type - use struct type */
            emit_raw(ctx, "fxsh_");
            sp_str_t mangled = mangle_name(type_name);
            emit_string(ctx, mangled);
            emit_raw(ctx, "_t");
        } else {
            /* Unknown type - default to s64 */
            emit_raw(ctx, "s64 /* ");
            emit_string(ctx, type_name);
            emit_raw(ctx, " */");
        }
    }
}

/* Generate C struct definition for an ADT type */
static void gen_type_def_struct(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_TYPE_DEF)
        return;

    sp_str_t type_name = ast->data.type_def.name;

    /* Check if already generated */
    if (generated_types) {
        bool *found = sp_ht_getp(generated_types, type_name);
        if (found && *found)
            return;
    } else {
        sp_ht_init(generated_types);
    }
    sp_ht_insert(generated_types, type_name, true);

    /* Track this ADT type name for type resolution */
    if (!adt_type_names)
        adt_type_names = SP_NULLPTR;
    sp_dyn_array_push(adt_type_names, type_name);

    fxsh_ast_list_t constructors = ast->data.type_def.constructors;
    u32 num_constrs = (u32)sp_dyn_array_size(constructors);

    if (num_constrs == 0)
        return;

    /* Generate tag enum */
    emit_line(ctx, "/* ADT type definition */");
    emit_indent(ctx);
    emit_raw(ctx, "typedef enum {\n");
    ctx->indent_level++;

    sp_dyn_array_for(constructors, i) {
        fxsh_ast_node_t *constr = constructors[i];
        if (constr->kind != AST_DATA_CONSTR)
            continue;

        emit_indent(ctx);
        /* Mangle constructor name for enum: type_name_tag_constr_name */
        emit_raw(ctx, "fxsh_tag_");
        sp_str_t mangled = mangle_name(type_name);
        emit_string(ctx, mangled);
        emit_raw(ctx, "_");
        sp_str_t constr_mangled = mangle_name(constr->data.data_constr.name);
        emit_string(ctx, constr_mangled);

        if (i < num_constrs - 1)
            emit_raw(ctx, ",");
        emit_raw(ctx, "\n");
    }

    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "} fxsh_tag_");
    sp_str_t mangled_type = mangle_name(type_name);
    emit_string(ctx, mangled_type);
    emit_raw(ctx, "_t;\n");

    /* For self-referential types, we need a different approach:
     * Use a pointer for recursive fields instead of embedding the struct directly */

    /* Generate union for constructor payloads */
    emit_raw(ctx, "\n");
    emit_indent(ctx);
    emit_raw(ctx, "typedef union {\n");
    ctx->indent_level++;

    sp_dyn_array_for(constructors, i) {
        fxsh_ast_node_t *constr = constructors[i];
        if (constr->kind != AST_DATA_CONSTR)
            continue;

        u32 num_args = (u32)sp_dyn_array_size(constr->data.data_constr.arg_types);
        if (num_args == 0)
            continue;

        emit_indent(ctx);
        emit_raw(ctx, "struct {\n");
        ctx->indent_level++;

        /* Generate fields for each argument */
        sp_dyn_array_for(constr->data.data_constr.arg_types, j) {
            emit_indent(ctx);
            fxsh_ast_node_t *arg_type = constr->data.data_constr.arg_types[j];
            if (arg_type->kind == AST_IDENT) {
                /* Check if this is a self-reference */
                if (sp_str_equal(arg_type->data.ident, type_name)) {
                    /* Use pointer for recursive types */
                    emit_raw(ctx, "struct fxsh_");
                    emit_string(ctx, mangled_type);
                    emit_raw(ctx, "* _");
                } else {
                    emit_type_for_ident(ctx, arg_type->data.ident, type_name);
                    emit_raw(ctx, " _");
                }
            } else {
                emit_raw(ctx, "s64 _");
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", (unsigned)j);
            emit_raw(ctx, buf);
            emit_raw(ctx, ";\n");
        }

        ctx->indent_level--;
        emit_indent(ctx);
        emit_raw(ctx, "} ");
        sp_str_t constr_mangled = mangle_name(constr->data.data_constr.name);
        emit_string(ctx, constr_mangled);
        emit_raw(ctx, ";\n");
    }

    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "} fxsh_data_");
    emit_string(ctx, mangled_type);
    emit_raw(ctx, "_t;\n");

    /* Generate main struct */
    emit_raw(ctx, "\n");
    emit_indent(ctx);
    emit_raw(ctx, "typedef struct {\n");
    ctx->indent_level++;

    emit_indent(ctx);
    emit_raw(ctx, "fxsh_tag_");
    emit_string(ctx, mangled_type);
    emit_raw(ctx, "_t tag;\n");

    emit_indent(ctx);
    emit_raw(ctx, "fxsh_data_");
    emit_string(ctx, mangled_type);
    emit_raw(ctx, "_t data;\n");

    ctx->indent_level--;
    emit_indent(ctx);
    emit_raw(ctx, "} fxsh_");
    emit_string(ctx, mangled_type);
    embed_raw(ctx, "_t;\n\n");
}

/* Generate constructor functions for an ADT type */
static void gen_type_def_constructors(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_TYPE_DEF)
        return;

    sp_str_t type_name = ast->data.type_def.name;
    sp_str_t mangled_type = mangle_name(type_name);
    fxsh_ast_list_t constructors = ast->data.type_def.constructors;

    sp_dyn_array_for(constructors, i) {
        fxsh_ast_node_t *constr = constructors[i];
        if (constr->kind != AST_DATA_CONSTR)
            continue;

        sp_str_t constr_name = constr->data.data_constr.name;
        sp_str_t mangled_constr = mangle_name(constr_name);
        u32 num_args = (u32)sp_dyn_array_size(constr->data.data_constr.arg_types);

        /* Generate constructor function */
        emit_indent(ctx);
        emit_raw(ctx, "static fxsh_");
        emit_string(ctx, mangled_type);
        emit_raw(ctx, "_t fxsh_constr_");
        emit_string(ctx, mangled_constr);
        emit_raw(ctx, "(");

        /* Parameters */
        for (u32 j = 0; j < num_args; j++) {
            if (j > 0)
                emit_raw(ctx, ", ");
            fxsh_ast_node_t *arg_type = constr->data.data_constr.arg_types[j];
            if (arg_type->kind == AST_IDENT) {
                emit_type_for_ident(ctx, arg_type->data.ident, type_name);
            } else {
                emit_raw(ctx, "s64");
            }
            emit_raw(ctx, " arg");
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", (unsigned)j);
            emit_raw(ctx, buf);
        }
        if (num_args == 0) {
            emit_raw(ctx, "void");
        }
        emit_raw(ctx, ") {\n");

        ctx->indent_level++;

        emit_indent(ctx);
        emit_raw(ctx, "fxsh_");
        emit_string(ctx, mangled_type);
        emit_raw(ctx, "_t val;\n");

        emit_indent(ctx);
        emit_raw(ctx, "val.tag = fxsh_tag_");
        emit_string(ctx, mangled_type);
        emit_raw(ctx, "_");
        emit_string(ctx, mangled_constr);
        emit_raw(ctx, ";\n");

        /* Set data fields if any */
        for (u32 j = 0; j < num_args; j++) {
            emit_indent(ctx);
            emit_raw(ctx, "val.data.");
            emit_string(ctx, mangled_constr);
            emit_raw(ctx, "._");
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", (unsigned)j);
            emit_raw(ctx, buf);
            emit_raw(ctx, " = arg");
            emit_raw(ctx, buf);
            emit_raw(ctx, ";\n");
        }

        emit_indent(ctx);
        emit_raw(ctx, "return val;\n");

        ctx->indent_level--;
        emit_line(ctx, "}");
        emit_raw(ctx, "\n");
    }
}

static void gen_type_def(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* First pass: generate struct definitions */
    gen_type_def_struct(ctx, ast);

    /* Second pass: generate constructor functions */
    gen_type_def_constructors(ctx, ast);
}

/* Generate constructor application: Some 42 -> fxsh_constr_Some(42) */
static void gen_constr_appl(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_CONSTR_APPL)
        return;

    sp_str_t constr_name = ast->data.constr_appl.constr_name;
    sp_str_t mangled = mangle_name(constr_name);

    emit_raw(ctx, "fxsh_constr_");
    emit_string(ctx, mangled);
    emit_raw(ctx, "(");

    sp_dyn_array_for(ast->data.constr_appl.args, i) {
        if (i > 0)
            emit_raw(ctx, ", ");
        gen_expr(ctx, ast->data.constr_appl.args[i]);
    }

    emit_raw(ctx, ")");
}

static void gen_pattern(codegen_ctx_t *ctx, fxsh_ast_node_t *pattern, sp_str_t value_var) {
    /* TODO: implement pattern matching code generation */
    emit_raw(ctx, "/* pattern */ ");
    emit_string(ctx, value_var);
}

static void gen_match(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* TODO: implement match expression code generation */
    emit_raw(ctx, "/* match */ 0");
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
        case AST_CONSTR_APPL:
            gen_constr_appl(ctx, ast);
            break;
        case AST_MATCH:
            gen_match(ctx, ast);
            break;
        case AST_TUPLE:
            /* For now, emit first element of tuple for constructor args */
            if (ast->data.elements && sp_dyn_array_size(ast->data.elements) > 0) {
                gen_expr(ctx, ast->data.elements[0]);
            } else {
                emit_raw(ctx, "/* empty tuple */ 0");
            }
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

/* Get type name from constructor application */
static sp_str_t get_constr_type_name(fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_CONSTR_APPL)
        return (sp_str_t){0};

    sp_str_t constr_name = ast->data.constr_appl.constr_name;

    /* For now, use simple heuristics based on common constructor names */
    /* In a full implementation, we'd look up the type from the constructor environment */
    if (sp_str_equal(constr_name, sp_str_lit("None")) ||
        sp_str_equal(constr_name, sp_str_lit("Some"))) {
        return sp_str_lit("option");
    }
    if (sp_str_equal(constr_name, sp_str_lit("Nil")) ||
        sp_str_equal(constr_name, sp_str_lit("Cons"))) {
        return sp_str_lit("list");
    }
    if (sp_str_equal(constr_name, sp_str_lit("Leaf")) ||
        sp_str_equal(constr_name, sp_str_lit("Node"))) {
        return sp_str_lit("tree");
    }
    if (sp_str_equal(constr_name, sp_str_lit("Ok")) ||
        sp_str_equal(constr_name, sp_str_lit("Err"))) {
        return sp_str_lit("result");
    }

    return (sp_str_t){0};
}

/* Track ADT let declarations that need runtime initialization */
typedef struct {
    sp_str_t name;
    sp_str_t type_name;
    fxsh_ast_node_t *value;
} adt_init_entry_t;

static sp_dyn_array(adt_init_entry_t) adt_inits = SP_NULLPTR;

static void gen_decl_let(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    /* Global variable declaration */
    emit_indent(ctx);

    /* Determine the type of the value */
    fxsh_ast_node_t *value = ast->data.let.value;
    sp_str_t type_name = {0};

    if (value && value->kind == AST_CONSTR_APPL) {
        type_name = get_constr_type_name(value);
    }

    sp_str_t mangled = mangle_name(ast->data.let.name);

    if (type_name.len > 0) {
        /* ADT type - declare without initializer, will init at runtime */
        emit_raw(ctx, "static fxsh_");
        sp_str_t mangled_type = mangle_name(type_name);
        emit_string(ctx, mangled_type);
        emit_raw(ctx, "_t ");
        emit_string(ctx, mangled);
        emit_raw(ctx, ";\n");

        /* Track for runtime initialization */
        adt_init_entry_t entry = {.name = mangled, .type_name = mangled_type, .value = value};
        sp_dyn_array_push(adt_inits, entry);
    } else {
        /* Default to s64 */
        emit_raw(ctx, "static s64 ");
        emit_string(ctx, mangled);
        emit_raw(ctx, " = ");
        gen_expr(ctx, ast->data.let.value);
        emit_raw(ctx, ";\n");
    }
}

/* Generate runtime initialization for ADT values */
static void gen_adt_runtime_inits(codegen_ctx_t *ctx) {
    if (!adt_inits || sp_dyn_array_size(adt_inits) == 0)
        return;

    emit_line(ctx, "/* Runtime initialization for ADT values */");
    emit_line(ctx, "static void fxsh_init_adt_values(void) {");
    ctx->indent_level++;

    sp_dyn_array_for(adt_inits, i) {
        emit_indent(ctx);
        emit_string(ctx, adt_inits[i].name);
        emit_raw(ctx, " = ");
        gen_expr(ctx, adt_inits[i].value);
        emit_raw(ctx, ";\n");
    }

    ctx->indent_level--;
    emit_line(ctx, "}");
    emit_raw(ctx, "\n");
}

/*============================================================================-
 * Program Code Generation
 *=============================================================================*/

static void gen_program_prelude(codegen_ctx_t *ctx) {
    emit_line(ctx, "/* Generated by fxsh */");
    emit_line(ctx, "#include <stdbool.h>");
    emit_line(ctx, "#include <stdint.h>");
    emit_line(ctx, "");
    emit_line(ctx, "/* Type aliases */");
    emit_line(ctx, "typedef int64_t s64;");
    emit_line(ctx, "typedef int32_t s32;");
    emit_line(ctx, "typedef int16_t s16;");
    emit_line(ctx, "typedef int8_t s8;");
    emit_line(ctx, "typedef uint64_t u64;");
    emit_line(ctx, "typedef uint32_t u32;");
    emit_line(ctx, "typedef uint16_t u16;");
    emit_line(ctx, "typedef uint8_t u8;");
    emit_line(ctx, "typedef float f32;");
    emit_line(ctx, "typedef double f64;");
    emit_line(ctx, "typedef char c8;");
    emit_line(ctx, "");
    emit_line(ctx, "/* ADT Type Definitions */");
    emit_line(ctx, "");
}

/* Forward declaration */
static void gen_type_def(codegen_ctx_t *ctx, fxsh_ast_node_t *ast);

/* First pass: collect and generate all type definitions */
static void gen_type_definitions(codegen_ctx_t *ctx, fxsh_ast_node_t *ast) {
    if (!ast || ast->kind != AST_PROGRAM)
        return;

    sp_dyn_array_for(ast->data.decls, i) {
        fxsh_ast_node_t *decl = ast->data.decls[i];
        if (decl->kind == AST_TYPE_DEF) {
            gen_type_def(ctx, decl);
        }
    }

    emit_line(ctx, "/* Forward declarations */");
    emit_line(ctx, "");
}

static void gen_program_epilogue(codegen_ctx_t *ctx) {
    /* Generate ADT runtime initialization function if needed */
    gen_adt_runtime_inits(ctx);

    emit_line(ctx, "/* Main entry point */");
    emit_line(ctx, "int main(void) {");
    ctx->indent_level++;

    /* Call ADT initialization if needed */
    if (adt_inits && sp_dyn_array_size(adt_inits) > 0) {
        emit_line(ctx, "fxsh_init_adt_values();");
    }

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

    /* Initialize output array and clear ADT init tracking */
    *ctx.output = SP_NULLPTR;
    adt_inits = SP_NULLPTR;

    gen_program_prelude(&ctx);

    if (ast && ast->kind == AST_PROGRAM) {
        /* First pass: type definitions */
        gen_type_definitions(&ctx, ast);

        /* Second pass: forward declarations */
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

        /* Third pass: definitions */
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *decl = ast->data.decls[i];
            switch (decl->kind) {
                case AST_DECL_FN:
                    gen_decl_fn(&ctx, decl);
                    break;
                case AST_DECL_LET:
                    gen_decl_let(&ctx, decl);
                    break;
                case AST_TYPE_DEF:
                    /* Type definitions already generated in first pass */
                    break;
                default:
                    /* Expression at top level */
                    emit_indent(&ctx);
                    emit_raw(&ctx, "/* expr */\n");
                    break;
            }
        }

        /* Generate epilogue for programs */
        gen_program_epilogue(&ctx);
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
