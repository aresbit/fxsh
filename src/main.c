/*
 * main.c - fxsh entry point
 */

#define SP_IMPLEMENTATION
#include "fxsh.h"

#include <stdio.h>
#include <stdlib.h>

static void print_usage(const char *program) {
    printf("fxsh - Functional Core Minimal Bash\n");
    printf("Version: %d.%d.%d\n\n", FXSH_VERSION_MAJOR, FXSH_VERSION_MINOR, FXSH_VERSION_PATCH);
    printf("Usage:\n");
    printf("  %s [options] <file.fxsh>\n", program);
    printf("  %s [options] -e <expression>\n\n", program);
    printf("Options:\n");
    printf("  -e, --eval <expr>    Evaluate expression\n");
    printf("  -c, --codegen        Generate C code\n");
    printf("  -n, --native         Run closure-safe native runtime binary\n");
    printf("      --native-codegen Generate C, compile and run native codegen binary\n");
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
    MODE_CODEGEN,
    MODE_NATIVE,
    MODE_NATIVE_CODEGEN,
} run_mode_t;

static fxsh_error_t run_native_interp_harness(sp_str_t source) {
    FILE *f = fopen("build/fxsh_native_tmp.c", "wb");
    if (!f)
        return ERR_INTERNAL;

    fprintf(f, "/* Generated fallback native runner (closure-safe) */\n");
    fprintf(f, "#define SP_IMPLEMENTATION\n");
    fprintf(f, "#include \"fxsh.h\"\n");
    fprintf(f, "#include <stdio.h>\n\n");
    fprintf(f, "static const unsigned char fxsh_src[] = {");
    for (u32 i = 0; i < source.len; i++) {
        fprintf(f, "%u,", (unsigned)(u8)source.data[i]);
    }
    fprintf(f, "0};\n\n");
    fprintf(f, "int main(void) {\n");
    fprintf(f, "  fxsh_arena_t *arena = arena_create(NULL, 64 * 1024);\n");
    fprintf(f, "  fxsh_current_arena = arena;\n");
    fprintf(f, "  sp_str_t src = { .data = (const char *)fxsh_src, .len = %u };\n", source.len);
    fprintf(f, "  sp_str_t filename = sp_str_lit(\"<native>\");\n");
    fprintf(f, "  fxsh_token_array_t tokens = SP_NULLPTR;\n");
    fprintf(f, "  if (fxsh_lex(src, filename, &tokens) != ERR_OK) return 1;\n");
    fprintf(f, "  fxsh_parser_t parser;\n");
    fprintf(f, "  fxsh_parser_init(&parser, tokens);\n");
    fprintf(f, "  fxsh_ast_node_t *ast = fxsh_parse_program(&parser);\n");
    fprintf(f, "  fxsh_type_env_t tenv = SP_NULLPTR;\n");
    fprintf(f, "  fxsh_constr_env_t cenv = SP_NULLPTR;\n");
    fprintf(f, "  fxsh_type_t *type = NULL;\n");
    fprintf(f, "  if (fxsh_type_infer(ast, &tenv, &cenv, &type) != ERR_OK) return 1;\n");
    fprintf(f, "  sp_str_t out = {0};\n");
    fprintf(f, "  if (fxsh_interp_eval(ast, &out) != ERR_OK) return 1;\n");
    fprintf(f, "  return 0;\n");
    fprintf(f, "}\n");
    fclose(f);

    int cc = system("clang -std=gnu17 -D _GNU_SOURCE -Wall -Wextra -Wno-strict-prototypes "
                    "-pedantic -Iinclude -Ilib -DSP_PS_DISABLE -Oz -DNDEBUG "
                    "build/fxsh_native_tmp.c src/utils.c src/lexer/lexer.c src/parser/parser.c "
                    "src/types/types.c src/comptime/comptime.c src/interp/interp.c "
                    "src/runtime/runtime.c -lm -o bin/fxsh_native_tmp");
    if (cc != 0) {
        fprintf(stderr, "Native fallback compile failed\n");
        return ERR_INTERNAL;
    }

    int rc = system("./bin/fxsh_native_tmp");
    if (rc != 0) {
        fprintf(stderr, "Native fallback run failed (exit=%d)\n", rc);
        return ERR_INTERNAL;
    }
    return ERR_OK;
}

