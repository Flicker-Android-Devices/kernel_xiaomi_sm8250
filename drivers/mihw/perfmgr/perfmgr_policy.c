/*
 * Copyright (c) 2021, Xiaomi. All rights reserved.
 * D20230725
 */

#define pr_fmt(fmt) "perfmgr_policy: " fmt
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/module.h>
#include <linux/timer.h>
#include "perfmgr.h"

#define FREQ_MAX_DEFAULT_VALUE S32_MAX
#define FPS_30 30
#define FPS_45 45
#define FPS_49 49
#define FPS_60 60
#define FPS_61 61
#define FPS_90 90
#define FPS_91 91
#define FPS_120 120
#define FPS_121 121
#define FPS_144 144
#define FPS_UNKNOW -1
#define FRAME_COUNT_MAX 30
#define RESCUE_THRES 700
#define MIN_CONTINUS_NOJANK_CNT 6
#define MIN_FASTDOWN_CIRCLE 3
#define MIN_FREQ 614400

#define M 1000
#define N 100

#define MAX_TABLE_SIZE 28

struct kobject *perfmgr_policy_kobj;
struct perfmgr_work cur_work;

/* SM8250 CPU frequency limits: 0: Little (0-3), 1: Big (4-6), 2: Prime (7) */
static unsigned int perfmgr_limit_min[NR_CPUS];
static unsigned int perfmgr_limit_max[NR_CPUS];

static int last_freq_level = 0;
static int jank_happened = 0;

#ifdef CONFIG_MI_PERFMGR_GPU
int gpu_get_target_fps = 60; /* default 60 */
#endif

int debug_mask_perfmgr = 0;
module_param(debug_mask_perfmgr, int, 0644);
MODULE_PARM_DESC(debug_mask_perfmgr, "debug_mask_perfmgr");

int perfmgr_enable = 0;
module_param(perfmgr_enable, int, 0644);
MODULE_PARM_DESC(perfmgr_enable, "perfmgr_enable");

static int r_freq_level = 0; /* rescue_freq_level */
module_param(r_freq_level, int, 0644);
MODULE_PARM_DESC(r_freq_level, "r_freq_level");

static int timeout_r_freq_level = 2; /* timeout_rescue_freq_level */
module_param(timeout_r_freq_level, int, 0644);
MODULE_PARM_DESC(timeout_r_freq_level, "timeout_r_freq_level");

static int r_perf = 1; /* rescue_performance */
module_param(r_perf, int, 0644);
MODULE_PARM_DESC(r_perf, "r_perf");

static int r_step = 750; /* rescue_step */
module_param(r_step, int, 0644);
MODULE_PARM_DESC(r_step, "r_step");

static int p_freq_level = 1; /* predict_freq_level */
module_param(p_freq_level, int, 0644);
MODULE_PARM_DESC(p_freq_level, "p_freq_level");

static int p_perf = 0; /* predict_performance */
module_param(p_perf, int, 0644);
MODULE_PARM_DESC(p_perf, "p_perf");

static int p_step = 750; /* predict_step */
module_param(p_step, int, 0644);
MODULE_PARM_DESC(p_step, "p_step");

static int nor_f_keep = 12; /* normal_frame_keep_count */
module_param(nor_f_keep, int, 0644);
MODULE_PARM_DESC(nor_f_keep, "nor_f_keep");

static int cons_no_j_cnt = 10; /* continus_no_jank_count */
module_param(cons_no_j_cnt, int, 0644);
MODULE_PARM_DESC(cons_no_j_cnt, "cons_no_j_cnt");

static int j_f_k_count = 25; /* jank_frame_keep_count */
module_param(j_f_k_count, int, 0644);
MODULE_PARM_DESC(j_f_k_count, "j_f_k_count");

static int max_freq_limit_level = 27;
module_param(max_freq_limit_level, int, 0644);
MODULE_PARM_DESC(max_freq_limit_level, "max_freq_limit_level");

static int fast_down_level_thres = 50;
module_param(fast_down_level_thres, int, 0644);
MODULE_PARM_DESC(fast_down_level_thres, "fast_down_level_thres");

static int fast_down_circle_base = 5;
module_param(fast_down_circle_base, int, 0644);
MODULE_PARM_DESC(fast_down_circle_base, "fast_down_circle_base");

