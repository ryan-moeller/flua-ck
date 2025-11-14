/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Ryan Moeller
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef REFCOUNT_H
#define REFCOUNT_H

#include <stdbool.h>
#include <stdint.h>

#include <ck_pr.h>

#if __SIZEOF_POINTER__ > 4

typedef uint64_t refcount;

static inline void
refcount_init(refcount *refs)
{
	ck_pr_store_64(refs, 1);
}

static inline void
refcount_retain(refcount *refs)
{
	ck_pr_add_64(refs, 1);
}

static inline bool
refcount_release(refcount *refs)
{
	ck_pr_fence_release();
	if (ck_pr_faa_64(refs, -1) == 1) {
		ck_pr_fence_acquire();
		return (true);
	}
	return (false);
}

#else

typedef uint32_t refcount;

static inline void
refcount_init(refcount *refs)
{
	ck_pr_store_32(refs, 1);
}

static inline void
refcount_retain(refcount *refs)
{
	ck_pr_add_32(refs, 1);
}

static inline bool
refcount_release(refcount *refs)
{
	ck_pr_fence_release();
	if (ck_pr_faa_32(refs, -1) == 1) {
		ck_pr_fence_acquire();
		return (true);
	}
	return (false);
}

#endif

#endif
