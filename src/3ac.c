#include "3ac.h"
#include "type_check.h"
#include "dynarr.h"
#include "ast.h"
#include "alloc.h"
#include "map.h"
#include "token.h"
#include "print.h"
#include "attrs.h"
// TODO: remove
#include "gen/x86-64.h"
#include "gen/elf64.h"

#include <string.h>
#include <assert.h>
#include <stdbool.h>

typedef struct ir3_reloc {
	idx_t sym_in, offset_in;
	decl *ref;
} ir3_reloc;

static struct global_bytecode_state_t {
	dyn_arr blob;
	dyn_arr names;
	dyn_arr relocs;
	allocator *temps;
	idx_t cur_idx;
} bytecode;

void bytecode_init(allocator *temps)
{
	dyn_arr_init(&bytecode.blob, 0, temps);
	dyn_arr_init(&bytecode.names, 0, temps);
	dyn_arr_init(&bytecode.relocs, 0, temps);
	bytecode.temps = temps;
}

void bytecode_fini(void)
{
	dyn_arr_fini(&bytecode.names, bytecode.temps);
	dyn_arr_fini(&bytecode.relocs, bytecode.temps);
}

typedef struct map_stack {
	scope *scope;
	struct map_stack *next;
} map_stack;

static ssa_ref new_local(dyn_arr *locals, type *t)
{
	ssa_ref num = dyn_arr_size(locals)/sizeof t;
	dyn_arr_push(locals, &t, sizeof t, bytecode.temps);
	return num;
}

static void serialize_initlist(byte *blob, expr *e, map_stack *stk)
{
	switch (e->kind) {
case EXPR_INITLIST:
	{
	assert(e->type->kind == TYPE_ARRAY);
	idx_t depth = scratch_len(e->type->sizes) / sizeof(expr*);
	typedef struct { expr **at, **end; } iter;
	allocation m = ALLOC(bytecode.temps, depth * sizeof(iter), alignof(iter));
	iter *stack = m.addr, *top = stack;
	*top++ = (iter){ scratch_start(e->call.args), scratch_end(e->call.args) };
	while (top != stack) {
		iter *peek = &top[-1];
		if (peek->at == peek->end) {
			top--;
			continue;
		}
		idx_t reach = top - stack;
		if (reach == depth)
			serialize_initlist(blob, *peek->at, stk),
			blob += e->type->base->size;
		else
			*top++ = (iter){
				scratch_start(peek->at[0]->call.args),
				scratch_end  (peek->at[0]->call.args)
			};
		peek->at++;
	}
	DEALLOC(bytecode.temps, m);
	}
	break;
case EXPR_INT:
	// little endian things
	memcpy(blob, &e->value, e->type->size);
	break;
case EXPR_BOOL:
	memcpy(blob, &e->value, 1);
	break;
default:
	__builtin_unreachable();
	}
}

static map_entry global_name(ident_t name, size_t len, allocator *a)
{
	const char *src = (char*) name;
	allocation m = ALLOC(a, len+1, 1); // NUL
	assert(m.size >= len+1);
	char *dst = m.addr;
	memcpy(dst, src, len);
	dst[len] = '\0';
	map_entry r = { .k=(key_t)dst, .v=len };
	return r;
}

// rvalue is used in assignment contexts
// a[1] = b; ---> rvalue = b
// 12 ---> rvalue = REF_NONE
static idx_t ir3_expr(ir3_func *f, expr *e, map_stack *stk, ssa_ref rvalue, allocator *a)
{
	switch (e->kind) {
		ssa_ref number;
case EXPR_INT:
	number = new_local(&f->locals, e->type);
	assert(e->value <= (ssa_extension)-1);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_IMM, number }, sizeof(ssa_instr), a);
	// just little endian things
	dyn_arr_push(&f->ins, &e->value, sizeof(ssa_extension), a);
	return number;

case EXPR_BOOL:
	number = new_local(&f->locals, e->type);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_BOOL, number, e->name == tokens.kw_true }, sizeof(ssa_instr), a);
	return number;
	
case EXPR_NAME:
	return e->decl->id;

