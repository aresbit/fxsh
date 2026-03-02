/*
 * utils.c - fxsh utility functions
 */

#include "fxsh.h"

#include <stdio.h>

sp_str_t fxsh_read_file(sp_str_t path) {
    FILE *f = fopen(path.data, "rb");
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

fxsh_error_t fxsh_write_file(sp_str_t path, sp_str_t content) {
    FILE *f = fopen(path.data, "wb");
    if (!f) {
        return ERR_FILE_NOT_FOUND;
    }

    size_t written = fwrite(content.data, 1, content.len, f);
    fclose(f);

    if (written != content.len) {
        return ERR_IO_ERROR;
    }

    return ERR_OK;
}
