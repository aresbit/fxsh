/*
 * main.c - fxsh entry point
 */

#define SP_IMPLEMENTATION
#include "fxsh.h"

#include <stdio.h>

static void print_usage(const char *program) {
    printf("fxsh - Functional Core Minimal Bash\n");
    printf("Version: %d.%d.%d\n\n", FXSH_VERSION_MAJOR, FXSH_VERSION_MINOR, FXSH_VERSION_PATCH);
    printf("Usage:\n");
    printf("  %s [options] <file.fxsh>\n", program);
    printf("  %s [options] -e <expression>\n\n", program);
    printf("Options:\n");
    printf("  -e, --eval <expr>    Evaluate expression\n");
    printf("  -t, --tokens         Print tokens (debug)\n");
    printf("  -a, --ast            Print AST (debug)\n");
    printf("  -h, --help           Show this help\n");
    printf("  -v, --version        Show version\n");
}

static void print_version(void) {
    printf("fxsh version %d.%d.%d\n", FXSH_VERSION_MAJOR, FXSH_VERSION_MINOR, FXSH_VERSION_PATCH);
}

static sp_str_t read_file_to_str(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return (sp_str_t){.data = NULL, .len = 0};
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = sp_alloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    return (sp_str_t){.data = buf, .len = (u32)size};
}

typedef enum {
    MODE_COMPILE,
    MODE_EVAL,
    MODE_TOKENS,
    MODE_AST,
} run_mode_t;

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    run_mode_t mode = MODE_COMPILE;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tokens") == 0) {
            mode = MODE_TOKENS;
            continue;
        }
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--ast") == 0) {
            mode = MODE_AST;
            continue;
        }
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eval") == 0) {
            mode = MODE_EVAL;
            if (i + 1 < argc) {
                input = argv[++i];
            }
            continue;
        }
        if (argv[i][0] != '-') {
            input = argv[i];
        }
    }

    if (!input) {
        fprintf(stderr, "Error: No input file or expression specified\n");
        return 1;
    }

    sp_str_t source;
    sp_str_t filename;

    if (mode == MODE_EVAL) {
        source = sp_str_view(input);
        filename = sp_str_lit("<eval>");
    } else {
        source = read_file_to_str(input);
        if (source.data == NULL) {
            fprintf(stderr, "Error: Cannot read file: %s\n", input);
            return 1;
        }
        filename = sp_str_view(input);
    }

    /* Tokenize */
    fxsh_token_array_t tokens = SP_NULLPTR;
    fxsh_error_t err = fxsh_lex(source, filename, &tokens);
    if (err != ERR_OK) {
        fprintf(stderr, "Lexical error\n");
        return 1;
    }

    if (mode == MODE_TOKENS) {
        printf("Tokens:\n");
        sp_dyn_array_for(tokens, i) {
            fxsh_token_t *tok = &tokens[i];
            printf("  %s", fxsh_token_kind_name(tok->kind));
            switch (tok->kind) {
                case TOK_INT:
                    printf("(%ld)", tok->data.int_val);
                    break;
                case TOK_FLOAT:
                    printf("(%f)", tok->data.float_val);
                    break;
                case TOK_STRING:
                case TOK_IDENT:
                case TOK_TYPE_IDENT:
                    printf("(%.*s)", tok->data.str_val.len, tok->data.str_val.data);
                    break;
                case TOK_TRUE:
                case TOK_FALSE:
                    printf("(%s)", tok->kind == TOK_TRUE ? "true" : "false");
                    break;
                default:
                    break;
            }
            printf(" @ %d:%d\n", tok->loc.line, tok->loc.column);
        }
        fxsh_token_array_free(tokens);
        return 0;
    }

    /* Parse */
    fxsh_parser_t parser;
    fxsh_parser_init(&parser, tokens);
    fxsh_ast_node_t *ast = fxsh_parse_program(&parser);

    if (mode == MODE_AST) {
        printf("AST:\n");
        printf("  (program ...)\n");
        fxsh_ast_free(ast);
        fxsh_token_array_free(tokens);
        return 0;
    }

    /* Type check */
    fxsh_type_env_t type_env = SP_NULLPTR;
    fxsh_type_t *type = NULL;
    err = fxsh_type_infer(ast, &type_env, &type);
    if (err != ERR_OK) {
        fprintf(stderr, "Type error\n");
        fxsh_ast_free(ast);
        fxsh_token_array_free(tokens);
        return 1;
    }

    printf("Type: %s\n", fxsh_type_to_string(type));

    /* Comptime evaluation */
    printf("\nComptime evaluation:\n");
    fxsh_comptime_ctx_t ctx;
    fxsh_comptime_ctx_init(&ctx);
    ctx.type_env = type_env;

    /* Evaluate comptime declarations and expressions */
    if (ast->kind == AST_PROGRAM) {
        sp_dyn_array_for(ast->data.decls, i) {
            fxsh_ast_node_t *decl = ast->data.decls[i];

            /* Check if it's a comptime declaration */
            if (decl->kind == AST_DECL_LET && decl->data.let.is_comptime) {
                fxsh_ct_result_t ct_result = fxsh_ct_eval(decl, &ctx);
                if (ct_result.error == ERR_OK && ct_result.value) {
                    printf("  comptime %.*s = %s\n", decl->data.let.name.len,
                           decl->data.let.name.data, fxsh_ct_value_to_string(ct_result.value));
                }
            }
            /* Also evaluate regular expressions if they are compile-time computable */
            else if (mode == MODE_EVAL) {
                fxsh_ct_result_t ct_result = fxsh_ct_eval(decl, &ctx);
                if (ct_result.error == ERR_OK && ct_result.value) {
                    printf("  => %s\n", fxsh_ct_value_to_string(ct_result.value));
                }
            }
        }
    }

    /* Cleanup */
    fxsh_ast_free(ast);
    fxsh_token_array_free(tokens);

    printf("\nSuccess!\n");
    return 0;
}
