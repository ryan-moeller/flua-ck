/*
 * Copyright (c) 2025-2026 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ck_hp.h>
#include <ck_pr.h>
#include <ck_stack.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "pr.h"
#include "refcount.h"
#include "serde.h"
#include "serdebuf.h"
#include "luaerror.h"

#define CK_HP_RECORD_METATABLE "ck_hp_record_t"

#define SHARED_CONST_METATABLE "shared.const"
#define SHARED_MUT_METATABLE "shared.mut"
#define SHARED_PR_METATABLE "shared.pr"
#define SHARED_PR128_METATABLE "shared.pr128"

CK_STACK_CONTAINER(struct ck_hp_record, global_entry, ck_hp_record_container)

static ck_hp_t serialized_hp_domain;

#define HP_NPOINTERS 1

#ifndef HP_THRESHOLD
#define HP_THRESHOLD 1 /* TODO: tuning */
#endif

static void freeserialized(void *);

__attribute__((constructor(PRIO_HP)))
static void
init_hp_domains(void)
{
	ck_hp_init(&serialized_hp_domain, HP_NPOINTERS, HP_THRESHOLD,
	    freeserialized);
}

#if 0 /* this fails if the main thread dies and doesn't close created threads */
__attribute__((destructor(PRIO_HP)))
static void
fini_hp_domains(void)
{
	ck_hp_record_t *record;
	ck_stack_entry_t *entry, *next;

	CK_STACK_FOREACH_SAFE(&serialized_hp_domain.subscribers, entry, next) {
		record = ck_hp_record_container(entry);
		assert(record->state == CK_HP_FREE);
		free(record);
	}
}
#endif

#define HP_POINTERS_SIZE (sizeof(void *) * HP_NPOINTERS)
/* Allocate the pointers immediately following the record for simplicity. */
#define RECORD_ALLOCATION_SIZE (sizeof(ck_hp_record_t) + HP_POINTERS_SIZE)

static inline void
register_hp_record(lua_State *L, ck_hp_t *domain)
{
	ck_hp_record_t *record;

	/*
	 * Once registered, a record must survive for the lifetime of hp_domain.
	 * So, we have to allocate it on the heap but keep a lightuserdata
	 * uservalue for GC to purge and unregister the record when this thread
	 * is closed.
	 */
	if ((record = ck_hp_recycle(domain)) == NULL) {
		if ((record = malloc(RECORD_ALLOCATION_SIZE)) == NULL) {
			fatal(L, "malloc", ENOMEM);
		}
		/*
		 * The pointers follow record in the allocation and are
		 * initialized by ck_hp_register(), so we do not need to clear
		 * them beforehand.
		 */
		ck_hp_register(domain, record, (void **)(record + 1));
	}
	new(L, record, CK_HP_RECORD_METATABLE);
	lua_rawsetp(L, LUA_REGISTRYINDEX, domain);
}

static int
l_ck_hp_record_gc(lua_State *L)
{
	ck_hp_record_t *record;

	record = checkcookie(L, 1, CK_HP_RECORD_METATABLE);

	ck_hp_purge(record);
	ck_hp_unregister(record);
	return (0);
}

static inline ck_hp_record_t *
gethprecord(lua_State *L, ck_hp_t *domain)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, domain);
	return (checkcookie(L, -1, CK_HP_RECORD_METATABLE));
}

struct serialized {
	void *pointer;
	ck_hp_hazard_t hazard;
};

