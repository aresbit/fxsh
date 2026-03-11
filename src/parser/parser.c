/*
 * parser.c - fxsh recursive descent parser
 */

#include "fxsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*=============================================================================
 * Parser State
 *=============================================================================*/

void fxsh_parser_init(fxsh_parser_t *parser, fxsh_token_array_t tokens) {
    parser->tokens = tokens;
    parser->pos = 0;
    parser->modules = SP_NULLPTR;
    parser->imports = SP_NULLPTR;
    parser->traits = SP_NULLPTR;
    parser->had_error = false;
}

static bool parser_has_name(sp_dyn_array(sp_str_t) names, sp_str_t name) {
    sp_dyn_array_for(names, i) {
        if (sp_str_equal(names[i], name))
            return true;
    }
    return false;
}

static void parser_add_name(sp_dyn_array(sp_str_t) * names, sp_str_t name) {
    if (!parser_has_name(*names, name))
        sp_dyn_array_push(*names, name);
}

static sp_str_t mk_qualified_name(sp_str_t mod, sp_str_t name) {
    u32 len = mod.len + 2 + name.len;
    c8 *buf = (c8 *)fxsh_alloc0(len + 1);
    memcpy(buf, mod.data, mod.len);
    buf[mod.len] = '_';
    buf[mod.len + 1] = '_';
    memcpy(buf + mod.len + 2, name.data, name.len);
    return (sp_str_t){.data = buf, .len = len};
}

static inline bool match(fxsh_parser_t *parser, fxsh_token_kind_t kind);
static inline fxsh_token_t *consume(fxsh_parser_t *parser, fxsh_token_kind_t kind, const char *msg);
static inline fxsh_token_t *consume_name_token(fxsh_parser_t *parser, const char *msg);
static bool token_is_ident_text(fxsh_token_t *tok, const char *text);
static fxsh_ast_node_t *alloc_node(fxsh_ast_kind_t kind, fxsh_loc_t loc);

typedef struct {
    sp_str_t name;
    s32 arity;
} ctor_arity_entry_t;

static sp_str_t join_qualified_segments(sp_dyn_array(sp_str_t) segments, u32 count) {
    if (!segments || count == 0)
        return (sp_str_t){0};

    sp_str_t qualified = segments[0];
    for (u32 i = 1; i < count; i++)
        qualified = mk_qualified_name(qualified, segments[i]);
    return qualified;
}

static bool is_upper_name(sp_str_t name) {
    return name.len > 0 && name.data[0] >= 'A' && name.data[0] <= 'Z';
}

static bool collect_dotted_expr_segments(fxsh_ast_node_t *expr, sp_dyn_array(sp_str_t) * segments) {
    if (!expr || !segments)
        return false;

    if (expr->kind == AST_IDENT) {
        sp_dyn_array_push(*segments, expr->data.ident);
        return true;
    }

    if (expr->kind == AST_CONSTR_APPL && sp_dyn_array_size(expr->data.constr_appl.args) == 0) {
        sp_dyn_array_push(*segments, expr->data.constr_appl.constr_name);
        return true;
    }

    if (expr->kind != AST_FIELD_ACCESS)
        return false;

    if (!collect_dotted_expr_segments(expr->data.field.object, segments))
        return false;
    sp_dyn_array_push(*segments, expr->data.field.field);
    return true;
}

static fxsh_ast_node_t *rewrite_module_chain_expr(fxsh_parser_t *parser, fxsh_ast_node_t *expr) {
    sp_dyn_array(sp_str_t) segments = SP_NULLPTR;
    if (!collect_dotted_expr_segments(expr, &segments))
        return expr;

    u32 count = (u32)sp_dyn_array_size(segments);
    if (count < 2) {
        sp_dyn_array_free(segments);
        return expr;
    }

    for (u32 prefix_len = count - 1; prefix_len > 0; prefix_len--) {
        sp_str_t module_name = join_qualified_segments(segments, prefix_len);
        if (!(parser_has_name(parser->modules, module_name) ||
              parser_has_name(parser->imports, module_name))) {
            continue;
        }

        sp_str_t qualified = module_name;
        for (u32 i = prefix_len; i < count; i++)
            qualified = mk_qualified_name(qualified, segments[i]);

        fxsh_ast_node_t *rewritten = NULL;
        if (is_upper_name(segments[count - 1])) {
            rewritten = alloc_node(AST_CONSTR_APPL, expr->loc);
            rewritten->data.constr_appl.constr_name = qualified;
            rewritten->data.constr_appl.args = SP_NULLPTR;
        } else {
            rewritten = fxsh_ast_ident(qualified, expr->loc);
        }

        sp_dyn_array_free(segments);
        return rewritten;
    }

    sp_dyn_array_free(segments);
    return expr;
}

static sp_str_t parse_qualified_name_path(fxsh_parser_t *parser, const char *msg) {
    fxsh_token_t *name_tok = consume_name_token(parser, msg);
    if (!name_tok)
        return (sp_str_t){0};

    sp_dyn_array(sp_str_t) segments = SP_NULLPTR;
    sp_dyn_array_push(segments, name_tok->data.ident);
    while (match(parser, TOK_DOT)) {
        fxsh_token_t *part_tok = consume_name_token(parser, msg);
        if (!part_tok)
            return (sp_str_t){0};
        sp_dyn_array_push(segments, part_tok->data.ident);
    }

    sp_str_t qualified = join_qualified_segments(segments, (u32)sp_dyn_array_size(segments));
    sp_dyn_array_free(segments);
    return qualified;
}

static fxsh_ast_node_t *clone_type_ast(fxsh_ast_node_t *type_ast) {
    if (!type_ast)
        return NULL;

    fxsh_ast_node_t *copy = alloc_node(type_ast->kind, type_ast->loc);
    switch (type_ast->kind) {
        case AST_IDENT:
        case AST_TYPE_VAR:
            copy->data.ident = type_ast->data.ident;
            return copy;
        case AST_TYPE_ARROW:
            copy->data.type_arrow.param = clone_type_ast(type_ast->data.type_arrow.param);
            copy->data.type_arrow.ret = clone_type_ast(type_ast->data.type_arrow.ret);
            return copy;
        case AST_TYPE_APP:
            copy->data.type_con.name = type_ast->data.type_con.name;
            copy->data.type_con.args = SP_NULLPTR;
            sp_dyn_array_for(type_ast->data.type_con.args, i) {
                sp_dyn_array_push(copy->data.type_con.args,
                                  clone_type_ast(type_ast->data.type_con.args[i]));
            }
            return copy;
        case AST_TYPE_RECORD:
            copy->data.elements = SP_NULLPTR;
            sp_dyn_array_for(type_ast->data.elements, i) {
                fxsh_ast_node_t *field = type_ast->data.elements[i];
                if (!field || field->kind != AST_FIELD_ACCESS)
                    continue;
                fxsh_ast_node_t *field_copy = alloc_node(AST_FIELD_ACCESS, field->loc);
                field_copy->data.field.field = field->data.field.field;
                field_copy->data.field.object = clone_type_ast(field->data.field.object);
                field_copy->data.field.type = NULL;
                sp_dyn_array_push(copy->data.elements, field_copy);
            }
            return copy;
        default:
            return copy;
    }
}

static s32 ctor_arity_lookup(sp_dyn_array(ctor_arity_entry_t) entries, sp_str_t name) {
    sp_dyn_array_for(entries, i) {
        if (sp_str_equal(entries[i].name, name))
            return entries[i].arity;
    }
    return -1;
}

static void normalize_constructor_tuple_sugar_pattern(fxsh_ast_node_t *ast,
                                                      sp_dyn_array(ctor_arity_entry_t) entries);

static void normalize_constructor_tuple_sugar_expr(fxsh_ast_node_t *ast,
                                                   sp_dyn_array(ctor_arity_entry_t) entries) {
    if (!ast)
        return;
    switch (ast->kind) {
        case AST_PROGRAM:
            sp_dyn_array_for(ast->data.decls, i)
                normalize_constructor_tuple_sugar_expr(ast->data.decls[i], entries);
            return;
        case AST_BINARY:
            normalize_constructor_tuple_sugar_expr(ast->data.binary.left, entries);
            normalize_constructor_tuple_sugar_expr(ast->data.binary.right, entries);
            return;
        case AST_UNARY:
            normalize_constructor_tuple_sugar_expr(ast->data.unary.operand, entries);
            return;
        case AST_CALL:
            normalize_constructor_tuple_sugar_expr(ast->data.call.func, entries);
            sp_dyn_array_for(ast->data.call.args, i)
                normalize_constructor_tuple_sugar_expr(ast->data.call.args[i], entries);
            return;
        case AST_LAMBDA:
            sp_dyn_array_for(ast->data.lambda.params, i)
                normalize_constructor_tuple_sugar_pattern(ast->data.lambda.params[i], entries);
            normalize_constructor_tuple_sugar_expr(ast->data.lambda.body, entries);
            return;
        case AST_LET:
        case AST_DECL_LET:
            normalize_constructor_tuple_sugar_pattern(ast->data.let.pattern, entries);
            normalize_constructor_tuple_sugar_expr(ast->data.let.value, entries);
            return;
        case AST_LET_IN:
            sp_dyn_array_for(ast->data.let_in.bindings, i)
                normalize_constructor_tuple_sugar_expr(ast->data.let_in.bindings[i], entries);
            normalize_constructor_tuple_sugar_expr(ast->data.let_in.body, entries);
            return;
        case AST_IF:
            normalize_constructor_tuple_sugar_expr(ast->data.if_expr.cond, entries);
            normalize_constructor_tuple_sugar_expr(ast->data.if_expr.then_branch, entries);
            normalize_constructor_tuple_sugar_expr(ast->data.if_expr.else_branch, entries);
            return;
        case AST_MATCH:
            normalize_constructor_tuple_sugar_expr(ast->data.match_expr.expr, entries);
            sp_dyn_array_for(ast->data.match_expr.arms, i)
                normalize_constructor_tuple_sugar_expr(ast->data.match_expr.arms[i], entries);
            return;
        case AST_MATCH_ARM:
            normalize_constructor_tuple_sugar_pattern(ast->data.match_arm.pattern, entries);
            normalize_constructor_tuple_sugar_expr(ast->data.match_arm.guard, entries);
            normalize_constructor_tuple_sugar_expr(ast->data.match_arm.body, entries);
            return;
        case AST_PIPE:
            normalize_constructor_tuple_sugar_expr(ast->data.pipe.left, entries);
            normalize_constructor_tuple_sugar_expr(ast->data.pipe.right, entries);
            return;
        case AST_TUPLE:
        case AST_LIST:
        case AST_RECORD:
        case AST_PAT_TUPLE:
        case AST_PAT_RECORD:
        case AST_PAT_CONS:
            sp_dyn_array_for(ast->data.elements, i)
                normalize_constructor_tuple_sugar_expr(ast->data.elements[i], entries);
            return;
        case AST_RECORD_UPDATE:
            normalize_constructor_tuple_sugar_expr(ast->data.record_update.base, entries);
            sp_dyn_array_for(ast->data.record_update.updates, i)
                normalize_constructor_tuple_sugar_expr(ast->data.record_update.updates[i], entries);
            return;
        case AST_FIELD_ACCESS:
            normalize_constructor_tuple_sugar_expr(ast->data.field.object, entries);
            return;
        case AST_CONSTR_APPL: {
            sp_dyn_array_for(ast->data.constr_appl.args, i)
                normalize_constructor_tuple_sugar_expr(ast->data.constr_appl.args[i], entries);
            s32 arity = ctor_arity_lookup(entries, ast->data.constr_appl.constr_name);
            if (arity > 1 && sp_dyn_array_size(ast->data.constr_appl.args) == 1 &&
                ast->data.constr_appl.args[0] && ast->data.constr_appl.args[0]->kind == AST_TUPLE &&
                sp_dyn_array_size(ast->data.constr_appl.args[0]->data.elements) == (u32)arity) {
                fxsh_ast_node_t *tuple_arg = ast->data.constr_appl.args[0];
                ast->data.constr_appl.args = tuple_arg->data.elements;
                tuple_arg->data.elements = SP_NULLPTR;
                sp_free(tuple_arg);
            }
            return;
        }
        default:
            return;
    }
}

static void normalize_constructor_tuple_sugar_pattern(fxsh_ast_node_t *ast,
                                                      sp_dyn_array(ctor_arity_entry_t) entries) {
    if (!ast)
        return;
    switch (ast->kind) {
        case AST_PAT_TUPLE:
        case AST_PAT_RECORD:
        case AST_PAT_CONS:
            sp_dyn_array_for(ast->data.elements, i)
                normalize_constructor_tuple_sugar_pattern(ast->data.elements[i], entries);
            return;
        case AST_PAT_CONSTR:
            sp_dyn_array_for(ast->data.constr_appl.args, i)
                normalize_constructor_tuple_sugar_pattern(ast->data.constr_appl.args[i], entries);
            {
                s32 arity = ctor_arity_lookup(entries, ast->data.constr_appl.constr_name);
                if (arity > 1 && sp_dyn_array_size(ast->data.constr_appl.args) == 1 &&
                    ast->data.constr_appl.args[0] &&
                    ast->data.constr_appl.args[0]->kind == AST_PAT_TUPLE &&
                    sp_dyn_array_size(ast->data.constr_appl.args[0]->data.elements) == (u32)arity) {
                    fxsh_ast_node_t *tuple_arg = ast->data.constr_appl.args[0];
                    ast->data.constr_appl.args = tuple_arg->data.elements;
                    tuple_arg->data.elements = SP_NULLPTR;
                    sp_free(tuple_arg);
                }
            }
            return;
        case AST_FIELD_ACCESS:
            normalize_constructor_tuple_sugar_pattern(ast->data.field.object, entries);
            return;
        default:
            return;
    }
}

static void collect_constructor_arities(fxsh_ast_node_t *ast,
                                        sp_dyn_array(ctor_arity_entry_t) * entries) {
    if (!ast || !entries)
        return;
    if (ast->kind == AST_PROGRAM) {
        sp_dyn_array_for(ast->data.decls, i)
            collect_constructor_arities(ast->data.decls[i], entries);
        return;
    }
    if (ast->kind != AST_TYPE_DEF)
        return;
    sp_dyn_array_for(ast->data.type_def.constructors, i) {
        fxsh_ast_node_t *c = ast->data.type_def.constructors[i];
        if (!c || c->kind != AST_DATA_CONSTR)
            continue;
        ctor_arity_entry_t e = {.name = c->data.data_constr.name,
                                .arity = (s32)sp_dyn_array_size(c->data.data_constr.arg_types)};
        sp_dyn_array_push(*entries, e);
    }
}

static void normalize_constructor_tuple_sugar(fxsh_ast_node_t *ast) {
    sp_dyn_array(ctor_arity_entry_t) entries = SP_NULLPTR;
    collect_constructor_arities(ast, &entries);
    normalize_constructor_tuple_sugar_expr(ast, entries);
    sp_dyn_array_free(entries);
}

