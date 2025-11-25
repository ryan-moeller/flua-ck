/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <assert.h>
#include <errno.h>
#include <malloc_np.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ck_epoch.h>
#include <ck_ht.h>
#include <ck_malloc.h>
#include <ck_md.h>
#include <ck_pr.h>
#include <ck_spinlock.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "serde.h"
#include "serdebuf.h"
#include "luaerror.h"

#define CK_EPOCH_RECORD_METATABLE "ck_epoch_record_t"

#define SERDE_TYPE_CODE_MAX ((1 << (sizeof(serde_type_code) * NBBY)) - 1)
#define SERDE_CACHE_CAPACITY (SERDE_TYPE_CODE_MAX + 1)

static void *serde_cache[SERDE_CACHE_CAPACITY];
static unsigned int serde_cache_len;
static ck_ht_t serde_cache_types CK_CC_CACHELINE; /* serde => type */
static ck_spinlock_t serde_cache_lock; /* serializes cache+types updates */
static ck_epoch_t serde_cache_epoch; /* free deferral for types hash table */
static ck_epoch_record_t module_serde_cache_record; /* reserved for init/fini */
__thread static ck_epoch_record_t *thread_serde_cache_record;

static void *
serde_ck_malloc(size_t sz)
{
	ck_epoch_entry_t *entry;

	if ((entry = malloc(sizeof(*entry) + sz)) == NULL) {
		return (NULL);
	}
	return (++entry);
}

static void
serde_ck_destroy(ck_epoch_entry_t *entry)
{
	free(entry);
}

static void
serde_ck_free(void *p, size_t sz __unused, bool defer)
{
	ck_epoch_entry_t *entry = p;

	--entry;
	if (defer) {
		ck_epoch_record_t *record = thread_serde_cache_record;

		if (record == NULL) {
			/* Must be the module destructor fini_serde_cache(). */
			record = &module_serde_cache_record;
		}
		ck_epoch_call(record, entry, serde_ck_destroy);
	} else {
		free(entry);
	}
}

static struct ck_malloc serde_ck_allocator = {
	.malloc = serde_ck_malloc,
	.free = serde_ck_free,
	.realloc = NULL,
};

#ifndef SERDE_CACHE_NBUCKETS
#define SERDE_CACHE_NBUCKETS 64
#endif
#ifndef SERDE_CACHE_INIT_CAPACITY
#define SERDE_CACHE_INIT_CAPACITY SERDE_CACHE_NBUCKETS
#endif
#ifndef SERDE_CACHE_SEED
#define SERDE_CACHE_SEED 0
#endif

__attribute__((constructor(PRIO_HT)))
static void
init_serde_cache(void)
{
	bool ok;

	ck_epoch_init(&serde_cache_epoch);
	ck_epoch_register(&serde_cache_epoch, &module_serde_cache_record, NULL);
	ok = ck_ht_init(&serde_cache_types, CK_HT_MODE_BYTESTRING, NULL,
	    &serde_ck_allocator, SERDE_CACHE_NBUCKETS, SERDE_CACHE_SEED);
	assert(ok);
	ck_spinlock_init(&serde_cache_lock);
}

__attribute__((destructor(PRIO_HT)))
static void
fini_serde_cache(void)
{
	thread_serde_cache_record = NULL;
	ck_ht_destroy(&serde_cache_types);
	ck_epoch_reclaim(&module_serde_cache_record);
	ck_epoch_unregister(&module_serde_cache_record);
	while (serde_cache_len-- > 0) {
		free(serde_cache[serde_cache_len]);
	}
}

static inline void
register_epoch_record(lua_State *L)
{
	ck_epoch_record_t *record;

	/*
	 * Once registered, a record must survive for the lifetime of
	 * serde_cache_epoch.  So, we have to allocate it on the heap but keep a
	 * lightuserdata uservalue for GC to reclaim and unregister the record
	 * when this thread is closed.
	 */
	if ((record = ck_epoch_recycle(&serde_cache_epoch, NULL)) == NULL &&
	    (record = malloc(sizeof(*record))) == NULL) {
		luaL_error(L, "malloc: %s", strerror(ENOMEM));
	}
	ck_epoch_register(&serde_cache_epoch, record, NULL);
	thread_serde_cache_record = record;
	new(L, record, CK_EPOCH_RECORD_METATABLE);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &serde_cache_epoch);
}

