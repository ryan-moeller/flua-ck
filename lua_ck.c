/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"

/* TODO: bitmaps, stacks, locks, etc */

int
luaopen_ck(lua_State *L)
{
	luaL_requiref(L, "ck.serde", luaopen_ck_serde, 0);
	lua_newtable(L); /* ck */
	luaL_requiref(L, "ck.shared", luaopen_ck_shared, 0);
	lua_setfield(L, -2, "shared");
	luaL_requiref(L, "ck.sequence", luaopen_ck_sequence, 0);
	lua_setfield(L, -2, "sequence");
	luaL_requiref(L, "ck.ring", luaopen_ck_ring, 0);
	lua_setfield(L, -2, "ring");
	luaL_requiref(L, "ck.fifo", luaopen_ck_fifo, 0);
	lua_setfield(L, -2, "fifo");
	luaL_requiref(L, "ck.pr", luaopen_ck_pr, 0);
	lua_setfield(L, -2, "pr");
	return (1);
}
