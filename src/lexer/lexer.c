/*
 * lexer.c - fxsh lexical analyzer
 */

#include "fxsh.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Helper: Create string from pointer range */
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

static keyword_entry_t keywords[] = {
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
};

static fxsh_token_kind_t lookup_keyword(sp_str_t ident) {
    sp_carr_for(keywords, i) {
        if (sp_str_equal(ident, keywords[i].keyword)) {
            return keywords[i].kind;
        }
    }
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
 * Lexer Initialization
 *=============================================================================*/

void fxsh_lexer_init(fxsh_lexer_t *lexer, sp_str_t source, sp_str_t filename) {
    lexer->source = source;
    lexer->filename = filename;
    lexer->cursor = source.data;
    lexer->line_start = source.data;
    lexer->line = 1;
}

/*=============================================================================
 * Helper Functions
 *=============================================================================*/

static inline bool is_at_end(fxsh_lexer_t *lexer) {
    return lexer->cursor >= lexer->source.data + lexer->source.len;
}

static inline c8 peek(fxsh_lexer_t *lexer) {
    if (is_at_end(lexer))
        return '\0';
    return *lexer->cursor;
}

static inline c8 peek_next(fxsh_lexer_t *lexer) {
    if (is_at_end(lexer) || lexer->cursor + 1 >= lexer->source.data + lexer->source.len)
        return '\0';
    return *(lexer->cursor + 1);
}

static inline c8 advance(fxsh_lexer_t *lexer) {
    if (is_at_end(lexer))
        return '\0';
    return *lexer->cursor++;
}

static inline bool match(fxsh_lexer_t *lexer, c8 expected) {
    if (is_at_end(lexer) || peek(lexer) != expected)
        return false;
    lexer->cursor++;
    return true;
}

static inline void skip_whitespace(fxsh_lexer_t *lexer) {
    while (!is_at_end(lexer)) {
        c8 c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else {
            break;
        }
    }
}

static fxsh_loc_t make_loc(fxsh_lexer_t *lexer) {
    return FXSH_LOC(lexer->filename, lexer->line, (u32)(lexer->cursor - lexer->line_start) + 1);
}

static inline void advance_line(fxsh_lexer_t *lexer) {
    lexer->line++;
    lexer->line_start = lexer->cursor;
}

/*=============================================================================
 * Token Parsers
 *=============================================================================*/

static fxsh_error_t read_string(fxsh_lexer_t *lexer, fxsh_token_t *out_token) {
    fxsh_loc_t loc = make_loc(lexer);
    advance(lexer); /* consume opening " */

    const c8 *start = lexer->cursor;
    sp_dyn_array(c8) chars = SP_NULLPTR;

    while (!is_at_end(lexer) && peek(lexer) != '"') {
        if (peek(lexer) == '\\') {
            /* Handle escape sequences */
            advance(lexer);
            c8 escaped = advance(lexer);
            switch (escaped) {
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
                default:
                    sp_dyn_array_push(chars, escaped);
                    break;
            }
        } else {
            sp_dyn_array_push(chars, advance(lexer));
        }
    }

    if (is_at_end(lexer)) {
        out_token->kind = TOK_ERROR;
        sp_dyn_array_free(chars);
        return ERR_SYNTAX_ERROR;
    }

    advance(lexer); /* consume closing " */

    /* Copy to null-terminated buffer for convenience */
    c8 *buf = sp_alloc(chars ? sp_dyn_array_size(chars) + 1 : 1);
    if (chars) {
        memcpy(buf, chars, sp_dyn_array_size(chars));
        buf[sp_dyn_array_size(chars)] = '\0';
    }

    out_token->kind = TOK_STRING;
    out_token->loc = loc;
    out_token->data.str_val = (sp_str_t){.data = buf, .len = chars ? sp_dyn_array_size(chars) : 0};

    sp_dyn_array_free(chars);
    return ERR_OK;
}

static fxsh_error_t read_number(fxsh_lexer_t *lexer, fxsh_token_t *out_token) {
    fxsh_loc_t loc = make_loc(lexer);
    const c8 *start = lexer->cursor;

    /* Integer part */
    while (isdigit(peek(lexer))) {
        advance(lexer);
    }

    /* Check for decimal point */
    bool is_float = false;
    if (peek(lexer) == '.' && isdigit(peek_next(lexer))) {
        is_float = true;
        advance(lexer); /* consume . */
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
    }

    /* Check for exponent */
    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        is_float = true;
        advance(lexer);
        if (peek(lexer) == '+' || peek(lexer) == '-') {
            advance(lexer);
        }
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
    }

    sp_str_t num_str = sp_str_from_range(start, lexer->cursor);
    c8 *temp = sp_alloc(num_str.len + 1);
    memcpy(temp, num_str.data, num_str.len);
    temp[num_str.len] = '\0';

    out_token->loc = loc;
    if (is_float) {
        out_token->kind = TOK_FLOAT;
        out_token->data.float_val = strtod(temp, NULL);
    } else {
        out_token->kind = TOK_INT;
        out_token->data.int_val = strtoll(temp, NULL, 10);
    }

    sp_free(temp);
    return ERR_OK;
}