static int fast_down_freq_level = -2;
module_param(fast_down_freq_level, int, 0644);
MODULE_PARM_DESC(fast_down_freq_level, "down_freq_level");

static int f_t_fps = FPS_UNKNOW; /* fixed_target_fps */
module_param(f_t_fps, int, 0644);
MODULE_PARM_DESC(f_t_fps, "f_t_fps");

static int cpu0_offset = 0;
module_param(cpu0_offset, int, 0644);
MODULE_PARM_DESC(cpu0_offset, "cpu0_offset");

static int little_limit_enable = 1;
module_param(little_limit_enable, int, 0644);
MODULE_PARM_DESC(little_limit_enable, "little_limit_enable");

static int cpu5_offset = 0;
module_param(cpu5_offset, int, 0644);
MODULE_PARM_DESC(cpu5_offset, "cpu5_offset");

static int cpu7_offset = 0;
module_param(cpu7_offset, int, 0644);
MODULE_PARM_DESC(cpu7_offset, "cpu7_offset");

static int b_minfreq = 0;
module_param(b_minfreq, int, 0644);
MODULE_PARM_DESC(b_minfreq, "b_minfreq");

static int scaling_a = 380;
module_param(scaling_a, int, 0644);
MODULE_PARM_DESC(scaling_a, "scaling_a");

static int scaling_a_thres = 550;
module_param(scaling_a_thres, int, 0644);
MODULE_PARM_DESC(scaling_a_thres, "scaling_a_thres");

static int scaling_r_thres = RESCUE_THRES;
module_param(scaling_r_thres, int, 0644);
MODULE_PARM_DESC(scaling_r_thres, "scaling_r_thres");

static int scaling_b = -50;
module_param(scaling_b, int, 0644);
MODULE_PARM_DESC(scaling_b, "scaling_b");

static int scaling_c = 3;
module_param(scaling_c, int, 0644);
MODULE_PARM_DESC(scaling_c, "scaling_c");

static int load_reset = 1;
module_param(load_reset, int, 0644);
MODULE_PARM_DESC(load_reset, "load_reset");

static int load_scaling_x = 10;
module_param(load_scaling_x, int, 0644);
MODULE_PARM_DESC(load_scaling_x, "load_scaling_x");

static int load_scaling_y = -1;
module_param(load_scaling_y, int, 0644);
MODULE_PARM_DESC(load_scaling_y, "load_scaling_y");

static int t_fps_49 = 0;
module_param(t_fps_49, int, 0644);
MODULE_PARM_DESC(t_fps_49, "t_fps_49");

static int t_fps_61 = 0;
module_param(t_fps_61, int, 0644);
MODULE_PARM_DESC(t_fps_61, "t_fps_61");

static int t_fps_91 = 0;
module_param(t_fps_91, int, 0644);
MODULE_PARM_DESC(t_fps_91, "t_fps_91");

static int t_fps_121 = 0;
module_param(t_fps_121, int, 0644);
MODULE_PARM_DESC(t_fps_121, "t_fps_121");

static int l_cpu_start = 0;
module_param(l_cpu_start, int, 0644);
MODULE_PARM_DESC(l_cpu_start, "l_cpu_start");

static int min_freq_limit_level_limit = 0;
module_param(min_freq_limit_level_limit, int, 0644);
MODULE_PARM_DESC(min_freq_limit_level_limit, "min_freq_limit_level_limit");

static int f_minfreq = 710400; /* SM8250 Big core min freq */
module_param(f_minfreq, int, 0644);
MODULE_PARM_DESC(f_minfreq, "f_minfreq");

static int f_left_minfreq = 710400;
module_param(f_left_minfreq, int, 0644);
MODULE_PARM_DESC(f_left_minfreq, "f_left_minfreq");

static int f_minfreq0 = 614400; /* SM8250 Little core min freq */
module_param(f_minfreq0, int, 0644);
MODULE_PARM_DESC(f_minfreq0, "f_minfreq0");

static int f_left_minfreq0 = 614400;
module_param(f_left_minfreq0, int, 0644);
MODULE_PARM_DESC(f_left_minfreq0, "f_left_minfreq0");

static int better_perf = 1;
module_param(better_perf, int, 0644);
MODULE_PARM_DESC(better_perf, "better_perf");

