/**
 * Memory bandwidth controller for multi-core systems
 *
 * Copyright (C) 2013  Heechul Yun <heechul@illinois.edu>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define DEBUG(x)
#define DEBUG_RECLAIM(x)
#define DEBUG_USER(x)
#define DEBUG_PROFILE(x) x

/**************************************************************************
 * Included Files
 **************************************************************************/
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/smp.h> /* IPI calls */
#include <linux/irq_work.h>
#include <linux/hardirq.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/interrupt.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 13, 0)
#  include <linux/sched/types.h>
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#  include <linux/sched/rt.h>
#endif
#include <linux/sched.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NCPUS 64
#define CACHE_LINE_SIZE 64
#define BUF_SIZE 256
#define PREDICTOR 1  /* 0 - used, 1 - ewma(a=1/2), 2 - ewma(a=1/4) */

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 10, 0) // somewhere between 4.4-4.10
#  define TM_NS(x) (x)
#else
#  define TM_NS(x) (x).tv64
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/
struct event_info{

	char name[20];
	char counter[20];
	int counter_val;
	int mb_budget;
	int budget;

	u64 used_budget;
	u64 assigned_budget;
	int limit;
	int cur_budget;
	u64 old_val;
	int prev_throttle_error;
	struct irq_work pending;
	struct perf_event *event;
	int used[3];

	struct event_info *next;
};

struct memstat{
	u64 used_budget;         /* used budget*/
	u64 assigned_budget;
	u64 throttled_time_ns;
	int throttled;           /* throttled period count */
	u64 throttled_error;     /* throttled & error */
	int throttled_error_dist[10]; /* pct distribution */
	int exclusive;           /* exclusive period count */
	u64 exclusive_ns;        /* exclusive mode real-time duration */
	u64 exclusive_bw;        /* exclusive mode used bandwidth */
};

/* percpu info */
struct core_info {
	/* user configurations */
	int budget;              /* assigned budget */
	int limit;               /* limit mode (exclusive to weight)*/

	/* for control logic */
	int cur_budget;          /* currently available budget */

	struct task_struct * throttled_task;

	ktime_t throttled_time;  /* absolute time when throttled */

	u64 old_val;             /* hold previous counter value */
	int prev_throttle_error; /* check whether there was throttle error in
				    the previous period */

	int exclusive_mode;      /* 1 - if in exclusive mode */
	ktime_t exclusive_time;  /* time when exclusive mode begins */

	struct irq_work	pending; /* delayed work for NMIs */
	struct perf_event *event;/* PMC: LLC misses */
	struct task_struct *throttle_thread;  /* forced throttle idle thread */
	wait_queue_head_t throttle_evt; /* throttle wait queue */

	/* statistics */
	struct memstat overall;  /* stat for overall periods. reset by user */
	int used[3];             /* EWMA memory load */
	long period_cnt;         /* active periods count */

	struct event_info *head;
	struct event_info *tail;
};

/* global info */
struct memguard_info {
	int master;
	ktime_t period_in_ktime;
	ktime_t cur_period_start;
	int start_tick;
	long period_cnt;
	cpumask_var_t throttle_mask;
	cpumask_var_t active_mask;
	atomic_t wsum;
	struct hrtimer hr_timer;
};


/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct memguard_info memguard_info;
static struct core_info __percpu *core_info;

static char *g_hw_type = "";
static int g_period_us = 1000;
static int g_use_exclusive = 0;
static int g_budget_mb[MAX_NCPUS];

static struct dentry *memguard_dir;

/* copied from kernel/sched/sched.h */
static const int prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

static const u32 prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360437,
 /* -10 */    449829,    563644,    704093,    875809,   1099582,
 /*  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
 /*   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
 /*   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
 /*  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/
extern void register_get_cpi(int (*func_ptr)(void));

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/
static void period_timer_callback_slave(void *info);
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer);
static void memguard_process_overflow(struct irq_work *entry);
static int throttle_thread(void *arg);
static void memguard_on_each_cpu_mask(const struct cpumask *mask,
				      smp_call_func_t func,
				      void *info, bool wait);

/**************************************************************************
 * Module parameters
 **************************************************************************/

module_param(g_hw_type, charp,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_hw_type, "hardware type");

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");

/**************************************************************************
 * Module main code
 **************************************************************************/
/* similar to on_each_cpu_mask(), but this must be called with IRQ disabled */
static void memguard_on_each_cpu_mask(const struct cpumask *mask,
				      smp_call_func_t func,
				      void *info, bool wait)
{
	int cpu = smp_processor_id();
	smp_call_function_many(mask, func, info, wait);
	if (cpumask_test_cpu(cpu, mask)) {
		func(info);
	}
}

/** convert MB/s to #of events (i.e., LLC miss counts) per 1ms */
static inline u64 convert_mb_to_events(int mb)
{
	return div64_u64((u64)mb*1024*1024,
			 CACHE_LINE_SIZE* (1000000/g_period_us));
}
static inline int convert_events_to_mb(u64 events)
{
	int divisor = g_period_us*1024*1024;
	int mb = div64_u64(events*CACHE_LINE_SIZE*1000000 + (divisor-1), divisor);
	return mb;
}

