#include "type_check.h"
#include "print.h"

#include <stdbool.h>
#include <limits.h>


static struct type_checker_state {
	allocator *temps;
} types;

// TODO: use an intern map
static bool same_type(const type *L, const type *R)
{
	if (L->kind == TYPE_NONE || R->kind == TYPE_NONE) return true; // already got an error earlier, dont need more
	if (L->kind != R->kind) return false;
	switch (L->kind) {
	case TYPE_INT64:
	case TYPE_INT32:
	case TYPE_INT8:
	case TYPE_BOOL:
		return true;
	case TYPE_FUNC:
		if (!same_type(L->base, R->base)) return false;
		{
			assert(0 && "not implemented");
		}
	case TYPE_ARRAY:
		if (!same_type(L->base, R->base)) return false;
		if (scratch_len(L->sizes) != scratch_len(R->sizes)) return false;
		for (expr **Lsz = scratch_start(L->sizes), **Rsz = scratch_start(R->sizes); Lsz != scratch_end(L->sizes); Lsz++, Rsz++) {
			assert(Lsz[0]->kind == EXPR_INT && Rsz[0]->kind == EXPR_INT);
			if (Lsz[0]->value != Rsz[0]->value) return false;
		}
		return true;
	case TYPE_PTR:
		return same_type(L->base, R->base);
	default:
		__builtin_unreachable();
	}
}

static const size_t limits[TYPE_INT64-TYPE_INT8+1] = {
	[TYPE_INT8 -TYPE_INT8] = (1UL<<8)-1,
	[TYPE_INT32-TYPE_INT8] = (1UL<<32)-1,
	[TYPE_INT64-TYPE_INT8] = -1,
};

static bool compatible_type_strong(const type *test, const type *ref, const expr *extra)
{
	if (test->kind == TYPE_NONE || ref->kind == TYPE_NONE) return true;
	if (TYPE_INT8 <= ref->kind && ref->kind <= TYPE_INT64)
		return (TYPE_INT8 <= test->kind && test->kind <= ref->kind)
			|| (extra->kind == EXPR_INT && extra->value <= limits[ref->kind - TYPE_INT8]);
	return same_type(test, ref);
}

static bool compatible_type_weak(const type *test, const type *ref, const expr *extra)
{
	if (test->kind == TYPE_NONE || ref->kind == TYPE_NONE) return true;
	if (TYPE_PRIMITIVE_BEGIN <= ref->kind && ref->kind <= TYPE_PRIMITIVE_END)
		return TYPE_PRIMITIVE_BEGIN <= test->kind && test->kind <= TYPE_PRIMITIVE_END;
	return same_type(test, ref);
}

static type *type_check_expr(expr **e, scope_stack_l *stk, type *expecting, value_category c, map *e2t, allocator *up, bool eval);

void complete_type(type *t, scope_stack_l *stk, map *e2t, allocator *up)
{
	if (t->size != (uint64_t)-1) return;
	switch (t->kind) {
case TYPE_ARRAY:
	complete_type(t->base, stk, e2t, up);
	t->align = t->base->align;
	t->size = t->base->size;
	for (expr **sz = scratch_start(t->sizes); sz != scratch_end(t->sizes); sz++) {
		type_check_expr(sz, stk, &type_int64, RVALUE, e2t, up, true);
		// FIXME: handle overflow
		t->size *= sz[0]->value;
	}
	break;
case TYPE_PTR:
	complete_type(t->base, stk, e2t, up);
	t->size = 8;
	t->align = 8;
	break;
case TYPE_FUNC:
	{
	for (func_arg *param = scratch_start(t->params); param != scratch_end(t->params); param++)
		complete_type(param->type, stk, e2t, up);
	complete_type(t->base, stk, e2t, up);
	t->align = 1;
	t->size = 0;
	break;
	}
default:
	__builtin_unreachable();
	}
}

