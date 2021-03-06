/* Copyright (c) 2011-2015 PLUMgrid, http://plumgrid.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include "trace.h"

static DEFINE_PER_CPU(int, bpf_prog_active);

/**
 * trace_call_bpf - invoke BPF program
 * @call: tracepoint event
 * @ctx: opaque context pointer
 *
 * kprobe handlers execute BPF programs via this helper.
 * Can be used from static tracepoints in the future.
 *
 * Return: BPF programs always return an integer which is interpreted by
 * kprobe handler as:
 * 0 - return from kprobe (event is filtered out)
 * 1 - store kprobe event into ring buffer
 * Other values are reserved and currently alias to 1
 */
unsigned int trace_call_bpf(struct trace_event_call *call, void *ctx)
{
	unsigned int ret;

	if (in_nmi()) /* not supported yet */
		return 1;

	preempt_disable();

	if (unlikely(__this_cpu_inc_return(bpf_prog_active) != 1)) {
		/*
		 * since some bpf program is already running on this cpu,
		 * don't call into another bpf program (same or different)
		 * and don't send kprobe event into ring-buffer,
		 * so return zero here
		 */
		ret = 0;
		goto out;
	}

	/*
	 * Instead of moving rcu_read_lock/rcu_dereference/rcu_read_unlock
	 * to all call sites, we did a bpf_prog_array_valid() there to check
	 * whether call->prog_array is empty or not, which is
	 * a heurisitc to speed up execution.
	 *
	 * If bpf_prog_array_valid() fetched prog_array was
	 * non-NULL, we go into trace_call_bpf() and do the actual
	 * proper rcu_dereference() under RCU lock.
	 * If it turns out that prog_array is NULL then, we bail out.
	 * For the opposite, if the bpf_prog_array_valid() fetched pointer
	 * was NULL, you'll skip the prog_array with the risk of missing
	 * out of events when it was updated in between this and the
	 * rcu_dereference() which is accepted risk.
	 */
	ret = BPF_PROG_RUN_ARRAY_CHECK(call->prog_array, ctx, BPF_PROG_RUN);

 out:
	__this_cpu_dec(bpf_prog_active);
	preempt_enable();

	return ret;
}
EXPORT_SYMBOL_GPL(trace_call_bpf);

static u64 bpf_probe_read(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5)
{
	void *dst = (void *) (long) r1;
	int size = (int) r2;
	void *unsafe_ptr = (void *) (long) r3;

	return probe_kernel_read(dst, unsafe_ptr, size);
}

static const struct bpf_func_proto bpf_probe_read_proto = {
	.func		= bpf_probe_read,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_STACK,
	.arg2_type	= ARG_CONST_STACK_SIZE,
	.arg3_type	= ARG_ANYTHING,
};

/*
 * limited trace_printk()
 * only %d %u %x %ld %lu %lx %lld %llu %llx %p %s conversion specifiers allowed
 */
static u64 bpf_trace_printk(u64 r1, u64 fmt_size, u64 r3, u64 r4, u64 r5)
{
	char *fmt = (char *) (long) r1;
	bool str_seen = false;
	int mod[3] = {};
	int fmt_cnt = 0;
	u64 unsafe_addr;
	char buf[64];
	int i;

	/*
	 * bpf_check()->check_func_arg()->check_stack_boundary()
	 * guarantees that fmt points to bpf program stack,
	 * fmt_size bytes of it were initialized and fmt_size > 0
	 */
	if (fmt[--fmt_size] != 0)
		return -EINVAL;

	/* check format string for allowed specifiers */
	for (i = 0; i < fmt_size; i++) {
		if ((!isprint(fmt[i]) && !isspace(fmt[i])) || !isascii(fmt[i]))
			return -EINVAL;

		if (fmt[i] != '%')
			continue;

		if (fmt_cnt >= 3)
			return -EINVAL;

		/* fmt[i] != 0 && fmt[last] == 0, so we can access fmt[i + 1] */
		i++;
		if (fmt[i] == 'l') {
			mod[fmt_cnt]++;
			i++;
		} else if (fmt[i] == 'p' || fmt[i] == 's') {
			mod[fmt_cnt]++;
			/* disallow any further format extensions */
			if (fmt[i + 1] != 0 &&
			    !isspace(fmt[i + 1]) &&
			    !ispunct(fmt[i + 1]))
				return -EINVAL;
			fmt_cnt++;
			if (fmt[i] == 's') {
				if (str_seen)
					/* allow only one '%s' per fmt string */
					return -EINVAL;
				str_seen = true;

				switch (fmt_cnt) {
				case 1:
					unsafe_addr = r3;
					r3 = (long) buf;
					break;
				case 2:
					unsafe_addr = r4;
					r4 = (long) buf;
					break;
				case 3:
					unsafe_addr = r5;
					r5 = (long) buf;
					break;
				}
				buf[0] = 0;
				strncpy_from_unsafe(buf,
						    (void *) (long) unsafe_addr,
						    sizeof(buf));
			}
			continue;
		}

		if (fmt[i] == 'l') {
			mod[fmt_cnt]++;
			i++;
		}

		if (fmt[i] != 'd' && fmt[i] != 'u' && fmt[i] != 'x')
			return -EINVAL;
		fmt_cnt++;
	}

	return __trace_printk(1/* fake ip will not be printed */, fmt,
			      mod[0] == 2 ? r3 : mod[0] == 1 ? (long) r3 : (u32) r3,
			      mod[1] == 2 ? r4 : mod[1] == 1 ? (long) r4 : (u32) r4,
			      mod[2] == 2 ? r5 : mod[2] == 1 ? (long) r5 : (u32) r5);
}

