#include "fxsh.h"

#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static s64 wait_status_to_code(int status) {
    if (WIFEXITED(status))
        return (s64)WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return (s64)(128 + WTERMSIG(status));
    return (s64)status;
}

static sp_str_t g_fxsh_argv0 = {.data = "", .len = 0};
static s64 g_fxsh_argc = 0;
static char **g_fxsh_argv = NULL;

static char *dup_sp_str(sp_str_t s) {
    char *p = (char *)malloc((size_t)s.len + 1);
    if (!p)
        return NULL;
    if (s.len > 0 && s.data)
        memcpy(p, s.data, s.len);
    p[s.len] = '\0';
    return p;
}

void fxsh_set_argv_rt(sp_str_t argv0, s64 argc, char **argv) {
    g_fxsh_argv0 = argv0.data ? argv0 : (sp_str_t){.data = "", .len = 0};
    g_fxsh_argc = argc >= 0 ? argc : 0;
    g_fxsh_argv = argv;
}

sp_str_t fxsh_argv0_rt(void) {
    return g_fxsh_argv0;
}

s64 fxsh_argc_rt(void) {
    return g_fxsh_argc;
}

sp_str_t fxsh_argv_at_rt(s64 index) {
    if (index < 0 || index >= g_fxsh_argc || !g_fxsh_argv)
        return (sp_str_t){.data = "", .len = 0};
    return fxsh_from_cstr(g_fxsh_argv[index]);
}

sp_str_t fxsh_getenv_rt(sp_str_t key) {
    char *k = dup_sp_str(key);
    if (!k)
        return (sp_str_t){.data = "", .len = 0};
    const char *v = getenv(k);
    free(k);
    if (!v)
        return (sp_str_t){.data = "", .len = 0};
    return (sp_str_t){.data = v, .len = (u32)strlen(v)};
}

bool fxsh_file_exists_rt(sp_str_t path) {
    char *p = dup_sp_str(path);
    if (!p)
        return false;
    bool ok = access(p, F_OK) == 0;
    free(p);
    return ok;
}

sp_str_t fxsh_getcwd_rt(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof(buf)))
        return (sp_str_t){.data = "", .len = 0};
    return fxsh_from_cstr(buf);
}

bool fxsh_is_dir_rt(sp_str_t path) {
    char *p = dup_sp_str(path);
    if (!p)
        return false;
    struct stat st;
    bool ok = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
    free(p);
    return ok;
}

bool fxsh_is_file_rt(sp_str_t path) {
    char *p = dup_sp_str(path);
    if (!p)
        return false;
    struct stat st;
    bool ok = stat(p, &st) == 0 && S_ISREG(st.st_mode);
    free(p);
    return ok;
}

s64 fxsh_file_size_rt(sp_str_t path) {
    char *p = dup_sp_str(path);
    if (!p)
        return -1;
    struct stat st;
    s64 size = (stat(p, &st) == 0 && S_ISREG(st.st_mode)) ? (s64)st.st_size : -1;
    free(p);
    return size;
}

