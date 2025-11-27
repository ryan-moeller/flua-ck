/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/types.h>
#include <sys/umtx.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include <ck_ec.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "refcount.h"

#define CK_EC32_METATABLE "ck_ec32_t"
#ifdef CK_F_EC64
#define CK_EC64_METATABLE "ck_ec64_t"
#endif

static int gettime(const struct ck_ec_ops *, struct timespec *);
static void wait32(const struct ck_ec_wait_state *, const uint32_t *, uint32_t,
    const struct timespec *);
static void wait64(const struct ck_ec_wait_state *, const uint64_t *, uint64_t,
    const struct timespec *);
static void wake32(const struct ck_ec_ops *, const uint32_t *);
static void wake64(const struct ck_ec_ops *, const uint64_t *);

static const struct ck_ec_ops system_ec_ops = {
	.gettime = gettime,
	.wait32 = wait32,
	.wait64 = wait64,
	.wake32 = wake32,
	.wake64 = wake64,
	/* TODO: tuning? */
};

static int
gettime(const struct ck_ec_ops *ops __unused, struct timespec *out)
{
	assert(ops == &system_ec_ops);
	return (clock_gettime(CLOCK_MONOTONIC, out));
}

static void
wait32(const struct ck_ec_wait_state *state __unused, const uint32_t *address,
    uint32_t expected, const struct timespec *deadline)
{
	assert(state->ops == &system_ec_ops);
	_umtx_op(__DECONST(uint32_t *, address), UMTX_OP_WAIT_UINT, expected,
	    (void *)(uintptr_t)sizeof(*deadline),
	    __DECONST(struct timespec *, deadline));
}

static void
wait64(const struct ck_ec_wait_state *state __unused, const uint64_t *address,
    uint64_t expected, const struct timespec *deadline)
{
	assert(state->ops == &system_ec_ops);
	_umtx_op(__DECONST(uint64_t *, address), UMTX_OP_WAIT, expected,
	    (void *)(uintptr_t)sizeof(*deadline),
	    __DECONST(struct timespec *, deadline));
}

static void
wake32(const struct ck_ec_ops *ops __unused, const uint32_t *address)
{
	assert(ops == &system_ec_ops);
	_umtx_op(__DECONST(uint32_t *, address), UMTX_OP_WAKE, INT_MAX, NULL,
	    NULL);
}

static void
wake64(const struct ck_ec_ops *ops __unused, const uint64_t *address)
{
	assert(ops == &system_ec_ops);
	_umtx_op(__DECONST(uint64_t *, address), UMTX_OP_WAKE, INT_MAX, NULL,
	    NULL);
}

static const struct ck_ec_mode ec_mp = {
	.ops = &system_ec_ops,
	.single_producer = false,
};

#ifdef CK_F_EC_SP
static const struct ck_ec_mode ec_sp = {
	.ops = &system_ec_ops,
	.single_producer = true,
};
#endif

static int
l_ck_ec_deadline(lua_State *L)
{
	struct timespec new_deadline, timeout, *timeoutp;
	struct ck_ec_mode *mode;

	mode = checklightuserdata(L, 1);
	if (lua_isinteger(L, 2)) {
		timeoutp = &timeout;
		timeout.tv_sec = lua_tointeger(L, 2);
		timeout.tv_nsec = luaL_optinteger(L, 3, 0);
	} else {
		timeoutp = NULL;
	}

	if (ck_ec_deadline(&new_deadline, mode, timeoutp) == -1) {
		return (fail(L, errno));
	}
	lua_pushinteger(L, new_deadline.tv_sec);
	lua_pushinteger(L, new_deadline.tv_nsec);
	return (2);
}

struct rcec32 {
	ck_ec32_t ec;
	refcount refs;
};

static int
l_ck_ec32_new(lua_State *L)
{
	struct rcec32 *ecp;
	uint32_t value;

	value = luaL_checkinteger(L, 1);

	if ((ecp = malloc(sizeof(*ecp))) == NULL) {
		return (fatal(L, "malloc", ENOMEM));
	}
	ck_ec32_init(&ecp->ec, value);
	refcount_init(&ecp->refs);
	return (new(L, ecp, CK_EC32_METATABLE));
}

static int
l_ck_ec32_retain(lua_State *L)
{
	struct rcec32 *ecp;

	ecp = checklightuserdata(L, 1);

	refcount_retain(&ecp->refs);
	return (new(L, ecp, CK_EC32_METATABLE));
}