static int perf_count = 1;
module_param(perf_count, int, 0644);
MODULE_PARM_DESC(perf_count, "perf_count");

static int timeout = 6000;
module_param(timeout, int, 0644);
MODULE_PARM_DESC(timeout, "timeout");

static int timeout_144 = 9100;
module_param(timeout_144, int, 0644);
MODULE_PARM_DESC(timeout_144, "timeout_144");

static int timeout_120 = 13000;
module_param(timeout_120, int, 0644);
MODULE_PARM_DESC(timeout_120, "timeout_120");

static int timeout_90 = 16600;
module_param(timeout_90, int, 0644);
MODULE_PARM_DESC(timeout_90, "timeout_90");

static int timeout_60 = 25000;
module_param(timeout_60, int, 0644);
MODULE_PARM_DESC(timeout_60, "timeout_60");

static int timeout_49 = 27000;
module_param(timeout_49, int, 0644);
MODULE_PARM_DESC(timeout_49, "timeout_49");

static int timeout_30 = 44000;
module_param(timeout_30, int, 0644);
MODULE_PARM_DESC(timeout_30, "timeout_30");

static int timeout_left = 2333;
module_param(timeout_left, int, 0644);
MODULE_PARM_DESC(timeout_left, "timeout_left");

static int sf_comp_hint_enable = 0;
module_param(sf_comp_hint_enable, int, 0644);
MODULE_PARM_DESC(sf_comp_hint_enable, "sf_comp_hint_enable");

static int max_freq_limit_level_limit = 0;

/* Initial SM8250 Little (CPU 0-3), Big (CPU 4-6) and Prime (CPU 7) frequency tables */
int cpufreq_table0[MAX_TABLE_SIZE] = {
	1804800, 1804800, 1708800, 1708800, 1612800, 1612800, 1516800,
	1516800, 1420800, 1344000, 1248000, 1171200, 1075200, 979200,
	883200,	 787200,  691200,  614400,  614400,  614400,  614400,
	614400,	 614400,  614400,  614400,  614400,  614400,  614400
};

int cpufreq_table5[MAX_TABLE_SIZE] = {
	2419200, 2419200, 2323200, 2208000, 2092800, 1977600, 1862400,
	1747200, 1612800, 1497600, 1382400, 1248000, 1132800, 1017600,
	902400,	 710400,  710400,  710400,  710400,  710400,  710400,
	710400,	 710400,  710400,  710400,  710400,  710400,  710400
};

int cpufreq_table7[MAX_TABLE_SIZE] = {
	2841600, 2841600, 2745600, 2649600, 2534400, 2419200, 2323200,
	2208000, 2092800, 1977600, 1862400, 1747200, 1612800, 1497600,
	1382400, 1248000, 1132800, 1017600, 902400,  844800,  844800,
	844800,	 844800,  844800,  844800,  844800,  844800,  844800
};

/* Linux 4.19 cpufreq notifier constraint handler */
static int perfmgr_cpufreq_notifier(struct notifier_block *nb,
				    unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	unsigned int min_f;
	unsigned int max_f;

	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	min_f = perfmgr_limit_min[cpu];
	max_f = perfmgr_limit_max[cpu];

	if (max_f == 0)
		max_f = policy->cpuinfo.max_freq;
	if (min_f == 0)
		min_f = policy->cpuinfo.min_freq;

	cpufreq_verify_within_limits(policy, min_f, max_f);
	return NOTIFY_OK;
}

static struct notifier_block perfmgr_cpufreq_nb = {
	.notifier_call = perfmgr_cpufreq_notifier,
};

static void update_policy_online(void)
{
	unsigned int i;
	struct cpufreq_policy *policy;
	struct cpumask online_cpus;

	cpumask_copy(&online_cpus, cpu_online_mask);
	for_each_cpu (i, &online_cpus) {
		if (i >= l_cpu_start) {
			policy = cpufreq_cpu_get(i);
			if (!policy)
				continue;
			cpumask_andnot(&online_cpus, &online_cpus,
				       policy->related_cpus);
			cpufreq_update_policy(i);
			cpufreq_cpu_put(policy);
		}
	}
}