static fxsh_ast_node_t *substitute_self_type_ast(fxsh_ast_node_t *type_ast,
                                                 fxsh_ast_node_t *self_type_ast) {
    if (!type_ast)
        return NULL;

    if (type_ast->kind == AST_IDENT &&
        sp_str_equal(type_ast->data.ident, (sp_str_t){.data = "Self", .len = 4})) {
        return clone_type_ast(self_type_ast);
    }

    fxsh_ast_node_t *copy = alloc_node(type_ast->kind, type_ast->loc);
    switch (type_ast->kind) {
        case AST_IDENT:
        case AST_TYPE_VAR:
            copy->data.ident = type_ast->data.ident;
            return copy;
        case AST_TYPE_ARROW:
            copy->data.type_arrow.param =
                substitute_self_type_ast(type_ast->data.type_arrow.param, self_type_ast);
            copy->data.type_arrow.ret =
                substitute_self_type_ast(type_ast->data.type_arrow.ret, self_type_ast);
            return copy;
        case AST_TYPE_APP:
            copy->data.type_con.name = type_ast->data.type_con.name;
            copy->data.type_con.args = SP_NULLPTR;
            sp_dyn_array_for(type_ast->data.type_con.args, i) {
                sp_dyn_array_push(
                    copy->data.type_con.args,
                    substitute_self_type_ast(type_ast->data.type_con.args[i], self_type_ast));
            }
            return copy;
        case AST_TYPE_RECORD:
            copy->data.elements = SP_NULLPTR;
            sp_dyn_array_for(type_ast->data.elements, i) {
                fxsh_ast_node_t *field = type_ast->data.elements[i];
                if (!field || field->kind != AST_FIELD_ACCESS)
                    continue;
                fxsh_ast_node_t *field_copy = alloc_node(AST_FIELD_ACCESS, field->loc);
                field_copy->data.field.field = field->data.field.field;
                field_copy->data.field.object =
                    substitute_self_type_ast(field->data.field.object, self_type_ast);
                field_copy->data.field.type = NULL;
                sp_dyn_array_push(copy->data.elements, field_copy);
            }
            return copy;
        default:
            return copy;
    }
}

static fxsh_trait_decl_t *parser_lookup_trait(fxsh_parser_t *parser, sp_str_t name) {
    if (!parser)
        return NULL;
    sp_dyn_array_for(parser->traits, i) {
        if (sp_str_equal(parser->traits[i].name, name))
            return &parser->traits[i];
    }
    return NULL;
}

static fxsh_ast_node_t *trait_lookup_method_sig(fxsh_trait_decl_t *trait, sp_str_t name) {
    if (!trait)
        return NULL;
    sp_dyn_array_for(trait->methods, i) {
        fxsh_ast_node_t *method = trait->methods[i];
        if (method && (method->kind == AST_DECL_LET || method->kind == AST_LET) &&
            sp_str_equal(method->data.let.name, name)) {
            return method;
        }
    }
    return NULL;
}

static void parser_register_trait(fxsh_parser_t *parser, sp_str_t name, fxsh_ast_list_t methods) {
    if (!parser)
        return;
    sp_dyn_array_for(parser->traits, i) {
        if (sp_str_equal(parser->traits[i].name, name)) {
            parser->traits[i].methods = methods;
            return;
        }
    }
    fxsh_trait_decl_t trait = {.name = name, .methods = methods};
    sp_dyn_array_push(parser->traits, trait);
}

static bool trait_has_method_name(fxsh_ast_list_t methods, sp_str_t name) {
    sp_dyn_array_for(methods, i) {
        fxsh_ast_node_t *method = methods[i];
        if (method && (method->kind == AST_DECL_LET || method->kind == AST_LET) &&
            sp_str_equal(method->data.let.name, name)) {
            return true;
        }
    }
    return false;
}

static bool parse_brace_or_struct_body_start(fxsh_parser_t *parser, const char *what,
                                             bool *brace_body) {
    if (match(parser, TOK_STRUCT)) {
        *brace_body = false;
        return true;
    }
    if (match(parser, TOK_LBRACE)) {
        *brace_body = true;
        return true;
    }
    consume(parser, TOK_STRUCT, what);
    return false;
}

static void parse_brace_or_struct_body_end(fxsh_parser_t *parser, bool brace_body) {
    if (brace_body) {
        consume(parser, TOK_RBRACE, "'}'");
    } else {
        consume(parser, TOK_END, "'end'");
    }
}

static inline fxsh_token_t *current(fxsh_parser_t *parser) {
    if (parser->pos >= sp_dyn_array_size(parser->tokens)) {
        return &parser->tokens[sp_dyn_array_size(parser->tokens) - 1];
    }
    return &parser->tokens[parser->pos];
}

static inline fxsh_token_t *peek(fxsh_parser_t *parser, u32 offset) {
    u32 idx = parser->pos + offset;
    if (idx >= sp_dyn_array_size(parser->tokens)) {
        return &parser->tokens[sp_dyn_array_size(parser->tokens) - 1];
    }
    return &parser->tokens[idx];
}

static inline bool check(fxsh_parser_t *parser, fxsh_token_kind_t kind) {
    return current(parser)->kind == kind;
}

static inline bool check_any(fxsh_parser_t *parser, fxsh_token_kind_t *kinds, u32 count) {
    for (u32 i = 0; i < count; i++) {
        if (check(parser, kinds[i]))
            return true;
    }
    return false;
}

static inline fxsh_token_t *advance(fxsh_parser_t *parser) {
    fxsh_token_t *tok = current(parser);
    if (parser->pos < sp_dyn_array_size(parser->tokens) - 1) {
        parser->pos++;
    }
    return tok;
}

static inline bool match(fxsh_parser_t *parser, fxsh_token_kind_t kind) {
    if (check(parser, kind)) {
        advance(parser);
        return true;
    }
    return false;
}

static inline fxsh_token_t *consume(fxsh_parser_t *parser, fxsh_token_kind_t kind,
                                    const char *msg) {
    if (check(parser, kind)) {
        return advance(parser);
    }
    fprintf(stderr, "Parse error at %d:%d: expected %s, got %s\n", current(parser)->loc.line,
            current(parser)->loc.column, msg, fxsh_token_kind_name(current(parser)->kind));
    parser->had_error = true;
    return NULL;
}

static inline fxsh_token_t *consume_name_token(fxsh_parser_t *parser, const char *msg) {
    if (check(parser, TOK_IDENT) || check(parser, TOK_TYPE_IDENT)) {
        return advance(parser);
    }
    fprintf(stderr, "Parse error at %d:%d: expected %s, got %s\n", current(parser)->loc.line,
            current(parser)->loc.column, msg, fxsh_token_kind_name(current(parser)->kind));
    parser->had_error = true;
    return NULL;
}

static bool token_is_ident_text(fxsh_token_t *tok, const char *text) {
    if (!tok || tok->kind != TOK_IDENT || !text)
        return false;
    size_t n = strlen(text);
    return tok->data.ident.len == n && memcmp(tok->data.ident.data, text, n) == 0;
}

static void parser_error_at(fxsh_parser_t *parser, fxsh_loc_t loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Parse error at %d:%d: ", loc.line, loc.column);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    parser->had_error = true;
}

static inline void skip_newlines(fxsh_parser_t *parser) {
    while (check(parser, TOK_NEWLINE)) {
        advance(parser);
    }
}

/* Simple panic-mode recovery:
 * skip to newline, or to a likely declaration boundary. */
static void synchronize(fxsh_parser_t *parser) {
    while (!check(parser, TOK_EOF)) {
        if (check(parser, TOK_NEWLINE)) {
            advance(parser);
            return;
        }
        if (check(parser, TOK_LET) || check(parser, TOK_TYPE) || check(parser, TOK_MODULE) ||
            check(parser, TOK_IMPORT)) {
            return;
        }
        advance(parser);
    }
}

/* Consume optional block terminator without stealing next declaration line.
 * Accepts:
 *   - end
 *   - NEWLINE end
 */
static inline void maybe_consume_block_end(fxsh_parser_t *parser) {
    if (check(parser, TOK_END)) {
        advance(parser);
        return;
    }
    if (check(parser, TOK_NEWLINE) && peek(parser, 1)->kind == TOK_END) {
        advance(parser); /* newline */
        advance(parser); /* end */
    }
}

/*=============================================================================
 * AST Constructors
 *=============================================================================*/

static fxsh_ast_node_t *alloc_node(fxsh_ast_kind_t kind, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = sp_alloc(sizeof(fxsh_ast_node_t));
    node->kind = kind;
    node->loc = loc;
    return node;
}

fxsh_ast_node_t *fxsh_ast_lit_int(s64 val, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_LIT_INT, loc);
    node->data.lit_int = val;
    return node;
}

fxsh_ast_node_t *fxsh_ast_lit_float(f64 val, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_LIT_FLOAT, loc);
    node->data.lit_float = val;
    return node;
}

fxsh_ast_node_t *fxsh_ast_lit_string(sp_str_t val, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_LIT_STRING, loc);
    node->data.lit_string = val;
    return node;
}

fxsh_ast_node_t *fxsh_ast_lit_bool(bool val, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_LIT_BOOL, loc);
    node->data.lit_bool = val;
    return node;
}

fxsh_ast_node_t *fxsh_ast_ident(sp_str_t name, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_IDENT, loc);
    node->data.ident = name;
    return node;
}

fxsh_ast_node_t *fxsh_ast_binary(fxsh_token_kind_t op, fxsh_ast_node_t *left,
                                 fxsh_ast_node_t *right, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_BINARY, loc);
    node->data.binary.op = op;
    node->data.binary.left = left;
    node->data.binary.right = right;
    return node;
}

fxsh_ast_node_t *fxsh_ast_lambda(fxsh_ast_list_t params, fxsh_ast_node_t *body, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_LAMBDA, loc);
    node->data.lambda.params = params;
    node->data.lambda.body = body;
    node->data.lambda.ret_type = (sp_str_t){0};
    return node;
}

fxsh_ast_node_t *fxsh_ast_let(sp_str_t name, fxsh_ast_node_t *value, bool is_comptime, bool is_rec,
                              fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_DECL_LET, loc);
    node->data.let.name = name;
    node->data.let.pattern = alloc_node(AST_PAT_VAR, loc);
    node->data.let.pattern->data.ident = name;
    node->data.let.type = NULL;
    node->data.let.value = value;
    node->data.let.is_comptime = is_comptime;
    node->data.let.is_rec = is_rec;
    return node;
}

fxsh_ast_node_t *fxsh_ast_if(fxsh_ast_node_t *cond, fxsh_ast_node_t *then_branch,
                             fxsh_ast_node_t *else_branch, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_IF, loc);
    node->data.if_expr.cond = cond;
    node->data.if_expr.then_branch = then_branch;
    node->data.if_expr.else_branch = else_branch;
    return node;
}

fxsh_ast_node_t *fxsh_ast_call(fxsh_ast_node_t *func, fxsh_ast_list_t args, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_CALL, loc);
    node->data.call.func = func;
    node->data.call.args = args;
    return node;
}

fxsh_ast_node_t *fxsh_ast_program(fxsh_ast_list_t decls, fxsh_loc_t loc) {
    fxsh_ast_node_t *node = alloc_node(AST_PROGRAM, loc);
    node->data.decls = decls;
    return node;
}

/*=============================================================================
 * AST Destructor
 *=============================================================================*/

void fxsh_ast_free(fxsh_ast_node_t *node) {
    if (!node)
        return;

    switch (node->kind) {
        case AST_BINARY:
            fxsh_ast_free(node->data.binary.left);
            fxsh_ast_free(node->data.binary.right);
            break;
        case AST_UNARY:
            fxsh_ast_free(node->data.unary.operand);
            break;
        case AST_CALL:
            fxsh_ast_free(node->data.call.func);
            sp_dyn_array_for(node->data.call.args, i) {
                fxsh_ast_free(node->data.call.args[i]);
            }
            sp_dyn_array_free(node->data.call.args);
            break;
        case AST_LAMBDA:
            sp_dyn_array_for(node->data.lambda.params, i) {
                fxsh_ast_free(node->data.lambda.params[i]);
            }
            sp_dyn_array_free(node->data.lambda.params);
            fxsh_ast_free(node->data.lambda.body);
            break;
        case AST_LET_IN:
            sp_dyn_array_for(node->data.let_in.bindings, i) {
                fxsh_ast_free(node->data.let_in.bindings[i]);
            }
            sp_dyn_array_free(node->data.let_in.bindings);
            fxsh_ast_free(node->data.let_in.body);
            break;
        case AST_IF:
            fxsh_ast_free(node->data.if_expr.cond);
            fxsh_ast_free(node->data.if_expr.then_branch);
            fxsh_ast_free(node->data.if_expr.else_branch);
            break;
        case AST_MATCH:
            fxsh_ast_free(node->data.match_expr.expr);
            sp_dyn_array_for(node->data.match_expr.arms, i) {
                fxsh_ast_free(node->data.match_expr.arms[i]);
            }
            sp_dyn_array_free(node->data.match_expr.arms);
            break;
        case AST_MATCH_ARM:
            fxsh_ast_free(node->data.match_arm.pattern);
            fxsh_ast_free(node->data.match_arm.guard);
            fxsh_ast_free(node->data.match_arm.body);
            break;
        case AST_PIPE:
            fxsh_ast_free(node->data.pipe.left);
            fxsh_ast_free(node->data.pipe.right);
            break;
        case AST_TUPLE:
        case AST_LIST:
        case AST_PAT_TUPLE:
        case AST_PAT_RECORD:
        case AST_TYPE_RECORD:
            sp_dyn_array_for(node->data.elements, i) {
                fxsh_ast_free(node->data.elements[i]);
            }
            sp_dyn_array_free(node->data.elements);
            break;
        case AST_FIELD_ACCESS:
            fxsh_ast_free(node->data.field.object);
            fxsh_ast_free(node->data.field.type);
            break;
        case AST_RECORD_UPDATE:
            fxsh_ast_free(node->data.record_update.base);
            sp_dyn_array_for(node->data.record_update.updates, i) {
                fxsh_ast_free(node->data.record_update.updates[i]);
            }
            sp_dyn_array_free(node->data.record_update.updates);
            break;
        case AST_TYPE_ARROW:
            fxsh_ast_free(node->data.type_arrow.param);
            fxsh_ast_free(node->data.type_arrow.ret);
            break;
        case AST_TYPE_APP:
            sp_dyn_array_for(node->data.type_con.args, i) {
                fxsh_ast_free(node->data.type_con.args[i]);
            }
            sp_dyn_array_free(node->data.type_con.args);
            break;
        case AST_TYPE_VALUE:
            break;
        case AST_CT_TYPE_OF:
        case AST_CT_TYPE_NAME:
        case AST_CT_QUOTE:
        case AST_CT_UNQUOTE:
        case AST_CT_SPLICE:
        case AST_CT_EVAL:
        case AST_CT_SQL:
        case AST_CT_SQL_CHECK:
        case AST_CT_COMPILE_ERROR:
        case AST_CT_COMPILE_LOG:
        case AST_CT_PANIC:
            fxsh_ast_free(node->data.ct_type_of.operand);
            break;
        case AST_CT_SIZE_OF:
        case AST_CT_ALIGN_OF:
        case AST_CT_FIELDS_OF:
        case AST_CT_IS_RECORD:
        case AST_CT_IS_TUPLE:
        case AST_CT_JSON_SCHEMA:
            fxsh_ast_free(node->data.ct_type_op.type_expr);
            break;
        case AST_CT_HAS_FIELD:
            fxsh_ast_free(node->data.ct_has_field.type_expr);
            break;
        case AST_CT_SQLITE_SQL:
            fxsh_ast_free(node->data.ct_sqlite_sql.type_expr);
            break;
        case AST_CT_CTOR_APPLY:
            sp_dyn_array_for(node->data.ct_ctor_apply.type_args, i) {
                fxsh_ast_free(node->data.ct_ctor_apply.type_args[i]);
            }
            sp_dyn_array_free(node->data.ct_ctor_apply.type_args);
            break;
        case AST_PROGRAM:
            sp_dyn_array_for(node->data.decls, i) {
                fxsh_ast_free(node->data.decls[i]);
            }
            sp_dyn_array_free(node->data.decls);
            break;
        default:
            break;
    }

    sp_free(node);
}

