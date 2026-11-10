/*
 * Copyright (C) 2021 XiaoMi Inc.
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
#ifndef PERFMGR_H
#define PERFMGR_H

#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include <linux/suspend.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/ioctl.h>

#define FRAME_UNIT 5

enum print_reason {
	PERFMGR_FREQ = BIT(0),
	PERFMGR_POLICY = BIT(1),
	PERFMGR_INIT = BIT(2),
	PERFMGR_CALLBACK = BIT(3),
	PERFMGR_PARAMETERS = BIT(4),
	PERFMGR_DCVS = BIT(5),
};

enum PERFMGR_NOTIFIER_QUEUE_DEQUEUE_TYPE {
	POLICY_EGL_QUEUE = 1,
	POLICY_SF_COMPLETION,
};

#define perfmgr_dbg(debug_mask, reason, fmt, ...)                              \
	do {                                                                   \
		if ((debug_mask) & (reason))                                   \
			pr_info("%s: %s: " fmt, "perfmgr", __func__,           \
				##__VA_ARGS__);                                \
		else                                                           \
			pr_debug("%s: %s: " fmt, "perfmgr", __func__,          \
				 ##__VA_ARGS__);                               \
	} while (0)

struct _BUFFER_PACKAGE {
	__u32 pid;
	union {
		__u32 start;
		__u32 connectedAPI;
	};
	union {
		__u64 frame_time;
		__u64 bufID;
	};
	__u64 frame_id; /* for HWUI only*/
	__s32 queue_SF;
	__u64 identifier;
};

enum dcvs_hw_type {
	DCVS_DDR,
	DCVS_L3 = 2,
};

enum BUFFER_PUSH_TYPE {
	PERFMGR_NOTIFIER_QUEUE_DEQUEUE = 0x01,
	PERFMGR_NOTIFIER_CONNECT = 0x02,
	PERFMGR_NOTIFIER_VSYNC = 0x03,
};

enum PERFMGR_STATE {
	PERFMGR_FREQUENCY_RELEASED_ALL_STATE = 0x01,
	PERFMGR_FREQUENCY_LIMIT_STATE = 0x02,
	PERFMGR_FREQUENCY_RELEASE_AND_SET_FLOOR = 0x03,
	PERFMGR_FREQUENCY_RELEASE_AND_SET_FLOOR_HIGH = 0x04,
};

struct perfmgr_work {
	int fps;
	int perfmgr_state;
	struct work_struct queue_work;
	struct delayed_work release_work;
	struct hrtimer hr_timer;
	struct workqueue_struct *cpufreq_wq;
};

struct PERFMGR_NOTIFIER_PUSH_TAG {
	enum BUFFER_PUSH_TYPE ePushType;
	__u32 pid;
	__u32 hint;
	__u32 connectedAPI;
	__u64 identifier;
	struct work_struct sWork;
};

struct connected_buffer {
	int smooth;
	int is_rendering;
	int pid;
	ktime_t last_time;
	ktime_t last_jank_time;
	s64 last_frame_unit[FRAME_UNIT];
	int target_fps;
	int rescue_keep_count;
	int keep_continus_count;
	int rescue_keep_total_count;
	int frame_count;
	s64 count_time;
	__u64 identifier;
	struct list_head list;
};

#define BUFFER_QUEUE _IOW('g', 1, struct _BUFFER_PACKAGE)
#define BUFFER_DEQUEUE _IOW('g', 3, struct _BUFFER_PACKAGE)
#define BUFFER_VSYNC _IOW('g', 5, struct _BUFFER_PACKAGE)
#define BUFFER_TOUCH _IOW('g', 10, struct _BUFFER_PACKAGE)
#define BUFFER_QUEUE_CONNECT _IOW('g', 15, struct _BUFFER_PACKAGE)
#define BUFFER_GPU_HINT _IOW('g', 16, struct _BUFFER_PACKAGE)

extern int debug_mask_perfmgr;
extern int init_perfctl(struct proc_dir_entry *parent);
extern int perfmgr_policy_init(void);
extern void perfmgr_do_policy(struct connected_buffer *cur);
extern void perfmgr_update_sf_msg(struct connected_buffer *cur);
extern int perfmgr_is_enable(void);
extern void perfmgr_boost_dcvsfreq_and_timeout(unsigned int maxfreq);
extern int perfmgr_remap_init(void);
extern void perfmgr_remap_exit(void);
struct devfreq;
extern void perfmgr_register_ddr_devfreq(struct devfreq *df);
extern void perfmgr_register_llcc_devfreq(struct devfreq *df);

#ifdef CONFIG_MI_PERFMGR_GPU
extern int perfmgr_gpu_init(void);
extern void perfmgr_boost_gpu_and_timeout(unsigned long maxfreq);
extern int perfmgr_gpu_enable(void);
extern void perfmgr_notify_gpu_hint(int start, int pid,
				    unsigned long long frameNum);
extern void perfmgr_gpu_record_frame(unsigned long long frame_id,
				     s64 duration_ns);
#else
static inline int perfmgr_gpu_init(void)
{
	return 0;
}
static inline void perfmgr_boost_gpu_and_timeout(unsigned long maxfreq)
{
}
static inline int perfmgr_gpu_enable(void)
{
	return 0;
}
static inline void perfmgr_notify_gpu_hint(int start, int pid,
					   unsigned long long frameNum)
{
}
static inline void perfmgr_gpu_record_frame(unsigned long long frame_id,
					    s64 duration_ns)
{
}
#endif

#endif /* PERFMGR_H */
