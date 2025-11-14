/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <ck_ring.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "refcount.h"
#include "serde.h"
#include "serdebuf.h"
#include "luaerror.h"

#define RING_SPSC_METATABLE "ring.spsc"
#define RING_MPMC_METATABLE "ring.mpmc"
#define RING_SPMC_METATABLE "ring.spmc"
#define RING_MPSC_METATABLE "ring.mpsc"

struct rcring {
	ck_ring_t ring;
	ck_ring_buffer_t *buffer;
	refcount refs;
};

static inline int
newring(lua_State *L, const char *metatable)
{
	struct rcring *ringp;
	unsigned int size;
	int error;

	size = luaL_checkinteger(L, 1);

	if ((ringp = malloc(sizeof(*ringp))) == NULL) {
		return (luaL_error(L, "malloc: %s", strerror(ENOMEM)));
	}
	ck_ring_init(&ringp->ring, size);
	if ((ringp->buffer = malloc(sizeof(ck_ring_buffer_t) * size)) == NULL) {
		free(ringp);
		return (luaL_error(L, "malloc: %s", strerror(ENOMEM)));
	}
	refcount_init(&ringp->refs);
	return (new(L, ringp, metatable));
}

static inline int
retainring(lua_State *L, const char *metatable)
{
	struct rcring *ringp;

	ringp = checklightuserdata(L, 1);

	refcount_retain(&ringp->refs);
	return (new(L, ringp, metatable));
}

static inline int
releasering(lua_State *L, const char *metatable)
{
	struct rcring *ringp;

	ringp = checkcookie(L, 1, metatable);

	if (refcount_release(&ringp->refs)) {
		free(ringp->buffer);
		free(ringp);
	}
	invalidate(L, 1);
	return (0);
}

static inline int
ringsize(lua_State *L, const char *metatable)
{
	struct rcring *ringp;

	ringp = checkcookie(L, 1, metatable);

	lua_pushinteger(L, ck_ring_size(&ringp->ring));
	return (1);
}

static inline int
ringcapacity(lua_State *L, const char *metatable)
{
	struct rcring *ringp;

	ringp = checkcookie(L, 1, metatable);

	lua_pushinteger(L, ck_ring_capacity(&ringp->ring));
	return (1);
}

static int
l_ck_ring_spsc_new(lua_State *L)
{
	return (newring(L, RING_SPSC_METATABLE));
}

static int
l_ck_ring_spsc_retain(lua_State *L)
{
	return (retainring(L, RING_SPSC_METATABLE));
}

static int
l_ck_ring_spsc_gc(lua_State *L)
{
	return (releasering(L, RING_SPSC_METATABLE));
}

static int
l_ck_ring_spsc_cookie(lua_State *L)
{
	checkcookie(L, 1, RING_SPSC_METATABLE);

	return (1);
}

static int
l_ck_ring_spsc_size(lua_State *L)
{
	return (ringsize(L, RING_SPSC_METATABLE));
}

static int
l_ck_ring_spsc_capacity(lua_State *L)
{
	return (ringcapacity(L, RING_SPSC_METATABLE));
}

static int
l_ck_ring_spsc_enqueue(lua_State *L)
{
	struct serdebuf sb;
	struct rcring *ringp;
	void *v;
	unsigned int size;
	serde_type_code type;
	bool enqueued;
	int error;

	ringp = checkcookie(L, 1, RING_SPSC_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serdebuf_init(L, 2, &sb)) != 0) {
		return (luaL_error(L, "serdebuf_init: %s", strerror(error)));
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, 2, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		if (error < 0) {
			return (lua_error(L));
		}
		return (luaL_error(L, "serdebuf_serialize: %s",
		    strerror(error)));
	}
	if ((v = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		return (luaL_error(L, "serdebuf_finalize: %s",
		    strerror(ENOMEM)));
	}
	if (!(enqueued = ck_ring_enqueue_spsc_size(&ringp->ring, ringp->buffer,
	    v, &size))) {
		free(v);
	}
	lua_pushboolean(L, enqueued);
	lua_pushinteger(L, size);
	return (2);
}