bool fxsh_mkdir_p_rt(sp_str_t path) {
    char *p = dup_sp_str(path);
    if (!p)
        return false;
    size_t n = strlen(p);
    if (n == 0) {
        free(p);
        return false;
    }

    for (size_t i = 1; i <= n; i++) {
        if (p[i] != '/' && p[i] != '\0')
            continue;
        char saved = p[i];
        p[i] = '\0';
        if (p[0] != '\0') {
            struct stat st;
            if (stat(p, &st) != 0) {
                if (mkdir(p, 0755) != 0) {
                    free(p);
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                free(p);
                return false;
            }
        }
        p[i] = saved;
    }

    free(p);
    return true;
}

bool fxsh_remove_file_rt(sp_str_t path) {
    char *p = dup_sp_str(path);
    if (!p)
        return false;
    bool ok = unlink(p) == 0;
    free(p);
    return ok;
}

bool fxsh_rename_path_rt(sp_str_t src, sp_str_t dst) {
    char *s = dup_sp_str(src);
    char *d = dup_sp_str(dst);
    if (!s || !d) {
        free(s);
        free(d);
        return false;
    }
    bool ok = rename(s, d) == 0;
    free(s);
    free(d);
    return ok;
}

static sp_str_t read_text_path(const char *path) {
    if (!path)
        return (sp_str_t){.data = "", .len = 0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return (sp_str_t){.data = "", .len = 0};
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return (sp_str_t){.data = "", .len = 0};
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return (sp_str_t){.data = "", .len = 0};
    }
    rewind(f);
    if (n == 0) {
        fclose(f);
        return (sp_str_t){.data = "", .len = 0};
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return (sp_str_t){.data = "", .len = 0};
    }
    (void)fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[n] = '\0';
    return (sp_str_t){.data = buf, .len = (u32)n};
}

static int make_temp_path(char *tmpl, size_t n, const char *prefix) {
    if (!tmpl || n < 20)
        return -1;
    (void)mkdir(".fxsh_tmp", 0700);
    snprintf(tmpl, n, ".fxsh_tmp/%s_XXXXXX", prefix ? prefix : "fxsh");
    int fd = mkstemp(tmpl);
    if (fd >= 0)
        close(fd);
    return fd;
}

static s64 capture_cmd(const char *wrapped_cmd, sp_str_t *out_stdout, sp_str_t *out_stderr) {
    char out_tmpl[64];
    char err_tmpl[64];
    if (make_temp_path(out_tmpl, sizeof(out_tmpl), "fxsh_out") < 0 ||
        make_temp_path(err_tmpl, sizeof(err_tmpl), "fxsh_err") < 0) {
        if (out_stdout)
            *out_stdout = (sp_str_t){.data = "", .len = 0};
        if (out_stderr)
            *out_stderr = (sp_str_t){.data = "", .len = 0};
        return -1;
    }

    size_t need = strlen(wrapped_cmd) + strlen(out_tmpl) + strlen(err_tmpl) + 32;
    char *cmd = (char *)malloc(need);
    if (!cmd) {
        if (out_stdout)
            *out_stdout = (sp_str_t){.data = "", .len = 0};
        if (out_stderr)
            *out_stderr = (sp_str_t){.data = "", .len = 0};
        unlink(out_tmpl);
        unlink(err_tmpl);
        return -1;
    }

    snprintf(cmd, need, "( %s ) >\"%s\" 2>\"%s\"", wrapped_cmd, out_tmpl, err_tmpl);
    int status = system(cmd);
    free(cmd);

    if (out_stdout)
        *out_stdout = read_text_path(out_tmpl);
    if (out_stderr)
        *out_stderr = read_text_path(err_tmpl);
    unlink(out_tmpl);
    unlink(err_tmpl);
    return wait_status_to_code(status);
}

static s64 capture_cmd_stdout(const char *wrapped_cmd, sp_str_t *out_stdout) {
    sp_str_t err = {.data = "", .len = 0};
    return capture_cmd(wrapped_cmd, out_stdout, &err);
}

static s64 capture_cmd_stderr(const char *wrapped_cmd, sp_str_t *out_stderr) {
    sp_str_t out = {.data = "", .len = 0};
    return capture_cmd(wrapped_cmd, &out, out_stderr);
}

typedef struct {
    bool used;
    s64 code;
    sp_str_t out;
    sp_str_t err;
    u64 seq;
} capture_slot_t;

#define FXSH_CAPTURE_MAX 128
static capture_slot_t g_capture_slots[FXSH_CAPTURE_MAX];
static u64 g_capture_seq = 0;

static void capture_slot_release_idx(s64 i) {
    if (i < 0 || i >= FXSH_CAPTURE_MAX)
        return;
    capture_slot_t *s = &g_capture_slots[i];
    if (!s->used)
        return;
    if (s->out.data && s->out.len > 0)
        free((void *)s->out.data);
    if (s->err.data && s->err.len > 0)
        free((void *)s->err.data);
    s->used = false;
    s->code = 0;
    s->out = (sp_str_t){.data = "", .len = 0};
    s->err = (sp_str_t){.data = "", .len = 0};
    s->seq = 0;
}

static s64 capture_store(sp_str_t out, sp_str_t err, s64 code) {
    for (s64 i = 0; i < FXSH_CAPTURE_MAX; i++) {
        if (!g_capture_slots[i].used) {
            g_capture_slots[i].used = true;
            g_capture_slots[i].code = code;
            g_capture_slots[i].out = out;
            g_capture_slots[i].err = err;
            g_capture_slots[i].seq = ++g_capture_seq;
            return i;
        }
    }

    s64 victim = -1;
    u64 oldest = ~(u64)0;
    for (s64 i = 0; i < FXSH_CAPTURE_MAX; i++) {
        if (g_capture_slots[i].used && g_capture_slots[i].seq < oldest) {
            oldest = g_capture_slots[i].seq;
            victim = i;
        }
    }
    if (victim < 0)
        return -1;
    capture_slot_release_idx(victim);
    g_capture_slots[victim].used = true;
    g_capture_slots[victim].code = code;
    g_capture_slots[victim].out = out;
    g_capture_slots[victim].err = err;
    g_capture_slots[victim].seq = ++g_capture_seq;
    return victim;
}

static capture_slot_t *capture_get(s64 id) {
    if (id < 0 || id >= FXSH_CAPTURE_MAX)
        return NULL;
    if (!g_capture_slots[id].used)
        return NULL;
    return &g_capture_slots[id];
}

static int cmp_sp_str_lex(const void *a, const void *b) {
    const sp_str_t *sa = (const sp_str_t *)a;
    const sp_str_t *sb = (const sp_str_t *)b;
    u32 min_len = sa->len < sb->len ? sa->len : sb->len;
    int cmp = min_len > 0 ? memcmp(sa->data, sb->data, min_len) : 0;
    if (cmp != 0)
        return cmp;
    if (sa->len < sb->len)
        return -1;
    if (sa->len > sb->len)
        return 1;
    return 0;
}

static bool dir_entry_is_dot(const char *name) {
    return name && ((name[0] == '.' && name[1] == '\0') ||
                    (name[0] == '.' && name[1] == '.' && name[2] == '\0'));
}

static char *path_join_owned(const char *base, const char *name) {
    if (!base || !name)
        return NULL;
    size_t base_len = strlen(base);
    size_t name_len = strlen(name);
    bool add_sep = base_len > 0 && base[base_len - 1] != '/';
    char *out = (char *)malloc(base_len + (add_sep ? 1 : 0) + name_len + 1);
    if (!out)
        return NULL;
    memcpy(out, base, base_len);
    size_t off = base_len;
    if (add_sep)
        out[off++] = '/';
    memcpy(out + off, name, name_len);
    out[off + name_len] = '\0';
    return out;
}

static bool path_is_walkable_dir(const char *path) {
    struct stat st;
    if (!path || lstat(path, &st) != 0)
        return false;
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static void free_path_items(sp_dyn_array(sp_str_t) items) {
    sp_dyn_array_for(items, i) {
        if (items[i].data)
            free((void *)items[i].data);
    }
}

static sp_str_t join_path_items(sp_dyn_array(sp_str_t) items) {
    if (!items || sp_dyn_array_size(items) == 0)
        return (sp_str_t){.data = "", .len = 0};
    qsort(items, sp_dyn_array_size(items), sizeof(sp_str_t), cmp_sp_str_lex);
    size_t total = 0;
    sp_dyn_array_for(items, i) {
        total += items[i].len;
        if (i + 1 < sp_dyn_array_size(items))
            total += 1;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf)
        return (sp_str_t){.data = "", .len = 0};
    size_t off = 0;
    sp_dyn_array_for(items, i) {
        if (items[i].len > 0)
            memcpy(buf + off, items[i].data, items[i].len);
        off += items[i].len;
        if (i + 1 < sp_dyn_array_size(items))
            buf[off++] = '\n';
    }
    buf[off] = '\0';
    return (sp_str_t){.data = buf, .len = (u32)off};
}

static void collect_dir_paths(const char *root, bool recursive, sp_dyn_array(sp_str_t) * out) {
    DIR *dir = opendir(root);
    if (!dir)
        return;

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (dir_entry_is_dot(ent->d_name))
            continue;
        char *joined = path_join_owned(root, ent->d_name);
        if (!joined)
            continue;
        sp_str_t item = {.data = joined, .len = (u32)strlen(joined)};
        sp_dyn_array_push(*out, item);
        if (recursive && path_is_walkable_dir(joined))
            collect_dir_paths(joined, true, out);
    }
    closedir(dir);
}

static sp_str_t join_glob_results(glob_t *g) {
    if (!g || g->gl_pathc == 0)
        return (sp_str_t){.data = "", .len = 0};
    size_t total = 0;
    for (size_t i = 0; i < g->gl_pathc; i++) {
        total += strlen(g->gl_pathv[i]);
        if (i + 1 < g->gl_pathc)
            total += 1;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf)
        return (sp_str_t){.data = "", .len = 0};
    size_t off = 0;
    for (size_t i = 0; i < g->gl_pathc; i++) {
        size_t n = strlen(g->gl_pathv[i]);
        memcpy(buf + off, g->gl_pathv[i], n);
        off += n;
        if (i + 1 < g->gl_pathc)
            buf[off++] = '\n';
    }
    buf[off] = '\0';
    return (sp_str_t){.data = buf, .len = (u32)off};
}

s64 fxsh_exec_rt(sp_str_t cmd) {
    char *c = dup_sp_str(cmd);
    if (!c)
        return -1;
    int status = system(c);
    free(c);
    return (s64)status;
}

s64 fxsh_exec_code_rt(sp_str_t cmd) {
    char *c = dup_sp_str(cmd);
    if (!c)
        return -1;
    int status = system(c);
    free(c);
    return wait_status_to_code(status);
}

sp_str_t fxsh_exec_stdout_rt(sp_str_t cmd) {
    char *c = dup_sp_str(cmd);
    if (!c)
        return (sp_str_t){.data = "", .len = 0};
    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    (void)capture_cmd(c, &out, &err);
    free(c);
    return out;
}

s64 fxsh_exec_capture_rt(sp_str_t cmd) {
    char *c = dup_sp_str(cmd);
    if (!c)
        return -1;
    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    s64 code = capture_cmd(c, &out, &err);
    free(c);
    return capture_store(out, err, code);
}

s64 fxsh_capture_code_rt(s64 capture_id) {
    capture_slot_t *s = capture_get(capture_id);
    if (!s)
        return -1;
    return s->code;
}

sp_str_t fxsh_capture_stdout_rt(s64 capture_id) {
    capture_slot_t *s = capture_get(capture_id);
    if (!s)
        return (sp_str_t){.data = "", .len = 0};
    return s->out;
}

sp_str_t fxsh_capture_stderr_rt(s64 capture_id) {
    capture_slot_t *s = capture_get(capture_id);
    if (!s)
        return (sp_str_t){.data = "", .len = 0};
    return s->err;
}

bool fxsh_capture_release_rt(s64 capture_id) {
    if (!capture_get(capture_id))
        return false;
    capture_slot_release_idx(capture_id);
    return true;
}

sp_str_t fxsh_exec_stderr_rt(sp_str_t cmd) {
    char *c = dup_sp_str(cmd);
    if (!c)
        return (sp_str_t){.data = "", .len = 0};
    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    (void)capture_cmd(c, &out, &err);
    free(c);
    return err;
}

sp_str_t fxsh_exec_stdin_rt(sp_str_t cmd, sp_str_t input) {
    char in_tmpl[64];
    if (make_temp_path(in_tmpl, sizeof(in_tmpl), "fxsh_in") < 0)
        return (sp_str_t){.data = "", .len = 0};
    FILE *f = fopen(in_tmpl, "wb");
    if (!f) {
        unlink(in_tmpl);
        return (sp_str_t){.data = "", .len = 0};
    }
    if (input.len > 0)
        (void)fwrite(input.data, 1, input.len, f);
    fclose(f);

    char *c = dup_sp_str(cmd);
    if (!c) {
        unlink(in_tmpl);
        return (sp_str_t){.data = "", .len = 0};
    }

    size_t need = strlen(c) + strlen(in_tmpl) + 8;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(c);
        unlink(in_tmpl);
        return (sp_str_t){.data = "", .len = 0};
    }
    snprintf(wrapped, need, "%s <\"%s\"", c, in_tmpl);
    sp_str_t out = {.data = "", .len = 0};
    (void)capture_cmd_stdout(wrapped, &out);
    free(wrapped);
    free(c);
    unlink(in_tmpl);
    return out;
}

