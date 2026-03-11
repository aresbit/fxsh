/*
 * main.c - fxsh entry point
 */

#define SP_IMPLEMENTATION
#include "fxsh.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *program) {
    printf("fxsh - Functional Core Minimal Bash\n");
    printf("Version: %d.%d.%d\n\n", FXSH_VERSION_MAJOR, FXSH_VERSION_MINOR, FXSH_VERSION_PATCH);
    printf("Usage:\n");
    printf("  %s [options] <file.fxsh> [args...]\n", program);
    printf("  %s [options] -e <expression> [args...]\n\n", program);
    printf("Options:\n");
    printf("  -e, --eval <expr>    Evaluate expression\n");
    printf("  -c, --codegen        Generate C code\n");
    printf("  -n, --native         Run closure-safe native runtime binary\n");
    printf("      --native-codegen Generate C, compile and run native codegen binary\n");
    printf("  -t, --tokens         Print tokens (debug)\n");
    printf("  -a, --ast            Print AST (debug)\n");
    printf("      --anf            Print ANF IR dump (MVP)\n");
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

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sb_t;

static bool sb_reserve(sb_t *sb, size_t need_more) {
    if (!sb)
        return false;
    size_t need = sb->len + need_more + 1;
    if (need <= sb->cap)
        return true;
    size_t ncap = sb->cap ? sb->cap : 256;
    while (ncap < need)
        ncap *= 2;
    char *nb = (char *)sp_alloc(ncap);
    if (!nb)
        return false;
    if (sb->buf && sb->len > 0)
        memcpy(nb, sb->buf, sb->len);
    sb->buf = nb;
    sb->cap = ncap;
    return true;
}

static bool sb_append_n(sb_t *sb, const char *s, size_t n) {
    if (!sb_reserve(sb, n))
        return false;
    if (n > 0)
        memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return true;
}

static bool sb_append_cstr(sb_t *sb, const char *s) {
    return sb_append_n(sb, s, strlen(s));
}

static sp_str_t sb_take(sb_t *sb) {
    if (!sb || !sb->buf)
        return (sp_str_t){.data = "", .len = 0};
    sb->buf[sb->len] = '\0';
    return (sp_str_t){.data = sb->buf, .len = (u32)sb->len};
}

static bool sb_append_shell_quoted(sb_t *sb, const char *s) {
    if (!sb || !s)
        return false;
    if (!sb_append_cstr(sb, "'"))
        return false;
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            if (!sb_append_cstr(sb, "'\\''"))
                return false;
        } else {
            if (!sb_append_n(sb, p, 1))
                return false;
        }
    }
    return sb_append_cstr(sb, "'");
}

static bool parse_line_keyword_name(const char *line, size_t n, const char *kw,
                                    sp_str_t *out_name) {
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        i++;
    size_t kw_len = strlen(kw);
    if (i + kw_len >= n)
        return false;
    if (strncmp(line + i, kw, kw_len) != 0)
        return false;
    i += kw_len;
    if (!(line[i] == ' ' || line[i] == '\t'))
        return false;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= n || !(isalpha((unsigned char)line[i]) || line[i] == '_'))
        return false;
    size_t start = i;
    i++;
    while (i < n && (isalnum((unsigned char)line[i]) || line[i] == '_'))
        i++;
    if (out_name)
        *out_name = (sp_str_t){.data = line + start, .len = (u32)(i - start)};
    return true;
}

static bool parse_line_keyword_dotted_name(const char *line, size_t n, const char *kw,
                                           sp_str_t *out_name) {
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        i++;
    size_t kw_len = strlen(kw);
    if (i + kw_len >= n)
        return false;
    if (strncmp(line + i, kw, kw_len) != 0)
        return false;
    i += kw_len;
    if (!(line[i] == ' ' || line[i] == '\t'))
        return false;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= n || !(isalpha((unsigned char)line[i]) || line[i] == '_'))
        return false;
    size_t start = i;
    while (i < n) {
        if (isalnum((unsigned char)line[i]) || line[i] == '_') {
            i++;
            continue;
        }
        if (line[i] == '.' && i + 1 < n &&
            (isalpha((unsigned char)line[i + 1]) || line[i + 1] == '_')) {
            i++;
            continue;
        }
        break;
    }
    if (out_name)
        *out_name = (sp_str_t){.data = line + start, .len = (u32)(i - start)};
    return true;
}