case EXPR_CALL:
	{
	assert(e->call.operand->type->kind == TYPE_FUNC);
	idx_t num_args = scratch_len(e->call.args) / sizeof(expr*);
	assert(num_args < (1L << (8*sizeof(ssa_ref))) - 1);
	idx_t ratio = sizeof(ssa_extension) / sizeof(ssa_ref);
	idx_t num_ext = (num_args + ratio - 1) / ratio + 1;
	allocation m = ALLOC(a, (1 + num_ext) * sizeof(ssa_instr), alignof(ssa_instr));
	ssa_instr *instr = m.addr;
	*instr++ = (ssa_instr){ .kind=SSA_CALL, REF_NONE, .R=num_args };
#ifndef NDEBUG
	(*instr++).v = -1;
#else
	instr++;
#endif
	// since the operand is a function designator, there is nothing to compute
	// so it's ok to call and then evaluate it
	ident_t func = ir3_expr(f, e->call.operand, stk, REF_NONE, a);
	// little endian things
	ssa_ref buf[ratio];
	expr **base = scratch_start(e->call.args);
	idx_t arg, idx;
	for (arg = 0, idx = 0; arg < num_args - ratio; arg++, idx %= ratio) {
		// ??? // FIXME: not giving the right id, either here or in decode
		buf[idx++] = ir3_expr(f, base[arg], stk, REF_NONE, a);
		if ((arg + ratio - 1) % ratio == 0)
			memcpy(instr++, buf, sizeof buf);
	}
	// last iteration is special: must publish even if you have less arguments
	for (; arg < num_args; arg++)
		buf[idx++] = ir3_expr(f, base[arg], stk, REF_NONE, a);
	if (num_args)
		memcpy(instr++, buf, sizeof buf);
	// post after the arguments are evaluated
	number = new_local(&f->locals, e->type);
	ssa_instr *call = dyn_arr_push(&f->ins, m.addr, (void*) instr - m.addr, a);
	call->to = number;
	if (func == (idx_t) -1)
		dyn_arr_push(&bytecode.relocs, &(ir3_reloc){ .sym_in=bytecode.cur_idx, .offset_in=(void*) &call[1] - f->ins.buf.addr, .ref=e->call.operand->decl }, sizeof(ir3_reloc), a);
	else
		call[1].v = func;
	DEALLOC(a, m);
	return number;
	}

case EXPR_ADD:
	{
	ssa_ref L = ir3_expr(f, e->binary.L, stk, REF_NONE, a);
	ssa_ref R = ir3_expr(f, e->binary.R, stk, REF_NONE, a);
	number = new_local(&f->locals, e->type);
	token_kind op = e->binary.op;
	enum ssa_opcode opc = 	op == '+' ? SSA_ADD:
				op == '-' ? SSA_SUB:
				(assert(0), -1);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=opc, number, L, R }, sizeof(ssa_instr), a);
	return number;
	}

case EXPR_CMP:
	{
	ssa_ref L = ir3_expr(f, e->binary.L, stk, REF_NONE, a);
	ssa_ref R = ir3_expr(f, e->binary.R, stk, REF_NONE, a);
	number = new_local(&f->locals, e->type);
	token_kind op = e->binary.op;
	enum ssa_branch_cc cc =	op == TOKEN_EQ ? SSAB_EQ: op == TOKEN_NEQ? SSAB_NE:
				op == '<'      ? SSAB_LT: op == TOKEN_LEQ? SSAB_LE:
				op == '>'      ? SSAB_GT: op == TOKEN_GEQ? SSAB_GE:
				(assert(0), -1);
	// wanted to work around adding this redundant SET instruction,
	// but adding a jump in here while converting to a CFG will
	// probably just give me bugs.
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_SET, number, L, R }, sizeof(ssa_instr), a);
	dyn_arr_push(&f->ins, &(ssa_instr){ .to=cc }, sizeof(ssa_instr), a);
	return number;
	}

case EXPR_LOG_NOT:
	{
	ssa_ref inner = ir3_expr(f, e->unary.operand, stk, REF_NONE, a);
	number = new_local(&f->locals, e->type);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_BOOL_NEG, number, inner }, sizeof(ssa_instr), a);
	return number;
	}

