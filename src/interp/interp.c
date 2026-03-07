/*
 * interp.c - fxsh tree-walking interpreter (MVP)
 */

#include "fxsh.h"

#include <stdio.h>
#include <stdlib.h>
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
#define RV_RECORD   FXSH_RT_RECORD
#define RV_TUPLE    FXSH_RT_TUPLE
#define RV_LIST     FXSH_RT_LIST
#define RV_TENSOR   FXSH_RT_TENSOR

#define rv_unit              fxsh_rt_unit
#define rv_bool              fxsh_rt_bool
#define rv_int               fxsh_rt_int
#define rv_float             fxsh_rt_float
#define rv_string            fxsh_rt_string
#define rv_function          fxsh_rt_function
#define rv_constr            fxsh_rt_constr
#define rv_record            fxsh_rt_record
#define rv_record_get        fxsh_rt_record_get
#define rv_tuple             fxsh_rt_tuple
#define rv_tuple_get         fxsh_rt_tuple_get
#define rv_list_nil          fxsh_rt_list_nil
#define rv_list_cons         fxsh_rt_list_cons
#define rv_list_is_nil       fxsh_rt_list_is_nil
#define rv_list_head         fxsh_rt_list_head
#define rv_list_tail         fxsh_rt_list_tail
#define rv_tensor_new2       fxsh_rt_tensor_new2
#define rv_tensor_from_list2 fxsh_rt_tensor_from_list2
#define rv_tensor_shape2     fxsh_rt_tensor_shape2
#define rv_tensor_get2       fxsh_rt_tensor_get2
#define rv_tensor_set2       fxsh_rt_tensor_set2
#define rv_tensor_add        fxsh_rt_tensor_add
#define rv_tensor_dot        fxsh_rt_tensor_dot
#define env_bind             fxsh_rt_env_bind
#define env_lookup           fxsh_rt_env_lookup
#define rv_equal             fxsh_rt_equal
#define rv_to_string         fxsh_rt_to_string

static rv_value_t *eval_expr(fxsh_ast_node_t *ast, rv_env_t *env, fxsh_error_t *err);

static rv_value_t *rv_from_ct(fxsh_ct_value_t *v, fxsh_error_t *err) {
    if (!v)
        return NULL;
    switch (v->kind) {
        case CT_UNIT:
            return rv_unit();
        case CT_BOOL:
            return rv_bool(v->data.bool_val);
        case CT_INT:
            return rv_int(v->data.int_val);
        case CT_FLOAT:
            return rv_float(v->data.float_val);
        case CT_STRING:
            return rv_string(v->data.string_val);
        case CT_TYPE:
            return rv_string(sp_str_view(fxsh_type_to_string(v->data.type_val)));
        case CT_AST:
            return rv_string((sp_str_t){.data = "<ast>", .len = 5});
        case CT_FUNCTION:
            return rv_string((sp_str_t){.data = "<function>", .len = 10});
        case CT_LIST: {
            rv_value_t *lst = rv_list_nil();
            for (s32 i = (s32)v->data.list_val.len - 1; i >= 0; i--) {
                rv_value_t *it = rv_from_ct(v->data.list_val.items[i], err);
                if (!it || *err != ERR_OK)
                    return NULL;
                lst = rv_list_cons(it, lst);
            }
            return lst;
        }
        case CT_STRUCT: {
            sp_dyn_array(sp_str_t) names = SP_NULLPTR;
            sp_dyn_array(rv_value_t *) values = SP_NULLPTR;
            for (u32 i = 0; i < v->data.struct_val.num_fields; i++) {
                sp_dyn_array_push(names, v->data.struct_val.fields[i].name);
                rv_value_t *fv = rv_from_ct(v->data.struct_val.fields[i].value, err);
                if (!fv || *err != ERR_OK)
                    return NULL;
                sp_dyn_array_push(values, fv);
            }
            return rv_record(names, values);
        }
        default:
            break;
    }
    *err = ERR_INVALID_INPUT;
    return NULL;
}