static fxsh_error_t read_identifier(fxsh_lexer_t *lexer, fxsh_token_t *out_token) {
    fxsh_loc_t loc = make_loc(lexer);
    const c8 *start = lexer->cursor;

    /* First char is alpha or underscore */
    if (isalpha(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    /* Rest can be alphanumeric or underscore */
    while (isalnum(peek(lexer)) || peek(lexer) == '_' || peek(lexer) == '\'') {
        advance(lexer);
    }

    sp_str_t ident = sp_str_from_range(start, lexer->cursor);

    /* Check if type identifier (starts with uppercase) */
    bool is_type = isupper(ident.data[0]);

    /* Lookup keyword */
    fxsh_token_kind_t kind = lookup_keyword(ident);
    if (kind == TOK_IDENT) {
        kind = is_type ? TOK_TYPE_IDENT : TOK_IDENT;
    }

    out_token->kind = kind;
    out_token->loc = loc;

    /* Copy identifier for non-keywords */
    if (kind == TOK_IDENT || kind == TOK_TYPE_IDENT) {
        c8 *buf = sp_alloc(ident.len + 1);
        memcpy(buf, ident.data, ident.len);
        buf[ident.len] = '\0';
        out_token->data.ident = (sp_str_t){.data = buf, .len = ident.len};
    }

    return ERR_OK;
}

static fxsh_error_t read_comment(fxsh_lexer_t *lexer, fxsh_token_t *out_token) {
    fxsh_loc_t loc = make_loc(lexer);
    advance(lexer); /* consume # */

    const c8 *start = lexer->cursor;
    while (!is_at_end(lexer) && peek(lexer) != '\n') {
        advance(lexer);
    }

    out_token->kind = TOK_COMMENT;
    out_token->loc = loc;
    out_token->data.str_val = sp_str_from_range(start, lexer->cursor);
    return ERR_OK;
}

/*=============================================================================
 * Main Lexer
 *=============================================================================*/

fxsh_error_t fxsh_lexer_next(fxsh_lexer_t *lexer, fxsh_token_t *out_token) {
    skip_whitespace(lexer);

    if (is_at_end(lexer)) {
        out_token->kind = TOK_EOF;
        out_token->loc = make_loc(lexer);
        return ERR_OK;
    }

    c8 c = peek(lexer);

    /* Newline */
    if (c == '\n') {
        fxsh_loc_t loc = make_loc(lexer);
        advance(lexer);
        advance_line(lexer);
        out_token->kind = TOK_NEWLINE;
        out_token->loc = loc;
        return ERR_OK;
    }

    /* Comment */
    if (c == '#') {
        return read_comment(lexer, out_token);
    }

    /* Number */
    if (isdigit(c)) {
        return read_number(lexer, out_token);
    }

    /* Identifier or keyword */
    if (isalpha(c) || c == '_') {
        return read_identifier(lexer, out_token);
    }

    /* String */
    if (c == '"') {
        return read_string(lexer, out_token);
    }

    fxsh_loc_t loc = make_loc(lexer);

    /* Type variable (starts with ') */
    if (c == '\'') {
        advance(lexer);
        const c8 *start = lexer->cursor;
        while (isalpha(peek(lexer))) {
            advance(lexer);
        }
        sp_str_t ident = sp_str_from_range(start, lexer->cursor);
        c8 *buf = sp_alloc(ident.len + 1);
        memcpy(buf, ident.data, ident.len);
        buf[ident.len] = '\0';
        out_token->kind = TOK_IDENT;
        out_token->loc = loc;
        out_token->data.ident = (sp_str_t){.data = buf, .len = ident.len};
        return ERR_OK;
    }

    /* Multi-character operators */
    switch (c) {
        case '-':
            advance(lexer);
            if (match(lexer, '>')) {
                out_token->kind = TOK_ARROW;
            } else {
                out_token->kind = TOK_MINUS;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '=':
            advance(lexer);
            if (match(lexer, '>')) {
                out_token->kind = TOK_FAT_ARROW;
            } else if (match(lexer, '=')) {
                out_token->kind = TOK_EQ;
            } else {
                out_token->kind = TOK_ASSIGN;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '!':
            advance(lexer);
            if (match(lexer, '=')) {
                out_token->kind = TOK_NEQ;
            } else {
                out_token->kind = TOK_ERROR;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '<':
            advance(lexer);
            if (match(lexer, '=')) {
                out_token->kind = TOK_LEQ;
            } else if (match(lexer, '-')) {
                out_token->kind = TOK_ASSIGN; /* <- also assignment */
            } else {
                out_token->kind = TOK_LT;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '>':
            advance(lexer);
            if (match(lexer, '=')) {
                out_token->kind = TOK_GEQ;
            } else {
                out_token->kind = TOK_GT;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '+':
            advance(lexer);
            if (match(lexer, '+')) {
                out_token->kind = TOK_CONCAT;
            } else {
                out_token->kind = TOK_PLUS;
            }
            out_token->loc = loc;
            return ERR_OK;

        case ':':
            advance(lexer);
            if (match(lexer, ':')) {
                out_token->kind = TOK_APPEND;
            } else {
                out_token->kind = TOK_COLON;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '|':
            advance(lexer);
            if (match(lexer, '>')) {
                out_token->kind = TOK_PIPE;
            } else {
                out_token->kind = TOK_PIPE_SYM;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '.':
            advance(lexer);
            if (match(lexer, '.')) {
                out_token->kind = TOK_DOTDOT;
            } else {
                out_token->kind = TOK_DOT;
            }
            out_token->loc = loc;
            return ERR_OK;

        case '@':
            advance(lexer);
            out_token->kind = TOK_AT;
            out_token->loc = loc;
            return ERR_OK;
    }

    /* Single character tokens */
    advance(lexer);
    out_token->loc = loc;

    switch (c) {
        case '(':
            out_token->kind = TOK_LPAREN;
            return ERR_OK;
        case ')':
            out_token->kind = TOK_RPAREN;
            return ERR_OK;
        case '{':
            out_token->kind = TOK_LBRACE;
            return ERR_OK;
        case '}':
            out_token->kind = TOK_RBRACE;
            return ERR_OK;
        case '[':
            out_token->kind = TOK_LBRACKET;
            return ERR_OK;
        case ']':
            out_token->kind = TOK_RBRACKET;
            return ERR_OK;
        case ',':
            out_token->kind = TOK_COMMA;
            return ERR_OK;
        case ';':
            out_token->kind = TOK_SEMICOLON;
            return ERR_OK;
        case '*':
            out_token->kind = TOK_STAR;
            return ERR_OK;
        case '/':
            out_token->kind = TOK_SLASH;
            return ERR_OK;
        case '%':
            out_token->kind = TOK_PERCENT;
            return ERR_OK;
        default:
            out_token->kind = TOK_ERROR;
            return ERR_SYNTAX_ERROR;
    }
}

/*=============================================================================
 * Full Lex
 *=============================================================================*/

fxsh_error_t fxsh_lex(sp_str_t source, sp_str_t filename, fxsh_token_array_t *out_tokens) {
    fxsh_lexer_t lexer;
    fxsh_lexer_init(&lexer, source, filename);

    *out_tokens = SP_NULLPTR;

    while (true) {
        fxsh_token_t token;
        fxsh_error_t err = fxsh_lexer_next(&lexer, &token);
        if (err != ERR_OK) {
            return err;
        }

        /* Skip comments and excess newlines */
        if (token.kind == TOK_COMMENT) {
            continue;
        }

        /* Normalize newlines: collapse multiple newlines into one */
        if (token.kind == TOK_NEWLINE) {
            if (sp_dyn_array_size(*out_tokens) > 0) {
                fxsh_token_t *last = &(*out_tokens)[sp_dyn_array_size(*out_tokens) - 1];
                if (last->kind == TOK_NEWLINE) {
                    continue; /* Skip consecutive newlines */
                }
            }
        }

        sp_dyn_array_push(*out_tokens, token);

        if (token.kind == TOK_EOF) {
            break;
        }
    }

    return ERR_OK;
}
