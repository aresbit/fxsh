/*
 * comptime.c - fxsh compile-time evaluation engine
 *
 * Implements Zig-style comptime execution:
 * - Compile-time value representation
 * - Type reflection
 * - Code generation macros
 */

#include "fxsh.h"

#include <stdio.h>
#include <string.h>

/*=============================================================================
 * Comptime Value Constructors
 *=============================================================================*/

fxsh_ct_value_t *fxsh_ct_unit(void) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_UNIT;
    return val;
}

fxsh_ct_value_t *fxsh_ct_bool(bool b) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_BOOL;
    val->data.bool_val = b;
    return val;
}

fxsh_ct_value_t *fxsh_ct_int(s64 n) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_INT;
    val->data.int_val = n;
    return val;
}

fxsh_ct_value_t *fxsh_ct_float(f64 f) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_FLOAT;
    val->data.float_val = f;
    return val;
}

fxsh_ct_value_t *fxsh_ct_string(sp_str_t s) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_STRING;
    val->data.string_val = s;
    return val;
}

fxsh_ct_value_t *fxsh_ct_type(fxsh_type_t *t) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_TYPE;
    val->data.type_val = t;
    return val;
}

fxsh_ct_value_t *fxsh_ct_ast(fxsh_ast_node_t *ast) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_AST;
    val->data.ast_val = ast;
    return val;
}

fxsh_ct_value_t *fxsh_ct_list(fxsh_ct_value_t **items, u32 len) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
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
}

/*=============================================================================
 * Value Lookup
 *=============================================================================*/

static fxsh_ct_value_t *lookup_var(fxsh_comptime_ctx_t *ctx, sp_str_t name) {
    if (!ctx->env)
        return NULL;
    return sp_ht_getp(*ctx->env, name);
}

static void bind_var(fxsh_comptime_ctx_t *ctx, sp_str_t name, fxsh_ct_value_t *val) {
    if (!ctx->env) {
        ctx->env = sp_alloc(sizeof(fxsh_ct_env_t));
        *ctx->env = SP_NULLPTR;
    }
    sp_ht_insert(*ctx->env, name, *val);
}

/*=============================================================================
 * Expression Evaluation
 *=============================================================================*/

static fxsh_ct_value_t *eval_expr(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx);

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
        if (binding->kind == AST_DECL_LET) {
            fxsh_ct_value_t *val = eval_expr(binding->data.let.value, ctx);
            if (val) {
                bind_var(ctx, binding->data.let.name, val);
            }
        }
    }

    return eval_expr(ast->data.let_in.body, ctx);
}

static fxsh_ct_value_t *eval_call(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *func = eval_expr(ast->data.call.func, ctx);
    if (!func || func->kind != CT_FUNCTION)
        return NULL;

    /* Create new context with parameter bindings */
    fxsh_comptime_ctx_t new_ctx = *ctx;
    new_ctx.env = sp_alloc(sizeof(fxsh_ct_env_t));
    *new_ctx.env = SP_NULLPTR;

    /* Copy existing bindings */
    if (ctx->env) {
        /* TODO: Copy environment */
    }

    /* Bind arguments to parameters */
    sp_dyn_array_for(ast->data.call.args, i) {
        if (i < sp_dyn_array_size(func->data.func_val.params)) {
            fxsh_ct_value_t *arg = eval_expr(ast->data.call.args[i], ctx);
            if (arg) {
                /* Get param name from lambda */
                fxsh_ast_node_t *param = func->data.func_val.params[i];
                if (param->kind == AST_IDENT) {
                    bind_var(&new_ctx, param->data.ident, arg);
                }
            }
        }
    }

    return eval_expr(func->data.func_val.body, &new_ctx);
}

static fxsh_ct_value_t *eval_lambda(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
    val->kind = CT_FUNCTION;
    val->data.func_val.params = ast->data.lambda.params;
    val->data.func_val.body = ast->data.lambda.body;
    val->data.func_val.closure = ctx->type_env;
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
            /* Evaluate operand to get type */
            fxsh_ct_value_t *type_val = eval_expr(ast->data.ct_type_op.type_val, ctx);
            if (!type_val) {
                /* Try to get type from expression */
                type_val = fxsh_ct_op_type_of(ast, ctx);
            }
            return fxsh_ct_op_size_of(type_val);
        }
        case AST_CT_ALIGN_OF: {
            fxsh_ct_value_t *type_val = eval_expr(ast->data.ct_type_op.type_val, ctx);
            return fxsh_ct_op_align_of(type_val);
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
    if (ast->kind == AST_DECL_LET && ast->data.let.is_comptime) {
        fxsh_ct_value_t *val = eval_expr(ast->data.let.value, ctx);
        if (val) {
            bind_var(ctx, ast->data.let.name, val);
            result.value = val;
        } else {
            result.error = ERR_INTERNAL;
        }
        return result;
    }

    result.value = eval_expr(ast, ctx);
    if (!result.value) {
        result.error = ERR_INTERNAL;
    }

    return result;
}

