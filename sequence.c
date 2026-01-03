/*
 * Copyright (c) 2025-2026 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <ck_sequence.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "refcount.h"

#define SEQUENCE_METATABLE "sequence"

struct rcsequence {
	ck_sequence_t seqlock;
	refcount refs;
};

static int
l_ck_sequence_new(lua_State *L)
{
	struct rcsequence *seqp;

	if ((seqp = malloc(sizeof(*seqp))) == NULL) {
		return (fatal(L, "malloc", ENOMEM));
	}
	ck_sequence_init(&seqp->seqlock);
	refcount_init(&seqp->refs);
	return (new(L, seqp, SEQUENCE_METATABLE));
}

static int
l_ck_sequence_retain(lua_State *L)
{
	struct rcsequence *seqp;

	seqp = checklightuserdata(L, 1);

	refcount_retain(&seqp->refs);
	return (new(L, seqp, SEQUENCE_METATABLE));
}

static int
l_ck_sequence_gc(lua_State *L)
{
	struct rcsequence *seqp;

	seqp = checkcookie(L, 1, SEQUENCE_METATABLE);

	if (refcount_release(&seqp->refs)) {
		free(seqp);
	}
	return (0);
}

static int
l_ck_sequence_cookie(lua_State *L)
{
	checkcookieuv(L, 1, SEQUENCE_METATABLE);

	return (1);
}

static int
l_ck_sequence_read_begin(lua_State *L)
{
	struct rcsequence *seqp;
	unsigned int version;

	seqp = checkcookie(L, 1, SEQUENCE_METATABLE);

	version = ck_sequence_read_begin(&seqp->seqlock);
	lua_pushinteger(L, version);
	return (1);
}

static int
l_ck_sequence_read_retry(lua_State *L)
{
	struct rcsequence *seqp;
	unsigned int version;
	bool retry;

	seqp = checkcookie(L, 1, SEQUENCE_METATABLE);
	version = luaL_checkinteger(L, 2);

	retry = ck_sequence_read_retry(&seqp->seqlock, version);
	lua_pushboolean(L, retry);
	return (1);
}

static int
l_ck_sequence_write_begin(lua_State *L)
{
	struct rcsequence *seqp;

	seqp = checkcookie(L, 1, SEQUENCE_METATABLE);

	ck_sequence_write_begin(&seqp->seqlock);
	return (0);
}

static int
l_ck_sequence_write_end(lua_State *L)
{
	struct rcsequence *seqp;

	seqp = checkcookie(L, 1, SEQUENCE_METATABLE);

	ck_sequence_write_end(&seqp->seqlock);
	return (0);
}

static const struct luaL_Reg l_ck_sequence_funcs[] = {
	{"new", l_ck_sequence_new},
	{"retain", l_ck_sequence_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_sequence_meta[] = {
	{"__gc", l_ck_sequence_gc},
	{"cookie", l_ck_sequence_cookie},
	{"read_begin", l_ck_sequence_read_begin},
	{"read_retry", l_ck_sequence_read_retry},
	{"write_begin", l_ck_sequence_write_begin},
	{"write_end", l_ck_sequence_write_end},
	{NULL, NULL}
};

int
luaopen_ck_sequence(lua_State *L)
{
	luaL_newmetatable(L, SEQUENCE_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_sequence_meta, 0);

	luaL_newlib(L, l_ck_sequence_funcs); /* ck.sequence */
	return (1);
}