static int
l_ck_epoch_record_gc(lua_State *L)
{
	ck_epoch_record_t *record;

	record = checkcookie(L, 1, CK_EPOCH_RECORD_METATABLE);

	ck_epoch_reclaim(record);
	ck_epoch_unregister(record);
	return (0);
}

int
cache_serde(lua_State *L, int idx, serde_type_code * _Nonnull typep)
{
	struct serdebuf sb;
	ck_ht_entry_t entry;
	ck_ht_hash_t hash;
	void *serialized;
	size_t len;
	unsigned int i;
	serde_type_code type;
	int error;
	bool ok;

	/* ... */
	if ((error = getserdemethods(L, idx)) != 0) {
		return (error);
	}
	/* ..., ser, de */
	/* Check the registry for this serde first. */
	lua_rawgetp(L, LUA_REGISTRYINDEX, serde_cache);
	/* ..., ser, de, cache */
	/*
	 * The cache in the registry is a table with the following layout:
	 *
	 *   type => {serialize=fn, deserialize=fn, __index=self}
	 *
	 * Search the table for our serde metatable and return the type if
	 * found.
	 */
	lua_pushnil(L);
	/* ..., ser, de, cache, nil */
	while (lua_next(L, -2) != 0) {
		bool smatch, dmatch;

		/* ..., ser, de, cache, type, serde */
		assert(lua_istable(L, -1));

		lua_getfield(L, -1, "serialize");
		/* ..., ser, de, cache, type, serde, ser? */
		smatch = lua_rawequal(L, -6, -1);
		lua_pop(L, 1);
		/* ..., ser, de, cache, type, serde */

		lua_getfield(L, -1, "deserialize");
		/* ..., ser, de, cache, type, serde, de? */
		dmatch = lua_rawequal(L, -5, -1);
		lua_pop(L, 1);
		/* ..., ser, de, cache, type, serde */

		if (smatch && dmatch) {
			*typep = lua_tointeger(L, -2);
			lua_pop(L, 4);
			/* ..., ser */
			return (0);
		}
		lua_pop(L, 1);
		/* ..., ser, de, cache, type */
	}
	/* ..., ser, de, cache */
	if ((error = serdebuf_init(L, -2, &sb)) != 0) {
		return (error);
	}
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, -3, &sb, &type)) != 0) {
		return (error);
	}
	assert(type == SERDE_LCLOSURE || type == SERDE_CCLOSURE);
	type = SERDE_ANY;
	if ((error = serdebuf_serialize(L, -2, &sb, &type)) != 0) {
		return (error);
	}
	assert(type == SERDE_LCLOSURE || type == SERDE_CCLOSURE);
	if ((serialized = serdebuf_finalize(&sb, &len)) == NULL) {
		return (ENOMEM);
	}
	/* Hash table key length is a uint16_t parameter. */
	if (len > UINT16_MAX) {
		free(serialized);
		return (EOVERFLOW);
	}
	ck_ht_hash(&hash, &serde_cache_types, serialized, len);
	ck_ht_entry_key_set(&entry, serialized, len);
get:
	ck_epoch_begin(thread_serde_cache_record, NULL);
	ok = ck_ht_get_spmc(&serde_cache_types, hash, &entry);
	ck_epoch_end(thread_serde_cache_record, NULL);
	if (ok) {
		free(serialized);
		i = (unsigned int)(uintptr_t)ck_ht_entry_value(&entry);
		goto success;
	}
	if (error != 0) {
		/* Failed again. */
		free(serialized);
		return (error);
	}
	ck_epoch_begin(thread_serde_cache_record, NULL);
	ck_spinlock_lock(&serde_cache_lock);
	i = serde_cache_len;
	serde_cache[i] = serialized;
	ck_ht_entry_set(&entry, hash, serialized, len, (void *)(uintptr_t)i);
	if ((ok = ck_ht_put_spmc(&serde_cache_types, hash, &entry))) {
		++serde_cache_len;
	}
	ck_spinlock_unlock(&serde_cache_lock);
	ck_epoch_end(thread_serde_cache_record, NULL);
	if (!ok) {
		/*
		 * The CK HT API doesn't indicate why put failed.  If get fails
		 * again then put must have failed because of ENOMEM.  Set the
		 * error now to return if get fails.
		 *
		 * Ideally, the put operation would just update the value in
		 * the passed entry if its key was already present.
		 */
		error = ENOMEM;
		goto get;
	}