/*=============================================================================
 * Forward Declarations
 *=============================================================================*/

static fxsh_ast_node_t *parse_expr(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_pattern(fxsh_parser_t *parser);

static fxsh_ast_node_t *make_pat_cons_node(fxsh_loc_t loc, fxsh_ast_node_t *head,
                                           fxsh_ast_node_t *tail) {
    fxsh_ast_node_t *node = alloc_node(AST_PAT_CONS, loc);
    node->data.elements = SP_NULLPTR;
    sp_dyn_array_push(node->data.elements, head);
    sp_dyn_array_push(node->data.elements, tail);
    return node;
}

static fxsh_ast_node_t *lower_list_pattern_to_cons(fxsh_loc_t loc, fxsh_ast_list_t elems) {
    fxsh_ast_node_t *cur = alloc_node(AST_LIST, loc);
    cur->data.elements = SP_NULLPTR;
    for (s32 i = (s32)sp_dyn_array_size(elems) - 1; i >= 0; i--) {
        cur = make_pat_cons_node(loc, elems[i], cur);
    }
    return cur;
}
static fxsh_ast_node_t *parse_pattern(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_decl(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_primary(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_app_arg_primary(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_type_expr(fxsh_parser_t *parser);
static bool is_type_expr_start(fxsh_token_kind_t kind);
typedef struct {
    sp_str_t from;
    sp_str_t to;
} name_map_t;

static sp_str_t map_lookup(sp_dyn_array(name_map_t) map, sp_str_t key) {
    sp_dyn_array_for(map, i) {
        if (sp_str_equal(map[i].from, key))
            return map[i].to;
    }
    return (sp_str_t){0};
}

static void rewrite_module_type_refs(fxsh_ast_node_t *type_ast, sp_dyn_array(name_map_t) map) {
    if (!type_ast)
        return;
    switch (type_ast->kind) {
        case AST_IDENT: {
            sp_str_t q = map_lookup(map, type_ast->data.ident);
            if (q.data)
                type_ast->data.ident = q;
            return;
        }
        case AST_TYPE_VAR:
            return;
        case AST_TYPE_ARROW:
            rewrite_module_type_refs(type_ast->data.type_arrow.param, map);
            rewrite_module_type_refs(type_ast->data.type_arrow.ret, map);
            return;
        case AST_TYPE_APP:
            sp_dyn_array_for(type_ast->data.type_con.args, i)
                rewrite_module_type_refs(type_ast->data.type_con.args[i], map);
            return;
        case AST_TYPE_RECORD:
            sp_dyn_array_for(type_ast->data.elements, i) {
                fxsh_ast_node_t *f = type_ast->data.elements[i];
                if (f && f->kind == AST_FIELD_ACCESS)
                    rewrite_module_type_refs(f->data.field.object, map);
            }
            return;
        case AST_TUPLE:
            sp_dyn_array_for(type_ast->data.elements, i)
                rewrite_module_type_refs(type_ast->data.elements[i], map);
            return;
        default:
            return;
    }
}

static void collect_pattern_bound(fxsh_ast_node_t *pat, sp_dyn_array(sp_str_t) * out) {
    if (!pat)
        return;
    switch (pat->kind) {
        case AST_PAT_VAR:
            sp_dyn_array_push(*out, pat->data.ident);
            return;
        case AST_PAT_TUPLE:
        case AST_PAT_RECORD:
        case AST_PAT_CONS:
            sp_dyn_array_for(pat->data.elements, i)
                collect_pattern_bound(pat->data.elements[i], out);
            return;
        case AST_PAT_CONSTR:
            sp_dyn_array_for(pat->data.constr_appl.args, i)
                collect_pattern_bound(pat->data.constr_appl.args[i], out);
            return;
        default:
            return;
    }
}

static bool bound_has(sp_dyn_array(sp_str_t) bound, sp_str_t name) {
    sp_dyn_array_for(bound, i) {
        if (sp_str_equal(bound[i], name))
            return true;
    }
    return false;
}

static void rewrite_module_refs(fxsh_ast_node_t *ast, sp_dyn_array(name_map_t) map,
                                sp_dyn_array(sp_str_t) * bound) {
    if (!ast)
        return;
    switch (ast->kind) {
        case AST_IDENT: {
            if (bound_has(*bound, ast->data.ident))
                return;
            sp_str_t q = map_lookup(map, ast->data.ident);
            if (q.data)
                ast->data.ident = q;
            return;
        }
        case AST_BINARY:
            rewrite_module_refs(ast->data.binary.left, map, bound);
            rewrite_module_refs(ast->data.binary.right, map, bound);
            return;
        case AST_UNARY:
            rewrite_module_refs(ast->data.unary.operand, map, bound);
            return;
        case AST_CALL:
            rewrite_module_refs(ast->data.call.func, map, bound);
            sp_dyn_array_for(ast->data.call.args, i)
                rewrite_module_refs(ast->data.call.args[i], map, bound);
            return;
        case AST_LAMBDA: {
            u32 mark = (u32)sp_dyn_array_size(*bound);
            sp_dyn_array_for(ast->data.lambda.params, i) {
                collect_pattern_bound(ast->data.lambda.params[i], bound);
            }
            rewrite_module_refs(ast->data.lambda.body, map, bound);
            while (sp_dyn_array_size(*bound) > mark)
                sp_dyn_array_pop(*bound);
            return;
        }
        case AST_LET:
        case AST_DECL_LET: {
            rewrite_module_refs(ast->data.let.value, map, bound);
            rewrite_module_type_refs(ast->data.let.type, map);
            return;
        }
        case AST_LET_IN: {
            u32 mark = (u32)sp_dyn_array_size(*bound);
            sp_dyn_array_for(ast->data.let_in.bindings, i) {
                fxsh_ast_node_t *b = ast->data.let_in.bindings[i];
                if (!b || (b->kind != AST_LET && b->kind != AST_DECL_LET))
                    continue;
                rewrite_module_refs(b->data.let.value, map, bound);
                rewrite_module_type_refs(b->data.let.type, map);
                sp_dyn_array_push(*bound, b->data.let.name);
            }
            rewrite_module_refs(ast->data.let_in.body, map, bound);
            while (sp_dyn_array_size(*bound) > mark)
                sp_dyn_array_pop(*bound);
            return;
        }
        case AST_IF:
            rewrite_module_refs(ast->data.if_expr.cond, map, bound);
            rewrite_module_refs(ast->data.if_expr.then_branch, map, bound);
            rewrite_module_refs(ast->data.if_expr.else_branch, map, bound);
            return;
        case AST_MATCH:
            rewrite_module_refs(ast->data.match_expr.expr, map, bound);
            sp_dyn_array_for(ast->data.match_expr.arms, i)
                rewrite_module_refs(ast->data.match_expr.arms[i], map, bound);
            return;
        case AST_MATCH_ARM: {
            u32 mark = (u32)sp_dyn_array_size(*bound);
            rewrite_module_refs(ast->data.match_arm.pattern, map, bound);
            collect_pattern_bound(ast->data.match_arm.pattern, bound);
            rewrite_module_refs(ast->data.match_arm.guard, map, bound);
            rewrite_module_refs(ast->data.match_arm.body, map, bound);
            while (sp_dyn_array_size(*bound) > mark)
                sp_dyn_array_pop(*bound);
            return;
        }
        case AST_PIPE:
            rewrite_module_refs(ast->data.pipe.left, map, bound);
            rewrite_module_refs(ast->data.pipe.right, map, bound);
            return;
        case AST_TUPLE:
        case AST_LIST:
        case AST_RECORD:
        case AST_PAT_TUPLE:
        case AST_PAT_RECORD:
        case AST_PAT_CONS:
            sp_dyn_array_for(ast->data.elements, i)
                rewrite_module_refs(ast->data.elements[i], map, bound);
            return;
        case AST_RECORD_UPDATE:
            rewrite_module_refs(ast->data.record_update.base, map, bound);
            sp_dyn_array_for(ast->data.record_update.updates, i)
                rewrite_module_refs(ast->data.record_update.updates[i], map, bound);
            return;
        case AST_FIELD_ACCESS:
            rewrite_module_refs(ast->data.field.object, map, bound);
            rewrite_module_type_refs(ast->data.field.type, map);
            return;
        case AST_CONSTR_APPL:
            if (!bound_has(*bound, ast->data.constr_appl.constr_name)) {
                sp_str_t q = map_lookup(map, ast->data.constr_appl.constr_name);
                if (q.data)
                    ast->data.constr_appl.constr_name = q;
            }
            sp_dyn_array_for(ast->data.constr_appl.args, i)
                rewrite_module_refs(ast->data.constr_appl.args[i], map, bound);
            return;
        case AST_PAT_CONSTR:
            if (!bound_has(*bound, ast->data.constr_appl.constr_name)) {
                sp_str_t q = map_lookup(map, ast->data.constr_appl.constr_name);
                if (q.data)
                    ast->data.constr_appl.constr_name = q;
            }
            sp_dyn_array_for(ast->data.constr_appl.args, i)
                rewrite_module_refs(ast->data.constr_appl.args[i], map, bound);
            return;
        default:
            return;
    }
}

/*=============================================================================
 * Pattern Parsing
 *=============================================================================*/

static bool is_pattern_start(fxsh_token_kind_t kind) {
    return kind == TOK_IDENT || kind == TOK_TYPE_IDENT || kind == TOK_INT || kind == TOK_FLOAT ||
           kind == TOK_STRING || kind == TOK_TRUE || kind == TOK_FALSE || kind == TOK_LPAREN ||
           kind == TOK_LBRACKET || kind == TOK_LBRACE;
}

static bool is_type_expr_start(fxsh_token_kind_t kind) {
    return kind == TOK_IDENT || kind == TOK_TYPE_IDENT || kind == TOK_LPAREN || kind == TOK_LBRACE;
}

static fxsh_ast_node_t *parse_type_atom(fxsh_parser_t *parser) {
    fxsh_token_t *tok = current(parser);
    if (tok->kind == TOK_LPAREN) {
        advance(parser); /* consume '(' */
        fxsh_ast_node_t *first = parse_type_expr(parser);
        skip_newlines(parser);
        if (match(parser, TOK_COMMA)) {
            fxsh_ast_list_t elems = SP_NULLPTR;
            sp_dyn_array_push(elems, first);
            skip_newlines(parser);
            do {
                fxsh_ast_node_t *next = parse_type_expr(parser);
                if (!next)
                    return NULL;
                sp_dyn_array_push(elems, next);
                skip_newlines(parser);
            } while (match(parser, TOK_COMMA));
            consume(parser, TOK_RPAREN, "')'");
            fxsh_ast_node_t *tuple = alloc_node(AST_TUPLE, tok->loc);
            tuple->data.elements = elems;
            return tuple;
        }
        consume(parser, TOK_RPAREN, "')'");
        return first;
    }

    if (tok->kind == TOK_IDENT || tok->kind == TOK_TYPE_IDENT) {
        advance(parser);
        if (tok->data.ident.len > 0 && tok->data.ident.data[0] == '\'') {
            fxsh_ast_node_t *n = alloc_node(AST_TYPE_VAR, tok->loc);
            n->data.ident = tok->data.ident;
            return n;
        }
        sp_dyn_array(sp_str_t) segments = SP_NULLPTR;
        sp_dyn_array_push(segments, tok->data.ident);
        while (match(parser, TOK_DOT)) {
            fxsh_token_t *part_tok = consume_name_token(parser, "type name");
            if (!part_tok)
                return NULL;
            sp_dyn_array_push(segments, part_tok->data.ident);
        }

        sp_str_t type_name = segments[0];
        u32 count = (u32)sp_dyn_array_size(segments);
        if (count > 1) {
            for (u32 prefix_len = count - 1; prefix_len > 0; prefix_len--) {
                sp_str_t module_name = join_qualified_segments(segments, prefix_len);
                if (!(parser_has_name(parser->modules, module_name) ||
                      parser_has_name(parser->imports, module_name))) {
                    continue;
                }
                type_name = module_name;
                for (u32 i = prefix_len; i < count; i++)
                    type_name = mk_qualified_name(type_name, segments[i]);
                break;
            }
        }

        sp_dyn_array_free(segments);
        return fxsh_ast_ident(type_name, tok->loc);
    }

    if (tok->kind == TOK_LBRACE) {
        advance(parser); /* consume '{' */
        skip_newlines(parser);
        fxsh_ast_list_t fields = SP_NULLPTR;

        while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
            if (match(parser, TOK_DOTDOT)) {
                fxsh_token_t *rv = consume(parser, TOK_IDENT, "row type variable");
                if (!rv)
                    return NULL;
                fxsh_ast_node_t *rv_node = alloc_node(AST_TYPE_VAR, rv->loc);
                rv_node->data.ident = rv->data.ident;
                sp_dyn_array_push(fields, rv_node);
            } else {
                fxsh_token_t *name_tok = consume_name_token(parser, "field name");
                if (!name_tok)
                    return NULL;
                consume(parser, TOK_COLON, "':'");
                fxsh_ast_node_t *field_ty = parse_type_expr(parser);
                fxsh_ast_node_t *field = alloc_node(AST_FIELD_ACCESS, name_tok->loc);
                field->data.field.field = name_tok->data.ident;
                field->data.field.object = field_ty; /* store field type */
                sp_dyn_array_push(fields, field);
            }
            skip_newlines(parser);
            if (check(parser, TOK_RBRACE))
                break;
            if (!match(parser, TOK_SEMICOLON) && !match(parser, TOK_COMMA))
                break;
            skip_newlines(parser);
        }

        consume(parser, TOK_RBRACE, "'}'");
        fxsh_ast_node_t *n = alloc_node(AST_TYPE_RECORD, tok->loc);
        n->data.elements = fields;
        return n;
    }

    consume_name_token(parser, "type");
    return NULL;
}

static fxsh_ast_node_t *parse_type_expr(fxsh_parser_t *parser) {
    fxsh_ast_node_t *lhs = parse_type_atom(parser);
    if (!lhs)
        return NULL;

    skip_newlines(parser);
    while (is_type_expr_start(current(parser)->kind)) {
        fxsh_ast_node_t *rhs_atom = parse_type_atom(parser);
        if (!rhs_atom)
            break;
        fxsh_ast_node_t *app = alloc_node(AST_TYPE_APP, lhs->loc);
        app->data.type_con.name = (sp_str_t){0};
        app->data.type_con.args = SP_NULLPTR;
        sp_dyn_array_push(app->data.type_con.args, lhs);
        sp_dyn_array_push(app->data.type_con.args, rhs_atom);
        lhs = app;
        skip_newlines(parser);
    }

    if (match(parser, TOK_ARROW)) {
        fxsh_ast_node_t *rhs = parse_type_expr(parser); /* right-associative */
        fxsh_ast_node_t *n = alloc_node(AST_TYPE_ARROW, lhs->loc);
        n->data.type_arrow.param = lhs;
        n->data.type_arrow.ret = rhs;
        return n;
    }
    return lhs;
}

static fxsh_ast_node_t *parse_pattern_atom(fxsh_parser_t *parser) {
    fxsh_token_t *tok = current(parser);

    /* Wildcard pattern: _ */
    if (tok->kind == TOK_IDENT && tok->data.ident.len == 1 && tok->data.ident.data[0] == '_') {
        advance(parser);
        fxsh_ast_node_t *node = alloc_node(AST_PAT_WILD, tok->loc);
        node->data.ident = tok->data.ident; /* empty string? */
        return node;
    }

    /* Variable pattern: lowercase identifier */
    if (tok->kind == TOK_IDENT) {
        advance(parser);
        fxsh_ast_node_t *node = alloc_node(AST_PAT_VAR, tok->loc);
        node->data.ident = tok->data.ident;
        return node;
    }

    /* Literal patterns: integers, floats, strings, booleans */
    if (tok->kind == TOK_INT || tok->kind == TOK_FLOAT || tok->kind == TOK_STRING ||
        tok->kind == TOK_TRUE || tok->kind == TOK_FALSE) {
        return parse_primary(parser); /* keep concrete literal kind for pattern typing/codegen */
    }

    /* Constructor pattern: uppercase identifier (type ident) */
    if (tok->kind == TOK_TYPE_IDENT) {
        advance(parser);
        sp_dyn_array(sp_str_t) segments = SP_NULLPTR;
        sp_dyn_array_push(segments, tok->data.ident);
        while (match(parser, TOK_DOT)) {
            fxsh_token_t *part_tok = consume_name_token(parser, "constructor name");
            if (!part_tok)
                return NULL;
            sp_dyn_array_push(segments, part_tok->data.ident);
        }

        sp_str_t constr_name = segments[0];
        u32 count = (u32)sp_dyn_array_size(segments);
        if (count > 1) {
            for (u32 prefix_len = count - 1; prefix_len > 0; prefix_len--) {
                sp_str_t module_name = join_qualified_segments(segments, prefix_len);
                if (!(parser_has_name(parser->modules, module_name) ||
                      parser_has_name(parser->imports, module_name))) {
                    continue;
                }
                constr_name = module_name;
                for (u32 i = prefix_len; i < count; i++)
                    constr_name = mk_qualified_name(constr_name, segments[i]);
                break;
            }
        }
        sp_dyn_array_free(segments);

        /* Parse nested patterns for constructor arguments */
        fxsh_ast_list_t args = SP_NULLPTR;
        while (!check(parser, TOK_NEWLINE) && !check(parser, TOK_EOF) &&
               !check(parser, TOK_PIPE_SYM) && !check(parser, TOK_WITH) &&
               !check(parser, TOK_END) && !check(parser, TOK_ELSE) && !check(parser, TOK_IN) &&
               !check(parser, TOK_IF) && !check(parser, TOK_THEN) && !check(parser, TOK_RPAREN) &&
               !check(parser, TOK_RBRACKET) && !check(parser, TOK_RBRACE) &&
               !check(parser, TOK_COMMA) && !check(parser, TOK_SEMICOLON) &&
               !check(parser, TOK_DOT) && !check(parser, TOK_ARROW) &&
               !check(parser, TOK_FAT_ARROW) && !check(parser, TOK_APPEND) &&
               !check(parser, TOK_PLUS) && !check(parser, TOK_MINUS) &&
               !check(parser, TOK_STAR) && !check(parser, TOK_SLASH) &&
               !check(parser, TOK_PERCENT) && !check(parser, TOK_EQ) &&
               !check(parser, TOK_NEQ) && !check(parser, TOK_LT) &&
               !check(parser, TOK_GT) && !check(parser, TOK_LEQ) &&
               !check(parser, TOK_GEQ) && !check(parser, TOK_AND) &&
               !check(parser, TOK_OR) && !check(parser, TOK_PIPE)) {
            fxsh_ast_node_t *arg = parse_pattern(parser);
            if (!arg)
                break;
            sp_dyn_array_push(args, arg);
        }

        fxsh_ast_node_t *node = alloc_node(AST_PAT_CONSTR, tok->loc);
        node->data.constr_appl.constr_name = constr_name;
        node->data.constr_appl.args = args;
        return node;
    }

    /* Tuple pattern: (p1, p2, ...) */
    if (tok->kind == TOK_LPAREN) {
        advance(parser); /* consume ( */
        skip_newlines(parser);

        /* Check for unit () */
        if (check(parser, TOK_RPAREN)) {
            advance(parser);
            fxsh_ast_node_t *node = alloc_node(AST_LIT_UNIT, tok->loc);
            return node; /* unit is both expression and pattern? */
        }

        fxsh_ast_list_t elements = SP_NULLPTR;
        fxsh_ast_node_t *first = parse_pattern(parser);
        if (!first)
            return NULL;
        sp_dyn_array_push(elements, first);
        skip_newlines(parser);

        if (match(parser, TOK_COMMA)) {
            /* Tuple with multiple elements */
            skip_newlines(parser);
            while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
                sp_dyn_array_push(elements, parse_pattern(parser));
                skip_newlines(parser);
                if (!match(parser, TOK_COMMA))
                    break;
                skip_newlines(parser);
            }
            consume(parser, TOK_RPAREN, "')'");
            fxsh_ast_node_t *node = alloc_node(AST_PAT_TUPLE, tok->loc);
            node->data.elements = elements;
            return node;
        } else {
            /* Single element in parentheses - not a tuple */
            consume(parser, TOK_RPAREN, "')'");
            return first;
        }
    }

    /* List pattern: [p1; p2; ...] */
    if (tok->kind == TOK_LBRACKET) {
        advance(parser); /* consume [ */
        skip_newlines(parser);

        fxsh_ast_list_t elems = SP_NULLPTR;
        while (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
            fxsh_ast_node_t *e = parse_pattern(parser);
            if (!e)
                break;
            sp_dyn_array_push(elems, e);
            skip_newlines(parser);
            if (!match(parser, TOK_SEMICOLON) && !match(parser, TOK_COMMA))
                break;
            skip_newlines(parser);
        }
        consume(parser, TOK_RBRACKET, "']'");
        return lower_list_pattern_to_cons(tok->loc, elems);
    }

    /* Record pattern: {field1 = p1, field2 = p2} */
    if (tok->kind == TOK_LBRACE) {
        advance(parser); /* consume { */
        skip_newlines(parser);

        fxsh_ast_list_t fields = SP_NULLPTR;
        while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
            fxsh_token_t *field_tok = consume_name_token(parser, "field name");
            if (!field_tok)
                return NULL;
            consume(parser, TOK_ASSIGN, "'='");
            fxsh_ast_node_t *subpat = parse_pattern(parser);
            fxsh_ast_node_t *field = alloc_node(AST_FIELD_ACCESS, field_tok->loc);
            field->data.field.field = field_tok->data.ident;
            field->data.field.object = subpat; /* stores sub-pattern */
            sp_dyn_array_push(fields, field);
            skip_newlines(parser);
            if (!match(parser, TOK_COMMA) && !match(parser, TOK_SEMICOLON))
                break;
            skip_newlines(parser);
        }
        consume(parser, TOK_RBRACE, "'}'");
        fxsh_ast_node_t *node = alloc_node(AST_PAT_RECORD, tok->loc);
        node->data.elements = fields;
        return node;
    }

    /* Fallback: parse as expression (for now) */
    return parse_primary(parser);
}

static fxsh_ast_node_t *parse_pattern(fxsh_parser_t *parser) {
    fxsh_ast_node_t *lhs = parse_pattern_atom(parser);
    if (!lhs)
        return NULL;
    if (match(parser, TOK_APPEND)) {
        fxsh_ast_node_t *rhs = parse_pattern(parser); /* right-associative */
        return make_pat_cons_node(lhs->loc, lhs, rhs);
    }
    return lhs;
}

static bool is_app_arg_start(fxsh_token_kind_t kind) {
    switch (kind) {
        case TOK_INT:
        case TOK_FLOAT:
        case TOK_STRING:
        case TOK_FSTRING:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_IDENT:
        case TOK_TYPE_IDENT:
        case TOK_LPAREN:
        case TOK_LBRACKET:
        case TOK_LBRACE:
            return true;
        default:
            return false;
    }
}

static bool prev_token_is_newline(fxsh_parser_t *parser) {
    return parser && parser->pos > 0 && parser->tokens[parser->pos - 1].kind == TOK_NEWLINE;
}

static u32 token_display_width(const fxsh_token_t *tok) {
    if (!tok)
        return 0;
    switch (tok->kind) {
        case TOK_INT: {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)tok->data.int_val);
            return n > 0 ? (u32)n : 0;
        }
        case TOK_FLOAT: {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "%.17g", tok->data.float_val);
            return n > 0 ? (u32)n : 0;
        }
        case TOK_STRING:
        case TOK_FSTRING:
            return tok->data.str_val.len + 2;
        case TOK_IDENT:
        case TOK_TYPE_IDENT:
            return tok->data.ident.len;
        case TOK_TRUE:
            return 4;
        case TOK_FALSE:
            return 5;
        case TOK_LET:
        case TOK_NOT:
        case TOK_FOR:
        case TOK_REC:
            return 3;
        case TOK_FN:
        case TOK_IF:
        case TOK_IN:
        case TOK_OF:
        case TOK_DO:
        case TOK_OR:
            return 2;
        case TOK_END:
            return 3;
        case TOK_THEN:
            return 4;
        case TOK_ELSE:
            return 4;
        case TOK_WHILE:
            return 5;
        case TOK_MATCH:
            return 5;
        case TOK_WITH:
            return 4;
        case TOK_TYPE:
            return 4;
        case TOK_MODULE:
            return 6;
        case TOK_IMPORT:
            return 6;
        case TOK_COMPTIME:
            return 8;
        case TOK_STRUCT:
            return 6;
        case TOK_TRAIT:
            return 5;
        case TOK_IMPL:
            return 4;
        case TOK_RETURN:
            return 6;
        case TOK_AND:
            return 3;
        case TOK_PLUS:
        case TOK_MINUS:
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:
        case TOK_ASSIGN:
        case TOK_LT:
        case TOK_GT:
        case TOK_LPAREN:
        case TOK_RPAREN:
        case TOK_LBRACKET:
        case TOK_RBRACKET:
        case TOK_LBRACE:
        case TOK_RBRACE:
        case TOK_COMMA:
        case TOK_COLON:
        case TOK_SEMICOLON:
        case TOK_DOT:
        case TOK_PIPE_SYM:
        case TOK_AT:
            return 1;
        case TOK_ARROW:
        case TOK_FAT_ARROW:
        case TOK_PIPE:
        case TOK_EQ:
        case TOK_NEQ:
        case TOK_LEQ:
        case TOK_GEQ:
        case TOK_APPEND:
        case TOK_DOTDOT:
            return 2;
        default:
            return 0;
    }
}

