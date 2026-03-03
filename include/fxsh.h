/*
 * fxsh.h - Functional Core Minimal Bash
 * Main header file
 */

#ifndef FXSH_H
#define FXSH_H

#include "../lib/sp.h"

/*=============================================================================
 * Version
 *=============================================================================*/

#define FXSH_VERSION_MAJOR 0
#define FXSH_VERSION_MINOR 1
#define FXSH_VERSION_PATCH 0

/*=============================================================================
 * Error Handling
 *=============================================================================*/

typedef enum {
    ERR_OK = 0,
    ERR_OUT_OF_MEMORY,
    ERR_INVALID_INPUT,
    ERR_SYNTAX_ERROR,
    ERR_TYPE_ERROR,
    ERR_FILE_NOT_FOUND,
    ERR_IO_ERROR,
    ERR_INTERNAL,
} fxsh_error_t;

#define fxsh_try(expr)                                                                             \
    do {                                                                                           \
        fxsh_error_t _e = (expr);                                                                  \
        if (_e != ERR_OK)                                                                          \
            return _e;                                                                             \
    } while (0)

#define fxsh_require(cond, err)                                                                    \
    do {                                                                                           \
        if (!(cond))                                                                               \
            return (err);                                                                          \
    } while (0)

/*=============================================================================
 * Source Location
 *=============================================================================*/

typedef struct {
    sp_str_t filename;
    u32 line;
    u32 column;
} fxsh_loc_t;

static inline fxsh_loc_t fxsh_make_loc(sp_str_t filename, u32 line, u32 col) {
    return (fxsh_loc_t){.filename = filename, .line = line, .column = col};
}
#define FXSH_LOC(filename, line, col) fxsh_make_loc(filename, line, col)

/*=============================================================================
 * Token Types
 *=============================================================================*/

typedef enum {
    /* Literals */
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_BOOL,
    TOK_UNIT,

    /* Identifiers */
    TOK_IDENT,
    TOK_TYPE_IDENT, /* Type identifiers (uppercase start) */

    /* Keywords */
    TOK_LET,
    TOK_FN,
    TOK_IN,
    TOK_END,
    TOK_IF,
    TOK_THEN,
    TOK_ELSE,
    TOK_MATCH,
    TOK_WITH,
    TOK_MODULE,
    TOK_IMPORT,
    TOK_COMPTIME,
    TOK_TYPE,
    TOK_OF,
    TOK_STRUCT,
    TOK_TRAIT,
    TOK_IMPL,
    TOK_FOR,
    TOK_WHILE,
    TOK_RETURN,
    TOK_TRUE,
    TOK_FALSE,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_REC,

    /* Operators */
    TOK_PLUS,      /* + */
    TOK_MINUS,     /* - */
    TOK_STAR,      /* * */
    TOK_SLASH,     /* / */
    TOK_PERCENT,   /* % */
    TOK_ARROW,     /* -> */
    TOK_FAT_ARROW, /* => */
    TOK_PIPE,      /* |> */
    TOK_ASSIGN,    /* = */
    TOK_EQ,        /* == */
    TOK_NEQ,       /* != */
    TOK_LT,        /* < */
    TOK_GT,        /* > */
    TOK_LEQ,       /* <= */
    TOK_GEQ,       /* >= */
    TOK_CONCAT,    /* ++ */
    TOK_APPEND,    /* :: */

    /* Delimiters */
    TOK_LPAREN,    /* ( */
    TOK_RPAREN,    /* ) */
    TOK_LBRACE,    /* { */
    TOK_RBRACE,    /* } */
    TOK_LBRACKET,  /* [ */
    TOK_RBRACKET,  /* ] */
    TOK_COMMA,     /* , */
    TOK_SEMICOLON, /* ; */
    TOK_COLON,     /* : */
    TOK_DOT,       /* . */
    TOK_DOTDOT,    /* .. */
    TOK_PIPE_SYM,  /* | */
    TOK_AT,        /* @ */

    /* Special */
    TOK_NEWLINE,
    TOK_COMMENT,
    TOK_EOF,
    TOK_ERROR,
} fxsh_token_kind_t;

/*=============================================================================
 * Token Structure
 *=============================================================================*/