case EXPR_ADDRESS:
	{
	expr *sub = e->unary.operand;
	assert(rvalue == REF_NONE);
	if (sub->kind == EXPR_NAME) {
		ssa_ref name = ir3_expr(f, sub, stk, REF_NONE, a);
		number = new_local(&f->locals, e->type);
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_ADDRESS, number, name }, sizeof(ssa_instr), a);
	} else if (sub->kind == EXPR_INDEX) {
		// TODO: add sizeof instruction because structs arent complete at this stage
		type *base_t = sub->call.operand->type;
		assert(base_t->kind == TYPE_ARRAY);
		ssa_ref base;
		if (sub->call.operand->kind == EXPR_DEREF) {
			base = ir3_expr(f, sub->call.operand->unary.operand, stk, REF_NONE, a);
		} else {
			expr addr = { .kind=EXPR_ADDRESS, .unary = { .operand=sub->call.operand }, .type=e->type };
			base = ir3_expr(f, &addr, stk, REF_NONE, a);
		}
		number = new_local(&f->locals, e->type);

		expr **fst_idx = scratch_start(sub->call.args);
		ssa_ref offset = ir3_expr(f, *fst_idx, stk, REF_NONE, a);
		for (expr **idx = fst_idx+1, **sz = scratch_start(base_t->sizes) + sizeof *sz; idx != scratch_end(sub->call.args); idx++, sz++) {
			assert(sz[0]->kind == EXPR_INT);
			dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_IMM, number }, sizeof(ssa_instr), a);
			dyn_arr_push(&f->ins, &(ssa_instr){ .v=sz[0]->value }, sizeof(ssa_instr), a);
			dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_MUL, offset, offset, number }, sizeof(ssa_instr), a);
			ssa_ref evaluated_idx = ir3_expr(f, *idx, stk, RVALUE, a);
			dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_ADD, offset, offset, evaluated_idx }, sizeof(ssa_instr), a);
		}

		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_IMM, number }, sizeof(ssa_instr), a);
		dyn_arr_push(&f->ins, &(ssa_instr){ .v=base_t->base->size }, sizeof(ssa_instr), a);
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_MUL, number, number, offset }, sizeof(ssa_instr), a);
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_ADD, number, number, base }, sizeof(ssa_instr), a);
	} else if (sub->kind == EXPR_DEREF) {
		number = ir3_expr(f, sub->unary.operand, stk, REF_NONE, a);
	} else if (sub->kind == EXPR_FIELD) {
		type *inner = sub->field.operand->type;
		assert(inner->kind == TYPE_STRUCT);
		map_entry *field = map_find(&inner->fields, sub->field.name, intern_hash(sub->field.name), intern_cmp);
		assert(field);
		// FIXME: no constant pointer type yet
		expr aggr = { .kind=EXPR_ADDRESS, .unary = { .operand=sub->field.operand }, .type=&type_int64 };
		ssa_ref addr = ir3_expr(f, &aggr, stk, REF_NONE, a);
		ssa_ref offs = new_local(&f->locals, &type_int64);
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_OFFSETOF, offs, inner->id, ((decl*) field->v)->id }, sizeof(ssa_instr), a);
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_ADD, addr, addr, offs }, sizeof(ssa_instr), a);
		number = addr;
	} else __builtin_unreachable();
	return number;
	}

case EXPR_DEREF:
	{
	// if (e->unary.operand->kind == EXPR_ADDRESS) return ir3_expr(f, e->unary.operand->unary.operand, stk, REF_NONE, a);
	ssa_ref addr = ir3_expr(f, e->unary.operand, stk, REF_NONE, a);
	int size = e->type->size;
	bool primitive = size == 1 || size == 2 || size == 4 || size == 8;
	if (rvalue == REF_NONE) {
		assert(primitive);
		number = new_local(&f->locals, e->type);
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=primitive? SSA_LOAD: SSA_MEMCOPY, number, addr }, sizeof(ssa_instr), a);
	} else {
		type **rvt = f->locals.buf.addr + rvalue * sizeof *rvt;
		assert(rvt[0]->size > 0);
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_STORE, rvalue, addr }, sizeof(ssa_instr), a);
		number = rvalue; // maybe set this to REF_NONE, it should not get read, regardless
	}
	return number;
	}

