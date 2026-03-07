/*
 * lexer.c - fxsh lexical analyzer
 *
 * Fixes:
 *   1. advance_line: cursor must be advanced past '\n' BEFORE resetting line_start
 *   2. Type variable 'a: after consuming '\'', only treat as type-var if
 *      followed by [a-z]; otherwise it's a string delimiter error
 *   3. TRUE/FALSE tokens now set data.lit_bool correctly
 */

#include "fxsh.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static inline sp_str_t sp_str_from_range(const c8 *start, const c8 *end) {
    return (sp_str_t){.data = start, .len = (u32)(end - start)};
}

/*=============================================================================
 * Keyword Table
 *=============================================================================*/

typedef struct {
    sp_str_t keyword;
    fxsh_token_kind_t kind;
} keyword_entry_t;

static const keyword_entry_t keywords[] = {
    {sp_str_lit("let"), TOK_LET},       {sp_str_lit("fn"), TOK_FN},
    {sp_str_lit("in"), TOK_IN},         {sp_str_lit("end"), TOK_END},
    {sp_str_lit("if"), TOK_IF},         {sp_str_lit("then"), TOK_THEN},
    {sp_str_lit("else"), TOK_ELSE},     {sp_str_lit("match"), TOK_MATCH},
    {sp_str_lit("with"), TOK_WITH},     {sp_str_lit("module"), TOK_MODULE},
    {sp_str_lit("import"), TOK_IMPORT}, {sp_str_lit("comptime"), TOK_COMPTIME},
    {sp_str_lit("type"), TOK_TYPE},     {sp_str_lit("of"), TOK_OF},
    {sp_str_lit("struct"), TOK_STRUCT}, {sp_str_lit("trait"), TOK_TRAIT},
    {sp_str_lit("impl"), TOK_IMPL},     {sp_str_lit("for"), TOK_FOR},
    {sp_str_lit("while"), TOK_WHILE},   {sp_str_lit("return"), TOK_RETURN},
    {sp_str_lit("true"), TOK_TRUE},     {sp_str_lit("false"), TOK_FALSE},
    {sp_str_lit("and"), TOK_AND},       {sp_str_lit("or"), TOK_OR},
    {sp_str_lit("not"), TOK_NOT},       {sp_str_lit("rec"), TOK_REC},
    {sp_str_lit("do"), TOK_DO},
};
#define NUM_KEYWORDS (sizeof(keywords) / sizeof(keywords[0]))

static fxsh_token_kind_t lookup_keyword(sp_str_t ident) {
    for (u32 i = 0; i < NUM_KEYWORDS; i++)
        if (sp_str_equal(ident, keywords[i].keyword))
            return keywords[i].kind;
    return TOK_IDENT;
}

/*=============================================================================
 * Token Kind Names
 *=============================================================================*/