s64 fxsh_exec_stdin_code_rt(sp_str_t cmd, sp_str_t input) {
    char in_tmpl[64];
    if (make_temp_path(in_tmpl, sizeof(in_tmpl), "fxsh_in") < 0)
        return -1;
    FILE *f = fopen(in_tmpl, "wb");
    if (!f) {
        unlink(in_tmpl);
        return -1;
    }
    if (input.len > 0)
        (void)fwrite(input.data, 1, input.len, f);
    fclose(f);

    char *c = dup_sp_str(cmd);
    if (!c) {
        unlink(in_tmpl);
        return -1;
    }

    size_t need = strlen(c) + strlen(in_tmpl) + 8;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(c);
        unlink(in_tmpl);
        return -1;
    }
    snprintf(wrapped, need, "%s <\"%s\"", c, in_tmpl);
    sp_str_t out = {.data = "", .len = 0};
    s64 code = capture_cmd_stdout(wrapped, &out);
    free(wrapped);
    free(c);
    unlink(in_tmpl);
    return code;
}

s64 fxsh_exec_stdin_capture_rt(sp_str_t cmd, sp_str_t input) {
    char in_tmpl[64];
    if (make_temp_path(in_tmpl, sizeof(in_tmpl), "fxsh_in") < 0)
        return -1;
    FILE *f = fopen(in_tmpl, "wb");
    if (!f) {
        unlink(in_tmpl);
        return -1;
    }
    if (input.len > 0)
        (void)fwrite(input.data, 1, input.len, f);
    fclose(f);

    char *c = dup_sp_str(cmd);
    if (!c) {
        unlink(in_tmpl);
        return -1;
    }

    size_t need = strlen(c) + strlen(in_tmpl) + 8;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(c);
        unlink(in_tmpl);
        return -1;
    }
    snprintf(wrapped, need, "%s <\"%s\"", c, in_tmpl);
    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    s64 code = capture_cmd(wrapped, &out, &err);
    free(wrapped);
    free(c);
    unlink(in_tmpl);
    return capture_store(out, err, code);
}