static bool str_in_list(sp_dyn_array(sp_str_t) names, sp_str_t s) {
    sp_dyn_array_for(names, i) {
        if (sp_str_equal(names[i], s))
            return true;
    }
    return false;
}

static sp_str_t qualify_dotted_name(sp_str_t dotted) {
    if (!dotted.data || dotted.len == 0)
        return (sp_str_t){0};

    u32 extra = 0;
    for (u32 i = 0; i < dotted.len; i++) {
        if (dotted.data[i] == '.')
            extra++;
    }

    c8 *buf = (c8 *)sp_alloc((size_t)dotted.len + extra + 1);
    if (!buf)
        return (sp_str_t){0};

    u32 out = 0;
    for (u32 i = 0; i < dotted.len; i++) {
        if (dotted.data[i] == '.') {
            buf[out++] = '_';
            buf[out++] = '_';
        } else {
            buf[out++] = dotted.data[i];
        }
    }
    buf[out] = '\0';
    return (sp_str_t){.data = buf, .len = out};
}

static void build_lower_import_path(sp_str_t mod_name, char *out, size_t out_n, char sep) {
    if (!out || out_n == 0)
        return;
    size_t out_pos = 0;
    for (u32 i = 0; i < mod_name.len && out_pos + 1 < out_n; i++) {
        unsigned char ch = (unsigned char)mod_name.data[i];
        out[out_pos++] = (char)(ch == '.' ? sep : tolower(ch));
    }
    out[out_pos] = '\0';
}