static fxsh_error_t run_native_codegen(fxsh_ast_node_t *ast) {
    char *code = fxsh_codegen(ast);
    if (!code)
        return ERR_OUT_OF_MEMORY;

    sp_str_t c_path = sp_str_lit("build/fxsh_native_tmp.c");
    sp_str_t bin_path = sp_str_lit("bin/fxsh_native_tmp");
    fxsh_error_t err = fxsh_write_file(c_path, (sp_str_t){.data = code, .len = (u32)strlen(code)});
    sp_free(code);
    if (err != ERR_OK)
        return err;

    int cc = system("clang -std=gnu17 -O2 -Wall -Wextra build/fxsh_native_tmp.c -o "
                    "bin/fxsh_native_tmp");
    if (cc != 0) {
        fprintf(stderr, "Native codegen compile failed\n");
        return ERR_INTERNAL;
    }

    int rc = system("./bin/fxsh_native_tmp");
    if (rc != 0) {
        fprintf(stderr, "Native run failed (exit=%d)\n", rc);
        return ERR_INTERNAL;
    }
    (void)bin_path;
    return ERR_OK;
}

int main(int argc, char **argv) {
    fxsh_arena_t *arena = arena_create(NULL, 64 * 1024);
    fxsh_current_arena = arena;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    run_mode_t mode = MODE_COMPILE;
    const char *input = NULL;
    bool input_is_eval = false;

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
            if (mode == MODE_COMPILE) {
                mode = MODE_EVAL;
            }
            if (i + 1 < argc) {
                input = argv[++i];
                input_is_eval = true;
            }
            continue;
        }
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--codegen") == 0) {
            mode = MODE_CODEGEN;
            continue;
        }
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--native") == 0) {
            mode = MODE_NATIVE;
            continue;
        }
        if (strcmp(argv[i], "--native-codegen") == 0) {
            mode = MODE_NATIVE_CODEGEN;
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

    if (input_is_eval) {
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
    fxsh_constr_env_t constr_env = SP_NULLPTR;
    fxsh_type_t *type = NULL;
    err = fxsh_type_infer(ast, &type_env, &constr_env, &type);
    if (err != ERR_OK) {
        fprintf(stderr, "Type error\n");
        fxsh_ast_free(ast);
        fxsh_token_array_free(tokens);
        return 1;
    }

    printf("Type: %s\n", fxsh_type_to_string(type));

    if (mode == MODE_NATIVE || mode == MODE_NATIVE_CODEGEN) {
        printf("\nNative:\n");
        if (mode == MODE_NATIVE) {
            err = run_native_interp_harness(source);
        } else {
            err = run_native_codegen(ast);
        }
        if (err != ERR_OK) {
            fxsh_ast_free(ast);
            fxsh_token_array_free(tokens);
            return 1;
        }
        printf("  => native run success\n");
    }

    /* Interpreter execution (MVP runtime path) */
    if (mode != MODE_CODEGEN && mode != MODE_NATIVE && mode != MODE_NATIVE_CODEGEN) {
        sp_str_t interp_result = {0};
        err = fxsh_interp_eval(ast, &interp_result);
        if (err != ERR_OK) {
            fprintf(stderr, "Runtime error\n");
            fxsh_ast_free(ast);
            fxsh_token_array_free(tokens);
            return 1;
        }
        printf("\nInterpreter:\n");
        printf("  => %.*s\n", interp_result.len, interp_result.data);
    }

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
        }
    }

    /* Code generation */
    if (mode == MODE_CODEGEN) {
        printf("\n/* Generated C code */\n\n");
        char *code = fxsh_codegen(ast);
        if (code) {
            printf("%s\n", code);
            sp_free(code);
        }
    }

    /* Cleanup */
    fxsh_ast_free(ast);
    fxsh_token_array_free(tokens);

    printf("\nSuccess!\n");
    return 0;
}