static bool current_token_is_tightly_attached(fxsh_parser_t *parser) {
    if (!parser || parser->pos == 0)
        return false;
    fxsh_token_t *prev = &parser->tokens[parser->pos - 1];
    fxsh_token_t *cur = current(parser);
    if (!cur || prev->loc.line != cur->loc.line)
        return false;
    u32 prev_end_col = prev->loc.column + token_display_width(prev);
    return cur->loc.column == prev_end_col;
}

static sp_str_t fresh_do_tmp_name(void) {
    static u32 seq = 0;
    c8 *buf = (c8 *)fxsh_alloc0(32);
    if (!buf)
        return (sp_str_t){.data = "__do_tmp_oom", .len = 12};
    int n = snprintf((char *)buf, 32, "__do_tmp_%u", (unsigned)seq++);
    if (n < 0)
        n = 0;
    return (sp_str_t){.data = buf, .len = (u32)n};
}

static bool is_do_stmt_boundary(fxsh_parser_t *parser) {
    return check(parser, TOK_SEMICOLON) || check(parser, TOK_NEWLINE) ||
           check(parser, TOK_RBRACE) || check(parser, TOK_EOF);
}

typedef enum { DO_STMT_LET, DO_STMT_BIND } do_stmt_kind_t;
typedef enum { DO_BIND_RESULT, DO_BIND_OPTION } do_bind_mode_t;

typedef struct {
    do_stmt_kind_t kind;
    fxsh_ast_node_t *let_node; /* DO_STMT_LET */
    sp_str_t bind_name;        /* DO_STMT_BIND */
    fxsh_ast_node_t *bind_rhs; /* DO_STMT_BIND */
    do_bind_mode_t bind_mode;  /* DO_STMT_BIND */
    fxsh_loc_t loc;            /* DO_STMT_BIND */
} do_stmt_t;

static fxsh_ast_node_t *make_pat_var_node(sp_str_t name, fxsh_loc_t loc) {
    fxsh_ast_node_t *n = alloc_node(AST_PAT_VAR, loc);
    n->data.ident = name;
    return n;
}

static fxsh_ast_node_t *make_pat_constr1(sp_str_t cname, fxsh_ast_node_t *arg, fxsh_loc_t loc) {
    fxsh_ast_node_t *n = alloc_node(AST_PAT_CONSTR, loc);
    n->data.constr_appl.constr_name = cname;
    n->data.constr_appl.args = SP_NULLPTR;
    if (arg)
        sp_dyn_array_push(n->data.constr_appl.args, arg);
    return n;
}

static fxsh_ast_node_t *make_constr_appl1(sp_str_t cname, fxsh_ast_node_t *arg, fxsh_loc_t loc) {
    fxsh_ast_node_t *n = alloc_node(AST_CONSTR_APPL, loc);
    n->data.constr_appl.constr_name = cname;
    n->data.constr_appl.args = SP_NULLPTR;
    if (arg)
        sp_dyn_array_push(n->data.constr_appl.args, arg);
    return n;
}