static int
l_ck_ring_spsc_dequeue(lua_State *L)
{
	struct rcring *ringp;
	void *v;
	bool ok;

	ringp = checkcookie(L, 1, RING_SPSC_METATABLE);

	if (!ck_ring_dequeue_spsc(&ringp->ring, ringp->buffer, &v)) {
		lua_pushboolean(L, false);
		return (1);
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : luaL_error(L, "serde error"));
}

static int
l_ck_ring_mpmc_new(lua_State *L)
{
	return (newring(L, RING_MPMC_METATABLE));
}

static int
l_ck_ring_mpmc_retain(lua_State *L)
{
	return (retainring(L, RING_MPMC_METATABLE));
}

static int
l_ck_ring_mpmc_gc(lua_State *L)
{
	return (releasering(L, RING_MPMC_METATABLE));
}

static int
l_ck_ring_mpmc_cookie(lua_State *L)
{
	checkcookie(L, 1, RING_MPMC_METATABLE);

	return (1);
}

static int
l_ck_ring_mpmc_size(lua_State *L)
{
	return (ringsize(L, RING_MPMC_METATABLE));
}

static int
l_ck_ring_mpmc_capacity(lua_State *L)
{
	return (ringcapacity(L, RING_MPMC_METATABLE));
}

static int
l_ck_ring_mpmc_enqueue(lua_State *L)
{
	struct serdebuf sb;
	struct rcring *ringp;
	void *v;
	unsigned int size;
	serde_type_code type;
	bool enqueued;
	int error;

	ringp = checkcookie(L, 1, RING_MPMC_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serdebuf_init(L, 2, &sb)) != 0) {
		return (luaL_error(L, "serdebuf_init: %s", strerror(error)));
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, 2, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		if (error < 0) {
			return (lua_error(L));
		}
		return (luaL_error(L, "serdebuf_serialize: %s",
		    strerror(error)));
	}
	if ((v = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		return (luaL_error(L, "serdebuf_finalize: %s",
		    strerror(ENOMEM)));
	}
	if (!(enqueued = ck_ring_enqueue_mpmc_size(&ringp->ring, ringp->buffer,
	    v, &size))) {
		free(v);
	}
	lua_pushboolean(L, enqueued);
	lua_pushinteger(L, size);
	return (2);
}

static int
l_ck_ring_mpmc_trydequeue(lua_State *L)
{
	struct rcring *ringp;
	void *v;
	bool ok;

	ringp = checkcookie(L, 1, RING_MPMC_METATABLE);

	if (!ck_ring_trydequeue_mpmc(&ringp->ring, ringp->buffer, &v)) {
		lua_pushboolean(L, false);
		return (1);
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : luaL_error(L, "serde error"));
}

static int
l_ck_ring_mpmc_dequeue(lua_State *L)
{
	struct rcring *ringp;
	void *v;
	bool ok;

	ringp = checkcookie(L, 1, RING_MPMC_METATABLE);

	if (!ck_ring_dequeue_mpmc(&ringp->ring, ringp->buffer, &v)) {
		lua_pushboolean(L, false);
		return (1);
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : luaL_error(L, "serde error"));
}

static int
l_ck_ring_spmc_new(lua_State *L)
{
	return (newring(L, RING_SPMC_METATABLE));
}

static int
l_ck_ring_spmc_retain(lua_State *L)
{
	return (retainring(L, RING_SPMC_METATABLE));
}

static int
l_ck_ring_spmc_gc(lua_State *L)
{
	return (releasering(L, RING_SPMC_METATABLE));
}

static int
l_ck_ring_spmc_cookie(lua_State *L)
{
	checkcookie(L, 1, RING_SPMC_METATABLE);

	return (1);
}

static int
l_ck_ring_spmc_size(lua_State *L)
{
	return (ringsize(L, RING_SPMC_METATABLE));
}

static int
l_ck_ring_spmc_capacity(lua_State *L)
{
	return (ringcapacity(L, RING_SPMC_METATABLE));
}

