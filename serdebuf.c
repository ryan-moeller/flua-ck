/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <assert.h>
#include <errno.h>
#include <malloc_np.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <ck_md.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "serde.h"
#include "serdebuf.h"

static const size_t serde_type_size[] = {
	[SERDE_ENV] = 0,
	[SERDE_NIL] = 0,
	[SERDE_BOOLEAN] = sizeof(bool),
	[SERDE_LIGHTUSERDATA] = sizeof(void *),
	[SERDE_INTEGER] = sizeof(lua_Integer),
	[SERDE_NUMBER] = sizeof(lua_Number),
	/*
	 * Strings have a length prefixed. The actual size is known in advance
	 * and will be added before allocating.
	 */
	[SERDE_STRING] = sizeof(size_t),
	/*
	 * Closures have upvalues prefixed (nupvalues, upvalues).
	 *
	 * Lua closures have bytecode with length prefixed after upvalues:
	 * [SERDE_LCLOSURE] = sizeof(unsigned) + sizeof(size_t),
	 *
	 * C closures have a function pointer after upvalues:
	 * [SERDE_CCLOSURE] = sizeof(unsigned) + sizeof(lua_CFunction),
	 *
	 * Both forms of closures require indeterminate space for upvalues and
	 * bytecode (in the case of Lua closures).  Allocate a moderately sized
	 * buffer for Lua closures and a convervatively sized buffer for 
	 * closures on the assumption that there will be relatively few if any
	 * upvalues but a moderate amount of bytecode.
	 */
	[SERDE_CCLOSURE] = CK_MD_CACHELINE,
	[SERDE_LCLOSURE] = CK_MD_PAGESIZE,
	/* 
	 * Custom encoders produce a blob with length prefixed:
	 * [SERDE_CUSTOM] = sizeof(size_t),
	 *
	 * We don't know in advance how much space the value requires. It could
	 * be 0 bytes, it could be gigabytes.  Allocate a conseratively sized
	 * buffer.
	 */
	[SERDE_CUSTOM] = CK_MD_CACHELINE,
};

static inline int
serdebuf_flags(size_t size)
{
	if (size >= CK_MD_PAGESIZE) {
		return (MALLOCX_ALIGN(CK_MD_PAGESIZE));
	} else if (size >= CK_MD_CACHELINE) {
		return (MALLOCX_ALIGN(CK_MD_CACHELINE));
	} else {
		return (0);
	}
}

int
serdebuf_init(lua_State *L, int idx, struct serdebuf *sb)
{
	enum serde_type type = serde_type(L, idx);
	size_t size;

	if (type == SERDE_INVALID) {
		return (EINVAL);
	}
	size = sizeof(serde_type_code) + serde_type_size[type];
	if (type == SERDE_STRING) {
		size += luaL_len(L, idx);
	}
	if ((sb->buf = mallocx(size, serdebuf_flags(size))) == NULL) {
		return (ENOMEM);
	}
	sb->cur = sb->buf;
	sb->cap = size;
	return (0);
}

static inline size_t
serdebuf_roundup(size_t size)
{
	if (size >= CK_MD_PAGESIZE) {
		return (roundup2(size, CK_MD_PAGESIZE));
	}
	return (roundup2(size, CK_MD_CACHELINE));
}

static inline int
serdebuf_resize(struct serdebuf *sb, size_t minimum)
{
	void *p;
	size_t offset = serdebuf_size(sb);
	size_t size = serdebuf_roundup(minimum);

	if ((p = rallocx(sb->buf, size, serdebuf_flags(size))) == NULL) {
		return (ENOMEM);
	}
	sb->buf = p;
	sb->cap = size;
	sb->cur = p + offset;
	return (0);
}

int
serdebuf_append(struct serdebuf *sb, const void *p, size_t len)
{
	size_t needed = serdebuf_size(sb) + len;

	if (needed > sb->cap) {
		size_t newsize = sb->cap << 1;
		int error;

		if (newsize < needed) {
			newsize = needed;
		}
		if ((error = serdebuf_resize(sb, newsize)) != 0) {
			return (error);
		}
	}
	sb->cur = mempcpy(sb->cur, p, len);
	return (0);
}

/*
 * Upvalue serialization imposes a few constraints to keep things simple:
 *
 *  - Upvalues must be serializable, obviously.
 *
 *  - Upvalues cannot be functions.
 *
 *  - Upvalues cannot require use of a custom serde.
 *
 *  - Upvalue identity is not maintained.
 *
 * The latter three constraints are required to avoid having to encode a
 * potentially cyclic graph of references.  It's possible, but is it worth the
 * complexity?
 */
static inline int
serdebuf_serialize_upvalues(lua_State *L, int idx, struct serdebuf *sb)
{
	const char *name;
	unsigned *countp, i;
	size_t count_offset = serdebuf_size(sb);
	int error;

	/* Make room for the count to be filled in later. */
	if ((error = serdebuf_append(sb, &i, sizeof(i))) != 0) {
		return (error);
	}
	for (i = 1; (name = lua_getupvalue(L, idx, i)) != NULL; i++) {
		serde_type_code type;

		if (strcmp(name, "_ENV") == 0) {
			type = SERDE_ENV;
		} else switch (lua_type(L, -1)) {
		case LUA_TFUNCTION:
		case LUA_TTABLE:
		case LUA_TUSERDATA:
		case LUA_TTHREAD:
			return (EINVAL);
		default:
			type = SERDE_ANY;
		}
		if ((error = serdebuf_serialize(L, -1, sb, &type)) != 0) {
			return (error);
		}
		lua_pop(L, 1);
	}
	countp = sb->buf + count_offset;
	*countp = i - 1;
	return (0);
}