static inline void print_current_context(void)
{
	trace_printk("in_interrupt(%ld)(hard(%ld),softirq(%d)"
		     ",in_nmi(%d)),irqs_disabled(%d)\n",
		     in_interrupt(), in_irq(), (int)in_softirq(),
		     (int)in_nmi(), (int)irqs_disabled());
}

/** read current counter value. */
static inline u64 perf_event_count(struct perf_event *event)
{
	return local64_read(&event->count) +
		atomic64_read(&event->child_count);
}

/** return used event in the current period */
static inline u64 memguard_event_used(struct core_info *cinfo)
{
	return perf_event_count(cinfo->event) - cinfo->old_val;
}

/** return used event in the current period */
static inline u64 memguard_event_info_used(struct event_info *ei)
{
	return perf_event_count(ei->event) - ei->old_val;
}

static void print_core_info(int cpu, struct core_info *cinfo)
{
	pr_info("CPU%d: budget: %d, cur_budget: %d, period: %ld\n",
	       cpu, cinfo->budget, cinfo->cur_budget, cinfo->period_cnt);
}

/**
 * update per-core usage statistics
 */
void update_statistics(struct core_info *cinfo)
{
	/* counter must be stopped by now. */
	s64 new;
	int used;

	struct event_info* temp = cinfo->head;
	while(temp != NULL)
	{
		new = perf_event_count(temp->event);
		used = (int)(new - temp->old_val);

		temp->old_val = new;
		temp->used_budget += used;
		temp->assigned_budget += temp->budget;

		temp->used[0] = used;
		temp->used[1] = (temp->used[1] * (2 - 1) + used) >> 1;
		temp->used[2] = (temp->used[2] * (4 - 1) + used) >> 2;

		if(temp->prev_throttle_error && used < temp->budget)
		{
			int diff = temp->budget - used;
			int idx;
			cinfo->overall.throttled_error ++; // += diff;
			BUG_ON(temp->budget == 0);
			idx = (int)(diff * 10 / temp->budget);
			cinfo->overall.throttled_error_dist[idx]++;
			trace_printk("ERR: throttled_error: %d < %d\n", used, temp->budget);
			/* compensation for error to catch-up*/
			temp->used[PREDICTOR] = temp->budget + diff;
		}
		temp->prev_throttle_error = 0;

		temp = temp->next;
	}

	new = perf_event_count(cinfo->event);
	used = (int)(new - cinfo->old_val);

	cinfo->old_val = new;
	cinfo->overall.used_budget += used;
	cinfo->overall.assigned_budget += cinfo->budget;

	/* EWMA filtered per-core usage statistics */
	cinfo->used[0] = used;
	cinfo->used[1] = (cinfo->used[1] * (2-1) + used) >> 1;
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->used[2] = (cinfo->used[2] * (4-1) + used) >> 2;
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */

	/* core is currently throttled. */
	if (cinfo->throttled_task) {
		cinfo->overall.throttled_time_ns +=
			(TM_NS(ktime_get()) - TM_NS(cinfo->throttled_time));
		cinfo->overall.throttled++;
	}

	/* throttling error condition:
	   I was too aggressive in giving up "unsed" budget */
	if (cinfo->prev_throttle_error && used < cinfo->budget) {
		int diff = cinfo->budget - used;
		int idx;
		cinfo->overall.throttled_error ++; // += diff;
		BUG_ON(cinfo->budget == 0);
		idx = (int)(diff * 10 / cinfo->budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: throttled_error: %d < %d\n", used, cinfo->budget);
		/* compensation for error to catch-up*/
		cinfo->used[PREDICTOR] = cinfo->budget + diff;
	}
	cinfo->prev_throttle_error = 0;

	/* I was the lucky guy who used the DRAM exclusively */
	if (cinfo->exclusive_mode) {
		u64 exclusive_ns, exclusive_bw;

		/* used time */
		exclusive_ns = (TM_NS(ktime_get()) -
				TM_NS(cinfo->exclusive_time));

		/* used bw */
		exclusive_bw = (cinfo->used[0] - cinfo->budget);

		cinfo->overall.exclusive_ns += exclusive_ns;
		cinfo->overall.exclusive_bw += exclusive_bw;
		cinfo->exclusive_mode = 0;
		cinfo->overall.exclusive++;
	}
	DEBUG_PROFILE(trace_printk("%lld %d %p CPU%d org: %d cur: %d period: %ld\n",
			   new, used, cinfo->throttled_task,
			   smp_processor_id(),
			   cinfo->budget,
			   cinfo->cur_budget,
			   cinfo->period_cnt));
}

/**
 * budget is used up. PMU generate an interrupt
 * this run in hardirq, nmi context with irq disabled
 */
static void event_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->pending);
}

static void event_info_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct event_info* temp = cinfo->head;
	BUG_ON(!cinfo);
	while(temp != NULL)
	{
		if(temp->event->attr.config == event->attr.config)
		{
			irq_work_queue(&temp->pending);
			break;
		}
		temp = temp->next;
	}
}


