/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Base From schedutil cpufreq_govenor  
 *
 * Backported By DenomSly For k4.14 Redmi Note 10 Pro 
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include "sched.h"
#include <linux/binfmts.h>
#include "tune.h"

#define SUGOV_KTHREAD_PRIORITY	50

/* Define default efficient frequencies for big and LITTLE cores */
static unsigned int default_efficient_freq_lp[] = {CONFIG_SCHEDHORIZON_DEFAULT_EFFICIENT_FREQ_LP};
static unsigned int default_efficient_freq_perf[] = {CONFIG_SCHEDHORIZON_DEFAULT_EFFICIENT_FREQ_HP};

/* Define default up delays for big and LITTLE cores */
static unsigned int default_up_delay_lp[] = {CONFIG_SCHEDHORIZON_DEFAULT_UP_DELAY_LP};
static unsigned int default_up_delay_perf[] = {CONFIG_SCHEDHORIZON_DEFAULT_UP_DELAY_HP};

struct sugov_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	unsigned int *efficient_freq;
	int nefficient_freq;
	unsigned int *up_delay;
	int nup_delay;
	int current_step;
};

struct sugov_policy {
	struct cpufreq_policy *policy;

	struct sugov_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;  /* For shared policies */
	u64 last_freq_update_time;
	s64 min_rate_limit_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	unsigned long first_hp_request_time;

	/* The next fields are only needed if fast switch cannot be used. */
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;

	bool need_freq_update;
};

struct sugov_cpu {
	struct update_util_data update_util;
	struct sugov_policy *sg_policy;

	u64 last_update;

	struct sched_walt_cpu_load walt_load;

	/* The fields below are only needed when sharing a policy. */
	unsigned long util;
	unsigned long max;
	unsigned int flags;
	unsigned int cpu;

	/* The field below is for single-CPU policies only. */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);
static unsigned int stale_ns;
static DEFINE_PER_CPU(struct sugov_tunables *, cached_tunables);

/************************ Governor internals ***********************/

static int match_nearest_efficient_step(int freq,int maxstep,int *freq_table)
{
    int i;

    for (i = 0; i < maxstep; i++) {
        if (freq_table[i] >= freq)
            break;
    }

    return i;
}

static void do_freq_limit(struct sugov_policy *sg_policy, unsigned int *freq)
{
    if (*freq > sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step] && !sg_policy->first_hp_request_time) {
	    /* First request */
	    *freq = sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step];
	    sg_policy->first_hp_request_time = jiffies;
	} else if (*freq < sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step]) {
	    /* It's already under current efficient frequency */
	    /* Goto a lower one */
	    sg_policy->tunables->current_step = match_nearest_efficient_step(*freq, sg_policy->tunables->nefficient_freq, sg_policy->tunables->efficient_freq);
	    sg_policy->first_hp_request_time = 0;
	} else if ((sg_policy->first_hp_request_time 
	   && time_before(jiffies, sg_policy->first_hp_request_time + msecs_to_jiffies(sg_policy->tunables->up_delay[sg_policy->tunables->current_step])))){
	    /* Restrict it */
	    *freq = sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step];
	} else if (sg_policy->tunables->current_step + 1 <= sg_policy->tunables->nefficient_freq - 1
	       && sg_policy->tunables->current_step + 1 <= sg_policy->tunables->nup_delay - 1) {
	       /* Unlock a higher efficient frequency */
	        sg_policy->tunables->current_step++;
	 		sg_policy->first_hp_request_time = jiffies;
	  		if (*freq > sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step])
	     			*freq = sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step];
		}
}

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		/*
		 * This happens when limits change, so forget the previous
		 * next_freq value and force an update.
		 */
		sg_policy->next_freq = UINT_MAX;
		return true;
	}

	/* No need to recalculate next freq for min_rate_limit_us
	 * at least. However we might still decide to further rate
	 * limit once frequency change direction is decided, according
	 * to the separate rate limits.
	 */

	delta_ns = time - sg_policy->last_freq_update_time;
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool sugov_up_down_rate_limit(struct sugov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;

 	delta_ns = time - sg_policy->last_freq_update_time;

	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
			return true;

	if (next_freq < sg_policy->next_freq &&
	    delta_ns < sg_policy->down_rate_delay_ns)
			return true;

	return false;
}

