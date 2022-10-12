#include "ssa.h"
#include "type_check.h"
#include "dynarr.h"
#include "ast.h"
#include "alloc.h"
#include "map.h"
#include "token.h"
#include "print.h"

#include <assert.h>
#include <stdbool.h>


struct ssa_context ssa;

static const ssa_kind tok2ssa[TOKEN_NUM] = {
	['+'] = SSA_ADD, ['-'] = SSA_SUB,
};

static int _string_cmp2(key_t L, key_t R) { return L - R; }

static val_t idx_ref(idx_t i, ssa_ref ref)
{
	return (uint64_t) i << 32 | ref;
}

static idx_t ref3ac2idx(val_t v) { return v >> 32; }
static ssa_ref ref3ac2ssa(val_t v) { return v & 0xff; }

static ssa_ref conv3ac_expr(expr *e, map *refs, dyn_arr *ins, ssa_ref *next, allocator *a, value_category cat)
{
	// ahhh, wouldnt it be nice to just be able to write *p++ = ins{ .op=... }
	switch (e->kind) {
		ssa_instr i;
	case EXPR_INT:
		assert(cat == RVALUE); // the type checking has already been done. this is just a sanity check
		i.kind = SSA_INT;
		i.to = *next;
		dyn_arr_push(ins, &i, sizeof i, a);
		assert(sizeof (ssa_extension) == sizeof (uint32_t) && "adapt this a bit");
		i.v = e->value & ((1LU<<32) - 1);
		dyn_arr_push(ins, &i, sizeof i, a);
		i.v = e->value >> 32;
		dyn_arr_push(ins, &i, sizeof i, a);
		return (*next)++;
	case EXPR_NAME:
		{
		map_entry *entry = map_find(refs, e->name, string_hash(e->name), _string_cmp2);
		if (cat == LVALUE)
			entry->v = idx_ref(ref3ac2idx(entry->v), (*next)++);
		return ref3ac2ssa(entry->v);
		}
	case EXPR_BINARY:
		i.kind = tok2ssa[e->binary.op];
		i.L = conv3ac_expr(e->binary.L, refs, ins, next, a, RVALUE);
		i.R = conv3ac_expr(e->binary.R, refs, ins, next, a, RVALUE);
		i.to = (*next)++;
		dyn_arr_push(ins, &i, sizeof i, a);
		return i.to;
	case EXPR_CALL:
		i.kind = SSA_CALL;
		i.L = conv3ac_expr(e->call.operand, refs, ins, next, a, RVALUE);
		i.to = (*next)++;
		dyn_arr_push(ins, &i, sizeof i, a);
		return i.to;
	default:
		assert(0);
	}
}

static void conv3ac_decl(decl_idx i, map *refs, dyn_arr *ins, ssa_ref *next, allocator *a)
{
	decl *d = idx2decl(i);
	switch (d->kind) {
	case DECL_VAR:
		assert(!map_find(refs, d->name, string_hash(d->name), _string_cmp2));
		{
		map_entry *entry = map_add(refs, d->name, string_hash, a);
		entry->k = d->name;
		entry->v = idx_ref(-1, conv3ac_expr(d->var_d.init, refs, ins, next, a, RVALUE));
		}
		break;
	case DECL_FUNC:
		// should be handled in `conv3ac_func`
	default:
		assert(0);
	}
}

static void conv3ac_stmt(stmt *s, map *refs, dyn_arr *ins, ssa_ref *next, allocator *a)
{
	switch (s->kind) {
	case STMT_DECL:
		conv3ac_decl(s->d, refs, ins, next, a);
		break;
	case STMT_EXPR:
		conv3ac_expr(s->e, refs, ins, next, a, RVALUE);
		break;
	case STMT_ASSIGN:
		{
		ssa_ref R = conv3ac_expr(s->assign.R, refs, ins, next, a, RVALUE);
		ssa_ref L = conv3ac_expr(s->assign.L, refs, ins, next, a, LVALUE);
		ssa_instr i = { .kind=SSA_COPY, L, R };
		dyn_arr_push(ins, &i, sizeof i, a);
		}
		break;
	case STMT_RETURN:
		{
		ssa_ref r = conv3ac_expr(s->e, refs, ins, next, a, RVALUE);
		ssa_instr i = { .kind=SSA_RET, r };
		dyn_arr_push(ins, &i, sizeof i, a);
		}
		break;
	default:
		assert(0);
	}
}