static bool bind_pattern(fxsh_ast_node_t *pat, rv_value_t *val, rv_env_t **env) {
    if (!pat)
        return false;

    switch (pat->kind) {
        case AST_PAT_WILD:
            return true;
        case AST_PAT_VAR:
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
        case AST_PAT_TUPLE: {
            if (!val || val->kind != RV_TUPLE)
                return false;
            if (sp_dyn_array_size(pat->data.elements) != sp_dyn_array_size(val->as.tuple.items))
                return false;
            sp_dyn_array_for(pat->data.elements, i) {
                rv_value_t *it = rv_tuple_get(val, i);
                if (!it || !bind_pattern(pat->data.elements[i], it, env))
                    return false;
            }
            return true;
        }
        case AST_LIST: {
            if (!val || val->kind != RV_LIST)
                return false;
            rv_value_t *cur = val;
            sp_dyn_array_for(pat->data.elements, i) {
                if (!cur || cur->kind != RV_LIST || rv_list_is_nil(cur))
                    return false;
                rv_value_t *h = rv_list_head(cur);
                if (!h || !bind_pattern(pat->data.elements[i], h, env))
                    return false;
                cur = rv_list_tail(cur);
            }
            return cur && cur->kind == RV_LIST && rv_list_is_nil(cur);
        }
        case AST_PAT_CONS: {
            if (!val || val->kind != RV_LIST || rv_list_is_nil(val))
                return false;
            if (sp_dyn_array_size(pat->data.elements) != 2)
                return false;
            rv_value_t *h = rv_list_head(val);
            rv_value_t *t = rv_list_tail(val);
            return bind_pattern(pat->data.elements[0], h, env) &&
                   bind_pattern(pat->data.elements[1], t, env);
        }
        case AST_PAT_RECORD: {
            if (!val || val->kind != RV_RECORD)
                return false;
            sp_dyn_array_for(pat->data.elements, i) {
                fxsh_ast_node_t *f = pat->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    return false;
                rv_value_t *fv = rv_record_get(val, f->data.field.field);
                if (!fv)
                    return false;
                if (!bind_pattern(f->data.field.object, fv, env))
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

static bool try_eval_self_tail_call(rv_value_t *fn, fxsh_ast_node_t *expr, rv_env_t *env,
                                    sp_dyn_array(rv_value_t *) * out_args, fxsh_error_t *err) {
    if (!fn || fn->kind != RV_FUNCTION || !expr)
        return false;
    if (!fn->as.fn.self_name.data || fn->as.fn.self_name.len == 0)
        return false;

    switch (expr->kind) {
        case AST_CALL: {
            fxsh_ast_list_t flat_args = SP_NULLPTR;
            fxsh_ast_node_t *func = expr;
            while (func && func->kind == AST_CALL) {
                sp_dyn_array_for(func->data.call.args, i) {
                    sp_dyn_array_push(flat_args, func->data.call.args[i]);
                }
                func = func->data.call.func;
            }
            if (!func || func->kind != AST_IDENT ||
                !sp_str_equal(func->data.ident, fn->as.fn.self_name))
                return false;

            sp_dyn_array(rv_value_t *) args_in_order = SP_NULLPTR;
            for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
                rv_value_t *v = eval_expr(flat_args[i], env, err);
                if (!v || *err != ERR_OK)
                    return false;
                sp_dyn_array_push(args_in_order, v);
            }
            if (sp_dyn_array_size(args_in_order) != sp_dyn_array_size(fn->as.fn.params)) {
                *err = ERR_INVALID_INPUT;
                fprintf(stderr, "Runtime error: tail-call arity mismatch for `%.*s`\n",
                        fn->as.fn.self_name.len, fn->as.fn.self_name.data);
                return false;
            }
            *out_args = args_in_order;
            return true;
        }
        case AST_IF: {
            rv_value_t *c = eval_expr(expr->data.if_expr.cond, env, err);
            if (!c || *err != ERR_OK)
                return false;
            if (c->kind != RV_BOOL) {
                *err = ERR_INVALID_INPUT;
                fprintf(stderr, "Runtime error: if condition must be bool\n");
                return false;
            }
            fxsh_ast_node_t *branch =
                c->as.b ? expr->data.if_expr.then_branch : expr->data.if_expr.else_branch;
            return try_eval_self_tail_call(fn, branch, env, out_args, err);
        }
        case AST_LET_IN: {
            rv_env_t *new_env = env;
            sp_dyn_array_for(expr->data.let_in.bindings, i) {
                fxsh_ast_node_t *b = expr->data.let_in.bindings[i];
                if (!b || (b->kind != AST_LET && b->kind != AST_DECL_LET))
                    continue;
                if (b->data.let.is_comptime)
                    continue;
                if (b->data.let.is_rec) {
                    rv_value_t *slot = rv_unit();
                    new_env = env_bind(new_env, b->data.let.name, slot);
                    rv_value_t *v = eval_expr(b->data.let.value, new_env, err);
                    if (!v || *err != ERR_OK)
                        return false;
                    if (v->kind == RV_FUNCTION)
                        v->as.fn.self_name = b->data.let.name;
                    *slot = *v;
                } else {
                    rv_value_t *v = eval_expr(b->data.let.value, new_env, err);
                    if (!v || *err != ERR_OK)
                        return false;
                    if (v->kind == RV_FUNCTION)
                        v->as.fn.self_name = b->data.let.name;
                    new_env = env_bind(new_env, b->data.let.name, v);
                }
            }
            return try_eval_self_tail_call(fn, expr->data.let_in.body, new_env, out_args, err);
        }
        case AST_MATCH: {
            rv_value_t *mv = eval_expr(expr->data.match_expr.expr, env, err);
            if (!mv || *err != ERR_OK)
                return false;
            sp_dyn_array_for(expr->data.match_expr.arms, i) {
                fxsh_ast_node_t *arm = expr->data.match_expr.arms[i];
                if (!arm || arm->kind != AST_MATCH_ARM)
                    continue;

                rv_env_t *arm_env = env;
                if (!bind_pattern(arm->data.match_arm.pattern, mv, &arm_env))
                    continue;

                if (arm->data.match_arm.guard) {
                    rv_value_t *g = eval_expr(arm->data.match_arm.guard, arm_env, err);
                    if (!g || *err != ERR_OK)
                        return false;
                    if (g->kind != RV_BOOL || !g->as.b)
                        continue;
                }
                return try_eval_self_tail_call(fn, arm->data.match_arm.body, arm_env, out_args,
                                               err);
            }
            return false;
        }
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

    while (true) {
        rv_env_t *call_env = fn->as.fn.env;
        sp_dyn_array_for(fn->as.fn.params, i) {
            fxsh_ast_node_t *p = fn->as.fn.params[i];
            if (!p)
                continue;
            if (p->kind == AST_PAT_VAR) {
                call_env = env_bind(call_env, p->data.ident, bound[i]);
            }
        }

        sp_dyn_array(rv_value_t *) next_args = SP_NULLPTR;
        if (try_eval_self_tail_call(fn, fn->as.fn.body, call_env, &next_args, err)) {
            if (*err != ERR_OK)
                return NULL;
            bound = next_args;
            continue;
        }

        return eval_expr(fn->as.fn.body, call_env, err);
    }
}

static bool builtin_name_eq(sp_str_t n, const char *lit) {
    return sp_str_equal(n, sp_str_view((char *)lit));
}

static sp_str_t grep_lines_text(sp_str_t pattern, sp_str_t text) {
    return fxsh_grep_lines_regex(pattern, text);
}

static rv_value_t *eval_builtin_call(sp_str_t name, fxsh_ast_list_t args, rv_env_t *env,
                                     fxsh_error_t *err) {
    sp_dyn_array(rv_value_t *) av = SP_NULLPTR;
    sp_dyn_array_for(args, i) {
        rv_value_t *v = eval_expr(args[i], env, err);
        if (!v || *err != ERR_OK)
            return NULL;
        sp_dyn_array_push(av, v);
    }

    if (builtin_name_eq(name, "print")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: print expects string\n");
            return NULL;
        }
        printf("%.*s\n", av[0]->as.s.len, av[0]->as.s.data);
        return rv_unit();
    }
    if (builtin_name_eq(name, "getenv")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: getenv expects string\n");
            return NULL;
        }
        return rv_string(fxsh_getenv_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "file_exists")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: file_exists expects string\n");
            return NULL;
        }
        return rv_bool(fxsh_file_exists_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "read_file")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: read_file expects string\n");
            return NULL;
        }
        sp_str_t s = fxsh_read_file(av[0]->as.s);
        return rv_string(s.data ? s : (sp_str_t){.data = "", .len = 0});
    }
    if (builtin_name_eq(name, "write_file")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: write_file expects (string, string)\n");
            return NULL;
        }
        fxsh_error_t e = fxsh_write_file(av[0]->as.s, av[1]->as.s);
        return rv_bool(e == ERR_OK);
    }
    if (builtin_name_eq(name, "exec")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec expects string\n");
            return NULL;
        }
        return rv_int(fxsh_exec_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "exec_stdout")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_stdout expects string\n");
            return NULL;
        }
        return rv_string(fxsh_exec_stdout_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "exec_stderr")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_stderr expects string\n");
            return NULL;
        }
        return rv_string(fxsh_exec_stderr_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "exec_capture")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_capture expects string\n");
            return NULL;
        }
        return rv_int(fxsh_exec_capture_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "capture_code")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: capture_code expects int\n");
            return NULL;
        }
        return rv_int(fxsh_capture_code_rt(av[0]->as.i));
    }
    if (builtin_name_eq(name, "capture_stdout")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: capture_stdout expects int\n");
            return NULL;
        }
        return rv_string(fxsh_capture_stdout_rt(av[0]->as.i));
    }
    if (builtin_name_eq(name, "capture_stderr")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: capture_stderr expects int\n");
            return NULL;
        }
        return rv_string(fxsh_capture_stderr_rt(av[0]->as.i));
    }
    if (builtin_name_eq(name, "capture_release")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: capture_release expects int\n");
            return NULL;
        }
        return rv_bool(fxsh_capture_release_rt(av[0]->as.i));
    }
    if (builtin_name_eq(name, "exec_stdin")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_stdin expects (string, string)\n");
            return NULL;
        }
        return rv_string(fxsh_exec_stdin_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_stdin_code")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_stdin_code expects (string, string)\n");
            return NULL;
        }
        return rv_int(fxsh_exec_stdin_code_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_stdin_capture")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_stdin_capture expects (string, string)\n");
            return NULL;
        }
        return rv_int(fxsh_exec_stdin_capture_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_stdin_stderr")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_stdin_stderr expects (string, string)\n");
            return NULL;
        }
        return rv_string(fxsh_exec_stdin_stderr_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_pipe")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_pipe expects (string, string)\n");
            return NULL;
        }
        return rv_string(fxsh_exec_pipe_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_pipe_code")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_pipe_code expects (string, string)\n");
            return NULL;
        }
        return rv_int(fxsh_exec_pipe_code_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_pipe_capture")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_pipe_capture expects (string, string)\n");
            return NULL;
        }
        return rv_int(fxsh_exec_pipe_capture_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_pipefail_capture")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_pipefail_capture expects (string, string)\n");
            return NULL;
        }
        return rv_int(fxsh_exec_pipefail_capture_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_pipefail3_capture")) {
        if (sp_dyn_array_size(av) != 3 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING ||
            av[2]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_pipefail3_capture expects (string, string, "
                            "string)\n");
            return NULL;
        }
        return rv_int(fxsh_exec_pipefail3_capture_rt(av[0]->as.s, av[1]->as.s, av[2]->as.s));
    }
    if (builtin_name_eq(name, "exec_pipefail4_capture")) {
        if (sp_dyn_array_size(av) != 4 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING ||
            av[2]->kind != RV_STRING || av[3]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_pipefail4_capture expects (string, string, "
                            "string, string)\n");
            return NULL;
        }
        return rv_int(
            fxsh_exec_pipefail4_capture_rt(av[0]->as.s, av[1]->as.s, av[2]->as.s, av[3]->as.s));
    }
    if (builtin_name_eq(name, "exec_pipe_stderr")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_pipe_stderr expects (string, string)\n");
            return NULL;
        }
        return rv_string(fxsh_exec_pipe_stderr_rt(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "glob")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: glob expects string\n");
            return NULL;
        }
        return rv_string(fxsh_glob_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "grep_lines")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: grep_lines expects (string, string)\n");
            return NULL;
        }
        return rv_string(grep_lines_text(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "exec_code")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: exec_code expects string\n");
            return NULL;
        }
        return rv_int(fxsh_exec_code_rt(av[0]->as.s));
    }
    if (builtin_name_eq(name, "replace_once")) {
        if (sp_dyn_array_size(av) != 3 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING ||
            av[2]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: replace_once expects (string, string, string)\n");
            return NULL;
        }
        return rv_string(fxsh_replace_once(av[0]->as.s, av[1]->as.s, av[2]->as.s));
    }
    if (builtin_name_eq(name, "json_validate")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_validate expects string\n");
            return NULL;
        }
        return rv_bool(fxsh_json_validate(av[0]->as.s));
    }
    if (builtin_name_eq(name, "json_compact")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_compact expects string\n");
            return NULL;
        }
        return rv_string(fxsh_json_compact(av[0]->as.s));
    }
    if (builtin_name_eq(name, "json_has")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_has expects (string, string)\n");
            return NULL;
        }
        return rv_bool(fxsh_json_has(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "json_get")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_get expects (string, string)\n");
            return NULL;
        }
        return rv_string(fxsh_json_get(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "json_get_string")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_get_string expects (string, string)\n");
            return NULL;
        }
        return rv_string(fxsh_json_get_string(av[0]->as.s, av[1]->as.s));
    }
    if (builtin_name_eq(name, "json_get_int")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_get_int expects (string, string)\n");
            return NULL;
        }
        bool ok = false;
        s64 v = fxsh_json_get_int(av[0]->as.s, av[1]->as.s, &ok);
        return rv_int(ok ? v : 0);
    }
    if (builtin_name_eq(name, "json_get_float")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_get_float expects (string, string)\n");
            return NULL;
        }
        bool ok = false;
        f64 v = fxsh_json_get_float(av[0]->as.s, av[1]->as.s, &ok);
        return rv_float(ok ? v : 0.0);
    }
    if (builtin_name_eq(name, "json_get_bool")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_STRING || av[1]->kind != RV_STRING) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: json_get_bool expects (string, string)\n");
            return NULL;
        }
        bool ok = false;
        bool v = fxsh_json_get_bool(av[0]->as.s, av[1]->as.s, &ok);
        return rv_bool(ok && v);
    }
    if (builtin_name_eq(name, "c_null")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_UNIT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: c_null expects unit\n");
            return NULL;
        }
        /* Interpreter fallback has no real raw pointers; use 0 sentinel. */
        return rv_int(0);
    }
    if (builtin_name_eq(name, "c_malloc")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: c_malloc expects int\n");
            return NULL;
        }
        /* Interpreter fallback models pointer as opaque int sentinel. */
        return rv_int(av[0]->as.i > 0 ? av[0]->as.i : 1);
    }
    if (builtin_name_eq(name, "c_free")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: c_free expects ptr-sentinel int\n");
            return NULL;
        }
        return rv_unit();
    }
    if (builtin_name_eq(name, "c_cast_ptr")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: c_cast_ptr expects ptr-sentinel int\n");
            return NULL;
        }
        return av[0];
    }
    if (builtin_name_eq(name, "c_callback0")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_FUNCTION) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: c_callback0 expects function (unit -> unit)\n");
            return NULL;
        }
        /* Interpreter fallback: callback pointer as opaque non-zero sentinel. */
        return rv_int(1);
    }
    if (builtin_name_eq(name, "c_callback1_ptr")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_FUNCTION) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: c_callback1_ptr expects function (ptr -> unit)\n");
            return NULL;
        }
        return rv_int(1);
    }
    if (builtin_name_eq(name, "int_to_c_int") || builtin_name_eq(name, "c_int_to_int") ||
        builtin_name_eq(name, "int_to_c_uint") || builtin_name_eq(name, "c_uint_to_int") ||
        builtin_name_eq(name, "int_to_c_long") || builtin_name_eq(name, "c_long_to_int") ||
        builtin_name_eq(name, "int_to_c_ulong") || builtin_name_eq(name, "c_ulong_to_int") ||
        builtin_name_eq(name, "int_to_c_size") || builtin_name_eq(name, "c_size_to_int") ||
        builtin_name_eq(name, "int_to_c_ssize") || builtin_name_eq(name, "c_ssize_to_int")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: integer ABI cast expects int\n");
            return NULL;
        }
        return av[0];
    }
    if (builtin_name_eq(name, "tensor_new2")) {
        if (sp_dyn_array_size(av) != 3 || av[0]->kind != RV_INT || av[1]->kind != RV_INT ||
            (av[2]->kind != RV_FLOAT && av[2]->kind != RV_INT)) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_new2 expects (int, int, float)\n");
            return NULL;
        }
        f64 fill = av[2]->kind == RV_FLOAT ? av[2]->as.f : (f64)av[2]->as.i;
        rv_value_t *t = rv_tensor_new2(av[0]->as.i, av[1]->as.i, fill);
        if (!t) {
            *err = ERR_INVALID_INPUT;
            return NULL;
        }
        return t;
    }
    if (builtin_name_eq(name, "tensor_from_list2")) {
        if (sp_dyn_array_size(av) != 3 || av[0]->kind != RV_INT || av[1]->kind != RV_INT ||
            av[2]->kind != RV_LIST) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_from_list2 expects (int, int, float list)\n");
            return NULL;
        }
        rv_value_t *t = rv_tensor_from_list2(av[0]->as.i, av[1]->as.i, av[2]);
        if (!t) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_from_list2 shape/data mismatch\n");
            return NULL;
        }
        return t;
    }
    if (builtin_name_eq(name, "tensor_shape2")) {
        if (sp_dyn_array_size(av) != 1 || av[0]->kind != RV_TENSOR) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_shape2 expects tensor\n");
            return NULL;
        }
        return rv_tensor_shape2(av[0]);
    }
    if (builtin_name_eq(name, "tensor_get2")) {
        if (sp_dyn_array_size(av) != 3 || av[0]->kind != RV_TENSOR || av[1]->kind != RV_INT ||
            av[2]->kind != RV_INT) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_get2 expects (tensor, int, int)\n");
            return NULL;
        }
        rv_value_t *x = rv_tensor_get2(av[0], av[1]->as.i, av[2]->as.i);
        if (!x) {
            *err = ERR_INVALID_INPUT;
            return NULL;
        }
        return x;
    }
    if (builtin_name_eq(name, "tensor_set2")) {
        if (sp_dyn_array_size(av) != 4 || av[0]->kind != RV_TENSOR || av[1]->kind != RV_INT ||
            av[2]->kind != RV_INT || (av[3]->kind != RV_FLOAT && av[3]->kind != RV_INT)) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_set2 expects (tensor, int, int, float)\n");
            return NULL;
        }
        f64 val = av[3]->kind == RV_FLOAT ? av[3]->as.f : (f64)av[3]->as.i;
        rv_value_t *x = rv_tensor_set2(av[0], av[1]->as.i, av[2]->as.i, val);
        if (!x) {
            *err = ERR_INVALID_INPUT;
            return NULL;
        }
        return x;
    }
    if (builtin_name_eq(name, "tensor_add")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_TENSOR || av[1]->kind != RV_TENSOR) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_add expects (tensor, tensor)\n");
            return NULL;
        }
        rv_value_t *x = rv_tensor_add(av[0], av[1]);
        if (!x) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_add shape mismatch\n");
            return NULL;
        }
        return x;
    }
    if (builtin_name_eq(name, "tensor_dot")) {
        if (sp_dyn_array_size(av) != 2 || av[0]->kind != RV_TENSOR || av[1]->kind != RV_TENSOR) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_dot expects (tensor, tensor)\n");
            return NULL;
        }
        rv_value_t *x = rv_tensor_dot(av[0], av[1]);
        if (!x) {
            *err = ERR_INVALID_INPUT;
            fprintf(stderr, "Runtime error: tensor_dot shape mismatch\n");
            return NULL;
        }
        return x;
    }
    return NULL;
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
        case TOK_APPEND:
            if (r->kind == RV_LIST)
                return rv_list_cons(l, r);
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
            fxsh_ast_list_t flat_args = SP_NULLPTR;
            fxsh_ast_node_t *func = ast;
            while (func && func->kind == AST_CALL) {
                sp_dyn_array_for(func->data.call.args, i) {
                    sp_dyn_array_push(flat_args, func->data.call.args[i]);
                }
                func = func->data.call.func;
            }
            if (func && func->kind == AST_IDENT) {
                bool force_builtin = builtin_name_eq(func->data.ident, "exec_pipefail_capture") ||
                                     builtin_name_eq(func->data.ident, "exec_pipefail3_capture") ||
                                     builtin_name_eq(func->data.ident, "exec_pipefail4_capture") ||
                                     builtin_name_eq(func->data.ident, "capture_release");
                if (force_builtin || !env_lookup(env, func->data.ident)) {
                    sp_dyn_array(fxsh_ast_node_t *) args_in_order = SP_NULLPTR;
                    for (s32 i = (s32)sp_dyn_array_size(flat_args) - 1; i >= 0; i--) {
                        sp_dyn_array_push(args_in_order, flat_args[i]);
                    }
                    rv_value_t *builtin_res =
                        eval_builtin_call(func->data.ident, args_in_order, env, err);
                    if (builtin_res || *err != ERR_OK)
                        return builtin_res;
                }
            }

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
        case AST_LIST: {
            rv_value_t *list = rv_list_nil();
            for (s32 i = (s32)sp_dyn_array_size(ast->data.elements) - 1; i >= 0; i--) {
                rv_value_t *it = eval_expr(ast->data.elements[i], env, err);
                if (!it || *err != ERR_OK)
                    return NULL;
                list = rv_list_cons(it, list);
            }
            return list;
        }
        case AST_TUPLE: {
            sp_dyn_array(rv_value_t *) items = SP_NULLPTR;
            sp_dyn_array_for(ast->data.elements, i) {
                rv_value_t *it = eval_expr(ast->data.elements[i], env, err);
                if (!it || *err != ERR_OK)
                    return NULL;
                sp_dyn_array_push(items, it);
            }
            return rv_tuple(items);
        }
        case AST_RECORD: {
            sp_dyn_array(sp_str_t) names = SP_NULLPTR;
            sp_dyn_array(rv_value_t *) values = SP_NULLPTR;
            sp_dyn_array_for(ast->data.elements, i) {
                fxsh_ast_node_t *f = ast->data.elements[i];
                if (!f || f->kind != AST_FIELD_ACCESS || !f->data.field.object)
                    continue;
                rv_value_t *v = eval_expr(f->data.field.object, env, err);
                if (!v || *err != ERR_OK)
                    return NULL;
                sp_dyn_array_push(names, f->data.field.field);
                sp_dyn_array_push(values, v);
            }
            return rv_record(names, values);
        }
        case AST_FIELD_ACCESS: {
            rv_value_t *obj = eval_expr(ast->data.field.object, env, err);
            if (!obj || *err != ERR_OK)
                return NULL;
            rv_value_t *v = rv_record_get(obj, ast->data.field.field);
            if (!v) {
                *err = ERR_INVALID_INPUT;
                if (obj->kind != RV_RECORD) {
                    fprintf(stderr, "Runtime error: field access on non-record value\n");
                } else {
                    fprintf(stderr, "Runtime error: missing record field `%.*s`\n",
                            ast->data.field.field.len, ast->data.field.field.data);
                }
                return NULL;
            }
            return v;
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
            if (ast->data.let.is_comptime)
                return rv_unit();
            rv_value_t *v = eval_expr(ast->data.let.value, env, err);
            if (!v || *err != ERR_OK)
                return NULL;
            if (v->kind == RV_FUNCTION)
                v->as.fn.self_name = ast->data.let.name;
            env = env_bind(env, ast->data.let.name, v);
            (void)env;
            return rv_unit();
        }
        case AST_DECL_LET: {
            if (ast->data.let.is_comptime)
                return rv_unit();
            if (ast->data.let.is_rec) {
                rv_value_t *slot = rv_unit();
                rv_env_t *rec_env = env_bind(env, ast->data.let.name, slot);
                rv_value_t *v = eval_expr(ast->data.let.value, rec_env, err);
                if (!v || *err != ERR_OK)
                    return NULL;
                if (v->kind == RV_FUNCTION)
                    v->as.fn.self_name = ast->data.let.name;
                *slot = *v;
                return rv_unit();
            }
            rv_value_t *v = eval_expr(ast->data.let.value, env, err);
            if (!v || *err != ERR_OK)
                return NULL;
            if (v->kind == RV_FUNCTION)
                v->as.fn.self_name = ast->data.let.name;
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
                if (b->data.let.is_comptime)
                    continue;

                if (b->data.let.is_rec) {
                    rv_value_t *slot = rv_unit();
                    new_env = env_bind(new_env, b->data.let.name, slot);
                    rv_value_t *v = eval_expr(b->data.let.value, new_env, err);
                    if (!v || *err != ERR_OK)
                        return NULL;
                    if (v->kind == RV_FUNCTION)
                        v->as.fn.self_name = b->data.let.name;
                    *slot = *v;
                } else {
                    rv_value_t *v = eval_expr(b->data.let.value, new_env, err);
                    if (!v || *err != ERR_OK)
                        return NULL;
                    if (v->kind == RV_FUNCTION)
                        v->as.fn.self_name = b->data.let.name;
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
                    if (d->data.let.is_comptime) {
                        last = rv_unit();
                        continue;
                    }
                    if (d->data.let.is_rec) {
                        rv_value_t *slot = rv_unit();
                        prog_env = env_bind(prog_env, d->data.let.name, slot);
                        rv_value_t *v = eval_expr(d->data.let.value, prog_env, err);
                        if (!v || *err != ERR_OK)
                            return NULL;
                        if (v->kind == RV_FUNCTION)
                            v->as.fn.self_name = d->data.let.name;
                        *slot = *v;
                        last = rv_unit();
                    } else {
                        rv_value_t *v = eval_expr(d->data.let.value, prog_env, err);
                        if (!v || *err != ERR_OK)
                            return NULL;
                        if (v->kind == RV_FUNCTION)
                            v->as.fn.self_name = d->data.let.name;
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
        case AST_CT_TYPE_OF:
        case AST_CT_SIZE_OF:
        case AST_CT_ALIGN_OF:
        case AST_CT_FIELDS_OF:
        case AST_CT_HAS_FIELD:
        case AST_CT_JSON_SCHEMA:
        case AST_CT_QUOTE:
        case AST_CT_UNQUOTE:
        case AST_CT_SPLICE:
        case AST_CT_EVAL:
        case AST_CT_COMPILE_ERROR:
        case AST_CT_COMPILE_LOG:
        case AST_CT_PANIC: {
            fxsh_comptime_ctx_t cctx;
            fxsh_comptime_ctx_init(&cctx);
            fxsh_ct_value_t *cv = fxsh_ct_eval_expr(ast, &cctx);
            if (!cv) {
                *err = ERR_INVALID_INPUT;
                const c8 *ct_err = fxsh_ct_last_error();
                fprintf(stderr, "Runtime error: comptime expression failed: %s\n",
                        ct_err ? ct_err : "unknown");
                return NULL;
            }
            return rv_from_ct(cv, err);
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