static inline int
serialize(lua_State *L, int idx, struct serialized **serializedp)
{
	struct serdebuf sb;
	struct serialized *serialized;
	serde_type_code type;
	int error;

	if ((error = serdebuf_init(L, idx, &sb)) != 0) {
		return (error);
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, idx, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		return (error);
	}
	if ((serialized = malloc(sizeof(*serialized))) == NULL) {
		serdebuf_destroy(&sb);
		return (ENOMEM);
	}
	if ((serialized->pointer = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		free(serialized);
		return (ENOMEM);
	}
	*serializedp = serialized;
	return (0);
}

static void
freeserialized(void *p)
{
	struct serialized *serialized = p;

	free(serialized->pointer);
	free(serialized);
}

struct rcshared {
	struct serialized *serialized;
	refcount refs;
};

static inline int
newshared(lua_State *L, const char *metatable)
{
	struct rcshared *sharedp;
	int error;

	luaL_checkany(L, 1);

	if ((sharedp = malloc(sizeof(*sharedp))) == NULL) {
		return (fatal(L, "malloc", ENOMEM));
	}
	if ((error = serialize(L, 1, &sharedp->serialized)) != 0) {
		free(sharedp);
		if (error < 0) {
			return (lua_error(L));
		}
		return (fatal(L, "serialize", error));
	}
	refcount_init(&sharedp->refs);
	return (new(L, sharedp, metatable));
}

static inline int
retainshared(lua_State *L, const char *metatable)
{
	struct rcshared *sharedp;

	sharedp = checklightuserdata(L, 1);

	refcount_retain(&sharedp->refs);
	return (new(L, sharedp, metatable));
}

static int
l_ck_shared_const_new(lua_State *L)
{
	return (newshared(L, SHARED_CONST_METATABLE));
}

static int
l_ck_shared_const_retain(lua_State *L)
{
	return (retainshared(L, SHARED_CONST_METATABLE));
}

static int
l_ck_shared_const_gc(lua_State *L)
{
	struct rcshared *sharedp;

	sharedp = checkcookie(L, 1, SHARED_CONST_METATABLE);

	if (refcount_release(&sharedp->refs)) {
		free(sharedp->serialized->pointer);
		free(sharedp->serialized);
		free(sharedp);
	}
	invalidate(L, 1);
	return (0);
}

static int
l_ck_shared_const_cookie(lua_State *L)
{
	checkcookieuv(L, 1, SHARED_CONST_METATABLE);

	return (1);
}

static int
l_ck_shared_const_load(lua_State *L)
{
	struct rcshared *sharedp;

	sharedp = checkcookie(L, 1, SHARED_CONST_METATABLE);

	if (loadshared(L, sharedp->serialized->pointer) == NULL) {
		return (lua_error(L));
	}
	return (1);
}

static int
l_ck_shared_mut_new(lua_State *L)
{
	return (newshared(L, SHARED_MUT_METATABLE));
}

static int
l_ck_shared_mut_retain(lua_State *L)
{
	return (retainshared(L, SHARED_MUT_METATABLE));
}

static int
l_ck_shared_mut_gc(lua_State *L)
{
	struct rcshared *sharedp;
	ck_hp_record_t *record;
	struct serialized *serialized;

	sharedp = checkcookie(L, 1, SHARED_MUT_METATABLE);

	if (refcount_release(&sharedp->refs)) {
		record = gethprecord(L, &serialized_hp_domain);
		do {
			serialized = ck_pr_load_ptr(&sharedp->serialized);
			ck_hp_set(record, 0, serialized);
		} while (ck_pr_load_ptr(&sharedp->serialized) != serialized);
		ck_hp_free(record, &serialized->hazard, serialized, serialized);
		ck_hp_set(record, 0, NULL);
		free(sharedp);
	}
	invalidate(L, 1);
	return (0);
}

static int
l_ck_shared_mut_cookie(lua_State *L)
{
	checkcookieuv(L, 1, SHARED_MUT_METATABLE);

	return (1);
}

static int
l_ck_shared_mut_load(lua_State *L)
{
	struct rcshared *sharedp;
	ck_hp_record_t *record;
	struct serialized *serialized;
	bool error;

	sharedp = checkcookie(L, 1, SHARED_MUT_METATABLE);

	record = gethprecord(L, &serialized_hp_domain);
	do {
		serialized = ck_pr_load_ptr(&sharedp->serialized);
		ck_hp_set(record, 0, serialized);
	} while (ck_pr_load_ptr(&sharedp->serialized) != serialized);
	error = loadshared(L, serialized->pointer) == NULL;
	ck_hp_set(record, 0, NULL);
	if (error) {
		return (lua_error(L));
	}
	return (1);
}

static int
l_ck_shared_mut_rfo(lua_State *L)
{
	struct rcshared *sharedp;

	sharedp = checkcookie(L, 1, SHARED_MUT_METATABLE);

	ck_pr_rfo(&sharedp->serialized);
	return (0);
}

static int
l_ck_shared_mut_store(lua_State *L)
{
	struct rcshared *sharedp;
	ck_hp_record_t *record;
	struct serialized *oldp, *newp;
	int error;

	sharedp = checkcookie(L, 1, SHARED_MUT_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serialize(L, 2, &newp)) != 0) {
		if (error < 0) {
			return (lua_error(L));
		}
		return (fatal(L, "serialize", error));
	}
	oldp = ck_pr_fas_ptr(&sharedp->serialized, newp);
	record = gethprecord(L, &serialized_hp_domain);
	/* TODO: retire vs free? */
	ck_hp_free(record, &oldp->hazard, oldp, oldp);
	return (0);
}

_Static_assert(sizeof(uint8_t) == sizeof(bool), "bad bool size");
_Static_assert(sizeof(uint64_t) == sizeof(lua_Integer), "bad lua_Integer size");
_Static_assert(sizeof(double) == sizeof(lua_Number), "bad lua_Number size");

/*
 *            ST
 *        FT      PUSH/FIELD     TO        ...
 */
#define SERDE_BOOLEAN_T(X, ...) \
	X(8,      boolean,       boolean,  __VA_ARGS__)
#define SERDE_LIGHTUSERDATA_T(X, ...) \
	X(PTR,    lightuserdata, userdata, __VA_ARGS__)
#define SERDE_INTEGER_T(X, ...) \
	X(64,     integer,       integer,  __VA_ARGS__)
#define SERDE_NUMBER_T(X, ...) \
	X(DOUBLE, number,        number,   __VA_ARGS__)

/*        ST             ... */
#define SERDE_F_TYPES_LIST(X, ...) \
	X(BOOLEAN,       __VA_ARGS__) \
	X(LIGHTUSERDATA, __VA_ARGS__) \
	X(INTEGER,       __VA_ARGS__) \
	X(NUMBER,        __VA_ARGS__)
#define SERDE_F_BITWISE_TYPES_LIST(X, ...) \
	X(BOOLEAN,       __VA_ARGS__) \
	X(LIGHTUSERDATA, __VA_ARGS__) \
	X(INTEGER,       __VA_ARGS__)
#define SERDE_F_BITRMW_TYPES_LIST(X, ...) \
	X(LIGHTUSERDATA, __VA_ARGS__) \
	X(INTEGER,       __VA_ARGS__)

