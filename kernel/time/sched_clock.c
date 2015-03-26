/*
 * sched_clock.c: support for extending counters to full 64-bit ns counter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/syscore_ops.h>
#include <linux/hrtimer.h>
#include <linux/sched_clock.h>
#include <linux/seqlock.h>
#include <linux/bitops.h>

/**
 * struct clock_read_data - data required to read from sched_clock
 *
 * @epoch_ns:		sched_clock value at last update
 * @epoch_cyc:		Clock cycle value at last update
 * @sched_clock_mask:   Bitmask for two's complement subtraction of non 64bit
 *			clocks
 * @read_sched_clock:	Current clock source (or dummy source when suspended)
 * @mult:		Multipler for scaled math conversion
 * @shift:		Shift value for scaled math conversion
 * @suspended:		Flag to indicate if the clock is suspended (stopped)
 *
 * Care must be taken when updating this structure; it is read by
 * some very hot code paths. It occupies <=48 bytes and, when combined
 * with the seqcount used to synchronize access, comfortably fits into
 * a 64 byte cache line.
 */
struct clock_read_data {
	u64 epoch_ns;
	u64 epoch_cyc;
	u64 sched_clock_mask;
	u64 (*read_sched_clock)(void);
	u32 mult;
	u32 shift;
	bool suspended;
	bool needs_suspend;
};

/**
 * struct clock_data - all data needed for sched_clock (including
 *                     registration of a new clock source)
 *
 * @seq:		Sequence counter for protecting updates.
 * @read_data:		Data required to read from sched_clock.
 * @wrap_kt:		Duration for which clock can run before wrapping
 * @rate:		Tick rate of the registered clock
 * @actual_read_sched_clock: Registered clock read function
 *
 * The ordering of this structure has been chosen to optimize cache
 * performance. In particular seq and read_data (combined) should fit
 * into a single 64 byte cache line.
 */
struct clock_data {
	seqcount_t seq;
	struct clock_read_data read_data;
	ktime_t wrap_kt;
	unsigned long rate;
};

static struct hrtimer sched_clock_timer;
static int irqtime = -1;

core_param(irqtime, irqtime, int, 0400);

static u64 notrace jiffy_sched_clock_read(void)
{
	/*
	 * We don't need to use get_jiffies_64 on 32-bit arches here
	 * because we register with BITS_PER_LONG
	 */
	return (u64)(jiffies - INITIAL_JIFFIES);
}

static u32 __read_mostly (*read_sched_clock_32)(void);

static u64 notrace read_sched_clock_32_wrapper(void)
{
	return read_sched_clock_32();
}

static struct clock_data cd ____cacheline_aligned = {
	.read_data = { .mult = NSEC_PER_SEC / HZ,
		       .read_sched_clock = jiffy_sched_clock_read, },
};

static inline u64 notrace cyc_to_ns(u64 cyc, u32 mult, u32 shift)
{
	return (cyc * mult) >> shift;
}

static unsigned long long notrace sched_clock_32(void)
{
	u64 epoch_ns;
	u64 epoch_cyc;
	u64 cyc;
	unsigned long seq;
	struct clock_read_data *rd = &cd.read_data;

	if (rd->suspended)
		return rd->epoch_ns;

	do {
		seq = read_seqcount_begin(&cd.seq);
		epoch_cyc = rd->epoch_cyc;
		epoch_ns = rd->epoch_ns;
	} while (read_seqcount_retry(&cd.seq, seq));

	cyc = rd->read_sched_clock();
	cyc = (cyc - epoch_cyc) & rd->sched_clock_mask;
	return epoch_ns + cyc_to_ns(cyc, rd->mult, rd->shift);
}

/*
 * Atomically update the sched_clock epoch.
 */
static void notrace update_sched_clock(void)
{
	unsigned long flags;
	u64 cyc;
	u64 ns;
	struct clock_read_data *rd = &cd.read_data;

	cyc = rd->read_sched_clock();
	ns = rd->epoch_ns +
	     cyc_to_ns((cyc - rd->epoch_cyc) & rd->sched_clock_mask,
		       rd->mult, rd->shift);

	raw_local_irq_save(flags);
	write_seqcount_begin(&cd.seq);
	rd->epoch_ns = ns;
	rd->epoch_cyc = cyc;
	write_seqcount_end(&cd.seq);
	raw_local_irq_restore(flags);
}

