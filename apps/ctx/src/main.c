#include "regex.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *argv0) {
    printf("Usage: %s [--dump] <regex> [strings...]\\n", argv0);
    printf("Example: %s --dump 'a(b|c)*d' abcd accd ad\\n", argv0);
}

int main(int argc, char **argv) {
    int argi = 1;
    int dump = 0;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[argi], "--dump") == 0) {
        dump = 1;
        argi++;
    }

    if (argi >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *pattern = argv[argi++];
    ctx_regex_t re;

    if (!ctx_regex_compile(pattern, &re)) {
        fprintf(stderr, "compile error: %s\\n", re.err[0] ? re.err : "unknown");
        return 2;
    }

    if (dump) ctx_regex_dump(&re);

    if (argi >= argc) {
        printf("compiled ok: %s\\n", pattern);
        ctx_regex_free(&re);
        return 0;
    }

    for (int i = argi; i < argc; i++) {
        int ok = ctx_regex_match(&re, argv[i]) ? 1 : 0;
        printf("%s\\t%s\\n", ok ? "MATCH" : "MISS", argv[i]);
    }

    ctx_regex_free(&re);
    return 0;
}