/**
 * called by process_overflow
 */
static void __unthrottle_core(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	if (cinfo->throttled_task) {
		cinfo->exclusive_mode = 1;
		cinfo->exclusive_time = ktime_get();

		cinfo->throttled_task = NULL;
		DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
	}
}

static void __newperiod(void *info)
{
	long period = (long)info;
	ktime_t start = ktime_get();
	struct memguard_info *global = &memguard_info;

	if (period == global->period_cnt) {
		ktime_t new_expire = ktime_add(start, global->period_in_ktime);
		long new_period = ++global->period_cnt;

		/* arrived before timer interrupt is called */
		hrtimer_start_range_ns(&global->hr_timer, new_expire,
				       0, HRTIMER_MODE_ABS_PINNED);
		DEBUG(trace_printk("begin new period\n"));

		on_each_cpu(period_timer_callback_slave, (void *)new_period, 0);
	}
}

/**
 * memory overflow handler.
 * must not be executed in NMI context. but in hard irq context
 */
static void memguard_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	ktime_t start;
	s64 budget_used;

	start = ktime_get();

	BUG_ON(in_nmi() || !in_irq());

	budget_used = memguard_event_used(cinfo);

	/* erroneous overflow, that could have happend before period timer
	   stop the pmu */
	if (budget_used < cinfo->cur_budget) {
		trace_printk("ERR: overflow in timer. used %lld < cur_budget %d. ignore\n",
			     budget_used, cinfo->cur_budget);
		return;
	}

	/* no more overflow interrupt */
	local64_set(&cinfo->event->hw.period_left, 0xfffffff);

	/* check if we donated too much */
	if (budget_used < cinfo->budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_throttle_error = 1;
	}

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("ERR: not active\n");
		return;
	} else if (global->period_cnt != cinfo->period_cnt) {
		trace_printk("ERR: global(%ld) != local(%ld) period mismatch\n",
			     global->period_cnt, cinfo->period_cnt);
		return;
	}

	/* we are going to be throttled */
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	smp_mb(); // w -> r ordering of the local cpu.
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		if (g_use_exclusive == 1) {
			/* algorithm 1: last one get all remaining time */
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 2) {
			/* algorithm 2: wakeup all (i.e., non regulation) */
			smp_call_function_many(global->throttle_mask, __unthrottle_core, NULL, 0);
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			cinfo->throttled_task = NULL;
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 5) {
			smp_call_function_single(global->master, __newperiod,
					  (void *)cinfo->period_cnt, 0);
			return;
		} else if (g_use_exclusive > 5) {
			trace_printk("ERR: Unsupported exclusive mode %d\n",
				     g_use_exclusive);
			return;
		} else if (g_use_exclusive != 0 &&
			   cpumask_weight(global->active_mask) == 1) {
			trace_printk("ERR: don't throttle one active core\n");
			return;
		}
	}

	//if (cinfo->prev_throttle_error)
	//	return;
	/*
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec.\n",
				   TM_NS(ktime_get()) - TM_NS(start)));

	/* wake-up throttle task */
	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

	WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
	wake_up_interruptible(&cinfo->throttle_evt);
}

static void memguard_event_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	struct event_info *temp = cinfo->head;
	int no_throttle_error = 1;
	ktime_t start = ktime_get();
	s64 budget_used;

	while(temp != NULL)
	{
		BUG_ON(in_nmi() || !in_irq());

		budget_used = memguard_event_info_used(temp);

		if (budget_used < temp->cur_budget) {
			trace_printk("ERR: overflow in timer. used %lld < cur_budget %d. ignore\n",
				     budget_used, temp->cur_budget);
			break;
		}

		// no more overflow interrupt
		local64_set(&temp->event->hw.period_left, 0xfffffff);

		// check if we donated too much
		if (budget_used < temp->budget) {
			trace_printk("ERR: throttling error\n");
			temp->prev_throttle_error = 1;
		}
		else
		{
			//printk(KERN_INFO "EVENT %s OVERFLOW\n", temp->name);
			no_throttle_error = 0;
		}

		temp = temp->next;
	}

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("ERR: not active\n");
		return;
	} else if (global->period_cnt != cinfo->period_cnt) {
		trace_printk("ERR: global(%ld) != local(%ld) period mismatch\n",
			     global->period_cnt, cinfo->period_cnt);
		return;
	}

	/* we are going to be throttled */
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	smp_mb(); // w -> r ordering of the local cpu.
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		if (g_use_exclusive == 1) {
			/* algorithm 1: last one get all remaining time */
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 2) {
			/* algorithm 2: wakeup all (i.e., non regulation) */
			smp_call_function_many(global->throttle_mask, __unthrottle_core, NULL, 0);
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			cinfo->throttled_task = NULL;
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 5) {
			smp_call_function_single(global->master, __newperiod,
					  (void *)cinfo->period_cnt, 0);
			return;
		} else if (g_use_exclusive > 5) {
			trace_printk("ERR: Unsupported exclusive mode %d\n",
				     g_use_exclusive);
			return;
		} else if (g_use_exclusive != 0 &&
			   cpumask_weight(global->active_mask) == 1) {
			trace_printk("ERR: don't throttle one active core\n");
			return;
		}
	}

	//if (no_throttle_error)
	//	return;
	/*
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec.\n",
				   TM_NS(ktime_get()) - TM_NS(start)));

	/* wake-up throttle task */
	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

	WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
	wake_up_interruptible(&cinfo->throttle_evt);
}

