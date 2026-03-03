#define SP_IMPLEMENTATION
#include "fxsh.h"
#include <stdio.h>

int main() {
    sp_str_t source = sp_str_lit("type option = None | Some of int\n\nNone");
    sp_str_t filename = sp_str_lit("<test>");
    
    fxsh_token_array_t tokens = SP_NULLPTR;
    fxsh_error_t err = fxsh_lex(source, filename, &tokens);
    if (err != ERR_OK) {
        printf("Lex error\n");
        return 1;
    }
    
    fxsh_parser_t parser;
    fxsh_parser_init(&parser, tokens);
    fxsh_ast_node_t *ast = fxsh_parse_program(&parser);
    
    fxsh_type_env_t type_env = SP_NULLPTR;
    fxsh_constr_env_t constr_env = SP_NULLPTR;
    fxsh_type_t *type = NULL;
    
    err = fxsh_type_infer(ast, &type_env, &constr_env, &type);
    
    printf("Type: %s\n", fxsh_type_to_string(type));
    printf("Constr env size: %zu\n", constr_env ? (size_t)sp_ht_size(constr_env) : 0);
    
    /* Look up None constructor */
    fxsh_constr_info_t *info = sp_ht_getp(constr_env, sp_str_lit("None"));
    if (info) {
        printf("None constructor found!\n");
        printf("None type: %s\n", fxsh_type_to_string(info->constr_type));
        printf("None arity: %d\n", info->arity);
    } else {
        printf("None constructor NOT found!\n");
    }
    
    return 0;
}