static int
l_ck_ring_spmc_enqueue(lua_State *L)
{
	struct serdebuf sb;
	struct rcring *ringp;
	void *v;
	unsigned int size;
	serde_type_code type;
	bool enqueued;
	int error;

	ringp = checkcookie(L, 1, RING_SPMC_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serdebuf_init(L, 2, &sb)) != 0) {
		return (luaL_error(L, "serdebuf_init: %s", strerror(error)));
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, 2, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		if (error < 0) {
			return (lua_error(L));
		}
		return (luaL_error(L, "serdebuf_serialize: %s",
		    strerror(error)));
	}
	if ((v = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		return (luaL_error(L, "serdebuf_finalize: %s",
		    strerror(ENOMEM)));
	}
	if (!(enqueued = ck_ring_enqueue_spmc_size(&ringp->ring, ringp->buffer,
	    v, &size))) {
		free(v);
	}
	lua_pushboolean(L, enqueued);
	lua_pushinteger(L, size);
	return (2);
}

static int
l_ck_ring_spmc_trydequeue(lua_State *L)
{
	struct rcring *ringp;
	void *v;
	bool ok;

	ringp = checkcookie(L, 1, RING_SPMC_METATABLE);

	if (!ck_ring_trydequeue_spmc(&ringp->ring, ringp->buffer, &v)) {
		lua_pushboolean(L, false);
		return (1);
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : luaL_error(L, "serde error"));
}

static int
l_ck_ring_spmc_dequeue(lua_State *L)
{
	struct rcring *ringp;
	void *v;
	bool ok;

	ringp = checkcookie(L, 1, RING_SPMC_METATABLE);

	if (!ck_ring_dequeue_spmc(&ringp->ring, ringp->buffer, &v)) {
		lua_pushboolean(L, false);
		return (1);
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : luaL_error(L, "serde error"));
}

static int
l_ck_ring_mpsc_new(lua_State *L)
{
	return (newring(L, RING_MPSC_METATABLE));
}

static int
l_ck_ring_mpsc_retain(lua_State *L)
{
	return (retainring(L, RING_MPSC_METATABLE));
}

static int
l_ck_ring_mpsc_gc(lua_State *L)
{
	return (releasering(L, RING_MPSC_METATABLE));
}

static int
l_ck_ring_mpsc_cookie(lua_State *L)
{
	checkcookie(L, 1, RING_MPSC_METATABLE);

	return (1);
}

static int
l_ck_ring_mpsc_size(lua_State *L)
{
	return (ringsize(L, RING_MPSC_METATABLE));
}

static int
l_ck_ring_mpsc_capacity(lua_State *L)
{
	return (ringcapacity(L, RING_MPSC_METATABLE));
}

static int
l_ck_ring_mpsc_enqueue(lua_State *L)
{
	struct serdebuf sb;
	struct rcring *ringp;
	void *v;
	unsigned int size;
	serde_type_code type;
	bool enqueued;
	int error;

	ringp = checkcookie(L, 1, RING_MPSC_METATABLE);
	luaL_checkany(L, 2);

	if ((error = serdebuf_init(L, 2, &sb)) != 0) {
		return (luaL_error(L, "serdebuf_init: %s", strerror(error)));
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, 2, &sb, &type)) != 0) {
		serdebuf_destroy(&sb);
		if (error < 0) {
			return (lua_error(L));
		}
		return (luaL_error(L, "serdebuf_serialize: %s",
		    strerror(error)));
	}
	if ((v = serdebuf_finalize(&sb, NULL)) == NULL) {
		serdebuf_destroy(&sb);
		return (luaL_error(L, "serdebuf_finalize: %s",
		    strerror(ENOMEM)));
	}
	if (!(enqueued = ck_ring_enqueue_mpsc_size(&ringp->ring, ringp->buffer,
	    v, &size))) {
		free(v);
	}
	lua_pushboolean(L, enqueued);
	lua_pushinteger(L, size);
	return (2);
}

static int
l_ck_ring_mpsc_dequeue(lua_State *L)
{
	struct rcring *ringp;
	void *v;
	bool ok;

	ringp = checkcookie(L, 1, RING_MPSC_METATABLE);

	if (!ck_ring_dequeue_mpsc(&ringp->ring, ringp->buffer, &v)) {
		lua_pushboolean(L, false);
		return (1);
	}
	lua_pushboolean(L, true);
	ok = loadshared(L, v) != NULL;
	free(v);
	return (ok ? 2 : luaL_error(L, "serde error"));
}