static void do_freq(int cpu0_max, int cpu0_min, int cpu5_max, int cpu5_min,
		    int cpu7_max, int cpu7_min)
{
	unsigned int i;
	int maxfreq;

	pr_debug(
		"cpu0max %d cpu0min %d cpu5max %d cpu5min %d cpu7max %d cpu7min %d\n",
		cpu0_max, cpu0_min, cpu5_max, cpu5_min, cpu7_max, cpu7_min);

	maxfreq = max(cpu5_max, cpu7_max);
	perfmgr_boost_dcvsfreq_and_timeout(maxfreq);

	for_each_possible_cpu (i) {
		switch (i) {
		case 0:
		case 1:
		case 2:
		case 3:
			/* Little cluster (Cortex-A55, CPU 0-3) */
			if (little_limit_enable && i >= l_cpu_start) {
				perfmgr_limit_max[i] =
					cpu0_max ? (cpu0_max + cpu0_offset) :
						   UINT_MAX;
				perfmgr_limit_min[i] = cpu0_min;
			} else {
				perfmgr_limit_max[i] = UINT_MAX;
				perfmgr_limit_min[i] = 0;
			}
			break;
		case 4:
		case 5:
		case 6:
			/* Big cluster (Cortex-A77, CPU 4-6) */
			if (i >= l_cpu_start) {
				perfmgr_limit_max[i] =
					cpu5_max ? (cpu5_max + cpu5_offset) :
						   UINT_MAX;
				perfmgr_limit_min[i] = cpu5_min;
			}
			break;
		case 7:
			/* Prime core (Cortex-A77, CPU 7) */
			if (i >= l_cpu_start) {
				perfmgr_limit_max[i] =
					cpu7_max ? (cpu7_max + cpu7_offset) :
						   UINT_MAX;
				perfmgr_limit_min[i] = cpu7_min;
			}
			break;
		default:
			break;
		}
	}
	update_policy_online();
}

static void do_frame_limit_freq(int set_freq_level)
{
	if (cur_work.perfmgr_state == PERFMGR_FREQUENCY_LIMIT_STATE)
		do_freq(cpufreq_table0[set_freq_level], f_minfreq0,
			cpufreq_table5[set_freq_level], f_minfreq,
			cpufreq_table7[set_freq_level], f_minfreq);
}

static void do_maxfreq_release(void)
{
	unsigned int i;

	for_each_possible_cpu (i) {
		perfmgr_limit_max[i] = UINT_MAX;
		perfmgr_limit_min[i] = 0;
	}
	update_policy_online();
	perfmgr_boost_dcvsfreq_and_timeout(0);
}

static void do_maxfreq_release_work(struct work_struct *work)
{
	pr_debug("do_maxfreq_release_work\n");
	do_maxfreq_release();
}

static void maxfreq_release_timeout(int ms)
{
	schedule_delayed_work(&cur_work.release_work, msecs_to_jiffies(ms));
}

static int left_shift(s64 *str, int len)
{
	int i;
	long sum = 0;

	for (i = 1; i <= len; i++) {
		str[i - 1] = str[i];
		sum += str[i];
	}
	return sum;
}

static int calulate_fps(s64 frame_usecs64, struct connected_buffer *cur)
{
	int average_time = 0;
	int i;

	if (cur == NULL)
		return 0;

	if (cur->target_fps == 0) {
		for (i = 0; i < FRAME_UNIT; i++) {
			cur->count_time += cur->last_frame_unit[i];
		}
		average_time = cur->count_time / FRAME_UNIT;
	} else {
		cur->frame_count++;
		cur->count_time += frame_usecs64;
		if (cur->frame_count != FRAME_COUNT_MAX)
			return 0;
		average_time = cur->count_time / FRAME_COUNT_MAX;
		pr_debug("%s cur count_time=%lld average_time %d\n", __func__,
			 cur->count_time, average_time);
	}

	perfmgr_dbg(
		debug_mask_perfmgr, PERFMGR_POLICY,
		"cur->id=%lld cur->target fps=%d, cur->frame_count=%d avg_time=%d\n",
		cur->identifier, cur->target_fps, cur->frame_count,
		average_time);

	cur->frame_count = 0;
	cur->count_time = 0;

	if (average_time < 8130) /* 123-144 */
		return FPS_144;
	else if (average_time < 10753) { /* 93-123 */
		return t_fps_121 ? FPS_121 : FPS_120;
	} else if (average_time < 16129) { /* 62-93 */
		return t_fps_91 ? FPS_91 : FPS_90;
	} else if (average_time <
		   (t_fps_49 ? 20000 : 21277)) { /* 50-62 / 47-62 */
		return t_fps_61 ? FPS_61 : FPS_60;
	} else if (average_time < 31250) { /* 32-50 */
		return t_fps_49 ? FPS_49 : FPS_45;
	} else if (average_time < 50000) { /* 20-32 */
		return FPS_30;
	}
	return -1;
}

