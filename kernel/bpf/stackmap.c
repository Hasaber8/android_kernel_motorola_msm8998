/* Copyright (c) 2016 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <linux/bpf.h>
#include <linux/jhash.h>
#include <linux/filter.h>
#include <linux/vmalloc.h>
#include <linux/stacktrace.h>
#include <linux/perf_event.h>

struct stack_map_bucket {
	struct rcu_head rcu;
	u32 hash;
	u32 nr;
	u64 ip[];
};

struct bpf_stack_map {
	struct bpf_map map;
	u32 n_buckets;
	struct stack_map_bucket __rcu *buckets[];
};

/* Called from syscall */
static struct bpf_map *stack_map_alloc(union bpf_attr *attr)
{
	u32 value_size = attr->value_size;
	struct bpf_stack_map *smap;
	u64 cost, n_buckets;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return ERR_PTR(-EPERM);

	/* check sanity of attributes */
	if (attr->max_entries == 0 || attr->key_size != 4 ||
	    value_size < 8 || value_size % 8 ||
	    value_size / 8 > PERF_MAX_STACK_DEPTH)
		return ERR_PTR(-EINVAL);

	/* hash table size must be power of 2 */
	n_buckets = roundup_pow_of_two(attr->max_entries);

	cost = n_buckets * sizeof(struct stack_map_bucket *) + sizeof(*smap);
	if (cost >= U32_MAX - PAGE_SIZE)
		return ERR_PTR(-E2BIG);

	smap = kzalloc(cost, GFP_USER | __GFP_NOWARN);
	if (!smap) {
		smap = vzalloc(cost);
		if (!smap)
			return ERR_PTR(-ENOMEM);
	}

	err = -E2BIG;
	cost += n_buckets * (value_size + sizeof(struct stack_map_bucket));
	if (cost >= U32_MAX - PAGE_SIZE)
		goto free_smap;

	smap->map.map_type = attr->map_type;
	smap->map.key_size = attr->key_size;
	smap->map.value_size = value_size;
	smap->map.max_entries = attr->max_entries;
	smap->n_buckets = n_buckets;
	smap->map.pages = round_up(cost, PAGE_SIZE) >> PAGE_SHIFT;

	err = get_callchain_buffers();
	if (err)
		goto free_smap;

	return &smap->map;

free_smap:
	kvfree(smap);
	return ERR_PTR(err);
}

static u64 bpf_get_stackid(u64 r1, u64 r2, u64 flags, u64 r4, u64 r5)
{
	struct pt_regs *regs = (struct pt_regs *) (long) r1;
	struct bpf_map *map = (struct bpf_map *) (long) r2;
	struct bpf_stack_map *smap = container_of(map, struct bpf_stack_map, map);
	struct perf_callchain_entry *trace;
	struct stack_map_bucket *bucket, *new_bucket, *old_bucket;
	u32 max_depth = map->value_size / 8;
	/* stack_map_alloc() checks that max_depth <= PERF_MAX_STACK_DEPTH */
	u32 init_nr = PERF_MAX_STACK_DEPTH - max_depth;
	u32 skip = flags & BPF_F_SKIP_FIELD_MASK;
	u32 hash, id, trace_nr, trace_len;
	bool user = flags & BPF_F_USER_STACK;
	bool kernel = !user;
	u64 *ips;

	if (unlikely(flags & ~(BPF_F_SKIP_FIELD_MASK | BPF_F_USER_STACK |
			       BPF_F_FAST_STACK_CMP | BPF_F_REUSE_STACKID)))
		return -EINVAL;

	trace = get_perf_callchain(regs, init_nr, kernel, user, false, false);

	if (unlikely(!trace))
		/* couldn't fetch the stack trace */
		return -EFAULT;

	/* get_perf_callchain() guarantees that trace->nr >= init_nr
	 * and trace-nr <= PERF_MAX_STACK_DEPTH, so trace_nr <= max_depth
	 */
	trace_nr = trace->nr - init_nr;

	if (trace_nr <= skip)
		/* skipping more than usable stack trace */
		return -EFAULT;

	trace_nr -= skip;
	trace_len = trace_nr * sizeof(u64);
	ips = trace->ip + skip + init_nr;
	hash = jhash2((u32 *)ips, trace_len / sizeof(u32), 0);
	id = hash & (smap->n_buckets - 1);
	bucket = rcu_dereference(smap->buckets[id]);

	if (bucket && bucket->hash == hash) {
		if (flags & BPF_F_FAST_STACK_CMP)
			return id;
		if (bucket->nr == trace_nr &&
		    memcmp(bucket->ip, ips, trace_len) == 0)
			return id;
	}

	/* this call stack is not in the map, try to add it */
	if (bucket && !(flags & BPF_F_REUSE_STACKID))
		return -EEXIST;

	new_bucket = kmalloc(sizeof(struct stack_map_bucket) + map->value_size,
			     GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!new_bucket))
		return -ENOMEM;

	memcpy(new_bucket->ip, ips, trace_len);
	memset(new_bucket->ip + trace_len / 8, 0, map->value_size - trace_len);
	new_bucket->hash = hash;
	new_bucket->nr = trace_nr;

	old_bucket = xchg(&smap->buckets[id], new_bucket);
	if (old_bucket)
		kfree_rcu(old_bucket, rcu);
	return id;
}

