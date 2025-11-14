/*
 * Copyright (c) 2025 Ryan Moeller
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sys/param.h>

#include <lua.h>

#include "serde.h"

struct serdebuf {
	void *buf;
	void *cur;
	size_t cap;
};

static inline size_t
serdebuf_size(struct serdebuf *sb)
{
	return (sb->cur - sb->buf);
}

int serdebuf_init(lua_State *L, int idx, struct serdebuf *sb);
int serdebuf_append(struct serdebuf *sb, const void *p, size_t len);
int serdebuf_serialize(lua_State *L, int idx, struct serdebuf *sb,
    serde_type_code *typep);
void *serdebuf_finalize(struct serdebuf *sb, size_t *lenp);
void serdebuf_destroy(struct serdebuf *sb);
