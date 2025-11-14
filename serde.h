/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sys/param.h>
#include <errno.h>
#include <stdint.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

static inline int
getserdemethods(lua_State *L, int idx)
{
	int top = lua_gettop(L);

	if (luaL_getmetafield(L, idx, "serialize") != LUA_TNIL &&
	    luaL_getmetafield(L, idx, "deserialize") != LUA_TNIL) {
		return (0);
	}
	lua_settop(L, top);
	return (EINVAL);
}

enum serde_type {
	SERDE_ENV,
	SERDE_NIL,
	SERDE_BOOLEAN,
	SERDE_LIGHTUSERDATA,
	SERDE_INTEGER,
	SERDE_NUMBER,
	SERDE_STRING,
	SERDE_CCLOSURE,
	SERDE_LCLOSURE,
	SERDE_CUSTOM, /* marker */
	SERDE_INVALID = -1,
	SERDE_ANY = -2
};

static inline enum serde_type
serde_type(lua_State *L, int idx)
{
	switch (lua_type(L, idx)) {
	case LUA_TNIL: return (SERDE_NIL);
	case LUA_TBOOLEAN: return (SERDE_BOOLEAN);
	case LUA_TLIGHTUSERDATA: return (SERDE_LIGHTUSERDATA);
	case LUA_TNUMBER:
		if (lua_isinteger(L, idx)) {
			return (SERDE_INTEGER);
		}
		return (SERDE_NUMBER);
	case LUA_TSTRING: return (SERDE_STRING);
	case LUA_TTABLE:
	case LUA_TUSERDATA:
		if (getserdemethods(L, idx) != 0) {
			return (SERDE_INVALID);
		}
		lua_pop(L, 2);
		return (SERDE_CUSTOM);
	case LUA_TFUNCTION:
		if (lua_iscfunction(L, idx)) {
			return (SERDE_CCLOSURE);
		}
		return (SERDE_LCLOSURE);
	default: return (SERDE_INVALID);
	}
}

typedef int8_t serde_type_code;

int cache_serde(lua_State *L, int idx, serde_type_code *tp);
const void *loadshared(lua_State *L, const void *p);
int luaopen_ck_serde(lua_State *L);
