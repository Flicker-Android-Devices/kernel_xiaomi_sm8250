/*
 * Copyright (C) 2021 Xiaomi Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/devfreq.h>
#include <linux/mi_perfmgr.h>
#include "perfmgr.h"

extern int update_devfreq(struct devfreq *devfreq);

#define HZ_PER_KHZ 1000
#define GPU_LOAD_ITMES 60
#define FREQ_MAX_DEFAULT_VALUE S32_MAX
#define GPU_FRAME_UNIT 5

static int gpu_iffm_enable = 0;
module_param(gpu_iffm_enable, int, 0644);
MODULE_PARM_DESC(gpu_iffm_enable, "gpu_iffm_enable");

static int gpufreq_timeout_ms = 1500;
module_param(gpufreq_timeout_ms, int, 0644);
MODULE_PARM_DESC(gpufreq_timeout_ms, "gpufreq_timeout_ms");

static int set_freq_direct = 1;
module_param(set_freq_direct, int, 0644);
MODULE_PARM_DESC(set_freq_direct, "set_freq_direct");

static int get_df_success = 0;
module_param(get_df_success, int, 0644);
MODULE_PARM_DESC(get_df_success, "get_df_success");

static int gpu_reset_freq = 0;
module_param(gpu_reset_freq, int, 0644);
MODULE_PARM_DESC(gpu_reset_freq, "gpu_reset_freq");

unsigned long target_gpu_freq = FREQ_MAX_DEFAULT_VALUE;
module_param(target_gpu_freq, ulong, 0644);
MODULE_PARM_DESC(target_gpu_freq, "target_gpu_freq");

static unsigned long gpu_req_min_freq = FREQ_MAX_DEFAULT_VALUE;
module_param(gpu_req_min_freq, ulong, 0644);
MODULE_PARM_DESC(gpu_req_min_freq, "gpu_req_min_freq");

static int rescue_time_60 = 16000000;
module_param(rescue_time_60, int, 0644);
MODULE_PARM_DESC(rescue_time_60, "rescue_time_60");

static int release_time_60 = 0;
module_param(release_time_60, int, 0644);
MODULE_PARM_DESC(release_time_60, "release_time_60");

static int unit_rescue_time_60 = 0;
module_param(unit_rescue_time_60, int, 0644);
MODULE_PARM_DESC(unit_rescue_time_60, "unit_rescue_time_60");

static int unit_release_time_60 = 10500000;
module_param(unit_release_time_60, int, 0644);
MODULE_PARM_DESC(unit_release_time_60, "unit_release_time_60");

static int debug_mask_gpu = 0;
module_param(debug_mask_gpu, int, 0644);
MODULE_PARM_DESC(debug_mask_gpu, "debug_mask_gpu");

struct gpu_load_items {
	u64 frameId;
	s64 gpu_completion_time; /* ns */
};

static struct gpu_load_items gpu_load_history[GPU_LOAD_ITMES];
unsigned int perfmgr_gpu_cur_history_items = 0;

static struct delayed_work gpufreq_release_work;
static struct work_struct gpufreq_boost_work;
static unsigned long boost_intent_freq;
static struct devfreq *perfmgr_df;
static struct hrtimer gpufreq_boost_timer;

static int release_req = 0;
static int is_freq_match = 1;
static u64 cur_frame_id = 0;

extern int gpu_get_target_fps;
extern void (*perfmgr_notify_gpu_hint_fp)(int start, int pid,
					  unsigned long long frameNum);

static unsigned long *gpu_freq_table = NULL;
static int gpu_freq_table_len = 0;
static unsigned long cur_gpu_freq;
static s64 last_frame_unit[GPU_FRAME_UNIT];
static s64 frame_four_usecs64;

int (*perfmgr_notify_gpu_freq_fp)(bool need_update);
EXPORT_SYMBOL(perfmgr_notify_gpu_freq_fp);