/**
 * per-core period processing
 *
 * called by scheduler tick to replenish budget and unthrottle if needed
 * run in interrupt context (irq disabled)
 */

/*
 * period_timer algorithm:
 *	excess = 0;
 *	if predict < budget:
 *	   excess = budget - predict;
 *	   global += excess
 *	set interrupt at (budget - excess)
 */
static void period_timer_callback_slave(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct event_info *temp = cinfo->head;
	struct memguard_info *global = &memguard_info;
	struct task_struct *target;
	long new_period = (long)info;
	int cpu = smp_processor_id();

	/* must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled());
	WARN_ON_ONCE(!in_irq());

	if (new_period <= cinfo->period_cnt) {
		trace_printk("ERR: new_period(%ld) <= cinfo->period_cnt(%ld)\n",
			     new_period, cinfo->period_cnt);
		return;
	}

	/* assign local period */
	cinfo->period_cnt = new_period;

	/* stop counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);

	/* I'm actively participating */
	cpumask_clear_cpu(cpu, global->throttle_mask);
	smp_mb();
	cpumask_set_cpu(cpu, global->active_mask);

	/* unthrottle tasks (if any) */
	if (cinfo->throttled_task)
		target = (struct task_struct *)cinfo->throttled_task;
	else
		target = current;
	cinfo->throttled_task = NULL;

	DEBUG(trace_printk("%p|New period %ld. global->budget=%d\n",
			   cinfo->throttled_task,
			   cinfo->period_cnt, global->budget));

	/* update statistics. */
	update_statistics(cinfo);

	/* new budget assignment from user */
	if (cinfo->limit > 0) {
		/* limit mode */
		cinfo->budget = cinfo->limit;
	}

	/* budget can't be zero? */
	cinfo->budget = max(cinfo->budget, 1);

	if (cinfo->event->hw.sample_period != cinfo->budget) {
		/* new budget is assigned */
		DEBUG(trace_printk("MSG: new budget %d is assigned\n",
				   cinfo->budget));
		cinfo->event->hw.sample_period = cinfo->budget;
	}

	/* per-task donation policy */
	cinfo->cur_budget = cinfo->budget;

	/* setup an interrupt */
	cinfo->cur_budget = max(1, cinfo->cur_budget);
	local64_set(&cinfo->event->hw.period_left, cinfo->cur_budget);

	/* enable performance counter */
	cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);

	while(temp != NULL)
	{
		temp->event->pmu->stop(temp->event, PERF_EF_UPDATE);

		if(temp->limit > 0)
		{
			temp->budget = temp->limit;
		}

		temp->budget = max(temp->budget, 1);

		if(temp->event->hw.sample_period != temp->budget)
		{
			DEBUG(trace_printk("MSG: new budget %d is assigned\n",
					   cinfo->budget));
			temp->event->hw.sample_period = temp->budget;
		}

		temp->cur_budget = temp->budget;

		temp->cur_budget = max(1, temp->cur_budget);
		local64_set(&temp->event->hw.period_left, temp->cur_budget);

		temp->event->pmu->start(temp->event, PERF_EF_RELOAD);

		temp = temp->next;
	}
}

/**
 *   called while cpu_base->lock is held by hrtimer_interrupt()
 */
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer)
{
	struct memguard_info *global = &memguard_info;

	ktime_t now;
	int orun;
	long new_period;

	now = timer->base->get_time();
	global->cur_period_start = now;
        DEBUG(trace_printk("master begin\n"));
	BUG_ON(smp_processor_id() != global->master);

	orun = hrtimer_forward(timer, now, global->period_in_ktime);
	if (orun == 0)
		return HRTIMER_RESTART;

	global->period_cnt += orun;
	new_period = global->period_cnt;

	if (orun > 1)
		trace_printk("ERR: timer overrun %d at period %ld\n",
			    orun, new_period);
	memguard_on_each_cpu_mask(global->active_mask,
		period_timer_callback_slave, (void *)new_period, 0);

	DEBUG(trace_printk("master end\n"));
	return HRTIMER_RESTART;
}

static struct perf_event *init_counter(int cpu, int budget)
{
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		/* use generalized hardware abstraction */
		.type           = PERF_TYPE_HARDWARE,
		.config         = PERF_COUNT_HW_CACHE_MISSES,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.exclude_kernel = 1,   /* TODO: 1 mean, no kernel mode counting */
	};

	if (!strcmp(g_hw_type, "core2")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x7024; /* 7024 - incl. prefetch
							       5024 - only prefetch
							       4024 - excl. prefetch */
	} else if (!strcmp(g_hw_type, "snb")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x08b0; /* 08b0 - incl. prefetch */
	} else if (!strcmp(g_hw_type, "armv7")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x17; /* Level 2 data cache refill */
	} else if (!strcmp(g_hw_type, "soft")) {
		sched_perf_hw_attr.type           = PERF_TYPE_SOFTWARE;
		sched_perf_hw_attr.config         = PERF_COUNT_SW_CPU_CLOCK;
	}

	/* select based on requested event type */
	sched_perf_hw_attr.sample_period = budget;

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		event_overflow_callback
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
		,NULL