#define SERDE_PR_T_EXPANDER(FT, PUSH, TO, X, ...) \
	PR_##FT##_T(X, FT, PUSH, TO, __VA_ARGS__)

#define SERDE_T_EXPANDER(ST, X, ...) \
	SERDE_##ST##_T(SERDE_PR_T_EXPANDER, X, ST, __VA_ARGS__)

#define SERDE_PR_TYPES_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)

struct rcsharedpr {
	union {
#define SERDE_PR_FIELD(CKT, CT, DT, FT, NAME, ...) \
		CT NAME;
		SERDE_PR_TYPES_LIST(SERDE_PR_FIELD)
#undef SERDE_PR_FIELD
	};
	enum serde_type type;
	refcount refs;
};

static int
l_ck_shared_pr_new(lua_State *L)
{
	struct rcsharedpr *sharedp;

	luaL_checkany(L, 1);

	if ((sharedp = malloc(sizeof(*sharedp))) == NULL) {
		return (fatal(L, "malloc", ENOMEM));
	}
	switch ((sharedp->type = serde_type(L, 1))) {
#define SERDE_PR_NEW_IMPL(CKT, CT, DT, FT, FIELD, TO, ST, ...) \
	case SERDE_##ST: \
		sharedp->FIELD = lua_to##TO(L, 1); \
		break;
	SERDE_PR_TYPES_LIST(SERDE_PR_NEW_IMPL)
#undef SERDE_PR_NEW_IMPL
	default:
		free(sharedp);
		return (luaL_typeerror(L, 1,
		    "boolean, lightuserdata, integer, or number"));
	}
	refcount_init(&sharedp->refs);
	return (new(L, sharedp, SHARED_PR_METATABLE));
}

static int
l_ck_shared_pr_retain(lua_State *L)
{
	struct rcsharedpr *sharedp;

	sharedp = checklightuserdata(L, 1);

	refcount_retain(&sharedp->refs);
	return (new(L, sharedp, SHARED_PR_METATABLE));
}

static int
l_ck_shared_pr_gc(lua_State *L)
{
	struct rcsharedpr *sharedp;

	sharedp = checkcookie(L, 1, SHARED_PR_METATABLE);

	if (refcount_release(&sharedp->refs)) {
		free(sharedp);
	}
	invalidate(L, 1);
	return (0);
}

static int
l_ck_shared_pr_cookie(lua_State *L)
{
	checkcookieuv(L, 1, SHARED_PR_METATABLE);

	return (1);
}

static int l_ck_shared_pr_rfo(lua_State *L)
{
	struct rcsharedpr *sharedp;

	sharedp = checkcookie(L, 1, SHARED_PR_METATABLE);

	ck_pr_rfo(sharedp);
	return (0);
}

#define CK_PR(OP, CKT, VARIANT) ck_pr_##OP##_##CKT##VARIANT

#define SERDE_PR_UNARY_LOAD_CHECKS(idx) ({})
#define SERDE_PR_UNARY_LOAD_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_UNARY_LOAD_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PUSH(L, PR(p)); 1; \
})

#define SERDE_PR_UNARY_STORE_CHECKS(idx) ({ \
	luaL_checkany(L, idx); \
})
#define SERDE_PR_UNARY_STORE_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_UNARY_STORE_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PR(p, TO(L, idx)); 0; \
})

#define SERDE_PR_UNARY_ARITHMETIC_CHECKS(idx) ({})
#define SERDE_PR_UNARY_ARITHMETIC_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_UNARY_ARITHMETIC_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PR(p); 0; \
})

#define SERDE_PR_UNARY_ARITHMETIC_Z_CHECKS(idx) ({})
#define SERDE_PR_UNARY_ARITHMETIC_Z_LIST(X, ...) \
	SERDE_F_BITWISE_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_UNARY_ARITHMETIC_Z_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	lua_pushboolean(L, PR(p)); 1; \
})

#define SERDE_PR_UNARY_BITWISE_CHECKS(idx) ({})
#define SERDE_PR_UNARY_BITWISE_LIST(X, ...) \
	SERDE_F_BITWISE_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_UNARY_BITWISE_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PR(p); 0; \
})

#define SERDE_PR_BINARY_ARITHMETIC_CHECKS(idx) ({ \
	luaL_checkany(L, idx); \
})
#define SERDE_PR_BINARY_ARITHMETIC_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_BINARY_ARITHMETIC_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PR(p, (DT)TO(L, idx)); 0; \
})

#define SERDE_PR_BINARY_BITWISE_CHECKS(idx) ({ \
	luaL_checkany(L, idx); \
})
#define SERDE_PR_BINARY_BITWISE_LIST(X, ...) \
	SERDE_F_BITWISE_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_BINARY_BITWISE_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PR(p, (DT)TO(L, idx)); 0; \
})

#define SERDE_PR_BINARY_FAA_CHECKS(idx) ({ \
	luaL_checkany(L, idx); \
})
#define SERDE_PR_BINARY_FAA_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_BINARY_FAA_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PUSH(L, (CT)PR(p, (DT)TO(L, idx))); 1; \
})

#define SERDE_PR_BINARY_FAS_CHECKS(idx) ({ \
	luaL_checkany(L, idx); \
})
#define SERDE_PR_BINARY_FAS_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_BINARY_FAS_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	PUSH(L, PR(p, TO(L, idx))); 1; \
})