case EXPR_INDEX:
	{
	expr addr  = { .kind=EXPR_ADDRESS, .unary = { .operand=e }, .type=&type_int64 };
	expr deref = { .kind=EXPR_DEREF  , .unary = { .operand=&addr }, .type=e->type };
	// kind of messy because this deref shouldnt be elided
	// means that *&x = 1; doesnt elide
	// TODO: maybe consider adding a NO_ELIDE_DEREF
	return ir3_expr(f, &deref, stk, rvalue, a);
	}

case EXPR_INITLIST:
	{
	ir3_sym blob = { .m=ALLOC(a, e->type->size, 8), .align=e->type->align, .kind=IR3_BLOB };
	idx_t ref = dyn_arr_size(&bytecode.blob) / sizeof(ir3_sym);
	dyn_arr_push(&bytecode.blob, &blob, sizeof blob, a);
	serialize_initlist(blob.m.addr, e, stk);
	char buf[16];
	int len = snprintf(buf, sizeof buf, ".G%x", ref);
	assert(buf[len] == '\0');
	map_entry name = global_name((ident_t) buf, len, a);
	dyn_arr_push(&bytecode.names, &name, sizeof name, a);
	ssa_ref local = new_local(&f->locals, &type_int64);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_GLOBAL_REF, local }, sizeof(ssa_instr), a);
	dyn_arr_push(&f->ins, &(ssa_instr){ .v=ref }, sizeof(ssa_extension), a);
	number = new_local(&f->locals, e->type);
	// FIXME: also take the address of target, and remove the `lea` in codegen
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_MEMCOPY, number, local }, sizeof(ssa_instr), a);
	return number;
	}

case EXPR_CONVERT:
	{
	assert(rvalue == REF_NONE);
	ssa_ref from = ir3_expr(f, e->convert.operand, stk, rvalue, a);
	number = new_local(&f->locals, e->type);
	type *from_t = from[(type**) f->locals.buf.addr];
	assert(TYPE_PRIMITIVE_BEGIN <= e->type->kind && e->type->kind <= TYPE_PRIMITIVE_END);
	assert(TYPE_PRIMITIVE_BEGIN <= from_t->kind && from_t->kind <= TYPE_PRIMITIVE_END);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_CONVERT, .to=number, .L=from,
			.R=COMBINE_TYPE(e->type->kind, from_t->kind) }, sizeof(ssa_instr), a);
	return number;
	}

case EXPR_FIELD:
	{
	expr addr  = { .kind=EXPR_ADDRESS, .unary = { .operand=e }, .type=&type_int64 };
	expr deref = { .kind=EXPR_DEREF  , .unary = { .operand=&addr }, .type=e->type };
	return ir3_expr(f, &deref, stk, rvalue, a);
	}

case EXPR_UNDEF:
	number = new_local(&f->locals, e->type);
	// no-op
	return number;

default:
	__builtin_unreachable();
	}
}

static void ir3_decl(ir3_func *f, decl_idx i, map_stack *stk, allocator *a)
{
	decl *d = idx2decl(i);
	switch (d->kind) {
case DECL_VAR:
	{
	ssa_ref val = ir3_expr(f, d->init, stk, REF_NONE, a);
	type *init_type = d->init->type;
	if (d->init->kind == EXPR_NAME) {
		assert(init_type->kind == d->type->kind);
		ssa_ref number = new_local(&f->locals, d->type);
		d->id = number;
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_COPY, number, val }, sizeof(ssa_instr), a);
	} else {
		d->id = val;
	}
	}
	break;
default:
	__builtin_unreachable();
}
}

static void ir3_stmt(ir3_func *f, stmt *s, map_stack *stk, scope **blk, allocator *a)
{
	switch (s->kind) {
	ssa_instr buf[2];
case STMT_DECL:
	ir3_decl(f, s->d, stk, a);
	break;
case STMT_ASSIGN:
	{
	ssa_ref R = ir3_expr(f, s->assign.R, stk, REF_NONE, a);
	ir3_expr(f, s->assign.L, stk, R, a);
	break;
	}
case STMT_RETURN:
	{
	ssa_ref ret = ir3_expr(f, s->e, stk, REF_NONE, a);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_RET, ret }, sizeof(ssa_instr), a);
	break;
	}