#endif
		);

	if (!event)
		return NULL;

	if (IS_ERR(event)) {
		/* vary the KERN level based on the returned errno */
		if (PTR_ERR(event) == -EOPNOTSUPP)
			pr_info("cpu%d. not supported\n", cpu);
		else if (PTR_ERR(event) == -ENOENT)
			pr_info("cpu%d. not h/w event\n", cpu);
		else
			pr_err("cpu%d. unable to create perf event: %ld\n",
			       cpu, PTR_ERR(event));
		return NULL;
	}

	/* This is needed since 4.1? */
	perf_event_enable(event);

	/* success path */
	pr_info("cpu%d enabled counter.\n", cpu);

	return event;
}

static struct perf_event *init_event_counter(int cpu, int counter, int budget)
{
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		/* use generalized hardware abstraction */
		.type           = PERF_TYPE_RAW,
		.config         = counter,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.exclude_kernel = 1,   /* TODO: 1 mean, no kernel mode counting */
	};

	pr_info("%llx\n", sched_perf_hw_attr.config);

	/* select based on requested event type */
	sched_perf_hw_attr.sample_period = budget;

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		event_info_overflow_callback
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
		,NULL
#endif
		);

	if (!event)
		return NULL;

	if (IS_ERR(event)) {
		/* vary the KERN level based on the returned errno */
		if (PTR_ERR(event) == -EOPNOTSUPP)
			pr_info("cpu%d. not supported\n", cpu);
		else if (PTR_ERR(event) == -ENOENT)
			pr_info("cpu%d. not h/w event\n", cpu);
		else
			pr_err("cpu%d. unable to create perf event: %ld\n",
			       cpu, PTR_ERR(event));
		return NULL;
	}

	/* This is needed since 4.1? */
	perf_event_enable(event);

	/* success path */
	pr_info("event cpu%d enabled counter.\n", cpu);

	return event;
}

static struct event_info* create_event_info(int cpu, char name[20], char counter[20], int budget)
{
	long temp_counter;
	struct event_info *new = kmalloc(sizeof(struct event_info), GFP_KERNEL);
	int temp_budget = convert_mb_to_events(budget);
	new->next = NULL;
	strcpy(new->name, name);
	new->mb_budget = budget;
	strcpy(new->counter, counter);
	if(!kstrtol(counter, 0, &temp_counter)) {}
	new->counter_val = (int)temp_counter;	//sscanf(name, "%xu", &new->counter_val);

	pr_info("Created event_info for cpu %i %s %i\n", cpu, new->counter, new->counter_val);

	new->event = init_event_counter(cpu, new->counter_val, temp_budget);

	new->budget = new->limit = new->event->hw.sample_period;

	new->used[0] = new->used[1] = new->used[2] = new->budget;
	new->cur_budget = new->budget;
	new->used_budget = 0;
	new->assigned_budget = 0;

	init_irq_work(&new->pending, memguard_event_overflow);

	return new;
}

static void __disable_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->event);

	/* stop the counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
	cinfo->event->pmu->del(cinfo->event, 0);

	pr_info("LLC bandwidth throttling disabled\n");
}

static void disable_counters(void)
{
	on_each_cpu(__disable_counter, NULL, 0);
}


static void __start_counter(void* info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	cinfo->event->pmu->add(cinfo->event, PERF_EF_START);
}

static void start_counters(void)
{
	on_each_cpu(__start_counter, NULL, 0);
}

/**************************************************************************
 * Local Functions
 **************************************************************************/

static ssize_t memguard_control_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;

	if (!strncmp(p, "exclusive ", 10))
		sscanf(p+10, "%d", &g_use_exclusive);
	else
		pr_info("ERROR: %s\n", p);
	return cnt;
}

static int memguard_control_show(struct seq_file *m, void *v)
{
	struct memguard_info *global = &memguard_info;
	char buf[BUF_SIZE];
	seq_printf(m, "exclusive: %d\n", g_use_exclusive);
	cpumap_print_to_pagebuf(1, buf, global->active_mask);
	seq_printf(m, "active: %s\n", buf);
	cpumap_print_to_pagebuf(1, buf, global->throttle_mask);
	seq_printf(m, "throttle: %s\n", buf);
	return 0;
}

static int memguard_control_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_control_show, NULL);
}

static const struct file_operations memguard_control_fops = {
	.open		= memguard_control_open,
	.write          = memguard_control_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static void __update_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);

	if ((unsigned long)info == 0) {
		pr_info("ERR: Requested budget is zero\n");
		return;
	}
	cinfo->limit = (unsigned long)info;
	DEBUG_USER(trace_printk("MSG: New budget of Core%d is %d\n",
				smp_processor_id(), cinfo->budget));

}