static enum hrtimer_restart sched_clock_poll(struct hrtimer *hrt)
{
	update_sched_clock();
	hrtimer_forward_now(hrt, cd.wrap_kt);
	return HRTIMER_RESTART;
}

void __init sched_clock_register(u64 (*read)(void), int bits,
				 unsigned long rate)
{
	unsigned long r;
	u64 res, wrap;
	char r_unit;
	struct clock_read_data *rd = &cd.read_data;

	if (cd.rate > rate)
		return;

	WARN_ON(!irqs_disabled());
	rd->read_sched_clock = read;
	rd->sched_clock_mask = CLOCKSOURCE_MASK(bits);
	cd.rate = rate;

	/* calculate the mult/shift to convert counter ticks to ns. */
	clocks_calc_mult_shift(&rd->mult, &rd->shift, rate, NSEC_PER_SEC, 3600);

	r = rate;
	if (r >= 4000000) {
		r /= 1000000;
		r_unit = 'M';
	} else if (r >= 1000) {
		r /= 1000;
		r_unit = 'k';
	} else
		r_unit = ' ';

	/* calculate how many ns until we wrap */
	wrap = clocks_calc_max_nsecs(rd->mult, rd->shift, 0, rd->sched_clock_mask);
	cd.wrap_kt = ns_to_ktime(wrap - (wrap >> 3));

	/* calculate the ns resolution of this counter */
	res = cyc_to_ns(1ULL, rd->mult, rd->shift);
	pr_info("sched_clock: %u bits at %lu%cHz, resolution %lluns, wraps every %lluns\n",
		bits, r, r_unit, res, wrap);

	update_sched_clock();

	/*
	 * Ensure that sched_clock() starts off at 0ns
	 */
	rd->epoch_ns = 0;

	/* Enable IRQ time accounting if we have a fast enough sched_clock */
	if (irqtime > 0 || (irqtime == -1 && rate >= 1000000))
		enable_sched_clock_irqtime();

	pr_debug("Registered %pF as sched_clock source\n", read);
}

void __init setup_sched_clock(u32 (*read)(void), int bits, unsigned long rate)
{
	read_sched_clock_32 = read;
	sched_clock_register(read_sched_clock_32_wrapper, bits, rate);
}

unsigned long long __read_mostly (*sched_clock_func)(void) = sched_clock_32;

unsigned long long notrace sched_clock(void)
{
	return sched_clock_func();
}

void __init sched_clock_postinit(void)
{
	/*
	 * If no sched_clock function has been provided at that point,
	 * make it the final one one.
	 */
	if (cd.read_data.read_sched_clock == jiffy_sched_clock_read)
		sched_clock_register(jiffy_sched_clock_read, BITS_PER_LONG, HZ);

	update_sched_clock();

	/*
	 * Start the timer to keep sched_clock() properly updated and
	 * sets the initial epoch.
	 */
	hrtimer_init(&sched_clock_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sched_clock_timer.function = sched_clock_poll;
	hrtimer_start(&sched_clock_timer, cd.wrap_kt, HRTIMER_MODE_REL);
}

static int sched_clock_suspend(void)
{
	struct clock_read_data *rd = &cd.read_data;

	update_sched_clock();
	hrtimer_cancel(&sched_clock_timer);
	rd->suspended = true;
	return 0;
}

static void sched_clock_resume(void)
{
	struct clock_read_data *rd = &cd.read_data;

	rd->epoch_cyc = rd->read_sched_clock();
	hrtimer_start(&sched_clock_timer, cd.wrap_kt, HRTIMER_MODE_REL);
	rd->suspended = false;
}

static struct syscore_ops sched_clock_ops = {
	.suspend = sched_clock_suspend,
	.resume = sched_clock_resume,
};

static int __init sched_clock_syscore_init(void)
{
	register_syscore_ops(&sched_clock_ops);
	return 0;
}
device_initcall(sched_clock_syscore_init);