static fxsh_ast_node_t *make_match_arm(fxsh_ast_node_t *pat, fxsh_ast_node_t *body,
                                       fxsh_loc_t loc) {
    fxsh_ast_node_t *arm = alloc_node(AST_MATCH_ARM, loc);
    arm->data.match_arm.pattern = pat;
    arm->data.match_arm.guard = NULL;
    arm->data.match_arm.body = body;
    return arm;
}

/* let! x = rhs  ==>  match rhs with | Ok x -> cont | Err e -> Err e */
static fxsh_ast_node_t *lower_do_bind_result(sp_str_t name, fxsh_ast_node_t *rhs,
                                             fxsh_ast_node_t *cont, fxsh_loc_t loc) {
    sp_str_t ok_name = sp_str_lit("Ok");
    sp_str_t err_name = sp_str_lit("Err");
    sp_str_t err_var = fresh_do_tmp_name();

    fxsh_ast_node_t *pat_ok = make_pat_constr1(ok_name, make_pat_var_node(name, loc), loc);
    fxsh_ast_node_t *pat_err = make_pat_constr1(err_name, make_pat_var_node(err_var, loc), loc);

    fxsh_ast_node_t *err_body = make_constr_appl1(err_name, fxsh_ast_ident(err_var, loc), loc);

    fxsh_ast_node_t *m = alloc_node(AST_MATCH, loc);
    m->data.match_expr.expr = rhs;
    m->data.match_expr.arms = SP_NULLPTR;
    sp_dyn_array_push(m->data.match_expr.arms, make_match_arm(pat_ok, cont, loc));
    sp_dyn_array_push(m->data.match_expr.arms, make_match_arm(pat_err, err_body, loc));
    return m;
}

/* let? x = rhs  ==>  match rhs with | Some x -> cont | None -> None */
static fxsh_ast_node_t *lower_do_bind_option(sp_str_t name, fxsh_ast_node_t *rhs,
                                             fxsh_ast_node_t *cont, fxsh_loc_t loc) {
    sp_str_t some_name = sp_str_lit("Some");
    sp_str_t none_name = sp_str_lit("None");

    fxsh_ast_node_t *pat_some = make_pat_constr1(some_name, make_pat_var_node(name, loc), loc);
    fxsh_ast_node_t *pat_none = make_pat_constr1(none_name, NULL, loc);
    fxsh_ast_node_t *none_body = make_constr_appl1(none_name, NULL, loc);

    fxsh_ast_node_t *m = alloc_node(AST_MATCH, loc);
    m->data.match_expr.expr = rhs;
    m->data.match_expr.arms = SP_NULLPTR;
    sp_dyn_array_push(m->data.match_expr.arms, make_match_arm(pat_some, cont, loc));
    sp_dyn_array_push(m->data.match_expr.arms, make_match_arm(pat_none, none_body, loc));
    return m;
}

static fxsh_ast_node_t *do_block_fail_sync(fxsh_parser_t *parser, const c8 *msg) {
    fxsh_token_t *bad = current(parser);
    parser_error_at(parser, bad->loc, "%s, got %s", msg, fxsh_token_kind_name(bad->kind));
    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF))
        advance(parser);
    if (check(parser, TOK_RBRACE))
        advance(parser);
    return NULL;
}

static fxsh_ast_node_t *parse_do_block(fxsh_parser_t *parser, fxsh_loc_t loc) {
    consume(parser, TOK_LBRACE, "'{'");
    skip_newlines(parser);

    sp_dyn_array(do_stmt_t) stmts = SP_NULLPTR;
    fxsh_ast_node_t *body = NULL;

    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        skip_newlines(parser);
        if (check(parser, TOK_RBRACE))
            break;

        if (match(parser, TOK_LET)) {
            if (check(parser, TOK_BANG) || check(parser, TOK_QMARK)) {
                do_bind_mode_t bind_mode = DO_BIND_RESULT;
                if (match(parser, TOK_BANG))
                    bind_mode = DO_BIND_RESULT;
                else if (match(parser, TOK_QMARK))
                    bind_mode = DO_BIND_OPTION;
                if (!check(parser, TOK_IDENT) && !check(parser, TOK_TYPE_IDENT))
                    return do_block_fail_sync(parser,
                                              bind_mode == DO_BIND_RESULT
                                                  ? "expected identifier after let! in do-block"
                                                  : "expected identifier after let? in do-block");
                fxsh_token_t *name_tok = advance(parser);
                if (!match(parser, TOK_ASSIGN))
                    return do_block_fail_sync(parser,
                                              bind_mode == DO_BIND_RESULT
                                                  ? "expected '=' after do-block let! binding"
                                                  : "expected '=' after do-block let? binding");
                fxsh_ast_node_t *rhs = parse_expr(parser);
                if (!is_do_stmt_boundary(parser)) {
                    return do_block_fail_sync(
                        parser, bind_mode == DO_BIND_RESULT
                                    ? "expected ';' or newline after do-block let! statement"
                                    : "expected ';' or newline after do-block let? statement");
                }
                do_stmt_t s = {.kind = DO_STMT_BIND,
                               .let_node = NULL,
                               .bind_name = name_tok->data.ident,
                               .bind_rhs = rhs,
                               .bind_mode = bind_mode,
                               .loc = name_tok->loc};
                sp_dyn_array_push(stmts, s);
                skip_newlines(parser);
                (void)match(parser, TOK_SEMICOLON);
                skip_newlines(parser);
                continue;
            }

            bool is_comptime = match(parser, TOK_COMPTIME);
            bool is_rec = match(parser, TOK_REC);

            if (!check(parser, TOK_IDENT) && !check(parser, TOK_TYPE_IDENT))
                return do_block_fail_sync(parser, "expected identifier after let in do-block");
            fxsh_token_t *name_tok = advance(parser);

            fxsh_ast_list_t params = SP_NULLPTR;
            while (is_pattern_start(current(parser)->kind)) {
                fxsh_ast_node_t *pat = parse_pattern(parser);
                if (!pat)
                    break;
                sp_dyn_array_push(params, pat);
                skip_newlines(parser);
            }

            fxsh_ast_node_t *type_ann = NULL;
            if (match(parser, TOK_COLON)) {
                skip_newlines(parser);
                type_ann = parse_type_expr(parser);
            }

            if (!match(parser, TOK_ASSIGN))
                return do_block_fail_sync(parser, "expected '=' after do-block let binding");
            fxsh_ast_node_t *value = parse_expr(parser);
            if (!is_do_stmt_boundary(parser)) {
                return do_block_fail_sync(parser,
                                          "expected ';' or newline after do-block let statement");
            }

            if (params && sp_dyn_array_size(params) > 0) {
                for (int i = (int)sp_dyn_array_size(params) - 1; i >= 0; i--) {
                    fxsh_ast_list_t single_param = SP_NULLPTR;
                    sp_dyn_array_push(single_param, params[i]);
                    value = fxsh_ast_lambda(single_param, value, name_tok->loc);
                }
            }

            fxsh_ast_node_t *b =
                fxsh_ast_let(name_tok->data.ident, value, is_comptime, is_rec, name_tok->loc);
            b->data.let.type = type_ann;
            do_stmt_t s = {.kind = DO_STMT_LET,
                           .let_node = b,
                           .bind_name = (sp_str_t){0},
                           .bind_rhs = NULL,
                           .bind_mode = DO_BIND_RESULT,
                           .loc = name_tok->loc};
            sp_dyn_array_push(stmts, s);
            skip_newlines(parser);
            (void)match(parser, TOK_SEMICOLON);
            skip_newlines(parser);
            continue;
        }

        if (match(parser, TOK_RETURN)) {
            body = parse_expr(parser);
            skip_newlines(parser);
            (void)match(parser, TOK_SEMICOLON);
            skip_newlines(parser);
            if (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
                return do_block_fail_sync(parser, "expected end of do-block after return");
            }
            break;
        }

        fxsh_ast_node_t *expr = parse_expr(parser);
        bool had_semi = match(parser, TOK_SEMICOLON);
        skip_newlines(parser);
        if (had_semi) {
            fxsh_ast_node_t *b = fxsh_ast_let(fresh_do_tmp_name(), expr, false, false, expr->loc);
            do_stmt_t s = {.kind = DO_STMT_LET,
                           .let_node = b,
                           .bind_name = (sp_str_t){0},
                           .bind_rhs = NULL,
                           .bind_mode = DO_BIND_RESULT,
                           .loc = expr->loc};
            sp_dyn_array_push(stmts, s);
            continue;
        }
        if (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
            /* Line-separated statement in do-block: discard value and keep parsing. */
            fxsh_ast_node_t *b = fxsh_ast_let(fresh_do_tmp_name(), expr, false, false, expr->loc);
            do_stmt_t s = {.kind = DO_STMT_LET,
                           .let_node = b,
                           .bind_name = (sp_str_t){0},
                           .bind_rhs = NULL,
                           .bind_mode = DO_BIND_RESULT,
                           .loc = expr->loc};
            sp_dyn_array_push(stmts, s);
            continue;
        }
        body = expr;
        break;
    }

    consume(parser, TOK_RBRACE, "'}'");
    if (!body)
        body = alloc_node(AST_LIT_UNIT, loc);

    fxsh_ast_node_t *cur = body;
    for (s32 i = (s32)sp_dyn_array_size(stmts) - 1; i >= 0; i--) {
        do_stmt_t s = stmts[i];
        if (s.kind == DO_STMT_LET) {
            fxsh_ast_node_t *n = alloc_node(AST_LET_IN, s.let_node ? s.let_node->loc : loc);
            n->data.let_in.bindings = SP_NULLPTR;
            if (s.let_node)
                sp_dyn_array_push(n->data.let_in.bindings, s.let_node);
            n->data.let_in.body = cur;
            cur = n;
        } else {
            if (s.bind_mode == DO_BIND_OPTION)
                cur = lower_do_bind_option(s.bind_name, s.bind_rhs, cur, s.loc);
            else
                cur = lower_do_bind_result(s.bind_name, s.bind_rhs, cur, s.loc);
        }
    }
    return cur;
}

static fxsh_ast_node_t *fstring_concat(fxsh_ast_node_t *lhs, fxsh_ast_node_t *rhs, fxsh_loc_t loc) {
    if (!lhs)
        return rhs;
    if (!rhs)
        return lhs;
    return fxsh_ast_binary(TOK_CONCAT, lhs, rhs, loc);
}

static fxsh_ast_node_t *fstring_literal_node(sp_dyn_array(c8) lit_buf, fxsh_loc_t loc) {
    u32 len = lit_buf ? (u32)sp_dyn_array_size(lit_buf) : 0;
    c8 *buf = (c8 *)fxsh_alloc0(len + 1);
    if (len)
        memcpy(buf, lit_buf, len);
    buf[len] = '\0';
    return fxsh_ast_lit_string((sp_str_t){.data = buf, .len = len}, loc);
}

static fxsh_ast_node_t *parse_fstring_embedded_expr(fxsh_parser_t *parser, const c8 *src, u32 len,
                                                    fxsh_loc_t loc) {
    while (len > 0 && (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' ||
                       src[len - 1] == '\n')) {
        len--;
    }
    if (len == 0) {
        parser_error_at(parser, loc, "empty f-string interpolation");
        return NULL;
    }

    c8 *buf = (c8 *)fxsh_alloc0(len + 1);
    memcpy(buf, src, len);
    buf[len] = '\0';
    sp_str_t sub_src = {.data = buf, .len = len};

    fxsh_token_array_t sub_tokens = SP_NULLPTR;
    if (fxsh_lex(sub_src, loc.filename, &sub_tokens) != ERR_OK)
        return NULL;

    fxsh_parser_t sub_parser;
    fxsh_parser_init(&sub_parser, sub_tokens);
    sub_parser.modules = parser->modules;
    sub_parser.imports = parser->imports;
    sub_parser.traits = parser->traits;

    fxsh_ast_node_t *expr = parse_expr(&sub_parser);
    if (!expr)
        return NULL;

    while (current(&sub_parser)->kind == TOK_NEWLINE)
        advance(&sub_parser);
    if (current(&sub_parser)->kind != TOK_EOF) {
        parser_error_at(parser, loc, "invalid f-string interpolation expression");
        return NULL;
    }
    return expr;
}