static void sugov_update_commit(struct sugov_policy *sg_policy, u64 time,
				unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;

	if (sg_policy->next_freq == next_freq)
		return;

	if (sugov_up_down_rate_limit(sg_policy, time, next_freq))
		return;

	if (sg_policy->next_freq > next_freq)
		next_freq = (sg_policy->next_freq + next_freq) >> 1;

	do_freq_limit(sg_policy, &next_freq);

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	if (policy->fast_switch_enabled) {
		next_freq = cpufreq_driver_fast_switch(policy, next_freq);
		if (next_freq == CPUFREQ_ENTRY_INVALID)
			return;

		policy->cur = next_freq;
		trace_cpu_frequency(next_freq, smp_processor_id());
	} else {
		sg_policy->work_in_progress = true;
	}
}

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: schedhorizon policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */
static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;

	freq = (freq + (freq >> 2)) * int_sqrt(util * 100 / max) / 10;
	trace_sugov_next_freq(policy->cpu, util, max, freq);

	if (freq == sg_policy->cached_raw_freq && sg_policy->next_freq != UINT_MAX)
		return sg_policy->next_freq;
	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void sugov_get_util(unsigned long *util, unsigned long *max, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long cfs_max;
	struct sugov_cpu *loadcpu = &per_cpu(sugov_cpu, cpu);

	cfs_max = arch_scale_cpu_capacity(NULL, cpu);

	*util = min(rq->cfs.avg.util_avg, cfs_max);
	*max = cfs_max;

	*util = boosted_cpu_util(cpu, &loadcpu->walt_load);
}

#ifdef CONFIG_NO_HZ_COMMON
static bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls();
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu) { return false; }
#endif /* CONFIG_NO_HZ_COMMON */

static void sugov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util, max;
	unsigned int next_f;
	bool busy;

	flags &= ~SCHED_CPUFREQ_RT_DL;

	if (flags & SCHED_CPUFREQ_PL)
		return;

	sg_cpu->last_update = time;

	if (!sugov_should_update_freq(sg_policy, time))
		return;

	busy = sugov_cpu_is_busy(sg_cpu);

	if (flags & SCHED_CPUFREQ_RT_DL) {
		next_f = policy->cpuinfo.max_freq;
	} else {
		sugov_get_util(&util, &max, sg_cpu->cpu);

		next_f = get_next_freq(sg_policy, util, max);
		/*
		 * Do not reduce the frequency if the CPU has not been idle
		 * recently, as the reduction is likely to be premature then.
		 */
		if (busy && next_f < sg_policy->next_freq)
			next_f = sg_policy->next_freq;
	}

	sugov_update_commit(sg_policy, time, next_f);
}

static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long j_util, j_max;
		
		if (j_sg_cpu->flags & SCHED_CPUFREQ_RT_DL)
			return policy->cpuinfo.max_freq;

		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}

	}

	return get_next_freq(sg_policy, util, max);
}