static int frame_time(int target_fps)
{
	if (target_fps == FPS_144)
		return 6944;
	else if (target_fps == FPS_121)
		return 8264;
	else if (target_fps == FPS_120)
		return 8333;
	else if (target_fps == FPS_91)
		return 10989;
	else if (target_fps == FPS_90)
		return 11111;
	else if (target_fps == FPS_61)
		return 16393;
	else if (target_fps == FPS_60)
		return 16666;
	else if (target_fps == FPS_49)
		return 20408;
	else if (target_fps == FPS_45)
		return 22222;
	else if (target_fps == FPS_30)
		return 33333;
	else
		return 11111;
}

static void perfmgr_set_hrtimer(struct connected_buffer *cur, int us)
{
	cur_work.fps = cur->target_fps;
	pr_debug("%s us %d cur.fps %d\n", __func__, us, cur_work.fps);
	hrtimer_start(&cur_work.hr_timer, ns_to_ktime(us * 1000),
		      HRTIMER_MODE_REL);
}

static void perfmgr_set_ceiling_and_floor(struct connected_buffer *cur,
					  int set_freq_level)
{
	int time = 0;

	if (better_perf == 1) {
		if (cur->target_fps == 144)
			time = timeout_144;
		else if (cur->target_fps == 120)
			time = timeout_120;
		else if (cur->target_fps == 90)
			time = timeout_90;
		else if (cur->target_fps == 60)
			time = timeout_60;
		else if ((cur->target_fps == 49) || (cur->target_fps == 45))
			time = timeout_49;
		else if (cur->target_fps == 30)
			time = timeout_30;
		else
			time = timeout_60;
	} else {
		time = timeout;
	}

	pr_debug("%s smooth %d timeout %d\n", __func__, cur->smooth, time);
	cur_work.perfmgr_state = PERFMGR_FREQUENCY_LIMIT_STATE;
	perfmgr_set_hrtimer(cur, time);

	if (cur->smooth)
		do_freq(cpufreq_table0[set_freq_level], f_minfreq0,
			cpufreq_table5[set_freq_level], f_minfreq,
			cpufreq_table7[set_freq_level], f_minfreq);
	else
		do_freq(cpufreq_table0[0], f_minfreq0, cpufreq_table5[0],
			f_minfreq, cpufreq_table7[0], f_minfreq);
}

static void perfmgr_set_freq(struct connected_buffer *cur, int r_p_level,
			     int level)
{
	int set_freq_level;

	if (cur == NULL)
		return;

	pr_debug("%s last_freq_level %d level=%d start\n", __func__,
		 last_freq_level, level);
	set_freq_level = last_freq_level - r_p_level - level;

	if (max_freq_limit_level > max_freq_limit_level_limit)
		max_freq_limit_level = max_freq_limit_level_limit;
	if (set_freq_level < min_freq_limit_level_limit)
		set_freq_level = min_freq_limit_level_limit;
	if (set_freq_level > max_freq_limit_level)
		set_freq_level = max_freq_limit_level;

	if (better_perf)
		perfmgr_set_ceiling_and_floor(cur, set_freq_level);
	else
		do_frame_limit_freq(set_freq_level);

	last_freq_level = set_freq_level;
	pr_debug("%s last_freq_level %d level=%d end\n", __func__,
		 last_freq_level, level);

	if (load_reset && (level >= 0)) {
		cur->last_frame_unit[FRAME_UNIT - 1] =
			(frame_time(cur->target_fps) +
			 ((frame_time(cur->target_fps) * load_scaling_y) >>
			  load_scaling_x));
	}
}