static fxsh_ast_node_t *parse_fstring_expr(fxsh_parser_t *parser, sp_str_t raw, fxsh_loc_t loc) {
    fxsh_ast_node_t *acc = NULL;
    sp_dyn_array(c8) lit = SP_NULLPTR;

    for (u32 i = 0; i < raw.len; i++) {
        c8 c = raw.data[i];
        if (c == '{') {
            if (i + 1 < raw.len && raw.data[i + 1] == '{') {
                sp_dyn_array_push(lit, '{');
                i++;
                continue;
            }

            if (lit && sp_dyn_array_size(lit) > 0) {
                acc = fstring_concat(acc, fstring_literal_node(lit, loc), loc);
                sp_dyn_array_free(lit);
                lit = SP_NULLPTR;
            }

            u32 start = i + 1;
            s32 depth = 1;
            bool in_string = false;
            bool esc = false;
            for (i = start; i < raw.len; i++) {
                c8 ch = raw.data[i];
                if (in_string) {
                    if (esc) {
                        esc = false;
                        continue;
                    }
                    if (ch == '\\') {
                        esc = true;
                        continue;
                    }
                    if (ch == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    in_string = true;
                    continue;
                }
                if (ch == '{') {
                    depth++;
                    continue;
                }
                if (ch == '}') {
                    depth--;
                    if (depth == 0)
                        break;
                }
            }
            if (depth != 0) {
                parser_error_at(parser, loc, "unterminated f-string interpolation");
                if (lit)
                    sp_dyn_array_free(lit);
                return NULL;
            }

            fxsh_ast_node_t *sub =
                parse_fstring_embedded_expr(parser, raw.data + start, i - start, loc);
            if (!sub) {
                if (lit)
                    sp_dyn_array_free(lit);
                return NULL;
            }
            acc = fstring_concat(acc, sub, loc);
            continue;
        }

        if (c == '}') {
            if (i + 1 < raw.len && raw.data[i + 1] == '}') {
                sp_dyn_array_push(lit, '}');
                i++;
                continue;
            }
            parser_error_at(parser, loc, "unmatched '}' in f-string");
            if (lit)
                sp_dyn_array_free(lit);
            return NULL;
        }

        sp_dyn_array_push(lit, c);
    }

    if (lit && sp_dyn_array_size(lit) > 0) {
        acc = fstring_concat(acc, fstring_literal_node(lit, loc), loc);
        sp_dyn_array_free(lit);
        lit = SP_NULLPTR;
    } else if (lit) {
        sp_dyn_array_free(lit);
    }

    if (!acc) {
        c8 *buf = (c8 *)fxsh_alloc0(1);
        return fxsh_ast_lit_string((sp_str_t){.data = buf, .len = 0}, loc);
    }
    return acc;
}

/*=============================================================================
 * Primary Expressions
 *=============================================================================*/

static fxsh_ast_node_t *parse_primary(fxsh_parser_t *parser) {
    fxsh_token_t *tok = current(parser);

    switch (tok->kind) {
        case TOK_INT: {
            advance(parser);
            return fxsh_ast_lit_int(tok->data.int_val, tok->loc);
        }
        case TOK_FLOAT: {
            advance(parser);
            return fxsh_ast_lit_float(tok->data.float_val, tok->loc);
        }
        case TOK_STRING: {
            advance(parser);
            return fxsh_ast_lit_string(tok->data.str_val, tok->loc);
        }
        case TOK_FSTRING: {
            advance(parser);
            return parse_fstring_expr(parser, tok->data.str_val, tok->loc);
        }
        case TOK_TRUE: {
            advance(parser);
            return fxsh_ast_lit_bool(true, tok->loc);
        }
        case TOK_FALSE: {
            advance(parser);
            return fxsh_ast_lit_bool(false, tok->loc);
        }
        case TOK_IDENT: {
            advance(parser);
            return fxsh_ast_ident(tok->data.ident, tok->loc);
        }
        case TOK_TYPE_IDENT: {
            /* Constructor application: Some 5, Cons(x, xs), Cons(42, Nil) */
            advance(parser);
            sp_str_t constr_name = tok->data.ident;

            /* Parse constructor arguments */
            fxsh_ast_list_t args = SP_NULLPTR;
            while (!check(parser, TOK_NEWLINE) && !check(parser, TOK_EOF) &&
                   !check(parser, TOK_PIPE_SYM) && !check(parser, TOK_WITH) &&
                   !check(parser, TOK_END) && !check(parser, TOK_ELSE) && !check(parser, TOK_IN) &&
                   !check(parser, TOK_IF) && !check(parser, TOK_THEN) &&
                   !check(parser, TOK_RPAREN) && !check(parser, TOK_RBRACKET) &&
                   !check(parser, TOK_RBRACE) && !check(parser, TOK_COMMA) &&
                   !check(parser, TOK_SEMICOLON) && !check(parser, TOK_DOT) &&
                   !check(parser, TOK_ARROW) && !check(parser, TOK_FAT_ARROW) &&
                   !check(parser, TOK_APPEND) && !check(parser, TOK_PLUS) &&
                   !check(parser, TOK_MINUS) && !check(parser, TOK_STAR) &&
                   !check(parser, TOK_SLASH) && !check(parser, TOK_PERCENT) &&
                   !check(parser, TOK_EQ) && !check(parser, TOK_NEQ) &&
                   !check(parser, TOK_LT) && !check(parser, TOK_GT) &&
                   !check(parser, TOK_LEQ) && !check(parser, TOK_GEQ) &&
                   !check(parser, TOK_AND) && !check(parser, TOK_OR) &&
                   !check(parser, TOK_PIPE)) {
                fxsh_ast_node_t *arg = parse_primary(parser);
                if (!arg)
                    break;

                sp_dyn_array_push(args, arg);
            }

            fxsh_ast_node_t *node = alloc_node(AST_CONSTR_APPL, tok->loc);
            node->data.constr_appl.constr_name = constr_name;
            node->data.constr_appl.args = args;
            return node;
        }
        case TOK_AT: {
            advance(parser); /* consume @ */
            fxsh_token_t *op_tok = consume_name_token(parser, "compile-time operator name");
            if (!op_tok)
                return NULL;

            /* Parse compile-time operator: @typeOf, @sizeOf, etc. */
            sp_str_t op_name = op_tok->data.ident;

            if (sp_str_equal(op_name, (sp_str_t){.data = "typeOf", .len = 6})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_TYPE_OF, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "typeName", .len = 8})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_TYPE_NAME, tok->loc);
                node->data.ct_type_op.type_expr = type_expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "sizeOf", .len = 6})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_SIZE_OF, tok->loc);
                node->data.ct_type_op.type_expr = type_expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "alignOf", .len = 7})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_ALIGN_OF, tok->loc);
                node->data.ct_type_op.type_expr = type_expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "fieldsOf", .len = 8})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_FIELDS_OF, tok->loc);
                node->data.ct_type_op.type_expr = type_expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "isRecord", .len = 8})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_IS_RECORD, tok->loc);
                node->data.ct_type_op.type_expr = type_expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "isTuple", .len = 7})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_IS_TUPLE, tok->loc);
                node->data.ct_type_op.type_expr = type_expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "jsonSchema", .len = 10})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_JSON_SCHEMA, tok->loc);
                node->data.ct_type_op.type_expr = type_expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "sqliteSQL", .len = 9})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_COMMA, "','");
                fxsh_token_t *table_tok = consume(parser, TOK_STRING, "table name string");
                consume(parser, TOK_RPAREN, "')'");
                if (!table_tok)
                    return NULL;

                fxsh_ast_node_t *node = alloc_node(AST_CT_SQLITE_SQL, tok->loc);
                node->data.ct_sqlite_sql.type_expr = type_expr;
                node->data.ct_sqlite_sql.table_name = table_tok->data.str_val;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "hasField", .len = 8})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *type_expr = parse_expr(parser);
                consume(parser, TOK_COMMA, "','");
                fxsh_token_t *field_tok = consume(parser, TOK_STRING, "field name string");
                consume(parser, TOK_RPAREN, "')'");
                if (!field_tok)
                    return NULL;

                fxsh_ast_node_t *node = alloc_node(AST_CT_HAS_FIELD, tok->loc);
                node->data.ct_has_field.type_expr = type_expr;
                node->data.ct_has_field.field_name = field_tok->data.str_val;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "quote", .len = 5})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_QUOTE, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "unquote", .len = 7})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_UNQUOTE, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "splice", .len = 6})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_SPLICE, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "compileError", .len = 12})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_COMPILE_ERROR, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "compileLog", .len = 10})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_COMPILE_LOG, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "panic", .len = 5})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_PANIC, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "sql", .len = 3})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_SQL, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else if (sp_str_equal(op_name, (sp_str_t){.data = "sqlCheck", .len = 8})) {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_node_t *expr = parse_expr(parser);
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_SQL_CHECK, tok->loc);
                node->data.ct_type_of.operand = expr;
                return node;
            } else {
                consume(parser, TOK_LPAREN, "'('");
                fxsh_ast_list_t type_args = SP_NULLPTR;
                skip_newlines(parser);
                if (!check(parser, TOK_RPAREN)) {
                    while (true) {
                        fxsh_ast_node_t *arg = parse_expr(parser);
                        if (!arg)
                            return NULL;
                        sp_dyn_array_push(type_args, arg);
                        skip_newlines(parser);
                        if (!match(parser, TOK_COMMA))
                            break;
                        skip_newlines(parser);
                    }
                }
                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_CT_CTOR_APPLY, tok->loc);
                node->data.ct_ctor_apply.ctor_name = op_name;
                node->data.ct_ctor_apply.type_args = type_args;
                return node;
            }
        }
        case TOK_LPAREN: {
            advance(parser); /* consume ( */
            skip_newlines(parser);

            /* Check for unit () */
            if (check(parser, TOK_RPAREN)) {
                advance(parser);
                return alloc_node(AST_LIT_UNIT, tok->loc);
            }

            fxsh_ast_node_t *expr = parse_expr(parser);
            skip_newlines(parser);

            /* Check for tuple (a, b) */
            if (match(parser, TOK_COMMA)) {
                fxsh_ast_list_t elements = SP_NULLPTR;
                sp_dyn_array_push(elements, expr);

                skip_newlines(parser);
                while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
                    sp_dyn_array_push(elements, parse_expr(parser));
                    skip_newlines(parser);
                    if (!match(parser, TOK_COMMA))
                        break;
                    skip_newlines(parser);
                }

                consume(parser, TOK_RPAREN, "')'");

                fxsh_ast_node_t *node = alloc_node(AST_TUPLE, tok->loc);
                node->data.elements = elements;
                return node;
            }

            consume(parser, TOK_RPAREN, "')'");
            return expr;
        }
        case TOK_LBRACKET: {
            advance(parser); /* consume [ */
            skip_newlines(parser);

            fxsh_ast_list_t elements = SP_NULLPTR;

            while (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
                sp_dyn_array_push(elements, parse_expr(parser));
                skip_newlines(parser);
                if (!match(parser, TOK_SEMICOLON) && !match(parser, TOK_COMMA))
                    break;
                skip_newlines(parser);
            }

            consume(parser, TOK_RBRACKET, "']'");

            fxsh_ast_node_t *node = alloc_node(AST_LIST, tok->loc);
            node->data.elements = elements;
            return node;
        }
        case TOK_LBRACE: {
            advance(parser); /* consume { */
            skip_newlines(parser);
            if (check(parser, TOK_RBRACE)) {
                consume(parser, TOK_RBRACE, "'}'");
                fxsh_ast_node_t *node = alloc_node(AST_RECORD, tok->loc);
                node->data.elements = SP_NULLPTR;
                return node;
            }
            fxsh_ast_node_t *first_expr = parse_expr(parser);
            skip_newlines(parser);

            if (match(parser, TOK_WITH)) {
                fxsh_ast_list_t updates = SP_NULLPTR;
                skip_newlines(parser);
                while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
                    fxsh_token_t *field_tok = consume(parser, TOK_IDENT, "field name");
                    if (!field_tok)
                        return NULL;
                    consume(parser, TOK_ASSIGN, "'='");
                    fxsh_ast_node_t *field_value = parse_expr(parser);

                    fxsh_ast_node_t *field_node = alloc_node(AST_FIELD_ACCESS, field_tok->loc);
                    field_node->data.field.object = field_value;
                    field_node->data.field.field = field_tok->data.ident;
                    field_node->data.field.type = NULL;
                    sp_dyn_array_push(updates, field_node);

                    skip_newlines(parser);
                    if (!match(parser, TOK_COMMA) && !match(parser, TOK_SEMICOLON))
                        break;
                    skip_newlines(parser);
                }
                consume(parser, TOK_RBRACE, "'}'");

                fxsh_ast_node_t *node = alloc_node(AST_RECORD_UPDATE, tok->loc);
                node->data.record_update.base = first_expr;
                node->data.record_update.updates = updates;
                return node;
            }

            fxsh_ast_list_t fields = SP_NULLPTR;
            while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
                if (!first_expr || first_expr->kind != AST_IDENT) {
                    parser_error_at(parser, first_expr ? first_expr->loc : tok->loc,
                                    "expected field name in record literal");
                    return NULL;
                }

                sp_str_t field_name = first_expr->data.ident;
                fxsh_ast_node_t *field_type = NULL;
                if (match(parser, TOK_COLON)) {
                    field_type = parse_type_expr(parser);
                }

                consume(parser, TOK_ASSIGN, "'='");
                fxsh_ast_node_t *field_value = parse_expr(parser);

                fxsh_ast_node_t *field_node = alloc_node(AST_FIELD_ACCESS, first_expr->loc);
                field_node->data.field.object = field_value;
                field_node->data.field.field = field_name;
                field_node->data.field.type = field_type;
                sp_dyn_array_push(fields, field_node);

                skip_newlines(parser);
                if (!match(parser, TOK_COMMA) && !match(parser, TOK_SEMICOLON))
                    break;
                skip_newlines(parser);
                if (check(parser, TOK_RBRACE))
                    break;
                first_expr = parse_expr(parser);
                skip_newlines(parser);
            }

            consume(parser, TOK_RBRACE, "'}'");

            fxsh_ast_node_t *node = alloc_node(AST_RECORD, tok->loc);
            node->data.elements = fields;
            return node;
        }
        case TOK_FN: {
            advance(parser); /* consume fn */

            fxsh_ast_list_t params = SP_NULLPTR;

            /* Parse parameter list */
            skip_newlines(parser);
            while (is_pattern_start(current(parser)->kind)) {
                fxsh_ast_node_t *pat = parse_pattern(parser);
                if (!pat)
                    break;
                sp_dyn_array_push(params, pat);
                skip_newlines(parser);
            }

            consume(parser, TOK_ARROW, "'->'");

            fxsh_ast_node_t *body = parse_expr(parser);

            return fxsh_ast_lambda(params, body, tok->loc);
        }
        case TOK_IF: {
            advance(parser); /* consume if */

            fxsh_ast_node_t *cond = parse_expr(parser);

            skip_newlines(parser);
            consume(parser, TOK_THEN, "'then'");

            fxsh_ast_node_t *then_branch = parse_expr(parser);

            skip_newlines(parser);
            consume(parser, TOK_ELSE, "'else'");
            fxsh_ast_node_t *else_branch = parse_expr(parser);
            /* Avoid stealing enclosing `end` (e.g. module/match) for single-line if-expressions. */
            if (tok->loc.line != cond->loc.line ||
                (then_branch && tok->loc.line != then_branch->loc.line) ||
                (else_branch && tok->loc.line != else_branch->loc.line)) {
                maybe_consume_block_end(parser);
            }

            return fxsh_ast_if(cond, then_branch, else_branch, tok->loc);
        }
        case TOK_COMPTIME: {
            advance(parser); /* consume comptime */
            fxsh_ast_node_t *expr = NULL;
            if (match(parser, TOK_LBRACE)) {
                skip_newlines(parser);
                expr = parse_expr(parser);
                skip_newlines(parser);
                consume(parser, TOK_RBRACE, "'}'");
            } else {
                expr = parse_expr(parser);
            }
            fxsh_ast_node_t *node = alloc_node(AST_CT_EVAL, tok->loc);
            node->data.ct_type_of.operand = expr;
            return node;
        }
        case TOK_DO: {
            advance(parser); /* consume do */
            return parse_do_block(parser, tok->loc);
        }
        case TOK_LET: {
            advance(parser); /* consume let */
            skip_newlines(parser);

            fxsh_ast_list_t bindings = SP_NULLPTR;

            while (!check(parser, TOK_IN) && !check(parser, TOK_EOF)) {
                bool is_comptime = match(parser, TOK_COMPTIME);
                bool is_rec = match(parser, TOK_REC);

                fxsh_token_t *name_tok = consume_name_token(parser, "identifier");
                if (!name_tok)
                    return NULL;

                /* Parse function parameters if present (function definition sugar: let f x y = ...)
                 */
                fxsh_ast_list_t params = SP_NULLPTR;
                while (is_pattern_start(current(parser)->kind)) {
                    fxsh_ast_node_t *pat = parse_pattern(parser);
                    if (!pat)
                        break;
                    sp_dyn_array_push(params, pat);
                    skip_newlines(parser);
                }

                fxsh_ast_node_t *type_ann = NULL;
                if (match(parser, TOK_COLON)) {
                    skip_newlines(parser);
                    type_ann = parse_type_expr(parser);
                }

                consume(parser, TOK_ASSIGN, "'='");

                fxsh_ast_node_t *value = parse_expr(parser);

                /* If we have params, wrap value in curried lambda */
                if (params && sp_dyn_array_size(params) > 0) {
                    for (int i = (int)sp_dyn_array_size(params) - 1; i >= 0; i--) {
                        fxsh_ast_list_t single_param = SP_NULLPTR;
                        sp_dyn_array_push(single_param, params[i]);
                        value = fxsh_ast_lambda(single_param, value, name_tok->loc);
                    }
                }

                fxsh_ast_node_t *binding =
                    fxsh_ast_let(name_tok->data.ident, value, is_comptime, is_rec, name_tok->loc);
                binding->data.let.type = type_ann;
                sp_dyn_array_push(bindings, binding);

                skip_newlines(parser);
            }

            consume(parser, TOK_IN, "'in'");

            fxsh_ast_node_t *body = parse_expr(parser);

            fxsh_ast_node_t *node = alloc_node(AST_LET_IN, tok->loc);
            node->data.let_in.bindings = bindings;
            node->data.let_in.body = body;
            return node;
        }
        case TOK_MATCH: {
            advance(parser); /* consume match */

            fxsh_ast_node_t *expr = parse_expr(parser);

            skip_newlines(parser);
            consume(parser, TOK_WITH, "'with'");
            skip_newlines(parser);

            fxsh_ast_list_t arms = SP_NULLPTR;

            while (check(parser, TOK_PIPE_SYM)) {
                advance(parser); /* consume | */

                fxsh_ast_node_t *pattern = parse_pattern(parser);

                fxsh_ast_node_t *guard = NULL;
                if (match(parser, TOK_IF)) {
                    guard = parse_expr(parser);
                } else if (token_is_ident_text(current(parser), "when")) {
                    advance(parser); /* consume `when` */
                    guard = parse_expr(parser);
                }

                consume(parser, TOK_ARROW, "'->'");

                fxsh_ast_node_t *arm_body = parse_expr(parser);

                fxsh_ast_node_t *arm = alloc_node(AST_MATCH_ARM, pattern->loc);
                arm->data.match_arm.pattern = pattern;
                arm->data.match_arm.guard = guard;
                arm->data.match_arm.body = arm_body;

                sp_dyn_array_push(arms, arm);
                skip_newlines(parser);
            }
            maybe_consume_block_end(parser);

            fxsh_ast_node_t *node = alloc_node(AST_MATCH, tok->loc);
            node->data.match_expr.expr = expr;
            node->data.match_expr.arms = arms;
            return node;
        }
        default:
            parser_error_at(parser, tok->loc, "unexpected %s", fxsh_token_kind_name(tok->kind));
            if (!check(parser, TOK_EOF))
                advance(parser);
            return NULL;
    }
}