#define SERDE_PR_TERNARY_CAS_CHECKS(idx) ({ \
	luaL_checkany(L, idx); \
	luaL_checkany(L, idx + 1); \
})
#define SERDE_PR_TERNARY_CAS_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_TERNARY_CAS_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	lua_pushboolean(L, PR(p, TO(L, idx), TO(L, idx + 1))); 1; \
})

#define SERDE_PR_TERNARY_CAS_VALUE_CHECKS(idx) ({ \
	luaL_checkany(L, idx); \
	luaL_checkany(L, idx + 1); \
})
#define SERDE_PR_TERNARY_CAS_VALUE_LIST(X, ...) \
	SERDE_F_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_TERNARY_CAS_VALUE_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	CT original; \
	lua_pushboolean(L, PR(p, TO(L, idx), TO(L, idx + 1), &original)); \
	PUSH(L, original); 2; \
})

#define SERDE_PR_BIT_RMW_CHECKS(idx) ({ \
	luaL_checkinteger(L, idx); \
})
#define SERDE_PR_BIT_RMW_LIST(X, ...) \
	SERDE_F_BITRMW_TYPES_LIST(SERDE_T_EXPANDER, X, __VA_ARGS__)
#define SERDE_PR_BIT_RMW_IMPL(idx, PR, PUSH, TO, CT, DT) ({ \
	lua_pushboolean(L, PR(p, lua_tointeger(L, idx))); 1; \
})

#define SERDE_PR_OP_CASE(CKT, CT, DT, FT, PUSH, TO, ST, OP, VARIANT, CLASS, ...) \
	case SERDE_##ST: { \
		CT *p = &sharedp->PUSH; \
		return (SERDE_PR_##CLASS##_IMPL(2, CK_PR(OP, CKT, VARIANT), \
		    lua_push##PUSH, lua_to##TO, CT, DT)); \
	}

#define SERDE_PR_OP(F, FV, OP, VARIANT, CLASS, ...) \
static int \
l_ck_shared_pr_##OP##VARIANT(lua_State *L) \
{ \
	struct rcsharedpr *sharedp; \
\
	sharedp = checkcookie(L, 1, SHARED_PR_METATABLE); \
	SERDE_PR_##CLASS##_CHECKS(2); \
\
	switch (sharedp->type) { \
	SERDE_PR_##CLASS##_LIST(SERDE_PR_OP_CASE, OP, VARIANT, CLASS) \
	default: \
		return (luaL_error(L, "internal error")); \
	} \
}

PR_OPS_LIST(SERDE_PR_OP);

static inline void
lua_pushchar(lua_State *L, char c)
{
	lua_pushlstring(L, &c, 1);
}

static inline char
lua_tochar(lua_State *L, int idx)
{
	const char *s;
	size_t len;

	s = lua_tolstring(L, idx, &len);
	return (len > 0 ? s[0] : '\0');
}

/*                  FT             X NAME lua_push*      lua_to*  ... */
#define SERDE_PR128_8(X, ...)      X(u8,  integer,       integer,  __VA_ARGS__)
#define SERDE_PR128_16(X, ...)     X(u16, integer,       integer,  __VA_ARGS__)
#define SERDE_PR128_32(X, ...)     X(u32, integer,       integer,  __VA_ARGS__)
#define SERDE_PR128_64(X, ...)     X(u64, integer,       integer,  __VA_ARGS__)
#define SERDE_PR128_CHAR(X, ...)   X(c,   char,          char,     __VA_ARGS__)
#define SERDE_PR128_DOUBLE(X, ...) X(d,   number,        number,   __VA_ARGS__)
#define SERDE_PR128_INT(X, ...)    X(i,   integer,       integer,  __VA_ARGS__)
#define SERDE_PR128_PTR(X, ...)    X(p,   lightuserdata, userdata, __VA_ARGS__)
#define SERDE_PR128_UINT(X, ...)   X(u,   integer,       integer,  __VA_ARGS__)

#define SERDE_PR128_T_EXPANDER(CKT, CT, DT, FT, N, X, ...) \
	SERDE_PR128_##FT(X, CKT, CT, DT, FT, N, __VA_ARGS__)

#define SERDE_PR128_TYPES_LIST(X, ...) \
	PR128_TYPES_LIST(SERDE_PR128_T_EXPANDER, X, __VA_ARGS__)

struct rcsharedpr128 {
	union {
#define SERDE_PR128_FIELD(NAME, PUSH, TO, CKT, CT, DT, FT, N, ...) \
		CT NAME[N];
		SERDE_PR128_TYPES_LIST(SERDE_PR128_FIELD)
#undef SERDE_PR128_FIELD
	};
	refcount refs;
};

#define MD128_ALIGN sizeof(uint64_t[2])