typedef struct {
    fxsh_token_kind_t kind;
    fxsh_loc_t loc;
    union {
        s64 int_val;
        f64 float_val;
        sp_str_t str_val;
        sp_str_t ident;
    } data;
} fxsh_token_t;

/*=============================================================================
 * Token Array
 *=============================================================================*/

typedef sp_dyn_array(fxsh_token_t) fxsh_token_array_t;

/*=============================================================================
 * Lexer State
 *=============================================================================*/

typedef struct {
    sp_str_t source;
    sp_str_t filename;
    const c8 *cursor;
    const c8 *line_start;
    u32 line;
} fxsh_lexer_t;

/*=============================================================================
 * AST Node Kinds
 *=============================================================================*/

typedef enum {
    /* Expressions */
    AST_LIT_INT,
    AST_LIT_FLOAT,
    AST_LIT_STRING,
    AST_LIT_BOOL,
    AST_LIT_UNIT,
    AST_IDENT,
    AST_TUPLE,
    AST_LIST,
    AST_RECORD,
    AST_FIELD_ACCESS,
    AST_CALL,
    AST_LAMBDA,
    AST_LET,
    AST_LET_IN,
    AST_IF,
    AST_MATCH,
    AST_MATCH_ARM,
    AST_BINARY,
    AST_UNARY,
    AST_PIPE,

    /* Patterns */
    AST_PAT_WILD,   /* _ */
    AST_PAT_VAR,    /* x */
    AST_PAT_LIT,    /* literal */
    AST_PAT_TUPLE,  /* (p1, p2) */
    AST_PAT_CONS,   /* p1 :: p2 */
    AST_PAT_RECORD, /* {x, y} */
    AST_PAT_CONSTR, /* Some x, None */

    /* Types */
    AST_TYPE_VAR,    /* 'a */
    AST_TYPE_CON,    /* int, list */
    AST_TYPE_APP,    /* 'a list */
    AST_TYPE_ARROW,  /* a -> b */
    AST_TYPE_RECORD, /* {x: t} */

    /* Compile-time type operators */
    AST_CT_TYPE_OF,   /* @typeOf expr */
    AST_CT_SIZE_OF,   /* @sizeOf type */
    AST_CT_ALIGN_OF,  /* @alignOf type */
    AST_CT_FIELDS_OF, /* @fieldsOf type */
    AST_CT_HAS_FIELD, /* @hasField(type, "name") */

    /* Type Definition (ADT) */
    AST_TYPE_DEF,    /* type 'a option = None | Some of 'a */
    AST_DATA_CONSTR, /* None | Some of 'a */
    AST_CONSTR_APPL, /* Some 5, Cons(x, xs) */

    /* Declarations */
    AST_DECL_LET,
    AST_DECL_FN,
    AST_DECL_TYPE,
    AST_DECL_MODULE,
    AST_DECL_IMPORT,

    /* Program */
    AST_PROGRAM,
} fxsh_ast_kind_t;

/*=============================================================================
 * Forward Declarations
 *=============================================================================*/

typedef struct fxsh_ast_node fxsh_ast_node_t;
typedef sp_dyn_array(fxsh_ast_node_t *) fxsh_ast_list_t;

/*=============================================================================
 * Type System (forward declarations)
 *=============================================================================*/

typedef struct fxsh_type fxsh_type_t;
typedef s32 fxsh_type_var_t;

typedef enum {
    TYPE_VAR,
    TYPE_CON,
    TYPE_ARROW,
    TYPE_TUPLE,
    TYPE_RECORD,
    TYPE_APP,
} fxsh_type_kind_t;

typedef struct {
    sp_str_t name;
    fxsh_type_t *type;
} fxsh_field_t;

typedef struct {
    sp_dyn_array(fxsh_type_var_t) vars;
    fxsh_type_t *type;
} fxsh_scheme_t;

typedef sp_ht(sp_str_t, fxsh_scheme_t) fxsh_type_env_t;

/*=============================================================================
 * Constructor Environment (for ADT)
 *=============================================================================*/

typedef struct {
    sp_str_t constr_name;
    sp_str_t type_name;
    fxsh_type_t *constr_type; /* e.g., 'a -> 'a option for Some */
    s32 arity;                /* Number of arguments */
} fxsh_constr_info_t;