static void conv3ac_func(decl *d, ssa_sym *to, scope *sc, allocator *a)
{
	to->name = d->name;
	map_clear(&ssa.refs);
	dyn_arr_init(&to->ins, 0*sizeof(ssa_instr), a);
	ssa_ref next = 0;

	// transfer from sc to refs
	for (map_entry *it = sc->refs.m.addr, *end = it + sc->refs.m.len/sizeof *it;
			it != end; it++) {
		if (!it->k) continue;
		idx_t idx = ref2idx(it->v);
		if (idx == -1) continue;
		map_entry *insert = map_add(&ssa.refs, it->k, string_hash, a);
		insert->k = it->k;
		insert->v = idx_ref(idx, next);

		ssa_instr i = { .kind=SSA_GLOBAL_REF, .to=next++ };
		dyn_arr_push(&to->ins, &i  , sizeof i, a);
		dyn_arr_push(&to->ins, &idx, sizeof i, a);
	}

	for (stmt **it = scratch_start(d->func_d.body), **end = scratch_end(d->func_d.body);
			it != end; it++) {
		conv3ac_stmt(*it, &ssa.refs, &to->ins, &next, a);
	}
}

ssa_module convert_to_3ac(module_t module, scope *sc, allocator *a)
{
	dyn_arr defs;
	dyn_arr_init(&defs, 0*sizeof(ssa_sym), a);
	decl_idx *decl_start = scratch_start(module), *decl_end = scratch_end(module), *decl_it = decl_start ;
	scope *scope_it = scratch_start(sc->sub);
	map_init(&ssa.refs, 2, a);
	for (; decl_it != decl_end; decl_it++, scope_it++) {
		decl *d = idx2decl(*decl_it);
		assert(d->kind == DECL_FUNC);
		ssa_sym *sym = dyn_arr_push(&defs, NULL, sizeof(ssa_sym), a);
		sym->idx = ref2idx(map_find(&sc->refs, d->name, string_hash(d->name), _string_cmp2)->v);
		conv3ac_func(d, sym, scope_it, a);
	}
	map_fini(&ssa.refs, a);
	return scratch_from(&defs, sizeof(ssa_sym), a, a);
}

void test_3ac(void)
{
	extern int printf(const char *, ...);
	printf("==3AC==\n");

	allocator *gpa = &malloc_allocator;
	ast_init(gpa);

	allocator_geom perma;
	allocator_geom_init(&perma, 16, 8, 0x100, gpa);

	token_init("cr/basic.cr", ast.temps, &perma.base);

	allocator_geom just_ast;
	allocator_geom_init(&just_ast, 10, 8, 0x100, gpa);
	module_t module = parse_module(&just_ast.base);

	scope global;
	resolve_init(1, gpa);
	resolve_refs(module, &global, ast.temps, &perma.base);
	resolve_fini(gpa);
	type_check(module, &global);

	if (!ast.errors) {
		ssa_module ssa_3ac = convert_to_3ac(module, &global, gpa);
		dump_3ac(ssa_3ac);

		for (ssa_sym *sym = scratch_start(ssa_3ac), *end = scratch_end(ssa_3ac);
				sym != end; sym++)
			dyn_arr_fini(&sym->ins, gpa);
		scratch_fini(ssa_3ac, gpa);
	}

	for (scope *it = scratch_start(global.sub), *end = scratch_end(global.sub);
			it != end; it++)
		map_fini(&it->refs, ast.temps);
	map_fini(&global.refs, ast.temps);
	allocator_geom_fini(&just_ast);
	token_fini();
	ast_fini(gpa);

	allocator_geom_fini(&perma);
}

int dump_3ac(ssa_module module)
{
	int prn = 0;
	for (ssa_sym *it = scratch_start(module), *end = scratch_end(module);
			it != end; it++)
		prn += print(stdout, *it);
	return prn;
}