sp_str_t fxsh_exec_stdin_stderr_rt(sp_str_t cmd, sp_str_t input) {
    char in_tmpl[64];
    if (make_temp_path(in_tmpl, sizeof(in_tmpl), "fxsh_in") < 0)
        return (sp_str_t){.data = "", .len = 0};
    FILE *f = fopen(in_tmpl, "wb");
    if (!f) {
        unlink(in_tmpl);
        return (sp_str_t){.data = "", .len = 0};
    }
    if (input.len > 0)
        (void)fwrite(input.data, 1, input.len, f);
    fclose(f);

    char *c = dup_sp_str(cmd);
    if (!c) {
        unlink(in_tmpl);
        return (sp_str_t){.data = "", .len = 0};
    }

    size_t need = strlen(c) + strlen(in_tmpl) + 8;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(c);
        unlink(in_tmpl);
        return (sp_str_t){.data = "", .len = 0};
    }
    snprintf(wrapped, need, "%s <\"%s\"", c, in_tmpl);
    sp_str_t err = {.data = "", .len = 0};
    (void)capture_cmd_stderr(wrapped, &err);
    free(wrapped);
    free(c);
    unlink(in_tmpl);
    return err;
}

sp_str_t fxsh_exec_pipe_rt(sp_str_t left, sp_str_t right) {
    char *l = dup_sp_str(left);
    char *r = dup_sp_str(right);
    if (!l || !r) {
        free(l);
        free(r);
        return (sp_str_t){.data = "", .len = 0};
    }
    size_t need = strlen(l) + strlen(r) + 16;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(l);
        free(r);
        return (sp_str_t){.data = "", .len = 0};
    }
    snprintf(wrapped, need, "(%s) | (%s)", l, r);
    sp_str_t out = {.data = "", .len = 0};
    (void)capture_cmd_stdout(wrapped, &out);
    free(wrapped);
    free(l);
    free(r);
    return out;
}