static int update_circle(int circle)
{
	if (last_freq_level < max_freq_limit_level / 3) {
		circle = circle - circle / 3;
	} else if (last_freq_level < max_freq_limit_level / 2 &&
		   last_freq_level >= max_freq_limit_level / 3) {
		circle = circle - circle / 4;
	} else if (last_freq_level >= max_freq_limit_level / 2 &&
		   last_freq_level > max_freq_limit_level * 2 / 3) {
		/* keep */
	} else {
		circle = circle + circle / 4;
	}
	return circle;
}

void perfmgr_do_policy(struct connected_buffer *cur)
{
	int target_fps = 0;
	int r_p_level = 0;
	int level = 0;
	int fast_down_circle = 0;
	ktime_t current_time;
	s64 frame_usecs64;
	s64 frame_four_usecs64;
	s64 frame_unit_usecs64;
	s64 frame_usecs64_x_fps;
	s64 frame_unit_usecs64_x_fps;
	static ktime_t last_limit_time;
	static int down_count = 0;

	if (cur == NULL)
		return;

	cancel_delayed_work(&cur_work.release_work);
	current_time = ktime_get();
	frame_usecs64 = ktime_to_us(ktime_sub(current_time, cur->last_time));

	if (better_perf) {
		if (hrtimer_active(&cur_work.hr_timer))
			hrtimer_cancel(&cur_work.hr_timer);
	}
	maxfreq_release_timeout(50000);

	if (cur->last_time == 0) {
		cur->last_time = current_time;
		return;
	}
	cur->last_time = current_time;

	if ((cur->target_fps == 0) && (cur->frame_count < FRAME_UNIT)) {
		cur->last_frame_unit[cur->frame_count] = frame_usecs64;
		cur->frame_count++;
	} else {
		frame_four_usecs64 =
			left_shift(cur->last_frame_unit, FRAME_UNIT - 1);
		cur->last_frame_unit[FRAME_UNIT - 1] = frame_usecs64;
		frame_unit_usecs64 = frame_four_usecs64 + frame_usecs64;

		if (f_t_fps == -1) {
			target_fps = calulate_fps(frame_usecs64, cur);
			if (target_fps == -1) {
				perfmgr_dbg(
					debug_mask_perfmgr, PERFMGR_POLICY,
					"cpufreq unkown fps continus,frame_usecs64=%lld\n",
					frame_usecs64);
			} else if (target_fps == 0) {
				perfmgr_dbg(
					debug_mask_perfmgr, PERFMGR_POLICY,
					"cpufreq target_fps 0 continus,frame_usecs64=%lld\n",
					frame_usecs64);
			} else {
				cur->target_fps = target_fps;
			}
		} else {
			cur->target_fps = f_t_fps;
		}

#ifdef CONFIG_MI_PERFMGR_GPU
		gpu_get_target_fps = cur->target_fps;
		perfmgr_gpu_record_frame(cur->identifier, frame_usecs64 * 1000);
#endif
		frame_usecs64_x_fps = frame_usecs64 * (cur->target_fps);
		frame_unit_usecs64_x_fps =
			frame_unit_usecs64 * (cur->target_fps);

		perfmgr_dbg(
			debug_mask_perfmgr, PERFMGR_POLICY,
			"t_fps=%d pid=%d id：%llu usecs64_x_fps %lld frame_unit_usecs64_x_fps %lld\n",
			cur->target_fps, cur->pid, cur->identifier,
			frame_usecs64_x_fps, frame_unit_usecs64_x_fps);

		if (frame_usecs64_x_fps > (1 * M * M + scaling_r_thres * M)) {
			jank_happened = 1;
			cur->rescue_keep_count = 0;
			cur->keep_continus_count = 0;
			cur->smooth = 0;

			if (r_perf == 0) {
				if ((r_step > 300) &&
				    (frame_usecs64_x_fps >
				     (1 * M * M + scaling_r_thres * M +
				      r_step * M))) {
					r_p_level = r_freq_level + 1;
					level = 1;
				} else {
					r_p_level = r_freq_level;
					level = 1;
				}
			} else {
				if (frame_usecs64_x_fps >
				    (1 * M * M + 5 * r_step * M))
					level = 5;
				else if (frame_usecs64_x_fps >
					 (1 * M * M + 4 * r_step * M))
					level = 4;
				else if (frame_usecs64_x_fps >
					 (1 * M * M + 3 * r_step * M))
					level = 3;
				else if (frame_usecs64_x_fps >
					 (1 * M * M + 2 * r_step * M))
					level = 2;
				else
					level = 1;
				r_p_level = r_freq_level;
			}
			perfmgr_set_freq(cur, r_p_level, level);
			perfmgr_dbg(debug_mask_perfmgr, PERFMGR_POLICY,
				    "scaling r up %lld %d\n",
				    frame_usecs64 * (cur->target_fps),
				    (1 * M * M + scaling_r_thres * M * N));
			return;
		} else {
			cur->smooth = 1;
			cur->keep_continus_count++;

			if (frame_unit_usecs64_x_fps <
			    (FRAME_UNIT * M * M - scaling_b * M)) {
				cur->rescue_keep_count++;
				if ((better_perf == 1) &&
				    (cur->keep_continus_count > perf_count))
					perfmgr_set_freq(cur, 0, 0);

				if (jank_happened)
					cur->rescue_keep_total_count =
						nor_f_keep + j_f_k_count;
				else
					cur->rescue_keep_total_count =
						nor_f_keep;

				if (fast_down_freq_level != -1) {
					fast_down_circle = update_circle(
						fast_down_circle_base);
					fast_down_circle =
						fast_down_circle >
								MIN_FASTDOWN_CIRCLE ?
							fast_down_circle :
							MIN_FASTDOWN_CIRCLE;
					perfmgr_dbg(
						debug_mask_perfmgr,
						PERFMGR_POLICY,
						"cons_no_j_cnt=%d fast_down_circle=%d jank_happened=%d\n",
						cons_no_j_cnt, fast_down_circle,
						jank_happened);
				}

				if (((cur->rescue_keep_count >=
				      cur->rescue_keep_total_count) ||
				     (cons_no_j_cnt &&
				      (cur->keep_continus_count >
				       cons_no_j_cnt))) &&
				    ((current_time - last_limit_time) *
					     (cur->target_fps) >
				     (scaling_c * M * M))) {
					jank_happened = 0;
					cur->rescue_keep_count = 0;
					cur->keep_continus_count = 0;
					r_p_level = 0;
					level = -1;
					if (level != fast_down_freq_level) {
						if (last_freq_level <
						    fast_down_level_thres) {
							down_count++;
							if (down_count %
								    fast_down_circle ==
							    1) {
								level = fast_down_freq_level;
								down_count = 1;
							}
						}
					}
					perfmgr_set_freq(cur, r_p_level, level);
					last_limit_time = current_time;
					perfmgr_dbg(
						debug_mask_perfmgr,
						PERFMGR_POLICY,
						"enter scaling_b function with level %d\n",
						level);
				}
			} else if (frame_unit_usecs64_x_fps >
				   (FRAME_UNIT * M * M + scaling_a * M)) {
				if (p_perf == 0) {
					if (frame_unit_usecs64_x_fps >
					    (FRAME_UNIT * M * M +
					     (scaling_a_thres + p_step) * M)) {
						r_p_level = p_freq_level;
						level = 2;
					} else if (frame_unit_usecs64_x_fps >
						   (FRAME_UNIT * M * M +
						    scaling_a_thres * M)) {
						r_p_level = p_freq_level;
						level = 1;
					} else {
						r_p_level = 0;
						level = 1;
					}
				} else {
					if (frame_unit_usecs64_x_fps >
					    (FRAME_UNIT * M * M +
					     (scaling_a_thres + 2 * p_step) *
						     M))
						level = 3;
					else if (frame_unit_usecs64_x_fps >
						 (FRAME_UNIT * M * M +
						  (scaling_a_thres + p_step) *
							  M))
						level = 2;
					else
						level = 1;
					r_p_level = p_freq_level;
				}
				perfmgr_set_freq(cur, r_p_level, level);
				perfmgr_dbg(
					debug_mask_perfmgr, PERFMGR_POLICY,
					"scaling p up %lld %d\n",
					frame_unit_usecs64_x_fps,
					(FRAME_UNIT * M * M + scaling_a * M));
			} else {
				perfmgr_dbg(debug_mask_perfmgr, PERFMGR_POLICY,
					    "scaling keep\n");
			}
		}
	}
}