static int
l_ck_shared_pr_md128_new(lua_State *L)
{
	struct rcsharedpr128 *sharedp;
	const char *s;
	size_t len;
	int i, n, type;
	bool integer;

	switch (lua_type(L, 1)) {
	case LUA_TNONE:
	case LUA_TNIL:
		s = NULL;
		n = 0;
		break;
	case LUA_TSTRING:
		s = luaL_checklstring(L, i, &len);
		if (len > sizeof(sharedp->c)) {
			goto bad;
		}
		break;
	case LUA_TTABLE:
		s = NULL;
		type = lua_geti(L, 1, 1);
		integer = lua_isinteger(L, -1);
		switch ((n = luaL_len(L, 1))) {
		case 2:
			switch (type) {
			case LUA_TLIGHTUSERDATA:
			case LUA_TNUMBER:
				break;
			default:
				goto bad;
			}
			break;
		case 4:
		case 8:
		case 16:
			switch (type) {
			case LUA_TNUMBER:
				if (integer) {
					break;
				}
				goto bad;
			default:
				goto bad;
			}
			break;
		default:
			goto bad;
		}
		for (i = 2; i <= n; i++) {
			if (lua_geti(L, 1, i) != type) {
				goto bad;
			}
			if (integer && !lua_isinteger(L, -1)) {
				goto bad;
			}
		}
		lua_pop(L, n);
		break;
	default:
		return (luaL_typeerror(L, 1, "table or string or nil or none"));
	}

	if ((sharedp = aligned_alloc(MD128_ALIGN, sizeof(*sharedp))) == NULL) {
		return (fatal(L, "aligned_alloc", ENOMEM));
	}
	if (s != NULL) {
		assert(len <= sizeof(sharedp->c));
		memcpy(sharedp->c, s, len);
		if (len < 16) {
			memset(sharedp->c + len, 0, sizeof(sharedp->c) - len);
		}
	} else for (i = 0; i < n; i++) {
		lua_geti(L, 1, i + 1);
		switch (type) {
		case LUA_TUSERDATA:
			sharedp->p[i] = lua_touserdata(L, -1);
			break;
		case LUA_TNUMBER:
			switch (n) {
			case 2:
				if (integer) {
					sharedp->u64[i] = lua_tointeger(L, -1);
				} else {
					sharedp->d[i] = lua_tonumber(L, -1);
				}
				break;
			case 4:
				assert(integer);
				sharedp->u32[i] = lua_tointeger(L, -1);
				break;
			case 8:
				assert(integer);
				sharedp->u16[i] = lua_tointeger(L, -1);
				break;
			case 16:
				assert(integer);
				sharedp->u8[i] = lua_tointeger(L, -1);
				break;
			default:
				__unreachable();
			}
			break;
		}
	}
	refcount_init(&sharedp->refs);
	return (new(L, sharedp, SHARED_PR128_METATABLE));
bad:
	return (luaL_argerror(L, 1, NULL));
}

static int
l_ck_shared_pr_md128_retain(lua_State *L)
{
	struct rcsharedpr128 *sharedp;

	sharedp = checklightuserdata(L, 1);

	refcount_retain(&sharedp->refs);
	return (new(L, sharedp, SHARED_PR128_METATABLE));
}

static int
l_ck_shared_pr_md128_gc(lua_State *L)
{
	struct rcsharedpr128 *sharedp;

	sharedp = checkcookie(L, 1, SHARED_PR128_METATABLE);

	if (refcount_release(&sharedp->refs)) {
		free(sharedp);
	}
	invalidate(L, 1);
	return (0);
}

static const char *md128_views[] = {
#define MD128_VIEW(NAME, ...) #NAME,
	SERDE_PR128_TYPES_LIST(MD128_VIEW)
#undef MD128_VIEW
	 NULL,
};

enum {
	MD128_VALUE,
	MD128_INDEX,
};

static int
l_ck_shared_pr_md128_index(lua_State *L)
{
	struct rcsharedpr128 *sharedp;
	int view;

	sharedp = checkcookie(L, 1, SHARED_PR128_METATABLE);
	if (luaL_getmetafield(L, 1, lua_tostring(L, 2)) != LUA_TNIL) {
		return (1);
	}
	view = luaL_checkoption(L, 2, NULL, md128_views);

	lua_pushfstring(L, "%s.%s", SHARED_PR128_METATABLE, md128_views[view]);
	lua_createtable(L, 1, 0);
	luaL_setmetatable(L, lua_tostring(L, -2));
	lua_pushvalue(L, 1);
	lua_rawseti(L, -2, MD128_VALUE);
	return (1);
}

static int
l_ck_shared_pr_md128_cookie(lua_State *L)
{
	checkcookieuv(L, 1, SHARED_PR128_METATABLE);

	return (1);
}

static int
l_ck_shared_pr_md128_rfo(lua_State *L)
{
	struct rcsharedpr128 *sharedp;

	sharedp = checkcookie(L, 1, SHARED_PR128_METATABLE);

	ck_pr_rfo(sharedp);
	return (0);
}

#define SERDE_PR128_TERNARY_CAS_CHECKS(idx, N) ({ \
	luaL_checktype(L, idx, LUA_TTABLE); \
	luaL_argcheck(L, luaL_len(L, idx) == N, idx, "bad length"); \
	luaL_checktype(L, idx + 1, LUA_TTABLE); \
	luaL_argcheck(L, luaL_len(L, idx + 1) == N, idx + 1, "bad length"); \
})
#define SERDE_PR128_TERNARY_CAS_IMPL(idx, PUSH, TO, CKT, CT, DT, N) ({ \
	CT old_value[N]; \
	CT new_value[N]; \
	for (int i = 0; i < N; i++) { \
		lua_rawgeti(L, idx, i + 1); \
		old_value[i] = TO(L, -1); \
		lua_rawgeti(L, idx + 1, i + 1); \
		new_value[i] = TO(L, -1); \
	} \
	lua_pushboolean(L, ck_pr_cas_##CKT##_##N(p, old_value, new_value)); 1; \
})