static bool file_exists_path(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static void split_dirname(const char *path, char *out_dir, size_t out_n) {
    if (!out_dir || out_n == 0) {
        return;
    }
    out_dir[0] = '.';
    out_dir[1] = '\0';
    if (!path)
        return;
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return;
    size_t n = (size_t)(slash - path);
    if (n >= out_n)
        n = out_n - 1;
    memcpy(out_dir, path, n);
    out_dir[n] = '\0';
}

static bool try_read_import_module(sp_str_t mod_name, const char *base_dir, sp_str_t *out_source,
                                   char *out_loaded_path, size_t out_loaded_path_n) {
    if (out_source)
        *out_source = (sp_str_t){.data = NULL, .len = 0};
    if (out_loaded_path && out_loaded_path_n > 0)
        out_loaded_path[0] = '\0';

    char lower_path[512];
    char lower_flat[512];
    build_lower_import_path(mod_name, lower_path, sizeof(lower_path), '/');
    build_lower_import_path(mod_name, lower_flat, sizeof(lower_flat), '.');
    if (lower_path[0] == '\0')
        return false;

    char cand1[512];
    snprintf(cand1, sizeof(cand1), "stdlib/%s.fxsh", lower_path);
    if (file_exists_path(cand1)) {
        sp_str_t s = read_file_to_str(cand1);
        if (s.data) {
            if (out_source)
                *out_source = s;
            if (out_loaded_path && out_loaded_path_n > 0) {
                strncpy(out_loaded_path, cand1, out_loaded_path_n - 1);
                out_loaded_path[out_loaded_path_n - 1] = '\0';
            }
            return true;
        }
    }

    if (strcmp(lower_flat, lower_path) != 0) {
        char cand1b[512];
        snprintf(cand1b, sizeof(cand1b), "stdlib/%s.fxsh", lower_flat);
        if (file_exists_path(cand1b)) {
            sp_str_t s = read_file_to_str(cand1b);
            if (s.data) {
                if (out_source)
                    *out_source = s;
                if (out_loaded_path && out_loaded_path_n > 0) {
                    strncpy(out_loaded_path, cand1b, out_loaded_path_n - 1);
                    out_loaded_path[out_loaded_path_n - 1] = '\0';
                }
                return true;
            }
        }
    }

    if (base_dir && base_dir[0]) {
        char cand2[512];
        snprintf(cand2, sizeof(cand2), "%s/%s.fxsh", base_dir, lower_path);
        if (file_exists_path(cand2)) {
            sp_str_t s = read_file_to_str(cand2);
            if (s.data) {
                if (out_source)
                    *out_source = s;
                if (out_loaded_path && out_loaded_path_n > 0) {
                    strncpy(out_loaded_path, cand2, out_loaded_path_n - 1);
                    out_loaded_path[out_loaded_path_n - 1] = '\0';
                }
                return true;
            }
        }

        if (strcmp(lower_flat, lower_path) != 0) {
            char cand2b[512];
            snprintf(cand2b, sizeof(cand2b), "%s/%s.fxsh", base_dir, lower_flat);
            if (file_exists_path(cand2b)) {
                sp_str_t s = read_file_to_str(cand2b);
                if (s.data) {
                    if (out_source)
                        *out_source = s;
                    if (out_loaded_path && out_loaded_path_n > 0) {
                        strncpy(out_loaded_path, cand2b, out_loaded_path_n - 1);
                        out_loaded_path[out_loaded_path_n - 1] = '\0';
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

static sp_str_t expand_imports_recursive(sp_str_t source, const char *base_dir,
                                         sp_dyn_array(sp_str_t) * loaded, int depth) {
    if (depth > 8)
        return source;

    sp_dyn_array(sp_str_t) local_mods = SP_NULLPTR;
    const char *src = source.data;
    size_t src_n = source.len;
    size_t pos = 0;
    while (pos < src_n) {
        size_t line_start = pos;
        while (pos < src_n && src[pos] != '\n')
            pos++;
        size_t line_len = pos - line_start;
        sp_str_t name = {0};
        if (parse_line_keyword_name(src + line_start, line_len, "module", &name)) {
            if (!str_in_list(local_mods, name))
                sp_dyn_array_push(local_mods, name);
        }
        if (pos < src_n && src[pos] == '\n')
            pos++;
    }

    sb_t prepend = {0};
    pos = 0;
    while (pos < src_n) {
        size_t line_start = pos;
        while (pos < src_n && src[pos] != '\n')
            pos++;
        size_t line_len = pos - line_start;
        sp_str_t import_name = {0};
        if (parse_line_keyword_dotted_name(src + line_start, line_len, "import", &import_name)) {
            sp_str_t import_qname = qualify_dotted_name(import_name);
            if (!str_in_list(local_mods, import_qname) && !str_in_list(*loaded, import_qname)) {
                sp_str_t mod_src = {0};
                char loaded_path[512] = {0};
                if (try_read_import_module(import_name, base_dir, &mod_src, loaded_path,
                                           sizeof(loaded_path))) {
                    sp_dyn_array_push(*loaded, import_qname);
                    char next_dir[512];
                    split_dirname(loaded_path, next_dir, sizeof(next_dir));
                    sp_str_t expanded_mod =
                        expand_imports_recursive(mod_src, next_dir, loaded, depth + 1);

                    sb_append_cstr(&prepend, "module ");
                    sb_append_n(&prepend, import_qname.data, import_qname.len);
                    sb_append_cstr(&prepend, " = {\n");
                    sb_append_n(&prepend, expanded_mod.data, expanded_mod.len);
                    if (expanded_mod.len == 0 || expanded_mod.data[expanded_mod.len - 1] != '\n')
                        sb_append_cstr(&prepend, "\n");
                    sb_append_cstr(&prepend, "}\n");
                    sb_append_cstr(&prepend, "import ");
                    sb_append_n(&prepend, import_qname.data, import_qname.len);
                    sb_append_cstr(&prepend, "\n\n");
                }
            }
        }
        if (pos < src_n && src[pos] == '\n')
            pos++;
    }

    if (prepend.len == 0)
        return source;

    sb_t out = {0};
    sb_append_n(&out, prepend.buf, prepend.len);
    sb_append_n(&out, source.data, source.len);
    return sb_take(&out);
}

static sp_str_t expand_imports(sp_str_t source, const char *origin_path) {
    char base_dir[512];
    split_dirname(origin_path, base_dir, sizeof(base_dir));
    sp_dyn_array(sp_str_t) loaded = SP_NULLPTR;
    return expand_imports_recursive(source, base_dir, &loaded, 0);
}

typedef enum {
    MODE_COMPILE,
    MODE_EVAL,
    MODE_TOKENS,
    MODE_AST,
    MODE_ANF,
    MODE_CODEGEN,
    MODE_NATIVE,
    MODE_NATIVE_CODEGEN,
} run_mode_t;

static fxsh_error_t run_native_interp_harness(sp_str_t source, int script_argc, char **script_argv,
                                              const char *argv0) {
    long pid = (long)getpid();
    char c_path[128];
    char bin_path[128];
    char cc_cmd[1024];

    snprintf(c_path, sizeof(c_path), "build/fxsh_native_%ld.c", pid);
    snprintf(bin_path, sizeof(bin_path), "bin/fxsh_native_%ld", pid);

    FILE *f = fopen(c_path, "wb");
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
    fprintf(f, "int main(int argc, char **argv) {\n");
    fprintf(f, "  fxsh_set_argv_rt(argc > 1 ? fxsh_from_cstr(argv[1]) : (argc > 0 ? "
               "fxsh_from_cstr(argv[0]) : fxsh_from_cstr(\"\")), argc > 2 ? (s64)(argc - 2) : 0, "
               "argc > 2 ? argv + 2 : NULL);\n");
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

#if defined(__linux__)
    const char *dl_link = " -ldl";
#else
    const char *dl_link = "";
#endif
    snprintf(cc_cmd, sizeof(cc_cmd),
             "clang -std=gnu17 -D _GNU_SOURCE -Wall -Wextra -Wno-strict-prototypes "
             "-pedantic -Iinclude -Ilib -DSP_PS_DISABLE -Oz -DNDEBUG "
             "%s src/utils.c src/lexer/lexer.c src/parser/parser.c "
             "src/types/types.c src/comptime/comptime.c src/interp/interp.c "
             "src/runtime/runtime.c src/runtime/json.c src/runtime/regex.c "
             "src/runtime/shell.c src/runtime/text.c "
             "-lm%s -o %s",
             c_path, dl_link, bin_path);
    int cc = system(cc_cmd);
    if (cc != 0) {
        fprintf(stderr, "Native fallback compile failed\n");
        return ERR_INTERNAL;
    }

    sb_t run_cmd = {0};
    sb_append_cstr(&run_cmd, "./");
    sb_append_cstr(&run_cmd, bin_path);
    if (argv0 && argv0[0]) {
        sb_append_cstr(&run_cmd, " ");
        sb_append_shell_quoted(&run_cmd, argv0);
    }
    for (int i = 0; i < script_argc; i++) {
        sb_append_cstr(&run_cmd, " ");
        sb_append_shell_quoted(&run_cmd, script_argv[i]);
    }
    int rc = system(run_cmd.buf ? run_cmd.buf : "");
    if (rc != 0) {
        fprintf(stderr, "Native fallback run failed (exit=%d)\n", rc);
        return ERR_INTERNAL;
    }
    return ERR_OK;
}

static fxsh_error_t run_native_codegen(fxsh_ast_node_t *ast, int script_argc, char **script_argv,
                                       const char *argv0) {
    char *code = fxsh_codegen(ast);
    if (!code)
        return ERR_OUT_OF_MEMORY;

    long pid = (long)getpid();
    char c_path_buf[128];
    char bin_path_buf[128];
    char main_obj_buf[128];
    char utils_obj_buf[128];
    char json_obj_buf[128];
    char regex_obj_buf[128];
    char shell_obj_buf[128];
    char text_obj_buf[128];

    snprintf(c_path_buf, sizeof(c_path_buf), "build/fxsh_native_%ld.c", pid);
    snprintf(bin_path_buf, sizeof(bin_path_buf), "bin/fxsh_native_%ld", pid);
    snprintf(main_obj_buf, sizeof(main_obj_buf), "build/fxsh_native_%ld.o", pid);
    snprintf(utils_obj_buf, sizeof(utils_obj_buf), "build/utils_native_%ld.o", pid);
    snprintf(json_obj_buf, sizeof(json_obj_buf), "build/json_native_%ld.o", pid);
    snprintf(regex_obj_buf, sizeof(regex_obj_buf), "build/regex_native_%ld.o", pid);
    snprintf(shell_obj_buf, sizeof(shell_obj_buf), "build/shell_native_%ld.o", pid);
    snprintf(text_obj_buf, sizeof(text_obj_buf), "build/text_native_%ld.o", pid);

    sp_str_t c_path = sp_str_view(c_path_buf);
    sp_str_t bin_path = sp_str_view(bin_path_buf);
    fxsh_error_t err = fxsh_write_file(c_path, (sp_str_t){.data = code, .len = (u32)strlen(code)});
    sp_free(code);
    if (err != ERR_OK)
        return err;

    const char *extra_cflags = getenv("FXSH_CFLAGS");
    const char *extra_ldflags = getenv("FXSH_LDFLAGS");
    char cc_cmd[4096];
#if defined(__ANDROID__)
    const char *platform_flags = "-DSP_PS_DISABLE";
#else
    const char *platform_flags = "";
#endif
#if defined(__linux__)
    const char *dl_link = " -ldl";
#else
    const char *dl_link = "";
#endif
    snprintf(cc_cmd, sizeof(cc_cmd),
             "clang -std=gnu17 -D _GNU_SOURCE -DSP_IMPLEMENTATION -O2 -Wall -Wextra -Iinclude "
             "-Ilib %s %s -c %s -o %s && "
             "clang -std=gnu17 -D _GNU_SOURCE -DSP_IMPLEMENTATION -O2 -Wall -Wextra -Iinclude "
             "-Ilib %s %s -c "
             "src/utils.c -o %s && "
             "clang -std=gnu17 -D _GNU_SOURCE -O2 -Wall -Wextra -Iinclude -Ilib %s %s -c "
             "src/runtime/json.c -o %s && "
             "clang -std=gnu17 -D _GNU_SOURCE -O2 -Wall -Wextra -Iinclude -Ilib %s %s -c "
             "src/runtime/regex.c -o %s && "
             "clang -std=gnu17 -D _GNU_SOURCE -O2 -Wall -Wextra -Iinclude -Ilib %s %s -c "
             "src/runtime/shell.c -o %s && "
             "clang -std=gnu17 -D _GNU_SOURCE -O2 -Wall -Wextra -Iinclude -Ilib %s %s -c "
             "src/runtime/text.c -o %s && "
             "clang %s %s %s %s %s %s -lm%s -o %s %s",
             platform_flags, extra_cflags ? extra_cflags : "", c_path_buf, main_obj_buf,
             platform_flags, extra_cflags ? extra_cflags : "", utils_obj_buf, platform_flags,
             extra_cflags ? extra_cflags : "", json_obj_buf, platform_flags,
             extra_cflags ? extra_cflags : "", regex_obj_buf, platform_flags,
             extra_cflags ? extra_cflags : "", shell_obj_buf, platform_flags,
             extra_cflags ? extra_cflags : "", text_obj_buf, main_obj_buf, utils_obj_buf,
             json_obj_buf, regex_obj_buf, shell_obj_buf, text_obj_buf, dl_link, bin_path_buf,
             extra_ldflags ? extra_ldflags : "");
    int cc = system(cc_cmd);
    if (cc != 0) {
        fprintf(stderr, "Native codegen compile failed\n");
        fprintf(stderr, "Compile command: %s\n", cc_cmd);
        return ERR_INTERNAL;
    }

    sb_t run_cmd = {0};
    sb_append_cstr(&run_cmd, "./");
    sb_append_cstr(&run_cmd, bin_path_buf);
    if (argv0 && argv0[0]) {
        sb_append_cstr(&run_cmd, " ");
        sb_append_shell_quoted(&run_cmd, argv0);
    }
    for (int i = 0; i < script_argc; i++) {
        sb_append_cstr(&run_cmd, " ");
        sb_append_shell_quoted(&run_cmd, script_argv[i]);
    }
    int rc = system(run_cmd.buf ? run_cmd.buf : "");
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
    bool end_of_options = false;
    char **script_argv = (char **)calloc((size_t)argc, sizeof(char *));
    int script_argc = 0;
    if (!script_argv) {
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (!end_of_options && strcmp(argv[i], "--") == 0) {
            end_of_options = true;
            continue;
        }
        if (input && (end_of_options || argv[i][0] != '-')) {
            script_argv[script_argc++] = argv[i];
            continue;
        }
        if (input && !end_of_options && argv[i][0] == '-') {
            script_argv[script_argc++] = argv[i];
            continue;
        }
        if (end_of_options) {
            input = argv[i];
            continue;
        }
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
        if (strcmp(argv[i], "--anf") == 0) {
            mode = MODE_ANF;
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
        source = expand_imports(source, ".");
    } else {
        source = read_file_to_str(input);
        if (source.data == NULL) {
            fprintf(stderr, "Error: Cannot read file: %s\n", input);
            return 1;
        }
        filename = sp_str_view(input);
        source = expand_imports(source, input);
    }
    fxsh_set_argv_rt(input_is_eval ? sp_str_lit("<eval>") : sp_str_view(input), (s64)script_argc,
                     script_argv);

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
    if (parser.had_error) {
        fxsh_ast_free(ast);
        fxsh_token_array_free(tokens);
        return 1;
    }

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

    if (mode == MODE_ANF) {
        printf("Type: %s\n", fxsh_type_to_string(type));
        char *anf = fxsh_anf_dump(ast);
        if (!anf) {
            fprintf(stderr, "ANF lower error\n");
            fxsh_ast_free(ast);
            fxsh_token_array_free(tokens);
            return 1;
        }
        printf("\nANF:\n%s\n", anf);
        fxsh_ast_free(ast);
        fxsh_token_array_free(tokens);
        return 0;
    }

    err = fxsh_ct_expand_program(ast, type_env);
    if (err != ERR_OK) {
        const c8 *ct_err = fxsh_ct_last_error();
        fprintf(stderr, "Comptime expansion error: %s\n", ct_err ? ct_err : "unknown error");
        fxsh_ast_free(ast);
        fxsh_token_array_free(tokens);
        return 1;
    }

    if (mode == MODE_NATIVE || mode == MODE_NATIVE_CODEGEN) {
        if (mode == MODE_NATIVE) {
            err = run_native_interp_harness(source, script_argc, script_argv,
                                            input_is_eval ? "<eval>" : input);
        } else {
            err =
                run_native_codegen(ast, script_argc, script_argv, input_is_eval ? "<eval>" : input);
        }
        if (err != ERR_OK) {
            fxsh_ast_free(ast);
            fxsh_token_array_free(tokens);
            return 1;
        }
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
        if (mode == MODE_EVAL) {
            printf("%.*s\n", interp_result.len, interp_result.data);
        }
    }

    /* Code generation */
    if (mode == MODE_CODEGEN) {
        char *code = fxsh_codegen(ast);
        if (code) {
            printf("%s\n", code);
            sp_free(code);
        }
    }

    /* Cleanup */
    fxsh_ast_free(ast);
    fxsh_token_array_free(tokens);
    return 0;
}
