/*
 * Copyright (c) 2025-2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <stdlib.h>

#include <ck_fifo.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "refcount.h"
#include "serde.h"
#include "serdebuf.h"
#include "luaerror.h"

#define FIFO_SPSC_METATABLE "fifo.spsc"
#define FIFO_MPMC_METATABLE "fifo.mpmc"

struct rcfifo_spsc {
	ck_fifo_spsc_t fifo;
	refcount refs;
};

static int
l_ck_fifo_spsc_new(lua_State *L)
{
	struct rcfifo_spsc *fifop;
	ck_fifo_spsc_entry_t *stubp;

	if ((fifop = malloc(sizeof(*fifop))) == NULL) {
		return (fatal(L, "malloc", ENOMEM));
	}
	if ((stubp = malloc(sizeof(*stubp))) == NULL) {
		free(fifop);
		return (fatal(L, "malloc", ENOMEM));
	}
	ck_fifo_spsc_init(&fifop->fifo, stubp);
	refcount_init(&fifop->refs);
	return (new(L, fifop, FIFO_SPSC_METATABLE));
}

static int
l_ck_fifo_spsc_retain(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checklightuserdata(L, 1);

	refcount_retain(&fifop->refs);
	return (new(L, fifop, FIFO_SPSC_METATABLE));
}

static int
l_ck_fifo_spsc_gc(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	if (refcount_release(&fifop->refs)) {
		ck_fifo_spsc_entry_t *garbage, *next;

		ck_fifo_spsc_deinit(&fifop->fifo, &garbage);
		while (garbage != NULL) {
			next = CK_FIFO_SPSC_NEXT(garbage);
			free(garbage);
			garbage = next;
		}
		free(fifop);
	}
	return (0);
}

static int
l_ck_fifo_spsc_cookie(lua_State *L)
{
	checkcookieuv(L, 1, FIFO_SPSC_METATABLE);

	return (1);
}

static int
l_ck_fifo_spsc_enqueue(lua_State *L)
{
	struct serdebuf sb;
	struct rcfifo_spsc *fifop;
	ck_fifo_spsc_entry_t *entry;
	void *v;
	serde_type_code type;
	int error;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serdebuf_init(L, 2, &sb)) != 0) {
		return (fatal(L, "serdebuf_init", error));
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, 2, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		if (error < 0) {
			return (lua_error(L));
		}
		return (fatal(L, "serdebuf_serialize", error));
	}
	if ((v = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		return (fatal(L, "serdebuf_finalize", ENOMEM));
	}
	if ((entry = ck_fifo_spsc_recycle(&fifop->fifo)) == NULL &&
	    (entry = malloc(sizeof(*entry))) == NULL) {
		free(v);
		return (fatal(L, "malloc", ENOMEM));
	}
	ck_fifo_spsc_enqueue(&fifop->fifo, entry, v);
	return (0);
}

static int
l_ck_fifo_spsc_dequeue(lua_State *L)
{
	struct rcfifo_spsc *fifop;
	void *v;
	bool ok;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	if (!ck_fifo_spsc_dequeue(&fifop->fifo, &v)) {
		lua_pushboolean(L, false);
		return (1);
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : lua_error(L));
}

static int
l_ck_fifo_spsc_isempty(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	lua_pushboolean(L, ck_fifo_spsc_isempty(&fifop->fifo));
	return (1);
}

static int
l_ck_fifo_spsc_enqueue_trylock(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	lua_pushboolean(L, ck_fifo_spsc_enqueue_trylock(&fifop->fifo));
	return (1);
}

static int
l_ck_fifo_spsc_enqueue_lock(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	ck_fifo_spsc_enqueue_lock(&fifop->fifo);
	return (0);
}

static int
l_ck_fifo_spsc_enqueue_unlock(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	ck_fifo_spsc_enqueue_unlock(&fifop->fifo);
	return (0);
}

static int
l_ck_fifo_spsc_dequeue_trylock(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	lua_pushboolean(L, ck_fifo_spsc_dequeue_trylock(&fifop->fifo));
	return (1);
}

static int
l_ck_fifo_spsc_dequeue_lock(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	ck_fifo_spsc_dequeue_lock(&fifop->fifo);
	return (0);
}

static int
l_ck_fifo_spsc_dequeue_unlock(lua_State *L)
{
	struct rcfifo_spsc *fifop;

	fifop = checkcookie(L, 1, FIFO_SPSC_METATABLE);

	ck_fifo_spsc_dequeue_unlock(&fifop->fifo);
	return (0);
}

struct rcfifo_mpmc {
	ck_fifo_mpmc_t fifo;
	refcount refs;
};

static int
l_ck_fifo_mpmc_new(lua_State *L)
{
	struct rcfifo_mpmc *fifop;
	ck_fifo_mpmc_entry_t *stubp;

	if ((fifop = malloc(sizeof(*fifop))) == NULL) {
		return (fatal(L, "malloc", ENOMEM));
	}
	if ((stubp = malloc(sizeof(*stubp))) == NULL) {
		free(fifop);
		return (fatal(L, "malloc", ENOMEM));
	}
	ck_fifo_mpmc_init(&fifop->fifo, stubp);
	refcount_init(&fifop->refs);
	return (new(L, fifop, FIFO_MPMC_METATABLE));
}

static int
l_ck_fifo_mpmc_retain(lua_State *L)
{
	struct rcfifo_mpmc *fifop;

	fifop = checklightuserdata(L, 1);

	refcount_retain(&fifop->refs);
	return (new(L, fifop, FIFO_MPMC_METATABLE));
}

static int
l_ck_fifo_mpmc_gc(lua_State *L)
{
	struct rcfifo_mpmc *fifop;

	fifop = checkcookie(L, 1, FIFO_MPMC_METATABLE);

	if (refcount_release(&fifop->refs)) {
		ck_fifo_mpmc_entry_t *garbage, *next;

		ck_fifo_mpmc_deinit(&fifop->fifo, &garbage);
		while (garbage != NULL) {
			next = CK_FIFO_MPMC_NEXT(garbage);
			free(garbage);
			garbage = next;
		}
		free(fifop);
	}
	return (0);
}

static int
l_ck_fifo_mpmc_cookie(lua_State *L)
{
	checkcookieuv(L, 1, FIFO_MPMC_METATABLE);

	return (1);
}

static int
l_ck_fifo_mpmc_enqueue(lua_State *L)
{
	struct serdebuf sb;
	struct rcfifo_mpmc *fifop;
	ck_fifo_mpmc_entry_t *entry;
	void *v;
	serde_type_code type;
	int error;

	fifop = checkcookie(L, 1, FIFO_MPMC_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serdebuf_init(L, 2, &sb)) != 0) {
		return (fatal(L, "serdebuf_init", error));
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, 2, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		if (error < 0) {
			return (lua_error(L));
		}
		return (fatal(L, "serdebuf_serialize", error));
	}
	if ((v = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		return (fatal(L, "serdebuf_finalize", ENOMEM));
	}
	if ((entry = malloc(sizeof(*entry))) == NULL) {
		free(v);
		return (fatal(L, "malloc", ENOMEM));
	}
	ck_fifo_mpmc_enqueue(&fifop->fifo, entry, v);
	return (0);
}

static int
l_ck_fifo_mpmc_tryenqueue(lua_State *L)
{
	struct serdebuf sb;
	struct rcfifo_mpmc *fifop;
	ck_fifo_mpmc_entry_t *entry;
	void *v;
	serde_type_code type;
	bool enqueued;
	int error;

	fifop = checkcookie(L, 1, FIFO_MPMC_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serdebuf_init(L, 2, &sb)) != 0) {
		return (fatal(L, "serdebuf_init", error));
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, 2, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		if (error < 0) {
			return (lua_error(L));
		}
		return (fatal(L, "serdebuf_serialize", error));
	}
	if ((v = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		return (fatal(L, "serdebuf_finalize", ENOMEM));
	}
	if ((entry = malloc(sizeof(*entry))) == NULL) {
		free(v);
		return (fatal(L, "malloc", ENOMEM));
	}
	if (!(enqueued = ck_fifo_mpmc_tryenqueue(&fifop->fifo, entry, v))) {
		free(v); /* oof */
	}
	lua_pushboolean(L, enqueued);
	return (1);
}