s64 fxsh_exec_pipe_code_rt(sp_str_t left, sp_str_t right) {
    char *l = dup_sp_str(left);
    char *r = dup_sp_str(right);
    if (!l || !r) {
        free(l);
        free(r);
        return -1;
    }
    size_t need = strlen(l) + strlen(r) + 16;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(l);
        free(r);
        return -1;
    }
    snprintf(wrapped, need, "(%s) | (%s)", l, r);
    sp_str_t out = {.data = "", .len = 0};
    s64 code = capture_cmd_stdout(wrapped, &out);
    free(wrapped);
    free(l);
    free(r);
    return code;
}

s64 fxsh_exec_pipe_capture_rt(sp_str_t left, sp_str_t right) {
    char *l = dup_sp_str(left);
    char *r = dup_sp_str(right);
    if (!l || !r) {
        free(l);
        free(r);
        return -1;
    }
    size_t need = strlen(l) + strlen(r) + 16;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(l);
        free(r);
        return -1;
    }
    snprintf(wrapped, need, "(%s) | (%s)", l, r);
    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    s64 code = capture_cmd(wrapped, &out, &err);
    free(wrapped);
    free(l);
    free(r);
    return capture_store(out, err, code);
}

s64 fxsh_exec_pipefail_capture_rt(sp_str_t left, sp_str_t right) {
    char *l = dup_sp_str(left);
    char *r = dup_sp_str(right);
    if (!l || !r) {
        free(l);
        free(r);
        return -1;
    }

    char st_tmpl[64];
    if (make_temp_path(st_tmpl, sizeof(st_tmpl), "fxsh_pf") < 0) {
        free(l);
        free(r);
        return -1;
    }

    size_t need = strlen(l) + strlen(r) + strlen(st_tmpl) * 3 + 256;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(l);
        free(r);
        unlink(st_tmpl);
        return -1;
    }

    snprintf(wrapped, need,
             "( ( %s ); echo $? >\"%s\" ) | ( %s ); "
             "_rc2=$?; _rc1=0; "
             "if [ -f \"%s\" ]; then _rc1=$(cat \"%s\"); fi; "
             "if [ \"$_rc2\" -ne 0 ]; then exit $_rc2; else exit $_rc1; fi",
             l, st_tmpl, r, st_tmpl, st_tmpl);

    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    s64 code = capture_cmd(wrapped, &out, &err);

    free(wrapped);
    free(l);
    free(r);
    unlink(st_tmpl);
    return capture_store(out, err, code);
}