static ssize_t memguard_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i;
	int use_mb = 0;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;

	if (!strncmp(p, "mb ", 3)) {
		use_mb = 1;
		p+=3;
	}
	for_each_online_cpu(i) {
		int input;
		unsigned long events;
		sscanf(p, "%d", &input);
		if (input == 0) {
			pr_err("ERR: CPU%d: input is zero: %s.\n",i, p);
			continue;
		}
		if (use_mb)
			events = (unsigned long)convert_mb_to_events(input);
		else
			events = input;

		pr_info("CPU%d: New budget=%ld (%d %s)\n", i,
			events, input, (use_mb)?"MB/s": "events");
		smp_call_function_single(i, __update_budget,
					 (void *)events, 0);

		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	return cnt;
}

static int memguard_limit_show(struct seq_file *m, void *v)
{
	int i, cpu;
	cpu = get_cpu();

	seq_printf(m, "cpu  |budget (MB/s,pct,weight)\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->limit > 0)
			budget = cinfo->limit;

		WARN_ON_ONCE(budget == 0);
		seq_printf(m, "CPU%d: %d (%dMB/s)\n",
				   i, budget,
				   convert_events_to_mb(budget));
	}

	put_cpu();
	return 0;
}

static int memguard_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_limit_show, NULL);
}

static const struct file_operations memguard_limit_fops = {
	.open		= memguard_limit_open,
	.write          = memguard_limit_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};



/**
 * Display usage statistics
 *
 * TODO: use IPI
 */
static int memguard_usage_show(struct seq_file *m, void *v)
{
	int i, j;

	/* current utilization */
	for (j = 0; j < 3; j++) {
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			u64 budget, used, util;

			budget = cinfo->budget;
			used = cinfo->used[j];
			util = div64_u64(used * 100, (budget) ? budget : 1);
			seq_printf(m, "%llu ", util);
		}
		seq_printf(m, "\n");
	}
	seq_printf(m, "<overall>----\n");

	/* overall utilization
	   WARN: assume budget did not changed */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		u64 total_budget, total_used, result;

		total_budget = cinfo->overall.assigned_budget;
		total_used   = cinfo->overall.used_budget;
		result       = div64_u64(total_used * 100,
					 (total_budget) ? total_budget : 1 );
		seq_printf(m, "%lld ", result);
	}
	return 0;
}

static int memguard_usage_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_usage_show, NULL);
}

static const struct file_operations memguard_usage_fops = {
	.open		= memguard_usage_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static ssize_t events_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
    /*char buf[BUF_SIZE];
	char *p = buf;
    int event_budget;
    char event_name[20],event_counter[20];
    struct event_node *new = kmalloc(sizeof(struct event_node), GFP_KERNEL);
    struct event_node *temp = events_head;
    int i;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;
    if (!strncmp(p, "add ", 4)) {
		p+=4;

        sscanf(p, "%s %s %d", event_name, event_counter, &event_budget);
        if(event_name == NULL || event_counter == NULL || event_budget == 0)
        {
            pr_err("ERR: Too few event details\n");
            kfree(new);
            return cnt;
        }


        strcpy(new->ev.event_name, event_name);
        strcpy(new->ev.event_counter, event_counter);
        new->ev.event_budget = event_budget;

        get_online_cpus();
        for_each_online_cpu(i) {
            int budget;
            new->ev.event_cinfo[i] = per_cpu_ptr(core_info, i);

            budget = convert_mb_to_events(new->ev.event_budget);
            pr_info("budget[%d] = %d (%d MB)\n", i, budget, new->ev.event_budget);

            //memset(new->ev.event_cinfo[i], 0, sizeof(struct core_info));

            new->ev.event_cinfo[i]->event = init_event_counter(i, new);
            if (!new->ev.event_cinfo[i]->event)
                break;

            new->ev.event_cinfo[i]->budget = new->ev.event_cinfo[i]->limit = new->ev.event_cinfo[i]->event->hw.sample_period;

            new->ev.event_cinfo[i]->throttled_task = NULL;

            init_waitqueue_head(&new->ev.event_cinfo[i]->throttle_evt);

            new->ev.event_cinfo[i]->period_cnt = 0;
            new->ev.event_cinfo[i]->used[0] = new->ev.event_cinfo[i]->used[1] = new->ev.event_cinfo[i]->used[2] =
                new->ev.event_cinfo[i]->budget;
            new->ev.event_cinfo[i]->cur_budget = new->ev.event_cinfo[i]->budget;
            new->ev.event_cinfo[i]->overall.used_budget = 0;
            new->ev.event_cinfo[i]->overall.assigned_budget = 0;
            new->ev.event_cinfo[i]->overall.throttled_time_ns = 0;
            new->ev.event_cinfo[i]->overall.throttled = 0;
            new->ev.event_cinfo[i]->overall.throttled_error = 0;

            memset(new->ev.event_cinfo[i]->overall.throttled_error_dist, 0, sizeof(int)*10);
            new->ev.event_cinfo[i]->throttled_time = ktime_set(0,0);

            print_core_info(smp_processor_id(), new->ev.event_cinfo[i]);

            init_irq_work(&new->ev.event_cinfo[i]->pending, memguard_event_overflow);

            new->ev.event_cinfo[i]->throttle_thread =
                kthread_create_on_node(throttle_thread,
                               (void *)((unsigned long)i),
                               cpu_to_node(i),
                               "kthrottle/%d", i);

            BUG_ON(IS_ERR(new->ev.event_cinfo[i]->throttle_thread));
            kthread_bind(new->ev.event_cinfo[i]->throttle_thread, i);
            wake_up_process(new->ev.event_cinfo[i]->throttle_thread);
        }

        if(events_head == NULL)
        {
            events_head = new;
            events_head->next = NULL;
            events_head->prev = NULL;

            events_tail = events_head;
            events_tail->next = NULL;
            events_tail->prev = NULL;
        }
        else
        {
            new->next = NULL;
            new->prev = events_tail;

            events_tail->next = new;
            events_tail = new;
        }

        for_each_online_cpu(i)
        {
            new->ev.event_cinfo[i]->event->pmu->add(new->ev.event_cinfo[i]->event, PERF_EF_START);
        }
	}
    else if (!strncmp(p, "del ", 4)) {
        p+=4;

		sscanf(p, "%s", event_name);
        if(event_name == NULL)
        {
            return cnt;
        }

        while(temp != NULL)
        {
            if(!strcmp(temp->ev.event_name, event_name))
            {
                disable_event(temp);

                if(temp == events_head)
                {
                    events_head = temp->next;
                }
                if(temp->next != NULL)
                {
                    temp->next->prev = temp->prev;
                }
                if(temp->prev != NULL)
                {
                    temp->prev->next = temp->next;
                }

                kfree(temp);
                break;
            }
            temp = temp->next;
        }
    }
    else if (!strncmp(p, "bud ", 4)) {
        p+=4;

        sscanf(p, "%s %d", event_name, &event_budget);
        if(event_name == NULL || event_budget == 0)
        {
            return cnt;
        }
        while(temp != NULL)
        {
            if(!strcmp(temp->ev.event_name, event_name))
            {
                temp->ev.event_budget = event_budget;
                get_online_cpus();
                for_each_online_cpu(i)
                {
                    temp->ev.event_cinfo[i]->budget = event_budget;
                }
                break;
            }
            temp = temp->next;
        }
    }*/
    return cnt;
}