static void sugov_update_shared(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;

	if (flags & SCHED_CPUFREQ_PL)
		return;

	sugov_get_util(&util, &max, sg_cpu->cpu);

	flags &= ~SCHED_CPUFREQ_RT_DL;

	raw_spin_lock(&sg_policy->update_lock);

	sg_cpu->util = util;
	sg_cpu->max = max;
	sg_cpu->flags = flags;

	sg_cpu->last_update = time;

	if (sugov_should_update_freq(sg_policy, time)) {
		if (flags & SCHED_CPUFREQ_RT_DL)
			next_f = sg_policy->policy->cpuinfo.max_freq;
		else
			next_f = sugov_next_freq_shared(sg_cpu, time);

		sugov_update_commit(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_work(struct kthread_work *work)
{
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq,
				CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);

	sg_policy->work_in_progress = false;
}

static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sugov_policy, irq_work);

	/*
	 * For RT and deadline tasks, the schedhorizon governor shoots the
	 * frequency to maximum. Special care must be taken to ensure that this
	 * kthread doesn't result in the same behavior.
	 *
	 * This is (mostly) guaranteed by the work_in_progress flag. The flag is
	 * updated only at the end of the sugov_work() function and before that
	 * the schedhorizon governor rejects all other frequency scaling requests.
	 *
	 * There is a very rare case though, where the RT thread yields right
	 * after the work_in_progress flag is cleared. The effects of that are
	 * neglected for now.
	 */
	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

static unsigned int *resolve_data (const char *buf, int *num_ret,size_t count)
{
	const char *cp;
	unsigned int *output;
	int num = 1, i;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " ")))
		num++;

	output = kzalloc(num * sizeof(unsigned int), GFP_KERNEL);

	cp = buf;
	i = 0;
	while (i < num && cp-buf<count) {
		if (sscanf(cp, "%u", &output[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " ");
		if (!cp)
			break;
		cp++;
	}

	*num_ret = num;
	return output;

err_kfree:
	kfree(output);
	return NULL;

}

/************************** sysfs interface ************************/

static struct sugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_ns(struct sugov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
 	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
 
	return sprintf(buf, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t efficient_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int i;
	ssize_t ret = 0;

	for (i = 0; i < tunables->nefficient_freq; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->efficient_freq[i], " ");

	sprintf(buf + ret - 1, "\n");

	return ret;
}

static ssize_t up_delay_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int i;
	ssize_t ret = 0;

	for (i = 0; i < tunables->nup_delay; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->up_delay[i], " ");

	sprintf(buf + ret - 1, "\n");

	return ret;
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;
    
    /* Apply init protection, else values will get overwritten */
	if (task_is_booster(current))
		return count;
		
	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(sg_policy);
	}

	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;
    
    /* Apply init protection, else values will get overwritten */
	if (task_is_booster(current))
		return count;
		
	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->down_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(sg_policy);
	}

	return count;
}

static ssize_t efficient_freq_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int new_num;
	unsigned int *new_efficient_freq = NULL, *old;

	new_efficient_freq = resolve_data(buf, &new_num, count);

	if (new_efficient_freq) {
	    old = tunables->efficient_freq;
	    tunables->efficient_freq = new_efficient_freq;
	    tunables->nefficient_freq = new_num;
	    tunables->current_step = 0;
	    if (old != default_efficient_freq_lp
	     && old != default_efficient_freq_perf)
	        kfree(old);
	}

	return count;
}

static ssize_t up_delay_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int new_num;
	unsigned int *new_up_delay = NULL, *old;

	new_up_delay = resolve_data(buf, &new_num, count);

	if (new_up_delay) {
	    old = tunables->up_delay;
	    tunables->up_delay = new_up_delay;
	    tunables->nup_delay = new_num;
	    tunables->current_step = 0;
	    if (old != default_up_delay_lp
	     && old != default_up_delay_perf)
	        kfree(old);
	}

	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);
static struct governor_attr efficient_freq = __ATTR_RW(efficient_freq);
static struct governor_attr up_delay = __ATTR_RW(up_delay);

static struct attribute *sugov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&efficient_freq.attr,
	&up_delay.attr,
	NULL
};

static void sugov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_sugov_tunables(attr_set));
}

static struct kobj_type sugov_tunables_ktype = {
	.default_attrs = sugov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor schedhorizon_gov;

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static inline void sugov_policy_free(struct sugov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int sugov_kthread_create(struct sugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, sugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void sugov_kthread_stop(struct sugov_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void sugov_tunables_save(struct cpufreq_policy *policy,
		struct sugov_tunables *tunables)
{
	int cpu;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!have_governor_per_policy())
		return;

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached) {
			pr_warn("Couldn't allocate tunables for caching\n");
			return;
		}
		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}

	cached->up_rate_limit_us = tunables->up_rate_limit_us;
	cached->down_rate_limit_us = tunables->down_rate_limit_us;
	cached->efficient_freq = tunables->efficient_freq;
	cached->up_delay = tunables->up_delay;
	cached->nefficient_freq = tunables->nefficient_freq;
	cached->nup_delay = tunables->nup_delay;
}

static void sugov_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

static void sugov_tunables_restore(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached)
		return;