typedef sp_ht(sp_str_t, fxsh_constr_info_t) fxsh_constr_env_t;

/*=============================================================================
 * Compile-time Values (defined early for use in AST)
 *=============================================================================*/

typedef enum {
    CT_UNIT,
    CT_BOOL,
    CT_INT,
    CT_FLOAT,
    CT_STRING,
    CT_TYPE,     /* Compile-time type value */
    CT_FUNCTION, /* Compile-time function */
    CT_AST,      /* AST node (for code generation) */
    CT_LIST,     /* List of compile-time values */
    CT_STRUCT,   /* Compile-time struct value */
} fxsh_ct_value_kind_t;

typedef struct fxsh_ct_value fxsh_ct_value_t;
typedef struct fxsh_ct_field fxsh_ct_field_t;

struct fxsh_ct_field {
    sp_str_t name;
    fxsh_ct_value_t *value;
};

struct fxsh_ct_value {
    fxsh_ct_value_kind_t kind;
    union {
        bool bool_val;
        s64 int_val;
        f64 float_val;
        sp_str_t string_val;
        fxsh_type_t *type_val;    /* For CT_TYPE */
        fxsh_ast_node_t *ast_val; /* For CT_AST */
        struct {
            fxsh_ct_value_t **params;
            fxsh_ast_node_t *body;
            fxsh_type_env_t *closure;
        } func_val;
        struct {
            fxsh_ct_value_t **items;
            u32 len;
        } list_val;
        struct {
            fxsh_ct_field_t *fields;
            u32 num_fields;
        } struct_val;
    } data;
};

/*=============================================================================
 * AST Node Structure
 *=============================================================================*/

struct fxsh_ast_node {
    fxsh_ast_kind_t kind;
    fxsh_loc_t loc;

    union {
        /* Literals */
        s64 lit_int;
        f64 lit_float;
        sp_str_t lit_string;
        bool lit_bool;

        /* Identifier */
        sp_str_t ident;

        /* Binary operation */
        struct {
            fxsh_token_kind_t op;
            fxsh_ast_node_t *left;
            fxsh_ast_node_t *right;
        } binary;

        /* Unary operation */
        struct {
            fxsh_token_kind_t op;
            fxsh_ast_node_t *operand;
        } unary;

        /* Function call */
        struct {
            fxsh_ast_node_t *func;
            fxsh_ast_list_t args;
        } call;

        /* Lambda */
        struct {
            fxsh_ast_list_t params; /* AST_PAT_* */
            fxsh_ast_node_t *body;
            sp_str_t ret_type; /* optional */
        } lambda;

        /* Let binding */
        struct {
            sp_str_t name;
            fxsh_ast_node_t *pattern; /* AST_PAT_* */
            fxsh_ast_node_t *type;    /* AST_TYPE_* optional */
            fxsh_ast_node_t *value;
            bool is_comptime;
            bool is_rec;
        } let;

        /* Let-in */
        struct {
            fxsh_ast_list_t bindings;
            fxsh_ast_node_t *body;
        } let_in;

        /* If expression */
        struct {
            fxsh_ast_node_t *cond;
            fxsh_ast_node_t *then_branch;
            fxsh_ast_node_t *else_branch;
        } if_expr;

        /* Match expression */
        struct {
            fxsh_ast_node_t *expr;
            fxsh_ast_list_t arms; /* AST_MATCH_ARM */
        } match_expr;

        /* Match arm */
        struct {
            fxsh_ast_node_t *pattern;
            fxsh_ast_node_t *guard; /* optional */
            fxsh_ast_node_t *body;
        } match_arm;

        /* Pipe expression */
        struct {
            fxsh_ast_node_t *left;
            fxsh_ast_node_t *right;
        } pipe;

        /* Tuple/List/Record */
        fxsh_ast_list_t elements;

        /* Field access */
        struct {
            fxsh_ast_node_t *object;
            sp_str_t field;
        } field;

        /* Type expressions */
        struct {
            sp_str_t name;
            fxsh_ast_list_t args; /* type arguments */
        } type_con;

        struct {
            fxsh_ast_node_t *param;
            fxsh_ast_node_t *ret;
        } type_arrow;

        /* Compile-time type operators */
        struct {
            fxsh_ast_node_t *operand; /* For @typeOf */
        } ct_type_of;