static int events_show(struct seq_file *m, void *v)
{
    /*int i, cpu;
	cpu = get_cpu();

	seq_printf(m, "cpu  |budget (MB/s,pct,weight)\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->limit > 0)
			budget = cinfo->limit;

		WARN_ON_ONCE(budget == 0);
		seq_printf(m, "CPU%d: %d (%dMB/s)\n",
				   i, budget,
				   convert_events_to_mb(budget));
	}
#if USE_BWLOCK
	struct memguard_info *global = &memguard_info;
	seq_printf(m, "bwlocked_core: %d\n", global->bwlocked_cores);
#endif

	put_cpu();
	return 0;

    struct event_node *temp = events_head;
    while(temp != NULL)
    {
        seq_printf(m, "%s %s %d\n", temp->ev.event_name, temp->ev.event_counter, temp->ev.event_budget);
        temp = temp->next;
    }*/

	int i;
	for_each_online_cpu(i)
	{
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		struct event_info* temp = cinfo->head;
		while(temp != NULL)
		{
			seq_printf(m, "%s %s %d (%d)\n", temp->name, temp->counter, temp->mb_budget, temp->budget);
			temp = temp->next;
		}
	}
    return 0;
}

static int events_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, events_show, NULL);
}

static const struct file_operations events_fops = {
	.open		= events_open,
	.write          = events_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int memguard_init_debugfs(void)
{

	memguard_dir = debugfs_create_dir("memguard", NULL);
	BUG_ON(!memguard_dir);
	debugfs_create_file("control", 0444, memguard_dir, NULL,
			    &memguard_control_fops);

	debugfs_create_file("limit", 0444, memguard_dir, NULL,
			    &memguard_limit_fops);

	debugfs_create_file("usage", 0666, memguard_dir, NULL,
			    &memguard_usage_fops);

	debugfs_create_file("events", 0666, memguard_dir, NULL,
				&events_fops);
	return 0;
}

static int throttle_thread(void *arg)
{
	int cpunr = (unsigned long)arg;
	struct core_info *cinfo = per_cpu_ptr(core_info, cpunr);

	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
	};

	sched_setscheduler(current, SCHED_FIFO, &param);

	while (!kthread_should_stop() && cpu_online(cpunr)) {

		DEBUG(trace_printk("wait an event\n"));
		wait_event_interruptible(cinfo->throttle_evt,
					 cinfo->throttled_task ||
					 kthread_should_stop());

		DEBUG(trace_printk("got an event\n"));

		if (kthread_should_stop())
			break;

		while (cinfo->throttled_task && !kthread_should_stop())
		{
			smp_mb();
			cpu_relax();
			/* TODO: mwait */
		}
	}

	DEBUG(trace_printk("exit\n"));
	return 0;
}