case STMT_IFELSE:
	{
	ssa_ref cond = ir3_expr(f, s->ifelse.cond, stk, REF_NONE, a);
	ssa_ref check = new_local(&f->locals, &type_bool);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_BOOL, check, 0 }, sizeof(ssa_instr), a);
	buf[0] = (ssa_instr){ .kind=SSA_BR, SSAB_NE, cond, check };
	buf[1] = (ssa_instr){ .v = -1 };
	// the then/else label fields are in the extension
	ssa_instr *br = dyn_arr_push(&f->ins, buf, 2*sizeof *buf, a) + sizeof(ssa_instr);
	idx_t br_idx = (void*) br - f->ins.buf.addr;
	br->L = dyn_arr_size(&f->nodes)/sizeof(ir3_node);

	ir3_node *then_n = dyn_arr_push(&f->nodes, NULL, sizeof *then_n, a);
	then_n->begin = then_n[-1].end = dyn_arr_size(&f->ins);
	ir3_stmt(f, s->ifelse.s_then, stk, blk, a);
	buf[0].kind = SSA_GOTO;
	ssa_instr *then_i = dyn_arr_push(&f->ins, buf, sizeof *buf, a);
	idx_t then_i_idx = (void*) then_i - f->ins.buf.addr;

	ssa_instr *else_i;
	idx_t else_i_idx;
	if (s->ifelse.s_else) {
		br = f->ins.buf.addr + br_idx;
		br->R = dyn_arr_size(&f->nodes)/sizeof(ir3_node);
		ir3_node *else_n = dyn_arr_push(&f->nodes, NULL, sizeof *else_n, a);
		else_n[-1].end = else_n->begin = dyn_arr_size(&f->ins);
		ir3_stmt(f, s->ifelse.s_else, stk, blk, a);
		else_i = dyn_arr_push(&f->ins, buf, sizeof *buf, a);
		else_i_idx = (void*) else_i - f->ins.buf.addr;
	}

	ir3_node *post_n = dyn_arr_push(&f->nodes, NULL, sizeof *post_n, a);
	ssa_ref label = post_n - (ir3_node*) f->nodes.buf.addr;
	then_i = f->ins.buf.addr + then_i_idx;
	then_i->to = label;
	if (s->ifelse.s_else) {
		else_i = f->ins.buf.addr + else_i_idx;
		else_i->to = label;
	} else {
		br = f->ins.buf.addr + br_idx;
		br->R = label;
	}
	post_n[-1].end = post_n->begin = dyn_arr_size(&f->ins);
	break;
	}

case STMT_WHILE:
	{

	/*
	 * L0:
	 * while (c)
	 * 	s;
	 * L3:
	 *
	 * becomes:
	 * L0:
	 * goto L2
	 * L1:
	 * 	s;
	 * 	goto L2
	 * L2:
	 * 	if (c) goto L3
	 * 	else goto L1
	 * L3:
	 */

	ssa_instr *goto_cond = dyn_arr_push(&f->ins, NULL, sizeof *goto_cond, a);
	idx_t cond_idx = (void*) goto_cond - f->ins.buf.addr;
	goto_cond->kind = SSA_GOTO;

	ssa_ref lbl_body = dyn_arr_size(&f->nodes) / sizeof(ir3_node);
	ir3_node *body = dyn_arr_push(&f->nodes, NULL, sizeof *body, a);
	body[-1].end = body->begin = dyn_arr_size(&f->ins);
	ir3_stmt(f, s->ifelse.s_then, stk, blk, a);
	ssa_ref lbl_cond = dyn_arr_size(&f->nodes) / sizeof(ir3_node);
	goto_cond = f->ins.buf.addr + cond_idx;
	goto_cond->to = lbl_cond;
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_GOTO, lbl_cond }, sizeof(ssa_instr), a);

	ir3_node *cond_blk = dyn_arr_push(&f->nodes, NULL, sizeof *cond_blk, a);
	cond_blk[-1].end = cond_blk->begin = dyn_arr_size(&f->ins);
	ssa_ref cond = ir3_expr(f, s->ifelse.cond, stk, REF_NONE, a);
	ssa_ref check = new_local(&f->locals, &type_bool);
	dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_BOOL, check, 0 }, sizeof(ssa_instr), a);
	ssa_ref lbl_post = dyn_arr_size(&f->nodes) / sizeof(ir3_node);
	ir3_node *post = dyn_arr_push(&f->nodes, NULL, sizeof *post, a);
	buf[0] = (ssa_instr){ .kind=SSA_BR, SSAB_EQ, cond, check };
	buf[1] = (ssa_instr){ .L=lbl_post, .R=lbl_body };
	dyn_arr_push(&f->ins, buf, 2*sizeof *buf, a);
	post[-1].end = post->begin = dyn_arr_size(&f->ins);

	break;
	}