static const struct bpf_func_proto bpf_trace_printk_proto = {
	.func		= bpf_trace_printk,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_STACK,
	.arg2_type	= ARG_CONST_STACK_SIZE,
};

const struct bpf_func_proto *bpf_get_trace_printk_proto(void)
{
	/*
	 * this program might be calling bpf_trace_printk,
	 * so allocate per-cpu printk buffers
	 */
	trace_printk_init_buffers();

	return &bpf_trace_printk_proto;
}

static u64 bpf_perf_event_read(u64 r1, u64 index, u64 r3, u64 r4, u64 r5)
{
	struct bpf_map *map = (struct bpf_map *) (unsigned long) r1;
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	struct perf_event *event;
	struct file *file;

	if (unlikely(index >= array->map.max_entries))
		return -E2BIG;

	file = (struct file *)array->ptrs[index];
	if (unlikely(!file))
		return -ENOENT;

	event = file->private_data;

	/* make sure event is local and doesn't have pmu::count */
	if (event->oncpu != smp_processor_id() ||
	    event->pmu->count)
		return -EINVAL;

	if (unlikely(event->attr.type != PERF_TYPE_HARDWARE &&
		     event->attr.type != PERF_TYPE_RAW))
		return -EINVAL;

	/*
	 * we don't know if the function is run successfully by the
	 * return value. It can be judged in other places, such as
	 * eBPF programs.
	 */
	return perf_event_read_local(event);
}

static const struct bpf_func_proto bpf_perf_event_read_proto = {
	.func		= bpf_perf_event_read,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_CONST_MAP_PTR,
	.arg2_type	= ARG_ANYTHING,
};

static u64 bpf_perf_event_output(u64 r1, u64 r2, u64 index, u64 r4, u64 size)
{
	struct pt_regs *regs = (struct pt_regs *) (long) r1;
	struct bpf_map *map = (struct bpf_map *) (long) r2;
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	void *data = (void *) (long) r4;
	struct perf_sample_data sample_data;
	struct perf_event *event;
	struct file *file;
	struct perf_raw_record raw = {
		.size = size,
		.data = data,
	};

	if (unlikely(index >= array->map.max_entries))
		return -E2BIG;

	file = (struct file *)array->ptrs[index];
	if (unlikely(!file))
		return -ENOENT;

	event = file->private_data;

	if (unlikely(event->attr.type != PERF_TYPE_SOFTWARE ||
		     event->attr.config != PERF_COUNT_SW_BPF_OUTPUT))
		return -EINVAL;

	if (unlikely(event->oncpu != smp_processor_id()))
		return -EOPNOTSUPP;

	perf_sample_data_init(&sample_data, 0, 0);
	sample_data.raw = &raw;
	perf_event_output(event, &sample_data, regs);
	return 0;
}

static const struct bpf_func_proto bpf_perf_event_output_proto = {
	.func		= bpf_perf_event_output,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
	.arg4_type	= ARG_PTR_TO_STACK,
	.arg5_type	= ARG_CONST_STACK_SIZE,
};

