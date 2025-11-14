/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ck_pr.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"

static int
l_ck_pr_barrier(lua_State *L __unused)
{
	ck_pr_barrier();
	return (0);
}

static int
l_ck_pr_rfo(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);

	ck_pr_rfo(lua_touserdata(L, 1));
	return (0);
}

static int
l_ck_pr_stall(lua_State *L __unused)
{
	ck_pr_stall();
	return (0);
}

#define FENCE_LIST(X) \
	X(atomic) \
	X(atomic_load) \
	X(atomic_store) \
	X(store_atomic) \
	X(load_atomic) \
	X(load_store) \
	X(store_load) \
	X(load) \
	X(store) \
	X(memory) \
	X(acquire) \
	X(release) \
	X(acqrel) \
	X(lock) \
	X(unlock)

#define FENCE_IMPL(T) \
static int \
l_ck_pr_fence_##T(lua_State *L) \
{ \
	ck_pr_fence_##T(); \
	return (0); \
} \
static int \
l_ck_pr_fence_strict_##T(lua_State *L) \
{ \
	ck_pr_fence_strict_##T(); \
	return (0); \
}
FENCE_LIST(FENCE_IMPL)
#undef FENCE_IMPL

#ifdef CK_F_PR_RTM
static int
l_ck_pr_rtm_begin(lua_State *L)
{
	lua_pushinteger(L, ck_pr_rtm_begin());
	return (1);
}

static int
l_ck_pr_rtm_code(lua_State *L)
{
	unsigned int status;

	status = luaL_checkinteger(L, 1);

	lua_pushinteger(L, CK_PR_RTM_CODE(status));
	return (1);
}

static int
l_ck_pr_rtm_end(lua_State *L __unused)
{
	ck_pr_rtm_end();
	return (0);
}

static int
l_ck_pr_rtm_abort(lua_State *L)
{
	ck_pr_rtm_end(lua_checkinteger(L, 1));
	return (0);
}

static int
l_ck_pr_rtm_test(lua_State *L)
{
	lua_pushboolean(L, ck_pr_rtm_test());
	return (1);
}
#endif

static const struct luaL_Reg l_ck_pr_funcs[] = {
	{"barrier", l_ck_pr_barrier},
	{"rfo", l_ck_pr_rfo},
	{"stall", l_ck_pr_stall},
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_pr_fence_funcs[] = {
#define FENCE_REG(T) \
	{#T, l_ck_pr_fence_##T},
	FENCE_LIST(FENCE_REG)
#undef FENCE_REG
	{NULL, NULL}
};

static const struct luaL_Reg l_ck_pr_fence_strict_funcs[] = {
#define FENCE_REG(T) \
	{#T, l_ck_pr_fence_strict_##T},
	FENCE_LIST(FENCE_REG)
#undef FENCE_REG
	{NULL, NULL}
};

#ifdef CK_F_PR_RTM
static const struct luaL_Reg l_ck_pr_rtm_funcs[] = {
	{"begin", l_ck_pr_rtm_begin},
	{"code", l_ck_pr_rtm_code},
	{"end", l_ck_pr_rtm_end},
	{"abort", l_ck_pr_rtm_abort},
	{"test", l_ck_pr_rtm_test},
	{NULL, NULL}
};
#endif

int
luaopen_ck_pr(lua_State *L)
{
	luaL_newlib(L, l_ck_pr_funcs); /* ck.pr */
	luaL_newlib(L, l_ck_pr_fence_funcs); /* ck.pr.fence */
	luaL_newlib(L, l_ck_pr_fence_strict_funcs); /* ck.pr.fence.strict */
	lua_setfield(L, -2, "strict");
	lua_setfield(L, -2, "fence");
#ifdef CK_F_PR_RTM
	luaL_newlib(L, l_ck_pr_rtm_funcs); /* ck.pr.rtm */
#define SETCONST(id) \
	lua_pushinteger(L, CK_PR_RTM_##id); \
	lua_setfield(L, -2, #id)
	SETCONST(EXPLICIT);
	SETCONST(RETRY);
	SETCONST(CONFLICT);
	SETCONST(CAPACITY);
	SETCONST(DEBUG);
	SETCONST(NESTED);
#undef SETCONST
	lua_setfield(L, -2, "rtm");
#endif
	return (1);
}