static fxsh_ast_node_t *parse_app_arg_primary(fxsh_parser_t *parser) {
    fxsh_token_t *tok = current(parser);
    if (tok->kind == TOK_TYPE_IDENT) {
        advance(parser);
        fxsh_ast_node_t *node = alloc_node(AST_CONSTR_APPL, tok->loc);
        node->data.constr_appl.constr_name = tok->data.ident;
        node->data.constr_appl.args = SP_NULLPTR;
        return node;
    }
    return parse_primary(parser);
}

/*=============================================================================
 * Postfix Expressions (calls, field access)
 *=============================================================================*/

static fxsh_ast_node_t *parse_postfix(fxsh_parser_t *parser) {
    fxsh_ast_node_t *expr = parse_primary(parser);
    if (!expr)
        return NULL;

    while (true) {
        if (check(parser, TOK_LPAREN)) {
            /* Function call (or constructor arg list for qualified constructors). */
            fxsh_loc_t loc = current(parser)->loc;
            advance(parser); /* consume ( */

            fxsh_ast_list_t args = SP_NULLPTR;
            skip_newlines(parser);

            if (check(parser, TOK_RPAREN)) {
                /* f() desugars to f(()) so unit-arity functions can be called naturally. */
                sp_dyn_array_push(args, alloc_node(AST_LIT_UNIT, loc));
            } else {
                while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
                    sp_dyn_array_push(args, parse_expr(parser));
                    skip_newlines(parser);
                    if (!match(parser, TOK_COMMA))
                        break;
                    skip_newlines(parser);
                }
            }

            consume(parser, TOK_RPAREN, "')'");

            if (expr->kind == AST_CONSTR_APPL) {
                sp_dyn_array_for(args, i)
                    sp_dyn_array_push(expr->data.constr_appl.args, args[i]);
            } else {
                expr = fxsh_ast_call(expr, args, loc);
            }
        } else if (match(parser, TOK_DOT)) {
            /* Field access */
            fxsh_loc_t loc = current(parser)->loc;
            fxsh_token_t *field = consume_name_token(parser, "field name");
            if (!field)
                return NULL;

            fxsh_ast_node_t *node = alloc_node(AST_FIELD_ACCESS, loc);
            node->data.field.object = expr;
            node->data.field.field = field->data.ident;
            expr = rewrite_module_chain_expr(parser, node);
        } else if (!prev_token_is_newline(parser) && is_app_arg_start(current(parser)->kind)) {
            /* Function application by juxtaposition: f x y */
            fxsh_loc_t loc = current(parser)->loc;
            fxsh_ast_node_t *arg = parse_app_arg_primary(parser);
            if (!arg)
                return expr;
            while (true) {
                if (check(parser, TOK_LPAREN) && current_token_is_tightly_attached(parser)) {
                    fxsh_loc_t cloc = current(parser)->loc;
                    advance(parser);
                    fxsh_ast_list_t cargs = SP_NULLPTR;
                    skip_newlines(parser);
                    if (check(parser, TOK_RPAREN)) {
                        sp_dyn_array_push(cargs, alloc_node(AST_LIT_UNIT, cloc));
                    } else {
                        while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
                            sp_dyn_array_push(cargs, parse_expr(parser));
                            skip_newlines(parser);
                            if (!match(parser, TOK_COMMA))
                                break;
                            skip_newlines(parser);
                        }
                    }
                    consume(parser, TOK_RPAREN, "')'");
                    arg = fxsh_ast_call(arg, cargs, cloc);
                } else if (check(parser, TOK_DOT) && current_token_is_tightly_attached(parser)) {
                    advance(parser);
                    fxsh_loc_t floc = current(parser)->loc;
                    fxsh_token_t *field = consume_name_token(parser, "field name");
                    if (!field)
                        return expr;
                    fxsh_ast_node_t *node = alloc_node(AST_FIELD_ACCESS, floc);
                    node->data.field.object = arg;
                    node->data.field.field = field->data.ident;
                    arg = rewrite_module_chain_expr(parser, node);
                } else {
                    break;
                }
            }
            if (expr->kind == AST_CONSTR_APPL) {
                sp_dyn_array_push(expr->data.constr_appl.args, arg);
            } else {
                fxsh_ast_list_t args = SP_NULLPTR;
                sp_dyn_array_push(args, arg);
                expr = fxsh_ast_call(expr, args, loc);
            }
        } else {
            break;
        }
    }

    return expr;
}

/*=============================================================================
 * Unary Expressions
 *=============================================================================*/

static fxsh_ast_node_t *parse_unary(fxsh_parser_t *parser) {
    if (check(parser, TOK_MINUS) || check(parser, TOK_NOT)) {
        fxsh_token_t *tok = advance(parser);
        fxsh_ast_node_t *operand = parse_unary(parser);

        fxsh_ast_node_t *node = alloc_node(AST_UNARY, tok->loc);
        node->data.unary.op = tok->kind;
        node->data.unary.operand = operand;
        return node;
    }

    return parse_postfix(parser);
}

/*=============================================================================
 * Multiplicative Expressions
 *=============================================================================*/