success:
	type = i + SERDE_CUSTOM;
	*typep = type;
	/* Cache deserialized serde methods for type in this thread. */
	/* ..., ser, de, cache */
	assert(lua_istable(L, -1)); /* cache */
	lua_createtable(L, 2, 0); /* {nil, nil} */
	/* ..., ser, de, cache, serde */
	lua_pushvalue(L, -4);
	/* ..., ser, de, cache, serde, ser */
	lua_setfield(L, -2, "serialize");
	/* ..., ser, de, cache, serde */
	lua_pushvalue(L, -3);
	/* ..., ser, de, cache, serde, de */
	lua_setfield(L, -2, "deserialize");
	/* ..., ser, de, cache, serde */
	lua_pushvalue(L, -1);
	/* ..., ser, de, cache, serde, serde */
	lua_setfield(L, -2, "__index");
	/* ..., ser, de, cache, serde */
	lua_rawseti(L, -2, type); /* cache[type] = serde */
	/* ..., ser, de, cache */
	lua_pop(L, 2);
	/* ..., ser */
	assert(lua_isfunction(L, -1)); /* serialize */
	return (0);
}

static inline const void * _Nonnull
consume(const void * _Nonnull p, size_t len, void * _Nonnull dst)
{
	memcpy(dst, p, len);
	return (p + len);
}

static inline const void *loadsharedimpl(lua_State *, const void * _Nonnull,
    bool * _Nonnull);

static inline const void *
loadupvalues(lua_State *L, const void * _Nonnull p, bool * _Nonnull envp)
{
	unsigned count;

	p = consume(p, sizeof(count), &count);
	while (count-- > 0 && (p = loadsharedimpl(L, p, envp)) != NULL) {
		continue;
	}
	return (p);
}

static inline const void *
loadclosure(lua_State *L, const void * _Nonnull p)
{
	size_t size;

	p = consume(p, sizeof(size), &size);
	if (luaL_loadbufferx(L, p, size, NULL, "b") != LUA_OK) {
		/* error message pushed by lua_load */
		return (NULL);
	}
	return (p + size);
}

static inline void
setupvalues(lua_State *L, int bottom, bool env)
{
	assert(lua_isfunction(L, -1));
	for (int idx = bottom, fidx = lua_gettop(L), n = 1; idx < fidx; n++) {
		if (n == 1 && env) {
			continue;
		}
		lua_pushvalue(L, idx++);
		lua_setupvalue(L, fidx, n);
	}
	/* Remove the upvalues, leaving the closure. */
	lua_insert(L, bottom);
	lua_settop(L, bottom);
}