static int
l_ck_ec32_gc(lua_State *L)
{
	struct rcec32 *ecp;

	ecp = checkcookie(L, 1, CK_EC32_METATABLE);

	if (refcount_release(&ecp->refs)) {
		free(ecp);
	}
	return (0);
}

static int
l_ck_ec32_cookie(lua_State *L)
{
	checkcookie(L, 1, CK_EC32_METATABLE);

	return (1);
}

static int
l_ck_ec32_value(lua_State *L)
{
	struct rcec32 *ecp;

	ecp = checkcookie(L, 1, CK_EC32_METATABLE);

	lua_pushinteger(L, ck_ec32_value(&ecp->ec));
	return (1);
}

static int
l_ck_ec32_has_waiters(lua_State *L)
{
	struct rcec32 *ecp;

	ecp = checkcookie(L, 1, CK_EC32_METATABLE);

	lua_pushboolean(L, ck_ec32_has_waiters(&ecp->ec));
	return (1);
}

static int
l_ck_ec32_inc(lua_State *L)
{
	struct rcec32 *ecp;
	struct ck_ec_mode *mode;

	ecp = checkcookie(L, 1, CK_EC32_METATABLE);
	mode = checklightuserdata(L, 2);

	ck_ec32_inc(&ecp->ec, mode);
	return (0);
}

static int
l_ck_ec32_add(lua_State *L)
{
	struct rcec32 *ecp;
	struct ck_ec_mode *mode;
	uint32_t delta;

	ecp = checkcookie(L, 1, CK_EC32_METATABLE);
	mode = checklightuserdata(L, 2);
	delta = luaL_checkinteger(L, 3);

	lua_pushinteger(L, ck_ec32_add(&ecp->ec, mode, delta));
	return (1);
}

static int
l_ck_ec32_wait(lua_State *L)
{
	struct timespec deadline, *deadlinep;
	struct rcec32 *ecp;
	struct ck_ec_mode *mode;
	uint32_t value;
	int error;

	ecp = checkcookie(L, 1, CK_EC32_METATABLE);
	mode = checklightuserdata(L, 2);
	value = luaL_checkinteger(L, 3);
	if (lua_isinteger(L, 4)) {
		deadlinep = &deadline;
		deadline.tv_sec = lua_tointeger(L, 4);
		deadline.tv_nsec = luaL_optinteger(L, 5, 0);
	} else {
		deadlinep = NULL;
	}

	error = ck_ec32_wait(&ecp->ec, mode, value, deadlinep);
	lua_pushboolean(L, error == 0);
	return (1);
}

static int
l_ck_ec32_wait_pred(lua_State *L)
{
	return (luaL_error(L, "TODO"));
}

#ifdef CK_F_EC64
struct rcec64 {
	ck_ec64_t ec;
	refcount refs;
};

static int
l_ck_ec64_new(lua_State *L)
{
	struct rcec64 *ecp;
	uint64_t value;

	value = luaL_checkinteger(L, 1);

	if ((ecp = malloc(sizeof(*ecp))) == NULL) {
		return (fatal(L, "malloc", ENOMEM));
	}
	ck_ec64_init(&ecp->ec, value);
	refcount_init(&ecp->refs);
	return (new(L, ecp, CK_EC64_METATABLE));
}

static int
l_ck_ec64_retain(lua_State *L)
{
	struct rcec64 *ecp;

	ecp = checklightuserdata(L, 1);

	refcount_retain(&ecp->refs);
	return (new(L, ecp, CK_EC64_METATABLE));
}

static int
l_ck_ec64_gc(lua_State *L)
{
	struct rcec64 *ecp;

	ecp = checkcookie(L, 1, CK_EC64_METATABLE);

	if (refcount_release(&ecp->refs)) {
		free(ecp);
	}
	return (0);
}

static int
l_ck_ec64_cookie(lua_State *L)
{
	checkcookie(L, 1, CK_EC64_METATABLE);

	return (1);
}

static int
l_ck_ec64_value(lua_State *L)
{
	struct rcec64 *ecp;

	ecp = checkcookie(L, 1, CK_EC64_METATABLE);

	lua_pushinteger(L, ck_ec64_value(&ecp->ec));
	return (1);
}