void perfmgr_update_sf_msg(struct connected_buffer *cur)
{
	s64 sf_completion_end;
	s64 frame_q2s;

	if (!sf_comp_hint_enable)
		return;

	sf_completion_end = ktime_get();
	frame_q2s = ktime_to_us(ktime_sub(sf_completion_end, cur->last_time));
	pr_debug("FEAS_SF queue to sf completion cost %lld\n", frame_q2s);
}

static void do_current_work(struct work_struct *work)
{
	pr_debug("%s perfmgr_state 0x%02x cur_work.fps %d\n", __func__,
		 cur_work.perfmgr_state, cur_work.fps);

	if (cur_work.perfmgr_state == PERFMGR_FREQUENCY_RELEASE_AND_SET_FLOOR ||
	    cur_work.perfmgr_state ==
		    PERFMGR_FREQUENCY_RELEASE_AND_SET_FLOOR_HIGH) {
		do_freq(cpufreq_table0[0], f_left_minfreq0, cpufreq_table5[0],
			f_left_minfreq, cpufreq_table7[0], f_left_minfreq);
	} else {
		do_maxfreq_release();
	}
}

static enum hrtimer_restart timercallback(struct hrtimer *timer)
{
	int timeout_now;

	pr_debug("timercallback perfmgr_state 0x%02x cur_work.fps %d\n",
		 cur_work.perfmgr_state, cur_work.fps);

