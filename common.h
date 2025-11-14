/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Debugging tools */
#define STR(x) #x
#define XSTR(x) STR(x)
#define MESSAGE(...) _Pragma(XSTR(message(#__VA_ARGS__)))

/* constructor/destructor priorities */
enum {
	PRIO_HP,
	PRIO_HT,
};

enum wrapperuv {
	COOKIE = 1,
};

static inline int
new(lua_State *L, void *cookie, const char *metatable)
{
	lua_newuserdatauv(L, 0, 1);
	luaL_setmetatable(L, metatable);

	lua_pushlightuserdata(L, cookie);
	lua_setiuservalue(L, -2, COOKIE);

	return (1);
}

static inline void *
checklightuserdata(lua_State *L, int idx)
{
	luaL_checktype(L, idx, LUA_TLIGHTUSERDATA);

	return (lua_touserdata(L, idx));
}

static inline void *
checkcookie(lua_State *L, int idx, const char *metatable)
{
	void *cookie;

	luaL_checkudata(L, idx, metatable);

	lua_getiuservalue(L, idx, COOKIE);
	cookie = lua_touserdata(L, -1);
	luaL_argcheck(L, cookie != NULL, idx, "cookie expired");
	return (cookie);
}

static inline void
invalidate(lua_State *L, int idx)
{
	lua_pushlightuserdata(L, NULL);
	lua_setiuservalue(L, idx, COOKIE);
}

static inline int
closestream(lua_State *L)
{
	luaL_Stream *stream;
	int res;

	stream = luaL_checkudata(L, 1, LUA_FILEHANDLE);
	res = fclose(stream->f);
	return (luaL_fileresult(L, res == 0, NULL));
}

static inline void
newstream(lua_State *L, FILE *f)
{
	luaL_Stream *stream;

	stream = lua_newuserdatauv(L, sizeof(*stream), 0);
	luaL_setmetatable(L, LUA_FILEHANDLE);
	stream->f = f;
	stream->closef = closestream;
}

int luaopen_ck_shared(lua_State *L);
int luaopen_ck_serde(lua_State *L);
int luaopen_ck_sequence(lua_State *L);
int luaopen_ck_fifo(lua_State *L);
int luaopen_ck_ring(lua_State *L);
int luaopen_ck_pr(lua_State *L);