#define SERDE_PR128_TERNARY_CAS_VALUE_CHECKS(idx, N) ({ \
	luaL_checktype(L, idx, LUA_TTABLE); \
	luaL_argcheck(L, luaL_len(L, idx) == N, idx, "bad length"); \
	luaL_checktype(L, idx + 1, LUA_TTABLE); \
	luaL_argcheck(L, luaL_len(L, idx + 1) == N, idx + 1, "bad length"); \
})
#define SERDE_PR128_TERNARY_CAS_VALUE_IMPL(idx, PUSH, TO, CKT, CT, DT, N) ({ \
	CT old_value[N]; \
	CT new_value[N]; \
	CT current_value[N]; \
	for (int i = 0; i < N; i++) { \
		lua_rawgeti(L, idx, i + 1); \
		old_value[i] = TO(L, -1); \
		lua_rawgeti(L, idx + 1, i + 1); \
		new_value[i] = TO(L, -1); \
	} \
	lua_pushboolean(L, ck_pr_cas_##CKT##_##N##_value(p, old_value, \
	    new_value, current_value)); \
	lua_createtable(L, N, 0); \
	for (int i = 0; i < N; i++) { \
		PUSH(L, current_value[i]); \
		lua_rawseti(L, -2, i + 1); \
	}; 2; \
})

#define ck_pr_md_load_64_2 ck_pr_load_64_2
#define ck_pr_md_load_ptr_2 ck_pr_load_ptr_2

#define SERDE_PR128_UNARY_LOAD_CHECKS(idx, N) ({})
#define SERDE_PR128_UNARY_LOAD_IMPL(idx, PUSH, TO, CKT, CT, DT, N) ({ \
	CT value[N]; \
	ck_pr_md_load_##CKT##_##N(p, value); \
	lua_createtable(L, N, 0); \
	for (int i = 0; i < N; i++) { \
		PUSH(L, value[i]); \
		lua_rawseti(L, -2, i + 1); \
	}; 1; \
})

#define SERDE_PR128_VIEW_OP_IMPL( \
    F, FV, N, OP, VARIANT, CLASS, NAME, PUSH, TO, CKT, CT, DT, ...) \
static int \
l_ck_shared_pr_md128_##NAME##_##OP##VARIANT(lua_State *L) \
{ \
	struct rcsharedpr128 *sharedp; \
	CT *p; \
\
	luaL_checktype(L, 1, LUA_TTABLE); \
	lua_rawgeti(L, 1, MD128_VALUE); \
	sharedp = checkcookie(L, -1, SHARED_PR128_METATABLE); \
	SERDE_PR128_##CLASS##_CHECKS(2, N); \
\
	p = sharedp->NAME; \
	return (SERDE_PR128_##CLASS##_IMPL(2, \
	    lua_push##PUSH, lua_to##TO, CKT, CT, DT, N)); \
}

#define SERDE_PR128_VIEW_OP(OP, NAME, PUSH, TO, CKT, CT, DT, FT, ...) \
	PR128_OP_##OP##_##FT(SERDE_PR128_VIEW_OP_IMPL, NAME, PUSH, TO, CKT, \
	    CT, DT, FT, __VA_ARGS__)

#define SERDE_PR128_VIEW_I_OP_IMPL( \
    F, FV, OP, VARIANT, CLASS, NAME, PUSH, TO, CKT, CT, DT, FT, N, ...) \
static int \
l_ck_shared_pr_md128_##NAME##_i_##OP##VARIANT(lua_State *L) \
{ \
	struct rcsharedpr128 *sharedp; \
	CT *p; \
	size_t i; \
\
	luaL_checktype(L, 1, LUA_TTABLE); \
	lua_rawgeti(L, 1, MD128_VALUE); \
	sharedp = checkcookie(L, -1, SHARED_PR128_METATABLE); \
	lua_rawgeti(L, 1, MD128_INDEX); \
	i = luaL_checkinteger(L, -1); \
	luaL_argcheck(L, i > 0 && i <= N, 1, "index out of bounds"); \
	SERDE_PR_##CLASS##_CHECKS(2); \
\
	p = &sharedp->NAME[i]; \
	return (SERDE_PR_##CLASS##_IMPL(2, CK_PR(OP, CKT, VARIANT), \
	    lua_push##PUSH, lua_to##TO, CT, DT)); \
}

#define SERDE_PR128_VIEW_I_OP(OP, NAME, PUSH, TO, CKT, CT, DT, FT, ...) \
	PR_OP_##OP##_##FT(SERDE_PR128_VIEW_I_OP_IMPL, NAME, PUSH, TO, CKT, CT, \
	    DT, FT, __VA_ARGS__)

/* metatable for methods on e.g. value.u32 */
#define SHARED_PR128_VIEW_METATABLE(NAME) \
	SHARED_PR128_METATABLE"."#NAME

/* metatable for methods on e.g. value.u32[i] */
#define SHARED_PR128_VIEW_I_METATABLE(NAME) \
	SHARED_PR128_VIEW_METATABLE(NAME)"[#]"

