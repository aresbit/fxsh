/*
 * parser.c - fxsh recursive descent parser
 */

#include "fxsh.h"

#include <stdio.h>
#include <string.h>

/*=============================================================================
 * Parser State
 *=============================================================================*/

void fxsh_parser_init(fxsh_parser_t *parser, fxsh_token_array_t tokens) {
    parser->tokens = tokens;
    parser->pos = 0;
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
    return NULL;
}

static inline fxsh_token_t *consume_name_token(fxsh_parser_t *parser, const char *msg) {
    if (check(parser, TOK_IDENT) || check(parser, TOK_TYPE_IDENT)) {
        return advance(parser);
    }
    fprintf(stderr, "Parse error at %d:%d: expected %s, got %s\n", current(parser)->loc.line,
            current(parser)->loc.column, msg, fxsh_token_kind_name(current(parser)->kind));
    return NULL;
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
    node->data.let.pattern = fxsh_ast_ident(name, loc);
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
            sp_dyn_array_for(node->data.elements, i) {
                fxsh_ast_free(node->data.elements[i]);
            }
            sp_dyn_array_free(node->data.elements);
            break;
        case AST_FIELD_ACCESS:
            fxsh_ast_free(node->data.field.object);
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
static fxsh_ast_node_t *parse_decl(fxsh_parser_t *parser);
static fxsh_ast_node_t *parse_primary(fxsh_parser_t *parser);

/*=============================================================================
 * Pattern Parsing
 *=============================================================================*/

static bool is_pattern_start(fxsh_token_kind_t kind) {
    return kind == TOK_IDENT || kind == TOK_TYPE_IDENT || kind == TOK_INT || kind == TOK_FLOAT ||
           kind == TOK_STRING || kind == TOK_TRUE || kind == TOK_FALSE || kind == TOK_LPAREN ||
           kind == TOK_LBRACKET || kind == TOK_LBRACE;
}

static fxsh_ast_node_t *parse_pattern(fxsh_parser_t *parser) {
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
        fxsh_ast_node_t *lit = parse_primary(parser); /* parse_primary handles literals */
        /* Convert literal expression to pattern node by changing its kind */
        lit->kind = AST_PAT_LIT;
        return lit;
    }

    /* Constructor pattern: uppercase identifier (type ident) */
    if (tok->kind == TOK_TYPE_IDENT) {
        advance(parser);
        sp_str_t constr_name = tok->data.ident;

        /* Parse nested patterns for constructor arguments */
        fxsh_ast_list_t args = SP_NULLPTR;
        while (!check(parser, TOK_NEWLINE) && !check(parser, TOK_EOF) &&
               !check(parser, TOK_PIPE_SYM) && !check(parser, TOK_WITH) &&
               !check(parser, TOK_END) && !check(parser, TOK_ELSE) && !check(parser, TOK_IN) &&
               !check(parser, TOK_THEN) && !check(parser, TOK_RPAREN) &&
               !check(parser, TOK_RBRACKET) && !check(parser, TOK_RBRACE) &&
               !check(parser, TOK_COMMA) && !check(parser, TOK_ARROW) &&
               !check(parser, TOK_FAT_ARROW)) {
            fxsh_ast_node_t *arg = parse_pattern(parser);
            if (!arg)
                break;
            /* Constructor tuple sugar in patterns:
             *   Cons(x, xs)  => args [x, xs] */
            if (arg->kind == AST_PAT_TUPLE) {
                sp_dyn_array_for(arg->data.elements, i) {
                    sp_dyn_array_push(args, arg->data.elements[i]);
                }
                sp_free(arg);
            } else {
                sp_dyn_array_push(args, arg);
            }
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

    /* List cons pattern: p1 :: p2 (right-associative) */
    /* TODO: implement later */

    /* Record pattern: {field1 = p1, field2 = p2} */
    /* TODO: implement later */

    /* Fallback: parse as expression (for now) */
    return parse_primary(parser);
}

static bool is_app_arg_start(fxsh_token_kind_t kind) {
    switch (kind) {
        case TOK_INT:
        case TOK_FLOAT:
        case TOK_STRING:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_IDENT:
        case TOK_TYPE_IDENT:
        case TOK_LPAREN:
        case TOK_LBRACKET:
        case TOK_LBRACE:
        case TOK_IF:
        case TOK_LET:
        case TOK_MATCH:
        case TOK_FN:
        case TOK_AT:
            return true;
        default:
            return false;
    }
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
                   !check(parser, TOK_THEN) && !check(parser, TOK_RPAREN) &&
                   !check(parser, TOK_RBRACKET) && !check(parser, TOK_RBRACE) &&
                   !check(parser, TOK_COMMA) && !check(parser, TOK_ARROW) &&
                   !check(parser, TOK_FAT_ARROW)) {
                fxsh_ast_node_t *arg = parse_primary(parser);
                if (!arg)
                    break;

                /* If argument is a tuple, flatten it into multiple args */
                if (arg->kind == AST_TUPLE) {
                    sp_dyn_array_for(arg->data.elements, i) {
                        sp_dyn_array_push(args, arg->data.elements[i]);
                    }
                    /* Free the tuple node but not its elements (now in args) */
                    sp_free(arg);
                } else {
                    sp_dyn_array_push(args, arg);
                }
            }

            fxsh_ast_node_t *node = alloc_node(AST_CONSTR_APPL, tok->loc);
            node->data.constr_appl.constr_name = constr_name;
            node->data.constr_appl.args = args;
            return node;
        }
        case TOK_AT: {
            advance(parser); /* consume @ */
            fxsh_token_t *op_tok = consume(parser, TOK_IDENT, "compile-time operator name");
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
            } else {
                fprintf(stderr, "Unknown compile-time operator: %.*s\n", op_name.len, op_name.data);
                return NULL;
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
                if (!match(parser, TOK_COMMA))
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

            fxsh_ast_list_t fields = SP_NULLPTR;

            while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
                /* Parse field: name = expr or name: type = expr */
                fxsh_token_t *field_tok = consume(parser, TOK_IDENT, "field name");
                if (!field_tok)
                    return NULL;

                sp_str_t field_name = field_tok->data.ident;

                /* TODO: Parse field type annotation if present */

                consume(parser, TOK_ASSIGN, "'='");

                fxsh_ast_node_t *field_value = parse_expr(parser);

                fxsh_ast_node_t *field_node = alloc_node(AST_FIELD_ACCESS, field_tok->loc);
                field_node->data.field.object = NULL;
                field_node->data.field.field = field_name;
                /* Store value in a different way - for now, store as tuple node */

                skip_newlines(parser);
                if (!match(parser, TOK_COMMA))
                    break;
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

            fxsh_ast_node_t *else_branch = NULL;
            skip_newlines(parser);
            if (match(parser, TOK_ELSE)) {
                else_branch = parse_expr(parser);
            }
            maybe_consume_block_end(parser);

            return fxsh_ast_if(cond, then_branch, else_branch, tok->loc);
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
                while (check(parser, TOK_IDENT) || check(parser, TOK_TYPE_IDENT)) {
                    fxsh_token_t *param_tok = advance(parser);
                    fxsh_ast_node_t *pat = fxsh_ast_ident(param_tok->data.ident, param_tok->loc);
                    sp_dyn_array_push(params, pat);
                    skip_newlines(parser);
                }

                /* TODO: Parse type annotation if present */

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
                sp_dyn_array_push(bindings, binding);

                skip_newlines(parser);
            }

            consume(parser, TOK_IN, "'in'");

            fxsh_ast_node_t *body = parse_expr(parser);
            maybe_consume_block_end(parser);

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
            fprintf(stderr, "Parse error at %d:%d: unexpected %s\n", tok->loc.line, tok->loc.column,
                    fxsh_token_kind_name(tok->kind));
            if (!check(parser, TOK_EOF))
                advance(parser);
            return NULL;
    }
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
            /* Function call */
            fxsh_loc_t loc = current(parser)->loc;
            advance(parser); /* consume ( */

            fxsh_ast_list_t args = SP_NULLPTR;
            skip_newlines(parser);

            while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
                sp_dyn_array_push(args, parse_expr(parser));
                skip_newlines(parser);
                if (!match(parser, TOK_COMMA))
                    break;
                skip_newlines(parser);
            }

            consume(parser, TOK_RPAREN, "')'");

            expr = fxsh_ast_call(expr, args, loc);
        } else if (match(parser, TOK_DOT)) {
            /* Field access */
            fxsh_loc_t loc = current(parser)->loc;
            fxsh_token_t *field = consume(parser, TOK_IDENT, "field name");
            if (!field)
                return NULL;

            fxsh_ast_node_t *node = alloc_node(AST_FIELD_ACCESS, loc);
            node->data.field.object = expr;
            node->data.field.field = field->data.ident;
            expr = node;
        } else if (is_app_arg_start(current(parser)->kind)) {
            /* Function application by juxtaposition: f x y */
            fxsh_loc_t loc = current(parser)->loc;
            fxsh_ast_node_t *arg = parse_primary(parser);
            if (!arg)
                return expr;
            fxsh_ast_list_t args = SP_NULLPTR;
            sp_dyn_array_push(args, arg);
            expr = fxsh_ast_call(expr, args, loc);
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

    while (check(parser, TOK_STAR) || check(parser, TOK_SLASH) || check(parser, TOK_PERCENT)) {
        fxsh_token_t *tok = advance(parser);
        fxsh_ast_node_t *right = parse_unary(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
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

    while (check(parser, TOK_PLUS) || check(parser, TOK_MINUS) || check(parser, TOK_CONCAT)) {
        fxsh_token_t *tok = advance(parser);
        fxsh_ast_node_t *right = parse_multiplicative(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
    }

    return left;
}

/*=============================================================================
 * Comparison Expressions
 *=============================================================================*/

static fxsh_ast_node_t *parse_comparison(fxsh_parser_t *parser) {
    fxsh_ast_node_t *left = parse_additive(parser);
    if (!left)
        return NULL;

    fxsh_token_kind_t cmp_ops[] = {TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LEQ, TOK_GEQ};

    while (check_any(parser, cmp_ops, 6)) {
        fxsh_token_t *tok = advance(parser);
        fxsh_ast_node_t *right = parse_additive(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
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

    while (check(parser, TOK_AND) || check(parser, TOK_OR)) {
        fxsh_token_t *tok = advance(parser);
        fxsh_ast_node_t *right = parse_comparison(parser);
        left = fxsh_ast_binary(tok->kind, left, right, tok->loc);
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

    while (match(parser, TOK_PIPE)) {
        fxsh_loc_t loc = current(parser)->loc;
        fxsh_ast_node_t *right = parse_logical(parser);

        fxsh_ast_node_t *node = alloc_node(AST_PIPE, loc);
        node->data.pipe.left = left;
        node->data.pipe.right = right;
        left = node;
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
                /* Parse simple type identifiers, not full expressions */
                /* This handles 'int', 'list', etc. as type names */
                fxsh_ast_node_t *arg_type = NULL;
                if (check(parser, TOK_IDENT) || check(parser, TOK_TYPE_IDENT)) {
                    fxsh_token_t *type_tok = advance(parser);
                    arg_type = fxsh_ast_ident(type_tok->data.ident, type_tok->loc);
                } else {
                    /* Fall back to expression parsing for complex types */
                    arg_type = parse_expr(parser);
                }
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

static fxsh_ast_node_t *parse_decl(fxsh_parser_t *parser) {
    skip_newlines(parser);

    if (check(parser, TOK_EOF)) {
        return NULL;
    }

    /* Type definition */
    if (check(parser, TOK_TYPE)) {
        return parse_type_def(parser);
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
    while (check(parser, TOK_IDENT) || check(parser, TOK_TYPE_IDENT)) {
        fxsh_token_t *param_tok = advance(parser);
        fxsh_ast_node_t *pat = fxsh_ast_ident(param_tok->data.ident, param_tok->loc);
        sp_dyn_array_push(params, pat);
        skip_newlines(parser);
    }

    /* TODO: Parse type annotation if present */

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

    return fxsh_ast_let(name_tok->data.ident, value, is_comptime, is_rec, name_tok->loc);
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
            sp_dyn_array_push(decls, decl);
        }
        if (parser->pos == start_pos) {
            synchronize(parser);
        }
        skip_newlines(parser);
    }

    return fxsh_ast_program(decls, current(parser)->loc);
}

fxsh_ast_node_t *fxsh_parse(fxsh_parser_t *parser) {
    return fxsh_parse_program(parser);
}