static int
l_ck_ec64_has_waiters(lua_State *L)
{
	struct rcec64 *ecp;

	ecp = checkcookie(L, 1, CK_EC64_METATABLE);

	lua_pushboolean(L, ck_ec64_has_waiters(&ecp->ec));
	return (1);
}

static int
l_ck_ec64_inc(lua_State *L)
{
	struct rcec64 *ecp;
	struct ck_ec_mode *mode;

	ecp = checkcookie(L, 1, CK_EC64_METATABLE);
	mode = checklightuserdata(L, 2);

	ck_ec64_inc(&ecp->ec, mode);
	return (0);
}

static int
l_ck_ec64_add(lua_State *L)
{
	struct rcec64 *ecp;
	struct ck_ec_mode *mode;
	uint64_t delta;

	ecp = checkcookie(L, 1, CK_EC64_METATABLE);
	mode = checklightuserdata(L, 2);
	delta = luaL_checkinteger(L, 3);

	lua_pushinteger(L, ck_ec64_add(&ecp->ec, mode, delta));
	return (1);
}

static int
l_ck_ec64_wait(lua_State *L)
{
	struct timespec deadline, *deadlinep;
	struct rcec64 *ecp;
	struct ck_ec_mode *mode;
	uint64_t value;
	int error;

	ecp = checkcookie(L, 1, CK_EC64_METATABLE);
	mode = checklightuserdata(L, 2);
	value = luaL_checkinteger(L, 3);
	if (lua_isinteger(L, 4)) {
		deadlinep = &deadline;
		deadline.tv_sec = lua_tointeger(L, 4);
		deadline.tv_nsec = luaL_optinteger(L, 5, 0);
	} else {
		deadlinep = NULL;
	}

	error = ck_ec64_wait(&ecp->ec, mode, value, deadlinep);
	lua_pushboolean(L, error == 0);
	return (1);
}

static int
l_ck_ec64_wait_pred(lua_State *L)
{
	return (luaL_error(L, "TODO"));
}
#endif

static const struct luaL_Reg l_ck_ec_funcs[] = {
	{"deadline", l_ck_ec_deadline},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ec32_funcs[] = {
	{"new", l_ck_ec32_new},
	{"retain", l_ck_ec32_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ec32_meta[] = {
	{"__gc", l_ck_ec32_gc},
	{"cookie", l_ck_ec32_cookie},
	{"value", l_ck_ec32_value},
	{"has_waiters", l_ck_ec32_has_waiters},
	{"inc", l_ck_ec32_inc},
	{"add", l_ck_ec32_add},
	{"wait", l_ck_ec32_wait},
	{"wait_pred", l_ck_ec32_wait_pred},
	{NULL, NULL}
};

#ifdef CK_F_EC64
static const struct luaL_Reg l_ck_ec64_funcs[] = {
	{"new", l_ck_ec64_new},
	{"retain", l_ck_ec64_retain},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_ec64_meta[] = {
	{"__gc", l_ck_ec64_gc},
	{"cookie", l_ck_ec64_cookie},
	{"value", l_ck_ec64_value},
	{"has_waiters", l_ck_ec64_has_waiters},
	{"inc", l_ck_ec64_inc},
	{"add", l_ck_ec64_add},
	{"wait", l_ck_ec64_wait},
	{"wait_pred", l_ck_ec64_wait_pred},
	{NULL, NULL}
};
#endif

int
luaopen_ck_ec(lua_State *L)
{
	luaL_newmetatable(L, CK_EC32_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_ec32_meta, 0);

#ifdef CK_F_EC64
	luaL_newmetatable(L, CK_EC64_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_ec64_meta, 0);
#endif

	luaL_newlib(L, l_ck_ec_funcs); /* ck.ec */
	lua_pushlightuserdata(L, __DECONST(struct ck_ec_mode *, &ec_mp));
	lua_setfield(L, -2, "mp");
#ifdef CK_F_EC_SP
	lua_pushlightuserdata(L, __DECONST(struct ck_ec_mode *, &ec_sp));
	lua_setfield(L, -2, "sp");
#endif
	luaL_newlib(L, l_ck_ec32_funcs); /* ck.ec.ec32 */
	lua_setfield(L, -2, "ec32");
#ifdef CK_F_EC64
	luaL_newlib(L, l_ck_ec64_funcs); /* ck.ec.ec64 */
	lua_setfield(L, -2, "ec64");
#endif
	return (1);
}