	tunables->up_rate_limit_us = cached->up_rate_limit_us;
	tunables->down_rate_limit_us = cached->down_rate_limit_us;
	tunables->efficient_freq = cached->efficient_freq;
	tunables->up_delay = cached->up_delay;
	tunables->nefficient_freq = cached->nefficient_freq;
	tunables->nup_delay = cached->nup_delay;
}

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = sugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	/*
	 * NOTE:
	 * intializing up_rate/down_rate to 0 explicitly in kernel
	 * since WALT expects so by default.
	 */
    
    tunables->up_rate_limit_us =
				cpufreq_policy_transition_delay_us(policy);
	tunables->down_rate_limit_us =
				cpufreq_policy_transition_delay_us(policy);
		
    if (cpumask_test_cpu(policy->cpu, cpu_perf_mask)) {
		tunables->up_rate_limit_us =
					CONFIG_SCHEDHORIZON_DEFAULT_UP_RATE_LIMIT_HP;
		tunables->down_rate_limit_us =
					CONFIG_SCHEDHORIZON_DEFAULT_DOWN_RATE_LIMIT_HP;
	}
        	
	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask)) {
		tunables->up_rate_limit_us =
					CONFIG_SCHEDHORIZON_DEFAULT_UP_RATE_LIMIT_LP;
		tunables->down_rate_limit_us =
					CONFIG_SCHEDHORIZON_DEFAULT_DOWN_RATE_LIMIT_LP;
	}
	
	if (cpumask_test_cpu(sg_policy->policy->cpu, cpu_lp_mask)) {
		tunables->efficient_freq = default_efficient_freq_lp;
    	tunables->nefficient_freq = ARRAY_SIZE(default_efficient_freq_lp);
		tunables->up_delay = default_up_delay_lp;
		tunables->nup_delay = ARRAY_SIZE(default_up_delay_lp);
	} else if (cpumask_test_cpu(sg_policy->policy->cpu, cpu_perf_mask)) {
		tunables->efficient_freq = default_efficient_freq_perf;
    	tunables->nefficient_freq = ARRAY_SIZE(default_efficient_freq_perf);
		tunables->up_delay = default_up_delay_perf;
		tunables->nup_delay = ARRAY_SIZE(default_up_delay_perf);
	}

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;
	stale_ns = sched_ravg_window + (sched_ravg_window >> 3);

	sugov_tunables_restore(policy);

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   schedhorizon_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	policy->governor_data = NULL;
	sugov_clear_global_tunables();

stop_kthread:
	sugov_kthread_stop(sg_policy);

free_sg_policy:
	mutex_unlock(&global_tunables_lock);

	sugov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		sugov_tunables_save(policy, tunables);
		sugov_clear_global_tunables();
	}

	mutex_unlock(&global_tunables_lock);

	sugov_kthread_stop(sg_policy);
	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_ns(sg_policy);
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq = UINT_MAX;
	sg_policy->work_in_progress = false;
	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->sg_policy = sg_policy;
		sg_cpu->cpu = cpu;
		sg_cpu->flags = SCHED_CPUFREQ_RT;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							sugov_update_shared :
							sugov_update_single);
	}
	return 0;
}

static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_sched();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}

	sg_policy->need_freq_update = true;
}

static struct cpufreq_governor schedhorizon_gov = {
	.name = "schedhorizon",
	.owner = THIS_MODULE,
	.init = sugov_init,
	.exit = sugov_exit,
	.start = sugov_start,
	.stop = sugov_stop,
	.limits = sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDHORIZON
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedhorizon_gov;
}
#endif

cpufreq_governor_init(schedhorizon_gov);