type *type_check_expr(expr **pe, scope_stack_l *stk, type *expecting, value_category c, map *e2t, allocator *up, bool eval)
{
	expr *e = *pe;
	type *t = &type_none;
	switch (e->kind) {
case EXPR_INT:
	if (!expect_or(c == RVALUE,
			e->pos, "cannot assign to an integer.\n")) break;
	if (expecting->kind == TYPE_INT8 || expecting->kind == TYPE_INT32 || expecting->kind == TYPE_INT64) {
		t = expecting;
		break;
	}
	t = 	e->value <= limits[TYPE_INT8 -TYPE_INT8]? &type_int8: 
		e->value <= limits[TYPE_INT32-TYPE_INT8]? &type_int32: &type_int64;
	break;

case EXPR_BOOL:
	if (!expect_or(c == RVALUE,
			e->pos, "cannot assign to a boolean.\n")) break;
	t = &type_bool;
	break;

case EXPR_NAME:
	{
	// LVALUE is ok
	if (!expect_or(!eval, e->pos, "cannot evaluate a variable in a compilation context.\n")) break;
	map_entry *entry = map_find(&stk->scope->refs, e->name, intern_hash(e->name), intern_cmp);
	while (!entry && stk->next) {
		stk = stk->next;
		entry = map_find(&stk->scope->refs, e->name, intern_hash(e->name), intern_cmp);
	}
	if (entry) t = ((decl*) entry->v)->type;
	break;
	}

case EXPR_ADD:
case EXPR_CMP:
	{
	if (!expect_or(c == RVALUE,
			e->pos, "cannot assign to the result of binary expression.\n")) break;
	// TODO: find a way to be less strict about the expected type here
	type *L = type_check_expr(&e->binary.L, stk, &type_none, RVALUE, e2t, up, eval);
	type *R = type_check_expr(&e->binary.R, stk, &type_none, RVALUE, e2t, up, eval);
	expr *Lv = e->binary.L, *Rv = e->binary.R;
	expr *smaller_e = Rv;
	type *bigger = L, *smaller = R;
	int cmp = L->size - R->size;
	if (cmp > 0) {
		smaller_e = Rv = e->binary.R = expr_convert(up, Rv, bigger = L);
		map_entry *asso = map_add(e2t, (key_t) smaller_e, intern_hash, types.temps);
		asso->k = (key_t) smaller_e;
		asso->v = (val_t) bigger;
		smaller = R;
	} else if (cmp < 0) {
		smaller_e = Lv = e->binary.L = expr_convert(up, Lv, bigger = R);
		map_entry *asso = map_add(e2t, (key_t) smaller_e, intern_hash, types.temps);
		asso->k = (key_t) smaller_e;
		asso->v = (val_t) bigger;
		smaller = L;
	}
	if (!expect_or(compatible_type_strong(smaller, bigger, smaller_e),
			e->pos, "the operands to this binary operation are incompatible.\n")) break;
	if (e->kind == EXPR_ADD && !expect_or(compatible_type_strong(bigger, &type_int64, smaller_e),
				e->pos, "cannot add/sub non-integers.\n")) break;
	t = 	e->kind == EXPR_ADD? bigger:
		e->kind == EXPR_CMP? &type_bool: &type_none;
	if (!eval) break;
	token_kind op = e->binary.op;
	if (e->kind == EXPR_ADD) {
		e->kind = EXPR_INT;
		if (op == '+') 		e->value = Lv->value + Rv->value;
		else if (op == '-')	e->value = Lv->value - Rv->value;
		else __builtin_unreachable();
	} else if (e->kind == EXPR_CMP) {
		e->kind = EXPR_INT;
		if (op == TOKEN_EQ)		e->value = Lv->value == Rv->value;
		else if (op == TOKEN_NEQ)	e->value = Lv->value != Rv->value;
		else if (op == '<')		e->value = Lv->value <  Rv->value;
		else if (op == TOKEN_LEQ)	e->value = Lv->value <= Rv->value;
		else if (op == '>')		e->value = Lv->value >  Rv->value;
		else if (op == TOKEN_GEQ)	e->value = Lv->value >= Rv->value;
		else __builtin_unreachable();
	}
	break;
	}

case EXPR_INITLIST:
	{
	if (!expect_or(c == RVALUE,
			e->pos, "cannot assign to an initializer list.\n")) break;
	if (!expect_or(expecting->kind == TYPE_ARRAY,
			e->pos, "can only initialize an array with an initializer list.\n")) break;
	idx_t depth = scratch_len(expecting->sizes) / sizeof(expr*);
	typedef struct { expr **at, **end; } iter;
	allocation m = ALLOC(types.temps, depth * sizeof(iter), alignof(iter));
	iter *stack = m.addr, *top = stack;
	*top++ = (iter){ scratch_start(pe[0]->call.args), scratch_end(pe[0]->call.args) };
	while (top != stack) {
		iter *peek = &top[-1];
		if (peek->at == peek->end) {
			top--;
			continue;
		}
		idx_t reach = top - stack;
		expr **sizes = scratch_start(expecting->sizes);
		if (reach == depth)
			type_check_expr(peek->at, stk, expecting->base, RVALUE, e2t, up, true);
		else if (expect_or(reach < depth && sizes[reach]->kind == EXPR_INT && scratch_len(peek->at[0]->call.args) / sizeof(expr*) == sizes[reach]->value,
					pe[0]->pos, "attempt to assign a value out of bounds of array"))
			*top++ = (iter){
				scratch_start(peek->at[0]->call.args),
				scratch_end  (peek->at[0]->call.args)
			};
		else goto fail;
		peek->at++;
	}
	t = expecting;
fail:
	DEALLOC(types.temps, m);
	break;
	}

case EXPR_LOG_NOT:
	{
	if (!expect_or(c == RVALUE,
			e->pos, "cannot assign to result of a function call.\n")) break;
	type *op = type_check_expr(&e->unary.operand, stk, expecting, RVALUE, e2t, up, eval);
	if (!expect_or(same_type(t, &type_bool),
			e->pos, "cannot find the boolean complement of non-boolean.\n")) break;
	t = op;
	if (!eval) break;
	if (e->unary.op == '!') {
		e->kind = EXPR_INT;
		e->value = !e->unary.operand->value;
	}
	break;
	}

case EXPR_CALL:
	{
	if (!expect_or(!eval, e->pos, "cannot evaluate a function call in a constant expression.\n")) break;
	if (!expect_or(c == RVALUE, e->pos, "cannot assign to result of a function call.\n")) break;
	type *operand = type_check_expr(&e->call.operand, stk, &type_none, RVALUE, e2t, up, eval);
	if (!expect_or(operand->kind == TYPE_FUNC,
			e->pos, "attempt to call a non-callable:\n")) break;
	scratch_arr params = operand->params;
	expr **arg = scratch_start(e->call.args);
	if (!expect_or(scratch_len(e->call.args) / sizeof(expr*) == scratch_len(params) / sizeof(func_arg),
			e->pos, "function call with the wrong number of arguments provided.\n"))
		break;
	for (func_arg *param = scratch_start(params); param != scratch_end(params); param++, arg++)
		type_check_expr(arg, stk, param->type, RVALUE, e2t, up, eval);
	t = operand->base;
	break;
	}

case EXPR_CONVERT:
	{
	if (!expect_or(c == RVALUE, e->pos, "cannot assign to the result of a cast expression.\n")) break;
	type *operand = type_check_expr(&e->convert.operand, stk, &type_none, RVALUE, e2t, up, eval);
	complete_type(e->convert.type, stk, e2t, up);
	if (!expect_or(compatible_type_weak(operand, e->convert.type, e->convert.operand),
				e->pos, "attempt to cast between fully incompatible types.\n")) break;
	t = e->convert.type;
	if (eval) {
		assert(TYPE_PRIMITIVE_BEGIN <= t->kind && t->kind <= TYPE_PRIMITIVE_END);
		if (t->kind == TYPE_BOOL) {
			assert(0);
		} else if (operand->size > t->size) {
			idx_t shift = 8 * t->size;
			uint64_t mask = (1ULL << shift) - 1;
			assert(e->convert.operand->kind == EXPR_INT);
			e->value = e->convert.operand->value & mask;
			e->kind = EXPR_INT;
		}
	}
	break;
	}

case EXPR_ADDRESS:
	if (!expect_or(c == RVALUE, e->pos, "cannot take the result of an address-of operation as an lvalue.\n")) break;
	assert(!eval);
	if (expecting->kind == TYPE_PTR) {
		type_check_expr(&e->unary.operand, stk, expecting->base, LVALUE, e2t, up, false);
		t = expecting;
	} else {
		type *operand = type_check_expr(&e->unary.operand, stk, &type_none, LVALUE, e2t, up, false);
		t = type_ptr(up, operand);
	}
	break;

case EXPR_INDEX:
	// LVALUE is ok
	{
	if (!expect_or(!eval, e->pos, "cannot evaluate an indexing operation in a constant expression.\n")) break;
	type *base  = type_check_expr(&e->call.operand, stk, &type_none, RVALUE, e2t, up, eval);
	if (!expect_or(base->kind == TYPE_ARRAY,
			e->pos, "attempt to index something that does not support indexing.\n")) break;
	if (!expect_or(scratch_len(e->call.args) == scratch_len(base->sizes),
			e->pos, "mismatch between the number of indices and the dimension of the array.\n")) break;
	for (expr **idx = scratch_start(e->call.args); idx != scratch_end(e->call.args); idx++) {
		type *ti = type_check_expr(idx, stk, &type_int64, RVALUE, e2t, up, eval);
		assert(ti == &type_int64);
	}
	t = base->base;
	break;
	}

case EXPR_DEREF:
	{
	assert(!eval);
	type *operand = type_check_expr(&e->unary.operand, stk, &type_none, RVALUE, e2t, up, false);
	if (!expect_or(operand->kind == TYPE_PTR, e->pos, "cannot dereference a non-pointer.\n")) break;
	t = operand->base;
	break;
	}

case EXPR_UNDEF:
	if (!expect_or(c == RVALUE, e->pos, "cannot assign to undef-expression.\n")) break;
	if (!expect_or(!eval, e->pos, "cannot evaluate undef-expression.\n")) break;
	t = expecting;
	break;
case EXPR_NONE:
	break;
default:
	assert(0);
	}
	expect_or(compatible_type_strong(t, expecting, e),
			e->pos, "the type of this expression mismatches what is expected here.\n");
	// since each expression is only created once, pointer equality is enough
	map_entry *asso = map_add(e2t, (key_t) e, intern_hash, types.temps);
	asso->k = (key_t) e;
	asso->v = (val_t) t;
	complete_type(t, stk, e2t, up);
	if (!eval && expecting->kind != TYPE_NONE && expecting->kind != t->kind) {
		e = *pe = expr_convert(up, e, expecting);
		t = expecting;
		map_entry *cvt = map_add(e2t, (key_t) e, intern_hash, types.temps);
		cvt->k = (key_t) e;
		cvt->v = (val_t) expecting;
	}
	return t;
}