static int
l_ck_fifo_mpmc_dequeue(lua_State *L)
{
	struct rcfifo_mpmc *fifop;
	ck_fifo_mpmc_entry_t *garbage, *next;
	void *v;
	bool ok;

	fifop = checkcookie(L, 1, FIFO_MPMC_METATABLE);

	if (!ck_fifo_mpmc_dequeue(&fifop->fifo, &v, &garbage)) {
		lua_pushboolean(L, false);
		return (1);
	}
	while (garbage != NULL) {
		next = CK_FIFO_MPMC_NEXT(garbage);
		free(garbage);
		garbage = next;
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : lua_error(L));
}

static int
l_ck_fifo_mpmc_trydequeue(lua_State *L)
{
	struct rcfifo_mpmc *fifop;
	ck_fifo_mpmc_entry_t *garbage, *next;
	void *v;
	bool ok;

	fifop = checkcookie(L, 1, FIFO_MPMC_METATABLE);

	if (!ck_fifo_mpmc_trydequeue(&fifop->fifo, &v, &garbage)) {
		lua_pushboolean(L, false);
		return (1);
	}
	while (garbage != NULL) {
		next = CK_FIFO_MPMC_NEXT(garbage);
		free(garbage);
		garbage = next;
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : lua_error(L));
}