s64 fxsh_exec_pipefail3_capture_rt(sp_str_t c1, sp_str_t c2, sp_str_t c3) {
    char *a = dup_sp_str(c1);
    char *b = dup_sp_str(c2);
    char *c = dup_sp_str(c3);
    if (!a || !b || !c) {
        free(a);
        free(b);
        free(c);
        return -1;
    }

    char st1[64];
    char st2[64];
    if (make_temp_path(st1, sizeof(st1), "fxsh_pf1") < 0 ||
        make_temp_path(st2, sizeof(st2), "fxsh_pf2") < 0) {
        free(a);
        free(b);
        free(c);
        return -1;
    }

    size_t need = strlen(a) + strlen(b) + strlen(c) + strlen(st1) * 3 + strlen(st2) * 3 + 320;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(a);
        free(b);
        free(c);
        unlink(st1);
        unlink(st2);
        return -1;
    }

    snprintf(wrapped, need,
             "( ( %s ); echo $? >\"%s\" ) | ( ( %s ); echo $? >\"%s\" ) | ( %s ); "
             "_rc3=$?; _rc2=0; _rc1=0; "
             "if [ -f \"%s\" ]; then _rc2=$(cat \"%s\"); fi; "
             "if [ -f \"%s\" ]; then _rc1=$(cat \"%s\"); fi; "
             "if [ \"$_rc3\" -ne 0 ]; then exit $_rc3; "
             "elif [ \"$_rc2\" -ne 0 ]; then exit $_rc2; "
             "else exit $_rc1; fi",
             a, st1, b, st2, c, st2, st2, st1, st1);

    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    s64 code = capture_cmd(wrapped, &out, &err);

    free(wrapped);
    free(a);
    free(b);
    free(c);
    unlink(st1);
    unlink(st2);
    return capture_store(out, err, code);
}