static void type_check_decl(decl_idx i, scope *sc, scope_stack_l *stk, map *e2t, allocator *up);
static void type_check_stmt_block(stmt_block blk, type *surrounding, scope *sc, scope_stack_l *stk,
		map *e2t, allocator *up);

static scope *type_check_stmt(stmt *s, type *surrounding, scope *sc, scope_stack_l *stk,
		map *e2t, allocator *up)
{
	switch (s->kind) {
	case STMT_EXPR:
		type_check_expr(&s->e, stk, &type_none, RVALUE, e2t, up, false);
		return sc;
	case STMT_ASSIGN:
		type_check_expr(&s->assign.R, stk,
				type_check_expr(&s->assign.L, stk, &type_none, LVALUE, e2t, up, false),
				RVALUE, e2t, up, false);
		return sc;
	case STMT_DECL:
		type_check_decl(s->d, sc, stk, e2t, up);
		return sc;
	case STMT_RETURN:
		type_check_expr(&s->e, stk, surrounding, RVALUE, e2t, up, false);
		return sc;
	case STMT_IFELSE:
		type_check_expr(&s->ifelse.cond, stk, &type_bool, RVALUE, e2t, up, false);
		sc = type_check_stmt(s->ifelse.s_then, surrounding, sc, stk, e2t, up);
		if (s->ifelse.s_else)
			sc = type_check_stmt(s->ifelse.s_else, surrounding, sc, stk, e2t, up);
		return sc;
	case STMT_WHILE:
		type_check_expr(&s->ifelse.cond, stk, &type_bool, RVALUE, e2t, up, false);
		return type_check_stmt(s->ifelse.s_then, surrounding, sc, stk, e2t, up);
	case STMT_NONE:
		return sc;
	case STMT_BLOCK:
		type_check_stmt_block(s->blk, surrounding, sc, stk, e2t, up);
		return sc + 1;
	default:
		assert(0);
	}
}