        struct {
            fxsh_ct_value_t *type_val; /* For @sizeOf, @alignOf, @fieldsOf */
        } ct_type_op;

        struct {
            fxsh_ct_value_t *type_val;
            sp_str_t field_name;
        } ct_has_field;

        /* Type Definition */
        struct {
            sp_str_t name;
            fxsh_ast_list_t type_params;  /* 'a, 'b */
            fxsh_ast_list_t constructors; /* None, Some of 'a */
        } type_def;

        /* Data Constructor Definition */
        struct {
            sp_str_t name;
            fxsh_ast_list_t arg_types; /* of 'a * 'b, empty if no args */
        } data_constr;

        /* Constructor Application */
        struct {
            sp_str_t constr_name; /* Some, Cons, etc. */
            fxsh_ast_list_t args; /* 5, [x; xs] */
        } constr_appl;

        /* Declaration */
        struct {
            sp_str_t name;
            fxsh_ast_list_t params;
            fxsh_ast_node_t *ret_type;
            fxsh_ast_node_t *body;
            bool is_comptime;
        } decl_fn;

        /* Program */
        fxsh_ast_list_t decls;
    } data;
};

/*=============================================================================
 * Type System (complete definition)
 *=============================================================================*/

struct fxsh_type {
    fxsh_type_kind_t kind;
    union {
        fxsh_type_var_t var; /* TYPE_VAR */
        sp_str_t con;        /* TYPE_CON */
        struct {             /* TYPE_ARROW */
            fxsh_type_t *param;
            fxsh_type_t *ret;
        } arrow;
        sp_dyn_array(fxsh_type_t *) tuple; /* TYPE_TUPLE */
        struct {                           /* TYPE_RECORD (row polymorphism) */
            sp_dyn_array(fxsh_field_t) fields;
            fxsh_type_var_t row_var; /* 'r for row extension */
        } record;
        struct { /* TYPE_APP */
            fxsh_type_t *con;
            fxsh_type_t *arg;
        } app;
    } data;
};

/* Type constructors */
#define TYPE_UNIT   ((sp_str_t){.data = "unit", .len = 4})
#define TYPE_BOOL   ((sp_str_t){.data = "bool", .len = 4})
#define TYPE_INT    ((sp_str_t){.data = "int", .len = 3})
#define TYPE_FLOAT  ((sp_str_t){.data = "float", .len = 5})
#define TYPE_STRING ((sp_str_t){.data = "string", .len = 6})
#define TYPE_LIST   ((sp_str_t){.data = "list", .len = 4})
#define TYPE_OPTION ((sp_str_t){.data = "option", .len = 6})
#define TYPE_RESULT ((sp_str_t){.data = "result", .len = 6})

/*=============================================================================
 * Substitution (for unification)
 *=============================================================================*/

typedef struct {
    fxsh_type_var_t var;
    fxsh_type_t *type;
} fxsh_subst_entry_t;

typedef sp_dyn_array(fxsh_subst_entry_t) fxsh_subst_t;

/*=============================================================================
 * Parser State
 *=============================================================================*/

typedef struct {
    fxsh_token_array_t tokens;
    u32 pos;
} fxsh_parser_t;

/*=============================================================================
 * Forward Declarations - Lexer
 *=============================================================================*/

void fxsh_lexer_init(fxsh_lexer_t *lexer, sp_str_t source, sp_str_t filename);
fxsh_error_t fxsh_lexer_next(fxsh_lexer_t *lexer, fxsh_token_t *out_token);
fxsh_error_t fxsh_lex(sp_str_t source, sp_str_t filename, fxsh_token_array_t *out_tokens);
const c8 *fxsh_token_kind_name(fxsh_token_kind_t kind);
#define fxsh_token_array_free sp_dyn_array_free

/*=============================================================================
 * Forward Declarations - Parser
 *=============================================================================*/

void fxsh_parser_init(fxsh_parser_t *parser, fxsh_token_array_t tokens);
fxsh_ast_node_t *fxsh_parse(fxsh_parser_t *parser);
fxsh_ast_node_t *fxsh_parse_expr(fxsh_parser_t *parser);
fxsh_ast_node_t *fxsh_parse_program(fxsh_parser_t *parser);

