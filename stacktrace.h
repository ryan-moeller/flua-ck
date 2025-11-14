/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdio.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#ifdef NDEBUG
#define stacktrace(L, bottom) (void)
#else
static inline void
_stacktrace(lua_State *L, int bottom, const char *func, int line)
{
	bottom = lua_absindex(L, bottom);
	fprintf(stderr, "%s:%d:\n", func, line);
	for (int top = lua_gettop(L); bottom <= top; --top) {
		const char *typename, *string;

		typename = luaL_typename(L, top);
		string = luaL_tolstring(L, top, NULL);
		fprintf(stderr, "\tL[%d]: %s = %s\n", top, typename, string);
		lua_pop(L, 1);
	}
}

#define stacktrace(L, bottom) _stacktrace(L, bottom, __func__, __LINE__)
#endif