fxsh_ct_value_t *fxsh_ct_eval_expr(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx) {
    fxsh_ct_result_t result = fxsh_ct_eval(ast, ctx);
    return result.value;
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

    fxsh_type_info_t *info = sp_alloc(sizeof(fxsh_type_info_t));
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
    fxsh_ast_node_t *node = sp_alloc(sizeof(fxsh_ast_node_t));
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

/*=============================================================================
 * Derived Macros
 *=============================================================================*/

fxsh_ast_node_t *fxsh_ct_derive_show(fxsh_type_t *type) {
    /* Generate show function for a type */
    if (!type)
        return NULL;

    /* Create: fn show(x: T) -> string = ... */
    fxsh_ast_node_t *func = sp_alloc(sizeof(fxsh_ast_node_t));
    func->kind = AST_LAMBDA;
    func->loc = (fxsh_loc_t){0};

    /* Parameter: x */
    fxsh_ast_list_t params = SP_NULLPTR;
    sp_dyn_array_push(params, fxsh_ast_ident((sp_str_t){.data = "x", .len = 1}, (fxsh_loc_t){0}));
    func->data.lambda.params = params;

    /* Body: generate based on type */
    fxsh_ast_node_t *body = sp_alloc(sizeof(fxsh_ast_node_t));
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

    fxsh_ast_node_t *func = sp_alloc(sizeof(fxsh_ast_node_t));
    func->kind = AST_LAMBDA;
    func->loc = (fxsh_loc_t){0};

    fxsh_ast_list_t params = SP_NULLPTR;
    sp_dyn_array_push(params, fxsh_ast_ident((sp_str_t){.data = "a", .len = 1}, (fxsh_loc_t){0}));
    sp_dyn_array_push(params, fxsh_ast_ident((sp_str_t){.data = "b", .len = 1}, (fxsh_loc_t){0}));
    func->data.lambda.params = params;

    fxsh_ast_node_t *body = sp_alloc(sizeof(fxsh_ast_node_t));
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
        case CT_LIST:
            return "<list>";
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
    fxsh_ct_value_t *val = sp_alloc(sizeof(fxsh_ct_value_t));
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
    fxsh_ct_field_t *new_fields = sp_alloc(sizeof(fxsh_ct_field_t) * new_count);

    /* Copy old fields */
    for (u32 i = 0; i < record->data.struct_val.num_fields; i++) {
        new_fields[i] = record->data.struct_val.fields[i];
    }

    /* Add new field */
    new_fields[record->data.struct_val.num_fields] = field;

    /* Free old array and update */
    if (record->data.struct_val.fields) {
        sp_free(record->data.struct_val.fields);
    }
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

    /* For now, return the type based on expression kind */
    fxsh_type_t *type = NULL;

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
            /* Look up variable type */
            fxsh_ct_value_t *val = lookup_var(ctx, expr->data.ident);
            if (val && val->kind == CT_TYPE) {
                type = val->data.type_val;
            }
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

    /* Return list of field names for record types */
    /* For now, return empty list for primitive types */
    fxsh_ct_value_t **items = SP_NULLPTR;
    return fxsh_ct_list(items, 0);
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

/*=============================================================================
 * Generic Type Instantiation
 *=============================================================================*/

fxsh_type_t *fxsh_ct_instantiate_generic(fxsh_type_constructor_t *ctor,
                                         sp_dyn_array(fxsh_type_t *) type_args) {
    if (!ctor)
        return NULL;

    /* Create a new concrete type from the constructor */
    fxsh_type_t *instance = sp_alloc(sizeof(fxsh_type_t));
    instance->kind = ctor->kind;

    /* TODO: Substitute type parameters with arguments */

    return instance;
}

/*=============================================================================
 * Vector Type Constructor
 *=============================================================================*/

fxsh_type_constructor_t *fxsh_ct_make_vector_ctor(void) {
    fxsh_type_constructor_t *ctor = sp_alloc(sizeof(fxsh_type_constructor_t));
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