void type_check_stmt_block(stmt_block blk, type *surrounding, scope *sc, scope_stack_l *stk,
		map *e2t, allocator *up)
{
	scope_stack_l top = { .scope=sc, .next=stk };
	scope *sub = scratch_start(sc->sub);
	for (stmt **it = scratch_start(blk), **end = scratch_end(blk);
			it != end; it++) {
		sub = type_check_stmt(*it, surrounding, sub, &top, e2t, up);
	}
}

void type_check_decl(decl_idx i, scope *sc, scope_stack_l *stk, map *e2t, allocator *up)
{
	decl *d = idx2decl(i);
	complete_type(d->type, stk, e2t, up);
	switch (d->kind) {
	case DECL_VAR:
		type_check_expr(&d->var_d.init, stk, d->type, RVALUE, e2t, up, false);
		break;
	case DECL_FUNC:
		type_check_stmt_block(d->func_d.body, d->type->base, sc, stk, e2t, up);
		break;
	case DECL_NONE:
		break;
	default:
		assert(0);
	}
}

void type_check(module_t module, scope *top, map *expr2type, allocator *up)
{
	map_init(expr2type, 0, types.temps);
	decl_idx *decl_it = scratch_start(module)  , *decl_end = scratch_end(module   );
	scope *scope_it   = scratch_start(top->sub), *scope_end = scratch_end(top->sub);
	assert(decl_end - decl_it == scope_end - scope_it);
	scope_stack_l bottom = { .scope=top, .next=NULL };
	for (; decl_it != decl_end; decl_it++, scope_it++)
		type_check_decl(*decl_it, scope_it, &bottom, expr2type, up);
	print(stdout, (print_acquire_e2t){ expr2type });
	ast_dump(module);
}

void type_init(allocator *temps)
{
	types.temps = temps;
}

void type_fini(void)
{
}