int init_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;

	/* initialized memguard_info structure */
	memset(global, 0, sizeof(struct memguard_info));
	zalloc_cpumask_var(&global->throttle_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&global->active_mask, GFP_NOWAIT);

	if (g_period_us < 0 || g_period_us > 1000000) {
		printk(KERN_INFO "Must be 0 < period < 1 sec\n");
		return -ENODEV;
	}

	global->start_tick = jiffies;
	global->period_in_ktime = ktime_set(0, g_period_us * 1000);

	/* initialize all online cpus to be active */
	cpumask_copy(global->active_mask, cpu_online_mask);

	pr_info("ARCH: %s\n", g_hw_type);
	pr_info("HZ=%d, g_period_us=%d\n", HZ, g_period_us);

	pr_info("Initilizing perf counter\n");
	core_info = alloc_percpu(struct core_info);

	get_online_cpus();
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget;

		if (i >= MAX_NCPUS) {
			printk(KERN_INFO "too many cores. up to %d is supported\n", MAX_NCPUS);
			return -ENODEV;
		}
		/* initialize counter h/w & event structure */
		if (g_budget_mb[i] == 0)
			g_budget_mb[i] = 1000;

		budget = convert_mb_to_events(g_budget_mb[i]);
		pr_info("budget[%d] = %d (%d MB)\n", i, budget, g_budget_mb[i]);

		/* initialize per-core data structure */
		memset(cinfo, 0, sizeof(struct core_info));

		/* create performance counter */
		cinfo->event = init_counter(i, budget);
		if (!cinfo->event)
			break;

		/* initialize budget */
		cinfo->budget = cinfo->limit = cinfo->event->hw.sample_period;

		/* throttled task pointer */
		cinfo->throttled_task = NULL;

		init_waitqueue_head(&cinfo->throttle_evt);

		/* initialize statistics */
		/* update local period information */
		cinfo->period_cnt = 0;
		cinfo->used[0] = cinfo->used[1] = cinfo->used[2] =
			cinfo->budget; /* initial condition */
		cinfo->cur_budget = cinfo->budget;
		cinfo->overall.used_budget = 0;
		cinfo->overall.assigned_budget = 0;
		cinfo->overall.throttled_time_ns = 0;
		cinfo->overall.throttled = 0;
		cinfo->overall.throttled_error = 0;

		memset(cinfo->overall.throttled_error_dist, 0, sizeof(int)*10);
		cinfo->throttled_time = ktime_set(0,0);

		print_core_info(smp_processor_id(), cinfo);

		/* initialize nmi irq_work_queue */
		init_irq_work(&cinfo->pending, memguard_process_overflow);

		/* create and wake-up throttle threads */
		cinfo->throttle_thread =
			kthread_create_on_node(throttle_thread,
					       (void *)((unsigned long)i),
					       cpu_to_node(i),
					       "kthrottle/%d", i);

		cinfo->head = create_event_info(i, "read_limit", "0x17", 300);
		cinfo->head->next = create_event_info(i, "write_limit", "0x18", 100);
		cinfo->tail = cinfo->head->next;

		BUG_ON(IS_ERR(cinfo->throttle_thread));
		kthread_bind(cinfo->throttle_thread, i);
		wake_up_process(cinfo->throttle_thread);
	}

	memguard_init_debugfs();

	pr_info("Start event counters\n");
	start_counters();

	pr_info("Start period timer (period=%lld us)\n",
		div64_u64(TM_NS(global->period_in_ktime), 1000));

	get_cpu();
	global->master = smp_processor_id();

	hrtimer_init(&global->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED );
	global->hr_timer.function = &period_timer_callback_master;
	hrtimer_start(&global->hr_timer, global->period_in_ktime,
		      HRTIMER_MODE_REL_PINNED);
	put_cpu();

	return 0;
}

void cleanup_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;

	get_online_cpus();

	/* unregister sched-tick callback */
	pr_info("Cancel timer\n");
	hrtimer_cancel(&global->hr_timer);

	/* stop perf_event counters */
	disable_counters();

	/* destroy perf objects */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		struct event_info *temp = cinfo->head;
		pr_info("Stopping kthrottle/%d\n", i);
		cinfo->throttled_task = NULL;
		kthread_stop(cinfo->throttle_thread);

		perf_event_disable(cinfo->event);
		perf_event_release_kernel(cinfo->event);
		cinfo->event = NULL;

		while(temp != NULL)
		{
			perf_event_disable(temp->event);
			perf_event_release_kernel(temp->event);
			temp->event = NULL;

			cinfo->head = temp->next;
			kfree(temp);
			temp = cinfo->head;
		}
	}

	/* remove debugfs entries */
	debugfs_remove_recursive(memguard_dir);

	/* free allocated data structure */
	free_cpumask_var(global->throttle_mask);
	free_cpumask_var(global->active_mask);
	free_percpu(core_info);

	pr_info("module uninstalled successfully\n");
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heechul Yun <heechul@illinois.edu>");