const struct bpf_func_proto bpf_get_stackid_proto = {
	.func		= bpf_get_stackid,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
};

/* Called from syscall or from eBPF program */
static void *stack_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_stack_map *smap = container_of(map, struct bpf_stack_map, map);
	struct stack_map_bucket *bucket;
	u32 id = *(u32 *)key;

	if (unlikely(id >= smap->n_buckets))
		return NULL;
	bucket = rcu_dereference(smap->buckets[id]);
	return bucket ? bucket->ip : NULL;
}

static int stack_map_get_next_key(struct bpf_map *map, void *key, void *next_key)
{
	return -EINVAL;
}

static int stack_map_update_elem(struct bpf_map *map, void *key, void *value,
				 u64 map_flags)
{
	return -EINVAL;
}

/* Called from syscall or from eBPF program */
static int stack_map_delete_elem(struct bpf_map *map, void *key)
{
	struct bpf_stack_map *smap = container_of(map, struct bpf_stack_map, map);
	struct stack_map_bucket *old_bucket;
	u32 id = *(u32 *)key;

	if (unlikely(id >= smap->n_buckets))
		return -E2BIG;

	old_bucket = xchg(&smap->buckets[id], NULL);
	if (old_bucket) {
		kfree_rcu(old_bucket, rcu);
		return 0;
	} else {
		return -ENOENT;
	}
}

/* Called when map->refcnt goes to zero, either from workqueue or from syscall */
static void stack_map_free(struct bpf_map *map)
{
	struct bpf_stack_map *smap = container_of(map, struct bpf_stack_map, map);
	int i;

	synchronize_rcu();

	for (i = 0; i < smap->n_buckets; i++)
		if (smap->buckets[i])
			kfree_rcu(smap->buckets[i], rcu);
	kvfree(smap);
	put_callchain_buffers();
}

static const struct bpf_map_ops stack_map_ops = {
	.map_alloc = stack_map_alloc,
	.map_free = stack_map_free,
	.map_get_next_key = stack_map_get_next_key,
	.map_lookup_elem = stack_map_lookup_elem,
	.map_update_elem = stack_map_update_elem,
	.map_delete_elem = stack_map_delete_elem,
};

static struct bpf_map_type_list stack_map_type __read_mostly = {
	.ops = &stack_map_ops,
	.type = BPF_MAP_TYPE_STACK_TRACE,
};

static int __init register_stack_map(void)
{
	bpf_register_map_type(&stack_map_type);
	return 0;
}
late_initcall(register_stack_map);