static const struct luaL_Reg l_ck_fifo_spsc_funcs[] = {
	{"new", l_ck_fifo_spsc_new},
	{"retain", l_ck_fifo_spsc_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_fifo_spsc_meta[] = {
	{"__gc", l_ck_fifo_spsc_gc},
	{"cookie", l_ck_fifo_spsc_cookie},
	{"enqueue", l_ck_fifo_spsc_enqueue},
	{"dequeue", l_ck_fifo_spsc_dequeue},
	{"isempty", l_ck_fifo_spsc_isempty},
	/* TODO: iterators? */
	{"enqueue_trylock", l_ck_fifo_spsc_enqueue_trylock},
	{"enqueue_lock", l_ck_fifo_spsc_enqueue_lock},
	{"enqueue_unlock", l_ck_fifo_spsc_enqueue_unlock},
	{"dequeue_trylock", l_ck_fifo_spsc_dequeue_trylock},
	{"dequeue_lock", l_ck_fifo_spsc_dequeue_lock},
	{"dequeue_unlock", l_ck_fifo_spsc_dequeue_unlock},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_fifo_mpmc_funcs[] = {
	{"new", l_ck_fifo_mpmc_new},
	{"retain", l_ck_fifo_mpmc_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_fifo_mpmc_meta[] = {
	{"__gc", l_ck_fifo_mpmc_gc},
	{"cookie", l_ck_fifo_mpmc_cookie},
	{"enqueue", l_ck_fifo_mpmc_enqueue},
	{"tryenqueue", l_ck_fifo_mpmc_tryenqueue},
	{"dequeue", l_ck_fifo_mpmc_dequeue},
	{"trydequeue", l_ck_fifo_mpmc_trydequeue},
	/* TODO: iterators? */
	{NULL, NULL}
};

int
luaopen_ck_fifo(lua_State *L)
{
	luaL_newmetatable(L, FIFO_SPSC_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_fifo_spsc_meta, 0);

	luaL_newmetatable(L, FIFO_MPMC_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_fifo_mpmc_meta, 0);

	lua_newtable(L); /* ck.fifo */
	luaL_newlib(L, l_ck_fifo_spsc_funcs);
	lua_setfield(L, -2, "spsc");
	luaL_newlib(L, l_ck_fifo_mpmc_funcs);
	lua_setfield(L, -2, "mpmc");

	return (1);
}