#define SERDE_PR128_VIEW(NAME, PUSH, TO, CKT, CT, DT, FT, ...) \
static int \
l_ck_shared_pr_md128_##NAME##_index(lua_State *L) \
{ \
	luaL_checktype(L, 1, LUA_TTABLE); \
	if (lua_type(L, 2) == LUA_TSTRING) { \
		if (luaL_getmetafield(L, 1, lua_tostring(L, 2)) == LUA_TNIL) { \
			return (0); \
		} \
		return (1); \
	} \
	luaL_checkinteger(L, 2); \
\
	lua_createtable(L, 0, 2); \
	luaL_setmetatable(L, SHARED_PR128_VIEW_I_METATABLE(NAME)); \
	lua_rawgeti(L, 1, MD128_VALUE); \
	lua_rawseti(L, -2, MD128_VALUE); \
	lua_pushvalue(L, 2); \
	lua_rawseti(L, -2, MD128_INDEX); \
	return (1); \
} \
PR128_##FT##_OPS_LIST(SERDE_PR128_VIEW_OP, NAME, PUSH, TO, CKT, CT, DT, FT, \
    __VA_ARGS__) \
PR_##FT##_OPS_LIST(SERDE_PR128_VIEW_I_OP, NAME, PUSH, TO, CKT, CT, DT, FT, \
    __VA_ARGS__)

SERDE_PR128_TYPES_LIST(SERDE_PR128_VIEW)