static const struct luaL_Reg l_ck_ring_spsc_funcs[] = {
	{"new", l_ck_ring_spsc_new},
	{"retain", l_ck_ring_spsc_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ring_spsc_meta[] = {
	{"__gc", l_ck_ring_spsc_gc},
	{"cookie", l_ck_ring_spsc_cookie},
	{"size", l_ck_ring_spsc_size},
	{"capacity", l_ck_ring_spsc_capacity},
#if 0 /* maybe if we could serde the ring buffer? */
	{"repair", l_ck_ring_spsc_repair},
	{"valid", l_ck_ring_spsc_valid},
#endif
	{"enqueue", l_ck_ring_spsc_enqueue},
	{"dequeue", l_ck_ring_spsc_dequeue},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ring_mpmc_funcs[] = {
	{"new", l_ck_ring_mpmc_new},
	{"retain", l_ck_ring_mpmc_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ring_mpmc_meta[] = {
	{"__gc", l_ck_ring_mpmc_gc},
	{"cookie", l_ck_ring_mpmc_cookie},
	{"size", l_ck_ring_mpmc_size},
	{"capacity", l_ck_ring_mpmc_capacity},
#if 0 /* maybe if we could serde the ring buffer? */
	{"repair", l_ck_ring_mpmc_repair},
	{"valid", l_ck_ring_mpmc_valid},
#endif
	{"enqueue", l_ck_ring_mpmc_enqueue},
	{"trydequeue", l_ck_ring_mpmc_trydequeue},
	{"dequeue", l_ck_ring_mpmc_dequeue},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ring_spmc_funcs[] = {
	{"new", l_ck_ring_spmc_new},
	{"retain", l_ck_ring_spmc_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ring_spmc_meta[] = {
	{"__gc", l_ck_ring_spmc_gc},
	{"cookie", l_ck_ring_spmc_cookie},
	{"size", l_ck_ring_spmc_size},
	{"capacity", l_ck_ring_spmc_capacity},
#if 0 /* maybe if we could serde the ring buffer? */
	{"repair", l_ck_ring_spmc_repair},
	{"valid", l_ck_ring_spmc_valid},
#endif
	{"enqueue", l_ck_ring_spmc_enqueue},
	{"trydequeue", l_ck_ring_spmc_trydequeue},
	{"dequeue", l_ck_ring_spmc_dequeue},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ring_mpsc_funcs[] = {
	{"new", l_ck_ring_mpsc_new},
	{"retain", l_ck_ring_mpsc_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ring_mpsc_meta[] = {
	{"__gc", l_ck_ring_mpsc_gc},
	{"cookie", l_ck_ring_mpsc_cookie},
	{"size", l_ck_ring_mpsc_size},
	{"capacity", l_ck_ring_mpsc_capacity},
#if 0 /* maybe if we could serde the ring buffer? */
	{"repair", l_ck_ring_mpsc_repair},
	{"valid", l_ck_ring_mpsc_valid},
#endif
	{"enqueue", l_ck_ring_mpsc_enqueue},
	{"dequeue", l_ck_ring_mpsc_dequeue},
	{NULL, NULL}
};

int
luaopen_ck_ring(lua_State *L)
{
	luaL_newmetatable(L, RING_SPSC_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_ring_spsc_meta, 0);

	luaL_newmetatable(L, RING_MPMC_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_ring_mpmc_meta, 0);

	luaL_newmetatable(L, RING_SPMC_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_ring_spmc_meta, 0);

	luaL_newmetatable(L, RING_MPSC_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_ring_mpsc_meta, 0);

	lua_newtable(L); /* ck.ring */
	luaL_newlib(L, l_ck_ring_spsc_funcs);
	lua_setfield(L, -2, "spsc");
	luaL_newlib(L, l_ck_ring_mpmc_funcs);
	lua_setfield(L, -2, "mpmp");
	luaL_newlib(L, l_ck_ring_spmc_funcs);
	lua_setfield(L, -2, "spmc");
	luaL_newlib(L, l_ck_ring_mpsc_funcs);
	lua_setfield(L, -2, "mpsc");

	return (1);
}