s64 fxsh_exec_pipefail4_capture_rt(sp_str_t c1, sp_str_t c2, sp_str_t c3, sp_str_t c4) {
    char *a = dup_sp_str(c1);
    char *b = dup_sp_str(c2);
    char *c = dup_sp_str(c3);
    char *d = dup_sp_str(c4);
    if (!a || !b || !c || !d) {
        free(a);
        free(b);
        free(c);
        free(d);
        return -1;
    }

    char st1[64];
    char st2[64];
    char st3[64];
    if (make_temp_path(st1, sizeof(st1), "fxsh_pf1") < 0 ||
        make_temp_path(st2, sizeof(st2), "fxsh_pf2") < 0 ||
        make_temp_path(st3, sizeof(st3), "fxsh_pf3") < 0) {
        free(a);
        free(b);
        free(c);
        free(d);
        return -1;
    }

    size_t need = strlen(a) + strlen(b) + strlen(c) + strlen(d) + strlen(st1) * 3 +
                  strlen(st2) * 3 + strlen(st3) * 3 + 420;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(a);
        free(b);
        free(c);
        free(d);
        unlink(st1);
        unlink(st2);
        unlink(st3);
        return -1;
    }

    snprintf(wrapped, need,
             "( ( %s ); echo $? >\"%s\" ) | ( ( %s ); echo $? >\"%s\" ) | "
             "( ( %s ); echo $? >\"%s\" ) | ( %s ); "
             "_rc4=$?; _rc3=0; _rc2=0; _rc1=0; "
             "if [ -f \"%s\" ]; then _rc3=$(cat \"%s\"); fi; "
             "if [ -f \"%s\" ]; then _rc2=$(cat \"%s\"); fi; "
             "if [ -f \"%s\" ]; then _rc1=$(cat \"%s\"); fi; "
             "if [ \"$_rc4\" -ne 0 ]; then exit $_rc4; "
             "elif [ \"$_rc3\" -ne 0 ]; then exit $_rc3; "
             "elif [ \"$_rc2\" -ne 0 ]; then exit $_rc2; "
             "else exit $_rc1; fi",
             a, st1, b, st2, c, st3, d, st3, st3, st2, st2, st1, st1);

    sp_str_t out = {.data = "", .len = 0};
    sp_str_t err = {.data = "", .len = 0};
    s64 code = capture_cmd(wrapped, &out, &err);

    free(wrapped);
    free(a);
    free(b);
    free(c);
    free(d);
    unlink(st1);
    unlink(st2);
    unlink(st3);
    return capture_store(out, err, code);
}

sp_str_t fxsh_exec_pipe_stderr_rt(sp_str_t left, sp_str_t right) {
    char *l = dup_sp_str(left);
    char *r = dup_sp_str(right);
    if (!l || !r) {
        free(l);
        free(r);
        return (sp_str_t){.data = "", .len = 0};
    }
    size_t need = strlen(l) + strlen(r) + 16;
    char *wrapped = (char *)malloc(need);
    if (!wrapped) {
        free(l);
        free(r);
        return (sp_str_t){.data = "", .len = 0};
    }
    snprintf(wrapped, need, "(%s) | (%s)", l, r);
    sp_str_t err = {.data = "", .len = 0};
    (void)capture_cmd_stderr(wrapped, &err);
    free(wrapped);
    free(l);
    free(r);
    return err;
}

sp_str_t fxsh_glob_rt(sp_str_t pattern) {
    char *pat = dup_sp_str(pattern);
    if (!pat)
        return (sp_str_t){.data = "", .len = 0};
    glob_t g;
    memset(&g, 0, sizeof(g));
    int rc = glob(pat, 0, NULL, &g);
    free(pat);
    if (rc != 0) {
        globfree(&g);
        return (sp_str_t){.data = "", .len = 0};
    }
    sp_str_t joined = join_glob_results(&g);
    globfree(&g);
    return joined;
}

sp_str_t fxsh_list_dir_text_rt(sp_str_t path) {
    char *root = dup_sp_str(path);
    if (!root)
        return (sp_str_t){.data = "", .len = 0};
    sp_dyn_array(sp_str_t) items = SP_NULLPTR;
    if (path_is_walkable_dir(root))
        collect_dir_paths(root, false, &items);
    sp_str_t out = join_path_items(items);
    free_path_items(items);
    if (items)
        sp_dyn_array_free(items);
    free(root);
    return out;
}

sp_str_t fxsh_walk_dir_text_rt(sp_str_t path) {
    char *root = dup_sp_str(path);
    if (!root)
        return (sp_str_t){.data = "", .len = 0};
    sp_dyn_array(sp_str_t) items = SP_NULLPTR;
    if (path_is_walkable_dir(root))
        collect_dir_paths(root, true, &items);
    sp_str_t out = join_path_items(items);
    free_path_items(items);
    if (items)
        sp_dyn_array_free(items);
    free(root);
    return out;
}