static inline const void *
loadsharedimpl(lua_State *L, const void * _Nonnull p, bool * _Nonnull envp)
{
	serde_type_code type;

	p = consume(p, sizeof(type), &type);
	if (type < 0) {
		lua_pushfstring(L, "invalid type (%d)", type);
		return (NULL);
	}
	switch (type) {
	case SERDE_ENV:
		*envp = true;
		return (p);
	case SERDE_NIL:
		lua_pushnil(L);
		return (p);
	case SERDE_BOOLEAN: {
		bool value;

		p = consume(p, sizeof(value), &value);
		lua_pushboolean(L, value);
		return (p);
	}
	case SERDE_LIGHTUSERDATA: {
		void *value;

		p = consume(p, sizeof(value), &value);
		lua_pushlightuserdata(L, value);
		return (p);
	}
	case SERDE_NUMBER: {
		lua_Number value;

		p = consume(p, sizeof(value), &value);
		lua_pushnumber(L, value);
		return (p);
	}
	case SERDE_INTEGER: {
		lua_Integer value;

		p = consume(p, sizeof(value), &value);
		lua_pushinteger(L, value);
		return (p);
	}
	case SERDE_STRING: {
		size_t len;

		p = consume(p, sizeof(len), &len);
		lua_pushlstring(L, p, len);
		return (p + len);
	}
	case SERDE_LCLOSURE: {
		int bottom = lua_gettop(L) + 1;
		bool env = false;

		if ((p = loadupvalues(L, p, &env)) == NULL) {
			return (NULL);
		}
		if ((p = loadclosure(L, p)) == NULL) {
			return (NULL);
		}
		setupvalues(L, bottom, env);
		return (p);
	}
	case SERDE_CCLOSURE: {
		lua_CFunction value;
		int bottom = lua_gettop(L) + 1;
		bool env = false;

		if ((p = loadupvalues(L, p, &env)) == NULL) {
			return (NULL);
		}
		p = consume(p, sizeof(value), &value);
		lua_pushcfunction(L, value);
		setupvalues(L, bottom, env);
		return (p);
	}
	default: {
		FILE *f;
		size_t size;
		int error;

		p = consume(p, sizeof(size), &size);
		if ((f = fmemopen(__DECONST(char *, p), size, "rbe")) == NULL) {
			lua_pushfstring(L, "fmemopen: %s", strerror(errno));
			return (NULL);
		}
		setvbuf(f, NULL, _IONBF, 0);
		newstream(L, f);
		int bottom = lua_gettop(L);
		/* ..., stream */
		lua_rawgetp(L, LUA_REGISTRYINDEX, serde_cache);
		/* ..., stream, cache */
		if (lua_rawgeti(L, -1, type) == LUA_TTABLE) {
			/* ..., stream, cache, serde */
			lua_getfield(L, -1, "deserialize");
			/* ..., stream, cache, serde, de */
			lua_insert(L, -4);
			/* ..., de, stream, cache, serde */
			lua_insert(L, -4);
			/* ..., serde, de, stream, cache */
			lua_pop(L, 1);
			/* ..., serde, de, stream */
		} else {
			const void *serde = serde_cache[type - SERDE_CUSTOM];

			/* ..., stream, cache, nil */
			lua_pop(L, 1);
			/* ..., stream, cache */
			lua_createtable(L, 2, 0);
			/* ..., stream, cache, serde */
			if ((serde = loadshared(L, serde)) == NULL) {
				return (NULL);
			}
			if ((serde = loadshared(L, serde)) == NULL) {
				return (NULL);
			}
			/* ..., stream, cache, serde, ser, de */
			lua_pushvalue(L, -1);
			/* ..., stream, cache, serde, ser, de, de */
			lua_insert(L, -6);
			/* ..., de, stream, cache, serde, ser, de */
			lua_setfield(L, -3, "deserialize");
			/* ..., de, stream, cache, serde, ser */
			lua_setfield(L, -2, "serialize");
			/* ..., de, stream, cache, serde */
			lua_pushvalue(L, -1);
			/* ..., de, stream, cache, serde, serde */
			lua_setfield(L, -2, "__index");
			/* ..., de, stream, cache, serde */
			lua_pushvalue(L, -1);
			/* ..., de, stream, cache, serde, serde */
			lua_insert(L, -5);
			/* ..., serde, de, stream, cache, serde */
			lua_rawseti(L, -2, type);
			/* ..., serde, de, stream, cache */
			lua_pop(L, 1);
			/* ..., serde, de, stream */
		}
		/* ..., serde, de, stream */
		if ((error = lua_pcall(L, 1, 1, 0)) != LUA_OK) {
			/* error message pushed by lua_pcall */
			return (NULL);
		}
		/* ..., serde, deserialized */
		lua_insert(L, -2);
		/* ..., deserialized, serde */
		lua_setmetatable(L, -2);
		/* ..., custom */
		return (p);
	}
	}
}

const void *
loadshared(lua_State *L, const void * _Nonnull p)
{
	bool env;

	if ((p = loadsharedimpl(L, p, &env)) == NULL || env) {
	     return (NULL);
	}
	return (p);
}

static const struct luaL_Reg l_ck_epoch_record_meta[] = {
	{"__gc", l_ck_epoch_record_gc},
	{NULL, NULL}
};

int
luaopen_ck_serde(lua_State *L)
{
	luaL_newmetatable(L, CK_EPOCH_RECORD_METATABLE);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, l_ck_epoch_record_meta, 0);
	register_epoch_record(L);

	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, serde_cache);

	return (1);
}