static const struct bpf_func_proto *kprobe_prog_func_proto(enum bpf_func_id func_id)
{
	switch (func_id) {
	case BPF_FUNC_map_lookup_elem:
		return &bpf_map_lookup_elem_proto;
	case BPF_FUNC_map_update_elem:
		return &bpf_map_update_elem_proto;
	case BPF_FUNC_map_delete_elem:
		return &bpf_map_delete_elem_proto;
	case BPF_FUNC_probe_read:
		return &bpf_probe_read_proto;
	case BPF_FUNC_ktime_get_ns:
		return &bpf_ktime_get_ns_proto;
	case BPF_FUNC_tail_call:
		return &bpf_tail_call_proto;
	case BPF_FUNC_get_current_pid_tgid:
		return &bpf_get_current_pid_tgid_proto;
	case BPF_FUNC_get_current_uid_gid:
		return &bpf_get_current_uid_gid_proto;
	case BPF_FUNC_get_current_comm:
		return &bpf_get_current_comm_proto;
	case BPF_FUNC_trace_printk:
		return bpf_get_trace_printk_proto();
	case BPF_FUNC_get_smp_processor_id:
		return &bpf_get_smp_processor_id_proto;
	case BPF_FUNC_perf_event_read:
		return &bpf_perf_event_read_proto;
	case BPF_FUNC_perf_event_output:
		return &bpf_perf_event_output_proto;
	case BPF_FUNC_get_stackid:
		return &bpf_get_stackid_proto;
	default:
		return NULL;
	}
}

/* bpf+kprobe programs can access fields of 'struct pt_regs' */
static bool kprobe_prog_is_valid_access(int off, int size, enum bpf_access_type type)
{
	/* check bounds */
	if (off < 0 || off >= sizeof(struct pt_regs))
		return false;

	/* only read is allowed */
	if (type != BPF_READ)
		return false;

	/* disallow misaligned access */
	if (off % size != 0)
		return false;

	return true;
}

static struct bpf_verifier_ops kprobe_prog_ops = {
	.get_func_proto  = kprobe_prog_func_proto,
	.is_valid_access = kprobe_prog_is_valid_access,
};

static struct bpf_prog_type_list kprobe_tl = {
	.ops	= &kprobe_prog_ops,
	.type	= BPF_PROG_TYPE_KPROBE,
};

BPF_CALL_5(bpf_perf_event_output_tp, void *, tp_buff, struct bpf_map *, map,
	   u64, flags, void *, data, u64, size)
{
	struct pt_regs *regs = *(struct pt_regs **)tp_buff;

	/*
	 * r1 points to perf tracepoint buffer where first 8 bytes are hidden
	 * from bpf program and contain a pointer to 'struct pt_regs'. Fetch it
	 * from there and call the same bpf_perf_event_output() helper inline.
	 */
	return ____bpf_perf_event_output(regs, map, flags, data, size);
}

static const struct bpf_func_proto bpf_perf_event_output_proto_tp = {
	.func		= bpf_perf_event_output_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
	.arg4_type	= ARG_PTR_TO_STACK,
	.arg5_type	= ARG_CONST_STACK_SIZE,
};

BPF_CALL_3(bpf_get_stackid_tp, void *, tp_buff, struct bpf_map *, map,
	   u64, flags)
{
	struct pt_regs *regs = *(struct pt_regs **)tp_buff;

	/*
	 * Same comment as in bpf_perf_event_output_tp(), only that this time
	 * the other helper's function body cannot be inlined due to being
	 * external, thus we need to call raw helper function.
	 */
	return bpf_get_stackid((unsigned long) regs, (unsigned long) map,
			       flags, 0, 0);
}

static const struct bpf_func_proto bpf_get_stackid_proto_tp = {
	.func		= bpf_get_stackid_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
};

static const struct bpf_func_proto *tp_prog_func_proto(enum bpf_func_id func_id)
{
	switch (func_id) {
	case BPF_FUNC_perf_event_output:
		return &bpf_perf_event_output_proto_tp;
	case BPF_FUNC_get_stackid:
		return &bpf_get_stackid_proto_tp;
	default:
		return tracing_func_proto(func_id);
	}
}

static bool tp_prog_is_valid_access(int off, int size, enum bpf_access_type type,
				    enum bpf_reg_type *reg_type)
{
	if (off < sizeof(void *) || off >= PERF_MAX_TRACE_SIZE)
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0)
		return false;
	return true;
}

static const struct bpf_verifier_ops tracepoint_prog_ops = {
	.get_func_proto  = tp_prog_func_proto,
	.is_valid_access = tp_prog_is_valid_access,
};