static fxsh_ast_node_t *parse_multiplicative(fxsh_parser_t *parser) {
    fxsh_ast_node_t *left = parse_unary(parser);
    if (!left)
        return NULL;

    skip_newlines(parser);
    while (check(parser, TOK_STAR) || check(parser, TOK_SLASH) || check(parser, TOK_PERCENT)) {
        fxsh_token_t *tok = advance(parser);
        skip_newlines(parser);
        fxsh_ast_node_t *right = parse_unary(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
        skip_newlines(parser);
    }

    return left;
}

/*=============================================================================
 * Additive Expressions
 *=============================================================================*/

static fxsh_ast_node_t *parse_additive(fxsh_parser_t *parser) {
    fxsh_ast_node_t *left = parse_multiplicative(parser);
    if (!left)
        return NULL;

    skip_newlines(parser);
    while (check(parser, TOK_PLUS) || check(parser, TOK_MINUS) || check(parser, TOK_CONCAT)) {
        fxsh_token_t *tok = advance(parser);
        skip_newlines(parser);
        fxsh_ast_node_t *right = parse_multiplicative(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
        skip_newlines(parser);
    }

    return left;
}

/*=============================================================================
 * List Cons Expressions (right-associative)
 *=============================================================================*/

static fxsh_ast_node_t *parse_append_expr(fxsh_parser_t *parser) {
    fxsh_ast_node_t *left = parse_additive(parser);
    if (!left)
        return NULL;
    skip_newlines(parser);
    if (match(parser, TOK_APPEND)) {
        fxsh_token_t *tok = &parser->tokens[parser->pos - 1];
        skip_newlines(parser);
        fxsh_ast_node_t *right = parse_append_expr(parser);
        return fxsh_ast_binary(tok->kind, left, right, tok->loc);
    }
    return left;
}

/*=============================================================================
 * Comparison Expressions
 *=============================================================================*/

static fxsh_ast_node_t *parse_comparison(fxsh_parser_t *parser) {
    fxsh_ast_node_t *left = parse_append_expr(parser);
    if (!left)
        return NULL;

    fxsh_token_kind_t cmp_ops[] = {TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LEQ, TOK_GEQ};

    skip_newlines(parser);
    while (check_any(parser, cmp_ops, 6)) {
        fxsh_token_t *tok = advance(parser);
        skip_newlines(parser);
        fxsh_ast_node_t *right = parse_append_expr(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
        skip_newlines(parser);
    }

    return left;
}

/*=============================================================================
 * Logical AND/OR
 *=============================================================================*/

static fxsh_ast_node_t *parse_logical(fxsh_parser_t *parser) {
    fxsh_ast_node_t *left = parse_comparison(parser);
    if (!left)
        return NULL;

    skip_newlines(parser);
    while (check(parser, TOK_AND) || check(parser, TOK_OR)) {
        fxsh_token_t *tok = advance(parser);
        skip_newlines(parser);
        fxsh_ast_node_t *right = parse_comparison(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
        skip_newlines(parser);
    }

    return left;
}

/*=============================================================================
 * Pipe Expression
 *=============================================================================*/

static fxsh_ast_node_t *parse_pipe(fxsh_parser_t *parser) {
    fxsh_ast_node_t *left = parse_logical(parser);
    if (!left)
        return NULL;

    skip_newlines(parser);
    while (match(parser, TOK_PIPE)) {
        fxsh_loc_t loc = current(parser)->loc;
        skip_newlines(parser);
        fxsh_ast_node_t *right = parse_logical(parser);

        fxsh_ast_node_t *node = alloc_node(AST_PIPE, loc);
        node->data.pipe.left = left;
        node->data.pipe.right = right;
        left = node;
        skip_newlines(parser);
    }

    return left;
}

/*=============================================================================
 * Main Expression Entry Point
 *=============================================================================*/

static fxsh_ast_node_t *parse_expr(fxsh_parser_t *parser) {
    skip_newlines(parser);
    return parse_pipe(parser);
}

/*=============================================================================
 * Type Definition Parsing (ADT)
 *=============================================================================*/

static fxsh_ast_node_t *parse_type_def(fxsh_parser_t *parser) {
    fxsh_token_t *tok = advance(parser); /* consume 'type' */
    skip_newlines(parser);

    /* Parse optional type parameters ('a, 'b) */
    fxsh_ast_list_t type_params = SP_NULLPTR;
    while (check(parser, TOK_IDENT) && current(parser)->data.ident.data[0] == '\'') {
        fxsh_token_t *param = advance(parser);
        fxsh_ast_node_t *param_node = alloc_node(AST_TYPE_VAR, param->loc);
        param_node->data.ident = param->data.ident;
        sp_dyn_array_push(type_params, param_node);
        skip_newlines(parser);
    }

    /* Parse type name */
    fxsh_token_t *name_tok = consume(parser, TOK_IDENT, "type name");
    if (!name_tok)
        return NULL;

    skip_newlines(parser);
    consume(parser, TOK_ASSIGN, "'='");
    skip_newlines(parser);

    /* Parse constructors separated by | */
    fxsh_ast_list_t constructors = SP_NULLPTR;
    do {
        skip_newlines(parser);

        /* Parse constructor name (can be TOK_IDENT or TOK_TYPE_IDENT) */
        fxsh_token_t *constr_name = NULL;
        if (check(parser, TOK_IDENT) || check(parser, TOK_TYPE_IDENT)) {
            constr_name = advance(parser);
        } else {
            consume(parser, TOK_IDENT, "constructor name");
            return NULL;
        }

        /* Parse optional 'of' and argument types */
        fxsh_ast_list_t arg_types = SP_NULLPTR;
        if (match(parser, TOK_OF)) {
            skip_newlines(parser);

            /* Parse at least one argument type */
            do {
                skip_newlines(parser);
                fxsh_ast_node_t *arg_type = parse_type_expr(parser);
                if (arg_type)
                    sp_dyn_array_push(arg_types, arg_type);

                skip_newlines(parser);
            } while (match(parser, TOK_STAR) || match(parser, TOK_COMMA));
        }

        fxsh_ast_node_t *constr = alloc_node(AST_DATA_CONSTR, constr_name->loc);
        constr->data.data_constr.name = constr_name->data.ident;
        constr->data.data_constr.arg_types = arg_types;
        sp_dyn_array_push(constructors, constr);

        skip_newlines(parser);
    } while (match(parser, TOK_PIPE_SYM));

    fxsh_ast_node_t *node = alloc_node(AST_TYPE_DEF, tok->loc);
    node->data.type_def.name = name_tok->data.ident;
    node->data.type_def.type_params = type_params;
    node->data.type_def.constructors = constructors;
    return node;
}

/*=============================================================================
 * Declaration Parsing
 *=============================================================================*/

static fxsh_ast_node_t *parse_let_decl(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_trait_decl(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_impl_decl(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_module_decl(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_import_decl(fxsh_parser_t *parser);

static fxsh_ast_node_t *parse_decl(fxsh_parser_t *parser) {
    skip_newlines(parser);

    if (check(parser, TOK_EOF) || check(parser, TOK_RBRACE) || check(parser, TOK_END)) {
        return NULL;
    }

    /* Type definition */
    if (check(parser, TOK_TYPE)) {
        return parse_type_def(parser);
    }

    if (check(parser, TOK_MODULE)) {
        return parse_module_decl(parser);
    }

    if (check(parser, TOK_IMPORT)) {
        return parse_import_decl(parser);
    }

    if (check(parser, TOK_TRAIT)) {
        return parse_trait_decl(parser);
    }

    if (check(parser, TOK_IMPL)) {
        return parse_impl_decl(parser);
    }

    /* Let declaration at top level */
    if (check(parser, TOK_LET)) {
        u32 saved_pos = parser->pos;
        fxsh_ast_node_t *let_decl = parse_let_decl(parser);
        if (let_decl && !check(parser, TOK_IN)) {
            return let_decl;
        }
        /* It was likely a let-in expression: rollback and parse as expression. */
        parser->pos = saved_pos;
        return parse_expr(parser);
    }

    /* For now, just parse as expression */
    return parse_expr(parser);
}

static fxsh_ast_node_t *parse_trait_decl(fxsh_parser_t *parser) {
    fxsh_token_t *tok = advance(parser); /* trait */
    skip_newlines(parser);

    fxsh_token_t *name_tok = consume_name_token(parser, "trait name");
    if (!name_tok)
        return NULL;

    skip_newlines(parser);
    consume(parser, TOK_ASSIGN, "'='");
    skip_newlines(parser);

    bool brace_body = false;
    if (!parse_brace_or_struct_body_start(parser, "'struct' or '{'", &brace_body))
        return NULL;
    skip_newlines(parser);

    fxsh_ast_list_t methods = SP_NULLPTR;
    while (!check(parser, TOK_EOF)) {
        skip_newlines(parser);
        if (brace_body && check(parser, TOK_RBRACE))
            break;
        if (!brace_body && check(parser, TOK_END))
            break;

        if (!check(parser, TOK_LET)) {
            parser_error_at(parser, current(parser)->loc,
                            "trait body only supports `let name: type` signatures");
            return NULL;
        }

        advance(parser); /* let */
        skip_newlines(parser);
        fxsh_token_t *method_tok = consume_name_token(parser, "trait method name");
        if (!method_tok)
            return NULL;
        skip_newlines(parser);
        consume(parser, TOK_COLON, "':'");
        skip_newlines(parser);
        fxsh_ast_node_t *method_type = parse_type_expr(parser);
        if (!method_type)
            return NULL;
        if (trait_has_method_name(methods, method_tok->data.ident)) {
            parser_error_at(parser, method_tok->loc, "duplicate trait method `%.*s`",
                            method_tok->data.ident.len, method_tok->data.ident.data);
            return NULL;
        }
        fxsh_ast_node_t *sig =
            fxsh_ast_let(method_tok->data.ident, NULL, false, false, method_tok->loc);
        sig->data.let.type = method_type;
        sp_dyn_array_push(methods, sig);

        skip_newlines(parser);
    }

    parse_brace_or_struct_body_end(parser, brace_body);
    parser_register_trait(parser, name_tok->data.ident, methods);
    return fxsh_ast_program(SP_NULLPTR, tok->loc);
}

static fxsh_ast_node_t *parse_impl_decl(fxsh_parser_t *parser) {
    fxsh_token_t *tok = advance(parser); /* impl */
    skip_newlines(parser);

    fxsh_token_t *trait_tok = consume_name_token(parser, "trait name");
    if (!trait_tok)
        return NULL;
    fxsh_trait_decl_t *trait = parser_lookup_trait(parser, trait_tok->data.ident);
    if (!trait) {
        parser_error_at(parser, trait_tok->loc, "unknown trait `%.*s`", trait_tok->data.ident.len,
                        trait_tok->data.ident.data);
        return NULL;
    }

    skip_newlines(parser);
    consume(parser, TOK_FOR, "'for'");
    skip_newlines(parser);
    fxsh_ast_node_t *self_type = parse_type_expr(parser);
    if (!self_type)
        return NULL;
    if (self_type->kind != AST_IDENT) {
        parser_error_at(parser, self_type->loc,
                        "impl target must be a named type in MVP trait support");
        return NULL;
    }

    skip_newlines(parser);
    consume(parser, TOK_ASSIGN, "'='");
    skip_newlines(parser);

    bool brace_body = false;
    if (!parse_brace_or_struct_body_start(parser, "'struct' or '{'", &brace_body))
        return NULL;
    skip_newlines(parser);

    sp_str_t impl_module = mk_qualified_name(trait_tok->data.ident, self_type->data.ident);
    parser_add_name(&parser->modules, impl_module);

    fxsh_ast_list_t members = SP_NULLPTR;
    while (!check(parser, TOK_EOF)) {
        skip_newlines(parser);
        if (brace_body && check(parser, TOK_RBRACE))
            break;
        if (!brace_body && check(parser, TOK_END))
            break;

        if (!check(parser, TOK_LET)) {
            parser_error_at(parser, current(parser)->loc, "impl body only supports `let` methods");
            return NULL;
        }

        fxsh_ast_node_t *method = parse_let_decl(parser);
        if (!method)
            return NULL;
        if (method->data.let.is_comptime) {
            parser_error_at(parser, method->loc, "impl methods cannot be comptime");
            return NULL;
        }
        if (trait_has_method_name(members, method->data.let.name)) {
            parser_error_at(parser, method->loc, "duplicate impl method `%.*s`",
                            method->data.let.name.len, method->data.let.name.data);
            return NULL;
        }
        sp_dyn_array_push(members, method);
        skip_newlines(parser);
    }

    parse_brace_or_struct_body_end(parser, brace_body);

    sp_dyn_array_for(trait->methods, i) {
        fxsh_ast_node_t *sig = trait->methods[i];
        if (!trait_has_method_name(members, sig->data.let.name)) {
            parser_error_at(parser, tok->loc, "impl `%.*s` for `%.*s` is missing method `%.*s`",
                            trait_tok->data.ident.len, trait_tok->data.ident.data,
                            self_type->data.ident.len, self_type->data.ident.data,
                            sig->data.let.name.len, sig->data.let.name.data);
            return NULL;
        }
    }

    sp_dyn_array_for(members, i) {
        fxsh_ast_node_t *method = members[i];
        if (!trait_lookup_method_sig(trait, method->data.let.name)) {
            parser_error_at(parser, method->loc,
                            "impl method `%.*s` is not declared in trait `%.*s`",
                            method->data.let.name.len, method->data.let.name.data,
                            trait_tok->data.ident.len, trait_tok->data.ident.data);
            return NULL;
        }
    }

    sp_dyn_array(name_map_t) map = SP_NULLPTR;
    sp_dyn_array_for(members, i) {
        fxsh_ast_node_t *method = members[i];
        sp_str_t qualified_name = mk_qualified_name(impl_module, method->data.let.name);
        sp_dyn_array_push(map, ((name_map_t){.from = method->data.let.name, .to = qualified_name}));
    }

    sp_dyn_array(sp_str_t) bound = SP_NULLPTR;
    sp_dyn_array_for(members, i) {
        fxsh_ast_node_t *method = members[i];
        rewrite_module_refs(method->data.let.value, map, &bound);
        fxsh_ast_node_t *sig = trait_lookup_method_sig(trait, method->data.let.name);
        method->data.let.type = substitute_self_type_ast(sig->data.let.type, self_type);
        rewrite_module_type_refs(method->data.let.type, map);
        sp_str_t qualified_name = map_lookup(map, method->data.let.name);
        if (qualified_name.data) {
            method->data.let.name = qualified_name;
            if (method->data.let.pattern && method->data.let.pattern->kind == AST_PAT_VAR)
                method->data.let.pattern->data.ident = qualified_name;
        }
    }

    return fxsh_ast_program(members, tok->loc);
}

static fxsh_ast_node_t *parse_import_decl(fxsh_parser_t *parser) {
    fxsh_token_t *tok = advance(parser); /* import */
    skip_newlines(parser);
    sp_str_t module_name = parse_qualified_name_path(parser, "module name");
    if (!module_name.data)
        return NULL;
    parser_add_name(&parser->imports, module_name);

    fxsh_ast_node_t *n = alloc_node(AST_DECL_IMPORT, tok->loc);
    n->data.decl_import.module_name = module_name;
    return n;
}

static fxsh_ast_node_t *parse_module_decl(fxsh_parser_t *parser) {
    fxsh_token_t *tok = advance(parser); /* module */
    skip_newlines(parser);
    fxsh_token_t *name_tok = consume_name_token(parser, "module name");
    if (!name_tok)
        return NULL;
    skip_newlines(parser);
    consume(parser, TOK_ASSIGN, "'='");
    skip_newlines(parser);

    bool brace_body = false;
    if (match(parser, TOK_STRUCT)) {
        brace_body = false;
    } else if (match(parser, TOK_LBRACE)) {
        brace_body = true;
    } else {
        consume(parser, TOK_STRUCT, "'struct' or '{'");
        return NULL;
    }
    skip_newlines(parser);

    sp_str_t mname = name_tok->data.ident;
    parser_add_name(&parser->modules, mname);

    fxsh_ast_list_t members = SP_NULLPTR;
    while (!check(parser, TOK_END) && !check(parser, TOK_EOF)) {
        skip_newlines(parser);
        if (brace_body && check(parser, TOK_RBRACE))
            break;
        fxsh_ast_node_t *d = parse_decl(parser);
        if (!d)
            break;
        if ((d->kind == AST_DECL_LET || d->kind == AST_LET) && d->data.let.name.data) {
            sp_dyn_array_push(members, d);
        } else if (d->kind == AST_TYPE_DEF && d->data.type_def.name.data) {
            sp_dyn_array_push(members, d);
        } else if (d->kind == AST_PROGRAM) {
            sp_dyn_array_for(d->data.decls, i) {
                fxsh_ast_node_t *id = d->data.decls[i];
                if ((id->kind == AST_DECL_LET || id->kind == AST_LET) && id->data.let.name.data) {
                    sp_dyn_array_push(members, id);
                } else if (id->kind == AST_TYPE_DEF && id->data.type_def.name.data) {
                    sp_dyn_array_push(members, id);
                }
            }
        }
        skip_newlines(parser);
    }
    if (brace_body) {
        consume(parser, TOK_RBRACE, "'}'");
    } else {
        consume(parser, TOK_END, "'end'");
    }

    sp_dyn_array(name_map_t) map = SP_NULLPTR;
    sp_dyn_array_for(members, i) {
        fxsh_ast_node_t *d = members[i];
        if (d->kind == AST_DECL_LET || d->kind == AST_LET) {
            sp_str_t old_name = d->data.let.name;
            sp_str_t new_name = mk_qualified_name(mname, old_name);
            name_map_t e = {.from = old_name, .to = new_name};
            sp_dyn_array_push(map, e);
        } else if (d->kind == AST_TYPE_DEF) {
            sp_str_t old_t = d->data.type_def.name;
            sp_str_t new_t = mk_qualified_name(mname, old_t);
            name_map_t te = {.from = old_t, .to = new_t};
            sp_dyn_array_push(map, te);
            sp_dyn_array_for(d->data.type_def.constructors, ci) {
                fxsh_ast_node_t *c = d->data.type_def.constructors[ci];
                if (!c || c->kind != AST_DATA_CONSTR)
                    continue;
                sp_str_t old_c = c->data.data_constr.name;
                sp_str_t new_c = mk_qualified_name(mname, old_c);
                name_map_t ce = {.from = old_c, .to = new_c};
                sp_dyn_array_push(map, ce);
            }
        }
    }

    sp_dyn_array(sp_str_t) bound = SP_NULLPTR;
    sp_dyn_array_for(members, i) {
        fxsh_ast_node_t *d = members[i];
        if (d->kind == AST_DECL_LET || d->kind == AST_LET) {
            rewrite_module_refs(d->data.let.value, map, &bound);
            rewrite_module_type_refs(d->data.let.type, map);
            sp_str_t q = map_lookup(map, d->data.let.name);
            if (q.data) {
                d->data.let.name = q;
                if (d->data.let.pattern && d->data.let.pattern->kind == AST_PAT_VAR)
                    d->data.let.pattern->data.ident = q;
            }
        } else if (d->kind == AST_TYPE_DEF) {
            sp_str_t q = map_lookup(map, d->data.type_def.name);
            if (q.data)
                d->data.type_def.name = q;
            sp_dyn_array_for(d->data.type_def.constructors, ci) {
                fxsh_ast_node_t *c = d->data.type_def.constructors[ci];
                if (!c || c->kind != AST_DATA_CONSTR)
                    continue;
                sp_str_t cq = map_lookup(map, c->data.data_constr.name);
                if (cq.data)
                    c->data.data_constr.name = cq;
                sp_dyn_array_for(c->data.data_constr.arg_types, ai)
                    rewrite_module_type_refs(c->data.data_constr.arg_types[ai], map);
            }
        }
    }

    fxsh_ast_node_t *prog = alloc_node(AST_PROGRAM, tok->loc);
    prog->data.decls = members;
    return prog;
}

/* Parse top-level let declaration (without 'in' part) */
static fxsh_ast_node_t *parse_let_decl(fxsh_parser_t *parser) {
    advance(parser); /* consume let */
    skip_newlines(parser);

    bool is_comptime = match(parser, TOK_COMPTIME);
    bool is_rec = match(parser, TOK_REC);

    fxsh_token_t *name_tok = consume_name_token(parser, "identifier");
    if (!name_tok)
        return NULL;

    /* Parse function parameters if present (function definition sugar: let f x y = ...) */
    fxsh_ast_list_t params = SP_NULLPTR;
    while (is_pattern_start(current(parser)->kind)) {
        fxsh_ast_node_t *pat = parse_pattern(parser);
        if (!pat)
            break;
        sp_dyn_array_push(params, pat);
        skip_newlines(parser);
    }

    fxsh_ast_node_t *type_ann = NULL;
    if (match(parser, TOK_COLON)) {
        skip_newlines(parser);
        type_ann = parse_type_expr(parser);
    }

    consume(parser, TOK_ASSIGN, "'='");

    fxsh_ast_node_t *value = parse_expr(parser);

    /* If we have params, wrap value in curried lambda */
    if (params && sp_dyn_array_size(params) > 0) {
        for (int i = (int)sp_dyn_array_size(params) - 1; i >= 0; i--) {
            fxsh_ast_list_t single_param = SP_NULLPTR;
            sp_dyn_array_push(single_param, params[i]);
            value = fxsh_ast_lambda(single_param, value, name_tok->loc);
        }
    }

    fxsh_ast_node_t *n =
        fxsh_ast_let(name_tok->data.ident, value, is_comptime, is_rec, name_tok->loc);
    n->data.let.type = type_ann;
    return n;
}

/*=============================================================================
 * Program Parsing
 *=============================================================================*/

fxsh_ast_node_t *fxsh_parse_program(fxsh_parser_t *parser) {
    fxsh_ast_list_t decls = SP_NULLPTR;

    skip_newlines(parser);

    while (!check(parser, TOK_EOF)) {
        u32 start_pos = parser->pos;
        fxsh_ast_node_t *decl = parse_decl(parser);
        if (decl) {
            if (decl->kind == AST_PROGRAM) {
                sp_dyn_array_for(decl->data.decls, i) {
                    sp_dyn_array_push(decls, decl->data.decls[i]);
                }
            } else {
                sp_dyn_array_push(decls, decl);
            }
        }
        if (parser->pos == start_pos) {
            synchronize(parser);
        }
        skip_newlines(parser);
    }

    fxsh_ast_node_t *prog = fxsh_ast_program(decls, current(parser)->loc);
    normalize_constructor_tuple_sugar(prog);
    return prog;
}

fxsh_ast_node_t *fxsh_parse(fxsh_parser_t *parser) {
    return fxsh_parse_program(parser);
}