case STMT_BLOCK:
	{
	scope *sub = scratch_start(blk[0]->sub);
	map_stack top = { .scope=(*blk)++, .next=stk };
	for (stmt **iter = scratch_start(s->blk), **end = scratch_end(s->blk); iter != end; iter++) {
		ir3_stmt(f, *iter, &top, &sub, a);
	}
	break;
	}
default:
	__builtin_unreachable();
	}
}

static void ir3_decl_func(ir3_func *f, decl *d, map_stack *stk, scope** fsc, allocator *a)
{
	dyn_arr_init(&f->ins, 0, a);
	dyn_arr_init(&f->nodes, 0, a);
	ir3_node *first = dyn_arr_push(&f->nodes, NULL, sizeof *first, a);
	first->begin = 0; // end will be set by the next time something is pushed, and one last time at the end
	dyn_arr_init(&f->locals, 0, a);
	scope *sub = scratch_start(fsc[0]->sub);
	map_stack top = { .scope=(*fsc)++, .next=stk };
	for (decl *start = scratch_start(d->type->params), *arg = start; arg != scratch_end(d->type->params); arg++) {
		arg->id = arg - start;
		new_local(&f->locals, arg->type);
		// %2 = arg.2
		// no real constraint for both to be the same
		dyn_arr_push(&f->ins, &(ssa_instr){ .kind=SSA_ARG, arg - start, arg - start }, sizeof(ssa_instr), a);
	}
	for (stmt **iter = scratch_start(d->body), **end = scratch_end(d->body); iter != end; iter++) {
		ir3_stmt(f, *iter, &top, &sub, a);
	}
	ir3_node *last = f->nodes.end - sizeof *last;
	last->end = dyn_arr_size(&f->ins);
}

static void patch_relocs(ir3_module mod)
{
	ir3_sym *base_syms = scratch_start(mod);
	for (ir3_reloc *reloc = bytecode.relocs.buf.addr; reloc != bytecode.relocs.end; reloc++) {
		ir3_sym *sym = &base_syms[reloc->sym_in];
		ssa_instr *ins = sym->f.ins.buf.addr + reloc->offset_in;
		assert(reloc->ref->id != -1);
		ins->v = reloc->ref->id;
	}
}

// TODO:
// 1. convert to SSA
// 2. convert out of SSA

ir3_module convert_to_3ac(module_t ast, scope *enclosing, allocator *a)
{
	map_stack bottom = { .scope=enclosing, .next=NULL };
	// TODO: maybe incorporate fsc in the stack
	scope *fsc = scratch_start(enclosing->sub);
	for (decl_idx *start = scratch_start(ast), *end = scratch_end(ast),
			*iter = start; iter != end; iter++) {
		decl *d = idx2decl(*iter);
		assert(d->id == -1);
		d->id = bytecode.cur_idx = dyn_arr_size(&bytecode.blob) / sizeof(ir3_sym);
		ir3_sym sym;
		if (d->kind == DECL_STRUCT) {
			sym.kind = IR3_AGGREG;
			dyn_arr_init(&sym.fields, d->type->fields.cnt * sizeof(type*), a);
			type **base = sym.fields.buf.addr;
			for (map_entry *e = d->type->fields.m.addr, *end = d->type->fields.m.addr + d->type->fields.m.size;
					e != end; e++)
				if (e->k) {
					// does not allocate
					dyn_arr_push(&sym.fields, NULL, sizeof(type*), a);
					decl *field = (decl*) e->v;
					base[field->id] = field->type;
				}
			d->type->id = d->id;
			sym.back = d->type;
			dyn_arr_push(&bytecode.blob, &sym, sizeof sym, bytecode.temps);
			continue;
		}
		// TODO: maybe remove these since they only matter for object code
		map_entry global = global_name(d->name, ident_len(d->name), a);
		dyn_arr_push(&bytecode.names, &global, sizeof global, a);
		assert(d->kind == DECL_FUNC);
		sym.kind = IR3_FUNC;
		ptrdiff_t offset = dyn_arr_push(&bytecode.blob, NULL, sizeof sym, bytecode.temps) - bytecode.blob.buf.addr;
		ir3_decl_func(&sym.f, d, &bottom, &fsc, a);
		memcpy(bytecode.blob.buf.addr + offset, &sym, sizeof sym);
		// TODO: mmap trickery to reduce the need to copy potentially large amounts of data
	}
	ir3_module out = scratch_from(&bytecode.blob, bytecode.temps, a);
	patch_relocs(out);
	return out;
}