enum debug_mask {
	PERFMGR_GPU = BIT(0),
	PERFMGR_TEMP = BIT(1),
	PERFMGR_TEMP_1 = BIT(2),
};

enum cancel_timer_reason {
	CANCEL_REASON_NONE,
	DIFF_FRAME_START_ONE,
	DIFF_FRAME_START_ZERO,
	SAME_FRAME_START_ONE,
	SAME_FRAME_START_ZERO,
};

#define perfmgr_gpu(reason, fmt, ...)                                          \
	do {                                                                   \
		if (debug_mask_gpu & (reason))                                 \
			pr_info("%s: %s: " fmt, "perfmgr_gpu", __func__,       \
				##__VA_ARGS__);                                \
		else                                                           \
			pr_debug("%s: %s: " fmt, "perfmgr_gpu", __func__,      \
				 ##__VA_ARGS__);                               \
	} while (0)

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

int show_gpu_load(struct seq_file *m, void *v)
{
	int i;
	int idx;
	unsigned int total;
	unsigned int count;
	unsigned int start;

	total = perfmgr_gpu_cur_history_items;
	count = min_t(unsigned int, total, GPU_LOAD_ITMES);
	start = (total > GPU_LOAD_ITMES) ? (total % GPU_LOAD_ITMES) : 0;

	seq_printf(m, "gpu load history (recorded: %u frames)\n", count);
	for (i = 0; i < count; i++) {
		idx = (start + i) % GPU_LOAD_ITMES;
		if (!gpu_load_history[idx].frameId)
			continue;
		seq_printf(m, "frame %llu gpu completion time %lld us\n",
			   gpu_load_history[idx].frameId,
			   gpu_load_history[idx].gpu_completion_time / 1000);
	}
	seq_printf(m, "\n");
	return 0;
}

int perfmgr_gpu_enable(void)
{
	return gpu_iffm_enable && is_freq_match;
}

/*
 * Receive devfreq handle from GPU driver (kgsl)
 */
void get_perfmgr_devfreq(struct devfreq *df)
{
	int ret;

	if (IS_ERR(df)) {
		ret = PTR_ERR(df);
		perfmgr_gpu(PERFMGR_GPU,
			    "%s perfmgr Devfreq not available:%d\n", __func__,
			    ret);
		return;
	}
	perfmgr_gpu(PERFMGR_GPU, "%s +++++++++++++\n", __func__);
	if (perfmgr_df == NULL) {
		perfmgr_df = df;
		gpu_freq_table_len = df->profile->max_state;
		gpu_freq_table = df->profile->freq_table;
		target_gpu_freq = 0;
		cur_gpu_freq = 0;
	}

	if (perfmgr_df != NULL) {
		get_df_success = gpu_freq_table_len;
	}
}
EXPORT_SYMBOL(get_perfmgr_devfreq);

static void freq_level_adjust(int *cur_level)
{
	if (*cur_level < 0) {
		*cur_level = 0;
	}

	if (*cur_level > gpu_freq_table_len - 1) {
		*cur_level = gpu_freq_table_len - 1;
	}
}

int perfmgr_get_freq_level(unsigned long freq)
{
	int corres_level = -1;
	int lev;

	perfmgr_gpu(PERFMGR_GPU, "perfmgr_get_freq_lev freq: %ld\n", freq);

	if (!gpu_freq_table || gpu_freq_table_len <= 0)
		return -1;

	if ((freq == 0) || (freq < gpu_freq_table[gpu_freq_table_len - 1])) {
		return gpu_freq_table_len - 1;
	}

	if (freq > gpu_freq_table[0]) {
		return 0;
	}

	for (lev = 0; lev <= gpu_freq_table_len - 1; lev++) {
		if (freq >= gpu_freq_table[lev]) {
			corres_level = lev;
			break;
		}
	}

	return corres_level;
}

int calculate_rescue_time(int rescue_time, int unit_rescue_time)
{
	int target_rescue_time = 0;

	if (rescue_time > 0 || unit_rescue_time > 0) {
		if (unit_rescue_time > 0 &&
		    perfmgr_gpu_cur_history_items >= GPU_FRAME_UNIT) {
			target_rescue_time = unit_rescue_time * GPU_FRAME_UNIT -
					     frame_four_usecs64;
			if (target_rescue_time <= 0) {
				perfmgr_gpu(
					PERFMGR_GPU,
					"new frameNum %llu target_rescue_time < 0, need quick rescue last four frame cost %lld > %d\n",
					cur_frame_id, frame_four_usecs64,
					unit_rescue_time * GPU_FRAME_UNIT);
				return 0;
			}
		}
		if (target_rescue_time > 0) {
			target_rescue_time =
				rescue_time == 0 ?
					target_rescue_time :
					min(target_rescue_time, rescue_time);
		} else {
			target_rescue_time = rescue_time;
		}
	}

	return target_rescue_time;
}

void cancel_timer_if_needed(enum cancel_timer_reason reason)
{
	if (hrtimer_active(&gpufreq_boost_timer)) {
		hrtimer_cancel(&gpufreq_boost_timer);
		switch (reason) {
		case DIFF_FRAME_START_ONE:
			perfmgr_gpu(
				PERFMGR_GPU,
				"cancel timer DIFF_FRAME_START_ONE frame %llu\n",
				cur_frame_id);
			break;
		case DIFF_FRAME_START_ZERO:
			perfmgr_gpu(PERFMGR_GPU,
				    "cancel timer for no start info %llu\n",
				    cur_frame_id);
			break;
		case SAME_FRAME_START_ONE:
			perfmgr_gpu(PERFMGR_GPU,
				    "cancel timer for new frame %llu\n",
				    cur_frame_id);
			break;
		case SAME_FRAME_START_ZERO:
			perfmgr_gpu(
				PERFMGR_GPU,
				"cancel timer for frame end before timeout %llu\n",
				cur_frame_id);
			break;
		default:
			perfmgr_gpu(PERFMGR_GPU,
				    "cancel timer for unknown reason %llu\n",
				    cur_frame_id);
			break;
		}
	}
}

s64 store_five_frame_info_and_calc_avg(s64 frame_gpu_completion,
				       unsigned long long frameNum)
{
	int cur_history_items_size = 0;
	s64 frame_gpu_cost_total = 0;
	s64 frame_gpu_cost_avg = 0;
	int i;
	int history_idx;

	if (perfmgr_gpu_cur_history_items < GPU_FRAME_UNIT) {
		last_frame_unit[perfmgr_gpu_cur_history_items] =
			frame_gpu_completion;
		cur_history_items_size = perfmgr_gpu_cur_history_items + 1;
		for (i = 0; i < cur_history_items_size; i++) {
			frame_gpu_cost_total += last_frame_unit[i];
		}
	} else {
		frame_four_usecs64 =
			left_shift(last_frame_unit, GPU_FRAME_UNIT - 1);
		last_frame_unit[GPU_FRAME_UNIT - 1] = frame_gpu_completion;
		frame_gpu_cost_total =
			frame_four_usecs64 + frame_gpu_completion;
		cur_history_items_size = GPU_FRAME_UNIT;
	}

	frame_gpu_cost_avg = frame_gpu_cost_total / cur_history_items_size;
	perfmgr_gpu(
		PERFMGR_TEMP_1,
		"unit_size %d frame count %d frameNum %llu comp [%llu] ns, frame_gpu_cost_total %lldns frame_gpu_cost_avg %lldns\n",
		cur_history_items_size, perfmgr_gpu_cur_history_items, frameNum,
		frame_gpu_completion, frame_gpu_cost_total, frame_gpu_cost_avg);

	history_idx = perfmgr_gpu_cur_history_items % GPU_LOAD_ITMES;
	gpu_load_history[history_idx].frameId = frameNum;
	gpu_load_history[history_idx].gpu_completion_time =
		frame_gpu_completion;

	perfmgr_gpu_cur_history_items++;
	if (perfmgr_gpu_cur_history_items >= UINT_MAX) {
		perfmgr_gpu_cur_history_items = 0;
	}

	return frame_gpu_cost_avg;
}

void perfmgr_gpu_record_frame(unsigned long long frame_id, s64 duration_ns)
{
	int history_idx;

	history_idx = perfmgr_gpu_cur_history_items % GPU_LOAD_ITMES;
	gpu_load_history[history_idx].frameId = frame_id;
	gpu_load_history[history_idx].gpu_completion_time = duration_ns;

	perfmgr_gpu_cur_history_items++;
	if (perfmgr_gpu_cur_history_items >= UINT_MAX) {
		perfmgr_gpu_cur_history_items = 0;
	}
}
EXPORT_SYMBOL(perfmgr_gpu_record_frame);

static void perfmgr_boost_gpu_online(unsigned long freq, int is_release);

void perfmgr_notify_gpu_hint(int start, int pid, unsigned long long frameNum)
{
	static ktime_t start_time;
	s64 frame_gpu_completion;
	ktime_t current_time = ktime_get();
	int cur_level, pre_level;
	int rescue_time = 0;
	int release_time = 0;
	int unit_rescue_time = 0;
	int unit_release_time = 0;
	int target_ns;
	s64 frame_gpu_completion_avg;
	static int final_rescue_time = 0;
	unsigned long freq;

	if (gpu_get_target_fps > 0) {
		is_freq_match = 1;
		if (gpu_get_target_fps == 60) {
			rescue_time = rescue_time_60;
			release_time = release_time_60;
			unit_rescue_time = unit_rescue_time_60;
			unit_release_time = unit_release_time_60;
		} else {
			target_ns = 1000000000 / gpu_get_target_fps;
			rescue_time = target_ns;
			release_time = 0;
			unit_rescue_time = 0;
			unit_release_time = (target_ns * 65) / 100;
		}
		perfmgr_gpu(PERFMGR_GPU, "match cur target fps %d\n",
			    gpu_get_target_fps);
	} else {
		is_freq_match = 0;
		rescue_time = 0;
		release_time = 0;
		unit_rescue_time = 0;
		unit_release_time = 0;
		perfmgr_gpu(PERFMGR_GPU, "can't match any target fps %d\n",
			    gpu_get_target_fps);
	}

	if (!perfmgr_gpu_enable()) {
		perfmgr_gpu(PERFMGR_GPU, "perfmgr gpu not enabled!\n");
		return;
	}

	perfmgr_gpu(
		PERFMGR_TEMP_1,
		"FEAS_GPU call perfmgr_notify_gpu_hint start is %d, pid is %d, frameNum is %llu\n",
		start, pid, frameNum);

	if (cur_frame_id != frameNum) {
		if (start == 1) {
			cancel_timer_if_needed(DIFF_FRAME_START_ONE);

			cur_frame_id = frameNum;
			start_time = current_time;
			final_rescue_time = calculate_rescue_time(
				rescue_time, unit_rescue_time);
			if (final_rescue_time > 0) {
				hrtimer_start(&gpufreq_boost_timer,
					      ktime_set(0, final_rescue_time),
					      HRTIMER_MODE_REL);
			}
			perfmgr_gpu(
				PERFMGR_TEMP_1,
				"frameNum %llu start time final_rescue_time %d\n",
				cur_frame_id, final_rescue_time);
		} else if (start == 0) {
			cancel_timer_if_needed(DIFF_FRAME_START_ZERO);
			return;
		} else {
			perfmgr_gpu(PERFMGR_GPU,
				    "unknown start value:%d framenum: %llu\n",
				    start, cur_frame_id);
			return;
		}
	} else {
		if (start == 1) {
			cancel_timer_if_needed(SAME_FRAME_START_ONE);

			cur_frame_id = frameNum;
			start_time = current_time;
			final_rescue_time = calculate_rescue_time(
				rescue_time, unit_rescue_time);
			if (final_rescue_time > 0) {
				hrtimer_start(&gpufreq_boost_timer,
					      ktime_set(0, final_rescue_time),
					      HRTIMER_MODE_REL);
			}
			perfmgr_gpu(
				PERFMGR_TEMP_1,
				"error frameNum %llu double start time final_rescue_time %d\n",
				cur_frame_id, final_rescue_time);
		} else if (start == 0) {
			frame_gpu_completion =
				ktime_sub(current_time, start_time);
			perfmgr_gpu(
				PERFMGR_TEMP,
				"frame count %d frameNum %llu comp %llu ns\n",
				perfmgr_gpu_cur_history_items, cur_frame_id,
				frame_gpu_completion);

			frame_gpu_completion_avg =
				store_five_frame_info_and_calc_avg(
					frame_gpu_completion, frameNum);

			pre_level = cur_level =
				perfmgr_get_freq_level(cur_gpu_freq);
			if (cur_level == -1) {
				perfmgr_gpu(PERFMGR_GPU,
					    "perfmgr_get_freq_level error\n");
				return;
			}
			if ((release_time > 0 &&
			     frame_gpu_completion <= release_time) ||
			    (unit_release_time > 0 &&
			     frame_gpu_completion_avg <= unit_release_time)) {
				cur_level++;
				if (hrtimer_active(&gpufreq_boost_timer)) {
					perfmgr_gpu(
						PERFMGR_TEMP_1,
						"perfmgr_notify_gpu_hint frame %llu cost %llu ns, cancel for less than %d, or unit avg %lld less than %d\n",
						cur_frame_id,
						frame_gpu_completion,
						release_time,
						frame_gpu_completion_avg,
						unit_release_time);
					hrtimer_cancel(&gpufreq_boost_timer);
				}
				freq_level_adjust(&cur_level);

				perfmgr_gpu(
					PERFMGR_GPU,
					"perfmgr_notify_gpu_hint boost from lev %d to %d\n",
					pre_level, cur_level);
				if (cur_level != pre_level &&
				    perfmgr_gpu_enable()) {
					perfmgr_gpu(
						PERFMGR_GPU,
						"perfmgr_boost_gpu_and_timeout frameNum %llu cost %lld ns avg frame cost %lld down from lev %d to %d, targetfreq: %lu\n",
						cur_frame_id,
						frame_gpu_completion,
						frame_gpu_completion_avg,
						pre_level, cur_level,
						gpu_freq_table[cur_level]);
					freq = gpu_freq_table[cur_level];
					perfmgr_boost_gpu_and_timeout(freq);
				}
			}
			if ((rescue_time > 0 &&
			     frame_gpu_completion <= rescue_time) ||
			    (unit_rescue_time > 0 &&
			     frame_gpu_completion_avg <= unit_rescue_time)) {
				if (hrtimer_active(&gpufreq_boost_timer)) {
					perfmgr_gpu(
						PERFMGR_TEMP_1,
						"perfmgr_notify_gpu_hint frame %llu cost %llu ns, cancel for less than %d\n",
						cur_frame_id,
						frame_gpu_completion,
						final_rescue_time);
					hrtimer_cancel(&gpufreq_boost_timer);
				}
			}
			if (final_rescue_time > 0 &&
			    (frame_gpu_completion > final_rescue_time ||
			     frame_gpu_completion_avg > final_rescue_time)) {
				perfmgr_gpu(
					PERFMGR_TEMP_1,
					"perfmgr_notify_gpu_hint timeout!! frame %llu comp cost %llu ns rescue time %d\n",
					cur_frame_id, frame_gpu_completion,
					final_rescue_time);
			}
		} else {
			perfmgr_gpu(PERFMGR_GPU,
				    "unknown start value:%d framenum: %llu\n",
				    start, cur_frame_id);
		}
	}
}

int perfmgr_get_gpu_hook(unsigned long *freq, bool *check_return)
{
	if (freq == NULL) {
		perfmgr_gpu(PERFMGR_GPU, "perfmgr_get_gpu_hook error input\n");
		return -1;
	}

	cur_gpu_freq = *freq;
	perfmgr_gpu(PERFMGR_GPU, "perfmgr_get_gpu_hook get cur_gpu_freq: %ld\n",
		    cur_gpu_freq);

	if (!set_freq_direct && !release_req) {
		perfmgr_gpu(
			PERFMGR_GPU,
			"perfmgr_get_gpu_hook return -1 for !set_freq_direct && !release_req\n");
		return -1;
	}

	if (!perfmgr_gpu_enable() && !release_req) {
		perfmgr_gpu(
			PERFMGR_GPU,
			"perfmgr_get_gpu_hook return -1 for !perfmgr_gpu_enable() && !release_req\n");
		return -1;
	}
	if (target_gpu_freq == FREQ_MAX_DEFAULT_VALUE) {
		perfmgr_gpu(
			PERFMGR_GPU,
			"perfmgr_get_gpu_hook return -1 for target_gpu_freq == FREQ_MAX_DEFAULT_VALUE\n");
		return -1;
	}
	if (!release_req && target_gpu_freq == 0) {
		perfmgr_gpu(
			PERFMGR_GPU,
			"perfmgr_get_gpu_hook return -1 for !release_req && target_gpu_freq == 0\n");
		return -1;
	}

	if (target_gpu_freq == 0) {
		perfmgr_gpu(PERFMGR_GPU,
			    "perfmgr_get_gpu_hook target_gpu_freq == 0\n");
		if (gpu_freq_table && gpu_freq_table_len > 0)
			*freq = gpu_freq_table[gpu_freq_table_len - 1];
	} else {
		*freq = target_gpu_freq;
	}
	*check_return = true;
	perfmgr_gpu(
		PERFMGR_GPU,
		"curFrame %llu before hook %lu after hook freq is %lu target_gpufreq %lu\n",
		cur_frame_id, cur_gpu_freq, *freq, target_gpu_freq);
	cur_gpu_freq = *freq;
	release_req = 0;

	return 0;
}
EXPORT_SYMBOL(perfmgr_get_gpu_hook);

/*
 * Boost GPU freq floor on Linux 4.19 devfreq
 */
static int perfmgr_boost_gpu_min(unsigned long minFreq)
{
	if (perfmgr_df == NULL || !get_df_success) {
		perfmgr_gpu(PERFMGR_GPU, "perfmgr_df is NULL 1\n");
		return -1;
	}

	perfmgr_gpu(
		PERFMGR_TEMP_1,
		"perfmgr_boost_gpu_min gpu_req_min_freq is %ld, minFreq: %ld\n",
		gpu_req_min_freq, minFreq);
	gpu_req_min_freq = minFreq;

	mutex_lock(&perfmgr_df->lock);
	perfmgr_df->min_freq = minFreq;
	update_devfreq(perfmgr_df);
	mutex_unlock(&perfmgr_df->lock);

	return 0;
}

static int perfmgr_boost_gpu(unsigned long target_freq, int is_release)
{
	int ret = 0;
	s32 qos_min_freq = 0;

	if (set_freq_direct) {
		perfmgr_gpu(PERFMGR_GPU,
			    "set freq direct 1 is_release %d target %ld\n",
			    is_release, target_freq);
		qos_min_freq = perfmgr_df ? perfmgr_df->min_freq : 0;
		if (perfmgr_df != NULL && qos_min_freq > target_freq) {
			perfmgr_gpu(
				PERFMGR_GPU,
				"set freq direct qos_minfreq block qos %d max %ld\n",
				qos_min_freq, target_freq);
			perfmgr_boost_gpu_min(target_freq);
		}
		target_gpu_freq = target_freq;
		release_req = is_release;
		if (gpu_req_min_freq != gpu_reset_freq) {
			perfmgr_gpu(
				PERFMGR_GPU,
				"+++set freq direct gpu_req_min_freq %lu gpu_reset_freq:%d max %ld\n",
				gpu_req_min_freq, gpu_reset_freq, target_freq);
			gpu_req_min_freq = gpu_reset_freq;
			perfmgr_boost_gpu_min(gpu_req_min_freq);
		}
		ret = 2;
	} else {
		perfmgr_gpu(
			PERFMGR_GPU,
			"not set freq direct %lu gpu_reset_freq:%d max %ld\n",
			target_gpu_freq, gpu_reset_freq, target_freq);
		if (target_gpu_freq != gpu_reset_freq) {
			target_gpu_freq = gpu_reset_freq;
			release_req = 1;
		}
		ret = perfmgr_boost_gpu_min(target_freq);
	}

	if (perfmgr_notify_gpu_freq_fp) {
		perfmgr_notify_gpu_freq_fp(true);
	}

	perfmgr_gpu(
		PERFMGR_GPU,
		"frame %llu from %lu boost to %lu is_release %d by set_freq_direct %d ret %d qos %d\n",
		cur_frame_id, cur_gpu_freq, target_freq, is_release,
		set_freq_direct, ret, qos_min_freq);
	return ret;
}

static void perfmgr_boost_gpu_online(unsigned long freq, int is_release)
{
	if (freq) {
		perfmgr_boost_gpu(freq, is_release);
	} else {
		perfmgr_boost_gpu(gpu_reset_freq, is_release);
	}
}

static void perfmgr_gpufreq_release_work(struct work_struct *work)
{
	perfmgr_gpu(PERFMGR_GPU, "%s\n", __func__);
	perfmgr_boost_gpu_online(gpu_reset_freq, 1);
}

void perfmgr_boost_gpu_and_timeout(unsigned long intent_freq)
{
	perfmgr_gpu(PERFMGR_GPU, "%s intent_freq: %ld\n", __func__,
		    intent_freq);
	cancel_delayed_work(&gpufreq_release_work);
	perfmgr_boost_gpu_online(intent_freq, 0);
	schedule_delayed_work(&gpufreq_release_work,
			      msecs_to_jiffies(gpufreq_timeout_ms));
}

static void perfmgr_gpufreq_boost_work_func(struct work_struct *work)
{
	if (boost_intent_freq)
		perfmgr_boost_gpu_and_timeout(boost_intent_freq);
}

static enum hrtimer_restart
perfmgr_gpufreq_boost_timer_func(struct hrtimer *timer)
{
	int cur_level, pre_level;

	pre_level = cur_level = perfmgr_get_freq_level(cur_gpu_freq);
	if (cur_level == -1) {
		perfmgr_gpu(PERFMGR_GPU, "perfmgr_get_freq_level error\n");
		return HRTIMER_NORESTART;
	}
	cur_level--;
	freq_level_adjust(&cur_level);

	if (cur_level != pre_level && perfmgr_gpu_enable()) {
		perfmgr_gpu(
			PERFMGR_GPU,
			"hrtimer_restart perfmgr_boost_gpu_and_timeout frameNum %llu boost from lev %d to %d, target_freq:%lu\n",
			cur_frame_id, pre_level, cur_level,
			gpu_freq_table[cur_level]);
		boost_intent_freq = gpu_freq_table[cur_level];
		schedule_work(&gpufreq_boost_work);
	}

	return HRTIMER_NORESTART;
}

int perfmgr_gpu_init(void)
{
	perfmgr_gpu(PERFMGR_GPU, "%s\n", __func__);
	INIT_DELAYED_WORK(&gpufreq_release_work, perfmgr_gpufreq_release_work);
	INIT_WORK(&gpufreq_boost_work, perfmgr_gpufreq_boost_work_func);
	perfmgr_notify_gpu_hint_fp = perfmgr_notify_gpu_hint;
	hrtimer_init(&gpufreq_boost_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	gpufreq_boost_timer.function = perfmgr_gpufreq_boost_timer_func;

	return 0;
}
