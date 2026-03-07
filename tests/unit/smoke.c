#define SP_IMPLEMENTATION
#include "fxsh.h"

#include <stdio.h>
#include <stdlib.h>

static void require_true(bool cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "test failure: %s\n", msg);
        exit(1);
    }
}

static void test_ct_instantiate_generic_vector_ctor(void) {
    fxsh_reset_type_vars();

    fxsh_type_constructor_t *ctor = fxsh_ct_make_vector_ctor();
    sp_dyn_array(fxsh_type_t *) args = SP_NULLPTR;
    sp_dyn_array_push(args, fxsh_type_con(TYPE_STRING));

    fxsh_type_t *instance = fxsh_ct_instantiate_generic(ctor, args);
    require_true(instance != NULL, "vector ctor instantiation returned NULL");
    require_true(instance->kind == TYPE_RECORD, "vector ctor did not instantiate to record");
    require_true(sp_dyn_array_size(instance->data.record.fields) == 3,
                 "vector ctor field count mismatch");
    require_true(
        sp_str_equal(instance->data.record.fields[0].name, (sp_str_t){.data = "data", .len = 4}),
        "vector ctor first field name mismatch");
    require_true(instance->data.record.fields[0].type->kind == TYPE_CON,
                 "vector ctor data field kind mismatch");
    require_true(sp_str_equal(instance->data.record.fields[0].type->data.con, TYPE_STRING),
                 "vector ctor data field type mismatch");
    require_true(sp_str_equal(instance->data.record.fields[1].type->data.con, TYPE_INT),
                 "vector ctor len field type mismatch");
    require_true(sp_str_equal(instance->data.record.fields[2].type->data.con, TYPE_INT),
                 "vector ctor cap field type mismatch");

    sp_dyn_array_free(args);
}

static void test_ct_instantiate_generic_nested_types(void) {
    fxsh_reset_type_vars();

    fxsh_type_var_t a = fxsh_fresh_var();
    fxsh_type_var_t b = fxsh_fresh_var();

    fxsh_type_constructor_t ctor = {
        .name = (sp_str_t){.data = "FnBox", .len = 5},
        .kind = TYPE_ARROW,
        .type_params = SP_NULLPTR,
        .fields = SP_NULLPTR,
        .target_type =
            fxsh_type_arrow(fxsh_type_apply(fxsh_type_con(TYPE_LIST), fxsh_type_var(a)),
                            fxsh_type_apply(fxsh_type_con(TYPE_OPTION), fxsh_type_var(b))),
    };
    sp_dyn_array_push(ctor.type_params, a);
    sp_dyn_array_push(ctor.type_params, b);

    sp_dyn_array(fxsh_type_t *) args = SP_NULLPTR;
    sp_dyn_array_push(args, fxsh_type_con(TYPE_INT));
    sp_dyn_array_push(args, fxsh_type_con(TYPE_STRING));

    fxsh_type_t *instance = fxsh_ct_instantiate_generic(&ctor, args);
    require_true(instance != NULL, "nested generic instantiation returned NULL");
    require_true(instance->kind == TYPE_ARROW, "nested generic instantiation kind mismatch");
    require_true(instance->data.arrow.param->kind == TYPE_APP,
                 "nested generic param kind mismatch");
    require_true(sp_str_equal(instance->data.arrow.param->data.app.con->data.con, TYPE_LIST),
                 "nested generic param constructor mismatch");
    require_true(sp_str_equal(instance->data.arrow.param->data.app.arg->data.con, TYPE_INT),
                 "nested generic param argument mismatch");
    require_true(instance->data.arrow.ret->kind == TYPE_APP, "nested generic return kind mismatch");
    require_true(sp_str_equal(instance->data.arrow.ret->data.app.con->data.con, TYPE_OPTION),
                 "nested generic return constructor mismatch");
    require_true(sp_str_equal(instance->data.arrow.ret->data.app.arg->data.con, TYPE_STRING),
                 "nested generic return argument mismatch");

    sp_dyn_array_free(args);
    sp_dyn_array_free(ctor.type_params);
}

static void test_ct_instantiate_generic_arity_mismatch(void) {
    fxsh_reset_type_vars();

    fxsh_type_constructor_t *ctor = fxsh_ct_make_vector_ctor();
    sp_dyn_array(fxsh_type_t *) args = SP_NULLPTR;

    require_true(fxsh_ct_instantiate_generic(ctor, args) == NULL,
                 "arity mismatch should reject zero args");
    sp_dyn_array_push(args, fxsh_type_con(TYPE_INT));
    sp_dyn_array_push(args, fxsh_type_con(TYPE_STRING));
    require_true(fxsh_ct_instantiate_generic(ctor, args) == NULL,
                 "arity mismatch should reject extra args");

    sp_dyn_array_free(args);
}

static void test_ct_instantiate_generic_binary_postfix_app(void) {
    fxsh_reset_type_vars();

    fxsh_type_var_t ok = fxsh_fresh_var();
    fxsh_type_var_t err = fxsh_fresh_var();
    fxsh_type_constructor_t ctor = {
        .name = (sp_str_t){.data = "Result", .len = 6},
        .kind = TYPE_APP,
        .type_params = SP_NULLPTR,
        .fields = SP_NULLPTR,
        .target_type = fxsh_type_apply(fxsh_type_con(TYPE_RESULT),
                                       fxsh_type_apply(fxsh_type_var(err), fxsh_type_var(ok))),
    };
    sp_dyn_array_push(ctor.type_params, ok);
    sp_dyn_array_push(ctor.type_params, err);

    sp_dyn_array(fxsh_type_t *) args = SP_NULLPTR;
    sp_dyn_array_push(args, fxsh_type_con(TYPE_INT));
    sp_dyn_array_push(args, fxsh_type_con(TYPE_STRING));

    fxsh_type_t *instance = fxsh_ct_instantiate_generic(&ctor, args);
    require_true(instance != NULL, "binary postfix ctor instantiation returned NULL");
    require_true(instance->kind == TYPE_APP, "binary postfix ctor outer kind mismatch");
    require_true(instance->data.app.con->kind == TYPE_CON,
                 "binary postfix ctor head kind mismatch");
    require_true(sp_str_equal(instance->data.app.con->data.con, TYPE_RESULT),
                 "binary postfix ctor head name mismatch");
    require_true(instance->data.app.arg->kind == TYPE_APP,
                 "binary postfix ctor arg shape mismatch");
    require_true(sp_str_equal(instance->data.app.arg->data.app.con->data.con, TYPE_STRING),
                 "binary postfix ctor second arg mismatch");
    require_true(sp_str_equal(instance->data.app.arg->data.app.arg->data.con, TYPE_INT),
                 "binary postfix ctor first arg mismatch");

    sp_dyn_array_free(args);
    sp_dyn_array_free(ctor.type_params);
}

int main(void) {
    test_ct_instantiate_generic_vector_ctor();
    test_ct_instantiate_generic_nested_types();
    test_ct_instantiate_generic_arity_mismatch();
    test_ct_instantiate_generic_binary_postfix_app();
    return 0;
}
