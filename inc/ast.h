#ifndef CROUTE_AST_H
#define CROUTE_AST_H


#include "alloc.h"
#include "dynarr.h"
#include "token.h"
#include "map.h"


typedef int32_t idx_t;
typedef idx_t decl_idx;

typedef enum type_kind {
	TYPE_NONE,
	TYPE_PRIMITIVE,
	TYPE_FUNC,
	
	TYPE_END = TYPE_FUNC
} type_kind;

typedef enum decl_kind {
	DECL_NONE,
	DECL_VAR,
	DECL_FUNC,

	DECL_END = DECL_FUNC
} decl_kind;

typedef enum stmt_kind {
	STMT_NONE,
	STMT_EXPR,
	STMT_ASSIGN,
	STMT_DECL,
	STMT_IFELSE,
	STMT_BLOCK,
	STMT_RETURN,

	STMT_END = STMT_RETURN
} stmt_kind;

typedef enum expr_kind {
	EXPR_NONE,
	EXPR_INT,
	EXPR_BOOL,
	EXPR_NAME,
	EXPR_CALL,
	EXPR_BINARY,
	EXPR_UNARY,
} expr_kind;

struct expr;
struct decl;
struct stmt;
struct type_t;

typedef struct type_t {
	union {
		ident_t name;
		struct {
			// func_arg array
			scratch_arr params;
			struct type_t *ret_t;
		} func_t;
	};
	type_kind kind;
} type_t;

typedef struct func_arg {
	ident_t name;
	type_t *type;
} func_arg;

typedef struct expr {
	union {
		uint64_t value;
		ident_t name;
		struct {
			struct expr *operand;
			scratch_arr args; // array of expr*
		} call;
		struct {
			struct expr *L, *R;
			token_kind op;
		} binary;
		struct {
			struct expr *operand;
			token_kind op;
		} unary;
	};
	expr_kind kind;
	source_idx pos;
} expr;

typedef scratch_arr stmt_block; // array of stmt*

typedef struct decl {
	ident_t name;
	type_t *type;
	source_idx pos;
	union {
		struct {
			expr *init;
		} var_d;
		struct {
			stmt_block body;
		} func_d;
	};
	decl_kind kind;
} decl;

typedef struct stmt {
	union {
		expr *e;
		struct { expr *L, *R; } assign;
		decl_idx d;
		struct { expr *cond; struct stmt *s_then, *s_else /* may be null */; } ifelse;
		stmt_block blk;
	};
	stmt_kind kind;
} stmt;

typedef scratch_arr module_t; // array of decl_idx

extern struct ast_state_t
{
	allocator *temps;
	size_t errors;
	dyn_arr decls; // array of decl*
} ast;

int ast_init(allocator *up);
void ast_fini(allocator *up);
void ast_one_more_error(void);

module_t parse_module(allocator *up);
decl *idx2decl(decl_idx i);

#define AST_DUP(a,v) ast_dup((a),&(v),sizeof (v))

#endif /* CROUTE_AST_H */

