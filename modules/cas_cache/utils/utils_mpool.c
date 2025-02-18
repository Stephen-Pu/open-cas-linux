/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "utils_mpool.h"
#include "ocf_env.h"

struct env_mpool {
	env_allocator *allocator[env_mpool_max];
		/*!< OS handle to memory pool */

	int mpool_max;
		/*!< Max mpool allocation order */

	uint32_t hdr_size;
		/*!< Data header size (constant allocation part) */

	uint32_t elem_size;
		/*!< Per element size increment (variable allocation part) */

	bool fallback;
		/*!< Should mpool fallback to vmalloc */

	int flags;
		/*!< Allocation flags */
};

struct env_mpool *env_mpool_create(uint32_t hdr_size, uint32_t elem_size,
		int flags, int mpool_max, bool fallback,
		const uint32_t limits[env_mpool_max],
		const char *name_perfix)
{
	uint32_t i;
	char name[MPOOL_ALLOCATOR_NAME_MAX] = { '\0' };
	int result;
	struct env_mpool *mpool;
	size_t size;

	mpool = env_zalloc(sizeof(*mpool), ENV_MEM_NORMAL);
	if (!mpool)
		return NULL;

	mpool->flags = flags;
	mpool->fallback = fallback;
	mpool->mpool_max = mpool_max;
	mpool->hdr_size = hdr_size;
	mpool->elem_size = elem_size;

	for (i = 0; i < min(env_mpool_max, mpool_max + 1); i++) {
		result = snprintf(name, sizeof(name), "%s_%u", name_perfix,
				(1 << i));
		if (result < 0 || result >= sizeof(name))
			goto err;

		size = hdr_size + (elem_size * (1 << i));

		mpool->allocator[i] = env_allocator_create_extended(
				size, name, limits ? limits[i] : -1);

		if (!mpool->allocator[i])
			goto err;
	}

	return mpool;

err:
	env_mpool_destroy(mpool);
	return NULL;
}

void env_mpool_destroy(struct env_mpool *mallocator)
{
	if (mallocator) {
		uint32_t i;

		for (i = 0; i < env_mpool_max; i++)
			if (mallocator->allocator[i])
				env_allocator_destroy(mallocator->allocator[i]);

		env_free(mallocator);
	}
}

static env_allocator *env_mpool_get_allocator(
	struct env_mpool *mallocator, uint32_t count)
{
	unsigned int idx;

	if (unlikely(count == 0))
		return env_mpool_1;

	idx = 31 - __builtin_clz(count);

	if (__builtin_ffs(count) <= idx)
		idx++;

	if (idx >= env_mpool_max || idx > mallocator->mpool_max)
		return NULL;

	return mallocator->allocator[idx];
}

void *env_mpool_new_f(struct env_mpool *mpool, uint32_t count, int flags)
{
	void *items = NULL;
	env_allocator *allocator;
	size_t size = mpool->hdr_size + (mpool->elem_size * count);

	allocator = env_mpool_get_allocator(mpool, count);

	if (allocator) {
		items = env_allocator_new(allocator);
	} else if(mpool->fallback) {
		items = cas_vmalloc(size,
			flags | __GFP_ZERO | __GFP_HIGHMEM);
	}

#ifdef ZERO_OR_NULL_PTR
	if (ZERO_OR_NULL_PTR(items))
		return NULL;
#endif

	return items;
}

void *env_mpool_new(struct env_mpool *mpool, uint32_t count)
{
	return env_mpool_new_f(mpool, count, mpool->flags);
}

bool env_mpool_del(struct env_mpool *mpool,
		void *items, uint32_t count)
{
	env_allocator *allocator;

	allocator = env_mpool_get_allocator(mpool, count);

	if (allocator)
		env_allocator_del(allocator, items);
	else if (mpool->fallback)
		cas_vfree(items);
	else
		return false;

	return true;
}