static void ir2_decl_func(ir3_func *dst, ir3_func *src, allocator *a)
{
	dyn_arr_init(&dst->ins, 0, a);
	dyn_arr_init(&dst->nodes, 0, a); // just lazy to make a new type without them
	dst->locals = src->locals;
	for (const ir3_node *start = src->nodes.buf.addr, *node = start; node != src->nodes.end; node++) {
		dyn_arr_push(&dst->ins, &(ssa_instr){ .kind=SSA_LABEL, node-start }, sizeof(ssa_instr), a);
		for (const ssa_instr *instr = src->ins.buf.addr + node->begin, *end = src->ins.buf.addr + node->end; instr != end; instr++) switch (instr->kind) {
		case SSA_IMM:
		case SSA_SET:
		case SSA_GLOBAL_REF:
			dyn_arr_push(&dst->ins, instr, sizeof *instr, a);
			instr++;
			/* fallthrough */
		case SSA_COPY:
		case SSA_BOOL:
		case SSA_RET:
		case SSA_GOTO:
		case SSA_ARG:
		case SSA_BOOL_NEG:
		case SSA_LOAD: case SSA_STORE: case SSA_ADDRESS:
		case SSA_MEMCOPY:
		case SSA_CONVERT:
		case SSA_OFFSETOF:
			dyn_arr_push(&dst->ins, instr, sizeof *instr, a);
			break;
		case SSA_ADD:
		case SSA_SUB:
		case SSA_MUL: // x86 mul/imul are a reminder that RAX was the accumulator // basically they are 1-address (can be 2/3 for imul)
			dyn_arr_push(&dst->ins, &(ssa_instr){ .kind=SSA_COPY, instr->to, instr->L }, sizeof *instr, a);
			dyn_arr_push(&dst->ins, &(ssa_instr){ .kind=instr->kind, instr->to, instr->to, instr->R }, sizeof *instr, a);
			break;
		case SSA_BR:
			dyn_arr_push(&dst->ins, &(ssa_instr){ .kind=SSA_BR, instr->to, instr->L, instr->R }, sizeof *instr, a);
			dyn_arr_push(&dst->ins, &(ssa_instr){ .L=instr[1].L }, sizeof *instr, a);
			dyn_arr_push(&dst->ins, &(ssa_instr){ .kind=SSA_GOTO, instr[1].R }, sizeof(ssa_instr), a);
			instr++;
			break;
		case SSA_CALL:
			{
			idx_t ratio = sizeof(ssa_extension) / sizeof(ssa_ref);
			idx_t num_ext = (instr->R + ratio - 1) / ratio + 1;
			dyn_arr_push(&dst->ins, instr, (1 + num_ext) * sizeof *instr, a);
			instr += num_ext;
			}
			break;
		default:
			assert(0);
		}
	}
	dst->num_labels = dyn_arr_size(&src->nodes) / sizeof(ir3_node);
	dyn_arr_fini(&src->ins, a);
	dyn_arr_fini(&src->nodes, a);
	dyn_arr_push(&dst->nodes, &(ir3_node){ .begin=0, .end=dyn_arr_size(&dst->ins) }, sizeof(ir3_node), a);
}