static struct bpf_prog_type_list tracepoint_tl = {
	.ops	= &tracepoint_prog_ops,
	.type	= BPF_PROG_TYPE_TRACEPOINT,
};

static bool pe_prog_is_valid_access(int off, int size, enum bpf_access_type type,
				    enum bpf_reg_type *reg_type)
{
	if (off < 0 || off >= sizeof(struct bpf_perf_event_data))
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0)
		return false;
	if (off == offsetof(struct bpf_perf_event_data, sample_period)) {
		if (size != sizeof(u64))
			return false;
	} else {
		if (size != sizeof(long))
			return false;
	}
	return true;
}

static u32 pe_prog_convert_ctx_access(enum bpf_access_type type, int dst_reg,
				      int src_reg, int ctx_off,
				      struct bpf_insn *insn_buf,
				      struct bpf_prog *prog)
{
	struct bpf_insn *insn = insn_buf;

	switch (ctx_off) {
	case offsetof(struct bpf_perf_event_data, sample_period):
		BUILD_BUG_ON(FIELD_SIZEOF(struct perf_sample_data, period) != sizeof(u64));

		*insn++ = BPF_LDX_MEM(BPF_FIELD_SIZEOF(struct bpf_perf_event_data_kern,
						       data), dst_reg, src_reg,
				      offsetof(struct bpf_perf_event_data_kern, data));
		*insn++ = BPF_LDX_MEM(BPF_DW, dst_reg, dst_reg,
				      offsetof(struct perf_sample_data, period));
		break;
	default:
		*insn++ = BPF_LDX_MEM(BPF_FIELD_SIZEOF(struct bpf_perf_event_data_kern,
						       regs), dst_reg, src_reg,
				      offsetof(struct bpf_perf_event_data_kern, regs));
		*insn++ = BPF_LDX_MEM(BPF_SIZEOF(long), dst_reg, dst_reg, ctx_off);
		break;
	}

	return insn - insn_buf;
}

static const struct bpf_verifier_ops perf_event_prog_ops = {
	.get_func_proto		= tp_prog_func_proto,
	.is_valid_access	= pe_prog_is_valid_access,
	.convert_ctx_access	= pe_prog_convert_ctx_access,
};

static DEFINE_MUTEX(bpf_event_mutex);

int perf_event_attach_bpf_prog(struct perf_event *event,
			       struct bpf_prog *prog)
{
	struct bpf_prog_array __rcu *old_array;
	struct bpf_prog_array *new_array;
	int ret = -EEXIST;

	mutex_lock(&bpf_event_mutex);

	if (event->prog)
		goto out;

	old_array = rcu_dereference_protected(event->tp_event->prog_array,
					      lockdep_is_held(&bpf_event_mutex));
	ret = bpf_prog_array_copy(old_array, NULL, prog, &new_array);
	if (ret < 0)
		goto out;

	/* set the new array to event->tp_event and set event->prog */
	event->prog = prog;
	rcu_assign_pointer(event->tp_event->prog_array, new_array);
	bpf_prog_array_free(old_array);

out:
	mutex_unlock(&bpf_event_mutex);
	return ret;
}

void perf_event_detach_bpf_prog(struct perf_event *event)
{
	struct bpf_prog_array __rcu *old_array;
	struct bpf_prog_array *new_array;
	int ret;

	mutex_lock(&bpf_event_mutex);

	if (!event->prog)
		goto out;

	old_array = rcu_dereference_protected(event->tp_event->prog_array,
					      lockdep_is_held(&bpf_event_mutex));

	ret = bpf_prog_array_copy(old_array, event->prog, NULL, &new_array);
	if (ret < 0) {
		bpf_prog_array_delete_safe(old_array, event->prog);
	} else {
		rcu_assign_pointer(event->tp_event->prog_array, new_array);
		bpf_prog_array_free(old_array);
	}

	bpf_prog_put(event->prog);
	event->prog = NULL;

out:
	mutex_unlock(&bpf_event_mutex);
}

static struct bpf_prog_type_list perf_event_tl = {
	.ops	= &perf_event_prog_ops,
	.type	= BPF_PROG_TYPE_PERF_EVENT,
};

static int __init register_kprobe_prog_ops(void)
{
	bpf_register_prog_type(&kprobe_tl);
	return 0;
}
late_initcall(register_kprobe_prog_ops);