/* AST Constructors */
fxsh_ast_node_t *fxsh_ast_lit_int(s64 val, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_lit_float(f64 val, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_lit_string(sp_str_t val, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_lit_bool(bool val, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_ident(sp_str_t name, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_binary(fxsh_token_kind_t op, fxsh_ast_node_t *left,
                                 fxsh_ast_node_t *right, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_lambda(fxsh_ast_list_t params, fxsh_ast_node_t *body, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_let(sp_str_t name, fxsh_ast_node_t *value, bool is_comptime, bool is_rec,
                              fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_if(fxsh_ast_node_t *cond, fxsh_ast_node_t *then_branch,
                             fxsh_ast_node_t *else_branch, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_call(fxsh_ast_node_t *func, fxsh_ast_list_t args, fxsh_loc_t loc);
fxsh_ast_node_t *fxsh_ast_program(fxsh_ast_list_t decls, fxsh_loc_t loc);

/* AST Destructor */
void fxsh_ast_free(fxsh_ast_node_t *node);

/*=============================================================================
 * Forward Declarations - Type System
 *=============================================================================*/

fxsh_type_t *fxsh_type_var(fxsh_type_var_t var);
fxsh_type_t *fxsh_type_con(sp_str_t name);
fxsh_type_t *fxsh_type_arrow(fxsh_type_t *param, fxsh_type_t *ret);
fxsh_type_t *fxsh_type_apply(fxsh_type_t *con, fxsh_type_t *arg);

fxsh_error_t fxsh_type_infer(fxsh_ast_node_t *ast, fxsh_type_env_t *env,
                             fxsh_constr_env_t *constr_env, fxsh_type_t **out_type);

/* Constructor registration for ADT */
void fxsh_register_type_constrs(fxsh_ast_node_t *type_def, fxsh_constr_env_t *constr_env);
fxsh_error_t fxsh_type_unify(fxsh_type_t *t1, fxsh_type_t *t2, fxsh_subst_t *out_subst);
void fxsh_type_apply_subst(fxsh_subst_t subst, fxsh_type_t **type);

const c8 *fxsh_type_to_string(fxsh_type_t *type);

/*=============================================================================
 * Compile-time Types (must be defined before use)
 *=============================================================================*/

/* Compile-time evaluation result */
typedef struct {
    fxsh_error_t error;
    fxsh_ct_value_t *value;
} fxsh_ct_result_t;

/* Type info for reflection */
typedef struct {
    sp_str_t name;
    fxsh_type_kind_t kind;
    union {
        struct {
            fxsh_ct_field_t *fields;
            u32 num_fields;
        } record;
        struct {
            fxsh_type_t *param;
            fxsh_type_t *ret;
        } arrow;
    } data;
} fxsh_type_info_t;

/*=============================================================================
 * Comptime Environment (forward declaration)
 *=============================================================================*/

typedef sp_ht(sp_str_t, fxsh_ct_value_t) fxsh_ct_env_t;

typedef struct {
    fxsh_ct_env_t *env;
    fxsh_type_env_t *type_env;
    bool in_comptime; /* Whether we're currently in compile-time context */
} fxsh_comptime_ctx_t;

/*=============================================================================
 * Forward Declarations - Comptime
 *=============================================================================*/

/* Comptime value constructors */
fxsh_ct_value_t *fxsh_ct_unit(void);
fxsh_ct_value_t *fxsh_ct_bool(bool val);
fxsh_ct_value_t *fxsh_ct_int(s64 val);
fxsh_ct_value_t *fxsh_ct_float(f64 val);
fxsh_ct_value_t *fxsh_ct_string(sp_str_t val);
fxsh_ct_value_t *fxsh_ct_type(fxsh_type_t *type);
fxsh_ct_value_t *fxsh_ct_ast(fxsh_ast_node_t *ast);

/* Comptime evaluation */
void fxsh_comptime_ctx_init(fxsh_comptime_ctx_t *ctx);
fxsh_ct_result_t fxsh_ct_eval(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx);
fxsh_ct_value_t *fxsh_ct_eval_expr(fxsh_ast_node_t *ast, fxsh_comptime_ctx_t *ctx);

/* Type reflection */
fxsh_type_info_t *fxsh_ct_type_info(fxsh_type_t *type);
fxsh_ct_value_t *fxsh_ct_fields_of(fxsh_type_t *type);
fxsh_ct_value_t *fxsh_ct_make_type(sp_str_t name, fxsh_type_kind_t kind);

/* Code generation */
fxsh_ast_node_t *fxsh_ct_quote(fxsh_ast_node_t *ast);
fxsh_ast_node_t *fxsh_ct_unquote(fxsh_ct_value_t *val);
fxsh_ast_node_t *fxsh_ct_splice(fxsh_ct_value_t *val);

/* Comptime operations */
fxsh_ct_value_t *fxsh_ct_add(fxsh_ct_value_t *a, fxsh_ct_value_t *b);
fxsh_ct_value_t *fxsh_ct_multiply(fxsh_ct_value_t *a, fxsh_ct_value_t *b);
bool fxsh_ct_equal(fxsh_ct_value_t *a, fxsh_ct_value_t *b);

/* Builtin comptime functions */
fxsh_ct_value_t *fxsh_ct_builtin_size_of(fxsh_ct_value_t *type_val);
fxsh_ct_value_t *fxsh_ct_builtin_align_of(fxsh_ct_value_t *type_val);
fxsh_ct_value_t *fxsh_ct_builtin_type_name(fxsh_ct_value_t *type_val);

/* Derived macros */
fxsh_ast_node_t *fxsh_ct_derive_show(fxsh_type_t *type);
fxsh_ast_node_t *fxsh_ct_derive_eq(fxsh_type_t *type);

/* Value to string */
const c8 *fxsh_ct_value_to_string(fxsh_ct_value_t *val);

/* Compile-time record type (for type-level programming) */
typedef struct {
    sp_str_t name;
    fxsh_ct_value_t *type_val;
    fxsh_ct_value_t *default_val;
} fxsh_ct_record_field_t;

/* Type constructor for compile-time type programming */
typedef struct {
    sp_str_t name;
    fxsh_type_kind_t kind;
    sp_dyn_array(fxsh_ct_record_field_t) fields;
    fxsh_type_t *target_type;
} fxsh_type_constructor_t;

/* Type constructor functions */
fxsh_ct_value_t *fxsh_ct_make_record_type(sp_str_t name);
fxsh_ct_value_t *fxsh_ct_record_add_field(fxsh_ct_value_t *record, sp_str_t field_name,
                                          fxsh_ct_value_t *field_type);
fxsh_ct_value_t *fxsh_ct_record_get_field(fxsh_ct_value_t *record, sp_str_t field_name);

/* Compile-time type operators (syntax: @operator) */
fxsh_ct_value_t *fxsh_ct_op_type_of(fxsh_ast_node_t *expr, fxsh_comptime_ctx_t *ctx);
fxsh_ct_value_t *fxsh_ct_op_size_of(fxsh_ct_value_t *type_val);
fxsh_ct_value_t *fxsh_ct_op_align_of(fxsh_ct_value_t *type_val);
fxsh_ct_value_t *fxsh_ct_op_fields_of(fxsh_ct_value_t *type_val);
fxsh_ct_value_t *fxsh_ct_op_has_field(fxsh_ct_value_t *type_val, sp_str_t field_name);

/* Generic type instantiation */
fxsh_type_t *fxsh_ct_instantiate_generic(fxsh_type_constructor_t *ctor,
                                         sp_dyn_array(fxsh_type_t *) type_args);

/* Make vector type constructor (like Zig's ArrayList) */
fxsh_type_constructor_t *fxsh_ct_make_vector_ctor(void);
fxsh_ct_value_t *fxsh_ct_make_vector(fxsh_ct_value_t *elem_type);

/*=============================================================================
 * Forward Declarations - Code Generation
 *=============================================================================*/

char *fxsh_codegen(fxsh_ast_node_t *ast);
fxsh_error_t fxsh_codegen_to_file(fxsh_ast_node_t *ast, sp_str_t path);

/*=============================================================================
 * Forward Declarations - Utilities
 *=============================================================================*/

sp_str_t fxsh_read_file(sp_str_t path);
fxsh_error_t fxsh_write_file(sp_str_t path, sp_str_t content);

#endif /* FXSH_H */