ir3_module convert_to_2ac(ir3_module m3ac, allocator *a)
{
	dyn_arr m2ac; dyn_arr_init(&m2ac, 0, a);
	for (ir3_sym *src = scratch_start(m3ac); src != scratch_end(m3ac); src++) {
		if (src->kind == IR3_BLOB || src->kind == IR3_AGGREG) {
			dyn_arr_push(&m2ac, src, sizeof *src, a);
		} else if (src->kind == IR3_FUNC) {
			ir3_sym *dst = dyn_arr_push(&m2ac, NULL, sizeof *dst, a);
			dst->kind = IR3_FUNC;
			ir2_decl_func(&dst->f, &src->f, a);
		}
	}
	scratch_fini(m3ac, a);
	return scratch_from(&m2ac, a, a);
}

static void ir3_fini(ir3_module m, allocator *a)
{
	for (ir3_sym *f = scratch_start(m); f != scratch_end(m); f++) {
		if (f->kind == IR3_BLOB) {
			DEALLOC(a, f->m);
		} else if (f->kind == IR3_FUNC) {
			dyn_arr_fini(&f->f.ins, a);
			dyn_arr_fini(&f->f.nodes, a);
			dyn_arr_fini(&f->f.locals, a);
		} else if (f->kind == IR3_AGGREG) {
			dyn_arr_fini(&f->fields, a);
		}
	}
	scratch_fini(m, a);
}

void test_3ac(void)
{
	extern int printf(const char *, ...);
	printf("==3AC==\n");

	allocator *gpa = (allocator*)&malloc_allocator;
	ast_init(gpa);
	allocator_geom perma; allocator_geom_init(&perma, 16, 8, 0x100, gpa);
	token_init("nyan/simpler.nyan", ast.temps, &perma.base);
	allocator_geom just_ast; allocator_geom_init(&just_ast, 10, 8, 0x100, gpa);
	module_t module = parse_module(&just_ast.base);
	scope global;
	resolve_refs(module, &global, ast.temps, &perma.base);
	type_init(gpa);
	type_check(module, &global, &just_ast.base);
	type_fini();
	token_fini();

	if (!ast.errors) {
		bytecode_init(gpa);
		ir3_module m3ac = convert_to_3ac(module, &global, gpa);

		print(stdout, "3-address code:\n");
		dump_3ac(m3ac, bytecode.names.buf.addr);
		ir3_module m2ac = convert_to_2ac(m3ac, gpa);
		// print(stdout, "2-address code:\n");
		// dump_3ac(m2ac, bytecode.names.buf.addr);

		map_fini(&tokens.idents, tokens.up);
		scope_fini(&global, ast.temps);
		allocator_geom_fini(&perma);

		gen_module gen = gen_x86_64(m2ac, gpa);
		allocator_geom_fini(&just_ast);
		ast_fini(gpa);
		int e = elf_object_from(&gen, "simpler.o", &bytecode.names, gpa);
		if (e < 0) perror("objfile not generated");

		gen_fini(&gen, gpa);
		ir3_fini(m2ac, gpa);
		for (map_entry *s = bytecode.names.buf.addr; s != bytecode.names.end; s++) {
			allocation m = { (void*)s->k, s->v };
			DEALLOC(gpa, m);
		}
		bytecode_fini();
	}
	// FIXME: else leaks
}

int dump_3ac(ir3_module m, map_entry *globals)
{
	int printed = 0;
	// assert(scratch_len(m) / sizeof(ir3_sym) == dyn_arr_size(&bytecode.names) / sizeof(map_entry));
	for (ir3_sym *start = scratch_start(m), *end = scratch_end(m),
			*f = start; f != end; f++) {
		printed += print(stdout, "sym.", (print_int){ f-start }, " ", (char*) globals->k);
		if (f->kind == IR3_FUNC) {
			printed += print(stdout, " func:\n", &f->f);
			globals++;
		} else if (f->kind == IR3_BLOB) {
			printed += print(stdout, " blob:\n");
			for (idx_t i = 0; i < (idx_t) f->m.size; i++)
				printed += print(stdout, (print_int){ i[(byte*) f->m.addr] }, " ");
			globals++;
		} else if (f->kind == IR3_AGGREG) {
			printed += print(stdout, " aggregate:\n");
			for (type **start = f->fields.buf.addr, **field = start; field != (type**) f->fields.end; field++)
				printed += print(stdout, "\t", (print_int){ field - start }, ": ", *field, "\n");
		} else assert(0);
		printed += print(stdout, "\n\n");
	}
	return printed;
}