const c8 *fxsh_token_kind_name(fxsh_token_kind_t kind) {
    switch (kind) {
        case TOK_INT:
            return "INT";
        case TOK_FLOAT:
            return "FLOAT";
        case TOK_STRING:
            return "STRING";
        case TOK_FSTRING:
            return "FSTRING";
        case TOK_BOOL:
            return "BOOL";
        case TOK_UNIT:
            return "UNIT";
        case TOK_IDENT:
            return "IDENT";
        case TOK_TYPE_IDENT:
            return "TYPE_IDENT";
        case TOK_LET:
            return "LET";
        case TOK_FN:
            return "FN";
        case TOK_IN:
            return "IN";
        case TOK_END:
            return "END";
        case TOK_IF:
            return "IF";
        case TOK_THEN:
            return "THEN";
        case TOK_ELSE:
            return "ELSE";
        case TOK_MATCH:
            return "MATCH";
        case TOK_WITH:
            return "WITH";
        case TOK_MODULE:
            return "MODULE";
        case TOK_IMPORT:
            return "IMPORT";
        case TOK_COMPTIME:
            return "COMPTIME";
        case TOK_TYPE:
            return "TYPE";
        case TOK_OF:
            return "OF";
        case TOK_STRUCT:
            return "STRUCT";
        case TOK_TRAIT:
            return "TRAIT";
        case TOK_IMPL:
            return "IMPL";
        case TOK_FOR:
            return "FOR";
        case TOK_WHILE:
            return "WHILE";
        case TOK_RETURN:
            return "RETURN";
        case TOK_TRUE:
            return "TRUE";
        case TOK_FALSE:
            return "FALSE";
        case TOK_AND:
            return "AND";
        case TOK_OR:
            return "OR";
        case TOK_NOT:
            return "NOT";
        case TOK_REC:
            return "REC";
        case TOK_DO:
            return "DO";
        case TOK_PLUS:
            return "PLUS";
        case TOK_MINUS:
            return "MINUS";
        case TOK_STAR:
            return "STAR";
        case TOK_SLASH:
            return "SLASH";
        case TOK_PERCENT:
            return "PERCENT";
        case TOK_ARROW:
            return "ARROW";
        case TOK_FAT_ARROW:
            return "FAT_ARROW";
        case TOK_PIPE:
            return "PIPE";
        case TOK_ASSIGN:
            return "ASSIGN";
        case TOK_EQ:
            return "EQ";
        case TOK_NEQ:
            return "NEQ";
        case TOK_BANG:
            return "BANG";
        case TOK_QMARK:
            return "QMARK";
        case TOK_LT:
            return "LT";
        case TOK_GT:
            return "GT";
        case TOK_LEQ:
            return "LEQ";
        case TOK_GEQ:
            return "GEQ";
        case TOK_CONCAT:
            return "CONCAT";
        case TOK_APPEND:
            return "APPEND";
        case TOK_LPAREN:
            return "LPAREN";
        case TOK_RPAREN:
            return "RPAREN";
        case TOK_LBRACE:
            return "LBRACE";
        case TOK_RBRACE:
            return "RBRACE";
        case TOK_LBRACKET:
            return "LBRACKET";
        case TOK_RBRACKET:
            return "RBRACKET";
        case TOK_COMMA:
            return "COMMA";
        case TOK_SEMICOLON:
            return "SEMICOLON";
        case TOK_COLON:
            return "COLON";
        case TOK_DOT:
            return "DOT";
        case TOK_DOTDOT:
            return "DOTDOT";
        case TOK_PIPE_SYM:
            return "PIPE_SYM";
        case TOK_AT:
            return "AT";
        case TOK_NEWLINE:
            return "NEWLINE";
        case TOK_COMMENT:
            return "COMMENT";
        case TOK_EOF:
            return "EOF";
        case TOK_ERROR:
            return "ERROR";
    }
    return "UNKNOWN";
}

/*=============================================================================
 * Lexer Init
 *=============================================================================*/

void fxsh_lexer_init(fxsh_lexer_t *lexer, sp_str_t source, sp_str_t filename) {
    lexer->source = source;
    lexer->filename = filename;
    lexer->cursor = source.data;
    lexer->line_start = source.data;
    lexer->line = 1;
}

/*=============================================================================
 * Helpers
 *=============================================================================*/

static inline bool is_at_end(fxsh_lexer_t *l) {
    return l->cursor >= l->source.data + l->source.len;
}

static inline c8 peek(fxsh_lexer_t *l) {
    return is_at_end(l) ? '\0' : *l->cursor;
}

static inline c8 peek_next(fxsh_lexer_t *l) {
    if (l->cursor + 1 >= l->source.data + l->source.len)
        return '\0';
    return *(l->cursor + 1);
}

static inline c8 peek_n(fxsh_lexer_t *l, u32 n) {
    if (l->cursor + n >= l->source.data + l->source.len)
        return '\0';
    return *(l->cursor + n);
}