	if (cur_work.perfmgr_state == PERFMGR_FREQUENCY_LIMIT_STATE) {
		pr_debug("%s scaling t up\n", __func__);
		if (timeout_r_freq_level)
			last_freq_level =
				last_freq_level - timeout_r_freq_level;
		cur_work.perfmgr_state =
			PERFMGR_FREQUENCY_RELEASE_AND_SET_FLOOR;
		timeout_now = timeout_left;
		queue_work(cur_work.cpufreq_wq, &cur_work.queue_work);
		hrtimer_forward_now(&cur_work.hr_timer,
				    ns_to_ktime(timeout_now * 1000));
		return HRTIMER_RESTART;
	} else if (cur_work.perfmgr_state ==
		   PERFMGR_FREQUENCY_RELEASE_AND_SET_FLOOR) {
		cur_work.perfmgr_state =
			PERFMGR_FREQUENCY_RELEASE_AND_SET_FLOOR_HIGH;
		timeout_now = timeout_left;
		queue_work(cur_work.cpufreq_wq, &cur_work.queue_work);
		hrtimer_forward_now(&cur_work.hr_timer,
				    ns_to_ktime(timeout_now * 1000));
		return HRTIMER_RESTART;
	} else {
		cur_work.perfmgr_state = PERFMGR_FREQUENCY_RELEASED_ALL_STATE;
		queue_work(cur_work.cpufreq_wq, &cur_work.queue_work);
		return HRTIMER_NORESTART;
	}
}

int perfmgr_policy_init(void)
{
	int cpu;

	max_freq_limit_level_limit =
		min3(ARRAY_SIZE(cpufreq_table0), ARRAY_SIZE(cpufreq_table5),
		     ARRAY_SIZE(cpufreq_table7)) -
		1;
	pr_info("%s max_freq_limit_level_limit %d\n", __func__,
		max_freq_limit_level_limit);

	cur_work.cpufreq_wq = alloc_workqueue("cpufreq_wq", WQ_HIGHPRI, 0);
	if (!cur_work.cpufreq_wq)
		return -EFAULT;

	INIT_DELAYED_WORK(&cur_work.release_work, do_maxfreq_release_work);
	INIT_WORK(&cur_work.queue_work, do_current_work);

	hrtimer_init(&cur_work.hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cur_work.hr_timer.function = timercallback;

	for_each_possible_cpu (cpu) {
		perfmgr_limit_max[cpu] = UINT_MAX;
		perfmgr_limit_min[cpu] = 0;
	}

	/* Register Linux 4.19 cpufreq policy notifier */
	cpufreq_register_notifier(&perfmgr_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);

	if (cpu_subsys.dev_root)
		perfmgr_policy_kobj = kobject_create_and_add(
			"perfmgr_policy", &cpu_subsys.dev_root->kobj);

	return 0;
}