static int
serdebuf_writer(lua_State *L __unused, const void *p, size_t sz, void *ud)
{
	struct serdebuf *sb = ud;

	return (serdebuf_append(sb, p, sz));
}

static inline int
serdebuf_dump(lua_State *L, int idx, struct serdebuf *sb)
{
	size_t *sizep;
	size_t start;
	int error;

	/* Make room for the size to be filled in later. */
	if ((error = serdebuf_append(sb, &start, sizeof(start))) != 0) {
		return (error);
	}
	start = serdebuf_size(sb);
	lua_pushvalue(L, idx);
	if ((error = lua_dump(L, serdebuf_writer, sb, true)) != 0) {
		return (error);
	}
	lua_pop(L, 1);
	sizep = sb->buf + start - sizeof(start);
	*sizep = serdebuf_size(sb) - start;
	return (0);
}

static int
serdebuf_writefn(void *cookie, const char *p, int len)
{
	struct serdebuf *sb = cookie;
	int error;

	if ((error = serdebuf_append(sb, p, len)) != 0) {
		errno = error;
		return (-1);
	}
	return (len);
}

static inline int
serdebuf_serialize_custom(lua_State *L, int idx, struct serdebuf *sb)
{
	FILE *f;
	const char *p;
	size_t *sizep;
	size_t start, len;
	int error;

	/* ..., ser */
	assert(lua_isfunction(L, -1)); /* serialize */

	lua_pushvalue(L, idx);
	/* ..., ser, obj */
	/* Make room for the size to be filled in later. */
	if ((error = serdebuf_append(sb, &start, sizeof(start))) != 0) {
		return (error);
	}
	start = serdebuf_size(sb);
	/* Wrap sb in a write-only file stream to pass to the function. */
	if ((f = fwopen(sb, serdebuf_writefn)) == NULL) {
		return (errno);
	}
	setvbuf(f, NULL, _IONBF, 0);
	newstream(L, f);
	/* ..., ser, obj, stream */
	if ((error = lua_pcall(L, 2, 0, 0) != LUA_OK)) {
		return (-error);
	}
	/* ... */
	sizep = sb->buf + start - sizeof(start);
	*sizep = serdebuf_size(sb) - start;
	return (0);
}

static inline serde_type_code
serde_type_encode(lua_State *L, int idx, serde_type_code t)
{
	return (t == SERDE_ANY ? serde_type(L, idx) : t);
}

int
serdebuf_serialize(lua_State *L, int idx, struct serdebuf *sb,
    serde_type_code *typep)
{
	size_t type_offset = serdebuf_size(sb);
	serde_type_code type = serde_type_encode(L, idx, *typep); 
	int error;

	*typep = type;
	if ((error = serdebuf_append(sb, typep, sizeof(*typep))) != 0) {
		return (error);
	}
	switch (type) {
	case SERDE_ENV:
	case SERDE_NIL:
		return (0);
	case SERDE_BOOLEAN: {
		bool value = lua_toboolean(L, idx);

		return (serdebuf_append(sb, &value, sizeof(value)));
	}
	case SERDE_LIGHTUSERDATA: {
		void *value = lua_touserdata(L, idx);

		return (serdebuf_append(sb, &value, sizeof(value)));
	}
	case SERDE_NUMBER: {
		lua_Number value = lua_tonumber(L, idx);

		return (serdebuf_append(sb, &value, sizeof(value)));
	}
	case SERDE_INTEGER: {
		lua_Integer value = lua_tointeger(L, idx);

		return (serdebuf_append(sb, &value, sizeof(value)));
	}
	case SERDE_STRING: {
		const char *value;
		size_t len;

		value = lua_tolstring(L, idx, &len);
		if ((error = serdebuf_append(sb, &len, sizeof(len))) != 0) {
			return (error);
		}
		return (serdebuf_append(sb, value, len));
	}
	case SERDE_LCLOSURE: {
		if ((error = serdebuf_serialize_upvalues(L, idx, sb)) != 0) {
			return (error);
		}
		return (serdebuf_dump(L, idx, sb));
	}
	case SERDE_CCLOSURE: {
		lua_CFunction value = lua_tocfunction(L, idx);

		if ((error = serdebuf_serialize_upvalues(L, idx, sb)) != 0) {
			return (error);
		}
		return (serdebuf_append(sb, &value, sizeof(value)));
	}
	case SERDE_CUSTOM:
		if ((error = cache_serde(L, idx, typep)) != 0) {
			return (error);
		}
		memcpy(sb->buf + type_offset, typep, sizeof(*typep));
		return (serdebuf_serialize_custom(L, idx, sb));
	case SERDE_INVALID:
	default:
		return (EINVAL);
	}
}

void *
serdebuf_finalize(struct serdebuf *sb, size_t *lenp)
{
	void *p = sb->buf;
	size_t size = serdebuf_size(sb);

	memset(sb, 0, sizeof(*sb));
	if (lenp != NULL) {
		*lenp = size;
	}
	return (rallocx(p, size, serdebuf_flags(size)));
}

void
serdebuf_destroy(struct serdebuf *sb)
{
	free(sb->buf);
	memset(sb, 0, sizeof(*sb));
}