static inline c8 advance(fxsh_lexer_t *l) {
    return is_at_end(l) ? '\0' : *l->cursor++;
}

static inline bool match_char(fxsh_lexer_t *l, c8 expected) {
    if (is_at_end(l) || peek(l) != expected)
        return false;
    l->cursor++;
    return true;
}

static inline void skip_whitespace(fxsh_lexer_t *l) {
    while (!is_at_end(l)) {
        c8 c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r')
            advance(l);
        else
            break;
    }
}

static inline fxsh_loc_t make_loc(fxsh_lexer_t *l) {
    return FXSH_LOC(l->filename, l->line, (u32)(l->cursor - l->line_start) + 1);
}

/*=============================================================================
 * Token Readers
 *=============================================================================*/

static s32 hex_digit(c8 c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static fxsh_error_t read_string_core(fxsh_lexer_t *l, fxsh_token_t *out, fxsh_loc_t loc, bool raw,
                                     bool bytes, bool fstr) {
    bool triple = peek(l) == '"' && peek_next(l) == '"' && peek_n(l, 2) == '"';
    if (triple) {
        advance(l);
        advance(l);
        advance(l);
    } else {
        advance(l); /* consume opening " */
    }
    sp_dyn_array(c8) chars = SP_NULLPTR;

    while (!is_at_end(l)) {
        if (triple) {
            if (peek(l) == '"' && peek_next(l) == '"' && peek_n(l, 2) == '"')
                break;
        } else if (peek(l) == '"') {
            break;
        }

        c8 c = advance(l);
        if (c == '\n') {
            l->line++;
            l->line_start = l->cursor;
        }

        if (!raw && c == '\\') {
            if (is_at_end(l)) {
                out->kind = TOK_ERROR;
                sp_dyn_array_free(chars);
                return ERR_SYNTAX_ERROR;
            }
            c8 e = advance(l);
            if (e == '\n') {
                l->line++;
                l->line_start = l->cursor;
            }
            switch (e) {
                case 'n':
                    sp_dyn_array_push(chars, '\n');
                    break;
                case 't':
                    sp_dyn_array_push(chars, '\t');
                    break;
                case 'r':
                    sp_dyn_array_push(chars, '\r');
                    break;
                case '\\':
                    sp_dyn_array_push(chars, '\\');
                    break;
                case '"':
                    sp_dyn_array_push(chars, '"');
                    break;
                case '0':
                    sp_dyn_array_push(chars, '\0');
                    break;
                case 'x': {
                    s32 h1 = hex_digit(peek(l));
                    s32 h2 = hex_digit(peek_next(l));
                    if (h1 < 0 || h2 < 0) {
                        fprintf(stderr, "Lexer error at %d:%u: invalid hex escape\n", l->line,
                                (u32)(l->cursor - l->line_start));
                        out->kind = TOK_ERROR;
                        sp_dyn_array_free(chars);
                        return ERR_SYNTAX_ERROR;
                    }
                    advance(l);
                    advance(l);
                    sp_dyn_array_push(chars, (c8)((h1 << 4) | h2));
                    break;
                }
                default:
                    sp_dyn_array_push(chars, e);
                    break;
            }
            continue;
        }

        sp_dyn_array_push(chars, c);
    }

    if (is_at_end(l)) {
        fprintf(stderr, "Lexer error at %d:%u: unterminated string\n", l->line,
                (u32)(l->cursor - l->line_start));
        out->kind = TOK_ERROR;
        sp_dyn_array_free(chars);
        return ERR_SYNTAX_ERROR;
    }
    if (triple) {
        advance(l);
        advance(l);
        advance(l);
    } else {
        advance(l); /* consume closing " */
    }

    u32 len = chars ? (u32)sp_dyn_array_size(chars) : 0;
    c8 *buf = (c8 *)sp_alloc(len + 1);
    if (len)
        memcpy(buf, chars, len);
    buf[len] = '\0';

    (void)bytes; /* bytes literal reuses string runtime representation */
    out->kind = fstr ? TOK_FSTRING : TOK_STRING;
    out->loc = loc;
    out->data.str_val = (sp_str_t){.data = buf, .len = len};
    sp_dyn_array_free(chars);
    return ERR_OK;
}

static fxsh_error_t read_string(fxsh_lexer_t *l, fxsh_token_t *out) {
    return read_string_core(l, out, make_loc(l), false, false, false);
}

static fxsh_error_t read_prefixed_string(fxsh_lexer_t *l, fxsh_token_t *out) {
    fxsh_loc_t loc = make_loc(l);
    c8 p = advance(l); /* consume prefix char: r/b/f */
    bool raw = (p == 'r');
    bool bytes = (p == 'b');
    bool fstr = (p == 'f');
    if (peek(l) != '"') {
        out->kind = TOK_ERROR;
        out->loc = loc;
        return ERR_SYNTAX_ERROR;
    }
    return read_string_core(l, out, loc, raw, bytes, fstr);
}

static fxsh_error_t read_number(fxsh_lexer_t *l, fxsh_token_t *out) {
    fxsh_loc_t loc = make_loc(l);
    const c8 *start = l->cursor;
    bool is_float = false;

    /* Integer digits */
    if (peek(l) == '0' && (peek_next(l) == 'x' || peek_next(l) == 'X')) {
        /* Hex literal */
        advance(l);
        advance(l);
        while (isxdigit(peek(l)))
            advance(l);
        goto make_num;
    }
    while (isdigit(peek(l)))
        advance(l);

    /* Decimal fraction */
    if (peek(l) == '.' && isdigit(peek_next(l))) {
        is_float = true;
        advance(l);
        while (isdigit(peek(l)))
            advance(l);
    }

    /* Exponent */
    if (peek(l) == 'e' || peek(l) == 'E') {
        is_float = true;
        advance(l);
        if (peek(l) == '+' || peek(l) == '-')
            advance(l);
        while (isdigit(peek(l)))
            advance(l);
    }

make_num:;
    /* Copy to NUL-terminated temp buffer for strtod/strtoll */
    u32 nlen = (u32)(l->cursor - start);
    char tmp[64];
    if (nlen < sizeof(tmp)) {
        memcpy(tmp, start, nlen);
        tmp[nlen] = '\0';
    } else {
        out->kind = TOK_ERROR;
        return ERR_SYNTAX_ERROR;
    }

    out->loc = loc;
    if (is_float) {
        out->kind = TOK_FLOAT;
        out->data.float_val = strtod(tmp, NULL);
    } else {
        out->kind = TOK_INT;
        out->data.int_val = strtoll(tmp, NULL, 0);
    }
    return ERR_OK;
}

static fxsh_error_t read_identifier(fxsh_lexer_t *l, fxsh_token_t *out) {
    fxsh_loc_t loc = make_loc(l);
    const c8 *start = l->cursor;

    while (isalnum(peek(l)) || peek(l) == '_')
        advance(l);
    /* Allow trailing primes in identifiers: x', x'' */
    while (peek(l) == '\'')
        advance(l);

    sp_str_t ident = sp_str_from_range(start, l->cursor);
    bool is_type = isupper((unsigned char)ident.data[0]);

    fxsh_token_kind_t kind = lookup_keyword(ident);
    if (kind == TOK_IDENT)
        kind = is_type ? TOK_TYPE_IDENT : TOK_IDENT;

    out->loc = loc;
    out->kind = kind;

    if (kind == TOK_IDENT || kind == TOK_TYPE_IDENT) {
        /* Copy identifier string */
        c8 *buf = (c8 *)sp_alloc(ident.len + 1);
        memcpy(buf, ident.data, ident.len);
        buf[ident.len] = '\0';
        out->data.ident = (sp_str_t){.data = buf, .len = ident.len};
    } else if (kind == TOK_TRUE) {
        out->data.int_val = 1; /* lit_bool reuses int_val slot via union */
    } else if (kind == TOK_FALSE) {
        out->data.int_val = 0;
    }
    return ERR_OK;
}

static fxsh_error_t read_comment(fxsh_lexer_t *l, fxsh_token_t *out) {
    fxsh_loc_t loc = make_loc(l);
    advance(l); /* consume # */
    const c8 *start = l->cursor;
    while (!is_at_end(l) && peek(l) != '\n')
        advance(l);
    out->kind = TOK_COMMENT;
    out->loc = loc;
    out->data.str_val = sp_str_from_range(start, l->cursor);
    return ERR_OK;
}

/*=============================================================================
 * Main Lexer
 *=============================================================================*/

fxsh_error_t fxsh_lexer_next(fxsh_lexer_t *l, fxsh_token_t *out) {
    skip_whitespace(l);

    if (is_at_end(l)) {
        out->kind = TOK_EOF;
        out->loc = make_loc(l);
        return ERR_OK;
    }

    c8 c = peek(l);

    /* ── Newline ─────────────────────────────── */
    if (c == '\n') {
        out->loc = make_loc(l);
        advance(l);                /* move past '\n' */
        l->line++;                 /* THEN increment line */
        l->line_start = l->cursor; /* THEN reset line start */
        out->kind = TOK_NEWLINE;
        return ERR_OK;
    }

    /* ── Comment ─────────────────────────────── */
    if (c == '#')
        return read_comment(l, out);

    /* ── Number ──────────────────────────────── */
    if (isdigit((unsigned char)c))
        return read_number(l, out);

    /* ── String prefixes: r"", b"", f"" ─────── */
    if ((c == 'r' || c == 'b' || c == 'f') && peek_next(l) == '"')
        return read_prefixed_string(l, out);

    /* ── Identifier / Keyword ────────────────── */
    if (isalpha((unsigned char)c) || c == '_')
        return read_identifier(l, out);

    /* ── String ──────────────────────────────── */
    if (c == '"')
        return read_string(l, out);

    fxsh_loc_t loc = make_loc(l);

    /* ── Type variable / character ───────────── */
    if (c == '\'') {
        advance(l); /* consume '\'' */
        if (isalpha((unsigned char)peek(l))) {
            /* Type variable: 'a, 'b, 'myvar */
            const c8 *start = l->cursor;
            while (isalnum((unsigned char)peek(l)) || peek(l) == '_')
                advance(l);
            u32 len = (u32)(l->cursor - start);
            /* Allocate: include the leading ' so callers can recognise it */
            c8 *buf = (c8 *)sp_alloc(len + 2);
            buf[0] = '\'';
            memcpy(buf + 1, start, len);
            buf[len + 1] = '\0';
            out->kind = TOK_IDENT; /* type vars are plain IDENT with leading ' */
            out->loc = loc;
            out->data.ident = (sp_str_t){.data = buf, .len = len + 1};
            return ERR_OK;
        }
        /* Bare ' — treat as error (we don't have char literals) */
        out->kind = TOK_ERROR;
        out->loc = loc;
        return ERR_SYNTAX_ERROR;
    }

    /* ── Multi-character operators ───────────── */
    switch (c) {
        case '-':
            advance(l);
            out->kind = match_char(l, '>') ? TOK_ARROW : TOK_MINUS;
            out->loc = loc;
            return ERR_OK;
        case '=':
            advance(l);
            if (match_char(l, '>'))
                out->kind = TOK_FAT_ARROW;
            else if (match_char(l, '='))
                out->kind = TOK_EQ;
            else
                out->kind = TOK_ASSIGN;
            out->loc = loc;
            return ERR_OK;
        case '!':
            advance(l);
            if (match_char(l, '=')) {
                out->kind = TOK_NEQ;
                out->loc = loc;
                return ERR_OK;
            }
            out->kind = TOK_BANG;
            out->loc = loc;
            return ERR_OK;
        case '?':
            advance(l);
            out->kind = TOK_QMARK;
            out->loc = loc;
            return ERR_OK;
        case '<':
            advance(l);
            if (match_char(l, '='))
                out->kind = TOK_LEQ;
            else if (match_char(l, '-'))
                out->kind = TOK_ASSIGN; /* <- */
            else
                out->kind = TOK_LT;
            out->loc = loc;
            return ERR_OK;
        case '>':
            advance(l);
            out->kind = match_char(l, '=') ? TOK_GEQ : TOK_GT;
            out->loc = loc;
            return ERR_OK;
        case '+':
            advance(l);
            out->kind = match_char(l, '+') ? TOK_CONCAT : TOK_PLUS;
            out->loc = loc;
            return ERR_OK;
        case ':':
            advance(l);
            out->kind = match_char(l, ':') ? TOK_APPEND : TOK_COLON;
            out->loc = loc;
            return ERR_OK;
        case '|':
            advance(l);
            out->kind = match_char(l, '>') ? TOK_PIPE : TOK_PIPE_SYM;
            out->loc = loc;
            return ERR_OK;
        case '.':
            advance(l);
            out->kind = match_char(l, '.') ? TOK_DOTDOT : TOK_DOT;
            out->loc = loc;
            return ERR_OK;
        case '@':
            advance(l);
            out->kind = TOK_AT;
            out->loc = loc;
            return ERR_OK;
        default:
            break;
    }

    /* ── Single-character tokens ─────────────── */
    advance(l);
    out->loc = loc;
    switch (c) {
        case '(':
            out->kind = TOK_LPAREN;
            return ERR_OK;
        case ')':
            out->kind = TOK_RPAREN;
            return ERR_OK;
        case '{':
            out->kind = TOK_LBRACE;
            return ERR_OK;
        case '}':
            out->kind = TOK_RBRACE;
            return ERR_OK;
        case '[':
            out->kind = TOK_LBRACKET;
            return ERR_OK;
        case ']':
            out->kind = TOK_RBRACKET;
            return ERR_OK;
        case ',':
            out->kind = TOK_COMMA;
            return ERR_OK;
        case ';':
            out->kind = TOK_SEMICOLON;
            return ERR_OK;
        case '*':
            out->kind = TOK_STAR;
            return ERR_OK;
        case '/':
            out->kind = TOK_SLASH;
            return ERR_OK;
        case '%':
            out->kind = TOK_PERCENT;
            return ERR_OK;
        default:
            fprintf(stderr, "Lexer error at %d:%u: unexpected character '%c'\n", l->line,
                    (u32)(l->cursor - l->line_start), c);
            out->kind = TOK_ERROR;
            return ERR_SYNTAX_ERROR;
    }
}

/*=============================================================================
 * Full Lex
 *=============================================================================*/

fxsh_error_t fxsh_lex(sp_str_t source, sp_str_t filename, fxsh_token_array_t *out_tokens) {
    fxsh_lexer_t l;
    fxsh_lexer_init(&l, source, filename);
    *out_tokens = SP_NULLPTR;

    while (true) {
        fxsh_token_t tok;
        fxsh_error_t err = fxsh_lexer_next(&l, &tok);
        if (err != ERR_OK)
            return err;
        if (tok.kind == TOK_COMMENT)
            continue;

        /* Collapse consecutive newlines */
        if (tok.kind == TOK_NEWLINE) {
            u32 sz = (u32)sp_dyn_array_size(*out_tokens);
            if (sz > 0 && (*out_tokens)[sz - 1].kind == TOK_NEWLINE)
                continue;
        }

        sp_dyn_array_push(*out_tokens, tok);
        if (tok.kind == TOK_EOF)
            break;
    }
    return ERR_OK;
}