static const struct luaL_Reg l_ck_hp_record_meta[] = {
	{"__gc", l_ck_hp_record_gc},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_shared_const_funcs[] = {
	{"new", l_ck_shared_const_new},
	{"retain", l_ck_shared_const_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_shared_const_meta[] = {
	{"__gc", l_ck_shared_const_gc},
	{"cookie", l_ck_shared_const_cookie},
	{"load", l_ck_shared_const_load},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_shared_mut_funcs[] ={
	{"new", l_ck_shared_mut_new},
	{"retain", l_ck_shared_mut_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_shared_mut_meta[] = {
	{"__gc", l_ck_shared_mut_gc},
	{"cookie", l_ck_shared_mut_cookie},
	{"load", l_ck_shared_mut_load},
	{"rfo", l_ck_shared_mut_rfo},
	{"store", l_ck_shared_mut_store},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_shared_pr_funcs[] = {
	{"new", l_ck_shared_pr_new},
	{"retain", l_ck_shared_pr_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_shared_pr_meta[] = {
	{"__gc", l_ck_shared_pr_gc},
	{"cookie", l_ck_shared_pr_cookie},
	{"rfo", l_ck_shared_pr_rfo},
#define SHARED_PR_OPS_REG(F, FV, OP, VARIANT, CLASS, ...) \
	{#OP#VARIANT, l_ck_shared_pr_##OP##VARIANT},
	PR_OPS_LIST(SHARED_PR_OPS_REG)
#undef SHARED_PR_OPS_REG
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_shared_pr_md128_funcs[] = {
	{"new", l_ck_shared_pr_md128_new},
	{"retain", l_ck_shared_pr_md128_retain},
	{NULL, NULL}
};

/*
 * MD128/PR128 Metatables
 *
 * Value References <md128 value>
 * ___________
 * } md128   {
 * | __gc    |
 * | __index | => Typed Views
 * | cookie  |
 * | rfo     |
 * ===========
 *
 * Typed Views {<md128 value>}
 * _____________ _____________ _____________ _____________
 * } u8        { } u16       { } u32       { } u64       {
 * | __index   | | __index   | | __index   | | __index   | => Components
 * | cas       | | cas       | | cas       | | cas       |
 * | cas_value | | cas_value | | cas_value | | cas_value |
 * | load      | | load      | | load      | | load      |
 * ============= ============= ============= =============
 * _____________ ___________ _____________ _____________ _____________
 * } c         { } d       { } i         { } p         { } u         {
 * | __index   | | __index | | __index   | | __index   | | __index   |
 * | cas       | | cas     | | cas       | | cas       | | cas       |
 * | cas_value | =========== | cas_value | | cas_value | | cas_value |
 * | load      |             | load      | | load      | | load      |
 * =============             ============= ============= =============
 *
 * Typed View Components (self-indexed) {<md128 value>,<integer index>}
 * _______________ _______________ _______________ _______________
 * } u8[#]       { } u16[#]      { } u32[#]      { } u64[#]      {
 * | add         | | add         | | add         | | add         |
 * | and         | | and         | | and         | | and         |
 * | cas         | | btc         | | btc         | | btc         |
 * | cas_value   | | btr         | | btr         | | btr         |
 * | dec         | | bts         | | bts         | | bts         |
 * | dec_is_zero | | cas         | | cas         | | cas         |
 * | faa         | | cas_value   | | cas_value   | | cas_value   |
 * | fas         | | dec         | | dec         | | dec         |
 * | inc         | | dec_is_zero | | dec_is_zero | | dec_is_zero |
 * | inc_is_zero | | faa         | | faa         | | faa         |
 * | load        | | fas         | | fas         | | fas         |
 * | neg         | | inc         | | inc         | | inc         |
 * | neg_is_zero | | inc_is_zero | | inc_is_zero | | inc_is_zero |
 * | not         | | load        | | load        | | load        |
 * | or          | | neg         | | neg         | | neg         |
 * | store       | | neg_is_zero | | neg_is_zero | | neg_is_zero |
 * | sub         | | not         | | not         | | not         |
 * | xor         | | or          | | or          | | or          |
 * =============== | store       | | store       | | store       |
 *                 | sub         | | sub         | | sub         |
 *                 | xor         | | xor         | | xor         |
 *                 =============== =============== ===============
 * _______________ _____________ _______________ _______________ _______________
 * } c[#]        { } d[#]      { } i[#]        { } p[#]        { } u[#]        {
 * | add         | | add       | | add         | | add         | | add         |
 * | and         | | cas       | | and         | | and         | | and         |
 * | cas         | | cas_value | | btc         | | btc         | | btc         |
 * | cas_value   | | dec       | | btr         | | btr         | | btr         |
 * | dec         | | faa       | | bts         | | bts         | | bts         |
 * | dec_is_zero | | fas       | | cas         | | cas         | | cas         |
 * | faa         | | inc       | | cas_value   | | cas_value   | | cas_value   |
 * | fas         | | load      | | dec         | | dec         | | dec         |
 * | inc         | | neg       | | dec_is_zero | | dec_is_zero | | dec_is_zero |
 * | inc_is_zero | | store     | | faa         | | faa         | | faa         |
 * | load        | | sub       | | fas         | | fas         | | fas         |
 * | neg         | ============= | inc         | | inc         | | inc         |
 * | neg_is_zero |               | inc_is_zero | | inc_is_zero | | inc_is_zero |
 * | not         |               | load        | | load        | | load        |
 * | or          |               | neg         | | neg         | | neg         |
 * | store       |               | neg_is_zero | | neg_is_zero | | neg_is_zero |
 * | sub         |               | not         | | not         | | not         |
 * | xor         |               | or          | | or          | | or          |
 * ===============               | store       | | store       | | store       |
 *                               | sub         | | sub         | | sub         |
 *                               | xor         | | xor         | | xor         |
 *                               =============== =============== ===============
 */

static const struct luaL_Reg l_ck_shared_pr_md128_meta[] = {
	{"__gc", l_ck_shared_pr_md128_gc},
	{"__index", l_ck_shared_pr_md128_index},
	{"cookie", l_ck_shared_pr_md128_cookie},
	{"rfo", l_ck_shared_pr_md128_rfo},
	{NULL, NULL}
};

#define MD128_VIEW_OP_IMPL(F, FV, N, OP, VARIANT, CLASS, NAME, ...) \
	{#OP#VARIANT, l_ck_shared_pr_md128_##NAME##_##OP##VARIANT},

#define MD128_VIEW_OP(OP, NAME, PUSH, TO, CKT, CT, DT, FT, ...) \
	PR128_OP_##OP##_##FT(MD128_VIEW_OP_IMPL, NAME)

#define MD128_VIEW_I_OP_IMPL(F, FV, OP, VARIANT, CLASS, NAME, ...) \
	{#OP#VARIANT, l_ck_shared_pr_md128_##NAME##_i_##OP##VARIANT},

#define MD128_VIEW_I_OP(OP, NAME, PUSH, TO, CKT, CT, DT, FT, ...) \
	PR_OP_##OP##_##FT(MD128_VIEW_I_OP_IMPL, NAME)

#define MD128_VIEW_META(NAME, PUSH, TO, CKT, CT, DT, FT, ...) \
static const struct luaL_Reg l_ck_shared_pr_md128_##NAME##_meta[] = { \
	{"__index", l_ck_shared_pr_md128_##NAME##_index}, \
	PR128_##FT##_OPS_LIST(MD128_VIEW_OP, NAME, PUSH, TO, CKT, CT, DT, FT) \
	{NULL, NULL} \
}; \
static const struct luaL_Reg l_ck_shared_pr_md128_##NAME##_i_meta[] = { \
	PR_##FT##_OPS_LIST(MD128_VIEW_I_OP, NAME, PUSH, TO, CKT, CT, DT, FT) \
	{NULL, NULL} \
};
SERDE_PR128_TYPES_LIST(MD128_VIEW_META)
#undef MD128_VIEW_META

int
luaopen_ck_shared(lua_State *L)
{
	luaL_newmetatable(L, CK_HP_RECORD_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_hp_record_meta, 0);
	register_hp_record(L, &serialized_hp_domain);

	luaL_newmetatable(L, SHARED_CONST_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_shared_const_meta, 0);

	luaL_newmetatable(L, SHARED_MUT_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_shared_mut_meta, 0);

	luaL_newmetatable(L, SHARED_PR_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_shared_pr_meta, 0);

	luaL_newmetatable(L, SHARED_PR128_METATABLE);
	luaL_setfuncs(L, l_ck_shared_pr_md128_meta, 0);

#define MD128_VIEW_META(NAME, PUSH, TO, FT, ...) \
	luaL_newmetatable(L, SHARED_PR128_VIEW_METATABLE(NAME)); \
	luaL_setfuncs(L, l_ck_shared_pr_md128_##NAME##_meta, 0); \
	\
	luaL_newmetatable(L, SHARED_PR128_VIEW_I_METATABLE(NAME)); \
	luaL_setfuncs(L, l_ck_shared_pr_md128_##NAME##_i_meta, 0);
	SERDE_PR128_TYPES_LIST(MD128_VIEW_META)
#undef MD128_VIEW_META

	lua_newtable(L); /* ck.shared */
	luaL_newlib(L, l_ck_shared_const_funcs);
	lua_setfield(L, -2, "const");
	luaL_newlib(L, l_ck_shared_mut_funcs);
	lua_setfield(L, -2, "mut");
	luaL_newlib(L, l_ck_shared_pr_funcs);
	luaL_newlib(L, l_ck_shared_pr_md128_funcs);
	lua_setfield(L, -2, "md128");
	lua_setfield(L, -2, "pr");

	return (1);
}
